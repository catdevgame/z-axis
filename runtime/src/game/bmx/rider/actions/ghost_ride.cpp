// BMX XXX's ghost ride (action.c): action state 10, entered from riding by a move of its own (ridingState). BMX
// XXX's own; named by its motions ("ground.ghostRide.start", ".loop").

#include "../rider.h"
#include "steering.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/player_riders.h"
#include "bmx/profile/players.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/ghost_ride.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The ghost ride's sub-states (level 1).
        enum GhostRideSubState : uint8_t
        {
            kGhostRideIn = 0,      // the start's motion
            kGhostRideGoing = 1,   // until the tumble (physics state 17)
            kGhostRideLoopIn = 2,  // the loop's motion
            kGhostRideEnding = 3,  // then back to riding
        };
        // The physics states it asks for: the ghost ride as it starts, its tumble once it has gone on for more
        // than 240 frames.
        constexpr uint32_t kGhostRideFrames = 240;
        // The loop plays at least this long before the ride ends (at the end of a loop).
        constexpr uint32_t kGhostRideEndFrames = 120;
        // Back to riding, coasting in this phase.
        constexpr uint32_t kCoastPhase4 = 4;
        // The camera's mode at level 1 the ghost ride puts it in (level 1's modes are not named).
        constexpr uint32_t kCameraActionLevel = 1;
        constexpr uint32_t kCameraGhostRideMode = 4;
        // Steering (steering.h) by the ghost ride's own least share and units, by the front wheel's speed.
        inline ee::Global<ee::Float, sym::g_ghostRideSteerLeast> g_ghostRideSteerLeast;         // 0.05
        inline ee::Global<ee::Float, sym::g_ghostRideDegreesToTurn> g_ghostRideDegreesToTurn; // 182.04: a degree in turn units
        using steering::steerAtSpeed;
        using steering::yawRateFalloff;

        // A motion started over.
        void startMotion(Rider *rider, ee::Local<char[0x40]> &text, uint32_t name)
        {
            zge::formatString(text.address(), name);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
    }

    // Action state 10, the ghost ride. By sub-state: the start's motion with physics state 16 (the yaw rate
    // stopped; the camera's ghost ride mode unless the rider keeps the camera); going on until physics state 17,
    // which it asks for after 240 frames, the stick steering it while the features menu has its feature switched
    // on (else the yaw rate as it was), the loop's motion each time the motion ends; then the loop's motion; and
    // after 120 more frames, at the end of a loop, the physics reset and back to riding (coasting, phase 4).
    // Counts its sub-state's frames while it stays. Returns 0 the frame the state changes.
    int32_t ghostRideState(Rider *rider)
    {
        ee::Local<char[0x40]> motionText;
        uint16_t &frames = rider->actionFrames[kLevelSubState];
        switch (actionState(rider, uint32_t(kLevelSubState)))
        {
        case kGhostRideIn:
            setPhysicsState(rider, kPhysicsGhostRide);
            rider->yawRate = 0;
            startMotion(rider, motionText, sym::textGroundGhostRideStart);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kGhostRideGoing));
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, kCameraGhostRideMode);
            break;
        case kGhostRideGoing:
            if (frames > kGhostRideFrames)
                setPhysicsState(rider, kPhysicsGhostRideTumble);
            if (physicsState(rider) == kPhysicsGhostRideTumble)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kGhostRideLoopIn));
                return 0;
            }
            if (g_savedGame->featuresOn & kFeatureGhostRideSteering)
            {
                const ee::Float steer = inputStick(&rider->input, 0u)->x;
                if (steer == ee::Float(0.0f))
                    yawRateFalloff(rider);
                else
                    steerAtSpeed(rider, steer, rider->wheelPoses[0].speed, *g_ghostRideSteerLeast, *g_ghostRideDegreesToTurn);
            }
            if (hasFlag(rider, kFlagMotionEnded))
                startMotion(rider, motionText, sym::textGroundGhostRideLoop);
            break;
        case kGhostRideLoopIn:
            startMotion(rider, motionText, sym::textGroundGhostRideLoop);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kGhostRideEnding));
            break;
        case kGhostRideEnding:
            if (frames > kGhostRideEndFrames && hasFlag(rider, kFlagMotionEnded))
            {
                resetPhysics(rider, 0);
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                setActionState(rider, uint32_t(kLevelPhase), kCoastPhase4);
                return 0;
            }
            break;
        default:
            break;
        }
        ++frames;
        return 1;
    }
    READABLE_FUNCTION_WAITS(ghostRideState, Verified);
}
