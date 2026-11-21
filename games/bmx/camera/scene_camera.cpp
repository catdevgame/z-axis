// A camera node's scene camera's settings (scene_camera.h): BMX XXX's own code
// (camera.c's block). Each finds the node's camera (its scene word) and sets it.

#include "scene_camera.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/camera/scene_camera.cpp.h"

namespace game::bmx
{
    using namespace zge;


    // setSceneClearColour: the node's camera's clear colour.
    void setSceneClearColour(ee::Ptr<SceneObject> object, ee::Address colour)
    {
        game::tail(setCameraClearColour, objectCamera(object), colour);
    }
    READABLE_FUNCTION(setSceneClearColour, Verified);

    // setSceneFogColour: the node's camera's fog colour.
    void setSceneFogColour(ee::Ptr<SceneObject> object, ee::Address colour)
    {
        game::tail(setCameraFogColour, objectCamera(object), colour);
    }
    READABLE_FUNCTION(setSceneFogColour, Verified);

    // setSceneFarClip: the node's camera's far plane (the draw distance the
    // level camera sets). Word for word Aggressive Inline's
    // (games/aggressive/camera_far_clip.cpp).
    void setSceneFarClip(ee::Ptr<SceneObject> object, ee::Float distance)
    {
        const ee::Ptr<Camera> camera = objectCamera(object);
        game::tail(setCameraFarPlane, camera, distance);
    }
    READABLE_FUNCTION(setSceneFarClip, Verified);
}
