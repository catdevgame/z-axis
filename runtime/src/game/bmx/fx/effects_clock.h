#pragma once
// The particle effects' clock (effects_clock.cpp).

#include <cstdint>

namespace game::bmx
{
    // The effects' clock, whole milliseconds.
    int32_t effectsClock();
    // The clock moved on by a frame; a frame counted (the effects' two tasks; each returns 1).
    uint32_t advanceEffectsClock();
    uint32_t countEffectsFrame();
    // Every affector object's first word: empty.
    void affectorNothing();
}
