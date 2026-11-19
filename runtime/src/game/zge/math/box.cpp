// The engine's box routines (block 168, zprofile).

#include "box.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "vec.h"
#include "vector.h"
#include "matrix.h"
#include "random.h"
#include "sym/zge/math/box.cpp.h"

namespace zge
{
    ee::Ptr<Box> boxTransform(ee::Ptr<Box> out, ee::Ptr<Box> box, ee::Ptr<Matrix> m)
    {
        // Every corner of the box, transformed. The first (corner 7: low x, y
        // and z) starts the result and the others widen it. The corner keeps its
        // place in the frame: its w starts as what the stack held, then the
        // transform's.
        ee::Local<Vec4> corner;
        for (int32_t i = 7; i >= 0; --i)
        {
            corner->x = (i & 1) ? box->low.x : box->high.x;
            corner->y = (i & 2) ? box->low.y : box->high.y;
            corner->z = (i & 4) ? box->low.z : box->high.z;
            corner = transformed(*m, corner);
            if (i == 7)
            {
                out->low = corner;
                out->high = corner;
            }
            else
            {
                aligned(out).low = minimum(out->low, corner);
                aligned(out).high = maximum(out->high, corner);
            }
        }
        return out;
    }

    ee::Ptr<Box> boxFromPoint(ee::Ptr<Box> box, ee::Ptr<Vec3> point);
    ee::Ptr<Box> boxExtend(ee::Ptr<Box> box, ee::Ptr<Vec4> point);

    // boxOfTransformedCorners: ZGE2's boxTransform: the corners in the other order (corner 0, high x, y and z,
    // first), each through ZGE2's point transform and into the box by boxFromPoint, then boxExtend. (ZGE3 writes the
    // transform and the widening inline.)
    inline ee::Ptr<Box> boxOfTransformedCorners(ee::Ptr<Box> out, ee::Ptr<Box> box, ee::Ptr<Matrix> m)
    {
        ee::Local<Vec4> corner;
        for (int32_t i = 0; i < 8; ++i)
        {
            corner->x = (i & 1) ? box->low.x : box->high.x;
            corner->y = (i & 2) ? box->low.y : box->high.y;
            corner->z = (i & 4) ? box->low.z : box->high.z;
            transformPoint(corner, m, corner);
            if (i == 0)
                boxFromPoint(out, &corner->xyz());
            else
                boxExtend(out, corner);
        }
        return out;
    }

    // boxOfScaledRowSums: ZGE4's boxTransform: the matrix's rows scaled by each corner's coordinates once (six
    // vectors), and each corner the sum of its x and y rows and of its z row and the translation. The corners go high
    // to low (x outermost), the first starting the box. The scaled rows keep the w VU0's result register had.
    inline ee::Ptr<Box> boxOfScaledRowSums(ee::Ptr<Box> out, ee::Ptr<Box> box, ee::Ptr<Matrix> m)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<Vec4> lowX, lowY, lowZ, highX, highY, highZ;
        ee::Local<Vec4> corner;
        ee::Local<Vec4> alongXY;  // the x and y rows' sum
        ee::Local<Vec4> alongZ;   // the z row and the translation
        lowX = matrixRow(*m, 0) * box->low.x;
        lowY = matrixRow(*m, 1) * box->low.y;
        lowZ = matrixRow(*m, 2) * box->low.z;
        highX = matrixRow(*m, 0) * box->high.x;
        highY = matrixRow(*m, 1) * box->high.y;
        highZ = matrixRow(*m, 2) * box->high.z;
        const Vec4 &translation = matrixRow(*m, 3);
        auto cornerOf = [&](const Vec4 &x, const Vec4 &y, const Vec4 &z)
        {
            alongXY = x + y;
            alongZ = z + translation;
            return *alongXY + *alongZ;
        };
        out->low = cornerOf(highX, highY, highZ);
        out->high = out->low;
        const Vec4 *xs[2] = {&*highX, &*lowX}, *ys[2] = {&*highY, &*lowY}, *zs[2] = {&*highZ, &*lowZ};
        for (int i = 1; i < 8; ++i)
        {
            corner = cornerOf(*xs[i >> 2], *ys[(i >> 1) & 1], *zs[i & 1]);
            aligned(out).low = minimum(out->low, corner);
            aligned(out).high = maximum(out->high, corner);
        }
        return out;
    }
    READABLE_FUNCTION_OWN(boxTransform, Verified, dm2, boxOfTransformedCorners, bmx, boxOfScaledRowSums);

    ee::Ptr<Vec3> boxCentre(ee::Ptr<Vec3> out, const Box *box)
    {
        // A component at a time, read and written in the original's order.
        out->x = (box->high.x - box->low.x) * 0.5f + box->low.x;
        out->y = (box->high.y - box->low.y) * 0.5f + box->low.y;
        out->z = (box->high.z - box->low.z) * 0.5f + box->low.z;
        return out;
    }
    READABLE_FUNCTION(boxCentre, Verified);

    namespace
    {
        // An axis thinner than a hundredth widened by half a hundredth each way.
        void padThinAxis(ee::Float &low, ee::Float &high)
        {
            const ee::Float top = high;
            const ee::Float bottom = low;
            if (ee::abs(top - bottom) <= ee::Float(0.01f))
            {
                high = top + ee::Float(0.005f);
                low = bottom - ee::Float(0.005f);
            }
        }
    }

    // Gives a flat box some depth: each axis thinner than a hundredth grows by
    // half a hundredth on both sides (a floor's box, a wall's), so that box
    // tests against it still find it. Returns the box.
    ee::Ptr<Box> padThinBox(ee::Ptr<Box> box)
    {
        padThinAxis(box->low.x, box->high.x);
        padThinAxis(box->low.y, box->high.y);
        padThinAxis(box->low.z, box->high.z);
        return box;
    }
    READABLE_FUNCTION(padThinBox, Verified);

    // A sphere of (almost) no size gets a thousandth more radius. Returns the
    // sphere.
    ee::Ptr<BoundingSphere> padSphereRadius(ee::Ptr<BoundingSphere> sphere)
    {
        const ee::Float radius = sphere->radius;
        if (radius <= ee::Float(0.001f))
            sphere->radius = radius + ee::Float(0.001f);
        return sphere;
    }
    READABLE_FUNCTION(padSphereRadius, Verified);

    // The box around two boxes, into out (which may be either). Returns out.
    ee::Ptr<Box> boxUnion(ee::Ptr<Box> out, ee::Ptr<Box> a, ee::Ptr<Box> b)
    {
        out->low.x = ee::min(b->low.x, a->low.x);
        out->high.x = ee::max(b->high.x, a->high.x);
        out->low.y = ee::min(b->low.y, a->low.y);
        out->high.y = ee::max(b->high.y, a->high.y);
        out->low.z = ee::min(b->low.z, a->low.z);
        out->high.z = ee::max(b->high.z, a->high.z);
        return out;
    }
    READABLE_FUNCTION(boxUnion, Verified);

    // A box of no size at the point: both corners are the point's whole
    // quadword (ZGE2: its x, y and z, one at a time on the FPU; the corners' w
    // are left). Returns the box.
    ee::Ptr<Box> boxFromPoint(ee::Ptr<Box> box, ee::Ptr<Vec3> point)
    {
        if constexpr (game::readable::kIsDm2)
        {
            box->low.x = box->high.x = point->x;
            box->low.y = box->high.y = point->y;
            box->low.z = box->high.z = point->z;
            return box;
        }
        aligned(ee::Ptr<Vec4>(&box->low)) = quadwordOf(point);
        aligned(ee::Ptr<Vec4>(&box->high)) = quadwordOf(point);
        return box;
    }
    READABLE_FUNCTION(boxFromPoint, Verified);

    // boxContainsBox: whether inner lies within outer (faces touching
    // count). ZGE2 compares the corners one component at a time on the FPU
    // (x low, x high, y low, y high, z low, z high, stopping at the first
    // outside), so a point handed as the inner box (its findRegionAt) needs
    // no alignment; ZGE3 and ZGE4 load the corners whole on VU0 (boxWithin).
    int32_t boxContainsBox(ee::Ptr<Box> outer, ee::Ptr<Box> inner)
    {
        if constexpr (game::readable::kIsDm2)
        {
            return outer->low.x <= inner->low.x && inner->high.x <= outer->high.x &&
                   outer->low.y <= inner->low.y && inner->high.y <= outer->high.y &&
                   outer->low.z <= inner->low.z && inner->high.z <= outer->high.z ? 1 : 0;
        }
        else
            return boxWithin(inner->low, inner->high, outer->low, outer->high) ? 1 : 0;
    }
    READABLE_FUNCTION(boxContainsBox, Verified);

    // boxExtend: the box grown to take in the point (its whole quadword;
    // ZGE2: x, y and z, a corner written only where the point is past it).
    // Returns the box.
    ee::Ptr<Box> boxExtend(ee::Ptr<Box> box, ee::Ptr<Vec4> point)
    {
        if constexpr (game::readable::kIsDm2)
        {
            if (point->x < box->low.x)
                box->low.x = point->x;
            if (box->high.x < point->x)
                box->high.x = point->x;
            if (point->y < box->low.y)
                box->low.y = point->y;
            if (box->high.y < point->y)
                box->high.y = point->y;
            if (point->z < box->low.z)
                box->low.z = point->z;
            if (box->high.z < point->z)
                box->high.z = point->z;
            return box;
        }
        aligned(ee::Ptr<Vec4>(&box->low)) = minimum(box->low, quadwordOf(point));
        aligned(ee::Ptr<Vec4>(&box->high)) = maximum(box->high, quadwordOf(point));
        return box;
    }
    READABLE_FUNCTION(boxExtend, Verified);

    // sphereFromBox: the sphere around the box: its centre, and the distance
    // from there to the high corner as its radius (the centre's w takes it
    // too). Returns the sphere.
    ee::Ptr<BoundingSphere> sphereFromBox(ee::Ptr<BoundingSphere> sphere, ee::Ptr<Box> box)
    {
        ee::Local<Vec4> centre;
        if constexpr (game::readable::kIsDm2) // ZGE2: the centre made in the sphere itself; its w is left
        {
            boxCentre(&sphere->centre.xyz(), box);
            *centre = box->high - sphere->centre;
            sphere->radius = vec3Length(&centre->xyz());
            return sphere;
        }
        boxCentre(&centre->xyz(), box);
        sphere->centre = *centre;
        // The old radius in w, which VU0's difference below takes along.
        sphere->centre.w = sphere->radius;
        *centre = box->high - sphere->centre;
        const ee::Float radius = vec3Length(&centre->xyz());
        sphere->centre.w = radius;
        sphere->radius = radius;
        return sphere;
    }
    READABLE_FUNCTION(sphereFromBox, Verified);

    namespace
    {
        // The engine's random number (math/random.cpp), as a signed count.
        int32_t nextRandom() { return static_cast<int32_t>(randomNumber()); }
        // A random number for a share of a length, and its scale: ZGE3 takes it signed and scales it by 2^-32, so
        // the share lands in [-0.5, 0.5); ZGE4 keeps its low 31 bits and scales them by 2^-31: [0, 1).
        int32_t randomForShare()
        {
            const int32_t random = nextRandom();
            return game::readable::kIsBmx ? random & 0x7FFFFFFF : random;
        }
        constexpr ee::Float kRandomScale = game::readable::kIsBmx ? 0x1p-31f : 0x1p-32f;
    }

    // boxContainsPointInFrame (no caller in this build): whether the point
    // lies in the box, the box given in the frame `matrix` places in the
    // world (the point taken into it by the matrix's rigid inverse).
    uint32_t boxContainsPointInFrame(ee::Ptr<Box> box, ee::Ptr<Vec3> point, ee::Ptr<Matrix> matrix)
    {
        ee::Local<Vec4> local;
        ee::Local<Matrix> worldToBox;
        const ee::Ptr<Matrix> inverse = matrixInvertRigid(&*worldToBox, matrix);
        *local = transformed(*inverse, quadwordOf(point));
        return boxContainsPoint(box.get(), &local->xyz()) ? 1 : 0;
    }
    READABLE_FUNCTION(boxContainsPointInFrame, Draft);

    // boxesIntersect (no caller in this build): whether the two boxes overlap,
    // faces touching included.
    uint32_t boxesIntersect(ee::Ptr<Box> a, ee::Ptr<Box> b)
    {
        return a->low.x <= b->high.x && b->low.x <= a->high.x &&
               a->low.y <= b->high.y && b->low.y <= a->high.y &&
               a->low.z <= b->high.z && b->low.z <= a->high.z;
    }
    READABLE_FUNCTION(boxesIntersect, Draft);

    // boxContainsShrunkBox (no caller in this build): whether `outer` holds
    // `inner` shrunk by the margin on every face (strictly: a face of outer
    // on the shrunk one does not count).
    uint32_t boxContainsShrunkBox(ee::Ptr<Box> outer, ee::Ptr<Box> inner, ee::Float margin)
    {
        return outer->low.x < inner->low.x + margin && inner->high.x - margin < outer->high.x &&
               outer->low.y < inner->low.y + margin && inner->high.y - margin < outer->high.y &&
               outer->low.z < inner->low.z + margin && inner->high.z - margin < outer->high.z;
    }
    READABLE_FUNCTION(boxContainsShrunkBox, Draft);

    // boxesWellFormed (no caller in this build): per axis, the two boxes'
    // half extents' sum against their difference. The sum falls below the
    // difference only when one of them is negative, so it answers whether
    // neither box is inside out (its low corner above its high on an axis).
    uint32_t boxesWellFormed(ee::Ptr<Box> a, ee::Ptr<Box> b)
    {
        const auto halvesAgree = [](ee::Float lowA, ee::Float highA, ee::Float lowB, ee::Float highB) {
            const ee::Float halfA = (highA - lowA) * ee::Float(0.5f);
            const ee::Float halfB = (highB - lowB) * ee::Float(0.5f);
            return !(halfA + halfB < ee::abs(halfA - halfB));
        };
        return halvesAgree(a->low.x, a->high.x, b->low.x, b->high.x) && halvesAgree(a->low.y, a->high.y, b->low.y, b->high.y) &&
               halvesAgree(a->low.z, a->high.z, b->low.z, b->high.z);
    }
    READABLE_FUNCTION(boxesWellFormed, Draft);

    // randomPointInBox (no caller in this build): a random point for the box,
    // w 1. ZGE3 takes the random numbers as signed, so on each axis the point
    // lies within half the box's extent either side of its low corner, not
    // in the box (kept); ZGE4's is in the box.
    ee::Ptr<Vec4> randomPointInBox(ee::Ptr<Box> box, ee::Ptr<Vec4> out)
    {
        const ee::Float width = box->high.x - box->low.x;
        const ee::Float height = box->high.y - box->low.y;
        const ee::Float depth = box->high.z - box->low.z;
        out->x = box->low.x + width * kRandomScale * ee::toFloat(randomForShare());
        out->y = box->low.y + height * kRandomScale * ee::toFloat(randomForShare());
        const int32_t random = randomForShare();
        out->w = ee::Float(1.0f);
        out->z = box->low.z + depth * kRandomScale * ee::toFloat(random);
        return out;
    }
    READABLE_FUNCTION(randomPointInBox, Draft);

    // randomPointInSphere (no caller in this build): a random point near the
    // sphere's centre: a random distance along x, turned by three random
    // angles, then moved to the centre. ZGE3 takes the distance's random number
    // as signed, so it is at most half the radius (kept; ZGE4's is up to the radius). Returns out.
    ee::Ptr<Vec4> randomPointInSphere(ee::Ptr<BoundingSphere> sphere, ee::Ptr<Vec4> out)
    {
        ee::Local<EulerAngles> angles;
        ee::Local<Matrix> turn;
        const int32_t distance = randomForShare();
        const ee::Float scale = sphere->radius * kRandomScale;
        out->y = ee::Float(0.0f);
        out->w = ee::Float(1.0f);
        out->z = ee::Float(0.0f);
        out->x = scale * ee::toFloat(distance);
        angles->x = static_cast<Angle>(static_cast<uint32_t>(nextRandom()) % 0xFFFFu);
        angles->y = static_cast<Angle>(static_cast<uint32_t>(nextRandom()) % 0xFFFFu);
        angles->z = static_cast<Angle>(static_cast<uint32_t>(nextRandom()) % 0xFFFFu);
        matrixIdentity(&*turn);
        matrixRotateXYZ(&*turn, &*turn, &*angles);
        *out = transformed(*turn, *out);
        *out = *out + sphere->centre;
        return out;
    }
    READABLE_FUNCTION(randomPointInSphere, Draft);
}
