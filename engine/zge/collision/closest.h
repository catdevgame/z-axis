#pragma once

// Closest points: on a segment (closestPointOnSegment) and on a triangle
// (closestPointOnTriangle). The sphere face test and the level's setup take an
// edge's point nearest a point; the wall search (findWall) a wall
// face's point nearest the skater.
//
// The arithmetic of a segment is also here on its own, on plain numbers, for
// host code (the model of the level's setup, zge/native/collision_setup.h).

#include "ee/vu0.h"
#include "ee/readable.h"
#include "zge/math/types.h"

namespace zge
{
    // The point of segment a-b nearest `point`, seen along `direction`
    // (unit length; null: b - a normalised): `a` when the point is not
    // beyond a, `b` when it is beyond b, else b moved back along the
    // direction to the point's foot. Only the ends' order along the
    // direction is used: a point beside the segment gets its foot.
    // The host's: `direction` is b - a normalised, and VU0's registers stay as they are.
    Vec4 closestOnSegment(const Vec4 &a, const Vec4 &b, const Vec4 &point);
    // closestPointOnSegment (a, b, direction or 0, point, out).
    void closestPointOnSegment(const Vec4 *a, const Vec4 *b, const Vec4 *direction, const Vec4 *point, Vec4 *out);

    // The point of triangle (v0, v1, v2) nearest `point` (Eberly's
    // regions over v0 + s (v1 - v0) + t (v2 - v0)) and its squared
    // distance.
    // closestPointOnTriangle (point, v0, v1, v2, out point or 0, out squared distance or 0).
    void closestPointOnTriangle(const Vec4 *point, const Vec4 *v0, const Vec4 *v1, const Vec4 *v2, Vec4 *out,
                                ee::Float *squared);
}
