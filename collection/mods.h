#pragma once

// Mods: content a player adds to the games, every kind in one format
// (docs/MODS.md). A mod is a folder, or a zip read in place (runtime/
// zip_archive.h: never unpacked), in the collection's Mods folder
// (<support>/Mods), with a manifest, mod.ini:
//
//   [mod]
//   id = hd-rails                ; unique among the mods: the settings know it by this
//   name = HD Rails              ; shown on the Mods page
//   version = 1.2
//   author = ...
//   games = ai, dm2              ; the games that read it (ai, dm2, bmx)
//   needs = base-pack            ; ids it is read after and only with (optional)
//   conflicts = other-rails      ; ids it is not read with (optional)
//
// What a mod holds is told by its layout, not declared: the paths the games
// already load by.
//   - files: any disc path (ASSETS/..., MOVIES/...): that file of the game's
//     disc (runtime/game_files.h); and for an archive, a folder of its
//     name holding single entries (ASSETS/UILIB.ZAL/CommonFonts.zif): those
//     entries of it, so mods with other entries of one archive all count;
//   - music: Music/NN*.ogg (.wav, .flac, .mp3): song NN of the soundtrack;
//   - tuning: tuning.ini, the developer tuning table's values (Aggressive
//     Inline's);
//   - textures: Textures/<game>/ (or the game's disc serial, or Textures/
//     itself for a mod of one game): replacement pictures for the GPU
//     renderer, PCSX2's names or .xtp packs (ZX_TEXTURE_PACKS);
//   - a map: a [map] section in mod.ini (map.ini's keys, collection/maps.h)
//     or a map.ini: a custom map for Custom Freeplay, read by no other boot.
// Files, tuning and maps change game logic; music and textures never do
// (the page marks the first).
//
// The layouts before manifests still work unchanged: each game's own Mods
// folder (<support>/games/<id>/Mods; the single-game app's <support>/Mods)
// is that game's, its loose files read under every mod and its folders
// (and zips) mods of that game, by their names; the collection's maps
// folder (<support>/Maps) still holds maps. A mod there may carry a mod.ini
// too, for its name and version (its games are its folder's game).
//
// The order: one list for every mod (the collection's settings.ini [mod
// order]), a later mod's file read over an earlier one's; mods not in it come
// after, each game's own folder's first, then the collection's, each by name.
// A mod is read after the mods it needs, and not at all while one of them is
// missing or not read, or while a mod it conflicts with is read. Each game
// switches each mod on or off (its settings.ini [mods], by the mod's key);
// a mod is on unless switched off. A mod no game can read (no mod.ini
// naming its games, a manifest that does not parse, the id of a mod listed
// before it) is listed with why.

#include "collection/maps.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zx::app
{
    constexpr const char *kModManifestFile = "mod.ini";
    // The collection's Mods folder, in its support folder.
    constexpr const char *kModsFolder = "Mods";

    // What a mod holds (its layout's parts).
    enum ModContent : unsigned
    {
        kModFiles = 1u,    // disc paths
        kModMusic = 2u,    // Music/
        kModTuning = 4u,   // tuning.ini
        kModTextures = 8u, // Textures/
        kModMap = 16u,     // [map] or map.ini
    };

    // mod.ini's [mod] section.
    struct ModManifest
    {
        std::string id, name, version, author;
        std::vector<std::string> games;     // collection ids, lower case
        std::vector<std::string> needs, conflicts;
    };
    // mod.ini's text into `out`: false (with why) without a [mod] section
    // naming its id and its games (known ones: ai, dm2, bmx). A mod.ini that
    // describes only a map ([map], no [mod]) is not a manifest: false, with
    // `error` left empty.
    bool parseModManifest(std::string_view text, ModManifest &out, std::string *error = nullptr);

    struct Mod
    {
        std::string name;             // its key: the manifest's id, else the folder's (or zip's) name
        std::filesystem::path folder; // the folder or the zip
        bool on = true;               // the game's switch ([mods]); for a game's list: switched on

        std::string title;            // shown: the manifest's name, else the folder's
        std::string version, author;
        std::vector<std::string> games; // the games that read it
        std::vector<std::string> needs, conflicts;
        std::string place;            // the folder it is in, as the page says it: "Mods", "games/ai/Mods"
        std::string ownGame;          // a game's own Mods folder's mod: that game ("" the collection's)
        bool manifest = false;        // it has a [mod] section
        bool zip = false;
        unsigned contents = 0;        // ModContent bits
        std::vector<std::string> notes; // parts of it no game reads ("Textures/bmx: BMX XXX is not one of its games")
        std::string problem;          // why no game reads it ("" none)
        CustomMap map;                // contents & kModMap: its description

        // In a game's list (modsForGame): whether the game reads it, and why
        // not ("Off", "needs base-pack"...).
        bool read = true;
        std::string why;
        // In a game's list, once checkArchiveEntries has looked: the entries
        // it gives the game's archives that are not read, a line each
        // ("ASSETS/UILIB.ZAL has no entry Foo.zsc").
        std::vector<std::string> entriesNotRead;

        // The order's key: where it is ("Mods/hd.zip", "games/ai/Mods/10-park").
        std::string orderKey() const;
        bool changesLogic() const { return (contents & (kModFiles | kModTuning | kModMap)) != 0u; }
        bool isMap() const { return (contents & kModMap) != 0u; }
        bool forGame(std::string_view game) const;
    };

    // A game's own Mods folder's mods (`modsRoot`), in name order (byte by
    // byte, as the names compare). A folder named like one of the game's own
    // top-level disc folders (`discRoot`'s: ASSETS, MOVIES...; compared
    // without case) or Music is the loose layout's, not a mod; a folder with
    // a map.ini is a custom map (collection/maps.h), not a mod either,
    // whether it is switched on or off: no boot reads it. Those named in
    // `off` (exactly) are listed switched off. Each is `game`'s. Empty when
    // there is no Mods folder.
    std::vector<Mod> listMods(const std::filesystem::path &modsRoot, const std::filesystem::path &discRoot,
                              const std::vector<std::string> &off = {}, std::string_view game = {});
    // The custom maps of a game's own Mods folder (`modsRoot`, games/<game>/
    // Mods: the folders and zips listMods leaves out for their map.ini or
    // their mod.ini's [map] section), in name order, each where it is
    // ("games/ai/Mods/park"); a description that names no game is `game`'s,
    // one that does not parse is left out (collection/maps.h).
    std::vector<CustomMap> listGameMaps(const std::filesystem::path &modsRoot, std::string_view game);
    // The collection's Mods folder's mods (`sharedRoot`), maps among them, in
    // name order; one with the id of one before it gets a problem.
    std::vector<Mod> listSharedMods(const std::filesystem::path &sharedRoot);
    // One folder or zip as the collection's Mods folder reads it (one of
    // listSharedMods', the ids of others aside), wherever it is: what a
    // folder dropped on the window would be there (collection/mod_install.h).
    Mod sharedModAt(const std::filesystem::path &entry);

    // The mods in the order's order (`order`: orderKey()s): those it names
    // first, as it has them, then the rest as they come.
    void orderMods(std::vector<Mod> &mods, const std::vector<std::string> &order);
    // The order's list for these mods as they are now.
    std::vector<std::string> modOrderOf(const std::vector<Mod> &mods);

    // One game's mods in the order it reads them: its own folder's (`own`,
    // listMods's) and the collection's (`shared`) that name it, maps left
    // out, ordered by `order`, each switched by `off`, with read and why:
    // off, a problem, a need missing or not read, a conflict read, the key of
    // a mod before it. A mod comes after the mods it needs.
    std::vector<Mod> modsForGame(std::string_view game, const std::vector<Mod> &own, const std::vector<Mod> &shared,
                                 const std::vector<std::string> &order, const std::vector<std::string> &off);

    // The folders a game's file system reads, lowest first: the Mods folder
    // itself, then each mod's that it reads (ZX_MODS_DIR's text: modsText).
    std::vector<std::filesystem::path> modsRoots(const std::filesystem::path &modsRoot, const std::vector<Mod> &mods);
    // For each mod of a game's list that it reads, the single archive
    // entries it gives that the game does not read (Mod::entriesNotRead): an
    // entry the archive has not (as the game's disc at `discRoot` has it,
    // with the mods before it on it), an archive the disc has not, one in a
    // folder of its own. `modsRoot`: the game's Mods folder (modsRoots').
    void checkArchiveEntries(std::vector<Mod> &mods, const std::filesystem::path &modsRoot, const std::filesystem::path &discRoot);

    // The texture folder a mod gives a game (Textures/<game>, the game's disc
    // serial, or Textures itself for a mod of that game alone), empty for none.
    std::filesystem::path modTextureFolder(const Mod &mod, std::string_view game);
    // The texture folders of the mods a game reads, lowest first
    // (ZX_TEXTURE_PACKS's text: modsText).
    std::vector<std::filesystem::path> texturePacks(const std::vector<Mod> &mods, std::string_view game);

    // What wins where two mods a game reads have the same thing: a disc file
    // (its path), an archive's entry (its archive and its id, whatever name
    // gives it), a song (its number), tuning.ini, a texture (its file name):
    // the later mod's. A mod's whole archive later than another's entries of
    // it wins over them too (entries later than a whole archive are put in
    // it: both count). For each mod of the list (by index), the things it
    // has, and the mods it wins over and loses to, with how many each.
    struct ModOverlap
    {
        size_t mod = 0;   // the other mod's index in the list
        size_t count = 0; // things both have
        std::string example; // one of them, for the page and the log
    };
    struct ModWinners
    {
        size_t things = 0; // what it gives the game
        std::vector<ModOverlap> winsOver, losesTo;
    };
    std::vector<ModWinners> modWinners(const std::vector<Mod> &mods, std::string_view game);

    // A game's disc serial (SLUS-20327...), "" for none known.
    const char *gameSerial(std::string_view game);
}
