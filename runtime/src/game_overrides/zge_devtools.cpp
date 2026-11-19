// The developer menu's debug display, shared by DM2 and BMX XXX
// (zge_devtools.h).

#include "zge_devtools.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <iostream>

namespace zge_host::devtools
{
    namespace
    {
        constexpr int kKeyF1 = 290;

        struct State
        {
            DebugDisplay display{};
            std::atomic<bool> request{false};
            bool f1Latched = false;
        };
        State &state() { return ps2x::session::state<State>(); }

        void onVsync(uint8_t *rdram, uint64_t)
        {
            State &d = state();
            const bool f1 = PSPadBackend::keyPressedWithin(kKeyF1, 0.25);
            if (f1 && !d.f1Latched)
                d.request = true;
            d.f1Latched = f1;
            if (!d.request.exchange(false))
                return;
            uint8_t *shown = getMemPtr(rdram, d.display.flag);
            if (!shown)
                return;
            *shown = *shown != 0u ? 0u : d.display.on;
            std::cout << "[" << d.display.game << "] debug display " << (*shown ? "on" : "off") << std::endl;
        }
    }

    void installDebugDisplay(PS2Runtime &runtime, const DebugDisplay &display)
    {
        if (const char *on = ps2x::boot::text("PS2X_DEV_MENU"); !on || on[0] != '1')
            return;
        state().display = display;
        ps2x::settings::Setting open;
        open.key = "dev.debug_menu";
        open.label = "Debug Display";
        open.help = display.help;
        open.current = []() { return 0; };
        open.apply = [](int value)
        {
            if (value != 0)
                state().request = true;
        };
        ps2x::session::addSetting(open);
        runtime.addVSyncHook(&onVsync);
        std::cout << "[" << display.game << "] developer menu on: F1 turns the game's debug display on and off" << std::endl;
    }
}
