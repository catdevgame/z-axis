#pragma once

// The game's frame (game_frame.cpp): what the rest of the game calls.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace aggressive
{
    // (game_frame.cpp)
    uint64_t levelEffectsFrame(ee::Float seconds);
    uint64_t playFrame(ee::Float seconds);
    int32_t updatePlayers(ee::Float seconds);
}
