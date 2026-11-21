#pragma once

// The follow camera (updateFollowCamera updates it each frame; one of 0x80 bytes
// per player from g_followCameras). Worked out for mouse-look
// (game_overrides/aggressive_inline_slus20327.cpp) and readable round 13
// (camera.cpp). Fields past those the code names are inferred from what
// the update does with them.

#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/render/fx_manager.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include "sym/aggressive/camera.h.h"

namespace aggressive
{
    struct FollowCamera
    {
        // +0x00 the camera's scene node: its rotation (quaternion x y z w at
        // node +0x20; matrix rows are the camera's axes in world space, row
        // 2 the view direction).
        ee::Ptr<zge::SceneObject> node;
        ee::Ptr<skater::Player> player; // +0x04 the skater followed
        uint8_t _08[0x08];
        zge::Vec4 eye;     // +0x10 (the eye logiccheck compares is g_followCameraEyeLow)
        zge::Vec4 lookAt;  // +0x20 (g_followCameraLookAtLow)
        // +0x30 the field of view, degrees (handed to the camera's node, and
        // the weather's spawns place flakes and drops within it).
        ee::Float fov;
        // +0x34 seconds the chase camera's eye stays where it is (cleared at
        // the camera's set-up; no live code sets it).
        ee::Float eyeHold;
        // +0x38 0.25 once a quarter pipe's frame has turned the view
        // (updateFollowCamera); no camera function reads it.
        ee::Float quarterPipeMark;
        // +0x3C how far behind the look-at point the eye is: grows toward
        // the wanted distance by 0.04 of the gap a frame, shrinks at once.
        ee::Float distance;
        // +0x40 the eye's easing step, the mean of this frame's and the last
        // (easeStep; kept, not read by the update).
        ee::Float easeStep;
        uint8_t _44[0x0C];
        // +0x50 the look-at point's sideways offset, eased toward the
        // skater's forward axis by Camera / Horiz Offset Dampen and kept
        // square to the view.
        zge::Vec4 sideOffset;
        // +0x60 the camera's own orientation, eased toward the skater's
        // drawn one (a slerp by 0.05 a frame).
        zge::Quaternion orientation;
        // +0x70 the skater's side the R1 camera is on: 1 the right, -1 the
        // left. It changes side when the other one has more room.
        int8_t side;
        uint8_t mode;      // +0x71 updateCamera's: a CameraMode
        // +0x72 set by the game for a camera cut: the next update snaps to
        // the wanted eye instead of easing (turnCameraView).
        uint8_t snap;
        uint8_t _73;
        // +0x74 the full-screen motion blur effect record, or 0
        // (applyLevelCameraSettings starts it, releaseFollowCameras stops it).
        ee::Ptr<zge::RunningEffect> motionBlur;
        uint8_t _78[0x08];
    };
    static_assert(offsetof(FollowCamera, eye) == 0x10 && offsetof(FollowCamera, lookAt) == 0x20 && offsetof(FollowCamera, fov) == 0x30);
    static_assert(offsetof(FollowCamera, quarterPipeMark) == 0x38 && offsetof(FollowCamera, easeStep) == 0x40 && offsetof(FollowCamera, sideOffset) == 0x50);
    static_assert(offsetof(FollowCamera, orientation) == 0x60 && offsetof(FollowCamera, mode) == 0x71 && offsetof(FollowCamera, snap) == 0x72);
    static_assert(offsetof(FollowCamera, eyeHold) == 0x34 && offsetof(FollowCamera, side) == 0x70 && offsetof(FollowCamera, motionBlur) == 0x74);
    static_assert(sizeof(FollowCamera) == 0x80);

    // FollowCamera::mode (setFollowCameraMode, updateCamera). Camera / Type
    // and Camera / Replay Type pick among the first five.
    enum class CameraMode : uint8_t
    {
        Follow = 0,        // updateFollowCamera
        R1 = 1,            // updateR1Camera: from the skater's side
        Plain2 = 2,        // no frame of its own
        Tracking = 3,      // updateTrackingCamera: the eye stays, the view turns to the skater (Replay Type's default)
        Behind = 4,        // updateBehindCamera: the eye straight behind the skater
        Free = 5,          // the free camera: the view from the eye to the look-at point others set
        Plain6 = 6,        // no frame of its own (the level's end)
        Animation = 7,     // updateAnimatedCamera: an animation places the camera and aims it
        AnimationAimed = 8, // updateAnimatedCamera: an animation places the eye, the view goes to the look-at point
        Editor = 9,        // the park editor moves it
    };

    constexpr uint32_t kFollowCameraCount = 2; // one per screen of a split screen

    inline ee::Global<FollowCamera[kFollowCameraCount], sym::g_followCameras> g_followCameras;
    inline ee::Global<FollowCamera, sym::g_followCameras> g_followCamera; // the first of g_followCameras

    // The tuning table's Camera category (category 2, 43 entries).
    enum class CameraTuning : uint32_t
    {
        Distance = 0,
        DistanceHPole = 1,
        Height = 2,
        CharOffset = 3,
        CharOffsetSkitch = 4,
        FallingBase = 5,
        FallingSpeed = 6,
        Fov = 7,
        Type = 8,
        FarClip = 9,
        MpBoardwalkFov = 10,
        MpCivicFov = 11,
        MpMovieFov = 12,
        MpMuseumFov = 13,
        MpBoardwalkFarClip = 14,
        MpCivicFarClip = 15,
        MpMovieFarClip = 16,
        MpMuseumFarClip = 17,
        TapDampen = 18,
        TapSide = 19,
        TapTimeout = 20,
        GrindSlerp = 21,
        QPipeDampen = 22,
        QPipeOffset = 23,
        SpineDampen = 24,
        R1Distance = 25,
        R1Fov = 26,
        R1Height = 27,
        R1CharOffset = 28,
        ReplayType = 29,
        VertLookaround = 30,
        HorzLookaround = 31,
        InvertVertLook = 32,
        InvertHorizLook = 33,
        VPoleRotate = 34,
        VPoleRotateTime = 35,
        HPoleCharOffset = 36,
        GrindHeight = 37,
        GrindDistance = 38,
        GrindCharOffset = 39,
        HorizOffset = 40,
        HorizOffsetDampen = 41,
        NormalBlur = 42,
    };
    constexpr uint32_t kCameraTuningCategory = 2;

    // A Camera tuning value as the game reads it: through the category's
    // entry pointer each time (the debug menu edits them live).
    inline ee::Float cameraTuning(CameraTuning entry)
    {
        return (*g_tuningCategories)[kCameraTuningCategory].entries[static_cast<uint32_t>(entry)].value;
    }

    // Set-up, by the level and the park editor (camera.cpp).
    ee::Ptr<FollowCamera> followCameras();
    void setFollowCameraTarget(ee::Ptr<FollowCamera> camera, ee::Ptr<skater::Player> target);
    // The camera's eye and look-at point (camera.cpp).
    void setEye(ee::Ptr<FollowCamera> camera, ee::Ptr<const zge::Vec4> eye);
    void setLookAt(ee::Ptr<FollowCamera> camera, ee::Ptr<const zge::Vec4> lookAt);

    // The moves of orbitFreeCamera (the free camera and the motion viewer's).
    enum class FreeCameraMove : uint8_t
    {
        Turn = 1,  // the look-at point around the eye, about the vertical
        Tilt = 2,  // the look-at point up or down around the eye, short of the pole by a degree
        Orbit = 3, // the eye around the look-at point
    };
    // orbitFreeCamera: the first camera moved by a 16-bit angle (camera.cpp).
    void orbitFreeCamera(uint32_t move, uint32_t angle);
    // setSceneFarClip: a scene node's camera's far plane, the draw distance (camera_far_clip.cpp).
    void setSceneFarClip(ee::Ptr<zge::SceneObject> object, ee::Float distance);
}

namespace aggressive
{
    // (camera.cpp)
    uint32_t applyLevelCameraSettings(ee::Ptr<FollowCamera> camera);
    void cameraAnimationEvent();
    uint32_t playCameraAnimation(ee::Ptr<FollowCamera> camera, ee::Address name, ee::Ptr<const ee::Float> fov, uint32_t aimed);
    void releaseFollowCameras();
    void setCameraBlur(ee::Ptr<FollowCamera> camera, uint32_t strength, ee::Float scaleX, ee::Float scaleY);
    uint32_t setFollowCameraMode(ee::Ptr<FollowCamera> camera, uint32_t newMode);
    uint32_t setupFollowCameras(uint32_t gameType);
    uint32_t updateCamera(ee::Ptr<FollowCamera> camera, ee::Float seconds);
}
