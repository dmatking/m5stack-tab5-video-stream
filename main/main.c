// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Tab5 video player — top-level application.
//
// State machine:
//   MENU    → user taps channel → START_PLAYBACK event
//   PLAYING → user taps back   → STOP_PLAYBACK event → MENU
//
// LVGL port task runs only in MENU state.  During PLAYING, the video decode
// task writes directly to the DPI hardware framebuffers; the HUD module draws
// control graphics into the letterbox regions and polls touch events.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_lvgl_port.h"

#include "app_state.h"
#include "board_interface.h"
#include "hud.h"
#include "lv_port.h"
#include "player.h"
#include "ui_menu.h"

static const char *TAG = "APP";

QueueHandle_t g_app_event_queue = NULL;

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    10

static EventGroupHandle_t s_wifi_eg;
static int s_wifi_retries = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < WIFI_MAX_RETRIES) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_wifi_retries, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP32-C6 co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        return true;
    }
    ESP_LOGE(TAG, "WiFi connection failed");
    return false;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "M5Stack Tab5 player starting");
    board_init();

    if (!wifi_init()) {
        ESP_LOGE(TAG, "WiFi failed — halting");
        while (1) vTaskDelay(portMAX_DELAY);
    }

    board_audio_init();
    lv_port_init();

    g_app_event_queue = xQueueCreate(8, sizeof(app_event_t));
    ui_menu_show();

    // ---------------------------------------------------------------------------
    // Main event loop
    // ---------------------------------------------------------------------------
    app_state_t state   = APP_STATE_MENU;
    bool        paused  = false;

    app_event_t ev;
    for (;;) {
        if (xQueueReceive(g_app_event_queue, &ev, portMAX_DELAY) != pdTRUE)
            continue;

        switch (ev.type) {

        case APP_EVENT_START_PLAYBACK:
            if (state != APP_STATE_MENU) break;
            paused = false;

            ui_menu_hide();
            lvgl_port_stop();

            hud_draw(ev.start.title, 75, false);
            player_start(ev.start.channel_id);
            hud_touch_poll_start();

            state = APP_STATE_PLAYING;
            ESP_LOGI(TAG, "→ PLAYING: %s", ev.start.title);
            break;

        case APP_EVENT_STOP_PLAYBACK:
            if (state != APP_STATE_PLAYING) break;

            hud_touch_poll_stop();
            player_stop();
            lvgl_port_resume();
            ui_menu_show();

            state  = APP_STATE_MENU;
            paused = false;
            ESP_LOGI(TAG, "→ MENU");
            break;

        case APP_EVENT_PAUSE_TOGGLE:
            if (state != APP_STATE_PLAYING) break;
            paused = !paused;
            player_set_paused(paused);
            break;

        case APP_EVENT_NEXT_TRACK:
            if (state != APP_STATE_PLAYING) break;
            // Stop and return to menu; future: advance to next channel.
            {
                app_event_t stop = { .type = APP_EVENT_STOP_PLAYBACK };
                xQueueSend(g_app_event_queue, &stop, 0);
            }
            break;

        case APP_EVENT_VOLUME:
            if (state != APP_STATE_PLAYING) break;
            player_set_volume(ev.volume.level);
            break;
        }
    }
}
