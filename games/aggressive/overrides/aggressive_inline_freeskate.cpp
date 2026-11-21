// Aggressive Inline (SLUS-20327): the Xbox release's free skate scripts for
// the Boardwalk and the Cannery (ZX_XBOX_FREESKATE=1; the app: Settings >
// Game > Xbox Free Skate, settings.ini [extras] xbox_free_skate).
//
// Free skate runs <LEVEL>T.BIN (func_1A4D78). The Xbox release's are much
// larger for these two levels (Boardwalk 391 -> 1095 code words, Cannery
// 698 -> 1782; the other levels' differ in a version float or a hidden
// prop): they open the Boardwalk's Mayan gates, Aztec door and rock door
// and the Cannery's eight doors (the ones the career opens, most once the
// level's key is found), run the Boardwalk's coaster cars, fan and boulder
// loop and the Cannery's cutter (it crushes the skater), and give the idle
// hints. The PS2 scripts leave those objects alone (the doors stay shut),
// and the Boardwalk's still has the demo version's "the other half of the
// level isn't available" boxes, whose trigger boxes the level does not have.
// Same script format (version 2.73 against 2.70, which nothing checks); every
// native, object and animation they use is on the PS2 disc. Served from the
// Xbox files in place of the PS2 disc's (single player only: split screen's
// MP\BRDWLK\BRDWLKT.BIN stays).

#include "aggressive_inline_freeskate.h"

#include "machine.h"
#include "runtime/game_files.h"
#include "runtime/xbox_disc.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aggressive_inline::freeskate
{
    namespace
    {
        namespace gf = zx::game_files;

        struct Script
        {
            const char *path; // as the game opens it (normalized)
            const char *xbox; // in the Xbox files
        };
        constexpr Script kScripts[] = {
            {"ASSETS/LEVELS/BRDWLK/BRDWLKT.BIN", "levels/BRDWLK/BRDWLKT.bin"},
            {"ASSETS/LEVELS/CANNERY/CANNERYT.BIN", "levels/CANNERY/CANNERYT.bin"},
        };

        struct Doors
        {
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files; // by path
        };
        Doors &doors() { return zx::session::state<Doors>(); }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            for (const Script &script : kScripts)
            {
                if (path != script.path)
                    continue;
                std::lock_guard<std::mutex> lock(doors().mutex);
                auto &made = doors().files[path];
                if (!made)
                {
                    auto bytes = std::make_shared<std::vector<uint8_t>>();
                    if (!ps2_xbox_disc::readFile(script.xbox, *bytes) || bytes->size() < 16u)
                        return nullptr;
                    std::cout << "[freeskate] " << path << ": the Xbox release's free skate script" << std::endl;
                    made = bytes;
                }
                return made;
            }
            return nullptr;
        }
    }

    void install(PS2Machine &)
    {
        const char *on = zx::boot::text("ZX_XBOX_FREESKATE");
        if (!on || on[0] != '1')
            return;
        if (!ps2_xbox_disc::init())
        {
            std::cout << "[freeskate] no Xbox files (" << ps2_xbox_disc::description() << ")" << std::endl;
            return;
        }
        size_t count = 0;
        for (const Script &script : kScripts)
            if (ps2_xbox_disc::exists(script.xbox))
            {
                const std::string path(script.path);
                gf::registerProvider(path.substr(0, path.rfind('/') + 1u), &provide, "freeskate");
                zx::session::onEnd([]() { gf::removeProviders("freeskate"); });
                ++count;
            }
        std::cout << "[freeskate] " << count << " Xbox free skate scripts (Boardwalk, Cannery)" << std::endl;
    }
}

namespace aggressive_inline
{
    void installXboxFreeSkate(PS2Machine &runtime)
    {
        freeskate::install(runtime);
    }
}
