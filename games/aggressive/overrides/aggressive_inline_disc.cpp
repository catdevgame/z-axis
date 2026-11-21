// Aggressive Inline (SLUS-20327): the game's disc files read straight from
// the host, by the engine's disc device's host form (zge/host/disc_device.cpp,
// where the device's contract is described; zge_disc.cpp, its switch). AI's
// own on top: the Xbox textures follow the files the game
// opens (zge_xbox_textures.h), the native asset readers every read's
// file, offset and guest destination (zge/native/disc_files.h), and the Xbox
// release's skaters are served in place of the PS2 files (below).
//
// [host] disc = ps2: none of this is installed (ZCDDVD over the emulated IOP
// and cdvdman, as on the console). ZX_MODS_DIR, ZX_TRACE_DISC: zge_disc.cpp.
#include "aggressive_inline_disc.h"
#include "sym/game_overrides/aggressive_inline_disc.cpp.h"
#include "overrides/zge_host.h"
#include "overrides/zge_xbox_textures.h"

#include "ee/game_id.h"

#include "runtime/boot_config.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/settings.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The readable game's native asset readers (engine/zge/native/disc_files.h):
// every read's file, offset and guest destination.
namespace zge::native
{
    void noteDiscRead(const std::string &path, uint64_t offset, uint32_t address, uint32_t bytes);
}

namespace game::readable
{
    bool runsInGame(uint32_t address); // ps2/ee/readable.h
}

namespace aggressive_inline
{
    std::shared_ptr<zx::game_files::GameFile> openGameFile(const std::string &path)
    {
        return zge_host::openGameFile(path);
    }

    // The Xbox release's skaters (made at build time by zx-native-assets
    // --xbox-models into <ZX_XBOX_PACK>/models/<PS2 path>: the PS2 model
    // file with the Xbox geometry, ready for the game's own loader). Served
    // in place of the PS2 files when Settings > Display > Xbox Skaters is on
    // (display.xbox_skaters, on by default; ZX_XBOX_MODELS=0 turns it off
    // without the app's row) and the readable packet builder runs; from the
    // next skater load.
    namespace xbox_models
    {
        constexpr const char *kKey = "display.xbox_skaters";
        struct XboxModels
        {
            bool envOn = true;
            std::filesystem::path folder;
            bool saidNeedsReadable = false;
            std::mutex mutex;
            std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> cache; // by normalized path
        };
        XboxModels &models() { return zx::session::state<XboxModels>(); }

        // The PS2's packet builder (buildPartPacket) builds a first packet
        // in a fixed 160 KB buffer that the larger Xbox skaters overrun; the
        // readable one (engine/zge/render/packet.h) sizes it to the part.
        constexpr uint32_t kPacketBuilder = sym::buildPartPacket;

        bool wanted()
        {
            const bool on = zx::settings::has(kKey) ? zx::settings::value(kKey) != 0 : models().envOn;
            if (!on)
                return false;
            if (!game::readable::runsInGame(kPacketBuilder))
            {
                if (!models().saidNeedsReadable)
                {
                    models().saidNeedsReadable = true;
                    std::cout << "[disc] Xbox skaters need the readable packet builder (buildPartPacket, "
                                 "ZX_READABLE): the PS2 skaters"
                              << std::endl;
                }
                return false;
            }
            return true;
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &normalized)
        {
            if (!wanted())
                return nullptr;
            XboxModels &m = models();
            std::lock_guard<std::mutex> lock(m.mutex);
            auto it = m.cache.find(normalized);
            if (it == m.cache.end())
            {
                std::shared_ptr<const std::vector<uint8_t>> bytes;
                std::ifstream in(models().folder / normalized, std::ios::binary);
                if (in)
                    bytes = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(in),
                                                                         std::istreambuf_iterator<char>());
                it = m.cache.emplace(normalized, bytes).first;
            }
            return it->second;
        }

        void install()
        {
            const char *pack = zx::boot::text("ZX_XBOX_PACK");
            std::error_code ec;
            if (!pack || !pack[0] || !std::filesystem::is_directory(std::filesystem::path(pack) / "models", ec))
                return;
            models().folder = std::filesystem::path(pack) / "models";
            const char *on = zx::boot::text("ZX_XBOX_MODELS");
            models().envOn = !on || on[0] != '0'; // on unless ZX_XBOX_MODELS=0
            zx::game_files::registerProvider("ASSETS/SKATERS/", &provide, "xbox-skaters");
            zx::session::onEnd([]() { zx::game_files::removeProviders("xbox-skaters"); });
            if (!zx::settings::has(kKey))
            {
                zx::settings::Setting row;
                row.key = kKey;
                row.label = "Xbox Skaters";
                row.help = "More detailed skaters, from the next level.";
                row.current = [] { return models().envOn ? 1 : 0; };
                row.apply = [](int value) { models().envOn = value != 0; };
                zx::session::addSetting(row);
            }
            std::cout << "[disc] Xbox skaters from " << models().folder.string() << " (" << (wanted() ? "on" : "off") << ")"
                      << std::endl;
        }
    }

    bool installNativeDisc(PS2Machine &runtime)
    {
        static constexpr zge_host::DiscAdditions kAdditions{
            .game = "Aggressive Inline",
            .id = game::Game::Ai,
            .fileOpened = &zge_host::xbox_textures::noteFileOpened,
            .fileRead = &zge::native::noteDiscRead,
        };
        if (!zge_host::installNativeDisc(runtime, kAdditions))
            return false;
        xbox_models::install();
        return true;
    }
}
