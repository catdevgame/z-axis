#pragma once
// BMX XXX's level NPCs (game.c, npcs.cpp): the other riders of a level (up to
// three), each riding a replay of
// its own while the player's turn runs.
//
// An NPC's state comes in levels, as a rider's action states and the camera's
// modes do: setting a level's state clears the levels after it and restarts
// its frame count.

#include "ee/ee.h"
#include "zge/scene/model_records.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    constexpr uint32_t kNpcs = 3;
    constexpr uint32_t kNpcStateLevels = 4;

    // An NPC's record (the game's table, kNpcRecordsPerLevel a level:
    // setupNpcs gives each NPC its own): its rider character, its rider's
    // and bike's models' names, and the name of the objective it challenges
    // the player to (the NPC screen shows it).
    struct NpcRecord
    {
        uint8_t character;              // +0x00 an index of the rider characters
        uint8_t _01[3];
        ee::Ptr<const char> riderModel; // +0x04 ("rider.Mika"; none: the level has no more NPCs)
        ee::Ptr<const char> bikeModel;  // +0x08 ("NPR.Bike00")
        char objectiveName[0x20];       // +0x0C
    };
    static_assert(offsetof(NpcRecord, riderModel) == 0x04 && offsetof(NpcRecord, objectiveName) == 0x0C && sizeof(NpcRecord) == 0x2C);
    // A level's records: one for each NPC, and a fourth an NPC takes when
    // it is the current player's own character (with that NPC's objective).
    constexpr uint32_t kNpcRecordsPerLevel = 4;
    constexpr uint32_t kStandInRecord = 3;

    struct Npc
    {
        uint8_t rider;                    // +0x00 its rider's index; kNoNpc ends the list
        uint8_t _01[3];
        // +0x04 a copy of the "NPR available" icon (the objectives' icon
        // model 0), shown over the NPC while it waits to be met (npcsFrame).
        ee::Ptr<zge::Scene> icon;
        uint8_t replay;                   // +0x08 the replay it rides
        uint8_t states[kNpcStateLevels];  // +0x09 (kNpcMet, kNpcRiding, kNpcWaiting at level 0)
        uint8_t _0d;
        int16_t stateFrames[kNpcStateLevels]; // +0x0E the frames each level's state has run
        uint8_t _16[0x18 - 0x16];
        ee::Ptr<const NpcRecord> record;      // +0x18 what the NPC is (npcFacts reads it)
        uint8_t meetings;                 // +0x1C cleared by a new state (DM2's name, inferred there)
        uint8_t goal;                     // +0x1D the goal number of the objective it challenges the player to
        uint8_t _1e[2];
        ee::Ptr<zge::ModelRecord> model;  // +0x20 its rider's model record (the engine's; freeNpcs frees it)
    };
    static_assert(offsetof(Npc, icon) == 0x04 && offsetof(Npc, replay) == 0x08 && offsetof(Npc, states) == 0x09 && offsetof(Npc, stateFrames) == 0x0E);
    static_assert(offsetof(Npc, record) == 0x18 && offsetof(Npc, meetings) == 0x1C && offsetof(Npc, goal) == 0x1D);
    static_assert(offsetof(Npc, model) == 0x20 && sizeof(Npc) == 0x24);
    constexpr uint8_t kNoNpc = 0xFF;

    // Npc::states[0] (inferred from the code that sets them).
    constexpr uint8_t kNpcStanding = 0; // it stands, playing the standing motions (npcStandFrame)
    constexpr uint8_t kNpcMet = 1;     // the player rode up to it (it stands, playing the standing motions)
    constexpr uint8_t kNpcRiding = 2;  // it rides its replay
    constexpr uint8_t kNpcBailed = 3;  // its rider bailed: 180 frames, then kNpcRideOver
    constexpr uint8_t kNpcRideOver = 4; // its replay ended (or it bailed): it stays there while its sub-state is set
    constexpr uint8_t kNpcWaiting = 5; // it waits to ride (set as a turn starts): put back at its replay's start,
                                       // then kNpcStanding

    // How the NPCs' replays run with the game (setNpcPlay): stopped, or
    // played while a turn runs. Two more are handled though nothing passes
    // them: 3 starts them as a turn does, and a play of 2 before a start
    // keeps the NPCs from going back to waiting.
    constexpr uint32_t kNpcPlayStopped = 0;
    constexpr uint32_t kNpcPlayTurn = 1;
    constexpr uint32_t kNpcPlayKept = 2;
    constexpr uint32_t kNpcPlayStartToo = 3;

    // The NPCs told how the game runs; returns `play`.
    uint32_t setNpcPlay(uint32_t play);
    // An NPC's state at a level, and setting it (returns the state).
    uint8_t npcState(uint32_t npc, uint32_t level);
    uint32_t setNpcState(uint32_t npc, uint32_t level, uint32_t state);
    // An NPC's replay set going or stopped by its state and the game's.
    void updateNpcReplay(uint32_t npc);
    // The NPCs' frame: their riders' models, their states' handlers, their
    // icons.
    void npcsFrame();
    // The level's NPCs made: their riders, icons and replays (none in
    // multiplayer or the park editor). Always 1.
    uint32_t setupNpcs();
    // The level's NPCs freed: their replays, icons, riders and models, and
    // the NPR models.
    void freeNpcs();
    void freeNprModels();
    // How near the players' nearest rider is to an NPC (its index in
    // `nearest`, kNoNpc for none).
    enum NpcContact : uint32_t
    {
        kNpcNoneNear = 0, // no rider within 5 units
        kNpcNear = 1,
        kNpcTouching = 2, // within 1.5 units, not moving apart
        kNpcContacts = 3,
    };
    uint32_t nearestRiderToNpc(uint32_t npc, ee::Ptr<uint8_t> nearest);
    // A rider touching an NPC: the lower one bounces off and bails.
    uint32_t bounceRiderOffNpc(uint32_t npc, uint32_t riderIndex);
    // A player meeting a standing NPC (1 when they met).
    uint32_t meetNpc(uint32_t npc, uint32_t player);
    // The states' handlers (g_npcStateFrames): 0 when the state changed
    // and the new state's handler runs too.
    uint32_t npcStandFrame(uint32_t npc);
    uint32_t npcMetFrame(uint32_t npc);
    uint32_t npcRideFrame(uint32_t npc);
    uint32_t npcBailFrame(uint32_t npc);
    uint32_t npcRideOverFrame(uint32_t npc);
    uint32_t npcWaitFrame(uint32_t npc);

    // What the NPC screen, the camera and the rewards read of an NPC
    // (npcFacts fills it).
    struct NpcFacts
    {
        ee::Ptr<const char> name;          // +0x00 its character's name as the leaderboard lists it
        uint8_t character;                 // +0x04 an index of the rider characters
        uint8_t _05[3];
        ee::Ptr<const char> objectiveName; // +0x08 the objective it challenges the player to
        uint8_t goal;                      // +0x0C that objective's goal number
        uint8_t meetings;                  // +0x0D (Npc::meetings)
        uint8_t rider;                     // +0x0E its rider's index
        uint8_t _0f;
    };
    static_assert(offsetof(NpcFacts, objectiveName) == 0x08 && offsetof(NpcFacts, rider) == 0x0E && sizeof(NpcFacts) == 0x10);
    // An NPC's facts filled in.
    void npcFacts(uint32_t npc, ee::Ptr<NpcFacts> facts);
    // The first NPC (0), or kNoNpc when the level has none.
    uint32_t firstNpc();
}
