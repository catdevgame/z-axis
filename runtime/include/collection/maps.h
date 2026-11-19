#pragma once

// Custom maps for Custom Freeplay. A map is a mod (collection/mods.h) in the
// collection's Mods folder (<support>/Mods/<map>/, or a zip read in place),
// or, as before mods had manifests, a folder in the collection's maps
// folder (<support>/Maps/<map>/): the level files of the game it was made
// in, laid out as that game's slot has them (below), with a description, a
// [map] section in its mod.ini or a map.ini:
//
//   [map]
//   name = Test Park            ; shown in the menu
//   game = ai                   ; the game it was made in (ai, dm2, bmx): its formats
//   slot = TUT                  ; that game's slot its files are laid out in (ASSETS/LEVELS/TUT)
//   preview = preview.bmp       ; a picture in the folder (a 4, 8, 24 or 32-bit BMP), optional
//   author = ...                ; optional
//   npcs = none                 ; none (the default) | slot, below
//
// Every player starts every map. A player of the game the map was made in
// reads the folder as it is: last of the mods, in the level slot the game
// lends its free play (mapSlots). A player of another game gets the map
// converted at the boot into that game's level files for its slot
// (collection/levels: the tooling's Python converters, natively), in
// memory: the boot reads them as a memory folder (runtime/ps2_game_files.h),
// and nothing is written. A map that does not convert (over a game's
// budget) says so in the menu.
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

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::app
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
        std::filesystem::path folder; // the map's folder (or zip)
        std::string folderName;       // its name in the folder it is in (a zip's with .zip)
        std::string where;            // where it is, as the menu says it: "Mods/park", "Maps/park"

        std::filesystem::path previewFile() const { return preview.empty() ? std::filesystem::path() : folder / preview; }
        // The preview's bytes (from the folder or the zip): false for none.
        bool readPreview(std::vector<uint8_t> &out) const;
    };

    // map.ini's text into `out`: false (with why) without a [map] section
    // naming name, game and slot (and npcs none or slot, when it has one).
    // Other keys are kept out; the slot comes out upper case, the game lower
    // case.
    bool parseMapDescription(std::string_view text, CustomMap &out, std::string *error = nullptr);

    // Whether the folder (or zip) carries a map.ini.
    bool isMapFolder(const std::filesystem::path &folder);

    // The maps of the maps folder (its folders with a map.ini), in name
    // order, each with its description read; a folder whose map.ini does not
    // parse is left out.
    std::vector<CustomMap> listCustomMaps(const std::filesystem::path &mapsRoot);
    // Every map the menu lists, each once where it is: the Mods folder's
    // maps (`sharedMods`, listSharedMods's: those with no problem), then the
    // maps folder's (`mapsRoot`).
    std::vector<CustomMap> listAllMaps(const std::vector<Mod> &sharedMods, const std::filesystem::path &mapsRoot);

    // The level slot a game lends a custom map in free play, and how the
    // game is started into it. A slot is a level folder's name: the map's
    // files go where the game keeps that folder (AI's and BMX XXX's
    // ASSETS/LEVELS/<slot>).
    struct MapSlot
    {
        const char *game;       // collection id
        const char *slot;       // the level folder the map's files replace
        const char *folder;     // the folder that level folder is in
        const char *startLevel; // the game's free-play start of that slot (PS2X_DEV_START_LEVEL's word)
        const char *needs;      // a boot switch the start needs on ("" none): AI's entry, DM2's level camera points, BMX XXX's goal counts kept
        // The boot name that has the slot level's own NPCs come (1) or not
        // (0), given 0 unless the map keeps them ("" when the slot has none
        // outside the map's files: AI's Tutorial's are its scripts, DM2's
        // park has none).
        const char *slotNpcs;
    };
    // Every slot lent; nullptr when the game lends none for that slot.
    const std::vector<MapSlot> &mapSlots();
    const MapSlot *mapSlot(std::string_view game, std::string_view slot);
    // The slot a game lends (each lends one), nullptr for none.
    const MapSlot *mapSlotOf(std::string_view game);

    // Whether `player`'s game can start the map: the map is in the slot its
    // own game lends (where its files are read from) and the player's game
    // lends one. Then every player can: the map is converted for another.
    bool mapPlayableIn(const CustomMap &map, std::string_view player);
    // Whether a boot of `player`'s game reads the map converted into its
    // formats (in memory) rather than its folder.
    bool mapNeedsConversion(const CustomMap &map, std::string_view player);
    // A map made into another game's files, for its boot.
    struct ConvertedMap
    {
        levels::MapFiles files;          // in a Mods folder's layout: the memory folder's
        size_t bytes = 0;                // their size, held while the boot runs
        double milliseconds = 0.0;       // the conversion's time
        std::vector<std::string> report; // the converter's
    };
    // The map's level files made into `player`'s game's for its slot.
    // Throws levels::LevelError (why: over a budget, a file missing).
    ConvertedMap convertMap(const CustomMap &map, std::string_view player);

    // The folders a boot into `map` reads, lowest first: the Mods folder,
    // the mods the game reads (`mods`, modsForGame's: no maps among them),
    // then the map's folder unless the boot reads it converted (then the
    // memory folder holds its files).
    std::vector<std::filesystem::path> freeplayModsRoots(const std::filesystem::path &modsRoot, const std::vector<Mod> &mods,
                                                         const CustomMap &map, bool converted = false);
}
