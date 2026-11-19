#pragma once

// The discs in the places a player gives the setup (a folder of every game's
// dumps, files and folders dropped on the window, a folder picked in
// Android's documents UI): every PS2 disc of the collection's games and every
// Xbox and GameCube disc the extras are made from, told by what they hold,
// not their names (collection/game_setup.h's DiscSource and checkSource,
// collection/extras/setup.h's checkExtrasSource), in the folders below them
// too. Paths and documents alike: Mac and Windows folders and Android's
// documents go through the same search.

#include "collection/extras/setup.h"
#include "collection/game_setup.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::app
{
    // One disc the search found.
    struct FoundDisc
    {
        enum class Kind
        {
            Ps2,    // a PS2 disc (a game's, another release's, or another game's)
            Extras, // an Xbox or GameCube game disc
        };
        Kind kind = Kind::Ps2;
        std::filesystem::path source; // as the setup opens it (a BIN/CUE dump by its CUE; a document's URI)
        std::string name;             // for the player ("Aggressive Inline-ps2.iso")
        int game = -1;                // Ps2: its game, an index of the games searched for (-1: none of them)
        SourceCheck::Status status = SourceCheck::Status::Unreadable; // Ps2: how it checked
        uint64_t bytes = 0;           // Ps2: its files' size
        const extras::ExtrasDisc *extras = nullptr; // Extras: the extras disc it is (null: one the app makes nothing from)
        std::string message;          // what its check said, for the player
    };

    struct DiscSearch
    {
        std::vector<FoundDisc> discs; // in the order found: folders by name, a folder's files before its folders
        size_t looked = 0;            // the files and folders looked at
        bool cancelled = false;

        // The first disc found of a game that the setup installs (checked
        // Ok), null none.
        const FoundDisc *ps2Of(int game) const;
        // The first other PS2 disc of that game (another release, another
        // version), for the line that says why it is not set up; null none.
        const FoundDisc *otherReleaseOf(int game) const;
        // The extras discs of a game (by collection id), the first of each release.
        std::vector<const FoundDisc *> extrasOf(const std::string &game) const;
        // The discs of `more` not already found (by source).
        void add(DiscSearch more);
    };

    struct SearchProgress
    {
        size_t looked = 0;
        std::string at; // the file or folder being looked at, its name
    };
    // Called for each file and folder; false stops the search (cancelled).
    using SearchProgressFn = std::function<bool(const SearchProgress &)>;

    // How deep below a place the search goes (a folder of folders of each
    // game's dumps is two).
    constexpr int kSearchDepth = 4;

    // Every disc in `places` (each a file or a folder, a path or a
    // document): a folder with SYSTEM.CNF at its top is a PS2 disc; any
    // other folder is searched, kSearchDepth deep; a file is a disc when its
    // bytes say so (a BIN a CUE beside it reads is the CUE's). Hidden files
    // and folders are passed over. `extrasOpener` opens an Xbox or GameCube
    // image (none: no extras discs are looked for).
    DiscSearch findDiscs(const std::vector<std::filesystem::path> &places, const std::vector<SetupGame> &games,
                         const extras::ImageOpener &extrasOpener, const SearchProgressFn &progress = {});

    // The PS2 discs a picked or dropped path or document stands for: itself
    // when it is a disc; for a folder that is none, each PS2 disc in it and
    // its folders (a folder of every game's dumps); else itself (for its
    // message).
    std::vector<std::filesystem::path> discSourcesIn(const std::filesystem::path &path);
}
