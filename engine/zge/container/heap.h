#pragma once

// The engine's priority queue (container/heap.cpp): a binary heap of {key,
// value} pairs in an array from index 1.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct HeapEntry
    {
        uint32_t key;   // +0x00 compared unsigned
        uint32_t value; // +0x04
    };
    static_assert(sizeof(HeapEntry) == 8);

    struct PriorityQueue
    {
        int32_t capacity;           // +0x00
        int32_t count;              // +0x04
        int32_t largestFirst;       // +0x08 0: the smallest key on top
        ee::Ptr<HeapEntry> entries; // +0x0C [0] the sentinel, [1 .. count] the heap
    };
    static_assert(offsetof(PriorityQueue, largestFirst) == 0x08 && offsetof(PriorityQueue, entries) == 0x0C);

    // initPriorityQueue: an empty queue with room for `capacity` entries (at least 10) from the heap, its sentinel a
    // key nothing goes above (0 smallest first, 0xFFFFFFFF largest first).
    void initPriorityQueue(ee::Ptr<PriorityQueue> queue, uint32_t largestFirst, int32_t capacity);
    // freePriorityQueue: the queue's entries given back to the heap.
    void freePriorityQueue(ee::Ptr<PriorityQueue> queue);
    // priorityQueueInsert: {key, value} put in the queue. 0 if the queue was full, else 1.
    uint32_t priorityQueueInsert(uint32_t key, uint32_t value, ee::Ptr<PriorityQueue> queue);
    // priorityQueueTop: the entry on top (still in the queue), or none when the queue is empty.
    ee::Ptr<HeapEntry> priorityQueueTop(ee::Ptr<PriorityQueue> queue);
    // priorityQueuePop: the entry on top taken off and copied to the one place every pop shares (g_poppedEntry): its
    // address, or 0 when the queue is empty.
    uint32_t priorityQueuePop(ee::Ptr<PriorityQueue> queue);

    // The typed view of a queue's entries, for the code that keeps its own
    // {key, value} pairs in one (a record's address as the value):
    // priorityQueuePop itself, its entry as the caller's Entry (the same 8
    // bytes; 0 stays 0). A queue as its holder keeps it (a word, an ee::Ptr)
    // is an ee::Address.
    template <typename Entry>
    ee::Ptr<Entry> priorityQueuePopAs(ee::Address queue)
    {
        static_assert(sizeof(Entry) == sizeof(HeapEntry), "a queue entry is a key and a value");
        return ee::Ptr<Entry>(priorityQueuePop(ee::Ptr<PriorityQueue>(queue)));
    }
}
