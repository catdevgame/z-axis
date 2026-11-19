// DM2's park editor: the editor's record pools. Each of the editor's modules
// makes a pool for its records when the editor is set up and names it by
// the index newEditPool gave; all are destroyed together.
#pragma once

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // A pool of the editor's table: the engine's pool and its records' size
    // (a record handed out is cleared over that many bytes).
    struct EditPool
    {
        ee::Ptr<zge::Pool> pool;
        uint32_t recordBytes;
    };
    static_assert(sizeof(EditPool) == 8);

    // Every pool of the table destroyed and the table emptied.
    void destroyEditPools();
    void shutDownEditPools();

    // A pool of records of `recordBytes`, growing `recordsABlock` at a time:
    // its index in the table.
    int8_t newEditPool(uint32_t recordBytes, uint32_t recordsABlock);

    // A zeroed record of pool `pool`; a record handed back (`slot` is the
    // word its address is kept in: cleared; nothing is done when it holds 0).
    uint32_t editPoolAllocate(int8_t pool);
    void editPoolFree(int8_t pool, ee::Address slot);
}
