#pragma once
// BMX XXX's replays (replays.cpp): the recorder's slots, each a recording of
// a rider (its pad's input, or its motion), made by the game's set-up and
// started and stopped by the round's phases. DM2's line
// (game/dm2/replay/replays.h), with a recording format and the rider's
// weapon added.
//
// Names are DM2's where the code is its line; "(inferred)" marks a meaning
// read from use only.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/assets/zfile_library.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a new replay is made for: its format, its flags, the rider it
    // follows, and the bytes its recording may take (0: 4 KB).
    struct ReplaySetup
    {
        uint8_t format; // ReplayFormat
        uint8_t flags;  // kReplayPlaysOnly, kReplayMultiplayerSetUp
        uint8_t rider;  // kReplayNoRider: the round's replay, of every rider
        uint8_t _03;
        uint32_t weapon; // +0x04 gameSetup puts the player's weapon here (newReplay does not read it)
        uint32_t dataBytes;
    };
    static_assert(offsetof(ReplaySetup, weapon) == 4 && offsetof(ReplaySetup, dataBytes) == 8);
    constexpr uint8_t kReplayNoRider = 0xFF;

    // What a replay records. (inferred) A motion recording's entries hold
    // 16 and 12 bytes where an input recording's hold the pad's two; the
    // level's replayed riders are made with it.
    enum ReplayFormat : uint8_t
    {
        kInputReplay = 0,
        kMotionReplay = 1,
    };

    // ReplaySetup::flags (DM2's): a replay that plays only never records and
    // keeps no level goals; one made by a multiplayer set-up keeps none.
    constexpr uint8_t kReplayPlaysOnly = 0x02;
    constexpr uint8_t kReplayMultiplayerSetUp = 0x04;
    constexpr uint8_t kReplayRecordsPositions = 0x08; // an input replay: the rider's position with every entry

    // A replay's state (setReplayState; DM2's): made, recording, played
    // back, played to its last recorded frame.
    constexpr uint8_t kReplayIdle = 0;
    constexpr uint8_t kReplayRecording = 1;
    constexpr uint8_t kReplayPlaying = 2;
    constexpr uint8_t kReplayPlayedOut = 4;

    constexpr uint8_t kNoReplay = 0xFF;

    // A buffer for a replay's record (the heap's), and giving one back.
    uint32_t allocReplayBuffer(uint32_t bytes);
    void freeReplayBuffer(ee::Address buffer);

    // Every replay slot emptied; every replay freed; one freed and its slot emptied.
    void clearReplays();
    void freeReplays();
    void freeReplay(uint8_t replay);

    // A replay made for `setup`: its slot, or kNoReplay.
    uint8_t newReplay(ee::Ptr<ReplaySetup> setup);

    // A replay's state set (its start, its end); returns the state it has.
    uint8_t setReplayState(uint8_t replay, uint32_t state);
    uint8_t replayState(uint8_t replay);

    // The rider a replay follows and the weapon recorded with it; the
    // setters return what they set.
    uint8_t setReplayRider(uint8_t replay, uint8_t rider);
    uint8_t replayRider(uint8_t replay);
    uint32_t setReplayWeapon(uint8_t replay, uint32_t weapon);
    uint32_t replayWeapon(uint8_t replay);

    // What the recording was made in and of: its format, the level, and
    // whether its player watched (the player set-up's byte, kept with it).
    uint8_t replayFormat(uint8_t replay);
    uint8_t replayLevel(uint8_t replay);
    uint8_t replayPlayerWatches(uint8_t replay);

    // The rider as the recording started: its record's first bytes, its
    // position and its rows.
    ee::Ptr<uint8_t> replayRiderHead(uint8_t replay);
    ee::Ptr<ee::Float> replayRiderPosition(uint8_t replay);
    ee::Ptr<ee::Float> replayRiderRows(uint8_t replay);

    // Where a replay's saved part (what a memory card or the game's library
    // holds) starts, and its bytes.
    void replaySavedPart(uint8_t replay, ee::Ptr<uint32_t> start, ee::Ptr<uint32_t> bytes);
    // The library's current asset (one of the game's own replays) read to
    // where a replay's saved part goes, and the record's layout set for it.
    void readLibraryReplay(ee::Ptr<zge::ZfileLibrary> library, ee::Address savedPart);

    // A recording replay's next entry started: its header written (the
    // repeats so far and the kind), and true; when the entry does not fit the
    // replay goes idle instead, and false.
    bool beginReplayEntry(uint8_t replay, uint32_t kind);
    // Every replay's frame: a recording replay records, a playing one feeds
    // its rider.
    void replayUpdate();
}
