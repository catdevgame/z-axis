#pragma once

// A dual-screen device's bottom screen while the collection's menu is up (a game's
// own panel, collection/panel.h, takes it once a game runs). The top screen
// keeps the chosen game's loop, riders and card; the bottom one is for the
// thumbs: the chosen game's name and roster, the games as big cards, Play,
// and the shared pages as buttons.
//
// Plain C++ like the panel: the layout and the hit test are pure, and the
// drawing goes into a Canvas (collection/menu.h) the painter copies to the
// surface. Pictures come from collection/front_art.h; a game not set up is
// drawn from its name and colour alone.

#include "collection/front_art.h"
#include "collection/menu.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zx::collection
{
    // The shared pages' buttons, left to right (Custom Freeplay only when
    // the menu has it).
    enum class BottomButton
    {
        CustomFreeplay,
        Settings,
        Mods,
        Quit,
    };

    struct BottomLayout
    {
        Rect name;               // the chosen game's name
        Rect line;               // under it: when it was last played, or how to set it up
        std::vector<Rect> heads; // its roster's portraits, left to right
        std::vector<Rect> cards; // one per game
        Rect play;
        std::vector<Rect> buttons;               // one per entry of `kinds`
        std::vector<BottomButton> kinds;
        Rect hint;
    };

    // For a surface of width x height (the Thor's 1240 x 1080), `games`
    // cards, up to `heads` portraits and the buttons (without Custom
    // Freeplay when `freeplay` is false). Everything stays inside the
    // surface; a card or button is at least 120 pixels each way, for a thumb.
    BottomLayout bottomLayout(int width, int height, size_t games, size_t heads, bool freeplay);

    struct BottomHit
    {
        enum Kind
        {
            None,
            Card,   // index: the game
            Play,
            Button, // button: which
        } kind = None;
        int index = -1;
        BottomButton button = BottomButton::Settings;
    };
    BottomHit bottomHit(const BottomLayout &layout, float x, float y);

    // What the bottom screen shows.
    struct BottomView
    {
        const std::vector<FrontGame> *games = nullptr; // the cards, in the collection's order (loadFrontGames)
        std::vector<std::string> names;                // each game's name
        std::vector<std::string> lines;                // each game's line under its name ("Last played ...")
        const std::vector<Image> *heads = nullptr;     // the chosen game's roster (null or empty: none)
        int chosen = 0;
        // The connected gamepad's buttons (the one last used, else the
        // first), for the bottom line (bottomHint); none: no gamepad.
        std::optional<HintDevice> pad;
        // A setup or an update running (the top screen's Games page shows
        // it): what it does ("Updating..."), in place of the cards, Play and
        // the buttons, so nothing looks there to be chosen. `busyLine`: its
        // state for a job with no part per game ("Exporting the saves 40%");
        // `busyGames`: the games with a part, each its line in `lines` and
        // how far it is in `busyDone` (0 to 1; below 0: no bar).
        std::string busy, busyLine, busyHint;
        std::vector<int> busyGames;
        std::vector<float> busyDone;
    };

    // The touch help under the buttons: one clause in sentence case with
    // DM2's full stop (menutext1's style sheet, rules 4 and 9), naming what
    // the big button says: "Tap a game, then Play." or "... then Set Up.".
    std::string bottomHelp(const FrontGame &game);

    // The bottom line, the same for every game and whichever was used last:
    // the touch help, and with a gamepad connected what its choose button
    // does on the cards too, as the top screen's line names it (buttonHint):
    // "Tap a game, then Play.   A Play".
    std::string bottomHint(const FrontGame &game, std::optional<HintDevice> pad);

    // The label of the big button: "Play", or "Set Up" for a game not set up.
    const char *bottomPlayLabel(const FrontGame &game);

    void drawBottom(Canvas &canvas, const BottomLayout &layout, const BottomView &view, const BitmapFont &font);

    // ---- between the menu and the bottom screen's painter -------------------

    // What the menu hands the painter: everything a BottomView points at,
    // held for as long as the painter draws it.
    struct BottomScene
    {
        std::shared_ptr<const std::vector<FrontGame>> games;
        std::vector<std::string> names, lines;
        std::shared_ptr<const std::vector<Image>> heads; // the chosen game's roster
        std::shared_ptr<const BitmapFont> font;
        int chosen = 0;
        bool freeplay = false; // the Custom Freeplay button
        std::optional<HintDevice> pad;
        std::string busy, busyLine, busyHint; // (BottomView's)
        std::vector<int> busyGames;
        std::vector<float> busyDone;
    };

    // The menu's bottom screen between the menu's thread (the app, which
    // shows a scene while the menu is up and takes the taps) and the
    // painter's (android_app.cpp, which draws it in place of a game's panel
    // and gives it the touches). One for the app.
    class BottomScreen
    {
    public:
        static BottomScreen &shared();

        // The menu is up with this scene; null: it is not (a game runs, its
        // panel has the screen).
        void show(std::shared_ptr<const BottomScene> scene);
        std::shared_ptr<const BottomScene> scene() const;

        // The painter: draws the scene shown into `canvas` (its size is the
        // surface's). False when no scene is shown.
        bool draw(Canvas &canvas);
        // The painter: a finger down and up at (x, y) on a surface of
        // width x height. A tap is down and up on the same target, kept
        // for the menu.
        void press(float x, float y, int width, int height);
        void release(float x, float y, int width, int height);

        // The menu: the taps since the last call, oldest first.
        std::vector<BottomHit> takeTaps();

    private:
        BottomHit hitAt(float x, float y, int width, int height) const;

        mutable std::mutex m_mutex;
        std::shared_ptr<const BottomScene> m_scene;
        BottomHit m_pressed;
        bool m_down = false;
        std::vector<BottomHit> m_taps;
    };
}
