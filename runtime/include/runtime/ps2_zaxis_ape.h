#pragma once

// Aggressive Inline's Ape (the Xbox release's secret skater) made into the
// PS2's model format at build time, from the user's Xbox files: the
// packaging step runs ps2xXboxConvert (tools/xbox_convert.cpp), which writes
// what build() makes next to the Zoo's (runtime/ps2_zaxis_zoo.h); the game
// override (game_overrides/aggressive_inline_ape.cpp) only serves the file.
//
//   ASSETS/SKATERS/GORILLA/GOR.ZIT   the Xbox GOR.ZIT made into the PS2's
//                                    (runtime/ps2_zaxis_xbox.h convertModel:
//                                    textures as 8-bit BMPs at most 256 a side
//                                    as the PS2 skaters', strips, materials),
//                                    its materials' first colour as every
//                                    other skater's (recolour)
//   textures/ASSETS/SKATERS/GORILLA/GOR.ZIT.xtp
//                                    its Xbox originals (512) for the GPU
//                                    renderer, keyed by the converted textures

#include "runtime/ps2_zaxis_zoo.h"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace ps2x::zaxis::ape
{
    constexpr const char kXboxModel[] = "skaters/Gorilla/GOR.ZIT"; // Xbox path (media/assets)
    constexpr const char kModelPath[] = "ASSETS/SKATERS/GORILLA/GOR.ZIT";
    constexpr int kMaxTextureSide = 256; // the PS2 skaters'

    // The fur's materials have 00 00 80 FF as their first colour; every
    // other skater's material on both discs has E5 E5 E5 FF there (their
    // other colours and the Ape's are alike: 33 33 33 FF twice); made as
    // theirs. Neither game lights skinned models with these colours (changed
    // live in RAM on the PC and in xemu: no change); the Ape's look on the
    // Xbox comes from the Xbox's character lights (Character Lighting,
    // aggressive_inline_native.cpp). Returns the materials changed.
    size_t recolour(std::vector<uint8_t> &zit);

    // Both files above into out.files. False (with a reason) when the Xbox
    // model is missing or does not convert.
    bool build(const zoo::Reader &xbox, zoo::Made &out, std::ostream &log, std::string *error = nullptr);
}
