#pragma once
// BMX XXX's game records (game.c, records.cpp): the best value of each kind of
// record a level's games set, and who set it. DM2's line (games/dm2/game/
// records.h): the same key, fourteen records a span in place of DM2's 27,
// kept in the round's state (round.h, RoundState::records).
//
// A key packs which record: the mode in bits 24 .. 31, the kind in 16 .. 21,
// the span in 12 .. 15. DM2's single-player mode and entry bits are not read
// here: every span holds one record of each kind, and a mode's own record
// (kinds 0 and 1) is the slot its table names.

#include "round.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // GameRecord::marks: set in this span's play, and its message shown.
    constexpr uint8_t kRecordSet = 0x01;
    constexpr uint8_t kRecordAnnounced = 0x02;

    // The spans of play a record is kept for (the key's span), DM2's.
    enum RecordSpan : uint32_t
    {
        kSavedRecords = 0, // the level's, as saved
        kGameRecords = 1,  // cleared when a game starts
        kRoundRecords = 2, // cleared when a round starts
        kTurnRecords = 3,  // cleared when a player's turn starts
    };

    constexpr uint32_t kRecordKeyModeShift = 24;
    constexpr uint32_t kRecordKeyKindShift = 16;
    constexpr uint32_t kRecordKeyKindMask = 0x3F;
    constexpr uint32_t kRecordKeySpanShift = 12;
    constexpr uint32_t kRecordKeySpanMask = 0xF;
    constexpr uint32_t recordKey(uint32_t mode, uint32_t kind, uint32_t span, uint32_t entry = 0)
    {
        return mode << kRecordKeyModeShift | kind << kRecordKeyKindShift | span << kRecordKeySpanShift | entry;
    }
    // The kinds a mode's own record stands for (its slot comes from the
    // mode's table).
    constexpr uint32_t kFirstKindRecord = 2;
    // The kinds of record, in the order of the objectives' RECORDTYPE words
    // (objectives/record_type.cpp), which the objectives are told them by.
    // A line is a trick with its modifiers; the run of lines is the stack.
    enum RecordKind : uint32_t
    {
        kRecordGameScore = 0,       // SCORE_GAME
        kRecordRunScore = 1,        // SCORE_RUN: the turn's score
        kRecordStackScore = 2,      // SCORE_STACK: the run's points (ScoreBlock::linePoints)
        kRecordTrickScore = 3,      // SCORE_TRICK: a line's points
        kRecordCombo = 4,           // COMBO: the run's tricks
        kRecordSpin = 5,            // SPIN: a line's spin in half turns
        kRecordAirDistance = 6,     // DISTANCE_AIR
        kRecordTapDistance = 7,     // DISTANCE_TAP
        kRecordGrindDistance = 8,   // DISTANCE_GRIND
        kRecordJumpDistance = 9,    // DISTANCE_JUMP
        kRecordManualDistance = 10, // DISTANCE_MANUAL
        kRecordNoseWheelieDistance = 11, // DISTANCE_NOSEWHEELIE
        kRecordSkidDistance = 12,   // DISTANCE_SKID
        kRecordWallrideDistance = 13, // DISTANCE_WALLRIDE
    };

    // The record a key names (always one: BMX XXX keeps a record of every kind).
    ee::Ptr<GameRecord> gameRecord(uint32_t key);
    // A player's value of a kind of record checked against each span's
    // record (rewardKind: the reward display's item when the saved record is
    // beaten; flags: kAnnounceOnce), the objectives told of it.
    void checkGameRecord(uint32_t player, uint32_t kind, uint32_t rewardKind, uint32_t flags, ee::Float value);
    // A player's beaten-record sparkle for the frame (records.cpp).
    void recordEffectFrame(uint32_t player);
}
