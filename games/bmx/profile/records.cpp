// BMX XXX's saved records (records.h): BMX XXX's own code (bmx.c).

#include "records.h"

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/profile/records.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr ee::Ptr<LevelSavedRecords> kSavedRecords{sym::g_savedRecords};
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
    }

    ee::Ptr<SavedRecord> savedRecord(uint32_t key)
    {
        const uint32_t kind = key >> 16 & 0x3Fu;
        LevelSavedRecords &level = kSavedRecords[*g_currentLevel];
        return ee::Ptr<SavedRecord>(level.kinds) + static_cast<ptrdiff_t>(kind);
    }
    READABLE_FUNCTION(savedRecord, Verified);

    ee::Ptr<SavedRecord> levelSavedRecord(uint32_t level, uint32_t kind)
    {
        LevelSavedRecords &records = kSavedRecords[level & 0xFFu];
        return ee::Ptr<SavedRecord>(records.kinds) + static_cast<ptrdiff_t>(kind & 0xFFu);
    }
    READABLE_FUNCTION(levelSavedRecord, Verified);
}
