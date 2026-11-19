// BMX XXX's skitch (action.c): the rider holding on to a car, action state
// 9. BMX XXX's own; named by its motions ("ground.skitch.start", ".loop",
// ".end").

#include "../rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/skitch.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The skitch's sub-states (level 1).
        enum SkitchSubState : uint8_t
        {
            kSkitchIn = 0,       // the start's motion
            kSkitchStarting = 1, // until the hold (physics state 15)
            kSkitchLoopIn = 2,   // the loop's motion
            kSkitchHolding = 3,  // while the skitch move is held
        };
        constexpr uint32_t kMoveSkitch = 9;
        constexpr uint32_t kMoveHeld = 1;
        // The landing kind of a skitch's end. (The motions' texts are
        // copied through formatString before each start, as the original
        // does.)
        constexpr uint8_t kLandingSkitchEnd = 6;
        // Off a skitch into the air: the air's sub-state 3 in the phase
        // leaving a grind starts it in (grind.cpp's).
        constexpr uint32_t kAirOffGrind = 3;
    }

    // Action state 9, the skitch. In the air: "ground.skitch.end" and the
    // air (its sub-state 3 in phase 3), the motion's end cleared and
    // kFlagLeftRail set; on the ground: the landing with the skitch's end;
    // a bail: the bail. Else by sub-state: the start's motion (mirrored on
    // the bike's mirrored side), the start until the hold (physics state 15), the
    // loop's motion, and the hold while the skitch move is held (the loop
    // again each time it ends): let go, the physics back to the ground and
    // the landing. Counts its sub-state's frames while it stays. Returns 0
    // the frame the state changes.
    int32_t skitchState(Rider *rider)
    {
        ee::Local<char[0x40]> motionText;
        auto land = [&]() {
            rider->landingKind = kLandingSkitchEnd;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        };
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            zge::formatString(motionText.address(), sym::textGroundSkitchEnd);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            setActionState(rider, uint32_t(kLevelPhase), kAirOffGrind);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            setFlag(rider, kFlagLeftRail);
            return 0;
        case kPhysicsGround:
            return land();
        case kPhysicsBail:
        case kPhysicsTumble:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        default:
            break;
        }
        uint16_t &frames = rider->actionFrames[kLevelSubState];
        switch (actionState(rider, uint32_t(kLevelSubState)))
        {
        case kSkitchStarting:
            if (physicsState(rider) == kPhysicsSkitch)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kSkitchLoopIn));
                return 0;
            }
            break;
        case kSkitchIn:
            zge::formatString(motionText.address(), sym::textGroundSkitchStart);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            if (hasFlag(rider, kFlagMirrorSide))
                setFlag(rider, kFlagMirrored);
            else
                clearFlag(rider, kFlagMirrored);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kSkitchStarting));
            break;
        case kSkitchLoopIn:
            zge::formatString(motionText.address(), sym::textGroundSkitchLoop);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kSkitchHolding));
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        case kSkitchHolding:
            if (moveButtons(uint32_t(rider->controllerLayout), &rider->input, kMoveSkitch, kMoveHeld) == 0)
            {
                setPhysicsState(rider, uint32_t(kPhysicsGround));
                return land();
            }
            if (hasFlag(rider, kFlagMotionEnded))
                rider->motionFlags |= kMotionRestart;
            break;
        default:
            break;
        }
        ++frames;
        return 1;
    }
    READABLE_FUNCTION_WAITS(skitchState, Draft);
}
