// The world a level is loaded into (zglTerrainRender.c, releaseWorldNodes ..; BMX XXX
// PS2 word for word): the grid of sectors that holds the level's nodes and
// three lists: the nodes in the world, the geometry objects and the
// textures that are the world's own. A node stands in every sector its box
// touches (AxisCells); the scene draw and the queries walk sectors.

#include "collision.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/game_address.h"
#include "zge/math/vec.h"
#include "zge/assets/geo.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/math/box.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/render/draw.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/scene/camera.h"
#include "zge/scene/visibility_regions.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/display.h"
#include "zge/render/part.h"
#include "zge/render/material.h"
#include "zge/render/terrain.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/collision/terrain.cpp.h"

namespace zge
{

    namespace
    {
        void displayIdle() { displayNoop(); }
        constexpr uint32_t kFinishWorldMesh = sym::finishWorldMesh;           // finishCollisionWorld's visits
        constexpr uint32_t kReleaseOrphanPart = sym::releaseOrphanPart;
        constexpr uint32_t kForEachWorldMesh = sym::forEachWorldMesh;          // (visit, context)
        constexpr uint32_t kFreeCollisionWorld = sym::releaseCollisionWorld;        // (world)
        constexpr uint32_t kSetSkyNode = sym::setSkyNode;                // (node)
        constexpr uint32_t kSetCollisionWorld = sym::setCollisionWorld;         // (world)
        constexpr uint32_t kBuildWorldGrid = sym::buildWorldGrid;            // ()
        constexpr uint32_t kFreeWorldGrid = sym::freeWorldGrid;             // (world)
        constexpr uint32_t kAssignCellRegions = sym::assignCellRegions;         // ()
        constexpr uint32_t kUpdateMovedNodeVisitor = sym::updateMovedNodeVisitor; // (node, context): updateMovedNode(node), 1 (ZGE4's finishCollisionWorld)
        constexpr uint32_t kMatchNthNodeByHash = sym::matchNthNodeByHash;        // (node, NodeSearch *)
        constexpr uint32_t kRemoveWorldTexture = sym::removeWorldTexture;        // (texture)
        constexpr uint32_t kSkyNode = sym::skyNode;                   // () -> the node
        constexpr uint32_t kRequestWorldGrid = sym::requestWorldGrid;          // the terrain's parts, set up and shut down
        constexpr uint32_t kReleaseWorldGrid = sym::releaseWorldGrid;
        constexpr uint32_t kStartTerrainRenderTask = sym::startTerrainRenderTask;
        constexpr uint32_t kStopTerrainRenderTask = sym::stopTerrainRenderTask;
        constexpr uint32_t kRegisterSdrdChunkHandler = sym::registerSdrdChunkHandler;
        constexpr uint32_t kShutdownSectorFlags = sym::shutdownSectorFlags;
        constexpr uint32_t kClearSectorFlags = sym::clearSectorFlags;
        constexpr uint32_t kInitGeometryPool = sym::initGeometryPool;
        constexpr uint32_t kShutdownGeometryPool = sym::shutdownGeometryPool;
        constexpr uint32_t kRegisterGeometryChunkHandlers = sym::registerGeometryChunkHandlers;
        constexpr uint32_t kResetGeometryNodeFlag = sym::resetGeometryNodeFlag;
        constexpr uint32_t kInitGeometryNodes = sym::initGeometryNodes;
        constexpr uint32_t kShutdownGeometryNodes = sym::shutdownGeometryNodes;
        constexpr uint32_t kShutdownCollisionFramePool = sym::shutdownCollisionFramePool;
        constexpr uint32_t kShutdownFramePartPool = sym::shutdownFramePartPool;
        constexpr uint32_t kRegisterPvsChunkHandlers = sym::registerPvsChunkHandlers;
        constexpr uint32_t kShutdownRegions = sym::shutdownRegions;
        constexpr uint32_t kFreePointerArray = sym::freePointerArray;          // (array)
        constexpr uint32_t kPointerArrayRemove = sym::pointerArrayRemove;        // (array, item)
        constexpr uint32_t kPointerArrayRemoveAt = sym::pointerArrayRemoveAt;      // (array, index)
        constexpr uint32_t kSphereFromBox = sym::sphereFromBox;             // (sphere, box)

        // CollisionWorld::lists.
        enum WorldList
        {
            kWorldNodes = 0,
            kWorldObjects = 1,  // GeoObjects
            kWorldTextures = 2,
        };
        constexpr uint32_t kWorldNodeCapacity = 0x400;
        constexpr uint32_t kWorldObjectCapacity = 0x100;
        constexpr uint32_t kWorldTextureCapacity = 0x200;
        constexpr uint32_t kSectorNodeCapacity = 0x40;

        constexpr uint32_t kTerrainRenderTask = sym::drawScene;
        constexpr uint32_t kTerrainTaskPriority = 257;
        constexpr uint32_t kThisFile = sym::g_textNgengineZge3ComZglCZglTerrainRende;                  // the path of zglTerrainRender.c
        constexpr uint32_t kThisLine = game::readable::perGame(867u, 359u, 847u); // the newTask call's line in each engine's file
        constexpr uint32_t kSdrdChunk = 0x44524453u;                 // 'SDRD'
        constexpr uint32_t kReadSdrdChunk = sym::readSdrdChunk;

        // A record an SDRD chunk would fill.
        struct SdrdRecord
        {
            uint64_t bits; // +0x00 kSdrdRecordUsed, the record's index on top (with the record's id in the low word)
            uint8_t _08[0x58];
        };
        static_assert(sizeof(SdrdRecord) == 0x60);
        constexpr uint32_t kSdrdRecords = 32;
        constexpr uint64_t kSdrdRecordUsed = 1ull << 58;
        constexpr uint32_t kSdrdRecordIndexShift = 59;
        constexpr uint64_t kSdrdRecordBelowIndex = ~0ull >> 5;
        const ee::Ptr<SdrdRecord> g_sdrdRecords(sym::g_sectors);
        inline ee::Global<uint32_t, sym::g_activeSdrdRecords> g_activeSdrdRecords;
        inline ee::Global<uint32_t, sym::g_changedSdrdRecords> g_changedSdrdRecords;

        inline ee::Global<uint32_t, sym::g_terrainRenderTaskRunning> g_terrainRenderTaskRunning;
        inline ee::Global<ee::Ptr<Task>, sym::g_terrainRenderTask> g_terrainRenderTask;
        inline ee::Global<uint32_t, sym::g_terrainReady> g_terrainReady;
        inline ee::Global<uint32_t, sym::g_sceneFileMode> g_sceneFileMode;
        inline ee::Global<uint32_t, sym::g_worldGridRequested> g_worldGridRequested;

        PointerArray &worldList(ee::Ptr<CollisionWorld> world, WorldList list) { return *world->lists[list]; }

        // A sector begins with its box, and holds its nodes as a pointer array.
        ee::Ptr<Box> sectorBox(ee::Ptr<Sector> sector) { return ee::Ptr<uint8_t>(&sector->box[0]).cast<Box>(); }
        // ZGE2's sector has four words before its box, which buildWorldGrid clears. (A template, so that the words the
        // other layouts lack are named only in ZGE2's build.)
        template <typename SectorType = Sector>
        void clearSectorHead(ee::Ptr<SectorType> sector)
        {
            if constexpr (game::readable::kIsDm2)
                for (uint32_t &word : sector->cleared)
                    word = 0;
        }
        ee::Ptr<PointerArray> sectorNodes(ee::Ptr<Sector> sector) { return sector->nodes.cast<PointerArray>(); }

        // Where the list is empty, where a walk's first item is tested, and
        // the loop's own.
        // A walk over a world list: ZGE3 writes the array's walk inline (array.h
        // walkFirst, walkNext), ZGE2 calls arrayFirst and arrayNext.
        uint32_t firstListed(PointerArray &array)
        {
            if constexpr (game::readable::kIsDm2)
                return arrayFirst(&array);
            else
                return walkFirst(array);
        }
        void nextListed(PointerArray &array, uint32_t &item)
        {
            if constexpr (game::readable::kIsDm2)
                item = arrayNext(&array);
            else
                walkNext(array, item);
        }

        // The list emptied: each pass takes its first item, and `release`
        // must take that out of it.
        template <typename Release>
        void drainWorldList(ee::Ptr<CollisionWorld> world, WorldList list, const Release &release)
        {
            uint32_t item = 0;
            for (bool first = true;; first = false)
            {
                if (!first)
                    release(item);
                PointerArray &array = worldList(world, list);
                item = firstListed(array);
                if (!item)
                    return;
            }
        }

        // Each of a list's items through callback(item, context) until one
        // returns 0.
        uint32_t visitWorldList(WorldList list, uint32_t callback, uint32_t context)
        {
            const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
            if (world)
            {
                PointerArray &array = worldList(world, list);
                uint32_t item = firstListed(array);
                if (item)
                {
                    for (;;)
                    {
                        if (ee::call<int32_t>(callback, item, context) == 0)
                            return 0;
                        nextListed(worldList(world, list), item);
                        if (!item)
                            break;
                    }
                }
            }
            return 1;
        }

        ee::Ptr<Sector> sectorAt(const CollisionWorld &world, int32_t x, int32_t y, int32_t z)
        {
            const uint32_t index = (static_cast<uint32_t>(x) * static_cast<uint32_t>(world.cellCount[1]) + static_cast<uint32_t>(y)) *
                                       static_cast<uint32_t>(world.cellCount[2]) + static_cast<uint32_t>(z);
            return world.cell + index;
        }

        // Every sector of the grid.
        template <typename Visit>
        void forEachSector(const CollisionWorld &world, const Visit &visit)
        {
            for (int32_t x = 0; x < world.cellCount[0]; ++x)
                for (int32_t y = 0; y < world.cellCount[1]; ++y)
                    for (int32_t z = 0; z < world.cellCount[2]; ++z)
                        visit(sectorAt(world, x, y, z), x, y, z);
        }

        // Shutdown's and startup's steps: each says whether it worked.
        bool stepDone(uint32_t target)
        {
            return ee::call<int32_t>(target) != 0;
        }
    }

    // startTerrainRenderTask: the task that draws the world each frame (drawScene).
    uint32_t startTerrainRenderTask()
    {
        const ee::Ptr<Task> task = *g_terrainRenderTask;
        *g_terrainRenderTaskRunning = 1;
        if (!task)
            *g_terrainRenderTask = newTask(kTerrainTaskPriority, kTerrainRenderTask, 0, 1, kThisFile, kThisLine);
        return 1;
    }
    READABLE_FUNCTION_WAITS(startTerrainRenderTask, Verified);

    // stopTerrainRenderTask.
    uint32_t stopTerrainRenderTask()
    {
        if (*g_terrainRenderTaskRunning != 0)
        {
            const ee::Ptr<Task> task = *g_terrainRenderTask;
            if (task)
                deleteTask(task);
            *g_terrainRenderTask = ee::Ptr<Task>();
            *g_terrainRenderTaskRunning = 0;
        }
        return 1;
    }
    READABLE_FUNCTION(stopTerrainRenderTask, Verified);

    // registerSdrdChunkHandler: each SDRD record unused and told its index, and the
    // chunk's handler registered. (No file of the nine releases has such a
    // chunk, so the rest of the records' code stays in the sweep, dead.)
    uint32_t registerSdrdChunkHandler()
    {
        for (uint32_t index = 0;;)
        {
            uint64_t &bits = g_sdrdRecords[index].bits;
            bits = (bits & ~kSdrdRecordUsed & kSdrdRecordBelowIndex) | static_cast<uint64_t>(index) << kSdrdRecordIndexShift;
            if (++index >= kSdrdRecords)
                break;
        }
        *g_activeSdrdRecords = 0;
        *g_changedSdrdRecords = 0;
        addChunkHandler(kSdrdChunk, kReadSdrdChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerSdrdChunkHandler, Verified);

    void clearSectorFlags();

    // shutdownSectorFlags.
    uint32_t shutdownSectorFlags()
    {
        clearSectorFlags();
        return 1;
    }
    READABLE_FUNCTION(shutdownSectorFlags, Verified);

    // clearSectorFlags.
    void clearSectorFlags()
    {
        for (uint32_t index = 0;;)
        {
            g_sdrdRecords[index].bits &= ~kSdrdRecordUsed;
            if (++index >= kSdrdRecords)
                break;
        }
        *g_activeSdrdRecords = 0;
        *g_changedSdrdRecords = 0;
    }

    // ZGE2's cell tree: the world's grid of cells as a box cut in two, each half cut again, down to single cells or
    // six levels deep (buildCellTree, cutCellBox). Its records sit in one block from the heap (0x200 of them), the
    // root first. ZGE2 has no SDRD records.
    struct CellBox
    {
        uint8_t cutAxis;   // 0: not cut; 1, 2, 3: cut across x, y or z into halves[0] (below the cut) and halves[1]
        uint8_t low[3];    // the box's first cell along x, y and z
        uint8_t high[3];   // past its last cell
        uint8_t cutAt;     // the cell plane its parent was cut at
        int16_t halves[2]; // the two halves, counted in records from this one
    };
    static_assert(sizeof(CellBox) == 0xC);
    inline ee::Global<ee::Ptr<CellBox>, sym::dm2::g_cellTree> g_cellTree;
    constexpr uint32_t kCellTreeRecords = 0x200;

    // clearSectorRegionHeader: ZGE2's clearSectorFlags: the tree's root neither cut nor holding a cell.
    void clearSectorRegionHeader()
    {
        (*g_cellTree)->cutAxis = 0;
        for (int axis = 0; axis < 3; ++axis)
            (*g_cellTree)->low[axis] = 0;
        for (int axis = 0; axis < 3; ++axis)
            (*g_cellTree)->high[axis] = 0;
    }
    READABLE_FUNCTION_OWN(clearSectorFlags, Verified, dm2, clearSectorRegionHeader);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t freePointerArray(ee::Address record) { return ee::game<int32_t>(kFreePointerArray, record); }
    }

    // releaseWorldNodes: the world's nodes released with their children (which
    // takes each out of the list), then the list.
    int32_t releaseWorldNodes(ee::Ptr<CollisionWorld> world)
    {
        drainWorldList(world, kWorldNodes, [&](uint32_t node) {
            releaseNode(ee::Ptr<Node>(node), 1); // the list's item word
        });
        const int32_t result = freePointerArray(world->lists[kWorldNodes]);
        world->lists[kWorldNodes] = ee::Ptr<PointerArray>();
        return result;
    }
    READABLE_FUNCTION_WAITS(releaseWorldNodes, Verified);

    // releaseWorldObjects.
    void releaseWorldObjects(ee::Ptr<CollisionWorld> world)
    {
        drainWorldList(world, kWorldObjects, [&](uint32_t object) {
            releaseGeoObject(ee::Ptr<GeoObject>(object));
        });
        freePointerArray(world->lists[kWorldObjects]);
        world->lists[kWorldObjects] = ee::Ptr<PointerArray>();
    }
    READABLE_FUNCTION_WAITS(releaseWorldObjects, Verified);

    uint32_t removeWorldTexture(uint32_t texture);

    // releaseWorldTextures.
    int32_t releaseWorldTextures(ee::Ptr<CollisionWorld> world)
    {
        drainWorldList(world, kWorldTextures, [&](uint32_t texture) {
            removeWorldTexture(texture);
            releaseLoadedTexture(ee::Ptr<Texture>(texture));
        });
        const int32_t result = freePointerArray(world->lists[kWorldTextures]);
        world->lists[kWorldTextures] = ee::Ptr<PointerArray>();
        return result;
    }
    READABLE_FUNCTION_WAITS(releaseWorldTextures, Verified);

    // setSkyNode.
    void setSkyNode(ee::Ptr<Node> node)
    {
        (*g_collisionWorld)->skyNode = node;
    }
    READABLE_FUNCTION(setSkyNode, Verified);

    // initTerrain: the terrain's parts up, each one needing the last.
    uint32_t initTerrain()
    {
        // (ZGE2 has no SDRD chunk handler to register: its first step is the render task.)
        const bool sdrdHandler = game::readable::kIsDm2 ? true : stepDone(kRegisterSdrdChunkHandler);
        const bool ready = sdrdHandler && stepDone(kStartTerrainRenderTask) &&
                           stepDone(kRequestWorldGrid) && stepDone(kInitGeometryPool) &&
                           stepDone(kInitGeometryNodes) && initCollisionFrames() != 0 &&
                           initFrameParts() != 0 && initDrawBuckets() != 0 &&
                           initParts() != 0 && stepDone(kRegisterGeometryChunkHandlers) &&
                           stepDone(kRegisterPvsChunkHandlers);
        if (ready)
            *g_terrainReady = 1;
        return ready ? 1u : 0u;
    }
    READABLE_FUNCTION_WAITS(initTerrain, Verified);

    // shutdownTerrain: the world freed and the parts down, stopping at the
    // first that fails (the terrain then still counts as up).
    uint32_t shutdownTerrain()
    {
        if (*g_terrainReady != 0)
        {
            const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
            if (world)
                releaseCollisionWorld(world);
            const bool regionsDown = stepDone(kShutdownRegions) && stepDone(kResetGeometryNodeFlag);
            // (ZGE2 has no sector flags to shut down: its chain ends with the parts)
            const bool down = regionsDown &&
                              freeDrawBuckets() != 0 && stepDone(kStopTerrainRenderTask) &&
                              stepDone(kShutdownGeometryNodes) && stepDone(kReleaseWorldGrid) &&
                              stepDone(kShutdownGeometryPool) && stepDone(kShutdownCollisionFramePool) &&
                              stepDone(kShutdownFramePartPool) && shutDownParts() != 0 &&
                              (game::readable::kIsDm2 ? true : stepDone(kShutdownSectorFlags));
            if (!down)
                return 0;
            *g_terrainReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownTerrain, Verified);

    namespace
    {
        // ZGE2's tree over the world's cells: 1 once startEngine has allocated its nodes (512 of 12 bytes: an axis, the
        // cell range's first and count along x, y, z, two child indices); finishCollisionWorld splits the world's cell
        // counts into it, and nothing walks it (its two walkers have no caller). ZGE3 has no such tree.
        inline ee::Global<uint32_t, sym::dm2::g_worldCellTreeReady> g_worldCellTreeReady;
    }

    // shutdownWorldCellTree (ZGE2 only; shutdownEngine's step after the tag tables): the world cell tree no longer
    // ready, 1; 0 when it was not. The nodes' block stays allocated.
    inline uint32_t shutdownWorldCellTree()
    {
        if (*g_worldCellTreeReady == 0)
            return 0;
        *g_worldCellTreeReady = 0;
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, shutdownWorldCellTree);

    // initWorldCellTree (ZGE2 only; startEngine's step): the cell tree's block of records from the heap, the tree
    // ready and its root cleared (clearSectorFlags). 1; 0 when it was ready already.
    inline uint32_t initWorldCellTree()
    {
        if (*g_worldCellTreeReady == 1)
            return 0;
        *g_cellTree = ee::Ptr<CellBox>(heapAllocate(kCellTreeRecords * sizeof(CellBox)));
        *g_worldCellTreeReady = 1;
        clearSectorFlags();
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, initWorldCellTree);

    void setCollisionWorld(ee::Ptr<CollisionWorld> world);

    void buildWorldGrid();

    // newCollisionWorld: a world of the bounds' size, in cells of cellSize, with
    // its grid. While a level adds a file to the scene (setSceneFileMode) the
    // current world stays.
    ee::Ptr<CollisionWorld> newCollisionWorld(ee::Ptr<Vec4> cellSize, ee::Ptr<Box> bounds)
    {
        // ZGE2 has no scene file mode, keeps no inverse cell size and no cell total (its world is 0x60 bytes,
        // kWorldBytes), and has no level frame to reset.
        using game::readable::kIsDm2;
        if constexpr (!kIsDm2)
            if (*g_sceneFileMode != 0)
                return *g_collisionWorld;
        constexpr uint32_t kWorldBytes = game::readable::perGame(0x70u, 0x60u, 0x70u);
        const ee::Ptr<CollisionWorld> world(heapAllocate(kWorldBytes));
        if (!world)
            return 0;
        memoryFill(world, 0, kWorldBytes);
        world->cellSize = *cellSize;
        world->boundsMin = bounds->low;
        world->boundsMax = bounds->high;
        if constexpr (!kIsDm2)
        {
            world->inverseCellSize.x = ee::Float(1.0f) / world->cellSize.x;
            world->inverseCellSize.y = ee::Float(1.0f) / world->cellSize.y;
            world->inverseCellSize.z = ee::Float(1.0f) / world->cellSize.z;
        }
        // Cells along an axis: ZGE4 rounds the quotient to the nearest (half away from zero), ZGE3 truncates it.
        const auto cellsAlong = [](ee::Float extent, ee::Float size)
        {
            ee::Float count = extent / size;
            if constexpr (game::readable::kIsBmx)
                count = count < ee::Float(0.0f) ? count - ee::Float(0.5f) : count + ee::Float(0.5f);
            return ee::toInt(count);
        };
        world->cellCount[0] = cellsAlong(bounds->high.x - bounds->low.x, world->cellSize.x);
        world->cellCount[1] = cellsAlong(bounds->high.y - bounds->low.y, world->cellSize.y);
        world->cellCount[2] = cellsAlong(bounds->high.z - bounds->low.z, world->cellSize.z);
        if constexpr (!kIsDm2)
            world->cells = static_cast<int32_t>(static_cast<uint32_t>(world->cellCount[0]) * static_cast<uint32_t>(world->cellCount[1]) *
                                                static_cast<uint32_t>(world->cellCount[2]));
        world->lists[kWorldObjects] = newCursorArray(kWorldObjectCapacity);
        world->lists[kWorldNodes] = newCursorArray(kWorldNodeCapacity);
        world->lists[kWorldTextures] = newCursorArray(kWorldTextureCapacity);
        setCollisionWorld(world);
        buildWorldGrid();
        collisionWorldHook();
        if constexpr (!kIsDm2)
            resetLevelFrame();
        return world;
    }
    READABLE_FUNCTION_WAITS(newCollisionWorld, Verified);

    void freeWorldGrid(ee::Ptr<CollisionWorld> world);

    // releaseCollisionWorld: the frame finished, then everything the world holds.
    uint32_t releaseCollisionWorld(ee::Ptr<CollisionWorld> world)
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4 resets the frame packets before the wait too)
            resetFramePackets();
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no frame-ready wait; its freeAllRegions is identical)
            waitForFrameReady();
        freeAllRegions();
        clearSectorFlags();
        releaseWorldNodes(world);
        releaseWorldObjects(world);
        releaseWorldTextures(world);
        freeWorldGrid(world);
        setSkyNode(ee::Ptr<Node>());
        freeLitoRecords();
        heapFree(world);
        setCollisionWorld(ee::Ptr<CollisionWorld>());
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no material table count to reset)
            resetMaterialTableCount();
        resetFramePackets();
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseCollisionWorld, Verified);

    // setSceneFileMode.
    void setSceneFileMode(uint32_t mode) { *g_sceneFileMode = mode; }
    READABLE_FUNCTION(setSceneFileMode, Verified);

    void assignCellRegions();

    // finishCollisionWorld: after a level's files: regions finished, each sector
    // given its own, every mesh finished and the parts nothing uses released.
    uint32_t finishCollisionWorld()
    {
        finishRegions();
        assignCellRegions();
        if constexpr (!game::readable::kIsDm2) // (ZGE2 finishes no meshes and releases no parts here)
        {
            forEachWorldMesh(kFinishWorldMesh, 0);
            forEachPart(kReleaseOrphanPart, 0);
            if constexpr (game::readable::kIsBmx) // (ZGE4 brings every mesh's moved node up to date here too)
                forEachWorldMesh(kUpdateMovedNodeVisitor, 0);
            displayIdle();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(finishCollisionWorld, Verified);

    // forEachWorldObject.
    uint32_t forEachWorldObject(uint32_t callback, ee::Address context)
    {
        return visitWorldList(kWorldObjects, callback, context);
    }
    READABLE_FUNCTION(forEachWorldObject, Verified);

    // forEachWorldMesh.
    uint32_t forEachWorldMesh(uint32_t callback, ee::Address context)
    {
        return visitWorldList(kWorldNodes, callback, context);
    }
    READABLE_FUNCTION(forEachWorldMesh, Verified);

    // setCollisionWorld.
    void setCollisionWorld(ee::Ptr<CollisionWorld> world) { *g_collisionWorld = world; }
    READABLE_FUNCTION(setCollisionWorld, Verified);

    // collisionWorldBounds: the world's two corners, or 0.
    ee::Ptr<Vec4> collisionWorldBounds()
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        return world ? ee::Ptr<Vec4>(&world->boundsMin) : ee::Ptr<Vec4>();
    }
    READABLE_FUNCTION(collisionWorldBounds, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void pointerArrayAdd(ee::Ptr<PointerArray> array, ee::Address item) { game::tail(zge::pointerArrayAdd, array, item); }
        }
    }

    // addWorldObject.
    void addWorldObject(ee::Address object)
    {
        tail::pointerArrayAdd((*g_collisionWorld)->lists[kWorldObjects], object);
    }
    READABLE_FUNCTION_WAITS(addWorldObject, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t pointerArrayRemove(ee::Ptr<PointerArray> array, ee::Address item) { return ee::tailCall<int32_t>(kPointerArrayRemove, array, item); }
        }
    }

    // removeWorldObject.
    uint32_t removeWorldObject(ee::Address object)
    {
        return tail::pointerArrayRemove((*g_collisionWorld)->lists[kWorldObjects], object);
    }
    READABLE_FUNCTION(removeWorldObject, Verified);

    // findWorldObjectByHash: the first object whose name hashes to this (GeoObject::nameHash) that is no copy, or 0
    // (ZGE2 walks with arrayFirst / arrayNext).
    uint32_t findWorldObjectByHash(uint32_t nameHash)
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2 walks the list with arrayFirst / arrayNext)
        {
            PointerArray &objects = worldList(*g_collisionWorld, kWorldObjects);
            for (uint32_t object = firstListed(objects); object; nextListed(objects, object))
            {
                const ee::Ptr<GeoObject> geo(object);
                if (geo->nameHash == nameHash && (geo->bits & kGeoObjectCopy) == 0)
                    return object;
            }
            return 0;
        }
        PointerArray &objects = worldList(*g_collisionWorld, kWorldObjects);
        uint32_t object = walkFirst(objects);
        if (object)
        {
            for (;;)
            {
                const ee::Ptr<GeoObject> geo(object);
                if (geo->nameHash == nameHash && (geo->bits & kGeoObjectCopy) == 0)
                    return object;
                walkNext(objects, object);
                if (!object)
                    break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(findWorldObjectByHash, Verified);

    namespace
    {
        // The word with a node's in-world and sky bits: ZGE2 keeps them in the flags' upper word (in-world as its bit 9;
        // the sky bit is bit 4 in both), ZGE3 in the flags.
        uint32_t &worldFlags(Node &node)
        {
            if constexpr (game::readable::kIsDm2)
                return node.flagsHigh;
            else
                return node.flags;
        }
        constexpr uint32_t kInWorldBit = game::readable::perGame(uint32_t(kNodeInWorld), 1u << 9, uint32_t(kNodeInWorld));
    }

    // addWorldNode: a node on the world's list and in the grid; a sky node
    // is the world's sky.
    int32_t addWorldNode(ee::Ptr<Node> node)
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        worldFlags(*node) |= kInWorldBit;
        pointerArrayAdd(world->lists[kWorldNodes], node);
        if (worldFlags(*node) & kNodeSky)
            setSkyNode(node);
        return game::tail(addNodeToGrid, node, 1);
    }
    READABLE_FUNCTION_WAITS(addWorldNode, Verified);

    // removeWorldNode: out of the list and the grid.
    int32_t removeWorldNode(ee::Ptr<Node> node)
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        worldFlags(*node) &= ~kInWorldBit;
        pointerArrayRemove(world->lists[kWorldNodes], node);
        return game::tail(addNodeToGrid, node, 0);
    }
    READABLE_FUNCTION_WAITS(removeWorldNode, Verified);

    namespace
    {
        // findWorldNodeByHash's search: the nth node of a name.
        struct NodeSearch
        {
            uint32_t nameHash; // +0x00
            int32_t nth;       // +0x04 counted down
            ee::Ptr<Node> node; // +0x08 the answer
        };
        static_assert(sizeof(NodeSearch) == 0x0C);
    }

    // matchNthNodeByHash (forEachNodeInSubtree's visit): 0 stops the walk at the
    // node sought.
    uint32_t matchNthNodeByHash(ee::Ptr<Node> node, ee::Ptr<NodeSearch> search)
    {
        if (node->nameHash == search->nameHash)
        {
            search->nth = search->nth - 1;
            if (search->nth <= 0)
            {
                search->node = node;
                return 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(matchNthNodeByHash, Verified);

    // findWorldNodeByHash: the nth node of the world's list by name, or with
    // `subtrees` of their trees too. 0: none (each walk starts from the same
    // search, so nth counts through them).
    ee::Ptr<Node> findWorldNodeByHash(uint32_t nameHash, int32_t nth, uint32_t subtrees)
    {
        ee::Local<NodeSearch> search;
        PointerArray &nodes = worldList(*g_collisionWorld, kWorldNodes);
        uint32_t node = walkFirst(nodes);
        search->nameHash = nameHash;
        search->nth = nth;
        search->node = ee::Ptr<Node>();
        if (node)
        {
            for (;;)
            {
                int32_t goOn = 0;
                if (subtrees != 0)
                    goOn = forEachNodeInSubtree(ee::Ptr<Node>(node), kMatchNthNodeByHash, search);
                else
                    goOn = static_cast<int32_t>(matchNthNodeByHash(ee::Ptr<Node>(node), search));
                if (goOn == 0)
                    return search->node;
                walkNext(nodes, node);
                if (!node)
                    break;
            }
        }
        return ee::Ptr<Node>();
    }

    // findListedWorldNodeByHash: ZGE2's findWorldNodeByHash: the nth node of
    // the world's list by name, or 0; the listed nodes only (no walk of their
    // trees, so no third argument: ZGE3 added it with matchNthNodeByHash).
    ee::Ptr<Node> findListedWorldNodeByHash(uint32_t nameHash, int32_t nth)
    {
        PointerArray &nodes = worldList(*g_collisionWorld, kWorldNodes);
        for (uint32_t node = arrayFirst(&nodes); node; node = arrayNext(&nodes))
        {
            if (ee::Ptr<Node>(node)->nameHash == nameHash && --nth == -1)
                return ee::Ptr<Node>(node);
        }
        return ee::Ptr<Node>();
    }
    READABLE_FUNCTION_WAITS_OWN(findWorldNodeByHash, Verified, dm2, findListedWorldNodeByHash);

    // addWorldTexture: nothing without a world.
    void addWorldTexture(uint32_t texture)
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        if (world)
        {
            tail::pointerArrayAdd(world->lists[kWorldTextures], texture);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(addWorldTexture, Verified);

    // removeWorldTexture.
    uint32_t removeWorldTexture(uint32_t texture)
    {
        return tail::pointerArrayRemove((*g_collisionWorld)->lists[kWorldTextures], texture);
    }
    READABLE_FUNCTION(removeWorldTexture, Verified);

    // zglSceneFindTextureFromHash: the world's texture of a name, or 0.
    uint32_t zglSceneFindTextureFromHash(uint32_t nameHash)
    {
        PointerArray &textures = worldList(*g_collisionWorld, kWorldTextures);
        uint32_t texture = firstListed(textures);
        if (texture)
        {
            for (;;)
            {
                if (textureHash(ee::Ptr<Texture>(texture)) == nameHash)
                    return texture;
                nextListed(textures, texture);
                if (!texture)
                    break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(zglSceneFindTextureFromHash, Verified);

    // skyNode.
    ee::Ptr<Node> skyNode() { return (*g_collisionWorld)->skyNode; }
    READABLE_FUNCTION(skyNode, Verified);

    // requestWorldGrid.
    uint32_t requestWorldGrid()
    {
        *g_worldGridRequested = 1;
        return 1;
    }
    READABLE_FUNCTION(requestWorldGrid, Verified);

    // releaseWorldGrid.
    uint32_t releaseWorldGrid()
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        if (*g_worldGridRequested != 0)
        {
            freeWorldGrid(world);
            *g_worldGridRequested = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseWorldGrid, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sphereFromBox(ee::Address sphereOut, ee::Address box) { ee::game(kSphereFromBox, sphereOut, box); }
    }

    // buildWorldGrid: the sectors: each one's box and sphere and its empty
    // list of nodes.
    void buildWorldGrid()
    {
        CollisionWorld &world = **g_collisionWorld;
        world.cell = ee::Ptr<Sector>(heapAllocate(static_cast<uint32_t>(world.cellCount[0]) * static_cast<uint32_t>(world.cellCount[1]) *
                                                  static_cast<uint32_t>(world.cellCount[2]) * kSectorBytes));
        forEachSector(world, [&](ee::Ptr<Sector> sector, int32_t x, int32_t y, int32_t z) {
            clearSectorHead(sector);
            const ee::Ptr<Box> box = sectorBox(sector);
            box->low.x = ee::toFloat(x) * world.cellSize.x;
            box->low.y = ee::toFloat(y) * world.cellSize.y;
            box->low.z = ee::toFloat(z) * world.cellSize.z;
            box->low = box->low + world.boundsMin;
            box->high.x = ee::toFloat(x + 1) * world.cellSize.x;
            box->high.y = ee::toFloat(y + 1) * world.cellSize.y;
            box->high.z = ee::toFloat(z + 1) * world.cellSize.z;
            box->high = box->high + world.boundsMin;
            sphereFromBox(&sector->sphere, box);
            sector->nodes = newCursorArrayAs<NodeList>(kSectorNodeCapacity);
        });
    }
    READABLE_FUNCTION_WAITS(buildWorldGrid, Verified);

    // freeWorldGrid.
    void freeWorldGrid(ee::Ptr<CollisionWorld> grid)
    {
        if (!grid)
            return;
        CollisionWorld &world = *grid;
        forEachSector(world, [&](ee::Ptr<Sector> sector, int32_t, int32_t, int32_t) { freePointerArray(sector->nodes); });
        heapFree(world.cell);
    }
    READABLE_FUNCTION_WAITS(freeWorldGrid, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void pointerArrayRemoveAt(ee::Ptr<PointerArray> pointerArray, uint32_t index) { ee::game(kPointerArrayRemoveAt, pointerArray, index); }
    }

    // addToCells: the node added to (or taken out of) the lists of the
    // sectors of a range, clipped to the grid:
    //     for (x = low; x <= high; ++x) { if (x < 0) continue; if (x >= count) break; ... }
    // on each axis, which is the shape of the gotos below. What it leaves in
    uint64_t addToCells(ee::Ptr<Node> node, uint32_t add, ee::Ptr<AxisCells> cells)
    {
        if (node->flags & kNodeOutOfGrid)
            return 1;
        const CollisionWorld &world = **g_collisionWorld;
        const int32_t countX = world.cellCount[0];
        const int32_t countY = world.cellCount[1];
        const int32_t countZ = world.cellCount[2];
        // (The result callers get: the grid's height for an empty range, 1
        // when the range ended inside the grid, 0 when the grid ended first.)
        if (cells[0].high < cells[0].low)
            return static_cast<uint64_t>(static_cast<int64_t>(countY));
        for (int32_t x = cells[0].low;; ++x)
        {
            if (x >= countX)
                return 0;
            for (int32_t y = cells[1].low; x >= 0 && y <= cells[1].high && y < countY; ++y)
            {
                for (int32_t z = cells[2].low; y >= 0 && z <= cells[2].high && z < countZ; ++z)
                {
                    if (z < 0)
                        continue;
                    const ee::Ptr<PointerArray> list = sectorNodes(sectorAt(world, x, y, z));
                    if (add != 0)
                        pointerArrayAdd(list, node);
                    else
                    {
                        const int32_t index = pointerArrayFind(list, node);
                        if (index != -1)
                            pointerArrayRemoveAt(list, index);
                    }
                }
            }
            if (cells[0].high <= x)
                return 1;
        }
    }
    READABLE_FUNCTION_WAITS(addToCells, Verified);

    // moveNodeCells: a node that moved from one range of sectors to another.
    int32_t moveNodeCells(ee::Ptr<Node> node, ee::Ptr<AxisCells> from, ee::Ptr<AxisCells> to)
    {
        addToCells(node, 0, from);
        return game::tail(addToCells, node, 1, to);
    }
    READABLE_FUNCTION_WAITS(moveNodeCells, Verified);

    // addNodeToGrid: the node added to (or taken out of) the sectors its box
    // touches.
    int32_t addNodeToGrid(ee::Ptr<Node> node, uint32_t add)
    {
        ee::Local<AxisCells> cells;
        nodeGridCells(node, cells);
        return static_cast<int32_t>(addToCells(node, add, cells));
    }

    namespace
    {
        // ZGE2 keeps the node's out-of-grid bit (ZGE3's kNodeOutOfGrid) in the flags' upper word, as its bit 10.
        constexpr uint32_t kOutOfGridHigh = 1u << 10;
        // A high corner on a cell's edge belongs to the cell below it: the corner less this before the division.
        inline ee::Global<ee::Float, sym::g_cellEdgeEpsilon> g_cellEdgeEpsilon;
        // ZGE2's range of cells: the low cell on each axis, then the high one.
        struct GridCellRange
        {
            int32_t low[3];  // +0x00 x, y, z
            int32_t high[3]; // +0x0C
        };
        static_assert(sizeof(GridCellRange) == 0x18);
    }

    // addNodeToGridCells: ZGE2's addNodeToGrid, one function: the cells of the node's box worked out on the FPU,
    // ((corner - the grid's corner) / the cell size, truncated, the high corner less the cell edge's epsilon), then
    // each sector of that range within the grid has its list searched for the node, which is added (add), or taken out
    // where it was found. 1 when the range ends inside the grid (or the node is out of the grid), 0 when the grid ends
    // first. (ZGE3 splits it into nodeGridCells, on VU0, and addToCells, which searches only to take a node out.)
    inline int32_t addNodeToGridCells(ee::Ptr<Node> node, uint32_t add)
    {
        ee::Local<GridCellRange> cells;
        if (node->flagsHigh & kOutOfGridHigh)
            return 1;
        const CollisionWorld &world = **g_collisionWorld;
        const ee::Float *low = &node->boxMin.x, *high = &node->boxMax.x;
        const ee::Float *corner = &world.boundsMin.x, *size = &world.cellSize.x;
        for (int axis = 0; axis < 3; ++axis)
            cells->low[axis] = ee::toInt((low[axis] - corner[axis]) / size[axis]);
        const ee::Float edge = *g_cellEdgeEpsilon;
        for (int axis = 0; axis < 3; ++axis)
            cells->high[axis] = ee::toInt((high[axis] - corner[axis] - edge) / size[axis]);
        for (int32_t x = cells->low[0];; ++x)
        {
            if (cells->high[0] < x)
                return 1;
            if (x < 0)
                continue;
            if (x >= world.cellCount[0])
                return 0;
            for (int32_t y = cells->low[1]; y <= cells->high[1]; ++y)
            {
                if (y < 0)
                    continue;
                if (y >= world.cellCount[1])
                    break;
                for (int32_t z = cells->low[2]; z <= cells->high[2]; ++z)
                {
                    if (z < 0)
                        continue;
                    if (z >= world.cellCount[2])
                        break;
                    const ee::Ptr<PointerArray> list = sectorNodes(sectorAt(world, x, y, z));
                    const int32_t index = pointerArrayFind(list, node);
                    if (add != 0)
                        pointerArrayAdd(list, node);
                    else if (index != -1)
                        pointerArrayRemoveAt(list, index);
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS_OWN(addNodeToGrid, Verified, dm2, addNodeToGridCells);

    // assignCellRegions: each sector's region.
    void assignCellRegions()
    {
        CollisionWorld &world = **g_collisionWorld;
        forEachSector(world, [&](ee::Ptr<Sector> sector, int32_t, int32_t, int32_t) {
            sector->region = findRegion(sectorBox(sector));
        });
    }
    READABLE_FUNCTION_OWN(assignCellRegions, Verified, dm2, buildCellTree);

    namespace
    {
        inline ee::Global<ee::Ptr<CellBox>, sym::dm2::g_cellTreeNextRecord> g_cellTreeNextRecord;
        inline ee::Global<int32_t, sym::dm2::g_cellTreeRecords> g_cellTreeRecords;

        // A place a box could be cut at, and how well: cutCellBox's list.
        struct CellCut
        {
            uint8_t axis;     // CellBox::cutAxis
            uint8_t at;       // the cell plane
            uint8_t _02[2];
            ee::Float balance; // how near the box's middle the cut is (1 there), times the box's cells along the axis
        };
        static_assert(sizeof(CellCut) == 8);
        constexpr uint32_t kCellCutSlots = 64; // (cutCellBox does not test it)
        inline ee::Global<CellCut[kCellCutSlots], sym::dm2::g_cellCuts> g_cellCuts;
        inline ee::Global<int32_t, sym::dm2::g_cellCutCount> g_cellCutCount;

        constexpr uint8_t kCellTreeDepth = 6;
        constexpr float kNoBalance = -999999.0f; // below any cut's
    }

    // cutCellBox (ZGE2 only): the box cut where its halves come out most even, weighted by its size along each axis,
    // and each half cut the same way, until a box is one cell or `depth` reaches six. The second half's depth goes on
    // by two, the first's by one.
    inline void cutCellBox(ee::Ptr<CellBox> box, uint8_t depth)
    {
        if (box->high[0] == box->low[0] + 1 && box->high[1] == box->low[1] + 1 && box->high[2] == box->low[2] + 1)
            return;
        if (depth >= kCellTreeDepth)
            return;

        // Every cell plane inside the box, x's first.
        const ee::Ptr<CellCut> cuts = *g_cellCuts;
        *g_cellCutCount = 0;
        for (uint8_t axis = 1; axis <= 3; ++axis)
        {
            for (int32_t at = box->low[axis - 1] + 1; at < box->high[axis - 1]; ++at)
            {
                cuts[*g_cellCutCount].axis = axis;
                cuts[*g_cellCutCount].at = uint8_t(at);
                ++*g_cellCutCount;
            }
        }

        // The best: the cut's place over the box's middle (or the inverse, past it), times the box's extent.
        int32_t best = 0;
        ee::Float bestBalance = kNoBalance;
        for (int32_t index = 0; index < *g_cellCutCount; ++index)
        {
            CellCut &cut = cuts[index];
            const int axis = cut.axis - 1;
            cut.balance = ee::toFloat(cut.at * 2) / ee::toFloat(box->low[axis] + box->high[axis]);
            if (ee::Float(1.0f) < cut.balance)
                cut.balance = ee::Float(1.0f) / cut.balance;
            cut.balance = cut.balance * ee::toFloat(box->high[axis] - box->low[axis]);
            if (bestBalance < cut.balance)
            {
                bestBalance = cut.balance;
                best = index;
            }
        }

        // The two halves, the next records, each the box as it is.
        const CellCut &chosen = cuts[best];
        box->cutAxis = chosen.axis;
        ee::Ptr<CellBox> halves[2];
        for (int half = 0; half < 2; ++half)
        {
            const ee::Ptr<CellBox> record = *g_cellTreeNextRecord;
            halves[half] = record;
            *g_cellTreeNextRecord = record + 1;
            ++*g_cellTreeRecords;
            record->cutAxis = 0;
            record->cutAt = chosen.at;
            for (int axis = 0; axis < 3; ++axis)
                record->low[axis] = box->low[axis];
            for (int axis = 0; axis < 3; ++axis)
                record->high[axis] = box->high[axis];
            box->halves[half] = int16_t(bytesFrom(box, record) / int32_t(sizeof(CellBox)));
        }
        // Cut: the first half ends at the plane, the second starts there.
        halves[0]->high[chosen.axis - 1] = chosen.at;
        halves[1]->low[chosen.axis - 1] = chosen.at;
        cutCellBox(halves[0], uint8_t(depth + 1));
        cutCellBox(halves[1], uint8_t(depth + 2));
    }
    READABLE_FUNCTION_ONLY(dm2, cutCellBox);

    // buildCellTree: ZGE2's assignCellRegions: its cell tree made again for the world's grid.
    uint32_t buildCellTree()
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        clearSectorFlags();
        const ee::Ptr<CellBox> root = *g_cellTree;
        root->cutAxis = 0;
        for (int axis = 0; axis < 3; ++axis)
            root->low[axis] = 0;
        for (int axis = 0; axis < 3; ++axis)
            root->high[axis] = uint8_t(world->cellCount[axis]);
        *g_cellTreeRecords = 1;
        *g_cellTreeNextRecord = root + 1;
        cutCellBox(root, 0);
        return 1;
    }

    // terrainNoop.
    void terrainNoop() {}
    READABLE_FUNCTION(terrainNoop, Verified);

    namespace // the game functions below call: ee/plain.h
    {
    namespace tail
    {
        // drawNodeTree by address: it is void, and drawSky returns what it
        // leaves in v0.
        int32_t drawNodeTreeKeepingResult(ee::Ptr<Node> root, uint32_t value, uint32_t cullFlags)
        {
            return ee::tailCall<int32_t>(sym::drawNodeTree, root, value, cullFlags);
        }
    }
    }

    int32_t drawListedNode(ee::Ptr<Node> node, uint32_t data); // render/terrain.cpp (ZGE2 only)

    // drawSky: the sky, its node moved to the camera first. 0 without
    // one. ZGE2 draws it as a listed node (drawListedNode: culled, its level of detail chosen by its depth), with
    // no world matrices brought up to date here and no level set.
    int32_t drawSky()
    {
        const ee::Ptr<Node> sky = skyNode();
        if (!sky)
            return 0;
        const ee::Ptr<Matrix> camera = cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), 1);
        const ee::Ptr<Matrix> matrix = nodeMatrix(sky, 0);
        // The sky is centred on the camera.
        std::memcpy(matrix->m[3], camera->m[3], sizeof(Vec4));
        setNodeLocalMatrix(sky, matrix);
        if constexpr (game::readable::kIsDm2)
            return game::tail(drawListedNode, sky, 0);
        updateWorldMatrices(sky);
        setTreeLevelOfDetail(sky, 0);
        return tail::drawNodeTreeKeepingResult(sky, -1, 0);
    }
    READABLE_FUNCTION_WAITS(drawSky, Verified);
}
