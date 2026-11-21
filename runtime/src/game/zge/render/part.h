#pragma once

// Geometry of the ZGE3 engine in RAM: parts (meshes) and their streams
// (runtime/ps2_zaxis_mesh.h reads the same on the host).
//
// A part is the run-time form of a GEO file's TGPT chunk: TGPH fills the
// header, then one chunk per stream (positions, UVs, colours, normals,
// faces, strip groups, bones ...) allocates the stream on the part
// (newStream) and reads it. Indexed streams (TIVC/TIVN/TIVU/TIFN) are a
// table followed by one 8- or 16-bit index per vertex (buildIndexedStream). The
// VIF packet VU1 draws is built from the streams on the part's first draw.
//
// Fields marked [inferred] are named from how the loaders fill them, not
// from a use that proves the meaning.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"

namespace zge
{
    struct PointerArray; // zge/container/array.h
    struct TagTableHeader; // zge/assets/tag_table.h
    struct Node;           // zge/scene/node.h
    struct MaterialTable;  // zge/render/material.h
    struct LightBlock;     // zge/scene/light.h
    struct Vec3;           // zge/math/types.h
    struct Vec4;           // zge/math/types.h

    // Four floats with no alignment promise (zge::Vec4 is alignas(16)):
    // stream data and records the loaders fill one field at a time.
    struct Float4
    {
        ee::Float x, y, z, w;
    };
    static_assert(sizeof(Float4) == 16 && alignof(Float4) == 4);

    // A vertex's colour as the colours stream holds it: RGBA, a byte a
    // channel (0x80 is full intensity).
    struct VertexColour
    {
        uint8_t red, green, blue, alpha;
    };
    static_assert(sizeof(VertexColour) == 4);

    // What a stream holds, and the part field it hangs on (newStream's
    // jump table).
    enum class StreamKind : uint32_t
    {
        Positions = 0,   // +0x04 TGVP: float4 per vertex (w = 1)
        Faces = 1,       // +0x08 TGFP: 8-byte face records (collision)
        FaceNormals = 2, // +0x0C TGFN / TIFN
        Normals = 3,     // +0x10 TGVN / TIVN: float3 table (+ indices)
        Uvs = 4,         // +0x14 TGVU / TIVU: s16 pairs, 4096 = 1.0
        Colours = 5,     // +0x18 TGVC / TIVC: RGBA8 table (+ indices)
        Bones = 6,       // +0x1C TGPB: a bone record appended to the part's bone list
        ShadowEdges = 7, // +0x2C built by buildPartEdges
        SkinWeights = 8, // +0x20 TGVI: 8 bytes per vertex {bone, weight << 4} x 4
        Groups = 9,      // +0x24 TGFM: strip groups
        FaceTags = 10,   // TGFT [inferred name] (loadBufferStreamChunk, attachBuffer)
        VertexTags = 11, // TGVT [inferred name]
        FaceEdges = 12,  // TGFE [inferred name]
        MaterialSets = 13, // +0x28 a PointerArray of MaterialTables (attachMaterialTable adds one, meshMaterials picks one)
    };
    constexpr uint32_t kStreamKinds = 14;

    // A stream: a 16-byte header, the data at +0x10 (newStream returns
    // the data's address).
    struct ZgeStream
    {
        uint8_t type;                // +0x00 7 (the engine's object type; ZGE2's 6)
        uint8_t wideIndices;         // +0x01 indexed streams: 1 u16 indices, 0 u8
        uint16_t refCount;           // +0x02 parts sharing it (addStreamReference adds one, releaseStream frees it with the last)
        uint32_t bytes;              // +0x04 the allocation, header included
        ee::Ptr<uint8_t> indices;    // +0x08 indexed streams: the indices, after the table
        uint32_t _0c;          // +0x0C
        // +0x10 data

        uint8_t *data() { return reinterpret_cast<uint8_t *>(this + 1); }
        static ZgeStream *fromData(void *data) { return ee::at<ZgeStream>(ee::addressOf(data) - 0x10u); }
        // The stream a part's field holds (the header's address).
        static ZgeStream &of(ee::Ptr<uint8_t> field) { return *field.cast<ZgeStream>(); }
    };
    static_assert(sizeof(ZgeStream) == 0x10 && offsetof(ZgeStream, indices) == 0x08);

    // A stream's records, after its header.
    template <typename T>
    inline ee::Ptr<T> streamRecords(ee::Ptr<uint8_t> stream) { return (stream + sizeof(ZgeStream)).cast<T>(); }

    // Part flags (ZgePart::flags).
    enum : uint16_t
    {
        kPartFlagsFromDisc = 0x3800, // the bits TGPH takes from the file
        kPartNew = 0x0400,           // set by newPart [inferred: streams not built yet]
        kPartSkinned = 0x0800,       // skinned (or the skater's shadow); TGPT hands it to attachSkinnedPart
        kPartLitByNormals = 0x1000,  // has normals; no prelit colours needed
    };

    // A part (newPart: 0x68 bytes, zeroed, refCount 1, flags 0x400).
    struct PacketMaterial;

    struct ZgePart
    {
        uint16_t header;                   // +0x00 type 0 (TGPH writes it as a halfword)
        uint16_t refCount;                 // +0x02
        // The streams: each field holds the stream's ZgeStream header, its
        // data at +0x10 (as the packet builders read them).
        ee::Ptr<uint8_t> positions;        // +0x04 (StreamKind::Positions)
        ee::Ptr<uint8_t> faces;            // +0x08
        ee::Ptr<uint8_t> faceNormals;      // +0x0C
        ee::Ptr<uint8_t> normals;          // +0x10
        ee::Ptr<uint8_t> uvs;              // +0x14
        ee::Ptr<uint8_t> colours;          // +0x18
        uint32_t bones;                    // +0x1C bone list (newCursorArray), skinned parts
        ee::Ptr<uint8_t> skinWeights;      // +0x20
        ee::Ptr<uint8_t> groups;           // +0x24 strip groups (StripGroup at +0x10); freed once the packet is built
        ee::Ptr<PointerArray> materialSets; // +0x28 the material tables (StreamKind::MaterialSets), one a material set
        ee::Ptr<uint8_t> shadowEdges;      // +0x2C
        ee::Ptr<ZgePart> next;             // +0x30
        uint16_t vertexCount;              // +0x34
        uint16_t faceCount;                // +0x36
        uint16_t groupCount;               // +0x38
        // +0x3A TGPH's reserved byte (+0x0A; 0 in every one of the PS2
        // disc's 9,333 parts), +0x3B only ever copied by clonePart: no code
        // in AI or DM2 (the same layout, its TGPH handler)
        // reads either.
        uint8_t reserved[2];
        uint8_t boneCount;                 // +0x3C
        uint8_t materialCount;             // +0x3D submeshes
        uint16_t flags;                    // +0x3E kPart*; low bits: streams to rebuild
        uint32_t id;                       // +0x40 TGPH +0x10: what TGCI names its collision part by (findPartById finds it)
        // Buffer streams (attachBuffer attaches them; the data at +0x10 of
        // each): face tags (TGFT, StreamKind::FaceTags), vertex tags (TGVT),
        // face edges (TGFE).
        union
        {
            ee::Ptr<uint8_t> faceTags;     // +0x44
            ee::Ptr<TagTableHeader> faceTagTable;
        };
        union
        {
            ee::Ptr<uint8_t> vertexTags;   // +0x48
            ee::Ptr<TagTableHeader> vertexTagTable;
        };
        union
        {
            ee::Ptr<uint8_t> faceEdges;    // +0x4C
            ee::Ptr<TagTableHeader> faceEdgeTable;
        };
        // Two packet buffers (double buffered: one drawn while the other is
        // rebuilt), each built by buildPartPacket as one allocation: the VIF
        // packet, then its material table (PacketMaterial per material),
        // then a word set to 1.
        ee::Ptr<PacketMaterial> materialTables[2]; // +0x50 the material table of each buffer
        ee::Ptr<uint32_t> packets[2];        // +0x58 each buffer's packet (0: not built)
        uint32_t typeWord;                   // +0x60 kTypeWord* fields
        uint32_t bufferFlags;                // +0x64 kBuffer* fields
    };
    static_assert(sizeof(ZgePart) == 0x68);
    static_assert(offsetof(ZgePart, positions) == 0x04 && offsetof(ZgePart, groups) == 0x24 &&
                  offsetof(ZgePart, vertexCount) == 0x34 && offsetof(ZgePart, boneCount) == 0x3C &&
                  offsetof(ZgePart, flags) == 0x3E && offsetof(ZgePart, id) == 0x40 &&
                  offsetof(ZgePart, typeWord) == 0x60);
    static_assert(offsetof(ZgePart, faceTags) == 0x44 && offsetof(ZgePart, faceEdges) == 0x4C);

    // ZgePart::typeWord: the render type (setPartRenderClass) and what the packet
    // builders counted (13 bits each, the excess dropped).
    enum : uint32_t
    {
        kTypeWordShift = 26,            // the render type, bits 26-31
        kTypeWordEntries = 0x00001FFFu, // strip entries sent to VU1 (bits 0-12)
        kTypeWordTriangles = 0x03FFE000u, // triangles counted (bits 13-25)
    };

    // ZgePart::bufferFlags.
    enum : uint32_t
    {
        kBufferToBuild = 1u,   // the packet buffer built next (0 or 1)
        kBufferDrawn = 2u,     // the packet buffer drawn
        kBufferBytesShift = 2, // bits 2-31: the bytes of one buffer's allocation
    };

    // An entry of a packet's material table (8 bytes, the material's index):
    // where its part of the packet starts (0: no strips of that material).
    struct PacketMaterial
    {
        ee::Ptr<uint8_t> start; // +0x00
        // +0x04 the VU1 address the material's GS block unpacks to
        // (buildMaterialChain's UNPACK; the builders write 0).
        uint16_t gsBlockAddress;
        uint16_t _06;
    };
    static_assert(sizeof(PacketMaterial) == 8);

    // A strip group (TGFM, the same on disc and in RAM), 4-byte aligned:
    //     u16 submeshMode, u16 count, u16 indices[count], padding
    struct StripGroup
    {
        uint16_t submeshMode; // submesh << 8 | mode (bits 0-1)
        uint16_t count;
        uint16_t indices[1];  // [count]

        // Bytes to the next group.
        static constexpr uint32_t strideFor(uint32_t count) { return (count * 2u + 7u) & ~3u; }
        // The group after this one.
        static ee::Ptr<StripGroup> after(ee::Ptr<StripGroup> group) { return (group.cast<uint8_t>() + strideFor(group->count)).cast<StripGroup>(); }
    };
    enum : uint16_t
    {
        kStripVertexMask = 0x3FFF,
        kStripFlipped = 0x4000,    // this triangle's winding is reversed
        kStripNoTriangle = 0x8000, // a strip (re)start: no triangle ends here
    };

    // A face record (TGFP, 8 bytes; collision). [inferred]
    struct FaceRecord
    {
        uint8_t _00;
        uint8_t countAndFlags; // bits 0-3: a count (of edges?), 4-7 flags
        uint8_t _02[6];
    };
    static_assert(sizeof(FaceRecord) == 8);

    // A face as faceTriangle gives it (0x48 bytes).
    struct FaceTriangle
    {
        Float4 corners[3];     // +0x00 the corners' positions (the positions stream's records)
        Float4 normal;         // +0x30 the face's normal, w 0
        uint64_t flags;        // +0x40 kTriangleTwoSided, kTriangleCollides; the other bits the caller's
    };
    static_assert(offsetof(FaceTriangle, normal) == 0x30 && offsetof(FaceTriangle, flags) == 0x40);
    constexpr uint64_t kTriangleTwoSided = 1u << 0; // the face's kFaceTwoSided
    constexpr uint64_t kTriangleCollides = 1u << 1; // the face's kFaceCollides

    // A bone record (TGPB; StreamKind::Bones), its header filled by the
    // loader. [inferred]
    struct BoneRecord
    {
#if ZX_GAME_BMX // (ZGE4's: the offset first, a 0x20-byte record: BMX loadBoneChunk, posePart, copyStream)
        Float4 offset;       // BMX +0x00
        uint32_t bytes;      // BMX +0x10 the record's size (0x20)
        uint16_t poseIndex;  // BMX +0x14
#else
        uint32_t bytes;      // +0x00 the record's size (chunk size + 0x20)
        uint32_t _04[3];
        Float4 offset;       // +0x10 x, y, z from the file, w = 1
        uint16_t poseIndex;  // +0x20
#endif
    };
    static_assert(offsetof(BoneRecord, offset) == game::readable::perGame(0x10u, 0x10u, 0x00u) &&
                  offsetof(BoneRecord, poseIndex) == game::readable::perGame(0x20u, 0x20u, 0x14u) &&
                  offsetof(BoneRecord, bytes) == game::readable::perGame(0x00u, 0x00u, 0x10u));

    // newStream (part.cpp): a stream of a kind for the part (new: its
    // data's address; a copy, copyOf non-zero: the record's).
    uint32_t newStream(ee::Ptr<ZgePart> part, StreamKind kind, uint32_t bytes, uint32_t copyOf);

    // (ZGE4 only, part.cpp) a part's bone record by its pose number, 0 if none; a copy of one part's into another.
    ee::Ptr<BoneRecord> findPartBoneRecord(ee::Ptr<ZgePart> part, uint32_t poseNumber);
    void copyPartBoneRecord(ee::Ptr<ZgePart> to, ee::Ptr<ZgePart> from, uint32_t poseNumber);
}

namespace zge
{
    // (part.cpp)
    uint32_t partStream(ee::Ptr<ZgePart> part, StreamKind kind);
    void releaseStream(ee::Ptr<ZgePart> part, StreamKind kind);
    int32_t shareStream(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, StreamKind kind);
    uint32_t initParts();
    uint32_t shutDownParts();
    ee::Ptr<ZgePart> newPart();
    uint32_t releasePart(ee::Ptr<ZgePart> part);
    int32_t addPartReference(ee::Ptr<ZgePart> part);
    int32_t dropPartReference(ee::Ptr<ZgePart> part);
    ee::Ptr<ZgePart> findPartById(uint32_t id);
    void buildPartChainPackets(ee::Ptr<ZgePart> part);
    void partBounds(ee::Ptr<ZgePart> part, uint32_t box);
    int32_t attachBuffer(ee::Ptr<ZgePart> part, StreamKind kind, ee::Ptr<TagTableHeader> table);
    int32_t copyStream(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, StreamKind kind, uint32_t copyFlags);
    uint32_t partIgnoresStream(ee::Ptr<ZgePart> part, StreamKind kind);
    ee::Ptr<MaterialTable> meshMaterials(ee::Ptr<ZgePart> part, uint32_t set);
    READABLE_RESULT_EXCEPT(dm2, int32_t) attachMaterialTable(ee::Ptr<ZgePart> part, ee::Ptr<MaterialTable> table);
    int32_t materialSetCount(ee::Ptr<ZgePart> part);
    int32_t attachSkinnedPart(ee::Ptr<ZgePart> owner, ee::Ptr<ZgePart> skinned);
    int32_t shareBones(ee::Ptr<ZgePart> source, ee::Ptr<ZgePart> part);
    uint32_t faceMaterial(ee::Ptr<ZgePart> part, uint32_t face);
    int32_t faceCornerIndex(ee::Ptr<ZgePart> /*part*/, uint32_t face, uint32_t corner);
    ee::Ptr<Float4> vertexNormal(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Float4> out);
    ee::Ptr<uint8_t> setNormalEntry(ee::Ptr<ZgePart> part, uint32_t entry, ee::Ptr<ee::Float> normal);
    ee::Ptr<VertexColour> vertexColour(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<VertexColour> out);
    ee::Ptr<Float4> vertexUv(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Float4> out);
    ee::Ptr<Vec4> partUv(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> out); // (ZGE2's vertexUv)
    void forEachPart(uint32_t visit, uint32_t data);
    ee::Ptr<StripGroup> copyStripGroups(ee::Ptr<StripGroup> from, ee::Ptr<StripGroup> to, uint32_t count, uint32_t submeshOffset,
                                        uint32_t indexOffset);
    void copyFaces(ee::Ptr<ZgePart> from, ee::Ptr<ZgePart> to, uint32_t count, uint32_t firstFace, uint32_t materialOffset, uint32_t vertexOffset);
}

namespace zge
{
    // (part_render.cpp)
    uint32_t resolveRenderType(ee::Ptr<Node> node, ee::Ptr<ZgePart> part);
    ee::Ptr<uint32_t> markBuilt(ee::Ptr<ZgePart> part, ee::Ptr<uint32_t> buffer);
    int32_t buildMaterialChain(ee::Ptr<MaterialTable> table, ee::Ptr<ZgePart> part);
    int32_t buildNodeMaterialChain(ee::Ptr<Node> node); // ZGE2's buildMaterialChain (part_render.cpp)
    uint32_t newNodeRenderState();
    void freeNodeRenderState(ee::Ptr<Node> node);
    uint32_t clearRenderState(ee::Ptr<ZgePart> part);
    uint32_t cloneRenderState(ee::Ptr<ZgePart> source, ee::Ptr<ZgePart> clone, uint32_t shareFlags);
    uint32_t releasePartPackets(ee::Ptr<ZgePart> part);
    uint32_t buildChainPartPackets(ee::Ptr<ZgePart> part);
    uint32_t setPartRenderClass(ee::Ptr<ZgePart> part, uint32_t renderClass);
    int32_t renderIgnoresStream(ee::Ptr<ZgePart> part, StreamKind kind);
    uint32_t clearMaterialChains(ee::Ptr<MaterialTable> table);
    uint32_t freeMaterialChains(ee::Ptr<MaterialTable> table);
    void buildAllPartPackets();
    void updateNodeLights(ee::Ptr<Node> node);
    ee::Ptr<LightBlock> setNodeAmbient(ee::Ptr<Node> node, ee::Ptr<Vec3> colour);
    void nodeAmbient(ee::Ptr<Node> node, ee::Ptr<Vec3> colour);
}

namespace zge
{
    // (part_streams.cpp)
    void setPartPosition(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> position);
    void setPartColour(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<const VertexColour> colour);
    // ZGE2 only: a vertex's normal (w 1) and its colour, into out.
    ee::Ptr<Vec4> partNormal(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> out);
    ee::Ptr<VertexColour> partColour(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<VertexColour> out);
    void setPartUv(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<ee::Float> uv);
}
