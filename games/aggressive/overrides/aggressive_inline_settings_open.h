#pragma once

// Aggressive Inline's side of Settings... by the engine's own path
// (zge_settings_open.h): the game's state read by its names, and the pause
// its own Start makes (aggressive_inline_settings_open.cpp; the request's
// handler and the page's natives are aggressive_inline_settings.cpp's).

#include "machine.h"

#include <cstdint>

namespace aggressive_inline::settings_open
{
    // From the game's mode word (g_gameMode): in a level (play or paused);
    // play runs (not the pause menu, a tip or a message box).
    bool inLevel(const uint8_t *rdram);
    bool playing(const uint8_t *rdram);
    // The play frame: the level's mode is playing; the pause as
    // checkPauseButtons makes it for a Start of player one.
    bool pausable(const uint8_t *rdram);
    void pause(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    // The pause menu's group is up (Pause.Show or Tutorial.Show ended): its
    // Settings page.
    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t pauseGroup);
    // checkPauseButtons (the play mode's update calls it each frame) wrapped:
    // the original first, then a pending request. False if not installed.
    bool installPlayFrameHook(PS2Machine &runtime);
}
