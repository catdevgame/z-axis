#pragma once
// Create-a-model's part locks (part_locks.cpp): which parts of a model rule
// each other out, one table for each gender, read from
// "assets\create\MaleLock.csv" and "FemLock.csv" (a line a part: its name,
// then a cell a part, "X" where the two lock). BMX XXX's own code.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // A part's row: its name's hash, whether it locks any part, and a bit a
    // part (by the table's order) it locks; the table is symmetric.
    constexpr uint32_t kLockParts = 140;
    struct PartLock
    {
        uint32_t partHash;  // +0x00 nameHash of the part's name
        uint32_t locksAny;  // +0x04 1 once a cell of its row or column has an X
        uint8_t locks[0x14]; // +0x08 bit n: it locks the table's part n
    };
    static_assert(sizeof(PartLock) == 0x1C);

    // The two tables made (on the heap, cleared) and read (inferred name).
    void loadPartLockTables();
}
