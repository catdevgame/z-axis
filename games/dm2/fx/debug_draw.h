#pragma once

// DM2's debug draws (fx.c; debug_draw.cpp): what is left of them in the
// release.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::dm2
{
    // A marker at a point: a four-sided solid `size` big in the colour,
    // untextured, turning about the upright once every two seconds.
    // (The row's name is the one it was given before its body was read:
    // it draws no axes.)
    void debugDrawAxes(ee::Ptr<zge::Vec4> position, ee::Ptr<uint8_t> colour, ee::Float size);
    // The debug draw's sphere: empty in the release.
    void inlineFxNoopD(ee::Float radius, const ee::Float *centre, const uint8_t *colour);
    // Two functions the level's start calls: empty in the release.
    void fxStub();
    void fxStubB();
}
