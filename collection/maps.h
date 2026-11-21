#pragma once

// Custom maps for Custom Freeplay. A map is a mod (collection/mods.h) in the
// collection's Mods folder (<support>/Mods/<map>/, or a zip read in place),
// in a game's own Mods folder (<support>/games/<id>/Mods/<map>/), or, as
// before mods had manifests, a folder in the collection's maps folder
// (<support>/Maps/<map>/): the level files of the game it was made in, laid
// out as one of that game's slots has them (below), with a description, a
// [map] section in its mod.ini or a map.ini:
//
//   [map]
//   name = Test Park            ; shown in the menu
//   game = ai                   ; the game it was made in (ai, dm2, bmx): its formats
//                               ; (in a game's own Mods folder: that game, unless named)
//   slot = TUT                  ; that game's slot its files are laid out in (ASSETS/LEVELS/TUT)
//   slot.dm2 = COMMERCI         ; optional, for each other game: the slot its player gets the
//                               ; map converted into (default: that game's first it can start)
//   park = park.dat             ; DM2's park slot (slot = PARK): the saved park in the folder
//   preview = preview.png       ; a picture in the folder (a PNG, a JPEG, or a 4, 8, 24 or 32-bit BMP), optional
//   author = ...                ; optional
//   npcs = none                 ; none (the default) | slot, below
//
// A player of the game the map was made in reads the folder as it is: last
// of the mods, started into the slot its files are in (mapSlots). A player
// of another game gets the map converted at the boot into that game's level
// files for one of its slots (collection/levels: the tooling's Python
// converters, natively), in memory: the boot reads them as a memory folder
// (runtime/game_files.h), and nothing is written. A map that does not
// convert (over a game's budget) says so in the menu. A DM2 park (a park
// saved in DM2's park editor, over one of its themes) is built by DM2's own
// park code: it plays in DM2 only.
//
// A map's goals are its own files' (AI's level scripts, BMX XXX's
// OBJCTV.TXT) and run in its own game, as the slot level's would; what they
// mark done is the map's, not the slot level's (the slot level's record in
// the player's save is put back as it was: each game's start, mapSlots'
// `needs`). A conversion carries what the target game reads by name: the
// named scene points (AI's lesson points, BMX XXX's sniper points) and the
// faces' and edges' tag words (DM2's gaps, BMX XXX's objectives); goals
// themselves are not translated from one game's system into another's.
//
// A map's people, riders and traffic are its own files' (the objects its
// level files place, the scripts it carries). Where the game keeps a
// level's NPCs outside the level's folder, keyed by the level (BMX XXX's NPC
// riders: the executable's records, the models' definitions and the rides
// of its archives, game_overrides/bmx_xxx_npcs.cpp), the map leaves the
// slot's out unless it says `npcs = slot`: a map made from the slot's own
// level, which keeps its riders where they ride (in its own game only).
//
// No boot but the map's own Custom Freeplay boot reads a map: a map in the
// Mods folder is no mod a game reads, nor is a folder with a map.ini in a
// game's own Mods folder (collection/mods.h).

#include "collection/levels/convert.h"
#include "collection/levels/export.h"
#include "collection/offered.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zx::app
{
    struct Mod; // collection/mods.h

    constexpr const char *kMapDescriptionFile = "map.ini";
    // The collection's maps folder, in its support folder.
    constexpr const char *kMapsFolder = "Maps";
    // The memory folder a boot into a converted map reads.
    constexpr const char *kConvertedMapFolder = "custom-map";

    struct CustomMap
    {
        std::string name;
        std::string game;    // collection id: the game it was made in
        std::string slot;    // that game's slot its files are in, upper case
        std::string preview; // the picture's file name in the folder ("" none)
        std::string author;
        bool slotNpcs = false;        // npcs = slot: the slot level's own NPCs kept (none by default)
        // slot.<game>: the slot another game's player gets it converted into, by game ("" none named).
        std::map<std::string, std::string> targets;
        std::string park;             // a DM2 park (slot PARK): the park file's name in the folder
        std::filesystem::path folder; // the map's folder (or zip)
        std::string folderName;       // its name in the folder it is in (a zip's with .zip)
        std::string where;            // where it is, as the menu says it: "Mods/park", "Maps/park"

        std::filesystem::path previewFile() const { return preview.empty() ? std::filesystem::path() : folder / preview; }
        // The preview's bytes (from the folder or the zip): false for none.
        bool readPreview(std::vector<uint8_t> &out) const;
    };

    // map.ini's text into `out`: false (with why) without a [map] section
    // naming name, game and slot (and npcs none or slot, when it has one).
    // `game`: the game whose own Mods folder the map is in, its game when
    // the section names none ("" none: the section must). Other keys are
    // kept out; the slots come out upper case, the games lower case; a park
    // slot's file is park.dat unless `park` names another.
    bool parseMapDescription(std::string_view text, CustomMap &out, std::string *error = nullptr, std::string_view game = {});

    // Whether the folder (or zip) carries a map.ini.
    bool isMapFolder(const std::filesystem::path &folder);

    // The maps of the maps folder (its folders with a map.ini), in name
    // order, each with its description read; a folder whose map.ini does not
    // parse is left out.
    std::vector<CustomMap> listCustomMaps(const std::filesystem::path &mapsRoot);
    // Every map the menu lists, each once where it is: the Mods folder's
    // maps (`sharedMods`, listSharedMods's: those with no problem), then
    // each game's own Mods folder's (`gameMaps`, collection/mods.h listGameMaps's), then the
    // maps folder's (`mapsRoot`).
    std::vector<CustomMap> listAllMaps(const std::vector<Mod> &sharedMods, const std::filesystem::path &mapsRoot,
                                       const std::vector<CustomMap> &gameMaps = {});

    // A level slot a game lends custom maps in free play, and how the game
    // is started into it. A slot is a level folder's name: the map's files
    // go where the game keeps that folder (ASSETS/LEVELS/<slot>); DM2's park
    // slot is the park editor's, a saved park over a theme.
    struct MapSlot
    {
        const char *game;       // collection id
        const char *slot;       // the level folder the map's files replace
        const char *folder;     // the folder that level folder is in
        const char *startLevel; // the game's free-play start of that slot (ZX_DEV_START_LEVEL's word)
        const char *needs;      // a boot switch the start needs on ("" none): AI's Estate entry, DM2's level camera points
        // The boot switch that keeps the slot level's record in the player's
        // save as it was ("" none): AI's level records, BMX XXX's level goals.
        const char *keeps;
        // The boot name that has the slot level's own NPCs come (1) or not
        // (0), given 0 unless the map keeps them ("" when the slot has none
        // outside the map's files: AI's levels' are their scripts, DM2's
        // FREERIDE has none).
        const char *slotNpcs;
        // The name the bottom panel knows the slot's level by (its
        // <name>.pmap: collection/panel.h): the folder, or what the game
        // serves there (AI's Estate).
        const char *panelLevel;
        // What the start needs of the player's setup (collection/offered.h):
        // AI's Tutorial slot is the Estate's entry, made from its GameCube disc.
        Need need = Need::Nothing;
        // DM2's park slot: the map is a saved park (map.ini's park file, the
        // boot's ZX_DM2_PARK_FILE) built by the game over its theme, not
        // level files: never converted, played in DM2 only.
        bool park = false;
    };
    // Every slot lent, each game's in its order: the first a setup can start
    // is the one a converted map goes to unless the map names another.
    const std::vector<MapSlot> &mapSlots();
    // The slot `slot` of `game`; nullptr when the game lends none of that name.
    const MapSlot *mapSlot(std::string_view game, std::string_view slot);
    // The game's slots, in their order.
    std::vector<const MapSlot *> mapSlotsOf(std::string_view game);
    // The game's first slot, nullptr for none.
    const MapSlot *mapSlotOf(std::string_view game);

    // Whether the player's setup can start a slot (the app's: offered(need)
    // of the setup's folders). None given: every slot.
    using SlotStartable = std::function<bool(const MapSlot &)>;
    // The slot a boot of `player`'s game plays the map in: in its own game
    // the slot its files are in when the setup starts it, else (AI's
    // Tutorial without the Estate) the game's first other slot that starts,
    // the map converted into it with its own goals; in another game, the
    // slot the map names for it (slot.<game>) when that one starts, else
    // the game's first that starts (never a park). nullptr: the player
    // cannot start it (its files are in no slot its game lends, a park
    // where the park slot does not start or for another game, no slot
    // starts).
    const MapSlot *mapSlotFor(const CustomMap &map, std::string_view player, const SlotStartable &startable = {});
    // Whether `player`'s game can start the map (mapSlotFor has a slot).
    bool mapPlayableIn(const CustomMap &map, std::string_view player, const SlotStartable &startable = {});
    // Whether a boot into `slot` (mapSlotFor's) reads the map converted
    // (in memory) rather than its folder: any slot but its own.
    bool mapNeedsConversion(const CustomMap &map, const MapSlot &slot);
    // A map made into another game's files, for its boot.
    struct ConvertedMap
    {
        levels::MapFiles files;          // in a Mods folder's layout: the memory folder's
        const MapSlot *slot = nullptr;   // the player's slot they are made for
        size_t bytes = 0;                // their size, held while the boot runs
        double milliseconds = 0.0;       // the conversion's time
        std::vector<std::string> report; // the converter's
    };
    // The map's level files (where its own game's slot has them), any case;
    // a zip read in place. Throws levels::LevelError for a file missing.
    levels::LevelFiles readMapLevelFiles(const CustomMap &map);
    // The map's level files made into `target`'s game's for that slot (a
    // slot of the player's game: mapSlotFor). Into another slot of its own
    // game the map keeps its goals: its files that hold them go with it,
    // under the slot's names (AI's level scripts <slot>.BIN and <slot>T.BIN
    // and its MOTIONS.ZAL; BMX XXX's OBJCTV.TXT and the level's asset
    // libraries), where the converter writes empty ones. Throws
    // levels::LevelError (why: over a budget, a file missing).
    ConvertedMap convertMap(const CustomMap &map, const MapSlot &target);

    // The folders a boot into `map` reads, lowest first: the Mods folder,
    // the mods the game reads (`mods`, modsForGame's: no maps among them),
    // then the map's folder unless the boot reads it converted (then the
    // memory folder holds its files).
    std::vector<std::filesystem::path> freeplayModsRoots(const std::filesystem::path &modsRoot, const std::vector<Mod> &mods,
                                                         const CustomMap &map, bool converted = false);
}
