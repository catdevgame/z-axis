// BMX XXX's grind, stall and lip trick action states (action.c) and the
// grind's sub-state. DM2's (games/dm2/rider/actions/states.cpp), with BMX
// XXX's motions by name and its own grind balance.

#include "../rider.h"
#include "../vector.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/player_riders.h"
#include "bmx/game/score.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/grind.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The tricks' names for their motions: the grinds' (tricks 0x33 ..
        // 0x3B: "doublePeg", "smith", "feeble" ..) and the stalls' and
        // lips' (0x3C on: "doublePeg", "toothpick", "icepick" ..). Leaving
        // the rail plays "grind.NAME.end" or "stall.NAME.end".
        constexpr uint8_t kGrindTrickFirst = 0x33;
        constexpr uint8_t kStallTrickFirst = 0x3C;
        inline ee::Global<uint32_t[10], sym::g_grindTrickNames> g_grindTrickNames;
        inline ee::Global<uint32_t[21], sym::g_stallTrickNames> g_stallTrickNames;
        constexpr uint8_t kLandingOffRail = 5;  // the landing kind coming down from a grind or stall

        using SubState = ee::FunctionPtr<int32_t(Rider *)>;
        inline ee::Global<SubState[2], sym::g_grindSubStates> g_grindSubStates;
        inline ee::Global<SubState[2], sym::g_stallSubStates> g_stallSubStates;

        // The air's sub-state 3's phases the rail's ways off start it in.
        constexpr uint32_t kOffGrindFell = 0;
        constexpr uint32_t kOffGrind = 3;
        constexpr uint32_t kOffLip = 2;
        constexpr uint32_t kOffStallFell = 1;
        constexpr uint32_t kOffStall = 4;
        constexpr uint32_t kOffLipHigh = 5; // the hop off a lip at 85 degrees (the stall's sub-state)

        // The motion started over, the rider off the rail.
        void leaveRail(Rider *rider)
        {
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            setFlag(rider, kFlagLeftRail);
        }
    }

    // Action state 2, the grind: off the rail into the air (or physics
    // state 12) with the trick's end motion, in the air's sub-state 3 (its
    // phase by whether the rider fell off the rail's side); on the ground
    // the landing, of kind 5; a bail; else the grind's sub-state. DM2's
    // grindState, which goes from the ground to riding.
    int32_t grindState(Rider *rider)
    {
        ee::Local<char[0x40]> name;
        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsAir || physics == kPhysicsTumble)
        {
            zge::formatString(name.address(), sym::textGrindEndFormat, (*g_grindTrickNames)[rider->trick - kGrindTrickFirst]);
            riderMotionByName(name.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            setActionState(rider, uint32_t(kLevelPhase), hasFlag(rider, kFlagFellOff) ? kOffGrindFell : kOffGrind);
            leaveRail(rider);
            return 0;
        }
        if (physics == kPhysicsGround)
        {
            rider->landingKind = kLandingOffRail;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        }
        if (physics == kPhysicsBail)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }
        for (;;)
        {
            const int32_t kept = (*g_grindSubStates)[rider->actionStates[kLevelSubState]](rider);
            if (kept != 0 || actionState(rider, uint32_t(kLevelAction)) != kActionGrind)
                return kept;
        }
    }
    READABLE_FUNCTION(grindState, Verified);

    // Action states 3 and 4, the stall and the lip trick: as the grind's,
    // with the stalls' names; the air's sub-state 3 starts in phase 2 off
    // a lip trick, 1 fallen off a stall, 4 else. The sub-states run again
    // only while a stall. DM2's stallState.
    int32_t stallState(Rider *rider)
    {
        ee::Local<char[0x40]> name;
        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsAir || physics == kPhysicsTumble)
        {
            zge::formatString(name.address(), sym::textStallEndFormat, (*g_stallTrickNames)[rider->trick - kStallTrickFirst]);
            riderMotionByName(name.address(), &rider->motionName, &rider->motion);
            const bool lipTrick = actionState(rider, uint32_t(kLevelAction)) == kActionLipTrick;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            if (lipTrick)
                setActionState(rider, uint32_t(kLevelPhase), kOffLip);
            else
                setActionState(rider, uint32_t(kLevelPhase), hasFlag(rider, kFlagFellOff) ? kOffStallFell : kOffStall);
            leaveRail(rider);
            return 0;
        }
        if (physics == kPhysicsGround)
        {
            rider->landingKind = kLandingOffRail;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        }
        if (physics == kPhysicsBail)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }
        for (;;)
        {
            const int32_t kept = (*g_stallSubStates)[rider->actionStates[kLevelSubState]](rider);
            if (kept != 0 || actionState(rider, uint32_t(kLevelAction)) != kActionStall)
                return kept;
        }
    }
    READABLE_FUNCTION(stallState, Verified);

    namespace
    {
        // The grind's sub-state's phases: onto the rail (the snap's frames), the snap running until the grind
        // physics, the grind's own motions, on the rail, the trick on top (in, held; out), the crouch.
        enum GrindPhase : uint8_t
        {
            kGrindOnto = 0,
            kGrindLanding = 1,
            kGrindStart = 2,
            kGrindOn = 3,
            kGrindTrickHeld = 4,
            kGrindTrickOut = 5,
            kGrindCrouched = 6,
        };
        constexpr uint32_t kLevelStep = 3; // its frames: the stick's push kept (as the air's)
        constexpr uint32_t kMoveTrickB = 4;
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMovePressed = 0;
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kMoveReleased = 2;
        // The motions by the grind trick's name (the formats "grind.%s.start", ".loopStart",
        // ".loop", ".wobble.r", ".wobble.l", ".end") and the fall's ("fall.grind.start").
        // The camera's mode at level 1 the grind asks for once on the rail.
        constexpr uint32_t kCameraActionLevel = 1;
        constexpr uint32_t kCameraGrindMode = 1;
        // A trick's kinds (a word a trick): the balance left alone; the stick's y leans it (the tricks whose rows
        // are made from the right row, rails.cpp); no lean motions.
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickKinds;
        constexpr uint32_t kTrickNoBalance = 0x200u;
        constexpr uint32_t kTrickRowsByRight = 0x80u;
        constexpr uint32_t kTrickNoLean = 0x400u;
        // The tricks on top (a row of 0x56 a trick, by trickFromTaps' index), kNoTrick for none; a trick's
        // flags (kTrickCountsHeld: its loops counted while held). In by 0.05 a frame, out by a fifteenth.
        constexpr uint32_t kTopTrickRow = 0x56;
        inline ee::Global<uint8_t[256][kTopTrickRow], sym::grindTopTricks> g_topTricks;
        inline ee::Global<uint16_t[256], sym::g_trickFlags> g_trickFlags;
        constexpr uint16_t kTrickCountsHeld = 0x20;
        constexpr uint32_t kTopTrickCountBase = 15;
        constexpr int32_t kTopTrickCountSpread = 225;
        inline ee::Global<ee::Float, sym::g_grindTopBlendIn> g_grindTopBlendIn;
        inline ee::Global<ee::Float, sym::g_grindTopBlendOut> g_grindTopBlendOut;
        // The crouch let go: the hop up (as the crouch's, by the frames crouched), or with the stick to a side
        // off the rail's side (0.85 of the air's hop, 30 degrees off level), turning the rider over the snap's
        // frames.
        inline ee::Global<ee::Float, sym::g_airHopImpulse> g_airHopImpulse;
        inline ee::Global<ee::Float, sym::g_hopPerStat> g_hopPerStat;
        inline ee::Global<ee::Float, sym::g_hopLeast> g_hopLeast;
        inline ee::Global<uint32_t, sym::g_hopFullFrames> g_hopFullFrames;
        inline ee::Global<uint32_t, sym::g_airStickHoldFrames> g_stickHoldFrames;
        inline ee::Global<ee::Float, sym::g_grindSideHopShare> g_grindSideHopShare;
        constexpr ee::Float kStickPushed(0.5f);
        constexpr zge::Angle kSideHopAngle = 0x1555;
        constexpr int16_t kSideHopTurn = 0x111;
        constexpr int16_t kSideHopSnapFrames = 10;
        // The balance, after the action's first 7 frames: it tips the way it leans by 0.0125 a frame (a quarter
        // more at a stop, less with speed up to 1), less with the balance stat, more over the balance frames (as
        // the wheelie's) and by 0.0005 for each line of the trick line; at 1 either way the rider falls off.
        // The stick leans it by 0.03 times the manual stat. It is never nearer 0 than 0.001.
        constexpr uint16_t kBalanceFromFrames = 7;
        inline ee::Global<ee::Float, sym::g_grindSlowSpeed> g_grindSlowSpeed;
        inline ee::Global<ee::Float, sym::g_grindSlowTip> g_grindSlowTip;
        inline ee::Global<ee::Float, sym::g_grindTip> g_grindTip;
        inline ee::Global<uint32_t, sym::g_flatlandTipFrames> g_balanceTipFrames;
        inline ee::Global<ee::Float, sym::g_flatlandTipGrowth> g_balanceTipGrowth;
        inline ee::Global<ee::Float, sym::g_grindLineTip> g_grindLineTip;
        inline ee::Global<ee::Float, sym::g_grindStickLean> g_grindStickLean;
        inline ee::Global<ee::Float, sym::g_grindBalanceLeast> g_grindBalanceLeast;
        inline ee::Global<ee::Float, sym::g_grindBalanceLeastBack> g_grindBalanceLeastBack;
    }

    // The grind's sub-state 0. By phase: onto the rail (the grind trick by the stick; with a snap its start
    // motion at 16 / the snap's frames), the snap until the grind physics, the grind's motions (the loop's start,
    // the two wobbles), on the rail: the stick's push kept a moment, the crouch held to the crouch's phase, trick
    // button B with the taps for a trick on top (it fades in while held, its loops counted, and out once let go),
    // crouched: let go, the hop off (to the side with the stick: off the rail's side) into the air's sub-state
    // 3. Then the balance (above) and its lean motions' blend, the loop's motion again at each end, the
    // frames. Returns 0 the frame the state changes. DM2's grindSubState with the balance of BMX XXX.
    int32_t grindSubState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ee::Local<char[0x80]> text;
        ButtonRecord *const input = &rider->input;
        // The grind trick's name, for its motions' formats.
        auto trickName = [rider]() { return (*g_grindTrickNames)[rider->trick - kGrindTrickFirst]; };
        // The shared end: the frames counted (the trick on top's while it shows), 1.
        auto counted = [rider]() {
            if (!(rider->topTrickBlend == ee::Float(0.0f)))
                ++rider->topTrickFrames;
            ++rider->actionFrames[kLevelSubState];
            ++rider->actionFrames[kLevelPhase];
            return 1;
        };
        auto changed = [rider](uint32_t level, uint32_t state) {
            setActionState(rider, level, state);
            return 0;
        };

        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kGrindLanding)
        {
            if (physicsState(rider) == kPhysicsGrind)
                return changed(uint32_t(kLevelPhase), kGrindStart);
            return counted();
        }
        if (phase == kGrindOnto)
        {
            rider->trick = grindTrick(rider);
            if (rider->snapFrames != 0)
            {
                zge::formatString(text.address(), sym::textGrindStartFormat, trickName());
                riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                if (hasFlag(rider, kFlagMirrorSide))
                    setFlag(rider, kFlagMirrored);
                rider->motionRate = ee::Float(16.0f) / ee::toFloat(int32_t(static_cast<uint16_t>(rider->snapFrames)));
            }
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindLanding));
            clearFlag(rider, kFlagGrindPressed);
            return 0;
        }
        if (phase == kGrindStart)
        {
            zge::formatString(text.address(), sym::textGrindLoopStartFormat, trickName());
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            zge::formatString(text.address(), sym::textGrindWobbleRightFormat, trickName());
            riderMotionByName(text.address(), nullptr, &rider->leanMotions[0]);
            zge::formatString(text.address(), sym::textGrindWobbleLeftFormat, trickName());
            riderMotionByName(text.address(), nullptr, &rider->leanMotions[1]);
            rider->leanBlend[0] = 0.0f;
            rider->motionFlags |= kMotionRestart | kMotionLean;
            rider->leanBlend[1] = 0.0f;
            if (hasFlag(rider, kFlagMirrorSide))
                setFlag(rider, kFlagMirrored);
            rider->motionRate = ee::Float(1.0f);
            if (!hasFlag(rider, kFlagNoPlayer))
                trickLineEvent(uint32_t(rider->player), 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindOn));
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, kCameraGrindMode);
            return counted();
        }

        // On the rail. The stick's push past half kept a moment (as the air's).
        const uint16_t held = ++rider->actionFrames[kLevelStep];
        if (*g_stickHoldFrames < held)
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
        if (actionState(rider, uint32_t(kLevelPhase)) < kGrindCrouched &&
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
        {
            rider->airStick[0] = 0.0f;
            rider->airStick[1] = 0.0f;
            return changed(uint32_t(kLevelPhase), kGrindCrouched);
        }
        // The trick on top's blend going out (to none: the flag off).
        auto topTrickOut = [rider]() {
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_grindTopBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                clearFlag(rider, kFlagTopTrick);
        };
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kGrindOn:
        {
            uint8_t taps;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
            {
                if (!hasFlag(rider, kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                    break;
            }
            else
                taps = rider->tapCount;
            if (taps == 0)
            {
                setFlag(rider, kFlagTrickBWaiting);
                break;
            }
            const uint8_t tapIndex = trickFromTaps(rider);
            const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
            if (top == kNoTrick)
            {
                clearTaps(rider, 1u);
                break;
            }
            setFlag(rider, kFlagTopTrick);
            rider->trickMotion = top;
            rider->topTrickBlend = 0.0f;
            rider->topTrickLoopFrames = static_cast<uint8_t>(
                ee::toInt((ee::Float(1.0f) - rider->techniqueStat) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            return changed(uint32_t(kLevelPhase), kGrindTrickHeld);
        }
        case kGrindTrickHeld:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
                return changed(uint32_t(kLevelPhase), kGrindTrickOut);
            if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
            {
                const uint8_t every = rider->topTrickLoopFrames;
                if (every != 0 && rider->actionFrames[kLevelSubState] % every == 0) // (a zero is the original's trap; never)
                    ++rider->trickLoops;
            }
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_grindTopBlendIn, ee::Float(1.0f));
            break;
        case kGrindTrickOut:
            topTrickOut();
            break;
        case kGrindCrouched:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) == 0)
            {
                topTrickOut();
                break;
            }
            // The hop off.
            const ee::Float least = *g_hopLeast;
            const uint32_t full = *g_hopFullFrames;
            const uint32_t frames = rider->actionFrames[kLevelPhase];
            const uint32_t crouched = frames < full ? frames : full;
            ee::Float hop = (*g_airHopImpulse * rider->airStat - least) * zge::unsignedToFloat(crouched) / zge::unsignedToFloat(full) + least;
            *impulse = zge::kZero;
            const bool toSide = kStickPushed < ee::abs(rider->airStick[0]);
            if (toSide)
            {
                // Across the rail, away from the stick's side and along the way the rider goes.
                *impulse = *vec4(rider->snapRows);
                impulse->y = 0.0f;
                *impulse = zge::unit(*impulse);
                if (ee::Float(0.0f) <= rider->airStick[0])
                    zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                if (zge::dot(*vec4(&rider->snapRows[8]), *vec4(rider->velocity)) < ee::Float(0.0f))
                    zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                hop = *g_airHopImpulse * *g_grindSideHopShare;
                *impulse = *impulse * (hop * zge::angleSin(kSideHopAngle));
                impulse->y = impulse->y - hop * zge::angleCos(kSideHopAngle);
            }
            else
                impulse->y = impulse->y - hop;
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            if (kStickPushed < ee::abs(rider->airStick[0]))
            {
                *matrix(rider->alignFrom) = *matrix(rider->right);
                rider->alignAngles[1] = ee::Float(0.0f) <= rider->airStick[0] ? int16_t(-kSideHopTurn) : kSideHopTurn;
                rider->snapFrames = kSideHopSnapFrames;
            }
            zge::formatString(text.address(), sym::textGrindEndFormat, trickName());
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            setActionState(rider, uint32_t(kLevelPhase), kOffGrind);
            leaveRail(rider);
            return 0;
        }
        default:
            break;
        }

        // The balance.
        const uint32_t kinds = (*g_trickKinds)[rider->trick];
        if (!(kinds & kTrickNoBalance))
        {
            if (rider->actionFrames[kLevelAction] >= kBalanceFromFrames)
            {
                ee::Float tip = 1.0f;
                const ee::Float slow = *g_grindSlowSpeed;
                if (rider->speed < slow)
                    tip = (slow - rider->speed) * (*g_grindSlowTip / slow) + tip;
                tip = tip * *g_grindTip;
                tip = tip * (ee::Float(1.0f) - rider->balanceStat);
                const uint32_t most = *g_balanceTipFrames;
                const uint32_t ridden = uint32_t(rider->balanceFrames) < most ? uint32_t(rider->balanceFrames) : most;
                tip = tip + ee::toFloat(int32_t(ridden)) * (*g_balanceTipGrowth / ee::toFloat(int32_t(most)));
                tip = tip + ee::toFloat(int32_t(rider->score.lineCount)) * *g_grindLineTip;
                const ee::Float side = ee::Float(0.0f) < rider->grindBalance ? ee::Float(1.0f) : ee::Float(-1.0f);
                rider->grindBalance = rider->grindBalance + tip * side;
                if (ee::Float(1.0f) <= ee::abs(rider->grindBalance))
                {
                    // Tipped over: off the rail.
                    fallOffRail(rider);
                    switch (physicsState(rider))
                    {
                    case kPhysicsAir:
                        zge::formatString(text.address(), sym::textGrindEndFormat, trickName());
                        riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                        if (hasFlag(rider, kFlagFellOff))
                        {
                            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
                            setActionState(rider, uint32_t(kLevelPhase), kOffGrindFell);
                        }
                        leaveRail(rider);
                        return 0;
                    case kPhysicsGround:
                        rider->landingKind = kLandingOffRail;
                        return changed(uint32_t(kLevelAction), uint32_t(kActionLanding));
                    case kPhysicsTumble:
                        zge::formatString(text.address(), sym::textFallGrindStart);
                        riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                        return changed(uint32_t(kLevelAction), uint32_t(kActionBail));
                    default:
                        break;
                    }
                }
                // The stick's lean (its y for the tricks with rows by the right row).
                ee::Float lean = (kinds & kTrickRowsByRight) ? -inputStick(input, 0u)->y : inputStick(input, 0u)->x;
                if (hasFlag(rider, kFlagBalanceTurnedRound))
                    lean = -lean;
                rider->grindBalance = rider->grindBalance + lean * (*g_grindStickLean * rider->manualStat);
            }
            const ee::Float balance = rider->grindBalance;
            if (ee::abs(balance) < *g_grindBalanceLeast)
                rider->grindBalance = ee::Float(0.0f) <= balance ? *g_grindBalanceLeast : *g_grindBalanceLeastBack;
        }
        // The lean motions' blend: the balance's positive part to the first, its negative part to the second
        // (swapped on the mirrored side); none for the tricks without.
        if (kinds & kTrickNoLean)
        {
            rider->leanBlend[0] = 0.0f;
            rider->leanBlend[1] = 0.0f;
        }
        else
        {
            const ee::Float balance = rider->grindBalance;
            const ee::Float positive = ee::max(balance, ee::Float(0.0f));
            const ee::Float negative = balance < ee::Float(0.0f) ? -balance : ee::Float(0.0f);
            const bool mirrored = hasFlag(rider, kFlagMirrorSide);
            rider->leanBlend[0] = mirrored ? negative : positive;
            rider->leanBlend[1] = mirrored ? positive : negative;
        }
        if (hasFlag(rider, kFlagMotionEnded))
        {
            zge::formatString(text.address(), sym::textGrindLoopFormat, trickName());
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
        }
        rider->motionFlags |= kMotionLean;
        return counted();
    }
    READABLE_FUNCTION(grindSubState, Verified);

    namespace
    {
        // The stall's sub-state's phases: the grind's, then the hop off and the wait of the tricks that hop
        // forward.
        constexpr uint8_t kStallHop = 7;
        constexpr uint8_t kStallHopWait = 8;
        constexpr uint32_t kMoveStall = 8;
        // A trick's kinds: the hop off is forward at 45 degrees after a wait (kStallHopWait); the motion's end
        // hops off (else it loops).
        constexpr uint32_t kTrickHopsForward = 0x800u;
        constexpr uint32_t kTrickHopAtEnd = 0x20u;
        // The stall tricks with the camera's stall modes (below this trick: lip tricks have none).
        constexpr uint8_t kStallCameraTricksEnd = 0x43;
        constexpr uint32_t kCameraStallMode = 1, kCameraStallByRightMode = 2;
        inline ee::Global<ee::Float, sym::g_stallTopBlendIn> g_stallTopBlendIn;
        inline ee::Global<ee::Float, sym::g_stallTopBlendOut> g_stallTopBlendOut;
        // The hops: to a side (0.85 of the air's hop, 30 degrees off level, the rider turned 4 degrees a frame
        // over 15 frames); forward (0.65 of the per-stat hop, a quarter more with the crouch pressed in the wait,
        // after 11 frames of it); off a lip, ahead and up at 85 degrees, or 60 with the stick to the lip's side.
        inline ee::Global<ee::Float, sym::g_stallSideHopShare> g_stallSideHopShare;
        inline ee::Global<ee::Float, sym::g_stallForwardHopShare> g_stallForwardHopShare;
        constexpr ee::Float kForwardHopCrouched(1.25f);
        constexpr zge::Angle kForwardHopAngle = 0x2000;
        constexpr uint16_t kHopWaitFrames = 11;
        constexpr int16_t kStallHopTurn = 0x2D8;
        constexpr int16_t kStallHopSnapFrames = 15;
        constexpr zge::Angle kLipHopAngle = 0x3C71;
        constexpr zge::Angle kLipHopSideAngle = 0x2AAA;
        // The balance as the grind's, from the action's 10th frame and twice as fast, with no trick line's part.
        constexpr uint16_t kStallBalanceFromFrames = 10;
        inline ee::Global<ee::Float, sym::g_stallBalanceLeast> g_stallBalanceLeast;
        inline ee::Global<ee::Float, sym::g_stallBalanceLeastBack> g_stallBalanceLeastBack;
    }

    // The stall's and lip trick's sub-state 0. The stall button or the crouch let go lets go of the trick. By
    // phase: onto the target (with a snap the start motion at 16 / the snap's frames; the mirror by the side),
    // until the stall or lip physics, the stall's motions (the loop, the two wobbles) and the camera's stall
    // mode; then (a trick that hops forward goes to its wait at once) the stick's push kept (x and y swapped for
    // the tricks with rows by the right row), the crouch pressed to the crouch's phase, the trick on top as the
    // grind's, crouched: let go, the hop; the hop off: forward, off a lip (ahead and up), or to a side (by the
    // stick, else away from the rider's up) into the air's sub-state 3; the wait: 11 frames. Then the balance
    // (above), the lean motions' blend and the motion's end (a hop for the tricks that hop at it). Returns 0 the
    // frame the state changes, and always on the frame the stall's motions start. DM2's stallSubState with BMX
    // XXX's own hops and balance.
    int32_t stallSubState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ee::Local<char[0x80]> text;
        ButtonRecord *const input = &rider->input;
        auto trickName = [rider]() { return (*g_stallTrickNames)[rider->trick - kStallTrickFirst]; };
        auto counted = [rider]() {
            if (!(rider->topTrickBlend == ee::Float(0.0f)))
                ++rider->topTrickFrames;
            ++rider->actionFrames[kLevelSubState];
            ++rider->actionFrames[kLevelPhase];
            return 1;
        };
        auto changed = [rider](uint32_t level, uint32_t state) {
            setActionState(rider, level, state);
            return 0;
        };
        // The phase changed with a level's frames kept.
        auto changedKeeping = [rider](uint32_t state, uint32_t level) {
            const uint16_t frames = rider->actionFrames[level];
            setActionState(rider, uint32_t(kLevelPhase), state);
            rider->actionFrames[level] = frames;
            return 0;
        };

        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveStall, kMoveReleased) != 0 ||
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kGrindLanding)
        {
            const uint8_t physics = physicsState(rider);
            if (physics == kPhysicsStall || physics == kPhysicsLipTrick)
                return changed(uint32_t(kLevelPhase), kGrindStart);
            return counted();
        }
        if (phase == kGrindOnto)
        {
            if (rider->snapFrames != 0)
            {
                zge::formatString(text.address(), sym::textStallStartFormat, trickName());
                riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                if (hasFlag(rider, kFlagMirrorSide))
                    setFlag(rider, kFlagMirrored);
                else
                    clearFlag(rider, kFlagMirrored);
                rider->motionRate = ee::Float(16.0f) / ee::toFloat(int32_t(static_cast<uint16_t>(rider->snapFrames)));
            }
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindLanding));
            setFlag(rider, kFlagTrickHeld);
            clearFlag(rider, kFlagGrindPressed);
            return 0;
        }
        if (phase == kGrindStart)
        {
            zge::formatString(text.address(), sym::textStallLoopFormat, trickName());
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            zge::formatString(text.address(), sym::textStallWobbleRightFormat, trickName());
            riderMotionByName(text.address(), nullptr, &rider->leanMotions[0]);
            zge::formatString(text.address(), sym::textStallWobbleLeftFormat, trickName());
            riderMotionByName(text.address(), nullptr, &rider->leanMotions[1]);
            rider->leanBlend[0] = 0.0f;
            rider->motionFlags |= kMotionRestart | kMotionLean;
            rider->leanBlend[1] = 0.0f;
            if (hasFlag(rider, kFlagMirrorSide))
                setFlag(rider, kFlagMirrored);
            rider->motionRate = ee::Float(1.0f);
            if (!hasFlag(rider, kFlagNoPlayer))
                trickLineEvent(uint32_t(rider->player), 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindOn));
            if (rider->trick < kStallCameraTricksEnd && !hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
            {
                const uint32_t mode = ((*g_trickKinds)[rider->trick] & kTrickRowsByRight) ? kCameraStallByRightMode : kCameraStallMode;
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, mode);
            }
            return 0;
        }

        if (((*g_trickKinds)[rider->trick] & kTrickHopsForward) && actionState(rider, uint32_t(kLevelPhase)) < kGrindCrouched)
            return changed(uint32_t(kLevelPhase), kStallHopWait);
        // The stick's push past half kept a moment (its x and y swapped for the tricks with rows by the right row).
        const uint16_t held = ++rider->actionFrames[kLevelStep];
        if (*g_stickHoldFrames < held)
        {
            rider->airStick[0] = 0.0f;
            rider->airStick[1] = 0.0f;
            rider->actionFrames[kLevelStep] = 0;
        }
        const bool swapped = ((*g_trickKinds)[rider->trick] & kTrickRowsByRight) != 0;
        const ee::Float across = swapped ? inputStick(input, 0u)->y : inputStick(input, 0u)->x;
        if (kStickPushed < ee::abs(across))
        {
            rider->airStick[0] = across;
            rider->actionFrames[kLevelStep] = 0;
        }
        const bool swappedAgain = ((*g_trickKinds)[rider->trick] & kTrickRowsByRight) != 0;
        const ee::Float along = swappedAgain ? inputStick(input, 0u)->x : inputStick(input, 0u)->y;
        if (kStickPushed < ee::abs(along))
        {
            rider->airStick[1] = along;
            rider->actionFrames[kLevelStep] = 0;
        }
        if (actionState(rider, uint32_t(kLevelPhase)) < kGrindCrouched &&
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMovePressed) != 0)
        {
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindCrouched));
            setFlag(rider, kFlagTrickHeld);
            return 0;
        }
        auto topTrickOut = [rider]() {
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_stallTopBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                clearFlag(rider, kFlagTopTrick);
        };
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kGrindOn:
        {
            uint8_t taps;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
            {
                if (!hasFlag(rider, kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                    break;
            }
            else
                taps = rider->tapCount;
            if (taps == 0)
            {
                setFlag(rider, kFlagTrickBWaiting);
                break;
            }
            const uint8_t tapIndex = trickFromTaps(rider);
            const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
            if (top == kNoTrick)
            {
                clearTaps(rider, 1u);
                break;
            }
            setFlag(rider, kFlagTopTrick);
            rider->trickMotion = top;
            rider->topTrickBlend = 0.0f;
            rider->topTrickLoopFrames = static_cast<uint8_t>(
                ee::toInt((ee::Float(1.0f) - rider->techniqueStat) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            return changed(uint32_t(kLevelPhase), kGrindTrickHeld);
        }
        case kGrindTrickHeld:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
                return changed(uint32_t(kLevelPhase), kGrindTrickOut);
            if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
            {
                const uint8_t every = rider->topTrickLoopFrames;
                if (every != 0 && rider->actionFrames[kLevelSubState] % every == 0) // (a zero is the original's trap; never)
                    ++rider->trickLoops;
            }
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_stallTopBlendIn, ee::Float(1.0f));
            break;
        case kGrindCrouched:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
                return changedKeeping(kStallHop, uint32_t(kLevelPhase));
            topTrickOut();
            break;
        case kGrindTrickOut:
            topTrickOut();
            break;
        case kStallHop:
        {
            ee::Float turn = 0.0f;
            bool offLipHigh = false;
            ee::Float hop;
            if ((*g_trickKinds)[rider->trick] & kTrickHopsForward)
            {
                // Forward and up at 45 degrees.
                *impulse = *vec4(rider->forward);
                impulse->y = turn;
                *impulse = zge::unit(*impulse);
                *impulse = *impulse * zge::angleCos(kForwardHopAngle);
                impulse->y = -zge::angleSin(kForwardHopAngle);
                hop = *g_hopPerStat * *g_stallForwardHopShare;
                if (rider->actionFrames[kLevelStep] != 0)
                    hop = hop * kForwardHopCrouched;
                *impulse = *impulse * hop;
            }
            else
            {
                const ee::Float least = *g_hopLeast;
                const uint32_t full = *g_hopFullFrames;
                const uint32_t frames = rider->actionFrames[kLevelPhase];
                const uint32_t crouched = frames < full ? frames : full;
                hop = (*g_hopPerStat * rider->airStat - least) * zge::unsignedToFloat(crouched) / zge::unsignedToFloat(full) + least;
                if (actionState(rider, uint32_t(kLevelAction)) == kActionLipTrick)
                {
                    // Off a lip: ahead and up by the rows, 85 degrees (60 with the stick to the lip's side).
                    const ee::Float side = rider->airStick[0];
                    const bool mirrored = hasFlag(rider, kFlagMirrorSide);
                    const bool toLipSide = kStickPushed < ee::abs(side) && (ee::Float(0.0f) < side ? mirrored : !mirrored);
                    const zge::Angle angle = toLipSide ? kLipHopSideAngle : kLipHopAngle;
                    offLipHigh = !toLipSide;
                    const ee::Float cosine = zge::angleCos(angle);
                    const ee::Float ahead = cosine * (ee::Float(1.0f) / zge::angleSin(angle));
                    impulse->x = ahead;
                    if (hasFlag(rider, kFlagMirrorSide))
                        impulse->x = -impulse->x;
                    impulse->z = 0.0f;
                    impulse->y = 1.0f;
                    *impulse = zge::transformed(*matrix(rider->right), *impulse);
                    *impulse = *impulse * hop;
                }
                else
                {
                    // To a side: by the stick (its y across the rail's ahead row, its x along the first; the rows
                    // the other way for the tricks with rows by the right row), else off the rider's up side;
                    // away from the stick.
                    const bool byRight = ((*g_trickKinds)[rider->trick] & kTrickRowsByRight) != 0;
                    *impulse = zge::kZero;
                    if (kStickPushed < ee::abs(rider->airStick[1]))
                    {
                        *impulse = *vec4(byRight ? &rider->snapRows[0] : &rider->snapRows[8]);
                        impulse->y = 0.0f;
                        *impulse = zge::unit(*impulse);
                        if (ee::Float(0.0f) <= rider->airStick[1])
                            zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                    }
                    else if (kStickPushed < ee::abs(rider->airStick[0]))
                    {
                        *impulse = *vec4(byRight ? &rider->snapRows[8] : &rider->snapRows[0]);
                        if (!byRight)
                            turn = rider->airStick[0];
                        impulse->y = 0.0f;
                        *impulse = zge::unit(*impulse);
                        if (ee::Float(0.0f) <= rider->airStick[0])
                            zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                    }
                    else
                    {
                        *impulse = *vec4(byRight ? &rider->snapRows[8] : &rider->snapRows[0]);
                        if (!byRight)
                            turn = -1.0f;
                        if (zge::dot(*vec4(rider->up), *impulse) < ee::Float(0.0f))
                        {
                            turn = -turn;
                            zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                        }
                        impulse->y = 0.0f;
                        *impulse = zge::unit(*impulse);
                    }
                    hop = *g_airHopImpulse * *g_stallSideHopShare;
                    *impulse = *impulse * (hop * zge::angleSin(kSideHopAngle));
                    impulse->y = impulse->y - hop * zge::angleCos(kSideHopAngle);
                }
            }
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            if (kStickPushed < ee::abs(turn))
            {
                *matrix(rider->alignFrom) = *matrix(rider->right);
                rider->alignAngles[1] = ee::Float(0.0f) <= turn ? int16_t(-kStallHopTurn) : kStallHopTurn;
                rider->snapFrames = kStallHopSnapFrames;
            }
            zge::formatString(text.address(), sym::textStallEndFormat, trickName());
            riderMotionByName(text.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            setActionState(rider, uint32_t(kLevelPhase), offLipHigh ? kOffLipHigh : kOffStall);
            leaveRail(rider);
            return 0;
        }
        case kStallHopWait:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMovePressed) != 0)
                rider->actionFrames[kLevelStep] = 1;
            if (rider->actionFrames[kLevelPhase] >= kHopWaitFrames)
                return changedKeeping(kStallHop, uint32_t(kLevelStep));
            break;
        default:
            break;
        }

        // The balance.
        if (!((*g_trickKinds)[rider->trick] & kTrickNoBalance))
        {
            if (rider->actionFrames[kLevelAction] >= kStallBalanceFromFrames)
            {
                ee::Float tip = 1.0f;
                const ee::Float slow = *g_grindSlowSpeed;
                if (rider->speed < slow)
                    tip = (slow - rider->speed) * (*g_grindSlowTip / slow) + tip;
                tip = tip * *g_grindTip;
                tip = tip * (ee::Float(1.0f) - rider->balanceStat);
                const uint32_t most = *g_balanceTipFrames;
                const uint32_t ridden = uint32_t(rider->balanceFrames) < most ? uint32_t(rider->balanceFrames) : most;
                tip = tip + ee::toFloat(int32_t(ridden)) * (*g_balanceTipGrowth / ee::toFloat(int32_t(most)));
                const ee::Float side = ee::Float(0.0f) < rider->grindBalance ? ee::Float(2.0f) : ee::Float(-2.0f);
                rider->grindBalance = rider->grindBalance + tip * side;
                if (ee::Float(1.0f) <= ee::abs(rider->grindBalance))
                {
                    fallOffRail(rider);
                    switch (physicsState(rider))
                    {
                    case kPhysicsAir:
                        zge::formatString(text.address(), sym::textStallEndFormat, trickName());
                        riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                        // (the action state just set is the air's: the lip trick's test never holds)
                        if (hasFlag(rider, kFlagFellOff) || actionState(rider, uint32_t(kLevelAction)) == kActionLipTrick)
                        {
                            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
                            setActionState(rider, uint32_t(kLevelPhase), kOffGrindFell);
                        }
                        leaveRail(rider);
                        return 0;
                    case kPhysicsGround:
                        rider->landingKind = kLandingOffRail;
                        return changed(uint32_t(kLevelAction), uint32_t(kActionLanding));
                    case kPhysicsTumble:
                        zge::formatString(text.address(), sym::textFallGrindStart);
                        riderMotionByName(text.address(), &rider->motionName, &rider->motion);
                        return changed(uint32_t(kLevelAction), uint32_t(kActionBail));
                    default:
                        break;
                    }
                }
                ee::Float lean = ((*g_trickKinds)[rider->trick] & kTrickRowsByRight) ? -inputStick(input, 0u)->y : inputStick(input, 0u)->x;
                if (hasFlag(rider, kFlagBalanceTurnedRound))
                    lean = -lean;
                rider->grindBalance = rider->grindBalance + lean * (*g_grindStickLean * rider->manualStat);
            }
            const ee::Float balance = rider->grindBalance;
            if (ee::abs(balance) < *g_stallBalanceLeast)
                rider->grindBalance = ee::Float(0.0f) <= balance ? *g_stallBalanceLeast : *g_stallBalanceLeastBack;
        }
        if ((*g_trickKinds)[rider->trick] & kTrickNoLean)
        {
            rider->leanBlend[0] = 0.0f;
            rider->leanBlend[1] = 0.0f;
        }
        else
        {
            const ee::Float balance = rider->grindBalance;
            const ee::Float positive = ee::max(balance, ee::Float(0.0f));
            const ee::Float negative = balance < ee::Float(0.0f) ? -balance : ee::Float(0.0f);
            const bool mirrored = hasFlag(rider, kFlagMirrorSide);
            rider->leanBlend[0] = mirrored ? negative : positive;
            rider->leanBlend[1] = mirrored ? positive : negative;
            rider->motionFlags |= kMotionLean;
        }
        if (hasFlag(rider, kFlagMotionEnded))
        {
            if ((*g_trickKinds)[rider->trick] & kTrickHopAtEnd)
                return changed(uint32_t(kLevelPhase), kStallHop);
            rider->motionFlags |= kMotionRestart;
        }
        return counted();
    }
    READABLE_FUNCTION(stallSubState, Verified);
}
