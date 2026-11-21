#pragma once

// Where things lie on the bottom screen, in pixels at 1240x1080, and the
// maths that goes with the layout: which button a tap hits, where the follow
// view puts the skater, how the goal list groups its rows. No drawing and no
// game in here, so the tests can check it (tests/src/panel_tests.cpp).
//
// The live screen is "map first": the map fills the screen; over it lie the
// goal card (top left), the clock (top right), the where-to tile (bottom
// right), the legend (bottom left) and three buttons along the bottom edge
// into the references. A reference (the goal list, the items map, the trick
// table) keeps the split: the map 740 wide on the left, the side pane 500
// wide on the right.

#include "collection/panel_view.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace panel
{
    constexpr Rect kScreen{0, 0, 1240, 1080};
    constexpr float kEdge = 24.0f;            // the overlays' distance from the screen's edge
    constexpr float kTouchMinimum = 44.0f;    // no touch target smaller than this (a 6.5 inch panel 1240 px wide)

    // The live screen.
    constexpr float kBarHeight = 84.0f;
    constexpr Rect kLiveButtons[3] = {{0, kScreen.height - kBarHeight, 413, kBarHeight},
                                      {413, kScreen.height - kBarHeight, 414, kBarHeight},
                                      {827, kScreen.height - kBarHeight, 413, kBarHeight}};
    constexpr Rect kGoalCardFrame{kEdge, kEdge, 620, 0};                 // its height follows its text
    constexpr float kCardPadX = 28.0f, kCardPadY = 22.0f;
    constexpr float kCardRadius = 14.0f;
    constexpr Rect kClockTile{kScreen.width - kEdge - 230, kEdge, 230, 128};
    constexpr Rect kWhereTile{kScreen.width - kEdge - 330, kScreen.height - 120 - 154, 330, 154};
    constexpr Rect kLiveReturnButton{kWhereTile.x - 24 - 64, kWhereTile.y + (kWhereTile.height - 64) * 0.5f, 64, 64};
    constexpr Rect kLiveReturnTouch{kLiveReturnButton.x - 28, kLiveReturnButton.y - 28, 120, 120};
    constexpr Rect kLiveMapClear{4, 4, kScreen.width - 8, kScreen.height - kBarHeight - 8}; // the whole level on pause lies here

    // The split, for the references.
    constexpr Rect kSplitMapPane{0, 0, 740, 1080};
    constexpr Rect kSidePane{740, 0, 500, 1080};
    constexpr Rect kSplitMapClear{4, 4, 732, 1072}; // nothing lies over the map but the small return button and the legend
    constexpr Rect kSplitReturnButton{660, 1000, 64, 64};
    constexpr Rect kSplitReturnTouch{620, 960, 120, 120};

    // The follow view keeps the skater this far down the pane, so most of
    // the map is ahead.
    constexpr float kFollowShare = 2.0f / 3.0f;

    // The type, in pixels of line height (the design's sizes at 1240x1080).
    constexpr float kLabelPx = 26.0f;      // GOAL, PICK UP: caps, letter-spaced
    constexpr float kTitlePx = 52.0f;      // the goal's name on the card
    constexpr float kStatusPx = 40.0f;     // the game's own status line
    // A card's title and status take up to three lines each at their size; a longer one is made smaller, to no
    // less than this share of it, and then takes as many lines as it needs (the card grows): never cut.
    constexpr size_t kCardLines = 3;
    constexpr float kCardTextFloor = 0.7f;
    constexpr float kClockPx = 96.0f;
    constexpr float kDistancePx = 84.0f;
    constexpr float kButtonPx = 30.0f;     // GOALS 0/20, ITEMS, TRICKS: caps
    constexpr float kLineScorePx = 72.0f;  // the line's score under the card
    constexpr float kLineTrickPx = 36.0f;  // its trick names
    constexpr float kRowTitlePx = 38.0f;   // a goal row's name
    constexpr float kRowStatusPx = 30.0f;  // the open row's status line
    constexpr float kRowAsidePx = 26.0f;   // a row's distance or count, at the right
    constexpr float kRowTextPx = 28.0f;    // a tapped row's text
    constexpr float kHeaderPx = 30.0f;     // a group's word
    constexpr float kHeaderCountPx = 26.0f;
    constexpr float kHeaderNotePx = 20.0f; // "nearest first"
    constexpr float kLabelTracking = 0.12f; // of the label's size, between letters
    constexpr float kProgressBarHeight = 22.0f; // on the card
    constexpr float kRowBarHeight = 14.0f;      // on the open row

    // The scale that draws a font `pixels` tall: its line height times the
    // scale is the pixel size (the fonts differ by game, so a size is
    // asked for in pixels and the scale follows).
    inline float scaleFor(int lineHeight, float pixels) { return pixels / static_cast<float>(std::max(lineHeight, 1)); }

    // Which of the live screen's three buttons a tap hits: 0 goals, 1 items,
    // 2 tricks, -1 none.
    inline int liveButtonAt(Point at)
    {
        for (int i = 0; i < 3; ++i)
            if (kLiveButtons[i].contains(at))
                return i;
        return -1;
    }

    // The view centre that puts `skater` (map pixels) kFollowShare down the pane.
    inline Point followCenter(Point skater, float zoom, const Rect &pane)
    {
        return {skater.x, skater.y - pane.height * (kFollowShare - 0.5f) / std::max(zoom, 1e-3f)};
    }

    // The goal list's groups, in the order they are listed: the goal under
    // way, those that can be started (nearest giver first), the running
    // ones, the done ones, the locked ones (folded into their header until
    // it is tapped).
    enum class GoalGroup : uint8_t
    {
        Now,
        CanStart,
        Running,
        Done,
        Locked,
        Count,
    };
    struct GoalEntry
    {
        int goal;
        GoalGroup group;
        float distance; // to its giver (CanStart's order); anything for the rest
    };
    struct GoalRow
    {
        int goal; // -1: the group's header
        GoalGroup group;
    };
    // The rows top to bottom: each group that has a goal gets a header, then
    // its goals; Locked's goals only while `lockedOpen`.
    std::vector<GoalRow> groupRows(std::vector<GoalEntry> entries, bool lockedOpen);
    int groupCount(const std::vector<GoalEntry> &entries, GoalGroup group);
    const char *groupWord(GoalGroup group);

    constexpr float kGroupHeaderHeight = 64.0f;
    constexpr float kGoalRowHeight = 92.0f; // the least: a touch target and a half
    // A goal row's height from what it shows: its title's lines, the lines
    // under them (a status line, a tapped row's text) and a progress bar.
    float goalRowHeight(int titleLines, float titleGap, int moreLines, float moreGap, bool bar);

    // A mouse wheel (a desktop's Map Window, Panel::wheel): over a
    // reference's side pane it scrolls the pane's list, a notch by the
    // least goal row (up: towards the list's top); over the live screen's
    // buttons it does nothing (they take taps); anywhere else it zooms the
    // map about the pointer.
    enum class WheelTurns : uint8_t
    {
        Map,
        List,
        Nothing,
    };
    constexpr float kWheelScroll = kGoalRowHeight;
    inline WheelTurns wheelTurnsAt(Point at, bool reference)
    {
        if (reference)
            return kSidePane.contains(at) ? WheelTurns::List : WheelTurns::Map;
        return liveButtonAt(at) >= 0 ? WheelTurns::Nothing : WheelTurns::Map;
    }
    // A list's scroll after `notches` of the wheel (up positive), never
    // above its top (the draw keeps it above its bottom).
    inline float wheelScrolled(float scroll, float notches) { return std::max(0.0f, scroll - notches * kWheelScroll); }
}
