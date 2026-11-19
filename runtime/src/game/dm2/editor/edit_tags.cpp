// DM2's park editor: a piece's face and edge tags (edit_tags.h). DM2's own
// code (its editor block). The records are read by group and index (the
// engine's tagByte and tagWord), as the rider's surface tags are
// (rider/collision/tags.cpp).

#include "edit_tags.h"
#include "park_editor.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/edit_tags.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_editGapTag> g_editGapTag;
        inline ee::Global<uint32_t[kEditGapTagCount], sym::g_editGapTags> g_editGapTags;

        constexpr uint32_t kByteGroup = 2;
        constexpr uint32_t kWordGroup = 8;
        // An edge's byte 5: bit 0 says the edge can be ground.
        constexpr uint32_t kGrindByte = 5;
        constexpr uint32_t kGrindable = 0x01;
        // Scaffolding (unnamed-field): bits 1 .. 3 of the same byte number
        // the edge's group (1 .. 7, 0 for none); what a group is, is not
        // settled (setEditEdgesGrindable's callers are not read yet).
        constexpr uint32_t kEdgeGroupMask = 0x0E;
        constexpr uint32_t kEdgeGroupShift = 1;
        // A face's word 0: its tag.
        constexpr uint32_t kTagWord = 0;
        constexpr uint32_t kEdgesAFace = 3;

        // The node's collision mesh, 0 if it has none (hasCollisionFrame:
        // the original's test of the frame).
        ee::Ptr<zge::ZgePart> collisionMesh(ee::Ptr<zge::Node> node)
        {
            if (!hasCollisionFrame(node))
                return ee::Ptr<zge::ZgePart>();
            return node->lods->part;
        }

        uint32_t withGrindable(ee::Ptr<zge::TagRecord> record, bool on)
        {
            if (on)
                return (zge::tagByte(record, kByteGroup, kGrindByte) | kGrindable) & 0xFF;
            return (zge::tagByte(record, kByteGroup, kGrindByte) & ~kGrindable) & 0xFF;
        }
    }

    void setUpEditGapTags()
    {
        *g_editGapTag = zge::nameHash(ee::Ptr<const char>(sym::textEditGap));
        (*g_editGapTags)[4] = zge::nameHash(ee::Ptr<const char>(sym::textEditGap3));
        (*g_editGapTags)[1] = zge::nameHash(ee::Ptr<const char>(sym::textEditGap2));
        (*g_editGapTags)[2] = zge::nameHash(ee::Ptr<const char>(sym::textEditGap1));
        (*g_editGapTags)[3] = zge::nameHash(ee::Ptr<const char>(sym::textEditGap4));
    }
    READABLE_FUNCTION(setUpEditGapTags, Verified);

    void clearEditGapTags()
    {
        *g_editGapTag = 0;
        (*g_editGapTags)[0] = 0;
        (*g_editGapTags)[1] = 0;
        (*g_editGapTags)[4] = 0;
        (*g_editGapTags)[3] = 0;
        (*g_editGapTags)[2] = 0;
    }
    READABLE_FUNCTION(clearEditGapTags, Verified);

    uint32_t editEdgeGrindable(ee::Ptr<zge::Node> node, uint32_t edge)
    {
        const ee::Ptr<zge::TagRecord> record = editEdgeTagRecord(node, edge);
        if (!record)
            return 0;
        return zge::tagByte(record, kByteGroup, kGrindByte) & kGrindable;
    }
    READABLE_FUNCTION(editEdgeGrindable, Draft);

    uint32_t setEditEdgeGrindable(ee::Ptr<zge::Node> node, uint32_t edge, uint8_t on)
    {
        const ee::Ptr<zge::TagRecord> record = editEdgeTagRecord(node, edge);
        if (!record)
            return 0;
        zge::setTagByte(record, kByteGroup, kGrindByte, withGrindable(record, on != 0));
        return 1;
    }
    READABLE_FUNCTION(setEditEdgeGrindable, Draft);

    uint32_t setEditEdgesGrindable(ee::Ptr<zge::Node> node, uint8_t group, uint8_t on)
    {
        if (!node)
            return 0;
        const ee::Ptr<zge::ZgePart> mesh = collisionMesh(node);
        if (!mesh)
            return 0;
        const int32_t edges = mesh->faceCount * kEdgesAFace;
        const uint32_t wanted = (group + 1u) & 0xFF;
        for (int32_t edge = 0; edge < edges; ++edge)
        {
            const ee::Ptr<zge::TagRecord> record = editEdgeTagRecord(node, edge);
            if (!record)
                continue;
            if ((zge::tagByte(record, kByteGroup, kGrindByte) & kEdgeGroupMask) >> kEdgeGroupShift != wanted)
                continue;
            zge::setTagByte(record, kByteGroup, kGrindByte, withGrindable(record, on != 0));
        }
        return 1;
    }
    READABLE_FUNCTION(setEditEdgesGrindable, Draft);

    uint32_t editGapTag()
    {
        return *g_editGapTag;
    }
    READABLE_FUNCTION(editGapTag, Verified);

    uint32_t editGapTagOf(uint32_t index)
    {
        return ee::Ptr<uint32_t>(sym::g_editGapTags)[index];
    }
    READABLE_FUNCTION(editGapTagOf, Verified);

    int32_t editFaceTag(ee::Ptr<zge::Node> node, uint32_t face)
    {
        const ee::Ptr<zge::TagRecord> record = editFaceTagRecord(node, face);
        if (!record)
            return 0;
        return zge::tagWord(record, kWordGroup, kTagWord);
    }
    READABLE_FUNCTION(editFaceTag, Verified);

    uint32_t setEditFaceTag(ee::Ptr<zge::Node> node, uint32_t face, uint32_t tag)
    {
        const ee::Ptr<zge::TagRecord> record = editFaceTagRecord(node, face);
        if (!record)
            return 0;
        zge::setTagWord(record, kWordGroup, kTagWord, tag);
        return 1;
    }
    READABLE_FUNCTION(setEditFaceTag, Draft);

    uint32_t replaceEditFaceTags(ee::Ptr<zge::Node> node, int32_t from, uint32_t to)
    {
        const ee::Ptr<zge::ZgePart> mesh = collisionMesh(node);
        if (!mesh)
            return 0;
        const int32_t faces = mesh->faceCount;
        for (int32_t face = 0; face < faces; ++face)
        {
            const ee::Ptr<zge::TagRecord> record = editFaceTagRecord(node, face);
            if (record && zge::tagWord(record, kWordGroup, kTagWord) == from)
                zge::setTagWord(record, kWordGroup, kTagWord, to);
        }
        return 1;
    }
    READABLE_FUNCTION(replaceEditFaceTags, Verified);

    ee::Ptr<zge::TagRecord> editEdgeTagRecord(ee::Ptr<zge::Node> node, uint32_t edge)
    {
        const ee::Ptr<zge::ZgePart> mesh = collisionMesh(node);
        if (!mesh)
            return ee::Ptr<zge::TagRecord>();
        const ee::Ptr<zge::TagTableHeader> table = mesh->faceEdgeTable;
        if (!table)
            return ee::Ptr<zge::TagRecord>();
        return zge::tagElementRecord(table, edge);
    }
    READABLE_FUNCTION(editEdgeTagRecord, Draft);

    ee::Ptr<zge::TagRecord> editFaceTagRecord(ee::Ptr<zge::Node> node, uint32_t face)
    {
        const ee::Ptr<zge::ZgePart> mesh = collisionMesh(node);
        if (!mesh)
            return ee::Ptr<zge::TagRecord>();
        const ee::Ptr<zge::TagTableHeader> table = mesh->faceTagTable;
        if (!table)
            return ee::Ptr<zge::TagRecord>();
        return zge::tagElementRecord(table, face);
    }
    READABLE_FUNCTION(editFaceTagRecord, Verified);

    uint32_t findEditLightFaces(ee::Ptr<zge::Node> node, ee::Ptr<uint32_t> faces, ee::Ptr<uint32_t> count)
    {
        faces[0] = 0;
        if (!node)
            return 0;
        const ee::Ptr<zge::ZgePart> mesh = collisionMesh(node);
        if (!mesh)
            return 0;
        *count = 0;
        const int32_t lightTag = static_cast<int32_t>(zge::nameHash(ee::Ptr<const char>(sym::textEditLight)));
        const uint32_t faceCount = mesh->faceCount;
        for (uint32_t face = 0; face < faceCount; ++face)
        {
            const ee::Ptr<zge::TagRecord> record = editFaceTagRecord(node, face);
            if (record && zge::tagWord(record, kWordGroup, kTagWord) == lightTag)
            {
                faces[*count] = face;
                ++*count;
            }
        }
        return *count != 0;
    }
    READABLE_FUNCTION(findEditLightFaces, Verified);
}
