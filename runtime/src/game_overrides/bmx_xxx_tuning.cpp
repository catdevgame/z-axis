// BMX XXX (SLUS-20415): the developer tuning table, its GameCube release's, on
// the PS2's values (Mods/tuning.ini, as Aggressive Inline's).
//
// The GameCube's debug menu has a TUNING screen (its builder 0x800518A0: 114
// rows of name, kind, variable, step, least, most and value); the PS2 has
// neither the screen nor the table. Each row's variable is paired with the
// PS2 global it is by the code that uses them (tooling's tuning pairing: the
// two builds' matched functions refer to both, the data blocks' order kept;
// workdb notes, topic layout, at each PS2 global): 79 of the 114, and eight
// of the nine rider abilities, fields of the rider record (the GameCube's
// getter 0x800745BC is the PS2's riderOf; each stat's PS2 readers, by the
// readable code's names, and their GameCube matches read the same offset:
// air, speed, spin, flip, balance, technique, manual and wallride, the
// PS2's Rider airStat .. wallRideStat). Unpaired: rider abil - land (+0x3C:
// no matched rider function reads it on both builds), the
// camera's ten (none of their users matched), and NPR flags, col box - air -
// Y, NPR clip distance, far Z clip plane, display rider position, NPR to
// record, max NPRs riding around, timer disable, display sound channels,
// run length (sec), rotation max - grind, grind-to-grind ang tlrnce,
// wallride angle tolerance, cam clip dist - rider and - terrain, spline
// camera test (no PS2 code refers to a global the pairing could take). The
// two special feature flags rows (the saved game's features, read from the
// save) are left out: they are no tuning. Every PS2 default is the
// GameCube's but three floats a last bit apart (grind min velocity, pedal
// hysteresis, max stall speed).
//
// The table's names are the GameCube's, one section, [TUNING]; a row's
// kind is a byte, a 16-bit word, a 32-bit word, a float or an angle (a
// 16-bit word, 65536 a turn). tuning.ini (zge_devtools.h tuningFile) sets
// rows before the game starts; with ZX_DEV_MENU=1 tuning-reference.ini
// beside it lists every row with the game's value and the GameCube's range.
// With no tuning.ini and the developer menu off nothing is read or written.
//
// The rider abilities are the riding rider's (riderOf(0)), made from the
// character's stats at each ride, so they are the screen's alone, after the
// table's rows, while a rider rides; tuning.ini has no row of them.
//
// The TUNING screen (ZX_DEV_MENU=1): F2 shows the rows over the game, as
// the GameCube's screen (0x8004F1B4, 0x8004F3D8, 0x8004F478) does: twelve
// at a time, up and down move the cursor (the list scrolls with it), left
// and right change the row's value by its step within its range. The lines
// are the engine's debug text (debugPrintInColour, drawn by the strings'
// task drawColouredDebugText, which this prints them before), the engine's
// debug drawing on for them (g_debugEnabled, which the game's frame sets
// from its debug display at each frame's start) without the profiler.
// The keys also reach the game, as on the GameCube's in-game menu.
// ZX_TUNING_SCREEN=1 (developers) shows it from the start, ZX_TUNING_SCREEN_ROW=<n>
// with the cursor on row n (1 first).

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_tuning.cpp.h"
#include "zge_devtools.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_tuning_ini.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace bmx_xxx
{
    namespace
    {
        namespace tn = zx::tuning;

        constexpr const char kTag[] = "[tuning]";
        constexpr const char kCategory[] = "TUNING";

        enum class Kind
        {
            Byte,  // 0 or 1, or a small count
            Half,  // a 16-bit word
            Word,  // a 32-bit count
            Float,
            Angle, // a 16-bit word, 65536 a turn
        };

        // A surface's friction (game/bmx/rider/physics/physics.h Surface:
        // grip, slip, rolling loss, grind drag): the GameCube's concrete
        // friction rows are the table's first surface's.
        struct SurfaceFriction
        {
            float grip;
            float slip;
            float rollingLoss;
            float grindDrag;
        };
        static_assert(sizeof(SurfaceFriction) == 0x10 && offsetof(SurfaceFriction, grindDrag) == 0xC);

        struct Row
        {
            const char *name;  // the GameCube's
            uint32_t global;   // the PS2 global it is
            uint32_t field;    // within it
            Kind kind;
            float step, least, most; // the GameCube's
        };
        // The GameCube's rows in its order, each with its PS2 global.
        const Row kRows[] = {
        {"display profiling info", sym::bmx::g_debugDisplay, 0, Kind::Byte, 1, 0, 2},
        {"hide game menus", sym::bmx::g_overlayHidden, 0, Kind::Byte, 1, 0, 1},
        {"damage scalar", sym::bmx::g_rideDistanceScale, 0, Kind::Float, 0.005f, 0.0f, 100.0f},
        {"dyn bounce rider", sym::bmx::g_bailRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction rider", sym::bmx::g_bailForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"dyn bounce bbox", sym::bmx::g_riderTumbleRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction bbox", sym::bmx::g_riderTumbleForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"dyn bounce bike", sym::bmx::g_bikeTumbleRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction bike", sym::bmx::g_bikeTumbleForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"col box - air - X", sym::bmx::g_cornerSide, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"col box - air - Z", sym::bmx::g_cornerAlong, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"draw physics", sym::bmx::g_drawBodies, 0, Kind::Word, 1, 0, 1},
        {"grind off balance factor", sym::bmx::g_grindTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"grind balance correction", sym::bmx::g_grindStickLean, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"grind balance speed factor", sym::bmx::g_grindSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"grind balance speed factor cutoff", sym::bmx::g_grindSlowSpeed, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
        {"grind detection eccentricity", sym::bmx::g_railAcrossReach, 0, Kind::Float, 0.01f, 0.0f, 1.0f},
        {"grind jumpoff dir timeout", sym::bmx::g_airStickHoldFrames, 0, Kind::Word, 1, 0, 600},
        {"overall balance time factor (BTF)", sym::bmx::g_flatlandTipGrowth, 0, Kind::Float, 0.01f, -10.0f, 10.0f},
        {"overall BTF timeout", sym::bmx::g_flatlandTipFrames, 0, Kind::Word, 60, 0, 12000},
        {"grind speed boost", sym::bmx::g_railBoost, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"grind min velocity", sym::bmx::g_railBoostSpeedLeast, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"grind gravity scalar", sym::bmx::g_railGravityShare, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"wallride speed boost", sym::bmx::g_stallKeptSpeedShare, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"wallride min velocity", sym::bmx::g_stallKeptSpeedLeast, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"manual off balance factor", sym::bmx::g_flatlandTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"manual balance correction", sym::bmx::g_flatlandStickLean, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"manual balance speed factor", sym::bmx::g_flatlandSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"manual balance speed factor cutoff", sym::bmx::g_flatlandSlowSpeed, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
        {"manual slowdown friction", sym::bmx::g_flatlandDrag, 0, Kind::Float, 1e-05f, -1.0f, 1.0f},
        {"walltap/fastplant off balance factor", sym::bmx::g_lipTip, 0, Kind::Float, 0.001f, -100.0f, 100.0f},
        {"max pedal speed", sym::bmx::g_pedalSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"pedal acceleration", sym::bmx::g_pedalPush, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"pedal hysteresis", sym::bmx::g_pedalRepeat, 0, Kind::Float, 0.001f, 0.0f, 0.5f},
        {"max stall speed", sym::bmx::g_railMostAcross, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"rhythm jump speed", sym::bmx::g_launchSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"spine vel adjust", sym::bmx::g_vertKeep, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"grind/stall vel", sym::bmx::g_stallSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"level out frames", sym::bmx::g_alignFrames, 0, Kind::Word, 1, 1, 300},
        {"max jump force", sym::bmx::g_hopPerStat, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"min jump force", sym::bmx::g_hopLeast, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"ramp jump bonus", sym::bmx::g_hopSlopeLoss, 0, Kind::Float, 0.001f, -1.0f, 1.0f},
        {"brake friction scalar", sym::bmx::g_brakeDrag, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
        {"rotation adjust - ground", sym::bmx::g_wheelieSteerRate, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
        {"rotation max - ground", sym::bmx::g_wheelieSteerMost, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
        {"rotation speed factor", sym::bmx::g_landSteerSpeed, 0, Kind::Float, 0.0001f, 0.0f, 100.0f},
        {"rotation adjust - air turn", sym::bmx::g_airStickTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"rotation adjust - air spin", sym::bmx::g_airButtonTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"rotation max - air", sym::bmx::g_airTurnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"pitch adjust - air", sym::bmx::g_turnLeast, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"pitch max - air", sym::bmx::g_turnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"jump max timer", sym::bmx::g_hopFullFrames, 0, Kind::Word, 1, 1, 300},
        {"friction - concrete static", sym::bmx::g_surfaces, offsetof(SurfaceFriction, grip), Kind::Float, 0.01f, 0.0f, 1000.0f},
        {"friction - concrete sliding", sym::bmx::g_surfaces, offsetof(SurfaceFriction, slip), Kind::Float, 0.01f, 0.0f, 1000.0f},
        {"friction - concrete roll", sym::bmx::g_surfaces, offsetof(SurfaceFriction, rollingLoss), Kind::Float, 1e-06f, 0.0f, 1.0f},
        {"friction - concrete grind", sym::bmx::g_surfaces, offsetof(SurfaceFriction, grindDrag), Kind::Float, 1e-06f, 0.0f, 1.0f},
        {"friction - turning penalty", sym::bmx::g_slideLoss, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"brake bias - towards rear", sym::bmx::g_wheelieSlide, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"grind around corners factor", sym::bmx::g_railSpeedKept, 0, Kind::Float, 0.1f, 0.0f, 1.0f},
        {"roll angle tolerance", sym::bmx::g_wallMostAngle, 0, Kind::Angle, 182, 0, 16384},
        {"land angle tolerance", sym::bmx::g_landWithin, 0, Kind::Angle, 18, 0, 16384},
        {"land angle tolerance (fakie)", sym::bmx::g_rollWithin, 0, Kind::Angle, 18, 0, 16384},
        {"grind balance angle tlrnce", sym::bmx::g_railLeanMost, 0, Kind::Angle, 18, 0, 32585},
        {"grind edge-to-edge angle tlrnce", sym::bmx::g_nextRailMostTurn, 0, Kind::Angle, 18, 0, 32749},
        {"roll over factor", sym::bmx::g_slopeReachScale, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"turn down factor", sym::bmx::g_wheelieTurnDegrees, 0, Kind::Float, 0.1f, 0.0f, 1000.0f},
        {"orient interpolation", sym::bmx::g_railAlignShare, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
        {"max lean angle", sym::bmx::g_leanDegrees, 0, Kind::Float, 0.1f, 0.0f, 90.0f},
        {"approach frames - grind", sym::bmx::g_approachGrindSteps, 0, Kind::Word, 1, 1, 300},
        {"approach frames - walltap/wallride", sym::bmx::g_approachOtherSteps, 0, Kind::Word, 1, 1, 300},
        {"approach frames - min", sym::bmx::g_leastSnapFrames, 0, Kind::Word, 1, 0, 300},
        {"overall edge detect radius", sym::bmx::g_approachRailReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection distance - air", sym::bmx::g_approachAirReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection distance - rolling", sym::bmx::g_approachGroundReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection increment - edge", sym::bmx::g_approachReachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"detection increment - wall", sym::bmx::g_approachLipReachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"wallride traction", sym::bmx::g_lipGravityScale, 0, Kind::Float, 0.01f, 0.0f, 1.0f},        };

        constexpr size_t kTableRows = sizeof(kRows) / sizeof(kRows[0]);

        // The rider abilities: a field of the riding rider's record
        // (game/bmx/rider/rider.h Rider: airStat .. wallRideStat, as far as
        // them), the GameCube's names and ranges.
        struct RiderStats
        {
            uint8_t _000[0x20];
            float air, speed, spin, flip, balance, technique, manual;
            uint8_t _03c[4]; // (the GameCube's land: not paired)
            float wallRide;
        };
        static_assert(offsetof(RiderStats, air) == 0x20 && offsetof(RiderStats, manual) == 0x38 &&
                      offsetof(RiderStats, wallRide) == 0x40);
        const Row kRiderRows[] = {
            {"rider abil - air", 0, offsetof(RiderStats, air), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - speed", 0, offsetof(RiderStats, speed), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - spin", 0, offsetof(RiderStats, spin), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - flip", 0, offsetof(RiderStats, flip), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - balance", 0, offsetof(RiderStats, balance), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - technique", 0, offsetof(RiderStats, technique), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - manual", 0, offsetof(RiderStats, manual), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - wallride", 0, offsetof(RiderStats, wallRide), Kind::Float, 0.001f, 0.0f, 10.0f},
        };
        constexpr size_t kRiderRowCount = sizeof(kRiderRows) / sizeof(kRiderRows[0]);
        constexpr size_t kRowCount = kTableRows;

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

        std::vector<tn::Entry> entries(const uint8_t *rdram)
        {
            std::vector<tn::Entry> out;
            for (const Row &row : kRows)
            {
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

        const Row *rowAt(uint32_t at)
        {
            for (const Row &row : kRows)
                if (address(row) == at)
                    return &row;
            return nullptr;
        }

        void applyTuning(uint8_t *rdram, const std::vector<tn::Entry> &table, const std::filesystem::path &file)
        {
            std::string text;
            if (!zge_host::devtools::readTuningText(file, text))
                return;
            std::vector<std::string> warnings;
            const std::vector<tn::Override> overrides = tn::match(tn::parse(text, &warnings), table, &warnings);
            for (const std::string &warning : warnings)
                std::cerr << kTag << " " << file.filename().string() << " " << warning << std::endl;
            for (const tn::Override &o : overrides)
            {
                const Row *row = rowAt(o.entry->address);
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

        void writeReference(const std::vector<tn::Entry> &table)
        {
            const std::filesystem::path reference = zge_host::devtools::tuningReferenceFile();
            if (reference.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(reference.parent_path(), ec);
            std::ofstream out(reference, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out << tn::reference(table,
                                 "; BMX XXX: the GameCube release's developer tuning table on the PS2's values, every\n"
                                 "; row the PS2 has with the game's own value and the GameCube's range. Copy lines to\n"
                                 "; tuning.ini in this folder (without the ';') and change the value; they are set\n"
                                 "; when the game starts. Changing them changes how the game plays. Rewritten at\n"
                                 "; every start with the developer menu on.\n");
        }
    }

    namespace
    {
        // ---- the TUNING screen --------------------------------------------

        // Host key codes (runtime/ps2_pad.h keyPressedWithin's).
        constexpr int kKeyF2 = 291, kKeyRight = 262, kKeyLeft = 263, kKeyDown = 264, kKeyUp = 265;
        constexpr size_t kShownRows = 12;
        constexpr uint32_t kScreenX = 40, kScreenY = 60, kLineHeight = 12;
        constexpr int kRegSp = 29;
        constexpr const char kLineFormat[] = "%s";

        struct Screen
        {
            bool shown = false;
            size_t cursor = 0, top = 0;
            bool latched[5] = {};
            PS2Runtime::RecompiledFunction drawText = nullptr;
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

        // drawColouredDebugText(): the screen's lines printed first, then the
        // strings drawn as ever.
        void drawTextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Screen &sc = screen();
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
                if (uint8_t *enabled = getMemPtr(rdram, sym::bmx::g_debugEnabled))
                {
                    const uint32_t on = 1;
                    std::memcpy(enabled, &on, sizeof(on));
                }
                // The riding rider's record (riderOf(0)): its ability rows after the table's.
                const uint32_t stack0 = (getRegU32(ctx, kRegSp) - 0x600u) & ~0xFu;
                uint32_t rider = 0;
                runtime->callGuestFunction(rdram, ctx, sym::bmx::riderOf, {0u}, stack0, &rider);
                std::vector<Row> rows(std::begin(kRows), std::end(kRows));
                if (rider != 0)
                    for (Row r : kRiderRows)
                    {
                        r.global = rider;
                        rows.push_back(r);
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
                    const float value = std::clamp(read(rdram, row) + static_cast<float>(change) * row.step, row.least, row.most);
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
                    runtime->callGuestFunction(rdram, ctx, sym::bmx::debugPrintInColour, {kScreenX, y, text, text + 0x10u}, stack);
                };
                print(kScreenY, std::string("TUNING  ") + std::to_string(at + 1) + " of " + std::to_string(rows.size()));
                for (size_t i = sc.top; i < std::min(rows.size(), sc.top + kShownRows); ++i)
                    print(kScreenY + static_cast<uint32_t>(i - sc.top + 2) * kLineHeight,
                          std::string(i == at ? "> " : "  ") + rows[i].name + "  " + valueText(rows[i], read(rdram, rows[i])));
            }
            sc.drawText(rdram, ctx, runtime);
        }
    }

    void installTuning(PS2Runtime &runtime)
    {
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        const char *devMenu = zx::boot::text("ZX_DEV_MENU");
        const bool reference = devMenu && devMenu[0] == '1';
        const std::filesystem::path file = zge_host::devtools::tuningFile();
        const bool tuning = !file.empty() && zge_host::devtools::hasTuningFile(file);
        if (!reference && !tuning)
            return;
        const std::vector<tn::Entry> table = entries(rdram);
        if (reference)
        {
            writeReference(table);
            Screen &sc = screen();
            sc.drawText = runtime.lookupFunction(sym::bmx::drawColouredDebugText);
            if (sc.drawText && runtime.replaceFunction(sym::bmx::drawColouredDebugText, &drawTextHook))
            {
                const char *shown = zx::boot::text("ZX_TUNING_SCREEN"); // (developers: shown from the start)
                sc.shown = shown && shown[0] == '1';
                if (const char *row = zx::boot::text("ZX_TUNING_SCREEN_ROW"); row && *row)
                    sc.cursor = static_cast<size_t>(std::max(1L, std::strtol(row, nullptr, 10)) - 1);
                std::cout << kTag << " the TUNING screen: F2 shows it (" << kRowCount << " rows, and the riding rider's "
                          << kRiderRowCount << " abilities)" << std::endl;
            }
            else
                sc.drawText = nullptr;
        }
        if (tuning)
            applyTuning(rdram, table, file);
    }
}
