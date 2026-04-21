// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// HUD for landscape viewing.
//
// The display panel is physically portrait (720×1280) but the device sits
// landscape.  Physical↔landscape mapping:
//   landscape (lx, ly)  →  portrait (px=ly,  py=1279-lx)
//   portrait  (px, py)  →  landscape (lx=1279-py, ly=px)
//
// Video occupies the landscape center (lx=144..1135, ly=80..639 = 992×560).
// HUD regions in landscape:
//   Back bar:   ly=0..79,    lx=0..1279  → portrait X=0..79,    all Y
//   Ctrl bar:   ly=640..719, lx=0..1279  → portrait X=640..719, all Y
//   Volume bar: lx=1136..1279,ly=80..639 → portrait Y=0..143,   X=80..639
//   Left gap:   lx=0..143,  ly=80..639  → portrait Y=1136..1279, X=80..639

#include "hud.h"
#include "app_state.h"
#include "board_interface.h"
#include "lv_port.h"

#include <stdatomic.h>
#include <stdlib.h>
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HUD";

// Physical framebuffer (portrait)
#define FB_W  720
#define FB_H  1280

// Landscape HUD regions (landscape X=lx, Y=ly)
#define BACK_LY1    0
#define BACK_LY2    79

#define CTRL_LY1    640
#define CTRL_LY2    719

#define VOL_LX1     1136
#define VOL_LX2     1279

#define VID_LX1     144
#define VID_LX2     1135
#define VID_LY1     80
#define VID_LY2     639

// ---------------------------------------------------------------------------
// RGB565 palette
// ---------------------------------------------------------------------------
#define C_BG        0x18C3u   // dark blue-gray
#define C_BACK      0xFC00u   // orange
#define C_PLAY      0x07E0u   // green
#define C_PAUSE     0xFFE0u   // yellow
#define C_NEXT      0x001Fu   // blue
#define C_VOL_FILL  0x07E0u   // green
#define C_WHITE     0xFFFFu

// ---------------------------------------------------------------------------
// Drawing helpers — landscape coords → portrait physical framebuffer
//
// lrect(lx1, ly1, lx2, ly2, color):
//   landscape rect [lx1..lx2] × [ly1..ly2]
//   portrait:  X=[ly1..ly2], Y=[(1279-lx2)..(1279-lx1)]
// ---------------------------------------------------------------------------

static void lrect(uint16_t *fb, int lx1, int ly1, int lx2, int ly2, uint16_t color)
{
    int py_lo = FB_H - 1 - lx2;
    int py_hi = FB_H - 1 - lx1;
    int px_lo = ly1;
    int px_hi = ly2;
    if (py_lo < 0) py_lo = 0;
    if (py_hi >= FB_H) py_hi = FB_H - 1;
    if (px_lo < 0) px_lo = 0;
    if (px_hi >= FB_W) px_hi = FB_W - 1;
    for (int py = py_lo; py <= py_hi; py++)
        for (int px = px_lo; px <= px_hi; px++)
            fb[py * FB_W + px] = color;
}

// Right-pointing triangle ▶ centered at landscape (lcx, lcy), half-size.
static void ltri_right(uint16_t *fb, int lcx, int lcy, int half, uint16_t color)
{
    for (int dly = -half; dly <= half; dly++) {
        int w = half - abs(dly);
        for (int dlx = 0; dlx < w; dlx++) {
            int px = lcy + dly;
            int py = FB_H - 1 - (lcx + dlx);
            if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                fb[py * FB_W + px] = color;
        }
    }
}

// Left-pointing triangle ◄ centered at landscape (lcx, lcy), half-size.
static void ltri_left(uint16_t *fb, int lcx, int lcy, int half, uint16_t color)
{
    for (int dly = -half; dly <= half; dly++) {
        int w = half - abs(dly);
        for (int dlx = -w + 1; dlx <= 0; dlx++) {
            int px = lcy + dly;
            int py = FB_H - 1 - (lcx + dlx);
            if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                fb[py * FB_W + px] = color;
        }
    }
}

// Full HUD draw into one framebuffer.
static void draw_fb(uint16_t *fb, int volume, bool paused)
{
    // Back bar (landscape top, ly=0..79, lx=0..1279)
    lrect(fb, 0, BACK_LY1, 1279, BACK_LY2, C_BG);
    lrect(fb, 20, 16, 200, 63, C_BACK);
    ltri_left(fb, 110, 40, 20, C_WHITE);

    // Left gap (landscape left of video)
    lrect(fb, 0, VID_LY1, VID_LX1 - 1, VID_LY2, C_BG);

    // Volume bar (landscape right margin)
    lrect(fb, VOL_LX1, VID_LY1, VOL_LX2, VID_LY2, C_BG);
    int vol_h = (volume * (VID_LY2 - VID_LY1)) / 100;
    if (vol_h > 0)
        lrect(fb, VOL_LX1 + 6, VID_LY2 - vol_h, VOL_LX2 - 6, VID_LY2, C_VOL_FILL);

    // Control bar (landscape bottom, ly=640..719, lx=0..1279)
    lrect(fb, 0, CTRL_LY1, 1279, CTRL_LY2, C_BG);

    int ctrl_mid_lx = 640;   // landscape X centre of ctrl bar
    int ctrl_lcy    = (CTRL_LY1 + CTRL_LY2) / 2;  // landscape Y centre = 679

    // Pause/Play: landscape left half of ctrl bar (lx=0..580)
    uint16_t pc = paused ? C_PAUSE : C_PLAY;
    lrect(fb, 20, CTRL_LY1 + 8, 560, CTRL_LY2 - 8, pc);
    int pp_lcx = 280;   // centre of pause/play region in landscape X
    if (paused) {
        ltri_right(fb, pp_lcx, ctrl_lcy, 24, C_BG);
    } else {
        // Two vertical pause bars || (in landscape X direction)
        lrect(fb, pp_lcx - 36, CTRL_LY1 + 16, pp_lcx - 12, CTRL_LY2 - 16, C_BG);
        lrect(fb, pp_lcx + 12, CTRL_LY1 + 16, pp_lcx + 36, CTRL_LY2 - 16, C_BG);
    }

    // Next button: landscape right half (lx=660..1200)
    lrect(fb, 660, CTRL_LY1 + 8, 1200, CTRL_LY2 - 8, C_NEXT);
    int nx_lcx = 900;
    ltri_right(fb, nx_lcx - 20, ctrl_lcy, 20, C_WHITE);
    lrect(fb, nx_lcx + 20, CTRL_LY1 + 16, nx_lcx + 36, CTRL_LY2 - 16, C_WHITE);
}

static void draw_both(int volume, bool paused)
{
    for (int i = 0; i < board_lcd_num_hw_fbs(); i++) {
        uint16_t *fb = (uint16_t *)board_lcd_hw_fb(i);
        if (fb) draw_fb(fb, volume, paused);
        board_lcd_sync_hw_fb(i);
    }
}

static void update_vol_both(int volume)
{
    for (int i = 0; i < board_lcd_num_hw_fbs(); i++) {
        uint16_t *fb = (uint16_t *)board_lcd_hw_fb(i);
        if (!fb) continue;
        lrect(fb, VOL_LX1, VID_LY1, VOL_LX2, VID_LY2, C_BG);
        int vol_h = (volume * (VID_LY2 - VID_LY1)) / 100;
        if (vol_h > 0)
            lrect(fb, VOL_LX1 + 6, VID_LY2 - vol_h, VOL_LX2 - 6, VID_LY2, C_VOL_FILL);
        board_lcd_sync_hw_fb(i);
    }
}

static void update_pause_both(bool paused)
{
    for (int i = 0; i < board_lcd_num_hw_fbs(); i++) {
        uint16_t *fb = (uint16_t *)board_lcd_hw_fb(i);
        if (!fb) continue;

        lrect(fb, 0, CTRL_LY1, 1279, CTRL_LY2, C_BG);

        int ctrl_lcy = (CTRL_LY1 + CTRL_LY2) / 2;
        uint16_t pc = paused ? C_PAUSE : C_PLAY;
        lrect(fb, 20, CTRL_LY1 + 8, 560, CTRL_LY2 - 8, pc);
        int pp_lcx = 280;
        if (paused) {
            ltri_right(fb, pp_lcx, ctrl_lcy, 24, C_BG);
        } else {
            lrect(fb, pp_lcx - 36, CTRL_LY1 + 16, pp_lcx - 12, CTRL_LY2 - 16, C_BG);
            lrect(fb, pp_lcx + 12, CTRL_LY1 + 16, pp_lcx + 36, CTRL_LY2 - 16, C_BG);
        }

        lrect(fb, 660, CTRL_LY1 + 8, 1200, CTRL_LY2 - 8, C_NEXT);
        int nx_lcx = 900;
        ltri_right(fb, nx_lcx - 20, ctrl_lcy, 20, C_WHITE);
        lrect(fb, nx_lcx + 20, CTRL_LY1 + 16, nx_lcx + 36, CTRL_LY2 - 16, C_WHITE);

        board_lcd_sync_hw_fb(i);
    }
}

// ---------------------------------------------------------------------------
// Public HUD API
// ---------------------------------------------------------------------------

void hud_draw(const char *title, int volume, bool paused)
{
    (void)title;
    draw_both(volume, paused);
    ESP_LOGI(TAG, "HUD drawn (vol=%d paused=%d)", volume, paused);
}

void hud_update_volume(int volume)  { update_vol_both(volume); }
void hud_update_paused(bool paused) { update_pause_both(paused); }

// ---------------------------------------------------------------------------
// Touch polling task
// ---------------------------------------------------------------------------

static _Atomic bool  s_poll_stop  = false;
static TaskHandle_t  s_poll_task  = NULL;
static int           s_cur_volume = 75;
static bool          s_cur_paused = false;

static void send_event(app_event_type_t type, int vol_level)
{
    if (!g_app_event_queue) return;
    app_event_t ev = { .type = type };
    if (type == APP_EVENT_VOLUME) ev.volume.level = vol_level;
    xQueueSend(g_app_event_queue, &ev, 0);
}

static void touch_poll_task(void *arg)
{
    (void)arg;
    esp_lcd_touch_handle_t tp = lv_port_get_touch_handle();
    bool was_touching = false;

    while (!atomic_load(&s_poll_stop)) {
        vTaskDelay(pdMS_TO_TICKS(60));

        if (!tp) continue;
        esp_lcd_touch_read_data(tp);

        uint16_t tx, ty;
        uint16_t strength;
        uint8_t  count = 0;
        esp_lcd_touch_get_coordinates(tp, &tx, &ty, &strength, &count, 1);

        bool touching = (count > 0);

        if (touching && !was_touching) {
            // Convert portrait physical (tx, ty) → landscape (lx, ly)
            int lx = FB_H - 1 - (int)ty;
            int ly = (int)tx;
            ESP_LOGI(TAG, "touch portrait(%d,%d) → landscape(%d,%d)", tx, ty, lx, ly);

            if (ly < BACK_LY2) {
                send_event(APP_EVENT_STOP_PLAYBACK, 0);

            } else if (ly >= CTRL_LY1) {
                // Control bar — split at landscape X=620
                if (lx < 620) {
                    s_cur_paused = !s_cur_paused;
                    hud_update_paused(s_cur_paused);
                    send_event(APP_EVENT_PAUSE_TOGGLE, 0);
                } else if (lx > 680) {
                    send_event(APP_EVENT_NEXT_TRACK, 0);
                }

            } else if (lx >= VOL_LX1 && ly >= VID_LY1 && ly <= VID_LY2) {
                // Volume: landscape ly (80..639) maps top=100% bottom=0%
                int vol = ((VID_LY2 - ly) * 100) / (VID_LY2 - VID_LY1);
                vol = vol < 0 ? 0 : vol > 100 ? 100 : vol;
                s_cur_volume = vol;
                hud_update_volume(vol);
                send_event(APP_EVENT_VOLUME, vol);
            }
        }

        was_touching = touching;
    }

    s_poll_task = NULL;
    vTaskDelete(NULL);
}

void hud_touch_poll_start(void)
{
    atomic_store(&s_poll_stop, false);
    xTaskCreate(touch_poll_task, "hud_touch", 4096, NULL, 6, &s_poll_task);
}

void hud_touch_poll_stop(void)
{
    atomic_store(&s_poll_stop, true);
    for (int i = 0; i < 10 && s_poll_task; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
}
