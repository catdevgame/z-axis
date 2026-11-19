// Planes and the clipping of a segment by them (planeDistance-lookAtMatrix in the
// math block, clipSegmentToBox in zprofile; DM2 and BMX XXX have them word for
// word). A plane is a Vec4: its unit normal (x, y, z) and w = -(normal .
// a point on it), so a point's signed distance is normal . p + w. The
// sphere query clips its segment to a node's box with them
// (zge/scene/query.cpp); the skater's moves use the plane routines.

#include "plane.h"

#include "box.h"
#include "vector.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "vec.h"
#include "sym/zge/math/plane.cpp.h"

namespace zge
{
    namespace
    {
        // How close to a plane a point counts as on it.
        inline ee::Global<ee::Float, sym::g_planeEpsilon> g_planeEpsilon;
        // The six faces' outward normals, the low corner's three faces first
        // (the planes are put through the box's low corner for 0 .. 2, its high
        // corner for 3 .. 5).
        inline ee::Global<Vec4[6], sym::g_boxFaceNormals> g_boxFaceNormals;

        // Where a point lies against a plane.
        enum Side : int32_t
        {
            kInFront = 0,
            kBehind = 1,
            kOnPlane = 2,
        };
    }

    // planeDistance: point's signed distance from the plane (VU0's dot of
    // the xyz, then the plane's w added on the FPU).
    ee::Float planeDistance(ee::Ptr<Vec4> plane, ee::Ptr<Vec4> point)
    {
        return dot(*plane, *point) + plane->w;
    }
    READABLE_FUNCTION(planeDistance, Verified);

    // planeThrough: the plane through point with the given normal (its w as
    // the normal's copied in, then replaced). Returns plane.
    ee::Ptr<Vec4> planeThrough(ee::Ptr<Vec4> plane, ee::Ptr<Vec4> point, ee::Ptr<Vec4> normal)
    {
        aligned(plane) = aligned(normal);
        plane->w = -dot(*normal, *point);
        return plane;
    }
    READABLE_FUNCTION(planeThrough, Verified);

    // clipSegmentToPlane: the segment start .. end cut to the plane's front: false
    // if none of it is in front; an end behind it is moved onto the plane
    // (the other in front). Ends on the plane count as in front of it.
    bool clipSegmentToPlane(ee::Ptr<Vec4> start, ee::Ptr<Vec4> end, ee::Ptr<Vec4> plane)
    {
        // The cut point keeps its place in the frame: the lerp sets x, y and z
        // only, so its w is what the stack held, and goes into the segment with it.
        ee::Local<Vec4> cut;
        ee::Float distance[2];
        distance[0] = planeDistance(plane, start);
        distance[1] = planeDistance(plane, end);
        const ee::Float epsilon = *g_planeEpsilon;
        Side side[2];
        for (int i = 0;; ++i)
        {
            const bool onPlane = ee::abs(distance[i]) < epsilon;
            if (onPlane)
                side[i] = kOnPlane;
            else
                side[i] = ee::Float(0.0f) < distance[i] ? kInFront : kBehind;
            if (i == 1)
                break;
        }
        if (side[0] == side[1])
            return side[1] != kBehind;
        if (side[0] != kInFront && side[1] != kInFront)
        {
            return false; // one behind, one on the plane
        }
        if (side[0] == kOnPlane || side[1] == kOnPlane)
        {
            // One in front, one on the plane.
            return true;
        }
        vec3Lerp(&cut->xyz(), &start->xyz(), &end->xyz(), distance[0] / (distance[0] - distance[1]));
        if (side[0] == kInFront)
            *end = cut;
        else
            *start = cut;
        return true;
    }
    READABLE_FUNCTION(clipSegmentToPlane, Verified);

    // clipSegmentToBox: the segment start .. end cut to box, in place: by each
    // of its six face planes in turn, on copies, written back only if some
    // of it is inside. 1 if so (also when both ends are inside), else 0.
    uint32_t clipSegmentToBox(ee::Ptr<Vec4> start, ee::Ptr<Vec4> end, ee::Ptr<Box> box)
    {
        if (boxContainsPoint(box.get(), &start->xyz()) && boxContainsPoint(box.get(), &end->xyz()))
            return 1;
        ee::Local<Vec4> plane;
        ee::Local<Vec4> clippedStart;
        ee::Local<Vec4> clippedEnd;
        clippedStart = *start;
        clippedEnd = *end;
        for (uint32_t face = 0; face < 6; ++face)
        {
            Vec4 &corner = face < 3 ? box->low : box->high;
            planeThrough(plane, &corner, &(*g_boxFaceNormals)[face]);
            if (!clipSegmentToPlane(clippedStart, clippedEnd, plane))
                return 0;
        }
        *start = clippedStart;
        *end = clippedEnd;
        return 1;
    }
    READABLE_FUNCTION(clipSegmentToBox, Verified);

    // setVec4: a vector set from four floats. Returns out.
    ee::Ptr<Vec4> setVec4(ee::Ptr<Vec4> out, ee::Float x, ee::Float y, ee::Float z, ee::Float w)
    {
        out->x = x;
        out->y = y;
        out->z = z;
        out->w = w;
        return out;
    }
    READABLE_FUNCTION(setVec4, Draft);
}
