// BMX XXX's faces and edges in world space: a node's collision mesh and its
// face count, a face's corners, normal and centre, an edge's ends and
// direction, and the walk over the level's nodes. DM2's (games/dm2/rider/
// collision/faces.cpp); BMX XXX's own steps are said where they are.
// collision.h has the handles.

#include "collision.h"

#include "../engine_calls.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/plane.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "sym/bmx/rider/collision/faces.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        // forEachNode's adaptor: calls the walk's callback with the node's
        // handle and the walk's argument.
        constexpr uint32_t kNodeVisitor = sym::nodeVisitor;
        constexpr uint32_t kSubtreeNodeVisitor = sym::subtreeNodeVisitor;

        // The largest float: faceShortestSide's start.
        inline ee::Global<ee::Float, sym::g_floatMax> g_floatMax;

        // 1 / 3, for faceCentre.
        inline ee::Global<ee::Float, sym::g_oneThird> g_oneThird;

        // nodeMatrix's world matrix.
        constexpr uint32_t kWorldMatrix = 1u;

        // A walk's callback and its argument, in the walker's frame.
        struct Visit
        {
            uint32_t callback;
            uint32_t arg;
        };

        ee::Ptr<zge::Node> nodeRecord(uint64_t node) { return ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(node))); }
    }

    // A node as a handle, and a handle's node: both the same value.
    uint64_t nodeHandle(uint64_t node)
    {
        return node;
    }
    READABLE_FUNCTION(nodeHandle, Verified);

    uint64_t nodeOf(uint64_t node)
    {
        return node;
    }
    READABLE_FUNCTION(nodeOf, Verified);

    // A node's collision mesh (its collision frame's part).
    uint32_t nodeCollisionMesh(uint64_t node)
    {
        return nodeRecord(node)->lods->meshWord;
    }
    READABLE_FUNCTION(nodeCollisionMesh, Verified);

    // How many faces a node's collision mesh has, 0 with none. BMX XXX's own
    // (DM2 has no such test): the layer makes a face or edge handle only
    // for a node this says has faces.
    uint32_t nodeFaceCount(uint64_t node)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        if (!mesh)
            return 0;
        return mesh->faceCount;
    }
    READABLE_FUNCTION(nodeFaceCount, Verified);

    // A face's normal in world space: the mesh's normal turned by the
    // node's matrix (no translation).
    void faceNormalWorld(uint64_t node, int32_t face, ee::Float *normal)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        zge::faceNormal(mesh, static_cast<uint32_t>(face), ee::Ptr<zge::Vec4>(vec4(normal)));
        const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(nodeRecord(node), kWorldMatrix);
        *vec4(normal) = zge::rotated(*world, *vec4(normal));
    }
    READABLE_FUNCTION(faceNormalWorld, Verified);

    // How many sides a face has (the collision faces are triangles).
    int32_t faceSideCount(uint64_t node, int32_t face)
    {
        (void)node;
        (void)face;
        return 3;
    }
    READABLE_FUNCTION(faceSideCount, Verified);

    // A face's corner in world space: the mesh's vertex through the node's
    // matrix (BMX XXX transforms it inline, DM2 calls the engine).
    void vertexWorld(uint64_t node, int32_t face, uint32_t corner, ee::Float *point)
    {
        corner &= 0xFFu;
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        ee::Local<ee::Ptr<uint16_t>> vertices; // the face's vertex indices
        zge::faceVertices(mesh, static_cast<uint32_t>(face), vertices);
        const uint16_t vertex = (*vertices)[corner];
        zge::vertexPosition(mesh, vertex, ee::Ptr<zge::Vec4>(vec4(point)));
        const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(nodeRecord(node), kWorldMatrix);
        *vec4(point) = zge::transformed(*world, *vec4(point));
    }
    READABLE_FUNCTION(vertexWorld, Verified);

    // The same by FaceRef.
    void faceVertex(FaceRef ref, uint32_t corner, ee::Float *point)
    {
        game::tail(vertexWorld, widen(refNode(ref)), faceIndex(ref), corner & 0xFFu, point);
    }
    READABLE_FUNCTION(faceVertex, Verified);

    void faceNormal(FaceRef ref, ee::Float *normal)
    {
        game::tail(faceNormalWorld, widen(refNode(ref)), faceIndex(ref), normal);
    }
    READABLE_FUNCTION(sym::faceNormalOf, faceNormal, Verified);

    // One end of an edge (end 0 its side's first corner, else the next,
    // round the face).
    void edgePoint(EdgeRef edge, uint32_t end, ee::Float *point)
    {
        const uint64_t node = widen(refNode(edge));
        const int32_t face = edgeFace(edge);
        uint32_t corner = edgeSide(edge);
        if (end)
        {
            const int32_t sides = faceSideCount(node, face);
            corner = static_cast<uint32_t>((static_cast<int32_t>(corner) + 1) % sides) & 0xFFu;
        }
        game::tail(vertexWorld, node, face, corner, point);
    }
    READABLE_FUNCTION(edgePoint, Verified);

    // An edge's unit direction (first end to second) into `direction`; its
    // length. BMX XXX clears the difference's w before it is made a unit.
    ee::Float edgeDirection(EdgeRef edge, ee::Float *direction)
    {
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> end;
        edgePoint(edge, 0u, &start->x);
        edgePoint(edge, 1u, &end->x);
        *vec4(direction) = *end - *start;
        direction[3] = 0.0f;
        ee::Float length;
        *vec4(direction) = zge::unit(*vec4(direction), length);
        return length;
    }
    READABLE_FUNCTION(edgeDirection, Verified);

    // The upright plane through an edge: its normal the edge's direction
    // crossed with down (0, -1, 0; y grows downwards), through its first
    // end. Returns 1; 0, leaving the plane as it was, for an edge that goes
    // straight up or down (no run along x or z), where DM2 makes no test.
    int32_t edgePlane(EdgeRef edge, ee::Float *plane)
    {
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> normal;
        edgeDirection(edge, &direction->x);
        if (direction->x == 0.0f && direction->z == 0.0f)
            return 0;
        normal->x = 0.0f; // down (its w is left as it was)
        normal->y = -1.0f;
        normal->z = 0.0f;
        normal = zge::cross(*normal, *direction);
        normal = zge::unit(*normal);
        edgePoint(edge, 0u, &start->x);
        zge::planeThrough(vec4(plane), start, normal);
        return 1;
    }
    READABLE_FUNCTION(edgePlane, Verified);

    // A face's shortest side (the least squared length's root).
    ee::Float faceShortestSide(FaceRef ref)
    {
        ee::Local<zge::Vec4> side;
        ee::Local<zge::Vec4[3]> corners;
        for (uint32_t i = 0; i < 3; ++i)
            faceVertex(ref, i, &corners[i].x);
        ee::Float shortest = *g_floatMax; // squared
        for (uint8_t i = 0; i < 3; ++i)
        {
            side = corners[(i + 1) % 3] - corners[i];
            const ee::Float squared = zge::dot(*side, *side);
            if (squared < shortest)
                shortest = squared;
        }
        return ee::sqrt(shortest);
    }
    READABLE_FUNCTION(faceShortestSide, Verified);

    // A face's centre (the mean of its corners).
    void faceCentre(FaceRef ref, ee::Float *centre)
    {
        ee::Local<ee::Float[4]> corners[3];
        for (uint32_t i = 0; i < 3; ++i)
            faceVertex(ref, i, corners[i]);
        zge::Vec4 &sum = *vec4(centre);
        sum = *vec4(corners[0]) + *vec4(corners[1]);
        sum = sum + *vec4(corners[2]);
        // The scale carries what this function's caller left beside the
        // third, as DM2's does (games/dm2/rider/collision/faces.cpp).
        sum = zge::scaledCarrying(sum, *g_oneThird, zge::scaleLeftover(zge::LeftBy::caller));
    }
    READABLE_FUNCTION(faceCentre, Verified);

    // A filter that keeps everything.
    int32_t acceptAll()
    {
        return 1;
    }
    READABLE_FUNCTION(acceptAll, Verified);

    // ---- walks over the level's nodes

    // Calls callback(node, arg) for every node of the level (the engine's
    // walk with the game's adaptor).
    void forEachNode(uint32_t callback, ee::Address arg)
    {
        ee::Local<Visit> visit;
        visit->callback = callback;
        visit->arg = arg;
        zge::forEachWorldMesh(kNodeVisitor, visit);
    }
    READABLE_FUNCTION(forEachNode, Verified);

    // forEachNode's adaptor, which the engine's walk calls for each node of the world's meshes: in BMX XXX every
    // node of its subtree is visited (subtreeNodeVisitor), the walk's answer returned. DM2's adaptor calls the
    // callback itself.
    int32_t nodeVisitor(uint64_t node, ee::Address walk)
    {
        return zge::forEachNodeInSubtree(ee::Ptr<zge::Node>(static_cast<uint32_t>(node)), kSubtreeNodeVisitor, walk);
    }
    READABLE_FUNCTION(nodeVisitor, Verified);

    // The subtree walk's visit: the walk's callback with the node's handle and the walk's argument (its answer
    // returned). DM2's nodeVisitor.
    int32_t subtreeNodeVisitor(uint64_t node, ee::Address walk)
    {
        const ee::Ptr<Visit> visit(walk);
        const uint32_t arg = visit->arg;
        const ee::FunctionPtr<int32_t(uint64_t, uint32_t)> callback(visit->callback);
        return callback(nodeHandle(node), arg);
    }
    READABLE_FUNCTION(subtreeNodeVisitor, Verified);

    namespace
    {
        inline ee::Global<NodeFace, sym::g_nodeFace> g_nodeFace;
        const ee::Ptr<NodeFaceCorner> kNodeFaceCorners(sym::g_nodeFaceCorners);
    }

    // forEachNodeFace (node, visit, context): each face of a node's object's collision mesh (its class's frame's
    // first level), as a face record in the world (the node's world matrix), handed to visit(node, face,
    // context) until it returns other than 1. A face with a corner past the mesh's vertices is skipped. Nothing
    // without a frame or a level.
    void forEachNodeFace(uint64_t node, NodeFaceVisit visit, ee::Address context)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> position;
        ee::Local<ee::Float[4]> uv;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        const ee::Ptr<zge::CollisionFrame> frame = zge::classModel(nodeObjectClass(node), 0);
        if (!frame || !zge::frameLevels(frame))
            return;
        const ee::Ptr<zge::ZgePart> mesh = zge::framePartMesh(zge::frameLevelPart(frame, 0));
        const ee::Ptr<zge::MaterialTable> materials = zge::meshMaterials(mesh, 0);
        *world = *zge::nodeMatrix(ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(widen(static_cast<uint32_t>(node))))), kWorldMatrix);
        const uint32_t faces = mesh->faceCount;
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint8_t count = static_cast<uint8_t>(zge::faceVertices(mesh, face, vertices));
            bool whole = true;
            for (uint8_t i = 0; i < count; ++i)
            {
                if (!((*vertices)[i] < mesh->vertexCount))
                {
                    whole = false;
                    break;
                }
                NodeFaceCorner &corner = kNodeFaceCorners[i];
                zge::vertexPosition(mesh, (*vertices)[i], position);
                corner.position = zge::transformed(*world, *position);
                zge::vertexColour(mesh, (*vertices)[i], ee::Ptr<zge::VertexColour>(&corner.colour));
                zge::vertexUv(mesh, (*vertices)[i], ee::Ptr<zge::Float4>(uv.address()));
                corner.u = uv[0];
                corner.v = uv[1];
            }
            if (!whole)
                continue;
            NodeFace &record = *g_nodeFace;
            record.cornerCount = count;
            record.corners = kNodeFaceCorners;
            zge::faceNormal(mesh, face, normal);
            record.normal = zge::rotated(*world, *normal);
            record.material = materials->materials[zge::faceMaterial(mesh, face)];
            if (visit(static_cast<uint32_t>(node), ee::Ptr<NodeFace>(g_nodeFace.get()), context) != 1)
                return;
        }
    }
    READABLE_FUNCTION(forEachNodeFace, Verified);

    namespace
    {
        // nodeCollisionBounds' start: every lane at the float's most, the low ends up and the high ends down.
        inline ee::Global<ee::Float, sym::g_boundsLowStart> g_boundsLowStart;   // 3.4e38
        inline ee::Global<ee::Float, sym::g_boundsHighStart> g_boundsHighStart; // -3.4e38
    }

    // nodeCollisionBounds (node, bounds): the box round a node's collision mesh's corners (each face's), in its
    // node's space, then each end put through the node's world matrix.
    void nodeCollisionBounds(uint64_t node, NodeBounds *bounds)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<zge::Vec4> corner;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        ee::Float *const low = bounds->low;
        ee::Float *const high = bounds->high;
        const ee::Float lowStart = *g_boundsLowStart, highStart = *g_boundsHighStart;
        low[2] = lowStart;
        high[2] = highStart;
        low[0] = lowStart;
        low[1] = lowStart;
        high[0] = highStart;
        high[1] = highStart;
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        const uint32_t faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint32_t i = 0; i < sides; ++i)
            {
                zge::faceVertices(mesh, face, vertices);
                zge::vertexPosition(mesh, (*vertices)[i], corner);
                low[0] = ee::min(corner->x, low[0]);
                low[1] = ee::min(corner->y, low[1]);
                low[2] = ee::min(corner->z, low[2]);
                high[0] = ee::max(corner->x, high[0]);
                high[1] = ee::max(corner->y, high[1]);
                high[2] = ee::max(corner->z, high[2]);
            }
        }
        *world = *zge::nodeMatrix(ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(node))), kWorldMatrix);
        *vec4(low) = zge::transformed(*world, *vec4(low));
        *vec4(high) = zge::transformed(*world, *vec4(high));
    }
    READABLE_FUNCTION(nodeCollisionBounds, Verified);
}
