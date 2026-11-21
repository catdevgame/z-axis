// "AreYouCloser": objects that listen for the skater's action button (the
// givers of challenges among them) in a search tree: the nearest in reach
// answers the message, keeping its distance in the query.

#include "aggressive/level/closers.h"
#include "aggressive/skater/skater.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/tree.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "aggressive/zone_memory.h"
#include "aggressive/npc.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/closers.cpp.h"

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kCloserTree = sym::g_closerObjectTree;
        constexpr uint32_t kAreYouCloser = sym::g_textAreYouCloser;      // "AreYouCloser"
        constexpr uint32_t kRecordsPerBlock = 20u;

        // Which listeners an "AreYouCloser" query wants (pickups.cpp reads it):
        // the givers of challenges, the others, or any.
        enum CloserKind : uint32_t
        {
            kCloserGiver = 0,
            kCloserNotGiver = 1,
            kCloserAny = 2,
        };

        // What an "AreYouCloser" message carries: each listener of the wanted
        // kind nearer than `nearest` takes the query's place.
        struct CloserQuery
        {
            zge::Vec4 position;  // +0x00
            uint32_t found;      // +0x10 the listener so far, 0: none
            ee::Float nearest;   // +0x14 the search radius, then the found one's distance
            CloserKind kind;     // +0x18
            uint32_t _1c;        // +0x1C cleared with the rest, read by no listener
        };
        static_assert(sizeof(CloserQuery) == 0x20 && offsetof(CloserQuery, found) == 0x10 && offsetof(CloserQuery, kind) == 0x18);

        // The listeners, sorted by object (lessWord compares the first words of
        // two values).
        constexpr ee::Ptr<zge::SearchTree> kListeners(kCloserTree);
        constexpr uint32_t kCompareObjects = sym::lessWord;
        // What a walk of the tree calls for each listener (its CloserRecord), with the walk's context.
        constexpr zge::SearchTreeVisit kFreeListener(sym::freeCloserRecord);
        constexpr zge::SearchTreeVisit kAskListener(sym::askAreYouCloser);

        inline ee::Global<uint32_t, sym::g_closerRecordPool> g_closerRecordPool;
        inline ee::Global<uint32_t, sym::g_areYouCloserHash> g_areYouCloserHash;
    }

    // freeCloserRecord: visiting the tree: a node back to the pool.
    int32_t freeCloserRecord(uint32_t record)
    {
        if (record != 0)
            poolGive(*g_closerRecordPool, record);
        return 1;
    }
    READABLE_FUNCTION(freeCloserRecord, Verified);

    // askAreYouCloser: visiting the tree: the message to the listener, the
    // query as its data, answered before the walk goes on.
    int32_t askAreYouCloser(ee::Ptr<CloserRecord> record, ee::Ptr<CloserQuery> query)
    {
        return zge::sendNewMessage(0, record->object, *g_areYouCloserHash, query, 1);
    }
    READABLE_FUNCTION_WAITS(askAreYouCloser, Verified);

    // initCloserQueries: the listeners' pool and tree, and the message's hash.
    int32_t initCloserQueries()
    {
        *g_closerRecordPool = newPool(sizeof(CloserRecord), kRecordsPerBlock, 0);
        zge::initSearchTree(kListeners, kCompareObjects);
        const int32_t hash = zge::nameHash(ee::Ptr<const char>(kAreYouCloser));
        *g_areYouCloserHash = hash;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCloserQueries, Verified);

    // freeCloserQueries: every listener back to the pool, then the pool freed.
    int32_t freeCloserQueries()
    {
        zge::searchTreeClear(kListeners, kFreeListener, 0);
        return freePool(*g_closerRecordPool);
    }
    READABLE_FUNCTION_WAITS(freeCloserQueries, Verified);

    // registerCloserListener: an object is added to the listeners.
    void registerCloserListener(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<CloserRecord> record(poolTake(*g_closerRecordPool));
        record->object = object;
        zge::searchTreeInsert(kListeners, &record->links);
    }
    READABLE_FUNCTION_WAITS(registerCloserListener, Verified);

    // findCloserListener: the listener of an object.
    ee::Ptr<CloserRecord> findCloserListener(ee::Address object)
    {
        ee::Local<uint32_t> key;
        *key = object;
        return zge::searchTreeEntry<CloserRecord>(zge::searchTreeFind(kListeners, key));
    }
    READABLE_FUNCTION_WAITS(findCloserListener, Verified);

    // findCloserObject: the nearest giver listening in `radius` of a position
    // (0: none) and, when asked, its distance (-1: none). Every listener is
    // asked in turn; one nearer than the query's nearest takes its place.
    // Estate's override hooks it.
    uint32_t findCloserObject(ee::Ptr<zge::Vec4> position, ee::Ptr<ee::Float> distance, ee::Float radius)
    {
        ee::Local<CloserQuery> query;
        zge::memoryFill(query, 0, sizeof(CloserQuery));
        query->position = *position;
        query->nearest = radius;
        query->found = 0;
        query->kind = kCloserGiver;
        zge::searchTreeForEach(kListeners, kAskListener, query);
        if (distance)
            *distance = query->found ? query->nearest : ee::Float(-1.0f);
        return query->found;
    }
    READABLE_FUNCTION_WAITS(findCloserObject, Verified);

    namespace
    {
        constexpr uint32_t kSearchTreeForEach = sym::searchTreeForEach;
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(searchTreeForEach, ee::game, kSearchTreeForEach);
        }
    }

    // removeCloserRecord: the closer kept under `key` taken out of the closers' tree and freed; 0 when the
    // tree has none.
    uint32_t removeCloserRecord(uint32_t key)
    {
        ee::Local<int32_t> keySlot;
        *keySlot = key;
        const ee::Ptr<zge::SearchTreeNode> removed = zge::searchTreeRemove(kListeners, keySlot);
        if (!removed)
            return 0;
        return aggressive::level::freeCloserRecord(ee::Address(removed));
    }
    READABLE_FUNCTION_WAITS(removeCloserRecord, Verified);

    // The closers' save size (searchTreeForEach with sumCloserSaveSize)
    uint32_t closerRecordsSaveSize()
    {
        ee::Local<int32_t> context;
        *context = 0;
        routed::searchTreeForEach(kCloserTree, sym::sumCloserSaveSize, context.address());
        return *context;
    }
    READABLE_FUNCTION_WAITS(closerRecordsSaveSize, Verified);

    namespace
    {
        // The NPC records saved with their closers (pickups.cpp).
        constexpr uint32_t kAskAreYouCloser = sym::askAreYouCloser;
    }

    // ---- the closers saved with the level (were the sweep's
    // sumCloserSaveSize .. restoreCloserRecords) and a second nearest-closer
    // search (nearestCloser)
    namespace
    {
        // A save stream: the bytes left and where the next record goes.
        struct SaveStream
        {
            uint32_t bytes;  // +0x00
            uint32_t buffer; // +0x04
        };

        // measureCloserSaves' walk: the sum, and its own argument beside it.
        struct SaveSizeSum
        {
            uint32_t bytes;    // +0x00
            uint32_t argument; // +0x04 read by nothing
        };
    }

    // sumCloserSaveSize: (the closers' walk) one closer's save added to the
    // sum: its name hash and its NPC's bubble. Returns 1 (go on).
    uint32_t sumCloserSaveSize(uint32_t /*closer*/, ee::Ptr<uint32_t> bytes)
    {
        *bytes = *bytes + 4 + static_cast<uint32_t>(aggressive::npcSaveSize());
        return 1;
    }
    READABLE_FUNCTION(sumCloserSaveSize, Verified);

    // saveCloserRecord: (the closers' walk) the closer's object's name hash
    // and its NPC's bubble into the stream. Returns 1 (go on).
    uint32_t saveCloserRecord(ee::Ptr<CloserRecord> closer, ee::Ptr<SaveStream> stream)
    {
        ee::Local<uint32_t> name;
        const int32_t bubbleBytes = aggressive::npcSaveSize();
        const uint32_t at = stream->buffer;
        const ee::Ptr<Npc> npc = aggressive::pickupOf(closer->object);
        *name = zge::objectNameHash(closer->object);
        zge::memoryCopy(at, name, 4);
        aggressive::saveNpcState(at + 4, npc);
        stream->buffer = at + 4 + bubbleBytes;
        return 1;
    }
    READABLE_FUNCTION(saveCloserRecord, Tested);

    // measureCloserSaves: the closers' save size summed, then dropped.
    // Returns 1.
    uint32_t measureCloserSaves(uint32_t argument)
    {
        ee::Local<SaveSizeSum> sum;
        zge::memoryFill(sum, 0, sizeof(SaveSizeSum));
        sum->argument = argument;
        sum->bytes = 0;
        routed::searchTreeForEach(kCloserTree, sym::sumCloserSaveSize, sum);
        return 1;
    }
    READABLE_FUNCTION_WAITS(measureCloserSaves, Verified);

    // restoreCloserRecords: the closers' records read back: the object by
    // its name looked up, its NPC's bubble made again. Note: as the original
    // does it, only the first record is read (its loop tests the record's
    // size, never 0, for going on), and the object found is not used.
    // Returns 1.
    uint32_t restoreCloserRecords(uint32_t buffer, uint32_t bytes)
    {
        ee::Local<uint32_t> name;
        const int32_t bubbleBytes = aggressive::npcSaveSize();
        const uint32_t recordBytes = static_cast<uint32_t>(bubbleBytes) + 4;
        if (bytes < recordBytes)
            return 1;
        do
        {
            zge::memoryCopy(name, buffer, 4);
            buffer += 4;
            zge::findObjects(name, 1);
            aggressive::restoreNpcState(buffer);
            buffer += bubbleBytes;
        } while (recordBytes == 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreCloserRecords, Verified);

    // nearestCloser: findCloserObject's search among the NPCs that give no
    // challenge (the listener nearest the position within `radius`; when
    // asked its distance, -1 for none).
    // The walk is handed its three arguments: neither it nor the listeners'
    // question reads the registers past them.
    uint32_t nearestCloser(ee::Ptr<zge::Vec4> position, ee::Ptr<ee::Float> distance, uint32_t /*unused*/, ee::Float radius)
    {
        ee::Local<CloserQuery> query;
        zge::memoryFill(query, 0, sizeof(CloserQuery));
        query->position = zge::quadwordOf(position);
        query->nearest = radius;
        query->kind = kCloserNotGiver;
        query->found = 0;
        routed::searchTreeForEach(kCloserTree, kAskAreYouCloser, query);
        if (distance)
            *distance = query->found ? query->nearest : ee::Float(-1.0f);
        return query->found;
    }
    READABLE_FUNCTION_WAITS(nearestCloser, Verified);
}
