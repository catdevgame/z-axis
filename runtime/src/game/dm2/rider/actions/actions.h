#pragma once

// DM2's action states (physics.c, ridingState .. wheelieSubState): what the rider
// does, as the motions show it, over the physics states (rider +0xFF0) that move him.
//
// The action state is a stack of four levels (rider +0x165 ..; rider1's
// setActionState): level 0 the action (g_actionStates' callbacks, ActionState in
// ../rider.h), level 1 its sub-state (the tables below), level 2 the
// sub-state's phase (in, held, out) and level 3 a step within it. Each
// level counts the frames since it was set (+0x16A ..). riderFrame calls
// level 0's callback until one returns non-zero; a callback that changes
// the state returns 0, so the new state's callback runs in the same frame.
// The callbacks pick the motions: rider +0x1224 the motion's index into the
// riders' motion table (g_riderMotions), +0x1220 bit 0 (re)starts it at the next
// motion step.
//
// Fields not (yet) in ../rider.h's Rider are reached through the accessors
// here, by their offsets (the record is being named by several rounds at
// once: rider.h's layout is round 3's to change).

#include "../collision/collision.h"
#include "../rider.h"

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/rider/actions/actions.h.h"

namespace game::dm2::rider
{
    // Round 1's (rider_state.cpp, physics_state.cpp, input.cpp, tricks.cpp).
    uint8_t actionState(const Rider *rider, uint32_t level);
    uint8_t setActionState(Rider *rider, uint32_t level, uint32_t state);
    uint8_t physicsState(const Rider *rider);
    uint8_t setPhysicsState(Rider *rider, uint32_t state);
    ee::Ptr<Rider> riderOf(uint32_t index);
    int32_t buttonTest(const ButtonRecord *record, uint32_t buttons, uint32_t mode);
    int32_t moveButtons(uint32_t layout, const ButtonRecord *record, uint32_t move, uint32_t which);
    void clearTaps(Rider *rider, uint32_t keepStick);
    void addImpulse(Rider *rider, const ee::Float *impulse);

    // The action state levels (setActionState's level).
    enum ActionLevel : uint32_t
    {
        kLevelAction = 0,   // ActionState
        kLevelSubState = 1, // the action's sub-state
        kLevelPhase = 2,    // the sub-state's phase
        kLevelStep = 3,
    };

    // A sub-state's phases (level 2) as most sub-states use them.
    enum Phase : uint8_t
    {
        kPhaseIn = 0,   // entered this frame: start the motion
        kPhaseHeld = 1, // the motion running
        kPhaseOut = 2,
    };

    // The riding sub-states (level 1 of kActionRiding; g_ridingSubStates).
    enum RidingSubState : uint8_t
    {
        kRidingCoast = 0,
        kRidingPedal = 1,
        kRidingManual = 2,
        kRidingCrouch = 3,
        kRidingFakie = 4,
        kRidingNone = 5, // noSubState
    };

    // The air's sub-states (level 1 of kActionAir; g_airSubStates).
    enum AirSubState : uint8_t
    {
        kAirNoTrick = 0,
        kAirTrick = 1,
        kAirTrickB = 2,  // the second trick button's
        kAirSubState3 = 3,
    };

    // A sub-state's callback: non-zero keeps the action this frame. The
    // sub-state tables hold one a sub-state.
    using SubState = ee::FunctionPtr<int32_t(Rider *)>;
    inline ee::Global<SubState, sym::airSubStates> g_airSubStates;
    inline ee::Global<SubState, sym::grindSubStates> g_grindSubStates;
    inline ee::Global<SubState, sym::stallSubStates> g_stallSubStates; // stalls and lip tricks
    inline ee::Global<SubState, sym::wheelieSubStates> g_wheelieSubStates;
    inline ee::Global<SubState, sym::ridingSubStates> g_ridingSubStates;

    // Rider::flags read here (with rider.h's).

    // Rider +0x1220 (motionFlags) bits.
    constexpr uint32_t kMotionRestart = 0x1u; // play the motion (+0x1224) from its start

    // The motions (indices into the riders' motion table) the states name.
    constexpr uint16_t kMotionAirFall = 0x22;  // the air with nothing on (airSubState3's end; inferred)
    constexpr uint16_t kMotionAirLoop = 0x23;  // the air's loop
    constexpr uint16_t kMotionOffWheelie = 0x19;  // off a wheelie into the air
    // Off a grind or stall into the air: a motion a trick, five apart.
    constexpr uint16_t kMotionGrindOffFirst = 0x1A0; // grind tricks from 0x33
    constexpr uint8_t kGrindTrickFirst = 0x33;
    constexpr uint16_t kMotionStallOffFirst = 0x1CD; // stall and lip tricks from 0x3C
    constexpr uint8_t kStallTrickFirst = 0x3C;
    constexpr uint16_t kMotionsPerTrick = 5;

    // The motion's index as the states write it: Rider::motion's low
    // halfword.
    inline uint16_t &motionIndex(Rider *rider)
    {
        return *reinterpret_cast<uint16_t *>(&rider->motion);
    }
    // The frames in a level's state (+0x16A + 2 x level).
    inline uint16_t &levelFrames(Rider *rider, uint32_t level)
    {
        return rider->actionFrames[level];
    }

    // Runs a sub-state table's callback for the rider's level-1 state until
    // one keeps it or the action left `action`: the sub-states' dispatch of
    // the action states.
    int32_t runSubStates(Rider *rider, const SubState *table, uint8_t action);
}

namespace game::dm2::rider
{
    // (approach.cpp, bail.cpp, rails.cpp, turns.cpp)
    void bailBodyRelease(Rider *rider);
    void bailHandContact(Rider *rider, BailHand *hand, const zge::Vec4 *point);
    void bailHands(Rider *rider);
    void closestOnRail(const ee::Float *point, collision::EdgeRef edge, int32_t clamp, ee::Float *out);
    int32_t unfinishedTrickBail(Rider *rider);
}
