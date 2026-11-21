#pragma once

// The developer menu's host side for the games built on the Z-Axis engine
// that keep no debug menu of their own (Dave Mirra BMX 2, BMX XXX): their
// debug display, a byte the game's frame reads (the frame rate line, the
// profiler's display, the engine's debug drawing) and nothing in the retail
// game sets. With ZX_DEV_MENU=1 (the app's [general] developer_menu), F1
// or `set dev.debug_menu 1` (ZX_CONTROL) turns it on and off; off by
// default, when the byte is never written. Aggressive Inline has its
// hidden debug menu and tuning table (aggressive_inline_devtools.cpp).

#include <cstdint>
#include <filesystem>
#include <string>

class PS2Machine;

namespace zge_host::devtools
{
    struct DebugDisplay
    {
        const char *game;   // for the log ("dave-mirra")
        uint32_t flag;      // the byte the game's frame reads
        uint8_t on;         // its value for the display on (0: off)
        const char *help;   // the settings row's help: what the display shows
    };

    // The F1 key and the settings row (ZX_DEV_MENU=1 only).
    void installDebugDisplay(PS2Machine &runtime, const DebugDisplay &display);

    // A game's tuning table's Mods/tuning.ini (the games with one: Aggressive
    // Inline's, BMX XXX's): ZX_TUNING_INI, else tuning.ini in the last mod
    // folder that has one (ZX_MODS_DIR; a zip's too), else the first
    // folder's (where the reference is written). Empty: no mods.
    std::filesystem::path tuningFile();
    bool hasTuningFile(const std::filesystem::path &file);
    // Its text (a zip's entry too); false for none.
    bool readTuningText(const std::filesystem::path &file, std::string &text);
    // Where tuning-reference.ini goes: beside the tuning.ini in use, or in
    // the first mod folder when that one is in a zip. Empty: no mods.
    std::filesystem::path tuningReferenceFile();
}
