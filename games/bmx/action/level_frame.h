#pragma once
// BMX XXX's level state's frame and the camera states' frame functions for
// play and for a game screen (action.c, level_frame.cpp).

#include <cstdint>

namespace game::bmx
{
    // The level state's frame (the game state's frame table). Always 0.
    int32_t levelStateFrame();
    // The frame functions of play and of the screen state (not 0: the
    // frame ended early: the game paused, or the state changed).
    int32_t playStateFrame();
    int32_t screenStateFrame();
}
