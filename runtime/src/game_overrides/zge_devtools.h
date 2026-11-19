#pragma once

// The developer menu's host side for the games built on the Z-Axis engine
// that keep no debug menu of their own (Dave Mirra BMX 2, BMX XXX): their
// debug display, a byte the game's frame reads (the frame rate line, the
// profiler's display, the engine's debug drawing) and nothing in the retail
// game sets. With PS2X_DEV_MENU=1 (the app's [general] developer_menu), F1
// or `set dev.debug_menu 1` (PS2X_CONTROL) turns it on and off; off by
// default, when the byte is never written. Aggressive Inline has its
// hidden debug menu and tuning table (aggressive_inline_devtools.cpp).

#include <cstdint>

class PS2Runtime;

namespace zge_host::devtools
{
    struct DebugDisplay
    {
        const char *game;   // for the log ("dave-mirra")
        uint32_t flag;      // the byte the game's frame reads
        uint8_t on;         // its value for the display on (0: off)
        const char *help;   // the settings row's help: what the display shows
    };

    // The F1 key and the settings row (PS2X_DEV_MENU=1 only).
    void installDebugDisplay(PS2Runtime &runtime, const DebugDisplay &display);
}
