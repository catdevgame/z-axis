#pragma once
// BMX XXX's saved records (bmx.c, records.cpp): each level's table of best
// values, one for each kind of record the game keeps. BMX XXX's own code.
// The record's fields are the round's (game.c reads and writes them).

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A level's saved record of one kind (0x28 bytes): the best value, who
    // set it and the name they played under.
    struct SavedRecord
    {
        ee::Float value;            // +0x00
        uint8_t character;          // +0x04 the rider character
        uint8_t bike;               // +0x05 the character's RiderCharacter::bike and bikeChoice, copied
        uint8_t bikeChoice;         // +0x06 (unread there too)
        uint8_t flags;              // +0x07 kSavedRecordNew
        char name[0x20];            // +0x08
    };
    static_assert(offsetof(SavedRecord, flags) == 0x07 && offsetof(SavedRecord, name) == 0x08 && sizeof(SavedRecord) == 0x28);
    // SavedRecord::flags bit 0: set in this game.
    constexpr uint8_t kSavedRecordNew = 0x01;

    // The table: 14 kinds of record for each of 13 levels (the twelve, and
    // the park editor's).
    constexpr uint32_t kSavedRecordKinds = 14;
    constexpr uint32_t kSavedRecordLevels = 13;
    struct LevelSavedRecords
    {
        SavedRecord kinds[kSavedRecordKinds];
    };
    static_assert(sizeof(LevelSavedRecords) == 0x230);

    // The current level's record for a record key: the key's bits 16 .. 21
    // are the kind.
    ee::Ptr<SavedRecord> savedRecord(uint32_t key);
    // A level's record of a kind.
    ee::Ptr<SavedRecord> levelSavedRecord(uint32_t level, uint32_t kind);
}
