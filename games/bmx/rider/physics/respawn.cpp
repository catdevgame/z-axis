// BMX XXX's rider put back (physics.c): the physics reset at the spawn
// face. DM2's (games/dm2/rider/physics/respawn.cpp), read in BMX XXX's
// machine code.

#include "physics.h"

#include "../collision/collision.h"
#include "../engine_calls.h"
#include "../rider.h"

#include "bmx/game/spawn_points.h"
#include "bmx/level/level_collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/scene/animate_system.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/respawn.cpp.h"

namespace game::bmx::rider
{
    using collision::FaceRef;

    namespace
    {
        // The forward row closer than this (0.99) to the face's normal is
        // no use for the rows: the up row turned round stands in.
        inline ee::Global<ee::Float, sym::g_forwardAlongNormal> g_forwardAlongNormal;
        // The soft parts settle for this many steps after a reset.
        constexpr int32_t kSoftPartsSettleSteps = 30;
    }

    namespace
    {
        // findSpawnFace's kinds: bit 0 the spawn's own locator (no search around the point), bit 1 any height.
        constexpr uint32_t kSpawnAtLocator = 0x1;
        constexpr uint32_t kSpawnAnyHeight = 0x2;
        // The search: 24 points on a widening spiral (the radius 0, then 1, then 1.1 times as far each time,
        // a sixth of a turn round each time).
        constexpr uint32_t kSpawnTries = 24;
        inline ee::Global<ee::Float, sym::g_spawnSpiralGrowth> g_spawnSpiralGrowth; // 1.1
        constexpr uint32_t kSpawnSpiralTurn = 0x2AAA0000; // a sixth of a turn, in the angle's upper half
        // A face to spawn on: its shortest side at least 1.5; within 4 below and 2 above the point (unless any
        // height); within 30 degrees of level; ground (surface kind 0 or 4), not water (sounds 6 and 7); and a
        // ball of 1 at 1.25 above its centre clear.
        constexpr ee::Float kSpawnLeastSide = 1.5f;
        constexpr ee::Float kSpawnMostBelow = 4.0f;
        constexpr ee::Float kSpawnMostAbove = -2.0f;
        constexpr zge::Angle kSpawnSteepest = 0x1556;
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceKind4 = 4;
        constexpr uint32_t kSoundDeepWater = 6;
        constexpr uint32_t kSoundWater = 7;
        constexpr ee::Float kSpawnClearLift = 1.25f;
        constexpr ee::Float kSpawnClearRadius = 1.0f;
        // Failing all, the locators: the rider's own, then "PlayerOne"'s, then straight below the world's middle.
        constexpr uint32_t kOwnLocator = 0;
        constexpr uint32_t kPlayerOneLocator = 0x06DA4725; // nameHash("PlayerOne")
        inline ee::Global<ee::Float, sym::g_spawnFallbackHeight> g_spawnFallbackHeight; // -10000 (high above)
    }

    // findSpawnFace (point, kind, rows): the face to spawn on near `point`, and the spawn's rows on it (its
    // centre, its normal up, squared to the forward row the rows held). Searched on a widening spiral round the
    // point for a face fit to ride on (see the constants); else the face below a locator, the rider's own or
    // "PlayerOne"'s; else the face below the world's middle. 0 with none (the rows the identity).
    FaceRef findSpawnFace(ee::Float *point, uint32_t kind, zge::Matrix *rows)
    {
        ee::Local<GroundHit> hit;
        ee::Local<zge::Vec4> probe;
        FaceRef face = 0;
        zge::matrixIdentity(rows);
        if (!(kind & kSpawnAtLocator))
        {
            ee::Float radius = 0.0f;
            uint32_t nextTurn = kSpawnSpiralTurn;
            zge::Angle angle = 0;
            probe->x = radius;
            for (uint32_t tries = kSpawnTries;;)
            {
                probe->y = 0.0f;
                probe->z = 0.0f;
                zge::vec3RotateY(&probe->xyz(), &probe->xyz(), angle);
                *probe = *probe + *vec4(point);
                if (groundBelow(&probe->x, &*hit) && !(hit->flags & kGroundObject) && kSpawnLeastSide <= collision::faceShortestSide(hit->face))
                {
                    const ee::Float below = hit->point[1] - point[1];
                    const bool height = (kind & kSpawnAnyHeight) || (below < kSpawnMostBelow && kSpawnMostAbove <= below);
                    if (height && zge::angleAcos(-hit->plane[1]) < kSpawnSteepest && collision::acceptAll())
                    {
                        const uint32_t surface = collision::faceSurfaceKind(hit->face);
                        if (surface == kSurfaceGround || surface == kSurfaceKind4)
                        {
                            const uint32_t sound = collision::faceSound(hit->face);
                            if (sound != kSoundDeepWater && sound != kSoundWater)
                            {
                                collision::faceCentre(hit->face, &probe->x);
                                probe->y = probe->y - kSpawnClearLift;
                                if (!collision::sphereQuery(&probe->x, kSpawnClearRadius))
                                    face = hit->face;
                            }
                        }
                    }
                }
                angle = static_cast<zge::Angle>(static_cast<int32_t>(nextTurn) >> 16);
                nextTurn += kSpawnSpiralTurn;
                radius = radius == ee::Float(0.0f) ? radius + ee::Float(1.0f) : radius * *g_spawnSpiralGrowth;
                if (face != 0 || --tries == 0)
                    break;
                probe->x = radius;
            }
        }
        if (face == 0 && locatorMatrix(point, kOwnLocator, rows))
        {
            *probe = rows->row(3);
            if (groundBelow(&probe->x, &*hit))
                face = hit->face;
        }
        if (face == 0 && locatorMatrix(point, kPlayerOneLocator, rows))
        {
            *probe = rows->row(3);
            if (groundBelow(&probe->x, &*hit))
                face = hit->face;
        }
        if (face == 0)
        {
            *probe = zge::kZero;
            probe->y = *g_spawnFallbackHeight;
            if (groundBelow(&probe->x, &*hit))
                face = hit->face;
        }
        if (face == 0)
            return 0;
        collision::faceCentre(face, &rows->row(3).x);
        collision::faceNormal(face, &rows->row(1).x);
        rows->row(0) = zge::cross(rows->row(1), rows->row(2));
        rows->row(0) = zge::unit(rows->row(0));
        rows->row(2) = zge::cross(rows->row(0), rows->row(1));
        return face;
    }
    READABLE_FUNCTION(findSpawnFace, Verified);

    // The rider's physics put back at the spawn face: still, on the ground,
    // the wheels at rest, the contact the face, the position the face's
    // centre lifted by half the wheels' heights. With bit 0 of
    // `rowsFromSpawn` the rows are the spawn's own; else the up row is the
    // spawn's and the others are made square to it from the forward row.
    // The soft parts' rig is put back and left to settle, and the limbs'
    // rig's stepping stopped. DM2's resetPhysics, which stands the rows on the
    // face's normal and knows no rigs.
    void resetPhysics(Rider *rider, uint32_t rowsFromSpawn)
    {
        ee::Local<zge::Matrix> spawnRows; // right, up, forward, position
        ee::Local<FaceRef> face;

        rider->touchedObject = 0;
        *vec4(rider->rootOffset) = zge::kZero;
        *vec4(rider->motionRoot) = zge::kZero;
        *vec4(rider->drawnOffset) = zge::kZero;
        *vec4(rider->velocity) = zge::kZero;
        *vec4(rider->force) = zge::kZero;
        rider->speed = 0.0f;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        rider->bodyPitch = 0;
        rider->rideDistance = 0;
        rider->pedalForce = 0.0f;
        rider->wheelDrag = 0.0f;
        for (WheelPose &wheel : rider->wheelPoses)
        {
            *vec4(wheel.velocity) = zge::kZero;
            wheel.speed = 0.0f;
            wheel.ground = kWheelRolling;
            wheel.angle = 0;
            wheel.spin = 0;
        }

        *face = findSpawnFace(rider->position, rowsFromSpawn, &*spawnRows);
        setContact(rider, &*face, 0u);
        collision::faceCentre(*face, rider->position);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        if ((rowsFromSpawn & 1) == 0)
        {
            const zge::Vec4 &normal = spawnRows->row(1);
            if (*g_forwardAlongNormal < ee::abs(zge::dot(forward, normal)))
                zge::vec3Negate(&forward.xyz(), &up.xyz());
            up = normal;
            right = zge::cross(up, forward);
            right = zge::unit(right);
            forward = zge::cross(right, up);
        }
        else
        {
            *matrix(rider->right) = *spawnRows;
        }
        *vec4(rider->rowsOrigin) = zge::kZero;
        zge::setObjectRotationMatrix(rider->messenger, ee::Ptr<zge::Matrix>(matrix(rider->right)));

        for (int i = 0; i < 3; ++i)
            rider->drawnPosition[i] =
                rider->position[i] + rider->up[i] * ((rider->frontWheelHeight + rider->backWheelHeight) * ee::Float(0.5f));
        *vec4(rider->position) = *vec4(rider->drawnPosition);
        *matrix(rider->body) = *matrix(rider->right);
        rider->physicsState = kPhysicsGround;

        if (rider->softParts)
        {
            zge::clearSoftPartsRotations(rider->softParts);
            zge::resetSoftPartsRig(rider->softParts);
            zge::settleSoftPartsRig(rider->softParts, kSoftPartsSettleSteps);
        }
        if (rider->limbRig)
            ee::Ptr<engine::BodyRig>(rider->limbRig)->stepping = 0;
    }
    READABLE_FUNCTION(resetPhysics, Verified);

    // A player's rider put back for a new start and shown, or hidden. Shown:
    // the three shown flags set, the motion to start over, no input, taps
    // or sticks, the hands free of the bail's hold, riding forwards, action
    // state riding, the physics reset where it stands (resetPhysics, the
    // rows stood on the face), and for a rider with a camera the bail's
    // body laid on the rider, still. Hidden: the flags cleared and both
    // models hidden. DM2's, which shows the models too when shown.
    void riderReset(uint32_t index, uint32_t shown)
    {
        Rider *rider = riderOf(index & 0xFFu).get();
        if (shown != 0)
        {
            setFlag(rider, kFlagShown);
            setFlag(rider, kFlagRiderModelShown);
            setFlag(rider, kFlagBikeModelShown);
        }
        else
        {
            clearFlag(rider, kFlagShown);
            clearFlag(rider, kFlagRiderModelShown);
            clearFlag(rider, kFlagBikeModelShown);
        }
        if (shown == 0)
        {
            zge::setSceneObjectShown(rider->messenger, 0u);
            game::tail(zge::setSceneObjectShown, rider->bikeModel, 0u);
            return;
        }

        rider->motionFlags = 0;
        rider->input.held = 0;
        rider->input.changed = 0;
        for (int i = 0; i < 2; ++i)
        {
            rider->input.previous[i].x = 0.0f;
            rider->input.previous[i].y = 0.0f;
            rider->input.sticks[i].x = 0.0f;
            rider->input.sticks[i].y = 0.0f;
        }
        for (uint8_t &tap : rider->taps)
            tap = kDirNone;
        rider->tapCount = 0;
        clearFlag(rider, kFlagBailHands);
        rider->tapIdleFrames = 0;
        rider->hands[0].holding = 0;
        rider->hands[1].holding = 0;
        if (rider->limbRig)
        {
            const ee::Ptr<engine::BodyRig> rig(rider->limbRig);
            rig->leftHand->kind = zge::kPointFree;
            rig->rightHand->kind = zge::kPointFree;
        }
        clearFlag(rider, kFlagFakie);
        clearFlag(rider, kFlagDowned);
        setFlag(rider, kFlagMotionEnded);
        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
        setActionState(rider, uint32_t(kLevelSubState), 0u);
        setActionState(rider, uint32_t(kLevelPhase), 4u);
        rider->voiceWait = 0;
        rider->bumpWait = 0;
        rider->physicsState = kPhysicsNone;
        resetPhysics(rider, 0u);
        if (!hasFlag(rider, kFlagNoPlayer))
        {
            // The bail's tumbling body at the rider, still.
            const ee::Ptr<zge::RigidBody> body = rider->bailBody;
            body->rotation = *matrix(rider->right);
            body->position = *vec4(rider->position);
            zge::sweepUpdateBody(levelSweep(), body);
            body->velocity = *vec4(rider->velocity);
            body->spin = zge::kZero;
            if (rider->limbRig)
                ee::Ptr<engine::BodyRig>(rider->limbRig)->stepping = 0;
        }
    }
    READABLE_FUNCTION(riderReset, Verified);
}
