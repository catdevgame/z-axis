#pragma once
// The traffic cars' path choice (traffic_cars.cpp), which DM2's boats share:
// at a segment's end, the segment a follower goes on to.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace zge
{
    struct Follower;
    struct SceneObject;
}

namespace game::dm2
{
    // chooseNextSegment's answer when no exit is there.
    constexpr int32_t kNoSegment = -1;
    // The name hash of the segment the follower goes on to at its segment's
    // end (the start's exits going backwards), or kNoSegment: an exit whose
    // start's tags hold the object's name, else the only one, else one drawn
    // by the object's weights (three floats of its component from `weights`,
    // its random word after them).
    int32_t chooseNextSegment(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Follower> follower, uint32_t backwards, uint32_t words,
                              uint32_t weights);
}
