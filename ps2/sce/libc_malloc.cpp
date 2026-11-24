// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdlib/mallocr.c is
// Doug Lea's malloc 2.6.4, in the public domain (its words below); stdio/findfp.c carries
// the Berkeley notice below; stdlib/mstats.c carries none.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
//   A version of malloc/free/realloc written by Doug Lea and released to the
//   public domain.
//
// Copyright (c) 1990 The Regents of the University of California.
// All rights reserved.
//
// Redistribution and use in source and binary forms are permitted
// provided that the above copyright notice and this paragraph are
// duplicated in all such forms and that any documentation,
// advertising materials, and other materials related to such
// distribution and use acknowledge that the software was developed
// by the University of California, Berkeley.  The name of the
// University may not be used to endorse or promote products derived
// from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

// libc's allocator: newlib 1.8.2's stdlib/mallocr.c (Doug Lea's malloc 2.6.4) as
// the game links it, ported from that source with its macros as small functions.
// The EE build: SIZE_SZ 4, MALLOC_ALIGNMENT 16, MINSIZE 16, no mmap, 128 bins at
// __malloc_av_ (initialTop), the page size 4096.
//
// The runtime replaces all of it (config.game.toml's stubs: a host allocator over
// the guest heap, and no recompiled function here), so with nothing for zx-fncheck
// to compare them with these stay Draft and the runtime's handlers stay the game's.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libc.h"

#include <cstdint>
#include "sym/sce/libc_malloc.cpp.h"

namespace sce::libc
{
    namespace // the game functions below call (the runtime's: stubs in config.game.toml): ee/plain.h
    {
        void memcpy(uint32_t to, uint32_t from, uint32_t bytes) { ee::game(sym::memcpy, to, from, bytes); }
        void memset(uint32_t to, int32_t byte, uint32_t bytes) { ee::game(sym::memset, to, byte, bytes); }
    }

    namespace
    {
        struct Chunk
        {
            uint32_t prevSize; // the size of the previous chunk, if it is free
            uint32_t size;     // this one's, low bits: PREV_INUSE
            uint32_t fd, bk;   // while free: the bin's list
        };
        constexpr uint32_t kSizeSz = 4u, kAlignMask = 15u, kMinSize = 16u, kPrevInuse = 1u, kSizeBits = 3u;
        constexpr uint32_t kPageSize = 4096u, kMaxSmallBinSize = 512u, kBinBlockWidth = 4u;
        constexpr int32_t kMaxSmallBin = 63;
        constexpr uint32_t kSbrkFailed = 0xFFFFFFFFu;

        constexpr uint32_t kInitialTop = sym::initialTop; // bin_at(0): __malloc_av_
        constexpr uint32_t kLastRemainder = sym::g_mallocTop; // bin_at(1)

        inline ee::Global<uint32_t, sym::g_mallocTop> g_mallocTop; // top, bin_at(0)'s fd
        inline ee::Global<ee::Ptr<Reent>, sym::g_impurePtr> g_impurePtr;
        inline ee::Global<uint32_t, sym::g_trimThreshold> g_trimThreshold;
        inline ee::Global<uint32_t, sym::g_topPad> g_topPad;
        inline ee::Global<uint32_t, sym::g_sbrkBase> g_sbrkBase;
        inline ee::Global<uint32_t, sym::g_maxSbrkedMem> g_maxSbrkedMem;
        inline ee::Global<uint32_t, sym::g_maxTotalMem> g_maxTotalMem;
        inline ee::Global<uint32_t, sym::g_sbrkedMem> g_sbrkedMem; // current_mallinfo.arena

        constexpr uint32_t kSbrkR = sym::_sbrk_r;        // _sbrk_r (reent, bytes)

        Chunk *at(uint32_t chunk) { return ee::at<Chunk>(chunk); }
        uint32_t &top() { return at(kInitialTop)->fd; }
        uint32_t &binBlocks() { return at(kInitialTop)->size; }
        constexpr uint32_t binAt(int32_t i) { return kInitialTop + static_cast<uint32_t>(i) * 8u; }
        constexpr uint32_t chunkToMem(uint32_t p) { return p + 2u * kSizeSz; }
        constexpr uint32_t memToChunk(uint32_t mem) { return mem - 2u * kSizeSz; }
        uint32_t chunkSize(uint32_t p) { return at(p)->size & ~kSizeBits; }
        bool prevInuse(uint32_t p) { return (at(p)->size & kPrevInuse) != 0u; }
        uint32_t nextChunk(uint32_t p) { return p + (at(p)->size & ~kPrevInuse); }
        bool inuse(uint32_t p) { return prevInuse(nextChunk(p)); }
        void setInuseBitAt(uint32_t p, uint32_t s) { at(p + s)->size |= kPrevInuse; }
        void setHeadSize(uint32_t p, uint32_t s) { at(p)->size = (at(p)->size & kPrevInuse) | s; }
        void setHead(uint32_t p, uint32_t s) { at(p)->size = s; }
        void setFoot(uint32_t p, uint32_t s) { at(p + s)->prevSize = s; }
        constexpr int64_t longSub(uint32_t x, uint32_t y) { return static_cast<int64_t>(x) - static_cast<int64_t>(y); }

        uint32_t request2size(uint32_t req)
        {
            const uint32_t padded = req + kSizeSz + kAlignMask;
            return static_cast<int32_t>(padded) < static_cast<int32_t>(kMinSize + kAlignMask) ? (kMinSize + kAlignMask) & ~kAlignMask
                                                                                               : padded & ~kAlignMask;
        }

        constexpr int32_t smallBinIndex(uint32_t sz) { return static_cast<int32_t>(sz >> 3); }
        int32_t binIndex(uint32_t sz)
        {
            const uint32_t blocks = sz >> 9;
            if (blocks == 0u)
                return static_cast<int32_t>(sz >> 3);
            if (blocks <= 4u)
                return 56 + static_cast<int32_t>(sz >> 6);
            if (blocks <= 20u)
                return 91 + static_cast<int32_t>(sz >> 9);
            if (blocks <= 84u)
                return 110 + static_cast<int32_t>(sz >> 12);
            if (blocks <= 340u)
                return 119 + static_cast<int32_t>(sz >> 15);
            if (blocks <= 1364u)
                return 124 + static_cast<int32_t>(sz >> 18);
            return 126;
        }
        constexpr uint32_t binBlockOf(int32_t i) { return 1u << (static_cast<uint32_t>(i) / kBinBlockWidth); }

        void unlinkChunk(uint32_t p)
        {
            const uint32_t bk = at(p)->bk, fd = at(p)->fd;
            at(fd)->bk = bk;
            at(bk)->fd = fd;
        }
        // frontlink: into its bin, a large bin kept in size order.
        void frontLink(uint32_t p, uint32_t s)
        {
            uint32_t bk, fd;
            if (s < kMaxSmallBinSize)
            {
                const int32_t idx = smallBinIndex(s);
                binBlocks() |= binBlockOf(idx);
                bk = binAt(idx);
                fd = at(bk)->fd;
            }
            else
            {
                const int32_t idx = binIndex(s);
                bk = binAt(idx);
                fd = at(bk)->fd;
                if (fd == bk)
                    binBlocks() |= binBlockOf(idx);
                else
                {
                    while (fd != bk && s < chunkSize(fd))
                        fd = at(fd)->fd;
                    bk = at(fd)->bk;
                }
            }
            at(p)->bk = bk;
            at(p)->fd = fd;
            at(fd)->bk = p;
            at(bk)->fd = p;
        }
        void linkLastRemainder(uint32_t p)
        {
            at(kLastRemainder)->fd = p;
            at(kLastRemainder)->bk = p;
            at(p)->fd = kLastRemainder;
            at(p)->bk = kLastRemainder;
        }
        void clearLastRemainder()
        {
            at(kLastRemainder)->fd = kLastRemainder;
            at(kLastRemainder)->bk = kLastRemainder;
        }
        namespace // the game functions below call: ee/plain.h
        {
            uint32_t sbrkR(uint32_t reent, uint32_t incr) { return ee::game<uint32_t>(kSbrkR, reent, incr); }
        }

        uint32_t sbrk(uint32_t reent, int32_t bytes) { return sbrkR(reent, bytes); }

        void lock(uint32_t reent) { __malloc_lock(reent); }

        void unlock(uint32_t reent) { __malloc_unlock(reent); }
    }

    void _free_r(uint32_t reent, uint32_t mem);
    int32_t _malloc_trim_r(uint32_t reent, uint32_t pad);

    // malloc_extend_top: the arena grows by sbrk. Foreign sbrks leave a hole:
    // the old top is fenced off and freed.
    void malloc_extend_top(uint32_t reent, uint32_t nb)
    {
        const uint32_t oldTop = top();
        uint32_t oldTopSize = chunkSize(oldTop);
        const uint32_t oldEnd = oldTop + oldTopSize;
        uint32_t sbrkSize = nb + *g_topPad + kMinSize;
        if (*g_sbrkBase != kSbrkFailed)
            sbrkSize = (sbrkSize + (kPageSize - 1u)) & ~(kPageSize - 1u);
        uint32_t brk = sbrk(reent, static_cast<int32_t>(sbrkSize));
        if (brk == kSbrkFailed || (brk < oldEnd && oldTop != kInitialTop))
            return;
        *g_sbrkedMem += sbrkSize;
        if (brk == oldEnd)
            setHead(top(), (sbrkSize + oldTopSize) | kPrevInuse);
        else
        {
            if (*g_sbrkBase == kSbrkFailed)
                *g_sbrkBase = brk;
            else
                *g_sbrkedMem += brk - oldEnd;
            uint32_t correction = 0u;
            if (const uint32_t misaligned = chunkToMem(brk) & kAlignMask; misaligned > 0u)
            {
                correction = (kAlignMask + 1u) - misaligned;
                brk += correction;
            }
            correction += kPageSize - ((brk + sbrkSize) & (kPageSize - 1u));
            const uint32_t newBrk = sbrk(reent, static_cast<int32_t>(correction));
            if (newBrk == kSbrkFailed)
                return;
            *g_sbrkedMem += correction;
            top() = brk;
            setHead(brk, (newBrk - brk + correction) | kPrevInuse);
            if (oldTop != kInitialTop)
            {
                if (oldTopSize < kMinSize)
                {
                    setHead(brk, kPrevInuse);
                    return;
                }
                oldTopSize = (oldTopSize - 3u * kSizeSz) & ~kAlignMask;
                setHeadSize(oldTop, oldTopSize);
                at(oldTop + oldTopSize)->size = kSizeSz | kPrevInuse;
                at(oldTop + oldTopSize + kSizeSz)->size = kSizeSz | kPrevInuse;
                if (oldTopSize >= kMinSize)
                    _free_r(reent, chunkToMem(oldTop));
            }
        }
        if (*g_sbrkedMem > *g_maxSbrkedMem)
            *g_maxSbrkedMem = *g_sbrkedMem;
        if (*g_sbrkedMem > *g_maxTotalMem)
            *g_maxTotalMem = *g_sbrkedMem;
    }

    uint32_t _malloc_r(uint32_t reent, uint32_t bytes)
    {
        const uint32_t nb = request2size(bytes);
        const auto taken = [&](uint32_t victim) {
            unlock(reent);
            return chunkToMem(victim);
        };
        lock(reent);
        int32_t idx;
        uint32_t victim, victimSize;
        int64_t remainderSize;
        if (nb < kMaxSmallBinSize - 8u)
        {
            // The exact small bin (with chunks 16-aligned, the next one is never right).
            idx = smallBinIndex(nb);
            const uint32_t q = binAt(idx);
            victim = at(q)->bk;
            if (victim != q)
            {
                victimSize = chunkSize(victim);
                unlinkChunk(victim);
                setInuseBitAt(victim, victimSize);
                return taken(victim);
            }
            idx += 2;
        }
        else
        {
            idx = binIndex(nb);
            const uint32_t bin = binAt(idx);
            bool split = false;
            for (victim = at(bin)->bk; victim != bin; victim = at(victim)->bk)
            {
                victimSize = chunkSize(victim);
                remainderSize = longSub(victimSize, nb);
                if (remainderSize >= kMinSize)
                {
                    split = true;
                    break; // too big: leave it to the remainder's split below
                }
                if (remainderSize >= 0)
                {
                    unlinkChunk(victim);
                    setInuseBitAt(victim, victimSize);
                    return taken(victim);
                }
            }
            idx += split ? 0 : 1;
        }

        // The last remainder, the most recently split chunk.
        victim = at(kLastRemainder)->fd;
        if (victim != kLastRemainder)
        {
            victimSize = chunkSize(victim);
            remainderSize = longSub(victimSize, nb);
            if (remainderSize >= kMinSize)
            {
                const uint32_t remainder = victim + nb;
                setHead(victim, nb | kPrevInuse);
                linkLastRemainder(remainder);
                setHead(remainder, static_cast<uint32_t>(remainderSize) | kPrevInuse);
                setFoot(remainder, static_cast<uint32_t>(remainderSize));
                return taken(victim);
            }
            clearLastRemainder();
            if (remainderSize >= 0)
            {
                setInuseBitAt(victim, victimSize);
                return taken(victim);
            }
            frontLink(victim, victimSize);
        }

        // Other bins, a block of 4 at a time by the binblocks bitmap.
        uint32_t block = binBlockOf(idx);
        if (block <= binBlocks())
        {
            if ((block & binBlocks()) == 0u)
            {
                idx = (idx & ~static_cast<int32_t>(kBinBlockWidth - 1u)) + static_cast<int32_t>(kBinBlockWidth);
                block <<= 1;
                while ((block & binBlocks()) == 0u)
                {
                    idx += static_cast<int32_t>(kBinBlockWidth);
                    block <<= 1;
                }
            }
            for (;;)
            {
                int32_t startIdx = idx;
                uint32_t bin = binAt(idx), q = bin;
                do
                {
                    for (victim = at(bin)->bk; victim != bin; victim = at(victim)->bk)
                    {
                        victimSize = chunkSize(victim);
                        remainderSize = longSub(victimSize, nb);
                        if (remainderSize >= kMinSize)
                        {
                            const uint32_t remainder = victim + nb;
                            setHead(victim, nb | kPrevInuse);
                            unlinkChunk(victim);
                            linkLastRemainder(remainder);
                            setHead(remainder, static_cast<uint32_t>(remainderSize) | kPrevInuse);
                            setFoot(remainder, static_cast<uint32_t>(remainderSize));
                            return taken(victim);
                        }
                        if (remainderSize >= 0)
                        {
                            setInuseBitAt(victim, victimSize);
                            unlinkChunk(victim);
                            return taken(victim);
                        }
                    }
                    bin += 8u;
                    if (idx < kMaxSmallBin)
                    {
                        bin += 8u;
                        ++idx;
                    }
                } while ((++idx & static_cast<int32_t>(kBinBlockWidth - 1u)) != 0);

                // The block's bins are empty: clear its bit.
                do
                {
                    if ((startIdx & static_cast<int32_t>(kBinBlockWidth - 1u)) == 0)
                    {
                        binBlocks() &= ~block;
                        break;
                    }
                    --startIdx;
                    q -= 8u;
                } while (at(q)->fd == q);

                block <<= 1;
                if (block > binBlocks() || block == 0u)
                    break;
                while ((block & binBlocks()) == 0u)
                {
                    idx += static_cast<int32_t>(kBinBlockWidth);
                    block <<= 1;
                }
            }
        }

        // The top chunk, extended if it has to be.
        remainderSize = longSub(chunkSize(top()), nb);
        if (chunkSize(top()) < nb || remainderSize < kMinSize)
        {
            malloc_extend_top(reent, nb);
            remainderSize = longSub(chunkSize(top()), nb);
            if (chunkSize(top()) < nb || remainderSize < kMinSize)
            {
                unlock(reent);
                return 0u;
            }
        }
        victim = top();
        setHead(victim, nb | kPrevInuse);
        top() = victim + nb;
        setHead(top(), static_cast<uint32_t>(remainderSize) | kPrevInuse);
        return taken(victim);
    }

    void _free_r(uint32_t reent, uint32_t mem)
    {
        if (mem == 0u)
            return;
        lock(reent);
        uint32_t p = memToChunk(mem);
        const uint32_t hd = at(p)->size;
        uint32_t sz = hd & ~kPrevInuse;
        const uint32_t next = p + sz;
        const uint32_t nextSize = chunkSize(next);
        if (next == top())
        {
            sz += nextSize;
            if (!(hd & kPrevInuse))
            {
                const uint32_t prevSize = at(p)->prevSize;
                p -= prevSize;
                sz += prevSize;
                unlinkChunk(p);
            }
            setHead(p, sz | kPrevInuse);
            top() = p;
            if (sz >= *g_trimThreshold)
                _malloc_trim_r(reent, *g_topPad);
            unlock(reent);
            return;
        }
        setHead(next, nextSize);
        bool lastRemainder = false;
        if (!(hd & kPrevInuse))
        {
            const uint32_t prevSize = at(p)->prevSize;
            p -= prevSize;
            sz += prevSize;
            if (at(p)->fd == kLastRemainder)
                lastRemainder = true;
            else
                unlinkChunk(p);
        }
        if (!(at(next + nextSize)->size & kPrevInuse))
        {
            sz += nextSize;
            if (!lastRemainder && at(next)->fd == kLastRemainder)
            {
                lastRemainder = true;
                linkLastRemainder(p);
            }
            else
                unlinkChunk(next);
        }
        setHead(p, sz | kPrevInuse);
        setFoot(p, sz);
        if (!lastRemainder)
            frontLink(p, sz);
        unlock(reent);
    }

    uint32_t _realloc_r(uint32_t reent, uint32_t oldMem, uint32_t bytes)
    {
        if (oldMem == 0u)
            return _malloc_r(reent, bytes);
        lock(reent);
        const uint32_t oldP = memToChunk(oldMem);
        const uint32_t oldSize = chunkSize(oldP);
        uint32_t newP = oldP, newSize = oldSize;
        const uint32_t nb = request2size(bytes);
        const int64_t need = static_cast<int64_t>(nb);

        const auto split = [&] {
            const int64_t remainderSize = longSub(newSize, nb);
            if (remainderSize >= kMinSize)
            {
                const uint32_t remainder = newP + nb;
                setHeadSize(newP, nb);
                setHead(remainder, static_cast<uint32_t>(remainderSize) | kPrevInuse);
                setInuseBitAt(remainder, static_cast<uint32_t>(remainderSize));
                _free_r(reent, chunkToMem(remainder));
            }
            else
            {
                setHeadSize(newP, newSize);
                setInuseBitAt(newP, newSize);
            }
            unlock(reent);
            return chunkToMem(newP);
        };
        if (static_cast<int64_t>(oldSize) >= need)
            return split();

        // The next chunk (or the top), then the previous one, then a new block.
        uint32_t next = oldP + oldSize;
        uint32_t nextSize = 0u;
        if (next == top() || !inuse(next))
        {
            nextSize = chunkSize(next);
            if (next == top())
            {
                if (static_cast<int64_t>(nextSize + newSize) >= need + kMinSize)
                {
                    newSize += nextSize;
                    top() = oldP + nb;
                    setHead(top(), (newSize - nb) | kPrevInuse);
                    setHeadSize(oldP, nb);
                    unlock(reent);
                    return chunkToMem(oldP);
                }
            }
            else if (static_cast<int64_t>(nextSize + newSize) >= need)
            {
                unlinkChunk(next);
                newSize += nextSize;
                return split();
            }
        }
        else
            next = 0u;

        if (!prevInuse(oldP))
        {
            const uint32_t prev = oldP - at(oldP)->prevSize;
            const uint32_t prevSize = chunkSize(prev);
            if (next != 0u)
            {
                if (next == top())
                {
                    if (static_cast<int64_t>(nextSize + prevSize + newSize) >= need + kMinSize)
                    {
                        unlinkChunk(prev);
                        newP = prev;
                        newSize += prevSize + nextSize;
                        const uint32_t newMem = chunkToMem(newP);
                        memcpy(newMem, oldMem, oldSize - kSizeSz);
                        top() = newP + nb;
                        setHead(top(), (newSize - nb) | kPrevInuse);
                        setHeadSize(newP, nb);
                        unlock(reent);
                        return newMem;
                    }
                }
                else if (static_cast<int64_t>(nextSize + prevSize + newSize) >= need)
                {
                    unlinkChunk(next);
                    unlinkChunk(prev);
                    newP = prev;
                    newSize += nextSize + prevSize;
                    memcpy(chunkToMem(newP), oldMem, oldSize - kSizeSz);
                    return split();
                }
            }
            if (prev != 0u && static_cast<int64_t>(prevSize + newSize) >= need)
            {
                unlinkChunk(prev);
                newP = prev;
                newSize += prevSize;
                memcpy(chunkToMem(newP), oldMem, oldSize - kSizeSz);
                return split();
            }
        }

        const uint32_t newMem = _malloc_r(reent, bytes);
        if (newMem == 0u)
        {
            unlock(reent);
            return 0u;
        }
        newP = memToChunk(newMem);
        if (newP == nextChunk(oldP))
        {
            // It came from right after the old block: the two as one.
            newSize += chunkSize(newP);
            newP = oldP;
            return split();
        }
        memcpy(newMem, oldMem, oldSize - kSizeSz);
        _free_r(reent, oldMem);
        unlock(reent);
        return newMem;
    }

    uint32_t _memalign_r(uint32_t reent, uint32_t alignment, uint32_t bytes)
    {
        if (alignment <= kAlignMask + 1u)
            return _malloc_r(reent, bytes);
        if (alignment < kMinSize)
            alignment = kMinSize;
        const uint32_t nb = request2size(bytes);
        const uint32_t m = _malloc_r(reent, nb + alignment + kMinSize);
        if (m == 0u)
            return 0u;
        lock(reent);
        uint32_t p = memToChunk(m);
        if (m % alignment != 0u)
        {
            // The first aligned spot with room for a chunk before it; that lead is freed.
            uint32_t brk = memToChunk((m + alignment - 1u) & (0u - alignment));
            if (static_cast<int32_t>(brk - p) < static_cast<int32_t>(kMinSize))
                brk += alignment;
            const uint32_t leadSize = brk - p;
            const uint32_t newSize = chunkSize(p) - leadSize;
            setHead(brk, newSize | kPrevInuse);
            setInuseBitAt(brk, newSize);
            setHeadSize(p, leadSize);
            _free_r(reent, chunkToMem(p));
            p = brk;
        }
        const int64_t remainderSize = longSub(chunkSize(p), nb);
        if (remainderSize >= kMinSize)
        {
            const uint32_t remainder = p + nb;
            setHead(remainder, static_cast<uint32_t>(remainderSize) | kPrevInuse);
            setHeadSize(p, nb);
            _free_r(reent, chunkToMem(remainder));
        }
        unlock(reent);
        return chunkToMem(p);
    }

    uint32_t _calloc_r(uint32_t reent, uint32_t n, uint32_t elementSize)
    {
        const uint32_t mem = _malloc_r(reent, n * elementSize);
        if (mem != 0u)
            memset(mem, 0, chunkSize(memToChunk(mem)) - kSizeSz);
        return mem;
    }

    // malloc_trim: unused pages at the top go back by a negative sbrk.
    int32_t _malloc_trim_r(uint32_t reent, uint32_t pad)
    {
        lock(reent);
        const uint32_t topSize = chunkSize(top());
        const int64_t extra = (static_cast<int64_t>((topSize - pad - kMinSize + (kPageSize - 1u)) / kPageSize) - 1) * kPageSize;
        if (extra < kPageSize)
        {
            unlock(reent);
            return 0;
        }
        const uint32_t current = sbrk(reent, 0);
        if (current != top() + topSize)
        {
            unlock(reent);
            return 0;
        }
        if (sbrk(reent, -static_cast<int32_t>(extra)) == kSbrkFailed)
        {
            // Whatever sbrk left: take the arena from where it is now.
            const uint32_t now = sbrk(reent, 0);
            const uint32_t size = now - top();
            if (static_cast<int32_t>(size) >= static_cast<int32_t>(kMinSize))
            {
                *g_sbrkedMem = now - *g_sbrkBase;
                setHead(top(), size | kPrevInuse);
            }
            unlock(reent);
            return 0;
        }
        setHead(top(), (topSize - static_cast<uint32_t>(extra)) | kPrevInuse);
        *g_sbrkedMem -= static_cast<uint32_t>(extra);
        unlock(reent);
        return 1;
    }

    // Sony's build has one thread here, so the locks are empty.
    void __malloc_lock(ee::Address /*ptr*/)
    {
    }
    READABLE_FUNCTION(__malloc_lock, Verified);

    void __malloc_unlock(ee::Address /*ptr*/)
    {
    }
    READABLE_FUNCTION(__malloc_unlock, Verified);

    namespace
    {
        // current_mallinfo (MallocInfo: sce/newlib.h).
        inline ee::Global<MallocInfo, sym::g_sbrkedMem> g_currentMallinfo;

        constexpr int kBinCount = 128; // NAV

        constexpr uint32_t binAddress(int i) { return kInitialTop + static_cast<uint32_t>(i) * 8u; } // bin_at
        uint32_t chunkSize(ee::Ptr<Chunk> p) { return p->size & ~kSizeBits; }
    }

    // malloc_update_mallinfo: counts the free chunks in every bin, and the top one.
    void malloc_update_mallinfo()
    {
        const ee::Ptr<Chunk> top(*g_mallocTop);
        uint32_t free = chunkSize(top);
        int32_t freeChunks = free >= kMinSize ? 1 : 0;
        for (int i = 1; i < kBinCount; ++i)
        {
            const ee::Ptr<Chunk> bin(binAddress(i));
            for (ee::Ptr<Chunk> p(bin->bk); p.get() != bin.get(); p = ee::Ptr<Chunk>(p->bk))
            {
                ++freeChunks;
                free += chunkSize(p);
            }
        }
        g_currentMallinfo->ordblks = freeChunks;
        g_currentMallinfo->uordblks = static_cast<int32_t>(*g_sbrkedMem - free);
        g_currentMallinfo->fordblks = static_cast<int32_t>(free);
        g_currentMallinfo->keepcost = static_cast<int32_t>(chunkSize(top));
    }
    READABLE_FUNCTION(malloc_update_mallinfo, Verified);

    // malloc_stats: three lines on stderr. HAVE_MMAP is off, and the third is uordblks.
    void _malloc_stats_r(ee::Ptr<Reent> reent)
    {
        ee::Local<MallocInfo> snapshot;
        __malloc_lock(reent);
        malloc_update_mallinfo();
        const uint32_t maxTotalMem = *g_maxTotalMem;
        const uint32_t sbrkedMem = *g_sbrkedMem;
        *snapshot = *g_currentMallinfo;
        __malloc_unlock(reent);
        const ee::Ptr<File> errorOutput = reent->standardErr;
        fiprintf(errorOutput, sym::g_textMaxSystemBytes, static_cast<int32_t>(maxTotalMem));
        fiprintf(errorOutput, sym::g_textSystemBytes, static_cast<int32_t>(sbrkedMem));
        fiprintf(errorOutput, sym::g_textInUseBytes, snapshot->uordblks);
    }
    READABLE_FUNCTION_WAITS(_malloc_stats_r, Verified);

    void malloc_stats()
    {
        game::tail(_malloc_stats_r, *g_impurePtr);
    }
    READABLE_FUNCTION_WAITS(malloc_stats, Verified);

    // stdlib/mstats.c.
    void _mstats_r(ee::Ptr<Reent> reent, uint32_t title)
    {
        fiprintf(reent->standardErr, sym::g_textMemoryAllocationStatistics, static_cast<int32_t>(title));
        game::tail(_malloc_stats_r, reent);
    }
    READABLE_FUNCTION_WAITS(_mstats_r, Draft);

    void mstats(uint32_t title)
    {
        game::tail(_mstats_r, *g_impurePtr, title);
    }
    READABLE_FUNCTION_WAITS(mstats, Draft);

    // mallinfo: the numbers, into the caller's record (returned as a struct, so its address comes first).
    void _mallinfo_r(ee::Ptr<MallocInfo> result, ee::Ptr<Reent> reent)
    {
        ee::Local<MallocInfo> snapshot;
        __malloc_lock(reent);
        malloc_update_mallinfo();
        *snapshot = *g_currentMallinfo;
        __malloc_unlock(reent);
        *result = *snapshot;
    }
    READABLE_FUNCTION(_mallinfo_r, Draft);

    // mallinfo (stdlib/mstats.c): _mallinfo_r with _REENT. The record comes back through the
    // place its caller gives, as a returned struct does.
    ee::Ptr<MallocInfo> mallinfo(ee::Ptr<MallocInfo> result)
    {
        _mallinfo_r(result, *g_impurePtr);
        return result;
    }
    READABLE_FUNCTION(mallinfo, Draft);

    namespace
    {
        // mallopt's parameters (M_TRIM_THRESHOLD .. M_MMAP_MAX).
        constexpr int32_t kTrimThresholdParameter = -1, kTopPadParameter = -2;
        constexpr int32_t kMmapThresholdParameter = -3, kMmapMaxParameter = -4;
        // trim_threshold and top_pad are unsigned long, 64 bits on the EE: mallopt sets them
        // whole (the value sign-extended), where the allocator above reads their low word.
        inline ee::Global<int64_t, sym::g_trimThreshold> g_trimThresholdWhole;
        inline ee::Global<int64_t, sym::g_topPad> g_topPadWhole;
    }

    // _mallopt_r (mallocr.c's mALLOPt): sets one of the allocator's tuning values; 1 if it
    // took it. This build has no mmap: its threshold is taken and ignored, and its maximum
    // only as 0.
    int32_t _mallopt_r(ee::Ptr<Reent> reent, int32_t parameter, int32_t value)
    {
        __malloc_lock(reent);
        switch (parameter)
        {
        case kTrimThresholdParameter:
            *g_trimThresholdWhole = value;
            __malloc_unlock(reent);
            return 1;
        case kTopPadParameter:
            *g_topPadWhole = value;
            __malloc_unlock(reent);
            return 1;
        case kMmapThresholdParameter:
            __malloc_unlock(reent);
            return 1;
        case kMmapMaxParameter:
            __malloc_unlock(reent);
            return value == 0 ? 1 : 0;
        default:
            __malloc_unlock(reent);
            return 0;
        }
    }
    READABLE_FUNCTION(_mallopt_r, Draft);

    // mallopt (stdlib/mstats.c): _mallopt_r with _REENT.
    int32_t mallopt(int32_t parameter, int32_t value)
    {
        return _mallopt_r(*g_impurePtr, parameter, value);
    }
    READABLE_FUNCTION(mallopt, Draft);

    namespace
    {
        ee::Ptr<FileBlock> allocateFileBlock(ee::Ptr<Reent> reent, uint32_t bytes) { return ee::game<ee::Ptr<FileBlock>>(sym::_malloc_r, reent, bytes); }
        void clearFiles(ee::Ptr<File> files, uint32_t bytes) { ee::game(sym::memset, files, 0, bytes); }
        // The files follow their block.
        ee::Ptr<File> filesAfter(ee::Ptr<FileBlock> block) { return reinterpret_cast<File *>(block.get() + 1); }
    }

    // stdio/findfp.c: another block of n FILEs.
    ee::Ptr<FileBlock> __sfmoreglue(ee::Ptr<Reent> reent, int32_t n)
    {
        const uint32_t fileBytes = static_cast<uint32_t>(n) * sizeof(File);
        const ee::Ptr<FileBlock> block = allocateFileBlock(reent, sizeof(FileBlock) + fileBytes);
        if (!block)
            return block;
        const ee::Ptr<File> files = filesAfter(block);
        block->next = ee::Ptr<FileBlock>();
        block->fileCount = n;
        block->files = files;
        clearFiles(files, fileBytes);
        return block;
    }
    READABLE_FUNCTION(__sfmoreglue, Draft);
}
