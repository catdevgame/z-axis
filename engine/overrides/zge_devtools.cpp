// The developer menu's debug display, shared by DM2 and BMX XXX
// (zge_devtools.h).

#include "zge_devtools.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/game_files.h"
#include "runtime/zip_archive.h"
#include "runtime/pad.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace zge_host::devtools
{
    namespace
    {
        struct State
        {
            DebugDisplay display{};
            std::atomic<bool> request{false};
            uint32_t developerMenuPresses = 0; // the developer_menu key's presses as last seen
        };
        State &state() { return zx::session::state<State>(); }

        void onVsync(uint8_t *rdram, uint64_t)
        {
            State &d = state();
            const uint32_t presses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeyDeveloperMenu);
            if (presses != d.developerMenuPresses)
            {
                d.developerMenuPresses = presses;
                d.request = true;
            }
            if (!d.request.exchange(false))
                return;
            uint8_t *shown = getMemPtr(rdram, d.display.flag);
            if (!shown)
                return;
            *shown = *shown != 0u ? 0u : d.display.on;
            std::cout << "[" << d.display.game << "] debug display " << (*shown ? "on" : "off") << std::endl;
        }
    }

    void installDebugDisplay(PS2Machine &runtime, const DebugDisplay &display)
    {
        if (const char *on = zx::boot::text("ZX_DEV_MENU"); !on || on[0] != '1')
            return;
        state().display = display;
        state().developerMenuPresses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeyDeveloperMenu);
        zx::settings::Setting open;
        open.key = "dev.debug_menu";
        open.label = "Debug Display";
        open.help = display.help;
        open.current = []() { return 0; };
        open.apply = [](int value)
        {
            if (value != 0)
                state().request = true;
        };
        zx::session::addSetting(open);
        runtime.addVSyncHook(&onVsync);
        std::cout << "[" << display.game << "] developer menu on: the developer_menu key turns the game's debug display on and off" << std::endl;
    }

    bool readTuningText(const std::filesystem::path &file, std::string &text)
    {
        if (const zx::game_files::ZipPath zip = zx::game_files::zipPathOf(file); zip.zip)
        {
            std::vector<uint8_t> bytes;
            if (!zip.zip->read(zip.inside, bytes))
                return false;
            text.assign(bytes.begin(), bytes.end());
            return true;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
        return true;
    }

    bool hasTuningFile(const std::filesystem::path &file)
    {
        if (const zx::game_files::ZipPath zip = zx::game_files::zipPathOf(file); zip.zip)
            return zip.zip->hasFile(zip.inside);
        std::error_code ec;
        return std::filesystem::is_regular_file(file, ec);
    }

    std::filesystem::path tuningFile()
    {
        if (const char *file = std::getenv("ZX_TUNING_INI"); file && *file)
            return file;
        if (const char *text = zx::boot::text("ZX_MODS_DIR"); text && *text)
        {
            const std::vector<std::filesystem::path> mods = zx::game_files::modsRoots(text);
            for (auto root = mods.rbegin(); root != mods.rend(); ++root)
                if (hasTuningFile(*root / "tuning.ini"))
                    return *root / "tuning.ini";
            if (!mods.empty())
                return mods.front() / "tuning.ini";
        }
        return {};
    }

    std::filesystem::path tuningReferenceFile()
    {
        std::filesystem::path file = tuningFile();
        if (file.empty())
            return {};
        if (zx::game_files::zipPathOf(file).zip)
            if (const char *text = zx::boot::text("ZX_MODS_DIR"); text && *text)
                file = zx::game_files::modsRoots(text).front() / "tuning.ini";
        return file.parent_path() / "tuning-reference.ini";
    }
}
