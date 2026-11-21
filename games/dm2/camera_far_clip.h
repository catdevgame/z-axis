#pragma once

// The draw distance DM2's level camera sets (camera_far_clip.cpp).

#include "ee/ee.h"
#include "zge/scene/scene.h"

namespace game::dm2
{
    // A scene node's camera's far plane, the draw distance.
    void setSceneFarClip(ee::Ptr<zge::SceneObject> object, ee::Float distance);
}
