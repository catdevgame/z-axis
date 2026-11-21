// Aggressive Inline (SLUS-20327): gameplay settings and shortcuts. See
// docs/SETTINGS.md and docs/TUNING.md (the tuning table).
//
// Camera, Assists, Cheats. The developer tuning table (0x246138) holds the
// values the game reads while it plays (aggressive_inline_devtools.cpp);
// these pages scale or replace some of them, written at VBlank on the EE
// thread whenever they differ from what the settings want. Values at their
// default are never written (an entry this code changed is put back once).
// - Camera: Distance (Distance, Grind Distance, Distance HPole) and Height
//   (Height, Grind Height) as percentages of the game's, read live by the
//   follow camera; Camera PS2 / Xbox (the Xbox release's field of view, see
//   xboxFov). Field of View and Camera Xbox are drawn, not played: the game
//   keeps its own field of view (Camera FOV, or the level's MP FOV in split
//   screen) and the view settings draw a level's view cameras with the one
//   drawnFieldOfView gives (zge_view.h), so what the game decides by its
//   view stays the PS2's.
// - Assists: Death by Juice (an empty juice meter ends the run), Max Juice
//   Degradation/sec, Juice Lost/Level on crash.
// - Cheats: the game's own list (0x2541F0, names as the game shows them),
//   each as the tuning writes its code makes (inlineMenuCheat 0x1AA628).
//   The codes also set a bit in 0x24CBC8, which is part of the career save
//   and makes the next level flag its players (0x1A60D8); these toggles
//   leave it alone, so the save is untouched. BAKABAKA (bit 8, no table
//   write) is not offered.
// - Unlock Everything writes the career save's unlocks (see unlockAll);
//   ZX_DEV_UNLOCK_ALL=1 does the same in the front end, for test runs.
//
// Restart: the Restart key asks aggressive_inline_restart.cpp for what fits
// the mode (the level again, the Timed Run's Play Again, the split screen's
// game again, a career challenge or a Tutorial lesson again), done in place
// of one level update (0x1ACA68); the pause menus' items are in
// aggressive_inline_settings.cpp.
//
// Hide HUD: zgui's widget draw 0x171CA8 is wrapped to skip InGameUI's (and
// MultiplayerUI's) Overlay and OverlayToggle groups (score, juice meter,
// trick list, balance meters, timer, messages), and every widget in photo
// mode (aggressive_inline_photo.cpp). The screenshot key is the app's, for
// every game (collection/app.cpp).
//
// Clean Top Screen (runtime/panel_snapshot.h): the same draw leaves out the
// HUD pieces a bottom screen shows while it shows them: InGameUI's
// OverlayToggle.Timer (the clock) and OverlayToggle.TrickList (the line's
// tricks, its spin and its PotentialScore: the bottom screen's line card).
// The Objective message box stays (a goal's text with its Continue button,
// the hints: more than the bottom screen's goal card), and so do the score
// and the juice meter, which the bottom screen has not.
//
// Settings are kept in ZX_GAMEPLAY_INI (the app: gameplay.ini in the
// support folder).

#include "aggressive_inline_gameplay.h"
#include "sym/game_overrides/aggressive_inline_gameplay.cpp.h"
#include "runtime/play_recording.h"

#include "aggressive_inline_devtools.h"
#include "aggressive_inline_photo.h"
#include "aggressive_inline_restart.h"
#include "overrides/zge_gameplay.h"
#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/settings.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Its own namespace: the game override's files are compiled together
// (unity build) and share aggressive_inline's anonymous namespace.
namespace aggressive_inline::gameplay
{
    namespace
    {
        namespace st = zx::settings;
        namespace hi = ps2_host_input;

        constexpr uint32_t kGameMode = sym::g_gameMode;
        constexpr uint32_t kModeFrontEnd = 1u;
        constexpr uint32_t kModePlay = 2u;
        constexpr uint32_t kModePaused = 3u;
        constexpr uint32_t kLevelFrame = sym::g_levelFrame;
        constexpr uint32_t kPlayUpdate = sym::updatePlayMode;

        constexpr int kDefaultFov = 75; // the table's Camera FOV
        constexpr uint32_t kGameType = sym::g_gameTypeByte; // byte: 4 split screen
        constexpr uint8_t kGameTypeSplitScreen = 4u;

        constexpr uint32_t kGuiDraw = sym::drawWidget;
        constexpr uint32_t kWidgetParent = 0x08u;
        constexpr uint32_t kWidgetName = 0x14u;

        // Career save (its body is RAM from 0x24CBB8).
        constexpr uint32_t kUnlockWord = sym::g_unlockedSkaters;                 // 64-bit
        constexpr uint64_t kUnlockLevels = 0xFFFFFull;                // the D R R D L D L D R R R code
        constexpr uint64_t kUnlockMovies = 0x7Full << 48;             // Media Viewer cutscenes (0x24CBBE bits 0-6)
        constexpr uint32_t kSkaterRecords = sym::g_skaterRecords;              // 0x24C9B0 + 0x1180
        constexpr uint32_t kSkaterRecordSize = 0x510u;
        constexpr uint32_t kSkaterRecordCount = 20u;

        struct Hooks
        {
            PS2Machine *runtime = nullptr;
            PS2Machine::RecompiledFunction playUpdate = nullptr;
            PS2Machine::RecompiledFunction guiDraw = nullptr;
        };
        Hooks &hooks() { return zx::session::state<Hooks>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        using zge_host::gameplay::readFloat;
        using zge_host::gameplay::sameFloat;
        using zge_host::gameplay::writeFloat;

        // ---- the settings ---------------------------------------------------

        enum Option
        {
            kFov,
            kDistance,
            kHeight,
            kJuiceDeath,
            kJuiceDrain,
            kCrashLoss,
            kWallRide,
            kJuiceRegen,
            kInvulnerable,
            kPerfectGrind,
            kPerfectHandplant,
            kSuperSpin,
            kPerfectManual,
            kCameraStyle, // 0 the PS2's field of view, 1 the Xbox's (xboxFov)
            kOptionCount
        };

        const zge_host::gameplay::OptionInfo kOptions[kOptionCount] = {
            {"camera.fov", kDefaultFov},
            {"camera.distance", 100},
            {"camera.height", 100},
            {"assist.juice_runs_out", 0},
            {"assist.juice_drain", 100},
            {"assist.crash_juice_loss", 100},
            {"cheat.low_gravity_wall_ride", 0},
            {"cheat.juice_regeneration", 0},
            {"cheat.invulnerability", 0},
            {"cheat.perfect_grind", 0},
            {"cheat.perfect_handplant", 0},
            {"cheat.super_spin", 0},
            {"cheat.perfect_manual", 0},
            {"camera.style", 0},
        };
        constexpr const char *kOptionsHeader = "; Aggressive Inline: the Camera, Assists and Cheats pages of the Settings\n"
                                               "; screen (written by the game; numbers, 0/1 for off/on).\n";
        zge_host::gameplay::Options &options()
        {
            struct Made
            {
                zge_host::gameplay::Options options;
                Made() { options.define(kOptions, kOptionCount, kOptionsHeader); }
            };
            return zx::session::state<Made>().options;
        }

        int option(Option o)
        {
            return options().value(o);
        }

        // ---- the tuning table -------------------------------------------------

        struct Tune
        {
            const char *category;
            const char *name;
            zge_host::gameplay::GameFloat value; // its value before any setting (tuning.ini applied)
        };
        enum TuneId
        {
            kTDistance,
            kTGrindDistance,
            kTPoleDistance,
            kTHeight,
            kTGrindHeight,
            kTDeathByJuice,
            kTJuiceDrain,
            kTCrashLoss,
            kTWallRideGravity,
            kTGrindFactor,
            kTGrindInit,
            kTStallFactor,
            kTStallInit,
            kTManualFactor,
            kTManualInit,
            kTAirRotate,
            kTuneCount
        };
        struct Tuning
        {
            Tune tunes[kTuneCount] = {
                {"Camera", "Distance"},
                {"Camera", "Grind Distance"},
                {"Camera", "Distance HPole"},
                {"Camera", "Height"},
                {"Camera", "Grind Height"},
                {"General", "Death by Juice"},
                {"Scoring: Juice", "Max Juice Degradation/sec"},
                {"Scoring: Juice", "Juice Lost/Level on crash"},
                {"Grind/Walls", "Wall Ride Gravity"},
                {"Grind/Walls", "Grind Balance Factor"},
                {"Grind/Walls", "Grind Balance Init"},
                {"Grind/Walls", "Stall Balance Factor"},
                {"Grind/Walls", "Stall Balance Init"},
                {"Grind/Walls", "Manual Balance Factor"},
                {"Grind/Walls", "Manual Balance Init"},
                {"Physics", "Air Rotate"},
            };
        };
        Tuning &tuning() { return zx::session::state<Tuning>(); }

        // What the settings want for an entry (its base when they are at
        // their defaults). The cheat values are the codes' (0x1AA628).
        float wanted(int id)
        {
            const float base = tuning().tunes[id].value.game;
            const float distance = static_cast<float>(option(kDistance)) / 100.0f;
            const float height = static_cast<float>(option(kHeight)) / 100.0f;
            switch (id)
            {
            case kTDistance:
            case kTGrindDistance:
            case kTPoleDistance:
                return option(kDistance) == 100 ? base : base * distance;
            case kTHeight:
            case kTGrindHeight:
                return option(kHeight) == 100 ? base : base * height;
            case kTDeathByJuice:
                return option(kJuiceDeath) != 0 ? 0.0f : base;
            case kTJuiceDrain:
                if (option(kJuiceRegen) != 0)
                    return -0.2f;
                return option(kJuiceDrain) == 100 ? base : base * static_cast<float>(option(kJuiceDrain)) / 100.0f;
            case kTCrashLoss:
                if (option(kInvulnerable) != 0)
                    return 0.0f;
                return option(kCrashLoss) == 100 ? base : base * static_cast<float>(option(kCrashLoss)) / 100.0f;
            case kTWallRideGravity:
                return option(kWallRide) != 0 ? 3.0f : base;
            case kTGrindFactor:
            case kTGrindInit:
                return option(kPerfectGrind) != 0 ? 0.0f : base;
            case kTStallFactor:
            case kTStallInit:
                return option(kPerfectHandplant) != 0 ? 0.0f : base;
            case kTManualFactor:
            case kTManualInit:
                return option(kPerfectManual) != 0 ? 0.0f : base;
            case kTAirRotate:
                return option(kSuperSpin) != 0 ? 640.0f : base;
            default:
                return base;
            }
        }

        // VBlank (EE thread): the entries the settings change.
        void applyTunes(uint8_t *rdram)
        {
            for (int id = 0; id < kTuneCount; ++id)
                zge_host::gameplay::apply(rdram, tuning().tunes[id].value, wanted(id));
        }

        // The camera's entries (Distance .. Grind Height): the game's own
        // values in the table while `game`, else the settings' as they were.
        bool cameraTuningChanged()
        {
            return option(kDistance) != 100 || option(kHeight) != 100;
        }
        struct KeptCameraTuning
        {
            float values[kTHeight - kTDistance + 2] = {};
        };
        KeptCameraTuning &keptCameraTuning() { return zx::session::state<KeptCameraTuning>(); }
        void holdGameCameraTuning(uint8_t *rdram, bool game)
        {
            for (int id = kTDistance; id <= kTGrindHeight; ++id)
            {
                const zge_host::gameplay::GameFloat &value = tuning().tunes[id].value;
                if (!value.address)
                    continue;
                float &kept = keptCameraTuning().values[id - kTDistance];
                if (game)
                {
                    kept = readFloat(rdram, value.address);
                    writeFloat(rdram, value.address, value.game);
                }
                else
                    writeFloat(rdram, value.address, kept);
            }
        }

        // Camera / Distance: the game's own and the table's now (0, 0 when not found).
        void cameraDistance(const uint8_t *rdram, float &game, float &now)
        {
            const zge_host::gameplay::GameFloat &value = tuning().tunes[kTDistance].value;
            game = value.address ? value.game : 0.0f;
            now = value.address ? readFloat(rdram, value.address) : 0.0f;
        }

        // ---- field of view --------------------------------------------------

        float scaledFov(float game, int setting)
        {
            if (setting == kDefaultFov)
                return game;
            return std::clamp(game * static_cast<float>(setting) / static_cast<float>(kDefaultFov), 10.0f, 160.0f);
        }

        // Camera Xbox: the Xbox release's projection. Its follow camera is
        // the PS2's (the same functions, tuning, collision probe and
        // smoothing: 0x34460 / 0x32BB0 / 0x33250 / 0x35300 in default.xbe
        // against 0x18E858 / 0x18CC50 / 0x18D678 / 0x18FA00), but its
        // projection (0xC4005 -> 0x1003C2) takes the FOV as the vertical
        // angle divided by A = (640/480) x camera+0x424, that field being
        // (10/11) / ((640/480) x K), K 0.75, or 0.5625 when the console is
        // set to widescreen: vertical FOV = FOV x 1.1 / aspect, 61.9 degrees
        // at 4:3 and 46.4 at 16:9 for the game's 75, where the PS2 (and the
        // port, whose widescreen widens the sides) shows 0.75 x tan(75/2)
        // vertically at every aspect. So the Xbox keeps about the same
        // horizontal view and crops the top and bottom in widescreen: the
        // skater 1.34x as tall at 16:9 (0.96x at 4:3). Here the FOV the PS2
        // projection needs for that vertical extent at the picture's aspect
        // (77.3 at 4:3, 59.5 at 16:9). Not the Xbox's 10/11 pixel shape (it
        // expects TV pixels; in xemu its picture looks 10 % wide) and not in
        // split screen (the Xbox fixes 50 there; the PS2's per-level FOV
        // stays).
        float xboxFov(float fov, float aspect)
        {
            constexpr float kDegrees = 3.14159265f / 180.0f;
            const float vertical = std::tan(fov * 1.1f / aspect * kDegrees * 0.5f);
            return 2.0f * std::atan(vertical / 0.75f) / kDegrees;
        }

        float pictureAspect()
        {
            return hooks().runtime && play_recording::read(play_recording::Channel::Widescreen, hooks().runtime->widescreen()) ? hooks().runtime->wideAspect() : 4.0f / 3.0f;
        }

        bool splitScreen(const uint8_t *rdram)
        {
            const uint8_t *type = getConstMemPtr(rdram, kGameType);
            return type && *type == kGameTypeSplitScreen;
        }

        // The field of view the game's own is drawn with: the Xbox's
        // projection, then the FOV setting's scale.
        float wantedFov(const uint8_t *rdram, float game)
        {
            float fov = game;
            if (option(kCameraStyle) != 0 && !splitScreen(rdram) && game > 0.0f)
                fov = xboxFov(game, pictureAspect());
            return scaledFov(fov, option(kFov));
        }

        // ---- unlock everything ----------------------------------------------

        struct Unlocking
        {
            std::atomic<bool> unlockRequest{false};
            std::atomic<bool> unlocked{false};
            std::atomic<int64_t> unlockArmedUntil{0}; // ms, steady clock
        };
        Unlocking &unlocking() { return zx::session::state<Unlocking>(); }

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // What the game's two unlock codes write (0x1AA628), plus the Media
        // Viewer cutscenes no code sets: the unlock word's levels, skaters
        // and park pieces (bits 0-19) and cutscenes (bits 48-54), and in
        // each skater record +0x1181 = 10 and the level mask +0x1184 = all.
        // All of it is in the career save; native saves write it at the
        // next autosave (level end, quit) or save.
        void unlockAll(uint8_t *rdram)
        {
            uint64_t word = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, kUnlockWord))
                std::memcpy(&word, p, sizeof(word));
            word |= kUnlockLevels | kUnlockMovies;
            if (uint8_t *p = getMemPtr(rdram, kUnlockWord))
                std::memcpy(p, &word, sizeof(word));
            for (uint32_t n = 0; n < kSkaterRecordCount; ++n)
            {
                const uint32_t record = kSkaterRecords + n * kSkaterRecordSize;
                if (uint8_t *p = getMemPtr(rdram, record + 1u))
                    *p = 10u;
                writeWord(rdram, record + 4u, 0xFFFFFFFFu);
            }
            std::cout << "[gameplay] unlocked everything (levels, skaters, park pieces, cutscenes; in the career save)"
                      << std::endl;
        }

        bool allUnlocked(const uint8_t *rdram)
        {
            uint64_t word = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, kUnlockWord))
                std::memcpy(&word, p, sizeof(word));
            return (word & (kUnlockLevels | kUnlockMovies)) == (kUnlockLevels | kUnlockMovies);
        }

        // ---- restart ----------------------------------------------------------

        // The level's update (game mode 2): a restart asked for by the key or
        // a pause item runs in place of one update (aggressive_inline_restart.cpp).
        void playUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (restart::runPending(rdram, ctx, runtime))
                return;
            hooks().playUpdate(rdram, ctx, runtime);
        }

        // ---- hide HUD ---------------------------------------------------------

        struct Hud
        {
            std::atomic<bool> hidden{false};
        };
        Hud &hud() { return zx::session::state<Hud>(); }

        bool widgetNamed(const uint8_t *rdram, uint32_t widget, const char *name)
        {
            const uint32_t address = readWord(rdram, widget + kWidgetName);
            if (!address)
                return false;
            const size_t length = std::strlen(name) + 1u;
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p && getConstMemPtr(rdram, address + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
                   std::memcmp(p, name, length) == 0;
        }

        bool hudGroup(const uint8_t *rdram, uint32_t widget)
        {
            if (!widgetNamed(rdram, widget, "Overlay") && !widgetNamed(rdram, widget, "OverlayToggle"))
                return false;
            const uint32_t parent = readWord(rdram, widget + kWidgetParent);
            return parent && (widgetNamed(rdram, parent, "InGameUI") || widgetNamed(rdram, parent, "MultiplayerUI"));
        }

        // ---- Clean Top Screen ---------------------------------------------------

        // The HUD's pieces a bottom screen shows (zge_gameplay.h): single
        // player's (split screen draws MultiplayerUI's, and has no bottom screen).
        constexpr zge_host::gameplay::TopScreenPiece kTopScreenPieces[] = {
            {"InGameUI", "OverlayToggle", "Timer", panel::HudPiece::Clock},
            {"InGameUI", "OverlayToggle", "TrickList", panel::HudPiece::TrickLine},
        };

        uint32_t widgetParent(const uint8_t *rdram, uint32_t widget)
        {
            return readWord(rdram, widget + kWidgetParent);
        }

        bool leftOffTopScreen(const uint8_t *rdram, uint32_t widget)
        {
            return zge_host::gameplay::leftOffTopScreen(rdram, widget, kTopScreenPieces, &widgetNamed, &widgetParent, "gameplay");
        }

        void guiDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            // Photo mode: no widget at all (the pause menu, the HUD).
            if (photo::hidesUi() ||
                (hud().hidden.load(std::memory_order_relaxed) && hudGroup(rdram, getRegU32(ctx, 4))) ||
                leftOffTopScreen(rdram, getRegU32(ctx, 4)))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            hooks().guiDraw(rdram, ctx, runtime);
        }

        // ---- every VBlank -----------------------------------------------------

        struct Hotkeys
        {
            zge_host::gameplay::Hotkeys keys;
            uint32_t lastMode = 0;
        };
        Hotkeys &hotkeys() { return zx::session::state<Hotkeys>(); }

        void onVsync(uint8_t *rdram, uint64_t)
        {
            const uint32_t mode = readWord(rdram, kGameMode);
            zge_host::gameplay::Hotkeys &keys = hotkeys().keys;
            keys.poll();
            if (keys.pressed(hi::kHotkeyRestart) && mode == kModePlay && readWord(rdram, kLevelFrame) > 30u)
                restart::request(restart::Request::Key);
            restart::onVsync(rdram);
            if (keys.pressed(hi::kHotkeyHideHud) && (mode == kModePlay || mode == kModePaused))
            {
                hud().hidden = !hud().hidden.load();
                std::cout << "[gameplay] HUD " << (hud().hidden ? "hidden" : "shown") << std::endl;
            }
            if (mode == kModeFrontEnd && hotkeys().lastMode != kModeFrontEnd)
            {
                // Out of the level: the HUD shows again next time.
                hud().hidden = false;
                if (const uint32_t leftOut = zge_host::gameplay::takeLeftOffTopScreen())
                    std::cout << "[gameplay] Clean Top Screen: " << leftOut << " HUD widget draws left out in the level" << std::endl;
            }
            hotkeys().lastMode = mode;
            if (unlocking().unlockRequest.exchange(false))
            {
                unlockAll(rdram);
                unlocking().unlocked = true;
            }
            applyTunes(rdram);
        }

        // ---- registration -----------------------------------------------------

        st::Setting optionSetting(Option o, const char *label, const char *help)
        {
            return options().row(o, label, help);
        }

        st::Setting rangeSetting(Option o, const char *label, int lo, int hi, int step, const char *unit, const char *help)
        {
            return options().rangeRow(o, label, lo, hi, step, unit, help);
        }

        void registerSettings()
        {
            st::Setting fov = rangeSetting(kFov, "Field of View", 50, 110, 5, "", "Wider shows more. The game's is 75.");
            zx::session::addSetting(fov);
            zx::session::addSetting(rangeSetting(kDistance, "Camera Distance", 50, 250, 10, "%", "How far behind the skater."));
            zx::session::addSetting(rangeSetting(kHeight, "Camera Height", 50, 300, 10, "%", "How high above the skater."));
            st::Setting style = optionSetting(kCameraStyle, "Camera Style", "Xbox shows more in widescreen.");
            style.kind = st::Kind::Choice;
            style.choices = {"PS2", "Xbox"};
            zx::session::addSetting(style);

            st::Setting death = optionSetting(kJuiceDeath, "Out of Juice", "What happens when the juice runs out.");
            death.kind = st::Kind::Choice;
            death.choices = {"Run Ends", "Keep Skating"};
            zx::session::addSetting(death);
            zx::session::addSetting(rangeSetting(kJuiceDrain, "Juice Drain", 0, 100, 25, "%", "How fast the juice meter empties."));
            zx::session::addSetting(rangeSetting(kCrashLoss, "Juice Lost on Crash", 0, 100, 25, "%", "Juice a crash takes away."));

            const struct
            {
                Option option;
                const char *label;
                const char *help;
            } cheats[] = {
                {kWallRide, "Low Gravity Wall Ride", "Wall rides last longer."},
                {kJuiceRegen, "Juice Regeneration", "The juice meter fills up by itself."},
                {kInvulnerable, "Invulnerability", "Crashes cost no juice."},
                {kPerfectGrind, "Perfect Grind", "Grinds never lose balance."},
                {kPerfectHandplant, "Perfect Handplant", "Hand plants never lose balance."},
                {kSuperSpin, "Super Spin", "Spins twice as fast in the air."},
                {kPerfectManual, "Perfect Manual", "Manuals never lose balance."},
            };
            for (const auto &cheat : cheats)
                zx::session::addSetting(optionSetting(cheat.option, cheat.label, cheat.help));

            st::Setting unlock;
            unlock.key = "cheat.unlock_all";
            unlock.label = "Unlock Everything";
            unlock.kind = st::Kind::Choice;
            unlock.choices = {"No", "Sure?", "Unlocked"};
            unlock.help = "Opens all levels, skaters and movies.";
            unlock.current = []()
            {
                if (unlocking().unlocked.load() || (hooks().runtime && allUnlocked(hooks().runtime->memory().getRDRAM())))
                    return 2;
                return unlocking().unlockArmedUntil.load() > nowMs() ? 1 : 0;
            };
            unlock.apply = [](int value)
            {
                if (value == 1)
                {
                    unlocking().unlockArmedUntil = nowMs() + 5000;
                }
                else if (value == 2 && unlocking().unlockArmedUntil.load() > nowMs())
                {
                    unlocking().unlockArmedUntil = 0;
                    unlocking().unlockRequest = true;
                    unlocking().unlocked = true;
                }
                else
                {
                    unlocking().unlockArmedUntil = 0;
                }
            };
            zx::session::addSetting(unlock);
            // The shortcuts as settings (hotkey.*, for ZX_CONTROL) are the
            // input layer's, every game's (pad.cpp).
        }
    }

    // ---- developers: everything unlocked in test runs ------------------------

    // ZX_DEV_UNLOCK_ALL=1 (runner only, off by default): what Unlock
    // Everything writes (unlockAll: the two unlock codes' bits, every skater
    // record's level mask), applied in the front end whenever it is not all
    // set, so a fresh card and a save that loads later both end up unlocked
    // (agents reach the other levels' careers from a test run). It changes
    // the career save the run writes: use a scratch HOME and card.
    bool devAllUnlocked(const uint8_t *rdram)
    {
        if (!allUnlocked(rdram))
            return false;
        for (uint32_t n = 0; n < kSkaterRecordCount; ++n)
            if (readWord(rdram, kSkaterRecords + n * kSkaterRecordSize + 4u) != 0xFFFFFFFFu)
                return false;
        return true;
    }

    void devUnlockOnVsync(uint8_t *rdram, uint64_t)
    {
        if (readWord(rdram, kGameMode) == kModeFrontEnd && !devAllUnlocked(rdram))
            unlockAll(rdram);
    }

    void install(PS2Machine &runtime)
    {
        if (const char *dev = zx::boot::text("ZX_DEV_UNLOCK_ALL"); dev && dev[0] == '1')
        {
            runtime.addVSyncHook(&devUnlockOnVsync);
            std::cout << "[gameplay] ZX_DEV_UNLOCK_ALL: levels, skaters and cutscenes unlocked in the front end"
                      << std::endl;
        }
        if (const char *on = zx::boot::text("ZX_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        hooks().runtime = &runtime;
        if (const char *file = zx::boot::text("ZX_GAMEPLAY_INI"); file && file[0])
            options().setFile(file);
        options().load();
        for (Tune &tune : tuning().tunes)
        {
            zge_host::gameplay::takeGameValue(rdram, tune.value, tuningValueAddress(rdram, tune.category, tune.name));
            if (!tune.value.address)
                std::cerr << "[gameplay] tuning entry " << tune.category << " / " << tune.name << " not found" << std::endl;
        }

        hooks().playUpdate = runtime.lookupFunction(kPlayUpdate);
        hooks().guiDraw = runtime.lookupFunction(kGuiDraw);
        if (!hooks().playUpdate || !hooks().guiDraw || !restart::install(runtime) ||
            !runtime.replaceFunction(kPlayUpdate, &playUpdateHook) ||
            !runtime.replaceFunction(kGuiDraw, &guiDrawHook))
        {
            std::cerr << "[gameplay] hooks not installed" << std::endl;
            return;
        }
        registerSettings();
        photo::install(runtime);
        // Values from the file, clamped to the pages' ranges.
        options().takeRowValues();
        runtime.addVSyncHook(&onVsync);
        std::cout << "[gameplay] camera, assists, cheats, restart and hide HUD keys on"
                  << (options().file().empty() ? "" : " (" + options().file().string() + ")") << std::endl;
    }
}

namespace aggressive_inline
{
    void installGameplay(PS2Machine &runtime)
    {
        gameplay::install(runtime);
    }

    float drawnFieldOfView(const uint8_t *rdram, float gameDegrees)
    {
        return gameplay::wantedFov(rdram, gameDegrees);
    }

    bool cameraTuningChanged()
    {
        return gameplay::cameraTuningChanged();
    }

    void holdGameCameraTuning(uint8_t *rdram, bool game)
    {
        gameplay::holdGameCameraTuning(rdram, game);
    }

    void cameraDistance(const uint8_t *rdram, float &game, float &now)
    {
        gameplay::cameraDistance(rdram, game, now);
    }
}
