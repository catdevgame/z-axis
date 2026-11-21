// BMX XXX's riding action state and its sub-states (action.c): pedalling, the
// brake, the crouch, riding backwards. DM2's
// (games/dm2/rider/actions/states.cpp), with BMX XXX's motions by name and
// its own phases.

#include "../rider.h"
#include "steering.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/fx/spawn_effects.h"
#include "bmx/game/player_riders.h"
#include "bmx/weapon/weapon.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "sym/bmx/rider/actions/riding.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // "ground.pedal.loop" and "ground.pedal.end": the pedalling's motions.
        constexpr uint32_t kGroundPedalLoop = sym::textGroundPedalLoop;
        constexpr uint32_t kGroundPedalEnd = sym::textGroundPedalEnd;
        // Pedalling's phases (BMX XXX's own: DM2 has in and held).
        enum PedalPhase : uint8_t
        {
            kPedalIn = 0,       // entered this frame
            kPedalStarting = 1, // the motion before it playing to its end
            kPedalLoop = 2,     // the loop, a turn of the pedals at a time
            kPedalEnd = 3,      // start the end's motion
            kPedalEnding = 4,   // the end's motion playing
        };
        constexpr int32_t kTurn50Degrees = 0x238E;
        // moveButtons' moves and tests (DM2's).
        constexpr uint32_t kMovePedal = 1;
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kMoveReleased = 2;
        // The stick pulled back past this (0.95) with the bike near level
        // is the brake.
        inline ee::Global<ee::Float, sym::g_brakeStick> g_brakeStick;
        // Pedalling pushes (0.0325) below this share (0.2667) of the top
        // speed stat; the automatic pedalling goes on until 0.01475 short
        // of it.
        inline ee::Global<ee::Float, sym::g_pedalSpeed> g_pedalSpeed;
        inline ee::Global<ee::Float, sym::g_pedalPush> g_pedalPush;
        inline ee::Global<ee::Float, sym::g_pedalRepeat> g_pedalRepeat;
        // The pedalling motion's rate: 8 times the speed, 1 at least.
        constexpr ee::Float kPedalRatePerSpeed = 8.0f;
        // The brake's motions: "ground.brake.start", ".loop", ".end", and the
        // skid's: "ground.skid.start", ".loop", ".00" (out of it moving),
        // ".01" (out of it stopped).
        constexpr uint32_t kGroundBrakeStart = sym::textGroundBrakeStart;
        constexpr uint32_t kGroundBrakeLoop = sym::textGroundBrakeLoop;
        constexpr uint32_t kGroundBrakeEnd = sym::textGroundBrakeEnd;
        constexpr uint32_t kGroundSkidStart = sym::textGroundSkidStart;
        constexpr uint32_t kGroundSkidLoop = sym::textGroundSkidLoop;
        constexpr uint32_t kGroundSkidOutMoving = sym::textGroundSkid00;
        constexpr uint32_t kGroundSkidOutStopped = sym::textGroundSkid01;
        // The brake's phases (DM2's manual phases, with two ways out of
        // the skid).
        enum BrakePhase : uint8_t
        {
            kBrakeIn = 0,
            kBrakeHeld = 1,
            kBrakeOut = 2,
            kBrakeOutEnding = 3,
            kSkidIn = 4,
            kSkid = 5,
            kSkidOutMoving = 6,
            kSkidOutStopped = 7,
        };
        // The two turn moves (DM2's 10 and 11): held unevenly they make the
        // brake a skid.
        constexpr uint32_t kMoveTurnLeft = 11;
        constexpr uint32_t kMoveTurnRight = 12;
        // The brake holds while the stick is back past this (0.95).
        inline ee::Global<ee::Float, sym::g_brakeHoldStick> g_brakeHoldStick;
        // The wheels' drag the brake asks for: from 1/15 of the full drag
        // (0.05), 1.12 times more a frame.
        inline ee::Global<ee::Float, sym::g_brakeDrag> g_brakeDrag;
        inline ee::Global<ee::Float, sym::g_brakeDragStart> g_brakeDragStart;
        inline ee::Global<ee::Float, sym::g_brakeDragGrowth> g_brakeDragGrowth;
        // The crouch's motions: "ground.crouch.start", ".loop", and
        // ".end" for the hop out of it.
        constexpr uint32_t kGroundCrouchStart = sym::textGroundCrouchStart;
        constexpr uint32_t kGroundCrouchLoop = sym::textGroundCrouchLoop;
        constexpr uint32_t kGroundCrouchEnd = sym::textGroundCrouchEnd;
        enum CrouchPhase : uint8_t
        {
            kCrouchIn = 0,
            kCrouchGoingDown = 1, // the start motion playing (still pedalling at half push)
            kCrouchHeld = 2,      // the loop
        };
        // The hop: from the least (0.0917) to 0.15 times the air stat by
        // the crouch's frames, full at 15; rolling up a slope takes 0.25 of
        // it a unit of the climb.
        inline ee::Global<ee::Float, sym::g_hopPerStat> g_hopPerStat;
        inline ee::Global<ee::Float, sym::g_hopLeast> g_hopLeast;
        inline ee::Global<ee::Float, sym::g_hopSlopeLoss> g_hopSlopeLoss;
        inline ee::Global<uint32_t, sym::g_hopFullFrames> g_hopFullFrames;
        // Riding backwards' motions: "ground.reverse.start", ".loop", ".end"
        // (the turn round), ".end.coast" and ".end.stand" (out of it).
        constexpr uint32_t kGroundReverseStart = sym::textGroundReverseStart;
        constexpr uint32_t kGroundReverseLoop = sym::textGroundReverseLoop;
        constexpr uint32_t kGroundReverseEnd = sym::textGroundReverseEnd;
        constexpr uint32_t kGroundReverseEndCoast = sym::textGroundReverseEndCoast;
        constexpr uint32_t kGroundReverseEndStand = sym::textGroundReverseEndStand;
        // Riding backwards' phases (DM2's first seven, and BMX XXX's own).
        enum FakiePhase : uint8_t
        {
            kFakieIn = 0,
            kFakieStarting = 1,
            kFakieRolling = 2,
            kFakieTurning = 3,
            kFakieCrouched = 4,
            kFakieOutRolling = 5,
            kFakieOutStill = 6,
            kFakieStartingToTurn = 7, // the start motion, then the turn round at once
            kFakieTurnIn = 8,         // start the turn round's motion
            kFakiePedalOut = 9,       // pedalled out of it nearly still: the coasting end, then pedalling
        };
        constexpr uint32_t kMoveFirst = 0; // the first move of a configuration (not named)
        constexpr uint32_t kMovePressed = 0;
        constexpr uint32_t kMoveTurnRound = 10; // (DM2's 9)
        // Pedalling out of riding backwards works below this speed.
        constexpr ee::Float kFakiePedalOutSpeed = 0.125f;
        // With the stick back past 0.95 and the bike near level the wheels
        // drag as the brake's; else a share (1/30) of it. Read once while
        // rolling and once at the state's end, from two pairs of values.
        inline ee::Global<ee::Float, sym::g_fakieRollBrakeStick> g_fakieRollBrakeStick;
        inline ee::Global<ee::Float, sym::g_fakieRollDragShare> g_fakieRollDragShare;
        inline ee::Global<ee::Float, sym::g_fakieBrakeStick> g_fakieBrakeStick;
        inline ee::Global<ee::Float, sym::g_fakieDragShare> g_fakieDragShare;
        // A motion started over, and the phase after it.
        void motionThen(Rider *rider, uint32_t motion, uint32_t phase)
        {
            riderMotionByName(motion, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), phase);
        }
        // "air.coast.start": off the ground.
        constexpr uint32_t kAirCoastStart = sym::textAirCoastStart;
        // The riding action's sub-states: coasting, pedalling, the brake,
        // the crouch, riding backwards.
        using SubState = ee::FunctionPtr<int32_t(Rider *)>;
        inline ee::Global<SubState[5], sym::g_ridingSubStates> g_ridingSubStates;
        // Steering on the ground: as the landing's (steering.h: 15 times the stick a frame toward 75 times it,
        // less with speed; still, 3 times the stick toward 6 degrees), by riding's own least share and units.
        inline ee::Global<ee::Float, sym::g_rideSteerLeast> g_rideSteerLeast;
        inline ee::Global<ee::Float, sym::g_rideDegreesToTurn> g_rideDegreesToTurn;
        inline ee::Global<ee::Float, sym::g_rideStillDegreesToTurn> g_rideStillDegreesToTurn;
        using steering::kLandStillMost;
        using steering::steerAtSpeed;
        using steering::steerYawRate;
        using steering::yawRateFalloff;
        // The riding state's moves (the grind is DM2's 7; the others are
        // BMX XXX's own and not named) and what they lead to.
        constexpr uint32_t kMoveGrind = 7;
        constexpr uint32_t kMoveSkitch = 9;   // held: the skitch
        constexpr uint32_t kMove19 = 19; // pressed: the ghost ride
        constexpr uint32_t kMoveAskObjective = 24; // held: kFlagObjectiveAsked (the skitch's button in every layout)
        // The one-wheel states the stick's taps ask for: down after up the
        // manual, up after down the other (physics state 9), the two taps
        // less than 15 frames apart.
        constexpr uint32_t kPhysicsState9 = 9;
        constexpr uint32_t kTapsApartMost = 15;
        constexpr uint8_t kCoastStopped = 1; // coasting's phase at a stop (its level-3 step past 0: no steering)
        constexpr uint32_t kLevelStep = 3;
        ee::Float pedalRate(ee::Float speed)
        {
            const ee::Float rate = speed * kPedalRatePerSpeed;
            return ee::Float(1.0f) <= rate ? rate : ee::Float(1.0f);
        }
    }

    namespace
    {
        // Coasting's phases (DM2's, with its own motions by name).
        enum CoastPhase : uint8_t
        {
            kCoastIn = 0,       // a landing's motion, else standing or the coast's loop
            kCoastStill = 1,    // stopped, standing (its steps below)
            kCoastRolling = 2,  // the coast's loop
            kCoastStopping = 3, // "ground.stand.start" playing
            kCoastStanding = 4, // a stand motion, then still
        };
        // The still phase's steps (level 3). BMX XXX's still rider aims the
        // weapon with the second stick, where DM2's leans on a foot.
        enum StillStep : uint8_t
        {
            kStillWaiting = 0,   // the bars' lean falls off; long enough still, the rider idles
            kStillAimIn = 1,     // the camera to the aim's mode
            kStillAiming = 2,    // the bars and the shooter's aim follow the second stick
            kStillAimOut = 3,    // the camera back, the crosshairs hidden
            kStillPedalIn = 4,   // "ground.stand.end", then pedalling
            kStillPedal = 5,
            kStillLeaveIn = 6,   // "ground.stand.end", then the flatland tricks
            kStillLeave = 7,
        };
        // "ground.land.%02d", "ground.stand.%02d", "ground.stand.start",
        // "ground.stand.end", "ground.coast.loop".
        constexpr uint32_t kGroundStandStart = sym::textGroundStandStart;
        constexpr uint32_t kGroundStandEnd = sym::textGroundStandEnd;
        constexpr uint32_t kGroundCoastLoop = sym::textGroundCoastLoop;
        // A landing kind past these has no numbered motion of its own.
        constexpr uint8_t kLandingNumbered = 5;
        // The stand motions: one of four by the action's frames (none while
        // bits 6 .. 8 of them are set).
        constexpr uint16_t kStandPickMask = 0x1C0;
        constexpr uint32_t kStandPickShift = 10;
        constexpr uint32_t kStandMotions = 4;
        // The stick pulled back past this (0.95) while moving and upright is
        // the brake; pedalling needs it not pulled back past the other (0.95).
        inline ee::Global<ee::Float, sym::g_coastBrakeStick> g_coastBrakeStick;
        inline ee::Global<ee::Float, sym::g_coastPedalStick> g_coastPedalStick;
        // The second stick off centre by this (0.1) for 8 frames starts the
        // aim; at centre for 31 it ends it.
        constexpr uint32_t kSecondStick = 1;
        inline ee::Global<ee::Float, sym::g_aimStickLeast> g_aimStickLeast;
        constexpr uint16_t kAimInFrames = 8;
        constexpr uint16_t kAimOutFrames = 31;
        // The second stick's dead zone: under 0.25 none, the rest scaled to
        // a full 1.
        constexpr ee::Float kAimDeadZone = 0.25f;
        constexpr ee::Float kAimLive = 0.75f;
        // The bars lean toward the second stick (DM2's rest lean): a share a
        // frame of the way to its y and its x turned over.
        inline ee::Global<ee::Float, sym::g_restLeanX> g_restLeanX;
        inline ee::Global<ee::Float, sym::g_restLeanY> g_restLeanY;
        inline ee::Global<ee::Float, sym::g_restLeanShare> g_restLeanShare;
        // Waiting, the lean falls off by a share a frame; under 9/32768 it
        // is none.
        inline ee::Global<ee::Float, sym::g_barLeanFalloff> g_barLeanFalloff;
        const ee::Float kLeanStopped(9.0f / 32768.0f);
        // A lean is a turn of 0x10000 a whole one.
        constexpr ee::Float kLeanToAngle = 65536.0f;
        // The shot's start in the rider's space: half a unit up, a little
        // forward (0.1).
        constexpr ee::Float kMuzzleUp = 0.5f;
        inline ee::Global<ee::Float, sym::g_muzzleForward> g_muzzleForward;
        // Still this long (3601 frames) the rider idles; each 150 frames of
        // it the sleep effect shows (not in level 12, which level that is:
        // not read), three quarters of a unit up, a quarter forward, lifted.
        constexpr uint16_t kIdleFrames = 0xE11;
        constexpr uint32_t kSleepEvery = 150;
        constexpr uint8_t kLevel12 = 12;
        constexpr uint32_t kSleepLifetime = 3500;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        inline ee::Global<ee::Float, sym::g_sleepEffectLift> g_sleepEffectLift;
        // The eyelids' steps the idle's start and its end put them to.
        constexpr uint8_t kEyelidsIdle = 3;
        constexpr uint8_t kEyelidsWoken = 5;
        // The camera's level and its modes the aim takes (level 1; mode 5
        // the aim's, 0 the first).
        constexpr uint32_t kCameraActionLevel = 1;
        constexpr uint32_t kCameraAimMode = 5;
        constexpr uint32_t kCameraFirstMode = 0;
        // "Visible" (the crosshairs' variable).
        constexpr uint32_t kVisible = sym::textActionVisible;

        // A motion started, the phase set, then the restart asked.
        void motionIntoPhase(Rider *rider, uint32_t motion, uint32_t phase)
        {
            riderMotionByName(motion, &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), phase);
            rider->motionFlags |= kMotionRestart;
        }
        bool upright(const Rider *rider) { return zge::angleAcos(-rider->up[1]) < kTurn50Degrees; }
        bool level(const Rider *rider) { return zge::angleAsin(ee::abs(rider->forward[1])) < kTurn50Degrees; }
        // The camera of the rider's player to a mode, unless the rider keeps
        // the camera or the camera module is busy.
        void riderCameraMode(Rider *rider, uint32_t mode)
        {
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, mode);
        }
        // A lean a share of the way to its target, none when it is that small.
        ee::Float leanToward(ee::Float lean, ee::Float target, ee::Float share) { return lean + (target - lean) * share; }
        void clearSmallLean(ee::Float &lean)
        {
            if (ee::abs(lean) < kLeanStopped)
                lean = 0.0f;
        }
        // A stick's lane through the dead zone.
        ee::Float aimLane(ee::Float lane)
        {
            if (ee::abs(lane) < kAimDeadZone)
                return 0.0f;
            const ee::Float past = ee::Float(0.0f) <= lane ? lane - kAimDeadZone : lane + kAimDeadZone;
            return past / kAimLive;
        }
    }

    // Riding sub-state 0, coasting. Unless still and doing something there:
    // while moving, a grind asked on a rail (contact kinds 1 .. 3, the grind
    // button pressed while riding) goes to physics state 4, the crouch move
    // crouches (configuration 0: when let go), the stick pulled back while
    // upright brakes; on the level with the stick not pulled back, the
    // pedal move (or, moving and slow enough, the automatic pedalling; or
    // the first move pressed, stopped or with the automatic pedalling off)
    // pedals, from the still phase through its pedal steps. Then by phase:
    // a landing's numbered motion, or standing (stopped and upright) or the
    // coast's loop; still (below); the loop until stopped on the level;
    // the stand's start, then a stand motion, then still. Still, not upright
    // it coasts; else a stand motion again each time one ends, the full
    // drag, and by step: waiting (the second stick held off centre starts
    // the aim; long still the rider idles, with the sleep effect; the bars'
    // lean falls off), the aim (the camera's aim mode; the bars and the
    // shooter's aim and shot start follow the second stick until it is at
    // centre a while; then the camera back and the crosshairs hidden), and
    // "ground.stand.end" before pedalling or the flatland tricks (pedalling
    // pushed meanwhile). The steps that stay clear the taps but the
    // stick's. Returns 0 the frame the state changes. DM2's coastState with
    // BMX XXX's motions, the aim in place of the foot down, and the brake.
    int32_t coastState(Rider *rider)
    {
        ee::Local<ee::Float[2]> leanTarget;
        ee::Local<ee::Float[2]> aimStick;
        ee::Local<zge::Matrix> aim;
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Vec4> muzzle;
        ee::Local<char[0x40]> motionText;
        ee::Local<zge::Vec4> sleepAt;
        ee::Local<uint32_t> hidden;
        ButtonRecord *const input = &rider->input;
        const uint32_t layout = rider->controllerLayout;
        const bool busyStill =
            actionState(rider, uint32_t(kLevelPhase)) == kCoastStill && actionState(rider, kLevelStep) != kStillWaiting;
        if (!busyStill)
        {
            if (ee::Float(0.0f) < rider->speed)
            {
                const uint8_t kind = rider->contactKind;
                if (kind < 4 && kind != 0 && moveButtons(layout, input, kMoveGrind, kMoveHeld) != 0 &&
                    hasFlag(rider, kFlagGrindPressed) && setPhysicsState(rider, uint32_t(kPhysicsGrind)) == kPhysicsGrind)
                    return 0;
                const bool crouch = rider->controllerLayout != 0 ? moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0
                                                                 : moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0;
                if (crouch)
                {
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCrouch));
                    return 0;
                }
                if (*g_coastBrakeStick < inputStick(input, 0u)->y && upright(rider))
                {
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingBrake));
                    return 0;
                }
            }
            if (level(rider) && inputStick(input, 0u)->y <= *g_coastPedalStick)
            {
                bool pedal = moveButtons(uint32_t(rider->controllerLayout), input, kMovePedal, kMoveHeld) != 0;
                if (!pedal)
                {
                    const ee::Float speed = rider->speed;
                    pedal = !(speed == ee::Float(0.0f)) && rider->controllerLayout < 2 && !hasFlag(rider, kFlagOneWheelUpright) &&
                            hasFlag(rider, kFlagAutoPedal) && speed < *g_pedalSpeed * rider->speedStat - *g_pedalRepeat;
                    if (!pedal && moveButtons(uint32_t(rider->controllerLayout), input, kMoveFirst, kMovePressed) != 0)
                        pedal = rider->speed == ee::Float(0.0f) || !hasFlag(rider, kFlagAutoPedal);
                }
                if (pedal)
                {
                    if (actionState(rider, uint32_t(kLevelPhase)) == kCoastStill)
                    {
                        setActionState(rider, kLevelStep, uint32_t(kStillPedalIn));
                        return 0;
                    }
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingPedal));
                    setFlag(rider, kFlagAutoPedal);
                    return 0;
                }
            }
        }

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kCoastIn:
        {
            const uint8_t kind = rider->landingKind;
            if (kind < kLandingNumbered && kind != 0)
            {
                zge::formatString(motionText.address(), sym::textGroundLandFormat, kind - 1u);
                riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
                rider->motionFlags &= ~kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCoastRolling));
                return 1;
            }
            if (rider->speed == ee::Float(0.0f) && upright(rider))
                motionIntoPhase(rider, kGroundStandStart, kCoastStopping);
            else
                motionIntoPhase(rider, kGroundCoastLoop, kCoastRolling);
            return 1;
        }
        case kCoastStill:
            break;
        case kCoastRolling:
            if (!hasFlag(rider, kFlagMotionEnded))
                return 1;
            if (rider->speed == ee::Float(0.0f) && level(rider))
                motionIntoPhase(rider, kGroundStandStart, kCoastStopping);
            else
                motionIntoPhase(rider, kGroundCoastLoop, kCoastRolling);
            return 1;
        case kCoastStopping:
            if (!hasFlag(rider, kFlagMotionEnded))
                return 1;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCoastStanding));
            return 0;
        case kCoastStanding:
        {
            const uint16_t frames = rider->actionFrames[kLevelAction];
            const uint32_t pick = (frames & kStandPickMask) == 0 ? (frames >> kStandPickShift) % kStandMotions : 0u;
            zge::formatString(motionText.address(), sym::textGroundStandFormat, pick);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCoastStill));
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        default:
            return 1;
        }

        // Still.
        if (!upright(rider))
        {
            riderMotionByName(kGroundCoastLoop, &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCoastRolling));
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        if (hasFlag(rider, kFlagMotionEnded))
        {
            const uint16_t frames = rider->actionFrames[kLevelAction];
            const uint32_t pick = (frames & kStandPickMask) == 0 ? (frames >> kStandPickShift) % kStandMotions : 0u;
            zge::formatString(motionText.address(), sym::textGroundStandFormat, pick);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
        rider->wheelDrag = 1.0f;
        uint16_t &stepFrames = rider->actionFrames[kLevelStep];
        const uint32_t step = actionState(rider, kLevelStep);
        switch (step)
        {
        case kStillWaiting:
        {
            if (*g_aimStickLeast <= ee::abs(inputStick(input, kSecondStick)->x) ||
                *g_aimStickLeast <= ee::abs(inputStick(input, kSecondStick)->y))
            {
                if (++stepFrames >= kAimInFrames)
                {
                    setActionState(rider, kLevelStep, uint32_t(kStillAimIn));
                    return 0;
                }
            }
            else
                stepFrames = 0;
            uint16_t &stillFrames = rider->actionFrames[kLevelPhase];
            if (rider->yawRate == 0)
            {
                if (++stillFrames >= kIdleFrames)
                {
                    setFlag(rider, kFlagIdle);
                    if (stillFrames % kSleepEvery == 0 && *g_currentLevel != kLevel12)
                    {
                        sleepAt->y = rider->position[1] + rider->rootOffset[1] + rider->up[1] * ee::Float(0.75f) +
                                     rider->forward[1] * ee::Float(0.25f) + *g_sleepEffectLift;
                        sleepAt->z = rider->position[2] + rider->rootOffset[2] + rider->up[2] * ee::Float(0.75f) +
                                     rider->forward[2] * ee::Float(0.25f);
                        sleepAt->x = rider->position[0] + rider->rootOffset[0] + rider->up[0] * ee::Float(0.75f) +
                                     rider->forward[0] * ee::Float(0.25f);
                        bmx::sleep(sleepAt, kSleepLifetime);
                    }
                    if (rider->eyelidState < kEyelidsIdle)
                    {
                        rider->eyelidFrames = 0;
                        rider->eyelidState = kEyelidsIdle;
                    }
                }
            }
            else
            {
                clearFlag(rider, kFlagIdle);
                if (rider->eyelidState >= kEyelidsIdle)
                {
                    rider->eyelidFrames = 0;
                    rider->eyelidState = kEyelidsWoken;
                }
                stillFrames = 0;
            }
            const ee::Float falloff = *g_barLeanFalloff;
            rider->barLean[0] = rider->barLean[0] - rider->barLean[0] * falloff;
            clearSmallLean(rider->barLean[0]);
            rider->barLean[1] = rider->barLean[1] - rider->barLean[1] * falloff;
            clearSmallLean(rider->barLean[1]);
            break;
        }
        case kStillAimIn:
            riderCameraMode(rider, kCameraAimMode);
            setActionState(rider, kLevelStep, uint32_t(kStillAiming));
            return 0;
        case kStillAiming:
        {
            if (inputStick(input, kSecondStick)->x == ee::Float(0.0f) && inputStick(input, kSecondStick)->y == ee::Float(0.0f))
            {
                if (++stepFrames >= kAimOutFrames)
                {
                    setActionState(rider, kLevelStep, uint32_t(kStillAimOut));
                    return 0;
                }
            }
            else
                stepFrames = 0;
            aimStick[0] = inputStick(input, kSecondStick)->x;
            aimStick[1] = inputStick(input, kSecondStick)->y;
            const bool keepsCamera = hasFlag(rider, kFlagNoPlayer);
            for (uint32_t lane = 0; lane < 2; ++lane)
                aimStick[lane] = aimLane(aimStick[lane]);
            leanTarget[1] = -aimStick[0] * *g_restLeanX;
            leanTarget[0] = aimStick[1] * *g_restLeanY;
            const ee::Float share = *g_restLeanShare;
            rider->barLean[1] = leanToward(rider->barLean[1], leanTarget[1], share);
            rider->barLean[0] = leanToward(rider->barLean[0], leanTarget[0], share);
            clearSmallLean(rider->barLean[0]);
            clearSmallLean(rider->barLean[1]);
            if (keepsCamera)
                break;
            // The shooter aims along the rider's rows turned by the bars'
            // lean, from a point ahead of the rider's middle.
            *aim = *matrix(rider->right);
            zge::matrixIdentity(turn);
            zge::matrixRotateY(turn, turn, zge::Angle(ee::toInt(rider->barLean[1] * kLeanToAngle)));
            zge::matrixMultiply(aim, aim, turn);
            zge::matrixIdentity(turn);
            zge::matrixRotateX(turn, turn, zge::Angle(ee::toInt(rider->barLean[0] * kLeanToAngle)));
            zge::matrixMultiply(aim, aim, turn);
            muzzle->x = 0.0f;
            muzzle->y = kMuzzleUp;
            muzzle->z = *g_muzzleForward;
            *muzzle = zge::transformed(*matrix(rider->right), *muzzle);
            *muzzle = *muzzle + *vec4(rider->position);
            *muzzle = *muzzle + *vec4(rider->rootOffset);
            const ee::Ptr<Shooter> shooter(rider->shooter);
            shooter->aim = *aim;
            shooter->position = *muzzle;
            shooter->flags &= static_cast<uint8_t>(~(kShooterIgnoresButtons | kShooterNoCrosshair));
            break;
        }
        case kStillAimOut:
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
            {
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, kCameraFirstMode);
                rider->barLean[0] = 0.0f;
                rider->barLean[1] = 0.0f;
                *hidden = 0;
                zge::zgui::setVariable(ee::Ptr<Shooter>(rider->shooter)->ui, kVisible, hidden);
            }
            setActionState(rider, kLevelStep, uint32_t(kStillWaiting));
            return 0;
        case kStillPedalIn:
        case kStillLeaveIn:
            riderMotionByName(kGroundStandEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, kLevelStep, actionState(rider, kLevelStep) + 1u);
            rider->pedalForce = *g_pedalPush;
            break;
        case kStillPedal:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingPedal));
                setFlag(rider, kFlagAutoPedal);
                return 0;
            }
            rider->pedalForce = *g_pedalPush;
            break;
        case kStillLeave:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionFlatland));
                return 0;
            }
            rider->pedalForce = *g_pedalPush;
            break;
        default:
            break;
        }
        clearTaps(rider, 1u);
        return 1;
    }
    READABLE_FUNCTION_WAITS(coastState, Verified);

    // Riding sub-state 1, pedalling. On a slope of 50 degrees or more the
    // pedalling ends. The crouch button takes over (configuration 0: when
    // let go, keeping the sub-state's frame count; the others: while
    // held), then the stick pulled back for the brake. Else by phase: wait
    // for the motion before to end, loop the pedalling motion at a rate by
    // the speed, a turn again while the pedal button is held (or, in
    // configurations 0 and 1 with the automatic pedalling on, while under
    // the pedalling speed), then the end's motion and back to coasting.
    // While it pedals under the pedalling speed the back wheel is pushed.
    // Returns 0 the frame the state changes. DM2's pedalState has the two
    // phases and no end motion.
    int32_t pedalState(Rider *rider)
    {
        if (zge::angleAsin(ee::abs(rider->forward[1])) >= kTurn50Degrees &&
            actionState(rider, uint32_t(kLevelPhase)) < kPedalEnd)
        {
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPedalEnd));
            return 0;
        }
        ButtonRecord *const input = &rider->input;
        uint16_t &frames = rider->actionFrames[kLevelSubState];
        const bool crouch = rider->controllerLayout != 0
                                ? moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0
                                : moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0;
        if (crouch)
        {
            const uint16_t kept = frames;
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCrouch));
            if (rider->controllerLayout == 0)
                frames = kept; // configuration 0's crouch keeps the count
            return 0;
        }
        if (*g_brakeStick < inputStick(input, 0u)->y && zge::angleAcos(-rider->up[1]) < kTurn50Degrees)
        {
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingBrake));
            return 0;
        }

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kPedalIn:
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPedalStarting));
            setFlag(rider, kFlagMotionEnded);
            return 0;
        case kPedalStarting:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundPedalLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPedalLoop));
                rider->motionRate = pedalRate(rider->speed);
            }
            break;
        case kPedalLoop:
            rider->motionRate = pedalRate(rider->speed);
            if (hasFlag(rider, kFlagMotionEnded))
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMovePedal, kMoveHeld) == 0)
                {
                    // Not held: only the automatic pedalling goes on, in
                    // configurations 0 and 1, while well under the speed.
                    if (rider->controllerLayout >= 2 || hasFlag(rider, kFlagOneWheelUpright) || !hasFlag(rider, kFlagAutoPedal) ||
                        !(rider->speed < *g_pedalSpeed * rider->speedStat - *g_pedalRepeat))
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPedalEnding));
                        return 0;
                    }
                }
                riderMotionByName(kGroundPedalLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
            }
            break;
        case kPedalEnd:
            riderMotionByName(kGroundPedalEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPedalEnding));
            break;
        case kPedalEnding:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                // Back to coasting; configuration 0 with the crouch button
                // held keeps the sub-state's frame count.
                uint16_t kept = 0;
                if (rider->controllerLayout == 0 &&
                    moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
                    kept = frames;
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                frames = kept;
                return 0;
            }
            break;
        default:
            break;
        }
        if (rider->speed < *g_pedalSpeed * rider->speedStat)
            rider->pedalForce = *g_pedalPush;
        ++frames;
        return 1;
    }
    READABLE_FUNCTION(pedalState, Verified);

    namespace
    {
        // The two turn moves held unevenly.
        bool turnMovesDiffer(Rider *rider)
        {
            const int32_t left = moveButtons(uint32_t(rider->controllerLayout), &rider->input, kMoveTurnLeft, kMoveHeld);
            const int32_t right = moveButtons(uint32_t(rider->controllerLayout), &rider->input, kMoveTurnRight, kMoveHeld);
            return left != right;
        }
        // The brake's skid, stepped by the phase read again: its start,
        // mirrored when turning left; its loop while the turn moves stay
        // uneven; then out by one of two motions, moving or stopped. The
        // back wheel is held sliding at the brake's full drag meanwhile
        // and let roll when the out motion ends.
        int32_t skidStep(Rider *rider)
        {
            WheelPose &backWheel = rider->wheelPoses[1];
            switch (actionState(rider, uint32_t(kLevelPhase)))
            {
            case kSkidIn:
                riderMotionByName(kGroundSkidStart, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSkid));
                // Turning left (a negative yaw rate) mirrors the bike's motions.
                if (rider->yawRate < 0)
                    setFlag(rider, kFlagMirrored);
                else
                    clearFlag(rider, kFlagMirrored);
                break;
            case kSkid:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    const bool moving = ee::Float(0.0f) < rider->speed;
                    if (moving && turnMovesDiffer(rider))
                    {
                        riderMotionByName(kGroundSkidLoop, &rider->motionName, &rider->motion);
                    }
                    else if (moving)
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSkidOutMoving));
                        riderMotionByName(kGroundSkidOutMoving, &rider->motionName, &rider->motion);
                    }
                    else
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSkidOutStopped));
                        riderMotionByName(kGroundSkidOutStopped, &rider->motionName, &rider->motion);
                    }
                    rider->motionFlags |= kMotionRestart;
                }
                break;
            case kSkidOutMoving:
            case kSkidOutStopped:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    if (backWheel.ground == kWheelSliding)
                        backWheel.ground = kWheelRolling;
                    clearFlag(rider, kFlagMirrored);
                    if (actionState(rider, uint32_t(kLevelPhase)) == kSkidOutMoving)
                    {
                        setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                        return 0;
                    }
                    // Out of a stopped skid: coasting, in its phase 4.
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                    setActionState(rider, uint32_t(kLevelPhase), 4u);
                    return 0;
                }
                break;
            default:
                break;
            }
            rider->wheelDrag = *g_brakeDrag;
            if (backWheel.ground == kWheelRolling)
                backWheel.ground = kWheelSliding;
            backWheel.spin = 0;
            return 1;
        }
    }

    // Riding sub-state 2, the brake (DM2's "manual" state). It holds while
    // the stick is back, the bike is near level and it moves; else it goes
    // out. By phase: the brake's start motion, with the drag from a
    // fifteenth and the automatic pedalling off; its loop with the drag
    // growing to full, and a skid when the two turn moves are held
    // unevenly; its end motion (a sliding back wheel rolls again) and back
    // to coasting; the skid's phases are skidStep's. Returns 0 the
    // frame the state changes.
    int32_t brakeState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        const bool held = !(inputStick(input, 0u)->y <= *g_brakeHoldStick) && zge::angleAcos(-rider->up[1]) < kTurn50Degrees &&
                          !(rider->speed <= ee::Float(0.0f));
        if (!held && actionState(rider, uint32_t(kLevelPhase)) < kBrakeOut)
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kBrakeOut));

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kBrakeIn:
            riderMotionByName(kGroundBrakeStart, &rider->motionName, &rider->motion);
            rider->skidDistance = 0.0f;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kBrakeHeld));
            clearFlag(rider, kFlagAutoPedal);
            rider->wheelDrag = *g_brakeDrag * *g_brakeDragStart;
            return 1;
        case kBrakeHeld:
        {
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundBrakeLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
            }
            if (turnMovesDiffer(rider))
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSkidIn));
                return 0;
            }
            const ee::Float grown = rider->wheelDrag * *g_brakeDragGrowth;
            const ee::Float most = *g_brakeDrag;
            rider->wheelDrag = grown <= most ? grown : most;
            return 1;
        }
        case kBrakeOut:
            riderMotionByName(kGroundBrakeEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            if (rider->wheelPoses[1].ground == kWheelSliding)
                rider->wheelPoses[1].ground = kWheelRolling;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kBrakeOutEnding));
            return 1;
        case kBrakeOutEnding:
            if (!hasFlag(rider, kFlagMotionEnded))
                return 1;
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
            return 0;
        case kSkidIn:
        case kSkid:
        case kSkidOutMoving:
        case kSkidOutStopped:
            return skidStep(rider);
        default:
            return 1;
        }
    }
    READABLE_FUNCTION(brakeState, Verified);

    // Riding sub-state 3, the crouch. The crouch button let go hops: an
    // upward push by the frames crouched and the air stat, less when
    // rolling up a slope; off a contact of kind 1 or 3 with the stick
    // pushed straight up the hop is marked so; the rider is in the air
    // with the crouch's end motion. Else the crouch's start motion (the
    // pedalling's half push goes on under the pedalling speed while it
    // plays), then its loop. DM2's, with the least added before the
    // slope's loss.
    int32_t crouchState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ButtonRecord *const input = &rider->input;
        uint16_t &frames = rider->actionFrames[kLevelSubState];
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
        {
            const uint32_t full = *g_hopFullFrames;
            const ee::Float least = *g_hopLeast;
            const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
            const ee::Float shareOf = zge::unsignedToFloat(frames < full ? uint32_t(frames) : full);
            ee::Float hop = perFrame * shareOf / zge::unsignedToFloat(full) + least;
            if (rider->velocity[1] <= ee::Float(0.0f))
            {
                // Rolling up or level: less for the climb along the way the
                // rider moves.
                *impulse = *vec4(rider->forward);
                if (zge::dot(*vec4(rider->velocity), *impulse) < ee::Float(0.0f))
                    zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                hop = hop - hop * (*g_hopSlopeLoss * -impulse->y);
            }
            *impulse = zge::kZero;
            impulse->y = -hop;

            if (rider->contactKind == 1 || rider->contactKind == 3)
            {
                if (ee::Float(0.5f) < ee::abs(inputStick(input, 0u)->y) && ee::abs(inputStick(input, 0u)->x) < ee::Float(0.25f) &&
                    inputStick(input, 0u)->y < ee::Float(-0.5f))
                    setFlag(rider, kFlagHopUp);
            }
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            if (rider->edgeHeight < ee::Float(0.5f))
                setFlag(rider, kFlagWeakHop);
            else
                clearFlag(rider, kFlagWeakHop);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kGroundCrouchEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        }

        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kCrouchIn)
        {
            riderMotionByName(kGroundCrouchStart, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCrouchGoingDown));
        }
        else if (phase == kCrouchGoingDown || phase == kCrouchHeld)
        {
            if (phase == kCrouchGoingDown && rider->speed < *g_pedalSpeed * rider->speedStat &&
                zge::angleAsin(ee::abs(rider->forward[1])) < kTurn50Degrees)
                rider->pedalForce = *g_pedalPush * ee::Float(0.5f);
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundCrouchLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCrouchHeld));
            }
        }
        ++frames;
        return 1;
    }
    READABLE_FUNCTION(crouchState, Verified);

    // Riding sub-state 4, riding backwards (the motions' "reverse"). Once
    // the rider no longer faces backwards, the rolling phases end by a
    // motion out, coasting or standing. By phase: the start motion; the
    // loop, where pedalling (or the first move pressed) nearly still on
    // near-level ground pedals out of it, and facing backwards the turn
    // round move starts the turn and the crouch move the crouch; the turn
    // round, after whose motion the rows, the drawn body, the wheels and
    // the air's angles are all turned about and the rider coasts; the
    // crouch, whose hop is the riding crouch's by the phase's frames; the
    // ways out. The wheels' drag is the brake's with the stick back, a
    // share of it else. Returns 0 the frame the state changes. DM2's
    // fakieState, with the phases 7 to 9 and the pedalling out added and
    // no motion flag set by the turn.
    int32_t fakieState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ButtonRecord *const input = &rider->input;
        uint16_t &frames = rider->actionFrames[kLevelPhase];
        if (!hasFlag(rider, kFlagFakie))
        {
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase < kFakieTurning && phase != kFakieIn && hasFlag(rider, kFlagMotionEnded))
            {
                if (ee::Float(0.0f) < rider->speed)
                    motionThen(rider, kGroundReverseEndCoast, kFakieOutRolling);
                else
                    motionThen(rider, kGroundReverseEndStand, kFakieOutStill);
                clearFlag(rider, kFlagMotionEnded);
            }
        }
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kFakieIn:
            riderMotionByName(kGroundReverseStart, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), hasFlag(rider, kFlagHitHard) ? uint32_t(kFakieStartingToTurn) : uint32_t(kFakieStarting));
            break;
        case kFakieStarting:
        case kFakieStartingToTurn:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundReverseLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
                setActionState(rider, uint32_t(kLevelPhase), phase == kFakieStartingToTurn ? uint32_t(kFakieTurnIn) : uint32_t(kFakieRolling));
            }
            break;
        case kFakieRolling:
        {
            if ((moveButtons(uint32_t(rider->controllerLayout), input, kMovePedal, kMoveHeld) != 0 ||
                 moveButtons(uint32_t(rider->controllerLayout), input, kMoveFirst, kMovePressed) != 0) &&
                rider->speed < kFakiePedalOutSpeed && zge::angleAsin(ee::abs(rider->forward[1])) < kTurn50Degrees)
            {
                riderMotionByName(kGroundReverseEndCoast, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kFakiePedalOut));
                return 0;
            }
            if (hasFlag(rider, kFlagFakie))
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRound, kMovePressed) != 0)
                {
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kFakieTurnIn));
                    return 0;
                }
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
                {
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kFakieCrouched));
                    return 0;
                }
            }
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundReverseLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
            }
            if (*g_fakieRollBrakeStick < inputStick(input, 0u)->y && zge::angleAcos(-rider->up[1]) < kTurn50Degrees)
                rider->wheelDrag = *g_brakeDrag;
            else
                rider->wheelDrag = *g_brakeDrag * *g_fakieRollDragShare;
            break;
        }
        case kFakieTurnIn:
            motionThen(rider, kGroundReverseEnd, kFakieTurning);
            break;
        case kFakieTurning:
        {
            if (!hasFlag(rider, kFlagMotionEnded))
            {
                rider->yawRate = 0;
                break;
            }
            // Turned round: the rows, the drawn body's forward and right,
            // each wheel's third and first rows, the pitch and roll.
            clearFlag(rider, kFlagFakie);
            auto turnRound = [](zge::Vec4 &row) { zge::vec3Negate(&row.xyz(), &row.xyz()); };
            turnRound(*vec4(rider->forward));
            turnRound(*vec4(rider->right));
            zge::Matrix &body = *matrix(rider->body);
            turnRound(body.row(2));
            turnRound(body.row(0));
            for (WheelPose &wheel : rider->wheelPoses)
            {
                zge::Matrix &rows = *matrix(wheel.matrix);
                turnRound(rows.row(2));
                turnRound(rows.row(0));
            }
            const int16_t roll = static_cast<int16_t>(-rider->roll);
            rider->pitch = static_cast<int16_t>(-rider->pitch);
            rider->roll = roll;
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
            return 0;
        }
        case kFakieCrouched:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            {
                // The hop: as the crouch's, by the frames of the phase.
                *impulse = zge::kZero;
                const uint32_t full = *g_hopFullFrames;
                const ee::Float least = *g_hopLeast;
                const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
                const ee::Float hop = perFrame * zge::unsignedToFloat(frames < full ? uint32_t(frames) : full) / zge::unsignedToFloat(full);
                impulse->y = -(hop + least);
                addImpulse(rider, &impulse->x);
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                riderMotionByName(kGroundCrouchEnd, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                clearFlag(rider, kFlagMotionEnded);
                return 0;
            }
            if (hasFlag(rider, kFlagMotionEnded))
                rider->motionFlags |= kMotionRestart;
            break;
        case kFakieOutRolling:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                return 0;
            }
            break;
        case kFakieOutStill:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                // Coasting, in its phase 1.
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                setActionState(rider, uint32_t(kLevelPhase), 1u);
                return 0;
            }
            rider->wheelDrag = ee::Float(1.0f);
            break;
        case kFakiePedalOut:
            if (hasFlag(rider, kFlagMotionEnded) && !hasFlag(rider, kFlagFakie))
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingPedal));
                clearTaps(rider, 1u);
                return 0;
            }
            rider->pedalForce = *g_pedalPush;
            break;
        default:
            break;
        }
        // The wheels' drag by the stick (not in the still way out).
        if (*g_fakieBrakeStick < inputStick(input, 0u)->y && zge::angleAcos(-rider->up[1]) < kTurn50Degrees &&
            actionState(rider, uint32_t(kLevelPhase)) < kFakieTurning)
        {
            rider->wheelDrag = *g_brakeDrag;
            ++frames;
            return 1;
        }
        if (actionState(rider, uint32_t(kLevelPhase)) != kFakieOutStill)
            rider->wheelDrag = *g_brakeDrag * *g_fakieDragShare;
        ++frames;
        return 1;
    }
    READABLE_FUNCTION(fakieState, Verified);

    // Action state 0, riding. By the physics: the air (a turn round caught
    // half way is finished first: the rows, the drawn body, the air's
    // angles), a bail, a grind, a stall (with its trick), else riding:
    // facing backwards is sub-state 4; the stick steers (not coasting at a
    // stop); the grind button is noted, pressed then held; two moves of
    // BMX XXX's own lead to action states 10 and 9; while moving in the
    // coast, pedal or brake sub-state two quick opposite taps of the stick
    // go to a one-wheel state (the manual's action state); then the
    // sub-state runs. DM2's ridingState with these added; its grind and
    // stall entries here set no phase.
    int32_t ridingState(Rider *rider)
    {
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            if (actionState(rider, uint32_t(kLevelSubState)) == kRidingFakie &&
                actionState(rider, uint32_t(kLevelPhase)) == kFakieTurning)
            {
                zge::vec3Negate(&vec4(rider->forward)->xyz(), &vec4(rider->forward)->xyz());
                zge::vec3Negate(&vec4(rider->right)->xyz(), &vec4(rider->right)->xyz());
                zge::vec3Negate(&matrix(rider->body)->row(2).xyz(), &matrix(rider->body)->row(2).xyz());
                zge::vec3Negate(&matrix(rider->body)->row(0).xyz(), &matrix(rider->body)->row(0).xyz());
                const int16_t roll = static_cast<int16_t>(-rider->roll);
                rider->pitch = static_cast<int16_t>(-rider->pitch);
                rider->roll = roll;
            }
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        case kPhysicsBail:
        case kPhysicsTumble:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        case kPhysicsGrindApproach:
        case kPhysicsGrind:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
            return 0;
        case kPhysicsStallApproach:
        case kPhysicsStall:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionStall));
            rider->trick = stallTrick(rider);
            setFlag(rider, kFlagTrickHeld);
            return 0;
        default:
            break;
        }

        if (hasFlag(rider, kFlagFakie))
        {
            if (actionState(rider, uint32_t(kLevelSubState)) != kRidingFakie)
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingFakie));
        }
        else
            clearFlag(rider, kFlagHitHard);

        ButtonRecord *const input = &rider->input;
        const ee::Float steer = inputStick(input, 0u)->x;
        const bool stopped = steer != ee::Float(0.0f) && actionState(rider, uint32_t(kLevelSubState)) == kRidingCoast &&
                             actionState(rider, uint32_t(kLevelPhase)) == kCoastStopped && actionState(rider, kLevelStep) != 0;
        if (steer == ee::Float(0.0f) || stopped)
            yawRateFalloff(rider);
        else
        {
            const ee::Float speed = rider->speed;
            if (ee::Float(0.0f) < speed)
                steerAtSpeed(rider, steer, speed, *g_rideSteerLeast, *g_rideDegreesToTurn);
            else
            {
                const int32_t turned = int32_t(rider->yawRate) + ee::toInt(-steer * ee::Float(3.0f) * *g_rideStillDegreesToTurn);
                steerYawRate(rider, turned, kLandStillMost);
            }
        }

        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveAskObjective, kMoveHeld) != 0)
            setFlag(rider, kFlagObjectiveAsked);
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMovePressed) != 0)
            setFlag(rider, kFlagGrindPressed);
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMoveHeld) != 0 && hasFlag(rider, kFlagGrindPressed))
            setFlag(rider, kFlagGrindAsked);
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMove19, kMovePressed) != 0)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGhostRide));
            return 0;
        }
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveSkitch, kMoveHeld) != 0 &&
            setPhysicsState(rider, uint32_t(kPhysicsSkitchApproach)) == kPhysicsSkitchApproach)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionSkitch));
            return 0;
        }

        if (ee::Float(0.0f) < rider->speed &&
            (actionState(rider, uint32_t(kLevelSubState)) == kRidingCoast || actionState(rider, uint32_t(kLevelSubState)) == kRidingPedal ||
             actionState(rider, uint32_t(kLevelSubState)) == kRidingBrake) &&
            rider->tapCount >= 2)
        {
            const uint32_t last = rider->tapCount - 1u;
            const uint32_t before = rider->tapCount - 2u;
            uint32_t wheelState = 0;
            if (rider->taps[last] == kDirDown && rider->taps[before] == kDirUp)
            {
                if (tapFramesApart(rider, last, before) < kTapsApartMost)
                    wheelState = kPhysicsManual;
            }
            else if (rider->taps[last] == kDirUp && rider->taps[before] == kDirDown)
            {
                if (tapFramesApart(rider, last, before) < kTapsApartMost)
                    wheelState = kPhysicsState9;
            }
            if (wheelState != 0)
            {
                setFlag(rider, kFlagOnOneWheel);
                setPhysicsState(rider, wheelState);
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionWheelie));
                setFlag(rider, kFlagActed);
                return 0;
            }
        }

        int32_t kept;
        do
            kept = (*g_ridingSubStates)[rider->actionStates[kLevelSubState]](rider);
        while (kept == 0 && actionState(rider, uint32_t(kLevelAction)) == kActionRiding);
        rider->balanceFrames = 0;
        return kept;
    }
    READABLE_FUNCTION(ridingState, Verified);
}
