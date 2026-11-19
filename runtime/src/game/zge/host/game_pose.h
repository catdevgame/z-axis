#pragma once

// The game's own camera pose beside the drawn camera (zge/host/game_view.h), in plain types: a game override that
// runs a hidden camera keeps its pose here (AI's hidden follow camera, game_overrides/aggressive_inline_slus20327.cpp).

#include <cstdint>

namespace zge::host
{
    // A camera's pose as the game's own camera has it this frame: its axes and position (camera -> world, the camera
    // record's right, down, forward and position) and the inverse (its fromWorld), as setCameraMatrix leaves them from
    // the camera object's place. Kept each frame the hidden camera runs; the camera's next frustum (updateFrustum)
    // takes it, once: a frame it was not kept for has none (the drawn camera is the game's then).
    struct GamePose
    {
        float toWorld[4][4];
        float fromWorld[4][4];
    };
    // By the camera record's EE address.
    void keepGamePose(uint32_t camera, const GamePose &pose);
    void dropGamePose(uint32_t camera);
    // The pose kept since the last take, if any (true), and taken.
    bool takeGamePose(uint32_t camera, GamePose &pose);
}
