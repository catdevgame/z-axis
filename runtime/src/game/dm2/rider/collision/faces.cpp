// DM2's faces and edges in world space: a node's
// collision mesh, a face's corners, normal, centre and shortest side, an
// edge's ends, direction and plane, and the walks over the level's nodes.
// collision.h has the handles.

#include "collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/math/plane.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/scene/node.h"
#include "sym/dm2/rider/collision/faces.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        // forEachNode's adaptor: calls the walk's callback with the node's
        // handle and the walk's argument.
        constexpr uint32_t kNodeVisitor = sym::nodeVisitor;
        // The tagged face and edge walks' visitors.
        constexpr uint32_t kTaggedFaceVisitor = sym::taggedFaceVisitor;
        constexpr uint32_t kTaggedEdgeVisitor = sym::taggedEdgeVisitor;
        // collectTaggedFaces' and collectTaggedEdges' callbacks: keep a face
        // (edge) whose tag word is in the list.
        constexpr uint32_t kCollectFace = sym::collectFace;
        constexpr uint32_t kCollectEdge = sym::collectEdge;

        // The longest side faceMinEdgeLength starts from (FLT_MAX).
        inline ee::Global<ee::Float, sym::g_floatMax> g_floatMax;
        // 1 / 3, for faceCentre.
        inline ee::Global<ee::Float, sym::g_oneThird> g_oneThird;

        // The collected faces and edges, ended by a 0, and their counts.
        inline ee::Global<uint64_t, sym::g_taggedFaces> g_taggedFaces; // 0x400 slots
        inline ee::Global<uint64_t, sym::g_taggedEdges> g_taggedEdges; // 0x100 slots
        inline ee::Global<int32_t, sym::g_taggedFaceCount> g_taggedFaceCount;
        inline ee::Global<int32_t, sym::g_taggedEdgeCount> g_taggedEdgeCount;
        constexpr int32_t kTaggedFaceCapacity = 0x3FF;
        constexpr int32_t kTaggedEdgeCapacity = 0xFF;

        // nodeMatrix's world matrix.
        constexpr uint32_t kWorldMatrix = 1u;

        // A walk's callback and its argument, in the walker's frame.
        struct Visit
        {
            uint32_t callback;
            uint32_t arg;
        };
    }

    // A node as a handle, and a handle's node: both the same value in DM2's
    // PS2 build (the GameCube's are the same).
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
        const ee::Ptr<zge::Node> n(static_cast<uint32_t>(nodeOf(node)));
        return n->lods->meshWord;
    }
    READABLE_FUNCTION(nodeCollisionMesh, Verified);

    // Whether queries test the node (setNodeCollidable's flag); the engine's
    // answer.
    uint32_t nodeCollidable(uint64_t node)
    {
        return engine::nodeCollides(nodeOf(node));
    }
    READABLE_FUNCTION(nodeCollidable, Verified);

    // ---- a node's object, mesh and flags

    // The node's object class (the engine's objectClass).
    ee::Ptr<zge::GeoObject> nodeObjectClass(uint64_t node)
    {
        return zge::objectClass(ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(node))));
    }
    READABLE_FUNCTION(nodeObjectClass, Verified);

    // The node shown or hidden (the node alone, not its subtree); the
    // engine's answer.
    uint32_t showNode(uint64_t node, uint32_t shown)
    {
        const ee::Ptr<zge::Node> n(static_cast<uint32_t>(nodeOf(node)));
        return game::tail(zge::setNodeShown, n, shown, 0u);
    }
    READABLE_FUNCTION_WAITS(showNode, Verified);

    // Whether queries test the node (the node alone); the engine's answer.
    uint32_t setNodeCollidable(uint64_t node, uint32_t collides)
    {
        const ee::Ptr<zge::Node> n(static_cast<uint32_t>(nodeOf(node)));
        return game::tail(zge::setNodeCollides, n, collides, 0u);
    }
    READABLE_FUNCTION_WAITS(setNodeCollidable, Verified);

    // The faces of the node's collision mesh (0 with none).
    uint16_t nodeFaceCount(uint64_t node)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        if (!mesh)
            return 0;
        return mesh->faceCount;
    }
    READABLE_FUNCTION(nodeFaceCount, Verified);

    // Whether the node's collision mesh has face tags (TGFT), and edge tags
    // (the face edges, TGFE).
    bool nodeHasFaceTags(uint64_t node)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        return mesh && mesh->faceTags;
    }
    READABLE_FUNCTION(nodeHasFaceTags, Verified);

    bool nodeHasEdgeTags(uint64_t node)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        return mesh && mesh->faceEdges;
    }
    READABLE_FUNCTION(nodeHasEdgeTags, Verified);

    // A face's normal in world space: the mesh's normal turned by the
    // node's matrix (no translation).
    void faceNormalWorld(uint64_t node, int32_t face, ee::Float *normal)
    {
        const uint32_t mesh = nodeCollisionMesh(node);
        zge::faceNormal(ee::Ptr<zge::ZgePart>(mesh), static_cast<uint32_t>(face), vec4(normal));
        const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(ee::Ptr<zge::Node>(nodeOf(node)), kWorldMatrix);
        *vec4(normal) = zge::rotated(*world, *vec4(normal));
    }
    READABLE_FUNCTION(faceNormalWorld, Verified);

    // How many sides a face has (DM2's collision faces are triangles).
    int32_t faceSideCount(uint64_t node, int32_t face)
    {
        (void)node;
        (void)face;
        return 3;
    }
    READABLE_FUNCTION(sym::edgeHandle, faceSideCount, Verified);

    // A face's corner in world space: the mesh's vertex by the node's
    // matrix.
    void vertexWorld(uint64_t node, int32_t face, uint32_t corner, ee::Float *point)
    {
        corner &= 0xFFu;
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        ee::Local<ee::Ptr<uint16_t>> vertices; // the face's vertex indices
        zge::faceVertices(mesh, face, vertices);
        const uint16_t vertex = (*vertices)[corner];
        zge::vertexPosition(mesh, vertex, vec4(point));
        const ee::Ptr<zge::Matrix> world = zge::nodeMatrix(ee::Ptr<zge::Node>(nodeOf(node)), kWorldMatrix);
        engine::transformPoint(point, world, point);
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

    // One end of an edge (end 0 its side's first corner, else the next).
    void edgePoint(EdgeRef edge, uint32_t end, ee::Float *point)
    {
        const uint64_t node = widen(refNode(edge));
        const int32_t face = edgeFace(edge);
        uint32_t corner = edgeSide(edge);
        if (end)
        {
            const int32_t sides = faceSideCount(node, face);
            corner = corner == static_cast<uint32_t>(sides - 1) ? 0u : (corner + 1u) & 0xFFu;
        }
        game::tail(vertexWorld, node, face, corner, point);
    }
    READABLE_FUNCTION(sym::edgePoints, edgePoint, Verified);

    // An edge's unit direction (first end to second) into `direction`; its
    // length.
    ee::Float edgeDirection(EdgeRef edge, ee::Float *direction)
    {
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> end;
        edgePoint(edge, 0u, &start->x);
        edgePoint(edge, 1u, &end->x);
        ee::Float length;
        *vec4(direction) = zge::unit(*end - *start, length);
        return length;
    }
    READABLE_FUNCTION(sym::edgeOfHit, edgeDirection, Verified);

    // The upright plane through an edge: its normal the edge's direction
    // crossed with down (0, -1, 0; y grows downwards), through its first
    // end.
    void edgePlane(EdgeRef edge, ee::Float *plane)
    {
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> normal;
        edgePoint(edge, 0u, &start->x);
        edgeDirection(edge, &direction->x);
        normal->x = 0.0f; // down (its w is left as it was)
        normal->y = -1.0f;
        normal->z = 0.0f;
        normal = zge::cross(*normal, *direction);
        normal = zge::unit(*normal);
        zge::planeThrough(vec4(plane), start, normal);
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
        for (uint32_t i = 0; i < 3; ++i)
        {
            side = corners[(i + 1) % 3] - corners[i];
            const ee::Float squared = engine::lengthSquared(&side->x);
            if (squared < shortest)
                shortest = squared;
        }
        return ee::sqrt(shortest);
    }
    READABLE_FUNCTION(sym::isGroundFace, faceShortestSide, Verified);

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
        // third (the corners' lookups leave it alone), and DM2's later vector
        // code finds it: in pq-woodward 0x80 bytes a skinned model's vertices
        // carry on (the gate's 40 heap words while they were 0).
        sum = zge::scaledCarrying(sum, *g_oneThird, zge::scaleLeftover(zge::LeftBy::caller));
    }
    READABLE_FUNCTION(sym::faceNormalAt, faceCentre, Verified);

    // A filter that keeps everything.
    int32_t acceptAll()
    {
        return 1;
    }
    READABLE_FUNCTION(sym::alwaysTrue, acceptAll, Verified);

    // ---- walks over the level's nodes

    // Calls callback(node, arg) for every node of the level (the engine's
    // walk with DM2's adaptor).
    void forEachNode(uint32_t callback, ee::Address arg)
    {
        ee::Local<Visit> visit;
        visit->callback = callback;
        visit->arg = arg;
        zge::forEachWorldMesh(kNodeVisitor, visit); // callback(node, arg) for every mesh node of the world
    }
    READABLE_FUNCTION(sym::terrainTag, forEachNode, Verified);

    // forEachNode's adaptor, which the engine's walk calls for each node: the
    // walk's callback with the node's handle and the walk's argument (its
    // answer returned).
    int32_t nodeVisitor(uint64_t node, ee::Address walk)
    {
        const ee::Ptr<Visit> visit(walk);
        const ee::FunctionPtr<int32_t(uint64_t, uint32_t)> callback(visit->callback);
        const uint32_t arg = visit->arg;
        return callback(nodeHandle(node), arg);
    }
    READABLE_FUNCTION(nodeVisitor, Verified);

    // Calls callback(tagRecord, node, face, arg) for every tagged face of
    // the level, until one returns other than 1.
    void forEachTaggedFace(uint32_t callback, ee::Address arg)
    {
        ee::Local<Visit> visit;
        visit->callback = callback;
        visit->arg = arg;
        forEachNode(kTaggedFaceVisitor, visit);
    }
    READABLE_FUNCTION(forEachTaggedFace, Verified);

    // Calls callback(tagRecord, node, face, side, arg) for every tagged
    // edge of the level, until one returns other than 1.
    void forEachTaggedEdge(uint32_t callback, ee::Address arg)
    {
        ee::Local<Visit> visit;
        visit->callback = callback;
        visit->arg = arg;
        forEachNode(kTaggedEdgeVisitor, visit);
    }
    READABLE_FUNCTION(forEachTaggedEdge, Verified);

    uint32_t faceTags(uint64_t node, int32_t face); // tags.cpp

    // forEachTaggedFace's node visitor: callback(tagRecord, node, face, arg)
    // for each face of the node with a tag record; 0 as soon as one answers
    // other than 1 (the walk stops), else 1.
    int32_t taggedFaceVisitor(uint64_t node, ee::Address walk)
    {
        const ee::Ptr<Visit> visit(walk);
        if (!nodeHasFaceTags(node))
            return 1;
        const uint32_t faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t record = faceTags(node, static_cast<int32_t>(face));
            if (!record)
                continue;
            const ee::FunctionPtr<int32_t(uint32_t, uint64_t, uint32_t, uint32_t)> callback(visit->callback);
            if (callback(record, node, face, visit->arg) != 1)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(taggedFaceVisitor, Verified);

    // forEachTaggedEdge's node visitor: callback(tagRecord, node, face,
    // side, arg) for each side with a tag record; 0 as soon as one answers
    // other than 1, else 1.
    int32_t taggedEdgeVisitor(uint64_t node, ee::Address walk)
    {
        const ee::Ptr<Visit> visit(walk);
        if (!nodeHasEdgeTags(node))
            return 1;
        const uint32_t faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint32_t side = 0; side < sides; side = (side + 1) & 0xFFu)
            {
                const uint32_t record = edgeTags(node, static_cast<int32_t>(face), side);
                if (!record)
                    continue;
                const ee::FunctionPtr<int32_t(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t)> callback(visit->callback);
                if (callback(record, node, face, side, visit->arg) != 1)
                    return 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(taggedEdgeVisitor, Verified);

    namespace
    {
        // The faces (edges) whose tag word is one of `count` words: into
        // the list, ended by a 0; the list, or 0 if none.
        uint64_t *collectTagged(ee::Address words, uint32_t count, int32_t capacity, uint64_t *list,
                                int32_t *found, uint32_t collector, bool edges)
        {
            ee::Local<NodeList> state;
            state->arg0 = words;
            state->arg1 = count;
            state->capacity = capacity;
            state->items = ee::Ptr<uint64_t>(list);
            state->count = 0;
            if (!edges)
                forEachTaggedFace(collector, state);
            else
                forEachTaggedEdge(collector, state);
            const int32_t n = state->count;
            *found = n;
            if (n == 0)
                return nullptr;
            list[n] = 0;
            return list;
        }
    }

    // collectTaggedFaces' callback (forEachTaggedFace's): the face kept if
    // its tag word is one of the list's words; 0, which stops the walk, once
    // the list is full.
    int32_t collectFace(uint32_t record, uint32_t node, int32_t face, NodeList *list)
    {
        if (list->capacity == list->count)
            return 0;
        const uint32_t tag = faceRecordTag(record);
        const ee::Ptr<uint32_t> words(list->arg0);
        for (uint32_t i = 0; i < list->arg1; ++i)
        {
            if (words[i] == tag)
            {
                list->items[static_cast<size_t>(list->count)] = makeFace(node, face);
                ++list->count;
                break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(collectFace, Verified);

    // collectTaggedEdges' callback (forEachTaggedEdge's): the edge kept if
    // its tag word is one of the list's words; 0, which stops the walk, once
    // the list is full.
    int32_t collectEdge(uint32_t record, uint32_t node, uint32_t face, uint32_t side, NodeList *list)
    {
        if (list->capacity == list->count)
            return 0;
        const uint32_t tag = edgeRecordTag(record);
        const ee::Ptr<uint32_t> words(list->arg0);
        for (uint32_t i = 0; i < list->arg1; ++i)
        {
            if (words[i] == tag)
            {
                list->items[static_cast<size_t>(list->count)] = makeEdge(node, face, static_cast<uint8_t>(side));
                ++list->count;
                break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(collectEdge, Verified);

    // The level's faces whose tag word (tagWordA) is one of `count` words,
    // as FaceRefs in the level's list (at most 0x3FF), ended by a 0; 0 if
    // none.
    uint64_t *collectTaggedFaces(const uint32_t *words, uint32_t count)
    {
        return collectTagged(words, count, kTaggedFaceCapacity, g_taggedFaces.get(),
                             g_taggedFaceCount.get(), kCollectFace, false);
    }
    READABLE_FUNCTION(collectTaggedFaces, Verified);

    // The same for edges (tagWordB), as EdgeRefs (at most 0xFF).
    uint64_t *collectTaggedEdges(const uint32_t *words, uint32_t count)
    {
        return collectTagged(words, count, kTaggedEdgeCapacity, g_taggedEdges.get(),
                             g_taggedEdgeCount.get(), kCollectEdge, true);
    }
    READABLE_FUNCTION(collectTaggedEdges, Verified);
}
