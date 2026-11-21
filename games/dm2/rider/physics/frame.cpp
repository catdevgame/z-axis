// DM2's physics frame (physics.c): per rider, the physics state's callbacks
// in order (turn, forces, move, after), the wheels and the head posed.
//
// The callbacks are called through the state tables as the original calls
// them (setPhysicsState calls the enters the same way).
//
// Counterparts: DM2 GameCube 0x80076690;
// BMX XXX PS2 physicsFrame (medium).

#include "physics.h"

#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/physics/frame.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // A physics state's callback (the rider).
        using StateCallback = ee::FunctionPtr<void(Rider *rider)>;
        // The physics states' callbacks by state: 11 each (0: none).
        inline ee::Global<StateCallback[11], sym::g_turnCallbacks> g_turnCallbacks;
        inline ee::Global<StateCallback[11], sym::g_forceCallbacks> g_forceCallbacks;
        inline ee::Global<StateCallback[11], sym::g_moveCallbacks> g_moveCallbacks;
        inline ee::Global<StateCallback[11], sym::g_afterCallbacks> g_afterCallbacks;
        // The models' tint: (30 - groundHeight) x this, at least 4 (32 on the
        // ground: 30 x 1.0667), alpha 0x80.
        inline ee::Global<ee::Float, sym::g_tintScale> g_tintScale;
        constexpr int32_t kTintLeast = 4;
        // The bars' lean: the turn about x starts this far back (angle units).
        constexpr int32_t kBarsRest = 0x18E3;
        // Rider::flags cleared before the move (inferred: this frame's).
        constexpr uint32_t kFlagsThisFrame = 0x06000000u;

        int16_t angleOf(ee::Float turns) { return static_cast<int16_t>(ee::toInt(turns * ee::Float(65536.0f))); }
    }

    // A rider's physics frame: both models lit and tinted by the air's
    // frames (unless kFlagNoPlayer); the state's turn and forces; the wheels
    // turned by their spin; the move (entering a bail, the bail's move once
    // more); the state's after; the wheels' bones and the rider's head bone (by the
    // bars' lean) set. GameCube 0x80076690.
    void physicsFrame(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> bars;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<uint8_t[4]> tint;       // r, g, b, a

        if ((rider->flags & kFlagNoPlayer) == 0)
        {
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(rider->messenger), 1u);
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(rider->bikeModel), 1u);
        }
        const ee::Float shade = (ee::Float(30.0f) - rider->groundHeight) * *g_tintScale;
        uint8_t level = kTintLeast;
        if (ee::toInt(shade) >= kTintLeast)
            level = static_cast<uint8_t>(ee::toInt(shade) & 0xFF);
        tint[2] = level;
        tint[3] = 0x80;
        tint[0] = level;
        tint[1] = level;
        zge::setSceneRootColour(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->messenger)), tint);
        zge::setSceneRootColour(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->bikeModel)), tint);

        if (const StateCallback turnCallback = (*g_turnCallbacks)[physicsState(rider)])
            turnCallback(rider);
        if (const StateCallback forces = (*g_forceCallbacks)[physicsState(rider)])
            forces(rider);
        for (WheelPose &wheel : rider->wheelPoses)
            wheel.angle = static_cast<int16_t>(wheel.angle + static_cast<int16_t>(wheel.spin));
        rider->flags &= ~kFlagsThisFrame;

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
            zge::matrixRotateX(turn, turn, int32_t(wheel.angle));
            zge::matrixToQuaternion(rotation, turn);
            zge::setBoneRotation(wheel.bone, rotation);
        }

        if (rider->barLean[0] == ee::Float(0.0f) && rider->barLean[1] == ee::Float(0.0f))
            return;
        zge::matrixIdentity(bars);
        zge::matrixRotateX(bars, bars, int32_t(static_cast<int16_t>(angleOf(rider->barLean[0]) - kBarsRest)));
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, int32_t(angleOf(-rider->barLean[1])));
        engine::matrixProduct(bars, bars, turn);
        zge::matrixToQuaternion(rotation, bars);
        zge::setBoneRotation(rider->headBone, rotation);
    }
    READABLE_FUNCTION(physicsFrame, Verified);
}
