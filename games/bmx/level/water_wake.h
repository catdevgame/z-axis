#pragma once
// BMX XXX's water wake (water_wake.cpp): the rings a body leaves on water.

#include "ee/ee.h"
#include "zge/math/types.h"

namespace game::bmx
{
    // A body on a water face: the point on the water and the body's
    // velocity over a frame. Some calls leave rings there.
    void startWaterWake(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec4> velocity);
}
