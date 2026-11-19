#pragma once

// Photo mode's host side, shared by the games built on the Z-Axis engine: a
// free camera moved by the keyboard, the mouse and the pad while the game
// reads an idle pad (PSPadBackend::setFreeInput), the help and status lines
// on the overlay's notice, the picture filters (PS2Runtime::setPictureFilter,
// so the Screenshot key's pictures get them too) and the Screenshot key on
// the pad. Each game's override says when photo mode starts and ends (its
// own pause) and puts the camera into its game: Aggressive Inline's
// aggressive_inline_photo.cpp, Dave Mirra BMX 2's dave_mirra_photo.cpp.

#include "runtime/ps2_photo.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class PS2Runtime;

namespace zge_host::photo
{
    // The port's own words in the game's language (UTF-8), or the English.
    using Translate = std::string (*)(const std::string &english);

    // One photo mode session at a time; the EE thread's (the game's frame
    // and VBlank).
    class FreeLook
    {
    public:
        void setRuntime(PS2Runtime *runtime) { runtime_ = runtime; }
        void setTranslate(Translate translate) { translate_ = translate; }

        // VBlank: the help's and the status line's clock.
        void tick() { ++vsync_; }
        uint64_t vsync() const { return vsync_; }

        // Photo mode starts at the game's camera (its eye, the point it
        // looks at, its field of view in degrees), the camera kept within
        // maxDistance of there: the game's free input on, the filter and the
        // help shown.
        void begin(const ps2x::photo::Vec3 &eye, const ps2x::photo::Vec3 &target, float fieldOfView, float maxDistance);
        // One game frame: the free camera moved and the keys done. False
        // when the player leaves (Back, Esc) or requestExit was called:
        // the caller then puts its game back and calls end().
        bool frame();
        // The game's input back, no filter, and the notice cleared (unless
        // the game left photo mode some other way: the level ended).
        void end(bool clearNotice = true);
        // Leave at the next frame (the photo mode key again).
        void requestExit() { exitRequest_ = true; }

        bool active() const { return active_.load(); }
        const ps2x::photo::FreeCamera &camera() const { return camera_; }
        // The camera's roll in degrees (+ clockwise as seen through it);
        // read by a game's matrix hook.
        float roll() const { return roll_.load(); }

    private:
        std::string word(const std::string &english) const;
        std::string speedText() const;
        void showNotice(bool force);
        void status(const std::string &line);
        void applyPreset();

        PS2Runtime *runtime_ = nullptr;
        Translate translate_ = nullptr;
        std::atomic<bool> active_{false};
        bool exitRequest_ = false;
        uint64_t vsync_ = 0;
        ps2x::photo::FreeCamera camera_;
        ps2x::photo::FreeCamera cameraStart_;
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
