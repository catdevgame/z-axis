// BMX XXX's flatland tricks (action.c): action state 6, tricks on one
// wheel entered from the manual (wheelieState) with the trick button and
// the stick's taps, or from standing still (coastState). BMX XXX's own;
// named by its sub-states' motions ("flatland.%s.start", "flatland.%s.loop",
// "flatland.swivel.*").

#include "../rider.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/actions/flatland.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        constexpr uint32_t kAirCoastStart = sym::textAirCoastStart; // "air.coast.start"

        // The flatland's sub-states: the manual's table of sub-states goes
        // on into this one (the swivel, then the trick's start and loop).
        using SubState = ee::FunctionPtr<int32_t(Rider *)>;
        inline ee::Global<SubState[3], sym::g_flatlandSubStates> g_flatlandSubStates;

        // Steering: 15 times the stick a frame toward 75 times it, by a
        // share (0.15 moving, 0.05 still), in degrees a frame.
        inline ee::Global<ee::Float, sym::g_flatlandSteerMoving> g_flatlandSteerMoving;
        inline ee::Global<ee::Float, sym::g_flatlandSteerStill> g_flatlandSteerStill;
        inline ee::Global<ee::Float, sym::g_flatlandDegreesToTurn> g_degreesToTurn;
        inline ee::Global<ee::Float, sym::g_wheelieSteerRate> g_wheelieSteerRate;
        inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_wheelieSteerMost;
        inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnFalloff; // 0.35
        constexpr int32_t kTurnStopped = 91; // half a degree

        // Under this speed (0.075 less the pedalling's repeat) the back
        // wheel is pushed by the pedalling's push; the wheels never drag
        // but by the balance's drag below.
        inline ee::Global<ee::Float, sym::g_flatlandPedalSpeed> g_flatlandPedalSpeed;
        inline ee::Global<ee::Float, sym::g_pedalRepeat> g_pedalRepeat;
        inline ee::Global<ee::Float, sym::g_pedalPush> g_pedalPush;

        // The balance, after the sub-state's first 30 frames: it tips the
        // way it leans by 0.125 a frame (a quarter more at a stop, less
        // with speed up to 1), less with the manual stat, and more by 0.035
        // over the balance frames up to 1800; at 1 either way the rider
        // bails (kind 12). In the sub-states before the 16th phase the
        // stick's y leans it by 0.045 times the manual stat. It is never
        // nearer 0 than 0.001.
        constexpr uint16_t kBalanceFromFrames = 30;
        inline ee::Global<ee::Float, sym::g_flatlandSlowSpeed> g_flatlandSlowSpeed;
        inline ee::Global<ee::Float, sym::g_flatlandSlowTip> g_flatlandSlowTip;
        inline ee::Global<ee::Float, sym::g_flatlandTip> g_flatlandTip;
        inline ee::Global<uint32_t, sym::g_flatlandTipFrames> g_flatlandTipFrames;
        inline ee::Global<ee::Float, sym::g_flatlandTipGrowth> g_flatlandTipGrowth;
        inline ee::Global<ee::Float, sym::g_flatlandStickLean> g_flatlandStickLean;
        constexpr uint32_t kStickLeanPhases = 16;
        inline ee::Global<ee::Float, sym::g_flatlandBalanceLeast> g_balanceLeast;
        inline ee::Global<ee::Float, sym::g_flatlandBalanceLeastBack> g_balanceLeastBack;
        // The wheels' drag while on the flatland.
        inline ee::Global<ee::Float, sym::g_flatlandDrag> g_flatlandDrag;
        constexpr uint8_t kBailTippedOver = 12; // Rider::bailKind

        void steerYawRate(Rider *rider, int32_t turned, int16_t most)
        {
            const int16_t below = static_cast<int16_t>(int16_t(turned) < most ? int16_t(turned) : most);
            rider->yawRate = below < -most ? static_cast<int16_t>(-most) : below;
        }
        void yawRateFalloff(Rider *rider)
        {
            const int32_t yawRate = rider->yawRate;
            rider->yawRate = static_cast<int16_t>(
                yawRate - ee::toInt(ee::toFloat(yawRate) * ee::Float(1.0f / 65536.0f) * *g_turnFalloff * ee::Float(65536.0f)));
            if (ee::abs(ee::toFloat(int32_t(rider->yawRate))) < ee::Float(float(kTurnStopped)))
                rider->yawRate = 0;
        }
    }

    namespace
    {
        // The swivel's phases (the flatland's sub-state 0).
        enum SwivelPhase : uint8_t
        {
            kSwivelIn = 0,       // the start's motion
            kSwivelLoopIn = 1,   // the loop's motion
            kSwivel = 2,         // the loop: the crouch, the turn moves, the tricks
            kSwivelCrouchIn = 4, // the crouch's motion
            kSwivelCrouch = 5,   // crouched: let go, a hop
            kSwivelOutIn = 6,    // the end's motion
            kSwivelOut = 7,      // to the landing when it ends
        };
        constexpr uint32_t kGroundSwivelStart = sym::textFlatlandSwivelStart; // "flatland.swivel.start"
        constexpr uint32_t kGroundSwivelLoop = sym::textFlatlandSwivelLoop;   // "flatland.swivel.loop"
        constexpr uint32_t kGroundSwivelCrouch = sym::textFlatlandSwivelCrouch;
        constexpr uint32_t kGroundSwivelEnd = sym::textFlatlandSwivelEnd;
        constexpr uint32_t kGroundCrouchLoop = sym::textGroundCrouchLoop;
        constexpr uint32_t kGroundCrouchEnd = sym::textGroundCrouchEnd;
        // The moves (DM2's: the crouch, the two trick buttons; the two turn
        // moves the brake's skid uses).
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMoveTrickA = 3;
        constexpr uint32_t kMoveTrickB = 4;
        constexpr uint32_t kMoveTurnLeft = 11;
        constexpr uint32_t kMoveTurnRight = 12;
        constexpr uint32_t kMovePressed = 0;
        constexpr uint32_t kMoveHeld = 1;
        constexpr uint32_t kLevelStep = 3; // its frames count a trick button's wait
        constexpr uint16_t kTapWaitFrames = 8;
        // The flatland's sub-states a trick button leads to.
        constexpr uint32_t kFlatlandTrickA = 1;
        constexpr uint32_t kFlatlandTrickB = 2;
        // Crouched with a trick on top, its blend falls by this (1/15) a
        // frame; at 0 the trick is off the top.
        inline ee::Global<ee::Float, sym::g_topTrickFade> g_topTrickFade;
        // The hop out of the crouch: the crouch state's (riding.cpp), by the
        // phase's frames.
        inline ee::Global<ee::Float, sym::g_hopPerStat> g_hopPerStat;
        inline ee::Global<ee::Float, sym::g_hopLeast> g_hopLeast;
        inline ee::Global<uint32_t, sym::g_hopFullFrames> g_hopFullFrames;

        // A trick button: with the stick's taps in, the trick (the
        // flatland's sub-state for it); none yet, it waits up to 8 frames
        // for them, the other button's wait off. True when the trick starts.
        bool trickButton(Rider *rider, uint32_t move, RiderFlag waiting, RiderFlag other, uint32_t trick)
        {
            const bool pressed = moveButtons(uint32_t(rider->controllerLayout), &rider->input, move, kMovePressed) != 0;
            if (!pressed && !hasFlag(rider, waiting))
                return false;
            if (rider->tapCount != 0)
            {
                setActionState(rider, uint32_t(kLevelSubState), trick);
                return true;
            }
            if (pressed)
            {
                rider->actionFrames[kLevelStep] = 0;
                setFlag(rider, waiting);
                clearFlag(rider, other);
            }
            else if (++rider->actionFrames[kLevelStep] >= kTapWaitFrames)
                clearFlag(rider, waiting);
            return false;
        }
        // The motion started, into the swivel's loop, the taps cleared but
        // the stick's.
        void intoSwivelLoop(Rider *rider)
        {
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivel));
            clearTaps(rider, 1u);
        }
    }

    // The flatland's sub-state 0, the swivel ("flatland.swivel.*"). By
    // phase: the start's and the loop's motions (the taps cleared but the
    // stick's); the loop, where the crouch move crouches, the two turn
    // moves held unevenly end it, and trick button A or B with the stick's
    // taps starts the flatland's trick (B: the roll set level); the crouch
    // (while held, its loop, the trick on top's blend falling to 0; let go,
    // the crouch's hop into the air by the phase's frames); the end's
    // motion, then the landing. Counts the sub-state's and the phase's
    // frames while it stays. Returns 0 the frame the state changes.
    int32_t swivelState(Rider *rider)
    {
        ee::Local<zge::Vec4> impulse;
        ButtonRecord *const input = &rider->input;
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kSwivelIn:
            riderMotionByName(kGroundSwivelStart, &rider->motionName, &rider->motion);
            intoSwivelLoop(rider);
            break;
        case kSwivelLoopIn:
            riderMotionByName(kGroundSwivelLoop, &rider->motionName, &rider->motion);
            intoSwivelLoop(rider);
            break;
        case kSwivel:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelCrouchIn));
                return 0;
            }
            const int32_t left = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMovePressed);
            const int32_t right = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRight, kMovePressed);
            if (left != right)
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelOutIn));
                return 0;
            }
            if (trickButton(rider, kMoveTrickA, kFlagTrickAWaiting, kFlagTrickBWaiting, kFlatlandTrickA))
            {
                setFlag(rider, kFlagActed);
                return 0;
            }
            if (trickButton(rider, kMoveTrickB, kFlagTrickBWaiting, kFlagTrickAWaiting, kFlatlandTrickB))
            {
                rider->roll = 0;
                setFlag(rider, kFlagActed);
                return 0;
            }
            if (hasFlag(rider, kFlagMotionEnded))
            {
                riderMotionByName(kGroundSwivelLoop, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
            }
            break;
        }
        case kSwivelCrouchIn:
            riderMotionByName(kGroundSwivelCrouch, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelCrouch));
            return 0;
        case kSwivelCrouch:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
            {
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    riderMotionByName(kGroundCrouchLoop, &rider->motionName, &rider->motion);
                    rider->motionFlags |= kMotionRestart;
                }
                if (hasFlag(rider, kFlagTopTrick))
                {
                    rider->topTrickBlend = ee::max(rider->topTrickBlend - *g_topTrickFade, ee::Float(0.0f));
                    if (rider->topTrickBlend == ee::Float(0.0f))
                        clearFlag(rider, kFlagTopTrick);
                }
                break;
            }
            else
            {
                // The hop: the crouch state's by the phase's frames.
                *impulse = zge::kZero;
                const uint32_t full = *g_hopFullFrames;
                const ee::Float least = *g_hopLeast;
                const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
                const uint16_t frames = rider->actionFrames[kLevelPhase];
                const ee::Float shareOf = zge::unsignedToFloat(frames < full ? uint32_t(frames) : full);
                impulse->y = -(perFrame * shareOf / zge::unsignedToFloat(full) + least);
                addImpulse(rider, &impulse->x);
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                riderMotionByName(kGroundCrouchEnd, &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                clearFlag(rider, kFlagMotionEnded);
                return 0;
            }
        case kSwivelOutIn:
            riderMotionByName(kGroundSwivelEnd, &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelOut));
            break;
        case kSwivelOut:
            if (hasFlag(rider, kFlagMotionEnded))
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            break;
        default:
            break;
        }
        ++rider->actionFrames[kLevelSubState];
        ++rider->actionFrames[kLevelPhase];
        return 1;
    }
    READABLE_FUNCTION_WAITS(swivelState, Draft);

    namespace
    {
        // A flatland trick's phases (its sub-state's level 2).
        enum FlatlandTrickPhase : uint8_t
        {
            kTrickIn = 0,      // the trick of the taps, its start's motion
            kTrickStarting = 1,
            kTrickLooping = 2, // its loop, again while the button is held
            kTrickEndIn = 3,   // the end's motion
            kTrickEnding = 4,  // back to the swivel when it ends
        };
        constexpr uint8_t kFlatlandTrickFirst = 0x4A; // the flatland's tricks' numbers start here
        constexpr uint32_t kSwivel0 = 0;              // the flatland's sub-state 0, the swivel
        // The flatland tricks' names for their motions ("flatland.%s.start",
        // ".loop", ".end"), from the first flatland trick.
        inline ee::Global<ee::Ptr<const char>[26], sym::g_flatlandTrickNames> g_flatlandTrickNames;
        // The tricks' motion rates: the start's, the loop's and the end's.
        inline ee::Global<ee::Float[100][3], sym::g_trickMotionRates> g_trickMotionRates;
        // The trick flags (a word a trick): this bit, the loop plays once
        // whether the button is held or not.
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;
        constexpr uint32_t kTrickLoopsOnce = 0x20u;
        constexpr uint32_t kMoveReleased = 2;
        // While the trick is on top its blend grows by 0.05 a frame to 1;
        // ending it falls by 1/15 a frame.
        inline ee::Global<ee::Float, sym::g_flatlandTrickGrow> g_flatlandTrickGrow;
        inline ee::Global<ee::Float, sym::g_flatlandTrickFade> g_flatlandTrickFade;
        void topTrickGrows(Rider *rider)
        {
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_flatlandTrickGrow, ee::Float(1.0f));
        }
    }

    // The flatland's sub-state 2, a trick of button B. Letting the button
    // go ends its hold. By phase: the trick of the stick's taps for B
    // (none: back to the swivel), its start's motion at the technique's
    // rate, the bike not mirrored, the hold by the button, the taps
    // cleared but the stick's; at the start's end the loop (held, or a
    // trick that loops once) else the end; the loop again while held (a
    // trick that loops once lets the hold go); the end's motion, then the
    // swivel. The trick on top's blend grows meanwhile and falls as it
    // ends; while it is not 0 the top trick's frames count. Counts the
    // sub-state's frames. Returns 0 the frame the state changes.
    int32_t flatlandTrickBState(Rider *rider)
    {
        ee::Local<char[0x40]> motionText;
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kTrickIn:
            rider->trickMotion = kNoTrick;
            rider->trick = flatlandTrickByTaps(rider, 1u);
            if (rider->trick == kNoTrick)
            {
                clearTaps(rider, 1u);
                setActionState(rider, uint32_t(kLevelSubState), kSwivel0);
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelLoopIn));
                break;
            }
            zge::formatString(motionText.address(), sym::textFlatlandTrickStart,
                              (*g_flatlandTrickNames)[rider->trick - kFlatlandTrickFirst]);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMirrored);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickStarting));
            rider->motionRate = rider->techniqueStat * (*g_trickMotionRates)[rider->trick][0];
            break;
        case kTrickStarting:
        case kTrickLooping:
            switch (actionState(rider, uint32_t(kLevelPhase)))
            {
            case kTrickStarting:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    if (!hasFlag(rider, kFlagTrickHeld) && ((*g_trickFlags)[rider->trick] & kTrickLoopsOnce) == 0)
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickEndIn));
                        return 0;
                    }
                    zge::formatString(motionText.address(), sym::textFlatlandTrickLoop,
                                      (*g_flatlandTrickNames)[rider->trick - kFlatlandTrickFirst]);
                    riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
                    rider->motionFlags |= kMotionRestart;
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickLooping));
                    rider->motionRate = rider->techniqueStat * (*g_trickMotionRates)[rider->trick][1];
                }
                break;
            case kTrickLooping:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    if (((*g_trickFlags)[rider->trick] & kTrickLoopsOnce) != 0)
                        clearFlag(rider, kFlagTrickHeld);
                    if (!hasFlag(rider, kFlagTrickHeld))
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickEndIn));
                        return 0;
                    }
                    rider->motionFlags |= kMotionRestart;
                }
                break;
            default:
                break;
            }
            topTrickGrows(rider);
            break;
        case kTrickEndIn:
            zge::formatString(motionText.address(), sym::textFlatlandTrickEnd,
                              (*g_flatlandTrickNames)[rider->trick - kFlatlandTrickFirst]);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickEnding));
            rider->motionFlags |= kMotionRestart;
            topTrickGrows(rider);
            break;
        case kTrickEnding:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelSubState), kSwivel0);
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kSwivelLoopIn));
                return 0;
            }
            rider->topTrickBlend = ee::max(rider->topTrickBlend - *g_flatlandTrickFade, ee::Float(0.0f));
            if (rider->topTrickBlend == ee::Float(0.0f))
                clearFlag(rider, kFlagTopTrick);
            break;
        default:
            break;
        }
        if (!(rider->topTrickBlend == ee::Float(0.0f)))
            ++rider->topTrickFrames;
        ++rider->actionFrames[kLevelSubState];
        return 1;
    }
    READABLE_FUNCTION_WAITS(flatlandTrickBState, Draft);

    namespace
    {
        // Trick button A's flatland trick (the flatland's sub-state 1): its
        // phases. From the manual a trick of some kinds goes onto the
        // flatland by a motion of its own and back the same way; trick A
        // with new taps meanwhile chains the next trick, and the crouch cuts
        // the trick short.
        enum TrickAPhase : uint8_t
        {
            kAIn = 0,              // the trick of the taps
            kAOntoIn = 1,          // the motion from the manual onto the trick
            kAOnto = 2,
            kABackIn = 3,          // the motion back to the manual
            kABack = 4,
            kATrickIn = 5,         // the trick's start
            kATrickStarting = 6,
            kATrickLooping = 7,
            kATrickEndIn = 8,
            kATrickEnding = 9,
            kAChainFromStart = 10, // a chained trick asked: the end at once, then kAChained
            kAChainFromLoop = 11,
            kAChained = 12,        // the next chained trick at its end
            kACutFromStart = 13,   // the crouch: the end at once, then kACut
            kACutFromLoop = 14,
            kACut = 15,
            kAToManual = 16,       // the manual again, at its phase 1
            kAToManualCrouched = 17, // the manual again, at its phase 5 with the crouch's frames
        };
        // The trick flags of the ways on and off: onto the trick by pegs
        // (or none of the others), a bar hop, a rope, the wheelie's end; back
        // by pegs, a bar hop, a rope, the wheelie's start.
        constexpr uint32_t kTrickOntoByPegs = 0x2000u;
        constexpr uint32_t kTrickOntoByBarhop = 0x8000u;
        constexpr uint32_t kTrickOntoByRope = 0x20000u;
        constexpr uint32_t kTrickOntoByWheelieEnd = 0x80000u;
        constexpr uint32_t kTrickOnto = kTrickOntoByPegs | kTrickOntoByBarhop | kTrickOntoByRope | kTrickOntoByWheelieEnd;
        constexpr uint32_t kTrickBackByPegs = 0x4000u;
        constexpr uint32_t kTrickBackByBarhop = 0x10000u;
        constexpr uint32_t kTrickBackByRope = 0x40000u;
        constexpr uint32_t kTrickBackByWheelieStart = 1u << 20;
        constexpr uint32_t kTrickBack = kTrickBackByPegs | kTrickBackByBarhop | kTrickBackByRope | kTrickBackByWheelieStart;
        // The ways' motions: "ground.manual_to_barhop_%s.start", "_rope_",
        // "_pegs_" and back ("ground.barhop_to_manual_%s.start" ..) by the
        // wheel ("R" on the back wheel, "F" on the front), or the wheelie's
        // own ("ground.%s.end" and ".start" of "manual" or "nosewheelie").
        // In the manual phases the trick sets them (5 .. 12) the crouch and
        // trick A act.
        constexpr uint32_t kBackWheel = sym::textRearWheel;   // "R"
        constexpr uint32_t kFrontWheel = sym::textFrontWheel; // "F"
        constexpr uint32_t kManualName = sym::textManual;     // "manual"
        constexpr uint32_t kNoseWheelieName = sym::textNoseWheelie; // "nosewheelie"
        // The trick on top's blend grows by 0.05 a frame while the trick
        // plays (by the same again at its end's start) and falls by 1/15 as
        // it ends.
        inline ee::Global<ee::Float, sym::g_trickATopGrow> g_trickATopGrow;
        inline ee::Global<ee::Float, sym::g_trickATopGrowAtEnd> g_trickATopGrowAtEnd;
        inline ee::Global<ee::Float, sym::g_trickATopFade> g_trickATopFade;
        // A cut's end motion plays fast: 1.25 times chaining, 3 times out.
        constexpr ee::Float kChainRate = 1.25f;
        constexpr ee::Float kCutRate = 3.0f;
        // The manual's tricks the flatland goes back to: the manual's own on
        // the back wheel, the nose wheelie's on the front.
        constexpr uint8_t kManualTrick = 0x48;
        constexpr uint8_t kNoseWheelieTrick = 0x49;
        constexpr uint32_t kManualPhase1 = 1;
        constexpr uint32_t kManualPhase5 = 5;
        // Back crouched: the manual's phase frames as three quarters of the
        // hop's full frames.
        constexpr ee::Float kCrouchedShare = 0.75f;

        const ee::Ptr<const char> trickName(const Rider *rider) { return (*g_flatlandTrickNames)[rider->trick - kFlatlandTrickFirst]; }
        uint32_t cutPhaseAfter(uint32_t phase) { return phase < kACutFromStart ? uint32_t(kAChained) : uint32_t(kACut); }
        ee::Float cutRate(uint32_t phase) { return phase < kACutFromStart ? kChainRate : kCutRate; }
    }

    // The flatland's sub-state 1, a trick of button A, on the back wheel
    // (the manual's physics state) or the front. Letting A go ends the
    // hold. In the trick's phases 5 .. 12 while not held: the crouch cuts
    // it short (the end motion fast, then back to the manual crouched),
    // and trick A with new taps chains the next trick (two at most; not
    // the same twice), the current one ending fast. By phase: the trick of
    // the taps for the wheel (none: back to the manual), onto it by its
    // way's motion when it has one; the trick as the B button's (start,
    // loop while held, end); back to the manual by its way's motion when
    // it has one; the manual again (its phase 1, or crouched its phase 5)
    // with the sub-state's frames and the balance kept and the balance's
    // lean on. The trick on top's blend and frames as in trick B.
    int32_t flatlandTrickAState(Rider *rider)
    {
        ee::Local<char[0x40]> motionText;
        ButtonRecord *const input = &rider->input;
        const bool onBackWheel = physicsState(rider) == kPhysicsManual;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveReleased) != 0)
            clearFlag(rider, kFlagTrickHeld);

        const uint32_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase < kACutFromStart && phase >= kATrickIn && !hasFlag(rider, kFlagTrickHeld))
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMovePressed) != 0)
            {
                switch (actionState(rider, uint32_t(kLevelPhase)))
                {
                case kATrickIn:
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kACutFromStart));
                    return 0;
                case kATrickStarting:
                case kATrickLooping:
                case kAChainFromStart:
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kACutFromLoop));
                    setFlag(rider, kFlagMotionEnded);
                    return 0;
                default:
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kACut));
                    return 0;
                }
            }
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMovePressed) != 0 && rider->tapCount != 0)
            {
                // The first free chain place, and the trick before it.
                uint32_t place = 0;
                uint8_t last = rider->trick;
                while (place < 2 && rider->chainedTricks[place] != kNoTrick)
                    last = rider->chainedTricks[place++];
                if (place < 2)
                {
                    const uint8_t next = flatlandTrickByTaps(rider, uint32_t(onBackWheel));
                    if (next != last && next != kNoTrick)
                    {
                        rider->chainedTricks[place] = next;
                        clearTaps(rider, 1u);
                        switch (actionState(rider, uint32_t(kLevelPhase)))
                        {
                        case kATrickIn:
                            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAChainFromStart));
                            return 0;
                        case kATrickStarting:
                        case kATrickLooping:
                        case kAChainFromStart:
                            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAChainFromLoop));
                            setFlag(rider, kFlagMotionEnded);
                            return 0;
                        default:
                            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAChained));
                            return 0;
                        }
                    }
                }
            }
        }

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kAIn:
        {
            rider->trickMotion = kNoTrick;
            rider->trick = flatlandTrickByTaps(rider, uint32_t(onBackWheel));
            for (uint32_t place = 0; place < 2; ++place)
                rider->chainedTricks[place] = kNoTrick;
            clearTaps(rider, 1u);
            if (rider->trick == kNoTrick)
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAToManual));
                return 0;
            }
            const bool onto = ((*g_trickFlags)[rider->trick] & kTrickOnto) != 0;
            setActionState(rider, uint32_t(kLevelPhase), onto ? uint32_t(kAOntoIn) : uint32_t(kATrickIn));
            return 0;
        }
        case kAOntoIn:
        {
            const uint32_t flags = (*g_trickFlags)[rider->trick];
            const uint32_t wheel = onBackWheel ? kBackWheel : kFrontWheel;
            if (flags & kTrickOntoByBarhop)
                zge::formatString(motionText.address(), sym::textManualToBarhopFormat, wheel);
            else if (flags & kTrickOntoByRope)
                zge::formatString(motionText.address(), sym::textManualToRopeFormat, wheel);
            else if (flags & kTrickOntoByWheelieEnd)
                zge::formatString(motionText.address(), sym::textGroundEndFormat, onBackWheel ? kManualName : kNoseWheelieName);
            else
                zge::formatString(motionText.address(), sym::textManualToPegsFormat, wheel);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMirrored);
            rider->motionRate = 1.0f;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAOnto));
            break;
        }
        case kAOnto:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickIn));
                return 0;
            }
            break;
        case kABackIn:
        {
            // (The wheelie's start is asked by the onto flag, as the original tests.)
            const uint32_t flags = (*g_trickFlags)[rider->trick];
            const uint32_t wheel = onBackWheel ? kBackWheel : kFrontWheel;
            if (flags & kTrickBackByBarhop)
                zge::formatString(motionText.address(), sym::textBarhopToManualFormat, wheel);
            else if (flags & kTrickBackByRope)
                zge::formatString(motionText.address(), sym::textRopeToManualFormat, wheel);
            else if (flags & kTrickOntoByWheelieEnd)
                zge::formatString(motionText.address(), sym::textGroundStartFormat, onBackWheel ? kManualName : kNoseWheelieName);
            else
                zge::formatString(motionText.address(), sym::textPegsToManualFormat, wheel);
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMirrored);
            rider->motionRate = 1.0f;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kABack));
            break;
        }
        case kABack:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAToManual));
                return 0;
            }
            break;
        case kATrickIn:
            zge::formatString(motionText.address(), sym::textFlatlandTrickStart, trickName(rider));
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMirrored);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveHeld) != 0)
                setFlag(rider, kFlagTrickHeld);
            else
                clearFlag(rider, kFlagTrickHeld);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickStarting));
            rider->motionRate = rider->techniqueStat * (*g_trickMotionRates)[rider->trick][0];
            break;
        case kATrickStarting:
        case kATrickLooping:
            switch (actionState(rider, uint32_t(kLevelPhase)))
            {
            case kATrickStarting:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    if (!hasFlag(rider, kFlagTrickHeld) && ((*g_trickFlags)[rider->trick] & kTrickLoopsOnce) == 0)
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickEndIn));
                        return 0;
                    }
                    zge::formatString(motionText.address(), sym::textFlatlandTrickLoop, trickName(rider));
                    riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
                    rider->motionFlags |= kMotionRestart;
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickLooping));
                    rider->motionRate = rider->techniqueStat * (*g_trickMotionRates)[rider->trick][1];
                }
                break;
            case kATrickLooping:
                if (hasFlag(rider, kFlagMotionEnded))
                {
                    if (((*g_trickFlags)[rider->trick] & kTrickLoopsOnce) != 0)
                        clearFlag(rider, kFlagTrickHeld);
                    if (!hasFlag(rider, kFlagTrickHeld))
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickEndIn));
                        return 0;
                    }
                    rider->motionFlags |= kMotionRestart;
                }
                break;
            default:
                break;
            }
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_trickATopGrow, ee::Float(1.0f));
            break;
        case kATrickEndIn:
            zge::formatString(motionText.address(), sym::textFlatlandTrickEnd, trickName(rider));
            riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickEnding));
            rider->motionFlags |= kMotionRestart;
            if (hasFlag(rider, kFlagTopTrick))
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_trickATopGrowAtEnd, ee::Float(1.0f));
            // (the end plays at the loop's rate)
            rider->motionRate = rider->techniqueStat * (*g_trickMotionRates)[rider->trick][1];
            break;
        case kATrickEnding:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                const bool back = ((*g_trickFlags)[rider->trick] & kTrickBack) != 0;
                setActionState(rider, uint32_t(kLevelPhase), back ? uint32_t(kABackIn) : uint32_t(kAToManual));
                return 0;
            }
            rider->topTrickBlend = ee::max(rider->topTrickBlend - *g_trickATopFade, ee::Float(0.0f));
            if (rider->topTrickBlend == ee::Float(0.0f))
                clearFlag(rider, kFlagTopTrick);
            break;
        case kAChainFromStart:
        case kAChainFromLoop:
        case kACutFromStart:
        case kACutFromLoop:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                zge::formatString(motionText.address(), sym::textFlatlandTrickEnd, trickName(rider));
                riderMotionByName(motionText.address(), &rider->motionName, &rider->motion);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), cutPhaseAfter(actionState(rider, uint32_t(kLevelPhase))));
            }
            rider->motionRate = cutRate(actionState(rider, uint32_t(kLevelPhase)));
            rider->motionFlags |= kMotionFlag2;
            break;
        case kAChained:
            if (hasFlag(rider, kFlagMotionEnded))
            {
                rider->trick = rider->chainedTricks[0];
                rider->chainedTricks[0] = rider->chainedTricks[1];
                rider->chainedTricks[1] = kNoTrick;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kATrickIn));
                setFlag(rider, kFlagActed);
                return 0;
            }
            rider->motionRate = kChainRate;
            break;
        case kACut:
            if (hasFlag(rider, kFlagMotionEnded))
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAToManualCrouched));
            rider->motionRate = kCutRate;
            break;
        case kAToManual:
        case kAToManualCrouched:
        {
            const uint16_t frames = rider->actionFrames[kLevelSubState];
            const ee::Float balance = rider->wheelieBalance;
            const uint32_t from = actionState(rider, uint32_t(kLevelPhase));
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionWheelie));
            if (from != kAToManualCrouched)
                setActionState(rider, uint32_t(kLevelPhase), kManualPhase1);
            else
            {
                setActionState(rider, uint32_t(kLevelPhase), kManualPhase5);
                rider->actionFrames[kLevelPhase] = uint16_t(ee::toInt(zge::unsignedToFloat(*g_hopFullFrames) * kCrouchedShare));
            }
            rider->trick = onBackWheel ? kManualTrick : kNoseWheelieTrick;
            setFlag(rider, kFlagActed);
            rider->wheelieBalance = balance;
            clearFlag(rider, kFlagBalanceLeanOff);
            rider->actionFrames[kLevelSubState] = frames;
            return 0;
        }
        default:
            break;
        }
        if (!(rider->topTrickBlend == ee::Float(0.0f)))
            ++rider->topTrickFrames;
        ++rider->actionFrames[kLevelSubState];
        return 1;
    }
    READABLE_FUNCTION_WAITS(flatlandTrickAState, Verified);

    // Action state 6, the flatland. Into the air (or physics state 12):
    // the air with "air.coast.start", kFlagLeftRail set and the motion's end
    // cleared; on both wheels the landing; a bail the bail. Else the stick
    // steers, the back wheel is pushed while slow, the balance tips (above;
    // a bail when it goes over), the bars are held with the balance's lean
    // off, and the flatland's sub-state runs (again while it asks and the
    // action stays the flatland).
    int32_t flatlandState(Rider *rider)
    {
        switch (physicsState(rider))
        {
        case kPhysicsAir:
        case kPhysicsTumble:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            riderMotionByName(kAirCoastStart, &rider->motionName, &rider->motion);
            setFlag(rider, kFlagLeftRail);
            rider->motionFlags |= kMotionRestart;
            clearFlag(rider, kFlagMotionEnded);
            return 0;
        case kPhysicsGround:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionLanding));
            return 0;
        case kPhysicsBail:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        default:
            break;
        }

        ButtonRecord *const input = &rider->input;
        const ee::Float steer = inputStick(input, 0u)->x;
        if (steer == ee::Float(0.0f))
            yawRateFalloff(rider);
        else
        {
            const ee::Float share = ee::Float(0.0f) < rider->speed ? *g_flatlandSteerMoving : *g_flatlandSteerStill;
            const ee::Float scale = *g_degreesToTurn;
            const int32_t turned = int32_t(rider->yawRate) + ee::toInt(-steer * (*g_wheelieSteerRate * share) * scale);
            steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
        }

        if (rider->speed < *g_flatlandPedalSpeed - *g_pedalRepeat)
        {
            rider->wheelDrag = 0.0f;
            rider->pedalForce = *g_pedalPush;
        }
        else
        {
            rider->pedalForce = 0.0f;
            rider->wheelDrag = 0.0f;
        }

        if (rider->actionFrames[kLevelSubState] >= kBalanceFromFrames)
        {
            ee::Float tip = 1.0f;
            const ee::Float slow = *g_flatlandSlowSpeed;
            if (rider->speed < slow)
                tip = (slow - rider->speed) * (*g_flatlandSlowTip / slow) + tip;
            tip = tip * *g_flatlandTip;
            tip = tip * (ee::Float(1.0f) - rider->manualStat);
            const uint32_t most = *g_flatlandTipFrames;
            const uint32_t frames = uint32_t(rider->balanceFrames) < most ? uint32_t(rider->balanceFrames) : most;
            tip = tip + ee::toFloat(int32_t(frames)) * (*g_flatlandTipGrowth / ee::toFloat(int32_t(most)));
            const ee::Float side = ee::Float(0.0f) < rider->wheelieBalance ? ee::Float(1.0f) : ee::Float(-1.0f);
            rider->wheelieBalance = rider->wheelieBalance + tip * side;
            if (ee::Float(1.0f) <= ee::abs(rider->wheelieBalance))
            {
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                rider->bailKind = kBailTippedOver;
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
                return 0;
            }
            if (actionState(rider, uint32_t(kLevelPhase)) < kStickLeanPhases)
                rider->wheelieBalance = rider->wheelieBalance + inputStick(input, 0u)->y * (*g_flatlandStickLean * rider->manualStat);
        }
        if (ee::abs(rider->wheelieBalance) < *g_balanceLeast)
            rider->wheelieBalance = ee::Float(0.0f) <= rider->wheelieBalance ? *g_balanceLeast : *g_balanceLeastBack;
        rider->wheelDrag = *g_flatlandDrag;
        setFlag(rider, kFlagBarsHeld);
        setFlag(rider, kFlagBalanceLeanOff);

        int32_t kept;
        do
            kept = (*g_flatlandSubStates)[rider->actionStates[kLevelSubState]](rider);
        while (kept == 0 && actionState(rider, uint32_t(kLevelAction)) == kActionFlatland);
        return kept;
    }
    READABLE_FUNCTION(flatlandState, Verified);
}
