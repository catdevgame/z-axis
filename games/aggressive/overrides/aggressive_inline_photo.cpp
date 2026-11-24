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
// Next Frame (zge_photo.h's frame step): one frame of play with photo mode
// still on, the game paused again after it. The level's frame
// (updatePlayersStep) runs the update of the play mode the level is in
// (Level::mode, a row of the mode table modeRecords); for the frame after
// the player asks, the level's mode is play's row (updatePlayMode: the pads,
// idle, the objects, the skaters and the effects), then the pause's again
// at the start of the next frame (the word as it was, logged with it).
// Neither mode's exit or enter runs, so the pause stays as it was (its
// sound, its hidden menu); the camera is still the free one (mode 5, its
// eye and point as written). A play frame that ends the run (a timer, a
// goal) changes the mode itself: that one is kept, and photo mode ends as
// when the level ends.
//
// The free camera, its input (the game gets an idle pad on every port, so the
// pause menu does not move), the help, the filters and the Screenshot key
// are the host side the engine's games share (zge_photo.h). Widgets: the
// zgui widget draw is wrapped in aggressive_inline_gameplay.cpp (hidesUi).

#include "aggressive_inline_photo.h"
#include "sym/game_overrides/aggressive_inline_photo.cpp.h"

#include "aggressive_inline_language.h"
#include "overrides/zge_photo.h"
#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/photo.h"
#include "runtime/settings_registry.h"
#include "runtime/session.h"

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
        namespace ph = zx::photo;
        namespace st = zx::settings;

        constexpr uint32_t kInputMode = sym::g_gameMode; // 1 front end, 2 play, 3 menus
        constexpr uint32_t kInputPlay = 2u;
        constexpr uint32_t kLevelFrames = sym::g_levelFrame; // main-loop frames since the level started
        constexpr uint32_t kTopState = sym::g_topState; // the top state (game_state.h TopState)
        constexpr uint32_t kTopStateLevel = 0x10u;      // TopState::level, the Level in a level
        constexpr uint32_t kLevelMode = 0x04u;          // Level::mode, its row of modeRecords
        constexpr uint32_t kPlayModeRows = sym::modeRecords;
        constexpr uint32_t kPlayModeRowSize = 0x14u;    // sizeof(PlayMode)
        constexpr uint32_t kModePlay = 2u;              // PlayModeId kModePlaying
        constexpr uint32_t kModePause = 3u;             // kModePaused
        constexpr uint32_t kLevelFrame = sym::updatePlayersStep; // the level's frame: its mode's update
        constexpr uint32_t kPlayerOnePosition = sym::g_playerPosition;
        constexpr uint32_t kPauseUpdate = sym::restorePlayCameras;
        constexpr uint32_t kLookAtMatrix = sym::lookAtMatrix;
        constexpr uint32_t kCamera = sym::g_followCameras;
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
            PS2Machine *runtime = nullptr;
            PS2Machine::RecompiledFunction pauseUpdate = nullptr;
            PS2Machine::RecompiledFunction lookAtMatrix = nullptr;
            PS2Machine::RecompiledFunction levelFrame = nullptr;

            std::atomic<State> state{State::Idle};
            std::atomic<bool> menuRequest{false}; // the pause item
            std::atomic<bool> keyRequest{false};
            bool fromKey = false;
            uint64_t stateAt = 0; // vsync of the last state change
            uint32_t pauseFrames = 0; // pause updates while Opening
            bool startPressed = false;
            uint32_t hotkeySeen = 0;
            // Next Frame: the level is in play's mode for this frame.
            std::atomic<bool> stepping{false};
            uint64_t steppedAt = 0; // vsync of the step's start
            ph::Vec3 stepFrom;      // player one's position before it
            uint32_t modeBefore = 0; // the level's mode word before it (the pause's row), put back after

            std::array<uint8_t, kCameraSize> savedCamera{};
            // The camera's scene node (camera +0x00): position +0x10 and
            // rotation quaternion +0x20, which the look-at writes and the
            // follow camera goes on from.
            uint32_t savedNode = 0;
            std::array<uint8_t, kNodePoseSize> savedNodePose{};
            zge_host::photo::FreeLook look;
        };
        Photo &photo() { return zx::session::state<Photo>(); }

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

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
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

        // The level being played, 0 outside one.
        uint32_t currentLevel(const uint8_t *rdram)
        {
            const uint32_t state = readWord(rdram, kTopState);
            return state ? readWord(rdram, state + kTopStateLevel) : 0u;
        }

        // A play mode's row of the mode table.
        uint32_t playModeRow(uint32_t mode) { return kPlayModeRows + mode * kPlayModeRowSize; }

        // The level's play mode (its row's id), -1 when there is none
        // (outside a level).
        int gameMode(const uint8_t *rdram)
        {
            const uint32_t level = currentLevel(rdram);
            const uint32_t row = level ? readWord(rdram, level + kLevelMode) : 0u;
            return row ? static_cast<int>(readWord(rdram, row)) : -1;
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
        void pauseUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
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

        // The level's frame: Next Frame's frame of play (the level in play's
        // mode for it), then the pause's mode back for the frame after.
        void levelFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Photo &p = photo();
            const uint32_t level = currentLevel(rdram);
            if (p.stepping.load())
            {
                // The word as it was, unless the play frame moved the level on
                // to another mode itself (then that one stays).
                const bool stillPlay = level && readWord(rdram, level + kLevelMode) == playModeRow(kModePlay);
                if (stillPlay)
                    writeWord(rdram, level + kLevelMode, p.modeBefore);
                p.stepping = false;
                const uint32_t after = level ? readWord(rdram, level + kLevelMode) : 0u;
                const ph::Vec3 to = readVec(rdram, kPlayerOnePosition);
                std::cout << "[photo] frame step " << p.look.framesStepped() << ": player one from " << p.stepFrom.x
                          << " " << p.stepFrom.y << " " << p.stepFrom.z << " to " << to.x << " " << to.y << " " << to.z
                          << "; the level's mode word 0x" << std::hex << p.modeBefore << " before, 0x" << after
                          << " after" << std::dec
                          << (after == p.modeBefore ? " (put back)" : stillPlay ? " (NOT put back)"
                                                                               : " (the play frame changed the mode)")
                          << std::endl;
            }
            else if (p.state.load() == State::Active && level &&
                     readWord(rdram, level + kLevelMode) == playModeRow(kModePause) && p.look.takeFrameStep())
            {
                p.stepFrom = readVec(rdram, kPlayerOnePosition);
                p.modeBefore = readWord(rdram, level + kLevelMode);
                writeWord(rdram, level + kLevelMode, playModeRow(kModePlay));
                p.steppedAt = p.look.vsync();
                p.stepping = true;
            }
            p.levelFrame(rdram, ctx, runtime);
        }

        // The look-at matrix: the free camera's roll about the view axis
        // (rows: +0x00 right, +0x10 up, +0x20 forward, +0x30 position).
        void lookAtMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
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
                else if (input == kInputPlay && mode == 2 && readWord(rdram, kLevelFrames) > 30u)
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
                // (a frame step is play's frame: no more than a few vsyncs) One that did not come: the step not
                // taken, the pause's mode put back where it is still play's, so photo mode goes on paused (else the
                // check below takes the level for left and photo mode ends with the camera and the mode as the
                // step left them).
                if (photo().stepping.load() && photo().look.vsync() > photo().steppedAt + 10u)
                {
                    const uint32_t level = currentLevel(rdram);
                    if (level && readWord(rdram, level + kLevelMode) == playModeRow(kModePlay))
                        writeWord(rdram, level + kLevelMode, photo().modeBefore);
                    photo().stepping = false;
                    std::cout << "[photo] frame step " << photo().look.framesStepped() << ": no frame of play came; paused again"
                              << std::endl;
                }
                if (!photo().stepping.load() && mode != static_cast<int>(kModePause) &&
                    photo().look.vsync() > photo().stateAt + 2u)
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

    void install(PS2Machine &runtime)
    {
        photo().runtime = &runtime;
        photo().look.setRuntime(&runtime);
        photo().look.setTranslate(&word);
        photo().pauseUpdate = runtime.lookupFunction(kPauseUpdate);
        photo().lookAtMatrix = runtime.lookupFunction(kLookAtMatrix);
        photo().levelFrame = runtime.lookupFunction(kLevelFrame);
        if (!photo().pauseUpdate || !photo().lookAtMatrix || !photo().levelFrame ||
            !runtime.replaceFunction(kPauseUpdate, &pauseUpdateHook) ||
            !runtime.replaceFunction(kLookAtMatrix, &lookAtMatrixHook) || !runtime.replaceFunction(kLevelFrame, &levelFrameHook))
        {
            std::cerr << "[photo] hooks not installed" << std::endl;
            return;
        }
        photo().hotkeySeen = PSPadBackend::hotkeyPresses(hi::kHotkeyPhotoMode);
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
        std::cout << "[photo] photo mode: the pause menu item and the photo_mode key" << std::endl;
    }

    bool hidesUi()
    {
        return photo().state.load() == State::Active;
    }
}

namespace aggressive_inline
{
    void photoModeNative(uint8_t *, R5900Context *ctx, PS2Machine *)
    {
        photo::photo().fromKey = false;
        photo::photo().menuRequest = true;
        ctx->pc = getRegU32(ctx, 31);
    }
}
