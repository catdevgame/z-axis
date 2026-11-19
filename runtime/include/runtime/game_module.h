#pragma once

// Game modules: each game's recompiled code (its generated functions and their table), built apart from the runtime
// and reached through one table each way (game_module/abi.h). A module is linked into the program
// (PS2X_GAME_MODULES=static, the workshop's builds) or loaded as a shared library (PS2X_GAME_MODULES=shared, and the
// app's code built from the player's disc). Either way it registers its table as the runtime's generated code:
// AI's (id "") as the default, the others by their ELF (ps2_generated, ps2_runtime.h).

#include "game_module/abi.h"

#include <string>

namespace ps2x::game_module
{
    // A module linked into this program: the build's game_modules_linked.cpp adds each one's entry before main.
    struct Linked
    {
        Linked(const char *id, ZxGameModuleEntry entry); // id: the module's, for its messages
    };

    // Registers the code of the module whose entry this is (a linked one's, or a loaded one's): false, with the
    // reason in `error`, when it was built for another boundary.
    bool registerEntry(ZxGameModuleEntry entry, const std::string &from, std::string &error);

    // Loads a module built on its own (a shared library exporting zx_game_module) and registers its code. False,
    // with the reason in `error`, when it cannot be loaded or was built for another boundary.
    bool load(const std::string &path, std::string &error);

    // The linked modules, then every module in the game code folder (PS2X_GAME_CODE_DIR, else the build's): once,
    // before the runtime first chooses a function table (ps2_runtime.cpp).
    void ensureLoaded();

    // Whether a module registered so far (linked, in the game code folder, or loaded) has the code of this ELF
    // (its file name, case-insensitive).
    bool hasCodeFor(const std::string &elfName);

    // The table the runtime gives every module.
    const ZxRuntimeApi &runtimeApi();
}
