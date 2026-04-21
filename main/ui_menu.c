// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "ui_menu.h"
#include "app_state.h"
#include "lv_port.h"

#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "UI_MENU";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static lv_obj_t *s_screen   = NULL;
static lv_obj_t *s_list     = NULL;
static lv_obj_t *s_status   = NULL;   // "Loading…" / error label
static lv_obj_t *s_modal    = NULL;   // add-video modal overlay

// Payload passed from list-item tap callback into the event queue.
typedef struct {
    char channel_id[64];
    char title[128];
} channel_entry_t;

// ---------------------------------------------------------------------------
// Add-video modal
// ---------------------------------------------------------------------------

static lv_obj_t *s_id_ta    = NULL;
static lv_obj_t *s_name_ta  = NULL;
static lv_obj_t *s_keyboard = NULL;

typedef struct {
    char video_id[64];
    char title[128];
} add_video_args_t;

static void add_video_task(void *arg)
{
    add_video_args_t *a = (add_video_args_t *)arg;

    char body[256];
    snprintf(body, sizeof(body),
             "{\"video_id\":\"%s\",\"title\":\"%s\"}", a->video_id, a->title);

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/channel",
             CONFIG_SERVER_IP, CONFIG_SERVER_PORT);

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && (status == 200 || status == 201)) {
        ESP_LOGI(TAG, "Added video '%s' (%s)", a->title, a->video_id);
    } else {
        ESP_LOGW(TAG, "POST /channel failed: err=%s status=%d",
                 esp_err_to_name(err), status);
    }

    free(a);
    vTaskDelete(NULL);
}

static void modal_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_modal) {
        lv_port_lock();
        lv_obj_del(s_modal);
        lv_port_unlock();
        s_modal = NULL;
    }
}

static void modal_add_cb(lv_event_t *e)
{
    (void)e;
    if (!s_id_ta) return;

    add_video_args_t *a = calloc(1, sizeof(*a));
    lv_port_lock();
    const char *vid_id = lv_textarea_get_text(s_id_ta);
    const char *title  = s_name_ta ? lv_textarea_get_text(s_name_ta) : "";
    strlcpy(a->video_id, vid_id,  sizeof(a->video_id));
    strlcpy(a->title,    title,   sizeof(a->title));
    lv_port_unlock();

    if (a->video_id[0] != '\0') {
        xTaskCreate(add_video_task, "add_video", 4096, a, 3, NULL);
    } else {
        free(a);
    }

    modal_close_cb(NULL);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_keyboard) lv_keyboard_set_textarea(s_keyboard, ta);
}

static void open_add_video_modal(void)
{
    if (s_modal) return;

    lv_port_lock();

    // Dim overlay
    s_modal = lv_obj_create(s_screen);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);

    // Dialog panel
    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, 800, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_label_set_text(hdr, "Add YouTube Video");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);

    lv_obj_t *id_lbl = lv_label_create(panel);
    lv_label_set_text(id_lbl, "YouTube Video ID:");

    s_id_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(s_id_ta, true);
    lv_textarea_set_placeholder_text(s_id_ta, "e.g. dQw4w9WgXcQ");
    lv_obj_set_width(s_id_ta, LV_PCT(100));
    lv_obj_add_event_cb(s_id_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *name_lbl = lv_label_create(panel);
    lv_label_set_text(name_lbl, "Title (optional):");

    s_name_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_placeholder_text(s_name_ta, "leave blank to auto-fetch");
    lv_obj_set_width(s_name_ta, LV_PCT(100));
    lv_obj_add_event_cb(s_name_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    // Buttons
    lv_obj_t *btn_row = lv_obj_create(panel);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_0, 0);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(cancel_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_add_event_cb(cancel_btn, modal_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(add_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, "Add");
    lv_obj_add_event_cb(add_btn, modal_add_cb, LV_EVENT_CLICKED, NULL);

    // Keyboard at bottom of modal
    s_keyboard = lv_keyboard_create(s_modal);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 260);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_id_ta);

    lv_port_unlock();
}

// ---------------------------------------------------------------------------
// Channel list population (called from background task)
// ---------------------------------------------------------------------------

static void list_tap_cb(lv_event_t *e)
{
    channel_entry_t *entry = (channel_entry_t *)lv_event_get_user_data(e);
    if (!entry || !g_app_event_queue) return;

    app_event_t ev = { .type = APP_EVENT_START_PLAYBACK };
    strlcpy(ev.start.channel_id, entry->channel_id, sizeof(ev.start.channel_id));
    strlcpy(ev.start.title,      entry->title,      sizeof(ev.start.title));
    xQueueSend(g_app_event_queue, &ev, 0);
}

static void add_btn_cb(lv_event_t *e)
{
    (void)e;
    open_add_video_modal();
}

static void populate_list(cJSON *info)
{
    lv_port_lock();

    // Remove loading status label
    if (s_status) {
        lv_obj_del(s_status);
        s_status = NULL;
    }
    lv_obj_clean(s_list);

    cJSON *ch = NULL;
    cJSON_ArrayForEach(ch, info) {
        const char *slug = ch->string;
        cJSON *title_j   = cJSON_GetObjectItemCaseSensitive(ch, "title");
        const char *title = (title_j && title_j->valuestring)
                            ? title_j->valuestring : slug;

        channel_entry_t *entry = calloc(1, sizeof(*entry));
        strlcpy(entry->channel_id, slug,  sizeof(entry->channel_id));
        strlcpy(entry->title,      title, sizeof(entry->title));

        lv_obj_t *btn = lv_list_add_button(s_list, NULL, title);
        lv_obj_set_height(btn, 90);
        lv_obj_add_event_cb(btn, list_tap_cb, LV_EVENT_CLICKED, entry);
    }

    lv_port_unlock();
}

// ---------------------------------------------------------------------------
// /info fetch task
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t   written;
    size_t   cap;
} fetch_ctx_t;

static esp_err_t fetch_event_cb(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        size_t avail = ctx->cap - ctx->written;
        size_t n = evt->data_len < (int)avail ? (size_t)evt->data_len : avail;
        memcpy(ctx->buf + ctx->written, evt->data, n);
        ctx->written += n;
    }
    return ESP_OK;
}

static void fetch_info_task(void *arg)
{
    (void)arg;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/info",
             CONFIG_SERVER_IP, CONFIG_SERVER_PORT);

    const size_t cap = 8192;
    uint8_t *buf = malloc(cap);
    if (!buf) { vTaskDelete(NULL); return; }

    fetch_ctx_t ctx = { .buf = buf, .written = 0, .cap = cap };

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = fetch_event_cb,
        .user_data     = &ctx,
        .timeout_ms    = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET /info failed: %s %d", esp_err_to_name(err), status);
        lv_port_lock();
        if (s_status) lv_label_set_text(s_status, "Failed to load channels");
        lv_port_unlock();
        free(buf);
        vTaskDelete(NULL);
        return;
    }

    buf[ctx.written < cap ? ctx.written : cap - 1] = '\0';
    cJSON *info = cJSON_Parse((char *)buf);
    free(buf);

    if (info) {
        populate_list(info);
        cJSON_Delete(info);
    } else {
        lv_port_lock();
        if (s_status) lv_label_set_text(s_status, "Invalid response from server");
        lv_port_unlock();
    }

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_menu_show(void)
{
    lv_port_lock();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x1a1a2e), 0);

    // Header bar
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_size(header, LV_PCT(100), 80);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 20, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Tab5 Player");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *add_btn = lv_button_create(header);
    lv_obj_set_size(add_btn, 56, 56);
    lv_obj_set_style_bg_color(add_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(add_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, "+");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(add_lbl);
    lv_obj_add_event_cb(add_btn, add_btn_cb, LV_EVENT_CLICKED, NULL);

    // Channel list
    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100) - 80);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);

    // "Loading…" placeholder
    s_status = lv_label_create(s_list);
    lv_label_set_text(s_status, "Loading channels...");
    lv_obj_set_style_text_color(s_status, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_center(s_status);

    lv_screen_load(s_screen);
    lv_port_unlock();

    // Fetch /info in background — must happen after unlock so LVGL task can run
    xTaskCreate(fetch_info_task, "fetch_info", 6144, NULL, 3, NULL);
}

void ui_menu_hide(void)
{
    lv_port_lock();
    if (s_modal) { lv_obj_del(s_modal); s_modal = NULL; }
    lv_port_unlock();
}
