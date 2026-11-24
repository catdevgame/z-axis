#pragma once

// The bottom screen over the game on one screen: what a dual-screen
// handheld's second display shows (collection/panel.h: the level's map, the
// goals and the tricks), drawn into a corner of the game's own window on a
// computer or a single-screen Android device, shown and hidden by the
// bottom_screen shortcut (controls.ini [hotkeys]: M, or L3+R2 on a
// gamepad). Where a second display shows the bottom screen already (the
// Thor's), there is no overlay.
//
// Like the second display's painter (platform/android/android_app.cpp) it
// is off the game's frames: while shown, the game's thread takes a snapshot
// at each vsync (read-only: the game's logic cannot change), a thread of its
// own paints the panel from the latest about 20 times a second into a
// 1240x1080 picture, and the window's thread puts the newest picture over
// the game (the host's overlay picture, ps2_host::setOverlayPicture) and
// hands the pointer over it to the panel as touches. Hidden, nothing is
// captured, painted or drawn, and the pointer is the game's.
//
// It does not pause the game: the panel follows the game's own state as the
// second display's does (panel_policy.h: a pause shows a reference, play
// the live screen).

#include "collection/panel.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace panel
{
    class Overlay
    {
    public:
        static Overlay &shared();
        ~Overlay();

        // A second display shows the bottom screen now (the Thor's, while
        // its panel has a surface): the overlay is hidden and the shortcut
        // does nothing. Any thread.
        void setSecondScreen(bool up);
        bool secondScreen() const;

        // Shown or hidden (the shortcut; the window thread). Showing starts
        // the painter, hiding stops it and lets go of a touch in progress.
        void setShown(bool shown);
        bool shown() const;

        // The game's thread at each vsync: a snapshot while shown.
        void capture(const uint8_t *guestRam, size_t ramBytes);

        // The pointer over the overlay, in the bottom screen's pixels
        // (kScreenWidth x kScreenHeight): a touch's phase, or a wheel's
        // notches (up positive). The window thread; the painter applies them.
        void touch(TouchPhase phase, int pointer, float x, float y);
        void wheel(float x, float y, float notches);

        // The window thread: when a picture was painted since the last
        // call, `use` gets it (kScreenWidth x kScreenHeight RGBA, rows of
        // kScreenWidth pixels) while the painter waits, whether a level is
        // in it (none: the front end or a loading screen, a blank picture
        // not worth covering the game with) and the game's HUD pieces it
        // shows (Panel::shows, for Clean Top Screen). False: nothing new.
        bool takePicture(const std::function<void(const uint32_t *pixels, bool level, HudPieces shows)> &use);

        // What painting cost since the last call (the painter's thread).
        struct Cost
        {
            int paints = 0;
            double totalMs = 0.0;
            double worstMs = 0.0;
        };
        Cost takeCost();

    private:
        Overlay() = default;
        void paintLoop();
        void stopPainter();

        struct Input
        {
            bool isWheel = false;
            TouchEvent touch;
            float notches = 0.0f;
        };

        std::atomic<bool> secondScreen_{false};
        std::atomic<bool> shown_{false};

        // The game thread's latest snapshot (taken into captured_, the game
        // thread's own, then copied over under the mutex).
        std::unique_ptr<Snapshot> captured_;
        std::mutex snapshotMutex_;
        Snapshot snapshot_;
        std::atomic<double> capturedAt_{0.0};

        std::mutex inputMutex_;
        std::vector<Input> inputs_;
        std::vector<int> held_; // the pointers down and not up (the window thread's)

        // The painter: one Panel, used on the painter's thread only (one at a
        // time: a new one starts after the last has stopped), kept across
        // shows so the map keeps its view.
        std::unique_ptr<Panel> panel_;
        std::unique_ptr<Snapshot> painted_; // the snapshot the painter draws
        std::string dataFolder_, dataFallback_;
        std::thread painter_;
        std::mutex stopMutex_;
        std::condition_variable stopSignal_;
        bool stopping_ = false;

        // The pictures: the painter paints the back one and swaps it in.
        std::mutex pictureMutex_;
        std::vector<uint32_t> front_, back_;
        bool fresh_ = false;
        bool frontLevel_ = false;
        HudPieces frontShows_ = 0;
        Cost cost_;
    };
}
