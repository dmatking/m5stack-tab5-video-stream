// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// WiFi connect → fetch JPEG frames from Pi server → HW decode → display.
//
// The server produces 1280×720 landscape frames.  The display is 720×1280
// portrait.  Frames are rotated 90° CW in software so the full frame fills
// the screen — hold the device in landscape to view normally.
//
// Audio and video both reference wall clock from task start.  True I2S-counter
// A/V sync is deferred — HTTP audio chunks arrive in 100 ms bursts so the
// sample counter is too coarse to drive video timestamps smoothly.

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_codec_dev.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "board_interface.h"

static const char *TAG = "APP";

// WiFi event group bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    10

static EventGroupHandle_t s_wifi_eg;
static int s_wifi_retries = 0;

// Source (server) frame dimensions — must be divisible by 8 for HW JPEG decoder
#define SRC_W  992
#define SRC_H  560

// Display framebuffer dimensions (portrait: 720 wide × 1280 tall)
#define DST_W  720
#define DST_H  1280

// After 90° CW rotation SRC becomes SRC_H × SRC_W in framebuffer coords.
// Letterbox offsets centre it in DST_W × DST_H.
#define LBX  ((DST_W - SRC_H) / 2)   // (720 - 560) / 2 = 80
#define LBY  ((DST_H - SRC_W) / 2)   // (1280 - 992) / 2 = 144

// JPEG input buffer size — 128 KB is generous for 992×560 at q:v 25
#define JPEG_IN_MAX  (128 * 1024)

// Video pipeline: 2 ping-pong JPEG input slots.
// Fetch fills slot N while decode+rotate processes slot N-1.
#define PIPELINE_SLOTS  2

// Pipeline globals — allocated in app_main, used by both video tasks.
static uint8_t         *s_jpeg_in[PIPELINE_SLOTS];
static size_t           s_jpeg_in_size[PIPELINE_SLOTS];
static size_t           s_jpeg_len[PIPELINE_SLOTS];
static int              s_jpeg_ms[PIPELINE_SLOTS];
static QueueHandle_t    s_free_q;
static QueueHandle_t    s_ready_q;
static int64_t          s_vid_start_us;

// Audio: 16 kHz mono u8 PCM.  100 ms chunks → server round-trip fits in DMA buffer.
#define AUDIO_SAMPLE_RATE         16000
#define AUDIO_CHUNK_SAMPLES       1600   // 100 ms per HTTP fetch
#define AUDIO_DMA_LATENCY_SAMPLES 7680   // 8 desc × 960 frames = 480 ms in DMA

// Shared A/V sync clock — written by audio task, read by video task.
// Counts samples accepted by DMA (not yet played; subtract latency for playback pos).
static _Atomic int64_t s_audio_samples = 0;

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < WIFI_MAX_RETRIES) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_wifi_retries, WIFI_MAX_RETRIES);
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
    // esp_hosted MUST be initialized before anything in the WiFi stack.
    ESP_LOGI(TAG, "Initializing ESP32-C6 co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor ready");

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

    ESP_LOGI(TAG, "Connecting to '%s'...", CONFIG_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) return true;
    ESP_LOGE(TAG, "Failed to connect to WiFi");
    return false;
}

// ---------------------------------------------------------------------------
// HTTP frame fetch
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   written;
} http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        size_t avail = ctx->capacity - ctx->written;
        size_t n = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
        memcpy(ctx->buf + ctx->written, evt->data, n);
        ctx->written += n;
    }
    return ESP_OK;
}

// Fetches /frame/<channel>/<ms> into jpeg_buf, returns number of bytes
// received (0 on error).
static size_t fetch_frame(uint8_t *jpeg_buf, size_t buf_capacity, int ms)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/frame/%s/%d",
             CONFIG_SERVER_IP, CONFIG_SERVER_PORT, CONFIG_CHANNEL, ms);

    http_ctx_t ctx = { .buf = jpeg_buf, .capacity = buf_capacity, .written = 0 };

    esp_http_client_config_t hcfg = {
        .url           = url,
        .event_handler = http_event_cb,
        .user_data     = &ctx,
        .timeout_ms    = 10000,
        .buffer_size   = 8192,
    };

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch_frame(%d) err=%s status=%d", ms,
                 esp_err_to_name(err), status);
        return 0;
    }
    return ctx.written;
}

// ---------------------------------------------------------------------------
// Audio fetch + task
// ---------------------------------------------------------------------------

static size_t fetch_audio(uint8_t *buf, size_t buf_cap, int start, int count)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/audio/%s/%d/%d",
             CONFIG_SERVER_IP, CONFIG_SERVER_PORT, CONFIG_CHANNEL, start, count);

    http_ctx_t ctx = { .buf = buf, .capacity = buf_cap, .written = 0 };
    esp_http_client_config_t hcfg = {
        .url           = url,
        .event_handler = http_event_cb,
        .user_data     = &ctx,
        .timeout_ms    = 5000,
        .buffer_size   = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch_audio(%d,%d) err=%s status=%d", start, count,
                 esp_err_to_name(err), status);
        return 0;
    }
    return ctx.written;
}

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)arg;

    uint8_t *u8_buf  = heap_caps_malloc(AUDIO_CHUNK_SAMPLES, MALLOC_CAP_INTERNAL);
    int16_t *s16_buf = heap_caps_malloc(AUDIO_CHUNK_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_INTERNAL);
    assert(u8_buf && s16_buf);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    int ret = esp_codec_dev_open(spk, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        vTaskDelete(NULL);
    }
    esp_codec_dev_set_out_vol(spk, 75);

    int sample_pos = 0;
    ESP_LOGI(TAG, "Audio task running @ %d Hz, chunk=%d samples",
             AUDIO_SAMPLE_RATE, AUDIO_CHUNK_SAMPLES);

    while (1) {
        size_t got = fetch_audio(u8_buf, AUDIO_CHUNK_SAMPLES,
                                 sample_pos, AUDIO_CHUNK_SAMPLES);
        if (got == 0) {
            // EOF or transient error — write silence so the DMA paces the A/V clock
            // at real time rather than stalling s_audio_samples.
            memset(s16_buf, 0, AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
            got = AUDIO_CHUNK_SAMPLES;
        } else {
            // u8 PCM (0-255, centre=128) → s16 PCM
            for (size_t i = 0; i < got; i++)
                s16_buf[i] = (int16_t)((int)u8_buf[i] - 128) << 8;
        }
        esp_codec_dev_write(spk, s16_buf, (int)(got * sizeof(int16_t)));
        atomic_fetch_add(&s_audio_samples, (int64_t)got);
        sample_pos += (int)got;
    }
}

// ---------------------------------------------------------------------------
// Video pipeline: fetch task (producer) + decode task (consumer)
// ---------------------------------------------------------------------------

static void video_fetch_task(void *arg)
{
    ESP_LOGI(TAG, "Video fetch task running");

    while (1) {
        uint8_t slot;
        xQueueReceive(s_free_q, &slot, portMAX_DELAY);

        int ms = (int)((esp_timer_get_time() - s_vid_start_us) / 1000);

        size_t len = fetch_frame(s_jpeg_in[slot], s_jpeg_in_size[slot], ms);
        if (len == 0) {
            // Server not ready — return slot and back off
            xQueueSend(s_free_q, &slot, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        s_jpeg_len[slot] = len;
        s_jpeg_ms[slot]  = ms;
        xQueueSend(s_ready_q, &slot, portMAX_DELAY);
    }
}

static void video_decode_task(void *arg)
{
    // Allocate DMA-aligned JPEG output buffer for SRC_W × SRC_H RGB565
    jpeg_decode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_out_size = 0;
    uint8_t *jpeg_out = (uint8_t *)jpeg_alloc_decoder_mem(
        SRC_W * SRC_H * 2, &out_cfg, &jpeg_out_size);
    assert(jpeg_out && "JPEG output buffer allocation failed");

    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 1000 };
    jpeg_decoder_handle_t jpd;
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&eng_cfg, &jpd));

    ppa_client_handle_t ppa_srm;
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa_srm));

    uint8_t *backbuf = board_lcd_hw_framebuffer();
    if (!backbuf) backbuf = board_lcd_framebuffer();
    assert(backbuf);

    // Black borders stay black for the lifetime of the task — clear once.
    memset(backbuf, 0, DST_W * DST_H * 2);

    int frame_num = 0;
    ESP_LOGI(TAG, "Video decode task running (letterbox %dx%d in %dx%d, offset %d,%d)",
             SRC_H, SRC_W, DST_W, DST_H, LBX, LBY);

    while (1) {
        int64_t t_wait0 = esp_timer_get_time();
        uint8_t slot;
        xQueueReceive(s_ready_q, &slot, portMAX_DELAY);
        int64_t t_wait = esp_timer_get_time() - t_wait0;

        int    ms       = s_jpeg_ms[slot];
        size_t jpeg_len = s_jpeg_len[slot];

        // --- Decode ---
        int64_t t0 = esp_timer_get_time();
        jpeg_decode_cfg_t dec = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_used = 0;
        esp_err_t err = jpeg_decoder_process(jpd, &dec,
                                             s_jpeg_in[slot], jpeg_len,
                                             jpeg_out, jpeg_out_size, &out_used);
        int64_t t_decode = esp_timer_get_time() - t0;

        // Input slot no longer needed — release immediately so fetch can reuse it
        xQueueSend(s_free_q, &slot, portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
            continue;
        }

        // --- Rotate 90° CW via PPA hardware: src(1280×720) → dst(720×1280) ---
        t0 = esp_timer_get_time();
        ppa_srm_oper_config_t srm = {
            .in = {
                .buffer         = jpeg_out,
                .pic_w          = SRC_W,
                .pic_h          = SRC_H,
                .block_w        = SRC_W,
                .block_h        = SRC_H,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer         = backbuf,
                .buffer_size    = DST_W * DST_H * 2,
                .pic_w          = DST_W,
                .pic_h          = DST_H,
                .block_offset_x = LBX,
                .block_offset_y = LBY,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x        = 1.0f,
            .scale_y        = 1.0f,
            .mirror_x       = false,
            .mirror_y       = false,
            .mode           = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t ppa_err = ppa_do_scale_rotate_mirror(ppa_srm, &srm);
        int64_t t_rotate = esp_timer_get_time() - t0;

        if (ppa_err != ESP_OK) {
            ESP_LOGW(TAG, "PPA rotate failed: %s", esp_err_to_name(ppa_err));
            continue;
        }

        frame_num++;
        ESP_LOGI(TAG, "frame %4d @%dms | wait=%lldms dec=%lldms rot=%lldms",
                 frame_num, ms,
                 t_wait / 1000, t_decode / 1000, t_rotate / 1000);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "M5Stack Tab5 video stream starting");
    board_init();

    if (!wifi_init()) {
        ESP_LOGE(TAG, "WiFi failed — halting");
        while (1) vTaskDelay(portMAX_DELAY);
    }

    // Video pipeline setup — allocate ping-pong JPEG input buffers in PSRAM
    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    for (int i = 0; i < PIPELINE_SLOTS; i++) {
        s_jpeg_in[i] = (uint8_t *)jpeg_alloc_decoder_mem(JPEG_IN_MAX, &in_cfg,
                                                          &s_jpeg_in_size[i]);
        assert(s_jpeg_in[i] && "JPEG pipeline buffer allocation failed");
    }
    s_free_q  = xQueueCreate(PIPELINE_SLOTS, sizeof(uint8_t));
    s_ready_q = xQueueCreate(PIPELINE_SLOTS, sizeof(uint8_t));
    for (uint8_t i = 0; i < PIPELINE_SLOTS; i++)
        xQueueSend(s_free_q, &i, 0);

    s_vid_start_us = esp_timer_get_time();

    // Fetch: core 1, priority 4 — below audio (15), shares core with audio task
    xTaskCreatePinnedToCore(video_fetch_task,  "vid_fetch",  8192, NULL, 4, NULL, 1);
    // Decode: core 0, priority 5 — exclusive core for JPEG HW + PPA
    xTaskCreatePinnedToCore(video_decode_task, "vid_decode", 8192, NULL, 5, NULL, 0);

    // Audio: core 1, priority 15 — must preempt video to avoid underruns
    esp_codec_dev_handle_t spk = board_audio_init();
    if (spk) {
        xTaskCreatePinnedToCore(audio_task, "audio", 8192, spk, 15, NULL, 1);
    } else {
        ESP_LOGW(TAG, "No audio hardware — running video only");
    }
}
