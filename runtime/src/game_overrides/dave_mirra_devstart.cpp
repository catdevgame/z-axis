// Dave Mirra Freestyle BMX 2 (SLUS-20159): a developer start straight into
// a FREERIDE level, Aggressive Inline's devstart at DM2's menus
// (aggressive_inline_devstart.cpp): developer runs without menus, and the
// collection's Custom Freeplay starting the game into its map's slot.
//
//   ZX_DEV_START_LEVEL=<n>|PARK   the level select's value (0 Woodward
//                                   Camp .. 7 Airport, the Xbox levels after
//                                   the park), or PARK: value 8, the park
//                                   (the memory card's, or ZX_DM2_PARK_FILE's:
//                                   dave_mirra_saves.cpp)
//   ZX_DEV_START_SKATER=<n>       the rider choice's entry (default 0, the
//                                   first bike), or "ask": the game's rider
//                                   and bike choices wait for the player's
//                                   own picks, then the route goes on
//   ZX_DEV_START_QUIT=<s>         stop the runner s seconds after the rider
//                                   is placed
//   ZX_DM2_KEEP_LEVEL_RECORDS=1   the row's level's saved records (its
//                                   bests and who set them: g_savedRecords,
//                                   game/dm2/action/saved_records.h) kept as
//                                   the player's save had them: put back
//                                   whenever the ride sets one (a custom
//                                   map's, whose bests are not the level's)
//
// The presses are the game's own pad input (PSPadBackend::
// setOverrideButtons), each step waiting on the game's state: Start
// through the intro's movies while the game is in its intro state (its
// card check and auto load run by themselves to the main menu: the front
// end state, menu mode kMenuFrontEnd, zgui's focus on the main menu's
// frame; no Start once there, it would take the menu's item), FREERIDE
// (two down, Cross), the rider choice, the bike choice, the
// modifiers (Cross), the level select (its entry stepped to the value, as
// the menu's CurrentLevel shows it), Cross; then the level state with the
// rider placed. Each screen is a new zgui focus widget, as a new panel is
// in AI's devstart. Logged as "[devstart] ...", with the time from the
// level's Cross to the placed rider (the load).
//
// A level the player's characters have not opened (only Woodward Camp is
// open in a fresh game; ProQuest opens the others) is locked in the select
// (bmxSetupLevelStats plays LockLevel). For the pick the row's level is
// opened in every character's record of the saved game in memory, and the
// bits are put back as they were once the level state has begun, so a save
// made after keeps the player's own progress (BMX XXX's devstart does the
// same). ZX_DEV_UNLOCK_ALL=1 opens every level for good
// (dave_mirra_devlevels.cpp).
//
// The values are the boot's (zx::boot::text: the environment, else what
// the app gave this boot: collection/maps.h mapSlots).

#include "dave_mirra_devstart.h"
#include "sym/game_overrides/dave_mirra_devstart.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace dave_mirra::devstart
{
    namespace
    {
        // The game's states (game/dm2/action/game_state.cpp): the front end,
        // the level being ridden.
        constexpr uint8_t kGameStateFrontEnd = 0, kGameStateLevel = 1, kGameStateIntro = 3;
        // The menu mode of the front end's menus (menu/menu_mode.h): the
        // intro's modes come before it.
        constexpr uint32_t kMenuFrontEnd = 0;
        // zgui's focus widget follows its root in zgui's globals
        // (zge/zgui/widget.h Zgui: root +0x0C, focus +0x10).
        constexpr uint32_t kZguiFocusAfterRoot = 4u;
        // The level select's entry for the park (menu/level_menu.h: the
        // menu's CurrentLevel; the nine entries' game levels in
        // g_levelMenuGameLevels).
        constexpr uint32_t kParkEntry = 8u;
        constexpr uint32_t kMainMenuFreeRide = 2u; // FREERIDE: the main menu's third item
        // The characters' records (game/dm2/game/players.h RiderCharacter,
        // 21 of 0x3C8 bytes): each level's LevelGoals (0x44 bytes from
        // +0x13C), its flags at +0x3C, kLevelOpen the level choice's bit. A
        // level select value's game level is g_levelMenuGameLevels's byte.
        constexpr uint32_t kCharacters = 21u, kCharacterBytes = 0x3C8u;
        constexpr uint32_t kCharacterLevels = 0x13Cu, kLevelGoalsBytes = 0x44u, kLevelGoalsFlags = 0x3Cu;
        constexpr uint8_t kLevelOpen = 0x01u;
        constexpr uint32_t kGameLevels = 8u; // DM2's own levels, 0 .. 7 (the park's entry has none)
        // A level's saved records (game/dm2/game/records.h kRecordsPerSpan of
        // SavedRecord, 0x14 bytes each).
        constexpr uint32_t kLevelRecordsBytes = 27u * 0x14u;
        // The rider record's physics position (game/dm2/rider/rider.h, as the
        // panel reads it) and how far the physics has run.
        constexpr uint32_t kRiderPosition = 0x1040u;
        constexpr uint32_t kRiderBytes = 0x1700u;
        constexpr uint32_t kPlacedFrames = 2u;

        constexpr uint16_t kLeft = 0x0080u, kDown = 0x0040u, kRight = 0x0020u, kStart = 0x0008u;
        constexpr uint16_t kCross = 0x4000u, kTriangle = 0x1000u;

        enum class Step
        {
            Boot,
            MainMenu,
            WaitFocus,
            RiderChoice,
            BikeChoice,
            Modifiers,
            LevelSelect,
            Loading,
            Placed,
            Done
        };

        struct State
        {
            Step step = Step::Boot;
            Step afterFocus = Step::Done;
            std::string level;    // ZX_DEV_START_LEVEL
            int entry = -1;       // the level select's value, once known
            uint32_t rider = 0;
            bool askRider = false; // "ask": the player picks the rider and the bike
            double quitAfter = -1.0;
            int tries = 0;
            int downs = 0;
            int bootPresses = 0;
            uint32_t oldFocus = 0, lastFocus = 0;
            uint64_t waitUntil = 0;  // no step before this vsync
            uint64_t releaseAt = 0;  // the press's buttons up at this vsync
            uint16_t pressed = 0;
            uint64_t focusDeadline = 0;
            uint64_t settledAt = 0;
            uint64_t bootDeadline = 0;
            bool opening = false;              // the row's level opened in the characters' records for the pick
            std::vector<uint8_t> openedBefore; // each character's open bit as it was (0: it was shut)
            uint32_t gameLevel = 0;            // the row's game level
            bool keepRecords = false;          // ZX_DM2_KEEP_LEVEL_RECORDS
            std::vector<uint8_t> recordsBefore; // the level's saved records as the save had them
            int recordsPutBack = 0;
            std::chrono::steady_clock::time_point crossed, placed;
            PS2Runtime *runtime = nullptr;
        };
        State &devStart() { return zx::session::state<State>(); }

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        uint8_t byte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        float real(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        uint32_t focus(const uint8_t *rdram) { return word(rdram, sym::dm2::g_uiRoot + kZguiFocusAfterRoot); }

        void apply() { PSPadBackend::setOverrideButtons(devStart().pressed); }

        void press(uint16_t mask, uint64_t now, uint64_t hold = 6, uint64_t gap = 30)
        {
            devStart().pressed = mask;
            devStart().releaseAt = now + hold;
            devStart().waitUntil = now + hold + gap;
            apply();
        }

        double seconds(std::chrono::steady_clock::time_point from)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        }

        void fail(const char *what)
        {
            std::cerr << "[devstart] " << what << ": stopped" << std::endl;
            devStart().pressed = 0;
            apply();
            devStart().step = Step::Done;
        }

        // A new focus widget that stays the focus for two checks (20 vsyncs
        // apart): the next screen is up.
        void waitFocus(const uint8_t *rdram, uint64_t now, Step next, uint64_t deadline = 1200u)
        {
            devStart().oldFocus = focus(rdram);
            devStart().lastFocus = 0;
            devStart().afterFocus = next;
            devStart().focusDeadline = deadline == ~0ull ? ~0ull : now + deadline;
            devStart().step = Step::WaitFocus;
        }

        int resolveEntry(const std::string &level)
        {
            if (!level.empty() && std::isdigit(static_cast<unsigned char>(level[0])))
                return std::atoi(level.c_str());
            std::string upper;
            for (char c : level)
                upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return upper == "PARK" ? static_cast<int>(kParkEntry) : -1;
        }

        uint32_t levelFlags(uint32_t character, uint32_t level)
        {
            return sym::dm2::g_riderCharacters + character * kCharacterBytes + kCharacterLevels + level * kLevelGoalsBytes + kLevelGoalsFlags;
        }

        // The row's level open in every character's record, the bits as they
        // were kept for putBack. The park's entry has no level record.
        void openLevel(uint8_t *rdram)
        {
            State &s = devStart();
            if (s.entry < 0 || s.entry >= static_cast<int>(kParkEntry))
                return;
            if (s.openedBefore.empty())
            {
                s.gameLevel = byte(rdram, sym::dm2::g_levelMenuGameLevels + static_cast<uint32_t>(s.entry));
                if (s.gameLevel >= kGameLevels)
                    return;
                for (uint32_t character = 0; character < kCharacters; ++character)
                    s.openedBefore.push_back(byte(rdram, levelFlags(character, s.gameLevel)) & kLevelOpen);
                s.opening = true;
            }
            for (uint32_t character = 0; character < kCharacters; ++character)
                if (uint8_t *flags = getMemPtr(rdram, levelFlags(character, s.gameLevel)))
                    *flags |= kLevelOpen;
        }

        uint32_t levelRecords(uint32_t level) { return sym::dm2::g_savedRecords + level * kLevelRecordsBytes; }

        // The row's level's saved records kept (once the level is known).
        void keepRecords(const uint8_t *rdram)
        {
            State &s = devStart();
            if (!s.keepRecords || !s.recordsBefore.empty() || s.openedBefore.empty())
                return;
            for (uint32_t i = 0; i < kLevelRecordsBytes; ++i)
                s.recordsBefore.push_back(byte(rdram, levelRecords(s.gameLevel) + i));
        }

        // Every vsync: the kept records as they were, whatever the ride set.
        void putRecordsBack(uint8_t *rdram, uint64_t)
        {
            State &s = devStart();
            uint8_t *records = s.recordsBefore.empty() ? nullptr : getMemPtr(rdram, levelRecords(s.gameLevel));
            if (!records || std::memcmp(records, s.recordsBefore.data(), kLevelRecordsBytes) == 0)
                return;
            std::memcpy(records, s.recordsBefore.data(), kLevelRecordsBytes);
            if (s.recordsPutBack++ == 0)
                std::cout << "[devstart] level " << s.gameLevel << "'s saved records put back as they were (the ride set its own)"
                          << std::endl;
        }

        // The bits openLevel set put back: the records as the player's game had them.
        void putBack(uint8_t *rdram)
        {
            State &s = devStart();
            if (!s.opening)
                return;
            int shut = 0;
            for (uint32_t character = 0; character < kCharacters && character < s.openedBefore.size(); ++character)
                if (!s.openedBefore[character])
                    if (uint8_t *flags = getMemPtr(rdram, levelFlags(character, s.gameLevel)))
                    {
                        *flags = static_cast<uint8_t>(*flags & ~kLevelOpen);
                        ++shut;
                    }
            s.opening = false;
            std::cout << "[devstart] level " << s.gameLevel << " shut again for " << shut << " character(s), as the save had it" << std::endl;
        }

        bool mainMenuUp(const uint8_t *rdram)
        {
            return byte(rdram, sym::dm2::g_currentGameState) == kGameStateFrontEnd && word(rdram, sym::dm2::g_menuMode) == kMenuFrontEnd &&
                   focus(rdram) != 0u;
        }

        bool riderPlaced(const uint8_t *rdram)
        {
            if (byte(rdram, sym::dm2::g_currentGameState) != kGameStateLevel || word(rdram, sym::dm2::g_physicsTaskFrames) <= kPlacedFrames)
                return false;
            const uint32_t rider = word(rdram, sym::dm2::g_riders);
            if (!getConstMemPtr(rdram, rider) || !getConstMemPtr(rdram, rider + kRiderBytes - 1u))
                return false;
            const float y = real(rdram, rider + kRiderPosition + 4u);
            return y == y; // a number: the physics has placed it
        }

        void onVsync(uint8_t *rdram, uint64_t now)
        {
            State &s = devStart();
            if (s.step == Step::Done || !rdram)
                return;
            if (s.pressed && now >= s.releaseAt)
            {
                s.pressed = 0;
                apply();
            }
            if (s.opening && s.step != Step::Loading && s.step != Step::Placed)
                openLevel(rdram);
            if (now < s.waitUntil)
                return;
            switch (s.step)
            {
            case Step::Boot:
                // Start through the movies while the game is in its intro
                // state; its card check and auto load run by themselves to
                // the main menu, settled 90 vsyncs.
                if (mainMenuUp(rdram))
                {
                    s.waitUntil = now + 90u;
                    s.step = Step::MainMenu;
                    std::cout << "[devstart] main menu after " << s.bootPresses << " Start press(es)" << std::endl;
                    return;
                }
                if (s.bootDeadline == 0)
                    s.bootDeadline = now + 9000u;
                else if (now > s.bootDeadline)
                {
                    fail("no main menu");
                    return;
                }
                if (now >= 400u && byte(rdram, sym::dm2::g_currentGameState) == kGameStateIntro && s.bootPresses < 30)
                {
                    ++s.bootPresses;
                    press(kStart, now, 10, 90);
                }
                return;
            case Step::MainMenu:
                // The row's level opened for the pick from here on (a save
                // loaded later is opened too: every vsync until the load).
                if (s.entry < 0)
                    s.entry = resolveEntry(s.level);
                openLevel(rdram);
                keepRecords(rdram);
                // FREERIDE: the third item, then Cross.
                if (s.downs < static_cast<int>(kMainMenuFreeRide))
                {
                    ++s.downs;
                    press(kDown, now, 6, 40);
                    return;
                }
                press(kCross, now);
                waitFocus(rdram, now, Step::RiderChoice);
                return;
            case Step::WaitFocus:
            {
                const uint32_t f = focus(rdram);
                if (f && f != s.oldFocus && f == s.lastFocus)
                {
                    s.tries = 0;
                    s.step = s.afterFocus;
                    return;
                }
                s.lastFocus = f;
                s.waitUntil = now + 20u;
                if (now > s.focusDeadline)
                    fail("no next screen");
                return;
            }
            case Step::RiderChoice:
                if (s.askRider)
                {
                    // The player's own rider and bike: nothing pressed, no
                    // deadline; the bike choice follows the player's Cross.
                    if (s.tries++ == 0)
                        std::cout << "[devstart] rider choice: waiting for the player" << std::endl;
                    waitFocus(rdram, now, Step::BikeChoice, ~0ull);
                    return;
                }
                if (s.tries < static_cast<int>(s.rider))
                {
                    ++s.tries;
                    press(kRight, now, 6, 60);
                    return;
                }
                press(kCross, now);
                waitFocus(rdram, now, Step::BikeChoice);
                return;
            case Step::BikeChoice:
                if (s.askRider)
                {
                    if (s.tries++ == 0)
                        std::cout << "[devstart] bike choice: waiting for the player" << std::endl;
                    waitFocus(rdram, now, Step::Modifiers, ~0ull);
                    return;
                }
                press(kCross, now);
                waitFocus(rdram, now, Step::Modifiers);
                return;
            case Step::Modifiers:
                // The modifiers as they come.
                s.waitUntil = now + 60u;
                press(kCross, now, 6, 60);
                waitFocus(rdram, now, Step::LevelSelect);
                return;
            case Step::LevelSelect:
            {
                if (s.entry < 0 && (s.entry = resolveEntry(s.level)) < 0)
                {
                    std::cerr << "[devstart] no level select value for " << s.level << std::endl;
                    fail("level");
                    return;
                }
                const uint32_t at = word(rdram, sym::dm2::g_currentLevel);
                if (at != static_cast<uint32_t>(s.entry))
                {
                    if (++s.tries > 40)
                    {
                        std::cerr << "[devstart] the level select stays at " << at << ", not " << s.entry << std::endl;
                        fail("menu");
                        return;
                    }
                    s.settledAt = 0;
                    press(at < static_cast<uint32_t>(s.entry) ? kRight : kLeft, now, 6, 40);
                    return;
                }
                if (s.settledAt == 0)
                {
                    // The entry's preview settles first.
                    s.settledAt = now + 60u;
                    s.waitUntil = s.settledAt;
                    return;
                }
                std::cout << "[devstart] level select value " << s.entry << " (" << s.level << ")" << std::endl;
                press(kCross, now);
                s.crossed = std::chrono::steady_clock::now();
                s.waitUntil = now + 60u;
                s.step = Step::Loading;
                return;
            }
            case Step::Loading:
                if (s.opening && byte(rdram, sym::dm2::g_currentGameState) == kGameStateLevel)
                    putBack(rdram);
                if (riderPlaced(rdram))
                {
                    s.placed = std::chrono::steady_clock::now();
                    std::cout << "[devstart] placed in level " << word(rdram, sym::dm2::g_currentGameLevel) << " (" << s.level
                              << "): load " << seconds(s.crossed) << " s from the level select's Cross" << std::endl;
                    s.step = Step::Placed;
                }
                return;
            case Step::Placed:
                if (s.quitAfter >= 0.0 && seconds(s.placed) >= s.quitAfter)
                {
                    std::cout << "[devstart] done: stopping the runner" << std::endl;
                    s.step = Step::Done;
                    if (s.runtime)
                        s.runtime->requestStop();
                }
                return;
            case Step::Done:
                return;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *level = zx::boot::text("ZX_DEV_START_LEVEL");
        if (!level || !*level)
            return;
        State &s = devStart();
        s.level = level;
        if (const char *rider = zx::boot::text("ZX_DEV_START_SKATER"); rider && *rider)
        {
            s.askRider = std::strcmp(rider, "ask") == 0;
            s.rider = s.askRider ? 0u : static_cast<uint32_t>(std::atoi(rider));
        }
        if (const char *quit = zx::boot::text("ZX_DEV_START_QUIT"); quit && *quit)
            s.quitAfter = std::atof(quit);
        if (const char *keep = zx::boot::text("ZX_DM2_KEEP_LEVEL_RECORDS"); keep && keep[0] == '1')
        {
            s.keepRecords = true;
            runtime.addVSyncHook(&putRecordsBack);
        }
        s.runtime = &runtime;
        runtime.addVSyncHook([](uint8_t *rdram, uint64_t vsync) { onVsync(rdram, vsync); });
        std::cout << "[devstart] on: FREERIDE level " << s.level << ", rider "
                  << (s.askRider ? std::string("chosen by the player") : std::to_string(s.rider)) << std::endl;
    }
}

namespace dave_mirra
{
    void installDevStart(PS2Runtime &runtime)
    {
        devstart::install(runtime);
    }
}
