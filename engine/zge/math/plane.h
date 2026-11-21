#pragma once

// Planes (plane.cpp): a plane is a Vec4, its unit normal (x, y, z) and
// w = -(normal . a point on it).

#include "ee/ee.h"
#include "types.h"

namespace zge
{
    // planeDistance: point's signed distance from the plane.
    ee::Float planeDistance(ee::Ptr<Vec4> plane, ee::Ptr<Vec4> point);
    // planeThrough: the plane through point with the given normal. Returns plane.
    ee::Ptr<Vec4> planeThrough(ee::Ptr<Vec4> plane, ee::Ptr<Vec4> point, ee::Ptr<Vec4> normal);
}
