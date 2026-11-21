#pragma once

// The game's own view beside the drawn one (zge/scene/camera.cpp updateFrustum).
//
// The view settings (widescreen, a camera's field of view) change what is drawn: a camera's frustum and projection
// are the drawn view's. What the game decides from its view, whether a node was drawn in the last frame
// (nodeNotDrawn: the placed effects that spawn only on screen, the animators and NPCs that move only there), stays
// what the PS2 game would decide: the scene walk marks a node drawn by the game's own frustum (the camera's position
// and axes, its 4:3 width and the game's field of view), and only the drawing goes by the wider one.
//
// The camera the game decides by can also stand elsewhere than the drawn one: a game whose camera settings move its
// camera (AI's Camera Distance and Height, mouse-look) runs a hidden follow camera at the game's own tuning each frame
// and keeps its pose here (runtime/game_pose.h). The game's view is then that pose: its frustum, the area the scene walk
// stamps the regions out of view from, and the eye the placed effects measure their range from
// (gameCameraPosition); the drawn camera keeps the settings' place and its wall collision.
//
// While a camera's drawn frustum is its game's, nothing is kept and the walk marks as it always did.

#include "zge/math/types.h"
#include "runtime/game_pose.h"

#include <cstdint>

namespace zge
{
    struct Camera;
}

namespace zge::host
{
    // A camera's game frustum: its world planes, as Camera::planes holds the drawn view's, and the half-angle
    // tangents and sphere scales the view test reads (partInView), as the camera's are the drawn view's.
    struct GameFrustum
    {
        Matrix fromWorld; // the game's camera's world -> camera (Camera::fromWorld)
        Vec4 position;    // the game's camera's position (Camera::position)
        bool elsewhere;   // a hidden camera's pose: the game's camera is not the drawn one
        uint32_t planeCount;
        Vec4 planes[6]; // (zge/scene/camera.h's Plane, kFrustumPlanes of them)
        ee::Float tanHalfWidth, tanHalfHeight;
        ee::Float sphereScaleX, sphereScaleY;
    };

    // The camera's game frustum for its frame (updateFrustum), or none: the drawn frustum is the game's.
    void keepGameFrustum(ee::Ptr<const Camera> camera, const GameFrustum &frustum);
    void dropGameFrustum(ee::Ptr<const Camera> camera);
    // The camera's game frustum when it differs from the drawn one, else null.
    const GameFrustum *gameFrustum(ee::Ptr<const Camera> camera);

    // The camera's game pose while it holds (runtime/game_pose.h; updateFrustum's).
    using GamePose = zx::game_pose::Pose;
    bool takeGamePose(ee::Ptr<const Camera> camera, GamePose &pose);
}
