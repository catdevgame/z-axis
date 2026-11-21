#pragma once
// BMX XXX's input record: a button record with the four face buttons'
// levels behind it, now and a frame ago. A pad's record (PadInput::buttons
// and its face levels), a rider's (Rider::input and the 0x20 bytes after
// it) and a weapon's (weapon.c) all have this shape; play's frame copies the
// pad's into the rider's or the weapon's, and a replay records and plays it.

#include "bmx/rider/rider.h"
#include "ee/ee.h"

#include <cstddef>

namespace game::bmx
{
    struct InputRecord
    {
        rider::ButtonRecord buttons;
        ee::Float levels[rider::kPadFaceLevels];         // +0x28 the face buttons' levels (PadInput::faceLevels)
        ee::Float previousLevels[rider::kPadFaceLevels]; // +0x38
    };
    static_assert(offsetof(InputRecord, levels) == 0x28 && offsetof(InputRecord, previousLevels) == 0x38);
}
