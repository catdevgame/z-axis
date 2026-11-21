#pragma once
// A level object's node, as the level objects show, hide and break it: the
// engine's objectLevelNode, and the collision layer's handle of it.

#include "dm2/rider/collision/collision.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/level/object_node.h.h"

namespace game::dm2
{
    // The collision layer's handle of the object's node (what showNode and
    // setNodeCollidable take).
    inline uint64_t objectNodeHandle(ee::Ptr<zge::SceneObject> object)
    {
        return rider::collision::nodeHandle(ee::Address(zge::objectLevelNode(object)));
    }
}
