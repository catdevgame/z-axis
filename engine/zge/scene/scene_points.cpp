// Scene points (zscnpts).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/assets/chunk_reader.h"
#include "zge/container/list.h"
#include "zge/scene/locators.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/debug/debug.h"
#include <cstring>
#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/scene_points.cpp.h"


namespace zge
{

    namespace
    {
        // The scene points' module (zscnpts.c): its pools, the region their
        // tag data comes from, and the debug task that draws them. ZGE2 has
        // no SDTD chunks: no SceneDataRecord pool or reader.
        inline ee::Global<uint32_t, sym::g_scenePointsVisible> g_scenePointsVisible;
        inline ee::Global<ee::Ptr<Pool>, sym::g_scenePointPool> g_scenePointPool;             // ScenePoint records
        // The pools' blocks: ZGE4 makes the point pools smaller and the locators' larger.
        constexpr uint32_t kScenePointsABlock = game::readable::perGame(0x80u, 0x80u, 0x40u);
        constexpr uint32_t kLocatorsABlock = game::readable::perGame(0x20u, 0x20u, 0x40u);
        constexpr bool kHasSceneDataRecords = !game::readable::kIsDm2;
        inline ee::Global<ee::Ptr<Pool>, sym::g_scenePointRecordPool> g_scenePointRecordPool; // SceneDataRecord records
        inline ee::Global<ee::Ptr<Pool>, sym::g_locatorPool> g_locatorPool;                   // Locator records (findLocators')
        inline ee::Global<ee::Ptr<MemoryRegion>, sym::g_scenePointRegion> g_scenePointRegion;
        inline ee::Global<ee::Ptr<Task>, sym::g_scenePointTask> g_scenePointTask;
        inline ee::Global<uint32_t, sym::g_scenePointColour> g_scenePointColour; // the debug points' RGBA

        // An SDTD chunk's record: a block of data under two hashes, matched
        // the way findScenePoint matches a point's.
        struct SceneDataRecord
        {
            uint32_t kindHash; // +0x00
            uint32_t nameHash; // +0x04
            uint32_t bytes;    // +0x08
            uint32_t data;     // +0x0C in the scene points' region, 0: none
        };
        static_assert(sizeof(SceneDataRecord) == 0x10);

        // The fixed part of an SDTD chunk; its data follows.
        struct SdtdChunk
        {
            uint32_t headerBytes; // +0x00
            uint32_t kindHash;    // +0x04
            uint32_t nameHash;    // +0x08
        };
        static_assert(sizeof(SdtdChunk) == 12);

        constexpr uint32_t kScenePointRegionBytes = game::readable::perGame(0x50000u, 0x40000u);
        constexpr uint32_t kTaskSourceLine = game::readable::perGame(274u, 229u); // in zscnpts.c

        // A walk over a pool's records in use, the newest first: its links,
        // each step the game call it is.
        uint32_t firstLink(ee::Ptr<Pool> pool) { return poolFirst(ee::Ptr<PoolList>(&pool->used)); }
        uint32_t nextLink(uint32_t link) { return poolNext(ee::Ptr<PoolLink>(link)); }
        // (By address, like the two above: identical small functions may be folded by a linker.)
        uint32_t recordOf(uint32_t link) { return poolRecord(link); }
        // A point's position, as an address (ZGE2's is not at the record's start).
        uint32_t positionOf(uint32_t point) { return point + offsetof(ScenePoint, position); }
    }

    namespace
    {
        namespace routed
        {
        }
    }

    uint32_t resetScenePoints()
    {
        if (!*g_scenePointPool)
            *g_scenePointPool = ee::Ptr<Pool>(createPool(sizeof(ScenePoint), kScenePointsABlock, 0));
        if constexpr (kHasSceneDataRecords)
        {
            if (!*g_scenePointRecordPool)
                *g_scenePointRecordPool = ee::Ptr<Pool>(createPool(sizeof(SceneDataRecord), kScenePointsABlock, 0));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(resetScenePoints, Verified);

    // (record): given back to the scene points' pool (g_scenePointPool); poolForEach's callback in
    // leaveModelScenes.
    uint32_t freeScenePoint(uint32_t record)
    {
        if (record != 0)
        {
            poolFree(*g_scenePointPool, record);
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION(freeScenePoint, Verified);

    uint32_t freeScenePointRecord(uint32_t record)
    {
        if (record != 0)
        {
            poolFree(*g_scenePointRecordPool, record);
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION(freeScenePointRecord, Draft);

    namespace
    {
        // The pools kept in these globals destroyed (those there are) and forgotten.
        void destroyPools(std::initializer_list<ee::Ptr<Pool> *> pools)
        {
            for (ee::Ptr<Pool> *pool : pools)
            {
                if (*pool)
                {
                    destroyPool(*pool);
                    *pool = ee::Ptr<Pool>();
                }
            }
        }

        // The locator pool destroyed, when there is one (findLocators makes it again).
        void destroyLocatorPool()
        {
            if (*g_locatorPool)
            {
                destroyPool(*g_locatorPool);
                *g_locatorPool = ee::Ptr<Pool>();
            }
        }
    }

    // ZGE4 also gives each pool's empty blocks back and destroys the locator pool.
    uint32_t leaveModelScenes()
    {
        poolForEach(*g_scenePointPool, sym::freeScenePoint, 0);
        if constexpr (game::readable::kIsBmx)
            releaseEmptyPoolBlocks(*g_scenePointPool);
        if constexpr (kHasSceneDataRecords)
            poolForEach(*g_scenePointRecordPool, sym::freeScenePointRecord, 0);
        if constexpr (game::readable::kIsBmx)
            releaseEmptyPoolBlocks(*g_scenePointRecordPool);
        if constexpr (game::readable::kIsBmx)
            destroyLocatorPool();
        zge::rewindRegion(*g_scenePointRegion);
        return game::tail(resetScenePoints);
    }
    READABLE_FUNCTION_WAITS(leaveModelScenes, Verified);

    // readSdptChunk: a scene point from its SDPT chunk: its kind, name and
    // index, its position (w 1) and orientation; what the chunk holds after
    // the fixed part is its tag data, copied into the scene points' region.
    uint32_t readSdptChunk(ChunkReader *reader)
    {
        ee::Local<SdptChunk> chunk;
        readChunk(reader, chunk, sizeof(SdptChunk));
        const ee::Ptr<ScenePoint> point(poolAlloc(*g_scenePointPool));
        point->kindHash = chunk->kindHash;
        point->nameHash = chunk->nameHash;
        point->index = chunk->index;
        point->position.x = chunk->position.x;
        point->position.y = chunk->position.y;
        point->position.w = 1.0f;
        point->position.z = chunk->position.z;
        point->orientation.x = chunk->orientation[0];
        point->orientation.y = chunk->orientation[1];
        point->orientation.z = chunk->orientation[2];
        point->tags = 0;
        point->orientation.w = chunk->orientation[3];
        if (chunkDataSize(reader) != chunk->headerBytes)
        {
            const uint32_t bytes = chunkDataSize(reader) - chunk->headerBytes;
            const uint32_t tags = zge::regionAllocate(*g_scenePointRegion, bytes);
            point->tagBytes = static_cast<decltype(ScenePoint::tagBytes)>(bytes);
            point->tags = ee::Ptr<TagRecord>(tags);
            readChunk(reader, ee::Ptr<uint8_t>(tags), bytes);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readSdptChunk, Verified);

    // readSdtdChunk: a block of scene data from its SDTD chunk, kept the same
    // way (no level in this build has one).
    uint32_t readSdtdChunk(ChunkReader *reader)
    {
        ee::Local<SdtdChunk> chunk;
        readChunk(reader, chunk, sizeof(SdtdChunk));
        const ee::Ptr<SceneDataRecord> record(poolAlloc(*g_scenePointRecordPool));
        record->kindHash = chunk->kindHash;
        record->data = 0;
        record->nameHash = chunk->nameHash;
        if (chunkDataSize(reader) != chunk->headerBytes)
        {
            const uint32_t bytes = chunkDataSize(reader) - chunk->headerBytes;
            const uint32_t data = zge::regionAllocate(*g_scenePointRegion, bytes);
            record->bytes = bytes;
            record->data = data;
            readChunk(reader, ee::Ptr<uint8_t>(data), bytes);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readSdtdChunk, Draft);

    // registerScenePointChunkHandlers: the module set up: no pools or task
    // yet, a region for the tag data (320 KB; ZGE2's 256 KB), the pools made
    // (resetScenePoints), and the ZFSD, SDPT and SDTD chunks (ZGE2: ZFSD and
    // SDPT) handed to their readers.
    uint32_t registerScenePointChunkHandlers()
    {
        *g_scenePointTask = 0;
        *g_scenePointPool = ee::Ptr<Pool>();
        if constexpr (kHasSceneDataRecords)
            *g_scenePointRecordPool = ee::Ptr<Pool>();
        *g_locatorPool = ee::Ptr<Pool>();
        *g_scenePointRegion = zge::newMemoryRegion(0, kScenePointRegionBytes);
        resetScenePoints();
        addChunkHandler(chunkTag("ZFSD"), sym::readZfsdChunk);
        addChunkHandler(chunkTag("SDPT"), sym::readSdptChunk);
        if constexpr (kHasSceneDataRecords)
            addChunkHandler(chunkTag("SDTD"), sym::readSdtdChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerScenePointChunkHandlers, Verified);

    // setScenePointTask: the scene points drawn (1: a debug task draws them
    // each frame) or not.
    void setScenePointTask(uint32_t visible)
    {
        if (visible == 1)
        {
            if (*g_scenePointTask == 0)
                *g_scenePointTask = newTask(1, sym::drawScenePoints, 0, 1, sym::g_textNgengineZge3ZgeModsZscnpts, kTaskSourceLine);
        }
        else if (*g_scenePointTask != 0)
        {
            deleteTask(*g_scenePointTask);
            *g_scenePointTask = 0;
        }
        *g_scenePointsVisible = visible;
    }
    READABLE_FUNCTION_WAITS(setScenePointTask, Verified);

    // shutdownScenePoints: the task stopped, the pools destroyed and the
    // region freed. 1.
    uint32_t shutdownScenePoints()
    {
        setScenePointTask(0u);
        if constexpr (kHasSceneDataRecords)
            destroyPools({g_scenePointPool.get(), g_scenePointRecordPool.get(), g_locatorPool.get()});
        else
            destroyPools({g_scenePointPool.get(), g_locatorPool.get()});
        zge::freeMemoryRegion(*g_scenePointRegion);
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownScenePoints, Verified);

    // drawScenePoints: the debug task's work: while the points are shown, a
    // small sphere (radius 0.2) at each, in the points' colour.
    uint32_t drawScenePoints()
    {
        ee::Local<uint32_t> colour;
        if (*g_scenePointsVisible == 0)
            return 1;
        *colour = *g_scenePointColour;
        for (uint32_t link = firstLink(*g_scenePointPool); link != 0; link = nextLink(link))
            zge::addDebugPoint(positionOf(recordOf(link)), colour, 0, 0, ee::Float(0.2f));
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawScenePoints, Draft);

    // findScenePoint: the newest point of the kind and name (0: any) with
    // this index, or 0.
    ee::Ptr<ScenePoint> findScenePoint(uint32_t kindHash, uint32_t nameHash, uint32_t index)
    {
        for (uint32_t link = firstLink(*g_scenePointPool); link != 0; link = nextLink(link))
        {
            const ee::Ptr<ScenePoint> point = ee::Ptr<ScenePoint>(recordOf(link));
            if (kindHash != 0 && point->kindHash != kindHash)
                continue;
            if (nameHash != 0 && point->nameHash != nameHash)
                continue;
            if (point->index == index)
                return point;
        }
        return ee::Ptr<ScenePoint>();
    }
    READABLE_FUNCTION(findScenePoint, Verified);

    // findLocators: every point of the kind and name (0: any) put on the
    // list (emptied first) in a Locator, in the pool's order (the newest
    // first); how many (counted in 16 bits).
    uint32_t findLocators(ee::Address listAddress, uint32_t kindHash, uint32_t nameHash)
    {
        const ee::Ptr<List> list(listAddress); // the caller's LocatorList, as the list it is
        uint16_t found = 0;
        initList(list);
        for (uint32_t link = firstLink(*g_scenePointPool); link != 0; link = nextLink(link))
        {
            const uint32_t record = recordOf(link);
            const ee::Ptr<ScenePoint> point(record);
            if (kindHash != 0 && point->kindHash != kindHash)
                continue;
            if (nameHash != 0 && point->nameHash != nameHash)
                continue;
            if (!*g_locatorPool)
                *g_locatorPool = ee::Ptr<Pool>(createPool(sizeof(Locator), kLocatorsABlock, 0));
            const ee::Ptr<Locator> locator(poolAlloc(*g_locatorPool));
            locator->point = record;
            listPush(list, locator);
            ++found;
        }
        return found;
    }
    READABLE_FUNCTION_WAITS(findLocators, Verified);

    // freeLocators: the list's Locators given back.
    void freeLocators(ee::Address listAddress)
    {
        const ee::Ptr<List> list(listAddress); // the caller's LocatorList, as the list it is
        for (ee::Ptr<ListNode> locator = list->head; locator; locator = list->head)
        {
            listRemove(list, locator);
            poolFree(*g_locatorPool, locator);
        }
    }
    READABLE_FUNCTION(freeLocators, Verified);

    // addScenePoint (no caller in this build): a copy of `source` (and of its
    // tag data) as the next index of its kind and name: one past the highest
    // index those points have.
    void addScenePoint(ee::Ptr<ScenePoint> source)
    {
        const ee::Ptr<ScenePoint> point(poolAlloc(*g_scenePointPool));
        memoryCopy(point, source, sizeof(ScenePoint));
        if (point->tagBytes != 0)
        {
            point->tags = ee::Ptr<TagRecord>(zge::regionAllocate(*g_scenePointRegion, point->tagBytes));
            memoryCopy(point->tags, source->tags, point->tagBytes);
        }
        else
            point->tags = ee::Ptr<TagRecord>();
        uint32_t highest = 0;
        for (uint32_t link = firstLink(*g_scenePointPool); link != 0; link = nextLink(link))
        {
            const ee::Ptr<ScenePoint> other = ee::Ptr<ScenePoint>(recordOf(link));
            if (other->kindHash == point->kindHash && other->nameHash == point->nameHash && other->index > highest)
                highest = other->index;
        }
        point->index = static_cast<decltype(ScenePoint::index)>(highest + 1);
    }
    READABLE_FUNCTION_WAITS(addScenePoint, Draft);

    // findScenePointByKey (no caller in this build): the newest SDTD record
    // of the kind and name (0: any), or 0.
    ee::Ptr<SceneDataRecord> findScenePointByKey(uint32_t kindHash, uint32_t nameHash)
    {
        for (uint32_t link = firstLink(*g_scenePointRecordPool); link != 0; link = nextLink(link))
        {
            const ee::Ptr<SceneDataRecord> record = ee::Ptr<SceneDataRecord>(recordOf(link));
            if (kindHash != 0 && record->kindHash != kindHash)
                continue;
            if (record->nameHash == nameHash || nameHash == 0)
                return record;
        }
        return ee::Ptr<SceneDataRecord>();
    }
    // (BMX XXX's code at the address its bind gives is ZGE4's findModelByName, model_records.cpp: no registration there)
    READABLE_EXCEPT(bmx, READABLE_FUNCTION(findScenePointByKey, Draft));

}
