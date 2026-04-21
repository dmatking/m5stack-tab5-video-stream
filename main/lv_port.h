// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
#include "esp_lcd_touch.h"

// Initialise LVGL, the DSI display driver, and the GT911 touch driver.
// Must be called after board_init().
void lv_port_init(void);

// Thread-safe wrappers — call before/after any LVGL API access from a task
// other than the LVGL port task.
void lv_port_lock(void);
void lv_port_unlock(void);

// Raw GT911 handle — use for direct touch polling when LVGL task is stopped.
esp_lcd_touch_handle_t lv_port_get_touch_handle(void);
