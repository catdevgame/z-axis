// The engine's small heaps (zge/memory/heaps.h): a slot of 128, its
// allocation (the slot's allocator) laid out as one free block and the
// sentinel; blocks taken first fit, split when what is left is worth a block
// (32 bytes), given back with their free neighbours merged.

#include "heaps.h"
#include "alignment.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libc.h"
#include "zge/memory/regions.h"

#include <array>
#include <cstdint>
#include <cstring>
#include "sym/zge/memory/heaps.cpp.h"

namespace zge
{
    namespace
    {
        // The allocators a heap gets without its own (set by
        // setDefaultAllocators), and the flag it sets.
        inline ee::Global<HeapAllocators, sym::g_defaultAllocateFunction> g_defaultAllocators;
        inline ee::Global<uint32_t, sym::g_defaultAllocatorsSet> g_defaultAllocatorsSet;
        inline ee::Global<std::array<Heap, kHeapCount>, kHeaps> g_heaps;

        constexpr uint32_t kMinimumBytes = 0x10u;
        constexpr uint32_t kSplitBytes = 0x20u; // a split leaves a free block only this big or more

        // The header at a block's address; a block's memory follows its
        // header and runs to the next block's.
        HeapBlock &blockAt(uint32_t block) { return *ee::Ptr<HeapBlock>(block); }
        uint32_t memoryOf(uint32_t block) { return block + sizeof(HeapBlock); }
        uint32_t blockOf(uint32_t memory) { return memory - sizeof(HeapBlock); }
        uint32_t blockRoom(uint32_t block) { return blockAt(block).next - block - sizeof(HeapBlock); }
    }

    // takeHeapSlot: a free heap slot marked in use, 0 when all 128 are.
    ee::Ptr<Heap> takeHeapSlot()
    {
        for (Heap &heap : *g_heaps)
            if (heap.inUse == 0)
            {
                heap.inUse = 1;
                return &heap;
            }
        return ee::Ptr<Heap>(0u);
    }
    READABLE_FUNCTION(takeHeapSlot, Verified);

    // releaseHeapSlot: the slot free again.
    void releaseHeapSlot(ee::Ptr<Heap> heap)
    {
        heap->inUse = 0;
    }
    READABLE_FUNCTION(releaseHeapSlot, Verified);

    // defaultAllocate: the default allocate: the C library's heap.
    uint32_t defaultAllocate(uint32_t bytes)
    {
        return heapAllocate(bytes);
    }
    READABLE_FUNCTION_WAITS(defaultAllocate, Verified);

    // setDefaultAllocators (initEngine), shutDownHeaps (shutdownEngineSystems): the
    // default allocators set; their flag cleared. Always 1.
    uint32_t setDefaultAllocators()
    {
        g_defaultAllocators->allocate = sym::defaultAllocate;
        g_defaultAllocators->free = sym::heapFree;
        *g_defaultAllocatorsSet = 1;
        return 1;
    }
    READABLE_FUNCTION(setDefaultAllocators, Verified);

    uint32_t shutDownHeaps()
    {
        *g_defaultAllocatorsSet = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownHeaps, Verified);

    // newHeap: a heap of `bytes` (at least 16, rounded to 16) from the
    // allocators at `allocators` (allocate, free; 0: the defaults): one free
    // block and the sentinel at the end. 0 when no slot or no memory.
    ee::Ptr<Heap> newHeap(uint32_t bytes, uint32_t allocators)
    {
        const ee::Ptr<Heap> heap = takeHeapSlot();
        if (!heap)
            return heap;
        // Copied as bytes: a caller's table need not be word aligned.
        const ee::Ptr<HeapAllocators> own(allocators);
        std::memcpy(&heap->allocators, own ? own.get() : g_defaultAllocators.get(), sizeof(HeapAllocators));
        const uint32_t size = bytes < kMinimumBytes ? kMinimumBytes : roundUp16(bytes);
        const uint32_t memory = heap->allocators.allocate(size);
        if (memory == 0)
        {
            releaseHeapSlot(heap);
            return ee::Ptr<Heap>(0u);
        }
        const uint32_t block = memory;
        const uint32_t sentinel = memory + size - sizeof(HeapBlock);
        heap->first = block;
        searchFromFirst(heap);
        blockAt(sentinel).free = 0;
        blockAt(block).free = 1;
        blockAt(sentinel).next = 0;
        blockAt(block).prev = 0;
        blockAt(block).next = sentinel;
        blockAt(sentinel).prev = block;
        heap->last = sentinel;
        return heap;
    }
    READABLE_FUNCTION(newHeap, Verified);

    // deleteHeap: the heap's allocation freed, the slot released.
    void deleteHeap(ee::Ptr<Heap> heap)
    {
        heap->allocators.free(heap->first);
        game::tail(releaseHeapSlot, heap);
    }
    READABLE_FUNCTION(deleteHeap, Verified); // handed on: the heap's free takes the block alone

    // heapTake: `bytes` (at least 16, rounded to 16) from the first free
    // block with room for them and a header; the rest split off as a free
    // block when 32 bytes or more. The memory, 0 when no block has room.
    // (ZGE4 searches from the block after the last one taken: searchStart.)
    uint32_t heapTake(ee::Ptr<Heap> heap, uint32_t bytes)
    {
        const uint32_t size = bytes < kMinimumBytes ? kMinimumBytes : roundUp16(bytes);
        const uint32_t need = size + sizeof(HeapBlock);
        uint32_t block = searchStart(heap);
        while (block != 0 && !(blockAt(block).free && blockRoom(block) >= need))
            block = blockAt(block).next;
        if (block == 0)
            return 0;
        if (blockRoom(block) - need >= kSplitBytes)
        {
            const uint32_t rest = memoryOf(block) + size;
            const uint32_t after = blockAt(block).next;
            blockAt(rest).next = after;
            blockAt(after).prev = rest;
            blockAt(block).next = rest;
            blockAt(rest).free = 1;
            blockAt(rest).prev = block;
        }
        searchOnAfter(heap, block);
        blockAt(block).bytes = bytes;
        blockAt(block).free = 0;
        return memoryOf(block);
    }
    READABLE_FUNCTION(heapTake, Verified);

    // heapGive: memory from heapTake given back: its block free and merged
    // with the next and then the previous when they are free. (The heap
    // itself is not read; ZGE4 starts its next search at the first block.)
    void heapGive(ee::Ptr<Heap> heap, ee::Address memory)
    {
        if (memory == 0)
            return;
        searchFromFirst(heap);
        const uint32_t block = blockOf(memory);
        blockAt(block).free = 1;
        uint32_t next = blockAt(block).next;
        const uint32_t prev = blockAt(block).prev;
        if (blockAt(next).free)
        {
            next = blockAt(next).next;
            blockAt(block).next = next;
            blockAt(next).prev = block;
        }
        if (prev != 0 && blockAt(prev).free)
        {
            blockAt(prev).next = next;
            blockAt(next).prev = prev;
        }
    }
    READABLE_FUNCTION(heapGive, Verified);

    // heapForEachBlock (no caller in the game): visit(memory, bytes, free, data)
    // for each block up to the sentinel (none without a visit).
    void heapForEachBlock(ee::Ptr<Heap> heap, HeapVisitor visit, uint32_t data)
    {
        if (!visit)
            return;
        uint32_t block = heap->first;
        while (block != heap->last)
        {
            const uint32_t current = block;
            block = blockAt(current).next;
            visit(memoryOf(current), blockRoom(current), blockAt(current).free, data);
        }
    }
    READABLE_FUNCTION(heapForEachBlock, Draft); // handed on: visit takes the four it is given (no caller in the game)

    // reportMemoryStats (endEditCore's leak report): the C library's
    // malloc_stats (the original's tail call). ZGE4's does nothing.
    void reportMemoryStats()
    {
        if constexpr (!game::readable::kIsBmx)
            game::tail(sce::libc::malloc_stats);
    }
    READABLE_FUNCTION_WAITS(reportMemoryStats, Verified);
}
