#pragma once

// The game's sound mode and play phase, and the sound system's end as the
// game calls it (sound_mode.cpp).

#include <cstdint>

namespace aggressive
{
    // The sound system's end: the sound mode left, every bank freed, the
    // emitters, sources and channels stopped; -1 (no sound mode).
    int32_t shutdownSound();
    // The game's sound mode (-1 before the game).
    int32_t gameModeOf();
    // The play phase.
    int32_t playPhase();
}
