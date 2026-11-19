// BMX XXX's surface tags: a face's or edge's tag record (the level's tag
// data through the engine's tag tables) and what the rider reads from it:
// the surface kind, the grind flag and which side an edge is ground from,
// the wall kind, a face's linked edge, the material's sound. DM2's
// (game/dm2/rider/collision/tags.cpp); BMX XXX has more surface kinds (12
// to DM2's 10) and makes an edge handle only on a node with collision
// faces.
//
// A tag record is read by group and index (the engine's tagByte and
// tagWord): group 2 holds bytes, group 8 words.

#include "collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/assets/tag_table_header.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "sym/bmx/rider/collision/tags.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        constexpr uint32_t kByteGroup = 2u;
        constexpr uint32_t kWordGroup = 8u;
        // Group 2's bytes.
        constexpr uint32_t kGrindByte = 5u;     // bit 0: the edge can be ground
        constexpr uint32_t kEdgeKindByte = 6u;  // which side an edge is ground from (0 .. 3)
        constexpr uint32_t kWallByte = 7u;      // the wall kind (0 .. 15)
        constexpr uint32_t kSurfaceByte = 8u;   // the surface kind (0 .. 12)
        constexpr uint32_t kByte9 = 9u;
        // Group 8's words.
        constexpr uint32_t kTagWord = 0u;       // the tag the goals and collectors match
        constexpr uint32_t kLinkWord = 4u;      // a linked side: face (24 bits), side (8); ~0 none
        constexpr uint32_t kLinkSideShift = 24u;
        constexpr uint32_t kLastSurfaceKind = 12u;

        // The surface kind of each tagged kind (0 .. 12; each its own).
        inline ee::Global<uint32_t[kLastSurfaceKind + 1], sym::g_surfaceKindOfTag> g_surfaceKindOfTag;

        // A tag table's record count, from its descriptor's top bits (the
        // engine's tagRecordCount, which BMX XXX's compiler put inline here).
        constexpr uint32_t kDescriptorRecordsShift = 14u;

        uint32_t tagByte(uint32_t record, uint32_t index)
        {
            return zge::tagByte(ee::Ptr<zge::TagRecord>(record), kByteGroup, index);
        }
    }

    // ---- tag records

    // A face's tag record, or 0 (the mesh's face tag table).
    ee::Address faceTagRecord(uint64_t node, int32_t face)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        if (!mesh)
            return 0;
        const ee::Ptr<zge::TagTableHeader> table = mesh->faceTagTable;
        if (!table)
            return 0;
        return zge::tagElementRecord(table, static_cast<uint32_t>(face));
    }
    READABLE_FUNCTION(faceTagRecord, Verified);

    // An edge's tag record, or 0 (the mesh's edge tag table, by the edge's
    // index).
    ee::Address edgeTagRecord(uint64_t node, int32_t face, uint32_t side)
    {
        side &= 0xFFu;
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        if (!mesh)
            return 0;
        const ee::Ptr<zge::TagTableHeader> table = mesh->faceEdgeTable;
        if (!table)
            return 0;
        const int32_t edge = zge::faceCornerIndex(mesh, static_cast<uint32_t>(face), side);
        return zge::tagElementRecord(table, static_cast<uint32_t>(edge));
    }
    READABLE_FUNCTION(edgeTagRecord, Verified);

    // A face corner's tag record, or 0 (the mesh's vertex tag table, by the corner's index).
    ee::Address cornerTagRecord(uint64_t node, int32_t face, uint32_t corner)
    {
        corner &= 0xFFu;
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        if (!mesh)
            return 0;
        const ee::Ptr<zge::TagTableHeader> table = mesh->vertexTagTable;
        if (!table)
            return 0;
        const int32_t index = zge::faceCornerIndex(mesh, static_cast<uint32_t>(face), corner);
        return zge::tagElementRecord(table, static_cast<uint32_t>(index));
    }
    READABLE_FUNCTION(cornerTagRecord, Draft);

    // The first tag record of a face's material, or 0: the material set 0's
    // material of the face, its tag table's first record (ZGE4 keeps the
    // records' address in the table's header).
    ee::Address faceMaterialTags(uint64_t node, int32_t face)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        const ee::Ptr<zge::MaterialTable> materials = zge::meshMaterials(mesh, 0);
        const uint32_t material = zge::faceMaterial(mesh, static_cast<uint32_t>(face));
        const ee::Ptr<zge::TagTableHeader> tags = zge::materialTags(materials->materials[material]);
        if (!tags)
            return 0;
        if (!(tags->descriptor >> kDescriptorRecordsShift))
            return 0;
        return zge::keptTagRecords(tags);
    }
    READABLE_FUNCTION(faceMaterialTags, Verified);

    // The same, as the rider code calls them.
    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side)
    {
        return edgeTagRecord(node, face, side & 0xFFu);
    }
    READABLE_FUNCTION(edgeTags, Verified);

    uint32_t cornerTags(uint64_t node, int32_t face, uint32_t corner)
    {
        return cornerTagRecord(node, face, corner & 0xFFu);
    }
    READABLE_FUNCTION(cornerTags, Draft);

    uint32_t faceTags(uint64_t node, int32_t face)
    {
        return faceTagRecord(node, face);
    }
    READABLE_FUNCTION(faceTags, Verified);

    uint32_t materialTags(uint64_t node, int32_t face)
    {
        return faceMaterialTags(node, face);
    }
    READABLE_FUNCTION(sym::materialTags2, materialTags, Verified);

    // ---- what a record says

    // The edge can be ground (byte 5, bit 0).
    uint32_t tagGrindable(uint32_t record)
    {
        return tagByte(record, kGrindByte) & 1u;
    }
    READABLE_FUNCTION(tagGrindable, Verified);

    // The wall kind (byte 7: 0 .. 15, else 0).
    uint32_t tagWallKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kWallByte);
        return kind < 16u ? kind : 0u;
    }
    READABLE_FUNCTION(tagWallKind, Verified);

    // Which side an edge is ground from (byte 6: 0 .. 3, else 0).
    uint32_t tagEdgeKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kEdgeKindByte);
        return kind <= 3u ? kind : 0u;
    }
    READABLE_FUNCTION(tagEdgeKind, Verified);

    // The surface kind (byte 8: 0 .. 12, kinds 3 and 4 swapped; else 0).
    uint32_t tagSurfaceKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kSurfaceByte);
        if (kind > kLastSurfaceKind)
            return 0;
        if (kind == 3u)
            return 4;
        if (kind == 4u)
            return 3;
        return kind;
    }
    READABLE_FUNCTION(tagSurfaceKind, Verified);

    // Byte 9.
    uint32_t tagByte9(uint32_t record)
    {
        return tagByte(record, kByte9);
    }
    READABLE_FUNCTION(tagByte9, Verified);

    // The tag word (word 0), as two functions with the same code: one the
    // faces' (faceTagWord), one the edges' (edgeTagWord).
    uint32_t faceRecordTag(uint32_t record)
    {
        return zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord);
    }
    READABLE_FUNCTION(faceRecordTag, Verified);

    uint32_t edgeRecordTag(uint32_t record)
    {
        const uint32_t tag = zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord);
        return tag;
    }
    READABLE_FUNCTION(edgeRecordTag, Verified);

    // A record has a linked side (word 4 is not ~0).
    uint32_t tagHasLink(uint32_t record)
    {
        return zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord) != -1 ? 1u : 0u;
    }
    READABLE_FUNCTION(tagHasLink, Verified);

    // The linked side: its face (24 bits) and side (the top byte).
    void tagLink(uint32_t record, uint32_t *face, uint8_t *side)
    {
        const uint32_t link = zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord);
        *face = link & kRefFaceMask;
        *side = static_cast<uint8_t>(link >> kLinkSideShift);
    }
    READABLE_FUNCTION(tagLink, Verified);

    // A material record's sound (its first byte: 0 .. 15, else 0).
    uint32_t materialSound(const uint8_t *record)
    {
        const uint32_t sound = *record;
        return sound < 16u ? sound : 0u;
    }
    READABLE_FUNCTION(materialSound, Verified);

    // A record's surface kind, through the table of kinds (DM2 keeps 1 ..
    // 10 and makes the rest 0).
    uint32_t surfaceKind(uint32_t record)
    {
        return (*g_surfaceKindOfTag)[tagSurfaceKind(record)];
    }
    READABLE_FUNCTION(surfaceKind, Verified);

    // ---- by face and edge

    // A face's surface kind, or 0 untagged.
    uint32_t faceSurfaceKind(FaceRef ref)
    {
        const uint32_t record = faceTags(widen(refNode(ref)), faceIndex(ref));
        if (!record)
            return 0;
        return surfaceKind(record);
    }
    READABLE_FUNCTION(faceSurfaceKind, Verified);

    // A face's byte 9, or 0 untagged.
    uint32_t faceTagByte9(FaceRef ref)
    {
        const uint32_t record = faceTags(widen(refNode(ref)), faceIndex(ref));
        if (!record)
            return 0;
        return tagByte9(record);
    }
    READABLE_FUNCTION(faceTagByte9, Verified);

    // A face's tag word, or 0 untagged.
    uint32_t faceTagWord(FaceRef ref)
    {
        const uint32_t record = faceTags(widen(refNode(ref)), faceIndex(ref));
        if (!record)
            return 0;
        return faceRecordTag(record);
    }
    READABLE_FUNCTION(faceTagWord, Verified);

    // An edge's tag word, or 0 untagged.
    uint32_t edgeTagWord(EdgeRef edge)
    {
        const uint32_t record = edgeTags(widen(refNode(edge)), edgeFace(edge), edgeSide(edge));
        if (!record)
            return 0;
        return edgeRecordTag(record);
    }
    READABLE_FUNCTION(edgeTagWord, Verified);

    // An edge can be ground (0 untagged).
    uint32_t edgeGrindable(EdgeRef edge)
    {
        const uint32_t record = edgeTags(widen(refNode(edge)), edgeFace(edge), edgeSide(edge));
        if (!record)
            return 0;
        return tagGrindable(record) & 1u;
    }
    // (Draft: no BMX recording reaches it yet.)
    READABLE_FUNCTION(edgeGrindable, Verified);

    // Which side a grindable edge is ground from: -1 or 1 by its tag, by
    // its face's normal (kind 3), 0 for none or not grindable.
    int32_t edgeGrindSide(EdgeRef edge)
    {
        const uint64_t node = widen(refNode(edge));
        const int32_t face = edgeFace(edge);
        const uint32_t side = edgeSide(edge);
        const uint32_t record = edgeTags(node, face, side);
        if (!record)
            return 0;
        if (!(tagGrindable(record) & 1u))
            return 0;
        switch (tagEdgeKind(record))
        {
        case 1:
            return -1;
        case 2:
            return 1;
        case 3:
            return edgeSideByNormal(node, face, side);
        default:
            return 0;
        }
    }
    READABLE_FUNCTION(edgeGrindSide, Verified);

    // The side a face's tag links it to (an EdgeRef on the face's node), or
    // 0 if it has none or the node has no collision faces.
    EdgeRef faceLinkedEdge(FaceRef ref)
    {
        ee::Local<uint8_t> side;
        ee::Local<uint32_t> face;
        const int32_t node = refNode(ref);
        face = static_cast<uint32_t>(faceIndex(ref)); // the original keeps the face there first
        const uint32_t record = faceTags(widen(node), faceIndex(ref));
        if (!record)
            return 0;
        if (!tagHasLink(record))
            return 0;
        tagLink(record, face, side);
        if (!nodeFaceCount(widen(node)))
            return 0;
        return makeEdge(static_cast<uint32_t>(node), face, side);
    }
    READABLE_FUNCTION(faceLinkedEdge, Verified);
}
