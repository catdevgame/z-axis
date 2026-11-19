// The game's own view beside the drawn one (game_view.h).

#include "zge/host/game_view.h"

#include "runtime/ps2_session.h"

#include <unordered_map>

namespace zge::host
{
    namespace
    {
        struct GameViews
        {
            std::unordered_map<uint32_t, GameFrustum> frustums; // by camera
            std::unordered_map<uint32_t, GamePose> poses;
        };
        GameViews &gameViews() { return ps2x::session::state<GameViews>(); }
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

    void keepGamePose(uint32_t camera, const GamePose &pose)
    {
        gameViews().poses[camera] = pose;
    }

    void dropGamePose(uint32_t camera)
    {
        GameViews &views = gameViews();
        if (!views.poses.empty())
            views.poses.erase(camera);
    }

    bool takeGamePose(uint32_t camera, GamePose &pose)
    {
        GameViews &views = gameViews();
        if (views.poses.empty())
            return false;
        const auto found = views.poses.find(camera);
        if (found == views.poses.end())
            return false;
        pose = found->second;
        views.poses.erase(found);
        return true;
    }

    bool takeGamePose(ee::Ptr<const Camera> camera, GamePose &pose)
    {
        return takeGamePose(camera.address, pose);
    }
}
