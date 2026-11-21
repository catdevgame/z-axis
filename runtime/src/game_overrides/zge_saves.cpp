// Native save files for the ZGE4 games (zge_saves.h): the switch.
//
// ZX_SAVES: native (the default) or card (the memory card's jobs and
// dialogs on the HLE card folders, as on the console; the reference tools
// compare against PCSX2, whose timing includes the card dialogs).
// ZX_TRACE_SAVES=1: the game's own check of a save's buffer (its
// traceSave).

#include "zge_saves.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"

#include <iostream>
#include <string>

namespace game::host
{
    // Every host form of a system over the game's functions (src/game/ee/host_form.h).
    bool install(PS2Runtime &runtime, Game game, const char *system);
}

namespace zge_host::saves
{
    bool install(PS2Runtime &runtime, ::game::Game id, const ::game::host::saves::Game &game)
    {
        const char *mode = zx::boot::text("ZX_SAVES");
        if (mode && std::string(mode) == "card")
        {
            std::cout << "[saves] memory card (ZX_SAVES=card)" << std::endl;
            return false;
        }
        configure(game);
        if (!::game::host::install(runtime, id, "saves"))
        {
            std::cerr << "[saves] native save hooks not installed" << std::endl;
            return false;
        }
        std::cout << "[saves] native save files (ZX_SAVES=card for the memory card)" << std::endl;
        return true;
    }
}
