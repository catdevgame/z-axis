#pragma once
// DM2's replays (replays.cpp): the recorder's slots, each a recording of the
// game (a rider's run, or the whole round's), made by the game's set-up and
// the round's phases.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/assets/zfile_library.h"

#include <cstdint>

namespace game::dm2
{
    // What a new replay is made for: its flags (kReplayPlaysOnly,
    // kReplayMultiplayerSetUp), the rider it follows, and the bytes its
    // recording may take (0: 4 KB).
    struct ReplaySetup
    {
        uint8_t flags;
        uint8_t rider;
        uint8_t _02[2];
        uint32_t dataBytes;
    };
    static_assert(sizeof(ReplaySetup) == 8);

    // ReplaySetup::flags. The level's set-up makes the replay it loads to be
    // watched with one of them: a single-player game's plays only (it never
    // records, and keeps no level goals); a multiplayer game's keeps none.
    constexpr uint8_t kReplayPlaysOnly = 0x02;
    constexpr uint8_t kReplayMultiplayerSetUp = 0x04;
    constexpr uint8_t kReplayRecordsPositions = 0x08; // the rider's position after every entry (replayUpdate)

    // ReplaySetup::rider for a replay of no one rider (the whole round's).
    constexpr uint8_t kReplayNoRider = 0xFF;

    // A replay's state (setReplayState): made, recording (replayUpdate counts its
    // frames), played back (the rider put back as the recording started).
    constexpr uint8_t kReplayIdle = 0;
    constexpr uint8_t kReplayRecording = 1;
    constexpr uint8_t kReplayPlaying = 2;
    constexpr uint8_t kReplayPlayedOut = 4; // played to its last recorded frame (replayUpdate)

    // A replay's state set (its start, its end); returns the state set.
    uint8_t setReplayState(uint8_t replay, uint32_t state);

    // A replay made for `setup`: its slot, or 0xFF.
    uint8_t newReplay(ee::Ptr<ReplaySetup> setup);

    // Every replay slot emptied (the level's set-up, before its replays are
    // made).
    void clearReplays();
    // The rider a replay follows, and setting it (returns the rider).
    uint8_t replayRider(uint8_t replay);
    uint8_t setReplayRider(uint8_t replay, uint8_t rider);

    // A buffer for a replay's record of `bytes` (one of the replays' own, or
    // the heap's), and giving one back.
    uint32_t allocReplayBuffer(uint32_t bytes);
    void freeReplayBuffer(ee::Address buffer);
    // A replay freed and its slot emptied; every replay freed.
    void freeReplay(uint8_t replay);
    void freeReplays();

    // The level and single-player game a replay was made in.
    uint8_t replayLevel(uint8_t replay);
    uint8_t replaySinglePlayerMode(uint8_t replay);
    // Where a replay's saved part (what a memory card or the game's library
    // holds) starts, and its bytes.
    void replaySavedPart(uint8_t replay, ee::Ptr<uint32_t> start, ee::Ptr<uint32_t> bytes);
    // The library's current asset (one of the game's own replays) read into `dst`.
    void readLibraryReplay(ee::Ptr<zge::ZfileLibrary> library, ee::Address dst);

    // Every replay's frame: a recording one writes its rider's input, a
    // playing one feeds it back.
    void replayUpdate();

    // What a replay is doing, and its frames since it started.
    uint8_t replayState(uint8_t replay);
    uint32_t replayFrame(uint8_t replay);
    // The rider as the recording started: its record's first bytes, its
    // position and its rows.
    ee::Ptr<uint8_t> replayRiderHead(uint8_t replay);
    ee::Ptr<ee::Float> replayRiderPosition(uint8_t replay);
    ee::Ptr<ee::Float> replayRiderRows(uint8_t replay);
}
