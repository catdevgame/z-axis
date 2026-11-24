#pragma once

// Practice's host side, shared by the games built on the Z-Axis engine: a
// restart point and a timer, off by default. With Practice on (the app's row
// game.practice, collection/practice.h; without the app ZX_PRACTICE=1)
// the practice_point key ([hotkeys] in controls.ini) makes a restart point
// where the player is, and the Restart key then puts the player back there
// instead of restarting. Each try is timed from the point in the game's own
// seconds of play (a paused game's are not counted), shown as a notice while
// it runs, and its time and the best are shown and logged when the next one
// starts. With no point, or with Practice off, the keys do what they did.
//
// Each game's override says what its point holds and puts the player back
// with the game's own code, at its play frame (in place of one): Aggressive
// Inline's aggressive_inline_practice.cpp (the skater moving as at the
// point); Dave Mirra BMX 2's and BMX XXX's through zge_practice_rider.h (the
// rider stopped there: what their respawn puts back).

#include <cstdint>
#include <string>
#include <vector>

namespace zge_host::practice
{
    // Practice on: the app's row when it has one, else ZX_PRACTICE=1. Any
    // thread.
    bool enabled();

    // What the player asked for, run at the game's next play frame.
    enum class Request
    {
        None,
        SetPoint, // a restart point where the player is now
        Return,   // back to the point
    };

    // One game boot's practice (the session's): the EE thread's (the game's
    // VBlank and play frame).
    class Practice
    {
    public:
        // VBlank, while the game is in play: the keys pressed since the last
        // one. True when the Restart key was taken (Practice on and a point
        // to go back to), so the caller does not ask for its own restart.
        // Practice turned off drops the point.
        bool onVsync(bool pointPressed, bool restartPressed);

        // The play frame: what to do in place of it, once (None: play on).
        Request take();
        bool hasPoint() const { return hasPoint_; }

        // What the game did with a request: the point made, or not (why: a
        // short line for the player), the player put back at it, or not.
        void pointSet();
        void pointRefused(const std::string &why);
        void returned();
        void returnFailed(const std::string &why);

        // A frame of play while a point is set: its seconds of game time.
        void played(float seconds);

        // Out of the level (or the run started again some other way): the
        // point and its tries dropped.
        void leave();

        // The tries since the point was made, ended by a return (seconds).
        const std::vector<double> &tries() const { return tries_; }

    private:
        void show(const std::string &text, double seconds);

        Request pending_ = Request::None;
        bool hasPoint_ = false;
        double elapsed_ = 0.0;    // the try running: game seconds since the point
        std::vector<double> tries_;
        uint32_t holdFrames_ = 0; // a line of ours that the running clock leaves up
        uint32_t frames_ = 0;
        std::string shown_;       // our last notice (another's is left alone)
    };

    // The session's.
    Practice &current();

    // "12.48", "1:02.48".
    std::string timeText(double seconds);
}
