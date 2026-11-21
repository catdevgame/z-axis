// The saved records (saved_records.h): DM2's own code (action.c).

#include "saved_records.h"
#include "../game/records.h"

#include "ee/abi.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/action/saved_records.cpp.h"

namespace game::dm2
{
    namespace
    {
        // Every level's saved records (sym::g_savedRecords), a level's laid out
        // as a span of the game's records is (game/records.cpp).
        // The level of the session.
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;

        ee::Ptr<SavedRecord> recordAt(uint32_t index)
        {
            return ee::Ptr<SavedRecord>(sym::g_savedRecords)
                 + static_cast<ptrdiff_t>(*g_sessionLevel * kRecordsPerSpan + index);
        }
    }

    ee::Ptr<SavedRecord> savedRecord(uint32_t key)
    {
        const uint32_t entry = key & kRecordKeyEntryMask;
        const uint32_t kind = (key >> kRecordKeyKindShift) & kRecordKeyKindMask;
        const uint32_t single = (key >> kRecordKeySingleShift) & kRecordKeySingleMask;
        const uint32_t mode = key >> kRecordKeyModeShift;
        if (kind >= kFirstOwnKind)
            return recordAt(kKindRecords + kind);
        switch (mode)
        {
        case 0:
            return recordAt(kSinglePlayerRecords + entry);
        case 1:
            return recordAt(kMode1Records + single);
        case 2:
            return kind == 1 ? recordAt(kMode2Record) : ee::Ptr<SavedRecord>();
        case 3:
            return kind == 1 ? recordAt(kMode3Record) : ee::Ptr<SavedRecord>();
        case 4:
            if (kind == 0)
                return recordAt(kMode4Records + 1);
            if (kind == 1)
                return recordAt(kMode4Records);
            return ee::Ptr<SavedRecord>();
        case 5:
            return kind == 1 ? recordAt(kMode5Record) : ee::Ptr<SavedRecord>();
        // The multiplayer modes 6 .. 13 keep one record each: the kind the
        // game's records give the mode.
        case 6:
            return recordAt(kKindRecords + 8);
        case 7:
            return recordAt(kKindRecords + 7);
        case 8:
            return recordAt(kKindRecords + 6);
        case 9:
            return recordAt(kKindRecords + 4);
        case 10:
            return recordAt(kKindRecords + 5);
        case 11:
            return recordAt(kKindRecords + 9);
        case 12:
            return recordAt(kKindRecords + 10);
        case 13:
            return recordAt(kKindRecords + 11);
        case 14:
            return recordAt(kMode14Records + single);
        default:
            return ee::Ptr<SavedRecord>();
        }
    }
    READABLE_FUNCTION(savedRecord, Verified);
}
