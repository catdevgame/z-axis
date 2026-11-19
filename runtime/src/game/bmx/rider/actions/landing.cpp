// BMX XXX's landing action state (action.c). DM2's (game/dm2/rider/actions/
// states.cpp), with BMX XXX's motions by name and its landing kinds.

#include "../rider.h"
#include "steering.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/landing.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // "air.coast.start": falling off again. "ground.coast.loop": the
        // plain landing. "ground.land.%02d": the landings 1 .. 4, numbered
        // from 0. "ground.skitch.end": landing kind 6.
        constexpr uint32_t kAirCoastStart = sym::textAirCoastStart;
        constexpr uint32_t kGroundCoastLoop = sym::textGroundCoastLoop;
        constexpr uint32_t kGroundSkitchEnd = sym::textGroundSkitchEnd;
        constexpr uint8_t kLandingKinds = 7;
        constexpr uint8_t kLandingNumbered = 5; // kinds 1 .. 4 have numbered motions
        constexpr uint8_t kLandingSkitchEnd = 6;
        // The frames a landing of each kind lasts (7, 7, 40, 60, 90, 10, 10).
        inline ee::Global<uint32_t[kLandingKinds], sym::g_landingFrames> g_landingFrames;

        // Steering while landing (steering.h): 15 times the stick a frame toward 75 times it, both less with
        // speed (1 / (1 + 30 speed), 0.05 at least); still, 3 times the stick toward 6 degrees. In degrees a
        // frame; 182.04 turn units a degree.
        inline ee::Global<ee::Float, sym::g_landSteerLeast> g_landSteerLeast;
        inline ee::Global<ee::Float, sym::g_landDegreesToTurn> g_landDegreesToTurn;
        inline ee::Global<ee::Float, sym::g_landStillDegreesToTurn> g_landStillDegreesToTurn;
        using steering::kLandStillMost;
        using steering::steerAtSpeed;
        using steering::steerYawRate;
        using steering::yawRateFalloff;
    }

    // Action state 7, landing. In the air again: the air state, falling. A
    // bail (physics state 2 or 12): the bail state. Else the stick steers
    // (the yaw rate falls off with the stick let go), and by sub-state: the
    // landing's motion by its kind, then its frames counted until the kind's
    // length is past and the rider rides on, coasting in its phase 2 with
    // the landing kind kept. DM2's landingState, which knows a soft and a
    // hard landing only.
    int32_t landingState(Rider *rider)
    {
        ee::Local<char[0x40]> name;
        ee::Local<uint32_t[kLandingKinds]> lengths;
        for (uint32_t kind = 0; kind < kLandingKinds; ++kind)
            lengths[kind] = (*g_landingFrames)[kind];

        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsBail || physics == kPhysicsTumble)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }
        if (physics == kPhysicsAir)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        }

        const ee::Float steer = inputStick(&rider->input, 0u)->x;
        if (steer == ee::Float(0.0f))
            yawRateFalloff(rider);
        else
        {
            const ee::Float speed = rider->speed;
            if (ee::Float(0.0f) < speed)
                steerAtSpeed(rider, steer, speed, *g_landSteerLeast, *g_landDegreesToTurn);
            else
            {
                const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * ee::Float(3.0f) * *g_landStillDegreesToTurn);
                steerYawRate(rider, turned, kLandStillMost);
            }
        }

        const uint8_t phase = actionState(rider, uint32_t(kLevelSubState));
        if (phase == kPhaseIn)
        {
            const uint8_t kind = rider->landingKind;
            if (kind != 0 && kind < kLandingNumbered)
            {
                zge::formatString(name.address(), sym::textGroundLandFormat, kind - 1);
                riderMotionByName(name.address(), &rider->motionName, &rider->motion);
            }
            else if (kind == kLandingSkitchEnd)
                riderMotionByName(kGroundSkitchEnd, &rider->motionName, &rider->motion);
            else
                riderMotionByName(kGroundCoastLoop, &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kPhaseHeld));
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        if (phase != kPhaseHeld)
            return 1;
        const uint8_t kind = rider->landingKind;
        const uint16_t frames = ++rider->actionFrames[kLevelSubState];
        if (!(lengths[kind] < frames))
            return 1;
        // Riding on: coasting in its phase 2, the landing kind kept for it.
        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
        setActionState(rider, uint32_t(kLevelPhase), 2u);
        rider->landingKind = kind;
        return 0;
    }
    READABLE_FUNCTION(landingState, Verified);
}
