// Dave Mirra Freestyle BMX 2 (SLUS-20159): the gameplay pages and keys,
// Aggressive Inline's (aggressive_inline_gameplay.cpp) at DM2's own code.
// The options are kept in gameplay.ini and made settings rows by the shared
// host side (zge_gameplay.h); each is at the game's own value until picked,
// and at that value nothing here writes the game's memory.
//
// Camera. The follow camera (game/dm2/camera/follow_camera.cpp,
// followCamera) reads its settings table g_cameraSettings every frame: a row
// for each thing the camera can be doing (the ground cameras' kinds, a bail,
// at rest, the game's modes, then the four follow views the player picks:
// near, high, far ahead and the rider's eyes), each an offset in the rider's
// space (right, up, forward) and six rates, the last the field of view
// wanted in degrees, which the camera eases to. The page writes the table
// at VBlank, as AI's writes its tuning table:
// - Camera Distance: the behind views' offset forward (behind: negative),
//   as a share of the game's;
// - Camera Height: their offset up, and g_offGroundHeight, the height the
//   behind views take while the rider is in the air or on a grind. (A
//   coasting rider's 0.2 and an airborne contact's 1.5 are constants in the
//   follow camera's code and stay the game's.)
// - Field of View: every row's field of view, scaled by the setting over
//   the game's 62 (the first follow view's).
// The rider's-eyes view keeps its place. The camera code itself is not
// touched: followCamera is Tested, not Verified, so only the data it reads
// changes.
//
// Assists. DM2 has no juice meter (Aggressive Inline's assists are its
// rules); what a rider can fail at is the balance: a grind's (and a lip
// trick's) tips a little each frame by g_railTip (g_lipTip), the rider's
// balance stat against it, and off the rail past the most; a manual's or
// nose wheelie's tips by g_wheelieTip and, the longer it is held, by
// g_railTimeTip, and at the end of the balance the rider is down or bails
// (grindBalance, wheelieSubState: Verified). The two rows scale those rates
// as a share of the game's (100 %: the game's; 0 %: the balance stays where
// the rider holds it). g_railTimeTip is one word the game reads for both:
// a grind's tip also grows with its time by (1 + it), so Wheelie Balance
// Loss changes a long grind's tip by at most 5 %.
//
// Cheats. DM2's own codes (bmxCheckCheat, game/dm2/menu/cheats.cpp) are all
// unlocks, kept in the career save; it has no gameplay cheats. The page has
// Aggressive Inline's two that fit DM2: Perfect Grind (the grind's and lip
// trick's tips 0) and Perfect Manual (the manual's and nose wheelie's tips
// 0, its time tip with them), which leave the save alone; and Unlock
// Everything, which does what all the general codes do (every character's
// bikes, both looks, sponsors, levels and tricks, the hidden riders, the
// movies, the park editor's themes and objects) in the front end, so the
// save written next has it (two steps, as AI's). What the codes open of the
// extras it opens too: the Xbox levels, and Create Player's riders get what
// the game's riders get (dave_mirra.h, cheats).
//
// Hide HUD: the Hide HUD key (controls.ini [hotkeys] hide_hud) in a level
// hides the score, the timer, the trick, the tips, the goals and the
// messages: the two groups they are in, InGameUI.Overlay and
// InGameUI.OverlayToggle, are not drawn. zgui's widget draw (DM2's
// drawWidget, ZGE2's drawWidgetObject: a widget's scene object and what
// hangs from it) is wrapped to skip those two by their names and their
// parent's (InGameUI, or MultiplayerUI), as AI's. The pause menu and
// the round's screens (InGameUI's frames) stay. The HUD shows again when
// the level is left. The frames with no widget of their own (the ruler, the
// gauge, the replay's mark) draw outside the groups and stay.
//
// Restart: the Restart key in a level asks dave_mirra_restart.cpp for the
// turn again (requestKey), as the pause menu's Restart item.
//
// Photo mode (dave_mirra_photo.cpp) is installed with the pages; while it is
// on no widget is drawn.
//
// Settings are kept in ZX_GAMEPLAY_INI (the app: gameplay.ini in the
// game's support folder). On with the app's settings (ZX_GAME_SETTINGS_MENUS=1).

#include "dave_mirra.h"
#include "zge_gameplay.h"
#include "sym/game_overrides/dave_mirra_gameplay.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace dave_mirra::gameplay
{
    namespace
    {
        namespace gp = zge_host::gameplay;
        namespace st = zx::settings;

        // DM2's functions and data, by the names of its readable code
        // (runtime/tools/override_symbols.py).
        struct Symbols
        {
            uint32_t cameraSettings = sym::dm2::g_cameraSettings;   // CameraSettings rows, 0x30 bytes each
            uint32_t offGroundHeight = sym::dm2::g_offGroundHeight;
            uint32_t railTip = sym::dm2::g_railTip;
            uint32_t lipTip = sym::dm2::g_lipTip;
            uint32_t wheelieTip = sym::dm2::g_wheelieTip;
            uint32_t railTimeTip = sym::dm2::g_railTimeTip;         // the wheelie's time tip too
            uint32_t secretLevelUnlocked = sym::dm2::g_secretLevelUnlocked; // byte
            uint32_t openThemeCount = sym::dm2::g_openThemeCount;           // byte
            uint32_t riderCharacters = sym::dm2::g_riderCharacters; // RiderCharacter records
            uint32_t currentGameState = sym::dm2::g_currentGameState; // byte
            uint32_t drawWidget = sym::dm2::drawWidget;             // ZGE2's drawWidgetObject
        };
        constexpr Symbols kSym;

        // A CameraSettings row (game/dm2/camera/follow_camera.h).
        constexpr uint32_t kRowSize = 0x30u;
        constexpr uint32_t kOffsetUp = 0x04u, kOffsetForward = 0x08u;
        constexpr uint32_t kFieldOfView = 0x24u; // rates.fieldOfView
        // The rows the follow camera reads (kViewRows + its four views).
        constexpr uint32_t kRows = 26u;
        // The views behind the rider: near, high, far ahead (the fourth,
        // the rider's eyes, is at the rider).
        constexpr uint32_t kFirstBehindView = 22u, kBehindViews = 3u;
        constexpr int kGameFov = 62; // the first follow view's
        constexpr uint8_t kGameStateLevel = 1; // game/dm2/action/action.h
        constexpr int kRegA0 = 4, kRegRa = 31;

        enum Option
        {
            kFov,
            kDistance,
            kHeight,
            kGrindTip,
            kWheelieTip,
            kPerfectGrind,
            kPerfectManual,
            kOptionCount
        };
        const gp::OptionInfo kOptions[kOptionCount] = {
            {"camera.fov", kGameFov},
            {"camera.distance", 100},
            {"camera.height", 100},
            {"assist.grind_balance_loss", 100},
            {"assist.manual_balance_loss", 100, "assist.wheelie_balance_loss"}, // BMX XXX's key for the same row
            {"cheat.perfect_grind", 0},
            {"cheat.perfect_manual", 0},
        };
        constexpr const char *kHeader = "; Dave Mirra Freestyle BMX 2: the Camera, Assists and Cheats pages\n"
                                        "; (numbers, 0/1 for off/on).\n";

        struct State
        {
            gp::Options options;
            gp::Hotkeys keys;
            gp::GameFloat fov[kRows];
            gp::GameFloat distance[kBehindViews];
            gp::GameFloat height[kBehindViews];
            gp::GameFloat offGroundHeight;
            gp::GameFloat railTip, lipTip, wheelieTip, timeTip;
            int assistsLogged = -1;
            int cameraLogged = -1;
            PS2Runtime::RecompiledFunction drawWidget = nullptr;
            PS2Runtime *runtime = nullptr;
            State() { options.define(kOptions, kOptionCount, kHeader); }
        };
        State &state() { return zx::session::state<State>(); }

        int option(Option o) { return state().options.value(o); }

        float share(Option o) { return static_cast<float>(option(o)) / 100.0f; }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // ---- camera -----------------------------------------------------------

        void takeCameraValues(const uint8_t *rdram)
        {
            State &s = state();
            for (uint32_t row = 0; row < kRows; ++row)
                gp::takeGameValue(rdram, s.fov[row], kSym.cameraSettings + row * kRowSize + kFieldOfView);
            for (uint32_t view = 0; view < kBehindViews; ++view)
            {
                const uint32_t row = kSym.cameraSettings + (kFirstBehindView + view) * kRowSize;
                gp::takeGameValue(rdram, s.distance[view], row + kOffsetForward);
                gp::takeGameValue(rdram, s.height[view], row + kOffsetUp);
            }
            gp::takeGameValue(rdram, s.offGroundHeight, kSym.offGroundHeight);
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
            for (gp::GameFloat &fov : s.fov)
                gp::apply(rdram, fov, scaled(fov, option(kFov), kGameFov));
            for (uint32_t view = 0; view < kBehindViews; ++view)
            {
                gp::apply(rdram, s.distance[view], scaled(s.distance[view], option(kDistance), 100));
                gp::apply(rdram, s.height[view], scaled(s.height[view], option(kHeight), 100));
            }
            gp::apply(rdram, s.offGroundHeight, scaled(s.offGroundHeight, option(kHeight), 100));
            // The log reads the first view's row back when the options change.
            const int key = option(kFov) * 1000000 + option(kDistance) * 1000 + option(kHeight);
            if (key != s.cameraLogged)
            {
                s.cameraLogged = key;
                const uint32_t row = kSym.cameraSettings + kFirstBehindView * kRowSize;
                std::cout << "[dave-mirra] camera: near view forward " << gp::readFloat(rdram, row + kOffsetForward)
                          << " up " << gp::readFloat(rdram, row + kOffsetUp) << " field of view "
                          << gp::readFloat(rdram, row + kFieldOfView) << ", in the air "
                          << gp::readFloat(rdram, kSym.offGroundHeight) << std::endl;
            }
        }

        // ---- assists ----------------------------------------------------------------

        void takeBalanceValues(const uint8_t *rdram)
        {
            State &s = state();
            gp::takeGameValue(rdram, s.railTip, kSym.railTip);
            gp::takeGameValue(rdram, s.lipTip, kSym.lipTip);
            gp::takeGameValue(rdram, s.wheelieTip, kSym.wheelieTip);
            gp::takeGameValue(rdram, s.timeTip, kSym.railTimeTip);
        }

        void applyBalance(uint8_t *rdram)
        {
            State &s = state();
            const int grind = option(kPerfectGrind) != 0 ? 0 : option(kGrindTip);
            const int wheelie = option(kPerfectManual) != 0 ? 0 : option(kWheelieTip);
            gp::apply(rdram, s.railTip, scaled(s.railTip, grind, 100));
            gp::apply(rdram, s.lipTip, scaled(s.lipTip, grind, 100));
            gp::apply(rdram, s.wheelieTip, scaled(s.wheelieTip, wheelie, 100));
            gp::apply(rdram, s.timeTip, scaled(s.timeTip, wheelie, 100));
            const int key = grind * 1000 + wheelie;
            if (key != s.assistsLogged)
            {
                s.assistsLogged = key;
                std::cout << "[dave-mirra] balance: rail tip " << gp::readFloat(rdram, kSym.railTip) << ", lip tip "
                          << gp::readFloat(rdram, kSym.lipTip) << ", wheelie tip " << gp::readFloat(rdram, kSym.wheelieTip)
                          << ", time tip " << gp::readFloat(rdram, kSym.railTimeTip) << std::endl;
            }
        }

        // ---- unlock everything ----------------------------------------------------

        // A RiderCharacter record (game/dm2/game/players.h).
        constexpr uint32_t kCharacterSize = 0x3C8u;
        constexpr uint32_t kCharacterFlags = 0x000u, kOpenBikes = 0x064u, kOpenLooks = 0x065u;
        constexpr uint32_t kSponsorShown = 0x068u, kLevels = 0x13Cu, kTricksOff = 0x3A0u;
        constexpr uint32_t kLevelSize = 0x44u, kLevelFlags = 0x3Cu, kTricksOffBytes = 0x14u;
        constexpr uint32_t kCharacterLocked = 0x1u, kCharacterMovieOpen = 0x2u;
        constexpr uint8_t kLevelOpen = 0x01u;
        // As bmxCheckCheat's general codes (cheats.cpp); the characters they
        // reach are cheats::ridersCovered().
        constexpr uint32_t kMovieCharacters = 16, kFirstHidden = 15, kSecondHidden = 16;
        constexpr uint32_t kSponsors = 8, kRespectLevels = 8;
        constexpr uint8_t kAllThemesOpen = 10, kAllSecretLevels = 14, kAllBikesOpen = 5, kAllLooksOpen = 2;

        struct Unlocking
        {
            std::atomic<bool> request{false};
            std::atomic<bool> unlocked{false};
            std::atomic<int64_t> armedUntil{0}; // ms, steady clock
        };
        Unlocking &unlocking() { return zx::session::state<Unlocking>(); }

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        void writeByte(uint8_t *rdram, uint32_t address, uint8_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                *p = value;
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        uint32_t character(uint32_t number) { return kSym.riderCharacters + number * kCharacterSize; }

        void unlockAll(uint8_t *rdram)
        {
            writeByte(rdram, kSym.openThemeCount, kAllThemesOpen);
            writeByte(rdram, kSym.secretLevelUnlocked, kAllSecretLevels);
            // The game's codes' characters, and the extras' riders and levels
            // as the codes open them (dave_mirra.h, cheats).
            for (uint32_t n = 0; n < cheats::ridersCovered(); ++n)
            {
                const uint32_t c = character(n);
                uint32_t flags = readWord(rdram, c + kCharacterFlags);
                if (n < kMovieCharacters)
                    flags |= kCharacterMovieOpen;
                if (n == kFirstHidden || n == kSecondHidden)
                    flags &= ~kCharacterLocked;
                writeWord(rdram, c + kCharacterFlags, flags);
                writeByte(rdram, c + kOpenBikes, kAllBikesOpen);
                writeByte(rdram, c + kOpenLooks, kAllLooksOpen);
                for (uint32_t sponsor = 0; sponsor <= kSponsors; ++sponsor)
                    writeWord(rdram, c + kSponsorShown + 4u * sponsor, 1u);
                for (uint32_t level = 0; level < kRespectLevels; ++level)
                {
                    const uint32_t at = c + kLevels + level * kLevelSize + kLevelFlags;
                    writeByte(rdram, at, static_cast<uint8_t>(readByte(rdram, at) | kLevelOpen));
                }
                for (uint32_t i = 0; i < kTricksOffBytes; ++i)
                    writeByte(rdram, c + kTricksOff + i, 0u);
            }
            cheats::openXboxLevels(rdram);
            std::cout << "[dave-mirra] unlocked everything (every character's bikes, looks, sponsors, levels and tricks, "
                         "the hidden riders, the movies, the park editor's themes and objects; in the career save)"
                      << std::endl;
        }

        bool allUnlocked(const uint8_t *rdram)
        {
            if (readByte(rdram, kSym.openThemeCount) != kAllThemesOpen ||
                readByte(rdram, kSym.secretLevelUnlocked) != kAllSecretLevels)
                return false;
            for (uint32_t n = 0; n < cheats::ridersCovered(); ++n)
            {
                const uint32_t c = character(n);
                if (readByte(rdram, c + kOpenBikes) != kAllBikesOpen || readByte(rdram, c + kOpenLooks) != kAllLooksOpen ||
                    ((n == kFirstHidden || n == kSecondHidden) && (readWord(rdram, c + kCharacterFlags) & kCharacterLocked)))
                    return false;
                for (uint32_t level = 0; level < kRespectLevels; ++level)
                    if (!(readByte(rdram, c + kLevels + level * kLevelSize + kLevelFlags) & kLevelOpen))
                        return false;
            }
            return true;
        }

        constexpr uint8_t kGameStateFrontEnd = 0; // game/dm2/action/action.h

        void unlockOnVsync(uint8_t *rdram)
        {
            const uint8_t *gameState = getConstMemPtr(rdram, kSym.currentGameState);
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
            unlock.help = "Opens all bikes, levels, riders and movies.";
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

        bool inLevel(const uint8_t *rdram)
        {
            const uint8_t *p = getConstMemPtr(rdram, kSym.currentGameState);
            return p && *p == kGameStateLevel;
        }

        // A ZGE2 widget is a scene object (kind 4, the shown bit) with its
        // record at +0x3C, whose +0x4C is the widget's Name; what it hangs
        // from is its attachment's (+0x30) parent (zge/scene/scene.h,
        // zge/zgui/widget.h).
        constexpr uint32_t kObjectAttachment = 0x30u, kObjectRecord = 0x3Cu;
        constexpr uint32_t kAttachmentParent = 0x00u, kRecordName = 0x4Cu;
        constexpr uint32_t kObjectKindMask = 0xFu, kWidgetObjectKind = 4u;

        bool widgetNamed(const uint8_t *rdram, uint32_t object, const char *name)
        {
            if (!object || (readWord(rdram, object) & kObjectKindMask) != kWidgetObjectKind)
                return false;
            const uint32_t record = readWord(rdram, object + kObjectRecord);
            const uint32_t text = record ? readWord(rdram, record + kRecordName) : 0u;
            const size_t length = std::strlen(name) + 1u;
            const uint8_t *p = text ? getConstMemPtr(rdram, text) : nullptr;
            return p && getConstMemPtr(rdram, text + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
                   std::memcmp(p, name, length) == 0;
        }

        uint32_t widgetParent(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t attachment = readWord(rdram, object + kObjectAttachment);
            return attachment ? readWord(rdram, attachment + kAttachmentParent) : 0u;
        }

        // The HUD's groups: InGameUI's (MultiplayerUI's) Overlay and
        // OverlayToggle, as AI's.
        bool hudGroup(const uint8_t *rdram, uint32_t object)
        {
            if (!widgetNamed(rdram, object, "Overlay") && !widgetNamed(rdram, object, "OverlayToggle"))
                return false;
            const uint32_t parent = widgetParent(rdram, object);
            return widgetNamed(rdram, parent, "InGameUI") || widgetNamed(rdram, parent, "MultiplayerUI");
        }

        struct Hud
        {
            std::atomic<bool> hidden{false};
            std::atomic<uint32_t> skipped{0}; // groups not drawn
        };
        Hud &hud() { return zx::session::state<Hud>(); }

        void drawWidgetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Hud &h = hud();
            // Photo mode: no widget at all (the pause menu, the HUD).
            if (photo::hidesUi())
            {
                setReturnU32(ctx, 1u);
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            if (h.hidden.load(std::memory_order_relaxed) && hudGroup(rdram, getRegU32(ctx, kRegA0)))
            {
                h.skipped.fetch_add(1u, std::memory_order_relaxed);
                setReturnU32(ctx, 1u);
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
            std::cout << "[dave-mirra] HUD " << (h.hidden ? "hidden" : "shown") << " (" << h.skipped.exchange(0u)
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
            applyBalance(rdram);
            unlockOnVsync(rdram);
        }

        // ---- registration -------------------------------------------------------

        void registerSettings()
        {
            gp::Options &o = state().options;
            zx::session::addSetting(
                o.rangeRow(kFov, "Field of View", 42, 102, 5, "", "Wider shows more. The game's is 62."));
            zx::session::addSetting(
                o.rangeRow(kDistance, "Camera Distance", 50, 250, 10, "%", "How far behind the rider."));
            zx::session::addSetting(
                o.rangeRow(kHeight, "Camera Height", 50, 300, 10, "%", "How high above the rider."));
            zx::session::addSetting(o.rangeRow(kGrindTip, "Grind Balance Loss", 0, 100, 25, "%",
                                                 "How fast a grind or lip trick tips over."));
            zx::session::addSetting(o.rangeRow(kWheelieTip, "Manual Balance Loss", 0, 100, 25, "%",
                                                 "How fast a manual or nose wheelie tips over."));
            zx::session::addSetting(o.row(kPerfectGrind, "Perfect Grind", "Grinds and lip tricks never tip over."));
            zx::session::addSetting(o.row(kPerfectManual, "Perfect Manual", "Manuals and nose wheelies never tip over."));
            zx::session::addSetting(unlockRow());
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *on = zx::boot::text("ZX_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        State &s = state();
        s.runtime = &runtime;
        if (const char *file = zx::boot::text("ZX_GAMEPLAY_INI"); file && file[0])
            s.options.setFile(file);
        s.options.load();
        takeCameraValues(rdram);
        takeBalanceValues(rdram);
        s.drawWidget = runtime.lookupFunction(kSym.drawWidget);
        if (!s.drawWidget || !runtime.replaceFunction(kSym.drawWidget, &drawWidgetHook))
        {
            std::cerr << "[dave-mirra] gameplay: the widget draw hook not installed" << std::endl;
            return;
        }
        if (!installRestartKey(runtime))
            return;
        registerSettings();
        photo::install(runtime);
        s.options.takeRowValues();
        runtime.addVSyncHook(&onVsync);
        std::cout << "[dave-mirra] gameplay: camera, assists and cheats pages, Hide HUD and Restart keys on (field of view " << option(kFov) << ", distance "
                  << option(kDistance) << "%, height " << option(kHeight) << "%)"
                  << (s.options.file().empty() ? "" : " (" + s.options.file().string() + ")") << std::endl;
    }
}

namespace dave_mirra
{
    void installGameplay(PS2Runtime &runtime)
    {
        gameplay::install(runtime);
    }
}
