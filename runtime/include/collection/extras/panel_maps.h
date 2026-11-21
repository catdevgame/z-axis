#pragma once

// The bottom panel's files (collection/panel_file.h has their format): for
// each level a top-down map of it, made from its geometry, with the
// world-to-map transform and the level's goals; and UI.pmap, the game's own
// fonts, icons and trick list for the panel to draw with. The running game
// converts nothing: the setup makes these from the player's disc once, and
// the panel only reads them. What they hold is the disc's data, so they are
// never committed or shipped.
//
// For each game (the same bytes the tooling's panel_map.py build writes):
//
//   Aggressive Inline   UI.pmap (the special trick list, the trick tables
//                       with their names from the string table, the fonts
//                       and the icons of the UI library); <LEVEL>.pmap for
//                       the eight career levels (MOVIE ... TUT): the map,
//                       the level's name and the goals of its script (the
//                       objective table and what the handlers count); with
//                       the Xbox release's scripts, a second goal set for
//                       each level whose Xbox script differs (the career
//                       scripts setting runs those); ZOO.pmap and
//                       ESTATE.pmap from the converted Zoo and Estate
//                       (a map only: their scripts are not the PS2's).
//   Dave Mirra BMX 2    UI.pmap (its fonts and trick list); <FOLDER>.pmap
//                       for its eight levels (the map, the level's name and
//                       its ProQuest goals from the executable's goal
//                       tables); Venice and Greenville from the DM2 Xbox
//                       extras' folder (their goals from its CAREER.BIN).
//   BMX XXX             UI.pmap (its fonts, halved, and trick list);
//                       <FOLDER>.pmap for each level on the disc (the map,
//                       the level's name and the challenges its OBJCTV.TXT
//                       lists).
//
// The map: the faces the rider collides with, seen from above, painted in
// their textures' mean colours (Aggressive Inline) or one ground colour (the
// two BMX games) and shaded by height and slope as a relief map; the level
// proper is the connected stretch of faces around the start points; thick
// walls are filled and height steps outlined; in Aggressive Inline what the
// skater cannot roll to is dimmed and the grind rails are drawn.

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    enum class PanelGame
    {
        AggressiveInline,
        DaveMirra2,
        BmxXxx,
    };

    // The folders besides the PS2 disc a game's panel files can use; each
    // may be null.
    struct PanelExtras
    {
        // Aggressive Inline: the converted Zoo's files (ZOO.GEO, .INS, .TXR,
        // .DAT, .BIN in one folder), the converted Estate's (TUT.*), and the
        // Xbox release's assets folder (the one holding levels/<L>/<L>.bin).
        const SourceFiles *zoo = nullptr;
        const SourceFiles *estate = nullptr;
        const SourceFiles *xboxScripts = nullptr;
        // Dave Mirra BMX 2: the DM2 Xbox extras' folder (collection/extras/
        // dm2_xbox.h: VENICE/, GREENVIL/, CAREER.BIN).
        const SourceFiles *dm2Xbox = nullptr;
        // Only these levels (folder names); empty: every one.
        std::vector<std::string> levels;
    };

    // `disc`: the game's PS2 disc folder, its paths from the disc's root
    // ("ASSETS/LEVELS/MOVIE/MOVIE.GEO", "SLUS_203.27": FolderFiles opened on
    // it with the marker "ASSETS", a folder). Writes <out>/UI.pmap and
    // <out>/<LEVEL>.pmap; `log` gets a line a file. False with *error in
    // plain words when a file is missing, does not read as expected, or
    // cannot be written (the files made before it stay).
    bool makePanelMaps(PanelGame game, const SourceFiles &disc, const PanelExtras &extras,
                       const std::filesystem::path &out, std::vector<std::string> &log, std::string *error);

    // One level's file for a custom map (collection/maps.h), made at its
    // boot from the files the boot reads: `files` the game's PS2 disc with
    // the map's files over it (LayeredFiles, paths from the disc's root),
    // `folder` the slot's level folder, `title` the map's name. As the
    // setup's: AI's map and the goals of the slot's career script, DM2's
    // map (a FREERIDE row runs no goals), BMX XXX's map and the challenges
    // of the slot's OBJCTV.TXT (the map's own). False with *error when the
    // level's files do not read.
    bool makeMapPanelFile(PanelGame game, const SourceFiles &files, const std::string &folder, const std::string &title, Bytes &out,
                          std::vector<std::string> &log, std::string *error);
}
