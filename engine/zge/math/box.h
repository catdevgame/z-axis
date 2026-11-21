#pragma once

// Axis-aligned boxes as the engine keeps them (a node's bounds, a part's,
// the collision world's sectors): two corners, w unused.

#include "types.h"
#include "ee/readable.h"

namespace zge
{
    struct Box
    {
        Vec4 low;  // +0x00
        Vec4 high; // +0x10
    };
    static_assert(sizeof(Box) == 0x20);

    // A bounding sphere as a node keeps it: the centre (its w holds the
    // radius too, as sphereFromBox writes it), then the radius.
    struct BoundingSphere
    {
        Vec4 centre;      // +0x00
        ee::Float radius; // +0x10
    };
    static_assert(offsetof(BoundingSphere, radius) == 0x10);

    // boxUnion (zge/math/box.cpp): the box around two boxes, into out (which may be either). Returns out.
    ee::Ptr<Box> boxUnion(ee::Ptr<Box> out, ee::Ptr<Box> a, ee::Ptr<Box> b);

    // boxTransform: the box around `box`'s eight corners transformed by m
    // (row vectors, as points), on VU0. Returns out.
    ee::Ptr<Box> boxTransform(ee::Ptr<Box> out, ee::Ptr<Box> box, ee::Ptr<Matrix> m);

    // boxCentre: the box's centre (x, y, z). Returns out.
    ee::Ptr<Vec3> boxCentre(ee::Ptr<Vec3> out, const Box *box);

    // boxContainsPoint (zge/utilities.cpp): whether point lies in box (its faces
    // included).
    bool boxContainsPoint(const Box *box, const Vec3 *point);

    // boxContainsBox (zge/math/box.cpp): whether inner lies within outer (faces
    // touching count).
    int32_t boxContainsBox(ee::Ptr<Box> outer, ee::Ptr<Box> inner);

    // padThinBox (zge/math/box.cpp): each axis thinner than a hundredth grows by
    // half a hundredth on both sides. Returns the box.
    ee::Ptr<Box> padThinBox(ee::Ptr<Box> box);
}
