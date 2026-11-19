// DM2's game camera (camera.h): the players' camera records, their modes and
// views, the ground camera's placing, the rider's own view, and the state the
// camera node is given. DM2's own code (camera.c).

#include "camera.h"

#include "dm2/action/action.h"
#include "dm2/camera/scene_camera.h"
#include "dm2/game/players.h"
#include "dm2/level/level_globals.h"
#include "dm2/overlay/overlay_items.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/sound/sound.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "runtime/ps2_boot_config.h"
#include "sym/dm2/camera/camera.cpp.h"

namespace game::dm2::rider::physics
{
    collision::FaceRef respawnAtStart(const ee::Float *point, uint32_t flags); // respawn.cpp
}

namespace game::dm2
{
    using rider::Rider;
    using rider::vec4;

    namespace
    {
        // A player camera's modes by level: the bytes its record starts with.
        ee::Ptr<uint8_t> cameraModes(ee::Ptr<PlayerCamera> camera) { return camera.cast<uint8_t>(); }
        // The level being played, its camera points (five a level; a
        // level-point camera takes the next each time) and which is next.
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        constexpr uint32_t kLevelCameraPoints = 5;
        inline ee::Global<zge::Vec4[kLevels][kLevelCameraPoints], sym::g_levelCameraPoints> g_levelCameraPoints;
        inline ee::Global<uint8_t, sym::g_levelCameraPoint> g_levelCameraPoint;
        // A level-point camera stands this far above its point (y grows downwards).
        constexpr ee::Float kLevelPointLift = -5.0f;
        // A level's own camera points (the level camera points option): the
        // scene points of this kind and name, numbered from 1.
        constexpr uint32_t kCameraPointKind = zge::nameHashOf("CameraPos");
        constexpr uint32_t kIdleCameraName = zge::nameHashOf("idle");

        bool levelCameraPointsOn()
        {
            const char *on = ps2x::boot::text("PS2X_DM2_LEVEL_CAMERA_POINTS");
            return on && on[0] == '1';
        }

        // Whether the pad's zoom is held (the play frame reads the button; a
        // new mode or place lets go of it).
        inline ee::Global<uint32_t, sym::g_cameraZoomHeld> g_cameraZoomHeld;

        // The sound the game plays where the camera is, every frame.
        constexpr int32_t kCameraSound = 0x24;

        // The sound's listeners (cameraMoved): how many and their nodes.
        struct Listeners
        {
            uint32_t count;
            ee::Ptr<zge::SceneObject> node;
        };

        // The action mode in which a placed ground camera loses flag 0x10
        // (mode 0 is the play; what mode 1 is, action.c's code says: not read here).
        constexpr uint32_t kActionModeOne = 1;

        // The rider's model and bike shown or hidden.
        void showRider(Rider *rider, uint32_t shown)
        {
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), shown);
            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->bikeModel), shown);
        }

        // The centre of the 4-unit cell a coordinate is in.
        int32_t cellCentre(int32_t coordinate) { return (coordinate / 4) * 4 + 2; }
        int32_t sum(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }

        // The ground camera looks for its place this far ahead of the rider:
        // along the velocity when the rider moves, else along its forward row.
        constexpr ee::Float kMovingLead = 25.0f;
        constexpr ee::Float kStandingLead = 2.5f;
        // How far above the ground each kind stands (y grows downwards).
        constexpr ee::Float kGroundLift = -5.0f;
        constexpr ee::Float kGroundLiftKind1 = -3.0f;
        constexpr ee::Float kGroundLiftKind9 = -1.2f;
        constexpr ee::Float kGroundLiftKind12 = -8.0f;

        // The rider's own view: the eye above the position and a little
        // behind it along forward; the lean angles are turns.
        constexpr ee::Float kEyeUp = 0.5f;
        constexpr ee::Float kEyeForward = -0.1f;
        constexpr ee::Float kTurn = 65536.0f;
        constexpr ee::Float kViewDistance = 1.0f;
    }

    // copyCameraState: a player's camera state copied out, with the camera node.
    void copyCameraState(ee::Ptr<CameraState> out, int32_t player)
    {
        zge::memoryCopy(out, ee::Ptr<CameraState>(sym::g_cameraStates) + player, sizeof(CameraState));
        out->node = *g_cameraNode;
    }
    READABLE_FUNCTION(copyCameraState, Verified);

    // applyCameraState: the state becomes the applied one and the camera node
    // takes its field of view, position and rotation (right and up negated);
    // the sound's listener is the camera node, and the camera's sound plays
    // at its position.
    void applyCameraState(ee::Ptr<CameraState> state)
    {
        ee::Local<zge::Matrix> rows;
        ee::Local<Listeners> listeners;
        zge::memoryCopy(ee::Ptr<CameraState>(sym::g_appliedCameraState), state, sizeof(CameraState));
        zge::copyRotationAndTranslation(rows, ee::Ptr<zge::Matrix>(&state->rows));
        zge::vec3Negate(&rows->row(1).xyz(), &rows->row(1).xyz());
        zge::vec3Negate(&rows->row(0).xyz(), &rows->row(0).xyz());
        rows->translation() = state->position;
        zge::setFov(*g_cameraNode, static_cast<uint32_t>(state->fieldOfView));
        rider::physics::engine::setObjectPosition(ee::Address(*g_cameraNode), &state->position.x);
        zge::setObjectRotationMatrix(*g_cameraNode, rows);
        listeners->count = 1;
        listeners->node = *g_cameraNode;
        zge::cameraMoved(listeners);
        rider::soundEvent(kCameraSound, &state->position.x, 0, 0);
    }
    READABLE_FUNCTION(applyCameraState, Verified);

    // setCameraPlayer: the camera shows another player. The flag of the two
    // placeGroundCamera draws that the old player's camera has moves to the
    // new one's.
    void setCameraPlayer(uint32_t player)
    {
        if (player == *g_cameraPlayer)
            return;
        PlayerCamera &old = *playerCamera(*g_cameraPlayer);
        PlayerCamera &next = *playerCamera(player);
        *g_cameraPlayer = static_cast<uint8_t>(player);
        if (old.flags & kFlagGroundDrawFirst)
        {
            next.flags |= kFlagGroundDrawFirst;
            old.flags &= static_cast<uint8_t>(~kFlagGroundDrawFirst);
        }
        else if (old.flags & kFlagGroundDrawSecond)
        {
            next.flags |= kFlagGroundDrawSecond;
            old.flags &= static_cast<uint8_t>(~kFlagGroundDrawSecond);
        }
    }
    READABLE_FUNCTION(setCameraPlayer, Verified);

    // cameraPlayer: the player the camera shows.
    uint8_t cameraPlayer()
    {
        return *g_cameraPlayer;
    }
    READABLE_FUNCTION(cameraPlayer, Draft);

    // setCameraView: the player's follow view. Leaving or taking the rider's
    // eyes while the camera follows cuts, and shows (a shown rider) or hides
    // the rider's models.
    void setCameraView(uint32_t player, uint32_t view)
    {
        PlayerCamera &camera = *playerCamera(player);
        Rider *rider = rider::riderOf(riderIndexOf(player));
        if (camera.view == view)
            return;
        camera.cut = 0;
        if (camera.view == kRiderEyesView && cameraMode(static_cast<int32_t>(player), 0) == kCameraFollows)
        {
            camera.cut = 1;
            camera.flags &= static_cast<uint8_t>(~(kFlagNearRider | kFlagInWalls));
            if (rider->flags & rider::kFlagShown)
                showRider(rider, 1);
        }
        if (view == kRiderEyesView)
        {
            if (cameraMode(static_cast<int32_t>(player), 0) == kCameraFollows)
            {
                camera.cut = 1;
                camera.flags |= kFlagNearRider | kFlagInWalls;
                showRider(rider, 0);
            }
            camera.view = kRiderEyesView;
        }
        else
            camera.view = static_cast<uint8_t>(view);
        if (cameraMode(static_cast<int32_t>(player), 0) == kCameraFollows)
            camera.frames = 0;
    }
    READABLE_FUNCTION(setCameraView, Verified);

    // cameraView: the player's follow view.
    uint8_t cameraView(int32_t player)
    {
        return playerCamera(static_cast<uint32_t>(player))->view;
    }
    READABLE_FUNCTION(cameraView, Draft);

    // setCameraMode: the player's camera mode at a level; the levels after it
    // go to 0. Unless the game asks for a level point, the rider's models are
    // shown again first.
    //
    // Level 0 (the game's): the camera cuts unless it only followed (with
    // another view than the rider's eyes) or was in mode 2. The ground mode
    // places the camera (the default kind); a level point takes the level's
    // next camera point, lifted, with a random turn; modes 6 and 7 hide the
    // rider's models.
    // Level 1 (the rider's, while the camera follows): leaving or taking the
    // resting mode cuts.
    // Kept from the original: a level past the last writes the mode over the
    // bytes after the modes.
    void setCameraMode(uint32_t player, uint32_t level, uint32_t mode)
    {
        const ee::Ptr<PlayerCamera> record = playerCamera(player);
        PlayerCamera &camera = *record;
        Rider *rider = nullptr;
        camera.cut = 0;
        camera.flags &= static_cast<uint8_t>(~(kFlagNearRider | kFlagInWalls));
        if (level != 0 || mode != kCameraLevelPoint)
        {
            rider = rider::riderOf(riderIndexOf(player));
            showRider(rider, 1);
        }
        if (level == 0)
        {
            const uint8_t old = camera.modes[0];
            *g_cameraZoomHeld = 0;
            if (old == kCameraFollows)
            {
                if (camera.view == kRiderEyesView)
                    camera.cut = 1;
            }
            else if (old != 2)
                camera.cut = 1;
            switch (mode)
            {
            case kCameraFollows:
                if (camera.view == kRiderEyesView)
                    camera.cut = 1;
                break;
            case kCameraOnGround:
                camera.cut = 1;
                placeGroundCamera(player, kDefaultGroundKind);
                camera.flags = static_cast<uint8_t>((camera.flags | kFlagGroundPlaced) & ~kFlagGroundMoves);
                break;
            case 2:
                break;
            case kCameraLevelPoint:
            {
                const uint8_t point = *g_levelCameraPoint;
                camera.flags |= kFlagNearRider | kFlagInWalls;
                camera.wantedPosition = levelCameraPoint(point);
                camera.cut = 1;
                camera.wantedPosition.y = camera.wantedPosition.y + kLevelPointLift;
                *g_levelCameraPoint = static_cast<uint8_t>((point + 1u) % kLevelCameraPoints);
                camera.angle = static_cast<int16_t>(zge::randomNumber());
                camera.cut = 1;
                break;
            }
            case 6:
            case 7:
                camera.flags |= kFlagNearRider | kFlagInWalls;
                showRider(rider, 0);
                camera.cut = 1;
                break;
            default:
                camera.cut = 1;
                break;
            }
        }
        else if (level == 1 && camera.modes[0] == kCameraFollows)
        {
            if (camera.modes[1] == kRiderResting)
                camera.cut = 1;
            if (mode == kRiderResting)
            {
                camera.cut = 1;
                camera.flags |= kFlagNearRider | kFlagInWalls;
            }
        }
        const ee::Ptr<uint8_t> modes = cameraModes(record);
        modes[level] = static_cast<uint8_t>(mode);
        for (uint32_t after = level + 1; after < kCameraModeLevels; ++after)
            modes[after] = 0;
    }
    READABLE_FUNCTION(setCameraMode, Verified);

    // cameraMode: the player's camera mode at a level.
    uint8_t cameraMode(int32_t player, int32_t level)
    {
        return cameraModes(playerCamera(static_cast<uint32_t>(player)))[static_cast<uint32_t>(level)];
    }
    READABLE_FUNCTION(cameraMode, Verified);

    // placeGroundCamera: the player's camera goes to the ground mode (the
    // rider's level back to riding) as `kind`, cutting. Kinds 1, 4, 6 and
    // 9 .. 12 get a new place: ahead of the rider (along the velocity when it
    // moves, else along forward), at the centre of that 4-unit cell, on the
    // face the rider would start on there, at the face's centre lifted by the
    // kind's height. In action mode 1 flag 0x10 is cleared. In the other
    // modes kinds 11 and 12 keep the flags, and for the rest a rider in
    // physics state 1 or 3 .. 5 draws one of four: flag 0x10, flag 0x20 or
    // (twice) none. The ground flags are cleared either way.
    void placeGroundCamera(uint32_t player, uint32_t kind)
    {
        ee::Local<zge::Vec4> ahead;
        PlayerCamera &camera = *playerCamera(player);
        Rider *rider = rider::riderOf(riderIndexOf(player));
        camera.frames = 0;
        setCameraMode(player, 1, 0);
        camera.cut = 1;
        *g_cameraZoomHeld = 0;
        if (kind == 1 || kind == 4 || kind == 6 || (kind >= 9 && kind <= 12))
        {
            int32_t x, z;
            if (rider->speed == 0.0f)
            {
                x = sum(ee::toInt(rider->position[0]), ee::toInt(rider->forward[0] * kStandingLead));
                z = sum(ee::toInt(rider->position[2]), ee::toInt(rider->forward[2] * kStandingLead));
            }
            else
            {
                x = sum(ee::toInt(rider->position[0]), ee::toInt(rider->velocity[0] * kMovingLead));
                z = sum(ee::toInt(rider->position[2]), ee::toInt(rider->velocity[2] * kMovingLead));
            }
            ahead->x = ee::toFloat(cellCentre(x));
            ahead->y = rider->position[1];
            ahead->z = ee::toFloat(cellCentre(z));
            const rider::collision::FaceRef face = rider::physics::respawnAtStart(&ahead->x, 2);
            rider::collision::faceCentre(face, &camera.wantedPosition.x);
            ee::Float lift = kGroundLift;
            if (kind == 1)
                lift = kGroundLiftKind1;
            else if (kind == 9)
                lift = kGroundLiftKind9;
            else if (kind == 12)
                lift = kGroundLiftKind12;
            camera.wantedPosition.y = camera.wantedPosition.y + lift;
            if (actionMode() == kActionModeOne)
                camera.flags &= static_cast<uint8_t>(~kFlagGroundDrawFirst);
            else if (kind != 11 && kind != 12)
            {
                const uint8_t state = rider::physicsState(rider);
                if (state == 1 || (state >= 3 && state <= 5))
                {
                    const uint32_t drawn = zge::randomNumber() & 3;
                    if (drawn < 2)
                        camera.flags |= static_cast<uint8_t>(kFlagGroundDrawFirst << drawn);
                }
            }
        }
        camera.groundKind = static_cast<uint8_t>(kind);
        camera.flags &= static_cast<uint8_t>(~(kFlagGroundPlaced | kFlagGroundMoves));
    }
    READABLE_FUNCTION(placeGroundCamera, Verified);

    // groundCameraKind: the kind the player's ground camera was placed as.
    uint8_t groundCameraKind(int32_t player)
    {
        return playerCamera(static_cast<uint32_t>(player))->groundKind;
    }
    READABLE_FUNCTION(groundCameraKind, Verified);

    // riderViewCamera: the camera at the rider's own view. Its rows are the
    // rider's, turned by the rider's two lean angles (Rider::barLean: about y,
    // then about x); the eye is the drawn rider's position, up a little and
    // back a little; it looks along its forward row. The rider's models are
    // hidden.
    void riderViewCamera(PlayerCamera *camera, Rider *rider)
    {
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> back;
        ee::Local<zge::Matrix> rows;
        ee::Local<zge::Matrix> turn;
        zge::copyRotationAndTranslation(rows, ee::Ptr<ee::Float>(rider->right));
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(ee::toInt(rider->barLean[1] * kTurn)));
        rider::physics::engine::matrixProduct(rows, rows, turn);
        zge::matrixIdentity(turn);
        zge::matrixRotateX(turn, turn, static_cast<zge::Angle>(ee::toInt(rider->barLean[0] * kTurn)));
        rider::physics::engine::matrixProduct(rows, rows, turn);
        *eye = *vec4(rider->up) * kEyeUp;
        *back = *vec4(rider->forward) * kEyeForward;
        *eye = *eye + *back;
        *eye = *eye + *vec4(rider->position);
        *eye = *eye + *vec4(rider->rootOffset);
        camera->wantedTarget = rows->row(2) * kViewDistance;
        camera->wantedTarget = camera->wantedTarget + *eye;
        camera->wantedPosition = *eye;
        zge::copyRotationAndTranslation(ee::Ptr<zge::Matrix>(&camera->rows), rows);
        showRider(rider, 0);
    }
    READABLE_FUNCTION(riderViewCamera, Verified);
    namespace
    {
        // The camera's task (the fade's frame), its light, and the source
        // file and line the task is made at.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_cameraTask> g_cameraTask;
        inline ee::Global<uint32_t, sym::g_cameraLight> g_cameraLight;
        constexpr uint32_t kCameraSourcePath = sym::cameraSourcePath; // "./src/com/camera.c"
        constexpr uint32_t kCameraTaskLine = 323;

        // The menu scene's camera (the front end's; released before the
        // level's camera is made).
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_menuSceneCamera> g_menuSceneCamera;

        // The level's look before its "Globals" point is read: the clear, fog
        // and light colours, where the fog starts and how far the camera sees.
        inline ee::Global<uint32_t, sym::defaultClearColour> g_defaultClearColour;
        inline ee::Global<uint32_t, sym::defaultFogColour> g_defaultFogColour;
        inline ee::Global<uint32_t, sym::defaultLightColour> g_defaultLightColour;
        constexpr ee::Float kDefaultFogStart = 20.0f;
        constexpr ee::Float kDefaultFarClip = 60.0f;
        // The far clip: each level's own (0: the "Globals" point's), never
        // nearer than the least.
        inline ee::Global<ee::Float, sym::g_farClip> g_farClip;
        inline ee::Global<ee::Float[kLevels], sym::g_levelFarClips> g_levelFarClips;
        constexpr ee::Float kLeastFarClip = 11.0f;

        // The follow view the player picked.
        uint8_t pickedView(uint32_t player) { return ee::Ptr<PlayerSetup>(sym::g_playerSetups)[player].cameraView; }

        // The saved settings' flags (a halfword beside the volumes).
        // Scaffolding (unnamed-field): what sets this bit is not read yet.
        inline ee::Global<uint16_t, sym::g_settingFlags> g_settingFlags;
        constexpr uint16_t kSettingFlag2 = 0x2;

        // The fade: below this field of view the picture is washed out, the
        // more the narrower.
        constexpr ee::Float kFadeBelowDegrees = 35.0f;
        constexpr ee::Float kDegreesPerUnit = 0.005493164f; // 360 / 0x10000
        constexpr ee::Float kFadePerDegree = 1.1428572f;    // 40 / 35
        constexpr uint8_t kFadeGrey = 0x98;
        constexpr uint32_t kFadeQuads = 0x2000002;
        constexpr ee::Float kScreenWidth = 512.0f, kScreenHeight = 448.0f;
        // With setting flag 2 in a multiplayer game the whole screen gets
        // this rectangle instead.
        constexpr int32_t kFullWidth = 640, kFullHeight = 480;
        constexpr uint32_t kFullScreenStyle = 0x3F700070;

        // The fade's quad as the primitive API takes it: the record, its
        // four corners, and the colour they share.
        struct FadeQuad
        {
            uint32_t texture;                             // +0x00
            ee::Ptr<zge::PrimitiveVertex> vertices[4];    // +0x04
            uint8_t _14[0x20 - 0x14];
            zge::PrimitiveVertex corners[4];              // +0x20
            uint8_t colour[4];                            // +0xA0
        };
        static_assert(offsetof(FadeQuad, corners) == 0x20 && offsetof(FadeQuad, colour) == 0xA0);

    }

    // initCamera: the level's camera made. Each player's camera record
    // starts with the view the player picked, no mode, cutting. The menu
    // scene's camera is released; the camera node is made, clearing its
    // frame, with the level's clear colour, fog and far clip (the "Globals"
    // point's where the level has one), and a scene light in the level's
    // colour. The camera shows player 0 and its task draws the fade.
    void initCamera()
    {
        ee::Local<uint32_t> clearColour;
        ee::Local<uint32_t> fogColour;
        ee::Local<uint32_t> lightColour;
        ee::Local<ee::Float> fogStart;
        *clearColour = *g_defaultClearColour;
        *fogColour = *g_defaultFogColour;
        *lightColour = *g_defaultLightColour;
        *fogStart = kDefaultFogStart;
        *g_farClip = kDefaultFarClip;
        const uint32_t players = *g_multiplayerPlayers ? *g_multiplayerPlayers : 1u;
        for (uint32_t player = 0; player < players; ++player)
        {
            PlayerCamera &camera = *playerCamera(player);
            camera.view = pickedView(player);
            camera.modes[0] = kNoCameraMode;
            camera.cut = 1;
            camera.flags = 0;
            camera.groundKind = 0;
        }
        if (*g_menuSceneCamera)
        {
            const ee::Ptr<zge::SceneObject> menuCamera = *g_menuSceneCamera;
            *g_menuSceneCamera = {};
            zge::releaseSceneCamera(menuCamera);
        }
        const ee::Ptr<zge::SceneObject> node(zge::newSceneCamera(1));
        *g_cameraNode = node;
        setSceneClears(node, 1);
        globalsLightColour(lightColour);
        globalsClearColour(clearColour);
        globalsFogColour(fogColour);
        globalsFogStart(fogStart);
        *g_farClip = (*g_levelFarClips)[*g_sessionLevel];
        if (*g_farClip == 0.0f)
            globalsFarClip(ee::Ptr<ee::Float>(sym::g_farClip));
        if (*g_farClip < kLeastFarClip)
            *g_farClip = kLeastFarClip;
        setSceneClearColour(*g_cameraNode, clearColour.address());
        setSceneFogColour(*g_cameraNode, fogColour.address());
        setSceneFogStart(*g_cameraNode, *fogStart);
        setSceneFarPlane(*g_cameraNode, *g_farClip);
        *g_cameraLight = zge::newSceneLight(0, 1, lightColour.address());
        *g_cameraPlayer = 0;
        *g_cameraTask = zge::newTask(1, sym::cameraFadeFrame, 0, 1, kCameraSourcePath, kCameraTaskLine);
    }
    READABLE_FUNCTION_WAITS(initCamera, Verified);

    // shutdownCamera: the camera's task and node deleted.
    // Kept from the original: the scene light stays.
    void shutdownCamera()
    {
        zge::deleteTask(*g_cameraTask);
        zge::deleteSceneObject(*g_cameraNode);
        *g_cameraNode = {};
    }
    READABLE_FUNCTION_WAITS(shutdownCamera, Verified);

    // cameraFadeFrame (the camera's task): a grey quad over the picture,
    // its alpha growing as the applied field of view narrows below 35
    // degrees. Returns 1.
    // Kept from the original: at 35 degrees and wider the quad is still
    // drawn, in whatever colour the frame's bytes hold (they are not set).
    uint32_t cameraFadeFrame()
    {
        ee::Local<FadeQuad> quad;
        if ((*g_settingFlags & kSettingFlag2) && *g_multiplayerPlayers != 0)
        {
            drawOverlayRectangle(0, 0, kFullWidth, kFullHeight, kFullScreenStyle);
            return 1;
        }
        const ee::Float degrees = ee::toFloat(g_appliedCameraState->fieldOfView) * kDegreesPerUnit;
        if (degrees < kFadeBelowDegrees)
        {
            const ee::Float alpha = (kFadeBelowDegrees - degrees) * kFadePerDegree;
            quad->colour[2] = kFadeGrey;
            quad->colour[0] = kFadeGrey;
            quad->colour[1] = kFadeGrey;
            quad->colour[3] = static_cast<uint8_t>(ee::toInt(alpha));
        }
        quad->texture = 0;
        const ee::Ptr<zge::PrimitiveVertex> corners(&quad->corners[0]);
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            zge::PrimitiveVertex &vertex = quad->corners[corner];
            vertex.x = ((corner + 1) & 2) ? kScreenWidth : ee::Float(0.0f);
            vertex.y = (corner & 2) ? kScreenHeight : ee::Float(0.0f);
            vertex.z = 1.0f;
            std::memcpy(vertex.rgba, quad->colour, sizeof(vertex.rgba));
            quad->vertices[corner] = corners + (3 - corner);
        }
        zge::drawPrimitiveQuads(kFadeQuads, quad, 1, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(cameraFadeFrame, Verified);

    zge::Vec4 levelCameraPoint(uint32_t point)
    {
        if (levelCameraPointsOn())
        {
            uint32_t count = 0;
            while (zge::findScenePoint(kCameraPointKind, kIdleCameraName, count + 1) != 0)
                ++count;
            if (count != 0)
            {
                const ee::Ptr<zge::ScenePoint> own = zge::findScenePoint(kCameraPointKind, kIdleCameraName, point % count + 1);
                zge::Vec4 at = own->position;
                at.w = ee::Float(0.0f); // (the table's points have w 0)
                return at;
            }
        }
        return (*g_levelCameraPoints)[*g_sessionLevel][point];
    }
}
