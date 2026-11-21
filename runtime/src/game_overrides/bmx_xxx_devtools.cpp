// BMX XXX (SLUS-20415): the developer menu, Aggressive Inline's
// (aggressive_inline_devtools.cpp) as far as BMX XXX has one, as DM2's
// (dave_mirra_devtools.cpp).
//
// BMX XXX has no debug menu of its own and no tuning table (its GameCube
// release's, on the PS2's values: bmx_xxx_tuning.cpp). What it keeps
// for its developers is a debug display: the game's frame (playFrame,
// game/bmx/states/game_state.cpp) hands g_debugDisplay (a byte) to the
// profiler's display (setProfileDisplayMode) every frame, and at 2 draws
// the profiler's backdrop (drawProfilerBackdrop) and turns on the engine's
// debug drawing (setDebugMenuSelection: the debug markers, lines and text the
// code adds). Nothing in the retail game sets it.
//
// With ZX_DEV_MENU=1 (the app's [general] developer_menu), F1 or
// `set dev.debug_menu 1` (ZX_CONTROL) turns it between 0 and 2. Off by
// default: the byte is never written (zge_devtools.cpp, DM2's host side).

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_devtools.cpp.h"
#include "zge_devtools.h"

namespace bmx_xxx
{
    namespace
    {
        constexpr uint8_t kDebugDisplayProfiler = 2; // game/bmx/states/game_state.cpp
    }

    void installDevTools(PS2Runtime &runtime)
    {
        zge_host::devtools::installDebugDisplay(
            runtime, {"bmx-xxx", sym::bmx::g_debugDisplay, kDebugDisplayProfiler,
                      "The game's profiler, its backdrop and debug drawing."});
        installTuning(runtime);
    }
}
