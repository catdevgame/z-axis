#pragma once

// The generator of ahead-of-time compiled VU1 microprograms (src/lib/vu/ps2_vu1_aot.h): C++ for one micro memory
// image, the image's pairs decoded by the interpreter as constants and functions running the interpreter's step on
// them, written against the VU1 core alone (runtime/ps2_vu1_core.h). Its callers: ps2xVu1Aot (the workshop's
// builds) and the app's first-run build of a game's code from the player's disc (collection/game_code.h), which
// compiles the sources into the game's module.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2x::vu1aot
{
    // What the generator writes for one image (vu1_aot_<hash>: a header, the dispatcher and chunk sources).
    struct Written
    {
        uint64_t hash = 0;                        // the image's (VU1Interpreter::codeHash)
        std::vector<std::filesystem::path> files; // every file, header first
        uint32_t blocks = 0;                      // basic blocks with straight-line code,
        uint32_t pairsInBlocks = 0;               // and the pairs they cover
    };

    // The generator's key: a hash of what it writes from (its source, the VU1 decoder and core: runtime/CMakeLists.txt,
    // PS2X_VU1_GENERATOR_KEY; empty in a program built without it, which writes no code). Code made by another
    // generator is not this one's.
    const char *generatorKey();

    // Writes the code of the 16 KiB micro memory image `image` into `dir`. False with the reason in `error`.
    bool writeProgram(const std::vector<uint8_t> &image, const std::filesystem::path &dir, Written &written,
                      std::string &error);
}
