#pragma once

// The engine's growable array of pointers (words), with a cursor for a walk
// that removes items as it goes (block 166, endScript-drawProfilePeak; BMX XXX
// has it word for word, ZGE2's record differs: below). The moved-node list of
// the scene tree (zge/scene/transform.cpp), the collision world's lists and a
// sector's nodes (zge/scene/query.h NodeList) are such arrays.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
#if PS2X_GAME_DM2
    // ZGE2's array record (0x14 bytes; read in DM2's newCursorArray and every array function of
    // array.cpp at its DM2 address): whole words, the references a word of their own, and no sharing word: the
    // record and its items both come from the heap, and ZGE2 has one constructor (newCursorArray).
    struct PointerArray
    {
        ee::Ptr<uint32_t> items; // DM2 +0x00 capacity words, the first count in use
        int32_t count;           // DM2 +0x04
        int32_t capacity;        // DM2 +0x08 doubles when full
        int32_t cursor;          // DM2 +0x0C the walk's item, -1 when no walk is under way
        int32_t references;      // DM2 +0x10 holders (1 when made; addArrayReference adds one, releaseArrayReference takes one)
    };
    static_assert(sizeof(PointerArray) == 0x14);
    static_assert(offsetof(PointerArray, count) == 0x04 && offsetof(PointerArray, capacity) == 0x08 &&
                  offsetof(PointerArray, cursor) == 0x0C && offsetof(PointerArray, references) == 0x10);
#else
    struct PointerArray
    {
        ee::Ptr<uint32_t> items; // +0x00 capacity words, the first count in use
        int32_t cursor;          // +0x04 the walk's item, -1 when no walk is under way
        uint16_t count;          // +0x08
        uint16_t capacity;       // +0x0A doubles when full
        // +0x0C bits 0-15 the items' bytes, 16-28 references (addArrayReference
        // adds one, releaseArrayReference takes one), 29 the items from the sized
        // heap (allocateSized), 30 the array from the arrays' pool (else the
        // heap), 31 made by newPointerArray (nothing reads it); freePointerArray
        // frees both by these bits.
        uint32_t sharing;
    };
    static_assert(sizeof(PointerArray) == 0x10);
    static_assert(offsetof(PointerArray, cursor) == 0x04 && offsetof(PointerArray, count) == 0x08 &&
                  offsetof(PointerArray, capacity) == 0x0A);
#endif
    // The count and capacity as the record stores them (ZGE2: whole words).
    using ArrayCount = decltype(PointerArray::count);
    // The sharing word's fields (ZGE3 and ZGE4; ZGE2 keeps the references alone).
    constexpr uint32_t kArrayItemBytes = 0x0000FFFFu;
    constexpr uint32_t kArrayReferences = 0x1FFF0000u;
    constexpr int32_t kArrayReferenceShift = 16;
    constexpr uint32_t kArrayItemsSized = 0x20000000u;
    constexpr uint32_t kArrayPooled = 0x40000000u;
    constexpr uint32_t kArrayFromNewPointerArray = 0x80000000u;

    // The walk the engine's callers write inline (the macro behind every
    // "for each item" over such an array): first puts the cursor on item 0
    // (-1 and no item when empty), next moves it on (-1 past the end).
    enum class WalkStep
    {
        Item,     // the cursor moved to an item
        PastEnd,  // the cursor moved past the last item (set to -1)
        NoWalk,   // the cursor was already -1
    };
    inline uint32_t walkFirst(PointerArray &array)
    {
        if (array.count)
        {
            array.cursor = 0;
            return array.items[0];
        }
        array.cursor = -1;
        return 0;
    }
    inline WalkStep walkNext(PointerArray &array, uint32_t &item)
    {
        const int32_t at = array.cursor;
        item = 0;
        if (at < 0)
        {
            array.cursor = -1;
            return WalkStep::NoWalk;
        }
        array.cursor = at + 1;
        if (at + 1 < static_cast<int32_t>(array.count))
        {
            item = array.items[at + 1];
            return WalkStep::Item;
        }
        array.cursor = -1;
        return WalkStep::PastEnd;
    }

    // arrayFirst, arrayNext: the walk above as functions (ZGE3 keeps them out of line in another module; ZGE2's
    // callers call them where ZGE3's inline the walk: zge/scene/query.cpp forEachSectorNode).
    uint32_t arrayFirst(ee::Ptr<PointerArray> array);
    uint32_t arrayNext(ee::Ptr<PointerArray> array);

    // pointerArrayFind: the index of item, or -1.
    int32_t pointerArrayFind(ee::Ptr<PointerArray> array, ee::Address item);

    // pointerArrayAdd: item added (the array doubled first when full). During
    // a walk it goes at the cursor and the item there moves to the end, so
    // the walk still meets it.
    void pointerArrayAdd(ee::Ptr<PointerArray> array, ee::Address item);

    // pointerArrayPushBack: item added at the end (the array doubled first when
    // full), whatever the cursor.
    void pointerArrayPushBack(ee::Ptr<PointerArray> array, ee::Address item);

    // pointerArrayRemoveCurrent: the item at the cursor taken out (the last item when no
    // walk is under way), the last item moved into its place; the cursor
    // kept inside the array. Returns the item.
    uint32_t pointerArrayRemoveCurrent(ee::Ptr<PointerArray> array);

    // pointerArrayRemove: the first item equal to item taken out, the last
    // item moved into its place. Returns the item.
    uint32_t pointerArrayRemove(ee::Ptr<PointerArray> array, ee::Address item);
    // releaseArrayReference: one holder fewer; the holders left (0: the array is free to go).
    int32_t releaseArrayReference(ee::Ptr<PointerArray> array);
}
