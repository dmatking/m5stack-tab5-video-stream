// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Show the channel-list menu screen.  Fetches /info from the server in the
// background and populates the list; posts APP_EVENT_START_PLAYBACK when the
// user taps an entry.
void ui_menu_show(void);

// Remove the menu screen (call before entering playback mode).
void ui_menu_hide(void);
