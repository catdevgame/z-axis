#pragma once

// A camera node's scene camera, set from DM2's camera code (camera.c's block;
// scene_camera.cpp): whether its frame is cleared, the clear colour, the fog's
// colour and start. Aggressive Inline has the same wrappers
// (games/aggressive/camera.cpp: setSceneClearColour, setSceneFogColour).

#include "ee/ee.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::dm2
{
    // A camera node's scene word (sceneOf), which holds its camera.
    inline ee::Ptr<zge::Camera> objectCamera(ee::Ptr<zge::SceneObject> object) { return zge::sceneOf(object).cast<zge::Camera>(); }

    void setSceneClears(ee::Ptr<zge::SceneObject> object, uint32_t on);
    // `colour`: the address of its R, G, B, A.
    void setSceneClearColour(ee::Ptr<zge::SceneObject> object, uint32_t colour);
    void setSceneFogColour(ee::Ptr<zge::SceneObject> object, uint32_t colour);
    // The node's camera's far plane (camera.c's own copy of setSceneFarClip).
    void setSceneFarPlane(ee::Ptr<zge::SceneObject> object, ee::Float distance);
    // Returns the camera's flags doubleword (setCameraFogStart's).
    uint64_t setSceneFogStart(ee::Ptr<zge::SceneObject> object, ee::Float start);
}
