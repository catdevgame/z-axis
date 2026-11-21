#include "pool.h"
#include "alignment.h"

#include "zge/math/box.h"
#include "zge/scene/scene.h"
#include "ee/abi.h"
#include "ee/mirror.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/container/pointer_array.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <array>
#include "sym/zge/memory/pool.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kAlignMask = ~(kAllocationAlignment - 1u); // a block's alignment, as the allocator takes it
        constexpr uint32_t kPoolBlockAlignment = 0x40u; // allocateAligned64's: whole 64-byte units on a 64-byte boundary

        // A record's slot: its link and the record rounded up to 16 bytes.
        uint32_t recordStride(const Pool &pool) { return roundUp16(pool.recordBytes) + kPoolLinkBytes; }

        // The record a link is in front of.
        uint32_t recordAfter(uint32_t link) { return link + kPoolLinkBytes; }
        // A link of the pool's, its two links read as addresses.
        PoolLink &linkAt(uint32_t link) { return *ee::Ptr<PoolLink>(link); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        // FlushCache(0): the data cache written back.
        void flushCache(uint32_t mode) { ee::game(sym::flushCache, mode); }
    }
    uint32_t allocateAligned(uint32_t alignment, uint32_t bytes); // regions.cpp: a heap block on an alignment

    void poolGrow(ee::Ptr<Pool> pool)
    {
        const uint32_t stride = recordStride(*pool);
        const uint32_t bytes = stride * pool->recordsPerBlock + kPoolLinkBytes;
        uint32_t block;
        if constexpr (game::readable::kIsDm2) // ZGE2 has no allocator field: allocateAligned64's step, written in
            block = allocateAligned(kPoolBlockAlignment, (bytes + kPoolBlockAlignment - 1u) & ~(kPoolBlockAlignment - 1u));
        else
            block = pool->allocate(bytes, kAlignMask);
        if (pool->memoryFlags & kPoolUncached)
        {
            block = ee::uncachedMirror(block);
            flushCache(0);
        }
        if (pool->memoryFlags & kPoolUncachedAccelerated)
        {
            block = ee::uncachedAcceleratedMirror(block);
            flushCache(0);
        }
        // The block's first 16 bytes link it on the blocks list (ZGE2: at the front, the newest first).
        if constexpr (game::readable::kIsDm2)
            listPushFront(&pool->blocks, block);
        else
            listPush(&pool->blocks, block);
        uint32_t link = block + kPoolLinkBytes;
        for (uint32_t i = 0; i < pool->recordsPerBlock; ++i, link += stride)
            listPushFront(&pool->free, link);
    }
    READABLE_FUNCTION(poolGrow, Verified);

    uint32_t poolAlloc(ee::Ptr<Pool> pool)
    {
        uint32_t link = pool->free.head;
        if (link == 0)
        {
            poolGrow(pool);
            link = pool->free.head;
        }
        listRemove(&pool->free, link);
        listPushFront(&pool->used, link);
        noteRecordsInUse(pool); // ZGE4: the most records in use at once kept up to date
        return recordAfter(link);
    }
    READABLE_FUNCTION_WAITS(poolAlloc, Verified);

    void poolFree(ee::Ptr<Pool> pool, ee::Address record)
    {
        const uint32_t link = record - kPoolLinkBytes;
        listRemove(&pool->used, link);
        game::tail(listPushFront, &pool->free, link);
    }
    READABLE_FUNCTION(poolFree, Verified);

    void poolReset(ee::Ptr<Pool> pool)
    {
        for (uint32_t link = pool->used.head; link != 0; link = pool->used.head)
        {
            listRemove(&pool->used, link);
            listPushFront(&pool->free, link);
        }
    }
    READABLE_FUNCTION(poolReset, Verified);

    uint32_t poolFirst(ee::Ptr<PoolList> list)
    {
        return list->head;
    }
    READABLE_FUNCTION(poolFirst, Verified);

    uint32_t poolNext(ee::Ptr<PoolLink> link)
    {
        return link->next;
    }
    READABLE_FUNCTION(poolNext, Verified);

    uint32_t poolRecord(uint32_t link)
    {
        return link + kPoolLinkBytes;
    }
    READABLE_FUNCTION(poolRecord, Verified);
    uint32_t poolLast(ee::Ptr<PoolList> list)
    {
        return list->tail;
    }
    READABLE_FUNCTION(poolLast, Verified);

    uint32_t poolPrevious(ee::Ptr<PoolLink> link)
    {
        return link->prev;
    }
    READABLE_FUNCTION(poolPrevious, Verified);

    uint32_t poolLinkOf(ee::Address record)
    {
        return record - kPoolLinkBytes;
    }
    READABLE_FUNCTION(poolLinkOf, Verified);

    uint32_t poolForEach(ee::Ptr<Pool> pool, uint32_t visitFunction, ee::Address data)
    {
        const PoolVisitor visit(visitFunction);
        ee::Ptr<ListNode> link = firstPoolLink(pool);
        while (link)
        {
            // The next link first: the visit may free this record.
            const uint32_t record = poolRecord(ee::Address(link));
            link = nextPoolLink(link);
            if (visit(record, data) == 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(poolForEach, Verified);

    // poolForEachOldestFirst: ZGE4 only: poolForEach's walk from the other end: visit(record, data) for each
    // record in use, the oldest first, while it returns non-zero; 0 when one stopped the walk, else 1.
    inline uint32_t poolForEachOldestFirst(ee::Ptr<Pool> pool, uint32_t visitFunction, ee::Address data)
    {
        const PoolVisitor visit(visitFunction);
        ee::Ptr<PoolLink> link(poolLast(&pool->used));
        while (link)
        {
            // The link before first: the visit may free this record.
            const uint32_t record = poolRecord(ee::Address(link));
            link = ee::Ptr<PoolLink>(poolPrevious(link));
            if (visit(record, data) == 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, poolForEachOldestFirst);

    int32_t poolContains(ee::Ptr<Pool> pool, ee::Address record)
    {
        return listContains(&pool->used, record - kPoolLinkBytes); // its link (poolLinkOf, which the original has inline)
    }
    READABLE_FUNCTION(poolContains, Verified);

    int32_t poolCount(ee::Ptr<Pool> pool)
    {
        return pool->used.count;
    }
    READABLE_FUNCTION(poolCount, Verified);

    int32_t poolFreeCount(ee::Ptr<Pool> pool)
    {
        return pool->free.count;
    }
    READABLE_FUNCTION(poolFreeCount, Draft);

    uint32_t poolRecordBytes(ee::Ptr<Pool> pool)
    {
        return pool->recordBytes;
    }
    READABLE_FUNCTION(poolRecordBytes, Verified);

    // ---- the pools' list, the sized pools, destroying and shrinking ------------

    namespace
    {
        // The memory module's state: the pools made (a pointer array), the six
        // sized pools and their sizes, what they hand out and waste.
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_pools> g_pools;
        constexpr uint32_t kSizedPoolCount = 6;
        inline ee::Global<std::array<ee::Ptr<Pool>, kSizedPoolCount>, sym::g_sizedPools> g_sizedPools;
        inline ee::Global<std::array<uint32_t, kSizedPoolCount>, sym::g_sizedPoolBytes> g_sizedPoolBytes; // 16 .. 256
        inline ee::Global<std::array<uint32_t, kSizedPoolCount>, sym::g_sizedPoolRecords> g_sizedPoolRecords; // records a block of each
        inline ee::Global<int32_t, sym::g_sizedWaste> g_sizedWaste;  // bytes handed out past what was asked for
        inline ee::Global<int32_t, sym::g_sizedCount> g_sizedCount;  // memory handed out by the sized pools

        constexpr uint32_t kLeastRecordsABlock = 0x10u;
        constexpr uint32_t kPoolListCapacity = 0x80u;
        constexpr uint32_t kOneRecordBlocks = 0x4u; // the flag in createPool's memory bits: a block may be one record
        constexpr uint32_t kCreateMemoryFlags = game::readable::perGame(0x7u, 0x3u, 0x7u); // (ZGE2 has no one-record flag)
        // A block's address with the mirror bits off, as the heap gave it out.
        uint32_t physicalBlock(uint32_t block) { return ee::physical(block); }
        // Whether a link lies in a block of this many bytes.
        bool inBlock(uint32_t link, uint32_t block, uint32_t bytes) { return link >= block && link < block + bytes; }
    }

    uint32_t shutDownPools()
    {
        destroyTagRecordPools();
        freePointerArray(*g_pools);
        *g_pools = ee::Ptr<PointerArray>(0u);
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownPools, Verified);

    // createPool: a pool of records: its lists, its allocators (64-byte
    // blocks from the heap), its first block. recordsABlock's high half
    // holds PoolMemoryFlags, and a flag that lets a block be one record, else
    // 16 is the least. Later blocks are a quarter of the first, no fewer
    // than that least.
    uint32_t createPool(uint32_t recordBytes, uint32_t recordsABlock, uint32_t destroyRecord)
    {
        const uint32_t memory = heapAllocate(kPoolBytes);
        const uint32_t address = memoryFill(memory, 0, kPoolBytes);
        const ee::Ptr<Pool> pool(address);
        initList(&pool->used);
        initList(&pool->free);
        initList(&pool->blocks);
        const uint32_t memoryFlags = recordsABlock >> 16 & kCreateMemoryFlags;
        const uint32_t least = (memoryFlags & kOneRecordBlocks) != 0 ? 1u : kLeastRecordsABlock;
        if constexpr (!game::readable::kIsDm2) // (ZGE2's record has no allocators)
        {
            pool->allocate = sym::allocateAligned64;
            pool->freeBlock = sym::heapFree;
        }
        pool->recordBytes = recordBytes;
        pool->destroyRecord = destroyRecord;
        clearMostInUse(pool); // ZGE4 only
        pool->memoryFlags = static_cast<uint16_t>(memoryFlags);
        pool->recordsPerBlock = static_cast<uint16_t>(recordsABlock < least ? least : recordsABlock);
        poolGrow(pool);
        if constexpr (!game::readable::kIsDm2) // (ZGE2's later blocks are as big as the first)
        {
            const uint32_t quarter = pool->recordsPerBlock >> 2;
            pool->recordsPerBlock = static_cast<uint16_t>(least >= quarter ? least : quarter);
        }
        if (*g_pools)
            pointerArrayAdd(*g_pools, address);
        return address;
    }
    READABLE_FUNCTION_WAITS(createPool, Verified);

    // setPoolAllocators (no caller in this build).
    void setPoolAllocators(ee::Ptr<Pool> pool, uint32_t allocate, uint32_t freeBlock)
    {
        pool->freeBlock = freeBlock;
        pool->allocate = allocate;
    }
    READABLE_FUNCTION(setPoolAllocators, Draft);

    // createTagRecordPools: the pools of sized blocks (16 .. 256 bytes), with their
    // statistics cleared.
    void createTagRecordPools()
    {
        for (uint32_t i = 0; i < kSizedPoolCount; ++i)
            (*g_sizedPools)[i] = ee::Ptr<Pool>(createPool((*g_sizedPoolBytes)[i], (*g_sizedPoolRecords)[i], 0));
        *g_sizedCount = 0;
        *g_sizedWaste = 0;
    }
    READABLE_FUNCTION_WAITS(createTagRecordPools, Verified);

    // destroyTagRecordPools: what the last destroyPool gave.
    uint32_t destroyTagRecordPools()
    {
        uint32_t result = 0;
        for (uint32_t i = 0; i < kSizedPoolCount; ++i)
        {
            result = destroyPool((*g_sizedPools)[i]);
            (*g_sizedPools)[i] = ee::Ptr<Pool>(0u);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(destroyTagRecordPools, Verified);

    // initTagRecords: the list of every pool, the sized pools.
    void initTagRecords()
    {
        *g_pools = newCursorArray(kPoolListCapacity);
        createTagRecordPools();
    }
    READABLE_FUNCTION_WAITS(initTagRecords, Verified);

    uint32_t allocateSized(uint32_t bytes)
    {
        for (uint32_t i = 0; i < kSizedPoolCount; ++i)
        {
            const uint32_t size = (*g_sizedPoolBytes)[i];
            if (size >= bytes)
            {
                *g_sizedWaste = *g_sizedWaste + static_cast<int32_t>(size - bytes);
                *g_sizedCount = *g_sizedCount + 1;
                return poolAlloc((*g_sizedPools)[i]);
            }
        }
        return heapAllocate(bytes);
    }
    READABLE_FUNCTION_WAITS(allocateSized, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t freeRecord(ee::Ptr<Pool> pool, uint32_t record) { return ee::game<int32_t>(sym::poolFree, pool, record); }

        namespace tail
        {
        int32_t heapFree(uint32_t block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        }
    }

    // freeSized: its result is only what the free it ended in left.
    int32_t freeSized(ee::Address memory, uint32_t bytes)
    {
        for (uint32_t i = 0; i < kSizedPoolCount; ++i)
        {
            const uint32_t size = (*g_sizedPoolBytes)[i];
            if (size >= bytes)
            {
                *g_sizedWaste = *g_sizedWaste - static_cast<int32_t>(size - bytes);
                *g_sizedCount = *g_sizedCount - 1;
                return freeRecord((*g_sizedPools)[i], memory);
            }
        }
        return tail::heapFree(memory);
    }
    READABLE_FUNCTION_WAITS(freeSized, Verified); // handed on: the heap's free takes the block alone

    uint32_t destroyPool(ee::Ptr<Pool> pool)
    {
        const int32_t index = pointerArrayFind(*g_pools, pool);
        if (index == -1)
            return 0;
        pointerArrayRemoveAt(*g_pools, index);

        // The records in use: the destructor on each (which may take it off
        // the list itself), then off the list.
        for (uint32_t link = pool->used.head; link != 0; link = pool->used.head)
        {
            if (pool->destroyRecord)
                pool->destroyRecord(recordAfter(link));
            if (link == pool->used.head)
                listRemove(&pool->used, link);
        }

        // The free records off their list.
        for (uint32_t link = pool->free.head; link != 0; link = pool->free.head)
            listRemove(&pool->free, link);

        // The blocks off theirs, each freed by its physical address.
        for (uint32_t block = pool->blocks.head; block != 0; block = pool->blocks.head)
        {
            listRemove(&pool->blocks, block);
            if constexpr (game::readable::kIsDm2) // (ZGE2's blocks are always the heap's)
                heapFree(physicalBlock(block));
            else
                pool->freeBlock(physicalBlock(block));
        }
        heapFree(pool);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyPool, Verified);

    void releaseEmptyPoolBlocks(ee::Ptr<Pool> pool)
    {
        // A block: its link, then recordsPerBlock records of the rounded
        // size with their links.
        const uint32_t blockBytes = recordStride(*pool) * pool->recordsPerBlock + kPoolLinkBytes;
        // From the second block (the first is kept).
        uint32_t next;
        for (uint32_t block = linkAt(pool->blocks.head).next; block != 0; block = next)
        {
            next = linkAt(block).next;
            // A record in use inside the block?
            uint32_t used = pool->used.head;
            while (used != 0 && !inBlock(used, block, blockBytes))
                used = linkAt(used).next;
            if (used != 0)
                continue;

            // None: its free records off the free list, the block off the
            // blocks list and freed.
            for (uint32_t link = pool->free.head, after; link != 0; link = after)
            {
                after = linkAt(link).next;
                if (inBlock(link, block, blockBytes))
                    listRemove(&pool->free, link);
            }
            const uint32_t physical = physicalBlock(block);
            listRemove(&pool->blocks, physical);
            pool->freeBlock(physical);
        }
    }
    READABLE_FUNCTION(releaseEmptyPoolBlocks, Verified);

    void releaseAllEmptyPoolBlocks()
    {
        PointerArray &pools = *g_pools->get();
        uint32_t pool = 0;
        if (pools.count != 0)
        {
            pools.cursor = 0;
            pool = pools.items[0];
        }
        else
            pools.cursor = -1;
        if (pool == 0)
            return;
        for (;;)
        {
            releaseEmptyPoolBlocks(ee::Ptr<Pool>(pool));
            // The walk's next pool (the cursor counted on, -1 past the end).
            PointerArray &list = *g_pools->get();
            const int32_t at = list.cursor;
            pool = 0;
            bool more = false;
            if (at >= 0)
            {
                list.cursor = at + 1;
                if (at + 1 < static_cast<int32_t>(list.count))
                {
                    pool = list.items[static_cast<uint32_t>(at + 1)];
                    more = true;
                }
            }
            if (!more)
                list.cursor = -1;
            if (pool == 0)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(releaseAllEmptyPoolBlocks, Verified);

    // poolTakeCreating (no caller in this build): a record from the pool in
    // `slot`, the pool made there first if there is none (records of
    // `recordBytes`, the default block).
    uint32_t poolTakeCreating(ee::Ptr<ee::Ptr<Pool>> slot, uint32_t recordBytes)
    {
        if (!*slot)
            *slot = ee::Ptr<Pool>(createPool(recordBytes, 0, 0));
        return poolAlloc(*slot);
    }
    READABLE_FUNCTION_WAITS(poolTakeCreating, Draft);

    // poolGiveDestroying (no caller in this build): the record given back to
    // the pool in `slot`, and the pool destroyed (the slot emptied) once it
    // holds no record in use.
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record)
    {
        poolFree(*slot, record);
        if (zge::poolCount(*slot) != 0)
            return;
        destroyPool(*slot);
        *slot = ee::Ptr<Pool>();
    }
    READABLE_FUNCTION_WAITS(poolGiveDestroying, Draft);
}
