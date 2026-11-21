#pragma once

#include <memory>
#include <string>

class PS2Runtime;

namespace zx::game_files
{
    class GameFile;
}

namespace aggressive_inline
{
    // The game's dvd0 file device on host files (unless [host] disc = ps2);
    // true when installed (ZCDDVD.IRX is then not needed).
    bool installNativeDisc(PS2Runtime &runtime);

    // A game file as the game's own opens see it (mods folder, providers,
    // the disc, patches); null if there is none.
    std::shared_ptr<zx::game_files::GameFile> openGameFile(const std::string &path);
}
