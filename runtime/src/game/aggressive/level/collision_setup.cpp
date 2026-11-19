// The level's collision set-up at load (setupCollisionPart): for every collision
// part of the world's models (forEachModelPart walks them,
// rails_setup.cpp), the words the
// collision code reads later are filled into the part's tag tables
// (zge/collision/collision.h FaceTagRecord, EdgeRecord):
//   - a face tagged kind 1 or 2 gets the face-edge element of a matching
//     kind (1 or 9 for kind 1, 2 for kind 2) whose edge comes nearest the
//     face's centroid (+0x04);
//   - a face tagged kind 0xB gets its own index (+0x08);
//   - every edge record gets the last element that picks it (+0x04).
// The host makes the same words from the GEO file
// (collision_setup_host.cpp applyLevelSetupOnHost; the tag mirrors carry
// them); this is the game's own pass, which still writes them into RAM.
// With collision on host data (PS2X_NATIVE_COLLISION) the faces' vertex
// indices come from the host part (zge::faceVertexIndex) like the
// positions (vertexPosition) and the tag records (the accessors' mirrors).

#include "double_rails.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "aggressive/level/matrix_points.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/closest.h"
#include "zge/collision/collision.h"
#include "zge/container/tree.h"
#include "zge/math/vector.h"
#include "zge/scene/node.h"
#include "aggressive/skater/skater.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/zone_memory.h"

#include <cstring>
#include "sym/aggressive/level/collision_setup.cpp.h"

namespace aggressive::level
{
    namespace
    {
        using zge::Vec4;

        // centroid (count, points, out): their centroid, the points
        // summed onto (0, 0, 0, 1), then xyz times 1 / count.

        constexpr uint32_t kTagKind = offsetof(zge::FaceTagRecord, kind);
        constexpr uint32_t kTagNearestEdge = offsetof(zge::FaceTagRecord, nearestEdge);
        constexpr uint32_t kTagFace = offsetof(zge::FaceTagRecord, face);
        constexpr uint32_t kEdgeKind = offsetof(zge::EdgeRecord, kind);
        constexpr uint32_t kEdgeElement = offsetof(zge::EdgeRecord, element);
        constexpr uint32_t kWordSize = 8; // setTagWord's unused second argument, as the original passes it

        // .sdata: an edge shorter than this on every axis has no length.
        inline ee::Global<ee::Float, sym::g_shortestEdge> g_shortestEdge; // gp-0x7CE4: 1e-5
        inline ee::Global<ee::Float, sym::g_farthest> g_farthest;     // the largest float

        ee::Ptr<zge::TagTableHeader> tagTable(ee::Ptr<uint8_t> table)
        {
            return table.cast<zge::TagTableHeader>();
        }

        // A face of `kind`'s nearest edge of a matching kind (its tag's word
        // +0x04): the face-edge elements in turn, each edge's two ends read
        // into `ends`, the point on it nearest the centroid into `apart`, then
        // the way from one to the other.
        void nearestEdge(ee::Ptr<zge::ZgePart> part, uint32_t kind, ee::Ptr<zge::TagRecord> tag, uint32_t elements, ee::Ptr<Vec4> ends, ee::Ptr<Vec4> centre,
                         ee::Ptr<Vec4> apart, ee::Ptr<ee::Ptr<uint16_t>> indices)
        {
            ee::Float nearest = *g_farthest;
            for (uint32_t element = 0; element < elements; ++element)
            {
                const ee::Ptr<zge::TagTableHeader> faceEdges = tagTable(part->faceEdges);
                if (!faceEdges)
                    continue;
                const ee::Ptr<zge::TagRecord> edge = zge::tagElementRecord(faceEdges, element);
                if (!edge)
                    continue;
                // kind 1 takes 1 or 9, kind 2 takes 2.
                const uint32_t edgeKind = zge::tagByte(edge, 2u, kEdgeKind);
                if (edgeKind != kind && !(kind == 1 && edgeKind == 9))
                    continue;
                zge::elementEdgeVertices(part, static_cast<int32_t>(element), indices);
                for (uint32_t end = 0; end < 2; ++end)
                    zge::vertexPosition(part, (*indices)[end], ends + end);
                const ee::Float shortest = *g_shortestEdge;
                if (ee::abs(ends[0].x - ends[1].x) < shortest && ee::abs(ends[0].y - ends[1].y) < shortest && ee::abs(ends[0].z - ends[1].z) < shortest)
                    continue; // no length on every axis
                zge::closestPointOnSegment(&ends[0], &ends[1], static_cast<const Vec4 *>(nullptr), &*centre, &*apart);
                *apart = *apart - *centre;
                const ee::Float distance = lengthOf(apart);
                if (distance < nearest)
                {
                    zge::setTagWord(tag, kWordSize, kTagNearestEdge, element);
                    nearest = distance;
                }
            }
        }
    }

    int32_t setupCollisionPart(ee::Ptr<zge::ZgePart> part)
    {
        if (!part)
            return 1;
        ee::Local<Vec4[3]> corners;
        ee::Local<Vec4[2]> edgeEnds;
        ee::Local<Vec4> centre;
        ee::Local<Vec4> apart;
        ee::Local<ee::Ptr<uint16_t>> indices;

        const uint32_t faceCount = part->faceCount;
        const uint32_t elements = faceCount * 3u; // a face's three edges
        if (const ee::Ptr<zge::TagTableHeader> faceTags = tagTable(part->faceTags))
        {
            for (uint32_t face = 0; face < faceCount; ++face)
            {
                const ee::Ptr<zge::TagRecord> tag = zge::tagElementRecord(faceTags, face);
                if (!tag)
                    continue;
                const uint32_t kind = zge::tagByte(tag, 2u, kTagKind) & 0xFFu;
                if (kind == 1 || kind == 2)
                {
                    // The face's centroid.
                    zge::faceVertices(part, face, indices);
                    const ee::Ptr<uint16_t> faceIndices = *indices;
                    for (uint32_t corner = 0; corner < 3; ++corner)
                        zge::vertexPosition(part, zge::faceVertexIndex(part, face, faceIndices, corner), &corners[corner]);
                    centroid(3, corners, centre);
                    nearestEdge(part, kind, tag, elements, edgeEnds, centre, apart, indices);
                }
                if (kind == 0x0B)
                    zge::setTagWord(tag, kWordSize, kTagFace, face);
            }
        }

        // Every edge record: the element that picks it (the last one wins).
        if (const ee::Ptr<zge::TagTableHeader> faceEdges = tagTable(part->faceEdges))
        {
            for (uint32_t element = 0; element < elements; ++element)
            {
                if (const ee::Ptr<zge::TagRecord> edge = zge::tagElementRecord(faceEdges, element))
                    zge::setTagWord(edge, kWordSize, kEdgeElement, element);
            }
        }
        return 1;
    }
    READABLE_FUNCTION(setupCollisionPart, Verified);

    namespace
    {
        constexpr uint32_t kMeshEdgeTable = 0x4C, kMeshFaceCount = 0x36;
    }

    // collectDoubleRailEdges (object), given to the level's object walk: the object's
    // double rail edges (tags 5 and 6 in its mesh's grind edge table, three
    // edges a face) each put in its tag's tree (level/double_rails.h), as a
    // RailEdgeNode with no rail yet. Returns 1.
    int32_t collectDoubleRailEdges(ee::Ptr<zge::Node> object)
    {
        ee::Local<zge::Matrix> unusedMatrix;
        const ee::Ptr<zge::ZgePart> mesh = aggressive::skater::collisionMeshOf(object);
        if (!mesh || !mesh->faceEdges)
            return 1;

        const uint32_t count = mesh->faceCount * 3u; // a face's three edges
        // Its world matrix is copied and never read.
        *unusedMatrix = *zge::nodeMatrix(object, 1u);
        for (uint32_t edge = 0; edge < count; ++edge)
        {
            const ee::Ptr<zge::TagRecord> record = zge::tagElementRecord(mesh->faceEdgeTable, edge);
            if (!record)
                continue;
            const uint32_t kind = zge::tagByte(record, 2, kEdgeKind);
            if (static_cast<uint8_t>(kind - kDoubleRailEdgeA) >= 2)
                continue;
            const uint32_t taken = aggressive::poolTake(*g_railEdgeNodePool);
            const ee::Ptr<RailEdgeNode> node(taken);
            node->edge = edge;
            node->node = object;
            node->rail = 0;
            zge::searchTreeInsert(kind == kDoubleRailEdgeA ? kDoubleRailEdgesA : kDoubleRailEdgesB, &node->links);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(collectDoubleRailEdges, Verified);
}
