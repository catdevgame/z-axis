// Aggressive Inline (SLUS-20327): photo mode.
//
// A pause menu item (Photo Mode, Career, Freeskate and Timed Run) and the
// photo_mode key ([hotkeys] in controls.ini, P) stop the game, hide every
// widget, and give a free camera; filters and screenshots on top; leaving
// puts everything back as it was.
//
// Stopping the game: photo mode runs inside the game's own pause (game mode
// 3 of the mode table 0x24B810: enter 0x1BBC30, exit 0x1BC300, update
// 0x1BC280), which already holds the level, the skaters, the timers and
// the sound; from play the key opens it with a Start press first (and a
// Start press in the pause menu closes it after, as the player would; the
// menu shows meanwhile: zgui runs its Show and Hide and its Start sniffer
// only while it draws them). The
// pause update only sets each camera's mode (0x18FD68 with the tuning
// table's Camera Type); in photo mode it is replaced by the free camera.
// The game's frame (0x1BCC58) runs the mode update, then the camera update
// 0x18FA00 for the player's camera at 0x24C9D0 (+0x00 its scene node, +0x10
// eye, +0x20 look-at point, +0x30 FOV in degrees, +0x71 mode). Camera mode
// 5 (the developers' Art Viewer's) only looks from the eye at the point
// (0x15E188: 0x13A260 builds the matrix with the world's up, y down, then
// the node gets the position and a quaternion) and sets the FOV, so the free
// camera writes eye, point and FOV and sets mode 5. 0x13A260 has no roll
// (its fourth argument is ignored): it is wrapped to turn the matrix's
// right and up rows about the view axis when called for this camera. The
// camera's 0x80 bytes are kept when photo mode starts and put back when it
// ends; the pause update sets the mode again the next frame, as before.
//
// The free camera, its input (the game gets an idle pad on every port, so the
// pause menu does not move), the help, the filters and the Screenshot key
// are the host side the engine's games share (zge_photo.h). Widgets: the
// zgui widget draw is wrapped in aggressive_inline_gameplay.cpp (hidesUi).

#include "aggressive_inline_photo.h"

#include "aggressive_inline_language.h"
#include "zge_photo.h"
#include "ps2_runtime.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_photo.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_session.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>

namespace aggressive_inline::photo
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace ph = ps2x::photo;
        namespace st = ps2x::settings;

        constexpr uint32_t kInputMode = 0x00276FE4u; // 1 front end, 2 play, 3 menus
        constexpr uint32_t kInputPlay = 2u;
        constexpr uint32_t kLevelFrame = 0x00231CACu;
        constexpr uint32_t kTopState = 0x0024C9BCu; // the top-level state; +0x10 -> +0x04 the game mode's entry
        constexpr uint32_t kModePause = 3u;
        constexpr uint32_t kPauseUpdate = 0x001BC280u;
        constexpr uint32_t kLookAtMatrix = 0x0013A260u;
        constexpr uint32_t kCamera = 0x0024C9D0u;
        constexpr uint32_t kCameraSize = 0x80u;
        constexpr uint32_t kCameraEye = kCamera + 0x10u;
        constexpr uint32_t kCameraTarget = kCamera + 0x20u;
        constexpr uint32_t kCameraFov = kCamera + 0x30u;
        constexpr uint32_t kCameraMode = kCamera + 0x71u;
        constexpr uint8_t kCameraModeLookAt = 5u;
        // How far the camera may go from where photo mode started: the
        // level around the player is all there is to see.
        constexpr float kMaxDistance = 40.0f;
        // Pause frames before photo mode starts from the key: the pause
        // menu's Show (15 frames) gives its menu the focus, and with it its
        // Start sniffer, which closes the pause after. zgui runs widget
        // actions and key sniffers while it draws them, so a hidden menu
        // stands still: it is shown until then.
        constexpr uint32_t kPauseSettleFrames = 20u;

        enum class State
        {
            Idle,
            Opening, // the key in play: Start pressed, waiting for the pause to settle
            Active,
            Closing, // opened by the key: Start pressed, waiting for play
        };

        constexpr uint32_t kNodePose = 0x10u;
        constexpr uint32_t kNodePoseSize = 0x20u;
        struct Photo
        {
            PS2Runtime *runtime = nullptr;
            PS2Runtime::RecompiledFunction pauseUpdate = nullptr;
            PS2Runtime::RecompiledFunction lookAtMatrix = nullptr;

            std::atomic<State> state{State::Idle};
            std::atomic<bool> menuRequest{false}; // the pause item
            std::atomic<bool> keyRequest{false};
            bool fromKey = false;
            uint64_t stateAt = 0; // vsync of the last state change
            uint32_t pauseFrames = 0; // pause updates while Opening
            bool startPressed = false;
            uint32_t hotkeySeen = 0;

            std::array<uint8_t, kCameraSize> savedCamera{};
            // The camera's scene node (camera +0x00): position +0x10 and
            // rotation quaternion +0x20, which the look-at writes and the
            // follow camera goes on from.
            uint32_t savedNode = 0;
            std::array<uint8_t, kNodePoseSize> savedNodePose{};
            zge_host::photo::FreeLook look;
        };
        Photo &photo() { return ps2x::session::state<Photo>(); }

        // ---- memory ---------------------------------------------------------

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        ph::Vec3 readVec(const uint8_t *rdram, uint32_t address)
        {
            return {readFloat(rdram, address), readFloat(rdram, address + 4u), readFloat(rdram, address + 8u)};
        }

        void writeVec(uint8_t *rdram, uint32_t address, const ph::Vec3 &v)
        {
            writeFloat(rdram, address, v.x);
            writeFloat(rdram, address + 4u, v.y);
            writeFloat(rdram, address + 8u, v.z);
        }

        // The game mode (the mode table's id of the current entry), -1 when
        // there is none (outside a level).
        int gameMode(const uint8_t *rdram)
        {
            const uint32_t state = readWord(rdram, kTopState);
            const uint32_t modes = state ? readWord(rdram, state + 0x10u) : 0u;
            const uint32_t entry = modes ? readWord(rdram, modes + 0x04u) : 0u;
            return entry ? static_cast<int>(readWord(rdram, entry)) : -1;
        }

        void setState(State state)
        {
            photo().state = state;
            photo().stateAt = photo().look.vsync();
        }

        // ---- words ----------------------------------------------------------

        // The port's own words in the language (Latin-1 from the table) as
        // UTF-8 for the overlay (zge_host::photo::Translate).
        std::string word(const std::string &english)
        {
            const std::string latin = aggressive_inline::language::translate(english);
            if (latin.empty())
                return english;
            std::string out;
            for (char c : latin)
            {
                const auto u = static_cast<uint8_t>(c);
                if (u < 0x80u)
                {
                    out.push_back(c);
                }
                else
                {
                    out.push_back(static_cast<char>(0xC0u | (u >> 6)));
                    out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
                }
            }
            return out;
        }

        // ---- the camera ---------------------------------------------------------

        void writeCamera(uint8_t *rdram)
        {
            const ph::FreeCamera &camera = photo().look.camera();
            writeVec(rdram, kCameraEye, camera.position);
            writeVec(rdram, kCameraTarget, camera.target());
            writeFloat(rdram, kCameraFov, camera.fov);
            if (uint8_t *p = getMemPtr(rdram, kCameraMode))
                *p = kCameraModeLookAt;
        }

        void start(uint8_t *rdram)
        {
            for (uint32_t i = 0; i < kCameraSize; ++i)
                if (const uint8_t *p = getConstMemPtr(rdram, kCamera + i))
                    photo().savedCamera[i] = *p;
            photo().savedNode = readWord(rdram, kCamera);
            for (uint32_t i = 0; photo().savedNode && i < kNodePoseSize; ++i)
                if (const uint8_t *p = getConstMemPtr(rdram, photo().savedNode + kNodePose + i))
                    photo().savedNodePose[i] = *p;
            setState(State::Active);
            photo().look.begin(readVec(rdram, kCameraEye), readVec(rdram, kCameraTarget), readFloat(rdram, kCameraFov),
                               kMaxDistance);
            writeCamera(rdram);
            const ph::FreeCamera &camera = photo().look.camera();
            std::cout << "[photo] on" << (photo().fromKey ? " (key)" : " (pause menu)") << ": eye " << camera.position.x
                      << " " << camera.position.y << " " << camera.position.z << ", yaw " << camera.yaw << ", pitch "
                      << camera.pitch << ", fov " << camera.fov << std::endl;
        }

        void finish(uint8_t *rdram)
        {
            for (uint32_t i = 0; i < kCameraSize; ++i)
                if (uint8_t *p = getMemPtr(rdram, kCamera + i))
                    *p = photo().savedCamera[i];
            if (photo().savedNode && readWord(rdram, kCamera) == photo().savedNode)
                for (uint32_t i = 0; i < kNodePoseSize; ++i)
                    if (uint8_t *p = getMemPtr(rdram, photo().savedNode + kNodePose + i))
                        *p = photo().savedNodePose[i];
            photo().look.end();
            photo().startPressed = false;
            setState(photo().fromKey ? State::Closing : State::Idle);
            std::cout << "[photo] off" << std::endl;
        }

        // ---- hooks ----------------------------------------------------------

        // The pause update: the free camera in its place while photo mode is
        // on (and a request from the item or the key starts it).
        void pauseUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const State state = photo().state.load();
            const bool settled = state == State::Opening && ++photo().pauseFrames >= kPauseSettleFrames;
            if (state != State::Active && (photo().menuRequest.exchange(false) || settled))
            {
                photo().fromKey = settled;
                start(rdram);
            }
            if (photo().state.load() == State::Active)
            {
                if (photo().look.frame())
                {
                    writeCamera(rdram);
                    setReturnU32(ctx, 1u);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
                finish(rdram);
            }
            photo().pauseUpdate(rdram, ctx, runtime);
        }

        // The look-at matrix: the free camera's roll about the view axis
        // (rows: +0x00 right, +0x10 up, +0x20 forward, +0x30 position).
        void lookAtMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t matrix = getRegU32(ctx, 4);
            const bool ours = photo().state.load() == State::Active && getRegU32(ctx, 5) == kCameraEye &&
                              getRegU32(ctx, 6) == kCameraTarget;
            photo().lookAtMatrix(rdram, ctx, runtime);
            const float roll = photo().look.roll();
            if (!ours || roll == 0.0f)
                return;
            ph::Vec3 right = readVec(rdram, matrix), up = readVec(rdram, matrix + 0x10u);
            ph::rollRows(right, up, -roll); // the game's rows: + turns the view clockwise
            writeVec(rdram, matrix, right);
            writeVec(rdram, matrix + 0x10u, up);
        }

        // VBlank: the key, and the Start presses around a key's photo mode.
        void onVsync(uint8_t *rdram, uint64_t)
        {
            photo().look.tick();
            const uint32_t presses = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
            const bool key = presses != photo().hotkeySeen;
            photo().hotkeySeen = presses;
            const uint32_t input = readWord(rdram, kInputMode);
            const int mode = gameMode(rdram);
            switch (photo().state.load())
            {
            case State::Idle:
                if (!key || input == 1u || mode < 0)
                    break;
                if (mode == static_cast<int>(kModePause))
                {
                    photo().fromKey = false;
                    photo().menuRequest = true; // back to the pause menu after
                }
                else if (input == kInputPlay && mode == 2 && readWord(rdram, kLevelFrame) > 30u)
                {
                    // Pause first, as the player would; the pause update starts it.
                    PSPadBackend::pressButton(hi::kStart, 3);
                    photo().pauseFrames = 0;
                    setState(State::Opening);
                }
                break;
            case State::Opening:
                if (photo().look.vsync() > photo().stateAt + 90u + kPauseSettleFrames)
                {
                    std::cout << "[photo] the pause did not open" << std::endl;
                    setState(State::Idle);
                }
                break;
            case State::Active:
                if (key)
                    photo().look.requestExit();
                if (mode != static_cast<int>(kModePause) && photo().look.vsync() > photo().stateAt + 2u)
                {
                    // Out of the pause some other way (the level ended): nothing to put back.
                    photo().look.end(false);
                    setState(State::Idle);
                }
                break;
            case State::Closing:
                if (!photo().startPressed && photo().look.vsync() >= photo().stateAt + 1u)
                {
                    // Closes the pause menu as Start does (its sniffer: OnCancel, Hide, back to play).
                    PSPadBackend::pressButton(hi::kStart, 3);
                    photo().startPressed = true;
                    std::cout << "[photo] Start: back to play" << std::endl;
                }
                if (mode != static_cast<int>(kModePause) || photo().look.vsync() > photo().stateAt + 120u)
                    setState(State::Idle);
                break;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        photo().runtime = &runtime;
        photo().look.setRuntime(&runtime);
        photo().look.setTranslate(&word);
        photo().pauseUpdate = runtime.lookupFunction(kPauseUpdate);
        photo().lookAtMatrix = runtime.lookupFunction(kLookAtMatrix);
        if (!photo().pauseUpdate || !photo().lookAtMatrix || !runtime.replaceFunction(kPauseUpdate, &pauseUpdateHook) ||
            !runtime.replaceFunction(kLookAtMatrix, &lookAtMatrixHook))
        {
            std::cerr << "[photo] hooks not installed" << std::endl;
            return;
        }
        photo().hotkeySeen = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
        runtime.addVSyncHook(&onVsync);
        // For tests and the app menu: `set photo.mode 1` on PS2X_CONTROL.
        st::Setting open;
        open.key = "photo.mode";
        open.label = "Photo Mode";
        open.current = []() { return photo().state.load() == State::Active ? 1 : 0; };
        open.apply = [](int value)
        {
            if ((value != 0) != (photo().state.load() == State::Active))
                PSPadBackend::triggerHotkey(hi::kHotkeyPhotoMode);
        };
        ps2x::session::addSetting(open);
        std::cout << "[photo] photo mode: the pause menu item and the photo_mode key" << std::endl;
    }

    bool hidesUi()
    {
        return photo().state.load() == State::Active;
    }
}

namespace aggressive_inline
{
    void photoModeNative(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        photo::photo().fromKey = false;
        photo::photo().menuRequest = true;
        ctx->pc = getRegU32(ctx, 31);
    }
}
