#pragma once

// The extras of a game set up from the player's other releases' discs (the
// Games page's extras rows): each disc is checked by its own hash (its
// executable's SHA-256: an Xbox disc's default.xbe, a GameCube disc's
// main.dol), recorded with where it came from, and every extra the game
// has is made from the discs recorded for it and its installed PS2 disc
// (<support>/games/<id>/disc), into <support>/games/<id>/extras/<folder>,
// the folders the games read as the bundled extras of a workshop build:
//
//   game  disc       folder           what (setting)
//   ai    Xbox       xbox             the Xbox files read as they are: career and free skate
//                                     scripts, languages, the Tutorial's loading screens, the
//                                     Zoo's flythrough (Career Scripts, Xbox Free Skate, Language)
//                    xbox-converted   the Zoo and the Ape (Zoo, Ape)
//                    xbox-pack        textures, Bink movies, skaters (Xbox Textures, Movies, Skaters)
//   ai    GameCube   estate           TUT/Estate (Estate Level)
//   dm2   Xbox       xbox-levels      Venice, Greenville, Desert, Highway, the attract demo's
//                                     replays, the Retro movie (Xbox Levels, Themes, Retro Movie,
//                                     Attract Demo)
//   bmx   Xbox, GameCube
//                    releases         challenge scripts, strings, editor pieces, level previews,
//                                     logo movie, the GameCube's levels (each its setting)
//   each  (PS2)      panel            the bottom screen's maps and goals (collection/extras/
//                                     panel_maps.h), from the PS2 disc and the extras above
//
// Each folder is a part, kept under its own key (<gameDir>/extras/<folder>.key): the part's makers and each
// one's version (setup.cpp kMakers), the discs it reads (by their executable's SHA-256), the PS2 disc's boot ELF
// and the keys of the parts it reads (the bottom screen's maps read the Zoo, Estate and DM2's Xbox levels). A part
// is made again only when the key it would have now differs from its own: an update whose maker's output changed,
// a disc added or taken away, the PS2 disc set up from another release. It is made in <folder>.partial and put in
// its folder's place, its key written last, so a stopped or failed make leaves the part there was, and a part
// with no key is made again. A part whose disc does not open now (a drive not there, a moved image) stays as it is,
// with its key, and its disc stays recorded. <gameDir>/extras.manifest records the discs ("# z-axis extras 2"):
// 1.1.0's ("# z-axis extras 1") had no keys, and its parts count as made by every maker's version 0. Nothing reads
// a disc image while a game runs.

#include "collection/extras/ai_xbox_skaters.h"
#include "collection/extras/disc_image.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zx::app::extras
{
    // An extras disc the app knows.
    struct ExtrasDisc
    {
        const char *game;     // the collection's id ("ai")
        const char *release;  // "xbox" or "gamecube": the row's and the manifest's name
        const char *title;    // for the player ("Aggressive Inline (Xbox, USA)")
        const char *gameCode; // a GameCube disc's game ID ("GILE51"); "" for an Xbox disc
        const char *sha256;   // of its executable (default.xbe, main.dol)
    };
    const std::vector<ExtrasDisc> &extrasDiscs();
    // The game's extras discs, in the order of its rows.
    std::vector<const ExtrasDisc *> extrasDiscsOf(const std::string &game);

    // How a source (a path, or on Android a document's URI) is opened.
    using ImageOpener = std::function<std::unique_ptr<ImageFile>(const std::string &source, std::string *error)>;
    // ImageFile::open of the source as a UTF-8 path.
    ImageOpener pathOpener();

    // An extras disc opened and checked.
    struct ExtrasSource
    {
        const ExtrasDisc *disc = nullptr;
        std::string source; // as the player gave it (UTF-8, a URI kept as it is)
        std::unique_ptr<DiscFiles> files;
        std::string message; // for the player
        bool gameDisc = false; // an Xbox or GameCube game disc (an extras disc or not)
    };
    // Opens `source` and says which extras disc it is (any game's when
    // `game` is ""). disc is null, with the message in plain words, when it
    // cannot be read, is no Xbox or GameCube game disc, or is another game's
    // or release's than the app knows.
    ExtrasSource checkExtrasSource(const std::string &source, const std::string &game, const ImageOpener &open);

    // The extras discs recorded for a game (<gameDir>/extras.manifest): the
    // release and the source of each.
    struct RecordedDisc
    {
        std::string release;
        std::string source;
    };
    std::vector<RecordedDisc> recordedExtras(const std::filesystem::path &gameDir);
    std::filesystem::path extrasFolder(const std::filesystem::path &gameDir);

    // The step's progress (the extra being made, done of total); false stops.
    using ExtrasProgressFn = std::function<bool(const std::string &step, size_t done, size_t total)>;

    struct ExtrasResult
    {
        bool ok = false;
        bool cancelled = false;
        std::string message;           // for the player
        std::vector<std::string> log;  // the converters' lines
        std::vector<std::string> made; // the parts made (their folders)
        std::vector<std::string> kept; // the parts whose key differs, kept as they are for want of their disc
        std::vector<RecordedDisc> missing; // those discs
    };
    // Makes each part of `game`'s extras whose key differs from the one it would have now, from `sources` (each
    // checked: its disc set) and the installed PS2 disc in <gameDir>/disc, into <gameDir>/extras; the parts no disc
    // makes any more are removed, and the discs recorded. `unopened`: discs recorded before that do not open now:
    // the parts made from them stay as they are and they stay recorded. No sources: the bottom screen's maps alone.
    // `models`: the engine's Xbox model converter (engineXboxModelMaker; none: no Xbox skaters).
    ExtrasResult makeExtras(const std::string &game, const std::filesystem::path &gameDir,
                            std::vector<ExtrasSource> &sources, const XboxModelMaker &models,
                            const ExtrasProgressFn &progress = {}, const std::vector<RecordedDisc> &unopened = {});

    // The parts of a game's extras whose key differs from the one they would have now, by their recorded discs
    // (none: nothing to make), and the recorded discs those are made from. Reads the keys and the records alone,
    // never a disc. None when the game is not set up (no <gameDir>/disc). `models`: as makeExtras's.
    struct StaleExtras
    {
        std::vector<std::string> parts;  // folders
        std::vector<RecordedDisc> discs; // what they are made from
        bool unkeyed = false;            // made before parts had keys (1.1.0): its parts' keys and record to write
        bool empty() const { return parts.empty() && !unkeyed; }
    };
    StaleExtras staleExtras(const std::string &game, const std::filesystem::path &gameDir, bool models = true);
}
