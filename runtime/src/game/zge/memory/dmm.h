#pragma once

// The engine's handle regions ("dmm": zge/memory/dmm.cpp; the memory
// block, initDmmRegions-dmmDebugWalk and on): one heap block holding a header, the
// blocks handed out from its start upwards and their handles from its end
// downwards. A block is known by its handle's index (1: the last word pair
// of the region), so the region can move blocks and keep the handles; free
// blocks are on a list sorted by address and merge with their neighbours.

#include "ee/ee.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // DM2's region keeps no search hint (its search for a free handle always
    // starts at handle 1): the fields after it sit a word lower, and the
    // header keeps its 0x20 bytes (newDmmRegion's blocks start after them).
    struct DmmRegion
    {
        uint32_t start;     // +0x00 the blocks' memory (after this header)
        uint32_t end;       // +0x04 its end; handle i is at end - 8 * i
        uint32_t handleLow; // +0x08 the lowest handle made (end while none)
        uint32_t top;       // +0x0C the blocks' high-water mark
#if !ZX_GAME_DM2
        int32_t hint;       // +0x10 where the search for a free handle starts (an index - 1)
        int32_t searchHint() const { return hint; }
        void setSearchHint(int32_t index) { hint = index; }
#else
        // DM2's region keeps no hint: its search for a free handle starts at
        // handle 1 every time, and nothing moves a hint.
        int32_t searchHint() const { return 0; }
        void setSearchHint(int32_t) {}
#endif
        uint32_t bytes;     // +0x14 (DM2 +0x10)
        int32_t lockCount;  // +0x18 (DM2 +0x14) the blocks locked: dmmCompact moves nothing while there are any
        uint32_t freeList;  // +0x1C (DM2 +0x18) the first free block by address, kDmmNoFreeBlock when none
#if ZX_GAME_DM2
        uint32_t unused;    // DM2 +0x1C: nothing reads or writes it
#endif
    };
    static_assert(sizeof(DmmRegion) == 0x20 && offsetof(DmmRegion, freeList) == game::readable::perGame(0x1Cu, 0x18u) &&
                  offsetof(DmmRegion, lockCount) == game::readable::perGame(0x18u, 0x14u) &&
                  offsetof(DmmRegion, bytes) == game::readable::perGame(0x14u, 0x10u));

    constexpr uint32_t kDmmNoFreeBlock = 0xFFFFFFFFu;

    // A handle: its block's size in bytes << 3 with the flags, and its
    // address.
    struct DmmHandle
    {
        uint32_t bits;      // +0x00 kDmmHandle* | bytes << 3
        uint32_t memory;    // +0x04
    };
    static_assert(sizeof(DmmHandle) == 8);
    enum : uint32_t
    {
        kDmmHandleInUse = 1u << 0,
        kDmmHandleLocked = 1u << 1, // zmemDMMLock's: its address is in use, so the block stays put
        kDmmHandleSizeShift = 3,
    };

    // A free block's header.
    struct DmmFreeBlock
    {
        uint32_t prev;      // +0x00
        uint32_t next;      // +0x04
        uint32_t bytes;     // +0x08
    };

    // dmm.cpp
    uint32_t initDmmRegions();
    uint32_t shutdownDmmRegions();
    ee::Ptr<DmmRegion> newDmmRegion(uint32_t bytes);
    uint32_t freeDmmRegion(ee::Ptr<DmmRegion> region);
    uint32_t dmmFindFreeHandle(ee::Ptr<DmmRegion> region);
    uint32_t dmmCarveBlock(ee::Ptr<DmmRegion> region, uint32_t freeBlock, uint32_t bytes);
    uint32_t dmmAllocate(ee::Ptr<DmmRegion> region, uint32_t bytes);
    int32_t dmmRelease(ee::Ptr<DmmRegion> region, uint32_t handle);
    uint32_t dmmHandleOf(ee::Ptr<DmmRegion> region, uint32_t address);
    // dmmCompact: the region tidied (dmm.cpp); 1 when it got to the end.
    uint32_t dmmCompact(ee::Ptr<DmmRegion> region);
}
