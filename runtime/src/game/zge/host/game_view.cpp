// The game's own view beside the drawn one (game_view.h).

#include "zge/host/game_view.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include "runtime/ps2_session.h"

#include <unordered_map>

namespace zge::host
{
    namespace
    {
        struct GameViews
        {
            std::unordered_map<uint32_t, GameFrustum> frustums; // by camera
        };
        GameViews &gameViews() { return zx::session::state<GameViews>(); }
    }

    void keepGameFrustum(ee::Ptr<const Camera> camera, const GameFrustum &frustum)
    {
        gameViews().frustums[camera.address] = frustum;
    }

    void dropGameFrustum(ee::Ptr<const Camera> camera)
    {
        GameViews &views = gameViews();
        if (!views.frustums.empty())
            views.frustums.erase(camera.address);
    }

    const GameFrustum *gameFrustum(ee::Ptr<const Camera> camera)
    {
        GameViews &views = gameViews();
        if (views.frustums.empty())
            return nullptr;
        const auto found = views.frustums.find(camera.address);
        return found == views.frustums.end() ? nullptr : &found->second;
    }

    bool takeGamePose(ee::Ptr<const Camera> camera, GamePose &pose)
    {
        const ee::Ptr<const SceneObject> object(camera->object);
        const GamePose *kept = object ? zx::game_pose::current(camera.address, &object->position) : nullptr;
        if (kept)
            pose = *kept;
        return kept != nullptr;
    }
}
