#pragma once

// Photo mode's host side, shared by the games built on the Z-Axis engine: a
// free camera moved by the keyboard, the mouse and the pad while the game
// reads an idle pad (PSPadBackend::setFreeInput), the help and status lines
// on the overlay's notice, the picture filters (PS2Machine::setPictureFilter,
// so the Screenshot key's pictures get them too), the framing grid (drawn
// over the window only: zx::photo::setFramingGrid), the Screenshot key on
// the pad, and the player's ask for one frame of play (takeFrameStep). Each
// game's override says when photo mode starts and ends (its own pause),
// puts the camera into its game and runs the frame asked for: Aggressive
// Inline's aggressive_inline_photo.cpp, Dave Mirra BMX 2's
// dave_mirra_photo.cpp, BMX XXX's bmx_xxx_photo.cpp.

#include "runtime/photo.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class PS2Machine;

namespace zge_host::photo
{
    // The port's own words in the game's language (UTF-8), or the English.
    using Translate = std::string (*)(const std::string &english);

    // One photo mode session at a time; the EE thread's (the game's frame
    // and VBlank).
    class FreeLook
    {
    public:
        // The game's install, once per boot: the runtime the filters go to,
        // and photo mode's state outside the game (the framing grid, the
        // game's free input) put back when the game is left, in photo mode
        // or not (a game switched or restarted from its pause).
        void setRuntime(PS2Machine *runtime);
        void setTranslate(Translate translate) { translate_ = translate; }

        // VBlank: the help's and the status line's clock.
        void tick() { ++vsync_; }
        uint64_t vsync() const { return vsync_; }

        // Photo mode starts at the game's camera (its eye, the point it
        // looks at, its field of view in degrees), the camera kept within
        // maxDistance of there: the game's free input on, the filter and the
        // help shown.
        void begin(const zx::photo::Vec3 &eye, const zx::photo::Vec3 &target, float fieldOfView, float maxDistance);
        // One game frame: the free camera moved and the keys done. False
        // when the player leaves (Back, Esc) or requestExit was called:
        // the caller then puts its game back and calls end().
        bool frame();
        // The game's input back, no filter, and the notice cleared (unless
        // the game left photo mode some other way: the level ended).
        void end(bool clearNotice = true);
        // Leave at the next frame (the photo mode key again).
        void requestExit() { exitRequest_ = true; }
        // One frame of play (Next Frame: the period key, L3; ZX_CONTROL's
        // `set photo.step 1`): asked for once, taken once by the game's
        // override when it runs that frame. Photo mode goes on, the game
        // paused again after it.
        void requestFrameStep() { stepRequest_ = true; }
        bool takeFrameStep()
        {
            if (!stepRequest_.exchange(false))
                return false;
            ++framesStepped_;
            return true;
        }
        // The frames taken since photo mode started.
        uint32_t framesStepped() const { return framesStepped_; }

        bool active() const { return active_.load(); }
        const zx::photo::FreeCamera &camera() const { return camera_; }
        // The camera's roll in degrees (+ clockwise as seen through it);
        // read by a game's matrix hook.
        float roll() const { return roll_.load(); }

    private:
        std::string word(const std::string &english) const;
        std::string speedText() const;
        void showNotice(bool force);
        void status(const std::string &line);
        void applyPreset();
        void showGrid();

        PS2Machine *runtime_ = nullptr;
        Translate translate_ = nullptr;
        std::atomic<bool> active_{false};
        bool exitRequest_ = false;
        std::atomic<bool> stepRequest_{false};
        uint32_t framesStepped_ = 0;
        bool grid_ = false;            // the framing grid, kept from one photo mode to the next as the filter is
        bool levelling_ = false;       // both roll buttons held: the horizon levelled once
        bool gridChord_ = false;       // both up / down buttons held: the grid switched once
        uint64_t vsync_ = 0;
        zx::photo::FreeCamera camera_;
        zx::photo::FreeCamera cameraStart_;
        std::atomic<float> roll_{0.0f};
        size_t preset_ = 0;
        // The full help, at the start for a while and when asked (H); else
        // one line saying how to get it.
        bool help_ = true;
        bool helpAsked_ = false;
        uint64_t startedAt_ = 0;
        std::string status_; // a line under the help for a moment
        uint64_t statusUntil_ = 0;
        uint64_t noticeAt_ = 0;
        std::chrono::steady_clock::time_point lastFrame_{};
    };
}
