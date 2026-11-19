#pragma once

// The heap and the regions (zge/memory/regions.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // A region (0x10 bytes): allocation moves `top` up.
    struct MemoryRegion
    {
        uint32_t base;  // +0x00
        uint32_t end;   // +0x04
        uint32_t top;   // +0x08
        uint32_t flags; // +0x0C RegionFlags
    };
    static_assert(offsetof(MemoryRegion, flags) == 0x0C && sizeof(MemoryRegion) == 0x10);
    constexpr uint32_t kRegionOwnsMemory = 0x80000000u; // its memory came from the heap (given back with it)

    uint32_t copyQuad(uint32_t dst, uint32_t src);
    uint32_t alignUp16(uint32_t bytes);
    void heapAllocateHook();
    uint32_t initMemoryRegions();
    uint32_t shutdownMemoryRegions();
    uint32_t heapAllocate(uint32_t bytes);
    // The typed view of heap memory: heapAllocate itself, the block as the record (or bytes) it is made for (the same
    // address; 0 stays 0).
    template <typename Record>
    ee::Ptr<Record> heapAllocateAs(uint32_t bytes)
    {
        return ee::Ptr<Record>(heapAllocate(bytes));
    }
    uint32_t allocateAligned(uint32_t alignment, uint32_t bytes);
    int32_t heapFree(ee::Address block);
    uint32_t reallocate(ee::Address block, uint32_t bytes);
    void testHeapLimits(uint32_t blockBytes);
    int32_t largestFreeBlock();
    ee::Ptr<MemoryRegion> newMemoryRegion(uint32_t memory, uint32_t bytes);
    uint32_t freeMemoryRegion(ee::Ptr<MemoryRegion> region);
    uint32_t regionAllocate(ee::Ptr<MemoryRegion> region, uint32_t bytes);
    int32_t releaseRegionTop(ee::Ptr<MemoryRegion> region, ee::Address memory);
    int32_t rewindRegion(ee::Ptr<MemoryRegion> region);
    void regionNoop();
    uint32_t regionTop(ee::Ptr<MemoryRegion> region);
    int32_t regionFreeBytes(ee::Ptr<MemoryRegion> region);
    int32_t regionContains(ee::Ptr<MemoryRegion> region, ee::Address memory);
    uint32_t allocateAligned64(uint32_t bytes);
}
