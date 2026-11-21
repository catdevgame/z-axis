#pragma once

// Aggressive Inline's extras that the runtime's own converters make from the
// player's discs (the same code tools/zx-xbox-convert and zx-gc-convert run):
//
// - the Xbox release's Zoo (level, split screen, ambient NPCs) and Ape, made
//   into the PS2's formats with the Xbox originals of their textures
//   (runtime/ps2_zaxis_zoo.h, ps2_zaxis_ape.h): <out>/..., served from
//   ZX_XBOX_CONVERTED (Zoo, Ape);
// - the GameCube disc's TUT/Estate, an earlier Tutorial, made into the files
//   the game loads (runtime/ps2_zaxis_estate.h): <out>/..., served from
//   ZX_ESTATE_DIR (Estate Level).

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    // `xbox`: the Xbox disc's media/assets; `ps2Disc`: the installed PS2 disc
    // folder (its split-screen props). False with *error in plain words.
    bool makeAiZooAndApe(const SourceFiles &xbox, const std::filesystem::path &ps2Disc, const std::filesystem::path &out,
                         std::vector<std::string> &log, std::string *error);

    // `gc`: the GameCube disc's assets.
    bool makeAiEstate(const SourceFiles &gc, const std::filesystem::path &out, std::vector<std::string> &log,
                      std::string *error);
}
