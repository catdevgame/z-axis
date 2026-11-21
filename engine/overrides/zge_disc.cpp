// The Z-Axis engine's "dvd0" file device on the host's files, for every game
// on the engine (zge_host.h): the switch and each game's additions. The
// device itself is engine code, one body for the three games at each one's
// functions and data: zge/host/disc_device.cpp (the host form, over the
// files of engine/host/disc_files.h), installed over the CD/DVD client
// (zge/assets/dvd_device.cpp, the PS2 form) by its own switch, whatever
// ZX_READABLE says. ZCDDVD.IRX is then not loaded at all.
//
// The switch: [host] disc in settings.ini, ZX_DISC for a run: native (the
// default) or ps2 (the PS2 form over the emulated IOP and cdvdman, as on the
// console). ZX_MODS_DIR: the mod
// folders in order (the app sets the game's Mods folder and each mod in it,
// collection/mods.h; game_files.h reads them). ZX_TRACE_DISC=1 logs every
// open (path, size, where it came from), =2 also every read and seek.
#include "zge_host.h"

#include "ee/game_id.h"
#include "host/disc_files.h"

#include "machine.h"
#include "runtime/boot_config.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace game::host
{
    // Every host form of a system over the game's functions (ps2/ee/host_form.h).
    bool install(PS2Machine &runtime, Game game, const char *system);
}

namespace zge_host
{
    namespace
    {
        // [host] disc: true for the PS2 form.
        bool discOnPs2()
        {
            const char *disc = zx::boot::text("ZX_DISC");
            return disc && std::string(disc) == "ps2";
        }
    }

    std::shared_ptr<zx::game_files::GameFile> openGameFile(const std::string &path)
    {
        return game::host::disc::openGameFile(path);
    }

    bool installNativeDisc(PS2Machine &runtime, const DiscAdditions &additions)
    {
        if (discOnPs2())
        {
            std::cout << "[disc] ZCDDVD over the IOP ([host] disc = ps2)" << std::endl;
            return false;
        }
        int trace = 0;
        if (const char *level = std::getenv("ZX_TRACE_DISC"))
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
