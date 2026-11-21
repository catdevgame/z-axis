#pragma once

// GEO files: the ZGE3 engine's level and model geometry.
// registerGeometryChunkHandlers registers a chunk handler per tag; the parser
// (zge/assets/chunk_reader.h) calls them with the GEO loader's context:
//
//   FORM
//     TGEO                 an object: TGHD header, TGTD/TGFR/TGLO/TGCI, then
//       TGPT               parts: TGPH header, then one chunk per stream
//         TGVP TGVN TGVC TGVU TGVI     plain streams
//         TIVN TIVC TIVU TIFN          indexed: {TIDA table, TII8 | TII6 indices}
//         TGFP TGFN TGFM               faces, face normals, strip groups
//         TGPB TGFE TGFT TGVT          bones, buffer streams
//       TMTB { MATI ... }  the part's materials
//
// The part and stream layouts are in zge/render/part.h, materials in
// zge/render/material.h.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/math/types.h"
#include "zge/math/box.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "sym/zge/assets/geo.h.h"

namespace zge
{
    // ---- on disc ---------------------------------------------------------

    // TGHD (64 bytes): the object's bounds and name.
    struct GeoHeaderChunk
    {
        Vec3 boundsMin;     // +0x00
        Vec3 boundsMax;     // +0x0C
        uint32_t id;        // +0x18 what findGeometryById looks for
        char name[32];      // +0x1C
        // +0x3C nameHash of the object's whole name. name keeps only its first 31 characters, so a longer name's
        // hash is not name's (PropDef_vert-truss_short_carplant, Editobj_bridges_SMALLbridgecorner).
        uint32_t nameHash;
    };
    static_assert(sizeof(GeoHeaderChunk) == 0x40 && offsetof(GeoHeaderChunk, name) == 0x1C);

    // TGPH (20 bytes): a part's header.
    struct PartHeaderChunk
    {
        uint16_t flags;          // +0x00 bits 0-2: render class (setPartRenderClass), kPartFlagsFromDisc
        uint16_t _02;      // +0x02
        uint16_t vertexCount;    // +0x04
        uint16_t faceCount;      // +0x06
        uint16_t groupCount;     // +0x08
        uint8_t reserved;        // +0x0A to part +0x3A (0 in every PS2 part; nothing reads it)
        uint8_t pad0B;
        uint8_t boneCount;       // +0x0C
        uint8_t pad0D;
        uint8_t materialCount;   // +0x0E submeshes
        uint8_t pad0F;
        uint32_t id;             // +0x10 to ZgePart::id (TGCI names the collision part by it)
    };
    static_assert(sizeof(PartHeaderChunk) == 20 && offsetof(PartHeaderChunk, materialCount) == 0x0E);

    // TGPB's first 16 bytes: a bone. [inferred]
    struct BoneChunkHeader
    {
        uint16_t poseIndex;
        uint16_t pad02;
        Vec3 offset;
    };
    static_assert(sizeof(BoneChunkHeader) == 16);

    // The first 8 bytes of a TGFE / TGFT / TGVT chunk: the descriptor of
    // the buffer object (newBuffer) the rest is read into. [inferred
    // field names]
    struct BufferChunkHeader
    {
        uint32_t descriptor; // bits 0-2 kind, bit 3 (dropped in RAM), bits 4-13 size, 14-31 ...
        uint32_t count;      // an element count (only an empty debug loop reads it)
    };
    static_assert(sizeof(BufferChunkHeader) == 8);

    // A MATI chunk's bytes on disc, header included (TMTB's size / this is
    // the material count).
    constexpr uint32_t kMatiChunkBytes = sizeof(ChunkHeader) + sizeof(MatiChunk);
    // (ZGE2's MATI record is laid out only as far as its loaders read it: zge/render/material.h)
    static_assert(game::readable::kIsDm2 || kMatiChunkBytes == 0xA8);

    // ---- in RAM ----------------------------------------------------------

    struct TagTableHeader; // (zge/assets/tag_table.h)

    // An object (TGEO; newGeoObject: 0x60 bytes, zeroed, one reference;
    // zge/assets/geo_objects.cpp).
    struct GeoObject
    {
        uint16_t references;     // +0x00
        uint16_t bits;           // +0x02 bits 0-3 the parts' count (kGeoObjectPartCount), kGeoObjectCopy
        uint32_t parts;          // +0x04 a PointerArray of CollisionFrames, 0 before the first
#if ZX_GAME_BMX
        // ZGE4 keeps the tag table at +0x0C and moves id, name and nameHash 4 bytes down (loadGeoHeaderChunk,
        // copyGeoObject, findGeometryById, releaseGeoObject, setGeometryTags); the record is still 0x60 bytes.
        uint8_t _08[0x04];
        ee::Ptr<TagTableHeader> tags; // BMX +0x0C
        Float4 boundsMin;        // +0x10 w = 1
        Float4 boundsMax;        // +0x20 w = 1
        uint32_t id;             // BMX +0x30
        char name[32];           // BMX +0x34
        uint32_t nameHash;       // BMX +0x54
        uint8_t _58[0x08];
#else
        uint8_t _08[0x08];
        Float4 boundsMin;        // +0x10 w = 1
        Float4 boundsMax;        // +0x20 w = 1
        ee::Ptr<TagTableHeader> tags; // +0x30 its tag table (zge/assets/tag_table.h), 0: none
        uint32_t id;             // +0x34 TGHD id
        char name[32];           // +0x38
        // +0x58 TGHD nameHash: the hash of the object's whole name, what findWorldObjectByHash and a dynamics
        // definition (Dynamics::nameHash) know it by.
        uint32_t nameHash;
        uint32_t _5c;      // +0x5C
#endif

        // boundsMin and boundsMax as one box.
        Box &bounds() { return *reinterpret_cast<Box *>(&boundsMin); }
    };
    static_assert(sizeof(GeoObject) == 0x60 && offsetof(GeoObject, name) == game::readable::perGame(0x38u, 0x38u, 0x34u));
    static_assert(offsetof(GeoObject, boundsMin) == 0x10 && offsetof(GeoObject, boundsMax) == 0x20);
    constexpr uint16_t kGeoObjectPartCount = 0x000F;
    constexpr uint16_t kGeoObjectCopy = 1u << 4; // made by copyGeoObject: findWorldObjectByHash passes it by

    // The GEO loader's context (g_geoLoadContext), every handler's second
    // argument. Objects, parts and the material table being loaded, and the
    // pieces of an indexed stream until it is built.
    struct GeoLoadContext
    {
        union
        {
            struct
            {
                uint32_t owner;         // +0x00 readTerrChunk's owner (0 for loadGeometry)
                uint32_t world;         // +0x04 TEHD: the collision world
            };
            uint32_t _00[2];            // (the old name, until geometry_nodes.cpp's lane takes the two above)
        };
        uint32_t instance;                    // +0x08 the node TGIH made: TGIT's tags go on it
        ee::Ptr<GeoObject> object;            // +0x0C TGEO / TGHD
        uint32_t frame;                       // +0x10 TGFR: the object's collision frame (zge/collision/collision.h)
        uint32_t lod;                         // +0x14 TGLO: the FramePart it added (nothing reads it)
        ee::Ptr<ZgePart> part;                // +0x18 TGPH
        ee::Ptr<MaterialTable> materials;     // +0x1C TMTB, while its MATIs load
        uint32_t materialIndex;               // +0x20 next slot in it (storeMaterial)
#if ZX_GAME_DM2
        // ZGE2's context is 0x30 bytes: no indexed streams, the buffer streams' counts at +0x24 (loadBufferStreamChunk;
        // loadGeometry clears 0x30 bytes).
        uint32_t faceEdgeBytes;               // DM2 +0x24
        uint32_t faceTagBytes;                // DM2 +0x28
        uint32_t vertexTagBytes;              // DM2 +0x2C
        // [unplaced: ZGE2 has no TIDA, TII8, TII6 or TIV* chunks; AI's fields stand past the context so the shared
        // readers compile, and no DM2 function touches them]
        ee::Ptr<uint8_t> indexTable;
        uint32_t indexTableBytes;
        ee::Ptr<uint8_t> indices8;
        ee::Ptr<uint8_t> indices16;
        uint32_t indexBytes;
#else
        ee::Ptr<uint8_t> indexTable;          // +0x24 TIDA's table
        uint32_t indexTableBytes;             // +0x28
        ee::Ptr<uint8_t> indices8;            // +0x2C TII8
        ee::Ptr<uint8_t> indices16;           // +0x30 TII6
        uint32_t indexBytes;                  // +0x34
        uint32_t faceEdgeBytes;               // +0x38 TGFE bodies read
        uint32_t faceTagBytes;                // +0x3C TGFT
        uint32_t vertexTagBytes;              // +0x40 TGVT
#endif
    };
    static_assert(offsetof(GeoLoadContext, part) == 0x18 &&
                  offsetof(GeoLoadContext, vertexTagBytes) == game::readable::perGame(0x40u, 0x2Cu, 0x40u));
    // The bytes of the context the loaders clear (ZGE2's ends at +0x30).
    constexpr uint32_t kGeoLoadContextBytes =
        game::readable::perGame(static_cast<uint32_t>(sizeof(GeoLoadContext)), 0x30u,
                                static_cast<uint32_t>(sizeof(GeoLoadContext)));
    inline ee::Global<GeoLoadContext, sym::g_geoLoadContext> g_geoLoadContext;

    // The callback a loaded material is handed to (setMaterialCallback sets it;
    // TMTB: storeMaterial, which stores it in the table).
    inline ee::Global<uint32_t, sym::g_materialLoadedCallback> g_materialLoadedCallback;

    // ---- handlers (each returns non-zero to go on) -------------------------

    int32_t loadGeoObjectChunk(ChunkReader *reader, GeoLoadContext *ctx);       // TGEO loadGeoObjectChunk
    int32_t loadGeoHeaderChunk(ChunkReader *reader, GeoLoadContext *ctx);       // TGHD loadGeoHeaderChunk
    int32_t loadPartChunk(ChunkReader *reader, GeoLoadContext *ctx);            // TGPT loadPartChunk
    int32_t loadPartHeaderChunk(ChunkReader *reader, GeoLoadContext *ctx);      // TGPH loadPartHeaderChunk
    int32_t buildIndexedStream(GeoLoadContext *ctx, StreamKind kind);
    int32_t loadIndexTableChunk(ChunkReader *reader, GeoLoadContext *ctx);      // TIDA loadIndexTableChunk
    int32_t loadIndices8Chunk(ChunkReader *reader, GeoLoadContext *ctx);        // TII8 loadIndices8Chunk
    int32_t loadIndices16Chunk(ChunkReader *reader, GeoLoadContext *ctx);       // TII6 loadIndices16Chunk
    int32_t loadPositionsChunk(ChunkReader *reader, GeoLoadContext *ctx);       // TGVP loadPositionsChunk
    int32_t loadNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx);         // TGVN loadNormalsChunk
    int32_t loadIndexedNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx);  // TIVN loadIndexedNormalsChunk
    int32_t loadColoursChunk(ChunkReader *reader, GeoLoadContext *ctx);         // TGVC loadColoursChunk
    int32_t loadIndexedColoursChunk(ChunkReader *reader, GeoLoadContext *ctx);  // TIVC loadIndexedColoursChunk
    int32_t loadUvsChunk(ChunkReader *reader, GeoLoadContext *ctx);             // TGVU loadUvsChunk
    int32_t loadIndexedUvsChunk(ChunkReader *reader, GeoLoadContext *ctx);      // TIVU loadIndexedUvsChunk
    int32_t loadSkinWeightsChunk(ChunkReader *reader, GeoLoadContext *ctx);     // TGVI loadSkinWeightsChunk
    int32_t loadBoneChunk(ChunkReader *reader, GeoLoadContext *ctx);            // TGPB loadBoneChunk
    int32_t loadFacesChunk(ChunkReader *reader, GeoLoadContext *ctx);           // TGFP loadFacesChunk
    int32_t loadFaceNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx);     // TGFN loadFaceNormalsChunk
    int32_t loadIndexedFaceNormalsChunk(ChunkReader *reader, GeoLoadContext *ctx); // TIFN loadIndexedFaceNormalsChunk
    int32_t loadStripGroupsChunk(ChunkReader *reader, GeoLoadContext *ctx);     // TGFM loadStripGroupsChunk
    int32_t loadBufferStreamChunk(ChunkReader *reader, GeoLoadContext *ctx, StreamKind kind);
    int32_t loadFaceEdgesChunk(ChunkReader *reader, GeoLoadContext *ctx);       // TGFE loadFaceEdgesChunk
    int32_t loadFaceTagsChunk(ChunkReader *reader, GeoLoadContext *ctx);        // TGFT loadFaceTagsChunk
    int32_t loadCollisionTreeChunk(ChunkReader *reader, GeoLoadContext *ctx);   // TGCI loadCollisionTreeChunk
    // checkFaceTree: walks a face tree from `node` (zge/collision/collision.h); only takes time.
    void checkFaceTree(ee::Ptr<uint8_t> tree, ee::Ptr<uint8_t> node);
    int32_t loadVertexTagsChunk(ChunkReader *reader, GeoLoadContext *ctx);      // TGVT loadVertexTagsChunk
    int32_t loadMaterialTableChunk(ChunkReader *reader, GeoLoadContext *ctx);   // TMTB loadMaterialTableChunk
    int32_t loadMaterialChunk(ChunkReader *reader, void *context);              // MATI loadMaterialChunk
}

namespace zge
{
    struct CollisionFrame;
    // (geo_objects.cpp)
    ee::Ptr<GeoObject> newGeoObject();
    uint32_t releaseGeoObject(ee::Ptr<GeoObject> object);
    ee::Ptr<GeoObject> copyGeoObject(ee::Ptr<GeoObject> object, uint32_t flags);
    int32_t retainGeoObject(ee::Ptr<GeoObject> object);
    int32_t addGeoObject(ee::Ptr<GeoObject> object);
    ee::Ptr<GeoObject> findGeometryById(uint32_t id);
    void computeGeoObjectBox(ee::Ptr<GeoObject> object);
    ee::Ptr<TagTableHeader> classTags(ee::Ptr<GeoObject> object);
    uint64_t addModelPart(ee::Ptr<GeoObject> object, ee::Address frame);
    ee::Ptr<CollisionFrame> classModel(ee::Ptr<GeoObject> object, uint32_t index);
    int32_t modelPartCount(ee::Ptr<GeoObject> object);
    // geoObjectBounds (ZGE2 only, geo_objects.cpp): the object's box, its least corner then its most.
    ee::Ptr<Float4> geoObjectBounds(ee::Ptr<GeoObject> object);
}
