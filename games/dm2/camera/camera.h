#pragma once

// DM2's game camera (camera.c): one camera node shows the player whose turn
// it is. Each player has a camera record: the modes it is in, where it wants
// to be and where it is. Every frame the follow camera works out the place
// for the camera's player and applies it as a camera state (the node's
// position, rotation and field of view).

#include "dm2/game/players.h"
#include "dm2/rider/rider.h"
#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/camera/camera.h.h"

namespace game::dm2
{
    // A camera's modes come in two levels, as a rider's action states do:
    // setting a level's mode clears the levels after it. Level 0 is the
    // game's, level 1 the rider's while level 0 follows it.
    constexpr uint32_t kCameraModeLevels = 2;

    // Level 0. (2, 3 and 5 .. 7: what they show is not read yet; 6 and 7 hide
    // the rider's models.)
    constexpr uint8_t kCameraFollows = 0;    // behind the rider; level 1 says how
    constexpr uint8_t kCameraOnGround = 1;   // standing on the ground near the rider (placeGroundCamera)
    constexpr uint8_t kCameraLevelPoint = 4; // at one of the level's camera points, no rider needed

    // No mode yet (the camera's set-up; the level's start sets one).
    constexpr uint8_t kNoCameraMode = 0xFF;

    // Level 1, set by the rider's states: 0 riding, 1 as a bail starts,
    // 2 at rest with a foot down.
    constexpr uint8_t kRiderResting = 2;

    // PlayerCamera::view: the view the player picked for the follow camera.
    // This one is the rider's own eyes, so the rider's models are hidden.
    constexpr uint8_t kRiderEyesView = 3;

    // The ground camera's kind after a level-0 change to it.
    constexpr uint8_t kDefaultGroundKind = 7;

    // PlayerCamera::flags.
    // The follow camera's eye is not kept off the drawn rider
    // (keepOffRider) nor pushed out of the walls (keepOutOfWalls): set
    // together while it shows the rider's eyes, at a level point, in modes
    // 6 and 7 and while the rider rests; every mode change clears both.
    constexpr uint8_t kFlagNearRider = 0x01, kFlagInWalls = 0x02;
    constexpr uint8_t kFlagGroundPlaced = 0x04;     // the ground camera was placed by a mode change
    constexpr uint8_t kFlagGroundMoves = 0x08;      // the ground camera wants a new place
    // placeGroundCamera's draw: one of the two at random (half the time
    // neither) for a rider in the air or an approach; they follow the
    // camera to its next player (setCameraPlayer). No other reader of them
    // was found in DM2's PS2 code (the byte's bit tests searched).
    constexpr uint8_t kFlagGroundDrawFirst = 0x10, kFlagGroundDrawSecond = 0x20;

    struct PlayerCamera
    {
        uint8_t modes[kCameraModeLevels]; // +0x00
        uint8_t view;                     // +0x02 the follow camera's view (kRiderEyesView)
        uint8_t groundKind;               // +0x03 the ground camera's kind: its height, settings and behaviour
        uint32_t frames;                  // +0x04 counted up by the follow camera; cleared with a new place or view
        int16_t angle;                    // +0x08 a turn the follow camera steps (random at a level point)
        uint8_t cut;                      // +0x0A 1: the camera goes to its wanted place at once
        uint8_t flags;                    // +0x0B (kFlag...)
        uint16_t blockedFrames;           // +0x0C frames the follow camera's view of the rider has been blocked; cleared at a cut
        uint8_t _0e[0x10 - 0x0E];
        zge::Vec4 position;               // +0x10 where the camera is
        zge::Vec4 wantedPosition;         // +0x20 where it is going
        zge::Vec4 easedPosition;          // +0x30 the wanted place, eased: the follow camera is pulled toward it
        zge::Vec4 target;                 // +0x40 the point it looks at
        zge::Vec4 wantedTarget;           // +0x50
        zge::Vec4 velocity;               // +0x60 the follow camera's move a frame
        ee::Float fieldOfView;            // +0x70 degrees
        ee::Float wantedFieldOfView;      // +0x74
        uint8_t _78[0x80 - 0x78];
        zge::Matrix rows;                 // +0x80 its rotation
    };
    static_assert(offsetof(PlayerCamera, frames) == 0x04 && offsetof(PlayerCamera, angle) == 0x08);
    static_assert(offsetof(PlayerCamera, cut) == 0x0A && offsetof(PlayerCamera, flags) == 0x0B);
    static_assert(offsetof(PlayerCamera, wantedPosition) == 0x20 && offsetof(PlayerCamera, wantedTarget) == 0x50);
    static_assert(offsetof(PlayerCamera, rows) == 0x80 && sizeof(PlayerCamera) == 0xC0);
    static_assert(offsetof(PlayerCamera, blockedFrames) == 0x0C && offsetof(PlayerCamera, easedPosition) == 0x30);
    static_assert(offsetof(PlayerCamera, velocity) == 0x60 && offsetof(PlayerCamera, fieldOfView) == 0x70);

    // What the camera node is given: a player's each frame, and the one
    // applied last.
    struct CameraState
    {
        ee::Ptr<zge::SceneObject> node; // +0x00 the camera node (copyCameraState fills it in)
        int16_t fieldOfView;            // +0x04 0x10000 a turn
        uint8_t _06[0x10 - 0x06];
        zge::Vec4 position;             // +0x10
        zge::Vec4 target;               // +0x20
        zge::Matrix rows;               // +0x30 the camera's rows; the node gets right and up negated
    };
    static_assert(offsetof(CameraState, fieldOfView) == 0x04 && offsetof(CameraState, position) == 0x10);
    static_assert(offsetof(CameraState, rows) == 0x30 && sizeof(CameraState) == 0x70);

    inline ee::Global<PlayerCamera[kMaxPlayers], sym::g_playerCameras> g_playerCameras;
    inline ee::Global<CameraState[kMaxPlayers], sym::g_cameraStates> g_cameraStates;
    inline ee::Global<CameraState, sym::g_appliedCameraState> g_appliedCameraState;
    // The camera node, and the player it shows.
    inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_cameraNode> g_cameraNode;
    inline ee::Global<uint8_t, sym::g_cameraPlayer> g_cameraPlayer;

    // A player's camera record.
    inline ee::Ptr<PlayerCamera> playerCamera(uint32_t player)
    {
        return ee::Ptr<PlayerCamera>(sym::g_playerCameras) + static_cast<int32_t>(player);
    }

    // The level's camera made and taken down; its task's frame (the fade).
    void initCamera();
    void shutdownCamera();
    uint32_t cameraFadeFrame();
    // A player's camera state copied out, with the camera node.
    void copyCameraState(ee::Ptr<CameraState> out, int32_t player);
    // The camera node moved to a state, which becomes the applied one; the
    // sound's listener follows.
    void applyCameraState(ee::Ptr<CameraState> state);
    // The camera's player, and a new one.
    uint8_t cameraPlayer();
    void setCameraPlayer(uint32_t player);
    // A player's follow view.
    uint8_t cameraView(int32_t player);
    void setCameraView(uint32_t player, uint32_t view);
    // A player's camera mode at a level.
    uint8_t cameraMode(int32_t player, int32_t level);
    void setCameraMode(uint32_t player, uint32_t level, uint32_t mode);
    // The player's camera put on the ground near the rider, and its kind.
    void placeGroundCamera(uint32_t player, uint32_t kind);
    uint8_t groundCameraKind(int32_t player);
    // The camera at the rider's own view.
    void riderViewCamera(PlayerCamera *camera, rider::Rider *rider);
    // The level-point camera's point (0 to 4) of the level being played:
    // the executable's row of five for the session level. With the level
    // camera points option (ZX_DM2_LEVEL_CAMERA_POINTS=1), a level whose
    // scene has "CameraPos" points named "idle" (numbered from 1, as BMX
    // XXX's idle cameras) takes them in the row's place: point k is the
    // level's k mod N + 1, its position read as the table's (the camera
    // stands above it by the same lift).
    zge::Vec4 levelCameraPoint(uint32_t point);
}
