#pragma once

// A recording played back (collection/recordings.h): what the player can do
// while it plays, and what they are shown.
//
// The game's own input is the recording's, so the game's menus cannot be
// used to pause or leave. The window's input stays live and the app reads it
// on its thread: Start, Space or Escape pause; while paused Start, Cross,
// Space or Enter go on and Circle or Escape leave; the Switch Game key
// leaves at any time. The window shows the state on its overlay line
// (zx::settings::notify); a dual-screen device shows a strip at the foot
// of its bottom screen, over the game's panel, with Pause and Leave buttons
// for the thumbs.
//
// Plain C++ like the menu: the app gives it the input and draws what it
// lays out; the bottom screen's painter (android_app.cpp) draws the strip
// and hands it the touches.

#include "collection/menu.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zx::collection
{
    enum class PlaybackCommand
    {
        None,
        Pause,
        Resume,
        Leave,
    };

    // The command of one frame's keys (ps2_host_input codes) and gamepad
    // buttons (PadButton bits) that went down, as the playback is now.
    PlaybackCommand playbackCommand(const std::vector<int> &keysPressed, uint16_t padButtonsPressed, bool paused);

    // Where it is: "1:23 / 5:00" (collection/recordings.h lengthText).
    std::string playbackPosition(uint64_t vsync, uint64_t endVsync);
    // The overlay's line: "Playback 1:23 / 5:00   Start Pause", "Paused 1:23
    // / 5:00   Cross Resume   Circle Leave" (the buttons of `device`, as
    // the menu's bottom line names them; none on touch).
    std::string playbackLine(uint64_t vsync, uint64_t endVsync, bool paused, HintDevice device);

    // ---- the strip on a second screen -----------------------------------------

    struct StripLayout
    {
        Rect strip; // the band at the screen's foot
        Rect line;  // its text, at the left
        Rect pause; // Pause or Resume
        Rect leave;
    };
    // For a surface of width x height (the Thor's 1240 x 1080): a band the
    // buttons' height (at least 120 pixels, for a thumb) across the foot.
    StripLayout stripLayout(int width, int height);

    struct StripView
    {
        std::string position; // playbackPosition
        bool paused = false;
    };
    // The band: the position, Pause (or Resume) and Leave, into a canvas of
    // the band's size (its top at layout.strip's).
    void drawStrip(Canvas &band, const StripLayout &layout, const StripView &view, const BitmapFont &font);

    // The strip between the app (which shows it while a recording plays
    // back and takes its taps) and the bottom screen's painter (which draws
    // it over the game's panel and gives it the touches in the band). One
    // for the app.
    class PlaybackStrip
    {
    public:
        static PlaybackStrip &shared();

        // Shown with this view and font; nullopt: none (no playback).
        void show(std::optional<StripView> view, std::shared_ptr<const BitmapFont> font = nullptr);
        bool shown() const;

        // The painter: the band of a surface of width x height
        // (stripLayout's strip) drawn into `band`, a canvas of the band's
        // size, its top the band's. False when none is shown.
        bool draw(Canvas &band, int width, int height);
        // The painter: a finger down or up at (x, y) on a surface of
        // width x height. True when the band takes it (it is not the
        // panel's); a tap is down and up on the same button.
        bool press(float x, float y, int width, int height);
        bool release(float x, float y, int width, int height);

        // The app: the commands tapped since the last call, oldest first.
        std::vector<PlaybackCommand> takeTaps();

    private:
        PlaybackCommand hitAt(float x, float y, int width, int height) const;

        mutable std::mutex m_mutex;
        std::optional<StripView> m_view;
        std::shared_ptr<const BitmapFont> m_font;
        PlaybackCommand m_pressed = PlaybackCommand::None;
        bool m_down = false;
        std::vector<PlaybackCommand> m_taps;
    };
}
