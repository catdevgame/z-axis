// The Z-Axis engine's "dvd0" file device on the host's files, for every game
// on the engine (zge_host.h): the switch and each game's additions. The
// device itself is engine code, one body for the three games at each one's
// functions and data: zge/host/disc_device.cpp (the host form, over the
// files of game/host/disc_files.h), installed over the CD/DVD client
// (zge/assets/dvd_device.cpp, the PS2 form) by its own switch, whatever
// PS2X_READABLE says. ZCDDVD.IRX is then not loaded at all.
//
// The switch: [host] disc in settings.ini, ZAXIS_DISC for a run: native (the
// default) or ps2 (the PS2 form over the emulated IOP and cdvdman, as on the
// console); PS2X_DISC=cdvd, the old name, is ps2. PS2X_MODS_DIR: the mod
// folders in order (the app sets the game's Mods folder and each mod in it,
// collection/mods.h; ps2_game_files.h reads them). PS2X_TRACE_DISC=1 logs every
// open (path, size, where it came from), =2 also every read and seek.
#include "zge_host.h"

#include "../game/ee/game_id.h"
#include "../game/host/disc_files.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace game::host
{
    // Every host form of a system over the game's functions (src/game/ee/host_form.h).
    bool install(PS2Runtime &runtime, Game game, const char *system);
}

namespace zge_host
{
    namespace
    {
        // [host] disc: true for the PS2 form.
        bool discOnPs2()
        {
            if (const char *disc = ps2x::boot::text("ZAXIS_DISC"); disc && *disc)
                return std::string(disc) == "ps2";
            const char *old = std::getenv("PS2X_DISC");
            return old && std::string(old) == "cdvd";
        }
    }

    std::shared_ptr<ps2x::game_files::GameFile> openGameFile(const std::string &path)
    {
        return game::host::disc::openGameFile(path);
    }

    bool installNativeDisc(PS2Runtime &runtime, const DiscAdditions &additions)
    {
        if (discOnPs2())
        {
            std::cout << "[disc] ZCDDVD over the IOP ([host] disc = ps2)" << std::endl;
            return false;
        }
        int trace = 0;
        if (const char *level = std::getenv("PS2X_TRACE_DISC"))
            trace = std::atoi(level);
        game::host::disc::configure(trace, {additions.fileOpened, additions.fileRead});
        if (!game::host::install(runtime, additions.id, "disc"))
        {
            std::cerr << "[disc] " << additions.game << ": the host's disc device is not installed" << std::endl;
            return false;
        }
        std::cout << "[disc] native disc files ([host] disc = ps2 for ZCDDVD over the IOP)" << std::endl;
        return true;
    }
}
