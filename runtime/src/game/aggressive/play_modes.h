#pragma once

// The play modes (play_modes.cpp): what the rest of the game calls.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace aggressive
{
    // (play_modes.cpp)
    // enterGameMode: the level changes play mode (a PlayModeId).
    int32_t enterGameMode(uint32_t index);
    int32_t playAgain();
}
