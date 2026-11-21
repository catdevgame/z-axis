#pragma once

// The engine's record pools (block shutDownPools, the memory module; DM2 and
// BMX XXX have them word for word): records of one size, handed out from a
// free list and grown a block at a time. Each record sits after a 16-byte
// link (a ListNode, then padding): on the used list while handed out, on the
// free list after. A walk over the records in use goes link by link
// (poolFirst, poolNext, poolRecord). The collision queries' hits and face
// lists, the texture cache and the scene's regions are pools.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/memory/pool.h.h"

namespace zge
{
    constexpr uint32_t kPoolLinkBytes = 0x10u; // a record's link before it

    // A pool's own lists, as the pool reads them: a List whose links are
    // addresses. The pool works out where a record is from where its link
    // is (16 bytes on) and which block a link lies in from the block's
    // bytes, so it keeps them as numbers. The same layout as List and
    // ListNode, and the list functions take them as those.
    struct PoolLink
    {
        uint32_t next; // +0x00
        uint32_t prev; // +0x04
    };
    struct PoolList
    {
        uint32_t head; // +0x00
        uint32_t tail; // +0x04
        int32_t count; // +0x08
    };
    static_assert(sizeof(PoolLink) == sizeof(ListNode) && sizeof(PoolList) == sizeof(List) &&
                  offsetof(PoolList, count) == offsetof(List, count));

    struct Pool
    {
        PoolList used;            // +0x00 the links of the records handed out, the newest first
        PoolList free;            // +0x0C
        PoolList blocks;          // +0x18 the memory the records are in, a block per grow
        // +0x24 (record) called for each record in use as the pool is
        // destroyed (createPool's third argument), 0: none.
        ee::FunctionPtr<void(uint32_t record)> destroyRecord;
        uint32_t recordBytes;     // +0x28
#if ZX_GAME_BMX
        // ZGE4 counts the most records in use at once (createPool zeroes it, poolAlloc raises it to used.count); the
        // fields after it are a word on.
        uint32_t mostInUse;       // BMX +0x2C
#endif
        uint16_t recordsPerBlock; // +0x2C (BMX +0x30)
        uint16_t memoryFlags;     // +0x2E (BMX +0x32) PoolMemoryFlags
        ee::FunctionPtr<uint32_t(uint32_t bytes, uint32_t alignmentMask)> allocate; // +0x30 (BMX +0x34) a block's memory
        ee::FunctionPtr<void(uint32_t block)> freeBlock;                             // +0x34 (BMX +0x38) (heapFree)
    };
    static_assert(offsetof(Pool, free) == 0x0C && offsetof(Pool, blocks) == 0x18 && offsetof(Pool, destroyRecord) == 0x24);
    static_assert(offsetof(Pool, recordBytes) == 0x28 &&
                  offsetof(Pool, recordsPerBlock) == game::readable::perGame(0x2Cu, 0x2Cu, 0x30u) &&
                  offsetof(Pool, memoryFlags) == game::readable::perGame(0x2Eu, 0x2Eu, 0x32u) &&
                  offsetof(Pool, allocate) == game::readable::perGame(0x30u, 0x30u, 0x34u) &&
                  sizeof(Pool) == game::readable::perGame(0x38u, 0x38u, 0x3Cu));
    // ZGE4's count of the most records in use at once (Pool::mostInUse, BMX XXX only): createPool zeroes it and
    // poolAlloc raises it to the records in use. ZGE2 and ZGE3 keep no such count, so these do nothing there.
    inline void clearMostInUse(ee::Ptr<Pool> pool)
    {
#if ZX_GAME_BMX
        pool->mostInUse = 0;
#else
        (void)pool;
#endif
    }
    inline void noteRecordsInUse(ee::Ptr<Pool> pool)
    {
#if ZX_GAME_BMX
        const uint32_t inUse = static_cast<uint32_t>(pool->used.count);
        pool->mostInUse = inUse < pool->mostInUse ? pool->mostInUse : inUse;
#else
        (void)pool;
#endif
    }

    // The pool record as createPool allocates it: ZGE2's ends before the allocators (0x30 bytes; read in DM2's createPool,
    // poolGrow and destroyPool): its blocks are always allocateAligned64's, freed with heapFree. ZGE3 added the two
    // fields (setPoolAllocators).
    constexpr uint32_t kPoolBytes = game::readable::perGame(0x38u, 0x30u, 0x3Cu);

    // How the records are addressed: through an uncached mirror of RAM
    // (the cache flushed first), for records DMA reads.
    enum PoolMemoryFlags : uint16_t
    {
        kPoolUncached = 1u << 0,            // the uncached mirror
        kPoolUncachedAccelerated = 1u << 1, // the uncached accelerated mirror
    };

    // poolGrow: a block of recordsPerBlock records allocated and its
    // records put on the free list.
    void poolGrow(ee::Ptr<Pool> pool);

    // poolAlloc: a record (grown first when none is free).
    uint32_t poolAlloc(ee::Ptr<Pool> pool);

    // poolFree: a record handed back.
    void poolFree(ee::Ptr<Pool> pool, ee::Address record);

    // poolReset: every record handed back.
    void poolReset(ee::Ptr<Pool> pool);

    // poolFirst: the first link of a list (a pool's: its newest record
    // in use), or 0.
    uint32_t poolFirst(ee::Ptr<PoolList> list);

    // poolNext: the link after this one, or 0.
    uint32_t poolNext(ee::Ptr<PoolLink> link);

    // poolRecord: the record of a link.
    uint32_t poolRecord(uint32_t link);

    // poolTakeCreating: a record from the pool in the slot, the pool made first when the slot is empty.
    uint32_t poolTakeCreating(ee::Ptr<ee::Ptr<Pool>> slot, uint32_t recordBytes);
    // poolGiveDestroying: the record back to the pool in the slot, the pool destroyed with its last record.
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record);

    // poolLast: the last link of a list (a pool's: its oldest record in
    // use), or 0; poolPrevious: the link before this one; poolLinkOf: a
    // record's link.
    uint32_t poolLast(ee::Ptr<PoolList> list);
    uint32_t poolPrevious(ee::Ptr<PoolLink> link);
    uint32_t poolLinkOf(ee::Address record);

    // The typed views of a pool, for the code that keeps records in one: each
    // is the call itself, the word it returns as the type the pool holds (a
    // 0 link's record is 0x10, as poolRecord makes it). A pool or a link as
    // its holder keeps it (a word, an ee::Ptr) is an ee::Address.
    //   poolAllocAs<Record>(pool)    a record handed out (poolAlloc)
    //   poolRecordAs<Record>(link)   the record a link stands for (poolRecord)
    //   poolLinkOfRecord(record)     a record's link (poolLinkOf)
    template <typename Record>
    ee::Ptr<Record> poolAllocAs(ee::Address pool)
    {
        return ee::Ptr<Record>(poolAlloc(ee::Ptr<Pool>(pool)));
    }
    template <typename Record>
    ee::Ptr<Record> poolRecordAs(ee::Address link)
    {
        return ee::Ptr<Record>(poolRecord(link));
    }
    inline ee::Ptr<ListNode> poolLinkOfRecord(ee::Address record)
    {
        return ee::Ptr<ListNode>(poolLinkOf(record));
    }

    // The walk over a pool's records in use, the newest first, each step the
    // game call it is.
    inline ee::Ptr<ListNode> firstPoolLink(ee::Address pool)
    {
        const ee::Ptr<Pool> records(pool);
        return ee::Ptr<ListNode>(poolFirst(&records->used));
    }
    inline ee::Ptr<ListNode> nextPoolLink(ee::Ptr<ListNode> link)
    {
        return ee::Ptr<ListNode>(poolNext(link.cast<PoolLink>()));
    }

    // poolForEach: visit(record, data) for each record in use, the newest
    // first, while it returns non-zero; 0 when one stopped the walk, else 1.
    using PoolVisitor = ee::FunctionPtr<int32_t(uint32_t record, uint32_t data)>;
    uint32_t poolForEach(ee::Ptr<Pool> pool, uint32_t visit, ee::Address data);
    uint32_t poolForEachOldestFirst(ee::Ptr<Pool> pool, uint32_t visit, ee::Address data); // ZGE4 only
    // poolContains: non-zero when the record is in use.
    int32_t poolContains(ee::Ptr<Pool> pool, ee::Address record);
    // poolCount, poolFreeCount, poolRecordBytes: the records in use, the free
    // ones, a record's bytes.
    int32_t poolCount(ee::Ptr<Pool> pool);
    int32_t poolFreeCount(ee::Ptr<Pool> pool);
    uint32_t poolRecordBytes(ee::Ptr<Pool> pool);

    // destroyPool: the pool taken off the list of pools, its records'
    // destructor run on each record in use, its blocks freed, the pool
    // freed. 0 when it was not on the list, else 1.
    uint32_t destroyPool(ee::Ptr<Pool> pool);
    uint32_t createPool(uint32_t recordBytes, uint32_t recordsABlock, uint32_t destroyRecord);
    void setPoolAllocators(ee::Ptr<Pool> pool, uint32_t allocate, uint32_t freeBlock);
    void createTagRecordPools();
    uint32_t destroyTagRecordPools();
    void initTagRecords();
    // releaseEmptyPoolBlocks: the pool's blocks with no record in use freed (the first
    // block is kept). releaseAllEmptyPoolBlocks: the same for every pool.
    void releaseEmptyPoolBlocks(ee::Ptr<Pool> pool);
    void releaseAllEmptyPoolBlocks();

    // The sized pools: six pools of 16, 32, 64, 128, 192 and 256-byte
    // records (g_sizedPools, made by createTagRecordPools): allocateSized `bytes` from
    // the smallest that fits (else the heap), freeSized the memory handed
    // back by its size; shutDownPools the pools destroyed and their list
    // freed (always 1).
    uint32_t allocateSized(uint32_t bytes);
    int32_t freeSized(ee::Address memory, uint32_t bytes);
    uint32_t shutDownPools();
}
