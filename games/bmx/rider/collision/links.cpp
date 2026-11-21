// The links between a level's tagged faces and edges (collision.h), made
// when a level's or a park's nodes are loaded: a face whose surface kind
// has an edge to go with it (kinds 1 and 2) is linked to the tagged edge of
// that kind it belongs to, and the edge takes the face's tag. A face's own
// side is taken where it has one; a face without is linked to the node's
// nearest such edge that lies along it. The rider reads the link back
// (faceLinkedEdge). BMX XXX's own code (bmx.c).

#include "collision.h"

#include "bmx/level/level_collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/closest.h"
#include "zge/math/plane.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/rider/collision/links.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        constexpr uint32_t kByteGroup = 2u;
        constexpr uint32_t kWordGroup = 8u;
        // Group 2's byte 4: the surface kind of the faces an edge goes with
        // (1 or 2; anything else: none).
        constexpr uint32_t kLinkKindByte = 4u;
        // Group 8's words (tags.cpp).
        constexpr uint32_t kTagWord = 0u;
        constexpr uint32_t kLinkWord = 4u;
        constexpr uint32_t kLinkSideShift = 24u;
        constexpr uint32_t kNoLink = 0xFFFFFFFFu;

        // A node's tagged edge that faces can be linked to, in world space.
        struct LinkEdge
        {
            uint32_t tag;          // +0x00 the edge's tag record
            uint8_t _04[0x0C];
            zge::Vec4 start;       // +0x10
            zge::Vec4 end;         // +0x20
            zge::Vec4 direction;   // +0x30 start to end, length 1
            zge::Vec4 sidePlane;   // +0x40 the upright plane through the edge (its normal: down crossed with the direction)
            zge::Vec4 crossPlane;  // +0x50 the plane through the edge square to that one (the side plane's normal crossed with the direction)
            int32_t face;          // +0x60
            uint8_t side;          // +0x64
            uint8_t _65[0x0B];
        };
        static_assert(offsetof(LinkEdge, start) == 0x10 && offsetof(LinkEdge, sidePlane) == 0x40);
        static_assert(offsetof(LinkEdge, face) == 0x60 && sizeof(LinkEdge) == 0x70);

        // A face's corners (as many as a face has sides, at most).
        constexpr uint32_t kMaxCorners = 16;
        // A corner this far behind a plane is not in front of it.
        inline ee::Global<ee::Float, sym::g_linkBehindLimit> g_linkBehindLimit; // -0.01
        // The distance (squared) farther than any edge.
        inline ee::Global<ee::Float, sym::g_linkFarthest> g_linkFarthest;

        // The loader's two complaints, written and never shown.
        constexpr uint32_t kCoincidentText = sym::bmx::g_textEdgeCoincident; // "edge with two coincident points"
        constexpr uint32_t kVerticalText = sym::bmx::g_textEdgeVertical;     // "vertical edge tagged as grindable"

        constexpr uint32_t kLinkNodeVisitor = sym::linkNodeFaceEdges;
    }

    // An edge's tag word set (the tag the goals and collectors match).
    void setRecordTag(uint32_t record, uint32_t tag)
    {
        game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kTagWord, tag);
    }
    READABLE_FUNCTION(setRecordTag, Verified);

    // The surface kind of the faces an edge goes with: 1 or 2, else 0.
    uint32_t tagLinkKind(uint32_t record)
    {
        const uint32_t kind = zge::tagByte(ee::Ptr<zge::TagRecord>(record), kByteGroup, kLinkKindByte);
        if (kind == 1u)
            return 1;
        if (kind == 2u)
            return 2;
        return 0;
    }
    READABLE_FUNCTION(tagLinkKind, Verified);

    // A record's linked side set (a face and a side of it), and taken away.
    void setTagLink(uint32_t record, uint32_t face, uint32_t side)
    {
        game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord,
                   (face & kRefFaceMask) | side << kLinkSideShift);
    }
    READABLE_FUNCTION(setTagLink, Verified);

    void clearTagLink(uint32_t record)
    {
        game::tail(zge::setTagWord, ee::Ptr<zge::TagRecord>(record), kWordGroup, kLinkWord, kNoLink);
    }
    READABLE_FUNCTION(clearTagLink, Verified);

    // A face linked to a side of a face of its node: the face's record gets
    // the link, the side's edge record the face's tag.
    void linkFaceToEdge(uint32_t faceRecord, uint64_t node, int32_t face, uint32_t side)
    {
        side &= 0xFFu;
        const uint32_t edgeRecord = edgeTags(node, face, side);
        setTagLink(faceRecord, static_cast<uint32_t>(face), side);
        game::tail(setRecordTag, edgeRecord, faceRecordTag(faceRecord));
    }
    READABLE_FUNCTION(linkFaceToEdge, Verified);

    // How near (to its centre) an edge must come for a face to be linked
    // to it.
    ee::Float linkReach()
    {
        return 16.0f;
    }
    READABLE_FUNCTION(linkReach, Verified);

    // Whether every one of `count` points is in front of the plane (or
    // behind it by less than the limit).
    int32_t pointsInFront(ee::Ptr<zge::Vec4> plane, uint32_t count, ee::Ptr<zge::Vec4> points)
    {
        count &= 0xFFu;
        for (uint8_t i = 0; i < count; ++i)
        {
            if (zge::planeDistance(plane, points + i) < *g_linkBehindLimit)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(pointsInFront, Verified);

    // Whether a face lies along an edge: its normal not against the side
    // plane's, every corner in front of the side plane, its normal not
    // along the cross plane's, and every corner in front of the cross plane.
    int32_t faceAlongEdge(uint32_t count, ee::Ptr<zge::Vec4> corners, ee::Ptr<zge::Vec4> normal, ee::Ptr<zge::Vec4> sidePlane,
                          ee::Ptr<zge::Vec4> crossPlane)
    {
        ee::Local<zge::Vec4> sideNormal;
        ee::Local<zge::Vec4> crossNormal;
        count &= 0xFFu;
        // (Each plane's normal is copied without its fourth number.)
        sideNormal->x = sidePlane->x;
        sideNormal->y = sidePlane->y;
        sideNormal->z = sidePlane->z;
        if (zge::dot(*sideNormal, *normal) < 0.0f)
            return 0;
        if (!pointsInFront(sidePlane, count, corners))
            return 0;
        crossNormal->x = crossPlane->x;
        crossNormal->y = crossPlane->y;
        crossNormal->z = crossPlane->z;
        if (0.0f < zge::dot(*crossNormal, *normal))
            return 0;
        return pointsInFront(crossPlane, count, corners) != 0;
    }
    READABLE_FUNCTION(faceAlongEdge, Verified);

    // An edge's record made: its ends, its direction and its two planes.
    void makeLinkEdge(uint64_t node, int32_t face, uint32_t side, uint32_t record, ee::Ptr<LinkEdge> edge)
    {
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> crossNormal;
        ee::Local<char[0x100]> message;
        side &= 0xFFu;
        edge->tag = record;
        edge->side = static_cast<uint8_t>(side);
        edge->face = face;
        vertexWorld(node, face, side, &edge->start.x);
        const int32_t sides = faceSideCount(node, face);
        vertexWorld(node, face, static_cast<uint32_t>(static_cast<int32_t>(side + 1) % sides) & 0xFFu, &edge->end.x);

        edge->direction = edge->end - edge->start;
        const ee::Float length = zge::vec3Length(&edge->direction.xyz());
        if (0.0f < length)
        {
            // (x, y and z are scaled; the fourth number stays.)
            const ee::Float fourth = edge->direction.w;
            edge->direction = edge->direction * (ee::Float(1.0f) / length);
            edge->direction.w = fourth;
        }
        else
            zge::formatString(message, kCoincidentText);

        // Down (y grows downwards) crossed with the direction: level, and
        // square to the edge.
        normal->x = 0.0f;
        normal->y = -1.0f;
        normal->z = 0.0f;
        normal = zge::cross(*normal, edge->direction);
        const ee::Float width = zge::vec3Length(&normal->xyz());
        if (0.0f < width)
        {
            const ee::Float fourth = normal->w;
            normal = *normal * (ee::Float(1.0f) / width);
            normal->w = fourth;
        }
        else
            zge::formatString(message, kVerticalText);
        zge::planeThrough(ee::Ptr<zge::Vec4>(&edge->sidePlane), ee::Ptr<zge::Vec4>(&edge->start), normal);
        crossNormal = zge::cross(*normal, edge->direction);
        zge::planeThrough(ee::Ptr<zge::Vec4>(&edge->crossPlane), ee::Ptr<zge::Vec4>(&edge->start), crossNormal);
    }
    READABLE_FUNCTION_WAITS(makeLinkEdge, Verified);

    // The edge for a face that has none of its own: of the node's `count`
    // edges of the face's kind, the one whose nearest point to the face's
    // centre is nearest, within the reach, and that the face lies along.
    // Its face and side go to `linkFace` and `linkSide`; kNoLink when there
    // is none (the side is then left as it was).
    void findEdgeForFace(uint64_t node, int32_t face, uint32_t faceRecord, ee::Ptr<LinkEdge> edges, uint32_t count,
                         ee::Ptr<uint32_t> linkFace, ee::Ptr<uint8_t> linkSide)
    {
        ee::Local<zge::Vec4[kMaxCorners]> corners;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> nearest;
        *linkFace = kNoLink;
        const ee::Float reach = linkReach();
        const ee::Float reachSquared = reach * reach;
        ee::Float best = *g_linkFarthest;
        const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, face));
        for (uint8_t corner = 0; corner < sides; ++corner)
            vertexWorld(node, face, corner, &corners[corner].x);
        centroid(sides, ee::Ptr<zge::Vec4>(corners.address()), centre);
        faceNormalWorld(node, face, &normal->x);
        const uint32_t kind = tagSurfaceKind(faceRecord);
        for (uint32_t i = 0; i < count; ++i)
        {
            LinkEdge &edge = edges[i];
            if (kind != tagLinkKind(edge.tag))
                continue;
            zge::closestPointOnSegment(&edge.start, &edge.end, &edge.direction, &*centre, &*nearest);
            nearest = *nearest - *centre;
            const ee::Float distanceSquared = zge::dot(*nearest, *nearest);
            if (!(distanceSquared < best))
                continue;
            if (!(distanceSquared < reachSquared))
                continue;
            if (!faceAlongEdge(sides, ee::Ptr<zge::Vec4>(corners.address()), normal, ee::Ptr<zge::Vec4>(&edge.sidePlane),
                               ee::Ptr<zge::Vec4>(&edge.crossPlane)))
                continue;
            best = distanceSquared;
            *linkFace = static_cast<uint32_t>(edge.face);
            *linkSide = edge.side;
        }
    }
    READABLE_FUNCTION(findEdgeForFace, Verified);

    // A node's faces and edges linked (forEachNode's visitor). Always 1.
    int32_t linkNodeFaceEdges(uint64_t node)
    {
        ee::Local<uint8_t> linkSide;
        ee::Local<uint32_t> linkFace;
        const uint32_t faces = nodeFaceCount(node);

        // The edges that faces can be linked to, counted and listed.
        uint32_t count = 0;
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint8_t side = 0; side < sides; ++side)
            {
                const uint32_t edgeRecord = edgeTags(node, static_cast<int32_t>(face), side);
                if (edgeRecord && tagLinkKind(edgeRecord))
                    ++count;
            }
        }
        const ee::Ptr<LinkEdge> edges(count ? zge::allocateMemory(count * sizeof(LinkEdge)) : 0);

        // Each face's old link is taken away; a face with a side of its own
        // kind is linked to it (the first such side).
        uint32_t made = 0;
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t faceRecord = faceTags(node, static_cast<int32_t>(face));
            bool linkable = false;
            if (faceRecord && tagSurfaceKind(faceRecord))
            {
                linkable = true;
                clearTagLink(faceRecord);
            }
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint8_t side = 0; side < sides; ++side)
            {
                const uint32_t edgeRecord = edgeTags(node, static_cast<int32_t>(face), side);
                if (!edgeRecord || !tagLinkKind(edgeRecord))
                    continue;
                makeLinkEdge(node, static_cast<int32_t>(face), side, edgeRecord, edges + made);
                ++made;
                if (!linkable)
                    continue;
                if (tagSurfaceKind(faceRecord) != tagLinkKind(edgeRecord))
                    continue;
                if (tagHasLink(faceRecord))
                    continue;
                linkFaceToEdge(faceRecord, node, static_cast<int32_t>(face), side);
            }
        }

        // The faces still without a link: the nearest edge they lie along.
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t faceRecord = faceTags(node, static_cast<int32_t>(face));
            if (!faceRecord)
                continue;
            if (tagHasLink(faceRecord))
                continue;
            if (!tagSurfaceKind(faceRecord) || !edges)
                continue;
            *linkFace = kNoLink;
            findEdgeForFace(node, static_cast<int32_t>(face), faceRecord, edges, made, linkFace, linkSide);
            if (*linkFace != kNoLink)
                linkFaceToEdge(faceRecord, node, static_cast<int32_t>(*linkFace), *linkSide);
        }
        if (edges)
            zge::freeMemory(ee::Address(edges));
        return 1;
    }
    READABLE_FUNCTION_WAITS(linkNodeFaceEdges, Verified);

    // Every node's faces and edges linked. Always 1.
    int32_t scanNodesEdgeTags()
    {
        forEachNode(kLinkNodeVisitor, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(scanNodesEdgeTags, Verified);
}
