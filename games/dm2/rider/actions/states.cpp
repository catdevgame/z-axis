// DM2's action states that pick a sub-state or end one (physics.c,
// grindState .. airSubState3): the grind's and the stall's (and lip trick's)
// action, which follow the physics state, and the air's small sub-states.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8008C75C (grindState),
// 0x8008C894 (stallState), 0x8008ECBC (airNoTrick), 0x8008FB20
// (airSubState3); DM2 Xbox 0x05F4F0, 0x05F5D0.

#include "../physics/physics.h"
#include "actions.h"
#include "../../fx/sleep.h"
#include "../../game/score.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"

#include <cstring>
#include "zge/math/angle.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/actions/states.cpp.h"

namespace game::dm2
{
    // The player's camera mode at a level (camera/camera.cpp: the rider's
    // states use level 1; mode 0 on every action, 1 as a bail starts, 2
    // with a foot down at rest).
    void setCameraMode(uint32_t player, uint32_t level, uint32_t mode);
}

namespace game::dm2::rider
{
    Stick *inputStick(const ButtonRecord *record, uint32_t which); // input.cpp
    uint8_t trickFromTaps(const Rider *rider);                     // tricks.cpp
    uint8_t grindTrick(const Rider *rider);
    uint8_t stallTrick(const Rider *rider);
    uint8_t airTrickByTaps(Rider *rider);
    uint8_t lipTrick(const Rider *rider);
    int32_t groundBelow(const ee::Float *point, GroundHit *hit);

    namespace
    {
        // The wheelie's steering (wheelieState): the yaw rate a frame from
        // the stick, 15 degrees at full stick, less at speed (by 1 / (speed x
        // 30 + 1), at least 0.05), held within the stick's share of 75.
        inline ee::Global<ee::Float, sym::g_wheelieSteerLeast> g_wheelieSteerLeast;  // 0.05
        inline ee::Global<ee::Float, sym::g_degreesToTurn2> g_degreesToTurn;   // 65536 / 360
        inline ee::Global<ee::Float, sym::g_wheelieSteerRate> g_wheelieSteerRate;   // 15
        inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_wheelieSteerMost;   // 75
        // With the stick let go the yaw rate falls by this share a frame
        // (the ground's Turn tuning), to 0 under 91 (half a degree).
        inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnFalloff;
        constexpr int32_t kTurnStopped = 91;
        // The landing's steering: as the wheelie's (its own copies of the
        // least share and the degrees), 3 degrees at full stick when not
        // moving, held within 6 (1092).
        inline ee::Global<ee::Float, sym::g_landSteerSpeed> g_landSteerSpeed;  // 30
        inline ee::Global<ee::Float, sym::g_landSteerLeast> g_landSteerLeast;  // 0.05
        inline ee::Global<ee::Float, sym::g_landDegreesToTurn> g_landDegreesToTurn;
        inline ee::Global<ee::Float, sym::g_landStillDegreesToTurn> g_landStillDegreesToTurn;
        constexpr int16_t kLandStillMost = 1092;
        // A hard landing (Rider::flags 0x80000): motion 0x21 at 2.4 for 20
        // frames, else motion 4 for 7.
        constexpr uint32_t kFlagHardLanding = 0x80000u;
        inline ee::Global<ee::Float, sym::g_hardLandingRate> g_hardLandingRate; // 2.4
        constexpr uint16_t kMotionLanding = 4;
        constexpr uint16_t kMotionHardLanding = 0x21;
        constexpr uint16_t kLandingFrames = 7;
        constexpr uint16_t kHardLandingFrames = 20;

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
            if (ee::abs(ee::toFloat(rider->yawRate)) < ee::Float(float(kTurnStopped)))
                rider->yawRate = 0;
        }

        // Angles of a sine and a cosine, in turn units (0x10000 a turn;
        // inferred from use: pedalState's slope and lean tests).
        constexpr int32_t kTurn50Degrees = 9102;
        // The moves of the move table (moveButtons) the riding states test.
        constexpr uint32_t kMovePedal = 1;
        constexpr uint32_t kMoveCrouch = 6;
        constexpr uint32_t kMovePressed = 0; // moveButtons' tests: pressed, held, let go
        constexpr uint32_t kMoveHeld = 1;
        // Pedalling: the stick pulled back past 0.95 starts a manual; under
        // 0.2705 x the rider's speed stat the pedal motion keeps
        // going on its own (less 0.01475 for the next push), and below it
        // the drive on the back wheel (Rider::pedalForce) is 0.0325.
        inline ee::Global<ee::Float, sym::g_manualStick> g_manualStick;   // 0.95
        inline ee::Global<ee::Float, sym::g_pedalSpeed> g_pedalSpeed;    // 0.2705
        inline ee::Global<ee::Float, sym::g_pedalPush> g_pedalPush;     // 0.0325
        inline ee::Global<ee::Float, sym::g_pedalRepeat> g_pedalRepeat;   // 0.01475
        constexpr uint16_t kMotionPedal = 7;
        constexpr uint8_t kFakieTurnRound = 3; // the fakie's phase turning round
        constexpr uint32_t kFlagAutoPedal = 0x10u;     // (inferred) with kFlagUpright clear: pedal while under speed
        constexpr uint32_t kFlagUpright = 0x2000000u;  // one wheel up, the other's up row within 50 degrees of upright (ground_move.cpp's)

        // The hop (crouchState): (0.165 x the rider's air stat -
        // 0.09) x the crouch's frames, up to 50, / 50, less 0.75 of it a unit
        // of the climb when rolling up a slope (not falling), plus 0.09:
        // the impulse up.
        inline ee::Global<ee::Float, sym::g_hopPerStat> g_hopPerStat;   // 0.165
        inline ee::Global<ee::Float, sym::g_hopLeast> g_hopLeast;     // 0.09
        inline ee::Global<ee::Float, sym::g_hopSlopeLoss> g_hopSlopeLoss; // 0.75
        inline ee::Global<uint32_t, sym::g_hopFullFrames> g_hopFullFrames; // 50
        constexpr uint32_t kMoveReleased = 2;
        constexpr uint16_t kMotionCrouch = 12;
        constexpr uint16_t kMotionCrouchHeld = 13;
        constexpr uint16_t kMotionHop = 14;
        constexpr uint32_t kFlagHopUp = 0x10000u;      // hopped with the stick pushed straight up, off kinds 1 and 3 (inferred)
        constexpr uint32_t kFlagWeakHop = 0x20000000u; // hopped less than half a unit below a ramp's edge (Rider::edgeHeight; inferred)

        // An unsigned word as a float (the compiled code's two ways).
        ee::Float unsignedToFloat(uint32_t w)
        {
            if (int32_t(w) >= 0)
                return ee::toFloat(int32_t(w));
            const ee::Float half = ee::toFloat(int32_t((w & 1u) | (w >> 1)));
            return half + half;
        }

        // The air tricks (airTrickB): per trick its first motion (start; +1
        // held; +2 the end) and its flags (0x20: each loop held counts).
        inline ee::Global<uint16_t[256], sym::g_trickMotions> g_trickMotions;
        inline ee::Global<uint16_t[256], sym::g_trickFlags> g_trickFlags;
        constexpr uint16_t kTrickCountsHeld = 0x20;
        constexpr uint32_t kMoveTrickB = 4;
        constexpr uint32_t kFlagTrickHeld = 0x200u; // the trick button still held (inferred)
        enum TrickPhase : uint8_t
        {
            kTrickStart = 0,
            kTrickStarting = 1,
            kTrickHeld = 2,
            kTrickEnding = 3,
        };

        // The manual (manualState): the stick pulled back past 0.95 while
        // upright and moving holds it; the wheels' drag (Rider::wheelDrag)
        // from 0.05 / 15 grows 1.12 a frame to 0.05. Moves 10 and 11
        // (the turn buttons) pressed one without the other turn it.
        inline ee::Global<ee::Float, sym::g_manualHoldStick> g_manualHoldStick; // 0.95
        inline ee::Global<ee::Float, sym::g_manualDragStart> g_manualDragStart; // 1 / 15
        inline ee::Global<ee::Float, sym::g_manualDragGrowth> g_manualDragGrowth; // 1.12
        inline ee::Global<ee::Float, sym::g_manualDrag> g_manualDrag;      // 0.05
        constexpr uint32_t kMoveTurnLeft = 10;  // (inferred which is which)
        constexpr uint32_t kMoveTurnRight = 11;
        enum ManualPhase : uint8_t
        {
            kManualIn = 0,
            kManualHeld = 1,
            kManualOut = 2,
            kManualOutEnding = 3,
            kManualTurnIn = 4,
            kManualTurn = 5,
            kManualTurnOut = 6,
        };
        constexpr uint16_t kMotionManualIn = 9;
        constexpr uint16_t kMotionManual = 10;
        constexpr uint16_t kMotionManualOut = 11;
        constexpr uint16_t kMotionManualTurnIn = 20;
        constexpr uint16_t kMotionManualTurn = 21;
        constexpr uint16_t kMotionManualTurnOut = 22;

        // Riding (ridingState): the steering's own copies of the least
        // share and the degrees; the riding sub-states.
        inline ee::Global<ee::Float, sym::g_rideSteerLeast> g_rideSteerLeast; // 0.05
        inline ee::Global<ee::Float, sym::g_rideDegreesToTurn> g_rideDegreesToTurn;
        inline ee::Global<ee::Float, sym::g_rideStillDegreesToTurn> g_rideStillDegreesToTurn;
        constexpr uint32_t kMoveGrind = 7;
        constexpr uint32_t kFlagGrindAsked = (1u << 20);  // the grind button held on the ground (riderFrame clears it)
        constexpr uint32_t kMotionFlagTurnRound = 0x2u;  // (inferred) the motion's frame turned with the rows


        // The pedal motion's rate: the speed x 8, at least 1.
        ee::Float pedalRate(ee::Float speed)
        {
            const ee::Float rate = speed * ee::Float(8.0f);
            return ee::Float(1.0f) <= rate ? rate : ee::Float(1.0f);
        }
    }

    int32_t runSubStates(Rider *rider, const SubState *table, uint8_t action)
    {
        for (;;)
        {
            const int32_t kept = table[rider->actionStates[kLevelSubState]](rider);
            if (kept != 0 || actionState(rider, uint32_t(kLevelAction)) != action)
                return kept;
        }
    }

    // Action state 2, the grind: off the rail into the air (the trick's
    // motion off it; the air starts in sub-state 3 after a hop), to riding
    // or a bail as the physics went, else the grind's sub-state.
    // GameCube 0x8008C75C.
    int32_t grindState(Rider *rider)
    {
        namespace here = points::grindState;
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            motionIndex(rider) = static_cast<uint16_t>((int32_t(rider->trick) - kGrindTrickFirst) * kMotionsPerTrick +
                                                       kMotionGrindOffFirst);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            if (rider->flags & kFlagFellOff)
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            rider->flags |= kFlagLeftRail;
            return 0;
        case kPhysicsGround:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
            return 0;
        case kPhysicsBail:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        default:
            return runSubStates(rider, g_grindSubStates.get(), kActionGrind);
        }
    }
    READABLE_FUNCTION(grindState, Verified);

    // Action states 3 and 4, the stall and the lip trick: as the grind's
    // (the original asks whether the action is the lip trick after setting
    // it to the air, which is never so: the call is kept); the sub-states
    // run again only while a stall.
    // GameCube 0x8008C894.
    int32_t stallState(Rider *rider)
    {
        namespace here = points::stallState;
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            motionIndex(rider) = static_cast<uint16_t>((int32_t(rider->trick) - kStallTrickFirst) * kMotionsPerTrick +
                                                       kMotionStallOffFirst);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            if ((rider->flags & kFlagFellOff) ||
                actionState(rider, uint32_t(kLevelAction)) == kActionLipTrick)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            }
            rider->flags |= kFlagLeftRail;
            return 0;
        case kPhysicsGround:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
            return 0;
        case kPhysicsBail:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        default:
            return runSubStates(rider, g_stallSubStates.get(), kActionStall);
        }
    }
    READABLE_FUNCTION(stallState, Verified);

    // Action state 5, the manual and the nose wheelie: off the ground into
    // the air (motion 0x19), to the landing or a bail as the physics went;
    // on one wheel the stick steers (the yaw rate +0x10D2) and the
    // wheelie's sub-state runs.
    // GameCube 0x8008C9E0.
    int32_t wheelieState(Rider *rider)
    {
        namespace here = points::wheelieState;
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            motionIndex(rider) = kMotionOffWheelie;
            rider->flags |= kFlagLeftRail;
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

        const ee::Float steer = inputStick(&rider->input, 0u)->x;
        if (steer == ee::Float(0.0f))
            yawRateFalloff(rider);
        else
        {
            ee::Float share = ee::Float(1.0f) / (rider->speed * ee::Float(30.0f) + ee::Float(1.0f));
            if (!(*g_wheelieSteerLeast <= share))
                share = *g_wheelieSteerLeast;
            const ee::Float scale = *g_degreesToTurn;
            const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * (*g_wheelieSteerRate * share) * scale);
            steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
        }
        return runSubStates(rider, g_wheelieSubStates.get(), kActionWheelie);
    }
    READABLE_FUNCTION(wheelieState, Verified);

    // Riding sub-state 1, pedalling: to coasting on a slope steeper than 50
    // degrees, to the crouch on its button, to a manual with the stick pulled
    // back while upright; else the pedal loop at max(1, speed x 8), pushed
    // again as each loop ends while the pedal is pressed (or on its own
    // under speed), coasting when let go. GameCube 0x8008DE40; BMX XXX PS2
    // pedalState (its "ground.pedal.loop").
    int32_t pedalState(Rider *rider)
    {
        // To a riding sub-state (the original's shared tail).
        auto toSubState = [rider](uint32_t state) {
            setActionState(rider, uint32_t(kLevelSubState), state);
            return 0;
        };
        if (physics::engine::asinTurn(ee::abs(rider->forward[1])) >= kTurn50Degrees)
            return toSubState(kRidingCoast);
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
        {
            const uint16_t frames = levelFrames(rider, kLevelSubState);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCrouch));
            if (rider->controllerLayout == 1)
                levelFrames(rider, kLevelSubState) = frames; // configuration 1's crouch keeps the count (as the original)
            return 0;
        }
        if (*g_manualStick < inputStick(input, 0u)->y &&
            zge::angleAcos(-rider->up[1]) < kTurn50Degrees)
            return toSubState(kRidingManual);

        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kPhaseIn)
        {
            motionIndex(rider) = kMotionPedal;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseHeld));
            const ee::Float rate = pedalRate(rider->speed);
            rider->motionRate = rate;
        }
        else if (phase == kPhaseHeld)
        {
            const ee::Float rate = pedalRate(rider->speed);
            rider->motionRate = rate;
            if (rider->flags & kFlagMotionEnded)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMovePedal, kMoveHeld) == 0)
                {
                    if (rider->controllerLayout >= 2)
                        return toSubState(kRidingCoast);
                    if ((rider->flags & (kFlagUpright | kFlagAutoPedal)) != kFlagAutoPedal)
                        return toSubState(kRidingCoast);
                    if (!(rider->speed < *g_pedalSpeed * rider->speedStat - *g_pedalRepeat))
                        return toSubState(kRidingCoast);
                }
                motionIndex(rider) = kMotionPedal;
                rider->motionFlags |= kMotionRestart;
            }
        }
        if (rider->speed < *g_pedalSpeed * rider->speedStat)
            rider->pedalForce = *g_pedalPush;
        ++levelFrames(rider, kLevelSubState);
        return 1;
    }
    READABLE_FUNCTION(pedalState, Verified);

    // Action state 6, landed: into the air again or a bail as the physics
    // went; the stick steers (less at speed); the landing's motion (a hard
    // landing's slower and longer), then riding.
    // GameCube 0x8008CCC0; BMX XXX PS2 landingState.
    int32_t landingState(Rider *rider)
    {
        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsAir)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            motionIndex(rider) = kMotionAirFall;
            return 0;
        }
        if (physics == kPhysicsBail)
        {
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        }

        const ee::Float steer = inputStick(&rider->input, 0u)->x;
        if (steer == ee::Float(0.0f))
            yawRateFalloff(rider);
        else
        {
            const ee::Float speed = rider->speed;
            const ee::Float perSpeed = *g_landSteerSpeed;
            if (ee::Float(0.0f) < speed)
            {
                ee::Float share = ee::Float(1.0f) / (speed * perSpeed + ee::Float(1.0f));
                if (!(*g_landSteerLeast <= share))
                    share = *g_landSteerLeast;
                const ee::Float scale = *g_landDegreesToTurn;
                const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * (*g_wheelieSteerRate * share) * scale);
                steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
            }
            else
            {
                const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * ee::Float(3.0f) * *g_landStillDegreesToTurn);
                steerYawRate(rider, turned, kLandStillMost);
            }
        }

        const uint8_t phase = actionState(rider, uint32_t(kLevelSubState));
        if (phase == kPhaseIn)
        {
            if (rider->flags & kFlagHardLanding)
            {
                motionIndex(rider) = kMotionHardLanding;
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kPhaseHeld));
                rider->motionRate = *g_hardLandingRate;
            }
            else
            {
                motionIndex(rider) = kMotionLanding;
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kPhaseHeld));
            }
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        if (phase != kPhaseHeld)
            return 1;
        const uint16_t frames = ++levelFrames(rider, kLevelSubState);
        const uint32_t hard = rider->flags & kFlagHardLanding;
        if (frames <= (hard ? kHardLandingFrames : kLandingFrames))
            return 1;
        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
        // The hard landing's flag kept as it was (set again), else cleared.
        if (hard)
            rider->flags |= kFlagHardLanding;
        else
            rider->flags &= ~kFlagHardLanding;
        return 0;
    }
    READABLE_FUNCTION(landingState, Verified);

    // Riding sub-state 3, the crouch: let go, the hop (an impulse up by the
    // crouch's length, the rider's stat and the slope; physics into the air,
    // the hop's motion); held, the crouch's motions (in, then held), the
    // pedal's push kept going at half under speed.
    // GameCube 0x8008E450; BMX XXX PS2 crouchState.
    int32_t crouchState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        const bool letGo = moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0;
        const ee::Float perStat = *g_hopPerStat;
        if (letGo)
        {
            const uint32_t full = *g_hopFullFrames;
            const uint32_t frames = levelFrames(rider, kLevelSubState);
            ee::Float least = *g_hopLeast;
            const ee::Float perFrame = perStat * rider->airStat - least;
            const ee::Float shareOf = unsignedToFloat(frames >= full ? full : frames);
            ee::Float hop = perFrame * shareOf / unsignedToFloat(full);

            ee::Local<zge::Vec4> impulse;
            if (rider->velocity[1] <= ee::Float(0.0f))
            {
                // Rolling up or level: less for the climb along the way the
                // rider moves.
                *impulse = *vec4(rider->forward);
                if (zge::dot(*vec4(rider->velocity), impulse) < ee::Float(0.0f))
                {
                    zge::vec3Negate(&impulse->xyz(), &impulse->xyz());
                    least = *g_hopLeast;
                }
                hop = hop - hop * (*g_hopSlopeLoss * -impulse->y);
            }
            *impulse = zge::Vec4{0.0f, -(hop + least), 0.0f, 1.0f};

            if (rider->contactKind == 1 || rider->contactKind == 3)
            {
                if (ee::Float(0.5f) < ee::abs(inputStick(input, 0u)->y) &&
                    ee::abs(inputStick(input, 0u)->x) < ee::Float(0.25f) &&
                    inputStick(input, 0u)->y < ee::Float(-0.5f))
                    rider->flags |= kFlagHopUp;
            }
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            if (rider->edgeHeight < ee::Float(0.5f))
                rider->flags |= kFlagWeakHop;
            else
                rider->flags &= ~kFlagWeakHop;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            motionIndex(rider) = kMotionHop;
            return 0;
        }

        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        const ee::Float pedalSpeed = *g_pedalSpeed;
        if (phase == kPhaseIn)
        {
            motionIndex(rider) = kMotionCrouch;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseHeld));
        }
        else if (phase == kPhaseHeld || phase == kPhaseOut)
        {
            if (phase == kPhaseHeld)
            {
                if (rider->speed < pedalSpeed * rider->speedStat)
                {
                    if (physics::engine::asinTurn(ee::abs(rider->forward[1])) < kTurn50Degrees)
                        rider->pedalForce = *g_pedalPush * ee::Float(0.5f);
                }
            }
            if (rider->flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionCrouchHeld;
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseOut));
            }
        }
        ++levelFrames(rider, kLevelSubState);
        return 1;
    }
    READABLE_FUNCTION(crouchState, Verified);

    // Air sub-state 2, the second trick button's air trick: the trick from
    // the taps, its start, its loop while the button is held (counted), its
    // end, then no trick with the plain air motion. GameCube 0x8008F864.
    int32_t airTrickB(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
            rider->flags &= ~kFlagTrickHeld;

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kTrickStart:
        {
            const uint8_t trick = trickFromTaps(rider);
            rider->trickMotion = trick;
            motionIndex(rider) = (*g_trickMotions)[trick];
            rider->motionFlags |= kMotionRestart;
            rider->flags &= ~kFlagMirrored;
            rider->trickLoops = 0;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                rider->flags |= kFlagTrickHeld;
            else
                rider->flags &= ~kFlagTrickHeld;
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickStarting));
            rider->motionRate = rider->techniqueStat;
            return 1;
        }
        case kTrickStarting:
        {
            const uint32_t flags = rider->flags;
            if (!(flags & kFlagMotionEnded))
                return 1;
            ++rider->trickLoops;
            const uint16_t first = (*g_trickMotions)[rider->trickMotion];
            if (!(flags & kFlagTrickHeld))
            {
                rider->motionFlags |= kMotionRestart;
                motionIndex(rider) = static_cast<uint16_t>(first + 2);
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickEnding));
                return 1;
            }
            rider->motionFlags |= kMotionRestart;
            motionIndex(rider) = static_cast<uint16_t>(first + 1);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickHeld));
            rider->motionRate = rider->techniqueStat;
            return 1;
        }
        case kTrickHeld:
        {
            const uint32_t flags = rider->flags;
            if (!(flags & kFlagMotionEnded))
                return 1;
            if ((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld)
                ++rider->trickLoops;
            if (!(flags & kFlagTrickHeld))
            {
                motionIndex(rider) = static_cast<uint16_t>((*g_trickMotions)[rider->trickMotion] + 2);
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kTrickEnding));
                return 1;
            }
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        case kTrickEnding:
            if (rider->flags & kFlagMotionEnded)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                motionIndex(rider) = kMotionAirFall;
                return 0;
            }
            return 1;
        default:
            return 1;
        }
    }
    READABLE_FUNCTION(airTrickB, Verified);

    // Riding sub-state 2, the manual (wheelie): held with the stick pulled
    // back while upright and moving, else its way out; in, held, turned with
    // one turn button (in, held, out), out, then coasting.
    // GameCube 0x8008E0CC; BMX XXX PS2 brakeState.
    int32_t manualState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        const bool held = *g_manualHoldStick < inputStick(input, 0u)->y &&
                          zge::angleAcos(-rider->up[1]) < kTurn50Degrees &&
                          ee::Float(0.0f) < rider->speed;
        if (!held)
        {
            if (actionState(rider, uint32_t(kLevelPhase)) < kManualOut)
            {
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualOut));
            }
        }

        auto turnButtonsDiffer = [&]()
        {
            const int32_t left = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMoveHeld);
            const int32_t right = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRight, kMoveHeld);
            return left != right;
        };
        // The original's two shared ends: the next phase or sub-state set
        // (0), and the frame's end (1).
        auto changed = [rider](uint32_t level, uint32_t state)
        {
            setActionState(rider, level, state);
            return 0;
        };
        auto done = []()
        {
            return 1;
        };

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kManualIn:
            rider->skidDistance = 0.0f;
            motionIndex(rider) = kMotionManualIn;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualHeld));
            rider->wheelDrag = *g_manualDrag * *g_manualDragStart;
            return done();
        case kManualHeld:
        {
            if (rider->flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionManual;
                rider->motionFlags |= kMotionRestart;
            }
            if (turnButtonsDiffer())
                return changed(uint32_t(kLevelPhase), uint32_t(kManualTurnIn));
            const ee::Float most = *g_manualDrag;
            const ee::Float grown = rider->wheelDrag * *g_manualDragGrowth;
            const ee::Float drag = grown <= most ? grown : most;
            rider->wheelDrag = drag;
            return done();
        }
        case kManualOut:
            motionIndex(rider) = kMotionManualOut;
            rider->motionFlags |= kMotionRestart;
            if (rider->wheelPoses[1].ground == kWheelSliding)
                rider->wheelPoses[1].ground = kWheelRolling;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualOutEnding));
            return 1;
        case kManualOutEnding:
            if (!(rider->flags & kFlagMotionEnded))
                return done();
            return changed(uint32_t(kLevelSubState), uint32_t(kRidingCoast));
        case kManualTurnIn:
        case kManualTurn:
        case kManualTurnOut:
        {
            switch (actionState(rider, uint32_t(kLevelPhase)))
            {
            case kManualTurn:
                if (rider->flags & kFlagMotionEnded)
                {
                    if (turnButtonsDiffer())
                    {
                        motionIndex(rider) = kMotionManualTurn;
                        rider->motionFlags |= kMotionRestart;
                    }
                    else
                    {
                        setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualTurnOut));
                        motionIndex(rider) = kMotionManualTurnOut;
                        rider->motionFlags |= kMotionRestart;
                    }
                }
                break;
            case kManualTurnIn:
                motionIndex(rider) = kMotionManualTurnIn;
                rider->motionFlags |= kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualTurn));
                // Turning left (a negative yaw rate) mirrors the bike's motions.
                if (rider->yawRate < 0)
                    rider->flags |= kFlagMirrored;
                else
                    rider->flags &= ~kFlagMirrored;
                break;
            case kManualTurnOut:
                if (rider->flags & kFlagMotionEnded)
                {
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kManualHeld));
                    rider->flags &= ~kFlagMirrored;
                    return 0;
                }
                break;
            default:
                break;
            }
            rider->wheelDrag = *g_manualDrag;
            if (rider->wheelPoses[1].ground == kWheelRolling)
                rider->wheelPoses[1].ground = kWheelSliding;
            rider->wheelPoses[1].spin = 0;
            return done();
        }
        default:
            return done();
        }
    }
    READABLE_FUNCTION(manualState, Verified);

    // Action state 0, riding: into the air (rolling backwards out of a
    // fakie's turn round: the rows and the body turned round), a bail, a
    // grind or a stall as the physics went (the trick by the stick); else
    // riding backwards is the fakie's sub-state, the stick steers (less at
    // speed; not while coasting to a stop), the grind button asks for a
    // grind, and the riding sub-state runs. GameCube 0x8008B3A4; BMX XXX PS2
    // ridingState.
    int32_t ridingState(Rider *rider)
    {
        namespace here = points::ridingState;
        switch (physicsState(rider))
        {
        case kPhysicsAir:
            if (actionState(rider, uint32_t(kLevelSubState)) == kRidingFakie &&
                actionState(rider, uint32_t(kLevelPhase)) == kFakieTurnRound)
            {
                rider->motionFlags |= kMotionFlagTurnRound;
                zge::vec3Negate(&vec4(rider->forward)->xyz(), &vec4(rider->forward)->xyz());
                zge::vec3Negate(&vec4(rider->right)->xyz(), &vec4(rider->right)->xyz());
                zge::vec3Negate(&matrix(rider->body)->row(2).xyz(), &matrix(rider->body)->row(2).xyz());
                zge::vec3Negate(&matrix(rider->body)->row(0).xyz(), &matrix(rider->body)->row(0).xyz());
                rider->pitch = static_cast<int16_t>(-rider->pitch);
                rider->roll = static_cast<int16_t>(-rider->roll);
            }
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            motionIndex(rider) = kMotionAirFall;
            return 0;
        case kPhysicsBail:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
            return 0;
        case kPhysicsGrindApproach:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
            return 0;
        case kPhysicsGrind:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseOut));
            rider->trick = grindTrick(rider);
            return 0;
        case kPhysicsStall:
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionStall));
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseOut));
            rider->trick = stallTrick(rider);
            rider->flags |= kFlagTrickHeld;
            return 0;
        default: // the ground, the stall approach, the lip tricks and the wheelies
            break;
        }

        if ((rider->flags & kFlagFakie) && actionState(rider, uint32_t(kLevelSubState)) != kRidingFakie)
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingFakie));

        ButtonRecord *const input = &rider->input;
        const ee::Float steer = inputStick(input, 0u)->x;
        // Coasting to a stop (coast's held phase past its first step) does
        // not steer.
        const bool stopping = steer != ee::Float(0.0f) &&
                              actionState(rider, uint32_t(kLevelSubState)) == kRidingCoast &&
                              actionState(rider, uint32_t(kLevelPhase)) == kPhaseHeld &&
                              actionState(rider, uint32_t(kLevelStep)) != 0;
        if (steer == ee::Float(0.0f) || stopping)
            yawRateFalloff(rider);
        else
        {
            const ee::Float speed = rider->speed;
            const ee::Float perSpeed = *g_landSteerSpeed;
            if (ee::Float(0.0f) < speed)
            {
                ee::Float share = ee::Float(1.0f) / (speed * perSpeed + ee::Float(1.0f));
                if (!(*g_rideSteerLeast <= share))
                    share = *g_rideSteerLeast;
                const ee::Float scale = *g_rideDegreesToTurn;
                const int32_t turned = int32_t(rider->yawRate) + ee::toInt(-steer * (*g_wheelieSteerRate * share) * scale);
                steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
            }
            else
            {
                const int32_t turned = int32_t(rider->yawRate) + ee::toInt(-steer * ee::Float(3.0f) * *g_rideStillDegreesToTurn);
                steerYawRate(rider, turned, kLandStillMost);
            }
        }

        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMoveHeld) != 0)
            rider->flags |= kFlagGrindAsked;
        const int32_t kept = runSubStates(rider, g_ridingSubStates.get(), kActionRiding);
        rider->balanceFrames = 0;
        return kept;
    }
    READABLE_FUNCTION(ridingState, Verified);

    // Riding sub-state 5: nothing (kept).
    int32_t noSubState(Rider * /*rider*/)
    {
        return 1;
    }
    READABLE_FUNCTION(noSubState, Draft);

    // Air sub-state 0, no trick: the motion the air started with (set by
    // whoever left the ground), then the air's loop once it ended.
    // GameCube 0x8008ECBC.
    int32_t airNoTrick(Rider *rider)
    {
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kPhaseIn)
        {
            rider->flags &= ~kFlagTrickTurn;
            rider->trickLoops = 0;
            rider->motionFlags |= kMotionRestart;
            rider->trickMotion = kNoTrick;
            rider->trick = kNoTrick;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseHeld));
        }
        else if (phase == kPhaseHeld && (rider->flags & kFlagMotionEnded))
        {
            motionIndex(rider) = kMotionAirLoop;
            rider->motionFlags |= kMotionRestart;
        }
        return 1;
    }
    READABLE_FUNCTION(airNoTrick, Verified);

    // Air sub-state 3, off a rail or lip with a hop: the motion off it for
    // up to a second (60 frames), the air's loop when it ended; then back
    // to no trick with the plain air motion.
    // GameCube 0x8008FB20.
    int32_t airSubState3(Rider *rider)
    {
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kPhaseIn)
        {
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kPhaseHeld));
        }
        else if (phase == kPhaseHeld)
        {
            if (levelFrames(rider, kLevelSubState) > 60)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                motionIndex(rider) = kMotionAirFall;
                return 0;
            }
            if (rider->flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionAirLoop;
                rider->motionFlags |= kMotionRestart;
            }
        }
        ++levelFrames(rider, kLevelSubState);
        return 1;
    }
    READABLE_FUNCTION(airSubState3, Verified);

    namespace
    {
        // Coasting (coastState): the riding sub-state with nothing pressed.
        // Its phases: rolling in, stopped (resting, the foot down with move
        // 21 held: the bars lean by the stick), rolling, rolling to a stop,
        // and starting off.
        enum CoastPhase : uint8_t
        {
            kCoastIn = 0,
            kCoastStopped = 1,
            kCoastRolling = 2,
            kCoastStopping = 3,
            kCoastStarting = 4,
        };
        enum RestStep : uint8_t // kCoastStopped's steps (level 3)
        {
            kRestWaiting = 0,
            kRestFootDown = 1,
            kRestLeaning = 2,
            kRestFootUp = 3,
            kRestPedalIn = 4, // the stopped rider starts to pedal
            kRestPedal = 5,
        };
        constexpr uint32_t kMoveRest = 21;            // (inferred) the foot-down button
        constexpr uint16_t kRestFootDownFrames = 8;    // move 21 held this long puts the foot down
        constexpr uint16_t kIdleFrames = 1801;         // stopped this long: idle (Rider flag 8), each 32 frames the effect
        constexpr int32_t kIdleEffectSize = 3500;
        inline ee::Global<ee::Float, sym::g_coastManualStick> g_coastManualStick; // the stick pulled back past this: a manual
        inline ee::Global<ee::Float, sym::g_hardLandingCoastRate> g_hardLandingCoastRate;
        inline ee::Global<ee::Float, sym::g_idleEffectLift> g_idleEffectLift;
        inline ee::Global<ee::Float, sym::g_barLeanFalloff> g_barLeanFalloff;
        inline ee::Global<ee::Float, sym::g_restLeanX> g_restLeanX;
        inline ee::Global<ee::Float, sym::g_restLeanY> g_restLeanY;
        inline ee::Global<ee::Float, sym::g_restLeanShare> g_restLeanShare;
        const ee::Float kLeanStopped(9.0f / 32768.0f);   // a lean under this is none
        constexpr uint16_t kMotionStill = 0;
        constexpr uint16_t kMotionStartOff = 1;
        constexpr uint16_t kMotionRestPedal = 2;
        constexpr uint16_t kMotionCoast = 4;
        constexpr uint16_t kMotionHardCoast = 0x21;

        // A sub-state change asked at level `level`; the callback returns 0
        // (the new state runs this frame).
        int32_t changeTo(Rider *rider, uint32_t level, uint32_t state)
        {
            setActionState(rider, level, state);
            return 0;
        }
        // A phase's motion started (+0x1220 bit 0) with the phase set.
        int32_t startMotion(Rider *rider, uint16_t motion, uint32_t phase)
        {
            motionIndex(rider) = motion;
            setActionState(rider, uint32_t(kLevelPhase), phase);
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        // Upright enough: the up row within 50 degrees of straight up.
        bool upright(const Rider *rider)
        {
            return zge::angleAcos(-rider->up[1]) < kTurn50Degrees;
        }
        // Rolling on the level: the forward row within 50 degrees of flat.
        bool level(const Rider *rider)
        {
            return physics::engine::asinTurn(ee::abs(rider->forward[1])) < kTurn50Degrees;
        }
    }

    // Riding sub-state 0, coasting: while moving, a grind asked on a rail
    // (contact kinds 1 .. 3) goes to physics 4; the crouch button crouches;
    // the stick pulled back while upright starts a manual. On the level the
    // pedal button (or, slow enough, the auto pedal; stopped, any button of
    // move 0) pedals (from the stopped phase via its pedal steps). Then the
    // phases: the coast's motion (a hard landing's), stopped (resting, the
    // foot down and the bars leaning; idle after 1801 frames), rolling, to
    // a stop. GameCube 0x8008D5E0.
    int32_t coastState(Rider *rider)
    {
        // The bars' lean target at rest (x and y written), and the sleep
        // effect's point (x, y and z; its w the frame's).
        ee::Local<zge::Vec4> leanTarget;
        ee::Local<zge::Vec4> sleepAt;
        ButtonRecord *const input = &rider->input;
        const bool resting = actionState(rider, uint32_t(kLevelPhase)) == kCoastStopped &&
                             actionState(rider, uint32_t(kLevelStep)) != kRestWaiting;
        if (!resting)
        {
            if (ee::Float(0.0f) < rider->speed)
            {
                const uint8_t kind = rider->contactKind;
                if (kind < 4 && kind != 0 && moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMoveHeld) != 0 &&
                    setPhysicsState(rider, uint32_t(kPhysicsGrind)) == kPhysicsGrind)
                {
                    // (the original's return through its entry blocks)
                    return 0;
                }
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
                    return changeTo(rider, kLevelSubState, kRidingCrouch);
                if (*g_coastManualStick < inputStick(input, 0u)->y && upright(rider))
                    return changeTo(rider, kLevelSubState, kRidingManual);
            }
            if (level(rider))
            {
                bool pedal = moveButtons(uint32_t(rider->controllerLayout), input, kMovePedal, kMoveHeld) != 0;
                if (!pedal)
                {
                    const ee::Float speed = rider->speed;
                    if (!(speed == ee::Float(0.0f)))
                        pedal = rider->controllerLayout < 2 && (rider->flags & (kFlagUpright | kFlagAutoPedal)) == kFlagAutoPedal &&
                                speed < *g_pedalSpeed * rider->speedStat - *g_pedalRepeat;
                    else
                        pedal = moveButtons(uint32_t(rider->controllerLayout), input, 0u, kMovePressed) != 0;
                }
                if (pedal)
                {
                    if (actionState(rider, uint32_t(kLevelPhase)) == kCoastStopped)
                        return changeTo(rider, kLevelStep, kRestPedalIn);
                    setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingPedal));
                    rider->flags |= kFlagAutoPedal;
                    return 0;
                }
            }
        }

        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kCoastIn:
            if (rider->flags & kFlagHardLanding)
            {
                motionIndex(rider) = kMotionHardCoast;
                rider->motionFlags &= ~kMotionRestart;
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kCoastRolling));
                const ee::Float rate = *g_hardLandingCoastRate;
                rider->flags &= ~kFlagHardLanding;
                rider->motionRate = rate;
                return 1;
            }
            if (rider->speed == ee::Float(0.0f) && upright(rider))
                return startMotion(rider, kMotionStill, kCoastStopping);
            return startMotion(rider, kMotionCoast, kCoastRolling);
        case kCoastStopped:
            break;
        case kCoastRolling:
            if (!(rider->flags & kFlagMotionEnded))
                return 1;
            if (!(rider->speed == ee::Float(0.0f)) || !level(rider))
                return startMotion(rider, kMotionCoast, kCoastRolling);
            return startMotion(rider, kMotionStill, kCoastStopping);
        case kCoastStopping:
            if (!(rider->flags & kFlagMotionEnded))
                return 1;
            return changeTo(rider, kLevelPhase, kCoastStarting);
        case kCoastStarting:
            return startMotion(rider, kMotionStartOff, kCoastStopped);
        default:
            return 1;
        }

        // Stopped.
        if (!upright(rider))
            return startMotion(rider, kMotionCoast, kCoastRolling);
        if (rider->flags & kFlagMotionEnded)
        {
            motionIndex(rider) = kMotionStartOff;
            rider->motionFlags |= kMotionRestart;
        }
        rider->wheelDrag = ee::Float(1.0f);
        ee::Float lean;
        switch (actionState(rider, uint32_t(kLevelStep)))
        {
        case kRestWaiting:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveRest, kMoveHeld) != 0 &&
                ++levelFrames(rider, kLevelStep) >= kRestFootDownFrames)
                return changeTo(rider, kLevelStep, kRestFootDown);
            if (rider->yawRate == 0)
            {
                const uint16_t still = ++levelFrames(rider, kLevelPhase);
                if (still >= kIdleFrames)
                {
                    rider->flags |= kFlagIdle;
                    if ((still & 0x1Fu) == 0)
                    {
                        // The effect three quarters of a unit along up, lifted.
                        const ee::Float y = rider->position[1] + rider->rootOffset[1] + rider->up[1] * ee::Float(0.75f) +
                                            *g_idleEffectLift;
                        const ee::Float z = rider->position[2] + rider->rootOffset[2] + rider->up[2] * ee::Float(0.75f);
                        sleepAt->x = rider->position[0] + rider->rootOffset[0] + rider->up[0] * ee::Float(0.75f);
                        sleepAt->y = y;
                        sleepAt->z = z;
                        // The idle rider's sleep effect (the effect named
                        // "sleep") at the point, of a size: it takes the
                        // point's whole quadword, w as the frame left it.
                        dm2::sleep(sleepAt, kIdleEffectSize);
                    }
                }
            }
            else
            {
                levelFrames(rider, kLevelPhase) = 0;
                rider->flags &= ~kFlagIdle;
            }
            // The bars' lean falls off.
            const ee::Float falloff = *g_barLeanFalloff;
            const ee::Float lean0 = rider->barLean[0], lean1 = rider->barLean[1];
            lean = lean0 - lean0 * falloff;
            rider->barLean[0] = lean;
            rider->barLean[1] = lean1 - lean1 * falloff;
            break;
        }
        case kRestFootDown:
            if (!(rider->flags & kFlagNoPlayer))
                setCameraMode(uint32_t(rider->player), 1u, 2u);
            return changeTo(rider, kLevelStep, kRestLeaning);
        case kRestLeaning:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveRest, kMoveHeld) == 0)
                return changeTo(rider, kLevelStep, kRestFootUp);
            // The bars lean toward the stick, a share a frame.
            leanTarget->y = -inputStick(input, 0u)->x * *g_restLeanX;
            const ee::Float towardY = inputStick(input, 0u)->y * *g_restLeanY;
            const ee::Float lean1 = rider->barLean[1], lean0 = rider->barLean[0];
            const ee::Float share = *g_restLeanShare;
            leanTarget->x = towardY;
            lean = lean0 + (towardY - lean0) * share;
            rider->barLean[1] = lean1 + (leanTarget->y - lean1) * share;
            rider->barLean[0] = lean;
            break;
        }
        case kRestFootUp:
            if (!(rider->flags & kFlagNoPlayer))
                setCameraMode(uint32_t(rider->player), 1u, 0u);
            return changeTo(rider, kLevelStep, kRestWaiting);
        case kRestPedalIn:
            motionIndex(rider) = kMotionRestPedal;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelStep), uint32_t(kRestPedal));
            rider->pedalForce = *g_pedalPush;
            return 1;
        case kRestPedal:
            if (rider->flags & kFlagMotionEnded)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingPedal));
                rider->flags |= kFlagAutoPedal;
                return 0;
            }
            rider->pedalForce = *g_pedalPush;
            return 1;
        default:
            return 1;
        }
        if (ee::abs(lean) < kLeanStopped)
            rider->barLean[0] = 0.0f;
        if (ee::abs(rider->barLean[1]) < kLeanStopped)
            rider->barLean[1] = 0.0f;
        return 1;
    }
    READABLE_FUNCTION(coastState, Verified);

    namespace
    {
        // Riding backwards (fakieState): its phases.
        enum FakiePhase : uint8_t
        {
            kFakieIn = 0,
            kFakieStarting = 1,
            kFakieRolling = 2,
            kFakieTurning = 3,  // turning round (move 9): the rows turned at its end
            kFakieCrouched = 4, // crouched: let go, a hop
            kFakieOutRolling = 5, // out of the fakie, moving (Rider::flags' fakie bit cleared by others)
            kFakieOutStill = 6,
        };
        constexpr uint32_t kMoveTurnRound = 9;
        constexpr uint16_t kMotionFakieIn = 15;
        constexpr uint16_t kMotionFakie = 0x10;
        constexpr uint16_t kMotionFakieTurn = 17;
        constexpr uint16_t kMotionFakieOut = 18;
        constexpr uint16_t kMotionFakieOutStill = 19;
        inline ee::Global<ee::Float, sym::g_fakieLeanStick> g_fakieLeanStick;  // the stick back past this, upright: the full blend
        inline ee::Global<ee::Float, sym::g_fakieBlendShare> g_fakieBlendShare; // else this share of it
        inline ee::Global<ee::Float, sym::g_fakieTailStick> g_fakieTailStick;
        inline ee::Global<ee::Float, sym::g_fakieTailShare> g_fakieTailShare;

        // startMotion with the motion started before the phase is set.
        void motionThen(Rider *rider, uint16_t motion, uint32_t phase)
        {
            motionIndex(rider) = motion;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), phase);
        }
    }

    // Riding sub-state 4, fakie (backwards): out of it when the fakie bit
    // went (moving or still), else the fakie's motions; rolling, the turn
    // round button turns (the rows, the drawn body, the wheels and the
    // angles turned round at its end, then coasting) and the crouch button
    // crouches for a hop on letting go. The motion blend follows the stick.
    // GameCube 0x8008E788.
    int32_t fakieState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        if (!(rider->flags & kFlagFakie))
        {
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase < 3 && phase != kFakieIn && (rider->flags & kFlagMotionEnded))
            {
                if (ee::Float(0.0f) < rider->speed)
                    motionThen(rider, kMotionFakieOut, kFakieOutRolling);
                else
                    motionThen(rider, kMotionFakieOutStill, kFakieOutStill);
                rider->flags &= ~kFlagMotionEnded;
            }
        }
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kFakieIn:
            motionThen(rider, kMotionFakieIn, kFakieStarting);
            break;
        case kFakieStarting:
            if (rider->flags & kFlagMotionEnded)
                motionThen(rider, kMotionFakie, kFakieRolling);
            break;
        case kFakieRolling:
        {
            if (rider->flags & kFlagFakie)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRound, kMovePressed) != 0)
                    motionThen(rider, kMotionFakieTurn, kFakieTurning);
                else if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
                    return changeTo(rider, kLevelPhase, kFakieCrouched);
            }
            if (rider->flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionFakie;
                rider->motionFlags |= kMotionRestart;
            }
            const ee::Float pulledBack = inputStick(input, 0u)->y;
            ee::Float drag = *g_manualDrag;
            if (*g_fakieLeanStick < pulledBack)
            {
                const bool up = upright(rider);
                drag = *g_manualDrag;
                if (up)
                {
                    rider->wheelDrag = drag;
                    break;
                }
            }
            rider->wheelDrag = drag * *g_fakieBlendShare;
            break;
        }
        case kFakieTurning:
        {
            if (!(rider->flags & kFlagMotionEnded))
            {
                rider->yawRate = 0;
                break;
            }
            // Turned round: the rows, the drawn body's right and forward,
            // each wheel's first and third rows, the pitch and roll.
            const uint32_t flags = rider->flags;
            const uint32_t motionFlags = rider->motionFlags | kMotionFlagTurnRound;
            rider->flags = flags & ~kFlagFakie;
            rider->motionFlags = motionFlags;
            auto turnRound = [](zge::Vec4 &row) { zge::vec3Negate(&row.xyz(), &row.xyz()); };
            turnRound(*vec4(rider->forward));
            turnRound(*vec4(rider->right));
            zge::Matrix &body = *matrix(rider->body);
            turnRound(body.row(2)); // forward
            turnRound(body.row(0)); // right
            for (WheelPose &wheel : rider->wheelPoses)
            {
                zge::Matrix &rows = *matrix(wheel.matrix);
                turnRound(rows.row(2));
                turnRound(rows.row(0));
            }
            const int16_t roll = static_cast<int16_t>(-rider->roll);
            rider->pitch = static_cast<int16_t>(-rider->pitch);
            rider->roll = roll;
            return changeTo(rider, kLevelSubState, kRidingCoast);
        }
        case kFakieCrouched:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            {
                // The hop: as the crouch's, by the frames crouched.
                ee::Local<zge::Vec4> impulse;
                *impulse = zge::kZero;
                const uint32_t full = *g_hopFullFrames;
                const uint32_t frames = levelFrames(rider, kLevelPhase);
                const ee::Float least = *g_hopLeast;
                const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
                const ee::Float hop = perFrame * unsignedToFloat(frames >= full ? full : frames) / unsignedToFloat(full);
                impulse->y = -(hop + least);
                addImpulse(rider, &impulse->x);
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                motionIndex(rider) = kMotionHop;
                return 0;
            }
            if (rider->flags & kFlagMotionEnded)
                rider->motionFlags |= kMotionRestart;
            break;
        }
        case kFakieOutRolling:
            if (rider->flags & kFlagMotionEnded)
                return changeTo(rider, kLevelSubState, kRidingCoast);
            break;
        case kFakieOutStill:
            if (rider->flags & kFlagMotionEnded)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
                return changeTo(rider, kLevelPhase, kCoastStopped);
            }
            rider->wheelDrag = ee::Float(1.0f);
            break;
        default:
            break;
        }
        // The drag again by the stick (not in the still way out).
        if (*g_fakieTailStick < inputStick(input, 0u)->y && upright(rider))
        {
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            const ee::Float drag = *g_manualDrag;
            if (phase < 3)
            {
                rider->wheelDrag = drag;
                ++levelFrames(rider, kLevelPhase);
                return 1;
            }
        }
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        const ee::Float drag = *g_manualDrag;
        if (phase != kFakieOutStill)
            rider->wheelDrag = drag * *g_fakieTailShare;
        ++levelFrames(rider, kLevelPhase);
        return 1;
    }
    READABLE_FUNCTION(fakieState, Verified);

    namespace
    {
        // The grind's and stall's sub-state phases (grindSubState): onto the
        // rail (the snap's frames), on it, the trick on top (the trick
        // button and the taps: in, held, out), the hop off (crouch, let go).
        enum GrindPhase : uint8_t
        {
            kGrindOnto = 0,
            kGrindLanding = 1,  // the snap's motion running until the grind physics
            kGrindStart = 2,    // the grind's own motions
            kGrindOn = 3,
            kGrindTrickHeld = 4,
            kGrindTrickOut = 5,
            kGrindCrouched = 6,
        };
        // A trick's kind bits (g_trickKindsR, a halfword a trick).
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickKinds;
        constexpr uint16_t kTrickNoLean = 0x400;  // the grind's lean blend not used
        // The tricks on top of a grind, stall or wheelie (a row of 0x56
        // bytes a trick under, by trickFromTaps' index): the trick, or
        // kNoTrick.
        constexpr uint32_t kTopTrickRow = 0x56;
        inline ee::Global<uint8_t[256][kTopTrickRow], sym::grindTopTricks> g_topTricks;
        constexpr uint32_t kMotionFlagLean = 0x10u;        // the lean's motions blended (riderFrame)
        inline ee::Global<ee::Float, sym::g_topTrickBlendIn> g_topTrickBlendIn;
        inline ee::Global<ee::Float, sym::g_topTrickBlendOut> g_topTrickBlendOut;
        inline ee::Global<ee::Float, sym::g_grindDegreesToTurn> g_grindDegreesToTurn; // 65536 / 360 (the grind's copy)
        // The most the stick turns a grind's or a stall's tilt a frame, in
        // degrees (1.2): the GameCube BMX XXX's tuning row "rotation max -
        // grind" (its default 1.2 too; tuning.ini).
        inline ee::Global<ee::Float, sym::g_grindTurnMost> g_grindTurnMost;
        constexpr uint16_t kMotionGrindFirst = 0x19E;  // grind tricks from 0x33, five motions each: onto,
        // +1 the grind, +3 / +4 its lean motions (Rider::leanMotions), +2 off (grindState)
        constexpr uint32_t kTopTrickCountBase = 15;
        constexpr int32_t kTopTrickCountSpread = 225;

        int32_t grindMotion(const Rider *rider, int32_t offset)
        {
            const int32_t n = int32_t(rider->trick) - kGrindTrickFirst;
            return n * kMotionsPerTrick + kMotionGrindFirst + offset;
        }
        // A hop off along the rider's up row, as the crouch's by the frames
        // crouched (the phase's).
        void hopOffUp(Rider *rider)
        {
            const ee::Float perStat = *g_hopPerStat;
            const uint32_t full = *g_hopFullFrames;
            const uint32_t frames = levelFrames(rider, kLevelPhase);
            const ee::Float least = *g_hopLeast;
            const ee::Float perFrame = perStat * rider->airStat - least;
            const uint32_t counted = frames >= full ? full : frames;
            const ee::Float hop = perFrame * unsignedToFloat(counted) / unsignedToFloat(full) + least;
            // (x, y and z written; w as the frame left it)
            ee::Local<zge::Vec4> impulse;
            impulse->z = rider->up[2] * hop;
            impulse->x = rider->up[0] * hop;
            impulse->y = rider->up[1] * hop;
            addImpulse(rider, &impulse->x);
        }
        // The grind's lean: the stick turns the pitch (the other way
        // fakie); the lean blends from the balance (sides swapped
        // mirrored), none for tricks without; the motion goes on (looped
        // again at its end).
        void grindLean(Rider *rider, ButtonRecord *input)
        {
            const Stick *const stick = inputStick(input, 0u);
            const uint32_t flags = rider->flags;
            int16_t turn = static_cast<int16_t>(ee::toInt(stick->x * *g_grindTurnMost * *g_grindDegreesToTurn));
            if (flags & kFlagFakie)
                turn = static_cast<int16_t>(-turn);
            rider->pitch = static_cast<int16_t>(turn + rider->pitch);
            if ((*g_trickKinds)[rider->trick] & kTrickNoLean)
            {
                rider->leanBlend[0] = 0.0f;
                rider->leanBlend[1] = 0.0f;
            }
            else
            {
                // The balance's positive part to the first lean motion, its
                // negative part to the second (swapped when mirrored).
                const ee::Float balance = rider->balance;
                const ee::Float positive = ee::max(balance, ee::Float(0.0f));
                const ee::Float negative = balance < ee::Float(0.0f) ? -balance : ee::Float(0.0f);
                const bool mirrored = (flags & kFlagMirrorSide) != 0;
                rider->leanBlend[0] = mirrored ? negative : positive;
                rider->leanBlend[1] = mirrored ? positive : negative;
            }
            if (flags & kFlagMotionEnded)
                rider->motionFlags |= kMotionRestart | kMotionFlagTurnRound;
            rider->motionFlags |= kMotionFlagLean;
        }
        // The grind and stall sub-states' frame counts: the sub-state's
        // while a trick is on top, the phase's always.
        int32_t countGrindFrames(Rider *rider)
        {
            if (!(rider->topTrickBlend == ee::Float(0.0f)))
                ++levelFrames(rider, kLevelSubState);
            ++levelFrames(rider, kLevelPhase);
            return 1;
        }
        // The trick on top's blend going out (to none: the flag off).
        void topTrickOut(Rider *rider)
        {
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_topTrickBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                rider->flags &= ~kFlagTopTrick;
        }
    }

    // The grind's sub-state: onto the rail (the grind trick by the stick,
    // the motion at 16 / the snap's frames), the grind's motions, then on
    // the rail: a trick on top by the trick button and the taps (held,
    // counted, out), the crouch and its let-go hop (into the air), the
    // lean. GameCube 0x8008FBEC.
    int32_t grindSubState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        // The shared ends: the frame counted (1), the phase changed (0).
        auto counted = [rider]() { return countGrindFrames(rider); };
        auto changed = [rider](uint32_t state)
        {
            setActionState(rider, uint32_t(kLevelPhase), state);
            return 0;
        };
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kGrindLanding)
        {
            if (physicsState(rider) == kPhysicsGrind)
                return changed(kGrindStart);
            return counted();
        }
        if (phase == kGrindOnto)
        {
            const uint8_t trick = grindTrick(rider);
            rider->trick = trick;
            const uint32_t flags = rider->flags;
            rider->motionFlags |= kMotionRestart | kMotionFlagTurnRound;
            motionIndex(rider) = static_cast<uint16_t>(grindMotion(rider, 0));
            if (flags & kFlagMirrorSide)
                rider->flags = flags | kFlagMirrored;
            rider->motionRate = ee::Float(16.0f) / ee::toFloat(int32_t(static_cast<uint16_t>(rider->snapFrames)));
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindLanding));
            return counted();
        }
        if (phase == kGrindStart)
        {
            const uint32_t motionFlags = rider->motionFlags;
            rider->leanBlend[0] = 0.0f;
            uint32_t flags = rider->flags;
            motionIndex(rider) = static_cast<uint16_t>(grindMotion(rider, 1));
            rider->leanMotions[0] = static_cast<uint16_t>(grindMotion(rider, 3));
            rider->leanMotions[1] = static_cast<uint16_t>(grindMotion(rider, 4));
            rider->motionFlags = motionFlags | kMotionRestart | kMotionFlagLean;
            rider->leanBlend[1] = 0.0f;
            if (flags & kFlagMirrorSide)
            {
                flags |= kFlagMirrored;
                rider->flags = flags;
            }
            rider->motionRate = ee::Float(1.0f);
            if (!(flags & kFlagNoPlayer))
                trickEvent(uint32_t(rider->player), 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindOn));
            return counted();
        }

        // On the rail.
        if (actionState(rider, uint32_t(kLevelPhase)) < kGrindCrouched &&
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
            return changed(kGrindCrouched);
        switch (const uint8_t now = actionState(rider, uint32_t(kLevelPhase)))
        {
        case kGrindOn:
        {
            // The trick button (or pressed before) with taps: the trick on
            // top from the grind's row, else the taps cleared.
            uint8_t taps;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
            {
                if (!(rider->flags & kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                    break;
            }
            else
                taps = rider->tapCount;
            if (taps == 0)
            {
                rider->flags |= kFlagTrickBWaiting;
                break;
            }
            const uint8_t tapIndex = trickFromTaps(rider);
            const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
            if (top == kNoTrick)
            {
                clearTaps(rider, 1u);
                break;
            }
            const ee::Float rate = rider->techniqueStat;
            const uint32_t flags = rider->flags | kFlagTopTrick;
            rider->trickMotion = top;
            rider->flags = flags;
            rider->topTrickBlend = 0.0f;
            rider->topTrickLoopFrames = static_cast<uint8_t>(ee::toInt((ee::Float(1.0f) - rate) * ee::toFloat(kTopTrickCountSpread)) +
                                                        kTopTrickCountBase);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                rider->flags |= kFlagTrickHeld;
            else
            {
                rider->flags &= ~kFlagTrickHeld;
            }
            return changed(kGrindTrickHeld);
        }
        case kGrindTrickHeld:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
                return changed(kGrindTrickOut);
            if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
            {
                const uint8_t every = rider->topTrickLoopFrames;
                const uint16_t frames = levelFrames(rider, kLevelSubState);
                if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                    ++rider->trickLoops;
            }
            if (rider->flags & kFlagTopTrick)
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_topTrickBlendIn, ee::Float(1.0f));
            break;
        case kGrindTrickOut:
            topTrickOut(rider);
            break;
        case kGrindCrouched:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            {
                hopOffUp(rider);
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                motionIndex(rider) = static_cast<uint16_t>(grindMotion(rider, 2));
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                rider->flags |= kFlagLeftRail;
                return 0;
            }
            topTrickOut(rider);
            break;
        default:
            break;
        }
        grindLean(rider, input);
        return counted();
    }
    READABLE_FUNCTION(grindSubState, Verified);

    namespace
    {
        // The stall's and lip trick's sub-state (stallSubState): the
        // grind's phases, the let-go hop its own (kStallHop, from a lip
        // up and ahead at 85 degrees).
        enum StallPhase : uint8_t
        {
            kStallHop = 7, // after kGrindCrouched's let-go (or a trick's end): the hop off
        };
        constexpr uint32_t kMoveStall = 8;
        constexpr uint16_t kMotionStallFirst = 0x1CB; // stall tricks from 0x3C, five motions each (as the grind's)
        constexpr uint16_t kTrickNoTurn = 0x200;      // trick kinds: the stick does not turn
        constexpr uint16_t kTrickTurnByY = 0x80;      // the stick's y turns (else its x)
        constexpr uint16_t kTrickHopAtEnd = 0x20;     // the motion's end hops off (else it loops)
        constexpr int32_t kLipHopAngle = 15473;       // 85 degrees (0x10000 a turn)
        inline ee::Global<ee::Float, sym::g_stallTopBlendIn> g_stallTopBlendIn;
        inline ee::Global<ee::Float, sym::g_stallTopBlendOut> g_stallTopBlendOut;
        inline ee::Global<ee::Float, sym::g_stallDegreesToTurnX> g_stallDegreesToTurnX; // 65536 / 360 (the stall's copies,
        inline ee::Global<ee::Float, sym::g_stallDegreesToTurnY> g_stallDegreesToTurnY; // one for each stick axis)

        int32_t stallMotion(const Rider *rider, int32_t offset)
        {
            const int32_t n = int32_t(rider->trick) - kStallTrickFirst;
            return n * kMotionsPerTrick + kMotionStallFirst + offset;
        }
    }

    // The stall's and lip trick's sub-state: onto the target (the motion
    // at 16 / the snap's frames), the stall's motions, then held: a trick
    // on top (as the grind's), the crouch, the hop off (up the rider's up
    // row; off a lip ahead and up at 85 degrees, into the air's sub-state
    // 3); the stick turns the pitch (by x or y as the trick has it) and the
    // lean blends follow the balance; a trick that hops at its end hops.
    // GameCube 0x80090348.
    int32_t stallSubState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveStall, kMoveReleased) != 0 ||
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
            rider->flags &= ~kFlagTrickHeld;
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kGrindLanding)
        {
            const uint8_t physics = physicsState(rider);
            if (physics == kPhysicsStall || physics == kPhysicsLipTrick)
                return changeTo(rider, kLevelPhase, kGrindStart);
            return countGrindFrames(rider);
        }
        if (phase == kGrindOnto)
        {
            const uint32_t flags = rider->flags;
            rider->motionFlags |= kMotionRestart | kMotionFlagTurnRound;
            motionIndex(rider) = static_cast<uint16_t>(stallMotion(rider, 0));
            if (flags & kFlagMirrorSide)
                rider->flags = flags | kFlagMirrored;
            rider->motionRate = ee::Float(16.0f) / ee::toFloat(int32_t(static_cast<uint16_t>(rider->snapFrames)));
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindLanding));
            rider->flags |= kFlagTrickHeld;
            return countGrindFrames(rider);
        }
        if (phase == kGrindStart)
        {
            const uint32_t motionFlags = rider->motionFlags;
            rider->leanBlend[0] = 0.0f;
            uint32_t flags = rider->flags;
            motionIndex(rider) = static_cast<uint16_t>(stallMotion(rider, 1));
            rider->leanMotions[0] = static_cast<uint16_t>(stallMotion(rider, 3));
            rider->leanMotions[1] = static_cast<uint16_t>(stallMotion(rider, 4));
            rider->motionFlags = motionFlags | kMotionRestart | kMotionFlagLean;
            rider->leanBlend[1] = 0.0f;
            if (flags & kFlagMirrorSide)
            {
                flags |= kFlagMirrored;
                rider->flags = flags;
            }
            rider->motionRate = ee::Float(1.0f);
            if (!(flags & kFlagNoPlayer))
                trickEvent(uint32_t(rider->player), 1u);
            return changeTo(rider, kLevelPhase, kGrindOn);
        }

        if (actionState(rider, uint32_t(kLevelPhase)) < kGrindCrouched &&
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMovePressed) != 0)
        {
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kGrindCrouched));
            rider->flags |= kFlagTrickHeld;
            return 0;
        }
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kGrindOn:
        {
            uint8_t taps;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
            {
                if (!(rider->flags & kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                    break;
            }
            else
                taps = rider->tapCount;
            if (taps == 0)
            {
                rider->flags |= kFlagTrickBWaiting;
                break;
            }
            const uint8_t tapIndex = trickFromTaps(rider);
            const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
            if (top == kNoTrick)
            {
                clearTaps(rider, 1u);
                break;
            }
            const ee::Float rate = rider->techniqueStat;
            const uint32_t flags = rider->flags | kFlagTopTrick;
            rider->trickMotion = top;
            rider->flags = flags;
            rider->topTrickBlend = 0.0f;
            rider->topTrickLoopFrames = static_cast<uint8_t>(ee::toInt((ee::Float(1.0f) - rate) * ee::toFloat(kTopTrickCountSpread)) +
                                                        kTopTrickCountBase);
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                rider->flags |= kFlagTrickHeld;
            else
                rider->flags &= ~kFlagTrickHeld;
            return changeTo(rider, kLevelPhase, kGrindTrickHeld);
        }
        case kGrindTrickHeld:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
                return changeTo(rider, kLevelPhase, kGrindTrickOut);
            if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
            {
                const uint8_t every = rider->topTrickLoopFrames;
                const uint16_t frames = levelFrames(rider, kLevelSubState);
                if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                    ++rider->trickLoops;
            }
            if (rider->flags & kFlagTopTrick)
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_stallTopBlendIn, ee::Float(1.0f));
            break;
        case kGrindCrouched:
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveReleased) != 0)
                return changeTo(rider, kLevelPhase, kStallHop);
            [[fallthrough]];
        case kGrindTrickOut:
        {
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_stallTopBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                rider->flags &= ~kFlagTopTrick;
            break;
        }
        case kStallHop:
        {
            const uint32_t full = *g_hopFullFrames;
            const uint32_t frames = levelFrames(rider, kLevelPhase);
            const ee::Float least = *g_hopLeast;
            const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
            const ee::Float hop = perFrame * unsignedToFloat(frames >= full ? full : frames) / unsignedToFloat(full) + least;
            ee::Local<zge::Vec4> impulse;
            // The scale carries what the last call left beside the hop, as
            // airGravity's does.
            uint64_t carried;
            if (actionState(rider, uint32_t(kLevelAction)) != kActionLipTrick)
            {
                carried = zge::scaleLeftover(zge::LeftBy::lastCall);
                *impulse = *vec4(rider->up); // up the up row
            }
            else
            {
                // Off a lip: ahead and up, 85 degrees, by the rows (w as the
                // frame left it).
                const ee::Float cosine = physics::engine::cosTurn(kLipHopAngle);
                const ee::Float sine = physics::engine::sinTurn(kLipHopAngle);
                const uint32_t mirrored = rider->flags & kFlagMirrorSide;
                const ee::Float ahead = cosine * (ee::Float(1.0f) / sine);
                impulse->x = ahead;
                if (mirrored != 0)
                    impulse->x = -ahead;
                impulse->y = ee::Float(1.0f);
                impulse->z = 0.0f;
                physics::engine::transformPoint(&impulse->x, rider->right, &impulse->x);
                carried = zge::scaleLeftover(zge::LeftBy::lastCall);
            }
            *impulse = zge::scaledCarrying(*impulse, hop, carried);
            addImpulse(rider, &impulse->x);
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            motionIndex(rider) = static_cast<uint16_t>(stallMotion(rider, 2));
            const bool offLip = actionState(rider, uint32_t(kLevelAction)) == kActionLipTrick;
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
            if (offLip)
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirSubState3));
            rider->flags |= kFlagLeftRail;
            return 0;
        }
        default:
            break;
        }

        // The stick turns the pitch; the lean blends.
        const uint16_t kinds = (*g_trickKinds)[rider->trick];
        int16_t turn = 0;
        if (!(kinds & kTrickNoTurn))
        {
            ee::Float by;
            if (!(kinds & kTrickTurnByY))
                by = inputStick(input, 0u)->x * *g_grindTurnMost * *g_stallDegreesToTurnX;
            else
                by = -inputStick(input, 0u)->y * *g_grindTurnMost * *g_stallDegreesToTurnY;
            turn = static_cast<int16_t>(ee::toInt(by));
        }
        rider->pitch = static_cast<int16_t>(turn + rider->pitch);
        uint32_t flags;
        if ((*g_trickKinds)[rider->trick] & kTrickNoLean)
        {
            rider->leanBlend[0] = 0.0f;
            rider->leanBlend[1] = 0.0f;
            flags = rider->flags;
        }
        else
        {
            flags = rider->flags;
            const ee::Float balance = rider->balance;
            if (!(flags & kFlagMirrorSide))
            {
                rider->leanBlend[0] = ee::max(balance, ee::Float(0.0f));
                rider->leanBlend[1] = balance < ee::Float(0.0f) ? -balance : ee::Float(0.0f);
            }
            else
            {
                rider->leanBlend[0] = balance < ee::Float(0.0f) ? -balance : ee::Float(0.0f);
                rider->leanBlend[1] = ee::max(balance, ee::Float(0.0f));
            }
            rider->motionFlags |= kMotionFlagLean;
        }
        if (flags & kFlagMotionEnded)
        {
            if ((*g_trickKinds)[rider->trick] & kTrickHopAtEnd)
                return changeTo(rider, kLevelPhase, kStallHop);
            rider->motionFlags |= kMotionRestart | kMotionFlagTurnRound;
        }
        return countGrindFrames(rider);
    }
    READABLE_FUNCTION(stallSubState, Verified);

    namespace
    {
        // The wheelie's sub-state (wheelieSubState): its phases.
        enum WheeliePhase : uint8_t
        {
            kWheelieOnto = 0,
            kWheelieOn = 1,
            kWheelieTrickHeld = 2,
            kWheelieTrickOut = 3,
            kWheelieCrouched = 4,
            kWheelieOff = 5,     // the balancing wheel slowed: off the wheelie (both off: none)
            kWheelieOffEnding = 6,
        };
        constexpr uint16_t kMotionWheelieOnto = 23;
        constexpr uint16_t kMotionWheelieOntoFromHop = 24; // (from the hop's motion 14)
        constexpr uint16_t kMotionWheelie = 24;
        constexpr uint16_t kMotionWheelieOff = 25;
        constexpr uint8_t kTrickNoseWheelie = 71; // physics 9's (the trick names, g_trickNames: "nose wheelie")
        constexpr uint8_t kTrickManual = 70;      // physics 10's ("manual")
        constexpr uint16_t kWheelieBalanceFrom = 30;   // frames on one wheel before the balance tips
        constexpr uint8_t kBailKindWheelie = 12;       // Rider +0x178, the bail's kind
        inline ee::Global<ee::Float, sym::g_wheelieWheelLeast> g_wheelieWheelLeast; // the balancing wheel slower than this: off
        inline ee::Global<ee::Float, sym::g_wheelieTopBlendIn> g_wheelieTopBlendIn;
        inline ee::Global<ee::Float, sym::g_wheelieTopBlendOut> g_wheelieTopBlendOut;
        inline ee::Global<ee::Float, sym::g_wheelieCrouchBlendOut> g_wheelieCrouchBlendOut;
        inline ee::Global<ee::Float, sym::g_wheelieBalanceLeast> g_wheelieBalanceLeast; // the balance at least this either way
        inline ee::Global<ee::Float, sym::g_wheelieBalanceLeastBack> g_wheelieBalanceLeastBack;
        inline ee::Global<ee::Float, sym::g_wheelieSlowTip> g_wheelieSlowTip;      // slower than g_wheelieSlow tips this much more
        inline ee::Global<ee::Float, sym::g_wheelieSlow> g_wheelieSlow;
        inline ee::Global<ee::Float, sym::g_wheelieTip> g_wheelieTip;
        inline ee::Global<ee::Float, sym::g_wheelieStickBalance> g_wheelieStickBalance;
        inline ee::Global<ee::Float, sym::g_wheelieDrag> g_wheelieDrag;
        inline ee::Global<ee::Float, sym::g_railTimeTip> g_wheelieTimeTip;
        inline ee::Global<uint32_t, sym::g_railTimeFull> g_wheelieTimeFull;

        // A hop straight up (the impulse (0, -hop, 0, 1)), as the crouch's by
        // the phase's frames.
        void hopStraightUp(Rider *rider)
        {
            ee::Local<zge::Vec4> impulse;
            *impulse = zge::kZero;
            const uint32_t full = *g_hopFullFrames;
            const uint32_t frames = levelFrames(rider, kLevelPhase);
            const ee::Float least = *g_hopLeast;
            const ee::Float perFrame = *g_hopPerStat * rider->airStat - least;
            const uint32_t counted = frames >= full ? full : frames;
            const ee::Float hop = perFrame * unsignedToFloat(counted) / unsignedToFloat(full);
            impulse->y = -(hop + least);
            addImpulse(rider, &impulse->x);
        }
    }

    // The wheelie's sub-state (the manual's and the nose wheelie's): off
    // when the balancing wheel slows; up on the wheel (the trick by the
    // state: the manual or the nose wheelie), on it: a trick on top (as the
    // grind's), the crouch and its let-go hop (into the air); off the wheel.
    // After 30 frames the balance tips (more when slow, with the time on one
    // wheel, less with the skill; the stick holds it): past 1 tipped back
    // onto both wheels the rider rides on, tipped over he bails (kind 12).
    // GameCube 0x80090C9C.
    int32_t wheelieSubState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        // The shared end of a phase or state change.
        auto changed = [rider](uint32_t level, uint32_t state)
        {
            setActionState(rider, level, state);
            return 0;
        };
        bool bothOff = false;
        if (actionState(rider, uint32_t(kLevelPhase)) < kWheelieOff)
        {
            const ee::Float least = *g_wheelieWheelLeast;
            for (uint32_t wheel = 0;; ++wheel)
            {
                if (wheel >= 2)
                {
                    bothOff = true;
                    break;
                }
                if (rider->wheelPoses[wheel].ground == kWheelOffGround)
                {
                    continue;
                }
                if (rider->wheelPoses[wheel].speed < least)
                {
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieOff));
                }
                break;
            }
        }
        switch (actionState(rider, uint32_t(kLevelPhase)))
        {
        case kWheelieOnto:
        {
            const uint16_t onto = motionIndex(rider) == kMotionHop ? kMotionWheelieOntoFromHop : kMotionWheelieOnto;
            motionIndex(rider) = onto;
            rider->motionFlags |= kMotionRestart;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieOn));
            const uint8_t trick = physicsState(rider) == kPhysicsManual ? kTrickManual : kTrickNoseWheelie;
            rider->trick = trick;
            clearTaps(rider, 1u);
            rider->balance = 0.0f;
            break;
        }
        case kWheelieOn:
        case kWheelieTrickHeld:
        case kWheelieTrickOut:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) != 0)
                return changed(uint32_t(kLevelPhase), uint32_t(kWheelieCrouched));
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase == kWheelieTrickHeld)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
                    return changed(uint32_t(kLevelPhase), uint32_t(kWheelieTrickOut));
                if (((*g_trickFlags)[rider->trickMotion] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
                {
                    const uint8_t every = rider->topTrickLoopFrames;
                    const uint16_t frames = levelFrames(rider, kLevelSubState);
                    if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                        ++rider->trickLoops;
                }
                if (rider->flags & kFlagTopTrick)
                    rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_wheelieTopBlendIn, ee::Float(1.0f));
            }
            else if (phase == kWheelieOn)
            {
                uint8_t taps = 0;
                bool tryTrick = true;
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
                {
                    if (!(rider->flags & kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                        tryTrick = false;
                }
                else
                    taps = rider->tapCount;
                if (tryTrick)
                {
                    if (taps == 0)
                        rider->flags |= kFlagTrickBWaiting;
                    else
                    {
                        const uint8_t tapIndex = trickFromTaps(rider);
                        const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
                        if (top != kNoTrick)
                        {
                            const ee::Float rate = rider->techniqueStat;
                            const uint32_t flags = rider->flags | kFlagTopTrick;
                            rider->trickMotion = top;
                            rider->flags = flags;
                            rider->topTrickBlend = 0.0f;
                            rider->topTrickLoopFrames = static_cast<uint8_t>(
                                ee::toInt((ee::Float(1.0f) - rate) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
                            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                                rider->flags |= kFlagTrickHeld;
                            else
                                rider->flags &= ~kFlagTrickHeld;
                            return changed(uint32_t(kLevelPhase), uint32_t(kWheelieTrickHeld));
                        }
                        clearTaps(rider, 1u);
                    }
                }
            }
            else if (phase == kWheelieTrickOut)
            {
                const ee::Float blend = ee::max(rider->topTrickBlend - *g_wheelieTopBlendOut, ee::Float(0.0f));
                rider->topTrickBlend = blend;
                if (blend == ee::Float(0.0f))
                    rider->flags &= ~kFlagTopTrick;
            }
            if (rider->flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionWheelie;
                rider->motionFlags |= kMotionRestart;
            }
            break;
        }
        case kWheelieCrouched:
        {
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveCrouch, kMoveHeld) == 0)
            {
                hopStraightUp(rider);
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionAir));
                motionIndex(rider) = kMotionHop;
                return 0;
            }
            const uint32_t flags = rider->flags;
            if (flags & kFlagMotionEnded)
            {
                motionIndex(rider) = kMotionWheelie;
                rider->motionFlags |= kMotionRestart;
            }
            if (flags & kFlagTopTrick)
            {
                const ee::Float blend = ee::max(rider->topTrickBlend - *g_wheelieCrouchBlendOut, ee::Float(0.0f));
                rider->topTrickBlend = blend;
                if (blend == ee::Float(0.0f))
                    rider->flags = flags & ~kFlagTopTrick;
            }
            break;
        }
        case kWheelieOff:
        {
            const uint32_t flags = rider->flags & ~kFlagManual & ~kFlagNoseWheelie;
            rider->motionFlags |= kMotionRestart;
            rider->flags = flags;
            motionIndex(rider) = kMotionWheelieOff;
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kWheelieOffEnding));
            break;
        }
        default:
            break;
        }

        // The balance.
        const uint16_t onOneWheel = levelFrames(rider, kLevelSubState);
        if (onOneWheel >= kWheelieBalanceFrom)
        {
            const ee::Float speed = rider->speed;
            const ee::Float slow = *g_wheelieSlow;
            ee::Float tip = ee::Float(1.0f);
            const ee::Float slowTip = *g_wheelieSlowTip;
            if (speed < slow)
                tip = (slow - speed) * (slowTip / slow) + tip;
            const uint32_t full = *g_wheelieTimeFull;
            const uint32_t ridden = static_cast<uint32_t>(rider->balanceFrames);
            const ee::Float balance = rider->balance;
            ee::Float side = ee::Float(-1.0f);
            const ee::Float by = tip * *g_wheelieTip * (ee::Float(1.0f) - rider->manualStat) +
                                 ee::toFloat(int32_t(ridden < full ? ridden : full)) * (*g_wheelieTimeTip / ee::toFloat(int32_t(full)));
            if (ee::Float(0.0f) < balance)
                side = ee::Float(1.0f);
            const ee::Float tipped = balance + by * side;
            rider->balance = tipped;
            if (ee::Float(1.0f) <= ee::abs(tipped))
            {
                const bool towardRight = ee::Float(0.0f) < tipped;
                const bool manual = physicsState(rider) == kPhysicsManual;
                const bool dropToGround = manual ? !towardRight : towardRight;
                if (!dropToGround)
                {
                    setPhysicsState(rider, uint32_t(kPhysicsBail));
                    rider->bailKind = kBailKindWheelie;
                    setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionBail));
                    return 0;
                }
                // Down on both wheels: the balancing wheel's state goes to the other.
                setPhysicsState(rider, uint32_t(kPhysicsGround));
                const uint32_t from = physicsState(rider) == kPhysicsManual ? 1u : 0u;
                const uint32_t to = 1u - from;
                std::memcpy(&rider->wheelPoses[to], &rider->wheelPoses[from], 16);
                const ee::Float fromSpeed = rider->wheelPoses[from].speed;
                rider->wheelPoses[to].ground = kWheelRolling;
                rider->wheelPoses[to].speed = fromSpeed;
                return changed(uint32_t(kLevelAction), uint32_t(kActionLanding));
            }
            if (actionState(rider, uint32_t(kLevelPhase)) < kWheelieOff)
            {
                const Stick *const stick = inputStick(input, 0u);
                rider->balance = rider->balance + stick->y * (*g_wheelieStickBalance * rider->manualStat);
            }
        }
        const ee::Float balance = rider->balance;
        const ee::Float least = *g_wheelieBalanceLeast;
        if (ee::abs(balance) < least)
        {
            const ee::Float to = ee::Float(0.0f) <= balance ? least : *g_wheelieBalanceLeastBack;
            rider->balance = to;
        }
        const ee::Float drag = *g_wheelieDrag;
        levelFrames(rider, kLevelSubState) = static_cast<uint16_t>(levelFrames(rider, kLevelSubState) + 1);
        ++levelFrames(rider, kLevelPhase);
        rider->wheelDrag = drag;
        return 1;
    }
    READABLE_FUNCTION(wheelieSubState, Verified);

    namespace
    {
        // The first trick button's air trick (airTrick): its phases.
        enum AirTrickPhase : uint8_t
        {
            kAirTrickStart = 0,
            kAirTrickStarting = 1,
            kAirTrickHeld = 2,
            kAirTrickEnd = 3,
            kAirTrickEnding = 4,
        };
        constexpr uint32_t kMoveTrickA = 3;
        constexpr uint16_t kMotionAirNoTrick = 35;
        constexpr uint16_t kMotionAirTrickOut = 34;
        // The air tricks' first motions (+1 held, +2 the end),
        // the first button's (airTrickB's are g_trickMotions).
        inline ee::Global<uint16_t[256], sym::g_airTrickMotions> g_airTrickMotions;
        // A trick's kind bits & 7: none, or a spin or flip turned by the
        // rider's spin stat about the second turn's angles (spinAngles), up
        // to a most (the total in trickTurned).
        constexpr uint16_t kTrickTurnKinds = 0x7;
        constexpr uint32_t kFlagSnapAlign = 0x4000u;   // (inferred) cleared with the snap's frames
        inline ee::Global<ee::Float, sym::g_spinFalloff> g_spinFalloff;
        inline ee::Global<ee::Float, sym::g_turnRate1> g_turnRate1;
        inline ee::Global<ee::Float, sym::g_turnRate23> g_turnRate23;
        inline ee::Global<ee::Float, sym::g_turnRate4> g_turnRate4;
        inline ee::Global<ee::Float, sym::g_turnRate5> g_turnRate5;
        inline ee::Global<ee::Float, sym::g_airTopBlendIn> g_airTopBlendIn;
        inline ee::Global<ee::Float, sym::g_airTopBlendEnd> g_airTopBlendEnd;
        inline ee::Global<ee::Float, sym::g_airTopBlendOut> g_airTopBlendOut;
        inline ee::Global<ee::Float, sym::g_turnLeast> g_turnLeast;
        inline ee::Global<ee::Float, sym::g_turnMost> g_turnMost;

        // The second turn's angles fall off a share a frame.
        int16_t spunDown(int16_t angle, ee::Float falloff)
        {
            return static_cast<int16_t>(
                angle - ee::toInt(ee::toFloat(angle) * ee::Float(1.0f / 65536.0f) * falloff * ee::Float(65536.0f)));
        }
        // The trick on top's blend counted: the frames while it is on (the
        // air trick's end of a frame).
        int32_t countTopTrick(Rider *rider)
        {
            if (!(rider->topTrickBlend == ee::Float(0.0f)))
                ++levelFrames(rider, kLevelSubState);
            return 1;
        }
        // The trick on top blended in (when on) and the frames counted: the
        // air trick's common end.
        int32_t airTrickEnd(Rider *rider)
        {
            if (rider->flags & kFlagTopTrick)
                rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_airTopBlendIn, ee::Float(1.0f));
            return countTopTrick(rider);
        }
        // To the trick's end phase.
        int32_t toAirTrickEnd(Rider *rider)
        {
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickEnd));
            return 0;
        }
    }

    // Air sub-state 1, the first trick button's air trick: the trick from
    // the taps (none: the plain air), its start (a spin or flip kind turns
    // the second turn's matrix), held while the button is (counted), a trick
    // on top by the taps, the spin or flip turning up to its most, the end.
    // GameCube 0x8008ED74.
    int32_t airTrick(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveReleased) != 0)
            rider->flags &= ~kFlagTrickHeld;
        if (rider->trickMotion == kNoTrick ||
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveReleased) != 0)
        {
            if (rider->trickMotion != kNoTrick)
                rider->flags &= ~kFlagTrickHeld;
        }
        if (rider->flags & kFlagTrickTurn)
        {
            const ee::Float falloff = *g_spinFalloff;
            const int16_t yaw = spunDown(rider->spinAngles[2], falloff);
            const int16_t y = spunDown(rider->spinAngles[1], falloff);
            rider->spinAngles[0] = spunDown(rider->spinAngles[0], falloff);
            rider->spinAngles[1] = y;
            rider->spinAngles[2] = yaw;
        }

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
                motionIndex(rider) = kMotionAirNoTrick;
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                return countTopTrick(rider);
            }
            const uint32_t flags = rider->flags;
            rider->motionFlags |= kMotionRestart;
            motionIndex(rider) = (*g_airTrickMotions)[trick];
            rider->flags = flags & ~kFlagMirrored;
            physics::engine::copyMatrix(rider->spinFrom, rider->right);
            rider->spinAngles[0] = 0;
            rider->spinAngles[1] = 0;
            rider->spinAngles[2] = 0;
            if ((*g_trickKinds)[rider->trick] & kTrickTurnKinds)
            {
                const uint32_t spinning = rider->flags | kFlagTrickTurn;
                rider->flags = spinning;
                if (spinning & kFlagSnapAlign)
                {
                    rider->snapFrames = 0;
                    rider->flags = spinning & ~kFlagSnapAlign;
                }
            }
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickA, kMoveHeld) != 0)
                rider->flags |= kFlagTrickHeld;
            else
                rider->flags &= ~kFlagTrickHeld;
            clearTaps(rider, 1u);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickStarting));
            const uint8_t top = rider->trickMotion;
            rider->motionRate = rider->techniqueStat;
            if (top != kNoTrick)
            {
                rider->topTrickBlend = 0.0f;
                rider->flags |= kFlagTopTrick;
            }
            return countTopTrick(rider);
        }
        case kAirTrickStarting:
        case kAirTrickHeld:
            break;
        case kAirTrickEnd:
            motionIndex(rider) = static_cast<uint16_t>((*g_airTrickMotions)[rider->trick] + 2);
            setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickEnding));
            {
                const uint32_t topOn = rider->flags & kFlagTopTrick;
                rider->motionFlags |= kMotionRestart;
                if (topOn == 0)
                {
                    return countTopTrick(rider);
                }
            }
            rider->topTrickBlend = ee::min(rider->topTrickBlend + *g_airTopBlendEnd, ee::Float(1.0f));
            return countTopTrick(rider);
        case kAirTrickEnding:
        {
            const uint32_t flags = rider->flags;
            if (flags & kFlagMotionEnded)
            {
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
                motionIndex(rider) = kMotionAirTrickOut;
                return 0;
            }
            const ee::Float blend = ee::max(rider->topTrickBlend - *g_airTopBlendOut, ee::Float(0.0f));
            rider->topTrickBlend = blend;
            if (blend == ee::Float(0.0f))
                rider->flags = flags & ~kFlagTopTrick;
            return countTopTrick(rider);
        }
        default:
            return countTopTrick(rider);
        }

        // Started or held: a trick on top by the taps (none yet).
        const uint8_t none = rider->trickMotion;
        if (none == kNoTrick)
        {
            uint8_t taps = 0;
            bool tryTrick = true;
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMovePressed) == 0)
            {
                if (!(rider->flags & kFlagTrickBWaiting) || (taps = rider->tapCount) == 0)
                    tryTrick = false;
            }
            else
                taps = rider->tapCount;
            if (tryTrick)
            {
                if (taps == 0)
                {
                    rider->flags |= kFlagTrickBWaiting;
                }
                else
                {
                    const uint8_t tapIndex = trickFromTaps(rider);
                    const uint8_t top = (*g_topTricks)[rider->trick][tapIndex];
                    if (top != none)
                    {
                        rider->trickMotion = top;
                        clearTaps(rider, 1u);
                        const ee::Float rate = rider->techniqueStat;
                        rider->flags |= kFlagTopTrick;
                        rider->topTrickBlend = 0.0f;
                        rider->topTrickLoopFrames = static_cast<uint8_t>(
                            ee::toInt((ee::Float(1.0f) - rate) * ee::toFloat(kTopTrickCountSpread)) + kTopTrickCountBase);
                        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveTrickB, kMoveHeld) != 0)
                            rider->flags |= kFlagTrickHeld;
                        else
                            rider->flags &= ~kFlagTrickHeld;
                    }
                }
            }
        }

        const uint16_t kind = (*g_trickKinds)[rider->trick] & kTrickTurnKinds;
        if (kind == 0)
        {
            const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
            if (phase == kAirTrickStarting)
            {
                const uint32_t flags = rider->flags;
                if (flags & kFlagMotionEnded)
                {
                    if (!(flags & kFlagTrickHeld))
                    {
                        if (!((*g_trickKinds)[rider->trick] & kTrickCountsHeld))
                            return toAirTrickEnd(rider);
                    }
                    rider->motionFlags |= kMotionRestart;
                    motionIndex(rider) = static_cast<uint16_t>((*g_airTrickMotions)[rider->trick] + 1);
                    setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickHeld));
                    rider->motionRate = rider->techniqueStat;
                }
                return airTrickEnd(rider);
            }
            if (phase != kAirTrickHeld)
                return airTrickEnd(rider);
            // Held: counted while the trick counts held loops; at the
            // motion's end, on while the button is held.
            const uint8_t top = rider->trickMotion;
            if (top != kNoTrick)
            {
                if (((*g_trickFlags)[top] & kTrickCountsHeld) && !(rider->topTrickBlend == ee::Float(0.0f)))
                {
                    const uint8_t every = rider->topTrickLoopFrames;
                    const uint16_t frames = levelFrames(rider, kLevelSubState);
                    if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                    {
                        ++rider->trickLoops;
                    }
                }
            }
            uint32_t flags = rider->flags;
            if (!(flags & kFlagMotionEnded))
                return airTrickEnd(rider);
            uint32_t held = flags & kFlagTrickHeld;
            if ((*g_trickKinds)[rider->trick] & kTrickCountsHeld)
            {
                flags &= ~kFlagTrickHeld;
                rider->flags = flags;
                held = 0;
            }
            if (held == 0)
                return toAirTrickEnd(rider);
            rider->motionFlags |= kMotionRestart;
            return airTrickEnd(rider);
        }

        // A spin or flip: the second turn's first angle by the rate (the
        // kind's), within its least .. most, while the total is under the
        // kind's most.
        const ee::Float stat = rider->spinStat;
        uint32_t most;
        int16_t angle, limit;
        switch (kind - 1u)
        {
        case 1:
        case 2:
        {
            most = kind - 1u == 1 ? 0x1EAAAu : 0x2EAAAu;
            const ee::Float rate = *g_turnRate23;
            const uint16_t was = static_cast<uint16_t>(rider->spinAngles[0]);
            const int32_t least = ee::toInt(*g_turnLeast * stat * rate);
            const int32_t top = ee::toInt(*g_turnMost * stat * rate);
            angle = static_cast<int16_t>(was - least);
            limit = static_cast<int16_t>(top);
            break;
        }
        case 3:
        case 4:
        {
            most = kind - 1u == 3 ? 60074u : 0x1EAAAu;
            const ee::Float rate = kind - 1u == 3 ? *g_turnRate4 : *g_turnRate5;
            const int32_t top = ee::toInt(*g_turnMost * ee::Float(0.75f) * stat * rate);
            const int32_t least = ee::toInt(-(*g_turnLeast * ee::Float(0.75f)) * stat * rate);
            const uint16_t was = static_cast<uint16_t>(rider->spinAngles[0]);
            limit = static_cast<int16_t>(top);
            angle = static_cast<int16_t>(was - least);
            break;
        }
        default: // kind 1 (and 6, 7)
        {
            most = 60074u; // 330 degrees
            const ee::Float rate = *g_turnRate1;
            const int32_t least = ee::toInt(*g_turnLeast * stat * rate);
            const int32_t top = ee::toInt(*g_turnMost * stat * rate);
            const uint16_t was = static_cast<uint16_t>(rider->spinAngles[0]);
            angle = static_cast<int16_t>(was - least);
            limit = static_cast<int16_t>(top);
            break;
        }
        }
        const int32_t total = rider->trickTurned;
        const uint32_t sofar = static_cast<uint32_t>(total < 0 ? -total : total);
        if (sofar >= most)
            return toAirTrickEnd(rider);
        int32_t step = angle < limit ? angle : limit;
        if (step < -int32_t(limit))
            step = -int32_t(limit);
        rider->spinAngles[0] = static_cast<int16_t>(step);
        rider->trickTurned = total + static_cast<int16_t>(step);
        const uint8_t phase = actionState(rider, uint32_t(kLevelPhase));
        if (phase == kAirTrickStarting)
        {
            if (rider->flags & kFlagMotionEnded)
            {
                rider->motionFlags |= kMotionRestart;
                motionIndex(rider) = static_cast<uint16_t>((*g_airTrickMotions)[rider->trick] + 1);
                setActionState(rider, uint32_t(kLevelPhase), uint32_t(kAirTrickHeld));
            }
            return airTrickEnd(rider);
        }
        if (phase != kAirTrickHeld)
            return airTrickEnd(rider);
        if (rider->trickMotion != 0)
            return airTrickEnd(rider);
        // Held, the trick on top 0: counted as the held loops are.
        if (!(rider->topTrickBlend == ee::Float(0.0f)))
        {
            const uint8_t every = rider->topTrickLoopFrames;
            const uint16_t frames = levelFrames(rider, kLevelSubState);
            if (every != 0 && frames % every == 0) // (a zero is the original's trap; never)
                ++rider->trickLoops;
        }
        return airTrickEnd(rider);
    }
    READABLE_FUNCTION(airTrick, Verified);

    namespace
    {
        // The air (airState).
        constexpr uint32_t kMoveBailNow = 16;   // all four shoulder buttons (and 17, none: unused)
        constexpr uint32_t kMoveBailNowB = 17;
        constexpr uint8_t kBailKindAsked = 11;  // Rider +0x178
        constexpr uint32_t kFlagActed = 0x100u; // (inferred) an action taken in the air this frame
        constexpr uint16_t kTrickWaitFrames = 16; // a trick button's wait for taps
        constexpr uint8_t kLipTrickPlant = 69;    // lipTrick's trick that is the lip trick state's (else a stall)
        inline ee::Global<ee::Float, sym::g_airTurnMostShare> g_airTurnMostShare;
        inline ee::Global<ee::Float, sym::g_airTurnSlowA> g_airTurnSlowA;  // the turn buttons against the turn
        inline ee::Global<ee::Float, sym::g_airTurnSlowB> g_airTurnSlowB;
        inline ee::Global<ee::Float, sym::g_airTurnStart> g_airTurnStart;
        inline ee::Global<ee::Float, sym::g_airStickSlowA> g_airStickSlowA; // the stick against the turn
        inline ee::Global<ee::Float, sym::g_airStickSlowB> g_airStickSlowB;
        inline ee::Global<ee::Float, sym::g_bodyPitchFalloff> g_bodyPitchFalloff;
        inline ee::Global<ee::Float, sym::g_airStickTurn> g_airStickTurn;
        inline ee::Global<ee::Float, sym::g_airButtonTurn> g_airButtonTurn;
        inline ee::Global<ee::Float, sym::g_airTurnMost> g_airTurnMost;
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
        // The air's sub-state left for none (its callback run once as none).
        bool leaveAirSubState(Rider *rider)
        {
            if (actionState(rider, uint32_t(kLevelSubState)) == kAirNoTrick)
                return false;
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kAirNoTrick));
            g_airSubStates.get()[rider->actionStates[kLevelSubState]](rider);
            return true;
        }
        int32_t acted(Rider *rider)
        {
            rider->flags |= kFlagActed;
            return 0;
        }
    }

    // Action state 1, the air: landed (two down or up taps with the stick
    // that way: a manual or a nose wheelie), bailed, the bail buttons; the turn buttons or
    // the stick turn (the yaw rate within the spin stat's most); the body's
    // pitch by two taps and the stick; the trick buttons with taps (air
    // sub-states 1 and 2), the grind button (physics 3: a grind, or a stall
    // when slow), the stall button (physics 5: a stall or the lip trick);
    // else the air's sub-state. GameCube 0x8008B880.
    int32_t airState(Rider *rider)
    {
        ButtonRecord *const input = &rider->input;
        // The shared ends: a state set and the frame acted, an action state
        // set.
        auto actedEnd = [rider]()
        {
            acted(rider);
            return 0;
        };
        auto toAction = [rider](uint32_t state)
        {
            setActionState(rider, uint32_t(kLevelAction), state);
            return 0;
        };
        const uint8_t physics = physicsState(rider);
        if (physics == kPhysicsGround)
        {
            if (!(rider->flags & kFlagNoPlayer))
                trickEvent(uint32_t(rider->player), 1u);
            if (rider->tapCount >= 2)
            {
                if (twoTaps(rider, kDirDown))
                {
                    if (ee::Float(0.5f) < inputStick(input, 0u)->y)
                    {
                        rider->flags |= kFlagManual;
                        setPhysicsState(rider, uint32_t(kPhysicsManual));
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionWheelie));
                        return actedEnd();
                    }
                }
                if (twoTaps(rider, kDirUp))
                {
                    if (inputStick(input, 0u)->y < ee::Float(-0.5f))
                    {
                        rider->flags |= kFlagNoseWheelie;
                        setPhysicsState(rider, uint32_t(kPhysicsNoseWheelie));
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionWheelie));
                        return actedEnd();
                    }
                }
            }
            return toAction(kActionLanding);
        }
        if (physics == kPhysicsBail)
        {
            if (!(rider->flags & kFlagNoPlayer))
                trickEvent(uint32_t(rider->player), 0u);
            return toAction(kActionBail);
        }
        if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveBailNow, kMovePressed) != 0 ||
            moveButtons(uint32_t(rider->controllerLayout), input, kMoveBailNowB, kMovePressed) != 0)
        {
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailKindAsked;
            return toAction(kActionBail);
        }

        // The turn: the buttons (one of the two), else the stick.
        const int16_t most = static_cast<int16_t>(ee::toInt(*g_airTurnMost * rider->spinStat * *g_airTurnMostShare));
        const int32_t left = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnLeft, kMoveHeld);
        const int32_t right = moveButtons(uint32_t(rider->controllerLayout), input, kMoveTurnRight, kMoveHeld);
        // Slowing a turn against it: by one share, held to -most, then by
        // another, held to most (the buttons' and the stick's shares).
        auto slowTurn = [&](ee::Float slowA, ee::Float slowB) {
            const int16_t rate = rider->yawRate;
            const int16_t a = scaledTurn(rate, slowA);
            if (a < most ? a < -most : most < -most)
            {
                rider->yawRate = static_cast<int16_t>(-most);
                return;
            }
            const int16_t b = scaledTurn(rider->yawRate, slowB);
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
        // The turn into the total turned and the bars' lean; an alignment
        // still turning takes the turn in (or ends when the turn is more).
        {
            const int16_t rate = rider->yawRate;
            const ee::Float r = ee::toFloat(rate);
            const uint32_t flags = rider->flags;
            const int32_t total = rider->yawTurned + rate;
            rider->yawTurned = total;
            rider->barLean[1] = r * ee::Float(1.0f / 65536.0f) * ee::Float(5.0f);
            const bool aligning = (flags & kFlagSnapAlign) != 0;
            const bool snapping = static_cast<uint16_t>(rider->snapFrames) != 0;
            if (aligning && snapping)
            {
                const int16_t angle = rider->alignAngles[1];
                if (ee::abs(ee::toFloat(angle)) < ee::abs(r))
                {
                    rider->snapFrames = 0;
                    rider->flags = flags & ~kFlagSnapAlign;
                }
                else
                {
                    rider->yawTurned = total + angle;
                }
            }
        }
        // The roll halves a frame, to 0 under half a degree.
        rider->roll = static_cast<int16_t>(rider->roll >> 1);
        if (ee::abs(ee::toFloat(rider->roll)) < ee::Float(float(kTurnStopped)))
            rider->roll = 0;
        // The body's pitch: two down taps and the stick down lean it back,
        // two up taps and the stick up forward; else it falls off.
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

        // The buttons, unless a trick is under way (sub-states 1 and 2
        // before their ends).
        bool buttons = true;
        if (actionState(rider, uint32_t(kLevelSubState)) != kAirNoTrick &&
            actionState(rider, uint32_t(kLevelSubState)) != kAirSubState3)
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
            // A trick button: with taps, its trick (now, or within 16
            // frames of the press).
            auto trickButton = [&](uint32_t move, uint32_t waiting, uint32_t other, uint32_t subState) -> int32_t {
                uint8_t taps;
                if (moveButtons(uint32_t(rider->controllerLayout), input, move, kMovePressed) == 0)
                {
                    const uint32_t flags = rider->flags;
                    if (!(flags & waiting))
                        return 1;
                    taps = rider->tapCount;
                    if (taps == 0)
                    {
                        const uint16_t waited = ++levelFrames(rider, kLevelStep);
                        if (waited >= kTrickWaitFrames)
                        {
                            rider->flags = flags & ~waiting;
                        }
                        return 1;
                    }
                }
                else
                    taps = rider->tapCount;
                if (taps == 0)
                {
                    const uint32_t flags = rider->flags;
                    levelFrames(rider, kLevelStep) = 0;
                    const uint32_t now = (flags | waiting) & ~other;
                    rider->flags = now;
                    return 1;
                }
                if (subState == kAirTrick)
                {
                    if (leaveAirSubState(rider))
                        rider->roll = 0;
                    setActionState(rider, uint32_t(kLevelSubState), subState);
                    acted(rider);
                    return 0;
                }
                leaveAirSubState(rider);
                setActionState(rider, uint32_t(kLevelSubState), subState);
                const uint32_t flags = rider->flags;
                rider->roll = 0;
                rider->flags = flags | kFlagActed;
                return 0;
            };
            if (trickButton(kMoveTrickA, kFlagTrickAWaiting, kFlagTrickBWaiting, kAirTrick) == 0)
                return 0;
            if (trickButton(kMoveTrickB, kFlagTrickBWaiting, kFlagTrickAWaiting, kAirTrickB) == 0)
                return 0;
            if (actionState(rider, uint32_t(kLevelSubState)) != kAirSubState3)
            {
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveGrind, kMoveHeld) != 0 &&
                    actionState(rider, uint32_t(kLevelPhase)) != 0)
                {
                    const uint8_t asked = setPhysicsState(rider, uint32_t(kPhysicsGrindApproach));
                    if (asked == kPhysicsGrindApproach)
                    {
                        leaveAirSubState(rider);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionGrind));
                        acted(rider);
                        return 0;
                    }
                    if (asked == kPhysicsStallApproach)
                    {
                        leaveAirSubState(rider);
                        setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionStall));
                        const uint8_t trick = stallTrick(rider);
                        const uint32_t flags = rider->flags;
                        rider->trick = trick;
                        rider->flags = flags | kFlagActed;
                        return 0;
                    }
                }
                if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveStall, kMovePressed) != 0)
                {
                    if (lipTrick(rider) != kNoTrick)
                    {
                        const uint8_t asked = setPhysicsState(rider, uint32_t(kPhysicsStallApproach));
                        if (asked != kPhysicsStallApproach && asked != kPhysicsLipApproach)
                        {
                            clearTaps(rider, 1u);
                        }
                        else
                        {
                            const uint8_t trick = lipTrick(rider);
                            leaveAirSubState(rider);
                            setActionState(rider, uint32_t(kLevelAction),
                                                        uint32_t(trick != kLipTrickPlant ? kActionStall : kActionLipTrick));
                            rider->trick = trick;
                            acted(rider);
                            return 0;
                        }
                    }
                }
            }
        }
        return runSubStates(rider, g_airSubStates.get(), kActionAir);
    }
    READABLE_FUNCTION(airState, Verified);

    namespace
    {
        // The bail (bailState): its sub-states.
        enum BailSubState : uint8_t
        {
            kBailIn = 0,
            kBailTumbling = 1,
            kBailHandsReach = 2, // the grind button: the hands reach for the ground
            kBailOver = 3,       // back to the start (unless the stick is held a way)
            kBailFade = 4,       // off a wall: the sound, the models hidden 120 frames
            kBailHidden = 5,
            kBailTumblingB = 6,
        };
        constexpr uint32_t kMoveBailLook = 18;  // (inferred) read, its answer unused
        constexpr uint32_t kMoveBailReach = 19; // the grind button (0x10) held
        constexpr uint8_t kContactWallKind = 9;
        constexpr int32_t kBailFadeSound = 37;
        constexpr uint16_t kBailHiddenFrames = 120;
        constexpr uint16_t kBailReachMostFrames = 301; // the hands reach until the bail's 301st frame
        constexpr uint16_t kBailStillFrames = 31;      // slower than g_bailStill this long: over
        constexpr uint32_t kFlagHoldDirection = 0xCu;  // (Rider::flags bits 2, 3: a stick direction held; inferred)
        inline ee::Global<uint32_t, sym::g_bailMostFrames> g_bailMostFrames;
        inline ee::Global<ee::Float, sym::g_bailStill> g_bailStill;
        inline ee::Global<ee::Float, sym::g_bailMotionSpeed> g_bailMotionSpeed;
        constexpr int32_t kTurn30Degrees = 5461;
        constexpr int32_t kTurnEighth = 0x2000;
        // The bail's motions (0x1FD .. 0x207) by the body's lean.
        constexpr uint16_t kMotionBailSlow = 519;
        constexpr uint16_t kMotionBailRollLeft = 517;
        constexpr uint16_t kMotionBailRollRight = 518;
        constexpr uint16_t kMotionBailForward = 0x1FD;
        constexpr uint16_t kMotionBailBack = 0x201;

        bool handsDown(Rider *rider) { return rider->hands[0].holding != 0 || rider->hands[1].holding != 0; }
        int32_t bailOver(Rider *rider) { return changeTo(rider, kLevelSubState, kBailOver); }
        // Back to the start: the models shown, the physics reset, riding.
        int32_t bailRestart(Rider *rider)
        {
            physics::resetPhysics(rider);
            setActionState(rider, uint32_t(kLevelAction), uint32_t(kActionRiding));
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kRidingCoast));
            return changeTo(rider, kLevelPhase, kCoastStarting);
        }
        // The bail's motion as each ends: by speed and the body's lean.
        int32_t bailMotion(Rider *rider)
        {
            if (!(rider->flags & kFlagMotionEnded))
                return 1;
            uint16_t motion = kMotionBailSlow;
            if (*g_bailMotionSpeed <= rider->speed)
            {
                if (zge::angleAcos(ee::abs(rider->right[1])) >= kTurn30Degrees)
                {
                    const int32_t tilt = zge::angleAcos(-rider->up[1]);
                    const int32_t eighth = tilt / kTurnEighth;
                    motion = static_cast<uint16_t>(rider->forward[1] < ee::Float(0.0f) ? eighth + kMotionBailBack
                                                                                       : eighth + kMotionBailForward);
                }
                else
                    motion = rider->right[1] < ee::Float(0.0f) ? kMotionBailRollLeft : kMotionBailRollRight;
            }
            motionIndex(rider) = motion;
            rider->motionFlags |= kMotionRestart;
            return 1;
        }
        // Tumbling: the hands reach (the grind button, with a hand down),
        // over after the most frames or slow for 31.
        int32_t bailTumbling(Rider *rider, bool steep)
        {
            ButtonRecord *const input = &rider->input;
            if (steep)
            {
                moveButtons(uint32_t(rider->controllerLayout), input, kMoveBailLook, kMoveHeld);
                inputStick(input, 0u);
                inputStick(input, 0u);
            }
            if (moveButtons(uint32_t(rider->controllerLayout), input, kMoveBailReach, kMoveHeld) != 0 && handsDown(rider))
                setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailHandsReach));
            if (*g_bailMostFrames < uint32_t(levelFrames(rider, kLevelAction)))
                return bailOver(rider);
            if (!(rider->speed < *g_bailStill))
                return bailMotion(rider);
            if (++levelFrames(rider, kLevelSubState) < kBailStillFrames)
                return bailMotion(rider);
            return bailOver(rider);
        }
    }

    // Action state 7, the bail: in (the camera's), tumbling (against a wall:
    // fading; well above the ground: the stick read), the hands reaching,
    // over (back to the start unless a direction is held), faded (the
    // sound, the models hidden 120 frames, then the start); the motions as
    // each ends by speed and lean. Its sound can wait (_WAITS).
    // GameCube 0x8008D060.
    int32_t bailState(Rider *rider)
    {
        ee::Local<GroundHit> hit;
        switch (actionState(rider, uint32_t(kLevelSubState)))
        {
        case kBailIn:
        {
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailTumbling));
            const uint32_t flags = rider->flags | kFlagMotionEnded;
            rider->flags = flags;
            if (!(flags & kFlagNoPlayer))
                setCameraMode(uint32_t(rider->player), 1u, 1u);
            return bailMotion(rider);
        }
        case kBailTumbling:
        case kBailTumblingB:
        {
            if (rider->contactKind == kContactWallKind)
                return changeTo(rider, kLevelSubState, kBailFade);
            const bool found = groundBelow(rider->position, hit) != 0;
            const ee::Float y = hit->point[1];
            return bailTumbling(rider, found && ee::Float(1.0f) < ee::abs(y - rider->position[1]));
        }
        case kBailHandsReach:
            if (moveButtons(uint32_t(rider->controllerLayout), &rider->input, kMoveGrind, kMoveHeld) != 0 && handsDown(rider))
            {
                const uint16_t frames = levelFrames(rider, kLevelAction);
                rider->flags |= kFlagBailHands;
                if (frames < kBailReachMostFrames)
                    return bailMotion(rider);
                return bailOver(rider);
            }
            rider->flags &= ~kFlagBailHands;
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailTumblingB));
            return bailMotion(rider);
        case kBailOver:
            if (rider->flags & kFlagHoldDirection)
                return bailMotion(rider);
            rider->motionFlags |= kMotionFlagTurnRound;
            return bailRestart(rider);
        case kBailFade:
            soundEvent(kBailFadeSound, rider->position, 3, 0);
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), 0);
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->bikeModel), 0);
            setActionState(rider, uint32_t(kLevelSubState), uint32_t(kBailHidden));
            [[fallthrough]];
        case kBailHidden:
            if (++levelFrames(rider, kLevelSubState) != kBailHiddenFrames)
                return bailMotion(rider);
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), 1);
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->bikeModel), 1);
            return bailRestart(rider);
        default:
            return bailMotion(rider);
        }
    }
    READABLE_FUNCTION_WAITS(bailState, Verified);
}
