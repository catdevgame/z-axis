#pragma once
// The particle effects' clock (effects_clock.cpp).

#include <cstdint>

namespace game::dm2
{
    // The effects' clock, whole milliseconds.
    int32_t effectsClock();
    // The clock moved on by a frame (a sixtieth of a second). Returns 1.
    uint32_t advanceEffectsClock();
    // Every affector object's first word: empty.
    void affectorNothing();
}
