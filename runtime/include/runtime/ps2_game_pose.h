#pragma once

// The game's own camera pose beside the drawn camera: a host override that runs a hidden camera (AI's follow camera,
// BMX XXX's action cameras: game_overrides/zge_view.h CameraTwin) keeps the pose it leaves, and the engine's next
// frustum of that camera takes it (zge/scene/camera.cpp updateFrustum, zge/host/game_view.h). In the runtime so every
// game's engine instance and the overrides share one store (an instance's zge namespace is its own).

#include "runtime/ps2_session.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace zx::game_pose
{
    // A camera's pose as the game's own camera has it: its axes and position (camera -> world, the camera record's
    // right, down, forward and position) and the inverse (its fromWorld), as setCameraMatrix leaves them from the
    // camera object's place. Kept each frame the hidden camera runs; it holds while the drawn camera's object stays
    // where the player's camera left it after (`drawn`: the object's position and orientation, noted by notePlayer):
    // when something else moves the drawn camera, the game's camera is the drawn one again.
    struct Pose
    {
        float toWorld[4][4];
        float fromWorld[4][4];
        float drawn[2][4];
        bool drawnNoted = false;
    };

    namespace detail
    {
        struct Store
        {
            std::unordered_map<uint32_t, Pose> poses; // by the camera record's EE address
        };
        inline Store &store() { return zx::session::state<Store>(); }
    }

    // The twin's pose for this frame (the player's place to be noted after its run).
    inline void keep(uint32_t camera, const Pose &pose)
    {
        Pose &kept = detail::store().poses[camera];
        const Pose previous = kept;
        kept = pose;
        // (until the player's run notes its place, the last one holds: the frame's frustum may come first)
        std::memcpy(kept.drawn, previous.drawn, sizeof(kept.drawn));
        kept.drawnNoted = previous.drawnNoted;
    }

    // Where the player's camera object stands after its run (position, then orientation).
    inline void notePlayer(uint32_t camera, const float place[2][4])
    {
        detail::Store &s = detail::store();
        const auto found = s.poses.find(camera);
        if (found == s.poses.end())
            return;
        std::memcpy(found->second.drawn, place, sizeof(found->second.drawn));
        found->second.drawnNoted = true;
    }

    inline void drop(uint32_t camera)
    {
        detail::Store &s = detail::store();
        if (!s.poses.empty())
            s.poses.erase(camera);
    }

    // The game's camera's pose when it holds (the drawn camera's object at `place`, as the player left it), else null.
    inline const Pose *current(uint32_t camera, const void *place)
    {
        detail::Store &s = detail::store();
        if (s.poses.empty())
            return nullptr;
        const auto found = s.poses.find(camera);
        if (found == s.poses.end() || !found->second.drawnNoted || std::memcmp(found->second.drawn, place, sizeof(found->second.drawn)) != 0)
            return nullptr;
        return &found->second;
    }
}
