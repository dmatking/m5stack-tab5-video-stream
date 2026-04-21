// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Playback HUD: draws simple pixel-art controls into the video letterbox
// areas of both hardware framebuffers, then polls touch to dispatch events.
//
// Layout (portrait framebuffer 720×1280, video letterboxed to 560×992 centre):
//
//   y 0..143   (144px) — top bar:   back button
//   y 144..1135         — sides:    right 80px = volume slider; left 80px empty
//   y 1136..1279(144px) — bot bar:  pause/play | next
//
#pragma once
#include <stdbool.h>

// Draw the full HUD into both hardware framebuffers.
// Call once before stopping LVGL and starting the video task.
void hud_draw(const char *title, int volume, bool paused);

// Redraw only the volume bar in both HW framebuffers.
void hud_update_volume(int volume);

// Redraw only the pause/play button in both HW framebuffers.
void hud_update_paused(bool paused);

// Start a FreeRTOS task that polls GT911 touch and posts app events to
// g_app_event_queue.  Must be called after lvgl_port_stop().
void hud_touch_poll_start(void);

// Signal the touch polling task to exit and wait for it.
void hud_touch_poll_stop(void);
