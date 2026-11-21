// BMX XXX (SLUS-20415): the view settings, Aggressive Inline's host side
// (zge_view.cpp) at BMX XXX's functions: ZGE4's camera, zgui and the screen
// packet are AI's.
//
// Motion blur (settings.ini [graphics] motion_blur): ZGE4's camera blur
// (drawBlur, the camera's blur switch and colour at AI's offsets), set up by
// the level's "FSMotionBlur" object as AI's is. At the game's own (the
// default) the game's draw runs as it is.
//
// Widescreen (settings.ini [display] widescreen and aspect): the projection's
// horizontal scale, the frustum's half-width tangent at its first angleSin,
// the 2D primitives of the screen packet squeezed back to 4:3 and the HUD at
// the wide picture's sides, as AI's. A level is drawn while a ridden state
// runs (games/bmx/states/game_state.h: the level, the park editor, a saved
// park ridden; not while it loads); the front end, its loading and the
// movies stay 4:3. The HUD at the sides: the widgets of the one-view
// overlay's Overlay groups (UI_overDisp_com: the timer; UI_overDisp_P_full:
// the trick, the balance, the score, the health, the goals) in the left or
// right third of the screen; split screen's keep their places.
//
// Draw distance (settings.ini [gameplay] draw_distance, at boot): the level camera's
// lens (applyCameraLens, every frame) sets the far clip from the level's
// (levelFarClip; nearer in split screen, up to twice as far with a narrow
// field of view) through setCameraFarPlane, which caps it at 1000, and
// moves the fog to end there (moveCameraClipRange). Far and Farthest
// multiply the far clip, and the fog's depth with it, as below. The
// original's: the level's Globals point's FarClip (levelFarClip; 200 for a
// level without, at least 11), as far again with a narrow field of view, and
// the models' own LOD ends. Beyond Original the game's state differs (the
// far clips and LOD ends are its RAM), so BMX XXX's draw distance is Original
// by default and is given at boot (GameSettingsProfile::
// drawDistanceChangesGame).
//
// Camera Distance and Height (the gameplay page, bmx_xxx_gameplay.cpp) move
// the action cameras (games/bmx/camera/update.cpp updateCameras), and with
// them what the game decides by its view. With either changed the cameras'
// frame runs a hidden twin first (zge_view.h CameraTwin): its own copies of
// the action cameras, each shown camera's scene object and camera record,
// and the riders they follow with their models (a rider's camera shows or
// hides its rider), at the game's own tables, outside the game's time, with
// the sound listeners, the Doppler switch and the camera blur left alone
// while it runs, then the camera objects' own updates. Its poses are the
// game's views; the player's cameras run after it and are drawn.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_view.cpp.h"
#include "overrides/zge_view.h"

#include "machine.h"
#include "runtime/session.h"

#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace bmx_xxx::view
{
    namespace
    {
        // games/bmx/states/game_state.h and game_state.cpp: the ridden
        // states, and the state machine's phase while a state's frame runs.
        constexpr uint8_t kGameStateLevel = 1, kGameStateParkEditor = 2, kGameStateRidePark = 3;
        constexpr uint8_t kStateRunning = 0;
        constexpr uint32_t kDisplayWidth = 0x0C; // DisplaySetup::width (zge/render/display.h)
        constexpr uint32_t kNodeLods = 0x16C;    // Node::lods (zge/scene/node.h, BMX's)

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        bool ridden(const uint8_t *rdram, uint32_t &mode)
        {
            mode = readByte(rdram, sym::bmx::g_currentGameState);
            return (mode == kGameStateLevel || mode == kGameStateParkEditor || mode == kGameStateRidePark) &&
                   readByte(rdram, sym::bmx::g_gameStatePhase) == kStateRunning;
        }

        bool hudSideAnchored(const uint8_t *rdram, uint32_t, uint32_t parent, uint32_t grandparent)
        {
            using zge_host::view::widgetNamed;
            return widgetNamed(rdram, parent, "Overlay") &&
                   (widgetNamed(rdram, grandparent, "UI_overDisp_com") || widgetNamed(rdram, grandparent, "UI_overDisp_P_full"));
        }
    }
}

namespace bmx_xxx::view
{
    namespace
    {
        // games/bmx/camera/action_camera.h: the action cameras and what the twin keeps of each.
        constexpr uint32_t kActionCameraCount = 8, kActionCameraBytes = 0x190;
        constexpr uint32_t kCameraFlags = 0x00, kCameraObject = 0xE0, kCameraRider = 0x170;
        constexpr uint8_t kCameraShown = 0x02;
        constexpr uint32_t kSceneObjectBytes = 0x50, kSceneObjectScene = 0x38; // zge/scene/scene.h SceneObject
        constexpr uint32_t kCameraRecordBytes = 0x510;                         // zge/scene/camera.h Camera
        // games/bmx/rider/rider.h: a rider and its two models (riderOf: the table's pointer by the index's low byte).
        constexpr uint32_t kRiderBytes = 0x2D70, kRiderMessenger = 0x2838, kRiderBikeModel = 0x2D50;

        struct Twin
        {
            zge_host::view::CameraTwin twin;
            std::vector<std::pair<uint32_t, uint32_t>> objects; // each shown camera's scene object and camera record
            PS2Machine::RecompiledFunction updateCameras = nullptr, updateObjectCamera = nullptr;
            PS2Machine::RecompiledFunction disableDoppler = nullptr, updateCameraListeners = nullptr, updateCameraBlur = nullptr;
            bool running = false;
            // The player's cameras' update noted what it left as it returned (a thread switch can leave its frame to
            // the scheduler past the hook: then its camera objects' updates note the whole blocks).
            bool playerNoted = false;
        };
        Twin &twin() { return zx::session::state<Twin>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void dropTwin()
        {
            Twin &t = twin();
            for (const auto &[object, record] : t.objects)
                zge_host::view::dropTwinPose(record);
            t.objects.clear();
            t.twin.reset();
        }

        void runTwin(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Twin &t = twin();
            t.playerNoted = false;
            std::vector<std::pair<uint32_t, uint32_t>> blocks = {{sym::bmx::g_actionCameras, kActionCameraCount * kActionCameraBytes}};
            std::vector<std::pair<uint32_t, uint32_t>> objects;
            const auto add = [&](uint32_t address, uint32_t size) {
                if (!address)
                    return;
                for (const auto &b : blocks)
                    if (b.first == address)
                        return;
                blocks.emplace_back(address, size);
            };
            for (uint32_t i = 0; i < kActionCameraCount; ++i)
            {
                const uint32_t camera = sym::bmx::g_actionCameras + i * kActionCameraBytes;
                const uint8_t *flags = getConstMemPtr(rdram, camera + kCameraFlags);
                if (!flags || (*flags & kCameraShown) == 0)
                    continue;
                const uint32_t object = readWord(rdram, camera + kCameraObject);
                const uint32_t record = object ? readWord(rdram, object + kSceneObjectScene) : 0u;
                if (object && record)
                {
                    add(object, kSceneObjectBytes);
                    add(record, kCameraRecordBytes);
                    objects.emplace_back(object, record);
                }
                const uint32_t rider = readWord(rdram, sym::bmx::g_riders + 4u * (readWord(rdram, camera + kCameraRider) & 0xFFu));
                if (rider)
                {
                    add(rider, kRiderBytes);
                    add(readWord(rdram, rider + kRiderMessenger), kSceneObjectBytes);
                    add(readWord(rdram, rider + kRiderBikeModel), kSceneObjectBytes);
                }
            }
            t.objects = objects;
            t.twin.track(rdram, blocks);
            if (!t.twin.valid())
                return;
            t.twin.run(rdram, ctx, [&] {
                bmx_xxx::holdGameCameraTuning(rdram, true);
                t.running = true;
                t.updateCameras(rdram, ctx, runtime);
                // (each camera object's update after it, as the frame's stale objects run it: the record from its place)
                for (const auto &[object, record] : t.objects)
                {
                    SET_GPR_U32(ctx, 4, object);
                    t.updateObjectCamera(rdram, ctx, runtime);
                }
                t.running = false;
                bmx_xxx::holdGameCameraTuning(rdram, false);
            });
            for (const auto &[object, record] : t.objects)
                if (const zge_host::view::CameraTwin::Block *b = t.twin.blockAt(record))
                    zge_host::view::keepTwinPose(record, b->hidden);
        }

        // The cameras' frame: the twin first while Camera Distance or Height is changed.
        void updateCamerasHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Twin &t = twin();
            if (!t.running)
            {
                if (bmx_xxx::cameraTuningChanged())
                    runTwin(rdram, ctx, runtime);
                else if (t.twin.valid())
                    dropTwin();
            }
            t.updateCameras(rdram, ctx, runtime);
            // What the player's cameras left: the game's changes from here to the twin's next run (the rest of the
            // frame, the level's own, a camera made) are made to the twin's too.
            if (!t.running && t.twin.valid())
            {
                t.twin.notePlayer(rdram);
                t.playerNoted = true;
            }
        }

        // A camera object's update after the player's cameras ran: what it writes of its object and camera record is
        // the player's (the twin runs its own); where the camera stands for the pose rule. Without the update's own
        // note (a thread switch), what the player's cameras left, whole.
        void updateObjectCameraHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Twin &t = twin();
            const uint32_t object = getRegU32(ctx, 4);
            const bool player = !t.running && t.twin.valid() && t.twin.blockAt(object);
            uint32_t cameraRecord = 0;
            for (const auto &[cameraObject, record] : t.objects)
                if (cameraObject == object)
                    cameraRecord = record;
            std::vector<uint8_t> objectBefore, recordBefore;
            if (player && t.playerNoted)
            {
                objectBefore = t.twin.bytesNow(rdram, object);
                recordBefore = t.twin.bytesNow(rdram, cameraRecord);
            }
            t.updateObjectCamera(rdram, ctx, runtime);
            if (!player)
                return;
            if (t.playerNoted)
            {
                t.twin.notePlayerWrites(rdram, object, objectBefore);
                t.twin.notePlayerWrites(rdram, cameraRecord, recordBefore);
            }
            else
                t.twin.notePlayer(rdram);
            if (cameraRecord)
                zge_host::view::noteTwinPlayer(rdram, cameraRecord, object);
        }

        // Left alone while the twin runs: the sound engine's listeners and Doppler, and the camera blur's effects.
        void updateCameraListenersHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (!twin().running)
                twin().updateCameraListeners(rdram, ctx, runtime);
        }
        void disableDopplerHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (!twin().running)
                twin().disableDoppler(rdram, ctx, runtime);
        }
        void updateCameraBlurHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (!twin().running)
                twin().updateCameraBlur(rdram, ctx, runtime);
        }

        bool installTwin(PS2Machine &runtime)
        {
            Twin &t = twin();
            t.updateCameras = runtime.lookupFunction(sym::bmx::updateCameras);
            t.updateObjectCamera = runtime.lookupFunction(sym::bmx::updateObjectCamera);
            t.disableDoppler = runtime.lookupFunction(sym::bmx::disableDoppler);
            t.updateCameraListeners = runtime.lookupFunction(sym::bmx::updateCameraListeners);
            t.updateCameraBlur = runtime.lookupFunction(sym::bmx::updateCameraBlur);
            if (!t.updateCameras || !t.updateObjectCamera || !t.disableDoppler || !t.updateCameraListeners || !t.updateCameraBlur ||
                !runtime.replaceFunction(sym::bmx::updateCameras, &updateCamerasHook) ||
                !runtime.replaceFunction(sym::bmx::updateObjectCamera, &updateObjectCameraHook) ||
                !runtime.replaceFunction(sym::bmx::disableDoppler, &disableDopplerHook) ||
                !runtime.replaceFunction(sym::bmx::updateCameraListeners, &updateCameraListenersHook) ||
                !runtime.replaceFunction(sym::bmx::updateCameraBlur, &updateCameraBlurHook))
            {
                std::cerr << "[bmx-xxx] the cameras' hidden twin not installed" << std::endl;
                return false;
            }
            return true;
        }
    }
}

namespace bmx_xxx
{
    bool installView(PS2Machine &runtime)
    {
        namespace zv = zge_host::view;
        using namespace view;
        bool ok = zv::installMotionBlur(runtime, {"bmx-xxx", sym::bmx::drawBlur});
        ok = zv::installWidescreen(runtime, {"bmx-xxx", "bmx.widescreen.frustum", sym::bmx::updateProjection,
                                             sym::bmx::updateFrustum, sym::bmx::angleSin,
                                             sym::bmx::vif1PacketCloseUnpack, sym::bmx::drawWidget,
                                             sym::bmx::g_screenPrimitives, sym::bmx::g_displaySetupFlags + kDisplayWidth,
                                             nullptr, 0u, &ridden, &hudSideAnchored, &drawnFieldOfView}) && ok;
        ok = installTwin(runtime) && ok;
        ok = zv::installDrawDistance(runtime, {"bmx-xxx", sym::bmx::setCameraFarPlane, 0u, 0u,
                                               sym::bmx::moveCameraClipRange, sym::bmx::draw2DPass,
                                               "bmx.draw_distance.2d_pass", sym::bmx::setLodTable, kNodeLods,
                                               {1.0f, 2.0f, 4.0f}, {1000.0f, 1000.0f, 2500.0f}}) && ok;
        return ok;
    }
}
