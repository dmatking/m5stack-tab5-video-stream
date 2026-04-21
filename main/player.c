// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "player.h"
#include "board_interface.h"

#include <stdatomic.h>
#include <string.h>
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "PLAYER";

// Source frame dimensions — both must be divisible by 8 for HW JPEG decoder
#define SRC_W  992
#define SRC_H  560

// Display framebuffer dimensions (portrait)
#define DST_W  720
#define DST_H  1280

// Letterbox offsets
#define LBX  ((DST_W - SRC_H) / 2)   // 80
#define LBY  ((DST_H - SRC_W) / 2)   // 144

#define JPEG_IN_MAX     (128 * 1024)
#define PIPELINE_SLOTS  16

#define AUDIO_SAMPLE_RATE         16000
#define AUDIO_CHUNK_SAMPLES       1600
#define AUDIO_DMA_LATENCY_SAMPLES 7680

// Stop / pause flags
static _Atomic bool s_stop    = false;
static _Atomic bool s_paused  = false;

// A/V sync
static _Atomic bool  s_av_started    = false;
static _Atomic int64_t s_audio_samples = 0;
static int64_t         s_vid_start_us = 0;

// Pipeline
static uint8_t       *s_jpeg_in[PIPELINE_SLOTS];
static size_t         s_jpeg_in_size[PIPELINE_SLOTS];
static size_t         s_jpeg_len[PIPELINE_SLOTS];
static int            s_jpeg_ms[PIPELINE_SLOTS];
static QueueHandle_t  s_free_q;
static QueueHandle_t  s_ready_q;

// Task completion event group bits
#define BIT_VID_FETCH   BIT0
#define BIT_VID_DECODE  BIT1
#define BIT_AUDIO       BIT2
#define BITS_ALL        (BIT_VID_FETCH | BIT_VID_DECODE | BIT_AUDIO)

static EventGroupHandle_t s_done_eg = NULL;

// Current channel (set by player_start, read by tasks)
static char s_channel_id[64];

// ---------------------------------------------------------------------------
// HTTP helpers — persistent keep-alive client
// ---------------------------------------------------------------------------

typedef struct { uint8_t *buf; size_t cap; size_t written; } http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        size_t avail = ctx->cap - ctx->written;
        size_t n = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
        memcpy(ctx->buf + ctx->written, evt->data, n);
        ctx->written += n;
    }
    return ESP_OK;
}

static esp_http_client_handle_t make_client(http_ctx_t *ctx)
{
    static char seed[96];
    snprintf(seed, sizeof(seed), "http://%s:%d/", CONFIG_SERVER_IP, CONFIG_SERVER_PORT);
    esp_http_client_config_t cfg = {
        .url               = seed,
        .event_handler     = http_event_cb,
        .user_data         = ctx,
        .timeout_ms        = 5000,
        .buffer_size       = 4096,
        .keep_alive_enable = true,
    };
    return esp_http_client_init(&cfg);
}

static size_t fetch(esp_http_client_handle_t c, http_ctx_t *ctx,
                    uint8_t *buf, size_t cap, const char *url)
{
    ctx->buf = buf; ctx->cap = cap; ctx->written = 0;
    esp_http_client_set_url(c, url);
    esp_err_t err = esp_http_client_perform(c);
    int status    = esp_http_client_get_status_code(c);
    if (err != ESP_OK || status != 200) return 0;
    return ctx->written;
}

// ---------------------------------------------------------------------------
// Audio task
// ---------------------------------------------------------------------------

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)arg;

    uint8_t *u8  = heap_caps_malloc(AUDIO_CHUNK_SAMPLES, MALLOC_CAP_INTERNAL);
    int16_t *s16 = heap_caps_malloc(AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_SAMPLE_RATE,
    };
    esp_codec_dev_open(spk, &fs);
    esp_codec_dev_set_out_vol(spk, 75);

    while (!atomic_load(&s_av_started) && !atomic_load(&s_stop))
        vTaskDelay(pdMS_TO_TICKS(20));

    static http_ctx_t ctx;
    esp_http_client_handle_t client = make_client(&ctx);

    int sample_pos = 0;
    char url[128];

    while (!atomic_load(&s_stop)) {
        while (atomic_load(&s_paused) && !atomic_load(&s_stop))
            vTaskDelay(pdMS_TO_TICKS(50));
        if (atomic_load(&s_stop)) break;

        snprintf(url, sizeof(url), "http://%s:%d/audio/%s/%d/%d",
                 CONFIG_SERVER_IP, CONFIG_SERVER_PORT,
                 s_channel_id, sample_pos, AUDIO_CHUNK_SAMPLES);

        size_t got = fetch(client, &ctx, u8, AUDIO_CHUNK_SAMPLES, url);

        if (got == 0) {
            memset(s16, 0, AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
            got = AUDIO_CHUNK_SAMPLES;
        } else {
            for (size_t i = 0; i < got; i++)
                s16[i] = (int16_t)((int)u8[i] - 128) << 8;
        }

        esp_codec_dev_write(spk, s16, (int)(got * sizeof(int16_t)));
        atomic_fetch_add(&s_audio_samples, (int64_t)got);
        sample_pos += (int)got;
    }

    esp_codec_dev_close(spk);
    esp_http_client_cleanup(client);
    free(u8); free(s16);
    xEventGroupSetBits(s_done_eg, BIT_AUDIO);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Video fetch task
// ---------------------------------------------------------------------------

static void video_fetch_task(void *arg)
{
    (void)arg;
    static http_ctx_t ctx;
    esp_http_client_handle_t client = make_client(&ctx);
    char url[128];

    while (!atomic_load(&s_stop)) {
        while (atomic_load(&s_paused) && !atomic_load(&s_stop))
            vTaskDelay(pdMS_TO_TICKS(50));
        if (atomic_load(&s_stop)) break;

        uint8_t slot;
        if (xQueueReceive(s_free_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) continue;

        int ms = (int)((esp_timer_get_time() - s_vid_start_us) / 1000);

        snprintf(url, sizeof(url), "http://%s:%d/frame/%s/%d",
                 CONFIG_SERVER_IP, CONFIG_SERVER_PORT, s_channel_id, ms);

        size_t len = fetch(client, &ctx, s_jpeg_in[slot], s_jpeg_in_size[slot], url);

        if (len == 0) {
            xQueueSend(s_free_q, &slot, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!atomic_load(&s_av_started)) {
            s_vid_start_us = esp_timer_get_time();
            atomic_store(&s_av_started, true);
            ms = 0;
        }

        s_jpeg_len[slot] = len;
        s_jpeg_ms[slot]  = ms;
        xQueueSend(s_ready_q, &slot, portMAX_DELAY);
    }

    esp_http_client_cleanup(client);
    xEventGroupSetBits(s_done_eg, BIT_VID_FETCH);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Video decode task
// ---------------------------------------------------------------------------

static void video_decode_task(void *arg)
{
    (void)arg;

    jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t jpeg_out_size = 0;
    uint8_t *jpeg_out = (uint8_t *)jpeg_alloc_decoder_mem(
        SRC_W * SRC_H * 2, &out_cfg, &jpeg_out_size);

    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };
    jpeg_decoder_handle_t jpd;
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&eng_cfg, &jpd));

    ppa_client_handle_t ppa_srm;
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa_srm));

    TickType_t next_display = xTaskGetTickCount();
    int frame_num = 0;

    while (!atomic_load(&s_stop)) {
        uint8_t slot;
        if (xQueueReceive(s_ready_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (atomic_load(&s_stop)) {
            xQueueSend(s_free_q, &slot, 0);
            break;
        }

        jpeg_decode_cfg_t dec = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_used = 0;
        esp_err_t err = jpeg_decoder_process(jpd, &dec,
                                             s_jpeg_in[slot], s_jpeg_len[slot],
                                             jpeg_out, jpeg_out_size, &out_used);
        xQueueSend(s_free_q, &slot, portMAX_DELAY);

        if (err != ESP_OK) continue;

        uint8_t *backbuf = board_lcd_hw_framebuffer();
        if (!backbuf) backbuf = board_lcd_framebuffer();

        ppa_srm_oper_config_t srm = {
            .in  = { .buffer = jpeg_out, .pic_w = SRC_W, .pic_h = SRC_H,
                     .block_w = SRC_W, .block_h = SRC_H,
                     .block_offset_x = 0, .block_offset_y = 0,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = backbuf, .buffer_size = DST_W * DST_H * 2,
                     .pic_w = DST_W, .pic_h = DST_H,
                     .block_offset_x = LBX, .block_offset_y = LBY,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mode    = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(ppa_srm, &srm) == ESP_OK) {
            board_lcd_commit();
            frame_num++;
            ESP_LOGI(TAG, "frame %4d @%dms", frame_num, s_jpeg_ms[slot]);
        }

        vTaskDelayUntil(&next_display, pdMS_TO_TICKS(1000 / 20));
    }

    jpeg_del_decoder_engine(jpd);
    ppa_unregister_client(ppa_srm);
    xEventGroupSetBits(s_done_eg, BIT_VID_DECODE);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void player_start(const char *channel_id)
{
    strlcpy(s_channel_id, channel_id, sizeof(s_channel_id));

    atomic_store(&s_stop,        false);
    atomic_store(&s_paused,      false);
    atomic_store(&s_av_started,  false);
    atomic_store(&s_audio_samples, 0);

    s_vid_start_us = esp_timer_get_time();

    if (!s_done_eg) s_done_eg = xEventGroupCreate();
    xEventGroupClearBits(s_done_eg, BITS_ALL);

    // Allocate pipeline JPEG input buffers
    jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    for (int i = 0; i < PIPELINE_SLOTS; i++) {
        if (!s_jpeg_in[i]) {
            s_jpeg_in[i] = (uint8_t *)jpeg_alloc_decoder_mem(
                JPEG_IN_MAX, &in_cfg, &s_jpeg_in_size[i]);
        }
    }

    if (!s_free_q)  s_free_q  = xQueueCreate(PIPELINE_SLOTS, sizeof(uint8_t));
    if (!s_ready_q) s_ready_q = xQueueCreate(PIPELINE_SLOTS, sizeof(uint8_t));

    // Drain queues from any previous run
    uint8_t discard;
    while (xQueueReceive(s_free_q,  &discard, 0) == pdTRUE) {}
    while (xQueueReceive(s_ready_q, &discard, 0) == pdTRUE) {}
    for (uint8_t i = 0; i < PIPELINE_SLOTS; i++)
        xQueueSend(s_free_q, &i, 0);

    esp_codec_dev_handle_t spk = board_audio_speaker();

    xTaskCreatePinnedToCore(video_fetch_task,  "vid_fetch",  8192, NULL,  4, NULL, 1);
    xTaskCreatePinnedToCore(video_decode_task, "vid_decode", 8192, NULL,  5, NULL, 0);
    if (spk)
        xTaskCreatePinnedToCore(audio_task, "audio", 8192, spk, 15, NULL, 1);
    else
        xEventGroupSetBits(s_done_eg, BIT_AUDIO);  // no audio — mark done immediately

    ESP_LOGI(TAG, "playback started: %s", channel_id);
}

void player_stop(void)
{
    atomic_store(&s_stop, true);
    atomic_store(&s_paused, false);
    if (s_done_eg)
        xEventGroupWaitBits(s_done_eg, BITS_ALL, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "playback stopped");
}

void player_set_volume(int level)
{
    esp_codec_dev_handle_t spk = board_audio_speaker();
    if (spk) esp_codec_dev_set_out_vol(spk, level);
}

void player_set_paused(bool paused)
{
    atomic_store(&s_paused, paused);
}

bool player_is_running(void)
{
    if (!s_done_eg) return false;
    EventBits_t bits = xEventGroupGetBits(s_done_eg);
    return (bits & BITS_ALL) != BITS_ALL;
}
