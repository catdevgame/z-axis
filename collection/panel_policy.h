#pragma once

// Which screen the panel shows, and why. Three classes (the design's own
// words):
//
//   Default     the map and the live column; nobody asked for anything.
//   Second HUD  the live column's cards: read in a glance, large type.
//   Reference   looked up on purpose: the goal list, the items map, the trick table.
//
// The game's state moves the reference in and out; touch can open one and
// hold it. A reference never shares the screen with the live line.

#include "collection/panel.h"

namespace panel
{
    enum class Screen : uint8_t
    {
        Blank,  // no level: the panel has nothing to say
        Live,   // Default: map + the second HUD
        Goals,  // Reference
        Items,  // Reference: the level's gaps, collectables and other categories on the map
        Tricks, // Reference
    };

    class ScreenPolicy
    {
    public:
        // Once a draw: the game's state moves the screen.
        void update(GameMode mode, bool lineActive)
        {
            if (mode == GameMode::NoLevel)
            {
                screen_ = Screen::Blank;
                pinned_ = false;
            }
            else
            {
                if (screen_ == Screen::Blank)
                    screen_ = Screen::Live;
                if (mode == GameMode::Paused && lastMode_ == GameMode::Play && screen_ == Screen::Live)
                    screen_ = lastReference_;   // the eyes are free: what a pause is for
                else if (mode == GameMode::Play && lastMode_ == GameMode::Paused && !pinned_)
                    screen_ = Screen::Live;     // back to skating
                else if (mode == GameMode::Play && lineActive && !lastLine_ && !pinned_)
                    screen_ = Screen::Live;     // a line began: the reference gives way
            }
            lastMode_ = mode;
            lastLine_ = lineActive;
        }

        // A touch: open a reference, hold or let go of it, leave it.
        void open(Screen reference)
        {
            screen_ = reference;
            lastReference_ = reference;
        }
        void close()
        {
            screen_ = Screen::Live;
            pinned_ = false;
        }
        void togglePin() { pinned_ = !pinned_; }

        Screen screen() const { return screen_; }
        bool pinned() const { return pinned_; }
        bool isReference() const { return screen_ == Screen::Goals || screen_ == Screen::Items || screen_ == Screen::Tricks; }

    private:
        Screen screen_ = Screen::Blank;
        Screen lastReference_ = Screen::Goals;
        GameMode lastMode_ = GameMode::NoLevel;
        bool lastLine_ = false;
        bool pinned_ = false;
    };
}
