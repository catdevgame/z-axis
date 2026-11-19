// DM2's walk of a node's model face by face in the world (node_faces.h). DM2's
// own code (the collision layer, game.c's link range).

#include "node_faces.h"

#include "collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"

#include <cstdint>
#include "sym/dm2/rider/collision/node_faces.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        // The face the walk hands on, and its corners (as many as the face has).
        inline ee::Global<NodeFace, sym::g_walkedFace> g_walkedFace;
        constexpr ee::Ptr<NodeFaceVertex> kWalkedFaceVertices{sym::g_walkedFaceVertices};
        // The node matrix the walk takes (the world's).
        constexpr uint32_t kWorldMatrix = 1;
    }

    // forEachNodeFace: the node's model's first level of detail walked face
    // by face: each face's corners (through the node's world matrix), their
    // colours and texture coordinates, its normal (turned by the matrix) and
    // material, handed to the visit, until a visit answers other than 1.
    // Nothing for a node with no model or a model with no levels.
    void forEachNodeFace(uint64_t node, NodeFaceVisit visit, ee::Address context)
    {
        ee::Local<zge::Matrix> world;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> uv;
        ee::Local<ee::Ptr<uint16_t>> indices;
        const uint32_t nodeWord = static_cast<uint32_t>(node);
        const ee::Ptr<zge::CollisionFrame> model = zge::classModel(nodeObjectClass(node), 0);
        if (!model || zge::frameLevels(model) == 0)
            return;
        const ee::Ptr<zge::ZgePart> mesh = zge::framePartMesh(zge::frameLevelPart(model, 0));
        const ee::Ptr<zge::MaterialTable> table = zge::meshMaterials(mesh, 0);
        const ee::Ptr<ee::Ptr<zge::Material>> materials(&table->materials[0]);
        zge::copyRotationAndTranslation(world, zge::nodeMatrix(ee::Ptr<zge::Node>(nodeOf(nodeWord)), kWorldMatrix));
        NodeFace &face = *g_walkedFace;
        const uint32_t faces = mesh->faceCount;
        for (uint32_t index = 0; index < faces; ++index)
        {
            const uint8_t corners = static_cast<uint8_t>(zge::faceVertices(mesh, index, indices));
            for (uint8_t i = 0; i < corners; ++i)
            {
                const ee::Ptr<NodeFaceVertex> vertex = kWalkedFaceVertices + i;
                zge::vertexPosition(mesh, (*indices)[i], position);
                engine::transformPoint(&vertex->position.x, world, &position->x);
                zge::partColour(mesh, (*indices)[i], &vertex->colour);
                zge::partUv(mesh, (*indices)[i], uv);
                vertex->u = uv->x;
                vertex->v = uv->y;
            }
            face.vertexCount = corners;
            face.vertices = kWalkedFaceVertices;
            zge::faceNormal(mesh, index, normal);
            face.normal = zge::rotated(*world, *normal);
            face.material = ee::Address(materials[zge::faceMaterial(mesh, index)]);
            if (visit(nodeWord, ee::Ptr<NodeFace>(sym::g_walkedFace), context) != 1)
                break;
        }
    }
    READABLE_FUNCTION_VU0(forEachNodeFace, Verified);
}
