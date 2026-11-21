// The engine's priority queue (block 168, zprofile; DM2 and BMX XXX have it
// word for word): a binary heap of {key, value} pairs in an array from
// index 1, smallest key on top (or largest, by a flag). The collision
// queries keep their hits on one, nearest first (zge/collision/query.cpp,
// zge/scene/query.h: kInsertSorted, kSortedListPop).
//
// Index 0 is a sentinel the queue's owner sets (a key no other key sorts
// before), so the climb up the heap stops at the top without a test.

#include "heap.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/container/heap.cpp.h"

namespace zge
{
    // The entry the last pop took off (priorityQueuePop hands out its address).
    inline ee::Global<HeapEntry, sym::g_poppedEntry> g_poppedEntry;

    // priorityQueueEmpty: whether the queue holds nothing.
    bool priorityQueueEmpty(ee::Ptr<PriorityQueue> queue)
    {
        return queue->count == 0;
    }
    READABLE_FUNCTION(priorityQueueEmpty, Verified);

    // priorityQueueFull: whether the queue is full.
    bool priorityQueueFull(ee::Ptr<PriorityQueue> queue)
    {
        return queue->count == queue->capacity;
    }
    READABLE_FUNCTION(priorityQueueFull, Verified);

    namespace
    {
        // Whether key a goes above key b in this queue.
        bool above(uint32_t a, uint32_t b, bool largestFirst)
        {
            return largestFirst ? b < a : a < b;
        }

        // `moving` put in the heap at `hole`, sunk first past every child
        // that goes above it (of two children, the one that goes above its
        // sibling; the left one when there is no right one).
        void sink(ee::Ptr<PriorityQueue> queue, int32_t hole, const HeapEntry moving, bool largestFirst)
        {
            ee::Ptr<HeapEntry> entries = queue->entries;
            int32_t count = queue->count;
            for (int32_t child = hole * 2; child <= count; child = hole * 2)
            {
                int32_t pick = child;
                if (child != count)
                    pick = child | (above(entries[child + 1].key, entries[child].key, largestFirst) ? 1 : 0);
                if (!above(entries[pick].key, moving.key, largestFirst))
                    break;
                entries[hole] = entries[pick];
                hole = pick;
                count = queue->count;
                entries = queue->entries;
            }
            entries[hole] = moving;
        }
    }

    // initPriorityQueue: an empty queue with room for `capacity` entries (at
    // least 10) from the heap, its sentinel a key nothing goes above (0
    // smallest first, 0xFFFFFFFF largest first).
    void initPriorityQueue(ee::Ptr<PriorityQueue> queue, uint32_t largestFirst, int32_t capacity)
    {
        const int32_t room = capacity > 9 ? capacity : 10;
        const ee::Ptr<HeapEntry> entries(heapAllocate(static_cast<uint32_t>(room + 1) * sizeof(HeapEntry)));
        queue->capacity = room;
        queue->entries = entries;
        queue->largestFirst = static_cast<int32_t>(largestFirst);
        queue->count = 0;
        entries->key = largestFirst != 0 ? 0xFFFFFFFFu : 0u;
        queue->entries->value = 0;
    }
    READABLE_FUNCTION_WAITS(initPriorityQueue, Verified);

    namespace tail
    {
        void heapFree(ee::Ptr<HeapEntry> entries) { ee::tailCall<int32_t>(sym::heapFree, entries); }
    }

    // freePriorityQueue: the queue's entries given back to the heap.
    void freePriorityQueue(ee::Ptr<PriorityQueue> queue)
    {
        tail::heapFree(queue->entries);
    }
    READABLE_FUNCTION_WAITS(freePriorityQueue, Verified);

    // priorityQueueClear: the queue emptied.
    void priorityQueueClear(ee::Ptr<PriorityQueue> queue)
    {
        queue->count = 0;
    }
    READABLE_FUNCTION(priorityQueueClear, Verified);

    // priorityQueueTop: the top entry's address, or 0 if the queue is empty.
    ee::Ptr<HeapEntry> priorityQueueTop(ee::Ptr<PriorityQueue> queue)
    {
        if (priorityQueueEmpty(queue))
            return ee::Ptr<HeapEntry>();
        return queue->entries + 1;
    }
    READABLE_FUNCTION(priorityQueueTop, Verified);

    // priorityQueueInsert: {key, value} put in the queue, climbing from the end
    // past every parent it goes above. 0 if the queue was full, else 1.
    uint32_t priorityQueueInsert(uint32_t key, uint32_t value, ee::Ptr<PriorityQueue> queue)
    {
        if (priorityQueueFull(queue))
            return 0;
        const bool largestFirst = queue->largestFirst != 0;
        // The two orders are two copies of the code (their blocks below).
        int32_t slot = queue->count + 1;
        ee::Ptr<HeapEntry> entries = queue->entries;
        queue->count = slot;
        if (above(key, entries[slot / 2].key, largestFirst))
        {
            for (bool first = true;; first = false)
            {
                const int32_t parent = slot / 2;
                entries[slot] = entries[parent];
                slot = parent;
                entries = queue->entries;
                if (!above(key, entries[slot / 2].key, largestFirst))
                    break;
            }
        }
        entries[slot] = HeapEntry{key, value};
        return 1;
    }
    READABLE_FUNCTION(priorityQueueInsert, Verified);

    // priorityQueuePop: the top entry taken off (copied to g_poppedEntry, whose
    // address it returns; 0 if the queue was empty): the last entry sinks
    // from the top past every child that goes above it.
    uint32_t priorityQueuePop(ee::Ptr<PriorityQueue> queue)
    {
        if (priorityQueueEmpty(queue))
            return 0;
        *g_poppedEntry = queue->entries[1];
        const ee::Ptr<HeapEntry> entries = queue->entries;
        const bool largestFirst = queue->largestFirst != 0;
        const int32_t oldCount = queue->count;
        const HeapEntry last = entries[oldCount];
        queue->count = oldCount - 1;
        sink(queue, 1, last, largestFirst);
        return sym::g_poppedEntry;
    }
    READABLE_FUNCTION(priorityQueuePop, Verified);

    // priorityQueueRemove (no caller in this build): the entry holding
    // `value` taken out: the last entry sinks from its place. 1, or 0 when
    // none was found. The original's search stops short of the last entry,
    // so that one is never found (nor the only one, in a queue of one); kept.
    // The last entry is not climbed either, only sunk.
    uint32_t priorityQueueRemove(uint32_t value, ee::Ptr<PriorityQueue> queue)
    {
        if (priorityQueueEmpty(queue))
            return 0;
        const int32_t count = queue->count;
        int32_t found = -1;
        if (count > 1)
        {
            const ee::Ptr<HeapEntry> entries = queue->entries;
            if (entries[1].value == value)
                found = 1;
            else
                for (int32_t i = 2; i < count && found < 0; ++i)
                    if (entries[i].value == value)
                        found = i;
        }
        if (found < 0)
            return 0;
        ee::Local<HeapEntry> last;
        *last = queue->entries[count];
        queue->count = count - 1;
        sink(queue, found, *last, queue->largestFirst != 0);
        return 1;
    }
    READABLE_FUNCTION(priorityQueueRemove, Draft);
}

// The fields this source uses, at the offsets each game's machine code uses (AI's: its headers').
#include "sym/zge/container/heap.cpp.layout.h"
