#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge { struct SceneObject; }
namespace aggressive { struct TrafficCar; } // (level/traffic_car.h)

namespace aggressive
{
    // (effects.cpp)
    void driveTrafficCar(ee::Ptr<TrafficCar> car);
    int32_t moveObjectBox(ee::Ptr<zge::SceneObject> object, ee::Ptr<TrafficCar> car);
    int32_t updateLevelEffects(ee::Float dt);
}
