// The runtime's heap of the game's memory (runtime/guest_heap.h): first fit in address order, found through a
// tree of the largest free block per 4 KB of address.

#include "runtime/guest_heap.h"

#include "runtime/ps2_memory.h"

#include <algorithm>
#include <iterator>

namespace
{
    constexpr uint32_t kExtendedFirstBucket = PS2_LEGACY_RAM_SIZE >> 12; // (GuestHeap::kBucketShift)

    constexpr uint32_t bucketCount()
    {
        uint32_t count = 1;
        while (count < (PS2_RAM_SIZE >> 12))
            count <<= 1;
        return count;
    }
    constexpr uint32_t kBucketCount = bucketCount();

    uint64_t endOf(uint32_t addr, uint32_t size) { return static_cast<uint64_t>(addr) + size; }
}

static_assert((PS2_LEGACY_RAM_SIZE & ((1u << 12) - 1u)) == 0, "the 32 MB ends on a bucket's edge");

GuestHeap::GuestHeap() : m_buckets(kBucketCount), m_maxTree(2u * kBucketCount, 0u) {}

uint32_t GuestHeap::bucketOf(uint32_t addr)
{
    // (Every block is in the RAM; one past it would share the last bucket.)
    return std::min(addr >> kBucketShift, kBucketCount - 1u);
}

void GuestHeap::clear()
{
    m_used.clear();
    m_free.clear();
    std::fill(m_maxTree.begin(), m_maxTree.end(), 0u);
    m_mayTouch = false;
    m_usage = Usage{};
}

void GuestHeap::appendFree(uint32_t addr, uint32_t size)
{
    if (!m_free.empty())
    {
        const auto last = std::prev(m_free.end());
        if (endOf(last->first, last->second) == addr)
            m_mayTouch = true;
    }
    addFreeBlock(addr, size);
}

void GuestHeap::insertFree(uint32_t addr, uint32_t size)
{
    addFreeBlock(addr, size);
    mergeAfterChange(m_free.find(addr));
}

uint32_t GuestHeap::allocate(uint32_t size, uint32_t alignment, bool extended)
{
    const uint32_t firstBucket = extended ? kExtendedFirstBucket : 0u;
    const uint32_t endBucket = extended ? m_buckets : kExtendedFirstBucket;
    const uint64_t mask = alignment - 1u;
    for (uint32_t bucket = nextBucketWith(firstBucket, endBucket, size); bucket < endBucket;
         bucket = nextBucketWith(bucket + 1u, endBucket, size))
    {
        for (auto it = m_free.lower_bound(bucket << kBucketShift); it != m_free.end() && bucketOf(it->first) == bucket;
             ++it)
        {
            const uint32_t addr = it->first;
            const uint32_t blockSize = it->second;
            if ((addr >= PS2_LEGACY_RAM_SIZE) != extended)
                continue;
            const uint64_t blockEnd = endOf(addr, blockSize);
            const uint64_t aligned = std::min<uint64_t>((addr + mask) & ~mask, UINT32_MAX);
            if (aligned > blockEnd || aligned + size > blockEnd)
                continue;
            // Split: the free space before it, the block, the free space after it.
            const uint32_t start = static_cast<uint32_t>(aligned);
            const uint64_t allocEnd = aligned + size;
            if (start > addr)
                resizeFreeBlock(it, start - addr);
            else
                eraseFreeBlock(it);
            if (allocEnd < blockEnd)
                addFreeBlock(static_cast<uint32_t>(allocEnd), static_cast<uint32_t>(blockEnd - allocEnd));
            m_used.emplace(start, size);
            taken(start, size);
            return start;
        }
    }
    return 0u;
}

uint32_t GuestHeap::usedSize(uint32_t addr) const
{
    const auto it = m_used.find(addr);
    return it == m_used.end() ? 0u : it->second;
}

bool GuestHeap::release(uint32_t addr)
{
    const auto it = m_used.find(addr);
    if (it == m_used.end())
        return false;
    const uint32_t size = it->second;
    m_used.erase(it);
    givenBack(addr, size);
    addFreeBlock(addr, size);
    mergeAfterChange(m_free.find(addr));
    return true;
}

void GuestHeap::shrink(uint32_t addr, uint32_t size)
{
    uint32_t &used = m_used.at(addr);
    const uint32_t tail = used - size;
    used = size;
    givenBack(addr, tail);
    addFreeBlock(addr + size, tail);
    mergeAfterChange(m_free.find(addr + size));
}

bool GuestHeap::growInPlace(uint32_t addr, uint32_t size)
{
    uint32_t &used = m_used.at(addr);
    const auto next = m_free.find(addr + used);
    if (next == m_free.end() || static_cast<uint64_t>(used) + next->second < size)
        return false;
    const uint32_t extra = size - used;
    if (next->second == extra)
        eraseFreeBlock(next);
    else
        moveFreeBlock(next, next->first + extra, next->second - extra);
    used = size;
    taken(addr, extra);
    return true;
}

void GuestHeap::cutFreeAbove(uint32_t floor) { cutFree(floor, PS2_LEGACY_RAM_SIZE); }

void GuestHeap::cutFreeFrom(uint32_t floor) { cutFree(floor, UINT32_MAX); }

void GuestHeap::cutFree(uint32_t floor, uint32_t end)
{
    for (auto it = m_free.lower_bound(floor); it != m_free.end() && it->first < end;)
        it = eraseFreeBlock(it);
    auto it = m_free.lower_bound(floor);
    if (it != m_free.begin())
    {
        --it;
        if (it->first < end && it->first + it->second > floor)
            resizeFreeBlock(it, floor - it->first);
    }
}

bool GuestHeap::usedAbove(uint32_t addr) const
{
    for (const auto &[start, size] : m_used)
    {
        if (static_cast<uint64_t>(start) + size > addr)
            return true;
    }
    return false;
}

uint32_t GuestHeap::extendedInUse() const { return m_usage.extendedInUse; }

void GuestHeap::resetPeaks()
{
    m_usage.mainPeak = m_usage.mainInUse;
    m_usage.extendedPeak = m_usage.extendedInUse;
}

void GuestHeap::taken(uint32_t addr, uint32_t bytes)
{
    if (addr >= PS2_LEGACY_RAM_SIZE)
    {
        m_usage.extendedInUse += bytes;
        m_usage.extendedPeak = std::max(m_usage.extendedPeak, m_usage.extendedInUse);
    }
    else
    {
        m_usage.mainInUse += bytes;
        m_usage.mainPeak = std::max(m_usage.mainPeak, m_usage.mainInUse);
    }
}

void GuestHeap::givenBack(uint32_t addr, uint32_t bytes)
{
    (addr >= PS2_LEGACY_RAM_SIZE ? m_usage.extendedInUse : m_usage.mainInUse) -= bytes;
}

void GuestHeap::addFreeBlock(uint32_t addr, uint32_t size)
{
    m_free.emplace(addr, size);
    bucketGrew(bucketOf(addr), size);
}

GuestHeap::FreeMap::iterator GuestHeap::eraseFreeBlock(FreeMap::iterator it)
{
    const uint32_t addr = it->first;
    const uint32_t size = it->second;
    const auto next = m_free.erase(it);
    bucketLost(bucketOf(addr), size);
    return next;
}

void GuestHeap::resizeFreeBlock(FreeMap::iterator it, uint32_t size)
{
    const uint32_t old = it->second;
    it->second = size;
    if (size > old)
        bucketGrew(bucketOf(it->first), size);
    else
        bucketLost(bucketOf(it->first), old);
}

void GuestHeap::moveFreeBlock(FreeMap::iterator it, uint32_t addr, uint32_t size)
{
    eraseFreeBlock(it);
    addFreeBlock(addr, size);
}

void GuestHeap::bucketGrew(uint32_t bucket, uint32_t size)
{
    if (size > m_maxTree[m_buckets + bucket])
        setBucketMax(bucket, size);
}

void GuestHeap::bucketLost(uint32_t bucket, uint32_t size)
{
    // Only the largest block's going changes the bucket's largest.
    if (size < m_maxTree[m_buckets + bucket])
        return;
    uint32_t largest = 0;
    for (auto it = m_free.lower_bound(bucket << kBucketShift); it != m_free.end() && bucketOf(it->first) == bucket; ++it)
        largest = std::max(largest, it->second);
    setBucketMax(bucket, largest);
}

void GuestHeap::setBucketMax(uint32_t bucket, uint32_t value)
{
    uint32_t i = m_buckets + bucket;
    m_maxTree[i] = value;
    for (i >>= 1; i >= 1; i >>= 1)
        m_maxTree[i] = std::max(m_maxTree[2u * i], m_maxTree[2u * i + 1u]);
}

uint32_t GuestHeap::nextBucketWith(uint32_t from, uint32_t end, uint32_t size) const
{
    if (from >= end)
        return end;
    uint32_t i = m_buckets + from;
    if (m_maxTree[i] < size)
    {
        // Up to the first subtree to the right that has one, then down to its leftmost such bucket.
        for (;;)
        {
            while (i & 1u)
                i >>= 1;
            if (i == 0)
                return end;
            ++i;
            if (m_maxTree[i] >= size)
                break;
        }
        while (i < m_buckets)
        {
            i <<= 1;
            if (m_maxTree[i] < size)
                ++i;
        }
    }
    return std::min(i - m_buckets, end);
}

GuestHeap::FreeMap::iterator GuestHeap::mergeAround(FreeMap::iterator it)
{
    if (it != m_free.begin())
    {
        const auto prev = std::prev(it);
        if (endOf(prev->first, prev->second) == it->first)
        {
            const uint32_t size = prev->second + it->second;
            eraseFreeBlock(it);
            resizeFreeBlock(prev, size);
            it = prev;
        }
    }
    const auto next = std::next(it);
    if (next != m_free.end() && endOf(it->first, it->second) == next->first)
    {
        const uint32_t size = it->second + next->second;
        eraseFreeBlock(next);
        resizeFreeBlock(it, size);
    }
    return it;
}

void GuestHeap::mergeAfterChange(FreeMap::iterator it)
{
    if (m_mayTouch)
    {
        mergeAll();
        m_mayTouch = false;
        return;
    }
    mergeAround(it);
}

void GuestHeap::mergeAll()
{
    for (auto it = m_free.begin(); it != m_free.end();)
    {
        const auto next = std::next(it);
        if (next != m_free.end() && endOf(it->first, it->second) == next->first)
        {
            const uint32_t size = it->second + next->second;
            eraseFreeBlock(next);
            resizeFreeBlock(it, size);
            continue;
        }
        it = next;
    }
}
