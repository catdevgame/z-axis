// The level's geometry objects (zglTerrainRender.c, initGeometryPool ..; BMX XXX PS2
// word for word): GeoObjects in a pool, each with its parts (a level of
// detail's CollisionFrames) and a tag table; the two ways a GEO file is
// read (a whole level's TERR chunk, a model's) and the callback that keeps
// the materials the file's table holds. The chunks' own handlers are geo.cpp.

#include "geo.h"
#include "game_address.h"
#include "zge/math/box.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/images.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/container/pointer_array.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"
#include "zge/render/material.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/render/texture.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/geo_objects.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kNewGeoObject = sym::newGeoObject;              // ()
        constexpr uint32_t kReleaseGeoObject = sym::releaseGeoObject;          // (object): the pool's and the object nodes' release
        constexpr uint32_t kSetGeometryTags = sym::setGeometryTags;           // (object, table)
        constexpr uint32_t kAddModelPart = sym::addModelPart;              // (object, frame)
        constexpr uint32_t kClassModel = sym::classModel;                // (object, index) -> the part, 0 past the end
        constexpr uint32_t kModelPartCount = sym::modelPartCount;            // (object) -> parts
        constexpr uint32_t kReleaseObjectGeometry = sym::releaseObjectGeometry;     // (node): loadGeometry's visit
        constexpr uint32_t kStoreMaterial = sym::storeMaterial;             // the material callback
        constexpr uint32_t kAddNewTextureToWorld = sym::addNewTextureToWorld;           // the texture loaded callback
        constexpr uint32_t kCloneTagTable = sym::copyTagTable;             // (table) -> a copy
        constexpr uint32_t kAddTagTableReference = sym::addTagTableReference;      // (table)
        constexpr uint32_t kBoxUnion = sym::boxUnion;                  // (out, a, b)
        constexpr uint32_t kGeometryChunkHandlersReady = sym::g_geometryChunkHandlersReady; // what loadGeometry keeps in s2

        constexpr uint32_t kPoolFirst = sym::poolFirst;             // callAt: identical code folds with others
        constexpr uint32_t kPoolNext = sym::poolNext;
        constexpr uint32_t kPoolRecord = sym::poolRecord;

        constexpr uint32_t kGeoObjectsABlock = 0x200;
        constexpr uint32_t kFirstPartCapacity = 4;

        // copyGeoObject's flags: parts to copy (cloneCollisionFrame reads
        // them), the tag table too.
        constexpr uint32_t kCopyParts = 0x00027FFFu;
        constexpr uint32_t kCopyTags = 0x00040000u;

        constexpr uint32_t kTextureIsTheWorlds = 1u << 1; // kTextureFlagsByte
        constexpr int32_t kMaterialTextureNames = game::readable::perGame(2, 4, 1); // storeMaterial's texture slots

        inline ee::Global<uint32_t, sym::g_geometryPoolReady> g_geometryPoolReady;
        inline ee::Global<uint32_t, sym::g_geometryPool> g_geometryPool;
        inline ee::Global<uint32_t, sym::g_geoLoadInstance> g_geoLoadInstance; // GeoLoadContext::instance

        constexpr uint32_t kGeoLoadContext = sym::g_geoLoadContext;
    }

    // initGeometryPool.
    uint32_t initGeometryPool()
    {
        *g_geometryPoolReady = 1;
        *g_geometryPool = createPool(sizeof(GeoObject), kGeoObjectsABlock, kReleaseGeoObject);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initGeometryPool, Verified);

    // shutdownGeometryPool.
    uint32_t shutdownGeometryPool()
    {
        if (*g_geometryPoolReady != 0)
        {
            destroyPool(ee::Ptr<Pool>(*g_geometryPool));
            *g_geometryPool = 0;
            *g_geometryPoolReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownGeometryPool, Verified);

    // newGeoObject: an object with one reference, in the world.
    ee::Ptr<GeoObject> newGeoObject()
    {
        const ee::Ptr<GeoObject> object(poolAlloc(ee::Ptr<Pool>(*g_geometryPool)));
        if (object)
        {
            memoryFill(object, 0, sizeof(GeoObject));
            object->references = 1;
        }
        addWorldObject(object);
        return object;
    }
    READABLE_FUNCTION_WAITS(newGeoObject, Verified);

    // releaseGeoObject: a reference less; the last one frees its parts, tags,
    // part list and record.
    uint32_t releaseGeoObject(ee::Ptr<GeoObject> object)
    {
        int32_t endOfParts = 0;
        object->references -= 1;
        if (object->references != 0)
            return 1;
        const int32_t parts = modelPartCount(object);
        int32_t part = parts - 1;
        if (parts != 0)
        {
            endOfParts = -1;
            for (;;)
            {
                const ee::Ptr<CollisionFrame> frame = classModel(object, part);
                --part;
                releaseCollisionFrame(frame);
                if (part == endOfParts)
                    break;
            }
        }
        if (object->tags)
            releaseTagTable(object->tags);
        if (object->parts != 0)
            freePointerArray(ee::Ptr<PointerArray>(object->parts));
        if (*g_collisionWorld)
            removeWorldObject(object);
        poolFree(ee::Ptr<Pool>(*g_geometryPool), object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseGeoObject, Verified);

    void setGeometryTags(ee::Ptr<GeoObject> object, ee::Ptr<TagTableHeader> table);

    // copyGeoObject: a new object like this one: its parts shared, or
    // cloned as `flags` ask, its tags too or a reference.
    ee::Ptr<GeoObject> copyGeoObject(ee::Ptr<GeoObject> object, uint32_t flags)
    {
        const ee::Ptr<GeoObject> copy = newGeoObject();
        copy->bits |= kGeoObjectCopy;
        memoryCopy(&copy->boundsMin, &object->boundsMin, sizeof(GeoObject::boundsMin) + sizeof(GeoObject::boundsMax));
        stringCopy(copy->name, object->name);
        copy->nameHash = object->nameHash;
        const bool ownParts = (flags & kCopyParts) != 0;
        for (int32_t part = modelPartCount(object) - 1; part != -1; --part)
        {
            ee::Ptr<CollisionFrame> frame = classModel(object, part);
            if (ownParts)
                frame = cloneCollisionFrame(frame, flags);
            addModelPart(copy, frame);
        }
        ee::Ptr<TagTableHeader> tags = object->tags;
        if (tags)
        {
            const bool ownTags = (flags & kCopyTags) != 0;
            if (ownTags)
                tags = copyTagTable(tags);
            setGeometryTags(copy, tags);
            // DM2 releases the table it set whether it copied it or not: one
            // reference less on the source object's own table when it shares it.
            if ((ownTags || game::readable::kIsDm2) && tags)
                releaseTagTable(tags);
        }
        return copy;
    }
    READABLE_FUNCTION_WAITS(copyGeoObject, Verified);

    // retainGeoObject.
    int32_t retainGeoObject(ee::Ptr<GeoObject> object)
    {
        object->references += 1;
        return object->references;
    }
    READABLE_FUNCTION(retainGeoObject, Verified);

    // addGeoObject: its parts' packets built, from the last.
    int32_t addGeoObject(ee::Ptr<GeoObject> object)
    {
        int32_t result = modelPartCount(object);
        int32_t part = result - 1;
        if (result != 0)
        {
            for (;;)
            {
                const ee::Ptr<CollisionFrame> frame = classModel(object, part);
                --part;
                result = buildFramePackets(frame);
                if (part == -1)
                    break;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(addGeoObject, Verified);

    namespace
    {
        // The geometry pool's records, as a walk of its links.
        uint32_t firstGeometryLink() { return poolFirst(ee::Ptr<PoolList>(*g_geometryPool)); }
        uint32_t nextGeometryLink(uint32_t link) { return poolNext(ee::Ptr<PoolLink>(link)); }
        ee::Ptr<GeoObject> geometryAt(uint32_t link) { return poolRecordAs<GeoObject>(link); }
    }

    // findGeometryById: the pool's object with this id, or 0.
    ee::Ptr<GeoObject> findGeometryById(uint32_t id)
    {
        for (uint32_t link = firstGeometryLink(); link != 0; link = nextGeometryLink(link))
        {
            const ee::Ptr<GeoObject> object = geometryAt(link);
            if (object->id == id)
                return object;
        }
        return ee::Ptr<GeoObject>();
    }
    READABLE_FUNCTION(findGeometryById, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void boxUnion(ee::Address box, ee::Address box2, ee::Address box3) { ee::game(kBoxUnion, box, box2, box3); }
    }

    // computeGeoObjectBox: its bounds, all its parts' bounds together.
    void computeGeoObjectBox(ee::Ptr<GeoObject> object)
    {
        ee::Local<uint8_t[0x60]> partBox;
        const ee::Address box(&object->boundsMin);
        for (int32_t part = 0; part < static_cast<int32_t>(object->bits & kGeoObjectPartCount); ++part)
        {
            const ee::Ptr<CollisionFrame> frame = classModel(object, part);
            if (part == 0)
                collisionFrameBounds(frame, box);
            else
            {
                collisionFrameBounds(frame, partBox);
                boxUnion(box, box, partBox);
            }
        }
    }
    READABLE_FUNCTION(computeGeoObjectBox, Verified);

    // classTags.
    ee::Ptr<TagTableHeader> classTags(ee::Ptr<GeoObject> object) { return object->tags; }
    READABLE_FUNCTION(classTags, Verified);

    // setGeometryTags: a reference to the table in place of its old one. In
    // ZGE2 and ZGE3 the release is of the new table, not the old: the old one
    // keeps its reference (bug kept).
    void setGeometryTags(ee::Ptr<GeoObject> object, ee::Ptr<TagTableHeader> table)
    {
        if (object->tags)
        {
            // ZGE4 releases the old table, as the description says; ZGE2 and ZGE3 the new one.
            if constexpr (game::readable::kIsBmx)
                releaseTagTable(object->tags);
            else
                releaseTagTable(table);
        }
        object->tags = table;
        game::tail(addTagTableReference, table);
    }
    READABLE_FUNCTION_WAITS(setGeometryTags, Verified);

    // addModelPart: the result is the count's bits.
    uint64_t addModelPart(ee::Ptr<GeoObject> object, ee::Address frame)
    {
        if (object->parts == 0)
            object->parts = ee::Address(newPointerArray(kFirstPartCapacity));
        pointerArrayAdd(ee::Ptr<PointerArray>(object->parts), frame);
        const uint16_t count = static_cast<uint16_t>(pointerArrayCount(ee::Ptr<PointerArray>(object->parts))) & kGeoObjectPartCount;
        object->bits = static_cast<uint16_t>((object->bits & ~kGeoObjectPartCount) | count);
        return static_cast<uint64_t>(count) << 16;
    }
    READABLE_FUNCTION_WAITS(addModelPart, Verified);

    // classModel.
    ee::Ptr<CollisionFrame> classModel(ee::Ptr<GeoObject> object, uint32_t index)
    {
        return ee::Ptr<CollisionFrame>(pointerArrayAt(ee::Ptr<PointerArray>(object->parts), static_cast<int32_t>(index)));
    }
    READABLE_FUNCTION(classModel, Verified);

    // modelPartCount.
    int32_t modelPartCount(ee::Ptr<GeoObject> object)
    {
        return static_cast<int32_t>(pointerArrayCount(ee::Ptr<PointerArray>(object->parts)));
    }
    READABLE_FUNCTION(modelPartCount, Verified);

    // geoObjectBounds (ZGE2 only): where the object's box is, its least
    // corner then its most (boundsMin, boundsMax); ZGE3 reads the fields
    // in place.
    inline ee::Ptr<Float4> geoObjectBounds(ee::Ptr<GeoObject> object)
    {
        return &object->boundsMin;
    }
    READABLE_FUNCTION_ONLY(dm2, geoObjectBounds);

    // readTerrChunk: TERR, a whole level's geometry.
    int32_t readTerrChunk(ChunkReader *reader, uint32_t owner)
    {
        const uint32_t previous = setTextureLoadedCallback(kAddNewTextureToWorld);
        // ZGE2 keeps the level's materials through storeMaterial too, as loadGeometry does.
        uint32_t previousMaterial = 0;
        if constexpr (game::readable::kIsDm2)
            previousMaterial = setMaterialCallback(kStoreMaterial);
        // ZGE4 makes the smaller levels of the level's TZMP textures.
        uint32_t previousMipmaps = 0;
        if constexpr (game::readable::kIsBmx)
            previousMipmaps = setImageMipmaps(1);
        memoryFill(kGeoLoadContext, 0, kGeoLoadContextBytes);
        g_geoLoadContext->owner = owner;
        if (enterChunk(reader) == 0)
            return 0;
        parseChunks(reader, kGeoLoadContext);
        leaveChunk(reader);
        setupWorldMeshModels();
        if constexpr (game::readable::kIsBmx)
            setImageMipmaps(previousMipmaps);
        setTextureLoadedCallback(previous);
        if constexpr (game::readable::kIsDm2)
            setMaterialCallback(previousMaterial);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTerrChunk, Verified);

    // releaseObjectGeometry (loadGeometry's visit): the file's reference to the
    // node's object dropped. 1: the walk goes on.
    uint32_t releaseObjectGeometry(ee::Ptr<Node> node)
    {
        releaseGeoObject(nodeClass(node));
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseObjectGeometry, Verified);

    // loadGeometry: a model's file: its chunks read, its nodes' objects
    // handed over to them and the model made.
    int32_t loadGeometry(ChunkReader *reader, ee::Ptr<ee::Ptr<Scene>> modelOut)
    {
        const uint32_t previousLoaded = setTextureLoadedCallback(kAddNewTextureToWorld);
        const uint32_t previousMaterial = setMaterialCallback(kStoreMaterial);
        memoryFill(kGeoLoadContext, 0, kGeoLoadContextBytes);
        g_geoLoadContext->owner = 0;
        if (enterChunk(reader) == 0)
            return 0;
        parseChunks(reader, kGeoLoadContext);
        leaveChunk(reader);
        const ee::Ptr<Node> root = rootNode(ee::Ptr<Node>(*g_geoLoadInstance));
        forEachNodeInSubtree(root, kReleaseObjectGeometry, 0);
        *modelOut = setupGeometrySceneModel(root, 0);
        setTextureLoadedCallback(previousLoaded);
        setMaterialCallback(previousMaterial);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadGeometry, Verified);

    namespace
    {
        // A TEHD chunk.
        struct TerrainHeader
        {
            ee::Float cellSize[3], low[3], high[3]; // +0x00
        };
        static_assert(sizeof(TerrainHeader) == 0x24 && alignof(Vec4) == 16 && alignof(Box) == 16);
    }

    // readTehdChunk: TEHD: the world (with w 1 on each vector) made.
    int32_t readTehdChunk(ChunkReader *reader, uint32_t context)
    {
        ee::Local<TerrainHeader> header;
        ee::Local<Vec4> cellSize;
        ee::Local<Box> bounds;
        readChunkData(reader, header);
        const auto point = [](const ee::Float (&from)[3]) { return Vec4{from[0], from[1], from[2], ee::Float(1.0f)}; };
        cellSize = point(header->cellSize);
        bounds->low = point(header->low);
        bounds->high = point(header->high);
        const ee::Ptr<CollisionWorld> world = newCollisionWorld(cellSize, bounds);
        if (!world)
            return 0;
        ee::Ptr<GeoLoadContext>(context)->world = ee::Address(world);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTehdChunk, Verified);

    // readZftxChunk: ZFTX, a file of textures.
    int32_t readZftxChunk(ChunkReader *reader, uint32_t context)
    {
        return includeChunkFile(reader, context);
    }
    READABLE_FUNCTION_WAITS(readZftxChunk, Verified);

    namespace
    {
        inline ee::Global<uint32_t, sym::dm2::g_keepAllNormals> g_keepAllNormals; // (ZGE2: geo.cpp's partIgnoresStream)
    }

    // readZfgeChunk: ZFGE, a file of geometry.
    int32_t readZfgeChunk(ChunkReader *reader, uint32_t context)
    {
        const int32_t read = includeChunkFile(reader, context);
        if constexpr (game::readable::kIsDm2) // (ZGE2 stops keeping every part's normals after it)
            *g_keepAllNormals = 0;
        return read;
    }
    READABLE_FUNCTION_WAITS(readZfgeChunk, Verified);

    // readZfisChunk: ZFIS, a file of instances.
    int32_t readZfisChunk(ChunkReader *reader, uint32_t context)
    {
        return includeChunkFile(reader, context);
    }
    READABLE_FUNCTION_WAITS(readZfisChunk, Verified);

    // storeMaterial (the material callback while a GEO file loads): the
    // material into the table at the context's next slot (the table's
    // reference replaces the loader's), and its two textures found by their
    // names' hashes: the world's own, else a loaded one that keeps its name.
    // The first has one added even when it has no name.
    uint32_t storeMaterial(ee::Ptr<Material> material, ee::Ptr<GeoLoadContext> context)
    {
        replaceTableMaterial(context->materials, material, context->materialIndex);
        context->materialIndex += 1;
        releaseMaterial(material);
        // A material names up to two textures (ZGE2's four, ZGE4's one).
        for (int32_t index = 0; index < kMaterialTextureNames; ++index)
        {
            const int32_t nameHash = materialTextureSlot(material, index);
            if (nameHash == 0)
            {
                if (index == 0)
                    addMaterialTexture(material, 0);
                continue;
            }
            uint32_t texture = zglSceneFindTextureFromHash(nameHash);
            if constexpr (!game::readable::kIsDm2) // (ZGE2 looks among the world's textures only)
            {
                if (texture == 0)
                {
                    texture = findLoadedTextureByHash(nameHash);
                    if (texture != 0 && (textureFlagsByte(ee::Ptr<Texture>(texture)) & kTextureIsTheWorlds) == 0)
                        texture = 0;
                }
            }
            addMaterialTexture(material, texture);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(storeMaterial, Verified);
}
