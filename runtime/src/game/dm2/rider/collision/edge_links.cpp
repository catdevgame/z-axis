// The face-to-edge linker's node pass (the level's start, linkLevelFaces in
// tags.cpp): a node's sides that take faces are gathered as link edges, and
// each tagged face of a surface kind gets the side it belongs to: one of its
// own sides of that link kind, or else the nearest gathered edge whose wedge
// the face lies in. DM2's own code.

#include "collision.h"
#include "../vector.h"
#include "../../level/level_collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/closest.h"
#include "zge/math/plane.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/rider/collision/edge_links.cpp.h"

namespace game::dm2::rider::collision
{
    // faces.cpp and tags.cpp
    uint16_t nodeFaceCount(uint64_t node);
    int32_t faceSideCount(uint64_t node, int32_t face);
    void vertexWorld(uint64_t node, int32_t face, uint32_t corner, ee::Float *point);
    uint32_t faceTags(uint64_t node, int32_t face);
    uint32_t tagSurfaceKind(uint32_t record);
    uint32_t tagHasLink(uint32_t record);
    uint32_t tagLinkKind(uint32_t record);
    uint32_t clearTagLink(uint32_t record);
    uint32_t linkFaceToEdge(uint32_t faceRecord, uint64_t node, uint32_t face, uint32_t side);
    ee::Float linkReach();
    uint32_t faceInEdgeWedge(uint32_t count, ee::Ptr<zge::Vec4> points, ee::Ptr<zge::Vec4> normal,
                             ee::Ptr<zge::Vec4> firstPlane, ee::Ptr<zge::Vec4> secondPlane);

    namespace
    {
        // A side that takes faces (its tag has a link kind), as the linker
        // keeps it while it works on a node: the side's ends in the world,
        // its unit direction, and the two planes through it that bound its
        // wedge: the upright one (its normal the direction crossed with
        // down, as edgePlane's) and the one square to it along the edge.
        struct LinkEdge
        {
            uint32_t tagRecord;
            zge::Vec4 start;
            zge::Vec4 end;
            zge::Vec4 direction;
            zge::Vec4 firstPlane;
            zge::Vec4 secondPlane;
            uint32_t face;
            uint8_t side;
        };
        static_assert(offsetof(LinkEdge, start) == 0x10 && offsetof(LinkEdge, direction) == 0x30);
        static_assert(offsetof(LinkEdge, firstPlane) == 0x40 && offsetof(LinkEdge, secondPlane) == 0x50);
        static_assert(offsetof(LinkEdge, face) == 0x60 && offsetof(LinkEdge, side) == 0x64);
        static_assert(sizeof(LinkEdge) == 0x70);

        constexpr uint32_t kNoFace = 0xFFFFFFFFu;
        // The most corners a face has here.
        constexpr uint32_t kMaxCorners = 16;

        // The largest float: farther than any edge.
        inline ee::Global<ee::Float, sym::g_linkFarthest> g_linkFarthest;
    }

    // A link edge filled in for a side of a face and its tag record.
    void fillLinkEdge(uint64_t node, int32_t face, uint32_t side, uint32_t tagRecord, ee::Ptr<LinkEdge> edge)
    {
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> across;
        side &= 0xFFu;
        edge->tagRecord = tagRecord;
        edge->side = static_cast<uint8_t>(side);
        edge->face = static_cast<uint32_t>(face);
        vertexWorld(node, face, side, &edge->start.x);
        const int32_t sides = faceSideCount(node, face);
        vertexWorld(node, face, static_cast<uint32_t>(static_cast<int32_t>(side + 1) % sides) & 0xFFu, &edge->end.x);
        edge->direction = edge->end - edge->start;
        edge->direction = zge::unit(edge->direction);

        normal->x = 0.0f; // down (its w is left as it was)
        normal->y = -1.0f;
        normal->z = 0.0f;
        normal = zge::cross(*normal, edge->direction);
        normal = zge::unit(*normal);
        zge::planeThrough(&edge->firstPlane, &edge->start, normal);
        across = zge::cross(*normal, edge->direction);
        zge::planeThrough(&edge->secondPlane, &edge->start, across);
    }
    READABLE_FUNCTION(fillLinkEdge, Verified);

    // The link edge for a face: of the `count` edges whose link kind is the
    // face's surface kind, the one nearest the face's centre, within the
    // link reach, whose wedge the face lies in. Its face and side go to
    // `faceOut` and `sideOut`; `faceOut` gets kNoFace when there is none
    // (`sideOut` is then left alone).
    void nearestLinkEdge(uint64_t node, int32_t face, uint32_t faceRecord, ee::Ptr<LinkEdge> edges, uint32_t count,
                         ee::Ptr<uint32_t> faceOut, ee::Ptr<uint8_t> sideOut)
    {
        ee::Local<zge::Vec4[kMaxCorners]> corners;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> toEdge; // the edge's point nearest the centre, then the way to it
        ee::Local<uint32_t> edgeCount;
        ee::Local<ee::Ptr<uint8_t>> sideTo;
        edgeCount = count;
        sideTo = sideOut;
        *faceOut = kNoFace;
        const ee::Float reach = linkReach();
        const ee::Float reachSquared = reach * reach;
        ee::Float nearest = *g_linkFarthest; // squared
        const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, face));
        for (uint32_t corner = 0; corner < sides; corner = (corner + 1) & 0xFFu)
            vertexWorld(node, face, corner, &corners[corner].x);
        centroid(sides, corners, centre);
        faceNormalWorld(node, face, &normal->x);
        const uint32_t kind = tagSurfaceKind(faceRecord);

        const ee::Ptr<LinkEdge> end = edges + static_cast<ptrdiff_t>(*edgeCount);
        for (ee::Ptr<LinkEdge> edge = edges; edge < end; edge = edge + 1)
        {
            if (kind != tagLinkKind(edge->tagRecord))
                continue;
            zge::closestPointOnSegment(&edge->start, &edge->end, &edge->direction, centre, toEdge);
            toEdge = *toEdge - *centre;
            const ee::Float squared = engine::lengthSquared(&toEdge->x);
            if (!(squared < nearest) || !(squared < reachSquared))
                continue;
            if (!faceInEdgeWedge(sides, corners, normal, &edge->firstPlane, &edge->secondPlane))
                continue;
            *faceOut = edge->face;
            nearest = squared;
            **sideTo = edge->side;
        }
    }
    READABLE_FUNCTION(nearestLinkEdge, Verified);

    // forEachNode's callback for the level's linking. Always 1.
    uint32_t linkNodeFaces(uint64_t node)
    {
        ee::Local<uint8_t> linkedSide;
        ee::Local<uint32_t> linkedFace;
        ee::Local<uint32_t> edges; // the node's link edges, from the main region
        ee::Local<uint32_t> faces;
        ee::Local<uint32_t> surfaceTagged;

        // How many sides take faces.
        uint32_t edgeCount = 0;
        faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < *faces; ++face)
        {
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint32_t side = 0; side < sides; side = (side + 1) & 0xFFu)
            {
                const uint32_t edgeRecord = edgeTags(node, static_cast<int32_t>(face), side);
                if (edgeRecord && tagLinkKind(edgeRecord))
                    ++edgeCount;
            }
        }
        if (edgeCount)
            edges = zge::allocateMemory(edgeCount * static_cast<uint32_t>(sizeof(LinkEdge)));
        else
            edges = 0;

        // Every such side becomes a link edge; a face of a surface kind
        // loses its old link and takes the first of its own sides whose
        // link kind is its kind.
        edgeCount = 0;
        for (uint32_t face = 0; face < *faces; ++face)
        {
            const uint32_t faceRecord = faceTags(node, static_cast<int32_t>(face));
            surfaceTagged = 0;
            if (faceRecord && tagSurfaceKind(faceRecord))
            {
                surfaceTagged = 1;
                clearTagLink(faceRecord);
            }
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint32_t side = 0; side < sides; side = (side + 1) & 0xFFu)
            {
                const uint32_t edgeRecord = edgeTags(node, static_cast<int32_t>(face), side);
                if (!edgeRecord || !tagLinkKind(edgeRecord))
                    continue;
                fillLinkEdge(node, static_cast<int32_t>(face), side, edgeRecord,
                             ee::Ptr<LinkEdge>(*edges) + static_cast<ptrdiff_t>(edgeCount));
                ++edgeCount;
                if (*surfaceTagged && tagSurfaceKind(faceRecord) == tagLinkKind(edgeRecord) && !tagHasLink(faceRecord))
                    linkFaceToEdge(faceRecord, node, face, side);
            }
        }

        // The faces still without a link take the nearest edge that fits.
        for (uint32_t face = 0; face < *faces; ++face)
        {
            const uint32_t faceRecord = faceTags(node, static_cast<int32_t>(face));
            if (!faceRecord || tagHasLink(faceRecord) || !tagSurfaceKind(faceRecord) || !*edges)
                continue;
            linkedFace = kNoFace;
            nearestLinkEdge(node, static_cast<int32_t>(face), faceRecord, ee::Ptr<LinkEdge>(*edges), edgeCount,
                            linkedFace, linkedSide);
            if (*linkedFace != kNoFace)
                linkFaceToEdge(faceRecord, node, *linkedFace, *linkedSide);
        }

        if (*edges)
            zge::freeMemory(*edges);
        return 1;
    }
    READABLE_FUNCTION(linkNodeFaces, Verified);
}
