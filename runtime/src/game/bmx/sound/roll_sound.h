#pragma once
// BMX XXX's rolling sounds (bmx.c, roll_sound.cpp): a bike's two sound
// sources, the tyres on the surface and the chain, which follow the rider.

#include "ee/plain.h"

#include <cstdint>

namespace game::bmx
{
    // A rolling sound started in the first free of the 16 slots, both its
    // sources high above the level. Returns the slot, or -1 with none free
    // or no source.
    int32_t startRollSound();
    // A slot's two sources stopped. (The slot is not marked free: the
    // sound's mode frees them all when play ends.)
    void stopRollSound(uint32_t slot);
    // A slot's sound for the frame: the tyres' sample by the surface
    // (`sound`: the surface's own sample in the low byte, its kind in the
    // next, two flags above), both sources' volume and pitch by the speed,
    // at `position`.
    void updateRollSound(uint32_t slot, uint32_t sound, ee::Address position, int32_t speed);
}
