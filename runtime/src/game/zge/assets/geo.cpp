#include "geo.h"

#include "zge/collision/collision.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/assets/game_address.h"
#include "zge/assets/tag_table.h"
#include "zge/math/box.h"
#include "zge/memory/regions.h"
#include "zge/native/collision_data.h"
#include "zge/native/geo_meshes.h"
#include "zge/native/host.h"
#include "zge/assets/chunk_reader.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/fixed_point.h"
#include "zge/text/cstring.h"

#include <cstring>
#include "sym/zge/assets/geo.cpp.h"

namespace zge
{
    namespace
    {
        // TIDA's table, read as what it holds.
        template <typename T, typename Context>
        ee::Ptr<T> indexTableAs(Context *ctx) { return ctx->indexTable.template cast<T>(); }

        // Game functions the handlers call.
        constexpr uint32_t kNewGeoObject = sym::newGeoObject;        // () -> GeoObject
        constexpr uint32_t kAddGeoObject = sym::addGeoObject;        // (object) once its chunks are loaded
        constexpr uint32_t kLoadObjectChunks = sym::loadObjectChunks;    // (reader, ctx): enter, parse, leave (or load the file it names)
        constexpr uint32_t kPartIgnoresStream = sym::partIgnoresStream;   // (part, kind) -> non-zero: not needed
        constexpr uint32_t kNewBuffer = sym::newBuffer;           // (&descriptor) -> buffer object
        constexpr uint32_t kReleaseBuffer = sym::releaseTagTable;       // (buffer)
        constexpr uint32_t kStoreMaterial = sym::storeMaterial;       // TMTB's callback: (material, ctx)

        // A new stream on the current part; returns its data.
        template <typename T = uint8_t>
        T *newStream(GeoLoadContext *ctx, StreamKind kind, uint32_t bytes)
        {
            return ee::Ptr<T>(zge::newStream(ctx->part, kind, bytes, 0));
        }

        // ZGE2's test of the normals, which it makes here (ZGE3 asks partIgnoresStream): a part keeps its vertex
        // normals when it has bones or while the game keeps them all (set around a level's geometry files; readZfgeChunk
        // clears it), and its face normals always.
        inline ee::Global<uint32_t, sym::dm2::g_keepAllNormals> g_keepAllNormals;

        bool partIgnoresStream(GeoLoadContext *ctx, StreamKind kind)
        {
            if constexpr (game::readable::kIsDm2)
                return kind == StreamKind::Normals && ctx->part->boneCount == 0 && *g_keepAllNormals == 0;
            else
                return ee::call<int32_t>(kPartIgnoresStream, ctx->part, kind) != 0;
        }

        // A block of main memory for what is read from a chunk.
        template <typename T>
        ee::Ptr<T> allocateBlock(uint32_t bytes) { return ee::Ptr<T>(allocateMemory(bytes)); }
        void callMaterialLoaded(uint32_t callback, ee::Ptr<Material> material, void *context) { ee::game(callback, material, context); }
        // A bone record's header, in front of the chunk's body.
        constexpr uint32_t kBoneRecordHeaderBytes = 0x20u;

        // A stream that is the chunk's body as it is.
        int32_t loadPlainStream(ChunkReader *reader, GeoLoadContext *ctx, StreamKind kind)
        {
            const uint32_t bytes = chunkDataSize(reader);
            uint8_t *data = newStream(ctx, kind, bytes);
            readChunkData(reader, data);
            return 1;
        }

        // The children of the chunk being handled (TIDA, TII8, TII6 for an
        // indexed stream; the part's chunks for a TGPT).
        int32_t parseChildren(ChunkReader *reader, void *ctx)
        {
            return parseChunks(reader, ctx);
        }

        // An indexed stream: its table and indices from the children, then
        // made into one stream.

        int32_t loadIndexedStream(ChunkReader *reader, GeoLoadContext *ctx, StreamKind kind)
        {
            if (!enterChunk(reader))
                return 0;
            parseChildren(reader, ctx);
            leaveChunk(reader);
            buildIndexedStream(ctx, kind);
            return 1;
        }

        // UV floats to the s16 the VIF unpacks (ITOF12: 4096 = 1.0), with
        // the EE's rounding (multiply towards zero, CVT.W.S truncating).
        int16_t uvFixed(ee::Float value)
        {
            return static_cast<int16_t>(fixedUv(value));
        }
    }

    // ---- objects -------------------------------------------------------

    int32_t loadGeoObjectChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // TGHD makes the object (ctx->object); once all of the object's
        // chunks are in, it joins the engine's objects.
        const int32_t ok = loadObjectChunks(reader, ctx);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 adds no object here and keeps ctx->object)
        {
            if (ok)
                addGeoObject(ctx->object);
            ctx->object = ee::Ptr<GeoObject>();
        }
        return ok;
    }
    READABLE_FUNCTION_WAITS(loadGeoObjectChunk, Verified);

    int32_t loadGeoHeaderChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        ee::Local<GeoHeaderChunk> header;
        const ee::Ptr<GeoObject> object = newGeoObject();
        if (!object)
            return 0;
        readChunkData(reader, header);
        object->boundsMin = {header->boundsMin.x, header->boundsMin.y, header->boundsMin.z, 1.0f};
        object->boundsMax = {header->boundsMax.x, header->boundsMax.y, header->boundsMax.z, 1.0f};
        padThinBox(&object->bounds());
        object->id = header->id;
        object->nameHash = header->nameHash;
        stringCopyN(object->name, header->name, static_cast<uint32_t>(sizeof(object->name)));
        ctx->object = object;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadGeoHeaderChunk, Verified);

    // ---- parts -----------------------------------------------------------

    int32_t loadPartChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const ee::Ptr<ZgePart> previous = ctx->part;
        if (!enterChunk(reader))
            return 0;
        // Host side (not under ps2xFnCheck, which has only the memory the
        // original touched): where in the file this part comes from.
        const uint32_t chunkStart = native::hostExtrasOn() ? reader->frame->start : 0u;
        const int32_t ok = parseChildren(reader, ctx);
        leaveChunk(reader);
        if constexpr (game::readable::kIsDm2) // (ZGE2 clears a part's render state once its chunks are in; ZGE3 in newPart)
            clearRenderState(ctx->part);
        if (native::hostExtrasOn() && ctx->part.get() != previous.get())
            if (const ZFile *file = reader->file)
                native::notePart(native::gameRam(), ctx->part, std::string(file->path, strnlen(file->path, sizeof(file->path))),
                                 chunkStart);
        // A skinned part (TGPH set it as ctx->part) does not stay the
        // current part: it is linked in after the part before it, sharing
        // that part's bone matrices, and the part before it becomes
        // current again.
        const ee::Ptr<ZgePart> part = ctx->part;
        if (part->flags & kPartSkinned)
        {
            attachSkinnedPart(previous, part);
            ctx->part = previous;
        }
        return ok;
    }
    READABLE_FUNCTION_WAITS(loadPartChunk, Verified);

    namespace
    {
        // The flags a part takes from its TGPH: ZGE2 every bit.
        constexpr uint32_t kPartFlagsTaken = game::readable::perGame(uint32_t(kPartFlagsFromDisc), 0xFFFFu, uint32_t(kPartFlagsFromDisc));
    }

    int32_t loadPartHeaderChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        ee::Local<PartHeaderChunk> header;
        const ee::Ptr<ZgePart> part = newPart();
        if (!part)
            return 0;
        readChunkData(reader, header);
        part->header = 0;
        part->flags |= header->flags & kPartFlagsTaken;
        part->vertexCount = header->vertexCount;
        part->faceCount = header->faceCount;
        part->reserved[0] = header->reserved;
        part->boneCount = header->boneCount;
        part->groupCount = header->groupCount;
        part->materialCount = header->materialCount;
        part->id = header->id;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 sets no render class here)
            setPartRenderClass(part, static_cast<uint32_t>(header->flags & 7u));
        if (!(part->flags & kPartSkinned))
            dropPartReference(part); // what holds a rigid part takes its own reference
        ctx->part = part;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadPartHeaderChunk, Verified);

    // ---- indexed streams -------------------------------------------------

    int32_t buildIndexedStream(GeoLoadContext *ctx, StreamKind kind)
    {
        // One stream: the table, then the indices (one per vertex).
        uint8_t *data = newStream(ctx, kind, ctx->indexTableBytes + ctx->indexBytes);
        memoryCopy(data, ctx->indexTable, ctx->indexTableBytes);
        ZgeStream *stream = ZgeStream::fromData(data);
        stream->indices = ee::Ptr<uint8_t>(data) + ctx->indexTableBytes;
        if (ctx->indices8)
        {
            stream->wideIndices = 0;
            memoryCopy(stream->indices, ctx->indices8, ctx->indexBytes);
            freeMemory(ctx->indices8);
            ctx->indices8 = ee::Ptr<uint8_t>();
        }
        else
        {
            stream->wideIndices = 1;
            memoryCopy(stream->indices, ctx->indices16, ctx->indexBytes);
            freeMemory(ctx->indices16);
            ctx->indices16 = ee::Ptr<uint8_t>();
        }
        freeMemory(ctx->indexTable);
        ctx->indexTable = ee::Ptr<uint8_t>();
        return 1;
    }
    READABLE_FUNCTION(buildIndexedStream, Verified);

    int32_t loadIndexTableChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        ctx->indexTableBytes = bytes;
        ctx->indexTable = allocateBlock<uint8_t>(bytes);
        readChunkData(reader, ctx->indexTable.get());
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadIndexTableChunk, Verified);

    int32_t loadIndices8Chunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        ctx->indexBytes = bytes;
        ctx->indices8 = allocateBlock<uint8_t>(bytes);
        readChunkData(reader, ctx->indices8.get());
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadIndices8Chunk, Verified);

    int32_t loadIndices16Chunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        ctx->indexBytes = bytes;
        ctx->indices16 = allocateBlock<uint8_t>(bytes);
        readChunkData(reader, ctx->indices16.get());
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadIndices16Chunk, Verified);

    // ---- vertex streams --------------------------------------------------

    int32_t loadPositionsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // float3 on disc, float4 (w = 1) in RAM, for VU1's 128-bit loads.
        const uint32_t bytes = chunkDataSize(reader);
        const uint32_t count = ctx->part->vertexCount;
        Float4 *positions = newStream<Float4>(ctx, StreamKind::Positions, count * static_cast<uint32_t>(sizeof(Float4)));
        const ee::Ptr<Vec3> file = allocateBlock<Vec3>(bytes);
        readChunkData(reader, file);
        for (uint32_t i = 0; i < count; ++i)
            positions[i] = {file[i].x, file[i].y, file[i].z, 1.0f};
        freeMemory(file);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadPositionsChunk, Verified);

    int32_t loadNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // Only parts drawn with normals keep them.
        if (!partIgnoresStream(ctx, StreamKind::Normals))
        {
            loadPlainStream(reader, ctx, StreamKind::Normals);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadNormalsChunk, Verified);

    int32_t loadIndexedNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        if (partIgnoresStream(ctx, StreamKind::Normals))
            return 1;
        return loadIndexedStream(reader, ctx, StreamKind::Normals);
    }
    READABLE_FUNCTION_WAITS(loadIndexedNormalsChunk, Verified);

    int32_t loadColoursChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadPlainStream(reader, ctx, StreamKind::Colours);
    }
    READABLE_FUNCTION_WAITS(loadColoursChunk, Verified);

    int32_t loadIndexedColoursChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadIndexedStream(reader, ctx, StreamKind::Colours);
    }
    READABLE_FUNCTION_WAITS(loadIndexedColoursChunk, Verified);

    int32_t loadUvsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2 keeps the file's float pairs as they are)
            return loadPlainStream(reader, ctx, StreamKind::Uvs);
        // float2 on disc, s16 pairs in RAM.
        const uint32_t bytes = chunkDataSize(reader);
        int16_t *uvs = newStream<int16_t>(ctx, StreamKind::Uvs, ctx->part->vertexCount * 4u);
        const ee::Ptr<ee::Float> file = allocateBlock<ee::Float>(bytes);
        readChunkData(reader, file);
        for (uint32_t i = 0; i < ctx->part->vertexCount; ++i)
        {
            uvs[2 * i] = uvFixed(file[2 * i]);
            uvs[2 * i + 1] = uvFixed(file[2 * i + 1]);
        }
        freeMemory(file);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadUvsChunk, Verified);

    int32_t loadIndexedUvsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        if (!enterChunk(reader))
            return 0;
        // (The children go to the loader's global context, the same object
        // as ctx in every GEO load.)
        parseChildren(reader, g_geoLoadContext.get());
        leaveChunk(reader);
        // The table's floats become s16 in place, halving it.
        const ee::Ptr<ee::Float> floats = indexTableAs<ee::Float>(ctx);
        const ee::Ptr<int16_t> fixed = indexTableAs<int16_t>(ctx);
        const uint32_t count = ctx->indexTableBytes / 4u;
        for (uint32_t i = 0; i < count; ++i)
        {
            fixed[i] = uvFixed(floats[i]); // fixed[i] lies at or below floats[i]: read before overwritten
        }
        ctx->indexTableBytes /= 2u;
        buildIndexedStream(ctx, StreamKind::Uvs);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadIndexedUvsChunk, Verified);

    int32_t loadSkinWeightsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadPlainStream(reader, ctx, StreamKind::SkinWeights);
    }
    READABLE_FUNCTION_WAITS(loadSkinWeightsChunk, Verified);

    int32_t loadBoneChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // A bone record on the part's bone list: the chunk's size plus a
        // header, of which only the header is filled here.
        ee::Local<BoneChunkHeader> header;
        const uint32_t bytes = chunkDataSize(reader);
        readChunk(reader, header, static_cast<uint32_t>(sizeof(BoneChunkHeader)));
        // ZGE4 keeps the header alone: a record of 0x20 bytes, whatever the chunk's size.
        const uint32_t recordBytes = game::readable::kIsBmx ? kBoneRecordHeaderBytes : bytes + kBoneRecordHeaderBytes;
        BoneRecord *bone = newStream<BoneRecord>(ctx, StreamKind::Bones, recordBytes);
        bone->bytes = recordBytes;
        bone->poseIndex = header->poseIndex;
        bone->offset = {header->offset.x, header->offset.y, header->offset.z, 1.0f};
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadBoneChunk, Verified);

    // ---- faces and strips --------------------------------------------------

    int32_t loadFacesChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        uint8_t *faces = newStream(ctx, StreamKind::Faces, bytes);
        readChunkData(reader, faces);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadFacesChunk, Verified);

    int32_t loadFaceNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        if (!partIgnoresStream(ctx, StreamKind::FaceNormals))
        {
            uint8_t *normals = newStream(ctx, StreamKind::FaceNormals, bytes);
            readChunkData(reader, normals);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadFaceNormalsChunk, Verified);

    int32_t loadIndexedFaceNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        if (partIgnoresStream(ctx, StreamKind::FaceNormals))
            return 1;
        return loadIndexedStream(reader, ctx, StreamKind::FaceNormals);
    }
    READABLE_FUNCTION_WAITS(loadIndexedFaceNormalsChunk, Verified);

    int32_t loadStripGroupsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        const uint32_t bytes = chunkDataSize(reader);
        uint8_t *groups = newStream(ctx, StreamKind::Groups, bytes);
        readChunkData(reader, groups);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadStripGroupsChunk, Verified);

    // ---- buffer streams (TGFE, TGFT, TGVT) ---------------------------------

    int32_t loadBufferStreamChunk(ChunkReader *reader, GeoLoadContext *ctx, StreamKind kind)
    {
        ee::Local<BufferChunkHeader> header;
        const uint32_t bytes = chunkDataSize(reader);
        readChunk(reader, header, static_cast<uint32_t>(sizeof(BufferChunkHeader)));
        const uint32_t bodyBytes = bytes - static_cast<uint32_t>(sizeof(BufferChunkHeader));
        // The RAM descriptor has no bit 3.
        header->descriptor &= ~0x8u;
        const ee::Ptr<TagTableHeader> table = newBuffer(&header->descriptor);
        if (!table)
            return 0;
        if constexpr (game::readable::kIsBmx)
            tagDescriptorBytes(&table->descriptor); // (ZGE4 asks the table's size and drops it)
        readChunk(reader, table + 1, bodyBytes);
        // PS2X_NATIVE_COLLISION: the host mirrors a part's face tags and
        // edges (zge/native/collision_data.h); this is the table it mirrors.
        if ((kind == StreamKind::FaceTags || kind == StreamKind::FaceEdges) && native::hostExtrasOn() &&
            native::nativeCollisionEnabled())
            native::noteTagTable(table, ctx->part,
                                 kind == StreamKind::FaceTags ? native::TagTableKind::FaceTags
                                                              : native::TagTableKind::FaceEdges,
                                 bodyBytes);
        attachBuffer(ctx->part, kind, table);
        releaseTagTable(table);
        if (kind == StreamKind::VertexTags)
            ctx->vertexTagBytes += bodyBytes;
        else if (kind == StreamKind::FaceTags)
            ctx->faceTagBytes += bodyBytes;
        else if (kind == StreamKind::FaceEdges)
            ctx->faceEdgeBytes += bodyBytes;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadBufferStreamChunk, Verified);

    int32_t loadFaceEdgesChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadBufferStreamChunk(reader, ctx, StreamKind::FaceEdges);
    }
    READABLE_FUNCTION_WAITS(loadFaceEdgesChunk, Verified);

    int32_t loadFaceTagsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadBufferStreamChunk(reader, ctx, StreamKind::FaceTags);
    }
    READABLE_FUNCTION_WAITS(loadFaceTagsChunk, Verified);

    int32_t loadVertexTagsChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        return loadBufferStreamChunk(reader, ctx, StreamKind::VertexTags);
    }
    READABLE_FUNCTION_WAITS(loadVertexTagsChunk, Draft); // no TGVT chunk in the Tutorial: never recorded

    // ---- collision ---------------------------------------------------------

    // checkFaceTree: a walk over the whole tree that does nothing but take
    // time (an empty loop over each leaf's faces): debug code left in. The
    // greater side by a call of itself, the lesser side by going on.
    namespace
    {
        // A face tree's node, read as a split (a leaf's axis says it is one).
        ee::Ptr<FaceTreeSplit> splitAt(ee::Ptr<uint8_t> node) { return node.cast<FaceTreeSplit>(); }
    }

    void checkFaceTree(ee::Ptr<uint8_t> tree, ee::Ptr<uint8_t> node)
    {
        for (;;)
        {
            const ee::Ptr<FaceTreeSplit> split = splitAt(node);
            if (split->axis == kFaceTreeLeaf || split->axis > kFaceTreeSplitZ)
                return;
            checkFaceTree(tree, tree + 4 * split->greater);
            node = tree + 4 * split->lesser;
        }
    }
    READABLE_FUNCTION(checkFaceTree, Verified);

    int32_t loadCollisionTreeChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // TGCI: {u32 part id, u32 bytes, the face tree} for the object's
        // collision frame (TGFR made it, ctx->frame); the tree is kept as
        // it is on disc.
        const ee::Ptr<CollisionFrame> frame(ctx->frame);
        ee::Local<uint32_t> partId;
        ee::Local<uint32_t> bytes;
        readChunkU32(reader, partId);
        readChunkU32(reader, bytes);
        frame->tree = heapAllocateAs<uint8_t>(*bytes);
        frame->treeBytes = *bytes;
        readChunk(reader, frame->tree.get(), *bytes);
        checkFaceTree(frame->tree, frame->tree);
        // The collision part: the loaded part with that id.
        frame->part = findPartById(*partId);
        addPartReference(frame->part);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadCollisionTreeChunk, Verified);

    // ---- materials ---------------------------------------------------------

    int32_t loadMaterialTableChunk(ChunkReader *reader, GeoLoadContext *ctx)
    {
        // A table with a slot per MATI; while the MATIs load, each one is
        // handed to storeMaterial, which stores it in the next slot.
        const uint32_t count = chunkDataSize(reader) / kMatiChunkBytes;
        const ee::Ptr<MaterialTable> table = newMaterialTable(count);
        table->count = static_cast<uint16_t>(count);
        attachMaterialTable(ctx->part, table);
        ctx->materials = table;
        ctx->materialIndex = 0;
        // (ZGE2 sets storeMaterial once for the whole file: readTerrChunk.)
        uint32_t previousCallback = 0;
        if constexpr (!game::readable::kIsDm2)
            previousCallback = setMaterialCallback(kStoreMaterial);
        if (enterChunk(reader))
        {
            parseChildren(reader, ctx);
            leaveChunk(reader);
        }
        if constexpr (!game::readable::kIsDm2)
            setMaterialCallback(previousCallback);
        ctx->materials = ee::Ptr<MaterialTable>();
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadMaterialTableChunk, Verified);

    int32_t loadMaterialChunk(ChunkReader *reader, void *context)
    {
        ee::Local<MatiChunk> mati;
        readChunkData(reader, mati);
        const ee::Ptr<Material> material = newMaterial();
        if (!material)
            return 0;
        loadMaterial(material, mati);
        if (const uint32_t callback = *g_materialLoadedCallback)
            callMaterialLoaded(callback, material, context);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadMaterialChunk, Verified);
}
