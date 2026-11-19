#pragma once
// DM2's level NPCs (game.c, npcs.cpp): the other riders of a level (up to
// four; the "InGameUI.FrameNPC" screen is theirs), each riding a replay of
// its own while the player's turn runs.
//
// An NPC's state comes in levels, as a rider's action states and the camera's
// modes do: setting a level's state clears the levels after it and restarts
// its frame count.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kNpcs = 4;
    constexpr uint32_t kNpcStateLevels = 4;

    // The icons an NPC can have over its head (one shown at a time, by its
    // state: npcsFrame).
    constexpr uint32_t kNpcIcons = 5;

    // A level's NPC as the game defines it (five a level: the four NPCs,
    // then the one that stands in for an NPC who is the player's own
    // character): the rider character it is, its name, the rider and bike
    // models it is made with ("bmxnpcInit: failed to create NPC rider.
    // (rider model "%s", bike model "%s")"), and the goals it gives: of
    // which challenge, from which goal to which.
    struct NpcDefinition
    {
        uint8_t character;              // +0x00
        ee::Ptr<const char> name;       // +0x04
        ee::Ptr<const char> riderModel; // +0x08
        ee::Ptr<const char> bikeModel;  // +0x0C
        uint8_t challenge;              // +0x10
        uint8_t firstGoal;              // +0x11
        uint8_t lastGoal;               // +0x12
    };
    static_assert(offsetof(NpcDefinition, challenge) == 0x10 && sizeof(NpcDefinition) == 0x14);

    struct Npc
    {
        uint8_t rider;                    // +0x00 its rider's index; kNoNpc ends the list
        uint8_t _01[3];
        uint32_t icons[kNpcIcons];        // +0x04 the icons' scene objects (0: none)
        uint8_t replay;                   // +0x18 the replay it rides
        uint8_t states[kNpcStateLevels];  // +0x19 (kNpcMet, kNpcRiding, kNpcWaiting at level 0)
        uint8_t _1d;
        int16_t stateFrames[kNpcStateLevels]; // +0x1E the frames each level's state has run
        ee::Ptr<NpcDefinition> definition; // +0x28
        uint8_t meetings;                 // +0x2C counted up when the player rides up to it; a new state clears it (inferred)
        uint8_t goal;                     // +0x2D the goal of its range it offers the player
    };
    static_assert(offsetof(Npc, icons) == 0x04 && offsetof(Npc, replay) == 0x18 && offsetof(Npc, states) == 0x19 && offsetof(Npc, stateFrames) == 0x1E);
    static_assert(offsetof(Npc, definition) == 0x28 && offsetof(Npc, meetings) == 0x2C && sizeof(Npc) == 0x30);
    constexpr uint8_t kNoNpc = 0xFF;
    constexpr uint8_t kNoNpcReplay = 0xFF; // Npc::replay: none

    // Npc::states[0] (inferred from the code that sets them).
    constexpr uint8_t kNpcStanding = 0; // at its place, offering what its second level says, until its wait is over
    constexpr uint8_t kNpcMet = 1;     // the player rode up to it
    constexpr uint8_t kNpcRiding = 2;  // it rides its replay
    constexpr uint8_t kNpcBailing = 3; // its rider bails (180 frames): it fell in its replay or was run into
    constexpr uint8_t kNpcRideOver = 4; // its ride is over (its replay played out, or after a bail)
    constexpr uint8_t kNpcWaiting = 5; // it waits to ride (set as a turn starts)
    // Npc::states[1] while it stands (npcWaitingFrame decides it):
    // what it offers the player in a level-goals game.
    constexpr uint8_t kNpcOffersGoal = 0;      // a goal of its range not done (Npc::goal)
    constexpr uint8_t kNpcChallengeAhead = 1;  // its challenge is past the player's current one (the NPC
                                               // screen: nothing for the player)
    constexpr uint8_t kNpcGoalsBehind = 2;     // its challenge is behind the player's, or its goals are all done
                                               // (the NPC screen sends the player on)

    // How the NPCs' replays run with the game (setNpcPlay): stopped, or
    // played while a turn runs. Two more are handled though nothing passes
    // them: 3 starts them as a turn does, and a play of 2 before a start
    // keeps the NPCs from going back to waiting.
    constexpr uint32_t kNpcPlayStopped = 0;
    constexpr uint32_t kNpcPlayTurn = 1;
    constexpr uint32_t kNpcPlayKept = 2;
    constexpr uint32_t kNpcPlayStartToo = 3;

    // What the game tells of an NPC (npcInfo): its definition's name,
    // character, challenge and goal range, the goal it offers, its meetings
    // and its rider.
    struct NpcInfo
    {
        ee::Ptr<const char> name; // +0x00
        uint8_t character;        // +0x04
        uint8_t challenge;        // +0x05
        uint8_t firstGoal;        // +0x06
        uint8_t lastGoal;         // +0x07
        uint8_t goal;             // +0x08
        uint8_t meetings;         // +0x09
        uint8_t rider;            // +0x0A
    };
    static_assert(offsetof(NpcInfo, goal) == 0x08 && offsetof(NpcInfo, rider) == 0x0A);
    void npcInfo(uint32_t npc, ee::Ptr<NpcInfo> info);
    // The first of the level's NPCs whose goals are of `challenge`, or kNoNpc.
    uint8_t npcForChallenge(uint32_t challenge);

    // The rider of the first player within reach of the NPC (its drawn
    // body within 1.5) and riding into it (their velocities closing), or
    // kNoNpc.
    uint8_t riderTouchingNpc(uint32_t npc);
    // The player meets the first NPC standing within 4 of the player's
    // rider: the NPC is put back at its start (npcWaitingFrame) and met
    // (kNpcMet, keeping what it offers), and the rider put before it, facing
    // it. Returns the NPC, or kNoNpc for none.
    uint8_t meetNpc(uint32_t player);
    // An NPC and a player's rider run into each other: the lower of the two
    // (y grows downwards) loses its speed along the line between them, takes
    // the other's closing speed, and bails (the player's rider with bail
    // kind 3, the NPC by kNpcBailing). Returns the rider that bails.
    uint8_t npcCollision(uint32_t npc, uint32_t rider);
    // The NPC state handlers of standing (kNpcStanding), riding (kNpcRiding)
    // and of a bail (kNpcBailing): 1 while the state goes on, 0 once it
    // changed.
    uint32_t npcStandingFrame(uint32_t npc);
    // kNpcRideOver's handler: the NPC goes back to its replay's start (by
    // the steps in its second level), then waits. 1 while it is on its way.
    uint32_t npcRideOverFrame(uint32_t npc);
    // kNpcWaiting's handler: the NPC put back at its replay's start, its
    // rider reset; in a level-goals game it then stands, offering what its
    // challenge and goals leave. Answers 0 (the new state's handler runs).
    uint32_t npcWaitingFrame(uint32_t npc);
    uint32_t npcRidingFrame(uint32_t npc);
    uint32_t npcBailFrame(uint32_t npc);
    // The level's NPCs made as it loads (a ProQuest level's goals game of
    // one player). Answers 1.
    uint32_t setupNpcs();
    // The NPCs told how the game runs; returns `play`.
    uint32_t setNpcPlay(uint32_t play);
    // An NPC's state at a level, and setting it (returns the state).
    uint8_t npcState(uint32_t npc, uint32_t level);
    uint32_t setNpcState(uint32_t npc, uint32_t level, uint32_t state);
    // An NPC's replay set going or stopped by its state and the game's.
    void updateNpcReplay(uint32_t npc);
    // The level's NPCs freed as it ends (their replays and riders).
    void freeNpcs();
    // State kNpcMet's handler (answers 1: the state stays).
    uint32_t npcMetFrame(uint32_t npc);
    // The NPCs' frame while their replays play: each one's riders shown or
    // hidden, its state's handler run, and the icon over its head for its
    // state placed (bobbing and turning); the NPCs' frame count goes on.
    void npcsFrame();
}
