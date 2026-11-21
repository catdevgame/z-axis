// The nodes a level's or a model's instances make (zglTerrainRender.c,
// addNewTextureToWorld ..; BMX XXX PS2 word for word): the pool of nodes (records of
// 0x200 bytes), their references (bits 13-20 of Node::drawFlags), and the
// GEO chunks that place them: TGIH per instance, TGIT and TGTD a tag table
// for a node or an object, TGFR and TGLO a frame and its levels of detail.

#include "node.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/math/vec.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/geo.h"
#include "zge/assets/txr.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/math/box.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/render/part.h"
#include "zge/platform/platform.h"
#include "zge/scene/camera.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/terrain.h"
#include "zge/scene/scene.h"
#include "zge/container/cursor_array.h"
#include "zge/container/pointer_array.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/geometry_nodes.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kSetGeometryTags = sym::setGeometryTags;       // (object, table)
        constexpr uint32_t kReleaseNode = sym::releaseNode;           // (node, family): the pool's release
        constexpr uint32_t kSetNodeTags = sym::setNodeTags;           // (node, table)
        constexpr uint32_t kFindInstanceById = sym::findInstanceById;      // (id)
        constexpr uint32_t kAddGeometryChild = sym::addGeometryChild;      // (parent, child)
        constexpr uint32_t kSphereFromBox = sym::sphereFromBox;         // (sphere, box)
        constexpr uint32_t kPadSphereRadius = sym::padSphereRadius;       // (sphere)

        constexpr uint32_t kPoolFirst = sym::poolFirst;             // callAt: identical code folds with others
        constexpr uint32_t kPoolNext = sym::poolNext;
        constexpr uint32_t kPoolRecord = sym::poolRecord;
        constexpr uint32_t kFramePartMesh = sym::framePartMesh;         // callAt, as those

        // The node pool's block (node.h kNodeBytes is the record): ZGE2 and ZGE4 keep fewer nodes a block.
        constexpr uint32_t kNodesABlock = game::readable::perGame(1800u, 0x200u, 0x280u);
        constexpr uint32_t kMovedNodesCapacity = 0x40;

        constexpr uint32_t kNodeReferenceShift = 13; // in Node::drawFlags
        constexpr uint32_t kNodeReferences = 0xFFu << kNodeReferenceShift;

        // ZGE2's reference count: the flags' low halfword (its state bits, kNodeShown and the others, are in the
        // upper word: flagsHigh); ZGE3 and ZGE4 keep it in eight bits of drawFlags.
        constexpr uint32_t kDm2NodeReferences = 0xFFFFu;
        inline uint32_t nodeReferences(const Node &node) { return node.flags & kDm2NodeReferences; }
        // The count set to `references` (kept to its bits); the count it now holds.
        inline uint32_t setNodeReferences(Node &node, uint32_t references)
        {
            references &= kDm2NodeReferences;
            node.flags = (node.flags & ~kDm2NodeReferences) | references;
            return references;
        }

        // What a node's instance takes along of the source's flags, and
        // the second word's copied bits.
        constexpr uint32_t kInstancedFlags = kNodeShown | kNodeCollides | kNodeBoneTree | kNodeSecondBucket | kNodeOwnLights | 1u << 8;
        // (ZGE4 copies the second word's low 13 bits.)
        constexpr uint32_t kInstancedSecondWord = game::readable::perGame(0xFFFFu << 13, 0xFFFFu << 13, 0x1FFFu);

        // instanceGeometryNode's flags.
        constexpr uint32_t kInstanceCopyObject = 0x0007FFFFu;

        inline ee::Global<uint32_t, sym::g_geometryNodesReady> g_geometryNodesReady;
        inline ee::Global<uint32_t, sym::g_geometryNodePool> g_geometryNodePool;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_movedNodes> g_movedNodes;
        inline ee::Global<uint32_t, sym::g_geometryChunkHandlersReady> g_geometryChunkHandlersReady;

        constexpr uint32_t kInstanceBit15 = 1u << 8;
        constexpr uint32_t kInstanceBit16 = 1u << 9;
        constexpr uint32_t kInstanceMirrored = 1u << 10;
        constexpr uint32_t kInstanceKeepsRegion = 1u << 11;
        constexpr uint32_t kNodeFlagsFromChunk = 0xFFu | 1u << 15 | 1u << 16;
        constexpr uint32_t kNodeRegionFixed = 1u << 18;

        // The tag table a chunk's whole body is: one element, records of the
        // chunk's size.
        constexpr uint32_t kTagWordIndices = 3;
        constexpr uint32_t kTagBytesShift = 4;
        constexpr uint32_t kTagBytesMask = 0x3FFu;
        constexpr uint32_t kTagOneRecord = 1u << 14;

        // What newBuffer is told about a tag table.
        struct TagTableRequest
        {
            uint32_t bits;
            uint32_t indexCount;
        };

        int32_t readTagChunk(ChunkReader *reader, ee::Address owner, uint32_t attachFunction)
        {
            ee::Local<TagTableRequest> request;
            const uint32_t bytes = chunkDataSize(reader);
            request->bits = kTagWordIndices | (bytes & kTagBytesMask) << kTagBytesShift | kTagOneRecord;
            request->indexCount = 0;
            const ee::Ptr<TagTableHeader> table = newBuffer(request);
            if (table)
            {
                readChunk(reader, table + 1, bytes);
                const ee::FunctionPtr<void(ee::Address owner, ee::Ptr<TagTableHeader> table)> attach(attachFunction);
                attach(owner, table);
                releaseTagTable(table);
            }
            return 1;
        }

        struct GeoChunkHandler
        {
            char chunk[5];
            uint32_t handler;
        };
        // The chunks of a GEO file (this file's, geo.cpp's and geo_objects.cpp's).
        const GeoChunkHandler kGeometryChunkHandlers[] = {
            {"TERR", sym::readTerrChunk}, {"TEHD", sym::readTehdChunk}, {"MODL", sym::loadGeometry}, {"ZFTX", sym::readZftxChunk}, {"ZFGE", sym::readZfgeChunk},
            {"ZFIS", sym::readZfisChunk}, {"TMTB", sym::loadMaterialTableChunk}, {"TGIN", sym::readTginChunk}, {"TGIH", sym::loadGeometryInstance}, {"TGIT", sym::readTgitChunk},
            {"TGEO", sym::loadGeoObjectChunk}, {"TGHD", sym::loadGeoHeaderChunk}, {"TGFR", sym::readTgfrChunk}, {"TGLO", sym::readTgloChunk}, {"TGCI", sym::loadCollisionTreeChunk},
            {"TGTD", sym::readTgtdChunk}, {"TGPT", sym::loadPartChunk}, {"TGPH", sym::loadPartHeaderChunk}, {"TGVP", sym::loadPositionsChunk}, {"TGVN", sym::loadNormalsChunk},
            {"TGVC", sym::loadColoursChunk}, {"TGVU", sym::loadUvsChunk}, {"TGVI", sym::loadSkinWeightsChunk}, {"TGPB", sym::loadBoneChunk}, {"TIVC", sym::loadIndexedColoursChunk},
            {"TIVN", sym::loadIndexedNormalsChunk}, {"TIVU", sym::loadIndexedUvsChunk}, {"TIDA", sym::loadIndexTableChunk}, {"TII8", sym::loadIndices8Chunk}, {"TII6", sym::loadIndices16Chunk},
            {"TGVT", sym::loadVertexTagsChunk}, {"TGFP", sym::loadFacesChunk}, {"TGFN", sym::loadFaceNormalsChunk}, {"TGFM", sym::loadStripGroupsChunk}, {"TGFE", sym::loadFaceEdgesChunk},
            {"TGFT", sym::loadFaceTagsChunk}, {"TIFN", sym::loadIndexedFaceNormalsChunk},
        };
        constexpr uint32_t kFirstHandlerReturn = points::registerGeometryChunkHandlers::afterAddChunkHandler;
        constexpr uint32_t kHandlerCallBytes = 0x18;

        // The indexed meshes' chunks (ZGE3 on) are the ones whose tags start "TI".
        constexpr bool isIndexedMeshChunk(const char *name) { return name[0] == 'T' && name[1] == 'I'; }

        uint32_t chunkId(const char *name)
        {
            return static_cast<uint8_t>(name[0]) | static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8 |
                   static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24;
        }
    }

    // addNewTextureToWorld (the texture loaded callback while a GEO file loads): a
    // texture this file brought in belongs to the world; one that was loaded
    // already does not.
    uint32_t addNewTextureToWorld(uint32_t texture, ee::Ptr<TextureChunkLocals> chunk)
    {
        if (chunk->isNew != 0)
            addWorldTexture(texture);
        return 1;
    }
    READABLE_FUNCTION_WAITS(addNewTextureToWorld, Verified);

    // readTginChunk: TGIN, a group of instances.
    int32_t readTginChunk(ChunkReader *reader, uint32_t context)
    {
        return loadObjectChunks(reader, context);
    }
    READABLE_FUNCTION_WAITS(readTginChunk, Verified);

    // readTgitChunk: TGIT, the instance's tag table.
    uint32_t readTgitChunk(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        return static_cast<uint32_t>(readTagChunk(reader, context->instance, kSetNodeTags));
    }
    READABLE_FUNCTION_WAITS(readTgitChunk, Verified);

    int32_t updateNodeBounds(ee::Ptr<Node> node);

    ee::Ptr<Node> findInstanceById(uint32_t id);

    ee::Ptr<Node> addGeometryChild(ee::Ptr<Node> parent, ee::Ptr<Node> child);

    namespace
    {
        // loadGeometryInstance's locals: the chunk as read, then what it builds.
        struct GeometryInstanceLocals
        {
            InstanceChunk chunk;
            Quaternion rotation; // +0x70
            Matrix matrix;       // +0x80
        };
        static_assert(offsetof(GeometryInstanceLocals, rotation) == 0x70 && offsetof(GeometryInstanceLocals, matrix) == 0x80);
    }

    // loadGeometryInstance: TGIH: an instance of an object: a node placed from the
    // chunk, in the world or under its parent. A mirrored one gets the
    // matrix's first column negated. The chunk's own matrix w lanes stay
    // as quaternionToMatrix's stack left them.
    uint32_t loadGeometryInstance(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        ee::Local<GeometryInstanceLocals> locals;
        const InstanceChunk &chunk = locals->chunk;
        readChunkData(reader, &locals->chunk);
        const ee::Ptr<GeoObject> object = findGeometryById(chunk.geometryId);
        uint32_t address = 0;
        if (object)
            address = newAppInstanceNode(object, 0);
        if (address == 0)
            return 0;
        const ee::Ptr<Node> node(address);
        const uint32_t bits = chunk.flags;
        node->flags = (node->flags & ~kNodeFlagsFromChunk) | (bits & 0xFFu) | (bits & kInstanceBit16 ? 1u << 16 : 0u) |
                      (bits & kInstanceBit15 ? 1u << 15 : 0u);
        const uint32_t mirrored = (bits & kInstanceMirrored) != 0 ? 1u : 0u;
        node->instanceId = chunk.instanceId;
        node->nameHash = chunk.nameHash;
        stringCopyN(node->name, chunk.name, sizeof(Node::name) - 1);
        if (chunk.region != 0)
        {
            node->region = chunk.region;
            node->flags = (node->flags & ~kNodeKeepsRegion) | (chunk.flags & kInstanceKeepsRegion ? kNodeKeepsRegion : 0u) | kNodeRegionFixed;
        }
        locals->rotation = Quaternion{chunk.rotation[0], chunk.rotation[1], chunk.rotation[2], chunk.rotation[3]};
        quaternionToMatrix(&locals->matrix, &locals->rotation);
        Matrix &matrix = locals->matrix;
        if (mirrored != 0)
        {
            const ee::Float m0 = matrix.m[0][0];
            const ee::Float m1 = -matrix.m[1][0];
            const ee::Float m2 = -matrix.m[2][0];
            matrix.m[0][0] = -m0;
            matrix.m[1][0] = m1;
            matrix.m[2][0] = m2;
        }
        matrix.m[3][0] = chunk.position[0];
        matrix.m[3][1] = chunk.position[1];
        matrix.m[3][2] = chunk.position[2];
        matrix.m[3][3] = ee::Float(1.0f);
        nodeStateFlags(*node) |= kNodeNotMoved;
        setNodeLocalMatrix(node, &locals->matrix);
        node->flags = (node->flags & ~kNodeMirrored) | (mirrored != 0 ? kNodeMirrored : 0u);
        if (node->flags & kNodeSky)
            node->flags |= kNodeOutOfGrid;
        if (chunk.parent == 0)
        {
            updateWorldMatrices(node);
            updateNodeBounds(node);
            addWorldNode(node);
        }
        else
        {
            const ee::Ptr<Node> parent = findInstanceById(chunk.parent);
            addGeometryChild(parent, node);
            const ee::Ptr<Node> root = rootNode(node);
            updateWorldMatrices(root);
            updateNodeBounds(node);
        }
        setLodTable(node, chunk.level);
        setNodeMaterialSet(node, chunk.materialSet);
        context->instance = address;
        return 1;
    }

    namespace
    {
        // ZGE2's TGIH flags, bit fields that become the node's state word (the flags' upper word): bits 0-4 stay
        // where they are, bits 5-10 move up to 7-12, the others are kept as the chunk has them.
        constexpr uint32_t kInstanceStateLow = 0x1Fu;
        constexpr uint32_t kInstanceStateMoved = 0x3Fu << 5;
        constexpr uint32_t kInstanceStateMovedShift = 2;
        constexpr uint32_t instanceStateFlags(uint32_t bits)
        {
            const uint32_t placed = kInstanceStateLow | kInstanceStateMoved << kInstanceStateMovedShift;
            return (bits & ~placed) | (bits & kInstanceStateLow) | (bits & kInstanceStateMoved) << kInstanceStateMovedShift;
        }
        // ZGE2's out-of-grid bit in the state word (ZGE3's kNodeOutOfGrid; its sky bit is kNodeSky's number).
        constexpr uint32_t kStateOutOfGrid = 1u << 10;
    }

    // loadPlacedInstance: ZGE2's loadGeometryInstance. As ZGE3's, but no instance
    // is mirrored and none carries a region: the chunk's flags, taken to the
    // node's numbering (in the chunk as read), are the node's whole state word.
    uint32_t loadPlacedInstance(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        ee::Local<GeometryInstanceLocals> locals;
        InstanceChunk &chunk = locals->chunk;
        readChunkData(reader, &locals->chunk);
        chunk.flags = instanceStateFlags(chunk.flags);
        const ee::Ptr<GeoObject> object = findGeometryById(chunk.geometryId);
        uint32_t address = 0;
        if (object)
            address = newAppInstanceNode(object, 0);
        if (address == 0)
            return 0;
        const ee::Ptr<Node> node(address);
        node->flagsHigh = chunk.flags;
        node->instanceId = chunk.instanceId;
        node->nameHash = chunk.nameHash;
        stringCopyN(node->name, chunk.name, sizeof(Node::name) - 1);
        locals->rotation = Quaternion{chunk.rotation[0], chunk.rotation[1], chunk.rotation[2], chunk.rotation[3]};
        quaternionToMatrix(&locals->matrix, &locals->rotation);
        Matrix &matrix = locals->matrix;
        matrix.m[3][0] = chunk.position[0];
        matrix.m[3][1] = chunk.position[1];
        matrix.m[3][2] = chunk.position[2];
        matrix.m[3][3] = ee::Float(1.0f);
        nodeStateFlags(*node) |= kNodeNotMoved;
        setNodeLocalMatrix(node, &locals->matrix);
        if (nodeStateFlags(*node) & kNodeSky)
            nodeStateFlags(*node) |= kStateOutOfGrid;
        if (chunk.parent == 0)
        {
            updateWorldMatrices(node);
            updateNodeBounds(node);
            addWorldNode(node);
        }
        else
        {
            const ee::Ptr<Node> parent = findInstanceById(chunk.parent);
            addGeometryChild(parent, node);
            const ee::Ptr<Node> root = rootNode(node);
            updateWorldMatrices(root);
            updateNodeBounds(node);
        }
        setLodTable(node, chunk.level);
        setNodeMaterialSet(node, chunk.materialSet);
        context->instance = address;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(loadGeometryInstance, Verified, dm2, loadPlacedInstance);

    // readTgfrChunk: TGFR, a frame on the object being loaded: the low four bits
    // of the chunk's first, third and fourth bytes go into the frame's flags.
    int32_t readTgfrChunk(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        ee::Local<uint8_t[4]> chunk;
        const uint32_t address = newCollisionFrame();
        if (address == 0)
            return 0;
        readChunkData(reader, &chunk[0]);
        const ee::Ptr<CollisionFrame> collisionFrame(address);
        collisionFrame->flags = static_cast<uint16_t>((collisionFrame->flags & ~0xFF0Fu) | (chunk[0] & 0xFu) |
                                                      (chunk[2] & 0xFu) << 8 | (chunk[3] & 0xFu) << 12);
        addModelPart(context->object, address);
        context->frame = address;
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTgfrChunk, Verified);

    // readTgtdChunk: TGTD, the object's tag table.
    uint32_t readTgtdChunk(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        return static_cast<uint32_t>(readTagChunk(reader, context->object, kSetGeometryTags));
    }
    READABLE_FUNCTION_WAITS(readTgtdChunk, Verified);

    namespace
    {
        struct LevelOfDetailChunk
        {
            ee::Float distance; // +0x00
            uint32_t _04;
            uint32_t partId;    // +0x08
        };
        // By address: a linker may fold this with a function of identical code.
        ee::Ptr<ZgePart> meshOfLevel(ee::Ptr<FramePart> level) { return framePartMesh(level); }
    }

    // readTgloChunk: TGLO, a level of detail: the part it names (its parts
    // are loaded by now) on the frame. A level after the first shares the
    // first's bones.
    uint32_t readTgloChunk(ChunkReader *reader, ee::Ptr<GeoLoadContext> context)
    {
        ee::Local<LevelOfDetailChunk> chunk;
        const uint32_t address = newFramePart();
        if (address == 0)
            return 0;
        readChunkData(reader, &*chunk);
        const ee::Ptr<FramePart> level(address);
        level->distance = chunk->distance;
        const ee::Ptr<ZgePart> part = findPartById(chunk->partId);
        level->mesh = part;
        addPartReference(part);
        const ee::Ptr<CollisionFrame> collisionFrame(context->frame);
        addFrameLevel(collisionFrame, level);
        if (frameLevels(collisionFrame) >= 2u && level->mesh->boneCount != 0)
            shareBones(meshOfLevel(frameLevelPart(collisionFrame, 0)), level->mesh);
        context->lod = address;
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTgloChunk, Verified);

    // registerGeometryChunkHandlers.
    uint32_t registerGeometryChunkHandlers()
    {
        *g_geometryChunkHandlersReady = 1;
        for (const GeoChunkHandler &handler : kGeometryChunkHandlers)
        {
            // ZGE3 added the indexed meshes' chunks (the TI* tags: TIVC .. TII6, TIFN); ZGE2 registers the others.
            if constexpr (game::readable::kIsDm2)
                if (isIndexedMeshChunk(handler.chunk))
                    continue;
            addChunkHandler(chunkId(handler.chunk), handler.handler);
            // ZGE4 reads compressed geometry too (ZLZO, after MODL) and one stream more (TGFV, after TGFE).
            if constexpr (game::readable::kIsBmx)
            {
                if (handler.handler == sym::loadGeometry)
                    addChunkHandler(chunkTag("ZLZO"), sym::bmx::readZlzoGeometryChunk);
                else if (handler.handler == sym::loadFaceEdgesChunk)
                    addChunkHandler(chunkTag("TGFV"), sym::bmx::readTgfvChunk);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerGeometryChunkHandlers, Verified);

    // resetGeometryNodeFlag.
    uint32_t resetGeometryNodeFlag()
    {
        if (*g_geometryChunkHandlersReady == 0)
            return 1;
        *g_geometryChunkHandlersReady = 0;
        return 1;
    }
    READABLE_FUNCTION(resetGeometryNodeFlag, Verified);

    // initGeometryNodes.
    uint32_t initGeometryNodes()
    {
        *g_geometryNodesReady = 1;
        *g_geometryNodePool = createPool(kNodeBytes, kNodesABlock, kReleaseNode);
        *g_movedNodes = newCursorArray(kMovedNodesCapacity);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initGeometryNodes, Verified);

    // shutdownGeometryNodes.
    uint32_t shutdownGeometryNodes()
    {
        if (*g_geometryNodesReady != 0)
        {
            destroyPool(ee::Ptr<Pool>(*g_geometryNodePool));
            const ee::Ptr<PointerArray> moved = *g_movedNodes;
            *g_geometryNodePool = 0;
            freePointerArray(moved);
            *g_movedNodes = ee::Ptr<PointerArray>();
            *g_geometryNodesReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownGeometryNodes, Verified);

    namespace
    {
        void takeOffMovedList(ee::Ptr<Node> node); // (below, with the moved list's calls)
    }

    // freeGeometryNode (run from the deferred queue: releaseDeferred): its
    // object and tags released and the record freed.
    uint32_t freeGeometryNode(ee::Ptr<Node> node)
    {
        releaseGeoObject(node->model);
        if (node->tags != 0)
            releaseTagTable(ee::Ptr<TagTableHeader>(node->tags));
        // ZGE2 takes the node off the moved list here (ZGE3's moved-list update drops a freed node itself) and has no
        // render state to free.
        if constexpr (game::readable::kIsDm2)
            takeOffMovedList(node);
        else
            freeNodeRenderState(node);
        poolFree(ee::Ptr<Pool>(*g_geometryNodePool), node);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeGeometryNode, Verified);

    namespace
    {
        // The node taken off the moved list (g_movedNodes), when it is on it (ZGE2's freeGeometryNode).
        void takeOffMovedList(ee::Ptr<Node> node)
        {
            const int32_t index = pointerArrayFind(*g_movedNodes, node);
            if (index >= 0)
                pointerArrayRemoveAt(*g_movedNodes, index);
        }
    }

    // releaseNode: a reference less, with `family` also on its children and
    // its next siblings; the last takes it out of sight, of the world and of
    // the moved list, and queues it to be freed after the frame.
    uint32_t releaseNode(ee::Ptr<Node> node, uint32_t family)
    {
        if (family != 0)
        {
            const ee::Ptr<Node> firstChild = node->firstChild;
            if (firstChild)
                releaseNode(firstChild, family);
            const ee::Ptr<Node> nextSibling = node->nextSibling;
            if (nextSibling)
                releaseNode(nextSibling, family);
        }
        // ZGE2 keeps the count in the flags' low halfword, hides the node at every release, and keeps no moved
        // list to take it off.
        if constexpr (game::readable::kIsDm2)
        {
            node->flagsHigh &= ~kNodeShown;
            if (setNodeReferences(*node, nodeReferences(*node) - 1) == 0)
            {
                if (!node->parent)
                    removeWorldNode(node);
                deferRelease(node, 0);
            }
            return 1;
        }
        const uint32_t references = ((node->drawFlags & kNodeReferences) >> kNodeReferenceShift) - 1 & 0xFFu;
        node->drawFlags = (node->drawFlags & ~kNodeReferences) | references << kNodeReferenceShift;
        if (references == 0)
        {
            node->flags &= ~kNodeShown;
            if (!node->parent)
                removeWorldNode(node);
            const int32_t moved = pointerArrayFind(*g_movedNodes, node);
            if (moved >= 0)
                pointerArrayRemoveAt(*g_movedNodes, moved);
            deferRelease(node, 0);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseNode, Verified);

    namespace
    {
        // The bits of the flags' upper half an instance takes from its original.
        void copyInstancedWord(ee::Ptr<Node> copy, ee::Ptr<Node> node)
        {
            copy->flagsHigh = (copy->flagsHigh & ~kInstancedSecondWord) | (node->flagsHigh & kInstancedSecondWord);
        }
    }

    // instanceGeometryNode: a new node like this one, on a copy of its object as
    // `flags` ask, in the world if this one has no parent, with its name, and
    // its children and next siblings with kInstanceWithFamily.
    ee::Ptr<Node> instanceGeometryNode(ee::Ptr<Node> node, uint32_t flags)
    {
        ee::Ptr<GeoObject> geometry;
        const uint32_t inWorld = node->parent ? 0u : 1u;
        uint32_t address = 0;
        if (flags & kInstanceCopyObject)
        {
            geometry = copyGeoObject(node->model, flags);
            address = newAppInstanceNode(geometry, 0);
            releaseGeoObject(geometry);
        }
        else
            address = newAppInstanceNode(node->model, 0);
        const ee::Ptr<Node> copy(address);
        copy->flags = (copy->flags & ~kInstancedFlags) | (node->flags & kInstancedFlags);
        copyInstancedWord(copy, node);
        std::memcpy(&ee::Ptr<Node>(address)->boxMin, &node->boxMin,
                    offsetof(Node, origin) - offsetof(Node, boxMin));
        copy->local = node->local;
        copy->world = node->world;
        copy->inverseWorld = node->inverseWorld;
        if (inWorld != 0)
            addWorldNode(copy);
        if (flags & kInstanceKeepName)
        {
            copy->nameHash = node->nameHash;
            stringCopy(copy->name, node->name);
        }
        copy->geometryId = node->geometryId;
        setLodTable(copy, node->drawFlags & 0xFu);
        if (node->tags != 0)
        {
            // ZGE4 can give the instance a copy of the tag table of its own (setNodeTags takes its own reference).
            if (game::readable::kIsBmx && (flags & kInstanceCopyTags))
            {
                const ee::Ptr<TagTableHeader> own = copyTagTable(ee::Ptr<TagTableHeader>(node->tags));
                setNodeTags(copy, ee::Address(own));
                releaseTagTable(own);
            }
            else
                setNodeTags(copy, node->tags);
        }
        if (!(flags & kInstanceWithFamily))
            return copy;
        const ee::Ptr<Node> firstChild = node->firstChild;
        if (firstChild)
        {
            const ee::Ptr<Node> child = instanceGeometryNode(firstChild, flags);
            copy->firstChild = child;
            for (ee::Ptr<Node> each = child; each; each = each->nextSibling)
                each->parent = copy;
        }
        const ee::Ptr<Node> nextSibling = node->nextSibling;
        if (nextSibling)
        {
            copy->nextSibling = instanceGeometryNode(nextSibling, flags);
        }
        return copy;
    }

    // copyNodeMatrices (ZGE2 only): a node's three matrices from another's
    // (local, world, inverse world: three consecutive matrices from `src`),
    // each with copyRotationAndTranslation (the original's tail call last);
    // ZGE3 copies them inline (instanceGeometryNode).
    inline uint32_t copyNodeMatrices(ee::Ptr<Matrix> dst, ee::Ptr<Matrix> src)
    {
        copyRotationAndTranslation(ee::Address(dst), ee::Address(src));
        copyRotationAndTranslation(ee::Address(dst + 1), ee::Address(src + 1));
        return game::tail(copyRotationAndTranslation, ee::Address(dst + 2), ee::Address(src + 2));
    }
    READABLE_FUNCTION_ONLY(dm2, copyNodeMatrices);

    // instanceNodeOnItsObject: ZGE2's instanceGeometryNode: always on the
    // node's own object (ZGE2 copies no object), only the flags' upper bits
    // 0-3 taken from the original (shown, the bone tree, the second bucket:
    // nodeTreeFlags), its bounds and its matrices copied (copyNodeMatrices);
    // the rest as ZGE3's.
    ee::Ptr<Node> instanceNodeOnItsObject(ee::Ptr<Node> node, uint32_t flags)
    {
        constexpr uint32_t kInstancedTreeFlags = 0xFu;
        const uint32_t inWorld = node->parent ? 0u : 1u;
        const ee::Ptr<Node> copy(newAppInstanceNode(node->model, 0));
        nodeTreeFlags(*copy) = (nodeTreeFlags(*copy) & ~kInstancedTreeFlags) | (nodeTreeFlags(*node) & kInstancedTreeFlags);
        constexpr uint32_t kBoundsBytes = 0x40; // the box, the sphere and its radius (+0xD0 .. +0x10F)
        std::memcpy(&copy->boxMin, &node->boxMin, kBoundsBytes);
        copyNodeMatrices(&copy->local, &node->local);
        if (inWorld != 0)
            addWorldNode(copy);
        if (flags & kInstanceKeepName)
        {
            copy->nameHash = node->nameHash;
            stringCopy(copy->name, node->name);
        }
        copy->geometryId = node->geometryId;
        setLodTable(copy, node->drawFlags & 0xFu);
        if (node->tags != 0)
            setNodeTags(copy, node->tags);
        if (!(flags & kInstanceWithFamily))
            return copy;
        const ee::Ptr<Node> firstChild = node->firstChild;
        if (firstChild)
        {
            const ee::Ptr<Node> child = instanceGeometryNode(firstChild, flags);
            copy->firstChild = child;
            for (ee::Ptr<Node> each = child; each; each = each->nextSibling)
                each->parent = copy;
        }
        const ee::Ptr<Node> nextSibling = node->nextSibling;
        if (nextSibling)
            copy->nextSibling = instanceGeometryNode(nextSibling, flags);
        return copy;
    }
    READABLE_FUNCTION_WAITS_OWN(instanceGeometryNode, Verified, dm2, instanceNodeOnItsObject);

    // retainNode: a reference more.
    void retainNode(ee::Ptr<Node> node)
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2's count: the flags' low halfword)
        {
            setNodeReferences(*node, nodeReferences(*node) + 1);
            return;
        }
        const uint32_t references = ((node->drawFlags & kNodeReferences) >> kNodeReferenceShift) + 1 & 0xFFu;
        node->drawFlags = (node->drawFlags & ~kNodeReferences) | references << kNodeReferenceShift;
    }
    READABLE_FUNCTION(retainNode, Verified);

    // updatedNodeVisibleInView: a moved node's tree brought up to date, then the node tested
    // against the current camera (nodeVisibleInView's flags). Nothing calls it in AI; BMX XXX
    // has the same function and calls it.
    namespace // the game functions below call: ee/plain.h
    {
        // partInView: the sphere (centre, w its radius) against the camera's view (zge/scene/camera.cpp).
        uint32_t partInView(uint32_t camera, ee::Ptr<const Vec4> sphere) { return ee::game<uint32_t>(sym::partInView, camera, sphere); }
    }

    uint32_t updatedNodeVisibleInView(ee::Ptr<Node> node)
    {
        updateMovedNode(node);
        // ZGE2 has the view test inline (it has no nodeVisibleInView): its sphere, then its box when clipped.
        if constexpr (game::readable::kIsDm2)
        {
            uint32_t flags = partInView(*g_currentCamera, &node->sphereCentre);
            if (flags & 0x8u)
                flags |= partClipFlags(ee::Ptr<Camera>(*g_currentCamera), &node->modelBounds(), &node->world);
            return flags;
        }
        return nodeVisibleInView(node);
    }
    READABLE_FUNCTION_WAITS(updatedNodeVisibleInView, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t sphereFromBox(ee::Address sphereOut, ee::Address box) { return ee::game<int32_t>(kSphereFromBox, sphereOut, box); }
        // (ZGE2's refreshNodeBounds ends in a jump to it.)
        inline int32_t tailSphereFromBox(ee::Address sphereOut, ee::Address box) { return ee::tailCall<int32_t>(kSphereFromBox, sphereOut, box); }
    }

    // refreshNodeBounds: the object's box done again from its parts and the
    // node's from that, the node out of the grid and back in meanwhile. The
    // matrix is the world's moved by the node's origin.
    int32_t refreshNodeBounds(ee::Ptr<Node> node)
    {
        ee::Local<Matrix> matrix;
        if (node->flags & kNodeInWorld)
            addNodeToGrid(node, 0);
        computeGeoObjectBox(node->model);
        *matrix = node->world;
        Vec4 &translation = matrix->translation();
        translation = translation + node->origin;
        updateNodeBox(node, matrix);
        int32_t result = sphereFromBox(&node->sphereCentre, &node->boxMin);
        if (node->flags & kNodeInWorld)
            result = addNodeToGrid(node, 1);
        return result;
    }

    // refreshNodeBoxAndSphere: ZGE2's refreshNodeBounds: the object's box done again from its parts, the
    // node's box that through the world matrix (as ZGE2's updateNodeBounds makes it) and its sphere made of the
    // node's box (sphereFromBox's result, the original's tail call). ZGE2 leaves the grid alone and adds no
    // origin.
    int32_t refreshNodeBoxAndSphere(ee::Ptr<Node> node)
    {
        computeGeoObjectBox(node->model);
        boxTransform(&node->box(), &node->modelBounds(), &node->world);
        return tailSphereFromBox(&node->sphereCentre, &node->box());
    }
    READABLE_FUNCTION_WAITS_OWN(refreshNodeBounds, Verified, dm2, refreshNodeBoxAndSphere);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t padSphereRadius(ee::Address sphere) { return ee::tailCall<int32_t>(kPadSphereRadius, sphere); }
        }
    }

    // padNodeBounds (updateNodeBounds' end): the node's box padded where it is
    // thin, and its sphere's radius. A function of its own in both engines,
    // which updateNodeBounds jumps to: ZGE2's build keeps it apart, ZGE3's has
    // it inside updateNodeBounds, its only caller.
    inline int32_t padNodeBounds(ee::Ptr<Node> node)
    {
        padThinBox(&node->box());
        return tail::padSphereRadius(&node->sphereCentre);
    }
    READABLE_FUNCTION_ONLY(dm2, padNodeBounds);

    // updateNodeBounds: from the object's box and the world matrix; the sphere
    // is made of the object's box, in the object's space, not the node's.
    int32_t updateNodeBounds(ee::Ptr<Node> node)
    {
        // ZGE2 transforms the model's bounds into the box itself (boxTransform), with no margin; ZGE3 asks updateNodeBox,
        // which adds g_nodeBoxMargin.
        if constexpr (game::readable::kIsDm2)
            boxTransform(&node->box(), &node->modelBounds(), &node->world);
        else
            updateNodeBox(node, &node->world);
        sphereFromBox(&node->sphereCentre, &node->modelBounds());
        if constexpr (game::readable::kIsDm2)
            return game::tail(padNodeBounds, node); // (ZGE2's original jumps to its own function)
        padThinBox(&node->box());
        return tail::padSphereRadius(&node->sphereCentre);
    }
    READABLE_FUNCTION(updateNodeBounds, Verified);

    // nodeName.
    ee::Ptr<char> nodeName(ee::Ptr<Node> node) { return node->name; }
    READABLE_FUNCTION(nodeName, Verified);

    // nodeNameHash.
    uint32_t nodeNameHash(ee::Ptr<Node> node) { return node->nameHash; }
    READABLE_FUNCTION(nodeNameHash, Verified);

    // setNodeNameHash: the name hash the node is found by (Node::nameHash).
    void setNodeNameHash(ee::Ptr<Node> node, uint32_t nameHash) { node->nameHash = nameHash; }
    READABLE_FUNCTION(setNodeNameHash, Verified);

    // setNodeTags: a reference to the table in place of its old one.
    ee::Ptr<TagTableHeader> setNodeTags(ee::Ptr<Node> node, ee::Address table)
    {
        // DM2 (a bug it keeps) releases the table it is given, not the one it
        // replaces: the new table's count goes down and back up, the old one
        // keeps the node's reference.
        if (node->tags != 0)
            releaseTagTable(ee::Ptr<TagTableHeader>(game::readable::kIsDm2 ? static_cast<uint32_t>(table) : node->tags));
        node->tags = table;
        return game::tail(addTagTableReference, ee::Ptr<TagTableHeader>(table));
    }
    READABLE_FUNCTION_WAITS(setNodeTags, Verified);

    // setNodeGeometry: another object on the node (the old one released); 0
    // takes it off.
    void setNodeGeometry(ee::Ptr<Node> node, uint32_t object)
    {
        if (node->model)
            releaseGeoObject(node->model);
        node->model = ee::Ptr<GeoObject>(object);
        if (object != 0)
        {
            node->geometryId = ee::Ptr<GeoObject>(object)->id;
            retainGeoObject(node->model);
            game::tail(updateNodeBounds, node); return;
        }
        node->geometryId = 0;
    }
    READABLE_FUNCTION_WAITS(setNodeGeometry, Verified);

    // objectClass: the node's model (its GeoObject: the class its object is
    // made from), 0 for none.
    ee::Ptr<GeoObject> objectClass(ee::Ptr<Node> node) { return node->model; }
    READABLE_FUNCTION(objectClass, Verified);

    // findInstanceById: the node of an instance id, or 0.
    ee::Ptr<Node> findInstanceById(uint32_t id)
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(ee::Ptr<Pool>(*g_geometryNodePool)); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Node> node = poolRecordAs<Node>(link);
            if (node->instanceId == id)
                return node;
        }
        return ee::Ptr<Node>();
    }
    READABLE_FUNCTION(findInstanceById, Verified);

    // addGeometryChild: at the head of the parent's children. The old first
    // child is its result.
    ee::Ptr<Node> addGeometryChild(ee::Ptr<Node> parent, ee::Ptr<Node> child)
    {
        const ee::Ptr<Node> oldFirst = parent->firstChild;
        child->nextSibling = oldFirst;
        parent->firstChild = child;
        child->parent = parent;
        return oldFirst;
    }
    READABLE_FUNCTION(addGeometryChild, Verified);
}
