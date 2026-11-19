// BMX XXX's skitch (physics.c): the rider holding on to a car, physics
// states 14 (the approach) and 15 (the hold). BMX XXX's own. The cars'
// hold points are the tagged corners of their collision faces
// (collision::taggedCornersNear); the rider holds on beside one, above the
// ground under it, turned as the car is.

#include "../collision/collision.h"
#include "../rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/physics/skitch.cpp.h"

namespace game::bmx::rider
{
    using collision::EdgeRef;

    namespace
    {
        // The hold point from the corner, in the car's rows (right, up, forward; across for a mirrored hold):
        // (0.5, 0, -0.45).
        inline ee::Global<zge::Vec4, sym::g_skitchHoldOffset> g_skitchHoldOffset;
        // The ground is looked for this far below the hold point (its up row's way down), and found no deeper
        // than 1.5; the rider is held 1.15 above it.
        constexpr ee::Float kGroundReach(-2.0f);
        constexpr ee::Float kGroundDeepest(1.5f);
        inline ee::Global<ee::Float, sym::g_skitchHoldHeight> g_skitchHoldHeight;
    }

    // skitchSnap (rider, corner): the hold at a car's corner as the snap target: the car's rows (its
    // orientation's, the up and forward rows turned round; the rider's own rows when the corner is not an
    // object's), the hold point the corner and the hold offset through them, and the ground looked for below
    // it. Found: the snap point 1.15 above the ground there, the snap rows the car's, 1. No ground within reach:
    // physics state 1 (the air), 0; ground deeper than 1.5: state 0 (the ground), 0.
    int32_t skitchSnap(Rider *rider, EdgeRef corner)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> hold;
        ee::Local<zge::Vec4> work; // the hold offset, then the ray's end, then the drop to the ground
        ee::Local<zge::Vec4> ground;
        ee::Local<zge::Matrix> rows;
        collision::cornerPoint(corner, &point->x);
        const ee::Ptr<zge::SceneObject> car = zge::nodeSceneObject(ee::Ptr<zge::Node>(collision::refNode(corner)), {});
        if (car)
        {
            zge::quaternionToMatrix(rows, zge::objectOrientation(car));
            zge::vec3Negate(&zge::matrixRow(*rows, 1).xyz(), &zge::matrixRow(*rows, 1).xyz());
            zge::vec3Negate(&zge::matrixRow(*rows, 2).xyz(), &zge::matrixRow(*rows, 2).xyz());
            zge::matrixRow(*rows, 3) = zge::kZero;
        }
        else
        {
            *rows = *matrix(rider->right);
        }
        const zge::Vec4 &up = zge::matrixRow(*rows, 1);
        *work = *g_skitchHoldOffset;
        if (collision::cornerKind(corner) == collision::kCornerHoldMirrored)
            work->x = -work->x;
        *work = zge::transformed(*rows, *work);
        *hold = *point + *work;
        *work = up * kGroundReach;
        *work = *hold + *work;
        const collision::RayHit *hit = collision::rayCastTouching(rider, &hold->x, &work->x, 0u);
        if (hit == nullptr)
        {
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            return 0;
        }
        collision::hitPoint(hit, &ground->x);
        *work = *ground - *hold;
        const ee::Float drop = -zge::dot(*work, up);
        if (!(drop < kGroundDeepest))
        {
            setPhysicsState(rider, uint32_t(kPhysicsGround));
            return 0;
        }
        *work = up * *g_skitchHoldHeight;
        *hold = *ground + *work;
        *vec4(rider->snapPoint) = *hold;
        *matrix(rider->snapRows) = *rows;
        return 1;
    }
    READABLE_FUNCTION(skitchSnap, Draft);

    namespace
    {
        // The rider's contact kind while it holds on: the corner.
        constexpr uint8_t kContactCorner = 6;
        // The corners looked at are those within 2.5 of the drawn rider, at its place now and at each of the
        // next frames' (by its velocity) to look ahead (1: now only).
        inline ee::Global<ee::Float, sym::g_skitchReach> g_skitchReach;
        inline ee::Global<uint32_t, sym::g_skitchLookAheadFrames> g_skitchLookAheadFrames;
        inline ee::Global<ee::Float, sym::g_skitchNearestStart> g_skitchNearestStart; // 1e8: farther than any
        // A car's velocity (a second's) in a frame's.
        inline ee::Global<ee::Float, sym::g_skitchCarFrameShare> g_skitchCarFrameShare;
        // A car is held on to only while the rider moves with it: slower than 0.25 a frame from it.
        inline ee::Global<ee::Float, sym::g_skitchMostSpeedApart> g_skitchMostSpeedApart;
    }

    // skitchEnter (rider, from, state): physics states 14 and 15's enter. The approach (14): the nearest corner
    // a car can be held at (kinds 1 and 2), at which the rider moves with the car (its speed apart from the car's
    // velocity under 0.25; a corner that is no object's by the rider's own speed); the hold snapped to there,
    // the corner the contact, mirrored for a mirrored hold, the snap's step the rider's velocity over the frames
    // the corner was found ahead. The hold (15): the rider put at the snap point with the snap rows. Returns the
    // state, or `from` with no corner, or the state skitchSnap left when it found no hold.
    uint32_t skitchEnter(Rider *rider, uint32_t from, uint32_t state)
    {
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> apart; // the corner from the centre, then the car's velocity, then the rider's from it
        ee::Local<zge::Vec4> point;
        ee::Local<zge::EngineMessage> message;
        ee::Local<EdgeRef> corner;
        if (state == kPhysicsSkitchApproach)
        {
            *centre = *vec4(rider->position) + *vec4(rider->rootOffset);
            const EdgeRef *corners = collision::taggedCornersNear(&centre->x, *g_skitchReach);
            if (corners == nullptr)
                return from;
            EdgeRef nearest = 0;
            ee::Float nearestSquared = *g_skitchNearestStart;
            uint32_t nearestFrames = 0;
            for (uint32_t frames = 0; frames < *g_skitchLookAheadFrames;)
            {
                ++frames;
                for (const EdgeRef *next = corners; (*corner = *next) != 0; ++next)
                {
                    const uint32_t kind = collision::cornerKind(*corner);
                    if (kind > collision::kCornerHoldMirrored || kind == 0)
                        continue;
                    collision::cornerPoint(*corner, &point->x);
                    *apart = *point - *centre;
                    const ee::Float squared = zge::squaredLength(*apart);
                    if (!(squared < nearestSquared))
                        continue;
                    ee::Float speedApart = rider->speed;
                    const ee::Ptr<zge::SceneObject> car = zge::nodeSceneObject(ee::Ptr<zge::Node>(collision::refNode(*corner)), {});
                    if (car)
                    {
                        message->target = car;
                        message->sender = ee::Address(rider->messenger);
                        message->type = zge::kVelocityMessage;
                        message->flags = zge::kMessageNow;
                        message->data = apart;
                        zge::sendEngineMessage(message);
                        *apart = *apart * *g_skitchCarFrameShare;
                        *apart = *vec4(rider->velocity) - *apart;
                        speedApart = zge::vec3Length(&apart->xyz());
                    }
                    if (speedApart < *g_skitchMostSpeedApart)
                    {
                        nearestSquared = squared;
                        nearest = *corner;
                        nearestFrames = frames;
                    }
                }
                *centre = *centre + *vec4(rider->velocity);
            }
            if (nearest == 0)
                return from;
            *corner = nearest;
            if (skitchSnap(rider, nearest) != 1)
                return physicsState(rider);
            setContact(rider, &*corner, kContactCorner);
            if (collision::cornerKind(*corner) == collision::kCornerHoldMirrored)
                setFlag(rider, kFlagMirrorSide);
            else
                clearFlag(rider, kFlagMirrorSide);
            *vec4(rider->snapStep) = *vec4(rider->velocity);
            rider->snapFrames = static_cast<int16_t>(nearestFrames);
            return state;
        }
        if (skitchSnap(rider, contactRef(rider)) != 1)
            return physicsState(rider);
        *vec4(rider->position) = *vec4(rider->snapPoint);
        *vec4(rider->rootOffset) = zge::kZero;
        *matrix(rider->right) = *matrix(rider->snapRows);
        return state;
    }
    READABLE_FUNCTION(skitchEnter, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_skitchHoldFrameShare> g_skitchHoldFrameShare; // a car's velocity in a frame's
    }

    // skitchMove (rider): physics states 14 and 15's move. The approach: the snap's step added to the drawn body
    // and the position, the hold (15) once the snap's frames have gone. The hold: kept at the car's corner (the
    // snap point and rows; the drawn body and both wheels turned with them), moving at the car's velocity (still
    // when the corner is no object's), both wheels' too.
    void skitchMove(Rider *rider)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<zge::Vec4> carVelocity;
        const uint8_t state = physicsState(rider);
        if (state == kPhysicsSkitchApproach)
        {
            if (--rider->snapFrames == 0)
            {
                setPhysicsState(rider, uint32_t(kPhysicsSkitch));
                return;
            }
            *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(rider->snapStep);
            *vec4(rider->position) = *vec4(rider->position) + *vec4(rider->snapStep);
            return;
        }
        if (state != kPhysicsSkitch || skitchSnap(rider, contactRef(rider)) != 1)
            return;
        *vec4(rider->position) = *vec4(rider->snapPoint);
        *vec4(rider->rootOffset) = zge::kZero;
        *matrix(rider->right) = *matrix(rider->snapRows);
        *matrix(rider->body) = *matrix(rider->right);
        *matrix(rider->wheelPoses[0].matrix) = *matrix(rider->right);
        *matrix(rider->wheelPoses[1].matrix) = *matrix(rider->right);
        const ee::Ptr<zge::SceneObject> car = zge::nodeSceneObject(ee::Ptr<zge::Node>(collision::refNode(contactRef(rider))), {});
        if (car)
        {
            message->target = car;
            message->sender = ee::Address(rider->messenger);
            message->type = zge::kVelocityMessage;
            message->flags = zge::kMessageNow;
            message->data = carVelocity;
            zge::sendEngineMessage(message);
            *carVelocity = *carVelocity * *g_skitchHoldFrameShare;
        }
        else
        {
            *carVelocity = zge::kZero;
        }
        *vec4(rider->velocity) = *carVelocity;
        rider->speed = zge::vec3Length(&vec4(rider->velocity)->xyz());
        *vec4(rider->wheelPoses[0].velocity) = *carVelocity;
        *vec4(rider->wheelPoses[1].velocity) = *carVelocity;
        rider->wheelPoses[1].speed = rider->speed;
        rider->wheelPoses[0].speed = rider->speed;
    }
    READABLE_FUNCTION(skitchMove, Draft);
}
