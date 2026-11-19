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
// The setup makes them in <gameDir>/extras.partial and puts that in place
// of <gameDir>/extras when all are made, with <gameDir>/extras.manifest
// (the discs and their sources) written last: a stopped or failed setup
// leaves the extras there were. Nothing reads a disc image while a game runs.

#include "collection/extras/ai_xbox_skaters.h"
#include "collection/extras/disc_image.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::app::extras
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
    };
    // Makes every extra of `game` from `sources` (each checked: its disc
    // set) and the installed PS2 disc in <gameDir>/disc, into
    // <gameDir>/extras, and records the sources. No sources: the bottom
    // screen's maps alone, the extras made before and their record removed. `models`: the engine's Xbox model converter
    // (engineXboxModelMaker; none: no Xbox skaters).
    ExtrasResult makeExtras(const std::string &game, const std::filesystem::path &gameDir,
                            std::vector<ExtrasSource> &sources, const XboxModelMaker &models,
                            const ExtrasProgressFn &progress = {});
}
