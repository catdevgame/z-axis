#pragma once

// The engine's small heaps (zge/memory/heaps.cpp; the memory block,
// takeHeapSlot-initDmmRegions): up to 128 heaps, each one allocation (from its
// allocator, the heap's by default) cut first fit into blocks with a
// 16-byte header, and the handle regions (dmm.cpp).

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/memory/heaps.h.h"

namespace zge
{
    // A heap block's header; the memory follows it. The last block of a
    // heap is a sentinel (never free) at the allocation's end. A heap is a
    // run of bytes cut into blocks, so the links are the neighbours'
    // addresses: a block's room is the distance to the next header.
    struct HeapBlock
    {
        uint32_t next;           // +0x00 the next block's header, 0 after the sentinel
        uint32_t prev;           // +0x04 the previous block's header, 0 before the first
        uint32_t free;           // +0x08 1 when free
        uint32_t bytes;          // +0x0C what was asked for (heapTake's)
    };
    static_assert(sizeof(HeapBlock) == 0x10);

    // Where a heap's one allocation comes from and goes back to.
    struct HeapAllocators
    {
        ee::FunctionPtr<uint32_t(uint32_t bytes)> allocate; // +0x00
        ee::FunctionPtr<void(ee::Address memory)> free;     // +0x04
    };
    static_assert(sizeof(HeapAllocators) == 8);

    // A heap (128 at heaps).
    struct Heap
    {
        uint32_t first;            // +0x00 the first block's header: the allocation's start
        uint32_t last;             // +0x04 the sentinel's header
#if PS2X_GAME_BMX
        // ZGE4 starts heapTake's search at the block after the last one taken (0: at the first); newHeap and
        // heapGive set it back to 0. The fields after it are a word on.
        uint32_t searchFrom;       // BMX +0x08
#endif
        HeapAllocators allocators; // +0x08 (BMX +0x0C)
        uint32_t inUse;            // +0x10 (BMX +0x14)
    };
    static_assert(sizeof(Heap) == game::readable::perGame(0x14u, 0x14u, 0x18u) &&
                  offsetof(Heap, allocators) == game::readable::perGame(0x08u, 0x08u, 0x0Cu) &&
                  offsetof(Heap, inUse) == game::readable::perGame(0x10u, 0x10u, 0x14u));

    // Where heapTake's search starts (Heap::searchFrom, BMX XXX only): ZGE4 goes on from the block after the last
    // one taken and starts over at the first after newHeap and heapGive; ZGE2 and ZGE3 always start at the first.
    inline uint32_t searchStart(ee::Ptr<Heap> heap)
    {
#if PS2X_GAME_BMX
        return heap->searchFrom ? heap->searchFrom : heap->first;
#else
        return heap->first;
#endif
    }
    // (heapTake: the block it takes is the one before the next search's start)
    inline void searchOnAfter(ee::Ptr<Heap> heap, uint32_t block)
    {
#if PS2X_GAME_BMX
        heap->searchFrom = ee::Ptr<HeapBlock>(block)->next;
#else
        (void)heap;
        (void)block;
#endif
    }
    inline void searchFromFirst(ee::Ptr<Heap> heap)
    {
#if PS2X_GAME_BMX
        heap->searchFrom = 0;
#else
        (void)heap;
#endif
    }

    constexpr uint32_t kHeaps = sym::heaps;
    constexpr uint32_t kHeapCount = 128;

    // heaps.cpp
    ee::Ptr<Heap> takeHeapSlot();
    void releaseHeapSlot(ee::Ptr<Heap> heap);
    uint32_t defaultAllocate(uint32_t bytes);
    uint32_t setDefaultAllocators();
    uint32_t shutDownHeaps();
    ee::Ptr<Heap> newHeap(uint32_t bytes, uint32_t allocators);
    void deleteHeap(ee::Ptr<Heap> heap);
    uint32_t heapTake(ee::Ptr<Heap> heap, uint32_t bytes);
    void heapGive(ee::Ptr<Heap> heap, ee::Address memory);
    using HeapVisitor = ee::FunctionPtr<void(uint32_t memory, uint32_t bytes, uint32_t free, uint32_t data)>;
    void heapForEachBlock(ee::Ptr<Heap> heap, HeapVisitor visit, uint32_t data);
    void reportMemoryStats();
}
