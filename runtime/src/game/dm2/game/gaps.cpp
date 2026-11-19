// DM2's level gaps (gaps.h). DM2's own code (game.c).

#include "gaps.h"

#include "dm2/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/gaps.cpp.h"

namespace game::dm2::rider::collision
{
    uint64_t *collectTaggedFaces(const uint32_t *words, uint32_t count); // faces.cpp
}

namespace game::dm2
{
    namespace
    {
        // A gap as a built-in level's table has it: its two tags by name
        // (no first name, or an empty one, ends the level's list).
        struct LevelGapDef
        {
            ee::Ptr<const char> from;
            ee::Ptr<const char> to;
            uint32_t flags;
            uint32_t points;
            ee::Ptr<const char> name;
        };
        static_assert(sizeof(LevelGapDef) == 0x14);
        constexpr uint32_t kBuiltInLevels = 8; // the level after them is the park editor's
        inline ee::Global<LevelGapDef[kBuiltInLevels][kLevelGaps], sym::g_levelGapDefs> g_levelGapDefs;

        // The gaps of the level being played, the list the game reads them
        // through, and how many gaps each level has.
        inline ee::Global<LevelGap[kLevelGaps], sym::g_levelGapRecords> g_levelGapRecords;
        inline ee::Global<ee::Ptr<LevelGap>, sym::g_levelGaps> g_levelGaps;
        inline ee::Global<uint8_t[kBuiltInLevels + 1], sym::g_levelGapCounts> g_levelGapCounts;
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;

        bool endsList(const LevelGapDef &def)
        {
            return !def.from || *def.from == 0;
        }
    }

    ee::Ptr<char> gapName(uint32_t gap)
    {
        return (*g_levelGaps)[gap & 0xFF].name;
    }
    READABLE_FUNCTION(gapName, Verified);

    void countLevelGaps()
    {
        for (uint32_t level = 0; level < kBuiltInLevels; ++level)
        {
            uint8_t &count = (*g_levelGapCounts)[level];
            count = 0;
            const LevelGapDef *def = (*g_levelGapDefs)[level];
            while (!endsList(*def))
            {
                ++def;
                ++count;
                if (count >= kLevelGaps)
                    break;
            }
        }
    }
    READABLE_FUNCTION(countLevelGaps, Verified);

    uint32_t initLevelGaps()
    {
        *g_levelGaps = *g_levelGapRecords;
        LevelGap *const gaps = *g_levelGapRecords;
        if (*g_sessionLevel < kBuiltInLevels)
        {
            for (uint32_t i = 0; i < kLevelGaps; ++i)
            {
                gaps[i].flags = 0;
                gaps[i].name[0] = 0;
                gaps[i].from = 0;
                gaps[i].to = 0;
                gaps[i].points = 0;
            }
            for (uint32_t i = 0; i < kLevelGaps; ++i)
            {
                const LevelGapDef &def = (*g_levelGapDefs)[*g_sessionLevel][i];
                if (endsList(def))
                    break;
                LevelGap &gap = gaps[i];
                gap.from = zge::nameHash(def.from);
                gap.to = zge::nameHash(def.to);
                // The faces of each tag are gathered (the lists are not kept here).
                rider::collision::collectTaggedFaces(&gap.from, 1);
                rider::collision::collectTaggedFaces(&gap.to, 1);
                gap.flags = def.flags;
                gap.points = def.points;
                zge::stringCopy(gap.name, def.name);
            }
        }
        else
        {
            // The park editor's level: its gaps were set one by one (setLevelGap).
            uint8_t &count = (*g_levelGapCounts)[*g_sessionLevel];
            count = 0;
            while (gaps[count].from != 0)
            {
                ++count;
                if (count >= kLevelGaps)
                    break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(initLevelGaps, Verified);

    void cleanUpLevelGaps() {}
    READABLE_FUNCTION(cleanUpLevelGaps, Verified);

    uint32_t setLevelGap(uint32_t gap, const LevelGap *from)
    {
        LevelGap &record = (*g_levelGapRecords)[gap];
        record.flags = from->flags;
        record.from = from->from;
        record.to = from->to;
        record.points = from->points;
        return game::tail(zge::stringCopy, record.name, from->name);
    }
    READABLE_FUNCTION(setLevelGap, Verified);

    uint32_t findGap(uint32_t from, uint32_t to)
    {
        if (from == 0 || to == 0 || from == to)
            return kNoGap;
        const uint32_t count = (*g_levelGapCounts)[*g_sessionLevel];
        const ee::Ptr<LevelGap> gaps = *g_levelGaps;
        for (uint32_t i = 0; i < count; ++i)
        {
            const LevelGap &gap = gaps[i];
            if (gap.from == from && gap.to == to)
                return i;
            if ((gap.flags & kGapOneWay) == 0 && gap.to == from && gap.from == to)
                return i;
        }
        return kNoGap;
    }
    READABLE_FUNCTION(findGap, Verified);
}
