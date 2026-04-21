// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_codec_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c_master.h"

void board_init(void);
const char *board_get_name(void);
bool board_has_lcd(void);

// Optional: implement to visually verify the LCD on startup.
// A default no-op is provided by board_defaults.c for headless boards.
void board_lcd_sanity_test(void);

// Fill the LCD with a RGB565 color. No-op if no LCD.
void board_lcd_fill(uint16_t color);

// ---------------------------------------------------------------------------
// Display drawing API — framebuffer-based pixel access for graphics demos.
// Boards with an LCD should implement these. Weak no-op defaults are provided
// in board_defaults.c for headless boards.
// ---------------------------------------------------------------------------

// Display dimensions in pixels.
int board_lcd_width(void);
int board_lcd_height(void);

// Push the framebuffer contents to the display. Blocks until complete.
void board_lcd_flush(void);

// Clear the framebuffer to black.
void board_lcd_clear(void);

// Write a pre-packed pixel value (native format, already byte-swapped).
void board_lcd_set_pixel_raw(int x, int y, uint16_t color);

// Write an RGB888 pixel, converting to the display's native format.
void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b);

// Convert RGB888 to the display's native packed pixel format (RGB565).
uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b);

// Read a raw pixel value back from the framebuffer.
uint16_t board_lcd_get_pixel_raw(int x, int y);

// Extract RGB888 components from a raw pixel value.
void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b);

// Direct framebuffer access for zero-copy rendering (e.g. JPEG decode output).
// Returns the PSRAM render buffer and its size in bytes.
uint8_t *board_lcd_framebuffer(void);
size_t   board_lcd_framebuffer_size(void);

// Hardware (DPI) framebuffer — write directly, then call board_lcd_commit().
// Bypasses the render-buffer copy for lower latency.
uint8_t *board_lcd_hw_framebuffer(void);
void     board_lcd_commit(void);  // cache sync only, no memcpy

// Access all hardware framebuffers by index (0 or 1).
// Used by the HUD to pre-draw into both ping-pong buffers before video starts.
int      board_lcd_num_hw_fbs(void);
uint8_t *board_lcd_hw_fb(int idx);
void     board_lcd_sync_hw_fb(int idx);  // write-back cache for that buffer

// Draw a filled rectangle directly into a specific HW framebuffer.
// Handles cache writeback automatically.
void board_lcd_hw_fill_rect(int fb_idx, int x, int y, int w, int h, uint16_t color);

// ---------------------------------------------------------------------------
// Hardware handles — needed by the LVGL port and touch driver.
// NULL until board_init() has been called.
// ---------------------------------------------------------------------------
esp_lcd_panel_handle_t    board_lcd_panel_handle(void);
esp_lcd_panel_io_handle_t board_lcd_panel_io_handle(void);
i2c_master_bus_handle_t   board_i2c_bus_handle(void);

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

// Initialize audio hardware (I2S + ES8388 + speaker enable).
// Returns the speaker codec device handle, or NULL if no audio hardware.
// Must be called after board_init().
esp_codec_dev_handle_t board_audio_init(void);

// Return the speaker codec device handle (NULL until board_audio_init called).
esp_codec_dev_handle_t board_audio_speaker(void);
