// BMX XXX's bail (physics.c): the hands that reach for an edge while the
// rider tumbles. DM2's (games/dm2/rider/actions/bail.cpp, approach.cpp),
// read in BMX XXX's machine code: BMX XXX looks for the rail edges near
// the hand (DM2 walks the faces its bodies touched), and the rider's model
// has a limb rig whose hand points it lets go.

#include "../collision/collision.h"
#include "../engine_calls.h"
#include "../physics/physics.h"
#include "../rider.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/player_riders.h"
#include "bmx/profile/players.h"
#include "bmx/sound/roll_sound.h"
#include "../game_calls.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/debug/debug.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/bail.cpp.h"

namespace game::bmx::rider
{
    using collision::EdgeRef;

    namespace
    {
        // The rail edges looked at are within this of the hand (0.4), and
        // one is held when its nearest point is within reach (0.16, a
        // squared distance).
        inline ee::Global<ee::Float, sym::g_handEdgeRadius> g_handEdgeRadius;
        inline ee::Global<ee::Float, sym::g_handReach> g_handReach;
        // An edge shorter than this (a squared length, 0.0001) is held at its start.
        inline ee::Global<ee::Float, sym::g_handEdgeShortest> g_handEdgeShortest;
        // The debug display's points at the hand: red with no edge, green
        // holding one (each 0.4 across).
        inline ee::Global<uint32_t, sym::g_handFreeColour> g_handFreeColour;
        inline ee::Global<uint32_t, sym::g_handHeldColour> g_handHeldColour;
        inline ee::Global<ee::Float, sym::g_handHeldPointSize> g_handHeldPointSize;
        inline ee::Global<ee::Float, sym::g_handFreePointSize> g_handFreePointSize;
        constexpr uint32_t kWorldMatrix = 1;   // nodeMatrix's
        constexpr uint32_t kInverseMatrix = 2; // nodeMatrix's: the world into the node's space
        inline ee::Global<zge::Vec4, sym::origin> g_origin; // (0, 0, 0, 0)
        // The hands' bones (name hashes; DM2 hashes "LeftHand" and "RightHand").
        constexpr uint32_t kLeftHandBone = 0x09B8C124;
        constexpr uint32_t kRightHandBone = 0x0BD894F4;

        engine::BodyRig *limbRig(const Rider *rider) { return ee::Ptr<engine::BodyRig>(rider->limbRig); }
    }

    // A bail's hand against the rail edges near it: the edge whose nearest
    // point (on its line) is closest to the hand (`point`, in the world) is
    // held: the hand record gets the edge's ends, its node and the nearest
    // point. The first edge looked at is held only within reach: out of
    // reach, the next one starts over. With an edge held, the record's
    // share is how far along the edge the nearest point is, and the ends
    // go into the node's space (so the hold follows a moving object).
    void bailHandContact(Rider * /*rider*/, BailHand *hand, const zge::Vec4 *point)
    {
        ee::Local<uint32_t> freeColour;
        ee::Local<uint32_t> heldColour;
        ee::Local<zge::Vec4> onEdge;  // the point on the edge nearest the hand, then the best one from the edge's start
        ee::Local<zge::Vec4> toHand;  // from the hand to it, then the edge from start to end
        ee::Local<zge::Vec4> best;    // the nearest of all
        ee::Local<zge::Vec4> startInNode;
        ee::Local<zge::Vec4> endInNode;
        *freeColour = *g_handFreeColour;
        *heldColour = *g_handHeldColour;
        ee::Float nearest = ee::Float(0.0f); // the best edge's squared distance
        auto takeEdge = [&](EdgeRef edge) {
            hand->nodeRef = uint32_t(collision::refNode(edge));
            hand->node = static_cast<uint32_t>(collision::nodeOf(collision::widen(uint32_t(collision::refNode(edge)))));
        };
        if (const EdgeRef *next = collision::railEdgesNear(&point->x, *g_handEdgeRadius)) // ended by an edge of 0
        {
            for (EdgeRef edge = *next; edge != 0; edge = *++next)
            {
                if (hand->holding == 0)
                {
                    collision::edgePoint(edge, 0u, hand->edgeStart);
                    collision::edgePoint(edge, 1u, hand->edgeEnd);
                    hand->holding = 1;
                    takeEdge(edge);
                    closestOnRail(&point->x, edge, 0, &onEdge->x);
                    *toHand = *onEdge - *point;
                    nearest = zge::dot(*toHand, *toHand);
                    if (*g_handReach < nearest)
                        hand->holding = 0; // out of reach: the next edge starts over
                }
                else
                {
                    closestOnRail(&point->x, edge, 0, &onEdge->x);
                    *toHand = *onEdge - *point;
                    const ee::Float distance = zge::dot(*toHand, *toHand);
                    if (!(distance < nearest))
                        continue;
                    takeEdge(edge);
                    collision::edgePoint(edge, 0u, hand->edgeStart);
                    collision::edgePoint(edge, 1u, hand->edgeEnd);
                    *toHand = *onEdge - *point;
                    nearest = distance;
                }
                *best = *onEdge;
                *vec4(hand->nearest) = *best;
            }
        }
        if (hand->holding == 0)
        {
            zge::addDebugPoint(point, &*freeColour, 0, 0, *g_handFreePointSize);
            return;
        }
        // The share along the edge: (best - start).(end - start) / |end - start|^2.
        *onEdge = *best - *vec4(hand->edgeStart);
        *toHand = *vec4(hand->edgeEnd) - *vec4(hand->edgeStart);
        const ee::Float lengthSquared = zge::dot(*toHand, *toHand);
        if (lengthSquared < *g_handEdgeShortest)
            hand->share = 0.0f;
        else
            hand->share = zge::dot(*toHand, *onEdge) / lengthSquared;
        zge::addDebugPoint(point, &*heldColour, 0, 0, *g_handHeldPointSize);
        // The edge's ends into the node's space.
        *startInNode = zge::transformed(*zge::nodeMatrix(ee::Ptr<zge::Node>(hand->node), kInverseMatrix), *vec4(hand->edgeStart));
        *endInNode = zge::transformed(*zge::nodeMatrix(ee::Ptr<zge::Node>(hand->node), kInverseMatrix), *vec4(hand->edgeEnd));
        *vec4(hand->edgeStart) = *startInNode;
        *vec4(hand->edgeEnd) = *endInNode;
    }
    READABLE_FUNCTION(bailHandContact, Verified);

    // The bail's hands held where they touched (each hand holding an edge:
    // its body and the limb rig's hand point held at the point between the
    // edge's two ends, in the node's world space). DM2's, with the rig.
    void bailHandsHeld(Rider *rider)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> a;
        ee::Local<zge::Vec4> b;
        // (The scale's factor goes to VU0 by a register whose upper half
        // is the caller's, or the left hand's point once that was copied
        // through it.)
        uint64_t carried = zge::scaleLeftover(zge::LeftBy::caller);
        auto hold = [&](const BailHand &hand, zge::AnimatePoint *rigPoint) {
            *a = zge::transformed(*zge::nodeMatrix(ee::Ptr<zge::Node>(hand.node), kWorldMatrix), *vec4(hand.edgeStart));
            *b = zge::transformed(*zge::nodeMatrix(ee::Ptr<zge::Node>(hand.node), kWorldMatrix), *vec4(hand.edgeEnd));
            *point = *b - *a;
            *point = zge::scaledCarrying(*point, hand.share, carried);
            *point = *point + *a;
            const ee::Ptr<zge::BodyContact> body(hand.body);
            zge::setContactPoint(body, point);
            engine::enableContact(body);
            rigPoint->kind = zge::kPointHeld;
            rigPoint->position = *point;
            carried = physics::upperHalf(*point);
        };
        if (rider->hands[0].holding != 0)
            hold(rider->hands[0], limbRig(rider)->leftHand);
        if (rider->hands[1].holding != 0)
            hold(rider->hands[1], limbRig(rider)->rightHand);
    }
    READABLE_FUNCTION(bailHandsHeld, Verified);

    // The bail's hands against the rails, while the bail reaches with them:
    // each hand's bone in the world, then its contact (until one holds).
    // Not reaching, neither holds and the limb rig's hand points go free.
    // DM2's, with the rig.
    void bailHands(Rider *rider)
    {
        ee::Local<zge::Vec4> leftHand;
        ee::Local<zge::Vec4> rightHand;
        if (!hasFlag(rider, kFlagBailHands))
        {
            rider->hands[0].holding = 0;
            rider->hands[1].holding = 0;
            limbRig(rider)->leftHand->kind = zge::kPointFree;
            limbRig(rider)->rightHand->kind = zge::kPointFree;
            return;
        }
        if (rider->hands[0].holding != 0 || rider->hands[1].holding != 0)
            return;
        const ee::Ptr<zge::SceneObject> model = rider->messenger;
        zge::updatePose(model);
        zge::objectBonePoint(model, zge::findModelBone(model, kLeftHandBone), g_origin.get(), leftHand, 0);
        bailHandContact(rider, &rider->hands[0], leftHand);
        zge::objectBonePoint(model, zge::findModelBone(model, kRightHandBone), g_origin.get(), rightHand, 0);
        bailHandContact(rider, &rider->hands[1], rightHand);
    }
    READABLE_FUNCTION(bailHands, Verified);

    // The bail's hands let go: their bodies (the left's, then the right's)
    // and the limb rig's hand points. DM2's, with the rig.
    void bailBodyRelease(Rider *rider)
    {
        zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[0].body));
        zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[1].body));
        limbRig(rider)->rightHand->kind = zge::kPointFree;
        limbRig(rider)->leftHand->kind = zge::kPointFree;
    }
    READABLE_FUNCTION(bailBodyRelease, Verified);

    // The C library's memset, as the tumble calls it.
    // Scaffolding (address-call): docs/SCAFFOLDING.md
    namespace calls
    {
        inline void clearQuaternion(ee::Ptr<zge::Quaternion> q) { ee::game(sym::memset, q, 0u, static_cast<uint32_t>(sizeof(zge::Quaternion))); }
    }

    namespace
    {
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsName;
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        constexpr ee::Float kFramesASecond(60.0f);
        constexpr int32_t kFramesASecondWhole = 60;
        // The rider's tumble: how bouncy the motions' dynamics are (0.375) and its force's strength (0.1).
        inline ee::Global<ee::Float, sym::g_riderTumbleRestitution> g_riderTumbleRestitution;
        inline ee::Global<ee::Float, sym::g_riderTumbleForce> g_riderTumbleForce;
        // The bike's: 0.67 and 0.065, each body a box of these half extents (0.1, 0.35, 0.5), spun 5 a second about
        // its forward row.
        inline ee::Global<ee::Float, sym::g_bikeTumbleRestitution> g_bikeTumbleRestitution;
        inline ee::Global<ee::Float, sym::g_bikeTumbleForce> g_bikeTumbleForce;
        inline ee::Global<zge::Vec4, sym::g_bikeTumbleHalfExtents> g_bikeTumbleHalfExtents;
        constexpr ee::Float kBikeRightSpin(-2.0f);
        constexpr ee::Float kBikeForwardSpin(5.0f);

        // A tumbling body's weight: gravity a frame a frame as a second's, by its mass.
        ee::Float tumbleWeight(const zge::RigidBody &body) { return *g_gravity * kFramesASecond * kFramesASecond * body.mass; }
    }

    // wakeRiderTumble (rider, hipsHalfExtents, twoSidedFaces): the rider let go into the motions' dynamics: the
    // place moved by the root offset (none left), the model there, its hips' bone put back (no turn, no move) and
    // posed; the dynamics made bouncy and woken; the hips' body sized, every body's faces two-sided or not, weighed
    // and moving with the rider (a second's velocity). 1.
    uint32_t wakeRiderTumble(Rider *rider, ee::Ptr<zge::Vec4> hipsHalfExtents, uint32_t twoSidedFaces)
    {
        ee::Local<zge::Quaternion> still;
        calls::clearQuaternion(still);
        rider->position[0] = rider->position[0] + rider->rootOffset[0];
        rider->position[1] = rider->position[1] + rider->rootOffset[1];
        rider->position[2] = rider->position[2] + rider->rootOffset[2];
        still->w = 1.0f;
        *vec4(rider->rootOffset) = zge::kZero;
        zge::setObjectPosition(rider->messenger, ee::Ptr<zge::Vec4>(vec4(rider->position)));
        const ee::Ptr<zge::AnimationChannel> hips = zge::findAnimatorBone(rider->messenger, *g_hipsName);
        zge::setBoneRotation(hips, still);
        zge::setBoneTranslation(hips, ee::Ptr<zge::Vec4>(g_origin.get()));
        zge::updatePose(rider->messenger);
        zge::setDynamicsRestitution(rider->riderAnimDynamics, *g_riderTumbleRestitution);
        zge::setDynamicsContactDamping(rider->riderAnimDynamics, *g_riderTumbleForce);
        zge::wakeDynamics(rider->messenger, rider->riderAnimDynamics, 1u);
        for (ee::Ptr<zge::RigidBody> body = rider->riderAnimDynamics->bodies; body; body = body->next)
        {
            if (body->nameHash == *g_hipsName)
                zge::setBoxHalfExtents(body, hipsHalfExtents);
            zge::setBodyTwoSidedFaces(body, twoSidedFaces);
            body->force.x = 0.0f;
            body->force.z = 0.0f;
            body->force.y = tumbleWeight(*body);
            body->velocity = *vec4(rider->velocity) * ee::toFloat(kFramesASecondWhole);
        }
        return 1;
    }
    READABLE_FUNCTION(wakeRiderTumble, Verified);

    // wakeBikeTumble (rider): the bike let go into its own dynamics at a bail: made bouncy and woken; each body a
    // box of the bike's size, weighed, moving with the front wheel (a second's velocity) and spinning about its
    // forward row (a spin about its right row is set first and overwritten: bug kept). 1.
    uint32_t wakeBikeTumble(Rider *rider)
    {
        ee::Local<zge::Vec4> wheelVelocity;
        *wheelVelocity = *vec4(rider->wheelPoses[0].velocity);
        zge::setDynamicsRestitution(rider->bikeDynamics, *g_bikeTumbleRestitution);
        zge::setDynamicsContactDamping(rider->bikeDynamics, *g_bikeTumbleForce);
        zge::wakeDynamics(rider->bikeModel, rider->bikeDynamics, 1u);
        for (ee::Ptr<zge::RigidBody> body = rider->bikeDynamics->bodies; body; body = body->next)
        {
            zge::setBoxHalfExtents(body, ee::Ptr<zge::Vec4>(g_bikeTumbleHalfExtents.get()));
            body->force.x = 0.0f;
            body->force.z = 0.0f;
            body->force.y = tumbleWeight(*body);
            body->velocity = *wheelVelocity * ee::toFloat(kFramesASecondWhole);
            body->spin = zge::unit(zge::matrixRow(body->rotation, 0));
            body->spin = body->spin * kBikeRightSpin;
            body->spin = zge::unit(zge::matrixRow(body->rotation, 2));
            body->spin = body->spin * kBikeForwardSpin;
        }
        return 1;
    }
    READABLE_FUNCTION(wakeBikeTumble, Verified);

    namespace
    {
        // The state-12 tumble's hips' body (half extents 0.2, 0.9, 0.3).
        inline ee::Global<zge::Vec4, sym::g_tumbleHipsHalfExtents> g_tumbleHipsHalfExtents;
    }

    // tumbleEnter (rider, from, to): physics states 12 and 13's enter: into 12, the rider and the bike tumble (the
    // hips' body sized for it, one-sided faces). Returns the state.
    uint32_t tumbleEnter(Rider *rider, uint32_t from, uint32_t to)
    {
        (void)from;
        if (to == uint32_t(kPhysicsTumble))
        {
            wakeRiderTumble(rider, ee::Ptr<zge::Vec4>(g_tumbleHipsHalfExtents.get()), 0u);
            wakeBikeTumble(rider);
        }
        return to;
    }
    READABLE_FUNCTION(tumbleEnter, Verified);

    namespace
    {
        // The ghost ride's riderless bike: no faster than 1/12 a frame; slower than 1/15, its wheels sped up to it
        // (a wheel slower than 0.001 starts along the drawn body's up row).
        inline ee::Global<ee::Float, sym::g_ghostBikeMostSpeed> g_ghostBikeMostSpeed;
        inline ee::Global<ee::Float, sym::g_ghostBikeLeastSpeed> g_ghostBikeLeastSpeed;
        inline ee::Global<ee::Float, sym::g_ghostBikeStillSpeed> g_ghostBikeStillSpeed;
    }

    // ghostRideEnter (rider, from, state): physics states 16 and 17's enter. The ghost ride (any state but 17):
    // the bike's speed held between 1/15 and 1/12 a frame (too fast: the velocity slowed to the most; too slow:
    // the wheels' velocities sped up to the least), and the rider let go into the motions' dynamics (the hips'
    // body as the tumble's). Its tumble (17): the bike let go into its own dynamics. Returns the state.
    uint32_t ghostRideEnter(Rider *rider, uint32_t /*from*/, uint32_t state)
    {
        if (state == kPhysicsGhostRideTumble)
        {
            wakeBikeTumble(rider);
            return state;
        }
        if (*g_ghostBikeMostSpeed < rider->speed)
        {
            *vec4(rider->velocity) = *vec4(rider->velocity) * (*g_ghostBikeMostSpeed / rider->speed);
            rider->speed = *g_ghostBikeMostSpeed;
        }
        else if (rider->speed < *g_ghostBikeLeastSpeed)
        {
            for (WheelPose &wheel : rider->wheelPoses)
            {
                if (*g_ghostBikeStillSpeed < ee::abs(rider->speed))
                    *vec4(wheel.velocity) = *vec4(wheel.velocity) * (*g_ghostBikeLeastSpeed / rider->speed);
                else
                    *vec4(wheel.velocity) = zge::matrixRow(*matrix(rider->body), 1) * *g_ghostBikeLeastSpeed;
                wheel.speed = *g_ghostBikeLeastSpeed;
            }
        }
        wakeRiderTumble(rider, ee::Ptr<zge::Vec4>(g_tumbleHipsHalfExtents.get()), 0u);
        return state;
    }
    READABLE_FUNCTION(ghostRideEnter, Verified);

    namespace
    {
        // SavedGame::featuresOn's bit 1, "SUPER CRASH MODE" (STRING.ZAL lists the features' names from the last bit
        // to the first: "GUIDED GHOSTRIDES" is bit 0, kFeatureGhostRideSteering): the bail's bodies bounce back more
        // than they hit (restitution 1.15, else 0.55).
        constexpr uint16_t kFeatureSuperCrashMode = 0x0002;
        inline ee::Global<ee::Float, sym::g_superCrashRestitution> g_superCrashRestitution;
        inline ee::Global<ee::Float, sym::g_bailRestitution> g_bailRestitution;
        inline ee::Global<ee::Float, sym::g_bailForce> g_bailForce; // 0.15
        // A bail's bodies move no faster than 12 a second.
        constexpr ee::Float kBailMostSpeedSquared(144.0f);
        constexpr ee::Float kBailMostSpeed(12.0f);
    }

    // bailEnter (rider, from, to): physics state 2's enter: a rider with no player (no bail bodies) tumbles in state 12 instead. Else
    // the rider's dynamics made bouncy (more so with kFeatureSuperCrashMode) and woken, the limbs' rig reset and
    // stepping, the hands let go; each body's faces one-sided, driven from its bone, weighed and moving with the
    // rider (a second's velocity, at most 12); the bike tumbles; the ride distance and the air's frames start again.
    // The rider's turn is worked out as DM2's is and not read (BMX XXX gives every body the rider's velocity alone).
    // Returns the state asked for. DM2's bailEnter.
    uint32_t bailEnter(Rider *rider, uint32_t from, uint32_t to)
    {
        if (hasFlag(rider, kFlagNoPlayer))
            return tumbleEnter(rider, from, uint32_t(kPhysicsTumble));
        ee::Local<zge::Vec4> hipsPlace; // the hips' body's place (read and not used, as the rider's turn is)
        ee::Local<zge::Matrix> rotation;
        ee::Local<zge::Matrix> inverse;
        ee::Local<zge::EulerAngles> spin;
        zge::matrixIdentity(rotation);
        zge::matrixIdentity(inverse);
        zge::quaternionToMatrix(rotation, zge::objectOrientation(rider->messenger));
        zge::matrixRow(*rotation, 3) = zge::kZero;
        zge::matrixTranspose3(inverse, rotation);
        zge::matrixRow(*inverse, 3) = zge::kZero;
        spin->x = rider->pitch;
        spin->y = rider->yawRate;
        spin->z = rider->roll;

        const ee::Ptr<zge::Dynamics> bodies = rider->riderDynamics;
        zge::setDynamicsRestitution(bodies, (g_savedGame->featuresOn & kFeatureSuperCrashMode) ? *g_superCrashRestitution : *g_bailRestitution);
        zge::setDynamicsContactDamping(bodies, *g_bailForce);
        zge::wakeDynamics(rider->messenger, bodies, 1u);
        zge::resetBodyRig(ee::Ptr<zge::BodyRig>(rider->limbRig));
        limbRig(rider)->stepping = 1;
        if (!hasFlag(rider, kFlagNoPlayer))
        {
            rider->hands[0].holding = 0;
            rider->hands[1].holding = 0;
            clearFlag(rider, kFlagBailHands);
            zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[0].body));
            zge::disableContact(ee::Ptr<zge::BodyContact>(rider->hands[1].body));
            limbRig(rider)->leftHand->kind = 0;
            limbRig(rider)->rightHand->kind = 0;
        }
        // The hips' body (the first named so; with none, the quadword the original reads at a null body's place).
        ee::Ptr<zge::RigidBody> hipsBody = rider->riderDynamics->bodies;
        while (hipsBody && hipsBody->nameHash != *g_hipsName)
            hipsBody = hipsBody->next;
        *hipsPlace = hipsBody->position;
        for (ee::Ptr<zge::RigidBody> body = rider->riderDynamics->bodies; body; body = body->next)
        {
            zge::setBodyTwoSidedFaces(body, 0u);
            zge::driveBodyFromBone(rider->riderDynamics, body, 0.0f, 0.0f, 0.0f);
            body->force.z = 0.0f;
            body->force.x = 0.0f;
            body->force.y = tumbleWeight(*body);
            body->velocity = *vec4(rider->velocity) * ee::toFloat(kFramesASecondWhole);
            if (kBailMostSpeedSquared < zge::dot(body->velocity, body->velocity))
            {
                body->velocity = zge::unit(body->velocity);
                body->velocity = body->velocity * kBailMostSpeed;
            }
        }
        wakeBikeTumble(rider);
        rider->rideDistance = 0;
        rider->groundHeight = 0.0f;
        return to;
    }
    READABLE_FUNCTION(bailEnter, Verified);

    namespace
    {
        // The moves that follow the dynamics' bodies (the bail, the tumbles).
        inline ee::Global<int32_t, sym::g_drawBodies> g_drawBodies; // set: the bodies drawn after their step (debug)
        // The bail: in deep water the hips' body keeps 0.85 of its velocity a frame; the rider's bodies step 1/180
        // of a second, then twice 0.00555 (BMX XXX's sixtieth of a second, a hair short); the limbs' rig a sixtieth.
        inline ee::Global<ee::Float, sym::g_bailWaterDrag> g_bailWaterDrag;
        inline ee::Global<ee::Float, sym::g_bailFirstStep> g_bailFirstStep;
        inline ee::Global<ee::Float, sym::g_bailStep> g_bailStep;
        inline ee::Global<ee::Float, sym::g_bailRigStep> g_bailRigStep;
        // A body's velocity, a second's, to the rider's a frame's (1/60).
        inline ee::Global<ee::Float, sym::g_bailVelocityScale> g_bailVelocityScale;
        // The tumble (physics states 12 and 13): the touches' nearest starts at 1e6; the air's motions' bodies step
        // four times 1/240 of a second; their velocity to the rider's (1/60).
        inline ee::Global<ee::Float, sym::g_touchNearestStart> g_touchNearestStart;
        inline ee::Global<ee::Float, sym::g_tumbleStep> g_tumbleStep;
        inline ee::Global<ee::Float, sym::g_tumbleVelocityScale> g_tumbleVelocityScale;
        // The bike's tumble: its wheels keep 0.9967 of their spin a frame (none in deep water); its bodies step a
        // sixtieth of a second, as many steps as the dynamics take; the root body's velocity to the wheels' (1/60).
        inline ee::Global<ee::Float, sym::g_bikeTumbleSpinKept> g_bikeTumbleSpinKept;
        inline ee::Global<ee::Float, sym::g_bikeTumbleTime> g_bikeTumbleTime;
        inline ee::Global<ee::Float, sym::g_bikeTumbleVelocityScale> g_bikeTumbleVelocityScale;
        inline ee::Global<uint32_t, sym::g_hipsDeckBoneHash> g_bikeRootName; // the bike's root body: "Hipsdeck"

        // The sounds of a body's hit (soundEvent's: the game bank's sample by the body part, at the hit's level)
        // and of the rider's voice (one of his or her character's four, by the level).
        constexpr uint32_t kSoundBodyHit = 36;
        constexpr uint32_t kSoundRiderVoice = 37;
        constexpr uint8_t kBodyHitWait = 7;
        constexpr uint8_t kRiderVoiceWait = 40;
        // The bike's rolling sound in a tumble: kind 1, surface 3.
        constexpr uint32_t kTumbleRollKind = 1;
        constexpr uint32_t kTumbleRollSurface = 3;

        // The hardest hit the bodies made this frame sounds: its hit (unless one sounded within 7 frames) and,
        // from level 1, the rider's voice (unless within 40).
        void bodyHitSounds(Rider *rider)
        {
            const BodyTouches &touches = rider->touches;
            if (!(touches.flags & kTouchBump))
                return;
            if (rider->bumpWait == 0)
            {
                soundEvent(kSoundBodyHit, rider->position, ee::Address(touches.bumpPart), touches.bumpLevel, 0u);
                rider->bumpWait = kBodyHitWait;
            }
            if (rider->voiceWait == 0 && touches.bumpLevel != 0)
            {
                soundEvent(kSoundRiderVoice, rider->position, ee::Address(touches.bumpPart), touches.bumpLevel, riderIsFemale(rider));
                rider->voiceWait = kRiderVoiceWait;
            }
        }

        // The rider's rows from a body's turn (up and forward turned round), his velocity from the body's (a
        // frame's), his position the body's, no root offset; the speed.
        void followBody(Rider *rider, ee::Ptr<zge::RigidBody> body, ee::Float velocityScale)
        {
            zge::quaternionToMatrix(matrix(rider->right), &body->orientation);
            zge::vec3Negate(&vec4(rider->up)->xyz(), &vec4(rider->up)->xyz());
            zge::vec3Negate(&vec4(rider->forward)->xyz(), &vec4(rider->forward)->xyz());
            *vec4(rider->velocity) = zge::scaledCarrying(body->velocity, velocityScale, zge::scaleLeftover(zge::LeftBy::lastCall));
            *vec4(rider->position) = body->position;
            *vec4(rider->rootOffset) = zge::kZero;
            rider->speed = zge::vec3Length(&vec4(rider->velocity)->xyz());
        }
    }

    // bikeTumbleMove (rider): the bike's tumble a frame (the bail's, the tumbles'): the wheels spin down (stopped in
    // deep water), the bike's bodies step a sixtieth of a second, and the drawn bike follows its root body (the drawn
    // rows from its turn, the wheels' velocity, the drawn position). Then the rolling sound.
    void bikeTumbleMove(Rider *rider)
    {
        const ee::Float spinKept = *g_bikeTumbleSpinKept;
        for (WheelPose &wheel : rider->wheelPoses)
        {
            if (rider->contactKind == kContactWater)
                wheel.spin = 0;
            else
                wheel.spin = ee::toInt(ee::toFloat(wheel.spin) * spinKept);
        }
        ee::Float left = *g_bikeTumbleTime;
        do
            left = left - zge::updateDynamics(rider->bikeDynamics, ee::Ptr<zge::Sweep>(), left);
        while (ee::Float(0.0f) < left);
        if (*g_drawBodies != 0)
            zge::drawDynamicsObject(rider->bikeDynamics);
        const ee::Ptr<zge::RigidBody> root = bodyNamed(rider->bikeDynamics, *g_bikeRootName);
        zge::quaternionToMatrix(matrix(rider->body), &root->orientation);
        zge::vec3Negate(&vec4(&rider->body[4])->xyz(), &vec4(&rider->body[4])->xyz());
        zge::vec3Negate(&vec4(&rider->body[8])->xyz(), &vec4(&rider->body[8])->xyz());
        const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
        const ee::Float scale = *g_bikeTumbleVelocityScale;
        *vec4(rider->wheelPoses[0].velocity) = zge::scaledCarrying(root->velocity, scale, carried);
        *vec4(rider->wheelPoses[1].velocity) = zge::scaledCarrying(root->velocity, scale, carried);
        *vec4(rider->drawnPosition) = root->position;
        const int32_t sound = surfaceSoundFlags(0u, 0u, kTumbleRollKind, kTumbleRollSurface);
        game::tail(updateRollSound, rider->rollingVoice, uint32_t(sound), rider->drawnPosition, rider->wheelPoses[1].spin);
    }
    READABLE_FUNCTION(bikeTumbleMove, Verified);

    // riderTumbleMove (rider): the rider following the air's motions' bodies a frame (the tumble's, the air's in a
    // lip trick): the touches cleared, the bodies stepped four times (their contact callbacks fill the touches), the
    // hits' sounds, the rider moved with the hips' body. A lip trick whose bodies met a wall head on falls into the
    // air, its alignment kept.
    void riderTumbleMove(Rider *rider)
    {
        BodyTouches &touches = rider->touches;
        zge::memoryFill(&touches, 0, uint32_t(sizeof(BodyTouches)));
        touches.nearest = *g_touchNearestStart;
        for (uint32_t step = 0; step < 4; ++step)
            zge::updateDynamics(rider->riderAnimDynamics, ee::Ptr<zge::Sweep>(), *g_tumbleStep);
        bodyHitSounds(rider);
        const ee::Ptr<zge::RigidBody> hips = bodyNamed(rider->riderAnimDynamics, *g_hipsName);
        if (*g_drawBodies != 0)
            zge::drawDynamicsObject(rider->riderAnimDynamics);
        *vec4(rider->velocity) = zge::scaledCarrying(hips->velocity, *g_tumbleVelocityScale, zge::scaleLeftover(zge::LeftBy::lastCall));
        *vec4(rider->position) = hips->position;
        *vec4(rider->rootOffset) = zge::kZero;
        rider->speed = zge::vec3Length(&vec4(rider->velocity)->xyz());
        if ((touches.flags & (kTouchFace | kTouchWallHeadOn)) != (kTouchFace | kTouchWallHeadOn))
            return;
        if (physicsState(rider) != kPhysicsLipTrick)
            return;
        const zge::Matrix alignFrom = *matrix(rider->alignFrom);
        const int16_t alignAngles[3] = {rider->alignAngles[0], rider->alignAngles[1], rider->alignAngles[2]};
        const int16_t snapFrames = rider->snapFrames;
        setPhysicsState(rider, uint32_t(kPhysicsAir));
        *matrix(rider->alignFrom) = alignFrom;
        rider->snapFrames = snapFrames;
        for (int i = 0; i < 3; ++i)
            rider->alignAngles[i] = alignAngles[i];
    }
    READABLE_FUNCTION(riderTumbleMove, Verified);

    // tumbleMove (rider): physics states 12 and 13's move: the rider and the bike tumble.
    void tumbleMove(Rider *rider)
    {
        riderTumbleMove(rider);
        game::tail(bikeTumbleMove, rider);
    }
    READABLE_FUNCTION(tumbleMove, Verified);

    namespace
    {
        // Both hands holding: halfway between them.
        constexpr ee::Float kHalf(0.5f);
    }

    // pullUpEnter (rider, from, state): physics state 11's enter (the bail's pull-up off a held edge): the rider
    // put where the hands hold (between them when both do), still, the root offset none. Returns the state.
    uint32_t pullUpEnter(Rider *rider, uint32_t /*from*/, uint32_t state)
    {
        const BailHand &left = rider->hands[0];
        const BailHand &right = rider->hands[1];
        zge::Vec4 at;
        if (!left.holding)
            at = *vec4(right.nearest);
        else if (!right.holding)
            at = *vec4(left.nearest);
        else
        {
            at = *vec4(right.nearest) - *vec4(left.nearest);
            at = at * kHalf;
            at = at + *vec4(left.nearest);
        }
        *vec4(rider->position) = at;
        *vec4(rider->rootOffset) = zge::kZero;
        *vec4(rider->velocity) = zge::kZero;
        *vec4(rider->force) = zge::kZero;
        return state;
    }
    READABLE_FUNCTION(pullUpEnter, Verified);

    // pullUpExit (rider, from, to): physics state 11's exit: nothing to undo. Returns the state left.
    uint32_t pullUpExit(Rider * /*rider*/, uint32_t from, uint32_t /*to*/)
    {
        return from;
    }
    READABLE_FUNCTION(pullUpExit, Draft);

    // bailMove (rider): physics state 2's move: the hands held where they touched; in deep water the hips' body
    // slowed; the rider's bodies stepped (a sixtieth of a second in three steps) and the limbs' rig; the hands let
    // go and reach again; the hits' sounds; the rider follows his hips' body, and the bike tumbles. DM2's, which
    // drives the bodies to their bones and steps the bike's itself.
    void bailMove(Rider *rider)
    {
        if (!hasFlag(rider, kFlagNoPlayer))
            bailHandsHeld(rider);
        const ee::Ptr<zge::RigidBody> hips = bodyNamed(rider->riderDynamics, *g_hipsName);
        if (hips && rider->contactKind == kContactWater)
        {
            const zge::LeftBy by = hasFlag(rider, kFlagNoPlayer) ? zge::LeftBy::caller : zge::LeftBy::lastCall;
            hips->velocity = zge::scaledCarrying(hips->velocity, *g_bailWaterDrag, zge::scaleLeftover(by));
        }
        zge::updateDynamics(rider->riderDynamics, ee::Ptr<zge::Sweep>(), *g_bailFirstStep);
        const ee::Float step = *g_bailStep;
        zge::updateDynamics(rider->riderDynamics, ee::Ptr<zge::Sweep>(), step);
        zge::updateDynamics(rider->riderDynamics, ee::Ptr<zge::Sweep>(), step);
        zge::stepBodyRig(ee::Ptr<zge::BodyRig>(rider->limbRig), *g_bailRigStep);
        if (!hasFlag(rider, kFlagNoPlayer))
        {
            bailBodyRelease(rider);
            bailHands(rider);
        }
        bodyHitSounds(rider);
        followBody(rider, hips, *g_bailVelocityScale);
        game::tail(bikeTumbleMove, rider);
    }
    READABLE_FUNCTION(bailMove, Verified);

    namespace
    {
        // The bail's action state's sub-states (level 1 of kActionBail): DM2's first seven, then BMX XXX's own
        // pull-up on a held edge and the fall of a rider with no player (physics state 12, then 13 on the ground).
        enum BailSubState : uint8_t
        {
            kBailIn = 0,
            kBailTumbling = 1,
            kBailTumblingAgain = 2,  // after the hands let go
            kBailOver = 3,           // back to the start (unless the rider is held)
            kBailIntoWater = 4,      // into deep water: the splash sound, then hidden 120 frames
            kBailUnderWater = 5,
            kBailHandsReach = 6,     // the grind button held with a hand on an edge
            kBailPullUp = 7,         // the stick pulled back: the pull-up's motion (physics state 11)
            kBailPullingUp = 8,
            kBailNothing = 9,
            kBailFalling = 10,       // no player: falling in physics state 12
            kBailFallLanding = 11,   // on the ground (physics state 13)
            kBailFallLanded = 12,
        };
        constexpr uint32_t kMoveGrind = 7;
        constexpr uint32_t kMoveBailLook = 20;  // read when well above the ground, its answer unused (DM2's 18)
        constexpr uint32_t kMoveBailReach = 21; // the hands reach (DM2's 19)
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kCameraActionLevel = 1;
        constexpr uint32_t kCameraBailMode = 3;
        // The bail lasts at most 180 frames (five times that with kFeatureSuperCrashMode); slower than 0.03 for 31
        // frames it is over; the hands reach until its 301st frame; under water 120 frames; a fall's landing 61.
        inline ee::Global<uint32_t, sym::g_bailMostFrames> g_bailMostFrames;
        constexpr uint32_t kSuperCrashFramesTimes = 5;
        inline ee::Global<ee::Float, sym::g_bailStill> g_bailStill;
        inline ee::Global<ee::Float, sym::g_bailReachStill> g_bailReachStill;
        constexpr uint16_t kBailStillFrames = 31;
        constexpr uint16_t kBailReachMostFrames = 301;
        constexpr uint16_t kBailUnderWaterFrames = 120;
        constexpr uint16_t kFallLandedFrames = 61;
        constexpr ee::Float kPullUpStick = -0.5f;     // the stick pulled back past half
        constexpr ee::Float kWellAboveGround = 1.0f;  // a unit above the ground below
        // The splash: the sound event 55 with the water surface's activation's name.
        constexpr uint32_t kSoundNamed = 55;
        constexpr uint32_t kSplashKind = 3;
        constexpr uint32_t kSoundVolume = 127;
        // A fall's landing makes the air's motions' bodies less bouncy (0.05).
        inline ee::Global<ee::Float, sym::g_fallLandedRestitution> g_fallLandedRestitution;
        // The tumble's motions as each ends: "fall.10" slower than 0.03; else by the body's lean, "fall.08" or
        // ".09" rolled to a side, else "fall.00" .. "fall.07" by the tilt (an eighth of a turn each, the forward
        // row's way down 4 on).
        inline ee::Global<ee::Float, sym::g_bailMotionSpeed> g_bailMotionSpeed;
        constexpr zge::Angle kRolledMost = 0x1555;
        constexpr int32_t kTiltStep = 0x2000;
        constexpr int32_t kFallBackward = 4;
        constexpr int32_t kFallRolledLeft = 8;
        constexpr int32_t kFallRolledRight = 9;
        constexpr uint8_t kBailTumblesFrom = 6; // the sub-states before it tumble

        bool handHolds(const Rider *rider) { return rider->hands[0].holding != 0 || rider->hands[1].holding != 0; }
        // A downed rider's bail does not end, nor one still for the idle's frames.
        bool bailHeld(const Rider *rider) { return hasFlag(rider, kFlagDowned) || hasFlag(rider, kFlagIdle); }
        int32_t bailSubState(Rider *rider, uint32_t subState)
        {
            setActionState(rider, uint32_t(kLevelSubState), subState);
            return 0;
        }
        // Back to the start: the physics reset, riding on.
        int32_t bailRestart(Rider *rider)
        {
            resetPhysics(rider, 0u);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
            setActionState(rider, uint32_t(kLevelPhase), 4u);
            return 0;
        }
        void playMotion(Rider *rider, uint32_t name)
        {
            riderMotionByName(name, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
    }

    // bailState (rider): action state 8, the bail, by its sub-state (BailSubState). The motions while tumbling
    // follow the body's lean as each ends. DM2's, with BMX XXX's water, pull-up and fall of a rider with no player.
    int32_t bailState(Rider *rider)
    {
        ee::Local<GroundHit> hit;
        ee::Local<char[0x40]> motion;
        ee::Local<char[0x40]> sound;
        ButtonRecord *const input = &rider->input;
        const uint32_t layout = uint32_t(rider->controllerLayout);
        uint16_t &bailFrames = rider->actionFrames[kLevelAction];
        uint16_t &stateFrames = rider->actionFrames[kLevelSubState];
        // The tumble's motion as each ends (in the sub-states before the hands reach): by speed and the body's
        // lean. 1.
        const auto bailMotion = [&]() -> int32_t {
            if (!(actionState(rider, uint32_t(kLevelSubState)) < kBailTumblesFrom && hasFlag(rider, kFlagMotionEnded)))
                return 1;
            if (!(*g_bailMotionSpeed <= rider->speed))
            {
                riderMotionByName(sym::textFall10, &rider->motionName, &rider->motion);
                return 1;
            }
            if (zge::angleAcos(ee::abs(rider->right[1])) < kRolledMost)
            {
                zge::formatString(motion.address(), sym::textFallFormat, rider->right[1] < ee::Float(0.0f) ? kFallRolledLeft : kFallRolledRight);
            }
            else
            {
                const int32_t tilt = zge::angleAcos(-rider->up[1]);
                int32_t index = tilt / kTiltStep;
                if (rider->forward[1] < ee::Float(0.0f))
                    index = index + kFallBackward;
                zge::formatString(motion.address(), sym::textFallFormat, index);
            }
            riderMotionByName(motion.address(), &rider->motionName, &rider->motion);
            return 1;
        };
        switch (actionState(rider, uint32_t(kLevelSubState)))
        {
        case kBailIn:
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, kCameraBailMode);
            if (physicsState(rider) == kPhysicsTumble)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailFalling));
                rider->motionFlags |= kMotionRestart | kMotionFlag2;
            }
            else
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailTumbling));
                setFlag(rider, kFlagMotionEnded);
            }
            break;
        case kBailTumbling:
        case kBailTumblingAgain:
        {
            if (rider->contactKind == kContactWater)
                return bailSubState(rider, kBailIntoWater);
            if (groundBelow(rider->position, &*hit) && kWellAboveGround < ee::abs(hit->point[1] - rider->position[1]))
            {
                moveButtons(layout, input, kMoveBailLook, kMoveHeld);
                inputStick(input, 0u);
                inputStick(input, 0u);
            }
            if (!bailHeld(rider) && actionState(rider, uint32_t(kLevelSubState)) != kBailTumblingAgain &&
                moveButtons(layout, input, kMoveBailReach, kMoveHeld) != 0)
            {
                setFlag(rider, kFlagBailHands);
                if (handHolds(rider))
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailHandsReach));
            }
            uint32_t most = *g_bailMostFrames;
            if (g_savedGame->featuresOn & kFeatureSuperCrashMode)
                most = most * kSuperCrashFramesTimes;
            if (most < bailFrames)
                return bailSubState(rider, kBailOver);
            if (!(rider->speed < *g_bailStill))
                return bailMotion();
            if (++stateFrames < kBailStillFrames)
                return bailMotion();
            return bailSubState(rider, kBailOver);
        }
        case kBailOver:
            if (bailHeld(rider))
                return bailMotion();
            return bailRestart(rider);
        case kBailIntoWater:
            zge::formatString(sound.address(), sym::textWaterSurfaceActivate);
            soundEvent(kSoundNamed, rider->position, ee::Address(kSplashKind), sound.address(), kSoundVolume);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailUnderWater));
            [[fallthrough]];
        case kBailUnderWater:
            if (++stateFrames != kBailUnderWaterFrames)
                break;
            return bailRestart(rider);
        case kBailHandsReach:
            if (moveButtons(layout, input, kMoveGrind, kMoveHeld) != 0 && handHolds(rider) && !bailHeld(rider))
            {
                setFlag(rider, kFlagBailHands);
                if (!(bailFrames < kBailReachMostFrames))
                    return bailSubState(rider, kBailOver);
                if (rider->speed < *g_bailReachStill)
                    ++stateFrames;
                if (stateFrames < kBailStillFrames)
                    return bailMotion();
                if (inputStick(input, 0u)->y < kPullUpStick)
                    return bailSubState(rider, kBailPullUp);
                break;
            }
            clearFlag(rider, kFlagBailHands);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailTumblingAgain));
            bailFrames = 0;
            break;
        case kBailPullUp:
            if (rider->hands[0].holding != 0 && rider->hands[1].holding != 0)
                zge::formatString(motion.address(), sym::textFallPullup01);
            else
                zge::formatString(motion.address(), sym::textFallPullup00);
            setPhysicsState(rider, uint32_t(kPhysicsPullUp));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailPullingUp));
            playMotion(rider, motion.address());
            break;
        case kBailPullingUp:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailOver));
                return bailMotion();
            }
            break;
        case kBailFalling:
            if (*g_bailMostFrames < bailFrames ||
                (groundBelow(rider->position, &*hit) && hit->point[1] - rider->position[1] < kWellAboveGround))
            {
                setPhysicsState(rider, uint32_t(kPhysicsTumbleLanded));
                zge::formatString(motion.address(), sym::textFallLandStart);
                playMotion(rider, motion.address());
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailFallLanding));
                return 1;
            }
            if (hasFlag(rider, kFlagMotionEnded))
            {
                zge::formatString(motion.address(), sym::textFallFlailLoop);
                playMotion(rider, motion.address());
            }
            rider->motionFlags |= kMotionFlag2;
            break;
        case kBailFallLanding:
            zge::setDynamicsRestitution(rider->riderAnimDynamics, *g_fallLandedRestitution);
            if (hasFlag(rider, kFlagMotionEnded))
                return bailSubState(rider, kBailFallLanded);
            break;
        case kBailFallLanded:
            if (++stateFrames < kFallLandedFrames)
                return bailMotion();
            return bailSubState(rider, kBailOver);
        default:
            break;
        }
        return bailMotion();
    }
    READABLE_FUNCTION(bailState, Verified);
}
