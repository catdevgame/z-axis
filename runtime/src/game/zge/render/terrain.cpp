// The terrain's sectors, geometry records and nodes (zglTerrainRender).
//
// The SDRD sectors (g_sectors) are 32 records a level's SDRD chunks or
// addSector fill in. Each has a small state the game was to step: a sector
// starts at state 1; updateSectors hands the changed sectors of a group at
// state 1 to a callback and puts them at 0; clearSectorStates puts a state
// 0 back to the sector's default and counts states of 2 and more down by
// one. No AI level has an SDRD chunk and nothing in AI's PS2 build calls
// any of these: all of it is dead, kept for mods.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "ee/field.h"
#include "zge/scene/node.h"
#include "zge/scene/skeleton.h"
#include "zge/render/material.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/scene/query.h"
#include "zge/container/array.h"
#include "zge/memory/pool.h"
#include "zge/assets/chunk_reader.h"
#include "zge/math/box.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "ee/function_ptr.h"
#include "zge/render/part.h"
#include "zge/native/geo_meshes.h"
#include "zge/native/host.h"
#include "zge/render/terrain.h"
#include "zge/render/draw.h"
#include "zge/render/view_regions.h"
#include "zge/math/matrix.h"
#include "zge/scene/scene.h"
#include "zge/scene/camera.h"
#include "zge/scene/visibility_regions.h"
#include "zge/debug/debug.h"
#include "zge/container/pointer_array.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/terrain.cpp.h"

namespace zge
{
    // The part's vertex streams, read and set one vertex at a time
    // (part.cpp, part_streams.cpp, collision/mesh.cpp).

    namespace
    {
        constexpr uint32_t kSectors = sym::g_sectors;
        inline ee::Global<uint32_t, sym::g_activeSdrdRecords> g_activeSdrdRecords; // g_activeSdrdRecords ((anonymous)::g_activeSdrdRecords)
        inline ee::Global<uint32_t, sym::g_changedSdrdRecords> g_changedSdrdRecords; // g_changedSdrdRecords ((anonymous)::g_changedSdrdRecords)
        inline ee::Global<uint32_t, sym::g_latchedSectorMask> g_latchedSectorMask;
        inline ee::Global<ee::Ptr<Pool>, sym::g_geometryPool> g_geometryPool; // g_geometryPool ((anonymous)::g_geometryPool)
        inline ee::Global<ee::Ptr<Pool>, sym::g_geometryNodePool> g_geometryNodePool; // g_geometryNodePool ((anonymous)::g_geometryNodePool, (anonymous)::g_geometryNodePool)
    }

    // An SDRD sector (g_sectors holds kSdrdSectors).
    struct SdrdSector
    {
        uint32_t id;             // +0x00 findSector's key
        uint8_t group;           // +0x04 updateSectors steps a group at a time
        uint8_t defaultState : 4; // +0x05 bits 0-3: clearSectorStates puts state 0 back to it
        uint8_t state : 4;        // +0x05 bits 4-7
        uint8_t kind;            // +0x06 the chunk's; nothing reads it
        uint8_t chunkFlag : 1;   // +0x07 bit 0, the chunk's; nothing reads it
        uint8_t updateFlag : 1;  // +0x07 bit 1: updateSectors takes only the sectors its second argument names
        uint8_t used : 1;        // +0x07 bit 2: the record holds a sector
        // +0x07 bits 3-7: its bit in g_activeSdrdRecords and g_changedSdrdRecords.
        // addSector sets it to the record's slot (0 when all were used);
        // readSdrdChunk leaves the record's old one (0 for a record never
        // used), so every sector it reads into a fresh record takes bit 0.
        uint8_t maskBit : 5;
        uint32_t chunkWord;      // +0x08 the chunk's fourth word; nothing reads it
        Box box;                 // +0x10 (low's w 1, high's w the chunk's)
        uint32_t clearedWord;    // +0x30 set to 0 with the sector; nothing reads it
        Vec4 chunkVector;        // +0x40 three floats of the chunk (w not written); nothing reads them
        Vec4 chunkVector2;       // +0x50 the next three; nothing reads them
    };
    static_assert(offsetof(SdrdSector, chunkWord) == 0x08 && offsetof(SdrdSector, box) == 0x10);
    static_assert(offsetof(SdrdSector, clearedWord) == 0x30 && offsetof(SdrdSector, chunkVector) == 0x40);
    static_assert(sizeof(SdrdSector) == 0x60);
    constexpr uint32_t kSdrdSectors = 32;

    namespace
    {
        // The record in slot `index` of g_sectors.
        ee::Ptr<SdrdSector> sectorSlot(uint32_t index) { return ee::Ptr<SdrdSector>(kSectors) + index; }

        // The first unused record and its slot; when all are used, the last
        // record and kSdrdSectors.
        ee::Ptr<SdrdSector> freeSectorSlot(uint32_t &slot)
        {
            slot = 0;
            while (sectorSlot(slot)->used)
            {
                if (slot + 1 >= kSdrdSectors)
                {
                    slot = kSdrdSectors;
                    return sectorSlot(kSdrdSectors - 1);
                }
                ++slot;
            }
            return sectorSlot(slot);
        }

        // A sector set up: in use, at state 1, counted active and changed.
        void startSector(SdrdSector &sector)
        {
            sector.clearedWord = 0;
            sector.state = 1;
            *g_activeSdrdRecords = *g_activeSdrdRecords | 1u << sector.maskBit;
            *g_changedSdrdRecords = *g_changedSdrdRecords | 1u << sector.maskBit;
        }
    }

    // An SDRD chunk's body.
    struct SdrdChunk
    {
        uint32_t first;        // +0x00 not read
        uint32_t id;           // +0x04
        uint8_t group;         // +0x08
        uint8_t defaultState;  // +0x09 its low four bits
        uint8_t kind;          // +0x0A
        uint8_t flags;         // +0x0B bit 0 chunkFlag, bit 1 updateFlag
        uint32_t chunkWord;    // +0x0C
        Vec3 boxLow;           // +0x10
        Float4 boxHigh;        // +0x1C
        Vec3 chunkVector;      // +0x2C
        Vec3 chunkVector2;     // +0x38
    };
    static_assert(offsetof(SdrdChunk, boxHigh) == 0x1C && sizeof(SdrdChunk) == 0x44);

    // readSdrdChunk: an SDRD chunk's sector into the first unused record
    // (the last when all are used), started. 1.
    int32_t readSdrdChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<SdrdChunk> body;
        readChunk(reader, &*body, sizeof(SdrdChunk));
        uint32_t slot;
        SdrdSector &sector = *freeSectorSlot(slot);
        sector.used = 1;
        sector.chunkWord = body->chunkWord;
        sector.id = body->id;
        sector.chunkFlag = body->flags & 1u;
        sector.updateFlag = (body->flags >> 1) & 1u;
        sector.group = body->group;
        sector.kind = body->kind;
        sector.defaultState = body->defaultState & 0xFu;
        sector.chunkVector.xyz() = body->chunkVector;
        sector.chunkVector2.xyz() = body->chunkVector2;
        startSector(sector);
        sector.box.low.xyz() = body->boxLow;
        sector.box.low.w = ee::Float(1.0f);
        sector.box.high.x = body->boxHigh.x;
        sector.box.high.y = body->boxHigh.y;
        sector.box.high.z = body->boxHigh.z;
        sector.box.high.w = body->boxHigh.w;
        return 1;
    }
    READABLE_FUNCTION_WAITS(readSdrdChunk, Draft);

    // findSector: the record in use with the id, 0 for none.
    ee::Ptr<SdrdSector> findSector(uint32_t id)
    {
        for (uint32_t slot = 0; slot < kSdrdSectors; ++slot)
        {
            const ee::Ptr<SdrdSector> sector = sectorSlot(slot);
            if (sector->used && sector->id == id)
                return sector;
        }
        return ee::Ptr<SdrdSector>();
    }
    READABLE_FUNCTION(findSector, Draft);

    // sectorAt: the record in slot `index` when it is in use, else 0 (the
    // index unchecked).
    ee::Ptr<SdrdSector> sectorAt(uint32_t index)
    {
        const ee::Ptr<SdrdSector> sector = sectorSlot(index);
        return sector->used ? sector : ee::Ptr<SdrdSector>();
    }
    READABLE_FUNCTION(sectorAt, Draft);

    // addSector: a copy of `source` in the first unused record (the last
    // when all are used), its mask bit its slot, started. Returns the record.
    ee::Ptr<SdrdSector> addSector(ee::Ptr<const SdrdSector> source)
    {
        uint32_t slot;
        const ee::Ptr<SdrdSector> sector = freeSectorSlot(slot);
        memoryCopy(sector, source, sizeof(SdrdSector));
        sector->maskBit = slot & 0x1Fu; // all used: 32, which the five bits hold as 0
        sector->used = 1;
        startSector(*sector);
        return sector;
    }
    READABLE_FUNCTION(addSector, Draft);

    // What updateSectors hands each changed sector to.
    using SectorUpdate = ee::FunctionPtr<void(ee::Ptr<SdrdSector> sector)>;

    // updateSectors: each sector changed at the last latch (g_latchedSectorMask)
    // and active, of the group, at state 1 and with updateFlag `updateFlag`,
    // handed to `update` and put at state 0. The masks' bit for a record is
    // taken as its slot's.
    void updateSectors(uint32_t group, uint32_t updateFlag, SectorUpdate update)
    {
        const uint32_t selected = *g_latchedSectorMask & *g_activeSdrdRecords;
        for (uint32_t slot = 0; slot < kSdrdSectors; ++slot)
        {
            const ee::Ptr<SdrdSector> sector = sectorSlot(slot);
            if ((selected & 1u << slot) == 0 || sector->group != group)
                continue;
            if (sector->state != 1 || sector->updateFlag != updateFlag)
                continue;
            update(sector);
            sector->state = 0;
        }
    }
    READABLE_FUNCTION(updateSectors, Draft);

    // clearSectorStates: each active sector's state stepped: 0 back to its
    // default, 2 and more down by one, 1 left.
    void clearSectorStates()
    {
        const uint32_t active = *g_activeSdrdRecords;
        for (uint32_t slot = 0; slot < kSdrdSectors; ++slot)
        {
            SdrdSector &sector = *sectorSlot(slot);
            if ((active & 1u << slot) == 0)
                continue;
            if (sector.state == 0)
                sector.state = sector.defaultState;
            else if (sector.state >= 2)
                sector.state = sector.state - 1;
        }
    }
    READABLE_FUNCTION(clearSectorStates, Draft);

    // setGeoObjectBounds: the object's bounds set to the box (as the
    // original's two quadword copies).
    void setGeoObjectBounds(ee::Ptr<GeoObject> object, ee::Ptr<const Box> box)
    {
        object->bounds() = *box;
    }
    READABLE_FUNCTION(setGeoObjectBounds, Draft);

    // (bits): g_changedSdrdRecords |= bits.
    int32_t markSectorsChanged(uint32_t sectorBits)
    {
        const int32_t changedSdrdRecords = *g_changedSdrdRecords | sectorBits;
        *g_changedSdrdRecords = changedSdrdRecords;
        return changedSdrdRecords;
    }
    READABLE_FUNCTION(markSectorsChanged, Draft);

    // ZglTerrainRender: the changed bits moved to g_latchedSectorMask and cleared.
    int32_t latchChangedSectors()
    {
        const int32_t changedSdrdRecords = *g_changedSdrdRecords;
        *g_changedSdrdRecords = 0;
        *g_latchedSectorMask = changedSdrdRecords;
        return kSectors;
    }
    READABLE_FUNCTION(latchChangedSectors, Draft);

    // Empty; geometryPoolVisit's.
    void geometryRecordNoop()
    {
    }
    READABLE_FUNCTION(geometryRecordNoop, Draft);

    // A geometry pool callback: geometryRecordNoop, 1.
    uint32_t geometryPoolVisit()
    {
        geometryRecordNoop();
        return 1;
    }
    READABLE_FUNCTION(geometryPoolVisit, Draft);

    // PoolForEach of the geometry pool (g_geometryPool) with geometryPoolVisit.
    uint32_t visitGeometryPool()
    {
        return game::tail(zge::poolForEach, *g_geometryPool, sym::geometryPoolVisit, 0);
    }
    READABLE_FUNCTION_WAITS(visitGeometryPool, Draft);

    // PoolCount of the node pool (g_geometryNodePool)
    int32_t geometryNodeCount()
    {
        return zge::poolCount(*g_geometryNodePool);
    }
    READABLE_FUNCTION(geometryNodeCount, Draft);

    // PoolForEach of the node pool with walkTopLevelNode.
    uint32_t walkAllNodes()
    {
        return game::tail(zge::poolForEach, *g_geometryNodePool, sym::walkTopLevelNode, 0);
    }
    READABLE_FUNCTION_WAITS(walkAllNodes, Draft);

    // (table, materialTable, ..): each material put in the other table (materialCount, materialAt,
    // replaceTableMaterial)
    int32_t copyMaterialsInto(ee::Ptr<MaterialTable> table, ee::Ptr<MaterialTable> table2, uint32_t submeshOffset)
    {
        int32_t i = 0;
        int32_t result = materialCount(table);
        const int32_t result2 = result;
        if (result2 != 0)
        {
            int32_t index = i;
            for (;;)
            {
                const ee::Ptr<Material> material = materialAt(table, index);
                if constexpr (game::readable::kIsBmx)
                {
                    // ZGE4 puts a copy of the material in the table (the copy's own reference released after).
                    const ee::Ptr<Material> copy = copyMaterial(material);
                    const int32_t index2 = i + submeshOffset;
                    i = i + 1;
                    replaceTableMaterial(table2, copy, index2);
                    releaseMaterial(copy);
                }
                else
                {
                    const int32_t index2 = i + submeshOffset;
                    i = i + 1;
                    replaceTableMaterial(table2, material, index2);
                }
                result = static_cast<int32_t>(static_cast<uint32_t>(i) < static_cast<uint32_t>(result2));
                index = i;
                if (result != 0)
                    continue;
                break;
            }
            return result;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(copyMaterialsInto, Draft);

    // partStripGroupCount: how many strip groups the part has.
    uint32_t partStripGroupCount(ee::Ptr<ZgePart> part)
    {
        return part->groupCount;
    }
    READABLE_FUNCTION(partStripGroupCount, Draft);

    // faceEntry: a part's face `index` copied to out; out.
    uint32_t faceEntry(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Face> out)
    {
        const ee::Ptr<Face> face = streamRecords<Face>(part->faces) + index;
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            return game::tail(memoryCopy, out, face, sizeof(Face));
        return memoryCopy(out, face, sizeof(Face));
    }
    READABLE_FUNCTION(faceEntry, Draft);

    // setFaceEntry: a part's face `index` set from `from`; the face's
    // address.
    uint32_t setFaceEntry(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<const Face> from)
    {
        const ee::Ptr<Face> face = streamRecords<Face>(part->faces) + index;
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            return game::tail(memoryCopy, face, from, sizeof(Face));
        return memoryCopy(face, from, sizeof(Face));
    }
    READABLE_FUNCTION(setFaceEntry, Draft);

    // vertexWeightEntry: a part's vertex's skin weights record copied to
    // out; out.
    uint32_t vertexWeightEntry(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<SkinWeights> out)
    {
        const ee::Ptr<SkinWeights> weights = streamRecords<SkinWeights>(part->skinWeights) + vertex;
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            return game::tail(memoryCopy, out, weights, sizeof(SkinWeights));
        return memoryCopy(out, weights, sizeof(SkinWeights));
    }
    READABLE_FUNCTION(vertexWeightEntry, Draft);

    // setVertexWeightEntry: a part's vertex's skin weights record set from
    // `from`; the record's address.
    uint32_t setVertexWeightEntry(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<const SkinWeights> from)
    {
        const ee::Ptr<SkinWeights> weights = streamRecords<SkinWeights>(part->skinWeights) + vertex;
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            return game::tail(memoryCopy, weights, from, sizeof(SkinWeights));
        return memoryCopy(weights, from, sizeof(SkinWeights));
    }
    READABLE_FUNCTION(setVertexWeightEntry, Draft);

    namespace
    {
        constexpr uint32_t kMostBones = 32; // the bone map's room (the caller's lists hold at most 32)
    }

    // The one quadword of copyMeshVertices' frame a vertex's attributes go
    // through in turn: its position (setPartPosition reads it whole), then
    // its texture coordinates, then its normal (four floats each, as
    // vertexUv and vertexNormal write them).
    union VertexAttribute
    {
        Vec4 position;
        Float4 uv;
        Float4 normal;
    };
    static_assert(sizeof(VertexAttribute) == 16 && alignof(VertexAttribute) == 16);

    // copyMeshVertices: `count` vertices of part `from` copied into part `to`
    // from vertex `firstIndex` on (a mesh joined into another: the caller,
    // mergeSceneMeshes, calls it once per mesh): the position, and the colour,
    // UVs, normal and skin weights where `to` has those streams (white with
    // alpha 0, zeros, or no bones where `from` lacks them). The skin weights'
    // bones are moved from `from`'s bone list to `to`'s: each bone of the
    // first list (by its id) is looked up in the second first.
    // Kept bugs: a bone `to`'s list lacks keeps whatever the stack held in
    // its place in the map, and a slot with no bone (0) is not skipped: it
    // takes the byte just below the map, past this function's own stack,
    // plus one. (Dead in AI: nothing calls mergeSceneMeshes.)
    void copyMeshVertices(ee::Ptr<ZgePart> from, ee::Ptr<uint32_t> fromBoneIds, uint32_t fromBoneCount, ee::Ptr<ZgePart> to,
                          ee::Ptr<uint32_t> toBoneIds, uint32_t toBoneCount, uint32_t count, uint32_t firstIndex)
    {
        ee::Local<uint32_t[kMostBones]> boneMap; // a bone of `from`'s list: its place in `to`'s
        ee::Local<VertexColour> colour;
        ee::Local<VertexAttribute> attribute;
        ee::Local<SkinWeights> weights;

        for (uint32_t bone = 0; bone < fromBoneCount; ++bone)
        {
            for (uint32_t place = 0; place < toBoneCount; ++place)
            {
                if (fromBoneIds[bone] == toBoneIds[place])
                {
                    boneMap[bone] = place;
                    break;
                }
            }
        }

        for (uint32_t vertex = 0; vertex < count; ++vertex)
        {
            const uint32_t index = vertex + firstIndex;
            vertexPosition(from, vertex, &attribute->position);
            setPartPosition(to, index, &attribute->position);

            if (partStream(to, StreamKind::Colours))
            {
                if (partStream(from, StreamKind::Colours))
                    vertexColour(from, vertex, colour);
                else
                {
                    colour->red = colour->green = colour->blue = 255; // white, alpha 0
                    colour->alpha = 0;
                }
                setPartColour(to, index, colour);
            }

            if (partStream(to, StreamKind::Uvs))
            {
                if (partStream(from, StreamKind::Uvs))
                    vertexUv(from, vertex, &attribute->uv);
                else
                    attribute->uv.x = attribute->uv.y = attribute->uv.z = 0.0f;
                setPartUv(to, index, &attribute->uv.x);
            }

            if (partStream(to, StreamKind::Normals))
            {
                if (partStream(from, StreamKind::Normals))
                    vertexNormal(from, vertex, &attribute->normal);
                else
                    attribute->normal.x = attribute->normal.y = attribute->normal.z = 0.0f;
                setNormalEntry(to, index, &attribute->normal.x);
            }

            if (partStream(to, StreamKind::SkinWeights))
            {
                if (partStream(from, StreamKind::SkinWeights))
                {
                    vertexWeightEntry(from, vertex, weights);
                    // (Bone 0 reads below the map.)
                    const auto moveBone = [&](uint8_t &bone) { bone = static_cast<uint8_t>(boneMap[bone - 1] + 1); };
                    moveBone(weights->bone0);
                    moveBone(weights->bone1);
                    moveBone(weights->bone2);
                    moveBone(weights->bone3);
                }
                else
                {
                    *weights = SkinWeights{};
                }
                setVertexWeightEntry(to, index, weights);
            }
        }
    }
    READABLE_FUNCTION(copyMeshVertices, Draft);

    // partStripGroupBytes: the size of a part's strip groups (their stream's,
    // less its header).
    uint32_t partStripGroupBytes(ee::Ptr<ZgePart> part)
    {
        return ZgeStream::of(part->groups).bytes - sizeof(ZgeStream);
    }
    READABLE_FUNCTION(partStripGroupBytes, Draft);

    // partStripGroups: a part's first strip group.
    ee::Ptr<StripGroup> partStripGroups(ee::Ptr<ZgePart> part)
    {
        return streamRecords<StripGroup>(part->groups);
    }
    READABLE_FUNCTION(partStripGroups, Draft);

    // nextStripGroup: the strip group after this one.
    ee::Ptr<StripGroup> nextStripGroup(ee::Ptr<StripGroup> group)
    {
        return StripGroup::after(group);
    }
    READABLE_FUNCTION(nextStripGroup, Draft);

    // removePartMaterialTable: a material table taken off the part's list of
    // them (materialSets); 1, or 0 when it was not there.
    uint32_t removePartMaterialTable(ee::Ptr<ZgePart> part, ee::Ptr<MaterialTable> table)
    {
        const int32_t index = pointerArrayFind(part->materialSets, table);
        if (index == -1)
            return 0;
        if constexpr (game::readable::kIsBmx) // (ZGE4 releases the table it takes off)
            releaseMaterialTable(table);
        pointerArrayRemoveAt(part->materialSets, index);
        return 1;
    }
    READABLE_FUNCTION(removePartMaterialTable, Draft);

    // (part, vertices, faces, groups, bytes): its streams made (newStream kinds 0, 3, 4, 5, ... at the vertex
    // and face counts)
    int32_t allocateMeshStreams(ee::Ptr<ZgePart> zgePart, uint32_t vertexCount, uint32_t faceCount, uint32_t groupCount, uint32_t bytes)
    {
        // Each stream made new; ZGE4 releases the one the part had first.
        const auto stream = [&](StreamKind kind, uint32_t streamBytes) {
            if constexpr (game::readable::kIsBmx)
                releaseStream(zgePart, kind);
            return newStream(zgePart, kind, streamBytes, 0);
        };
        stream(static_cast<StreamKind>(0), vertexCount << 4);
        const int32_t bytes2 = vertexCount << 2;
        stream(static_cast<StreamKind>(3), vertexCount * 0xC);
        stream(static_cast<StreamKind>(4), bytes2);
        stream(static_cast<StreamKind>(5), bytes2);
        stream(static_cast<StreamKind>(8), vertexCount << 3);
        stream(static_cast<StreamKind>(9), bytes);
        stream(static_cast<StreamKind>(1), faceCount << 3);
        const int32_t streamResult = stream(static_cast<StreamKind>(2), faceCount * 0xC);
        zgePart->groupCount = static_cast<uint16_t>(groupCount);
        zgePart->vertexCount = static_cast<uint16_t>(vertexCount);
        zgePart->faceCount = static_cast<uint16_t>(faceCount);
        // Host side: no file has the part's mesh now; it is made from these streams once they are filled.
        if (native::hostExtrasOn())
            native::notePartStreamsMade(zgePart);
        return streamResult;
    }
    READABLE_FUNCTION_WAITS(allocateMeshStreams, Draft);

    // nodeSphereCentre: the centre of a node's bounding sphere (its address,
    // in the node's mirror).
    ee::Ptr<Vec4> nodeSphereCentre(ee::Ptr<Node> node)
    {
        return ee::field(node, &Node::sphereCentre);
    }
    READABLE_FUNCTION(nodeSphereCentre, Draft);

    // setNodeDrawCheck: the check the scene draw asks before it walks the
    // node's tree, and the data it is handed (zgui's models set theirs);
    // the check it had.
    uint32_t setNodeDrawCheck(ee::Ptr<Node> node, uint32_t check, ee::Address data)
    {
        const uint32_t before = node->drawCheck;
        node->drawCheckData = data;
        node->drawCheck = check;
        return before;
    }
    READABLE_FUNCTION(setNodeDrawCheck, Draft);

    // retainCollisionFrame: a node more shares the frame; the count after it
    // (the sum, before it is cut to 16 bits).
    int32_t retainCollisionFrame(ee::Ptr<CollisionFrame> frame)
    {
        const int32_t references = frame->references + 1;
        frame->references = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(retainCollisionFrame, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        // partInView: the sphere (centre, w its radius) against the camera's
        // view: bit 0 outside it, bit 3 across its edge. By address: the
        // engine's view test reads a centre and the radius after it, which
        // a sector and a node keep in their own layouts.
        uint32_t partInView(uint32_t camera, ee::Ptr<const Vec4> sphere) { return ee::game<uint32_t>(sym::partInView, camera, sphere); }
        // addDebugText (zdebug): while debug drawing is on, a debug record
        // (kind 4) of a box, placed by a matrix (0: none), in a colour.
    }

    // The visitors forEachWorldSector, forEachGeoObjectPart and
    // forEachObjectMesh call with each sector, part or mesh and the caller's data.
    using GeoPartVisit = ee::FunctionPtr<int32_t(ee::Ptr<CollisionFrame> part, uint32_t data)>;
    using MeshVisit = ee::FunctionPtr<int32_t(ee::Ptr<ZgePart> mesh, uint32_t data)>;

    // forEachWorldSector: visit(sector, data) for each sector of the world's
    // grid (x by y by z) until one returns 0 (then 0); else 1.
    int32_t forEachWorldSector(SectorVisit visit, uint32_t data)
    {
        const CollisionWorld &world = **g_collisionWorld;
        ee::Ptr<Sector> sector = world.cell;
        for (uint32_t left = world.cellCount[0] * world.cellCount[1] * world.cellCount[2]; left != 0; --left)
        {
            if (visit(sector, data) == 0)
                return 0;
            ++sector;
        }
        return 1;
    }
    READABLE_FUNCTION(forEachWorldSector, Draft);

    void reportWorldCells(ee::Ptr<Sector> sector);

    // reportWorld: a debug drawing of the world: 0 its grid's box, 1 its
    // sectors' boxes; other values nothing.
    void reportWorld(uint32_t what)
    {
        if (what == 0)
            game::tail(matchWorldName, *g_collisionWorld);
        else if (what == 1)
            game::tail(reportWorldCells, ee::Ptr<Sector>());
    }
    READABLE_FUNCTION_WAITS(reportWorld, Draft);

    // reportWorldCells: the sector's box drawn, or with none every sector's
    // of the world's grid.
    void reportWorldCells(ee::Ptr<Sector> sector)
    {
        if (sector)
        {
            game::tail(addDebugText, sector->box, ee::Ptr<Matrix>(), sym::g_worldCellsColour, 0);
            return;
        }
        const CollisionWorld &world = **g_collisionWorld;
        for (int32_t x = 0; x < world.cellCount[0]; ++x)
            for (int32_t y = 0; y < world.cellCount[1]; ++y)
                for (int32_t z = 0; z < world.cellCount[2]; ++z)
                {
                    const ee::Ptr<Sector> cell = world.cell + ((x * world.cellCount[1] + y) * world.cellCount[2] + z);
                    addDebugText(cell->box, 0, sym::g_worldCellsColour, 0);
                }
    }
    READABLE_FUNCTION_WAITS(reportWorldCells, Draft);

    // sectorInView: the sector's sphere against the current camera's view
    // (partInView's bits).
    uint32_t sectorInView(ee::Ptr<Sector> sector)
    {
        return partInView(*g_currentCamera, &sector->sphere);
    }
    READABLE_FUNCTION(sectorInView, Draft);

    namespace
    {
        // A region whose mark equals it is out of view (markRegionsOutOfView).
        inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;
        constexpr uint32_t kOutsideView = 1u; // partInView's (and nodeVisibleInView's) bit 0
        // ZGE2's draw flags bits 8-11: the node's level of detail (setTreeLevelOfDetail; ZGE3 keeps it per view).
        constexpr uint32_t kZge2LevelShift = 8;
    }

    // regionInView (ZGE2 only; ZGE3 and ZGE4 test it inline): whether a node or sector of this region can be seen
    // from the camera's area: with an area and a region, the region is not marked out of view.
    inline uint32_t regionInView(uint32_t area, uint32_t region)
    {
        if (area == 0 || region == 0)
            return 1;
        return ee::Ptr<Region>(region)->outOfViewMark != *g_outOfViewMark ? 1u : 0u;
    }
    READABLE_FUNCTION_ONLY(dm2, regionInView);

    // drawListedNode (ZGE2 only): drawSectorNodes' visitor for each node of a sector in the view, and drawSky's last
    // step: a shown node not yet listed in this scene draw is stamped with it and, when its region is in view and
    // updatedNodeVisibleInView does not find it outside, brought up to date (updateMovedNode), its position put in
    // the camera's view space (its view centre), given the level of detail for that depth (none: not drawn), asked
    // its draw check, and its tree queued with its cull flags (queueNodeTree). The data word is not read. Always 1.
    // ZGE3 does this inline in drawScene, ZGE4 in its own drawListedNodeTree.
    inline int32_t drawListedNode(ee::Ptr<Node> nodeAddress, uint32_t)
    {
        ee::Local<int32_t> level;          // handed to the draw check
        ee::Local<ee::Float> distancePast; // frameLevelForDistance's
        Node &node = *nodeAddress;
        CollisionWorld &world = **g_collisionWorld;
        if ((nodeTreeFlags(node) & kNodeShown) == 0 || nodeListedFrame(node) == world.frameStamp)
            return 1;
        setNodeListedFrame(node, world.frameStamp);
        if (!regionInView(cameraAreaInWorld(world), node.region))
            return 1;
        const uint32_t cullFlags = updatedNodeVisibleInView(nodeAddress);
        if (cullFlags & kOutsideView)
            return 1;
        updateMovedNode(nodeAddress);
        Vec4 &viewCentre = node.views[0].viewCentre;
        transformPoint(&viewCentre, cachedMatrix(*g_currentCamera, 0), &node.world.translation());
        *level = frameLevelForDistance(node.lods, &viewCentre.xyz(), distancePast);
        if (*level < 0)
            return 1;
        if (*level != static_cast<int32_t>((node.drawFlagsDoubleword() >> kZge2LevelShift) & 0xFu))
            setTreeLevelOfDetail(nodeAddress, static_cast<uint8_t>(*level));
        if (node.drawCheck != 0)
        {
            const ee::FunctionPtr<int32_t(ee::Ptr<Node> node, ee::Ptr<int32_t> level, uint32_t data)> check(node.drawCheck);
            if (check(nodeAddress, level, node.drawCheckData) == 0)
                return 1;
        }
        queueNodeTree(nodeAddress, ~0u, cullFlags);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, drawListedNode);

    // drawSectorNodes: ZGE2's sector visitor of drawScene (forEachWorldSector's): a sector in the view has each node
    // of its list drawn by drawListedNode (culled, given its level of detail and its tree queued; ZGE3 does that
    // inline in drawScene); one outside the view is passed over. Always 1. (The original tests the sphere's
    // crossing bits next and makes the same call either way.)
    inline int32_t drawSectorNodes(ee::Ptr<Sector> sector, uint32_t)
    {
        if ((sectorInView(sector) & kOutsideView) == 0)
            forEachSectorNode(sector, sym::dm2::drawListedNode, 0);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, drawSectorNodes);

    // forEachGeoObjectPart: visit(part, data) for each of the object's parts,
    // walked with the parts array's cursor, until one returns 0 (then 0);
    // else 1.
    int32_t forEachGeoObjectPart(ee::Ptr<GeoObject> object, GeoPartVisit visit, uint32_t data)
    {
        uint32_t part = walkFirst(*ee::Ptr<PointerArray>(object->parts));
        while (part != 0)
        {
            if (visit(ee::Ptr<CollisionFrame>(part), data) == 0)
                return 0;
            walkNext(*ee::Ptr<PointerArray>(object->parts), part);
        }
        return 1;
    }
    READABLE_FUNCTION(forEachGeoObjectPart, Draft);

    // nodeVisibleInView: the node's bounding sphere against the current
    // camera's view (partInView's bits); when it is across the view's edge,
    // with the planes its model's bounds cross (partClipFlags) as well; ZGE4
    // takes the world matrix moved by the node's origin.
    uint32_t nodeVisibleInView(ee::Ptr<Node> node)
    {
        uint32_t flags = partInView(*g_currentCamera, &node->sphereCentre);
        if (flags & 0x8u)
        {
            if constexpr (game::readable::kIsBmx) // (ZGE4 clips the bounds where the node's origin moves them, as refreshNodeBounds)
            {
                ee::Local<Matrix> matrix;
                *matrix = node->world;
                Vec4 &translation = matrix->translation();
                translation = translation + node->origin;
                flags |= partClipFlags(ee::Ptr<Camera>(*g_currentCamera), &node->modelBounds(), ee::Ptr<const Matrix>(matrix.address()));
            }
            else
                flags |= partClipFlags(ee::Ptr<Camera>(*g_currentCamera), &node->modelBounds(), &node->world); // the global's word
        }
        return flags;
    }
    READABLE_FUNCTION(nodeVisibleInView, Draft);

    // unlinkNodeFromParent: the node taken out of its parent's list of
    // children (its own parent and sibling links left as they are).
    void unlinkNodeFromParent(ee::Ptr<Node> node)
    {
        const ee::Ptr<Node> parent = node->parent;
        if (!parent)
            return;
        if (parent->firstChild == node)
        {
            parent->firstChild = node->nextSibling;
            return;
        }
        for (ee::Ptr<Node> before = parent->firstChild; before; before = before->nextSibling)
        {
            if (before->nextSibling == node)
            {
                before->nextSibling = node->nextSibling;
                return;
            }
        }
    }
    READABLE_FUNCTION(unlinkNodeFromParent, Draft);

    // nodeFlagBit32: bit 32 of the node's flags doubleword (bit 0 of
    // flagsHigh).
    uint32_t nodeFlagBit32(ee::Ptr<Node> node)
    {
        return node->flagsHigh & 1u;
    }
    READABLE_FUNCTION(nodeFlagBit32, Draft);

    // A node whose region field still holds its mesh's hash, not the region:
    // refreshNodeRegion looks the region up.
    constexpr uint32_t kNodeRegionIsMeshHash = 1u << 18;

    // refreshNodeRegion: the node's view region, looked up first from the
    // mesh hash the field holds when kNodeRegionIsMeshHash is set (and the
    // flag cleared).
    uint32_t refreshNodeRegion(ee::Ptr<Node> node)
    {
        if (node->flags & kNodeRegionIsMeshHash)
        {
            const uint32_t mesh = node->region;
            node->flags &= ~kNodeRegionIsMeshHash;
            node->region = ee::Address(findRegionForMesh(mesh)); // the hash's word holds the region from now on
        }
        return node->region;
    }
    READABLE_FUNCTION(refreshNodeRegion, Draft);

    void walkNodeChildren(ee::Ptr<Node> node);

    // walkNodeChildren: a walk down the subtrees of the node and of its
    // later siblings that does nothing at the nodes (whatever it did is not
    // in this build).
    void walkNodeChildren(ee::Ptr<Node> node)
    {
        for (; node; node = node->nextSibling)
        {
            if (node->firstChild)
                walkNodeChildren(node->firstChild);
        }
    }
    READABLE_FUNCTION(walkNodeChildren, Draft);

    // walkTopLevelNode (walkAllNodes' pool visitor): walkNodeChildren of a
    // node at the top of its tree. 1.
    int32_t walkTopLevelNode(ee::Ptr<Node> node)
    {
        if (!node->parent)
            walkNodeChildren(node);
        return 1;
    }
    READABLE_FUNCTION(walkTopLevelNode, Draft);

    // forEachObjectMesh: visit(mesh, data) for the mesh of each level of
    // detail of each of the node's model's parts, last first, until one
    // returns 0 (then 0); else 1.
    int32_t forEachObjectMesh(ee::Ptr<Node> node, MeshVisit visit, uint32_t data)
    {
        const ee::Ptr<GeoObject> model = objectClass(node);
        for (uint32_t index = modelPartCount(model); index-- != 0;)
        {
            const ee::Ptr<CollisionFrame> frame = classModel(model, index);
            for (uint32_t level = frameLevels(frame); level-- != 0;)
            {
                const ee::Ptr<ZgePart> mesh = framePartMesh(frameLevelPart(frame, level));
                if (visit(mesh, data) == 0)
                    return 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(forEachObjectMesh, Draft);

    // frameLevelForDistance: the first of the frame's levels of detail from
    // its first in use (flags bits 12-15) up to its count (bits 8-11) whose
    // distance is past the position's z, with how far past it in
    // *distancePast (when given); -1 for none.
    int32_t frameLevelForDistance(ee::Ptr<CollisionFrame> frame, ee::Ptr<const Vec3> position, ee::Ptr<ee::Float> distancePast)
    {
        const ee::Float depth = position->z;
        for (uint32_t level = (frame->flags >> 12) & 0xFu; level < ((frame->flags >> 8) & 0xFu); ++level)
        {
            const ee::Ptr<FramePart> part = frameLevelPart(frame, level);
            const ee::Float past = part->distance - depth;
            if (ee::Float(0.0f) < past)
            {
                if (distancePast)
                    *distancePast = past;
                return static_cast<int32_t>(level);
            }
        }
        return -1;
    }
    READABLE_FUNCTION(frameLevelForDistance, Draft);

    namespace
    {
        // Node::drawFlags: which of its class's geometry frames the node has
        // (setLodTable).
        constexpr uint32_t kNodeFrameIndex = 0xFu;
    }

    // findNodeMaterialByHash: among the materials of the node's mesh (the
    // first level of its geometry frame, material set 0), the material whose
    // name has this hash, after `skip` others that have it, from the first;
    // 0 if there are not that many. Nothing calls it.
    ee::Ptr<Material> findNodeMaterialByHash(ee::Ptr<Node> node, uint32_t nameHash, uint32_t skip)
    {
        const ee::Ptr<CollisionFrame> frame = classModel(node->model, node->drawFlags & kNodeFrameIndex);
        const ee::Ptr<FramePart> part(frameLevelPart(frame, 0));
        const ee::Ptr<MaterialTable> table = meshMaterials(part->mesh, 0);
        const uint32_t count = table->count;
        const ee::Ptr<ee::Ptr<Material>> materials(table->materials);
        for (uint32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<Material> material = materials[index];
            if (zge::materialNameHash(material) == nameHash && skip-- == 0)
                return material;
        }
        return ee::Ptr<Material>();
    }
    READABLE_FUNCTION(findNodeMaterialByHash, Draft);
}
