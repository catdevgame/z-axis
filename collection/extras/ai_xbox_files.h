#pragma once

// The Xbox release's files Aggressive Inline reads as they are
// (runtime/xbox_files.h: ZX_XBOX_DIR), copied from the player's Xbox
// disc into <out>/assets/<path with the disc's case>, as tools/modding/xbox_files.py
// bundle copies them (tools/modding/xbox-files.txt's list, the same files), and the
// Zoo's level-select flythrough as the disc has it (movies/flythrus/zoo.bik,
// which the game's movie player plays for assets\Movies\Flythrus\Zoo.PSM).
//
// What needs another format is made elsewhere: the Zoo and the Ape
// (runtime/zaxis_zoo.h, zaxis_ape.h), the texture pack
// (collection/extras/xbox_texture_pack.h).

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    // The files' patterns (paths below media/assets, lower case, `*` and `?`).
    const std::vector<std::string> &aiXboxFilePatterns();

    // False with *error when a file cannot be read or written.
    bool makeAiXboxFiles(const SourceFiles &xbox, const std::filesystem::path &out, std::vector<std::string> &log,
                         std::string *error);
}
