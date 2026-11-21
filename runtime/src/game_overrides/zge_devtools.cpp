// The developer menu's debug display, shared by DM2 and BMX XXX
// (zge_devtools.h).

#include "zge_devtools.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zip_archive.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"

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
        constexpr int kKeyF1 = 290;

        struct State
        {
            DebugDisplay display{};
            std::atomic<bool> request{false};
            bool f1Latched = false;
        };
        State &state() { return zx::session::state<State>(); }

        void onVsync(uint8_t *rdram, uint64_t)
        {
            State &d = state();
            const bool f1 = PSPadBackend::keyPressedWithin(kKeyF1, 0.25);
            if (f1 && !d.f1Latched)
                d.request = true;
            d.f1Latched = f1;
            if (!d.request.exchange(false))
                return;
            uint8_t *shown = getMemPtr(rdram, d.display.flag);
            if (!shown)
                return;
            *shown = *shown != 0u ? 0u : d.display.on;
            std::cout << "[" << d.display.game << "] debug display " << (*shown ? "on" : "off") << std::endl;
        }
    }

    void installDebugDisplay(PS2Runtime &runtime, const DebugDisplay &display)
    {
        if (const char *on = zx::boot::text("ZX_DEV_MENU"); !on || on[0] != '1')
            return;
        state().display = display;
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
        std::cout << "[" << display.game << "] developer menu on: F1 turns the game's debug display on and off" << std::endl;
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
