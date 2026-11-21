// The draw distance the level camera sets (camera.h): moved from
// zge/scene/camera_records.cpp, where it sat as engine code; the blocks say
// it is AI's own (layer2).

#include "aggressive/camera.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/aggressive/camera_far_clip.cpp.h"

namespace aggressive
{
    using namespace zge;

    // setSceneFarClip: a node's scene's far clip (its camera's far plane; the
    // game's level camera sets the draw distance through it). DM2 (PS2) has
    // it word for word.
    namespace
    {
        // A camera object's scene, which is its camera.
        ee::Ptr<Camera> objectCamera(ee::Ptr<SceneObject> object) { return zge::sceneOf(object).cast<Camera>(); }
    }

    void setSceneFarClip(ee::Ptr<SceneObject> object, ee::Float distance)
    {
        const ee::Ptr<Camera> camera = objectCamera(object);
        game::tail(setCameraFarPlane, camera, distance);
    }
    READABLE_FUNCTION(setSceneFarClip, Verified);
}
