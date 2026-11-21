#pragma once

// The runtime's heap of the game's memory: what the libc stubs (malloc, memalign, calloc, realloc, free and
// their _r forms, which each game's recompiled libc calls) and the runtime's own guest allocations get.
// PS2Machine keeps one (guestMalloc and its family, under its heap mutex) and decides the policy: the 32 MB
// heap first, extended memory after it, where the heap starts and ends.
//
// The blocks cover the heap's ranges in address order, each free or in use. An allocation is the first free
// block in address order (of the 32 MB, or of the extended memory above it) that holds the size at the
// alignment asked for; it is split into the free space before it, the block and the free space after it.
// A freed block is merged with the free blocks it touches. Which address a request gets is the game's
// behaviour (the pointers land in its RAM), so it is exactly this rule; the indexes below only find the
// same block faster: the blocks in use by address (a hash), the free ones in address order, and per 4 KB
// of address the largest free block that starts there, in a tree of maxima, so the search skips every
// stretch of free blocks too small to hold the request.

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

class GuestHeap
{
public:
    GuestHeap();

    // No blocks.
    void clear();
    // A free range at the end of the heap's ranges (above every block so far), not merged with any: a
    // reset lays out its ranges this way and they stay apart until the next merge (release, shrink, add).
    void appendFree(uint32_t addr, uint32_t size);
    // A free range among the blocks (in none of them), then every touching pair of free blocks merged.
    void insertFree(uint32_t addr, uint32_t size);

    // The first free block in address order, of the 32 MB (extended false: blocks starting below
    // PS2_LEGACY_RAM_SIZE) or of the extended memory (true), where `size` bytes (a multiple of 16) start
    // at a multiple of `alignment` (a power of two); 0 if none holds it. The block in use is
    // [result, result + size).
    uint32_t allocate(uint32_t size, uint32_t alignment, bool extended);
    // The size of the block in use at `addr`, or 0 if no block in use starts there.
    uint32_t usedSize(uint32_t addr) const;
    // The block in use at `addr` freed and merged with the free blocks it touches; false if there is none.
    bool release(uint32_t addr);
    // The block in use at `addr` (of more than `size` bytes) cut to `size`, its tail freed and merged.
    void shrink(uint32_t addr, uint32_t size);
    // The block in use at `addr` grown to `size` bytes from the free block right after it, if that one
    // starts where it ends and the two hold `size`; false (nothing changed) otherwise.
    bool growInPlace(uint32_t addr, uint32_t size);
    // The free space of the 32 MB at and above `floor` taken away (the main thread's stack is there).
    void cutFreeAbove(uint32_t floor);
    // The free space at and above `floor` taken away, of the 32 MB and of extended memory alike (the top of
    // extended memory kept out of the heap: PS2Machine::reserveExtendedTop).
    void cutFreeFrom(uint32_t floor);
    // Whether a block in use ends above `addr`.
    bool usedAbove(uint32_t addr) const;
    // The bytes in use in blocks that start in extended memory.
    uint32_t extendedInUse() const;

    // The bytes in use in blocks that start in the 32 MB and in extended memory, and the most each has held at
    // once since the heap was cleared or the peaks were reset (the high-water marks ZX_TRACE_HEAP logs:
    // runtime/heap_trace.h). Counting changes no block.
    struct Usage
    {
        uint32_t mainInUse = 0;
        uint32_t mainPeak = 0;
        uint32_t extendedInUse = 0;
        uint32_t extendedPeak = 0;
    };
    const Usage &usage() const { return m_usage; }
    // The peaks from here on: each the bytes in use now.
    void resetPeaks();

private:
    static constexpr uint32_t kBucketShift = 12; // 4 KB of address a bucket
    using FreeMap = std::map<uint32_t, uint32_t>;

    static uint32_t bucketOf(uint32_t addr);
    void addFreeBlock(uint32_t addr, uint32_t size);
    FreeMap::iterator eraseFreeBlock(FreeMap::iterator it);
    void resizeFreeBlock(FreeMap::iterator it, uint32_t size);
    void moveFreeBlock(FreeMap::iterator it, uint32_t addr, uint32_t size);
    void bucketGrew(uint32_t bucket, uint32_t size);
    void bucketLost(uint32_t bucket, uint32_t size);
    void setBucketMax(uint32_t bucket, uint32_t value);
    // The first bucket at or after `from` and before `end` whose largest free block is at least `size`;
    // `end` if none.
    uint32_t nextBucketWith(uint32_t from, uint32_t end, uint32_t size) const;
    // The free space at and above `floor` taken away, of the free blocks that start below `end`.
    void cutFree(uint32_t floor, uint32_t end);
    // The free block at `it` merged with the free ones it touches (before and after), itself or the one it
    // merged into returned.
    FreeMap::iterator mergeAround(FreeMap::iterator it);
    // Every touching pair of free blocks merged, if any may be (after appendFree); else the one at `it`.
    void mergeAfterChange(FreeMap::iterator it);
    void mergeAll();
    // `bytes` more (taken) or fewer (given back) in use in the block at `addr`'s part of the RAM.
    void taken(uint32_t addr, uint32_t bytes);
    void givenBack(uint32_t addr, uint32_t bytes);

    std::unordered_map<uint32_t, uint32_t> m_used; // address -> size
    FreeMap m_free;                                // address -> size, in address order
    uint32_t m_buckets = 0;                        // a power of two
    std::vector<uint32_t> m_maxTree;               // [m_buckets + b]: bucket b's largest; above, the maxima
    bool m_mayTouch = false;                       // appendFree may have left touching free blocks
    Usage m_usage;                                 // usage, extendedInUse
};
