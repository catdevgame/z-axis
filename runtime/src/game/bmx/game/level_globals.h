#pragma once
// The level's settings on its "Globals" scene point (bmx.c,
// level_globals.cpp): the fog's depth and the cameras' far clip, from the
// point's tags, with the ambient and fog colours. BMX XXX's own code.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace game::bmx
{
    // The value into `out` (1), or nothing when the level has no Globals
    // point (0).
    uint32_t levelAmbientColour(ee::Address colour);
    uint32_t levelFogColour(ee::Address colour);
    uint32_t levelFogDepth(ee::Float *depth);
    uint32_t levelFarClip(ee::Float *distance);
}
