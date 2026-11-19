// BMX XXX's air (action.c): action state 1 and its sub-state with no trick. DM2's (game/dm2/rider/actions/
// states.cpp), with BMX XXX's own: the ground's way into the manual and the nose wheelie, the side hop, the coast,
// the grind caught off a lip.

#include "../rider.h"
#include "../vector.h"

#include "bmx/game/score.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/air.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // "air.coast.loop": the air's loop.
        constexpr uint32_t kAirCoastLoop = sym::textAirCoastLoop;
    }

    // Air sub-state 0, no trick: the motion the air started with (set by
    // whoever left the ground), then the air's loop once it ended. (DM2
    // restarts the motion on entering too; BMX XXX does not.)
    int32_t airNoTrick(Rider *rider)
    {
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kPhaseIn)
        {
            rider->trickLoops = 0;
            rider->trickMotion = kNoTrick;
            clearFlag(rider, kFlagTrickTurn);
            rider->trick = kNoTrick;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseHeld));
        }
        else if (phase == kPhaseHeld && hasFlag(rider, kFlagMotionEnded))
        {
            riderMotionByName(kAirCoastLoop, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
        return 1;
    }
    READABLE_FUNCTION(airNoTrick, Verified);


    namespace
    {
        using SubState = ee::FunctionPtr<int32_t(Rider *)>;
        inline ee::Global<SubState[5], sym::g_airSubStates> g_airSubStates; // by AirSubState (the coast's airNoTrick)

        constexpr uint32_t kMoveTrickA = 3;
        constexpr uint32_t kMoveTrickB = 4;
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMoveGrind = 7;
        constexpr uint32_t kMoveStall = 8;
        constexpr uint32_t kMoveTurnLeft = 11;
        constexpr uint32_t kMoveTurnRight = 12;
        constexpr uint32_t kMoveCoast = 17;
        constexpr uint32_t kMoveBail = 18;
        constexpr uint32_t kMovePressed = 0;
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kMoveReleased = 2;
        constexpr uint32_t kLevelStep = 3; // its frames: a trick button's wait, the side hop's stick
        constexpr uint8_t kBailKindAsked = 11;  // the bail move (DM2's)
        constexpr uint16_t kTrickWaitFrames = 8; // a trick button's wait for taps (DM2 16)
        constexpr uint32_t kTapsApartMost = 15;  // two taps for the manual or the nose wheelie, within this many frames
        constexpr uint8_t kLipTrickPlant = 0x47; // lipTrick's trick that is the lip trick state's (else a stall)
        constexpr int32_t kTurnStopped = 91;     // half a degree
        // "ground.crouch.end"'s name hash: the hop's motion (the manual entered from it starts in phase 1).
        constexpr uint32_t kGroundCrouchEndHash = 0x0B45DBC4u;
        constexpr uint32_t kAirCoastLoopText = sym::textAirCoastLoop;
        // The landing's speed-ups into the manual and the nose wheelie (1.1, 1.33).
        inline ee::Global<ee::Float, sym::g_manualLandingSpeedUp> g_manualLandingSpeedUp;
        inline ee::Global<ee::Float, sym::g_noseWheelieLandingSpeedUp> g_noseWheelieLandingSpeedUp;
        // The hop in the air: the stick kept this many frames (2), the hop (0.15, three quarters of it up; to the
        // side 0.85 of it, 30 degrees off level), the turn it makes a frame (1.5 degrees) over the snap's frames.
        inline ee::Global<uint32_t, sym::g_airStickHoldFrames> g_airStickHoldFrames;
        inline ee::Global<ee::Float, sym::g_airHopImpulse> g_airHopImpulse;
        inline ee::Global<ee::Float, sym::g_airSideHopShare> g_airSideHopShare;
        constexpr ee::Float kAirHopUpShare(0.75f);
        constexpr zge::Angle kSideHopAngle = 0x1555;
        constexpr int16_t kSideHopTurn = 0x111;
        constexpr int16_t kSideHopSnapFrames = 10;
        constexpr uint16_t kSideHopFrames = 15;   // the air's first frames the side hop can be made in
        constexpr ee::Float kStickPushed(0.5f);
        // A grind caught off a lip in the air's first frames, at three quarters the speed.
        constexpr uint16_t kLipGrindFrames = 10;
        constexpr ee::Float kLipGrindSlow(0.75f);
        // The turn and the body's pitch (DM2's).
        inline ee::Global<ee::Float, sym::g_airTurnMost> g_airTurnMost;
        inline ee::Global<ee::Float, sym::g_airTurnMostShare> g_airTurnMostShare;
        inline ee::Global<ee::Float, sym::g_airTurnSlowA> g_airTurnSlowA; // the turn buttons against the turn
        inline ee::Global<ee::Float, sym::g_airTurnSlowB> g_airTurnSlowB;
        inline ee::Global<ee::Float, sym::g_airButtonTurn> g_airButtonTurn;
        inline ee::Global<ee::Float, sym::g_airTurnStart> g_airTurnStart;
        inline ee::Global<ee::Float, sym::g_airStickTurn> g_airStickTurn;
        inline ee::Global<ee::Float, sym::g_airStickSlowA> g_airStickSlowA; // the stick against the turn
        inline ee::Global<ee::Float, sym::g_airStickSlowB> g_airStickSlowB;
        inline ee::Global<ee::Float, sym::g_bodyPitchFalloff> g_bodyPitchFalloff;
        constexpr int16_t kBodyPitchStep = 0x16C;  // two down taps, the stick down: 2 degrees a frame back
        constexpr int16_t kBodyPitchLeast = -4551; // to -25 degrees
        constexpr int16_t kBodyPitchUpStep = 0xE3;
        constexpr int16_t kBodyPitchMost = 3640;   // 20 degrees

        // A turn rate scaled by a share (k / 65536 x share x 65536, as int16).
        int16_t scaledTurn(int16_t rate, ee::Float share)
        {
            return static_cast<int16_t>(ee::toInt(ee::toFloat(rate) * ee::Float(1.0f / 65536.0f) * share * ee::Float(65536.0f)));
        }
        // The last two taps both `direction`.
        bool twoTaps(const Rider *rider, uint8_t direction)
        {
            const uint32_t n = rider->tapCount;
            return rider->taps[n - 1] == direction && rider->taps[n - 2] == direction;
        }
        // The last tap `direction`, the one before it up or down, within kTapsApartMost frames.
        bool tapAfterUpOrDown(const Rider *rider, uint8_t direction)
        {
            const uint32_t n = rider->tapCount;
            const uint8_t before = rider->taps[n - 2];
            return rider->taps[n - 1] == direction && (before == kDirUp || before == kDirDown) &&
                   tapFramesApart(rider, n - 1, n - 2) < kTapsApartMost;
        }
        // The air's sub-state left for none (its callback run once as none).
        bool leaveAirSubState(Rider *rider)
        {
            if (actionState(rider, uint32_t(kLevelSubState)) == 0)
                return false;
            setActionState(rider, uint32_t(kLevelSubState), 0u);
            (*g_airSubStates)[rider->actionStates[kLevelSubState]](rider);
            return true;
        }
        // An action taken in the air this frame (kFlagActed, as DM2's at the same places).
        int32_t acted(Rider *rider)
        {
            setFlag(rider, kFlagActed);
            return 0;
        }
        // The landing's way onto one wheel: the physics state, the speed scaled up, the wheel's too.
        void ontoOneWheel(Rider *rider, uint32_t physics, ee::Float speedUp, WheelPose &wheel)
        {
            setFlag(rider, kFlagOnOneWheel);
            setPhysicsState(rider, physics);
            *vec4(rider->velocity) = *vec4(rider->velocity) * speedUp;
            rider->speed = rider->speed * speedUp;
            *vec4(wheel.velocity) = *vec4(wheel.velocity) * speedUp;
            wheel.speed = wheel.speed * speedUp;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionWheelie));
        }
    }

    // Action state 1, the air. Landed: the landing, or the manual or the nose wheelie by a down or an up tap after
    // a vertical one within 15 frames (the speed scaled up); a bail, a grind or a stall the physics found: their
    // states. Else the side hop (the crouch let go in the air's first frames after a grind's hop, kFlagOffRailIntoAir), the
    // coast (move 17 off a lip), the bail move; the turn buttons or the stick turn (within the spin stat's most);
    // the body's pitch by two taps and the stick; the trick buttons with taps (air sub-states 1 and 2), the grind
    // button (a grind or a stall, or off a lip in the first frames the grind itself), the stall button (a stall or
    // the lip trick); else the air's sub-state. DM2's airState with BMX XXX's own steps.
    int32_t airState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        switch (physicsState(rider))
        {
        case kPhysicsGround:
            if (!hasFlag(rider, kFlagNoPlayer))
                trickLineEvent(uint32_t(rider->player), 1u);
            if (rider->tapCount >= 2)
            {
                if (tapAfterUpOrDown(rider, kDirDown))
                {
                    ontoOneWheel(rider, uint32_t(kPhysicsManual), *g_manualLandingSpeedUp, rider->wheelPoses[1]);
                    if (rider->motionName == kGroundCrouchEndHash)
                        setActionState(rider, uint32_t(kLevelPhase), 1u);
                    return acted(rider);
                }
                if (tapAfterUpOrDown(rider, kDirUp))
                {
                    ontoOneWheel(rider, uint32_t(kPhysicsNoseWheelie), *g_noseWheelieLandingSpeedUp, rider->wheelPoses[0]);
                    return acted(rider);
                }
            }
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        case kPhysicsBail:
        case kPhysicsTumble:
            if (!hasFlag(rider, kFlagNoPlayer))
                trickLineEvent(uint32_t(rider->player), 0u);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        case kPhysicsGrind:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
            return 0;
        case kPhysicsStall:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionStall));
            rider->trick = stallTrick(rider);
            setFlag(rider, kFlagTrickHeld);
            return 0;
        default:
            break;
        }

        // The side hop: the stick's push kept a moment; the crouch let go hops up, with the stick to a side
        // sideways too (across the velocity, 30 degrees off level), turning the rider over the snap's frames.
        if (hasFlag(rider, kFlagOffRailIntoAir) && rider->actionFrames[kLevelAction] < kSideHopFrames)
        {
            const uint16_t held = ++rider->actionFrames[kLevelStep];
            if (*g_airStickHoldFrames < held)
            {
                rider->airStick[0] = 0.0f;
                rider->airStick[1] = 0.0f;
                rider->actionFrames[kLevelStep] = 0;
            }
            const ee::Float x = inputStick(input, 0u)->x;
            if (kStickPushed < ee::abs(x))
            {
                rider->airStick[0] = x;
                rider->actionFrames[kLevelStep] = 0;
            }
            const ee::Float y = inputStick(input, 0u)->y;
            if (kStickPushed < ee::abs(y))
            {
                rider->airStick[1] = y;
                rider->actionFrames[kLevelStep] = 0;
            }
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            {
                ee::Local<zge::Vec4> impulse;
                ee::Local<zge::Vec4> down;
                ee::Float hop = *g_airHopImpulse * kAirHopUpShare;
                rider->velocity[1] = 0.0f;
                *impulse = zge::kZero;
                if (kStickPushed < ee::abs(rider->airStick[0]))
                {
                    down->x = 0.0f;
                    down->y = -1.0f;
                    down->z = 0.0f;
                    *impulse = zge::cross(*down, *vec4(rider->velocity));
                    impulse->y = 0.0f;
                    *impulse = zge::unit(*impulse);
                    if (ee::Float(0.0f) <= rider->airStick[0])
                        zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                    hop = *g_airHopImpulse * *g_airSideHopShare;
                    *impulse = *impulse * (hop * zge::angleSin(kSideHopAngle));
                    impulse->y = impulse->y - hop * zge::angleCos(kSideHopAngle);
                }
                else
                    impulse->y = impulse->y - hop;
                addImpulse(rider, &impulse->x);
                if (kStickPushed < ee::abs(rider->airStick[0]))
                {
                    *matrix(rider->alignFrom) = *matrix(rider->right);
                    rider->alignAngles[1] = ee::Float(0.0f) <= rider->airStick[0] ? int16_t(-kSideHopTurn) : kSideHopTurn;
                    rider->snapFrames = kSideHopSnapFrames;
                }
                clearFlag(rider, kFlagOffRailIntoAir);
            }
        }
        // The coast: off a lip, not hopping, no contact of kinds 1 to 3.
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCoast, kMovePressed) != 0 && hasFlag(rider, kFlagLipLanding) &&
            !hasFlag(rider, kFlagHopUp) && !hasFlag(rider, kFlagHopDrop) && uint32_t(rider->contactKind - 1) >= 3u)
        {
            riderMotionByName(kAirCoastLoopText, &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirCoast));
            rider->yawTurned = 0;
            setFlag(rider, kFlagCoastStarted);
            rider->trickTurned = 0;
        }
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveBail, kMovePressed) != 0)
        {
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailKindAsked;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }

        // The turn: the buttons (one of the two), else the stick.
        const int16_t most = static_cast<int16_t>(ee::toInt(*g_airTurnMost * rider->spinStat * *g_airTurnMostShare));
        const int32_t left = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMoveHeld);
        const int32_t right = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRight, kMoveHeld);
        // Slowing a turn against it: by one share, held to -most, then by another, held to most (the buttons'
        // and the stick's shares).
        auto slowTurn = [&](ee::Float slowA, ee::Float slowB) {
            const int16_t rate = rider->yawRate;
            const int16_t a = scaledTurn(rate, slowA);
            if (a < most ? a < -most : most < -most)
            {
                rider->yawRate = static_cast<int16_t>(-most);
                return;
            }
            const int16_t b = scaledTurn(rate, slowB);
            rider->yawRate = b >= most ? most : b;
        };
        if (left != right)
        {
            const int16_t rate = rider->yawRate;
            bool start = true;
            if (rate != 0)
            {
                const int32_t leftAgain = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMoveHeld);
                if (rider->yawRate > 0)
                    start = (leftAgain ^ 1) != 0;
                else
                    start = leftAgain != 0;
            }
            if (start)
            {
                const int32_t leftNow = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMoveHeld);
                const ee::Float turn = leftNow != 0 ? *g_airButtonTurn : -*g_airButtonTurn;
                rider->yawRate = static_cast<int16_t>(ee::toInt(turn * *g_airTurnStart));
            }
            else
                slowTurn(*g_airTurnSlowA, *g_airTurnSlowB);
        }
        else
        {
            const ee::Float x = inputStick(input, 0u)->x;
            if (x == ee::Float(0.0f))
            {
                // Let go: the turn falls off by half a frame, to 0 under half a degree.
                const int16_t rate = rider->yawRate;
                const int32_t falling = static_cast<uint16_t>(rider->yawRate) -
                                        ee::toInt(ee::toFloat(rate) * ee::Float(1.0f / 65536.0f) * ee::Float(0.5f) * ee::Float(65536.0f));
                rider->yawRate = static_cast<int16_t>(falling);
                if (ee::abs(ee::toFloat(int32_t(static_cast<int16_t>(falling)))) < ee::Float(float(kTurnStopped)))
                    rider->yawRate = 0;
            }
            else
            {
                const int16_t rate = rider->yawRate;
                const bool against = rate != 0 && ((rate > 0) == (x < ee::Float(0.0f)));
                if (against)
                    slowTurn(*g_airStickSlowA, *g_airStickSlowB);
                else
                {
                    ee::Float turn = *g_airStickTurn;
                    if (ee::Float(0.0f) <= x)
                        turn = -turn;
                    rider->yawRate = static_cast<int16_t>(ee::toInt(turn * *g_airTurnStart));
                }
            }
        }
        // The turn into the total turned; an alignment still turning takes the turn in (or ends when the turn is
        // more).
        {
            const int16_t rate = rider->yawRate;
            const int32_t total = rider->yawTurned + rate;
            rider->yawTurned = total;
            if (hasFlag(rider, kFlagSnapAlign) && static_cast<uint16_t>(rider->snapFrames) != 0)
            {
                const int16_t angle = rider->alignAngles[1];
                if (ee::abs(ee::toFloat(angle)) < ee::abs(ee::toFloat(rate)))
                {
                    clearFlag(rider, kFlagSnapAlign);
                    rider->snapFrames = 0;
                }
                else
                    rider->yawTurned = total + angle;
            }
        }
        // The roll halves a frame, to 0 under half a degree.
        rider->roll = static_cast<int16_t>(rider->roll >> 1);
        if (ee::abs(ee::toFloat(rider->roll)) < ee::Float(float(kTurnStopped)))
            rider->roll = 0;
        // The body's pitch: two down taps and the stick down lean it back, two up taps and the stick up forward;
        // else it falls off.
        {
            const ee::Float y = inputStick(input, 0u)->y;
            bool leaned = false;
            if (rider->tapCount >= 2)
            {
                if (ee::Float(0.5f) < y && twoTaps(rider, kDirDown))
                {
                    const int32_t back = rider->bodyPitch - kBodyPitchStep;
                    rider->bodyPitch = static_cast<int16_t>(kBodyPitchLeast - 1 < back ? back : kBodyPitchLeast);
                    leaned = true;
                }
                else if (y < ee::Float(-0.5f) && twoTaps(rider, kDirUp))
                {
                    const int32_t forward = rider->bodyPitch + kBodyPitchUpStep;
                    rider->bodyPitch = static_cast<int16_t>(forward < kBodyPitchMost + 1 ? forward : kBodyPitchMost);
                    leaned = true;
                }
            }
            if (!leaned)
            {
                rider->bodyPitch = static_cast<int16_t>(static_cast<uint16_t>(rider->bodyPitch) -
                                                        ee::toInt(ee::toFloat(rider->bodyPitch) * ee::Float(1.0f / 65536.0f) *
                                                                  *g_bodyPitchFalloff * ee::Float(65536.0f)));
            }
        }

        // The buttons, unless a trick is under way (sub-states 1 and 2 before their ends, the coast).
        bool buttons = true;
        if (actionState(rider, uint32_t(kLevelSubState)) != 0 && actionState(rider, uint32_t(kLevelSubState)) != kAirSubState3)
        {
            bool ending = false;
            if (actionState(rider, uint32_t(kLevelSubState)) == kAirTrick)
                ending = actionState(rider, uint32_t(kLevelPhase)) >= 3;
            if (!ending)
            {
                buttons = actionState(rider, uint32_t(kLevelSubState)) == kAirTrickB &&
                          actionState(rider, uint32_t(kLevelPhase)) >= 3;
            }
        }
        if (buttons)
        {
            // A trick button: with taps, its trick (now, or within kTrickWaitFrames of the press).
            auto trickButton = [&](uint32_t move, RiderFlag waiting, RiderFlag other, uint32_t subState) -> int32_t {
                if (moveButtons(uint32_t(rider->controllerLayout), input, move, kMovePressed) == 0)
                {
                    if (!hasFlag(rider, waiting))
                        return 1;
                    if (rider->tapCount == 0)
                    {
                        if (++rider->actionFrames[kLevelStep] >= kTrickWaitFrames)
                            clearFlag(rider, waiting);
                        return 1;
                    }
                }
                else if (rider->tapCount == 0)
                {
                    rider->actionFrames[kLevelStep] = 0;
                    setFlag(rider, waiting);
                    clearFlag(rider, other);
                    return 1;
                }
                if (subState == kAirTrick)
                {
                    if (leaveAirSubState(rider))
                        rider->roll = 0;
                    setActionState(rider, uint32_t(kLevelSubState), subState);
                    return acted(rider);
                }
                leaveAirSubState(rider);
                setActionState(rider, uint32_t(kLevelSubState), subState);
                rider->roll = 0;
                return acted(rider);
            };
            if (trickButton(kMoveTrickA, kFlagTrickAWaiting, kFlagTrickBWaiting, kAirTrick) == 0)
                return 0;
            if (trickButton(kMoveTrickB, kFlagTrickBWaiting, kFlagTrickAWaiting, kAirTrickB) == 0)
                return 0;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMovePressed) != 0)
                setFlag(rider, kFlagGrindPressed);
            if (actionState(rider, uint32_t(kLevelSubState)) != kAirSubState3)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMoveHeld) != 0 && hasFlag(rider, kFlagGrindPressed) &&
                    actionState(rider, uint32_t(kLevelPhase)) != 0)
                {
                    // Off a lip in the air's first frames: the grind itself, three quarters as fast.
                    if (rider->controllerLayout == 0 && hasFlag(rider, kFlagLipLanding) &&
                        rider->actionFrames[kLevelAction] < kLipGrindFrames && rider->contactKind < 4 && rider->contactKind != 0)
                    {
                        const uint8_t caught = setPhysicsState(rider, uint32_t(kPhysicsGrind));
                        if (caught == kPhysicsGrind || caught == kPhysicsStall)
                        {
                            *vec4(rider->velocity) = *vec4(rider->velocity) * kLipGrindSlow;
                            rider->speed = rider->speed * kLipGrindSlow;
                            return 0;
                        }
                    }
                    const uint8_t asked = setPhysicsState(rider, uint32_t(kPhysicsGrindApproach));
                    if (asked == kPhysicsGrindApproach || asked == kPhysicsGrind)
                    {
                        leaveAirSubState(rider);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
                        return acted(rider);
                    }
                    if (asked == kPhysicsStallApproach || asked == kPhysicsStall)
                    {
                        leaveAirSubState(rider);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionStall));
                        rider->trick = stallTrick(rider);
                        return acted(rider);
                    }
                }
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveStall, kMovePressed) != 0 && lipTrick(rider) != kNoTrick)
                {
                    const uint8_t asked = setPhysicsState(rider, uint32_t(kPhysicsStallApproach));
                    if (asked == kPhysicsStallApproach || asked == kPhysicsLipApproach)
                    {
                        const uint8_t trick = lipTrick(rider);
                        leaveAirSubState(rider);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(trick != kLipTrickPlant ? kActionStall : kActionLipTrick));
                        rider->trick = trick;
                        return acted(rider);
                    }
                    clearTaps(rider, 1u);
                }
            }
        }
        for (;;)
        {
            const int32_t kept = (*g_airSubStates)[rider->actionStates[kLevelSubState]](rider);
            if (kept != 0 || actionState(rider, uint32_t(kLevelAction)) != kActionAir)
                return kept;
        }
    }
    READABLE_FUNCTION(airState, Verified);

    namespace
    {
        // The tricks' names in their motions' names, by trick (the air's "backflip" .. "raiseTheRoof", then the
        // grinds', the stalls' and the flatland's: "air.%s.start", ".loop", ".end" for the air's), and the rates of
        // the three motions (times the technique stat).
        inline ee::Global<ee::Ptr<const char>[100], sym::g_trickMotionNames> g_trickMotionNames;
        inline ee::Global<ee::Float[100][3], sym::g_trickMotionRates> g_trickMotionRates;
        // A trick's kinds (a word a trick): the flip it turns (& kTrickFlips), kTrickCountsHeld its loops counted
        // as held; and the flags of a trick on top (a half-word a trick: kTrickCountsHeld the same).
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickKinds;
        inline ee::Global<uint16_t[256], sym::g_trickFlags> g_trickFlags;
        constexpr uint16_t kTrickCountsHeld = 0x20;
        constexpr uint32_t kAirCoastStart = sym::textAirCoastStart; // "air.coast.start"
        // The tricks on top of an air trick (a grind's, a stall's: a row of 0x56 a trick, by trickFromTaps' index;
        // kNoTrick none), their loops counted every 15 frames and 225 more as the technique stat is less.
        constexpr uint32_t kTopTrickRow = 0x56;
        inline ee::Global<uint8_t[256][kTopTrickRow], sym::grindTopTricks> g_topTricks;
        constexpr uint32_t kTopTrickCountBase = 15;
        constexpr int32_t kTopTrickCountSpread = 225;

        // The first trick button's air trick: its phases.
        enum AirTrickPhase : uint8_t
        {
            kAirTrickStart = 0,
            kAirTrickStarting = 1,
            kAirTrickHeld = 2,
            kAirTrickEnd = 3,
            kAirTrickEnding = 4,
        };
        // The flips (a trick's kinds & kTrickFlips): the backflip, the double and the triple, the front flip and
        // the double (kFlipNone none). Each turns the trick's first angle (about the right row) a frame by more
        // each frame, up to a most, until the flip has turned one, two or three turns less 30 degrees.
        constexpr uint32_t kTrickFlips = 0x7;
        enum Flip : uint32_t
        {
            kFlipNone = 0,
            kFlipBack = 1,
            kFlipDoubleBack = 2,
            kFlipTripleBack = 3,
            kFlipFront = 4,
            kFlipDoubleFront = 5,
        };
        constexpr uint32_t kOneFlip = 0xEAAA;      // 330 degrees
        constexpr uint32_t kTwoFlips = 0x1EAAA;    // 690
        constexpr uint32_t kThreeFlips = 0x2EAAA;  // 1050
        constexpr ee::Float kFrontFlipShare(0.75f); // a front flip turns the other way, three quarters as fast
        // The flip's turn: its step and its most a frame (times the flip stat and the flip's rate, a degree in
        // 16-bit turns), DM2's.
        inline ee::Global<ee::Float, sym::g_turnLeast> g_turnLeast;
        inline ee::Global<ee::Float, sym::g_turnMost> g_turnMost;
        inline ee::Global<ee::Float, sym::g_turnRate1> g_turnRate1;   // the backflip's
        inline ee::Global<ee::Float, sym::g_turnRate23> g_turnRate23; // the double's and the triple's
        inline ee::Global<ee::Float, sym::g_turnRate4> g_turnRate4;   // the front flip's
        inline ee::Global<ee::Float, sym::g_turnRate5> g_turnRate5;   // the double front flip's
        // The turning trick's angles fall off by this share a frame (0.15).
        inline ee::Global<ee::Float, sym::g_spinFalloff> g_spinFalloff;
        // The trick on top: in by 0.05 a frame (and again at the end), out by a fifteenth.
        inline ee::Global<ee::Float, sym::g_airTopBlendIn> g_airTopBlendIn;
        inline ee::Global<ee::Float, sym::g_airTopBlendEnd> g_airTopBlendEnd;
        inline ee::Global<ee::Float, sym::g_airTopBlendOut> g_airTopBlendOut;

        // A turning trick's angle after its frame's fall-off.
        int16_t spunDown(int16_t angle, ee::Float falloff)
        {
            return static_cast<int16_t>(
                angle - ee::toInt(ee::toFloat(angle) * ee::Float(1.0f / 65536.0f) * falloff * ee::Float(65536.0f)));
        }
        // The air trick's end of a frame: the trick on top's frames counted while it shows, and the sub-state's.
        int32_t airTrickFrame(Rider *rider)
        {
            if (!(rider->topTrickBlend == ee::Float(0.0f)))
                ++rider->topTrickFrames;
            ++rider->actionFrames[kLevelSubState];
            return 1;
        }
        // The trick on top blended in (when on), then the frame's end.
        int32_t topTrickIn(Rider *rider)
        {
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_airTopBlendIn, ee::Float(1.0f));
            return airTrickFrame(rider);
        }
        // To the trick's end phase.
        int32_t toAirTrickEnd(Rider *rider)
        {
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickEnd));
            return 0;
        }
        // Held with a trick on top whose loops count as held: a loop every topTrickLoopFrames of the sub-state.
        void countTopTrickLoop(Rider *rider)
        {
            if (rider->topTrickBlend == ee::Float(0.0f))
                return;
            const uint8_t every = rider->topTrickLoopFrames;
            if (every != 0 && rider->actionFrames[kLevelSubState] % every == 0) // (a zero is the original's trap; never)
                ++rider->trickLoops;
        }
    }

    // Air sub-state 1, the first trick button's air trick: the trick from the taps (none: no trick with the air's
    // loop), its start (a flip turns the rows from where they were), its loop while the button is held (counted),
    // a trick on top by the second button's taps, a flip turning until it has turned its most, the end, then no
    // trick with "air.coast.start". DM2's airTrick with BMX XXX's motions by name and rates, the flips by the flip
    // stat, and the trick on top's frames counted apart from the sub-state's.
    int32_t airTrick(Rider *rider)
    {
        ee::Local<char[0x40]> text;
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);
        if (rider->trickMotion != kNoTrick && moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);
        if (hasFlag(rider, kFlagTrickTurn))
        {
            const ee::Float falloff = *g_spinFalloff;
            rider->spinAngles[0] = spunDown(rider->spinAngles[0], falloff);
            rider->spinAngles[1] = spunDown(rider->spinAngles[1], falloff);
            rider->spinAngles[2] = spunDown(rider->spinAngles[2], falloff);
        }
        auto rate = [rider](uint32_t motion) { return rider->techniqueStat * (*g_trickMotionRates)[rider->trick][motion]; };

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kAirTrickStart:
        {
            rider->trickMotion = kNoTrick;
            const uint8_t trick = airTrickByTaps(rider);
            rider->trick = trick;
            if (trick == kNoTrick)
            {
                clearTaps(rider, 1u);
                riderMotionByName(kAirCoastLoop, &rider->motionName, &rider->motion);
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                return airTrickFrame(rider);
            }
            zge::formatString(text.address(), sym::textAirTrickStartFormat, (*g_trickMotionNames)[trick]);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMirrored);
            *matrix(rider->spinFrom) = *matrix(rider->right);
            rider->spinAngles[0] = 0;
            rider->spinAngles[1] = 0;
            rider->spinAngles[2] = 0;
            if ((*g_trickKinds)[rider->trick] & kTrickFlips)
            {
                setFlag(rider, kFlagTrickTurn);
                if (hasFlag(rider, kFlagSnapAlign))
                {
                    rider->snapFrames = 0;
                    clearFlag(rider, kFlagSnapAlign);
                }
            }
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickStarting));
            rider->motionRate = rate(0);
            if (rider->trickMotion != kNoTrick) // (never: cleared above, as DM2's)
            {
                rider->topTrickBlend = 0.0f;
                setFlag(rider, kFlagTopTrick);
            }
            return airTrickFrame(rider);
        }
        case kAirTrickStarting:
        case kAirTrickHeld:
            break;
        case kAirTrickEnd:
            zge::formatString(text.address(), sym::textAirTrickEndFormat, (*g_trickMotionNames)[rider->trick]);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickEnding));
            rider->motionFlags |= kMotionRestart;
            rider->motionRate = rate(2);
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_airTopBlendEnd, ee::Float(1.0f));
            return airTrickFrame(rider);
        case kAirTrickEnding:
        {
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
                return 0;
            }
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_airTopBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                clearFlag(rider, kFlagTopTrick);
            return airTrickFrame(rider);
        }
        default:
            return airTrickFrame(rider);
        }

        // Started or held: a trick on top by the second button's taps (none yet; the button pressed before the
        // taps waits for them).
        if (rider->trickMotion == kNoTrick)
        {
            bool pressed = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) != 0;
            if (pressed || (hasFlag(rider, kFlagTrickBWaiting) && rider->tapCount != 0))
            {
                if (rider->tapCount == 0)
                    setFlag(rider, kFlagTrickBWaiting);
                else
                {
                    const uint8_t tapIndex = trickFromTaps(rider);
                    const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
                    if (top != kNoTrick)
                    {
                        rider->trickMotion = top;
                        clearTaps(rider, 1u);
                        setFlag(rider, kFlagTopTrick);
                        rider->topTrickBlend = 0.0f;
                        rider->topTrickLoopFrames = static_cast<uint8_t>(
                            ee::toInt((ee::Float(1.0f) - rider->techniqueStat) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
                        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                            setFlag(rider, kFlagTrickHeld);
                        else
                            clearFlag(rider, kFlagTrickHeld);
                    }
                }
            }
        }

        const uint32_t flip = (*g_trickKinds)[rider->trick] & kTrickFlips;
        if (flip == kFlipNone)
        {
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase == kAirTrickStarting)
            {
                // The start played: the loop, unless the button is let go (and the trick's loops count only held).
                if (!hasFlag(rider, kFlagMotionEnded))
                    return topTrickIn(rider);
                if (!hasFlag(rider, kFlagTrickHeld) && !((*g_trickKinds)[rider->trick] & kTrickCountsHeld))
                    return toAirTrickEnd(rider);
                zge::formatString(text.address(), sym::textAirTrickLoopFormat, (*g_trickMotionNames)[rider->trick]);
                riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickHeld));
                rider->motionRate = rate(1);
                return topTrickIn(rider);
            }
            if (phase != kAirTrickHeld)
                return topTrickIn(rider);
            // Held: the trick on top's loops counted; at the loop's end, again while the button is held (a trick
            // whose loops count held plays it once).
            if (rider->trickMotion != kNoTrick && ((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld))
                countTopTrickLoop(rider);
            if (!hasFlag(rider, kFlagMotionEnded))
                return topTrickIn(rider);
            if ((*g_trickKinds)[rider->trick] & kTrickCountsHeld)
                clearFlag(rider, kFlagTrickHeld);
            if (!hasFlag(rider, kFlagTrickHeld))
                return toAirTrickEnd(rider);
            rider->motionFlags |= kMotionRestart;
            return topTrickIn(rider);
        }

        // A flip: its angle a frame grows by the step to the most (a back flip's one way, a front flip's the other),
        // until the flip has turned its most.
        const ee::Float stat = rider->flipStat;
        uint32_t most;
        int16_t angle, limit;
        auto backFlip = [&](uint32_t turns, ee::Float flipRate) {
            most = turns;
            const int32_t step = ee::toInt(*g_turnLeast * stat * flipRate);
            const int32_t top = ee::toInt(*g_turnMost * stat * flipRate);
            angle = static_cast<int16_t>(static_cast<uint16_t>(rider->spinAngles[0]) - step);
            limit = static_cast<int16_t>(top);
        };
        auto frontFlip = [&](uint32_t turns, ee::Float flipRate) {
            most = turns;
            const int32_t top = ee::toInt(*g_turnMost * kFrontFlipShare * stat * flipRate);
            const int32_t step = ee::toInt(-(*g_turnLeast * kFrontFlipShare) * stat * flipRate);
            limit = static_cast<int16_t>(top);
            angle = static_cast<int16_t>(static_cast<uint16_t>(rider->spinAngles[0]) - step);
        };
        switch (flip)
        {
        case kFlipDoubleBack:
            backFlip(kTwoFlips, *g_turnRate23);
            break;
        case kFlipTripleBack:
            backFlip(kThreeFlips, *g_turnRate23);
            break;
        case kFlipFront:
            frontFlip(kOneFlip, *g_turnRate4);
            break;
        case kFlipDoubleFront:
            frontFlip(kTwoFlips, *g_turnRate5);
            break;
        default: // the backflip (and kinds 6, 7)
            backFlip(kOneFlip, *g_turnRate1);
            break;
        }
        const int32_t total = rider->trickTurned;
        const uint32_t sofar = total < 0 ? 0u - static_cast<uint32_t>(total) : static_cast<uint32_t>(total);
        if (sofar >= most)
            return toAirTrickEnd(rider);
        int32_t turn = angle < limit ? angle : limit;
        if (turn < -int32_t(limit))
            turn = -int32_t(limit);
        rider->spinAngles[0] = static_cast<int16_t>(turn);
        rider->trickTurned = total + static_cast<int16_t>(turn);
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kAirTrickStarting)
        {
            if (hasFlag(rider, kFlagMotionEnded))
            {
                zge::formatString(text.address(), sym::textAirTrickLoopFormat, (*g_trickMotionNames)[rider->trick]);
                riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickHeld));
            }
            return topTrickIn(rider);
        }
        // Held, the trick on top trick 0 (as DM2's: not kNoTrick): its loops counted.
        if (phase == kAirTrickHeld && rider->trickMotion == 0)
            countTopTrickLoop(rider);
        return topTrickIn(rider);
    }
    READABLE_FUNCTION(airTrick, Verified);

    namespace
    {
        // The trick B tricks ("modifiers"): their motions by name ("modifier.%s.start", ".loop", ".end": the
        // names by trick, after the tricks' 100) and the rates of the three (times the technique stat: after the
        // tricks' 100 rows).
        inline ee::Global<ee::Ptr<const char>[256], sym::g_modifierTrickNames> g_modifierTrickNames;
        inline ee::Global<ee::Float[256][3], sym::g_modifierMotionRates> g_modifierMotionRates;
        // Its phases.
        enum TrickBPhase : uint8_t
        {
            kTrickBStart = 0,
            kTrickBStarting = 1,
            kTrickBHeld = 2,
            kTrickBEnding = 3,
        };
    }

    // Air sub-state 2, the second trick button's air trick (a "modifier"): the trick from the taps (none: back to
    // no trick, "air.coast.start"), its start, its loop while the button is held (counted), its end, then no
    // trick with "air.coast.start". DM2's airTrickB with BMX XXX's motions by name and rates.
    int32_t airTrickB(Rider *rider)
    {
        ee::Local<char[0x40]> text;
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);
        auto ended = [rider]() {
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
            riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
            return 0;
        };
        auto rate = [rider](uint32_t motion) { return rider->techniqueStat * (*g_modifierMotionRates)[rider->trickMotion][motion]; };

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kTrickBStart:
        {
            const uint8_t trick = trickFromTaps(rider);
            if (trick == kNoTrick)
            {
                riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
                clearTaps(rider, 1u);
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                return 0;
            }
            rider->trickMotion = trick;
            zge::formatString(text.address(), sym::textModifierStartFormat, (*g_modifierTrickNames)[rider->trickMotion]);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            rider->trickLoops = 0;
            clearFlag(rider, kFlagMirrored);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickBStarting));
            rider->motionRate = rate(0);
            return 1;
        }
        case kTrickBStarting:
            if (!hasFlag(rider, kFlagMotionEnded))
                return 1;
            ++rider->trickLoops;
            if (hasFlag(rider, kFlagTrickHeld))
            {
                zge::formatString(text.address(), sym::textModifierLoopFormat, (*g_modifierTrickNames)[rider->trickMotion]);
                riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickBHeld));
                rider->motionRate = rate(1);
                return 1;
            }
            zge::formatString(text.address(), sym::textModifierEndFormat, (*g_modifierTrickNames)[rider->trickMotion]);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickBEnding));
            return 1;
        case kTrickBHeld:
            if (!hasFlag(rider, kFlagMotionEnded))
                return 1;
            if ((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld)
                ++rider->trickLoops;
            if (hasFlag(rider, kFlagTrickHeld))
            {
                rider->motionFlags |= kMotionRestart;
                return 1;
            }
            zge::formatString(text.address(), sym::textModifierEndFormat, (*g_modifierTrickNames)[rider->trickMotion]);
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickBEnding));
            rider->motionRate = rate(2);
            return 1;
        case kTrickBEnding:
            if (hasFlag(rider, kFlagMotionEnded))
                return ended();
            return 1;
        default:
            return 1;
        }
    }
    READABLE_FUNCTION(airTrickB, Verified);

    namespace
    {
        // How long the air's sub-state 3 keeps the motion it was given, by its phase (the way off the rail:
        // grind.cpp's kOff*): fallen off a grind 60 frames, a stall 60, off a lip 20, a grind 7, a stall 15, a lip
        // at 85 degrees 20.
        constexpr uint32_t kOffRailPhases = 6;
        inline ee::Global<uint32_t[kOffRailPhases], sym::g_offRailMotionFrames> g_offRailMotionFrames;
    }

    // Air sub-state 3, off a rail, a stall or a lip: the motion it left with (the air's loop once that ended)
    // for the frames its phase gives, then back to no trick with the air's loop. DM2's airSubState3, which
    // keeps it 60 frames whatever the phase.
    int32_t airSubState3(Rider *rider)
    {
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if ((*g_offRailMotionFrames)[phase] < rider->actionFrames[kLevelSubState])
        {
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
            riderMotionByName(kAirCoastLoop, &rider->motionName, &rider->motion);
            return 0;
        }
        if (hasFlag(rider, kFlagMotionEnded))
        {
            riderMotionByName(kAirCoastLoop, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
        ++rider->actionFrames[kLevelSubState];
        return 1;
    }
    READABLE_FUNCTION(airSubState3, Verified);
}
