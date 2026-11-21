// The engine's handle regions (zge/memory/dmm.h).

#include "dmm.h"
#include "alignment.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/memory/dmm.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_dmmRegionsReady> g_dmmRegionsReady;

        constexpr uint32_t kMinimumBytes = 0x10u;
        constexpr uint32_t kFreeBlockBytes = 0x10u; // what a split leaves at least
        constexpr uint32_t kFreeBlockLinks = 8u;    // a free block's two links, not counted in its bytes at the start

        // Handle `index` counts down from the region's end.
        uint32_t handleAddress(const DmmRegion &region, uint32_t index) { return region.end - index * sizeof(DmmHandle); }
        DmmHandle &handleIn(uint32_t address) { return *ee::Ptr<DmmHandle>(address); }
        DmmHandle &handleAt(const DmmRegion &region, uint32_t index) { return handleIn(handleAddress(region, index)); }

        DmmFreeBlock &freeBlockAt(uint32_t address) { return *ee::Ptr<DmmFreeBlock>(address); }
    }

    // initDmmRegions (initEngine), shutdownDmmRegions: the regions' set-up flag set
    // (1 the first time, else 0) and cleared (1 when it was set).
    uint32_t initDmmRegions()
    {
        if (*g_dmmRegionsReady != 0)
            return 0;
        *g_dmmRegionsReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initDmmRegions, Verified);

    uint32_t shutdownDmmRegions()
    {
        if (*g_dmmRegionsReady == 0)
            return 0;
        *g_dmmRegionsReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownDmmRegions, Verified);

    // newDmmRegion: a region of `bytes` (rounded to 16) from the heap: no
    // handles, one free block of all of it (less its header's 8 bytes), or
    // 0 when the heap has no room.
    ee::Ptr<DmmRegion> newDmmRegion(uint32_t bytes)
    {
        const uint32_t size = roundUp16(bytes);
        const uint32_t memory = heapAllocate(size + sizeof(DmmRegion));
        const ee::Ptr<DmmRegion> region(memory);
        if (!region)
            return region;
        // The blocks' memory follows the region's header.
        const uint32_t data = memory + sizeof(DmmRegion);
        region->bytes = size;
        region->freeList = data;
        region->handleLow = data + size;
        region->start = data;
        region->end = data + size;
        region->top = data;
        region->lockCount = 0;
        region->setSearchHint(0);
        freeBlockAt(data).prev = 0;
        freeBlockAt(data).next = 0;
        freeBlockAt(region->freeList).bytes = region->bytes - kFreeBlockLinks;
        return region;
    }
    READABLE_FUNCTION_WAITS(newDmmRegion, Verified);

    // freeDmmRegion: the region's memory back to the heap; 1, or 0 for none.
    uint32_t freeDmmRegion(ee::Ptr<DmmRegion> region)
    {
        if (!region)
            return 0;
        heapFree(region);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeDmmRegion, Verified); // handed on: the heap's free takes the block alone

    // dmmFindFreeHandle: a free handle's index, from the search's hint down;
    // past the handles made, a new one below them while it stays above the
    // blocks (0 when it would not). The hint left at it.
    uint32_t dmmFindFreeHandle(ee::Ptr<DmmRegion> region)
    {
        for (uint32_t index = static_cast<uint32_t>(region->searchHint()) + 1;; ++index)
        {
            const uint32_t handle = handleAddress(*region, index);
            if (handle < region->handleLow)
            {
                const uint32_t newLow = region->handleLow - sizeof(DmmHandle);
                if (region->top >= newLow)
                    return 0;
                region->handleLow = newLow;
                region->setSearchHint(static_cast<int32_t>(index));
                return index;
            }
            if ((handleIn(handle).bits & kDmmHandleInUse) == 0)
            {
                region->setSearchHint(static_cast<int32_t>(index));
                return index;
            }
        }
    }
    READABLE_FUNCTION(dmmFindFreeHandle, Verified);

    // What a failed allocation or a bad handle calls (defined below).
    void dmmDebugWalk(ee::Ptr<DmmRegion> region);

    // dmmCarveBlock (dmmAllocate's): `bytes` from the free block `freeBlock`
    // under a free handle: the rest split off as a free block when 16 bytes
    // or more are left, else the whole block given (the handle its size) and
    // the block off the free list. The handle's index, 0 (and the debug
    // walk) without a handle or room for one.
    uint32_t dmmCarveBlock(ee::Ptr<DmmRegion> region, uint32_t freeBlock, uint32_t bytes)
    {
        const uint32_t index = dmmFindFreeHandle(region);
        const uint32_t end = freeBlock + bytes;
        if (index != 0)
        {
            handleAt(*region, index).memory = 0;
            handleAt(*region, index).bits = 0;
        }
        if (index == 0 || region->handleLow < end + kFreeBlockBytes)
        {
            dmmDebugWalk(region);
            return 0;
        }
        DmmHandle &handle = handleAt(*region, index);
        handle.memory = freeBlock;
        handle.bits = (bytes << kDmmHandleSizeShift) | kDmmHandleInUse;
        uint32_t top = region->top;
        if (top < end)
        {
            region->top = end;
            top = end;
        }
        const uint32_t low = region->handleLow;
        const DmmFreeBlock taken = freeBlockAt(freeBlock);
        const auto noFreeBlocks = [&]() {
            region->freeList = kDmmNoFreeBlock;
        };
        if (top == low)
            noFreeBlocks(); // the blocks have reached the handles
        else
        {
            const uint32_t rest = handle.memory + bytes;
            if (freeBlock + taken.bytes < rest + kFreeBlockBytes)
            {
                // Too little left for a free block: all of it given, the
                // block off the list.
                handle.bits = (handle.bits & 7u) | (taken.bytes << kDmmHandleSizeShift);
                if (taken.prev == 0)
                {
                    if (taken.next == 0)
                        noFreeBlocks();
                    else
                    {
                        region->freeList = taken.next;
                        freeBlockAt(taken.next).prev = 0;
                    }
                }
                else
                {
                    if (taken.next == 0)
                        freeBlockAt(taken.prev).next = 0;
                    else
                    {
                        freeBlockAt(taken.prev).next = taken.next;
                        freeBlockAt(taken.next).prev = taken.prev;
                    }
                }
            }
            else
            {
                // The rest a free block in its place on the list.
                DmmFreeBlock &left = freeBlockAt(rest);
                left.next = taken.next;
                left.bytes = taken.bytes - bytes;
                left.prev = taken.prev;
                if (taken.prev != 0)
                    freeBlockAt(taken.prev).next = rest;
                if (left.next != 0)
                    freeBlockAt(left.next).prev = rest;
                if (left.prev == 0)
                    region->freeList = rest;
            }
        }
        return index;
    }
    READABLE_FUNCTION(dmmCarveBlock, Verified);

    // dmmAllocate: `bytes` (at least 16, rounded to 16) from the first free
    // block big enough; the handle's index, 0 for none (the debug walk), no
    // bytes, no free block or the regions not set up.
    uint32_t dmmAllocate(ee::Ptr<DmmRegion> region, uint32_t bytes)
    {
        if (*g_dmmRegionsReady == 0)
            return 0;
        if (bytes == 0 || region->freeList == kDmmNoFreeBlock)
            return 0;
        const uint32_t size = roundUp16(bytes < kMinimumBytes ? kMinimumBytes : bytes);
        uint32_t block = region->freeList;
        for (;;)
        {
            if (freeBlockAt(block).bytes >= size)
            {
                const uint32_t handle = dmmCarveBlock(region, block, size);
                return handle;
            }
            block = freeBlockAt(block).next;
            if (block == 0)
                break;
        }
        dmmDebugWalk(region);
        return 0;
    }
    READABLE_FUNCTION(dmmAllocate, Verified);

    // zmemDMMLock: the block of handle `index` locked where it is (dmmCompact
    // moves no block while any is locked), its address returned. A handle
    // below those made or not in use: the debug walk and 0. Locking a locked
    // handle changes nothing (the Xbox and GameCube builds say "Locking
    // previously locked handle"; this build's message is compiled out).
    uint32_t zmemDMMLock(ee::Ptr<DmmRegion> region, uint32_t index)
    {
        const uint32_t address = handleAddress(*region, index);
        if (address < region->handleLow || (handleIn(address).bits & kDmmHandleInUse) == 0)
        {
            dmmDebugWalk(region);
            return 0;
        }
        DmmHandle &handle = handleIn(address);
        if ((handle.bits & kDmmHandleLocked) == 0)
        {
            region->lockCount = region->lockCount + 1;
            handle.bits |= kDmmHandleLocked;
        }
        return handle.memory;
    }
    READABLE_FUNCTION(zmemDMMLock, Verified);

    // dmmUnlockHandle (dmmRelease's): a locked handle in use unlocked; any
    // other left as it is.
    void dmmUnlockHandle(ee::Ptr<DmmRegion> region, uint32_t index)
    {
        const uint32_t address = handleAddress(*region, index);
        if (address < region->handleLow)
            return;
        DmmHandle &handle = handleIn(address);
        if ((handle.bits & (kDmmHandleInUse | kDmmHandleLocked)) != (kDmmHandleInUse | kDmmHandleLocked))
            return;
        region->lockCount = region->lockCount - 1;
        handle.bits &= ~kDmmHandleLocked;
    }
    READABLE_FUNCTION(dmmUnlockHandle, Verified);

    namespace
    {
        // The freed block onto the region's free list, in address order.
        void insertFreeBlock(DmmRegion &region, uint32_t block)
        {
            DmmFreeBlock &freed = freeBlockAt(block);
            const uint32_t head = region.freeList;
            if (block < head)
            {
                if (head != kDmmNoFreeBlock)
                {
                    freeBlockAt(head).prev = block;
                    freed.prev = 0;
                    freed.next = region.freeList;
                }
                else
                {
                    freed.next = 0;
                    freed.prev = 0;
                }
                region.freeList = block;
                return;
            }
            // The first free block not below this one, else the last.
            uint32_t at = head;
            while (at < block && freeBlockAt(at).next != 0)
                at = freeBlockAt(at).next;
            if (at < block)
            {
                // Past the last free block: at the end.
                freeBlockAt(at).next = block;
                freed.prev = at;
                freed.next = 0;
            }
            else
            {
                freed.next = at;
                freed.prev = freeBlockAt(at).prev;
                freeBlockAt(freed.prev).next = block;
                freeBlockAt(at).prev = block;
            }
        }

        // Merged with the next free block when it follows on, then into the
        // previous when that ends where this starts.
        void mergeFreeBlock(uint32_t block)
        {
            DmmFreeBlock &freed = freeBlockAt(block);
            const uint32_t following = freed.next;
            if (following != 0 && block + freed.bytes == following)
            {
                freed.bytes = freed.bytes + freeBlockAt(following).bytes;
                freed.next = freeBlockAt(following).next;
                if (freed.next != 0)
                    freeBlockAt(freed.next).prev = block;
            }
            const uint32_t prev = freed.prev;
            if (prev != 0 && prev + freeBlockAt(prev).bytes == block)
            {
                freeBlockAt(prev).bytes = freeBlockAt(prev).bytes + freed.bytes;
                freeBlockAt(prev).next = freed.next;
                if (freed.next != 0)
                    freeBlockAt(freed.next).prev = prev;
            }
        }
    }

    // dmmRelease: the block of handle `index` given back (a locked one
    // unlocked first): the handle free, the block on the free list in address
    // order and merged with its neighbours; the handles' low mark moved up
    // past free handles when this was the lowest. The search's hint moves to
    // index - 1 before the handle is tested, so a bad index still moves it.
    // The result is only what the original's last test left: no caller reads it.
    int32_t dmmRelease(ee::Ptr<DmmRegion> region, uint32_t index)
    {
        const int32_t below = static_cast<int32_t>(index) - 1;
        if (below < region->searchHint())
            region->setSearchHint(below);
        const uint32_t address = handleAddress(*region, index);
        if (index == 0)
            return 0;
        if (address < region->handleLow)
            return 1;
        DmmHandle &handle = handleIn(address);
        if ((handle.bits & kDmmHandleInUse) == 0)
            return static_cast<int32_t>(handle.bits >> 1);
        if (handle.bits & kDmmHandleLocked)
            dmmUnlockHandle(region, index);

        const uint32_t block = handle.memory;
        handle.bits = handle.bits & ~kDmmHandleInUse;
        freeBlockAt(block).bytes = handle.bits >> kDmmHandleSizeShift;
        insertFreeBlock(*region, block);
        mergeFreeBlock(block);

        if (address != region->handleLow)
            return static_cast<int32_t>(region->handleLow);
        // The lowest handle: the low mark up past the free handles.
        uint32_t low = address;
        while (low < region->end && (handleIn(low).bits & kDmmHandleInUse) == 0)
            low += sizeof(DmmHandle);
        region->handleLow = low;
        region->setSearchHint(0);
        return low < region->end ? 1 : 0;
    }
    READABLE_FUNCTION(dmmRelease, Verified);

    // dmmHandleOf: the index of the handle of the block at `address`, 0 when
    // none has it.
    uint32_t dmmHandleOf(ee::Ptr<DmmRegion> region, uint32_t memory)
    {
        for (uint32_t index = 1;; ++index)
        {
            const uint32_t handle = handleAddress(*region, index);
            if (handleIn(handle).memory == memory)
                return index;
            if (region->handleLow >= handle)
                return 0;
        }
    }
    READABLE_FUNCTION(dmmHandleOf, Verified);

    namespace
    {
        bool handleInUse(uint32_t address) { return (handleIn(address).bits & kDmmHandleInUse) != 0; }
    }

    // dmmCompact (nothing calls it in AI): the region tidied in two steps.
    // First the handles: from handle 1 down, each free one takes a copy of
    // the lowest handle (which stays as it was), and the low mark moves up
    // past the handles not in use; the search hint goes back to the start.
    // Kept bug: a copied handle keeps its block in two places of the table,
    // and nothing tells the block's owner its new index. Then, with no block
    // locked, the blocks: while the first free block has a block after it
    // (dmmHandleOf finds its handle), that block moves down into the gap
    // (overlapping ones a word at a time, upwards) and the gap moves up past
    // it, merging with the next free block when they meet. 1 when it got to
    // the end, 0 when a block was locked or a block had no handle.
    uint32_t dmmCompact(ee::Ptr<DmmRegion> region)
    {
        uint32_t low = region->handleLow;
        uint32_t slot = handleAddress(*region, 1);
        while (low < slot)
        {
            if (!handleInUse(slot))
            {
                std::memcpy(&handleIn(slot), &handleIn(low), sizeof(DmmHandle)); // ldl/sdl: any alignment
                low += sizeof(DmmHandle);
                while (!handleInUse(low) && low < region->end)
                    low += sizeof(DmmHandle);
            }
            slot -= sizeof(DmmHandle);
        }
        const int32_t locked = region->lockCount;
        region->handleLow = low;
        region->setSearchHint(0);
        if (locked != 0)
            return 0;

        uint32_t gap = region->freeList;
        if (gap == kDmmNoFreeBlock)
            return 1;
        if (freeBlockAt(gap).prev == 0 && freeBlockAt(gap).next == 0)
            return 1;
        uint32_t following = freeBlockAt(gap).next;
        if (following == 0)
            return 1;
        while (true)
        {
            const uint32_t gapBytes = freeBlockAt(gap).bytes;
            const uint32_t block = gap + gapBytes;
            const uint32_t index = dmmHandleOf(region, block);
            if (index == 0)
                return 0;
            DmmHandle &handle = handleAt(*region, index);
            const uint32_t bytes = handle.bits >> kDmmHandleSizeShift;
            if (block < gap + bytes)
            {
                // It overlaps its new place: copied a word at a time, upwards.
                const ee::Ptr<uint32_t> to(gap), from(block);
                for (int32_t words = static_cast<int32_t>(bytes) >> 2, i = 0; i < words; ++i)
                    to[i] = from[i];
            }
            else
            {
                memoryCopy(gap, block, bytes);
            }
            handle.memory = gap;
            gap += handle.bits >> kDmmHandleSizeShift;
            DmmFreeBlock &moved = freeBlockAt(gap);
            moved.bytes = gapBytes;
            moved.prev = 0;
            moved.next = following;
            if (following != 0)
            {
                freeBlockAt(following).prev = gap;
                if (gap + moved.bytes == moved.next)
                {
                    // It meets the next free block: merged.
                    const DmmFreeBlock &next = freeBlockAt(moved.next);
                    moved.bytes = moved.bytes + next.bytes;
                    if (next.next == 0)
                        moved.next = 0;
                    else
                    {
                        freeBlockAt(next.next).prev = gap;
                        moved.next = freeBlockAt(moved.next).next;
                    }
                }
            }
            region->freeList = gap;
            following = moved.next;
            if (following == 0)
                return 1;
        }
    }
    READABLE_FUNCTION_WAITS(dmmCompact, Draft);

    namespace
    {
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<int32_t, sym::g_randomSeed> g_randomSeed; // g_randomSeed; its name is a low-confidence guess (readable_names.tsv)
    }

    // dmmDebugWalk (a failed allocation's, a bad handle's lock): the region's free blocks walked to the last, then its handles
    // from the top down to the lowest made. What it did with each is compiled
    // out, so it reads and changes nothing more. (Its callers pass a second
    // argument, 1, that it does not read.)
    void dmmDebugWalk(ee::Ptr<DmmRegion> region)
    {
        if (region->freeList != kDmmNoFreeBlock)
            for (ee::Ptr<DmmFreeBlock> block(region->freeList); block; block = ee::Ptr<DmmFreeBlock>(block->next))
            {
            }
        for (uint32_t handle = region->end - sizeof(DmmHandle); region->handleLow < handle; handle -= sizeof(DmmHandle))
        {
        }
    }
    READABLE_FUNCTION(dmmDebugWalk, Draft);


    // (value): g_randomSeed; setReplayMode.
    void setDmmMode(uint32_t randomSeed)
    {
        *g_randomSeed = randomSeed;
    }
    READABLE_FUNCTION(setDmmMode, Verified);
}
