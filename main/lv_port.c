// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// LVGL 9.x port for Tab5:
//   - Display: MIPI-DSI DPI, landscape logical display (1280×720) via 90°CW
//              software rotation on the physical portrait (720×1280) panel.
//   - Touch:   GT911-compatible controller in ST7123 combined IC at I2C 0x10.
//              GPIO 23 is the INT pin; RST is NC (PI4IOE manages power-on).

#include "lv_port.h"
#include "board_interface.h"

#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "LV_PORT";
static esp_lcd_touch_handle_t s_tp = NULL;

// Physical panel dimensions (portrait framebuffer)
#define LCD_PHYS_W  720
#define LCD_PHYS_H  1280

// Touch controller in ST7123 combined IC responds at 0x10 (confirmed by I2C scan).
// GT911 address macros don't cover this; set directly.
#define GT911_I2C_ADDR  0x10
#define GT911_INT_GPIO  GPIO_NUM_23

void lv_port_init(void)
{
    // ---- LVGL port task ------------------------------------------------
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity   = 0;
    port_cfg.task_priority   = 3;
    port_cfg.task_stack      = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    // ---- Display -------------------------------------------------------
    // sw_rotate=true: esp_lvgl_port uses PPA to rotate 720×1280 → 1280×720
    // lv_display_set_rotation(ROTATION_90) makes the logical display 1280×720.
    // avoid_tearing is incompatible with sw_rotate per esp_lvgl_port notes.
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = board_lcd_panel_io_handle(),
        .panel_handle  = board_lcd_panel_handle(),
        .buffer_size   = LCD_PHYS_W * LCD_PHYS_H,
        .double_buffer = true,
        .hres          = LCD_PHYS_W,
        .vres          = LCD_PHYS_H,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .sw_rotate   = true,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = false,
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return;
    }
    // Rotate 90°CW: logical display becomes 1280×720 (landscape).
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);

    // ---- Touch ----------------------------------------------------------
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr              = GT911_I2C_ADDR,
        .scl_speed_hz          = 400000,
        .control_phase_bytes   = 1,
        .dc_bit_offset         = 0,
        .lcd_cmd_bits          = 16,
        .lcd_param_bits        = 8,
        .flags.disable_control_phase = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(
        board_i2c_bus_handle(), &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = LCD_PHYS_W,
        .y_max        = LCD_PHYS_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GT911_INT_GPIO,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    esp_err_t tp_err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp);
    if (tp_err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 init failed (0x%x) — touch disabled", tp_err);
        s_tp = NULL;
    } else {
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = s_tp,
        };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed — touch disabled");
        }
    }

    ESP_LOGI(TAG, "LVGL port ready (logical 1280×720, GT911 @ 0x%02x)", GT911_I2C_ADDR);
}

void lv_port_lock(void)   { lvgl_port_lock(0); }
void lv_port_unlock(void) { lvgl_port_unlock(); }

esp_lcd_touch_handle_t lv_port_get_touch_handle(void) { return s_tp; }
