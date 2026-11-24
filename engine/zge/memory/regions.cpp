// The engine's memory (zge/memory/regions.h; copyQuad-allocateAligned64): the C
// library's heap behind the function table the platform fills in
// (initPlatformSettings), with 16-byte sizes and a hook after each
// allocation (empty in this build), and regions: a block taken from the bottom
// up with no freeing but back to a point.

#include "regions.h"
#include "alignment.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "pool.h"
#include "zge/container/array.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/zge/memory/regions.cpp.h"

namespace zge
{
    namespace
    {
        // The C library's heap, as initPlatformSettings fills the table in.
        inline ee::Global<ee::FunctionPtr<uint32_t(uint32_t bytes)>, sym::g_heapAllocateFunction> g_heapAllocateFunction;
        inline ee::Global<ee::FunctionPtr<uint32_t(uint32_t alignment, uint32_t bytes)>, sym::g_heapMemalignFunction> g_heapMemalignFunction;
        inline ee::Global<ee::FunctionPtr<int32_t(ee::Address block)>, sym::g_heapFreeFunction> g_heapFreeFunction;
        inline ee::Global<ee::FunctionPtr<uint32_t(uint32_t block, uint32_t bytes)>, sym::g_heapReallocateFunction> g_heapReallocateFunction;

        // The memory module's state: ready, the pools' list, the regions' pool, the sized pools. ZGE2's is 0x14 bytes
        // (initMemoryRegions clears it): ready, a word, the pools' list (+0x08), a word, the regions' pool (+0x10); no
        // sized pools. ZGE2 makes the pools' list here, ZGE3 in initTagRecords with the sized pools.
        constexpr uint32_t kMemoryState = sym::g_memoryRegionsReady;
        constexpr uint32_t kMemoryStateBytes = game::readable::perGame(0x30u, 0x14u, 0x30u);
        inline ee::Global<uint32_t, kMemoryState> g_memoryRegionsReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_memoryRegionPool> g_memoryRegionPool;
        // ZGE2's memory state's second word (+0x04): what its freeMemoryRegion gives back; no code writes it.
        inline ee::Global<uint32_t, sym::dm2::g_freedRegionMemory> g_freedRegionMemory;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_pools> g_pools; // every pool made (pool.cpp's)
        constexpr uint32_t kPoolListCapacity = 0x80u;

        constexpr uint32_t kRegionsABlock = 0x10u;
        constexpr uint32_t kBigAlignment = 0x40u; // allocateAligned64's

        // testHeapLimits's limits.
        constexpr uint32_t kHeapTestBlockMax = 0x400u;
        constexpr uint32_t kHeapTestBytes = (8u << 20) - 1;
        // largestFreeBlock's step.
        constexpr int32_t kLargestBlockStep = 0x400;

        // A quadword moved: 16 bytes, on a 16-byte boundary.
        struct Quadword
        {
            uint64_t low;
            uint64_t high;
        };
        constexpr uint32_t kQuadwordAligned = ~0xFu;

        // testHeapLimits's blocks: each holds the next one's address.
        struct HeapTestBlock
        {
            ee::Ptr<HeapTestBlock> next;
        };
    }

    // copyQuad (no caller): a 16-byte value copied (the low four bits of the
    // addresses ignored); dst.
    uint32_t copyQuad(uint32_t dst, uint32_t src)
    {
        *ee::Ptr<Quadword>(dst & kQuadwordAligned) = *ee::Ptr<Quadword>(src & kQuadwordAligned);
        return dst;
    }
    READABLE_FUNCTION(copyQuad, Draft);

    // alignUp16: bytes rounded up to a multiple of 16.
    uint32_t alignUp16(uint32_t bytes)
    {
        return roundUp16(bytes);
    }
    READABLE_FUNCTION(alignUp16, Verified);

    // heapAllocateHook: called after every allocation; nothing in this build.
    void heapAllocateHook()
    {
    }
    READABLE_FUNCTION(heapAllocateHook, Verified);

    ee::Ptr<PointerArray> newCursorArray(uint32_t capacity); // container/array.cpp
    void freePointerArray(ee::Ptr<PointerArray> array);      // container/array.cpp

    // initMemoryRegions.
    uint32_t initMemoryRegions()
    {
        memoryFill(kMemoryState, 0, kMemoryStateBytes);
        *g_memoryRegionsReady = 1;
        if constexpr (game::readable::kIsDm2) // (ZGE2: the pools' list alone; it has no sized pools)
            *g_pools = newCursorArray(kPoolListCapacity);
        else
            initTagRecords();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initMemoryRegions, Verified);

    // shutdownMemoryRegions.
    uint32_t shutdownMemoryRegions()
    {
        if (*g_memoryRegionsReady != 0)
        {
            if (*g_memoryRegionPool)
            {
                destroyPool(*g_memoryRegionPool);
                if constexpr (!game::readable::kIsDm2) // (ZGE2 leaves the pool's address in the state)
                    *g_memoryRegionPool = ee::Ptr<Pool>(0u);
            }
            if constexpr (game::readable::kIsDm2) // ZGE2: the pools' list freed here (ZGE3's shutDownPools, less the sized pools)
            {
                freePointerArray(*g_pools);
                *g_pools = ee::Ptr<PointerArray>(0u);
            }
            else
                shutDownPools();
            *g_memoryRegionsReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownMemoryRegions, Verified);

    // heapAllocate: a block of the C library's heap (rounded to 16 twice).
    uint32_t heapAllocate(uint32_t bytes)
    {
        const uint32_t rounded = roundUp16(bytes);
        const uint32_t aligned = alignUp16(rounded);
        const uint32_t block = (*g_heapAllocateFunction)(aligned);
        heapAllocateHook();
        return block;
    }
    READABLE_FUNCTION(heapAllocate, Verified);

    // allocateAligned: a block on an alignment.
    uint32_t allocateAligned(uint32_t alignment, uint32_t bytes)
    {
        return (*g_heapMemalignFunction)(alignment, roundUp16(bytes));
    }
    READABLE_FUNCTION(allocateAligned, Verified); // handed on: none: memalign takes two

    // heapFree.
    int32_t heapFree(ee::Address block)
    {
        return (*g_heapFreeFunction)(block);
    }
    READABLE_FUNCTION(heapFree, Verified); // handed on: none: free takes the block

    // reallocate.
    uint32_t reallocate(ee::Address block, uint32_t bytes)
    {
        return (*g_heapReallocateFunction)(block, alignUp16(roundUp16(bytes)));
    }
    READABLE_FUNCTION(reallocate, Verified);

    // testHeapLimits (the debug menu's memory test): blocks of up to 1 KB taken
    // until the heap fails or 8 MB are out, each holding the next one's
    // address, then all given back. (If the very first allocation fails there
    // is no last block, and the end marker goes to the word at address 0.)
    void testHeapLimits(uint32_t blockBytes)
    {
        ee::Ptr<HeapTestBlock> first;
        ee::Ptr<HeapTestBlock> last;
        uint32_t total = 0;
        const uint32_t aligned = alignUp16(roundUp16(blockBytes < kHeapTestBlockMax + 1u ? blockBytes : kHeapTestBlockMax));
        for (;;)
        {
            const ee::Ptr<HeapTestBlock> block((*g_heapAllocateFunction)(aligned));
            if (!block)
                break;
            if (!first)
                first = block;
            if (last)
                last->next = block;
            total += aligned;
            last = block;
            if (kHeapTestBytes < total)
                break;
        }
        last->next = ee::Ptr<HeapTestBlock>(0u);
        for (ee::Ptr<HeapTestBlock> block = first; block;)
        {
            const ee::Ptr<HeapTestBlock> next = block->next;
            (*g_heapFreeFunction)(block);
            block = next;
        }
    }
    READABLE_FUNCTION(testHeapLimits, Verified);

    // largestFreeBlock (the debug menu's): the first size in 1 KB steps the heap
    // refuses.
    int32_t largestFreeBlock()
    {
        for (int32_t bytes = kLargestBlockStep;; bytes += kLargestBlockStep)
        {
            const uint32_t block = (*g_heapAllocateFunction)(bytes);
            if (block == 0)
                return bytes;
            (*g_heapFreeFunction)(block);
        }
    }
    READABLE_FUNCTION(largestFreeBlock, Verified); // handed on: none: malloc and free take one

    // freeHeapBytes (ZGE4 only, where ZGE3 has testHeapLimits and largestFreeBlock): the heap's free bytes, from
    // the platform's count (mallocFreeBytes on the PS2). The GameCube build's prints them when its second word is
    // set ("ZMEM: Free memory: %dKb", "> %dKb" from 128 MB up); the PS2 build's print is gone, and it reads
    // neither of the two words its callers pass (1 KB, unread on the GameCube too, and 0).
    inline uint32_t freeHeapBytes()
    {
        return mallocFreeBytes();
    }
    READABLE_FUNCTION_ONLY(bmx, freeHeapBytes);

    // newMemoryRegion: a region over `memory`, or over a block of its own taken
    // from the heap when that is 0.
    ee::Ptr<MemoryRegion> newMemoryRegion(uint32_t memory, uint32_t bytes)
    {
        if (!*g_memoryRegionPool)
            *g_memoryRegionPool = ee::Ptr<Pool>(createPool(static_cast<uint32_t>(sizeof(MemoryRegion)), kRegionsABlock, 0));
        const uint32_t address = poolAlloc(*g_memoryRegionPool);
        memoryFill(address, 0, sizeof(MemoryRegion));
        const ee::Ptr<MemoryRegion> region(address);
        if (memory == 0)
        {
            memory = heapAllocate(bytes);
            region->flags |= kRegionOwnsMemory;
        }
        region->top = memory;
        region->end = memory + bytes;
        region->base = memory;
        return region;
    }
    READABLE_FUNCTION_WAITS(newMemoryRegion, Verified);

    // freeMemoryRegion: the region's memory given back when it owns it, the record cleared and freed. 1.
    // ZGE2 gives back the memory state's second word instead, which nothing writes: heapFree(0), the memory kept.
    uint32_t freeMemoryRegion(ee::Ptr<MemoryRegion> region)
    {
        if (region->flags & kRegionOwnsMemory)
        {
            if constexpr (game::readable::kIsDm2)
                heapFree(*g_freedRegionMemory);
            else
                heapFree(region->base);
        }
        memoryFill(region, 0, sizeof(MemoryRegion));
        poolFree(*g_memoryRegionPool, region);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeMemoryRegion, Verified);

    // regionAllocate: `bytes` from the top, 16-byte aligned; 0 when there is no room.
    uint32_t regionAllocate(ee::Ptr<MemoryRegion> region, uint32_t bytes)
    {
        const uint32_t rounded = roundUp16(bytes);
        const uint32_t aligned = alignUp16(rounded);
        const uint32_t memory = region->top;
        const uint32_t newTop = roundUp16(memory + aligned);
        if (region->end < newTop)
            return 0;
        region->top = newTop;
        heapAllocateHook();
        return memory;
    }
    READABLE_FUNCTION(regionAllocate, Verified);

    namespace
    {
        // The top back down to `memory`, rounded up to 16 bytes, if that is below it: -16 (the rounding mask),
        // else 0 and the top as it was.
        int32_t lowerTopTo(MemoryRegion &region, ee::Address memory)
        {
            if (memory >= region.top)
                return 0;
            region.top = roundUp16(memory);
            return -16;
        }
    }

    // releaseRegionTop: the top back down to `memory` (a pointer it gave out).
    // 0 when that is not below the top; else -16, the rounding mask it used.
    // (The original jumps to lowerRegionTop's code.)
    int32_t releaseRegionTop(ee::Ptr<MemoryRegion> region, ee::Address memory)
    {
        return lowerTopTo(*region, memory);
    }
    READABLE_FUNCTION(releaseRegionTop, Verified);

    // lowerRegionTop: the code releaseRegionTop and rewindRegion jump to: the top back down to `memory`, rounded
    // up to 16 bytes, if that is below it. -16 (the rounding mask) when it moved, else 0.
    int32_t lowerRegionTop(ee::Ptr<MemoryRegion> region, ee::Address memory)
    {
        return lowerTopTo(*region, memory);
    }
    READABLE_FUNCTION(lowerRegionTop, Verified);

    // rewindRegion: emptied (the top back to the region's base).
    int32_t rewindRegion(ee::Ptr<MemoryRegion> region)
    {
        return game::tail(lowerRegionTop, region, region->base);
    }
    READABLE_FUNCTION(rewindRegion, Verified);

    // regionNoop.
    void regionNoop()
    {
    }
    READABLE_FUNCTION(regionNoop, Verified);

    // regionTop (no caller in this build).
    uint32_t regionTop(ee::Ptr<MemoryRegion> region)
    {
        return region->top;
    }
    READABLE_FUNCTION(regionTop, Draft);

    // regionFreeBytes.
    int32_t regionFreeBytes(ee::Ptr<MemoryRegion> region)
    {
        return static_cast<int32_t>(region->end - region->top);
    }
    READABLE_FUNCTION(regionFreeBytes, Verified);

    // regionContains: whether `memory` lies in it (the end counts).
    int32_t regionContains(ee::Ptr<MemoryRegion> region, ee::Address memory)
    {
        return memory >= region->base && memory <= region->end ? 1 : 0;
    }
    READABLE_FUNCTION(regionContains, Verified);

    // allocateAligned64: 64-byte aligned, whole 64-byte units (the pools' blocks).
    uint32_t allocateAligned64(uint32_t bytes)
    {
        return allocateAligned(kBigAlignment, (bytes + (kBigAlignment - 1)) & ~(kBigAlignment - 1));
    }
    READABLE_FUNCTION_WAITS(allocateAligned64, Verified);
}
