// BMX XXX (SLUS-20415): photo mode, Aggressive Inline's and Dave Mirra BMX
// 2's (aggressive_inline_photo.cpp, dave_mirra_photo.cpp) at BMX XXX's own
// code.
//
// The pause menu's PHOTO MODE line (bmx_xxx_exit.cpp) and the photo_mode
// key ([hotkeys] in controls.ini, P) in a level stop the game, hide every
// widget and give a free camera, with the filters and the Screenshot key on
// top (zge_photo.h, the host side the games share); leaving puts everything
// back as it was (from the line, the pause menu again).
//
// Stopping the game: photo mode runs inside the game's own pause, the pause
// menu's game screen (games/bmx/overlay/screens.h kScreenPause), which already
// holds the riders, the level, the timers and the sound. From play the key
// opens it with a Start press first, and after it a Start press closes it
// again, as the player would. While photo mode is on, the screen state's frame
// (screenStateFrame, games/bmx/action/level_frame.cpp: the screens' input, the
// level's objects under some screens and the camera update) is not run; in
// its place the free camera is written into the player's camera's view
// (ActionCamera::view: the field of view, the eye, the point looked at and
// the axes as aimCamera makes them: forward to the point, right from the
// world's up, -y, and up from the two) and handed to the scene camera by
// setCameraView, as the camera update hands its own. The camera's record is
// kept when photo mode starts and put back when it ends, so the camera goes
// on from where it was.
//
// Next Frame (zge_photo.h's frame step): play's frame (playStateFrame: the
// level's objects and animators, the replays, the riders with their idle
// pads, the NPCs, the projectiles, the game and the cameras) runs once in
// the screen state's frame, as play would run it, and the free camera is
// handed to the scene camera over the one the cameras made; the pause
// screen stays up. playStateFrame's callees do not wait, so it runs as one
// guest call.
//
// Widgets: zgui's widget draw is wrapped in bmx_xxx_gameplay.cpp (hidesUi).
// On with the app's settings (ZX_GAME_SETTINGS_MENUS=1), as the gameplay
// pages; `set photo.mode 1` on ZX_CONTROL for checks.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_photo.cpp.h"
#include "overrides/zge_photo.h"

#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/photo.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace bmx_xxx::photo
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace ph = zx::photo;
        namespace st = zx::settings;

        // An action camera (games/bmx/camera/action_camera.h), as the host
        // reads it: the record, and what it shows (CameraView).
        struct Vec4
        {
            float x, y, z, w;
        };
        struct CameraView
        {
            int16_t fov;      // a 16-bit angle
            uint8_t _02[0x0E];
            Vec4 eye;
            Vec4 lookAt;
            Vec4 axes[4];     // right, up, forward, and a row the view leaves
            uint32_t object;
            uint8_t _74[0x0C];
        };
        static_assert(offsetof(CameraView, eye) == 0x10 && offsetof(CameraView, axes) == 0x30 && sizeof(CameraView) == 0x80);
        constexpr uint32_t kActionCameraBytes = 0x190;
        constexpr uint32_t kCameraFlags = 0x00, kCameraKind = 0x01, kCameraFov = 0x90, kCameraView = 0xF0, kCameraRider = 0x170;
        constexpr uint8_t kCameraShown = 0x02, kRiderCamera = 1, kPlayerOneRider = 0;
        constexpr uint32_t kActionCameras = 8;
        constexpr float kAnglesADegree = 65536.0f / 360.0f;

        // games/bmx/states/game_state.h, games/bmx/overlay/screens.h.
        constexpr uint8_t kGameStateLevel = 1, kStateRunning = 0;
        constexpr uint32_t kScreenPlay = 1, kScreenPause = 7;
        // How far the camera may go from where photo mode started.
        constexpr float kMaxDistance = 40.0f;
        // Pause frames before photo mode starts from the key: the pause
        // menu's own coming in first.
        constexpr uint32_t kPauseSettleFrames = 20u;
        constexpr int kRegSp = 29, kRegRa = 31;

        enum class State
        {
            Idle,
            Opening, // the key in play: Start pressed, waiting for the pause to settle
            Active,
            Closing, // opened by the key: Start pressed again, waiting for play
        };

        struct Photo
        {
            PS2Machine *runtime = nullptr;
            PS2Machine::RecompiledFunction screenStateFrame = nullptr;
            zge_host::photo::FreeLook look;

            std::atomic<State> state{State::Idle};
            std::atomic<bool> request{false}; // the pause line, or the key in the pause
            bool fromKey = false;
            uint64_t stateAt = 0;     // VBlank of the last state change
            uint32_t pauseFrames = 0; // pause frames while Opening
            bool startPressed = false;
            uint32_t hotkeySeen = 0;
            uint32_t camera = 0;      // the player's camera's record
            uint8_t savedCamera[kActionCameraBytes] = {};
        };
        Photo &photo() { return zx::session::state<Photo>(); }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        bool readBytes(const uint8_t *rdram, uint32_t address, void *out, size_t size)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || getConstMemPtr(rdram, address + static_cast<uint32_t>(size) - 1u) != p + size - 1u)
                return false;
            std::memcpy(out, p, size);
            return true;
        }

        bool writeBytes(uint8_t *rdram, uint32_t address, const void *in, size_t size)
        {
            uint8_t *p = getMemPtr(rdram, address);
            if (!p || getMemPtr(rdram, address + static_cast<uint32_t>(size) - 1u) != p + size - 1u)
                return false;
            std::memcpy(p, in, size);
            return true;
        }

        bool inLevel(const uint8_t *rdram)
        {
            return readByte(rdram, sym::bmx::g_currentGameState) == kGameStateLevel &&
                   readByte(rdram, sym::bmx::g_gameStatePhase) == kStateRunning;
        }
        bool paused(const uint8_t *rdram) { return inLevel(rdram) && readWord(rdram, sym::bmx::g_gameScreen) == kScreenPause; }
        bool playing(const uint8_t *rdram) { return inLevel(rdram) && readWord(rdram, sym::bmx::g_gameScreen) == kScreenPlay; }

        // Player 1's camera: the shown rider's camera on its rider, else the
        // first shown camera (the game's own).
        uint32_t playerCamera(const uint8_t *rdram)
        {
            uint32_t shown = 0;
            for (uint32_t i = 0; i < kActionCameras; ++i)
            {
                const uint32_t camera = sym::bmx::g_actionCameras + i * kActionCameraBytes;
                if (!(readByte(rdram, camera + kCameraFlags) & kCameraShown))
                    continue;
                if (readByte(rdram, camera + kCameraKind) == kRiderCamera &&
                    readByte(rdram, camera + kCameraRider) == kPlayerOneRider)
                    return camera;
                if (!shown)
                    shown = camera;
            }
            return shown;
        }

        void setState(State state)
        {
            photo().state = state;
            photo().stateAt = photo().look.vsync();
        }

        void setVec3(Vec4 &out, const ph::Vec3 &v)
        {
            out.x = v.x;
            out.y = v.y;
            out.z = v.z;
        }
        ph::Vec3 vec3(const Vec4 &v) { return ph::Vec3{v.x, v.y, v.z}; }

        // The view's axes as aimCamera makes them from the eye and the point
        // looked at, turned about forward by the free camera's roll.
        void cameraAxes(const ph::FreeCamera &camera, float roll, Vec4 axes[4])
        {
            const ph::Vec3 forward = camera.forward();
            ph::Vec3 right = ph::normalized(ph::cross(camera.worldUp, forward));
            ph::Vec3 up = ph::cross(forward, right);
            if (roll != 0.0f)
                ph::rollRows(right, up, -roll);
            setVec3(axes[0], right);
            setVec3(axes[1], up);
            setVec3(axes[2], forward);
        }

        // The view handed to the scene camera, as the camera update does.
        void showView(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, const CameraView &view)
        {
            const uint32_t at = photo().camera + kCameraView;
            if (writeBytes(rdram, at, &view, sizeof(view)))
            {
                const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
                runtime->callGuestFunction(rdram, ctx, sym::bmx::setCameraView, {photo().camera, at}, sp);
            }
        }

        bool start(uint8_t *rdram)
        {
            Photo &p = photo();
            p.camera = playerCamera(rdram);
            if (!p.camera || !readBytes(rdram, p.camera, p.savedCamera, sizeof(p.savedCamera)))
                return false;
            CameraView view{};
            std::memcpy(&view, p.savedCamera + kCameraView, sizeof(view));
            float fov = 0.0f;
            std::memcpy(&fov, p.savedCamera + kCameraFov, sizeof(fov));
            p.look.begin(vec3(view.eye), vec3(view.lookAt), fov, kMaxDistance);
            setState(State::Active);
            // The free camera's axes at the start against the game's own: the
            // same place, so the picture does not jump.
            Vec4 axes[4] = {};
            cameraAxes(p.look.camera(), 0.0f, axes);
            float apart = 0.0f;
            for (int r = 0; r < 3; ++r)
                apart = std::fmax(apart, std::fabs(axes[r].x - view.axes[r].x) + std::fabs(axes[r].y - view.axes[r].y) +
                                             std::fabs(axes[r].z - view.axes[r].z));
            std::cout << "[bmx-xxx] photo on" << (p.fromKey ? " (key from play)" : " (the pause)") << ": camera 0x"
                      << std::hex << p.camera << std::dec << ", eye " << view.eye.x << " " << view.eye.y << " "
                      << view.eye.z << ", fov " << fov << ", axes within " << apart << " of the game's" << std::endl;
            return true;
        }

        void finish(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            writeBytes(rdram, p.camera, p.savedCamera, sizeof(p.savedCamera));
            CameraView view{};
            std::memcpy(&view, p.savedCamera + kCameraView, sizeof(view));
            showView(rdram, ctx, runtime, view);
            p.look.end();
            p.startPressed = false;
            setState(p.fromKey ? State::Closing : State::Idle);
            std::cout << "[bmx-xxx] photo off" << std::endl;
        }

        void applyCamera(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            const ph::FreeCamera &camera = p.look.camera();
            CameraView view{};
            std::memcpy(&view, p.savedCamera + kCameraView, sizeof(view));
            view.fov = static_cast<int16_t>(std::lround(camera.fov * kAnglesADegree));
            setVec3(view.eye, camera.position);
            setVec3(view.lookAt, camera.target());
            cameraAxes(camera, p.look.roll(), view.axes);
            showView(rdram, ctx, runtime, view);
        }

        // Next Frame: play's frame once, the game still paused after it.
        void stepFrame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
            uint32_t result = 0;
            const bool ran = runtime->callGuestFunction(rdram, ctx, sym::bmx::playStateFrame, {}, sp, &result);
            std::cout << "[bmx-xxx] photo: frame step " << photo().look.framesStepped()
                      << (ran ? "" : ", play's frame did not return") << (result != 0u ? ", play's frame paused" : "")
                      << std::endl;
        }

        // The screen state's frame: the free camera in its place while photo
        // mode is on (and the key starts it).
        void screenStateFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            const State state = p.state.load();
            if (state != State::Active && paused(rdram))
            {
                const bool settled = state == State::Opening && ++p.pauseFrames >= kPauseSettleFrames;
                if (p.request.exchange(false) || settled)
                {
                    p.fromKey = settled;
                    if (!start(rdram))
                        setState(State::Idle);
                }
            }
            if (p.state.load() == State::Active)
            {
                if (p.look.frame())
                {
                    if (p.look.takeFrameStep())
                        stepFrame(rdram, ctx, runtime);
                    applyCamera(rdram, ctx, runtime);
                    setReturnU32(ctx, 0u); // the frame went on (as the original's end)
                    ctx->pc = getRegU32(ctx, kRegRa);
                    return;
                }
                finish(rdram, ctx, runtime);
            }
            p.screenStateFrame(rdram, ctx, runtime);
        }

        // VBlank: the key, and the Start presses around a key's photo mode.
        void onVsync(uint8_t *rdram, uint64_t)
        {
            Photo &p = photo();
            p.look.tick();
            const uint32_t presses = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
            const bool key = presses != p.hotkeySeen;
            p.hotkeySeen = presses;
            const uint64_t now = p.look.vsync();
            switch (p.state.load())
            {
            case State::Idle:
                if (!key)
                    break;
                if (paused(rdram))
                {
                    p.fromKey = false;
                    p.request = true; // back to the pause menu after
                }
                else if (playing(rdram))
                {
                    // Pause first, as the player would; the screen frame starts it.
                    PSPadBackend::pressButton(hi::kStart, 3);
                    p.pauseFrames = 0;
                    setState(State::Opening);
                }
                break;
            case State::Opening:
                if (now > p.stateAt + 90u + kPauseSettleFrames)
                {
                    std::cout << "[bmx-xxx] photo: the pause did not open" << std::endl;
                    setState(State::Idle);
                }
                break;
            case State::Active:
                if (key)
                    p.look.requestExit();
                if (!paused(rdram) && now > p.stateAt + 2u)
                {
                    // Out of the pause some other way (the level ended): nothing to put back.
                    p.look.end(false);
                    setState(State::Idle);
                }
                break;
            case State::Closing:
                if (!p.startPressed && now >= p.stateAt + 1u)
                {
                    PSPadBackend::pressButton(hi::kStart, 3);
                    p.startPressed = true;
                    std::cout << "[bmx-xxx] photo: Start, back to play" << std::endl;
                }
                if (!paused(rdram) || now > p.stateAt + 120u)
                    setState(State::Idle);
                break;
            }
        }
    }

    bool install(PS2Machine &runtime)
    {
        Photo &p = photo();
        p.runtime = &runtime;
        p.look.setRuntime(&runtime);
        p.screenStateFrame = runtime.lookupFunction(sym::bmx::screenStateFrame);
        if (!p.screenStateFrame || !runtime.lookupFunction(sym::bmx::setCameraView) ||
            !runtime.lookupFunction(sym::bmx::playStateFrame) ||
            !runtime.replaceFunction(sym::bmx::screenStateFrame, &screenStateFrameHook))
        {
            std::cerr << "[bmx-xxx] photo mode hooks not installed" << std::endl;
            return false;
        }
        p.hotkeySeen = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
        runtime.addVSyncHook(&onVsync);
        st::Setting open;
        open.key = "photo.mode";
        open.label = "Photo Mode";
        open.current = []() { return photo().state.load() == State::Active ? 1 : 0; };
        open.apply = [](int value)
        {
            if ((value != 0) != (photo().state.load() == State::Active))
                PSPadBackend::triggerHotkey(hi::kHotkeyPhotoMode);
        };
        zx::session::addSetting(open);
        // `set photo.step 1`: Next Frame, as the period key.
        st::Setting step;
        step.key = "photo.step";
        step.label = "Next Frame";
        step.current = []() { return 0; };
        step.apply = [](int value)
        {
            if (value != 0 && photo().state.load() == State::Active)
                photo().look.requestFrameStep();
        };
        zx::session::addSetting(step);
        exit_menu::addPhotoLine(runtime);
        std::cout << "[bmx-xxx] photo mode: the pause menu line and the photo_mode key" << std::endl;
        return true;
    }

    bool hidesUi()
    {
        return photo().state.load() == State::Active;
    }

    void requestFromPause()
    {
        photo().fromKey = false;
        photo().request = true;
    }
}
