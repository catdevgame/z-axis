#pragma once

// BMX XXX's cameras (./src/com/action/camera.c): eight records, each a view
// of the level on a rectangle of the screen. A rider's camera follows the
// rider in one of the camera modes; the game's own camera (the first one made)
// shows the level when no rider's does. A camera that is shown has a scene
// camera object, which the frame's update places from the camera's view.
//
// The layout is read from BMX XXX's machine code; fields are named for what
// the camera code does with them, and bytes no code read so far touches are
// padding.

#include "bmx/game/player_riders.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/render/fx_manager.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A camera's bits (ActionCamera::flags). The frame's update clears the
    // two top bits before the mode's function runs.
    enum ActionCameraFlags : uint8_t
    {
        kCameraInUse = 0x01,       // the record is taken (newActionCamera)
        kCameraShown = 0x02,       // it has its scene camera (showCamera)
        kCameraKeepsOffRider = 0x04, // a rider's camera's eye is kept a distance from the rider; set by every mode change, cleared by the modes that go through walls
        kCameraThroughWalls = 0x08, // the eye is not pushed out of the level's faces; the other way round
        kCameraShotHeld = 0x10,    // set with a mode change to the action shot (setCameraMode)
        kCameraShotBlocked = 0x20, // cleared with it
        kCameraModePlacedAxes = 0x40, // this frame's mode set the view's axes itself (the idle mode)
        kCameraModePlacedView = 0x80, // this frame's mode placed the scene camera itself
    };

    // How the view moves to where the mode wants it (ActionCamera::easing).
    enum class CameraEasing : uint8_t
    {
        Ease = 0,  // the eye on a spring, the rest a share of the gap each frame
        Snap = 1,  // there at once but for the field of view; Ease from the next frame
        Swing = 2, // a share of the gap that grows as a sine over the mode's first 30 frames
        Cut = 3,   // there at once, kept off the rider and the walls; Ease from the next frame
    };

    // A camera's modes (ActionCamera::modes[0], setCameraMode's level 0): an
    // index of the table of mode functions the frame's update calls. Named
    // where the mode's function says what it is.
    enum class CameraMode : uint8_t
    {
        Follow = 0,     // behind the rider (the camera type's set-up)
        ActionShot = 1, // one of the action shots (ActionCamera::shot)
        None = 2,       // nothing moves the view (noCameraMode)
        Orbit = 3,      // round the rider, a little further each frame
        Idle = 4,       // at one of the level's "idle" camera points
        Van = 5,        // at the level's "van" camera point
        NpcAlone = 6,   // on the NPC the game is showing, the players' riders not drawn
        Npc = 7,        // on the NPC the game is showing
        Animated = 8,   // the scene camera's own animation places it
        Held = 9,       // other code places the scene camera (heldCameraMode)
    };

    // What the camera's full-screen effects do (ActionCamera::blurPhase;
    // updateCameraBlur).
    enum class CameraBlur : uint8_t
    {
        None = 0,
        PulseUp = 1,   // up to full strength over 60 frames, then PulseDown
        PulseDown = 2, // back down over 60 frames, then None
        Steady = 3,    // a light blur (every change of the camera's mode starts it)
        Swell = 4,     // a half-strength blur whose scale and drift swing slowly
        Second = 5,    // the second effect's values set instead
    };

    // Which sounds a shown camera hears for (ActionCamera::hearing;
    // updateCameraListeners).
    enum class CameraHearing : uint8_t
    {
        Alone = 0,       // the one listener: the sound engine hears from this camera only
        Deaf = 1,        // not a listener
        SplitFirst = 2,  // split screen: the first player's side
        SplitSecond = 3, // split screen: the second player's side
    };

    // A camera's rectangle of the 512 by 448 screen (setCameraViewportLayout).
    enum class ViewportLayout : uint8_t
    {
        Full = 0,
        Letterbox = 1,    // the middle 336 lines
        Top = 2,          // two players, one above the other
        Bottom = 3,
        Left = 4,         // two players, side by side
        Right = 5,
        Quarter0 = 6,     // four players: 6 top right, 7 top left, 8 bottom right, 9 bottom left
        Quarter1 = 7,
        Quarter2 = 8,
        Quarter3 = 9,
        Inset = 10,       // a third of the screen, up on the right
        Count = 11,
    };

    struct ScreenRectangle
    {
        ee::Float x, y, width, height;
    };

    // What a camera shows (setCameraView, cameraView): 0x80 bytes.
    struct CameraView
    {
        zge::Angle fov;    // +0x00 the field of view, a 16-bit angle
        uint8_t _02[0x0E];
        zge::Vec4 eye;     // +0x10
        zge::Vec4 lookAt;  // +0x20
        zge::Matrix axes;  // +0x30 the view's axes (rows 0 and 1 are turned round for the scene camera)
        // +0x70 the camera's scene object: cameraView's copies have it; the
        // camera's own view does not.
        ee::Ptr<zge::SceneObject> object;
        uint8_t _74[0x0C];
    };
    static_assert(offsetof(CameraView, eye) == 0x10 && offsetof(CameraView, lookAt) == 0x20 && offsetof(CameraView, axes) == 0x30);
    static_assert(offsetof(CameraView, object) == 0x70 && sizeof(CameraView) == 0x80);

    constexpr uint32_t kCameraModeLevels = 4;

    struct ActionCamera
    {
        uint8_t flags;  // +0x00 ActionCameraFlags
        uint8_t kind;   // +0x01 from the set-up record: 1 is a rider's camera
        uint8_t type;   // +0x02 the player's camera type (the pause menu's Camera Type; setCameraType)
        uint8_t shot;   // +0x03 the action shot's number (cameraShot)
        CameraEasing easing; // +0x04
        ViewportLayout viewportLayout; // +0x05
        CameraHearing hearing; // +0x06
        CameraBlur blurPhase;  // +0x07
        // +0x08 the mode at each level, and at +0x0C the frames each has
        // run: a change at one level clears every level below it
        // (setCameraMode).
        uint8_t modes[kCameraModeLevels];
        uint32_t modeFrames[kCameraModeLevels];
        uint32_t stageFrames; // +0x1C frames the action shot's stage has run (the action shot mode counts them)
        uint32_t blurFrames; // +0x20 frames the blur's phase has run
        zge::Angle orbitAngle;  // +0x24 how far round the rider the orbit mode's eye is
        uint16_t blockedFrames; // +0x26 frames the level has stood between a rider's camera and its rider
        uint8_t _28[0x08];
        zge::Vec4 eye;          // +0x30 where the camera is
        zge::Vec4 wantedEye;    // +0x40 where the mode wants it
        zge::Vec4 easedEye;     // +0x50 the wanted eye, eased
        zge::Vec4 lookAt;       // +0x60 the point it looks at
        zge::Vec4 wantedLookAt; // +0x70
        zge::Vec4 eyeVelocity;  // +0x80 the eye's move this frame
        ee::Float fov;          // +0x90 the field of view, degrees
        ee::Float wantedFov;    // +0x94
        uint8_t _98[0x08];
        zge::Matrix axes;       // +0xA0 the view's axes, as the frame's update hands them to the view
        ee::Ptr<zge::SceneObject> object; // +0xE0 its scene camera, while shown
        uint8_t _E4[0x0C];
        CameraView view;        // +0xF0 what it shows now
        uint32_t rider;         // +0x170 the rider it follows (riderOf's index: its low byte). A session with one
                                //        rider for every player has one camera, on that rider
        ScreenRectangle viewport; // +0x174
        // +0x184 the full-screen motion blur running on it, or 0, and at
        // +0x188 a second effect showCamera stops with it (nothing read so
        // far starts one).
        ee::Ptr<zge::RunningEffect> motionBlur;
        ee::Ptr<zge::RunningEffect> secondEffect;
        uint8_t _18C[0x04];
    };
    static_assert(offsetof(ActionCamera, modes) == 0x08 && offsetof(ActionCamera, modeFrames) == 0x0C && offsetof(ActionCamera, eye) == 0x30);
    static_assert(offsetof(ActionCamera, fov) == 0x90 && offsetof(ActionCamera, axes) == 0xA0 && offsetof(ActionCamera, object) == 0xE0);
    static_assert(offsetof(ActionCamera, view) == 0xF0 && offsetof(ActionCamera, rider) == 0x170 && offsetof(ActionCamera, viewport) == 0x174);
    static_assert(offsetof(ActionCamera, motionBlur) == 0x184 && sizeof(ActionCamera) == 0x190);
    static_assert(offsetof(ActionCamera, blockedFrames) == 0x26);

    constexpr uint32_t kActionCameraCount = 8;

    // What a camera does while it is doing one thing: where a mode that
    // follows the rider stands (in a space the mode makes at the rider), and
    // how the view follows what the mode wants. One row for each thing a
    // camera can be doing (the tables are in settings below).
    struct CameraSettings
    {
        zge::Vec4 offset;      // +0x00 the eye's place in the mode's space
        ee::Float distance;    // +0x10 how far from the point looked at the modes that swing round it keep the eye
        ee::Float spring;      // +0x14 the eye is pulled this part of its distance from the eased eye, a frame
        ee::Float damping;     // +0x18 the part of its speed the eye loses, a frame
        ee::Float eyeShare;    // +0x1C the part of the way the eased eye goes to the wanted eye, a frame
        ee::Float lookAtShare; // +0x20 and the point looked at to the wanted one
        ee::Float fovShare;    // +0x24 and the field of view
        ee::Float fov;         // +0x28 the field of view wanted, degrees
        uint8_t _2C[4];
    };
    static_assert(sizeof(CameraSettings) == 0x30 && offsetof(CameraSettings, distance) == 0x10 && offsetof(CameraSettings, fov) == 0x28);

    // What newActionCamera makes a camera from.
    struct CameraSetup
    {
        uint8_t kind;            // +0x00 ActionCamera::kind
        uint8_t _01[3];
        uint32_t viewportLayout; // +0x04 a ViewportLayout
        uint8_t _08[4];
        uint32_t rider;          // +0x0C ActionCamera::rider
    };
    static_assert(offsetof(CameraSetup, viewportLayout) == 0x04 && offsetof(CameraSetup, rider) == 0x0C && sizeof(CameraSetup) == 0x10);

    // (setup.cpp)
    void initActionCameras();
    uint32_t idleCameraMode(ActionCamera *camera);
    uint32_t vanCameraMode(ActionCamera *camera);
    uint32_t orbitCameraMode(ActionCamera *camera);
    uint32_t npcAloneCameraMode(ActionCamera *camera);
    uint32_t npcCameraMode(ActionCamera *camera);

    // (follow.cpp)
    uint32_t followCameraMode(ActionCamera *camera);
    void riderViewCamera(ActionCamera *camera, const rider::Rider *rider);

    // (action_shot.cpp)
    void setCameraShot(ActionCamera *camera, uint32_t shot);
    uint32_t actionShotCameraMode(ActionCamera *camera);

    // (update.cpp)
    void updateCameras();
    uint32_t cameraTask();

    // (action_camera.cpp)
    ee::Ptr<ActionCamera> newActionCamera(const CameraSetup *setup);
    void freeActionCamera(ActionCamera *camera);
    void freeActionCameras();
    void showCamera(ActionCamera *camera, uint32_t shown);
    bool cameraShown(const ActionCamera *camera);
    ee::Ptr<zge::SceneObject> cameraSceneObject(const ActionCamera *camera);
    uint8_t cameraType(const ActionCamera *camera);
    void setCameraType(ActionCamera *camera, uint32_t type);
    uint8_t cameraMode(const ActionCamera *camera, uint32_t level);
    void setCameraMode(ActionCamera *camera, uint32_t level, uint32_t mode);
    uint8_t cameraShot(const ActionCamera *camera);
    uint8_t cameraViewportLayout(const ActionCamera *camera);
    ScreenRectangle *cameraScreenRectangle(ActionCamera *camera);
    void setCameraViewportLayout(ActionCamera *camera, uint32_t layout);
    void setCameraHearing(ActionCamera *camera, uint32_t hearing);
    void setCameraView(ActionCamera *camera, const CameraView *view);
    void cameraView(const ActionCamera *camera, CameraView *out);
    void applyCameraLens(ActionCamera *camera);
    void updateCameraListeners();
    uint32_t keepEyeOffRider(ActionCamera *camera, const rider::Rider *rider, zge::Vec4 *eye);
    uint32_t pushEyeOutOfWalls(ActionCamera *camera, zge::Vec4 *eye);
    void aimCamera(ActionCamera *camera);
    void updateCameraBlur(ActionCamera *camera);
    uint32_t animatedCameraMode(ActionCamera *camera);
    uint32_t noCameraMode(ActionCamera *camera);
    uint32_t heldCameraMode(ActionCamera *camera);

    // A player's action camera (action.c's playerCamera answers its address).
    inline ee::Ptr<ActionCamera> playerActionCamera(uint32_t player) { return ee::Ptr<ActionCamera>(playerCamera(player)); }
}
