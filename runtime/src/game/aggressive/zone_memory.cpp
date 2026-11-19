// The game's own memory calls, thin over the engine's: the zone (a 64 KB
// region of movable blocks, its blocks locked in place as they are taken) and
// the record pools.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/game.h"
#include "zge/memory/dmm.h"
#include "zge/memory/pool.h"
#include "zge/zgui/script.h" // zmemDMMLock

#include <cstdint>
#include "sym/aggressive/zone_memory.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kZoneBytes = 0x00010000u;

        inline ee::Global<ee::Ptr<zge::DmmRegion>, sym::g_zone> g_zone;
        inline ee::Global<int32_t, sym::g_zone> g_zoneWord; // the same, as newZoneMemory returns it

        ee::Ptr<zge::DmmRegion> zone()
        {
            return *g_zone;
        }
    }

    int32_t newZoneMemory()
    {
        *g_zone = zge::newDmmRegion(kZoneBytes);
        return *g_zoneWord;
    }
    READABLE_FUNCTION_WAITS(newZoneMemory, Verified);

    namespace
    {
        namespace tail
        {
        uint32_t freeDmmRegion(ee::Ptr<zge::DmmRegion> region) { return game::tail(zge::freeDmmRegion, region); }
        }
    }

    int32_t freeZoneMemory()
    {
        return static_cast<int32_t>(tail::freeDmmRegion(zone()));
    }
    READABLE_FUNCTION_WAITS(freeZoneMemory, Verified);

    // A zone block, locked: the caller gets its memory.
    int32_t allocateLockedBlock(uint32_t bytes)
    {
        const int32_t handle = static_cast<int32_t>(zge::dmmAllocate(zone(), bytes));
        return static_cast<int32_t>(zge::zmemDMMLock(*g_zone, handle));
    }
    READABLE_FUNCTION(allocateLockedBlock, Verified);

    namespace
    {
        namespace tail
        {
        int dmmRelease(ee::Ptr<zge::DmmRegion> region, uint32_t index) { return game::tail(zge::dmmRelease, region, index); }
        }
    }

    int32_t releaseZoneBlock(ee::Address address)
    {
        const uint32_t handle = static_cast<uint32_t>(zge::dmmHandleOf(zone(), address));
        return static_cast<int32_t>(tail::dmmRelease(zone(), handle));
    }
    READABLE_FUNCTION(releaseZoneBlock, Verified);
}

// The record pools, as the game wraps the engine's: the engine's animator and
// carried objects take their records through these too (zge/game.h), so they
// have the engine's names.
namespace zge
{
    int32_t newPool(uint32_t recordBytes, uint32_t recordsABlock, uint32_t destroyRecord)
    {
        return static_cast<int32_t>(createPool(recordBytes, recordsABlock, destroyRecord));
    }
    READABLE_FUNCTION_WAITS(newPool, Verified);

    int32_t freePool(uint32_t pool)
    {
        return static_cast<int32_t>(destroyPool(ee::Ptr<Pool>(pool)));
    }
    READABLE_FUNCTION_WAITS(freePool, Verified);

    int32_t poolTake(uint32_t pool)
    {
        return static_cast<int32_t>(poolAlloc(ee::Ptr<Pool>(pool)));
    }
    READABLE_FUNCTION_WAITS(poolTake, Verified);

    namespace
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, ee::Address record) { return ee::tailCall<int32_t>(sym::poolFree, pool, record); }
        }
    }

    int32_t poolGive(uint32_t pool, ee::Address record)
    {
        return tail::poolFree(pool, record);
    }
    READABLE_FUNCTION(poolGive, Verified);
}

namespace aggressive
{
    namespace
    {
        namespace tail
        {
        int32_t dmmCompact(ee::Ptr<zge::DmmRegion> region) { return ee::tailCall<int32_t>(sym::dmmCompact, region); }
        }
    }

    // compactGameHeap: no caller reaches it.
    uint32_t compactGameHeap()
    {
        return tail::dmmCompact(*g_zone);
    }
    READABLE_FUNCTION(compactGameHeap, Draft);
}
