#pragma once

// BMX XXX's path men (the level object types "PathMan" and "FastPathMan":
// the people who walk a level's paths): each one's own random numbers
// (pathman_random.cpp).

#include "ee/ee.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // A path man's tag record keeps its values in words and floats (the tag
    // kind 8), at these places.
    constexpr uint32_t kPathManTagWords = 8;
    constexpr uint32_t kPathManTagSeed = 0x60; // the state of its random numbers

    // The state of the path man's random numbers, and the same set.
    int32_t pathManSeed(ee::Ptr<zge::SceneObject> pathMan);
    void setPathManSeed(ee::Ptr<zge::SceneObject> pathMan, ee::Address seed);
    // The path man's next random number, 0 .. `range` (rounded to the
    // nearest whole number, so both ends can come up).
    int32_t pathManRandom(ee::Ptr<zge::SceneObject> pathMan, uint32_t range);
}
