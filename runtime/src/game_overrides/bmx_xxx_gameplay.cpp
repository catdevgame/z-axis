// BMX XXX (SLUS-20415): the gameplay pages and keys, Aggressive Inline's
// (aggressive_inline_gameplay.cpp) and Dave Mirra BMX 2's
// (dave_mirra_gameplay.cpp) at BMX XXX's own code. The options are kept in
// gameplay.ini and made settings rows by the shared host side
// (zge_gameplay.h); each is at the game's own value until picked, and at
// that value nothing here writes the game's memory.
//
// Camera. The follow mode (game/bmx/camera/follow.cpp) reads its settings
// every frame from g_followCameraSettings: a row for each way the screen is
// shared (one view, two above each other, two side by side, four) and each
// camera type the player picks (five: behind, far ahead, close, low and the
// rider's eyes), each the eye's offset in a space at the rider (right, up,
// forward: behind is negative), the distance the swinging modes keep, five
// rates and the field of view wanted in degrees, which the camera eases to;
// g_followSpecialCameraSettings (by how the screen is shared) and
// g_followFifthCameraSettings are the bail's and the fifth kind's. The page
// writes the tables at VBlank, as DM2's writes its own:
// - Camera Distance: the behind types' offset forward and their swing
//   distance, as a share of the game's;
// - Camera Height: their offset up, and g_followAirHeights, the height they
//   take while the rider is in the air or on a grind's approach;
// - Field of View: drawn, not played: the tables keep the game's, and a
//   level's view cameras are drawn with theirs scaled by the setting over
//   the game's 62 (the one-view behind type's; drawnFieldOfView,
//   zge_view.h), so what the game decides by its view stays the PS2's.
// The rider's-eyes type keeps its place. The camera code itself is not
// touched (the follow mode is not Verified), only the data it reads.
//
// Assists. BMX XXX has no juice meter (Aggressive Inline's assists are its
// rules); what a rider can fail at is the balance, as in DM2: a grind's (and
// a stall's) tips each frame by g_grindTip (more at a stop, by
// g_grindSlowTip times it), by g_grindLineTip for each line of the trick
// line, and the longer it is held by g_flatlandTipGrowth; a lip trick's by
// g_lipTip; a manual's, a nose wheelie's and a flatland trick's by
// g_flatlandTip (and by g_flatlandTipGrowth the longer it is held); at the
// end of the balance the rider falls off (game/bmx/rider/actions: grind.cpp,
// rails.cpp, manual.cpp, flatland.cpp). The two rows scale those rates as a
// share of the game's (100 %: the game's; 0 %: the balance stays where the
// rider holds it). g_flatlandTipGrowth is one word the game reads for both:
// Manual Balance Loss scales it, so it also changes a long grind's tip.
//
// Cheats. BMX XXX's own codes (the cheats menu's keyboard, bmxmenuscriptCheat:
// 54 of them in g_cheatCodes) are all unlocks, kept in the saved game: the
// levels for every character, the movies, the characters, their bikes and
// the special modes. The page has Aggressive Inline's two that fit, Perfect
// Grind (the grind's, stall's and lip trick's tips 0) and Perfect Manual (the
// manual's, wheelie's and flatland's tips 0, their growth with them), which
// leave the save alone; and Unlock Everything, which does what every code in
// the game's table does (each row as bmxmenuscriptCheat applies it, the
// special modes opened but not turned on) in the front end, so the save
// written next has it (two steps, as AI's and DM2's).
//
// Hide HUD: the Hide HUD key (controls.ini [hotkeys] hide_hud) in a level
// hides the timer, the score, the trick, the balance, the health, the hints,
// the goals and the rewards: zgui's widget draw (drawWidget, AI's) skips the
// Overlay group of each of the overlay's display files (UI_overDisp_com,
// UI_overDisp_P_full and the split screen's), matched by its name and its
// parent's, as AI's skips InGameUI's. The pause menu and the game's screens
// (UI_overlay's frames), the subtitles and the replay's marks stay. The HUD
// shows again when the level is left.
//
// Restart: the Restart key in a level asks bmx_xxx_restart.cpp for the turn
// again (requestKey), as the pause menu's RESTART line.
//
// Photo mode (bmx_xxx_photo.cpp) is installed with the pages; while it is on
// no widget is drawn.
//
// Settings are kept in PS2X_GAMEPLAY_INI (the app: gameplay.ini in the
// game's support folder). On with the app's settings (PS2X_GAME_SETTINGS_MENUS=1).

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_gameplay.cpp.h"
#include "zge_gameplay.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace bmx_xxx::gameplay
{
    namespace
    {
        namespace gp = zge_host::gameplay;
        namespace st = ps2x::settings;

        // A CameraSettings row (game/bmx/camera/action_camera.h), as the
        // host reads it.
        struct CameraSettings
        {
            float offset[4];   // right, up, forward, 1
            float distance;    // the swinging modes' distance from the point looked at
            float spring, damping, eyeShare, lookAtShare, fovShare;
            float fov;         // degrees
            uint32_t unused;
        };
        static_assert(offsetof(CameraSettings, distance) == 0x10 && offsetof(CameraSettings, fov) == 0x28 &&
                      sizeof(CameraSettings) == 0x30);
        constexpr uint32_t kOffsetUp = offsetof(CameraSettings, offset) + 4u;
        constexpr uint32_t kOffsetForward = offsetof(CameraSettings, offset) + 8u;
        constexpr uint32_t kDistance = offsetof(CameraSettings, distance);
        constexpr uint32_t kFieldOfView = offsetof(CameraSettings, fov);
        constexpr uint32_t kRowSize = sizeof(CameraSettings);

        // The follow mode's tables (game/bmx/camera/follow.cpp).
        constexpr uint32_t kLayoutGroups = 4, kCameraTypes = 5;
        constexpr uint32_t kRiderEyesType = 4;  // keeps its place
        constexpr uint32_t kBehindRows = kLayoutGroups * (kCameraTypes - 1);
        constexpr uint32_t kFovRows = kLayoutGroups * kCameraTypes + kLayoutGroups + 1;
        constexpr int kGameFov = 62; // the one-view behind type's
        constexpr uint8_t kGameStateLevel = 1; // game/bmx/states/game_state.h
        constexpr int kRegA0 = 4, kRegRa = 31;

        uint32_t followRow(uint32_t group, uint32_t type)
        {
            return sym::bmx::g_followCameraSettings + (group * kCameraTypes + type) * kRowSize;
        }
        // Every row the follow mode reads a field of view from, in order.
        uint32_t fovRow(uint32_t n)
        {
            if (n < kLayoutGroups * kCameraTypes)
                return followRow(n / kCameraTypes, n % kCameraTypes);
            n -= kLayoutGroups * kCameraTypes;
            return n < kLayoutGroups ? sym::bmx::g_followSpecialCameraSettings + n * kRowSize
                                     : sym::bmx::g_followFifthCameraSettings;
        }
        // The behind types' rows.
        uint32_t behindRow(uint32_t n)
        {
            return followRow(n / (kCameraTypes - 1), n % (kCameraTypes - 1));
        }

        enum Option
        {
            kFov,
            kDistanceShare,
            kHeightShare,
            kGrindTip,
            kManualTip,
            kPerfectGrind,
            kPerfectManual,
            kOptionCount
        };
        const gp::OptionInfo kOptions[kOptionCount] = {
            {"camera.fov", kGameFov},
            {"camera.distance", 100},
            {"camera.height", 100},
            {"assist.grind_balance_loss", 100},
            {"assist.manual_balance_loss", 100},
            {"cheat.perfect_grind", 0},
            {"cheat.perfect_manual", 0},
        };
        constexpr const char *kHeader = "; BMX XXX: the Camera, Assists and Cheats pages (numbers; cheats 0 or 1).\n";

        struct State
        {
            gp::Options options;
            gp::Hotkeys keys;
            gp::GameFloat fov[kFovRows];
            gp::GameFloat forward[kBehindRows];
            gp::GameFloat distance[kBehindRows];
            gp::GameFloat up[kBehindRows];
            gp::GameFloat airHeight[kCameraTypes - 1];
            gp::GameFloat grindTip, grindLineTip, lipTip, flatlandTip, tipGrowth;
            int cameraLogged = -1;
            int assistsLogged = -1;
            PS2Runtime::RecompiledFunction drawWidget = nullptr;
            PS2Runtime *runtime = nullptr;
            State() { options.define(kOptions, kOptionCount, kHeader); }
        };
        State &state() { return ps2x::session::state<State>(); }

        int option(Option o) { return state().options.value(o); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        bool inLevel(const uint8_t *rdram)
        {
            const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_currentGameState);
            return p && *p == kGameStateLevel;
        }

        // ---- camera -----------------------------------------------------------

        void takeCameraValues(const uint8_t *rdram)
        {
            State &s = state();
            for (uint32_t n = 0; n < kFovRows; ++n)
                gp::takeGameValue(rdram, s.fov[n], fovRow(n) + kFieldOfView);
            for (uint32_t n = 0; n < kBehindRows; ++n)
            {
                gp::takeGameValue(rdram, s.forward[n], behindRow(n) + kOffsetForward);
                gp::takeGameValue(rdram, s.distance[n], behindRow(n) + kDistance);
                gp::takeGameValue(rdram, s.up[n], behindRow(n) + kOffsetUp);
            }
            for (uint32_t type = 0; type < kCameraTypes - 1; ++type)
                gp::takeGameValue(rdram, s.airHeight[type], sym::bmx::g_followAirHeights + type * 4u);
        }

        // A value scaled, the game's own when the setting is at the game's.
        float scaled(const gp::GameFloat &value, int setting, int game)
        {
            return setting == game ? value.game
                                   : value.game * static_cast<float>(setting) / static_cast<float>(game);
        }

        void applyCamera(uint8_t *rdram)
        {
            State &s = state();
            // (Field of View is drawn, not played: drawnFieldOfView below.)
            for (uint32_t n = 0; n < kBehindRows; ++n)
            {
                gp::apply(rdram, s.forward[n], scaled(s.forward[n], option(kDistanceShare), 100));
                gp::apply(rdram, s.distance[n], scaled(s.distance[n], option(kDistanceShare), 100));
                gp::apply(rdram, s.up[n], scaled(s.up[n], option(kHeightShare), 100));
            }
            for (gp::GameFloat &height : s.airHeight)
                gp::apply(rdram, height, scaled(height, option(kHeightShare), 100));
            const int key = option(kFov) * 1000000 + option(kDistanceShare) * 1000 + option(kHeightShare);
            if (key != s.cameraLogged)
            {
                s.cameraLogged = key;
                const uint32_t row = followRow(0, 0);
                std::cout << "[bmx-xxx] camera: behind view forward " << gp::readFloat(rdram, row + kOffsetForward)
                          << " up " << gp::readFloat(rdram, row + kOffsetUp) << " swing distance "
                          << gp::readFloat(rdram, row + kDistance) << " field of view "
                          << gp::readFloat(rdram, row + kFieldOfView) << ", in the air "
                          << gp::readFloat(rdram, sym::bmx::g_followAirHeights) << std::endl;
            }
        }

        // ---- assists ------------------------------------------------------------

        void takeBalanceValues(const uint8_t *rdram)
        {
            State &s = state();
            gp::takeGameValue(rdram, s.grindTip, sym::bmx::g_grindTip);
            gp::takeGameValue(rdram, s.grindLineTip, sym::bmx::g_grindLineTip);
            gp::takeGameValue(rdram, s.lipTip, sym::bmx::g_lipTip);
            gp::takeGameValue(rdram, s.flatlandTip, sym::bmx::g_flatlandTip);
            gp::takeGameValue(rdram, s.tipGrowth, sym::bmx::g_flatlandTipGrowth);
        }

        void applyAssists(uint8_t *rdram)
        {
            State &s = state();
            const int grind = option(kPerfectGrind) != 0 ? 0 : option(kGrindTip);
            const int manual = option(kPerfectManual) != 0 ? 0 : option(kManualTip);
            gp::apply(rdram, s.grindTip, scaled(s.grindTip, grind, 100));
            gp::apply(rdram, s.grindLineTip, scaled(s.grindLineTip, grind, 100));
            gp::apply(rdram, s.lipTip, scaled(s.lipTip, grind, 100));
            gp::apply(rdram, s.flatlandTip, scaled(s.flatlandTip, manual, 100));
            gp::apply(rdram, s.tipGrowth, scaled(s.tipGrowth, manual, 100));
            const int key = grind * 1000 + manual;
            if (key != s.assistsLogged)
            {
                s.assistsLogged = key;
                std::cout << "[bmx-xxx] balance: grind tip " << gp::readFloat(rdram, sym::bmx::g_grindTip) << ", line tip "
                          << gp::readFloat(rdram, sym::bmx::g_grindLineTip) << ", lip tip "
                          << gp::readFloat(rdram, sym::bmx::g_lipTip) << ", manual tip "
                          << gp::readFloat(rdram, sym::bmx::g_flatlandTip) << ", growth "
                          << gp::readFloat(rdram, sym::bmx::g_flatlandTipGrowth) << std::endl;
            }
        }

        // ---- unlock everything ----------------------------------------------------

        // The saved game (game/bmx/profile/players.h SavedGame, RiderCharacter,
        // LevelGoals) as the host writes it.
        constexpr uint32_t kSavedFeatures = 0x00000, kSavedCharacters = 0x00024, kSavedMovies = 0x21F60;
        constexpr uint32_t kCharacterBytes = 0x1FF4, kCharacterFlags = 0x0000, kCharacterBikes = 0x0043;
        constexpr uint32_t kCharacterLevels = 0x078E, kLevelGoalsBytes = 0x206, kLevelGoalsFlags = 0x05;
        constexpr uint32_t kRiderCharacters = 17, kLevels = 12, kMovies = 20, kFeatures = 6;
        constexpr uint16_t kCharacterLocked = 0x0001;
        constexpr uint8_t kLevelGoalsOpen = 0x01;
        constexpr uint32_t kMovieRewardBit0 = 0x1;
        // A row of the cheat codes (game/bmx/ui/menu_script.cpp CheatCode):
        // the code's text, what it opens and which one (-1: every one).
        constexpr uint32_t kCheatCodes = 54, kCheatCodeBytes = 0x08, kCheatOpensAt = 0x04, kCheatWhichAt = 0x05;
        constexpr uint8_t kCheatLevels = 0x01, kCheatMovies = 0x02, kCheatCharacters = 0x04, kCheatBikes = 0x08,
                          kCheatFeatures = 0x10;
        constexpr int8_t kCheatEveryOne = -1;
        constexpr uint8_t kEveryBike = 5;
        constexpr uint8_t kGameStateFrontEnd = 0; // game/bmx/states/game_state.h

        struct Unlocking
        {
            std::atomic<bool> request{false};
            std::atomic<bool> unlocked{false};
            std::atomic<int64_t> armedUntil{0}; // ms, steady clock
        };
        Unlocking &unlocking() { return ps2x::session::state<Unlocking>(); }

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void writeByte(uint8_t *rdram, uint32_t address, uint8_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                *p = value;
        }

        uint16_t readHalf(const uint8_t *rdram, uint32_t address)
        {
            uint16_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void writeHalf(uint8_t *rdram, uint32_t address, uint16_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        uint32_t character(uint32_t number) { return sym::bmx::g_savedGame + kSavedCharacters + number * kCharacterBytes; }
        bool opens(int8_t which, uint32_t index) { return which == kCheatEveryOne || index == static_cast<uint32_t>(which); }

        // Every row of the game's cheat codes, as bmxmenuscriptCheat applies
        // the one typed, but the special modes only opened (featuresOn is the
        // player's).
        void unlockAll(uint8_t *rdram)
        {
            const uint32_t saved = sym::bmx::g_savedGame;
            for (uint32_t row = 0; row < kCheatCodes; ++row)
            {
                const uint32_t code = sym::bmx::g_cheatCodes + row * kCheatCodeBytes;
                const uint8_t what = readByte(rdram, code + kCheatOpensAt);
                const int8_t which = static_cast<int8_t>(readByte(rdram, code + kCheatWhichAt));
                for (uint32_t n = 0; n < kRiderCharacters; ++n)
                {
                    const uint32_t c = character(n);
                    if (what & kCheatLevels)
                        for (uint32_t level = 0; level < kLevels; ++level)
                            if (opens(which, level))
                            {
                                const uint32_t at = c + kCharacterLevels + level * kLevelGoalsBytes + kLevelGoalsFlags;
                                writeByte(rdram, at, static_cast<uint8_t>(readByte(rdram, at) | kLevelGoalsOpen));
                            }
                    if ((what & kCheatCharacters) && opens(which, n))
                        writeHalf(rdram, c + kCharacterFlags, static_cast<uint16_t>(readHalf(rdram, c + kCharacterFlags) & ~kCharacterLocked));
                    if ((what & kCheatBikes) && opens(which, n))
                        writeByte(rdram, c + kCharacterBikes, kEveryBike);
                }
                if (what & kCheatMovies)
                    for (uint32_t movie = 0; movie < kMovies; ++movie)
                        if (opens(which, movie))
                            writeWord(rdram, saved + kSavedMovies + movie * 4u, readWord(rdram, saved + kSavedMovies + movie * 4u) | kMovieRewardBit0);
                if (what & kCheatFeatures)
                    for (uint32_t feature = 0; feature < kFeatures; ++feature)
                        if (opens(which, feature))
                            writeHalf(rdram, saved + kSavedFeatures,
                                      static_cast<uint16_t>(readHalf(rdram, saved + kSavedFeatures) | (1u << feature)));
            }
            std::cout << "[bmx-xxx] unlocked everything (every cheat code's levels, movies, riders, bikes and special "
                         "modes; in the saved game)"
                      << std::endl;
        }

        bool allUnlocked(const uint8_t *rdram)
        {
            for (uint32_t n = 0; n < kRiderCharacters; ++n)
            {
                const uint32_t c = character(n);
                if ((readHalf(rdram, c + kCharacterFlags) & kCharacterLocked) || readByte(rdram, c + kCharacterBikes) < kEveryBike)
                    return false;
                for (uint32_t level = 0; level < kLevels; ++level)
                    if (!(readByte(rdram, c + kCharacterLevels + level * kLevelGoalsBytes + kLevelGoalsFlags) & kLevelGoalsOpen))
                        return false;
            }
            return true;
        }

        void unlockOnVsync(uint8_t *rdram)
        {
            const uint8_t *gameState = getConstMemPtr(rdram, sym::bmx::g_currentGameState);
            if (gameState && *gameState == kGameStateFrontEnd && unlocking().request.exchange(false))
            {
                unlockAll(rdram);
                unlocking().unlocked = true;
            }
        }

        st::Setting unlockRow()
        {
            st::Setting unlock;
            unlock.key = "cheat.unlock_all";
            unlock.label = "Unlock Everything";
            unlock.kind = st::Kind::Choice;
            unlock.choices = {"No", "Sure?", "Unlocked"};
            unlock.help = "Opens all levels, riders, bikes and movies.";
            unlock.current = []()
            {
                PS2Runtime *runtime = state().runtime;
                if (unlocking().unlocked.load() || (runtime && allUnlocked(runtime->memory().getRDRAM())))
                    return 2;
                return unlocking().armedUntil.load() > nowMs() ? 1 : 0;
            };
            unlock.apply = [](int value)
            {
                if (value == 1)
                    unlocking().armedUntil = nowMs() + 5000;
                else if (value == 2 && unlocking().armedUntil.load() > nowMs())
                {
                    unlocking().armedUntil = 0;
                    unlocking().request = true;
                    unlocking().unlocked = true;
                }
                else
                    unlocking().armedUntil = 0;
            };
            return unlock;
        }

        // ---- hide HUD -----------------------------------------------------------

        // A zgui node (zge/zgui/widget.h, ZGE4's: AI's layout).
        constexpr uint32_t kNodeParent = 0x08u, kNodeName = 0x14u;

        bool nodeNamed(const uint8_t *rdram, uint32_t node, const char *name)
        {
            const uint32_t address = node ? readWord(rdram, node + kNodeName) : 0u;
            if (!address)
                return false;
            const size_t length = std::strlen(name) + 1u;
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p && getConstMemPtr(rdram, address + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
                   std::memcmp(p, name, length) == 0;
        }

        bool nodeNameStarts(const uint8_t *rdram, uint32_t node, const char *prefix)
        {
            const uint32_t address = node ? readWord(rdram, node + kNodeName) : 0u;
            const size_t length = std::strlen(prefix);
            const uint8_t *p = address ? getConstMemPtr(rdram, address) : nullptr;
            return p && getConstMemPtr(rdram, address + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
                   std::memcmp(p, prefix, length) == 0;
        }

        // The HUD's groups: the Overlay of each of the overlay's display
        // files (UI_overDisp_com: the timer and the riders' places;
        // UI_overDisp_P_full, a player's in one view, and the split
        // screen's: the trick, the balance, the score, the health, the
        // goals, the rewards, the hints, the exit warning). Their
        // OverlayCutScene (the subtitles) and OverlayReplay stay.
        bool hudGroup(const uint8_t *rdram, uint32_t widget)
        {
            if (!nodeNamed(rdram, widget, "Overlay"))
                return false;
            return nodeNameStarts(rdram, readWord(rdram, widget + kNodeParent), "UI_overDisp");
        }

        struct Hud
        {
            std::atomic<bool> hidden{false};
            std::atomic<uint32_t> skipped{0}; // groups not drawn
        };
        Hud &hud() { return ps2x::session::state<Hud>(); }

        void drawWidgetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Hud &h = hud();
            if (photo::hidesUi())
            {
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            if (h.hidden.load(std::memory_order_relaxed) && hudGroup(rdram, getRegU32(ctx, kRegA0)))
            {
                h.skipped.fetch_add(1u, std::memory_order_relaxed);
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            state().drawWidget(rdram, ctx, runtime);
        }

        void hudOnVsync(const uint8_t *rdram, bool keyPressed)
        {
            Hud &h = hud();
            if (!inLevel(rdram))
            {
                h.hidden = false; // out of the level: shown again next time
                return;
            }
            if (!keyPressed)
                return;
            h.hidden = !h.hidden.load();
            std::cout << "[bmx-xxx] HUD " << (h.hidden ? "hidden" : "shown") << " (" << h.skipped.exchange(0u)
                      << " HUD groups not drawn while hidden)" << std::endl;
        }

        // ---- every VBlank -------------------------------------------------------

        void onVsync(uint8_t *rdram, uint64_t)
        {
            State &s = state();
            s.keys.poll();
            hudOnVsync(rdram, s.keys.pressed(ps2_host_input::kHotkeyHideHud));
            if (s.keys.pressed(ps2_host_input::kHotkeyRestart) && inLevel(rdram))
                restart::requestKey();
            applyCamera(rdram);
            applyAssists(rdram);
            unlockOnVsync(rdram);
        }

        void registerSettings()
        {
            gp::Options &o = state().options;
            ps2x::session::addSetting(
                o.rangeRow(kFov, "Field of View", 42, 102, 5, "", "Wider shows more. The game's is 62."));
            ps2x::session::addSetting(
                o.rangeRow(kDistanceShare, "Camera Distance", 50, 250, 10, "%", "How far behind the rider."));
            ps2x::session::addSetting(
                o.rangeRow(kHeightShare, "Camera Height", 50, 300, 10, "%", "How high above the rider."));
            ps2x::session::addSetting(o.rangeRow(kGrindTip, "Grind Balance Loss", 0, 100, 25, "%",
                                                 "How fast a grind, stall or lip trick tips over."));
            ps2x::session::addSetting(o.rangeRow(kManualTip, "Manual Balance Loss", 0, 100, 25, "%",
                                                 "How fast a manual, wheelie or flatland trick tips over."));
            ps2x::session::addSetting(o.row(kPerfectGrind, "Perfect Grind", "Grinds, stalls and lip tricks never tip over."));
            ps2x::session::addSetting(o.row(kPerfectManual, "Perfect Manual", "Manuals, wheelies and flatland never tip over."));
            ps2x::session::addSetting(unlockRow());
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *on = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        State &s = state();
        s.runtime = &runtime;
        if (const char *file = ps2x::boot::text("PS2X_GAMEPLAY_INI"); file && file[0])
            s.options.setFile(file);
        s.options.load();
        takeCameraValues(rdram);
        takeBalanceValues(rdram);
        s.drawWidget = runtime.lookupFunction(sym::bmx::drawWidget);
        if (!s.drawWidget || !runtime.replaceFunction(sym::bmx::drawWidget, &drawWidgetHook))
        {
            std::cerr << "[bmx-xxx] gameplay: the widget draw hook not installed" << std::endl;
            return;
        }
        if (!installRestartKey(runtime))
            return;
        registerSettings();
        s.options.takeRowValues();
        photo::install(runtime);
        runtime.addVSyncHook(&onVsync);
        std::cout << "[bmx-xxx] gameplay: the camera, assists and cheats pages, the Hide HUD and Restart keys on (field of view " << option(kFov)
                  << ", distance " << option(kDistanceShare) << "%, height " << option(kHeightShare) << "%)"
                  << (s.options.file().empty() ? "" : " (" + s.options.file().string() + ")") << std::endl;
    }
}

namespace bmx_xxx
{
    void installGameplay(PS2Runtime &runtime)
    {
        gameplay::install(runtime);
    }

    float drawnFieldOfView(const uint8_t *, float gameDegrees)
    {
        const int setting = gameplay::option(gameplay::kFov);
        return setting == gameplay::kGameFov ? gameDegrees
                                             : gameDegrees * static_cast<float>(setting) / static_cast<float>(gameplay::kGameFov);
    }
}
