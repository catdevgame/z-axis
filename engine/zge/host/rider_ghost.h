#pragma once

// Ghost riding's side in the engine (runtime/ghost_rider.h): the rider's skinned parts as the class renderer draws
// them, noted for the track being made, and the ghost's pose of each part handed to the draw.

#include "zge/render/draw.h"
#include "runtime/ghost_rider.h"

#include <memory>

namespace zge::host
{
    // A skinned draw (drawFromRecord's, on every render path): when its node is the rider's (the skinned node of one of
    // the rider's model objects, or under it: a bike's too), its part's pose noted (its vertex and bone counts, the
    // node's world, the bones' palette as VU1 gets it), and the ghost's pose of the same part now returned (null: not
    // the rider's, or no ghost). Reads the game's RAM only.
    std::shared_ptr<const zx::ghost_rider::Ghost> noteRiderDraw(const SkinnedDraw &draw);
}
