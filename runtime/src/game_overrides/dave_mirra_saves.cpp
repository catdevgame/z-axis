// Dave Mirra Freestyle BMX 2 (SLUS-20159): the game save as a native file,
// without the memory card dialogs. The work is the game's host form of its
// memory card frame (game/dm2/host/game_save.cpp: a game's save or load done
// at once on the file, the frame's bookkeeping kept, the request told after
// half a second), over game/host/save_files.h's WholeSave; this is the
// switch and the game's part: where the file comes from.
//
// File: <saves>/<profile>.sav, the bytes of BMX2SAVE.DAT (game/dm2/game/
// memory_card.h GameSaveFile: magic, size, the saved game and records),
// written atomically with a .bak; <saves> = PS2X_SAVES_DIR (the app's Saves
// folder), else a sibling of the slot 1 card folder (<card>-saves);
// <profile> = PS2X_SAVE_PROFILE (the app's [saves] profile: several careers
// side by side, as Aggressive Inline's) or "Career". On the first load
// BMX2SAVE.DAT from the card folders (slot 1, then slot 2) is copied in
// once. The app's PCSX2 import writes the same bytes to the same file. Saves
// of before profiles (Game.sav, XboxLevels.career) are copied to the Career
// profile's at the first run that has none of its own; the originals stay as
// they were, so an older app still finds them.
// PS2X_SAVES=card: none of this (the card dialogs, as on the console).
// PS2X_TRACE_SAVES=1 logs each operation.
//
// A park from a host file (PS2X_DM2_PARK_FILE=<file>, the boot's: the
// collection's Custom Freeplay gives its map's park.dat): the park loads
// read that file's bytes (one park stream, kParkBytes at most, the rest
// zero) into the request's park buffer, with no card frame (the host form's
// part: game/dm2/host/game_save.cpp). Without the name the park loads stay
// the game's (the card HLE).
#include "dave_mirra.h"
#include "zge_saves.h"

#include "ps2_runtime.h"
#include "runtime/ps2_save_store.h"
#include "Stubs/MemoryCard.h"
#include "runtime/ps2_boot_config.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace game::host
{
    // Every host form of a system over the game's functions (src/game/ee/host_form.h).
    bool install(PS2Runtime &runtime, Game game, const char *system);
}

namespace dave_mirra::saves
{
    namespace store = ps2x::saves;
    namespace files = ::game::host::saves;

    constexpr uint32_t kMagic = 0x2A81511Cu; // game/dm2/game/memory_card.h kGameSaveMagic
    constexpr uint32_t kSaveSize = 0x6080u;  // sizeof(GameSaveFile)
    constexpr const char *kCardFolder = "BASLUS-20159BMX2GAME";
    constexpr const char *kCardFile = "BMX2SAVE.DAT";
    constexpr const char *kDefaultProfile = "Career";
    constexpr const char *kGameSuffix = ".sav";

    bool validSave(const store::Bytes &data)
    {
        uint32_t magic = 0, size = 0;
        if (data.size() != kSaveSize)
            return false;
        std::memcpy(&magic, data.data(), 4);
        std::memcpy(&size, data.data() + 4, 4);
        return magic == kMagic && size == kSaveSize;
    }

    std::filesystem::path profileFile(const std::string &suffix)
    {
        return files::folder() / (files::profile() + suffix);
    }

    // The saves of before profiles, once: copied to the Career profile's
    // when it has none (a file and its .bak each). The originals are left
    // as they are.
    void adoptUnprofiledSaves()
    {
        if (files::profile() != kDefaultProfile)
            return;
        const std::filesystem::path pairs[][2] = {
            {files::folder() / "Game.sav", profileFile(kGameSuffix)},
            {files::folder() / "XboxLevels.career", profileFile(kXboxLevelsSuffix)},
        };
        for (const auto &pair : pairs)
        {
            std::error_code ec;
            if (std::filesystem::exists(pair[1], ec) || std::filesystem::exists(store::backupPath(pair[1]), ec))
                continue;
            const std::filesystem::path olds[2] = {pair[0], store::backupPath(pair[0])};
            const std::filesystem::path news[2] = {pair[1], store::backupPath(pair[1])};
            for (int i = 0; i < 2; ++i)
                if (std::filesystem::exists(olds[i], ec))
                {
                    std::filesystem::copy_file(olds[i], news[i], std::filesystem::copy_options::none, ec);
                    std::cout << "[dave-mirra] saves: " << olds[i].filename().string() << " copied to "
                              << news[i].filename().string() << (ec ? " (could not copy: " + ec.message() + ")" : "")
                              << std::endl;
                }
        }
    }

    // At the first load: the card's game save, if there is no native one yet.
    void importFromCard()
    {
        adoptUnprofiledSaves();
        const std::filesystem::path target = files::saveFile();
        std::error_code ec;
        if (std::filesystem::exists(target, ec) || std::filesystem::exists(store::backupPath(target), ec))
            return;
        for (int port = 0; port < 2; ++port)
        {
            const std::filesystem::path from = ps2_stubs::memoryCardFolder(port) / kCardFolder / kCardFile;
            store::Bytes data;
            if (!store::readFile(from, data) || !validSave(data))
                continue;
            if (store::writeFileAtomic(target, data.data(), data.size()))
                std::cout << "[dave-mirra] saves: imported " << from.string() << " -> " << target.string() << std::endl;
            return;
        }
    }

}

namespace dave_mirra
{
    bool installNativeSaves(PS2Runtime &runtime)
    {
        using namespace saves;
        if (const char *mode = ps2x::boot::text("PS2X_SAVES"); mode && std::string(mode) == "card")
        {
            std::cout << "[dave-mirra] saves: memory card dialogs (PS2X_SAVES=card)" << std::endl;
            return false;
        }
        files::configureWholeSave({.bytes = kSaveSize,
                                   .valid = &validSave,
                                   .beforeFirstLoad = &importFromCard,
                                   .log = "[dave-mirra] saves",
                                   .trace = std::getenv("PS2X_TRACE_SAVES") != nullptr});
        if (!::game::host::install(runtime, game::Game::Dm2, "saves"))
        {
            std::cerr << "[dave-mirra] saves: hooks not installed" << std::endl;
            return false;
        }
        adoptUnprofiledSaves();
        std::cout << "[dave-mirra] saves: game save in " << files::saveFile().string() << " (PS2X_SAVES=card for the card)" << std::endl;
        if (const char *park = ps2x::boot::text("PS2X_DM2_PARK_FILE"); park && *park)
        {
            files::configureHostPark(park);
            std::cout << "[dave-mirra] saves: park loads read " << park << std::endl;
        }
        return true;
    }
}
