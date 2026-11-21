// Dave Mirra Freestyle BMX 2 (SLUS-20159): photo mode, Aggressive Inline's
// (aggressive_inline_photo.cpp) at DM2's own code.
//
// The pause menu's PHOTO MODE line and the photo_mode key ([hotkeys] in
// controls.ini, P) stop the game, hide every widget and give a free camera,
// with the filters and the Screenshot key on top (zge_photo.h, the host side
// both games share); leaving puts everything back as it was.
//
// Stopping the game: photo mode runs inside the game's own pause, the
// action's screen mode with the pause screen up (games/dm2/action/
// action_frame.cpp), which already holds the riders, the level, the timers
// and the sound. From play the key opens it with a Start press first, and
// after it a Cross press on the menu's first line, Continue, closes it, as
// the player would (Start does not close DM2's pause menu). While photo mode is on
// the screen mode's frame (screenModeFrame: the menus' input, the screen's
// frame and the follow camera) is not run; in its place the free camera is
// written into the camera player's camera state and applied as the follow
// camera applies its own (applyCameraState: the camera node's position,
// rotation and field of view, the sound's listener). The camera record, the
// state and the applied state are kept when photo mode starts and put back
// when it ends, so the follow camera goes on from where it was.
//
// Next Frame (zge_photo.h's frame step): play's frame (actionFrame: the
// level's objects, the replays, the riders with their idle pads, the NPCs,
// the game and the follow camera) runs once in the screen mode's frame, as
// play would run it, and the free camera is applied over the camera it
// followed; the pause screen stays up. actionFrame's callees do not wait, so
// it runs as one guest call.
//
// Widgets: zgui's widget draw is wrapped in dave_mirra_gameplay.cpp
// (hidesUi); with no widget drawn the pause menu stands still. On with the
// app's settings (ZX_GAME_SETTINGS_MENUS=1), as the gameplay pages.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_photo.cpp.h"
#include "overrides/zge_photo.h"

#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/photo.h"
#include "runtime/session.h"
#include "runtime/settings.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace dave_mirra::photo
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace ph = zx::photo;
        namespace st = zx::settings;

        // DM2's camera (games/dm2/camera/camera.h), as the host reads it.
        struct Vec4
        {
            float x, y, z, w;
        };
        // A player's camera record (PlayerCamera): kept and put back whole.
        struct PlayerCamera
        {
            uint8_t modes[2];
            uint8_t view;
            uint8_t groundKind;
            uint32_t frames;
            int16_t angle;
            uint8_t cut;
            uint8_t flags;
            uint16_t blockedFrames;
            uint8_t unused0[2];
            Vec4 position;        // where the camera is
            Vec4 wantedPosition;
            Vec4 easedPosition;
            Vec4 target;          // the point it looks at
            Vec4 wantedTarget;
            Vec4 velocity;
            float fieldOfView;    // degrees
            float wantedFieldOfView;
            uint8_t unused1[8];
            Vec4 rows[4];
        };
        static_assert(offsetof(PlayerCamera, position) == 0x10 && offsetof(PlayerCamera, target) == 0x40);
        static_assert(offsetof(PlayerCamera, fieldOfView) == 0x70 && sizeof(PlayerCamera) == 0xC0);
        // What the camera node is given (CameraState).
        struct CameraState
        {
            uint32_t node;        // the camera node
            int16_t fieldOfView;  // 0x10000 a turn
            uint8_t unused[0x0A];
            Vec4 position;
            Vec4 target;
            Vec4 rows[4];         // right, up, forward, translation
        };
        static_assert(offsetof(CameraState, fieldOfView) == 0x04 && offsetof(CameraState, position) == 0x10);
        static_assert(offsetof(CameraState, rows) == 0x30 && sizeof(CameraState) == 0x70);
        constexpr float kFovUnitsPerDegree = 65536.0f / 360.0f;

        constexpr uint8_t kGameStateLevel = 1;     // games/dm2/action/action.h
        constexpr uint8_t kActionPlay = 0, kActionScreen = 1; // ActionMode
        constexpr uint8_t kPlayPausable = 1;       // play's Start is live
        constexpr uint8_t kScreenPause = 1;        // games/dm2/overlay/overlay.h
        // How far the camera may go from where photo mode started: the level
        // around the rider is all there is to see.
        constexpr float kMaxDistance = 40.0f;
        // Pause frames before photo mode starts from the key: the pause
        // menu's Show gives its menu the focus first (and with it the
        // Continue that closes it after).
        constexpr uint32_t kPauseSettleFrames = 20u;
        constexpr int kRegSp = 29, kRegRa = 31;

        enum class State
        {
            Idle,
            Opening, // the key in play: Start pressed, waiting for the pause to settle
            Active,
            Closing, // opened by the key: Continue chosen, waiting for play
        };

        struct Photo
        {
            PS2Machine *runtime = nullptr;
            PS2Machine::RecompiledFunction screenModeFrame = nullptr;
            zge_host::photo::FreeLook look;

            std::atomic<State> state{State::Idle};
            std::atomic<bool> menuRequest{false}; // the pause line, or the key in the pause
            bool fromKey = false;
            uint64_t stateAt = 0;     // VBlank of the last state change
            uint32_t pauseFrames = 0; // pause frames while Opening
            bool continuePressed = false;
            uint32_t hotkeySeen = 0;
            uint32_t player = 0;      // the camera's player when photo mode started

            PlayerCamera savedRecord{};
            CameraState savedState{};
            CameraState savedApplied{};
        };
        Photo &photo() { return zx::session::state<Photo>(); }

        // ---- memory -----------------------------------------------------------

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
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

        uint32_t recordAddress(uint32_t player) { return sym::dm2::g_playerCameras + player * static_cast<uint32_t>(sizeof(PlayerCamera)); }
        uint32_t stateAddress(uint32_t player)
        {
            return sym::dm2::g_cameraStates + player * static_cast<uint32_t>(sizeof(CameraState));
        }

        ph::Vec3 vec3(const Vec4 &v) { return {v.x, v.y, v.z}; }
        void setVec3(Vec4 &out, const ph::Vec3 &v)
        {
            out.x = v.x;
            out.y = v.y;
            out.z = v.z;
        }

        bool inLevel(const uint8_t *rdram) { return readByte(rdram, sym::dm2::g_currentGameState) == kGameStateLevel; }
        bool paused(const uint8_t *rdram)
        {
            return inLevel(rdram) && readByte(rdram, sym::dm2::g_actionMode) == kActionScreen &&
                   readByte(rdram, sym::dm2::g_gameScreen) == kScreenPause;
        }
        bool playing(const uint8_t *rdram)
        {
            return inLevel(rdram) && readByte(rdram, sym::dm2::g_actionMode) == kActionPlay &&
                   readByte(rdram, sym::dm2::g_actionStep) == kPlayPausable;
        }

        void setState(State state)
        {
            photo().state = state;
            photo().stateAt = photo().look.vsync();
        }

        // The camera's rows as the follow camera makes them from its place
        // and target (rowsFromTarget: forward to the target, right from the
        // world's up, y down, and up from the two), turned about forward by
        // the free camera's roll.
        void cameraRows(const ph::FreeCamera &camera, float roll, Vec4 rows[4])
        {
            const ph::Vec3 forward = camera.forward();
            ph::Vec3 right = ph::normalized(ph::cross(camera.worldUp, forward));
            ph::Vec3 up = ph::cross(forward, right);
            if (roll != 0.0f)
                ph::rollRows(right, up, -roll);
            setVec3(rows[0], right);
            setVec3(rows[1], up);
            setVec3(rows[2], forward);
            setVec3(rows[3], camera.position);
        }

        // ---- start and end ----------------------------------------------------

        void start(uint8_t *rdram)
        {
            Photo &p = photo();
            p.player = readByte(rdram, sym::dm2::g_cameraPlayer);
            readBytes(rdram, recordAddress(p.player), &p.savedRecord, sizeof(p.savedRecord));
            readBytes(rdram, stateAddress(p.player), &p.savedState, sizeof(p.savedState));
            readBytes(rdram, sym::dm2::g_appliedCameraState, &p.savedApplied, sizeof(p.savedApplied));
            const Vec4 &eye = p.savedRecord.position;
            const Vec4 &target = p.savedRecord.target;
            const float fov = p.savedRecord.fieldOfView;
            p.look.begin(vec3(eye), vec3(target), fov, kMaxDistance);
            setState(State::Active);
            // The free camera's rows at the start against the game's own: the
            // same place, so the picture does not jump.
            Vec4 rows[4];
            cameraRows(p.look.camera(), 0.0f, rows);
            float apart = 0.0f;
            for (int r = 0; r < 3; ++r)
                apart = std::fmax(apart, std::fabs(rows[r].x - p.savedApplied.rows[r].x) +
                                             std::fabs(rows[r].y - p.savedApplied.rows[r].y) +
                                             std::fabs(rows[r].z - p.savedApplied.rows[r].z));
            std::cout << "[dave-mirra] photo on" << (p.fromKey ? " (key)" : " (pause menu)") << ": player " << p.player
                      << ", eye " << eye.x << " " << eye.y << " " << eye.z << ", fov " << fov << ", rows within " << apart
                      << " of the game's" << std::endl;
        }

        void finish(uint8_t *rdram)
        {
            Photo &p = photo();
            writeBytes(rdram, recordAddress(p.player), &p.savedRecord, sizeof(p.savedRecord));
            writeBytes(rdram, stateAddress(p.player), &p.savedState, sizeof(p.savedState));
            writeBytes(rdram, sym::dm2::g_appliedCameraState, &p.savedApplied, sizeof(p.savedApplied));
            p.look.end();
            p.continuePressed = false;
            setState(p.fromKey ? State::Closing : State::Idle);
            std::cout << "[dave-mirra] photo off" << std::endl;
        }

        // The free camera into the camera player's state, applied as the
        // follow camera applies its own.
        void applyCamera(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            const ph::FreeCamera &camera = p.look.camera();
            CameraState state = p.savedState;
            state.fieldOfView = static_cast<int16_t>(std::lround(camera.fov * kFovUnitsPerDegree));
            setVec3(state.position, camera.position);
            setVec3(state.target, camera.target());
            cameraRows(camera, p.look.roll(), state.rows);
            const uint32_t at = stateAddress(p.player);
            if (writeBytes(rdram, at, &state, sizeof(state)))
            {
                const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
                runtime->callGuestFunction(rdram, ctx, sym::dm2::applyCameraState, {at}, sp);
            }
        }

        // Next Frame: play's frame once, the game still paused after it.
        void stepFrame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
            uint32_t result = 0;
            const bool ran = runtime->callGuestFunction(rdram, ctx, sym::dm2::actionFrame, {}, sp, &result);
            std::cout << "[dave-mirra] photo: frame step " << photo().look.framesStepped()
                      << (ran ? "" : ", play's frame did not return") << (result != 0u ? ", play's frame paused" : "")
                      << std::endl;
        }

        // ---- hooks ------------------------------------------------------------

        // The screen mode's frame: the free camera in its place while photo
        // mode is on (and a request from the line or the key starts it).
        void screenModeFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            const State state = p.state.load();
            if (state != State::Active && paused(rdram))
            {
                const bool settled = state == State::Opening && ++p.pauseFrames >= kPauseSettleFrames;
                if (p.menuRequest.exchange(false) || settled)
                {
                    p.fromKey = settled;
                    start(rdram);
                }
            }
            if (p.state.load() == State::Active)
            {
                if (p.look.frame())
                {
                    if (p.look.takeFrameStep())
                        stepFrame(rdram, ctx, runtime);
                    applyCamera(rdram, ctx, runtime);
                    setReturnU32(ctx, 0u);
                    ctx->pc = getRegU32(ctx, kRegRa);
                    return;
                }
                finish(rdram);
            }
            p.screenModeFrame(rdram, ctx, runtime);
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
                    p.menuRequest = true; // back to the pause menu after
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
                    std::cout << "[dave-mirra] photo: the pause did not open" << std::endl;
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
                if (!p.continuePressed && now >= p.stateAt + 1u)
                {
                    // The pause menu's Continue (its first line, still the
                    // selected one: the game had an idle pad meanwhile).
                    PSPadBackend::pressButton(hi::kCross, 3);
                    p.continuePressed = true;
                    std::cout << "[dave-mirra] photo: Continue, back to play" << std::endl;
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
        p.screenModeFrame = runtime.lookupFunction(sym::dm2::screenModeFrame);
        if (!p.screenModeFrame || !runtime.lookupFunction(sym::dm2::applyCameraState) ||
            !runtime.lookupFunction(sym::dm2::actionFrame) ||
            !runtime.replaceFunction(sym::dm2::screenModeFrame, &screenModeFrameHook))
        {
            std::cerr << "[dave-mirra] photo mode hooks not installed" << std::endl;
            return false;
        }
        p.hotkeySeen = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
        runtime.addVSyncHook(&onVsync);
        // For tests and the app menu: `set photo.mode 1` on ZX_CONTROL.
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
        restart::addPhotoLine(runtime);
        std::cout << "[dave-mirra] photo mode: the pause menu line and the photo_mode key" << std::endl;
        return true;
    }

    bool hidesUi()
    {
        return photo().state.load() == State::Active;
    }

    void requestFromPause()
    {
        photo().fromKey = false;
        photo().menuRequest = true;
    }
}
