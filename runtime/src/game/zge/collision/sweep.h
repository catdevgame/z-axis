#pragma once

// The box sweep (sweep and prune; zge/collision/sweep.cpp, the end points'
// moves are in zge/utilities.cpp): per axis a list of the boxes' end points,
// sorted by value. A box's ends passing another's are told to the sweep,
// which keeps a table per axis of the pairs that overlap on it and a table
// and a list of those that overlap on all three: the contacts, each with
// the handler of its two kinds. The game's level contacts are one of these.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct SweepEnd;

    // A box's record (sweepAddBody): its low ends, and the boxes it never
    // makes a contact with.
    struct SweepBox
    {
        ee::Ptr<SweepEnd> lowEnds[3]; // +0x00
        uint32_t ignoredCount;        // +0x0C
        uint32_t ignored[10];         // +0x10
    };
    static_assert(sizeof(SweepBox) == 0x38);
    // A box's record as the game allocates it: ZGE2's holds the three low ends alone (its sweep keeps no ignore lists;
    // the fields past them are AI's, past its record).
    constexpr uint32_t kSweepBoxBytes = game::readable::perGame(0x38u, 0x0Cu);

    struct SweepEnd
    {
        uint32_t kind;          // +0x00 the box's kind (the handlers are by pairs of kinds)
        uint32_t isHigh;        // +0x04 0: a box's low end
        uint32_t box;           // +0x08 the box it is an end of (its ends share it): what the handlers are given
#if !ZX_GAME_DM2
        ee::Ptr<SweepBox> ends; // +0x0C its box's record
        // Whether its box never makes a contact with box `other` (its
        // record's ignore list), and its box's record set (sweepAddBody).
        bool ignores(uint32_t other) const
        {
            const ee::Ptr<SweepBox> record = ends;
            for (uint32_t i = 0; i < record->ignoredCount; ++i)
                if (record->ignored[i] == other)
                    return true;
            return false;
        }
        void setBoxRecord(ee::Ptr<SweepBox> record) { ends = record; }
#else
        // DM2's sweep keeps no box records: no ignore lists.
        bool ignores(uint32_t) const { return false; }
        void setBoxRecord(ee::Ptr<SweepBox>) {}
#endif
        ee::Float value;        // +0x10 (DM2 +0x0C)
        ee::Ptr<SweepEnd> other; // +0x14 (DM2 +0x10) the box's other end on the axis
        ee::Ptr<SweepEnd> next; // +0x18 (DM2 +0x14) the next larger end point on the axis
        ee::Ptr<SweepEnd> prev; // +0x1C (DM2 +0x18)
    };
    static_assert(sizeof(SweepEnd) == game::readable::perGame(0x20u, 0x1Cu) && offsetof(SweepEnd, value) == game::readable::perGame(0x10u, 0x0Cu) &&
                  offsetof(SweepEnd, next) == game::readable::perGame(0x18u, 0x14u));

    // Two boxes overlapping on an axis, or a contact. In its table's bucket
    // by collisionPairHash; a contact also in the contact list.
    struct SweepPair
    {
        uint32_t handler;                // +0x00 a contact: its handler's slot (sweepHandlerSlot)
        uint32_t boxA;                   // +0x04 the lower box
        uint32_t boxB;                   // +0x08
        ee::Ptr<SweepPair> nextContact;  // +0x0C
        ee::Ptr<SweepPair> prevContact;  // +0x10
        ee::Ptr<SweepPair> nextInBucket; // +0x14
        ee::Ptr<SweepPair> prevInBucket; // +0x18
    };
    static_assert(sizeof(SweepPair) == 0x1C);

    constexpr uint32_t kSweepBuckets = 999;
    constexpr uint32_t kSweepHandlerSlots = game::readable::perGame(210u, 3u); // 20 kinds' pairs (ZGE2: 2 kinds', 3 slots)

    using SweepBuckets = ee::Ptr<SweepPair>[kSweepBuckets];

    struct Sweep
    {
        ee::Ptr<SweepEnd> axes[3];            // +0x0000 each axis's first end point
        SweepBuckets overlaps[3];             // +0x000C the pairs overlapping on each axis
        SweepBuckets contactBuckets;          // +0x2EE0
        uint8_t _3e7c[0x0C];
        ee::Ptr<SweepPair> contacts;          // +0x3E88
        uint32_t pairPool;                    // +0x3E8C SweepPair
        uint32_t endPool;                     // +0x3E90 SweepEnd
        uint32_t smallPool;                   // +0x3E94 0x10 bytes each: made and destroyed with the sweep, nothing takes a record
        uint32_t handlers[kSweepHandlerSlots]; // +0x3E98 (box, box), by sweepHandlerSlot
    };
    static_assert(sizeof(Sweep) == game::readable::perGame(0x41E0u, 0x3EA4u) && offsetof(Sweep, overlaps) == 0x0C && offsetof(Sweep, contactBuckets) == 0x2EE0);
    static_assert(offsetof(Sweep, contacts) == 0x3E88 && offsetof(Sweep, handlers) == 0x3E98);

    // A sweep and its pools freed.
    int32_t freeCollisionTable(ee::Ptr<Sweep> sweep);
}

namespace zge
{
    // (sweep.cpp)
    void runContactHandlers(ee::Ptr<Sweep> boxSweep);
    void setCollisionPairHandler(ee::Ptr<Sweep> boxSweep, uint32_t kindA, uint32_t kindB, uint32_t handler);
    void sweepPassLow(ee::Ptr<Sweep> boxSweep, ee::Ptr<SweepEnd> first, ee::Ptr<SweepEnd> second, uint32_t event);
    ee::Ptr<SweepBox> sweepAddBody(ee::Ptr<Sweep> boxSweep, uint32_t kind, ee::Address box, ee::Ptr<const ee::Float> low, ee::Ptr<const ee::Float> high);
    ee::Ptr<Sweep> newCollisionTable();
    int32_t removeSweepBody(ee::Ptr<Sweep> sweep, ee::Ptr<SweepBox> box);
    void sweepIgnoreBody(ee::Ptr<SweepBox> box, ee::Address other);
}

namespace zge
{
    // (zge/utilities.cpp) An end point moved along its axis (the axis's first
    // end point, the end point, the pass told to the sweep), and a box's six
    // end points brought up to date.
    void sweepMoveUp(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t axis);
    void sweepLowUp(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass);
    void sweepLowDown(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass);
    void sweepHighDown(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass);
    void sweepUpdateBox(ee::Ptr<Sweep> sweep, ee::Ptr<SweepBox> box);
}
