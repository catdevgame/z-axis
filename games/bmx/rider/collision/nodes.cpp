// A node through the rider's collision layer (collision.h): its object
// class, whether it is shown and whether queries test it, and which tag
// streams its collision mesh has. BMX XXX's own code (bmx.c), DM2's line
// (games/dm2/rider/collision/faces.cpp).

#include "collision.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "sym/bmx/rider/collision/nodes.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        ee::Ptr<zge::Node> nodeRecord(uint64_t node) { return ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(node))); }
        // The node alone, not its subtree.
        constexpr uint32_t kNodeAlone = 0;

        // The walks' node visitors (DM2's names).
        constexpr uint32_t kTaggedFaceVisitor = sym::taggedFaceVisitor;
        constexpr uint32_t kTaggedEdgeVisitor = sym::taggedEdgeVisitor;
        using FaceCallback = ee::FunctionPtr<int32_t(uint32_t record, uint64_t node, int32_t face, uint32_t arg)>;
        using EdgeCallback = ee::FunctionPtr<int32_t(uint32_t record, uint64_t node, int32_t face, uint32_t side, uint32_t arg)>;
    }

    // The node's object class: BMX XXX reads the node's own word (DM2 asks
    // the engine's objectClass).
    ee::Ptr<zge::GeoObject> nodeObjectClass(uint64_t node)
    {
        return nodeRecord(node)->model;
    }
    READABLE_FUNCTION(nodeObjectClass, Verified);

    // The node shown or hidden (the node alone, not its subtree); the
    // engine's answer.
    uint32_t showNode(uint64_t node, uint32_t shown)
    {
        const ee::Ptr<zge::Node> record = nodeRecord(node);
        return game::tail(zge::setNodeShown, record, shown, kNodeAlone);
    }
    READABLE_FUNCTION_WAITS(showNode, Verified);

    // Whether queries test the node (the node alone); the engine's answer.
    uint32_t setNodeCollidable(uint64_t node, uint32_t collides)
    {
        const ee::Ptr<zge::Node> record = nodeRecord(node);
        return game::tail(zge::setNodeCollides, record, collides, kNodeAlone);
    }
    READABLE_FUNCTION_WAITS(setNodeCollidable, Verified);

    // Whether the node's collision mesh has face tags (TGFT), edge tags (the
    // face edges, TGFE) and vertex tags (TGVT).
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

    bool nodeHasVertexTags(uint64_t node)
    {
        const ee::Ptr<zge::ZgePart> mesh(nodeCollisionMesh(node));
        return mesh && mesh->vertexTags;
    }
    READABLE_FUNCTION(nodeHasVertexTags, Verified);

    int32_t taggedFaceVisitor(uint64_t node, const TaggedWalk *walk)
    {
        if (!nodeHasFaceTags(node))
            return 1;
        const uint32_t faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t record = faceTags(node, static_cast<int32_t>(face));
            if (!record)
                continue;
            if (FaceCallback(walk->callback)(record, node, static_cast<int32_t>(face), walk->arg) != 1)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(taggedFaceVisitor, Verified);

    int32_t taggedEdgeVisitor(uint64_t node, const TaggedWalk *walk)
    {
        if (!nodeHasEdgeTags(node))
            return 1;
        const uint32_t faces = nodeFaceCount(node);
        for (uint32_t face = 0; face < faces; ++face)
        {
            const uint32_t sides = static_cast<uint32_t>(faceSideCount(node, static_cast<int32_t>(face)));
            for (uint8_t side = 0; side < sides; ++side)
            {
                const uint32_t record = edgeTags(node, static_cast<int32_t>(face), side);
                if (!record)
                    continue;
                if (EdgeCallback(walk->callback)(record, node, static_cast<int32_t>(face), side, walk->arg) != 1)
                    return 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(taggedEdgeVisitor, Verified);

    // Every node's tagged faces, and every node's tagged edges, handed to a
    // callback with its argument.
    void forEachTaggedFace(uint32_t callback, uint32_t arg)
    {
        ee::Local<TaggedWalk> walk;
        walk->callback = callback;
        walk->arg = arg;
        forEachNode(kTaggedFaceVisitor, walk);
    }
    READABLE_FUNCTION_WAITS(forEachTaggedFace, Verified);

    void forEachTaggedEdge(uint32_t callback, uint32_t arg)
    {
        ee::Local<TaggedWalk> walk;
        walk->callback = callback;
        walk->arg = arg;
        forEachNode(kTaggedEdgeVisitor, walk);
    }
    READABLE_FUNCTION_WAITS(forEachTaggedEdge, Verified);
}
