// BMX XXX's physics frame (physics.c): a rider's frame by its physics
// state's callbacks. DM2's (game/dm2/rider/physics/frame.cpp), over BMX
// XXX's 18 states.

#include "physics.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/physics/frame.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        // A physics state's callback (the rider).
        using StateCallback = ee::FunctionPtr<void(Rider *rider)>;
        // The physics states' callbacks by state (0: none).
        inline ee::Global<StateCallback[kPhysicsStates], sym::g_turnCallbacks> g_turnCallbacks;
        inline ee::Global<StateCallback[kPhysicsStates], sym::g_forceCallbacks> g_forceCallbacks;
        inline ee::Global<StateCallback[kPhysicsStates], sym::g_moveCallbacks> g_moveCallbacks;
        inline ee::Global<StateCallback[kPhysicsStates], sym::g_afterCallbacks> g_afterCallbacks;
        // The models' tint: (30 - groundHeight) x this, at least 4 (32 on the
        // ground: 30 x 1.0667), alpha 0x80.
        inline ee::Global<ee::Float, sym::g_tintScale> g_tintScale;
        constexpr int32_t kTintLeast = 4;
        // The bars' lean: the turn about x starts this far back (angle units).
        constexpr int32_t kBarsRest = 0x18E3;

        // What the rider's bodies touched (Rider::touches), cleared before
        // every frame's callbacks, its nearest face point then this far
        // (1,000,000, squared): the bodies' contact callback fills it.
        inline ee::Global<ee::Float, sym::g_frameRecordFar> g_frameRecordFar;

        int16_t angleOf(ee::Float turns) { return static_cast<int16_t>(ee::toInt(turns * ee::Float(65536.0f))); }
    }

    // A rider's physics frame: both models lit and tinted by the air's
    // frames (unless kFlagNoPlayer); the state's turn and forces; the wheels
    // turned by their spin; the move (entering a bail, the bail's move once
    // more); the state's after; the wheels' bones and the rider's head bone
    // (by the bars' lean) set. DM2's; BMX XXX marks the models' poses stale
    // after each bone it sets.
    void physicsFrame(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> bars;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<uint8_t[4]> tint; // r, g, b, a

        if (!hasFlag(rider, kFlagNoPlayer))
        {
            zge::setObjectCastsShadow(rider->messenger, 1u);
            zge::setObjectCastsShadow(rider->bikeModel, 1u);
        }
        const ee::Float shade = (ee::Float(30.0f) - rider->groundHeight) * *g_tintScale;
        uint8_t level = kTintLeast;
        if (ee::toInt(shade) >= kTintLeast)
            level = static_cast<uint8_t>(ee::toInt(shade) & 0xFF);
        tint[2] = level;
        tint[3] = 0x80;
        tint[0] = level;
        tint[1] = level;
        zge::setSceneRootColour(zge::sceneOf(rider->messenger), tint);
        zge::setSceneRootColour(zge::sceneOf(rider->bikeModel), tint);

        zge::memoryFill(&rider->touches, 0, uint32_t(sizeof(BodyTouches)));
        rider->touches.nearest = *g_frameRecordFar;

        if (const StateCallback turnCallback = (*g_turnCallbacks)[physicsState(rider)])
            turnCallback(rider);
        if (const StateCallback forces = (*g_forceCallbacks)[physicsState(rider)])
            forces(rider);
        for (WheelPose &wheel : rider->wheelPoses)
            wheel.angle = static_cast<int16_t>(wheel.angle + static_cast<int16_t>(wheel.spin));
        clearFlag(rider, kFlagOneWheelUpright);

        const uint8_t before = physicsState(rider);
        if (const StateCallback move = (*g_moveCallbacks)[physicsState(rider)])
            move(rider);
        const uint8_t after = physicsState(rider);
        if (after == kPhysicsBail && before != after)
            (*g_moveCallbacks)[kPhysicsBail](rider);
        if (const StateCallback afterCallback = (*g_afterCallbacks)[physicsState(rider)])
            afterCallback(rider);

        for (WheelPose &wheel : rider->wheelPoses)
        {
            zge::matrixIdentity(turn);
            zge::matrixRotateX(turn, turn, wheel.angle);
            zge::matrixToQuaternion(rotation, turn);
            zge::setBoneRotation(wheel.bone, rotation);
            zge::invalidateAnimator(rider->bikeModel);
        }

        if (rider->barLean[0] == ee::Float(0.0f) && rider->barLean[1] == ee::Float(0.0f))
            return;
        zge::matrixIdentity(bars);
        zge::matrixRotateX(bars, bars, static_cast<int16_t>(angleOf(rider->barLean[0]) - kBarsRest));
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, angleOf(-rider->barLean[1]));
        zge::matrixMultiply(bars, bars, turn);
        zge::matrixToQuaternion(rotation, bars);
        zge::setBoneRotation(rider->headBone, rotation);
        zge::invalidateAnimator(rider->messenger);
    }
    READABLE_FUNCTION(physicsFrame, Verified);
}
