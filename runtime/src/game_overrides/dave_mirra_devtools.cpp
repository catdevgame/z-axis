// Dave Mirra Freestyle BMX 2 (SLUS-20159): the developer menu, Aggressive
// Inline's (aggressive_inline_devtools.cpp) as far as DM2 has one.
//
// DM2 has no debug menu of its own and no tuning table (its rates are
// separate variables). What it keeps for its developers is a debug display:
// while g_debugStatsShown (a byte) is set, the game's frame (playFrame,
// game/dm2/action/game_state.cpp) prints its frame rate and the texture
// memory free ("FPS: %2.2f VramFree:%d"), and turns on the profiler's
// display and the engine's debug drawing (setProfileDisplayMode,
// setDebugMenuSelection: the debug markers, lines and text the code adds).
// Nothing in the retail game sets it.
//
// With PS2X_DEV_MENU=1 (the app's [general] developer_menu), F1 or
// `set dev.debug_menu 1` (PS2X_CONTROL) turns it on and off. Off by default:
// the byte is never written. The key and the row are the host side DM2
// shares with BMX XXX (zge_devtools.cpp).

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_devtools.cpp.h"
#include "zge_devtools.h"

namespace dave_mirra
{
    void installDevTools(PS2Runtime &runtime)
    {
        zge_host::devtools::installDebugDisplay(
            runtime, {"dave-mirra", sym::dm2::g_debugStatsShown, 1u, "The game's frame rate, profiler and debug drawing."});
    }
}
