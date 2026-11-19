#pragma once

// The saved records (saved_records.cpp): each level's best values as the
// saved game keeps them, with who set them. The game's own records
// (game/records.h) start from these and are written back when beaten.
// DM2's own code (action.c).

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // A saved record: its best value, the character who set it and the name
    // entered for it.
    struct SavedRecord
    {
        ee::Float value;
        uint8_t character;
        uint8_t bikeModel; // the character's bike, as RiderCharacter keeps it, when the record was set
        uint8_t bike;
        uint8_t flags;  // kSavedRecordSet
        char name[12];
    };
    static_assert(offsetof(SavedRecord, character) == 0x04 && offsetof(SavedRecord, flags) == 0x07);
    static_assert(offsetof(SavedRecord, name) == 0x08 && sizeof(SavedRecord) == 0x14);

    // SavedRecord::flags: a player has set this record.
    constexpr uint8_t kSavedRecordSet = 0x01;

    // The session level's saved record for a record key (game/records.h:
    // the key's span is not used), or 0 where its game keeps none of that
    // kind.
    ee::Ptr<SavedRecord> savedRecord(uint32_t key);
}
