// The box sweep's tables (block collisionPairHash; zge/collision/sweep.h, the end
// points' moves are in zge/utilities.cpp). BMX XXX (PS2) has these word for
// word; DM2 has them nearly so.

#include "sweep.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/heaps.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "zge/assets/game_address.h"
#include "zge/collision/sweep.h"
#include "sym/zge/collision/sweep.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kFindCollisionPair = sym::findCollisionPair;
        constexpr uint32_t kPairOverlapsAllAxes = sym::pairOverlapsAllAxes;
        constexpr uint32_t kPoolFree = sym::poolFree;
        constexpr uint32_t kPairBytes = sizeof(SweepPair);
        constexpr float kPastEverything = 1e+04f;

        // What sweepPassLow is told: per axis a pair began or stopped
        // overlapping on it, or a contact began or ended.
        enum SweepEvent : uint32_t
        {
            kEventOverlap = 0,    // + 2 * axis
            kEventSeparate = 1,   // + 2 * axis
            kEventContact = 6,
            kEventContactEnd = 7,
            kEventCount = 8,
        };

        // A table of buckets: its first bucket.
        using BucketTable = ee::Ptr<ee::Ptr<SweepPair>>;
        BucketTable overlapTable(ee::Ptr<Sweep> boxSweep, uint32_t axis) { return boxSweep->overlaps[axis]; }
        BucketTable contactTable(ee::Ptr<Sweep> boxSweep) { return boxSweep->contactBuckets; }
    }

    // collisionPairHash: a pair's bucket. (The sweep goes unused.)
    int32_t collisionPairHash(ee::Ptr<Sweep>, uint32_t boxA, uint32_t boxB)
    {
        const int32_t bucket = static_cast<int32_t>((boxA + boxB) % kSweepBuckets);
        return bucket;
    }
    READABLE_FUNCTION(collisionPairHash, Verified);

    namespace
    {
        void callContactHandler(uint32_t handler, uint32_t boxA, uint32_t boxB) { ee::game(handler, boxA, boxB); }
        // The head of a bucket of a pairs table.
        ee::Ptr<SweepPair> &bucketHead(BucketTable table, int32_t bucket) { return table[static_cast<uint32_t>(bucket)]; }
    }

    // runContactHandlers: each contact's handler (if its pair of kinds has one)
    // called with the two boxes.
    void runContactHandlers(ee::Ptr<Sweep> boxSweep)
    {
        for (ee::Ptr<SweepPair> contact = boxSweep->contacts; contact; contact = contact->nextContact)
        {
            const uint32_t handler = boxSweep->handlers[contact->handler];
            if (handler != 0)
                callContactHandler(handler, contact->boxA, contact->boxB);
        }
    }
    READABLE_FUNCTION(runContactHandlers, Verified);

    // setCollisionPairHandler: the handler of a pair of kinds (the triangular table's
    // slot of the two). Returns the slot's address less the table's offset.
    void setCollisionPairHandler(ee::Ptr<Sweep> boxSweep, uint32_t kindA, uint32_t kindB, uint32_t handler)
    {
        const auto slot = [](uint32_t larger, uint32_t smaller) { return ((larger * larger + larger) >> 1) + smaller; };
        uint32_t index;
        if (kindB < kindA)
            index = slot(kindA, kindB);
        else
            index = slot(kindB, kindA);
        boxSweep->handlers[index] = handler;
    }
    READABLE_FUNCTION(setCollisionPairHandler, Verified);

    // findCollisionPair: the table's pair of two boxes (either way round), or 0.
    ee::Ptr<SweepPair> findCollisionPair(ee::Ptr<Sweep> boxSweep, BucketTable table, uint32_t boxA, uint32_t boxB)
    {
        const uint32_t lower = boxB < boxA ? boxB : boxA, upper = boxB < boxA ? boxA : boxB;
        const int32_t bucket = collisionPairHash(boxSweep, lower, upper);
        ee::Ptr<SweepPair> pair = bucketHead(table, bucket);
        for (;; pair = pair->nextInBucket)
        {
            if (!pair)
                break;
            if (pair->boxA != lower)
            {
                if (pair->boxB == lower && pair->boxA == upper)
                    break;
                continue;
            }
            if (pair->boxB == upper)
                break;
        }
        return pair;
    }
    READABLE_FUNCTION(findCollisionPair, Verified);

    // pairOverlapsAllAxes: 1 if the two boxes overlap on all three axes.
    int32_t pairOverlapsAllAxes(ee::Ptr<Sweep> boxSweep, uint32_t boxA, uint32_t boxB)
    {
        bool all = findCollisionPair(boxSweep, overlapTable(boxSweep, 0), boxA, boxB) != 0;
        if (all)
            all = findCollisionPair(boxSweep, overlapTable(boxSweep, 1), boxA, boxB) != 0;
        if (!all)
            return 0;
        const int32_t overlaps = findCollisionPair(boxSweep, overlapTable(boxSweep, 2), boxA, boxB) != 0 ? 1 : 0;
        return overlaps;
    }
    READABLE_FUNCTION(pairOverlapsAllAxes, Verified);

    // sweepIgnoreBody (was listAdd; updateTrafficCar: a car and the one its
    // level names to leave alone): `other` added to the boxes this one never
    // makes a contact with. Kept bug: nothing checks the list's room (ten).
    void sweepIgnoreBody(ee::Ptr<SweepBox> box, ee::Address other)
    {
        box->ignored[box->ignoredCount] = other;
        ++box->ignoredCount;
    }
    READABLE_FUNCTION(sweepIgnoreBody, Verified);

    // walkSweepAxisEnds (walkSweepAxes'; nothing calls them in AI): each end
    // point of an axis's list walked to its box's other end (a low end
    // forward, a high end back). Whatever it checked on the way is compiled
    // out: it reads and changes nothing. `overlaps` is not read.
    void walkSweepAxisEnds(ee::Ptr<Sweep>, ee::Ptr<SweepEnd> first, ee::Ptr<ee::Ptr<SweepPair>> /*overlaps*/)
    {
        for (ee::Ptr<SweepEnd> end = first; end; end = end->next)
        {
            if (end->isHigh == 0)
            {
                ee::Ptr<SweepEnd> at = end->next;
                while (at && !(at->isHigh == 1 && at == end->other))
                    at = at->next;
            }
            if (end->isHigh == 1)
            {
                ee::Ptr<SweepEnd> at = end->prev;
                while (at && !(at->isHigh == 0 && at == end->other))
                    at = at->prev;
            }
        }
    }
    READABLE_FUNCTION(walkSweepAxisEnds, Draft);

    // walkSweepAxes: walkSweepAxisEnds over the three axes (the last a tail
    // call).
    void walkSweepAxes(ee::Ptr<Sweep> sweep)
    {
        walkSweepAxisEnds(sweep, sweep->axes[0], sweep->overlaps[0]);
        walkSweepAxisEnds(sweep, sweep->axes[1], sweep->overlaps[1]);
        game::tail(walkSweepAxisEnds, sweep, sweep->axes[2], sweep->overlaps[2]);
    }
    READABLE_FUNCTION(walkSweepAxes, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void poolFree(uint32_t pool, ee::Ptr<SweepPair> record) { ee::tailCall(kPoolFree, pool, record); }
        }
    }

    namespace
    {
        // What sweepPassLow's caller, one of the four moves of an end point
        // (sweepMoveUp, sweepHighDown, sweepLowUp and sweepLowDown, in all
        // three games), keeps across the call: the moving end, and a high
        // end's moves their pass, a low end's the sweep. The original's
        // sweepPassLow keeps a copy of both at the foot of its frame, where
        // nothing in the engine reads them; but BMX XXX's own code later
        // copies an angle record of its own, whose bytes past the angle it
        // never writes, into an object, so bytes of the kept pass reach the
        // game's memory from the dead stack. So they are written here as the
        // original leaves them.
        struct MoverKeeps
        {
            uint64_t movingEnd;   // +0x00
            uint64_t notWritten;  // +0x08 (the original leaves it as it was)
            uint64_t passOrSweep; // +0x10
        };
        static_assert(sizeof(MoverKeeps) == 0x18 && offsetof(MoverKeeps, passOrSweep) == 0x10);
    }

    // sweepPassLow (an end point passing a box's low end, sweepMoveUp and the
    // others): first and second are ends of the two boxes. Starting a pair's
    // overlap on an axis puts it in that axis's table and, overlapping on all
    // three, makes it a contact, unless either box ignores the other; ending
    // one takes it out and ends a contact it was. The result is
    // kept: on a contact made the list's old head, an ignored box's id, or
    // poolFree's.
    void sweepPassLow(ee::Ptr<Sweep> boxSweep, ee::Ptr<SweepEnd> first, ee::Ptr<SweepEnd> second, uint32_t event)
    {
        ee::Local<MoverKeeps> moverKeeps;
        moverKeeps->movingEnd = ee::Address(first);
        const uint32_t sweepKept = ee::Address(boxSweep);
        moverKeeps->passOrSweep = first->isHigh ? event : sweepKept;
        uint32_t lower = 0;
        uint32_t upper = 0;
        const auto hash = [&]()
        {
            return collisionPairHash(boxSweep, lower, upper);
        };
        // A pair at the head of its bucket.
        const auto addToBucket = [](ee::Ptr<SweepPair> pair, ee::Ptr<SweepPair> &bucket)
        {
            const ee::Ptr<SweepPair> head = bucket;
            pair->nextInBucket = head;
            bucket = pair;
            if (head)
                head->prevInBucket = pair;
        };
        uint32_t firstBox = first->box, secondBox = 0;
        for (;;)
        {
            secondBox = second->box;
            if (firstBox < secondBox)
            {
                lower = firstBox;
                upper = secondBox;
            }
            else
            {
                lower = secondBox;
                upper = firstBox;
            }
            if (event >= kEventCount)
                return;

            if (event < kEventContact && (event & 1) == kEventOverlap)
            {
                // The pair overlaps on this axis now.
                const uint32_t axis = event >> 1;
                const ee::Ptr<SweepPair> pair(poolAlloc(ee::Ptr<Pool>(boxSweep->pairPool)));
                memoryFill(pair, 0, kPairBytes);
                pair->boxA = lower;
                pair->boxB = upper;
                const int32_t bucket = hash();
                addToBucket(pair, bucketHead(overlapTable(boxSweep, axis), bucket));
                if (pairOverlapsAllAxes(boxSweep, lower, upper) == 0)
                    return;
                event = kEventContact;
                firstBox = first->box;
                continue;
            }

            if (event < kEventContact)
            {
                // The pair no longer overlaps on this axis.
                const uint32_t axis = event >> 1;
                const BucketTable table = overlapTable(boxSweep, axis);
                const int32_t bucket = hash();
                const ee::Ptr<SweepPair> pair(findCollisionPair(boxSweep, table, lower, upper));
                const ee::Ptr<SweepPair> before = pair->prevInBucket;
                if (before)
                    before->nextInBucket = pair->nextInBucket;
                else
                    bucketHead(table, bucket) = pair->nextInBucket;
                if (const ee::Ptr<SweepPair> after = pair->nextInBucket)
                    after->prevInBucket = before;
                poolFree(ee::Ptr<Pool>(boxSweep->pairPool), pair);
                if (!findCollisionPair(boxSweep, contactTable(boxSweep), lower, upper))
                    return;
                event = kEventContactEnd;
                firstBox = first->box;
                continue;
            }

            if (event == kEventContact)
                break;

            // The contact ends.
            const BucketTable table = contactTable(boxSweep);
            const int32_t bucket = hash();
            const ee::Ptr<SweepPair> contact(findCollisionPair(boxSweep, table, lower, upper));
            const ee::Ptr<SweepPair> before = contact->prevInBucket;
            if (before)
                before->nextInBucket = contact->nextInBucket;
            else
                bucketHead(table, bucket) = contact->nextInBucket;
            if (const ee::Ptr<SweepPair> after = contact->nextInBucket)
                after->prevInBucket = contact->prevInBucket;
            const ee::Ptr<SweepPair> previous = contact->prevContact, following = contact->nextContact;
            if (previous)
                previous->nextContact = following;
            else
                boxSweep->contacts = following;
            if (following)
                following->prevContact = previous;
            tail::poolFree(boxSweep->pairPool, contact); // the original's tail call
            return;
        }

        // A contact, unless either box ignores the other (DM2's sweep has no
        // ignore lists).
        if (!game::readable::kIsDm2 && (first->ignores(secondBox) || second->ignores(firstBox)))
            return;

        const ee::Ptr<SweepPair> contact(poolAlloc(ee::Ptr<Pool>(boxSweep->pairPool)));
        memoryFill(contact, 0, kPairBytes);
        // Its boxes in the order of the two ends, the one ordered lower first
        // (or the lower box first for two ordered alike), and the handler's
        // slot from their kinds. ZGE3 and ZGE4 order the ends by kind and
        // square the larger: setCollisionPairHandler's slot. ZGE2 (DM2) orders
        // them by their high flags, the passed low end against the mover, and
        // squares the kind of the end it put first: a high end passing a low
        // end of another kind reaches another pair's slot (kept, as DM2 plays
        // it).
        const uint32_t firstKind = first->kind, secondKind = second->kind;
        const uint32_t firstOrder = game::readable::kIsDm2 ? first->isHigh : firstKind;
        const uint32_t secondOrder = game::readable::kIsDm2 ? second->isHigh : secondKind;
        uint32_t squared = firstKind, added = secondKind;
        if (secondOrder < firstOrder)
        {
            contact->boxA = second->box;
            contact->boxB = first->box;
            if (game::readable::kIsDm2)
            {
                squared = secondKind;
                added = firstKind;
            }
        }
        else if (firstOrder < secondOrder)
        {
            contact->boxA = first->box;
            contact->boxB = second->box;
            if (!game::readable::kIsDm2)
            {
                squared = secondKind;
                added = firstKind;
            }
        }
        else
        {
            contact->boxA = lower;
            contact->boxB = upper;
        }
        contact->handler = ((squared * squared + squared) >> 1) + added;
        const int32_t bucket = hash();
        addToBucket(contact, bucketHead(contactTable(boxSweep), bucket));
        const ee::Ptr<SweepPair> head = boxSweep->contacts;
        boxSweep->contacts = contact;
        contact->nextContact = head;
        if (head)
            head->prevContact = contact;
    }
    READABLE_FUNCTION_WAITS(sweepPassLow, Verified);

    // sweepAddBody: a new box in the sweep: its record, and on each axis a low
    // and a high end point, joined to the list's head and moved into place
    // (the moves' events start its overlaps). The box's low and high are
    // vectors. Returns its record.
    ee::Ptr<SweepBox> sweepAddBody(ee::Ptr<Sweep> boxSweep, uint32_t kind, ee::Address box, ee::Ptr<const ee::Float> low, ee::Ptr<const ee::Float> high)
    {
        uint64_t record = 0;
        const auto newEnd = [&](uint32_t isHigh, ee::Float value)
        {
            const ee::Ptr<SweepEnd> end(poolAlloc(ee::Ptr<Pool>(boxSweep->endPool)));
            end->kind = kind;
            end->isHigh = isHigh;
            end->box = box;
            end->value = value;
            end->next = ee::Ptr<SweepEnd>();
            end->prev = ee::Ptr<SweepEnd>();
            return end;
        };
        const auto joinEnds = [](ee::Ptr<SweepEnd> lowEnd, ee::Ptr<SweepEnd> highEnd)
        {
            lowEnd->other = highEnd;
            highEnd->other = lowEnd;
            lowEnd->next = highEnd;
            highEnd->prev = lowEnd;
        };
        struct AxisBlocks
        {
            uint32_t notFirst, movedUp, joined;
        };
        // On the axis's list: at its head, then each end moved up to its place.
        const auto addToAxis = [&](uint32_t axis, ee::Ptr<SweepEnd> lowEnd, ee::Ptr<SweepEnd> highEnd)
        {
            const ee::Ptr<SweepEnd> head = boxSweep->axes[axis];
            if (!head)
                boxSweep->axes[axis] = lowEnd;
            else
            {
                highEnd->next = head;
                head->prev = highEnd;
                boxSweep->axes[axis] = lowEnd;
                sweepMoveUp(boxSweep, lowEnd, highEnd, 2 * axis);
                sweepLowUp(boxSweep, boxSweep->axes[axis], lowEnd, 2 * axis + 1);
            }
        };

        // The box's record: ZGE3 makes it first and tells each end of it; ZGE2 makes it last, its low ends alone.
        const auto newRecord = [&]
        {
            const uint32_t recordMemory = heapAllocate(kSweepBoxBytes);
            record = memoryFill(recordMemory, 0, kSweepBoxBytes);
        };
        if constexpr (!game::readable::kIsDm2)
            newRecord();
        const ee::Ptr<SweepEnd> lowX = newEnd(0, low[0]);
        const ee::Ptr<SweepEnd> highX = newEnd(1, high[0]);
        joinEnds(lowX, highX);
        addToAxis(0, lowX, highX);

        const ee::Ptr<SweepEnd> lowY = newEnd(0, low[1]);
        const ee::Ptr<SweepEnd> highY = newEnd(1, high[1]);
        joinEnds(lowY, highY);
        addToAxis(1, lowY, highY);

        const ee::Ptr<SweepEnd> lowZ = newEnd(0, low[2]);
        const ee::Ptr<SweepEnd> highZ = newEnd(1, high[2]);
        joinEnds(lowZ, highZ);

        const auto storeLowEnds = [&](ee::Ptr<SweepBox> added)
        {
            added->lowEnds[0] = lowX;
            added->lowEnds[1] = lowY;
            added->lowEnds[2] = lowZ;
        };
        if constexpr (game::readable::kIsDm2)
        {
            addToAxis(2, lowZ, highZ);
            newRecord();
            const ee::Ptr<SweepBox> added(static_cast<uint32_t>(record));
            storeLowEnds(added);
            return added;
        }
        else
        {
            const ee::Ptr<SweepBox> added(static_cast<uint32_t>(record));
            storeLowEnds(added);
            for (const ee::Ptr<SweepEnd> end : {lowX, lowY, lowZ, highX, highY, highZ})
                end->setBoxRecord(added);
            addToAxis(2, lowZ, highZ);
            return added;
        }
    }
    READABLE_FUNCTION_WAITS(sweepAddBody, Verified);

    // newCollisionTable: a new sweep, cleared, with its pools.
    ee::Ptr<Sweep> newCollisionTable()
    {
        constexpr uint32_t kRecordsABlock = game::readable::perGame(500u, 500u, 64u); // (ZGE4's pools grow in smaller blocks)
        const uint32_t memory = heapAllocate(sizeof(Sweep));
        const ee::Ptr<Sweep> boxSweep(memoryFill(memory, 0, sizeof(Sweep)));
        boxSweep->pairPool = createPool(sizeof(SweepPair), kRecordsABlock, 0);
        boxSweep->endPool = createPool(sizeof(SweepEnd), kRecordsABlock, 0);
        boxSweep->smallPool = createPool(0x10, kRecordsABlock, 0);
        return boxSweep;
    }
    READABLE_FUNCTION_WAITS(newCollisionTable, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t heapFree(ee::Ptr<Sweep> block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        int32_t heapFree(ee::Ptr<SweepBox> block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        }
    }

    // freeCollisionTable: a sweep and its pools.
    int32_t freeCollisionTable(ee::Ptr<Sweep> sweep)
    {
        destroyPool(ee::Ptr<Pool>(sweep->smallPool));
        destroyPool(ee::Ptr<Pool>(sweep->endPool));
        destroyPool(ee::Ptr<Pool>(sweep->pairPool));
        return tail::heapFree(sweep);
    }
    READABLE_FUNCTION_WAITS(freeCollisionTable, Verified);

    // removeSweepBody: a box out of the sweep: moved past every other on each
    // axis, which ends its contacts and leaves each axis's two ends last and
    // side by side; those are then unlinked and freed, and the record.
    int32_t removeSweepBody(ee::Ptr<Sweep> sweep, ee::Ptr<SweepBox> box)
    {
        constexpr uint32_t kAxes = 3;
        const ee::Float pastEverything(kPastEverything);
        for (uint32_t axis = 0; axis < kAxes; ++axis)
        {
            const ee::Ptr<SweepEnd> low = box->lowEnds[axis], high = low->other;
            low->value = low->value + pastEverything;
            high->value = high->value + pastEverything;
        }
        sweepUpdateBox(sweep, box);
        for (uint32_t axis = 0; axis < kAxes; ++axis)
        {
            const ee::Ptr<SweepEnd> low = box->lowEnds[axis], high = low->other;
            if (low->prev)
                low->prev->next = high->next;
            else
                sweep->axes[axis] = high->next;
            if (high->next)
                high->next->prev = low->prev;
            poolFree(ee::Ptr<Pool>(sweep->endPool), high);
            poolFree(ee::Ptr<Pool>(sweep->endPool), low);
        }
        return tail::heapFree(box);
    }
    READABLE_FUNCTION_WAITS(removeSweepBody, Verified);
}
