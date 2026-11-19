// BMX XXX (SLUS-20415): a developer start straight into a HARDCORE TOUR
// level, Aggressive Inline's devstart at BMX XXX's menus
// (aggressive_inline_devstart.cpp): developer runs without menus, and the
// collection's Custom Freeplay starting the game into its map's slot.
//
//   PS2X_DEV_START_LEVEL=<n>|<folder>  the level select's row (0 the first,
//                                      the Bronx), or a level's folder name
//                                      (CMP_RMPG): the row that lists it,
//                                      from the game's own level table
//   PS2X_DEV_START_SKATER=<n>          the rider select's entry (default 0),
//                                      or "ask": the game's rider select
//                                      waits for the player's own pick, then
//                                      the route goes on
//   PS2X_DEV_START_QUIT=<s>            stop the runner s seconds after the
//                                      rider is placed
//   PS2X_BMX_KEEP_LEVEL_GOALS=1        the row's level goal counts kept for
//                                      the player's record and put back once
//                                      the ride is over (a custom map's)
//
// The presses are the game's own pad input (PSPadBackend::
// setOverrideButtons), each step waiting on the front end's menu machine
// (game/bmx/ui/menu.h: the menu that has the screen, steady): Start on the
// title until the main menu, Cross on HARDCORE TOUR (its first item), the
// rider select, the level select (right to the row, Cross), the level load,
// then the level state with the rider placed. Logged as "[devstart] ...",
// with the time from the level's Cross to the placed rider (the load).
//
// The level's own screens before the ride (overlay/screens.h: the
// instructions on a first visit, the objectives list) are taken with
// Cross, as AI's devstart closes its message boxes; the rider is placed
// once play has the screen.
//
// A level the player's characters have not opened is marked disabled in the
// select (bmxmenuscriptGetLevelNames): for the pick, the row's level is
// opened in every character's record of the saved game in memory from the
// main menu on, and the bits are put back as they were once the level
// state has begun, so a save made after keeps the player's own progress.
//
// A level's goals are counted into the player's record of it as the level
// loads (loadLevelObjectives: the goals and gaps its OBJCTV.TXT has) and as
// the turn starts (resetTurnGoals: those done). A custom map in a level's
// slot has goals of its own (none, as bmx_level_convert.py writes it), so
// with PS2X_BMX_KEEP_LEVEL_GOALS every character's four counts of the row's
// level are kept at the pick and put back on the screens a ride ends on
// (game over, saving, the exit questions, the fade out) and in the menus:
// a save and the select show the player's own progress in that level.
// The marks of the goals done are not touched by a file without goals.
//
// The values are the boot's (ps2x::boot::text: the environment, else what
// the app gave this boot: collection/maps.h mapSlots).

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_devstart.cpp.h"

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

namespace bmx_xxx::devstart
{
    namespace
    {
        // The game's states (game/bmx/states/game_state.h) and the level
        // state's running phase.
        constexpr uint8_t kGameStateFrontEnd = 0, kGameStateLevel = 1, kGameStateIntro = 5;
        constexpr uint8_t kStateRunning = 0;
        // The level's screens (game/bmx/overlay/screens.h): play, and the
        // two that come before the ride and wait for Cross.
        constexpr uint32_t kScreenPlay = 1, kScreenInstructions = 25, kScreenObjectives = 26;
        // The screens a ride ends on, where the game can save or leave for
        // its menus: the fade out, the exit questions, game over, saving.
        constexpr uint32_t kScreenLeave = 4, kScreenExitGame = 17, kScreenExitLevel = 18, kScreenGameOver = 22, kScreenSaveGame = 23;
        // The front end's menus (game/bmx/states/front_end_menus.h) and the
        // menu machine's steady phase (ui/menu.cpp).
        constexpr int32_t kMenuStart = 0, kMenuPressStart = 1, kMenuTitleLanguage = 2, kMenuDemoLoop = 3, kMenuMainMenu = 4;
        constexpr int32_t kMenuRiderSelect = 7, kMenuLevelSelect = 11, kMenuLevelLoad = 13, kMenuSelectBike1 = 15;
        constexpr int32_t kMenuLeaveFrontEnd = 77;
        constexpr uint32_t kMenuSteady = 0;
        // The level table (game/bmx/profile/players.h LevelInfo: its files'
        // name at +0x04, 0x10 bytes a level) and the select's rows (eight
        // listed of twelve items; ui/menu_script.cpp).
        constexpr uint32_t kLevels = 12, kLevelInfoBytes = 0x10, kLevelInfoName = 0x04;
        constexpr uint32_t kListedLevels = 8;
        // The saved game (game/bmx/profile/players.h SavedGame,
        // RiderCharacter, LevelGoals), as bmx_xxx_gameplay.cpp's unlock reads
        // it: a character's level record and its open bit.
        constexpr uint32_t kSavedCharacters = 0x00024, kCharacterBytes = 0x1FF4, kCharacterLevels = 0x078E;
        constexpr uint32_t kLevelGoalsBytes = 0x206, kLevelGoalsFlags = 0x05, kRiderCharacters = 17;
        // LevelGoals' counts: goals, goals done, gaps, gaps done (+0x00 to +0x03).
        constexpr uint32_t kLevelGoalsCounts = 4;
        constexpr uint8_t kLevelGoalsOpen = 0x01;
        // The rider record's physics position (game/bmx/rider, as the panel
        // reads it) and how far the physics has run.
        constexpr uint32_t kRiderPosition = 0x2650u;
        constexpr uint32_t kRiderBytes = 0x2700u;
        constexpr uint32_t kPlacedFrames = 2u;

        constexpr uint16_t kRight = 0x0020u, kStart = 0x0008u, kCross = 0x4000u;

        enum class Step
        {
            Boot,
            MainMenu,
            WaitMenu,
            RiderSelect,
            LevelSelect,
            Loading,
            Placed,
            Done
        };

        struct State
        {
            Step step = Step::Boot;
        bool keepGoals = false;              // PS2X_BMX_KEEP_LEVEL_GOALS
        std::vector<uint8_t> goalsBefore;    // every character's counts of the row's level, as they were
        bool goalsPutBack = false;           // (logged once)
            Step afterMenu = Step::Done;
            int32_t menuWanted = -1;   // WaitMenu: this menu steady (-1: any other than menuLeft)
            int32_t menuLeft = -1;
            std::string level;         // PS2X_DEV_START_LEVEL
            int row = -1;              // the level select's row, once known
            int levelNumber = -1;      // its level
            uint32_t rider = 0;
            bool askRider = false;
            double quitAfter = -1.0;
            int tries = 0;
            int bootPresses = 0;
            uint64_t waitUntil = 0;
            uint64_t releaseAt = 0;
            uint16_t pressed = 0;
            uint64_t menuDeadline = 0;
            uint64_t settledAt = 0;
            bool opening = false;              // the row's level opened in the saved game for the pick
            std::vector<uint8_t> openedBefore; // each character's open bit as it was (0: it was shut)
            std::chrono::steady_clock::time_point crossed, placed;
            PS2Runtime *runtime = nullptr;
        };
        State &devStart() { return ps2x::session::state<State>(); }

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

        int32_t menuId(const uint8_t *rdram) { return static_cast<int32_t>(word(rdram, sym::bmx::g_menu)); }
        bool menuSteady(const uint8_t *rdram) { return word(rdram, sym::bmx::g_menuPhase) == kMenuSteady; }

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

        // The next step once `menu` has the screen and is steady (-1: once
        // the screen has left `left` for any steady menu).
        void waitMenu(uint64_t now, int32_t menu, int32_t left, Step next, uint64_t deadline = 1500u)
        {
            State &s = devStart();
            s.menuWanted = menu;
            s.menuLeft = left;
            s.afterMenu = next;
            s.menuDeadline = deadline == ~0ull ? ~0ull : now + deadline;
            s.step = Step::WaitMenu;
        }

        // The level select's row for the value: a row number, or a level
        // folder's name looked up in the game's level table and the select's
        // rows.
        bool resolveRow(const uint8_t *rdram)
        {
            State &s = devStart();
            if (!s.level.empty() && std::isdigit(static_cast<unsigned char>(s.level[0])))
            {
                s.row = std::atoi(s.level.c_str());
                s.levelNumber = s.row < static_cast<int>(kListedLevels) ? byte(rdram, sym::bmx::g_levelSelectLevels + static_cast<uint32_t>(s.row)) : -1;
                return s.row >= 0 && s.row < static_cast<int>(kListedLevels);
            }
            std::string upper;
            for (char c : s.level)
                upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (uint32_t level = 0; level < kLevels; ++level)
            {
                const uint32_t name = word(rdram, sym::bmx::g_levels + level * kLevelInfoBytes + kLevelInfoName);
                const uint8_t *text = getConstMemPtr(rdram, name);
                if (!text || std::strncmp(reinterpret_cast<const char *>(text), upper.c_str(), 16) != 0)
                    continue;
                for (uint32_t row = 0; row < kListedLevels; ++row)
                    if (byte(rdram, sym::bmx::g_levelSelectLevels + row) == level)
                    {
                        s.row = static_cast<int>(row);
                        s.levelNumber = static_cast<int>(level);
                        return true;
                    }
            }
            return false;
        }

        uint32_t levelFlags(uint32_t character, uint32_t level)
        {
            return sym::bmx::g_savedGame + kSavedCharacters + character * kCharacterBytes + kCharacterLevels + level * kLevelGoalsBytes + kLevelGoalsFlags;
        }

        // The row's level open in every character's record, the bits as they
        // were kept for putBack.
        void openLevel(uint8_t *rdram)
        {
            State &s = devStart();
            if (s.levelNumber < 0)
                return;
            if (s.openedBefore.empty())
            {
                for (uint32_t character = 0; character < kRiderCharacters; ++character)
                    s.openedBefore.push_back(byte(rdram, levelFlags(character, static_cast<uint32_t>(s.levelNumber))) & kLevelGoalsOpen);
                s.opening = true;
            }
            for (uint32_t character = 0; character < kRiderCharacters; ++character)
                if (uint8_t *flags = getMemPtr(rdram, levelFlags(character, static_cast<uint32_t>(s.levelNumber))))
                    *flags |= kLevelGoalsOpen;
        }

        uint32_t levelGoals(uint32_t character, uint32_t level)
        {
            return sym::bmx::g_savedGame + kSavedCharacters + character * kCharacterBytes + kCharacterLevels + level * kLevelGoalsBytes;
        }

        // Every character's goal counts of the row's level kept (once).
        void keepGoals(const uint8_t *rdram)
        {
            State &s = devStart();
            if (!s.keepGoals || s.levelNumber < 0 || !s.goalsBefore.empty())
                return;
            for (uint32_t character = 0; character < kRiderCharacters; ++character)
                for (uint32_t i = 0; i < kLevelGoalsCounts; ++i)
                    s.goalsBefore.push_back(byte(rdram, levelGoals(character, static_cast<uint32_t>(s.levelNumber)) + i));
        }

        // The kept counts written back (they stay kept: a next run's turn
        // start counts the map's goals again).
        void putGoalsBack(uint8_t *rdram)
        {
            State &s = devStart();
            if (s.goalsBefore.size() != kRiderCharacters * kLevelGoalsCounts)
                return;
            uint32_t changed = 0; // counts the map's goals had changed
            for (uint32_t character = 0; character < kRiderCharacters; ++character)
                if (uint8_t *counts = getMemPtr(rdram, levelGoals(character, static_cast<uint32_t>(s.levelNumber))))
                {
                    for (uint32_t i = 0; i < kLevelGoalsCounts; ++i)
                        changed += counts[i] != s.goalsBefore[character * kLevelGoalsCounts + i] ? 1u : 0u;
                    std::memcpy(counts, &s.goalsBefore[character * kLevelGoalsCounts], kLevelGoalsCounts);
                }
            if (!s.goalsPutBack)
                std::cout << "[devstart] the saved game's goal counts of level " << s.levelNumber << " as they were (" << changed
                          << " changed by the map)" << std::endl;
            s.goalsPutBack = true;
        }

        bool rideOver(uint32_t screen)
        {
            return screen == kScreenLeave || screen == kScreenExitGame || screen == kScreenExitLevel || screen == kScreenGameOver ||
                   screen == kScreenSaveGame;
        }

        void putBack(uint8_t *rdram)
        {
            State &s = devStart();
            if (!s.opening)
                return;
            for (uint32_t character = 0; character < kRiderCharacters && character < s.openedBefore.size(); ++character)
                if (!s.openedBefore[character])
                    if (uint8_t *flags = getMemPtr(rdram, levelFlags(character, static_cast<uint32_t>(s.levelNumber))))
                        *flags = static_cast<uint8_t>(*flags & ~kLevelGoalsOpen);
            s.opening = false;
            std::cout << "[devstart] the saved game's level bits as they were" << std::endl;
        }

        bool riderPlaced(const uint8_t *rdram)
        {
            if (byte(rdram, sym::bmx::g_currentGameState) != kGameStateLevel || byte(rdram, sym::bmx::g_gameStatePhase) != kStateRunning ||
                word(rdram, sym::bmx::g_physicsTaskFrames) <= kPlacedFrames)
                return false;
            if (word(rdram, sym::bmx::g_gameScreen) > kScreenPlay)
                return false;
            const uint32_t rider = word(rdram, sym::bmx::g_riders);
            if (!getConstMemPtr(rdram, rider) || !getConstMemPtr(rdram, rider + kRiderBytes - 1u))
                return false;
            const float y = real(rdram, rider + kRiderPosition + 4u);
            return y == y;
        }

        // The level's screen before the ride, taken with Cross.
        bool screenBeforeRide(const uint8_t *rdram)
        {
            const uint32_t screen = word(rdram, sym::bmx::g_gameScreen);
            return screen == kScreenInstructions || screen == kScreenObjectives;
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
            const uint8_t state = byte(rdram, sym::bmx::g_currentGameState);
            const int32_t menu = menuId(rdram);
            switch (s.step)
            {
            case Step::Boot:
                // Start on the title (or through the intro's movies) until
                // the main menu has the screen; settled 90 vsyncs.
                if (now < 400u)
                    return;
                if (state == kGameStateFrontEnd && menu == kMenuMainMenu && menuSteady(rdram))
                {
                    s.waitUntil = now + 90u;
                    s.step = Step::MainMenu;
                    std::cout << "[devstart] main menu after " << s.bootPresses << " Start press(es)" << std::endl;
                    return;
                }
                if (state == kGameStateIntro || menu == kMenuStart || menu == kMenuPressStart || menu == kMenuTitleLanguage || menu == kMenuDemoLoop)
                {
                    if (s.bootPresses < 30)
                    {
                        ++s.bootPresses;
                        press(kStart, now, 10, 90);
                        return;
                    }
                    fail("no main menu");
                }
                return;
            case Step::MainMenu:
                // HARDCORE TOUR: the main menu's first item, the cursor on it.
                if (!resolveRow(rdram))
                {
                    std::cerr << "[devstart] no level select row for " << s.level << std::endl;
                    fail("level");
                    return;
                }
                openLevel(rdram);
                keepGoals(rdram);
                press(kCross, now);
                waitMenu(now, kMenuRiderSelect, -1, Step::RiderSelect);
                return;
            case Step::WaitMenu:
            {
                const bool there = s.menuWanted >= 0 ? menu == s.menuWanted : menu != s.menuLeft;
                if (there && menuSteady(rdram) && state == kGameStateFrontEnd)
                {
                    s.tries = 0;
                    s.step = s.afterMenu;
                    s.waitUntil = now + 30u;
                    return;
                }
                if (s.menuWanted < 0 && state != kGameStateFrontEnd)
                {
                    // The screen was left for the level (a pick taken already).
                    s.tries = 0;
                    s.step = s.afterMenu;
                    return;
                }
                s.waitUntil = now + 20u;
                if (now > s.menuDeadline)
                    fail("no next screen");
                return;
            }
            case Step::RiderSelect:
                if (s.askRider)
                {
                    // The player's own rider: nothing pressed, no deadline;
                    // the level select follows the player's Cross.
                    if (s.tries++ == 0)
                        std::cout << "[devstart] rider select: waiting for the player" << std::endl;
                    waitMenu(now, kMenuLevelSelect, -1, Step::LevelSelect, ~0ull);
                    return;
                }
                if (s.tries < static_cast<int>(s.rider))
                {
                    ++s.tries;
                    press(kRight, now, 6, 120);
                    return;
                }
                press(kCross, now);
                waitMenu(now, kMenuLevelSelect, -1, Step::LevelSelect, 1800u);
                return;
            case Step::LevelSelect:
                if (menu == kMenuSelectBike1)
                {
                    // A bike choice between (two players' flow): the first bike.
                    press(kCross, now);
                    waitMenu(now, kMenuLevelSelect, -1, Step::LevelSelect);
                    return;
                }
                if (s.settledAt == 0)
                {
                    // The list settles first.
                    s.settledAt = now + 90u;
                    s.waitUntil = s.settledAt;
                    return;
                }
                if (s.tries < s.row)
                {
                    ++s.tries;
                    press(kRight, now, 6, 120);
                    return;
                }
                std::cout << "[devstart] level select row " << s.row << " (" << s.level << ", level " << s.levelNumber << ")" << std::endl;
                press(kCross, now);
                s.crossed = std::chrono::steady_clock::now();
                s.waitUntil = now + 60u;
                s.step = Step::Loading;
                return;
            case Step::Loading:
                if (state == kGameStateLevel && s.opening)
                    putBack(rdram);
                if (state == kGameStateLevel && screenBeforeRide(rdram))
                {
                    press(kCross, now, 6, 40);
                    return;
                }
                if (riderPlaced(rdram))
                {
                    s.placed = std::chrono::steady_clock::now();
                    const uint32_t level = byte(rdram, sym::bmx::g_currentLevel);
                    std::cout << "[devstart] placed in level " << level << " (" << s.level << "): load " << seconds(s.crossed)
                              << " s from the level select's Cross" << std::endl;
                    if (s.levelNumber >= 0 && level != static_cast<uint32_t>(s.levelNumber))
                        std::cerr << "[devstart] not the level asked for (" << s.levelNumber << ")" << std::endl;
                    s.step = Step::Placed;
                }
                return;
            case Step::Placed:
                // The ride over (its end screens, where the game saves, or the
                // game back in its menus): the goal counts kept put back.
                if (!s.goalsBefore.empty() && (state == kGameStateFrontEnd || rideOver(word(rdram, sym::bmx::g_gameScreen))))
                {
                    putGoalsBack(rdram);
                    if (state == kGameStateFrontEnd)
                    {
                        s.goalsBefore.clear();
                        s.step = Step::Done;
                        return;
                    }
                }
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
        const char *level = ps2x::boot::text("PS2X_DEV_START_LEVEL");
        if (!level || !*level)
            return;
        State &s = devStart();
        s.level = level;
        if (const char *rider = ps2x::boot::text("PS2X_DEV_START_SKATER"); rider && *rider)
        {
            s.askRider = std::strcmp(rider, "ask") == 0;
            s.rider = s.askRider ? 0u : static_cast<uint32_t>(std::atoi(rider));
        }
        if (const char *quit = ps2x::boot::text("PS2X_DEV_START_QUIT"); quit && *quit)
            s.quitAfter = std::atof(quit);
        if (const char *keep = ps2x::boot::text("PS2X_BMX_KEEP_LEVEL_GOALS"); keep && *keep && std::strcmp(keep, "0") != 0)
            s.keepGoals = true;
        s.runtime = &runtime;
        runtime.addVSyncHook([](uint8_t *rdram, uint64_t vsync) { onVsync(rdram, vsync); });
        std::cout << "[devstart] on: HARDCORE TOUR level " << s.level << ", rider "
                  << (s.askRider ? std::string("chosen by the player") : std::to_string(s.rider)) << std::endl;
    }
}

namespace bmx_xxx
{
    void installDevStart(PS2Runtime &runtime)
    {
        devstart::install(runtime);
    }
}
