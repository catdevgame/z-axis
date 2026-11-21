#pragma once
// DM2's game records (game.c, records.cpp): the best value of each kind of
// record (a score, a trick record) a level's games set, and who set it.
//
// The game keeps four spans of them, side by side (inferred from when each is
// cleared): the level's saved records (what its high-score tables hold, read
// in when the level is set up), the game's, the round's and the turn's. A
// key packs which record: the game (its multiplayer and single-player modes),
// the record's kind, the span and, for a single-player game's own records,
// which one.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/records.h.h"

namespace game::dm2
{
    // A record: its best value, the player who set it, and its marks.
    struct GameRecord
    {
        ee::Float value;
        uint8_t player;
        uint8_t marks; // kRecordSet, kRecordAnnounced
    };
    static_assert(offsetof(GameRecord, player) == 0x04 && offsetof(GameRecord, marks) == 0x05);
    static_assert(sizeof(GameRecord) == 0x08);

    // GameRecord::marks: set in this span's play, and its message shown.
    constexpr uint8_t kRecordSet = 0x01;
    constexpr uint8_t kRecordAnnounced = 0x02;

    // The spans of play a record is kept for (the key's span).
    enum RecordSpan : uint32_t
    {
        kSavedRecords = 0, // the level's, as saved
        kGameRecords = 1,  // cleared when a game starts
        kRoundRecords = 2, // cleared when a round starts
        kTurnRecords = 3,  // cleared when a player's turn starts
    };
    constexpr uint32_t kRecordSpans = 4;
    // The kinds of record every game keeps (the key's kind): the best game
    // score and the best score of one turn. A multiplayer mode from 6 has a
    // kind of its own.
    constexpr uint32_t kGameScoreRecord = 0;
    constexpr uint32_t kTurnScoreRecord = 1;
    // The trick records (the scoring checks them: checkGameRecord).
    constexpr uint32_t kRunPointsRecord = 2;       // a run's points
    constexpr uint32_t kLinePointsRecord = 3;      // one line's points
    constexpr uint32_t kRunTricksRecord = 4;       // the tricks in a run
    constexpr uint32_t kSpinRecord = 5;            // a line's spin, in half turns
    constexpr uint32_t kAirHeightRecord = 6;       // an air's height over its take-off
    constexpr uint32_t kHoldRecord = 7;            // tricks 0x43 .. 0x45 held: the greatest groundHeight under one (ScoreBlock::heldTrickHeight)
    constexpr uint32_t kGrindDistanceRecord = 8;
    constexpr uint32_t kContactDistanceRecord = 9; // the level distance between the wheels' two contacts at a landing
    constexpr uint32_t kWheelieDistanceRecord = 10;
    constexpr uint32_t kNoseWheelieDistanceRecord = 11;
    constexpr uint32_t kSkidRecord = 12;
    constexpr uint32_t kRecordKinds = 13;
    constexpr uint32_t kRecordsPerSpan = 27;
    constexpr uint32_t kMultiplayerModeCount = 15;

    // A record's key: the multiplayer mode in bits 24 .. 31, the
    // single-player mode in 22 .. 23, the kind in 16 .. 21, the span in
    // 12 .. 15, the entry (a single-player game's own records) in 0 .. 11.
    constexpr uint32_t kRecordKeyModeShift = 24;
    constexpr uint32_t kRecordKeySingleShift = 22;
    constexpr uint32_t kRecordKeySingleMask = 0x3;
    constexpr uint32_t kRecordKeyKindShift = 16;
    constexpr uint32_t kRecordKeyKindMask = 0x3F;
    constexpr uint32_t kRecordKeySpanShift = 12;
    constexpr uint32_t kRecordKeySpanMask = 0xF;
    constexpr uint32_t kRecordKeyEntryMask = 0xFFF;

    // Where a span's records are laid out (the game's four spans here and,
    // a level at a time, the saved game's: action/saved_records.h): the
    // single-player games' by the key's entry, then the multiplayer modes'
    // own, then one a kind.
    constexpr uint32_t kSinglePlayerRecords = 0;
    constexpr uint32_t kMode1Records = 3;   // by single-player mode
    constexpr uint32_t kMode2Record = 7;
    constexpr uint32_t kMode3Record = 8;
    constexpr uint32_t kMode4Records = 9;   // kind 1, then kind 0
    constexpr uint32_t kMode5Record = 11;
    constexpr uint32_t kMode14Records = 12; // by single-player mode
    constexpr uint32_t kKindRecords = 14;   // kind k (from 2) at 14 + k
    // The first kind that has a record of its own whatever the mode.
    constexpr uint32_t kFirstOwnKind = 2;

    // The kind of record a multiplayer mode keeps (modes 6 .. 13).
    inline ee::Global<uint8_t[kMultiplayerModeCount], sym::g_modeRecordKinds> g_modeRecordKinds;

    constexpr uint32_t recordKey(uint32_t multiplayerMode, uint32_t singlePlayerMode, uint32_t kind, uint32_t span,
                                 uint32_t entry = 0)
    {
        return multiplayerMode << kRecordKeyModeShift | singlePlayerMode << kRecordKeySingleShift
             | kind << kRecordKeyKindShift | span << kRecordKeySpanShift | entry;
    }

    // The record a key names, or 0 where its game keeps none of that kind.
    ee::Ptr<GameRecord> gameRecord(uint32_t key);

    // checkGameRecord's flags.
    constexpr uint32_t kAnnounceOnce = 0x1; // no reward shown for a saved record already announced

    // A value a player reached, checked against the game's record of its
    // kind in every span: where it is higher it becomes the record. A
    // saved record beaten shows `rewardKind` on the reward display (free
    // rides and park tests keep no saved records). Then the level's goals
    // take the value.
    void checkGameRecord(uint32_t player, uint32_t kind, uint32_t rewardKind, uint32_t flags, ee::Float value);
}
