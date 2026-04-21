// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>

// Start video + audio playback for the given channel.
// Must be called after board_audio_init() and after lvgl_port_stop().
void player_start(const char *channel_id);

// Signal the player to stop and block until all tasks exit.
void player_stop(void);

// Adjust output volume (0-100).
void player_set_volume(int level);

// Toggle pause state.  While paused the video fetch/decode loop stalls and
// audio is filled with silence so the DMA clock keeps ticking.
void player_set_paused(bool paused);

// True while video and audio tasks are running.
bool player_is_running(void);
