#pragma once

// A camera node's scene camera, set from BMX XXX's camera code (camera.c's block;
// scene_camera.cpp): the clear colour, the fog's colour and the far plane
// (setSceneClears waits for the engine's setCameraClears to have a body). Aggressive Inline has the same wrappers
// (game/aggressive/camera.cpp: setSceneClearColour, setSceneFogColour;
// camera_far_clip.cpp: setSceneFarClip); DM2's are game/dm2's.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // A camera node's scene word (sceneOf), which holds its camera.
    inline ee::Ptr<zge::Camera> objectCamera(ee::Ptr<zge::SceneObject> object) { return zge::sceneOf(object).cast<zge::Camera>(); }

    // `colour`: the address of its R, G, B, A.
    void setSceneClearColour(ee::Ptr<zge::SceneObject> object, ee::Address colour);
    void setSceneFogColour(ee::Ptr<zge::SceneObject> object, ee::Address colour);
    // Its far plane, the draw distance.
    void setSceneFarClip(ee::Ptr<zge::SceneObject> object, ee::Float distance);
}
