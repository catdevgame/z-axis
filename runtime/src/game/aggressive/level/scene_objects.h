#pragma once

// The level's scene objects as the game side asks after them (scene_objects.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace aggressive
{
    // (scene_objects.cpp)
    void levelFrame3(ee::Float seconds);
    int32_t objectDrawn(ee::Ptr<zge::SceneObject> object);
    uint32_t objectNode(ee::Address object);
    ee::Float triangleArea(ee::Ptr<zge::Vec4> corners);
    void turnCarWheels(ee::Ptr<zge::SceneObject> car, ee::Float circumference, ee::Float speed, ee::Float seconds);
}
