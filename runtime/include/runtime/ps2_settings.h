#pragma once

// User settings as one model for every front end: the app's menu bar, the
// settings pages in the game's own menus (a game override's script natives)
// and any fallback UI all read and change settings through this registry,
// so they show the same state and share the apply and save logic.
//
// Whoever owns a setting (the app for settings.ini, the input layer for
// controls.ini, the audio output for the volume) adds it with its apply
// callback. Values are plain integers (an on/off, a choice index or a
// number in a range) and can be read and changed from any thread; apply
// callbacks run on the window thread, from runPending(), which the
// runtime calls once per host frame, in the order the changes were made.
//
// A change made on the game's own Settings page (its script functions, on
// the game's thread) goes through the FromGame calls: the game's thread
// waits there until the window thread has run it, so it reaches the game
// at the point of the game's frames where the input made it, whenever the
// window thread runs, and a run repeats exactly. A change from the app's
// menu bar is the window thread's own: its next runPending runs it.

#include <functional>
#include <string>
#include <vector>

namespace zx::settings
{
    enum class Kind
    {
        Toggle, // 0 = off, 1 = on
        Choice, // index into choices
        Range,  // minValue..maxValue in steps of step
        Action, // no value: activate() runs when chosen (rebind a key, reset)
    };

    struct Setting
    {
        std::string key;   // "display.vsync"; the group is the part before the dot
        std::string label; // "VSync"
        Kind kind = Kind::Toggle;
        std::vector<std::string> choices; // Choice: names of the values; Toggle: off, on (default "Off", "On")
        int minValue = 0;
        int maxValue = 1;
        int step = 1;
        std::string unit;      // Range: shown after the number ("%")
        bool restart = false;  // takes effect when the game is started again
        std::string help;      // one line for the page's footer

        // The current value (window thread, at registration and once per
        // frame, so changes made elsewhere, like the window's own full
        // screen button, show up). Optional.
        std::function<int()> current;
        // Applies and saves a new value (window thread).
        std::function<void(int value)> apply;
        // Text for a value instead of the choice name or number (for an
        // Action, what it shows, e.g. the keys bound). Optional. Any thread.
        std::function<std::string(int value)> format;
        // Action: runs on the window thread when the row is chosen.
        std::function<void()> activate;
    };

    // Adds or replaces a setting (by key). Window thread.
    void add(Setting setting);
    void remove(const std::string &key);
    bool has(const std::string &key);
    // Keys in registration order, those starting with prefix ("display.").
    std::vector<std::string> keys(const std::string &prefix = {});

    // Any thread.
    bool describe(const std::string &key, Setting &out); // a copy, without the callbacks' state
    int value(const std::string &key, int fallback = 0);
    // Clamped to the setting's range; the apply runs on the window thread.
    // False for an unknown key.
    bool set(const std::string &key, int value);
    // value + delta, wrapping for choices and toggles if wrap is set.
    bool step(const std::string &key, int delta, bool wrap);
    std::string text(const std::string &key);
    // Runs an Action's activate() on the window thread. False if unknown.
    bool activate(const std::string &key);
    std::string textFor(const Setting &setting, int value);

    // Opening the settings from outside the game's menus (the app menu's
    // Settings..., Cmd+,): whoever shows them (a game override) sets the
    // handler; requestOpen() runs it on the window thread. False if none.
    void setOpenHandler(std::function<void()> handler);
    bool requestOpen();
    // Whether a handler is set (the app menu's item is greyed out without).
    bool hasOpenHandler();

    // Runs a function on the window thread at the next runPending().
    void post(std::function<void()> work);

    // The game's own Settings page (the game's thread): set, step and
    // activate, and then wait until the window thread's runPending has run
    // the change (and every change made before it) and read the rows again.
    // With no host frame loop running (setFrameLoop) the change runs here,
    // at once.
    bool setFromGame(const std::string &key, int value);
    bool stepFromGame(const std::string &key, int delta, bool wrap);
    bool activateFromGame(const std::string &key);
    // The host frame loop (PS2Runtime::run) says it is calling runPending
    // once per host frame (true before the game's thread starts, false when
    // the loop ends, which lets a game thread still waiting go on).
    void setFrameLoop(bool running);

    // A short message for the player ("Screenshot saved"): the overlay
    // shows it for `seconds`. Any thread.
    void notify(const std::string &text, double seconds = 2.5);
    // The message to show now, or "". Any thread.
    std::string notice();

    // Window thread, once per host frame: runs posted work and pending
    // applies, then refreshes values from current().
    void runPending();

    // Tests.
    void clear();
}
