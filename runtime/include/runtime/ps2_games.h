#pragma once

// The games the runtime runs: what each one is, below every layer that
// names them. The runtime knows its game by these (a recording's header
// from the boot ELF, the settings rows' notes on what changes the game's
// logic); the collection names them (collection/settings.h's
// GameSettingsProfile adds each game's options to its row here) and the
// games' own code is in their folders.

#include <string_view>
#include <vector>

namespace ps2x::games
{
    struct Game
    {
        const char *id;    // the collection's game id: "ai", "dm2", "bmx"
        const char *elf;   // its boot ELF (the single-game app and the developer runner find the game by it)
        const char *title; // "Dave Mirra Freestyle BMX 2"
        // The game's logic follows what is on screen: Aggressive Inline's
        // effects (particles, explosions) draw from the game's one random
        // number generator as they come into view, so a wider or turned view
        // (widescreen, the camera's field of view, mouse-look) changes the
        // random sequence; the game is otherwise the same. Those settings'
        // rows say so.
        bool viewChangesRandom = false;
        // Draw Distance changes the game: its cameras' far clips are the
        // game's own state (and BMX XXX's models' levels of detail with
        // them), so beyond Original the game runs otherwise than the
        // original. Such a game's draw distance is Original by default, is
        // given at boot (PS2X_DRAW_DISTANCE in its BootConfig) and its row
        // applies at the next start, and says so.
        bool drawDistanceChangesGame = false;
    };

    // Every game, in the collection's order (Aggressive Inline first).
    const std::vector<Game> &all();
    // By id (any case), else by boot ELF (any case); Aggressive Inline's for
    // neither (the baseline the runtime was first made for).
    const Game &game(std::string_view idOrElf);
}
