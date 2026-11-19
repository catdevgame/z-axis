// Settings... (the app menu, Cmd+,) by the engine's own path: see
// zge_settings_open.h.
#include "zge_settings_open.h"
#include "zge_settings.h"

#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <iostream>

namespace zge_host::settings_open
{
    namespace
    {
        constexpr const char *kNoticeOptions = "Settings are in Options";
        constexpr const char *kNoticePauseMenu = "Settings are in the pause menu";

        // One game boot's request (ps2x::session: gone with the boot).
        struct State
        {
            Hooks hooks{};
            PS2Runtime *runtime = nullptr;
            std::atomic<bool> requested{false};   // raised by the handler (window thread), consumed at the play frame (EE)
            std::atomic<bool> openAtPause{false}; // the play frame paused for it: the next pause menu opens the page
        };
        State &state() { return ps2x::session::state<State>(); }

        void log(const char *what)
        {
            std::cout << "[" << state().hooks.game << "] settings: " << what << std::endl;
        }

        // The app asked (the window thread).
        void requested()
        {
            State &st = state();
            const uint8_t *rdram = st.runtime ? st.runtime->memory().getRDRAM() : nullptr;
            if (!rdram)
                return;
            if (!st.hooks.inLevel(rdram))
            {
                if (st.hooks.elsewhere)
                {
                    log("asked outside a level: the front end's own way");
                    st.hooks.elsewhere();
                }
                else
                {
                    log("asked outside a level: the game's Options have the page");
                    ps2x::settings::notify(kNoticeOptions);
                }
                return;
            }
            if (!st.hooks.playing(rdram))
            {
                log("asked with play stopped: the pause menu has the page");
                ps2x::settings::notify(kNoticePauseMenu);
                return;
            }
            st.requested = true;
            log("asked in play: the next play frame pauses as Start does");
        }
    }

    void install(PS2Runtime &runtime, const Hooks &hooks)
    {
        State &st = state();
        st.hooks = hooks;
        st.runtime = &runtime;
        ps2x::settings::setOpenHandler(&requested);
        ps2x::session::onEnd([] { ps2x::settings::setOpenHandler({}); });
        if (zge_host::settings::tracing())
            log("Settings... opens the page by the game's own Start and pause menu");
    }

    bool consumeAtPlayFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        if (!st.requested.exchange(false))
            return false;
        if (!st.hooks.pausable(rdram))
        {
            // The menu is up, or play has not started: Start would do
            // nothing here either.
            log("play not pausable at this frame: dropped with the notice");
            ps2x::settings::notify(kNoticePauseMenu);
            return false;
        }
        log("pausing as the game's Start does; the pause menu opens the page");
        st.openAtPause = true;
        st.hooks.pause(rdram, ctx, runtime);
        return true;
    }

    void pauseMenuShown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t menuRoot)
    {
        State &st = state();
        if (!st.openAtPause.exchange(false))
            return;
        if (!menuRoot)
        {
            log("the pause menu is up but its tree's root is unknown: the page stays in its SETTINGS line");
            return;
        }
        log("the pause menu is up: the Settings page in its place");
        st.hooks.openFromPause(rdram, ctx, runtime, menuRoot);
    }
}
