#pragma once
// BMX XXX's spinning marker (fx.c; marker.cpp): a small pyramid, point
// down, turning about its axis.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::bmx
{
    // The marker drawn at `position` in `colour` (RGBA), `size` its scale
    // (its points are a unit apart). It turns once in 120 frames of the
    // effects' frame count.
    void drawSpinningMarker(ee::Ptr<zge::Vec4> position, ee::Ptr<uint8_t> colour, ee::Float size);
}
