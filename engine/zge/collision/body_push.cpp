// A rigid body against the game's own boxes (the NPCs', the traffic cars' and
// the skaters': aggressive/level/dynamics_objects.cpp's contact tests): the
// body as an oriented box, and the push one of them gives it.

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"

#include <cstdint>
#include "sym/zge/collision/body_push.cpp.h"

namespace zge
{
    namespace // the game functions below call: ee/plain.h
    {

        constexpr uint32_t kBoxShape = 0; // RigidBody::shape
        // Closer than this (squared) the push has no direction: it is up.
        constexpr ee::Float kNoDirection = ee::Float(1e-05f);
    }

    // bodyBox: the body as an oriented box (its position, half extents and
    // orientation): 1, or 0 when the body is not a box.
    uint32_t bodyBox(ee::Ptr<RigidBody> body, ee::Ptr<OrientedBox> box)
    {
        if (body->shape != kBoxShape)
            return 0;
        aligned(ee::Ptr<Vec4>(&box->centre)) = body->position;
        aligned(ee::Ptr<Vec4>(&box->halfExtents)) = quadwordOf(body->halfExtents);
        quaternionCopy(&box->orientation, &body->orientation);
        return 1;
    }
    READABLE_FUNCTION(bodyBox, Verified);

    // pushBodyFromPoint: the body pushed away from `point` (straight up when
    // the point is at its centre) by something moving at `velocity` of the
    // given mass: contactImpulse with that velocity scaled by mass / (the
    // body's mass + mass). The first argument (the other's box) is not read.
    void pushBodyFromPoint(ee::Ptr<OrientedBox> /*otherBox*/, ee::Ptr<RigidBody> body, ee::Ptr<Vec4> velocity,
                           ee::Ptr<Vec4> point, ee::Float mass)
    {
        ee::Local<Vec4> away;
        ee::Local<Vec4> pushing;
        *away = body->position - *point;
        if (dot(*away, *away) < kNoDirection)
            away->y = ee::Float(1.0f);
        *away = unit(*away);
        const ee::Float share = mass / (body->mass + mass);
        *pushing = scaledAfterMeasure(*velocity, share);
        contactImpulse(body, point, away, pushing);
    }
    READABLE_FUNCTION(pushBodyFromPoint, Verified);
}
