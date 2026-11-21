// The draw distance DM2's level camera sets (camera_far_clip.h): DM2's own
// code (its game block), word for word Aggressive Inline's
// (games/aggressive/camera_far_clip.cpp).

#include "camera_far_clip.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/camera_far_clip.cpp.h"

namespace game::dm2
{
    using namespace zge;

    namespace
    {
        // A camera object's scene word (sceneOf), which holds its camera.
        ee::Ptr<Camera> objectCamera(ee::Ptr<SceneObject> object) { return zge::sceneOf(object).cast<Camera>(); }
    }

    // setSceneFarClip: a node's scene's far clip (its camera's far plane; the
    // game's level camera sets the draw distance through it).
    void setSceneFarClip(ee::Ptr<SceneObject> object, ee::Float distance)
    {
        const ee::Ptr<Camera> camera = objectCamera(object);
        game::tail(setCameraFarPlane, camera, distance);
    }
    READABLE_FUNCTION(setSceneFarClip, Verified);
}
