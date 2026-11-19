#pragma once
// DM2's water wake (water_wake.cpp): the rings a body leaves on water.

#include "ee/ee.h"
#include "zge/math/types.h"

namespace game::dm2
{
    // A body on water: the point on the water and the body's velocity over
    // a step. Some calls leave rings there (the rider's landing in water, a
    // boat's move, a dynamics object's touch of a water face).
    void startWaterWake(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec4> velocity);
}
