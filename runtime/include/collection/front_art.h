#pragma once

// What the collection's front end shows of each game, read from the game's
// own disc folder (<support>/games/<id>/disc, setup1's install) and, for
// BMX XXX, the Xbox extras' folder when the player has set those up. Nothing
// of a game ships with the app: a game whose disc is not set up has none of
// this (FrontGame::installed false, every picture empty) and the menu draws
// it from its name and colour alone.
//
//   card     the game's title picture (artSource: AI's loading screen, DM2's
//            legal screen, BMX XXX's start screen), as the tiles show today;
//   riders   full-body cut-outs from the game's own menu library (AI's
//            pros, BMX XXX's two riders; DM2 has photos, not cut-outs);
//   loops    the menu's moving background: AI's BACKGRND.PSM, DM2's
//            UIMOVIE.PSM (both the games' own menu backgrounds), BMX XXX's
//            Xbox level previews; never BMX XXX's intro or level movies;
//   still    BMX XXX without the Xbox extras: its main menu's background,
//            shown with a slow pan in place of a movie, over the part of
//            it clear of the menu text drawn into it (its "MAIN MENU");
//   intro    the Z-Axis logo part of the game's developer movie, for the
//            collection's intro.

#include "collection/menu.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::collection
{
    // A part of a movie: from `start` to `end` seconds (end 0: to its end).
    struct MovieClip
    {
        std::filesystem::path movie; // empty: none
        double start = 0.0, end = 0.0;

        bool empty() const { return movie.empty(); }
    };

    struct FrontGame
    {
        std::string id;
        bool installed = false;    // its disc folder holds the game (SYSTEM.CNF)
        uint32_t colour = 0;       // its colour in the menu (rgba(); ours, not the disc's)
        Image card;                // empty: not installed or not readable
        Rect cardFocus;            // of card; empty: all of it
        std::vector<Image> riders; // cut-outs with alpha, in the roster's order
        std::vector<std::filesystem::path> loops; // played one after another, over and over
        Image still;               // shown when there is no loop (BMX XXX without its Xbox extras)
        Rect stillPart;            // of still: the part the menu pans over, clear of the menu text drawn into it; empty: all of it
        MovieClip intro;           // the Z-Axis logo
        std::vector<std::string> missing; // what was looked for and not found (for the log)
    };

    // The colour the menu gives a game (its card's frame, its page's rule):
    // Aggressive Inline blue, Dave Mirra BMX 2 orange, BMX XXX red.
    uint32_t gameColour(std::string_view game);

    // Everything above for one game. `extras`: the BMX XXX Xbox extras'
    // folder (the app's releases folder; empty or missing: none).
    FrontGame loadFrontGame(std::string_view game, const std::filesystem::path &disc,
                            const std::filesystem::path &extras = {});

    // Every game of the collection, in its order (collection/game_order.h:
    // DM2, AI, BMX XXX), each from the disc folder `discOf` gives for its id
    // (empty: not set up) and BMX XXX with `extras`.
    std::vector<FrontGame> loadFrontGames(const std::function<std::filesystem::path(std::string_view)> &discOf,
                                          const std::filesystem::path &extras = {});

    // The game's roster portraits for the Thor's bottom screen (AI's head_*,
    // DM2's face_*; BMX XXX none). Kept apart from loadFrontGame: DM2's are
    // in its 35 MB menu library.
    std::vector<Image> loadRosterHeads(std::string_view game, const std::filesystem::path &disc);

    // A picture of a texture library (TXR, ZIF, ZIT, or a ZAL archive of
    // them) by its name, with its alpha: TZMP's palette alpha (the PS2's
    // 0x80 for opaque made 0xFF), TBMP opaque. False if there is none.
    bool readLibraryPicture(const std::vector<uint8_t> &file, std::string_view name, Image &out);

    // The intro's choice among the installed games' logos, in turn: the
    // `launch`-th launch's (counting from 0), skipping games without one;
    // empty if none has one.
    MovieClip introFor(const std::vector<FrontGame> &games, uint32_t launch);
}
