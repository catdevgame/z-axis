// A camera node's scene camera's settings (scene_camera.h): DM2's own code
// (camera.c's block). Each finds the node's camera (its scene word) and sets it.

#include "scene_camera.h"

#include "ee/readable.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/camera/scene_camera.cpp.h"

namespace game::dm2
{
    using namespace zge;

    namespace
    {
    }

    // setSceneClears: whether the node's camera's frame is cleared.
    void setSceneClears(ee::Ptr<SceneObject> object, uint32_t on)
    {
        game::tail(setCameraClears, objectCamera(object), on);
    }
    READABLE_FUNCTION(setSceneClears, Verified);

    // setSceneClearColour: the node's camera's clear colour.
    void setSceneClearColour(ee::Ptr<SceneObject> object, uint32_t colour)
    {
        game::tail(setCameraClearColour, objectCamera(object), colour);
    }
    READABLE_FUNCTION(setSceneClearColour, Verified);

    // setSceneFogColour: the node's camera's fog colour.
    void setSceneFogColour(ee::Ptr<SceneObject> object, uint32_t colour)
    {
        game::tail(setCameraFogColour, objectCamera(object), colour);
    }
    READABLE_FUNCTION(setSceneFogColour, Verified);

    // setSceneFogStart: where the node's camera's fog starts.
    uint64_t setSceneFogStart(ee::Ptr<SceneObject> object, ee::Float start)
    {
        return game::tail(setCameraFogStart, objectCamera(object), start);
    }
    READABLE_FUNCTION(setSceneFogStart, Verified);

    // setSceneFarPlane: the node's camera's far plane. camera.c's own copy
    // of setSceneFarClip (camera_far_clip.cpp), word for word; the camera's
    // set-up calls this one.
    void setSceneFarPlane(ee::Ptr<SceneObject> object, ee::Float distance)
    {
        game::tail(setCameraFarPlane, objectCamera(object), distance);
    }
    READABLE_FUNCTION(setSceneFarPlane, Verified);
}
