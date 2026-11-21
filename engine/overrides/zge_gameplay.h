#pragma once

// The gameplay pages' host side, shared by the games built on the Z-Axis
// engine: their Camera, Assists and Cheats options kept in gameplay.ini
// (ZX_GAMEPLAY_INI; the app: the game's support folder), the settings rows
// made from them, the shortcut keys pressed since the last VBlank, and the
// game values an option changes (written while the option wants another
// value, put back once when it no longer does). Each game's override says
// what its options are and what they change: Aggressive Inline's
// aggressive_inline_gameplay.cpp, Dave Mirra BMX 2's dave_mirra_gameplay.cpp.
// And Clean Top Screen's part of the games' widget-draw hooks (below).

#include "runtime/panel_snapshot.h"
#include "runtime/host_input.h"
#include "runtime/settings.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zge_host::gameplay
{
    // An option: its settings key ("camera.fov": gameplay.ini's [camera]
    // fov), its value when the file has none, and the key an earlier
    // version wrote it under (read as the key; written back as the key).
    struct OptionInfo
    {
        const char *key;
        int defaultValue;
        const char *formerKey = nullptr;
    };

    // One game's options and their file. Values are read and set from any
    // thread.
    class Options
    {
    public:
        // The options (by index, in the file's order) and the file's first
        // lines (each starting with "; ").
        void define(const OptionInfo *options, int count, std::string header);
        // The file, from ZX_GAMEPLAY_INI (none: nothing is saved).
        void setFile(std::filesystem::path file) { file_ = std::move(file); }
        const std::filesystem::path &file() const { return file_; }

        // Every option at its default, then the file's values.
        void load();
        // Every option written to the file.
        void save();

        int count() const { return static_cast<int>(info_.size()); }
        const OptionInfo &info(int option) const { return info_[static_cast<size_t>(option)]; }
        int value(int option) const { return values_[static_cast<size_t>(option)].load(std::memory_order_relaxed); }
        void set(int option, int value) { values_[static_cast<size_t>(option)] = value; }
        // Each option from the settings model (its row clamps it to the
        // page's range); after the rows are added.
        void takeRowValues();

        // A settings row for an option: its apply sets the value and saves
        // the file.
        zx::settings::Setting row(int option, const char *label, const char *help);
        zx::settings::Setting rangeRow(int option, const char *label, int lo, int hi, int step, const char *unit,
                                         const char *help);
        zx::settings::Setting choiceRow(int option, const char *label, std::vector<std::string> choices,
                                          const char *help);

    private:
        std::vector<OptionInfo> info_;
        std::unique_ptr<std::atomic<int>[]> values_;
        std::string header_;
        std::filesystem::path file_;
        std::mutex fileMutex_;
    };

    // The shortcut keys (controls.ini [hotkeys], every game's: Restart, Hide
    // HUD, Photo Mode): which were pressed since the last poll.
    class Hotkeys
    {
    public:
        // VBlank (EE thread), once.
        void poll();
        bool pressed(ps2_host_input::Hotkey key) const { return pressed_[key]; }

    private:
        uint32_t seen_[ps2_host_input::kHotkeyCount] = {};
        bool pressed_[ps2_host_input::kHotkeyCount] = {};
    };

    // A float of the game's an option changes: its address and the game's
    // own value (taken before any option applies).
    struct GameFloat
    {
        uint32_t address = 0;
        float game = 0.0f;
        bool written = false; // holds a value of the options'
    };
    // The game's value taken (once: a later call keeps the first).
    void takeGameValue(const uint8_t *rdram, GameFloat &value, uint32_t address);
    // Writes want while it is not the game's own; puts the game's back once
    // when it is again. Values at the game's own are never written.
    void apply(uint8_t *rdram, GameFloat &value, float want);

    // Bit-identical floats (a written value is compared as the game reads it).
    bool sameFloat(float a, float b);
    float readFloat(const uint8_t *rdram, uint32_t address);
    void writeFloat(uint8_t *rdram, uint32_t address, float value);

    // ---- Clean Top Screen (runtime/panel_snapshot.h) ----------------------------

    // A HUD widget of the game's that is a piece a bottom screen shows: the
    // widget, by its name, its group's and its UI screen's (the zgui file it
    // is in), as the game's UI scripts name them.
    struct TopScreenPiece
    {
        const char *screen; // "InGameUI"
        const char *group;  // "OverlayToggle"
        const char *widget; // "TrickList"
        panel::HudPiece piece;
    };
    // The game's widget tree as its hook reads it: whether a node (never 0)
    // is named `name`, and the node it hangs from (0: none).
    using NodeNamed = bool (*)(const uint8_t *rdram, uint32_t node, const char *name);
    using NodeParent = uint32_t (*)(const uint8_t *rdram, uint32_t node);
    // A game's widget-draw hook, for each widget drawn: whether Clean Top
    // Screen leaves this draw out. The widget is one of `pieces` and
    // panel::hiddenOnTop says so, as the game's thread reads it: recorded
    // and replayed (play_recording's TopScreenHud), as what it leaves out
    // is in the game's RAM (the widget's place, its draw's messages). The
    // first draw left out of each piece logs a line under `tag`.
    bool leftOffTopScreen(const uint8_t *rdram, uint32_t widget, std::span<const TopScreenPiece> pieces, NodeNamed named,
                          NodeParent parent, const char *tag);
    // The draws left out since the last ask (each game logs it as a level is left).
    uint32_t takeLeftOffTopScreen();
}
