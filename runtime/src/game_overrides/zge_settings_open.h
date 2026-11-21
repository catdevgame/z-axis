#pragma once

// Settings... (the app menu, Cmd+,; `settings` on ZX_CONTROL) by the
// engine's own path, shared by the games built on the Z-Axis engine
// (zge_settings_open.cpp). No pad press is made up for the player and no
// wall clock is waited on: the request is a flag, and the game pauses for
// it where its own Start pauses.
//
// The registry's open handler (zx::settings::setOpenHandler, the window
// thread) only decides, from the game's state in RAM, what the request
// means now:
//   not in a level      the notice "Settings: in the game's Options" (or
//                       the game's own way to the page from its front end,
//                       Hooks::elsewhere); nothing is raised
//   in a level, paused  the notice "Settings: in the pause menu"; the menu
//                       is up (its SETTINGS line is the way), or play has
//                       not started
//   in a level, playing the flag is raised
// The flag is consumed on the EE thread at the game's play frame (the frame
// hook each game has for its Restart key): when the game's own Start would
// pause there (Hooks::pausable) the hook does what that Start does
// (Hooks::pause), as if the player had pressed it, and the frame ends as
// the game's does; else the flag is dropped with the notice. The pause menu
// the game then builds is where the page goes: at its build hook the game
// calls pauseMenuShown, which opens the page in the menu's place
// (Hooks::openFromPause, the SETTINGS line's own path). Each flag is
// cleared where it is consumed, by the next pause menu, or with the session
// (zx::session).
//
// The gate's runs never ask: nothing here runs unless the host asked.

#include "ps2_runtime.h"

#include <cstdint>

namespace zge_host::settings_open
{
    struct Hooks
    {
        const char *game; // the log's tag ("dave-mirra")
        // From RAM, any thread: a level is loaded (play or its menus); play
        // runs in it (no pause menu or other screen up).
        bool (*inLevel)(const uint8_t *rdram);
        bool (*playing)(const uint8_t *rdram);
        // Not in a level (the window thread): the game's own way to the page
        // from its front end; null: the notice.
        void (*elsewhere)();
        // The play frame (the EE thread): whether the game's own Start would
        // pause now, and what it does then (the sound mode, the pause screen).
        bool (*pausable)(const uint8_t *rdram);
        void (*pause)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        // The pause menu is up (its widget tree's root as the game's
        // settings_menu::openFromPause takes it): the page in its place.
        void (*openFromPause)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t menuRoot);
    };

    // The registry's open handler for this game (cleared when the session
    // ends). Call once the game's Settings page is installed.
    void install(PS2Runtime &runtime, const Hooks &hooks);

    // The play frame (the EE thread): consumes a raised flag. True when it
    // paused the game for it: the caller ends the frame as the game's Start
    // does. False (nothing to do, or the notice given) otherwise.
    bool consumeAtPlayFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // The pause menu was built (the EE thread): with a pause of ours pending
    // the page opens in the menu's place.
    void pauseMenuShown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t menuRoot);
}
