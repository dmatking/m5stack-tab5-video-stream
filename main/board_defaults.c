// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

// Default no-ops for boards without an LCD.
// Boards that have an LCD should override these with real implementations.
__attribute__((weak)) void board_lcd_sanity_test(void) {}
__attribute__((weak)) void board_lcd_fill(uint16_t color) { (void)color; }
__attribute__((weak)) int board_lcd_width(void) { return 0; }
__attribute__((weak)) int board_lcd_height(void) { return 0; }
__attribute__((weak)) void board_lcd_flush(void) {}
__attribute__((weak)) void board_lcd_clear(void) {}
__attribute__((weak)) void board_lcd_set_pixel_raw(int x, int y, uint16_t color) { (void)x; (void)y; (void)color; }
__attribute__((weak)) void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b) { (void)x; (void)y; (void)r; (void)g; (void)b; }
__attribute__((weak)) uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; return 0; }
__attribute__((weak)) uint16_t board_lcd_get_pixel_raw(int x, int y) { (void)x; (void)y; return 0; }
__attribute__((weak)) void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b) { (void)color; if (r) *r = 0; if (g) *g = 0; if (b) *b = 0; }
__attribute__((weak)) uint8_t *board_lcd_framebuffer(void)      { return NULL; }
__attribute__((weak)) size_t   board_lcd_framebuffer_size(void) { return 0; }
__attribute__((weak)) uint8_t *board_lcd_hw_framebuffer(void)   { return NULL; }
__attribute__((weak)) void     board_lcd_commit(void)           {}

__attribute__((weak)) esp_codec_dev_handle_t board_audio_init(void)    { return NULL; }
__attribute__((weak)) esp_codec_dev_handle_t board_audio_speaker(void) { return NULL; }
