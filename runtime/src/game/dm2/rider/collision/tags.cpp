// DM2's surface tags: a face's or edge's tag record (the level's TGFE data
// through the engine's tag tables) and what the rider reads from it: the
// surface kind, the grind flag and which side an edge is ground from, the
// wall kind, a face's linked edge, the material's sound.
//
// A tag record is read by group and index (the engine's tagByte and
// tagWord): group 2 holds bytes, group 8 words.
// The meanings of the bytes are DM2's. In a level of the other game, the level
// converters (tooling/ai/tools/ai_level.py, dm2_level.py) map them both ways: the
// ramp kinds (0-3 the same in both games, DM2's 4 and 7-10 none of AI's) and the
// surface sounds.

#include "collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/plane.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "sym/dm2/rider/collision/tags.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        constexpr uint32_t kFaceIndexMask = (1u << 24) - 1; // a face reference's low 24 bits: the face's index
        constexpr uint32_t kByteGroup = 2u;
        constexpr uint32_t kWordGroup = 8u;
        // Group 2's bytes.
        constexpr uint32_t kGrindByte = 5u;     // bit 0: the edge can be ground
        constexpr uint32_t kEdgeKindByte = 6u;  // which side an edge is ground from (0 .. 3)
        constexpr uint32_t kWallByte = 7u;      // the wall kind (0 .. 15)
        constexpr uint32_t kSurfaceByte = 8u;   // the surface kind (0 .. 10)
        constexpr uint32_t kByte9 = 9u;
        constexpr uint32_t kLinkKindByte = 4u;  // an edge's link kind: the surface kind of the faces it takes (1, 2)
        // Group 8's words.
        constexpr uint32_t kTagWord = 0u;       // the tag the goals and collectors match
        constexpr uint32_t kLinkWord = 4u;      // a linked side: face (24 bits), side (8); ~0 none
        constexpr uint32_t kLinkSideShift = 24u;

        // An edge's grind side by its face's normal (edgeGrindSide's kind 3).

        uint32_t tagByte(uint32_t record, uint32_t index)
        {
            return zge::tagByte(ee::Ptr<zge::TagRecord>(record), kByteGroup, index);
        }
    }

    uint32_t nodeCollisionMesh(uint64_t node);
    void forEachNode(uint32_t callback, ee::Address arg); // faces.cpp

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
        return zge::tagElementRecord(table, face);
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
        const uint32_t edge = zge::faceCornerIndex(mesh, face, side);
        return zge::tagElementRecord(table, edge);
    }
    READABLE_FUNCTION(edgeTagRecord, Verified);

    // The first tag record of a face's material, or 0.
    ee::Address faceMaterialTags(uint64_t node, int32_t face)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        const ee::Ptr<zge::MaterialTable> materials = zge::meshMaterials(mesh, 0);
        const uint32_t material = zge::faceMaterial(mesh, face);
        const ee::Ptr<zge::TagTableHeader> tags = zge::materialTags(materials->materials[material]);
        if (!tags)
            return 0;
        if (!zge::tagRecordCount(tags))
            return 0;
        return zge::tagRecordAt(tags, 0);
    }
    READABLE_FUNCTION(faceMaterialTags, Verified);

    // The same, as the rider code calls them.
    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side)
    {
        return edgeTagRecord(node, face, side & 0xFFu);
    }
    READABLE_FUNCTION(edgeTags, Verified);

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
    READABLE_FUNCTION(sym::tagGrind, tagGrindable, Verified);

    // The wall kind (byte 7: 0 .. 15, else 0).
    uint32_t tagWallKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kWallByte);
        return kind < 16u ? kind : 0u;
    }
    READABLE_FUNCTION(sym::tagWall, tagWallKind, Verified);

    // Which side an edge is ground from (byte 6: 0 .. 3, else 0).
    uint32_t tagEdgeKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kEdgeKindByte);
        return kind <= 3u ? kind : 0u;
    }
    READABLE_FUNCTION(sym::tagEdge, tagEdgeKind, Verified);

    // The surface kind (byte 8: 0 .. 10, kinds 3 and 4 swapped; else 0).
    uint32_t tagSurfaceKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kSurfaceByte);
        if (kind > 10u)
            return 0;
        if (kind == 3u)
            return 4;
        if (kind == 4u)
            return 3;
        return kind;
    }
    READABLE_FUNCTION(sym::tagKind, tagSurfaceKind, Verified);

    // Byte 9.
    uint32_t tagByte9(uint32_t record)
    {
        return tagByte(record, kByte9);
    }
    READABLE_FUNCTION(sym::tagByteA, tagByte9, Verified);

    // The tag word (word 0), as two functions with the same code: one the
    // faces' (faceTagWord), one the edges' (edgeTagWord).
    uint32_t faceRecordTag(uint32_t record)
    {
        return zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord);
    }
    READABLE_FUNCTION(sym::tagWordA, faceRecordTag, Verified);

    uint32_t edgeRecordTag(uint32_t record)
    {
        const uint32_t tag = zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord);
        return tag;
    }
    READABLE_FUNCTION(sym::tagWordB, edgeRecordTag, Verified);

    // A record has a linked side (word 4 is not ~0).
    uint32_t tagHasLink(uint32_t record)
    {
        return zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord) != -1 ? 1u : 0u;
    }
    READABLE_FUNCTION(sym::tagWordC, tagHasLink, Verified);

    // The linked side: its face (24 bits) and side (the top byte).
    void tagLink(uint32_t record, uint32_t *face, uint8_t *side)
    {
        const uint32_t link = zge::tagWord(ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord);
        *face = link & kRefFaceMask;
        *side = static_cast<uint8_t>(link >> kLinkSideShift);
    }
    READABLE_FUNCTION(sym::tagWordD, tagLink, Verified);

    // A material record's sound (its first byte: 0 .. 15, else 0).
    uint32_t materialSound(const uint8_t *record)
    {
        const uint32_t sound = *record;
        return sound < 16u ? sound : 0u;
    }
    READABLE_FUNCTION(materialSound, Verified);

    // A record's surface kind (1 .. 10, else 0).
    uint32_t surfaceKind(uint32_t record)
    {
        const uint32_t kind = tagSurfaceKind(record);
        return kind - 1u < 10u ? kind : 0u;
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
    READABLE_FUNCTION(sym::faceTagByte, faceTagByte9, Verified);

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
        return tagGrindable(record);
    }
    READABLE_FUNCTION(sym::edgeFlag, edgeGrindable, Verified);

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
        if (!tagGrindable(record))
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
    READABLE_FUNCTION(sym::edgeGrindable, edgeGrindSide, Verified);

    // The side a face's tag links it to (an EdgeRef on the face's node), or
    // 0 if it has none.
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
        return makeEdge(static_cast<uint32_t>(node), face, side);
    }
    READABLE_FUNCTION(sym::faceTagWords, faceLinkedEdge, Verified);

    // ---- linking faces to edges (the level's start: linkLevelFaces walks
    // every node with the linker, which gives each face of surface kind 1
    // or 2 the edge of that link kind it lies along)
    namespace
    {
        inline ee::Global<ee::Float, sym::g_linkPlaneSlack> g_linkPlaneSlack; // -0.01
        // The node linker (DM2's own code, no readable body yet): forEachNode's callback.
        constexpr uint32_t kLinkNodeFaces = sym::linkNodeFaces;
    }

    // An edge's link kind (byte 4: 1 or 2, else 0).
    uint32_t tagLinkKind(uint32_t record)
    {
        const uint32_t kind = tagByte(record, kLinkKindByte);
        return kind == 1u || kind == 2u ? kind : 0u;
    }
    READABLE_FUNCTION(tagLinkKind, Verified);

    // 1 when every one of `count` points is in front of the plane (or
    // within 0.01 behind it), else 0.
    uint32_t pointsInFront(ee::Ptr<zge::Vec4> plane, uint32_t count, ee::Ptr<zge::Vec4> points)
    {
        const ee::Float slack = *g_linkPlaneSlack;
        for (uint32_t index = 0; index < (count & 0xFFu); index = (index + 1) & 0xFFu)
        {
            if (zge::planeDistance(plane, &points[index]) < slack)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(pointsInFront, Verified);

    // 1 when a face (its `count` corner points and its normal) lies in an
    // edge's wedge: the normal not against the first plane and not along
    // the second, and every corner in front of both.
    uint32_t faceInEdgeWedge(uint32_t count, ee::Ptr<zge::Vec4> points, ee::Ptr<zge::Vec4> normal,
                             ee::Ptr<zge::Vec4> firstPlane, ee::Ptr<zge::Vec4> secondPlane)
    {
        count &= 0xFFu;
        ee::Local<zge::Vec4> first;  // the planes' normals, as the original copies them
        ee::Local<zge::Vec4> second;
        first->x = firstPlane->x;
        first->y = firstPlane->y;
        first->z = firstPlane->z;
        if (zge::dot(*first, *normal) < 0.0f)
            return 0;
        if (!pointsInFront(firstPlane, count, points))
            return 0;
        second->x = secondPlane->x;
        second->y = secondPlane->y;
        second->z = secondPlane->z;
        if (0.0f < zge::dot(*second, *normal))
            return 0;
        return pointsInFront(secondPlane, count, points) != 0 ? 1u : 0u;
    }
    READABLE_FUNCTION(faceInEdgeWedge, Verified);

    // A record's linked side set: the face (24 bits) and the side (the top
    // byte). setTagWord's result.
    uint32_t setTagLink(uint32_t record, uint32_t face, uint32_t side)
    {
        return game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord,
                               ee::Address((face & kFaceIndexMask) | (side << kLinkSideShift)));
    }
    READABLE_FUNCTION(setTagLink, Verified);

    // A record's linked side cleared (~0: none).
    uint32_t clearTagLink(uint32_t record)
    {
        return game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord, ee::Address(0xFFFFFFFFu));
    }
    READABLE_FUNCTION(clearTagLink, Verified);

    // A record's tag word set.
    uint32_t setRecordTag(uint32_t record, uint32_t tag)
    {
        return game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord, ee::Address(tag));
    }
    READABLE_FUNCTION(setRecordTag, Verified);

    // A face linked to a side: the face's record gets the link, and the
    // side's record the face's tag word (so a goal or collector that matches
    // the face matches its edge).
    uint32_t linkFaceToEdge(uint32_t faceRecord, uint64_t node, uint32_t face, uint32_t side)
    {
        side &= 0xFFu;
        const uint32_t edgeRecord = edgeTags(node, static_cast<int32_t>(face), side);
        setTagLink(faceRecord, face, side);
        return game::tail(setRecordTag, edgeRecord, faceRecordTag(faceRecord));
    }
    READABLE_FUNCTION(linkFaceToEdge, Verified);

    // The farthest a face's centre may be from the edge it links to.
    ee::Float linkReach()
    {
        return 16.0f;
    }
    READABLE_FUNCTION(linkReach, Verified);

    // The level's faces linked to their edges, node by node. Always 1.
    uint32_t linkLevelFaces()
    {
        forEachNode(kLinkNodeFaces, 0);
        return 1;
    }
    READABLE_FUNCTION(linkLevelFaces, Verified);

    // Its counterpart as the level ends (the level state's exit calls it):
    // it does nothing. Always 1.
    uint32_t unlinkLevelFaces()
    {
        return 1;
    }
    READABLE_FUNCTION(unlinkLevelFaces, Verified);
}
