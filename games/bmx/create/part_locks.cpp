// Create-a-model's part locks (part_locks.h). BMX XXX's own code.

#include "part_locks.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"

#include <cstdint>
#include "sym/bmx/create/part_locks.cpp.h"

namespace game::bmx
{
    namespace
    {
        inline ee::Global<ee::Ptr<PartLock>, sym::g_malePartLocks> g_malePartLocks;
        inline ee::Global<ee::Ptr<PartLock>, sym::g_femalePartLocks> g_femalePartLocks;
        constexpr uint32_t kTableBytes = kLockParts * sizeof(PartLock);
        constexpr uint32_t kMaleLockFile = sym::g_textMaleLockFile;     // "assets\\create\\MaleLock.csv"
        constexpr uint32_t kFemaleLockFile = sym::g_textFemaleLockFile; // "assets\\create\\FemLock.csv"
        constexpr uint32_t kNameBytes = 0x20;
        constexpr char kLockMark = 'X';

        void lockParts(ee::Ptr<PartLock> table, uint32_t row, uint32_t column)
        {
            table[row].locksAny = 1;
            table[row].locks[column >> 3] |= static_cast<uint8_t>(1u << (column & 7));
            table[column].locksAny = 1;
            table[column].locks[row >> 3] |= static_cast<uint8_t>(1u << (row & 7));
        }

        // One table read: a row a line, its cells after the name, each
        // "X," (the two parts lock) or "," (they do not).
        void readLockTable(uint32_t path, ee::Ptr<PartLock> &table, ee::Local<char[kNameBytes]> &name)
        {
            if (!zge::beginScript(path))
                return;
            uint32_t row = 0;
            while (zge::nextToken(name, kNameBytes))
            {
                const uint32_t hash = zge::nameHash(name);
                table[row].partHash = hash;
                table[row].locksAny = 0;
                const ee::Ptr<char> cells = zge::restOfTokenLine();
                ee::Ptr<char> cell = cells + 1;
                uint32_t column = 0;
                for (char mark = cell[0]; mark != 0; mark = cell[0])
                {
                    ++cell;
                    if (mark == kLockMark)
                    {
                        lockParts(table, row, column);
                        ++cell;
                    }
                    ++column;
                }
                zge::skipTokenLine();
                ++row;
            }
            zge::endScript();
        }
    }

    void loadPartLockTables()
    {
        ee::Local<char[kNameBytes]> maleName;
        ee::Local<char[kNameBytes]> femaleName;
        *g_malePartLocks = ee::Ptr<PartLock>(zge::memoryFill(zge::heapAllocate(kTableBytes), 0, kTableBytes));
        *g_femalePartLocks = ee::Ptr<PartLock>(zge::memoryFill(zge::heapAllocate(kTableBytes), 0, kTableBytes));
        readLockTable(kMaleLockFile, *g_malePartLocks, maleName);
        readLockTable(kFemaleLockFile, *g_femalePartLocks, femaleName);
    }
    READABLE_FUNCTION_WAITS(loadPartLockTables, Verified);
}
