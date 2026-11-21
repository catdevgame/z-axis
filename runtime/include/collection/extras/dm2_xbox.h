#pragma once

// Dave Mirra BMX 2's Xbox-only content in its PS2 release's formats, made
// from the player's Xbox disc (and PS2 disc) as tools/dm2_xbox.py makes it
// (the reference: the same bytes but the Retro movie, which the game plays
// as the Xbox disc has it). The game overrides serve them behind their
// settings (game_overrides/dave_mirra_xboxlevel.cpp, dave_mirra_xboxthemes.cpp,
// dave_mirra_gallery.cpp, dave_mirra_xcareer.cpp; ZX_DM2_XBOX_LEVELS_DIR).
// Under <out>:
//
//   <LEVEL>/<LEVEL>.{ZIT,GEO,INS,TXR,DAT}   VENICE and GREENVIL, the Xbox's
//       complete levels made into the PS2's formats (dm2_xbox_levels.cpp:
//       DXT textures as 8-bit BMPs within a texture budget, strips packed
//       into the PS2's 60-entry groups, the Xbox-only classes and material
//       types and the second levels of detail taken out)
//   <LEVEL>/<LEVEL>.XTP   the Xbox pictures, for the GPU renderer
//                         (runtime/ps2_texture_replace.h's pack)
//   <LEVEL>/INFO.TXT      the level select's text (the Xbox string table's)
//   PICTURES.ZIF          the level select's pictures (the Xbox UILIB's)
//   levels.txt            "<LEVEL> <display name>" a line
//   CAREER.BIN            the career's goals, gaps, rewards and texts, from
//                         default.xbe (dm2_xbox_career.cpp)
//   themes/...            with the PS2 disc: the park editor's Desert and
//                         Highway (dm2_xbox_themes.cpp)
//   demo/REPLAY.ZAL       with the PS2 disc: the attract demo's replays the
//                         PS2 disc lacks, from the Xbox's
//   movies/ASSETS/MOVIE/RETRO.BIK   the gallery's Retro movie, the Xbox's
//                         movie/retro.bik as it is

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    struct Dm2XboxOptions
    {
        // The largest texture side the levels keep (the PS2 levels' 128).
        int maxSide = 128;
        // A level's texels at most this many KB (0: none). DM2's texture
        // cache gives a frame's textures GS memory from block 5632 up and
        // checks neither the CLUTs above nor the end; at 2560 (the PS2
        // levels' size) the Xbox levels stay under both.
        int textureBudgetKb = 2560;
    };

    // `xbox`: the Xbox disc's media/assets; `defaultXbe` its default.xbe;
    // `ps2` (may be null: no themes, no demo replays) the PS2 disc's ASSETS.
    // False with *error in plain words when a file is missing or does not
    // read as the converter expects, or cannot be written.
    bool makeDm2XboxExtras(const SourceFiles *ps2, const SourceFiles &xbox, const Bytes &defaultXbe,
                           const std::filesystem::path &out, const Dm2XboxOptions &options,
                           std::vector<std::string> &log, std::string *error);
}
