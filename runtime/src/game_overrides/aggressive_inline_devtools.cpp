// Aggressive Inline (SLUS-20327): developer and mod tools. See
// docs/DEVTOOLS.md in the bring-up workspace.
//
// Tuning table. 20 categories of {entries, count, name[32]} (0x28 bytes) at
// 0x24AD28 (General, FX, Camera, Physics, Skater, Control, Scoring: ...,
// Grind/Walls, Action, BoobTech, Competitor, Vibrate, Mem Check; the last
// "None" ends it), each entry {name[32], value, max, min, step} (0x30
// bytes), 405 entries from 0x246138 (BoobTech and Mem Check are the same
// three). The game reads values as lw categories+0x28k, lwc1 off(base),
// mostly in per-frame code; only the level camera setup FUN_001901C0 writes
// one (Far Clip, from the level or the split screen entries). Mods/tuning.ini
// (ZX_TUNING_INI: another file) sets entries before the game starts; Far
// Clip, which the game rewrites, is set again at every vsync.
//
// Debug menu. Game mode 6 (the mode table 0x24B810: enter 0x1BC470, exit
// 0x1BC5B0, update 0x1BC518) plays InGameUI.ShowDebug (MultiplayerUI's in
// split screen): Resume, Art Viewer, Tuning (the table's categories and
// entries, left/right change a value: inlineTuneUp/Down), Respawn, Toggle
// Debug; inlineDebugMenuSelect (0x1BC600) acts on the choice. Nothing in
// the retail game enters mode 6. With ZX_DEV_MENU=1, F1 in a level (game
// mode 2) or `set dev.debug_menu 1` (ZX_CONTROL) enters it from the
// level's update (0x1ACA68), as func_001A70F0(6).
//
// Language. The game loads one string table at start, "assets\string.eus"
// (0x268788, FUN_001BC9F0 -> 0x1483B8); the disc also has STRING.EUK, .FRE
// and .GER with the same 236 keys. ZX_LANGUAGE=uk|french|german changes
// the name's extension before the game runs.

#include "aggressive_inline_devtools.h"
#include "aggressive_inline_language.h"
#include "zge_devtools.h"

#include "ps2_runtime.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_tuning_ini.h"
#include "runtime/ps2_zip_archive.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_session.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace aggressive_inline
{
    namespace
    {
        namespace tn = zx::tuning;

        constexpr uint32_t kCategories = 0x0024AD28u;
        constexpr uint32_t kCategorySize = 0x28u;
        constexpr uint32_t kMaxCategories = 32u;
        constexpr uint32_t kEntrySize = 0x30u;
        constexpr uint32_t kEntryValue = 0x20u;
        constexpr uint32_t kTableBegin = 0x00246138u;
        constexpr uint32_t kTableEnd = 0x0024AD48u;
        // Written by the game itself (FUN_001901C0, at every level start).
        constexpr uint32_t kFarClip = 0x00246708u;

        constexpr uint32_t kGameMode = 0x00276FE4u;
        constexpr uint32_t kModePlay = 2u;
        constexpr uint32_t kModeDebugMenu = 6u;
        constexpr uint32_t kSetGameMode = 0x001A70F0u;
        constexpr uint32_t kPlayUpdate = 0x001ACA68u;
        constexpr int kKeyF1 = 290;

        constexpr uint32_t kStringTableName = 0x00268788u;
        constexpr const char *kStringTableUs = "assets\\string.eus";

        struct Pinned
        {
            uint32_t address;
            float value;
        };
        struct DevTools
        {
            PS2Runtime::RecompiledFunction playUpdate = nullptr;
            PS2Runtime::RecompiledFunction setGameMode = nullptr;
            std::atomic<bool> debugMenuRequest{false};
            bool f1Latched = false;

            std::vector<Pinned> pinned;
        };
        DevTools &devTools() { return zx::session::state<DevTools>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        std::string readName(const uint8_t *rdram, uint32_t address)
        {
            std::string out;
            for (uint32_t i = 0; i < 32u; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + i);
                if (!p || *p == 0u)
                    break;
                out.push_back(static_cast<char>(*p));
            }
            return out;
        }

        // The table as the ELF has it (categories in the game's order).
        std::vector<tn::Entry> readTable(const uint8_t *rdram)
        {
            std::vector<tn::Entry> entries;
            for (uint32_t k = 0; k < kMaxCategories; ++k)
            {
                const uint32_t category = kCategories + k * kCategorySize;
                const uint32_t base = readWord(rdram, category);
                const uint32_t count = readWord(rdram, category + 4u);
                if (base < kTableBegin || count == 0u || base + count * kEntrySize > kTableEnd)
                    break;
                const std::string name = readName(rdram, category + 8u);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t e = base + i * kEntrySize;
                    tn::Entry entry;
                    entry.category = name;
                    entry.name = readName(rdram, e);
                    entry.address = e;
                    entry.value = readFloat(rdram, e + kEntryValue);
                    entry.maxValue = readFloat(rdram, e + kEntryValue + 4u);
                    entry.minValue = readFloat(rdram, e + kEntryValue + 8u);
                    entry.step = readFloat(rdram, e + kEntryValue + 12u);
                    entries.push_back(std::move(entry));
                }
            }
            return entries;
        }

        // (Mods/tuning.ini: zge_devtools.h tuningFile, shared with BMX XXX)
        using zge_host::devtools::hasTuningFile;
        using zge_host::devtools::readTuningText;
        using zge_host::devtools::tuningFile;

        bool envOn(const char *name)
        {
            const char *value = zx::boot::text(name);
            return value && value[0] == '1';
        }

        // ---- language -------------------------------------------------------

        void applyLanguage(uint8_t *rdram)
        {
            const char *language = zx::boot::text("ZX_LANGUAGE");
            if (!language || !*language)
                return;
            // French, German and Spanish from the Xbox release replace the
            // whole text (aggressive_inline_language.cpp).
            if (aggressive_inline::language::active())
                return;
            const std::string name = tn::folded(language);
            const char *extension = nullptr;
            if (name == "us" || name == "eus" || name == "english" || name == "spanish")
                return;
            if (name == "uk" || name == "euk")
                extension = "euk";
            else if (name == "french" || name == "fre" || name == "fr")
                extension = "fre";
            else if (name == "german" || name == "ger" || name == "de")
                extension = "ger";
            if (!extension)
            {
                std::cerr << "[aggressive-inline] unknown ZX_LANGUAGE " << language << " (english, french, german, spanish, uk)" << std::endl;
                return;
            }
            const size_t length = std::strlen(kStringTableUs);
            for (size_t i = 0; i < length; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, kStringTableName + static_cast<uint32_t>(i));
                if (!p || *p != static_cast<uint8_t>(kStringTableUs[i]))
                {
                    std::cerr << "[aggressive-inline] string table name not found, language not changed" << std::endl;
                    return;
                }
            }
            const uint32_t dot = kStringTableName + static_cast<uint32_t>(length) - 3u;
            for (uint32_t i = 0; i < 3u; ++i)
                if (uint8_t *p = getMemPtr(rdram, dot + i))
                    *p = static_cast<uint8_t>(extension[i]);
            std::cout << "[aggressive-inline] language: assets\\string." << extension << std::endl;
        }

        // ---- tuning.ini -----------------------------------------------------

        void applyTuning(uint8_t *rdram, const std::vector<tn::Entry> &entries)
        {
            const std::filesystem::path file = tuningFile();
            if (file.empty())
                return;
            std::string text;
            if (!readTuningText(file, text))
                return;
            std::vector<std::string> warnings;
            const std::vector<tn::Override> overrides = tn::match(tn::parse(text, &warnings), entries, &warnings);
            for (const std::string &warning : warnings)
                std::cerr << "[tuning] " << file.filename().string() << " " << warning << std::endl;
            for (const tn::Override &o : overrides)
            {
                const uint32_t address = o.entry->address + kEntryValue;
                const float before = readFloat(rdram, address);
                writeFloat(rdram, address, o.value);
                const bool outside = o.value < o.entry->minValue || o.value > o.entry->maxValue;
                std::cout << "[tuning] " << o.entry->category << " / " << o.entry->name << " = " << o.value << " (was "
                          << before << (outside ? ", outside the table's range" : "") << ")" << std::endl;
                if (o.entry->address == kFarClip)
                {
                    bool found = false;
                    for (Pinned &pin : devTools().pinned)
                        if (pin.address == address)
                        {
                            pin.value = o.value;
                            found = true;
                        }
                    if (!found)
                        devTools().pinned.push_back({address, o.value});
                }
            }
            std::cout << "[tuning] " << overrides.size() << " entr" << (overrides.size() == 1 ? "y" : "ies") << " from "
                      << file.string() << std::endl;
        }

        void writeReference(const std::vector<tn::Entry> &entries)
        {
            // (beside the tuning.ini in use; one in a zip: in the first folder)
            const std::filesystem::path reference = zge_host::devtools::tuningReferenceFile();
            if (reference.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(reference.parent_path(), ec);
            std::ofstream out(reference, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out << tn::reference(entries,
                                 "; Aggressive Inline: the developer tuning table, every entry with the game's own\n"
                                 "; value and range. Copy lines to tuning.ini in this folder (without the ';') and\n"
                                 "; change the value; they are set when the game starts. Changing them changes\n"
                                 "; how the game plays. Rewritten at every start with the developer menu on.\n");
        }

        // ---- the debug menu -------------------------------------------------

        // The level's per-frame update (game mode 2): enters the debug menu
        // instead of one update when asked, like a tail call of
        // func_001A70F0(6) (the mode's exit, then the debug menu's enter).
        void playUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool f1 = PSPadBackend::keyPressedWithin(kKeyF1, 0.25);
            if (f1 && !devTools().f1Latched)
                devTools().debugMenuRequest = true;
            devTools().f1Latched = f1;
            if (devTools().debugMenuRequest.exchange(false) && readWord(rdram, kGameMode) == kModePlay)
            {
                std::cout << "[aggressive-inline] debug menu" << std::endl;
                SET_GPR_U32(ctx, 4, kModeDebugMenu);
                devTools().setGameMode(rdram, ctx, runtime);
                return;
            }
            devTools().playUpdate(rdram, ctx, runtime);
        }

        void installDebugMenu(PS2Runtime &runtime)
        {
            devTools().playUpdate = runtime.lookupFunction(kPlayUpdate);
            devTools().setGameMode = runtime.lookupFunction(kSetGameMode);
            if (!devTools().playUpdate || !devTools().setGameMode || !runtime.replaceFunction(kPlayUpdate, &playUpdateHook))
            {
                std::cerr << "[aggressive-inline] debug menu hook not installed" << std::endl;
                return;
            }
            zx::settings::Setting open;
            open.key = "dev.debug_menu";
            open.label = "Debug Menu";
            open.help = "Opens the game's debug menu in a level.";
            open.current = []() { return 0; };
            open.apply = [](int value)
            {
                if (value != 0)
                    devTools().debugMenuRequest = true;
            };
            zx::session::addSetting(open);
            std::cout << "[aggressive-inline] developer menu on: F1 in a level opens the game's debug menu" << std::endl;
        }
    }

    uint32_t tuningValueAddress(const uint8_t *rdram, const std::string &category, const std::string &name)
    {
        for (const tn::Entry &entry : readTable(rdram))
            if (entry.category == category && entry.name == name)
                return entry.address + kEntryValue;
        return 0u;
    }

    void installDevTools(PS2Runtime &runtime)
    {
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        applyLanguage(rdram);
        const bool devMenu = envOn("ZX_DEV_MENU");
        const std::filesystem::path tuning = tuningFile();
        const bool hasTuning = !tuning.empty() && hasTuningFile(tuning);
        if (!devMenu && !hasTuning)
            return;
        const std::vector<tn::Entry> entries = readTable(rdram);
        if (entries.empty())
        {
            std::cerr << "[aggressive-inline] tuning table not found" << std::endl;
            return;
        }
        if (devMenu)
        {
            writeReference(entries);
            installDebugMenu(runtime);
        }
        if (hasTuning)
            applyTuning(rdram, entries);
        if (!devTools().pinned.empty())
            runtime.addVSyncHook([](uint8_t *memory, uint64_t)
                                 {
                                     for (const Pinned &pin : devTools().pinned)
                                         if (readFloat(memory, pin.address) != pin.value)
                                             writeFloat(memory, pin.address, pin.value);
                                 });
    }
}
