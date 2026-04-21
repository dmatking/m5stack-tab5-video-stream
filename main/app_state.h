// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    APP_STATE_MENU,
    APP_STATE_PLAYING,
} app_state_t;

typedef enum {
    APP_EVENT_START_PLAYBACK,   // payload: start
    APP_EVENT_STOP_PLAYBACK,    // no payload
    APP_EVENT_PAUSE_TOGGLE,     // no payload
    APP_EVENT_NEXT_TRACK,       // no payload
    APP_EVENT_VOLUME,           // payload: volume.level (0-100)
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            char channel_id[64];
            char title[128];
        } start;
        struct {
            int level;
        } volume;
    };
} app_event_t;

// Published by UI and HUD; consumed by main app loop.
extern QueueHandle_t g_app_event_queue;
