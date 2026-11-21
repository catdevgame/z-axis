#pragma once

// A dual-screen handheld's bottom screen in single player: a live map of the level with
// the goals and tricks beside it. Everything here is plain C++ with no
// platform or game dependency, so the Mac harness (zx-panelshot) and the
// tests run the same code the phone does.
//
// Who calls what (the Android side owns the threads):
//   game thread, once a game frame   captureSnapshot(guest RAM, snapshot)
//   painter thread, about 20 Hz      Panel::touch(event) for each new touch,
//                                    Panel::draw(latest snapshot, now, surface)
// The snapshot is a plain struct: hand it over with a mutex or a seqlock. The
// panel never sees guest RAM, and nothing here writes it: touch changes only
// the panel's own view.

#include "runtime/panel_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

namespace panel
{
    constexpr int kScreenWidth = 1240;
    constexpr int kScreenHeight = 1080;

    // An RGBA8888 target (bytes R, G, B, A; an ANativeWindow_Buffer with
    // WINDOW_FORMAT_RGBA_8888 fits). stride in pixels.
    struct Surface
    {
        uint32_t *pixels = nullptr;
        int width = 0;
        int height = 0;
        int stride = 0;
    };

    enum class TouchPhase : uint8_t
    {
        Down,
        Move,
        Up,
        Cancel, // the gesture is off (the display went away)
    };

    // One pointer's change, in the surface's pixels.
    struct TouchEvent
    {
        TouchPhase phase = TouchPhase::Down;
        int32_t pointer = 0; // stable from Down to Up
        float x = 0.0f;
        float y = 0.0f;
        double seconds = 0.0; // any steady clock
    };

    // The panel: the view, the loaded level file and the drawing. Use from
    // one thread at a time (the painter's).
    class Panel
    {
    public:
        Panel();
        ~Panel();
        Panel(const Panel &) = delete;
        Panel &operator=(const Panel &) = delete;

        // The folder of the packaged panel files (<FOLDER>.pmap, UI.pmap),
        // and the one each file is read from when the folder has none of it
        // or has it in another version ("" none). Level files load on the
        // draw that first needs them.
        void setDataFolder(const std::string &folder, const std::string &fallback = {});

        void touch(const TouchEvent &event);
        // A mouse wheel's turn at (x, y) in the surface's pixels, `notches`
        // up positive (a desktop's Map Window): over a reference's side pane
        // the pane's list scrolls, over the live screen's buttons nothing
        // happens, elsewhere the map zooms about the pointer as a pinch
        // would, closer for up (panel_layout.h wheelTurnsAt).
        void wheel(float x, float y, float notches, double seconds);
        void draw(const Snapshot &snapshot, double seconds, const Surface &surface);
        // The game's HUD pieces the last draw's screen shows whenever the
        // game has them (Clean Top Screen, runtime/panel_snapshot.h): on the
        // live screen its clock and its line, and its goal card when the
        // level file's goals are the game's; none on a blank screen or a
        // reference (the goal list, the items, the tricks).
        HudPieces shows() const;

        // Each goal's title and progress line as the goal list shows them
        // (after a draw of this snapshot): for checks against the game's own
        // counts (zx-panelshot --goal-lines). Empty when the level file's
        // goals are not the loaded script's.
        struct GoalLine
        {
            std::string title;
            std::string progress;
            int goalSet; // LevelFile::goalSet in use
            int barHave = -1, barTarget = 0; // its progress bar's count and target, -1: no bar
        };
        std::vector<GoalLine> goalLines(const Snapshot &snapshot) const;

        // Each goal text the draws laid out while recording (zx-panelshot
        // --text-fit: every goal of every level on the live card, the goal
        // list and the items list), and how: a text is never cut, it wraps,
        // and the card's and the items' rows make it smaller down to a floor
        // first.
        struct TextFit
        {
            std::string where; // "card title", "card status", "list title", "list status", "list text", "items title"
            int goal = -1;
            std::string text;
            int lines = 0;      // the lines it takes at the scale drawn
            int drawn = 0;      // the lines drawn
            float scale = 0.0f; // the font's scale drawn
            float widest = 0.0f, width = 0.0f; // its widest line, and the room's
            bool whole() const { return drawn >= lines && widest <= width + 0.5f; }
        };
        void recordTextFits(bool on);
        std::vector<TextFit> takeTextFits();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
