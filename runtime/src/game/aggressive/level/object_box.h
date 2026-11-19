#pragma once

// The box a model's bones span, for what collides with an NPC or a traffic car
// (object_box.cpp; dynamics_objects.cpp's contacts ask for it).

#include "aggressive/level/traffic_car.h"
#include "aggressive/npc.h"
#include "ee/ee.h"
#include "zge/collision/dynamics.h"

#include <cstdint>

namespace aggressive
{
    // (object_box.cpp)
    int32_t npcBox(ee::Ptr<Npc> npc, ee::Ptr<zge::OrientedBox> box);
    int32_t trafficCarBox(ee::Ptr<TrafficCar> car, ee::Ptr<zge::OrientedBox> box);
}
