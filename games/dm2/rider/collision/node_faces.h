#pragma once
// DM2's walk of a node's model face by face in the world (the collision
// layer's, node_faces.cpp): each face handed to a visit with its corners,
// normal and material. The goals' object targets draw from it (BMX XXX's
// level objects that break throw a piece from every face: its node_faces.h).

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "zge/math/types.h"
#include "zge/render/part.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2::rider::collision
{
    // A corner of a walked face.
    struct NodeFaceVertex
    {
        zge::Vec4 position;       // +0x00 in the world
        ee::Float u, v;           // +0x10 its texture coordinates
        zge::VertexColour colour; // +0x18
        uint32_t _1c;
    };
    static_assert(sizeof(NodeFaceVertex) == 0x20);

    // A face as the walk hands it to its visit (one record, reused).
    struct NodeFace
    {
        zge::Vec4 normal;                 // +0x00 in the world
        uint32_t material;                // +0x10 the face's material
        uint8_t vertexCount;              // +0x14
        uint8_t _15[3];
        ee::Ptr<NodeFaceVertex> vertices; // +0x18
    };
    static_assert(offsetof(NodeFace, material) == 0x10 && offsetof(NodeFace, vertexCount) == 0x14 && offsetof(NodeFace, vertices) == 0x18);

    // A visit: 1 to go on to the next face, anything else ends the walk.
    using NodeFaceVisit = ee::FunctionPtr<uint32_t(uint64_t node, ee::Ptr<NodeFace> face, ee::Address context)>;
    // Every face of the node's model (its first level of detail) visited.
    void forEachNodeFace(uint64_t node, NodeFaceVisit visit, ee::Address context);
}
