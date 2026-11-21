// The developer tuning table of DM2 and BMX XXX (zge_tuning.h): tuning.ini,
// tuning-reference.ini and the TUNING screen, over a game's rows.

#include "zge_tuning.h"
#include "zge_devtools.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/pad.h"
#include "runtime/session.h"
#include "runtime/tuning_ini.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace zge_host::tuning
{
    namespace
    {
        namespace tn = zx::tuning;

        constexpr const char kTag[] = "[tuning]";
        constexpr const char kCategory[] = "TUNING";

        uint32_t address(const Row &row) { return row.global + row.field; }

        float read(const uint8_t *rdram, const Row &row)
        {
            const uint8_t *p = getConstMemPtr(rdram, address(row));
            if (!p)
                return 0.0f;
            switch (row.kind)
            {
            case Kind::Byte: return static_cast<float>(*p);
            case Kind::Half:
            case Kind::Angle: { uint16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v); }
            case Kind::Word: { int32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v); }
            case Kind::Float: { float v; std::memcpy(&v, p, 4); return v; }
            }
            return 0.0f;
        }

        void write(uint8_t *rdram, const Row &row, float value)
        {
            uint8_t *p = getMemPtr(rdram, address(row));
            if (!p)
                return;
            switch (row.kind)
            {
            case Kind::Byte: *p = static_cast<uint8_t>(std::lround(value)); break;
            case Kind::Half:
            case Kind::Angle: { const uint16_t v = static_cast<uint16_t>(std::lround(value)); std::memcpy(p, &v, 2); break; }
            case Kind::Word: { const int32_t v = static_cast<int32_t>(std::lround(value)); std::memcpy(p, &v, 4); break; }
            case Kind::Float: std::memcpy(p, &value, 4); break;
            }
        }

        std::vector<tn::Entry> entries(const uint8_t *rdram, const Table &table)
        {
            std::vector<tn::Entry> out;
            for (size_t i = 0; i < table.rowCount; ++i)
            {
                const Row &row = table.rows[i];
                tn::Entry entry;
                entry.category = kCategory;
                entry.name = row.name;
                entry.address = address(row);
                entry.value = read(rdram, row);
                entry.maxValue = row.most;
                entry.minValue = row.least;
                entry.step = row.step;
                out.push_back(std::move(entry));
            }
            return out;
        }

        const Row *rowAt(const Table &table, uint32_t at)
        {
            for (size_t i = 0; i < table.rowCount; ++i)
                if (address(table.rows[i]) == at)
                    return &table.rows[i];
            return nullptr;
        }

        void applyTuning(uint8_t *rdram, const Table &table, const std::vector<tn::Entry> &entries,
                         const std::filesystem::path &file)
        {
            std::string text;
            if (!devtools::readTuningText(file, text))
                return;
            std::vector<std::string> warnings;
            const std::vector<tn::Override> overrides = tn::match(tn::parse(text, &warnings), entries, &warnings);
            for (const std::string &warning : warnings)
                std::cerr << kTag << " " << file.filename().string() << " " << warning << std::endl;
            for (const tn::Override &o : overrides)
            {
                const Row *row = rowAt(table, o.entry->address);
                if (!row)
                    continue;
                const float before = read(rdram, *row);
                write(rdram, *row, o.value);
                const bool outside = o.value < o.entry->minValue || o.value > o.entry->maxValue;
                std::cout << kTag << " " << o.entry->name << " = " << read(rdram, *row) << " (was " << before
                          << (outside ? ", outside the GameCube's range" : "") << ")" << std::endl;
            }
            std::cout << kTag << " " << overrides.size() << " entr" << (overrides.size() == 1 ? "y" : "ies") << " from "
                      << file.string() << std::endl;
        }

        void writeReference(const Table &table, const std::vector<tn::Entry> &entries)
        {
            const std::filesystem::path reference = devtools::tuningReferenceFile();
            if (reference.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(reference.parent_path(), ec);
            std::ofstream out(reference, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out << tn::reference(entries, std::string("; ") + table.game + ": " + table.about +
                                              "; row the PS2 has with the game's own value and the GameCube's range. Copy lines to\n"
                                              "; tuning.ini in this folder (without the ';') and change the value; they are set\n"
                                              "; when the game starts. Changing them changes how the game plays. Rewritten at\n"
                                              "; every start with the developer menu on.\n");
        }
    }

    namespace
    {
        // ---- the TUNING screen --------------------------------------------

        // Host key codes (runtime/pad.h keyPressedWithin's).
        constexpr int kKeyF2 = 291, kKeyRight = 262, kKeyLeft = 263, kKeyDown = 264, kKeyUp = 265;
        constexpr size_t kShownRows = 12;
        constexpr uint32_t kScreenX = 40, kScreenY = 60, kLineHeight = 12;
        constexpr int kRegSp = 29;
        constexpr const char kLineFormat[] = "%s";

        struct Screen
        {
            const Table *table = nullptr;
            bool shown = false;
            size_t cursor = 0, top = 0;
            bool latched[5] = {};
            PS2Machine::RecompiledFunction drawText = nullptr;
            // Each table row's range: the GameCube's, reaching the game's own
            // value at the start where that lies outside it (a game whose
            // code has another scale there).
            std::vector<float> least, most;
        };
        Screen &screen() { return zx::session::state<Screen>(); }

        // A key's press (once per press).
        bool pressed(int key, bool &latch)
        {
            const bool down = PSPadBackend::keyPressedWithin(key, 0.05);
            const bool edge = down && !latch;
            latch = down;
            return edge;
        }

        std::string valueText(const Row &row, float value)
        {
            char text[32];
            if (row.kind == Kind::Float)
                std::snprintf(text, sizeof(text), "%.4f", static_cast<double>(value));
            else
                std::snprintf(text, sizeof(text), "%d", static_cast<int>(value));
            return text;
        }

        // The strings' task: the screen's lines printed first, then the
        // strings drawn as ever.
        void drawTextHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Screen &sc = screen();
            const Table &table = *sc.table;
            if (pressed(kKeyF2, sc.latched[0]))
            {
                sc.shown = !sc.shown;
                std::cout << kTag << " TUNING screen " << (sc.shown ? "on" : "off") << std::endl;
            }
            if (sc.shown)
            {
                // The engine's debug drawing on for this frame's strings (the
                // game's frame sets it again from its debug display at the
                // next frame's start), without the profiler and its backdrop.
                if (uint8_t *enabled = getMemPtr(rdram, table.debugEnabled))
                {
                    const uint32_t on = 1;
                    std::memcpy(enabled, &on, sizeof(on));
                }
                // The riding rider's record (riderOf(0)): its ability rows after the table's.
                const uint32_t stack0 = (getRegU32(ctx, kRegSp) - 0x600u) & ~0xFu;
                uint32_t rider = 0;
                runtime->callGuestFunction(rdram, ctx, table.riderOf, {0u}, stack0, &rider);
                std::vector<Row> rows(table.rows, table.rows + table.rowCount);
                std::vector<float> least = sc.least, most = sc.most;
                if (rider != 0)
                    for (size_t i = 0; i < table.riderRowCount; ++i)
                    {
                        Row r = table.riderRows[i];
                        r.global = rider;
                        rows.push_back(r);
                        least.push_back(r.least);
                        most.push_back(r.most);
                    }
                if (pressed(kKeyDown, sc.latched[1]) && sc.cursor + 1 < rows.size())
                    ++sc.cursor;
                if (pressed(kKeyUp, sc.latched[2]) && sc.cursor > 0)
                    --sc.cursor;
                // (the rider's rows come and go with the ride: the cursor kept, shown on the last row there is)
                const size_t at = std::min(sc.cursor, rows.size() - 1);
                const Row &row = rows[at];
                const int change = pressed(kKeyRight, sc.latched[3]) ? 1 : pressed(kKeyLeft, sc.latched[4]) ? -1 : 0;
                if (change != 0)
                {
                    const float value = std::clamp(read(rdram, row) + static_cast<float>(change) * row.step, least[at], most[at]);
                    write(rdram, row, value);
                    std::cout << kTag << " " << row.name << " = " << valueText(row, read(rdram, row)) << std::endl;
                }
                sc.top = std::min(sc.top, at);
                if (at >= sc.top + kShownRows)
                    sc.top = at + 1 - kShownRows;
                // The lines as the debug text's arguments, below the caller's stack.
                const uint32_t stack = (getRegU32(ctx, kRegSp) - 0x600u) & ~0xFu, text = stack + 0x100u;
                auto print = [&](uint32_t y, const std::string &line)
                {
                    if (uint8_t *p = getMemPtr(rdram, text))
                    {
                        std::memcpy(p, kLineFormat, sizeof(kLineFormat));
                        const std::string cut = line.substr(0, 0x7F);
                        std::memcpy(p + 0x10, cut.c_str(), cut.size() + 1);
                    }
                    runtime->callGuestFunction(rdram, ctx, table.print, {kScreenX, y, text, text + 0x10u}, stack);
                };
                print(kScreenY, std::string("TUNING  ") + std::to_string(at + 1) + " of " + std::to_string(rows.size()));
                for (size_t i = sc.top; i < std::min(rows.size(), sc.top + kShownRows); ++i)
                    print(kScreenY + static_cast<uint32_t>(i - sc.top + 2) * kLineHeight,
                          std::string(i == at ? "> " : "  ") + rows[i].name + "  " + valueText(rows[i], read(rdram, rows[i])));
            }
            sc.drawText(rdram, ctx, runtime);
        }
    }

    void install(PS2Machine &runtime, const Table &table)
    {
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        const char *devMenu = zx::boot::text("ZX_DEV_MENU");
        const bool reference = devMenu && devMenu[0] == '1';
        const std::filesystem::path file = devtools::tuningFile();
        const bool tuning = !file.empty() && devtools::hasTuningFile(file);
        if (!reference && !tuning)
            return;
        const std::vector<tn::Entry> table0 = entries(rdram, table);
        if (reference)
        {
            writeReference(table, table0);
            Screen &sc = screen();
            sc.table = &table;
            for (const tn::Entry &entry : table0)
            {
                sc.least.push_back(std::min(entry.minValue, entry.value));
                sc.most.push_back(std::max(entry.maxValue, entry.value));
            }
            sc.drawText = runtime.lookupFunction(table.drawText);
            if (sc.drawText && runtime.replaceFunction(table.drawText, &drawTextHook))
            {
                const char *shown = zx::boot::text("ZX_TUNING_SCREEN"); // (developers: shown from the start)
                sc.shown = shown && shown[0] == '1';
                if (const char *row = zx::boot::text("ZX_TUNING_SCREEN_ROW"); row && *row)
                    sc.cursor = static_cast<size_t>(std::max(1L, std::strtol(row, nullptr, 10)) - 1);
                std::cout << kTag << " the TUNING screen: F2 shows it (" << table.rowCount << " rows, and the riding rider's "
                          << table.riderRowCount << " abilities)" << std::endl;
            }
            else
                sc.drawText = nullptr;
        }
        if (tuning)
            applyTuning(rdram, table, table0, file);
    }
}
