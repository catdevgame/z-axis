// DM2's park editor: the editor's record pools (edit_pools.h). DM2's own
// code (its editor block).

#include "edit_pools.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/editor/edit_pools.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<int8_t, sym::g_editPoolCount> g_editPoolCount;

        EditPool &editPool(int32_t index)
        {
            return ee::Ptr<EditPool>(sym::g_editPools)[index];
        }
    }

    void destroyEditPools()
    {
        for (int32_t index = 0; index < *g_editPoolCount; ++index)
            zge::destroyPool(editPool(index).pool);
        *g_editPoolCount = 0;
    }
    READABLE_FUNCTION(destroyEditPools, Verified);

    int8_t newEditPool(uint32_t recordBytes, uint32_t recordsABlock)
    {
        editPool(*g_editPoolCount).recordBytes = recordBytes;
        const ee::Ptr<zge::Pool> pool(zge::createPool(recordBytes, recordsABlock, 0));
        const int8_t index = *g_editPoolCount;
        editPool(index).pool = pool;
        *g_editPoolCount = static_cast<int8_t>(index + 1);
        return index;
    }
    READABLE_FUNCTION(newEditPool, Verified);

    uint32_t editPoolAllocate(int8_t pool)
    {
        const uint32_t record = zge::poolAlloc(editPool(pool).pool);
        zge::memoryFill(record, 0, editPool(pool).recordBytes);
        return record;
    }
    READABLE_FUNCTION(editPoolAllocate, Verified);

    void editPoolFree(int8_t pool, ee::Address slot)
    {
        const ee::Ptr<uint32_t> record(static_cast<uint32_t>(slot));
        if (*record)
        {
            zge::poolFree(editPool(pool).pool, *record);
            *record = 0;
        }
    }
    READABLE_FUNCTION(editPoolFree, Verified);

    void shutDownEditPools()
    {
        game::tail(destroyEditPools);
    }
    READABLE_FUNCTION(shutDownEditPools, Verified);
}
