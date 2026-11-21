// World meshes and their models.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/scene/visibility_regions.h"
#include "zge/debug/debug.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/world_mesh.cpp.h"

namespace zge
{

    // (node): setupSceneModel(node, 1, 0), a forEachWorldMesh visitor; returns 1.
    uint32_t setupWorldMeshModel(ee::Ptr<Node> node)
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4's takes options and retains no node)
            setupSceneModelWithOptions(node, 0, ee::Ptr<Scene>());
        else
            setupSceneModel(node, 1, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupWorldMeshModel, Verified);

    int32_t matchWorldName(ee::Ptr<CollisionWorld> world)
    {
        return game::tail(addDebugText, &world->boundsMin, ee::Ptr<Matrix>(), sym::g_worldNameColour, 0);
    }
    READABLE_FUNCTION_WAITS(matchWorldName, Draft);

    namespace // the game functions below call: ee/plain.h
    {

        // Set on a node whose instance chunk named its region (by a mesh's
        // hash, in Node::region) until finishWorldMesh looks the region up
        // (geometry_nodes.cpp sets it).
        constexpr uint32_t kNodeRegionFixed = 1u << 18;
    }

    // setupWorldMeshModels: every world mesh's node made a scene
    // (setupWorldMeshModel). Returns the walk's result (a tail call).
    uint32_t setupWorldMeshModels()
    {
        return game::tail(forEachWorldMesh, sym::setupWorldMeshModel, 0);
    }
    READABLE_FUNCTION_WAITS(setupWorldMeshModels, Verified);

    // finishWorldMesh (a world mesh visitor): a node whose instance named its
    // region gets the region itself in place of the mesh's hash, and keeps
    // it from now on (kNodeKeepsRegion). 1.
    uint32_t finishWorldMesh(ee::Ptr<Node> node)
    {
        if (node->flags & kNodeRegionFixed)
        {
            node->flags = (node->flags & ~kNodeRegionFixed) | kNodeKeepsRegion;
            node->region = ee::Address(findRegionForMesh(node->region)); // the hash's word holds the region from now on
        }
        return 1;
    }
    READABLE_FUNCTION(finishWorldMesh, Verified);
}
