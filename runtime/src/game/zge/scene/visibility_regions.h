#pragma once
// The level's view regions (zge/scene/visibility_regions.cpp; the Region
// record: zge/render/view_regions.h).

#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/render/view_regions.h"

#include <cstdint>

namespace zge
{
    ee::Ptr<Region> newRegion(uint32_t hiddenCount);
    uint32_t freeAllRegions();
    uint32_t finishRegions();
    ee::Ptr<Region> findRegionAt(ee::Ptr<const Vec3> position);
    // ZGE2's findRegionAt (and markRegionsOutOfView) are given a box where
    // ZGE3's are given a point: the argument as the box it is there.
    inline ee::Ptr<Box> boxGivenAsPoint(ee::Ptr<const Vec3> position) { return position.cast<Box>(); }
    // ZGE2's findRegion (transform.cpp) is given a point where ZGE3's is given a box: the point is the
    // box's first corner.
    inline ee::Ptr<const Vec3> pointGivenAsBox(ee::Ptr<Box> box) { return box.cast<const Vec3>(); }
    ee::Ptr<Region> findRegionForMesh(uint32_t meshHash);
    ee::Ptr<Region> markRegionsOutOfView(ee::Ptr<const Vec3> position);
}
