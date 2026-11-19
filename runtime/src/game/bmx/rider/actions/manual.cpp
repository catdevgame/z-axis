// BMX XXX's manual action state (action.c): action state 5, on one wheel
// (its motions are "ground.manual.*"; DM2's wheelieState, game/dm2/rider/
// actions/states.cpp, whose name the state keeps here).

#include "../rider.h"
#include "../collision/collision.h"
#include "../game_calls.h"
#include "../physics/physics.h"
#include "../vector.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/manual.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // "ground.manual.end": off the wheel into the air.
        constexpr uint32_t kGroundManualEnd = sym::textGroundManualEnd;

        // The manual's sub-states (0; no 1). The table runs on into the
        // flatland's (flatland.cpp: its 2 .. 4 are the flatland's 0 .. 2).
        using SubState = ee::FunctionPtr<int32_t(Rider *)>;
        inline ee::Global<SubState[5], sym::g_wheelieSubStates> g_wheelieSubStates;

        // Steering: 15 times the stick a frame toward 75 times it, both
        // less with speed (1 / (1 + 30 speed), 0.05 at least), in degrees
        // a frame (182.04 turn units a degree).
        constexpr ee::Float kSteerSpeed = 30.0f;
        inline ee::Global<ee::Float, sym::g_wheelieSteerLeast> g_wheelieSteerLeast;
        inline ee::Global<ee::Float, sym::g_wheelieDegreesToTurn> g_degreesToTurn;
        inline ee::Global<ee::Float, sym::g_wheelieSteerRate> g_wheelieSteerRate;
        inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_wheelieSteerMost;
        inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnFalloff; // 0.35
        constexpr int32_t kTurnStopped = 91; // half a degree

        // A trick button waits this many frames for the stick's taps.
        constexpr uint16_t kTapWaitFrames = 8;
        constexpr uint32_t kMoveTrickA = 3; // (DM2's)
        constexpr uint32_t kMovePressed = 0;
        constexpr uint32_t kLevelStep = 3;  // the action levels' fourth: its frames count the wait
        constexpr uint8_t kManualPhases = 5; // the manual's phases a trick can start from

        // The yaw rate turned by the stick, held within -most .. most.
        void steerYawRate(Rider *rider, int32_t turned, int16_t most)
        {
            const int16_t below = static_cast<int16_t>(int16_t(turned) < most ? int16_t(turned) : most);
            rider->yawRate = below < -most ? static_cast<int16_t>(-most) : below;
        }
        // The stick let go: the yaw rate falls off, to 0 under half a degree.
        void yawRateFalloff(Rider *rider)
        {
            const int32_t yawRate = rider->yawRate;
            rider->yawRate = static_cast<int16_t>(
                yawRate - ee::toInt(ee::toFloat(yawRate) * ee::Float(1.0f / 65536.0f) * *g_turnFalloff * ee::Float(65536.0f)));
            if (ee::abs(ee::toFloat(int32_t(rider->yawRate))) < ee::Float(float(kTurnStopped)))
                rider->yawRate = 0;
        }
    }

    // Action state 5, the manual. Off the wheel into the air (or physics
    // state 12): the air state, with the manual's end motion; on both
    // wheels: the landing; a bail: the bail. Else the stick steers, and in
    // the manual's first five phases trick button A starts a trick: with
    // the stick's taps in, the flatland's sub-state 1 with the
    // sub-state's frames and the manual's balance kept; with none yet, the
    // button waits up to 8 frames for them. Then the manual's sub-state.
    // DM2's wheelieState with the trick's start added.
    int32_t wheelieState(Rider *rider)
    {
        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsAir || physics == kPhysicsTumble)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kGroundManualEnd, &rider->motionName, &rider->motion);
            setFlag(rider, kFlagLeftRail);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        }
        if (physics == kPhysicsGround)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        }
        if (physics == kPhysicsBail)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }

        ButtonRecord *const input = &rider->input;
        const ee::Float steer = inputStick(input, 0u)->x;
        if (steer == ee::Float(0.0f))
            yawRateFalloff(rider);
        else
        {
            ee::Float share = ee::Float(1.0f) / (rider->speed * kSteerSpeed + ee::Float(1.0f));
            if (!(*g_wheelieSteerLeast <= share))
                share = *g_wheelieSteerLeast;
            const ee::Float scale = *g_degreesToTurn;
            const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * (*g_wheelieSteerRate * share) * scale);
            steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
        }

        if (actionState(rider, uint32_t(kLevelPhase)) < kManualPhases)
        {
            const bool pressed = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMovePressed) != 0;
            if (pressed || hasFlag(rider, kFlagTrickAWaiting))
            {
                if (rider->tapCount != 0)
                {
                    // The trick: the flatland, the frames and the balance kept.
                    const uint16_t frames = rider->actionFrames[kLevelSubState];
                    const ee::Float balance = rider->wheelieBalance;
                    setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionFlatland));
                    setActionState(rider, uint32_t(kLevelSubState), 1u);
                    rider->actionFrames[kLevelSubState] = frames;
                    rider->wheelieBalance = balance;
                    setFlag(rider, kFlagActed);
                    return 0;
                }
                if (pressed)
                {
                    // No taps yet: the button waits for them.
                    rider->actionFrames[kLevelStep] = 0;
                    setFlag(rider, kFlagTrickAWaiting);
                    clearFlag(rider, kFlagTrickBWaiting);
                }
                else if (++rider->actionFrames[kLevelStep] >= kTapWaitFrames)
                    clearFlag(rider, kFlagTrickAWaiting);
            }
        }

        for (;;)
        {
            const int32_t kept = (*g_wheelieSubStates)[rider->actionStates[kLevelSubState]](rider);
            if (kept != 0 || actionState(rider, uint32_t(kLevelAction)) != kActionWheelie)
                return kept;
        }
    }
    READABLE_FUNCTION(wheelieState, Verified);

    namespace
    {
        // The manual's sub-state 0 (wheelieSubState): DM2's with one phase more (onto the wheel in the loop's
        // motion) and BMX XXX's motions by name ("ground.manual.*", "ground.noseWheelie.*").
        enum WheeliePhase : uint8_t
        {
            kWheelieOnto = 0,        // the start's motion
            kWheelieOntoLooping = 1, // the loop's motion at once (what enters it so is not read)
            kWheelieOn = 2,
            kWheelieTrickHeld = 3,
            kWheelieTrickOut = 4,
            kWheelieCrouched = 5,
            kWheelieOff = 6,
            kWheelieOffEnding = 7,
        };
        // "noseWheelie", "manual": the motions' middle word by the wheel balanced on (the back's in the manual).
        inline ee::Global<uint32_t[2], sym::g_wheelieMotionNames> g_wheelieMotionNames;
        constexpr uint32_t kGroundCrouchEnd = sym::textGroundCrouchEnd;     // "ground.crouch.end"
        // The tricks by the wheel (DM2's 70 and 71, its trick names' "manual" and "nose wheelie").
        constexpr uint8_t kTrickManual = 0x48;
        constexpr uint8_t kTrickNoseWheelie = 0x49;
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMoveTrickB = 4;
        constexpr uint32_t kMoveWheelieHeld = 1;
        constexpr uint32_t kMoveWheelieReleased = 2;
        // The balancing wheel slower than this: the crouch's phase (the original's number, DM2's off phase).
        inline ee::Global<ee::Float, sym::g_wheelieWheelLeast> g_wheelieWheelLeast;     // 0.005
        // The trick on top: in by 0.05 a frame, out by a fifteenth (out of the crouch too).
        inline ee::Global<ee::Float, sym::g_wheelieTopBlendIn> g_wheelieTopBlendIn;
        inline ee::Global<ee::Float, sym::g_wheelieTopBlendOut> g_wheelieTopBlendOut;
        inline ee::Global<ee::Float, sym::g_wheelieCrouchBlendOut> g_wheelieCrouchBlendOut;
        // The tricks on top of a grind, stall or wheelie (a row of 0x56 a trick, by trickFromTaps' index), kNoTrick
        // for none; a trick's flags (kTrickCountsHeld: its loops counted while held).
        constexpr uint32_t kTopTrickRow = 0x56;
        inline ee::Global<uint8_t[256][kTopTrickRow], sym::grindTopTricks> g_topTricks;
        inline ee::Global<uint16_t[256], sym::g_trickFlags> g_trickFlags;
        constexpr uint16_t kTrickCountsHeld = 0x20;
        constexpr uint32_t kTopTrickCountBase = 15;
        constexpr int32_t kTopTrickCountSpread = 225;
        // The crouch let go: the hop (riding's crouch's: crouchState).
        inline ee::Global<ee::Float, sym::g_hopPerStat> g_hopPerStat;
        inline ee::Global<ee::Float, sym::g_hopLeast> g_hopLeast;
        inline ee::Global<ee::Float, sym::g_hopSlopeLoss> g_hopSlopeLoss;
        inline ee::Global<uint32_t, sym::g_hopFullFrames> g_hopFullFrames;
        // The balance, after the sub-state's first 30 frames (flatland.cpp's, the same values): it tips the way
        // it leans by 0.125 a frame (a quarter more at a stop, less with speed up to 1), less with the manual
        // stat, and more by 0.035 over the balance frames up to 1800; at 1 either way the rider bails (kind 12) or,
        // tipped back onto both wheels, lands. Before the off phase the stick's y leans it by 0.045 times the
        // manual stat. It is never nearer 0 than 0.001.
        constexpr uint16_t kBalanceFromFrames = 30;
        inline ee::Global<ee::Float, sym::g_flatlandSlowSpeed> g_balanceSlowSpeed;
        inline ee::Global<ee::Float, sym::g_flatlandSlowTip> g_balanceSlowTip;
        inline ee::Global<ee::Float, sym::g_flatlandTip> g_balanceTip;
        inline ee::Global<uint32_t, sym::g_flatlandTipFrames> g_balanceTipFrames;
        inline ee::Global<ee::Float, sym::g_flatlandTipGrowth> g_balanceTipGrowth;
        inline ee::Global<ee::Float, sym::g_flatlandStickLean> g_balanceStickLean;
        inline ee::Global<ee::Float, sym::g_wheelieBalanceLeast> g_wheelieBalanceLeast;
        inline ee::Global<ee::Float, sym::g_wheelieBalanceLeastBack> g_wheelieBalanceLeastBack;
        // The wheels' drag on one wheel.
        inline ee::Global<ee::Float, sym::g_flatlandDrag> g_wheelieDrag;
        constexpr uint8_t kBailTippedOver = 12; // Rider::bailKind
        // Rider::contactKind: on kinds 1 .. 3 the hop is less up a slope; on 1 and 3 the stick straight up asks
        // for kFlagHopUp (the kinds are not named).
        constexpr uint8_t kContactNoStickTest = 2;
    }

    // The manual's sub-state 0: on one wheel (the back in the manual, physics state 10, else the front). Below the
    // off phase the sub-state goes to the crouch's phase once a wheel on the ground rolls slower than 0.005. By
    // phase: onto the wheel (the start's motion, or the loop's at once), the trick by the wheel and the taps
    // cleared; on it: the crouch held to the crouch's phase, trick button B with the stick's taps for a trick on
    // top (as the grind's; it fades in while held, its loops counted, and out once let go), the loop's motion at
    // each end; crouched: let go, the hop into the air ("ground.crouch.end"), else the trick on top fades; off: the
    // end's motion on the ground. Then the balance (above), the sub-state's and the phase's frames, the wheels'
    // drag and the trick on top's frames. Returns 0 the frame the state changes.
    int32_t wheelieSubState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ee::Local<zge::Matrix> rows;
        ee::Local<char[0x40]> motionText;
        ee::Local<uint32_t[2]> motionNames;
        ButtonRecord *const input = &rider->input;
        motionNames[0] = (*g_wheelieMotionNames)[0];
        motionNames[1] = (*g_wheelieMotionNames)[1];
        const uint32_t onBack = physicsState(rider) == kPhysicsManual;
        // The motion "ground.<the wheel's name>.loop" (start, end) started over.
        enum class Motion { Loop, Start, End };
        auto motion = [&](Motion which) {
            if (which == Motion::Loop)
                zge::formatString(motionText.address(), sym::textGroundLoopFormat, motionNames[onBack]);
            else if (which == Motion::Start)
                zge::formatString(motionText.address(), sym::textGroundStartFormat, motionNames[onBack]);
            else
                zge::formatString(motionText.address(), sym::textGroundEndFormat, motionNames[onBack]);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        };
        auto changed = [rider](uint32_t level, uint32_t state) {
            setActionState(rider, level, state);
            return 0;
        };

        if (actionState(rider, uint32_t(kLevelPhase)) < kWheelieCrouched)
        {
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                if (rider->wheelPoses[wheel].ground == kWheelOffGround)
                    continue;
                if (rider->wheelPoses[wheel].speed < *g_wheelieWheelLeast)
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieCrouched));
                break;
            }
        }

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kWheelieOnto:
        case kWheelieOntoLooping:
        {
            motion(actionState(rider, uint32_t(kLevelPhase)) == kWheelieOntoLooping ? Motion::Loop : Motion::Start);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieOn));
            rider->trick = onBack ? kTrickManual : kTrickNoseWheelie;
            clearTaps(rider, 1u);
            break;
        }
        case kWheelieOn:
        case kWheelieTrickHeld:
        case kWheelieTrickOut:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveWheelieHeld) != 0)
                return changed(uint32_t(kLevelPhase), uint32_t(kWheelieCrouched));
            const uint32_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase == kWheelieTrickHeld)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveWheelieReleased) != 0)
                    return changed(uint32_t(kLevelPhase), uint32_t(kWheelieTrickOut));
                if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
                {
                    const uint8_t every = rider->topTrickLoopFrames;
                    const uint16_t frames = rider->actionFrames[kLevelSubState];
                    if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                        ++rider->trickLoops;
                }
                if (hasFlag(rider, kFlagTopTrick))
                    rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_wheelieTopBlendIn, ee::Float(1.0f));
            }
            else if (phase == kWheelieOn)
            {
                uint8_t taps = 0;
                bool tryTrick = true;
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
                {
                    if (!hasFlag(rider, kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                        tryTrick = false;
                }
                else
                    taps = rider->tapCount;
                if (tryTrick)
                {
                    if (taps == 0)
                        setFlag(rider, kFlagTrickBWaiting);
                    else
                    {
                        const uint8_t tapIndex = trickFromTaps(rider);
                        const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
                        if (top != kNoTrick)
                        {
                            setFlag(rider, kFlagTopTrick);
                            rider->trickMotion = top;
                            rider->topTrickBlend = 0.0f;
                            rider->topTrickLoopFrames = static_cast<uint8_t>(
                                ee::toInt((ee::Float(1.0f) - rider->techniqueStat) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
                            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveWheelieHeld) != 0)
                                setFlag(rider, kFlagTrickHeld);
                            else
                                clearFlag(rider, kFlagTrickHeld);
                            return changed(uint32_t(kLevelPhase), uint32_t(kWheelieTrickHeld));
                        }
                        clearTaps(rider, 1u);
                    }
                }
            }
            else if (phase == kWheelieTrickOut)
            {
                rider->topTrickBlend = ee::max(rider->topTrickBlend - *g_wheelieTopBlendOut, ee::Float(0.0f));
                if (rider->topTrickBlend == ee::Float(0.0f))
                    clearFlag(rider, kFlagTopTrick);
            }
            if (hasFlag(rider, kFlagMotionEnded))
                motion(Motion::Loop);
            break;
        }
        case kWheelieCrouched:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveWheelieHeld) != 0)
            {
                if (hasFlag(rider, kFlagMotionEnded))
                    motion(Motion::Loop);
                if (hasFlag(rider, kFlagTopTrick))
                {
                    rider->topTrickBlend = ee::max(rider->topTrickBlend - *g_wheelieCrouchBlendOut, ee::Float(0.0f));
                    if (rider->topTrickBlend == ee::Float(0.0f))
                        clearFlag(rider, kFlagTopTrick);
                }
                break;
            }
            // Let go: the hop, as the crouch's (more with the hop stat over the phase's first 15 frames), on
            // contact kinds 1 .. 3 less up a slope along the balancing wheel's forward row.
            *impulse = zge::kZero;
            const uint32_t full = *g_hopFullFrames;
            const ee::Float least = *g_hopLeast;
            const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
            const uint32_t frames = rider->actionFrames[kLevelPhase];
            const ee::Float shareOf = zge::unsignedToFloat(frames < full ? frames : full);
            ee::Float hop = perFrame * shareOf / zge::unsignedToFloat(full) + least;
            const uint8_t contact = rider->contactKind;
            if (contact == 1 || contact == kContactNoStickTest || contact == 3)
            {
                if (contact != kContactNoStickTest && ee::Float(0.5f) < ee::abs(inputStick(input, 0u)->y) &&
                    ee::abs(inputStick(input, 0u)->x) < ee::Float(0.25f) && inputStick(input, 0u)->y < ee::Float(-0.5f))
                    setFlag(rider, kFlagHopUp);
            }
            if ((contact == 1 || contact == kContactNoStickTest || contact == 3) && rider->velocity[1] <= ee::Float(0.0f))
            {
                *impulse = *vec4(&rider->wheelPoses[onBack].matrix[8]);
                if (zge::dot(*vec4(rider->velocity), *impulse) < ee::Float(0.0f))
                    zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                hop = hop - hop * (*g_hopSlopeLoss * -impulse->y);
            }
            *impulse = zge::kZero;
            impulse->y = -hop;
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kGroundCrouchEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        }
        case kWheelieOff:
            motion(Motion::End);
            setPhysicsState(rider, uint32_t(kPhysicsGround));
            clearFlag(rider, kFlagOnOneWheel);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieOffEnding));
            break;
        default:
            break;
        }

        // The balance.
        if (rider->actionFrames[kLevelSubState] >= kBalanceFromFrames)
        {
            ee::Float tip = 1.0f;
            const ee::Float slow = *g_balanceSlowSpeed;
            if (rider->speed < slow)
                tip = (slow - rider->speed) * (*g_balanceSlowTip / slow) + tip;
            tip = tip * *g_balanceTip;
            tip = tip * (ee::Float(1.0f) - rider->manualStat);
            const uint32_t most = *g_balanceTipFrames;
            const uint32_t ridden = uint32_t(rider->balanceFrames) < most ? uint32_t(rider->balanceFrames) : most;
            tip = tip + ee::toFloat(int32_t(ridden)) * (*g_balanceTipGrowth / ee::toFloat(int32_t(most)));
            const ee::Float side = ee::Float(0.0f) < rider->wheelieBalance ? ee::Float(1.0f) : ee::Float(-1.0f);
            rider->wheelieBalance = rider->wheelieBalance + tip * side;
            if (ee::Float(1.0f) <= ee::abs(rider->wheelieBalance))
            {
                const bool towardFront = ee::Float(0.0f) < rider->wheelieBalance;
                if (towardFront == bool(onBack))
                {
                    setPhysicsState(rider, uint32_t(kPhysicsBail));
                    rider->bailKind = kBailTippedOver;
                    setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
                    return 0;
                }
                // Down on both wheels: the rows stood up on the balancing wheel's up and the drawn body's forward
                // row, given to both wheels, the drawn body and the rider; the other wheel takes the balancing
                // wheel's velocity, speed, spin and ground.
                WheelPose &from = rider->wheelPoses[onBack];
                WheelPose &to = rider->wheelPoses[onBack ? 0 : 1];
                zge::Vec4 &right = zge::matrixRow(*rows, 0);
                zge::Vec4 &up = zge::matrixRow(*rows, 1);
                zge::Vec4 &forward = zge::matrixRow(*rows, 2);
                up = *vec4(&from.matrix[4]);
                forward = zge::matrixRow(*matrix(rider->body), 2);
                right = zge::cross(up, forward);
                right = zge::unit(right);
                forward = zge::cross(right, up);
                zge::matrixRow(*rows, 3) = zge::kZero;
                *matrix(from.matrix) = *rows;
                *matrix(to.matrix) = *rows;
                *matrix(rider->body) = *rows;
                *matrix(rider->right) = *rows;
                *vec4(from.velocity) = *vec4(rider->velocity);
                *vec4(to.velocity) = *vec4(rider->velocity);
                from.speed = rider->speed;
                to.speed = rider->speed;
                to.spin = from.spin;
                to.ground = from.ground;
                setPhysicsState(rider, uint32_t(kPhysicsGround));
                return changed(uint32_t(kLevelAction), uint32_t(kActionLanding));
            }
            if (actionState(rider, uint32_t(kLevelPhase)) < kWheelieOff)
                rider->wheelieBalance = rider->wheelieBalance + inputStick(input, 0u)->y * (*g_balanceStickLean * rider->manualStat);
        }
        if (ee::abs(rider->wheelieBalance) < *g_wheelieBalanceLeast)
            rider->wheelieBalance = ee::Float(0.0f) <= rider->wheelieBalance ? *g_wheelieBalanceLeast : *g_wheelieBalanceLeastBack;
        ++rider->actionFrames[kLevelSubState];
        rider->wheelDrag = *g_wheelieDrag;
        ++rider->actionFrames[kLevelPhase];
        if (!(rider->topTrickBlend == ee::Float(0.0f)))
            ++rider->topTrickFrames;
        return 1;
    }
    READABLE_FUNCTION_WAITS(wheelieSubState, Verified);

    namespace
    {
        // The probe ahead of the balancing wheel (wheelieProbe): two rays a quarter unit to each side; a wall the
        // rider rides into is steeper than 30 degrees from the wheel's right row and within the most of its up
        // row, unless a floor (within 70 degrees of flat) or one of the world's surface kinds 1 .. 3 (a ramp's,
        // kind 8, always is one).
        constexpr ee::Float kProbeSide(0.25f);
        constexpr uint32_t kTouchWithRays = 1;      // touchObject's word for the rays
        constexpr uint32_t kTouchBail = 6;          // and for the bail into it
        constexpr int32_t kWallSteeper = 0x1556;    // 30 degrees
        constexpr int32_t kFloorAngle = 0x31C8;     // 70 degrees
        constexpr uint32_t kSurfaceWorldFirst = 1, kSurfaceWorldLast = 3;
        constexpr uint32_t kSurfaceRamp = 8;
        inline ee::Global<int16_t, sym::g_wallMostAngle> g_wallMostAngle;
        // The hit's sound above this closing speed (0.1), its volume 400 a unit of speed to 127; the pad's rumble
        // (the large motor) 765 a unit to 255; slower than g_wallBailSpeed (0.58) down onto both wheels.
        inline ee::Global<ee::Float, sym::g_wallSoundLeast> g_wallSoundLeast;
        inline ee::Global<ee::Float, sym::g_wallRumblePerSpeed> g_wallRumblePerSpeed;
        inline ee::Global<ee::Float, sym::g_wallBailSpeed> g_wallBailSpeed;
        constexpr uint32_t kWallHitSound = 0x23;
        constexpr ee::Float kVolumePerSpeed(400.0f);
        constexpr int32_t kMostVolume = 127;
        constexpr int32_t kMostRumble = 255;
        constexpr uint32_t kRumbleLargeMotor = 1;
        constexpr uint8_t kBailWallBounce = 3; // Rider::bailKind: bounced back off the wall
        constexpr uint32_t kWallSoundSurfaceRock = 7, kWallSoundSurfaceMetal = 2;
    }

    // wheelieProbe (rider): physics states 9 and 10's probe: two rays from the drawn body along the balancing wheel's
    // forward row (the way the rider moves), a quarter unit to each side, going on past objects the rider does not
    // touch. A hit on a wall the rider rides into makes the hit's sound and the pad's rumble; slow, the rider drops
    // onto both wheels (1); else he bails (2), bounced off a moving object when the impact bounced him, else the
    // distance counted and the object touched. 0 when neither ray hits so. DM2's wheelieProbe with BMX XXX's rays,
    // impact and rumble.
    int32_t wheelieProbe(Rider *rider)
    {
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> side;
        ee::Local<zge::Matrix> wheelRows; // the balancing wheel's matrix: right, up, forward, position
        ee::Local<collision::HitImpact> impact;
        ee::Local<zge::Vec4> plane;
        ee::Local<calls::Rumble> rumble;
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const ee::Float *const from = rider->drawnPosition;
        const uint8_t physics = physicsState(rider);
        *wheelRows = *matrix(rider->wheelPoses[physics == kPhysicsManual ? 1 : 0].matrix);
        const zge::Vec4 &wheelRight = wheelRows->row(0);
        const zge::Vec4 &wheelUp = wheelRows->row(1);
        const zge::Vec4 &wheelForward = wheelRows->row(2);
        for (uint32_t ray = 0; ray < 2; ++ray)
        {
            *to = wheelForward;
            if (zge::dot(velocity, wheelForward) < ee::Float(0.0f))
                zge::vec3Negate(&to->xyz(), &to->xyz());
            *side = *vec4(rider->right) * ((ray & 1u) ? -kProbeSide : kProbeSide);
            *to = *to + *side;
            *to = *to + *vec4(from);
            collision::RayHit *const hit = collision::rayCastTouching(rider, from, &to->x, kTouchWithRays);
            if (hit == nullptr)
                continue;
            const collision::FaceRef face = collision::hitFace(hit);
            collision::hitPlane(hit, &plane->x);

            // Is it a wall the rider rides into? A flat face of the world is not looked at; a steep one is, unless
            // a floor or one of the world's kinds.
            const bool flatWorld = plane->y == ee::Float(0.0f) &&
                                   (!(collision::hitFlags(hit) & 1) || collision::faceSurfaceKind(face) != kSurfaceWorldFirst);
            if (!flatWorld && zge::angleAcos(ee::abs(zge::dot(wheelRight, *plane))) >= kWallSteeper &&
                zge::angleAcos(zge::dot(wheelUp, *plane)) < *g_wallMostAngle &&
                !((collision::hitFlags(hit) & 1) && collision::faceSurfaceKind(face) == kSurfaceRamp))
            {
                if (zge::angleAcos(-plane->y) < kFloorAngle)
                    continue; // a floor
                bool world = false;
                for (uint32_t kind = kSurfaceWorldFirst; kind <= kSurfaceWorldLast && !world; ++kind)
                    world = collision::faceSurfaceKind(face) == kind;
                if (world)
                    continue;
            }

            impact->hit = ee::Ptr<collision::RayHit>(hit);
            *vec4(impact->velocity) = velocity;
            const ee::Float closing = collision::hitImpact(impact);
            if (!(ee::Float(0.0f) < closing))
                continue; // moving away from it

            // The hit's sound, by the face's, and the pad's rumble.
            uint32_t surface = 0;
            if (collision::faceSound(face) != 0xFFFF)
            {
                const uint32_t sound = collision::faceSound(face);
                surface = sound == 1 || sound == 3 ? kWallSoundSurfaceRock : sound == 2 ? kWallSoundSurfaceMetal : 0u;
            }
            if (*g_wallSoundLeast < closing)
            {
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, surface);
                const int32_t loud = ee::toInt(closing * kVolumePerSpeed);
                soundEvent(kWallHitSound, rider->position, static_cast<uint32_t>(code),
                                  static_cast<uint32_t>(loud < kMostVolume ? loud : kMostVolume), 0u);
                if (!hasFlag(rider, kFlagNoPlayer))
                {
                    const int32_t strength = ee::toInt(closing * *g_wallRumblePerSpeed);
                    rumble->motor = kRumbleLargeMotor;
                    rumble->timing = 0;
                    rumble->strength = static_cast<uint8_t>(strength < kMostRumble ? strength : kMostRumble);
                    calls::rumble(uint32_t(rider->player), rumble);
                }
            }
            if (closing < *g_wallBailSpeed)
            {
                setPhysicsState(rider, uint32_t(kPhysicsGround));
                return 1;
            }
            if (impact->flags & collision::kImpactBounced)
            {
                velocity = *vec4(impact->riderVelocity);
                rider->bailKind = kBailWallBounce;
            }
            else
            {
                rider->bailKind = 0;
                addRideDistance(rider, closing);
                collision::touchObject(rider, hit, kTouchBail);
            }
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            return 2;
        }
        return 0;
    }
    READABLE_FUNCTION(wheelieProbe, Verified);

    namespace
    {
        // wheelieMove's (DM2's): gravity a frame (0.0055); the pedals' push in the manual against this share of the
        // drag (0.75), the nose wheelie the rest; slower than 0.0015 and a share of the drag (0.3, the rider's
        // 0.3), stopped; no faster than the top speed (0.35) times the speed stat.
        inline ee::Global<ee::Float, sym::g_gravity> g_gravityStep;
        inline ee::Global<ee::Float, sym::g_wheelieSlide> g_wheelieSlide;
        inline ee::Global<ee::Float, sym::g_restSpeed> g_restSpeed;
        inline ee::Global<ee::Float, sym::g_stopDragShare> g_stopDragShare;
        inline ee::Global<ee::Float, sym::g_stopDragShareBody> g_stopDragShareBody;
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;
        // The drawn body's lean by the balance (turns a unit of balance, then a base): the manual's 1/16 and 25
        // degrees, the nose wheelie's 1/12 and 35 degrees. Its rows turn a quarter of the way to the wheel's
        // right row a frame, and 0.15 of the way to its up row.
        constexpr ee::Float kManualLean(0.0625f);
        constexpr int32_t kManualLeanBase = 0x11C7;
        inline ee::Global<ee::Float, sym::g_noseWheelieLean> g_noseWheelieLean;
        constexpr int32_t kNoseWheelieLeanBase = 0x18E3;
        constexpr ee::Float kRightShare(0.25f);
        inline ee::Global<ee::Float, sym::g_wheelieUpShare> g_upShare;
        // The wheel's spin from its speed (turns a unit: 2 / pi).
        inline ee::Global<ee::Float, sym::g_spinRate> g_spinRate;
        constexpr ee::Float kTurn(65536.0f); // angle units a turn

        // A wheel's point in the body's space and its height (the stats).
        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelHeight(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }
    }

    // wheelieMove (rider): states 9 and 10's move: the balancing wheel (the front in 9, the back in 10) rides the
    // ground as in the ground move. The wheel's point (in the world) is probed for walls (a bail ends it), moved
    // by its spring; groundAhead finds the face under it (none: into the air). The wheel's rows stand on the
    // face's plane, along the drawn body's forward row, turned by the yaw rate; the spring pulls toward its rest
    // (gravity across the face on the wheel's share of the mass); the impact and the hit are the ground's. The
    // wheel's speed: the drag of the surface, gravity along the slope, the pedals' push in the manual and the drag
    // of both; its velocity along its forward row. The drawn body turns toward the wheel's rows, leaned by the
    // balance (not with kFlagBalanceLeanOff), and is placed by the wheel's point, the rider's position the
    // wheel's point; the rider moves with the wheel's velocity (stopped when slow, held under the top speed),
    // riding fakie when it is against the drawn forward row. DM2's, without its grind test.
    void wheelieMove(Rider *rider)
    {
        ee::Local<zge::Vec4> across;     // the wheel's velocity across the face; then its point turned by the new rows
        ee::Local<zge::Vec4[2]> points;  // the wheel's point in the world (the original keeps a slot for each wheel)
        ee::Local<zge::Matrix> rows;     // the drawn body's new rows
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> before;   // the wheel's matrix before the move
        ee::Local<zge::Vec4> groundPoint;
        ee::Local<zge::Vec4> groundPlane;
        ee::Local<ee::Float> height;     // groundAhead's: the spring's new length in, the height over the ground out
        ee::Local<ee::Float> clearance;  // and the height less the spring's length
        const uint32_t wheel = physicsState(rider) == kPhysicsManual ? 1u : 0u;
        const bool manual = (wheel & 1u) != 0; // on the back wheel, which the pedals drive
        WheelPose &pose = rider->wheelPoses[wheel];
        zge::Matrix &wheelRows = *matrix(pose.matrix);
        zge::Vec4 &wheelRight = wheelRows.row(0);
        zge::Vec4 &wheelUp = wheelRows.row(1);
        zge::Vec4 &wheelForward = wheelRows.row(2);
        zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
        zge::Vec4 &drawn = *vec4(rider->drawnPosition);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyRight = body.row(0);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);

        // The wheel's point in the world.
        zge::Vec4 &point = points[wheel];
        pose.touched = 0;
        point = *vec4(wheelPoint(rider, wheel));
        point = zge::transformed(body, point);
        point = point + drawn;
        if (wheelieProbe(rider) == 2)
            return; // a bail (the impact)

        // The spring: its new length held within the stats' limits (its rate stopped there); the wheel's point
        // moves along the drawn up row by the change.
        const ee::Float length = pose.springLength, rate = pose.springRate;
        const ee::Float least = rider->wheelHeightLeast;
        ee::Float to = length + rate;
        *height = to;
        ee::Float moved = rate;
        if (to < least)
        {
            *height = least;
            moved = least - length;
            to = least;
            pose.springRate = ee::Float(0.0f);
        }
        else if (const ee::Float most = rider->wheelHeightMost; most < to)
        {
            *height = most;
            moved = most - length;
            to = most;
            pose.springRate = ee::Float(0.0f);
        }
        pose.springLength = to;
        const ee::Float zero = ee::Float(0.0f); // (the original keeps a zero for the lean's term and the damping's test)
        {
            const ee::Float ux = bodyUp.x * moved, uy = bodyUp.y * moved, uz = bodyUp.z * moved;
            point.x = point.x + ux;
            point.y = point.y + uy;
            point.z = point.z + uz;
        }
        *clearance = zero;
        collision::RayHit *const hit = physics::groundAhead(rider, wheel, &point.x, height, clearance);
        if (hit == nullptr)
        {
            setPhysicsState(rider, uint32_t(kPhysicsAir)); // no ground under the wheel: into the air
            return;
        }

        collision::hitFace(hit); // (asked for, not used)
        collision::hitPoint(hit, &groundPoint->x);
        collision::hitPlane(hit, &groundPlane->x);
        // The wheel's share of the mass (the lean's term is zero: half, either way).
        ee::Float share;
        {
            const ee::Float lean = manual ? zero : -zero;
            const ee::Float lift = wheelHeight(rider, wheel) + rider->massHeight;
            const ee::Float wheelbase = rider->frontWheelPoint[2] - rider->backWheelPoint[2];
            share = lean * (lift + pose.springLength) / wheelbase;
            share = ee::max(ee::min(share, ee::Float(0.5f)), ee::Float(-0.5f)) + ee::Float(0.5f);
            share = share * rider->mass;
        }
        *before = wheelRows;
        // The wheel's rows: up the face's normal, forward the drawn body's, square, then turned by the yaw rate.
        wheelUp = *groundPlane;
        wheelForward = bodyForward;
        wheelRight = zge::cross(wheelUp, wheelForward);
        wheelRight = zge::unit(wheelRight);
        wheelForward = zge::cross(wheelRight, wheelUp);
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, rider->yawRate);
        zge::matrixMultiply(&wheelRows, &wheelRows, turn);
        // The wheel's velocity across the face, its part along the drawn up row (for groundAhead's second result).
        *across = wheelVelocity;
        {
            const ee::Float into = zge::dot(*across, wheelUp);
            const ee::Float x = wheelUp.x * into, y = wheelUp.y * into, z = wheelUp.z * into;
            across->x = across->x - x;
            across->y = across->y - y;
            across->z = across->z - z;
        }
        const ee::Float alongUp = zge::dot(*across, bodyUp);
        // The spring: gravity across the face on the wheel's share, the pull toward the most height, over half the
        // mass; damped.
        {
            const ee::Float pull = -bodyUp.y * *g_gravityStep;
            const ee::Float got = *height;
            const ee::Float spring = (rider->wheelHeightMost - got) * rider->springStiffness;
            *clearance = alongUp;
            ee::Float springRate = pose.springRate + (share * pull + spring) / (rider->mass * ee::Float(0.5f));
            const ee::Float damping = springRate < zero ? rider->springDamping[0] : rider->springDamping[1];
            springRate = springRate * (ee::Float(1.0f) - damping);
            pose.springLength = got;
            pose.springRate = springRate;
        }
        if (groundImpact(rider, wheel, hit) == 2)
            return; // bailed
        groundHit(rider, hit);

        // The wheel's speed: turned back with the wheel, less the surface's drag, plus gravity along the forward
        // row; the pedals' push in the manual. Then the drag of both, never past zero.
        ee::Float &speed = pose.speed;
        if (zge::dot(wheelVelocity, before->row(2)) < ee::Float(0.0f))
            speed = -speed;
        {
            const ee::Float drag = (*physics::g_surfaces)[pose.sound].rollingLoss;
            const ee::Float slope = *g_gravityStep * wheelForward.y;
            speed = speed * (ee::Float(1.0f) - drag) + slope;
        }
        ee::Float slide;
        if (manual)
        {
            const ee::Float push = rider->pedalForce;
            slide = *g_wheelieSlide;
            speed = speed + push / share;
        }
        else
            slide = ee::Float(1.0f) - *g_wheelieSlide;
        {
            const ee::Float v = speed;
            const ee::Float most = rider->wheelDrag * slide / share;
            const ee::Float drag = ee::abs(v) <= most ? ee::abs(v) : most;
            speed = ee::Float(0.0f) <= v ? v - drag : v + drag;
        }
        wheelVelocity = wheelForward * speed;
        speed = ee::abs(speed);
        pose.ground = kWheelRolling;

        // The drawn body: toward the wheel's rows, leaned by the balance (about its right row), at the origin while
        // it turns.
        *rows = wheelRows;
        rows->row(3) = zge::kZero;
        if (!hasFlag(rider, kFlagBalanceLeanOff))
        {
            const ee::Float balance = rider->wheelieBalance;
            int32_t lean;
            if (manual)
                lean = -(ee::toInt(balance * kManualLean * kTurn) + kManualLeanBase);
            else
                lean = ee::toInt(-balance * *g_noseWheelieLean * kTurn) + kNoseWheelieLeanBase;
            zge::matrixIdentity(turn);
            zge::matrixRotateX(turn, turn, static_cast<zge::Angle>(lean));
            zge::matrixMultiply(rows, rows, turn);
        }
        auto blend = [](const zge::Vec4 &from, zge::Vec4 &row, ee::Float by) {
            const ee::Float x = from.x, y = from.y, z = from.z;
            const ee::Float dx = (row.x - x) * by, dy = (row.y - y) * by, dz = (row.z - z) * by;
            row.x = x + dx;
            row.y = y + dy;
            row.z = z + dz;
            row = zge::unit(row);
        };
        blend(bodyRight, rows->row(0), kRightShare);
        blend(bodyUp, rows->row(1), *g_upShare);
        rows->row(2) = zge::cross(rows->row(0), rows->row(1));
        rows->row(2) = zge::unit(rows->row(2));
        rows->row(1) = zge::cross(rows->row(2), rows->row(0));
        *matrix(rider->body) = *rows;
        *matrix(rider->right) = *matrix(rider->body);
        // Placed so the wheel's point is where it rides; the position the wheel's point.
        *across = *vec4(wheelPoint(rider, wheel));
        *across = zge::transformed(*rows, *across);
        {
            const ee::Float x = point.x - across->x, y = point.y - across->y, z = point.z - across->z;
            drawn.x = x;
            drawn.y = y;
            drawn.z = z;
        }
        *vec4(rider->position) = point;

        // The wheel's speed: none when slow, held under the top speed.
        speed = zge::vec3Length(&wheelVelocity.xyz());
        if (speed < *g_restSpeed + rider->wheelDrag * *g_stopDragShare)
        {
            wheelVelocity = zge::kZero;
            speed = ee::Float(0.0f);
        }
        else if (*g_topSpeed * rider->speedStat < speed)
        {
            wheelVelocity = zge::unit(wheelVelocity);
            wheelVelocity = wheelVelocity * (*g_topSpeed * rider->speedStat);
        }
        // The wheel's spin from its speed, backwards when it rolls back.
        const int32_t spin = static_cast<int16_t>(ee::toInt(speed * *g_spinRate * kTurn));
        pose.spin = spin;
        if (zge::dot(wheelForward, wheelVelocity) < ee::Float(0.0f))
            pose.spin = -spin;
        // The other wheel goes with it.
        WheelPose &other = rider->wheelPoses[wheel ^ 1u];
        *vec4(other.velocity) = wheelVelocity;
        other.speed = speed;

        // The rider: the wheel's velocity (the force the change), stopped when slow; fakie when moving against the
        // drawn forward row.
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &force = *vec4(rider->force);
        force = velocity;
        velocity = wheelVelocity;
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (rider->speed < *g_restSpeed + rider->wheelDrag * *g_stopDragShareBody)
        {
            velocity = zge::kZero;
            rider->speed = ee::Float(0.0f);
        }
        force = velocity - force;
        if (zge::dot(velocity, bodyForward) < ee::Float(0.0f))
            setFlag(rider, kFlagFakie);
        else
            clearFlag(rider, kFlagFakie);
    }
    READABLE_FUNCTION(wheelieMove, Verified);
}
