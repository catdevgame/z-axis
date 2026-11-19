// Aggressive Inline (SLUS-20327): a developer start straight into a free
// skate level, for measuring without menus or a control channel (the
// Windows desktop, unattended runs).
//
//   PS2X_DEV_START_LEVEL=<entry>|ESTATE
//       the free skate level-select entry (0 the first; the row as the game
//       and the overrides made it), or ESTATE (TUT/Estate,
//       aggressive_inline_estate).
//   PS2X_DEV_START_SKATER=<n>   the skater-select entry (default 0), or
//                               "ask": the game's skater select waits for
//                               the player's own choice (the collection's
//                               Custom Freeplay), then the route goes on.
//   PS2X_DEV_START_SKATE=<n>    once placed, n rounds of tools/sweep.py's
//                               skate routine (pushes, jumps, grabs, turns).
//   PS2X_DEV_START_QUIT=<s>     stop the runner s seconds after the
//                               routine (or the placing) ends.
//
// The presses are the game's own pad input (PSPadBackend::
// setOverrideButtons), each step waiting on the game's state the way
// tools/routes.py's `boot` and `freeskate` do: the intro skipped with Start,
// the main menu (game mode 1, a zgui panel), Free Skate (right, Cross), the
// skater select, a new skater's name entry (Cross on END), the level select
// (the panel's cursor at +0x114 stepped to the entry), Cross; then message
// boxes (mode 3) closed until the skater is placed (mode 2, a position, the
// level's frame count past 2). Logged as "[devstart] ...", with the time
// from the level's Cross to the placed skater (the load).
//
// Free skate levels other than the Tutorial and the Movie Lot need an
// unlocked card or PS2X_DEV_UNLOCK_ALL=1.
//
// The four values are the boot's (ps2x::boot::text: the environment, else
// what the app gave this boot): the collection's Custom Freeplay starts the
// game into its map's slot with them (collection/maps.h mapSlots).

#include "aggressive_inline_devstart.h"
#include "aggressive_inline_estate.h"

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
#include <sstream>
#include <string>
#include <vector>

namespace aggressive_inline::devstart
{
    namespace
    {
        constexpr uint32_t kGameMode = 0x00276FE4u;  // 1 front end, 2 level, 3 message box
        constexpr uint32_t kPanel = 0x00240A20u;     // the active zgui panel
        constexpr uint32_t kPanelCursor = 0x114u;    // its menu cursor
        constexpr uint32_t kLevelFrame = 0x00231CACu;
        constexpr uint32_t kLevelIndex = 0x0024C9B4u;
        constexpr uint32_t kPlayerY = 0x002A76F4u;   // player one's position y
        constexpr uint32_t kUnplaced = 0x469C4000u;  // 20000.0: not placed yet
        constexpr uint32_t kNameEntryEnd = 38u;      // a new skater's name entry, the cursor on END

        constexpr uint16_t kLeft = 0x0080u, kDown = 0x0040u, kRight = 0x0020u, kUp = 0x0010u, kStart = 0x0008u;
        constexpr uint16_t kSquare = 0x8000u, kCross = 0x4000u, kCircle = 0x2000u, kTriangle = 0x1000u;
        constexpr uint16_t kR1 = 0x0800u, kL1 = 0x0400u;

        // tools/sweep.py's skate routine (control channel commands).
        const char *const kSkateRoutine[] = {
            "down UP", "wait 120", "press CROSS 4 30", "press SQUARE 20 40", "wait 60", "hold LEFT 25", "wait 120",
            "press CROSS 4 20", "press TRIANGLE 40 30", "hold RIGHT 40", "wait 150", "press CROSS 4 20",
            "press SQUARE 20 40", "hold L1 20", "wait 120", "up UP", "press CROSS 4 30", "down DOWN", "wait 40",
            "up DOWN", "down UP", "hold RIGHT 60", "wait 200", "press CROSS 4 10", "press CIRCLE 30 30",
            "hold LEFT 50", "wait 200", "press CROSS 4 20", "press TRIANGLE 50 30", "hold R1 20", "wait 200",
            "press CROSS 20 20", "hold LEFT 30", "wait 200", "up UP"};

        enum class Step
        {
            Boot,
            MainMenu,
            WaitPanel,
            SkaterSelect,
            AfterSkater,
            LevelSelect,
            Loading,
            Skating,
            Placed,
            Done
        };

        struct State
        {
            Step step = Step::Boot;
            Step afterPanel = Step::Done;
            std::string level;   // PS2X_DEV_START_LEVEL
            int entry = -1;      // the level-select entry, once known
            uint32_t skater = 0;
            bool askSkater = false; // PS2X_DEV_START_SKATER=ask: the player picks on the game's skater select
            int skateRounds = 0;
            double quitAfter = -1.0;
            int bootPresses = 0;
            int tries = 0;
            uint32_t oldPanel = 0, lastPanel = 0;
            uint64_t waitUntil = 0;   // no step before this vsync
            uint64_t releaseAt = 0;   // the press's buttons up at this vsync
            uint16_t held = 0;        // "down" buttons
            uint16_t pressed = 0;     // the current press
            uint64_t panelDeadline = 0;
            std::chrono::steady_clock::time_point crossed, placed, skated;
            uint64_t settledAt = 0;   // the level-select entry reached, Cross from this vsync
            uint64_t crossedAt = 0;
            int crossTries = 0;
            uint32_t crossPanel = 0;  // the level select's panel
            size_t routineAt = 0;
            int routineRound = 0;
            PS2Runtime *runtime = nullptr;
        };
        State &devStart() { return ps2x::session::state<State>(); }

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFCu), 4);
            return v;
        }

        void apply()
        {
            PSPadBackend::setOverrideButtons(static_cast<uint16_t>(devStart().held | devStart().pressed));
        }

        void press(uint16_t mask, uint64_t now, uint64_t hold = 6, uint64_t gap = 30)
        {
            devStart().pressed = mask;
            devStart().releaseAt = now + hold;
            devStart().waitUntil = now + hold + gap;
            apply();
        }

        uint16_t buttonMask(const std::string &name)
        {
            static const struct
            {
                const char *name;
                uint16_t mask;
            } kNames[] = {{"LEFT", kLeft}, {"DOWN", kDown}, {"RIGHT", kRight}, {"UP", kUp}, {"START", kStart},
                          {"SQUARE", kSquare}, {"CROSS", kCross}, {"CIRCLE", kCircle}, {"TRIANGLE", kTriangle},
                          {"R1", kR1}, {"L1", kL1}};
            for (const auto &b : kNames)
                if (name == b.name)
                    return b.mask;
            return 0u;
        }

        double seconds(std::chrono::steady_clock::time_point from)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        }

        void fail(const char *what)
        {
            std::cerr << "[devstart] " << what << ": stopped" << std::endl;
            devStart().held = devStart().pressed = 0;
            apply();
            devStart().step = Step::Done;
        }

        // A new panel that stays the active one for two checks (20 vsyncs
        // apart), as routes.py's new_panel.
        void waitPanel(const uint8_t *rdram, uint64_t now, Step next)
        {
            devStart().oldPanel = word(rdram, kPanel);
            devStart().lastPanel = 0;
            devStart().afterPanel = next;
            devStart().panelDeadline = now + 1200u;
            devStart().step = Step::WaitPanel;
        }

        // Steps the panel's cursor towards `want`; true when it is there.
        bool cursorTo(const uint8_t *rdram, uint64_t now, uint32_t want, int limit)
        {
            const uint32_t panel = word(rdram, kPanel);
            const uint32_t cursor = panel ? word(rdram, panel + kPanelCursor) : 0u;
            if (cursor == want)
                return true;
            if (++devStart().tries > limit)
            {
                std::cerr << "[devstart] the cursor stays at " << cursor << ", not " << want << std::endl;
                fail("menu");
                return false;
            }
            press(cursor < want ? kRight : kLeft, now, 6, 25);
            return false;
        }

        int resolveEntry(const std::string &level)
        {
            if (!level.empty() && std::isdigit(static_cast<unsigned char>(level[0])))
                return std::atoi(level.c_str());
            std::string upper;
            for (char c : level)
                upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return upper == "ESTATE" ? estateLevelEntry() : -1;
        }

        // One command of the skate routine; false when the routine is over.
        bool routineStep(uint64_t now)
        {
            constexpr size_t kCount = sizeof(kSkateRoutine) / sizeof(kSkateRoutine[0]);
            if (devStart().routineAt >= kCount)
            {
                devStart().routineAt = 0;
                if (++devStart().routineRound >= devStart().skateRounds)
                    return false;
            }
            std::istringstream in(kSkateRoutine[devStart().routineAt++]);
            std::string op, button;
            uint64_t a = 6, b = 6;
            in >> op;
            if (op == "wait")
            {
                in >> a;
                devStart().waitUntil = now + a;
                return true;
            }
            in >> button;
            const uint16_t mask = buttonMask(button);
            if (op == "down")
                devStart().held = static_cast<uint16_t>(devStart().held | mask);
            else if (op == "up")
                devStart().held = static_cast<uint16_t>(devStart().held & ~mask);
            else if (op == "hold")
            {
                in >> a;
                press(mask, now, a, 0);
                return true;
            }
            else if (op == "press")
            {
                in >> a >> b;
                press(mask, now, a, b);
                return true;
            }
            apply();
            return true;
        }

        void onVsync(uint8_t *rdram, uint64_t now)
        {
            if (devStart().step == Step::Done || !rdram)
                return;
            if (devStart().pressed && now >= devStart().releaseAt)
            {
                devStart().pressed = 0;
                apply();
            }
            if (now < devStart().waitUntil)
                return;
            const uint32_t mode = word(rdram, kGameMode);
            switch (devStart().step)
            {
            case Step::Boot:
                // routes.py boot: past the intro with Start, then the main
                // menu (mode 1 with a panel), settled 90 vsyncs. Start is
                // pressed only while no front-end panel is up: with Skip
                // Intro Movies the main menu is there at once, and a Start
                // on it would choose Career.
                if (now < 400u)
                    return;
                if (mode == 1u && word(rdram, kPanel) != 0u)
                {
                    devStart().waitUntil = now + 90u;
                    devStart().step = Step::MainMenu;
                    std::cout << "[devstart] main menu after " << devStart().bootPresses << " Start press(es)" << std::endl;
                    return;
                }
                if (devStart().bootPresses < 12)
                {
                    ++devStart().bootPresses;
                    press(kStart, now, 10, 60);
                    return;
                }
                fail("no main menu");
                return;
            case Step::MainMenu:
                // Free Skate: one to the right of Career, then Cross.
                if (devStart().tries++ == 0)
                {
                    devStart().oldPanel = word(rdram, kPanel);
                    press(kRight, now);
                    return;
                }
                devStart().tries = 0;
                {
                    const uint32_t old = devStart().oldPanel;
                    press(kCross, now);
                    waitPanel(rdram, now, Step::SkaterSelect);
                    devStart().oldPanel = old;
                }
                return;
            case Step::WaitPanel:
            {
                const uint32_t panel = word(rdram, kPanel);
                if (panel && panel != devStart().oldPanel && panel == devStart().lastPanel)
                {
                    devStart().tries = 0;
                    devStart().step = devStart().afterPanel;
                    return;
                }
                devStart().lastPanel = panel;
                devStart().waitUntil = now + 20u;
                if (now > devStart().panelDeadline)
                    fail("no new menu panel");
                return;
            }
            case Step::SkaterSelect:
                if (devStart().askSkater)
                {
                    // The player's own choice on the game's skater select:
                    // nothing pressed, no deadline; the next panel (the
                    // player's Cross) goes on to the level select.
                    if (devStart().tries++ == 0)
                        std::cout << "[devstart] skater select: waiting for the player" << std::endl;
                    waitPanel(rdram, now, Step::AfterSkater);
                    devStart().panelDeadline = ~0ull;
                    return;
                }
                if (!cursorTo(rdram, now, devStart().skater, 20))
                    return;
                press(kCross, now);
                waitPanel(rdram, now, Step::AfterSkater);
                return;
            case Step::AfterSkater:
            {
                const uint32_t panel = word(rdram, kPanel);
                if (panel && word(rdram, panel + kPanelCursor) == kNameEntryEnd)
                {
                    press(kCross, now);
                    waitPanel(rdram, now, Step::LevelSelect);
                    return;
                }
                devStart().tries = 0;
                devStart().step = Step::LevelSelect;
                return;
            }
            case Step::LevelSelect:
                if (devStart().entry < 0 && (devStart().entry = resolveEntry(devStart().level)) < 0)
                {
                    std::cerr << "[devstart] no level-select entry for " << devStart().level << std::endl;
                    fail("level");
                    return;
                }
                if (!cursorTo(rdram, now, static_cast<uint32_t>(devStart().entry), 40))
                    return;
                if (devStart().settledAt == 0)
                {
                    // The row's move and the entry's preview settle first.
                    devStart().settledAt = now + 60u;
                    devStart().waitUntil = devStart().settledAt;
                    return;
                }
                std::cout << "[devstart] level select entry " << devStart().entry << " (" << devStart().level << ")" << std::endl;
                press(kCross, now);
                devStart().crossed = std::chrono::steady_clock::now();
                devStart().crossedAt = now;
                devStart().crossPanel = word(rdram, kPanel);
                devStart().step = Step::Loading;
                return;
            case Step::Loading:
                if (mode == 1u && word(rdram, kPanel) == devStart().crossPanel && now > devStart().crossedAt + 300u && devStart().crossTries < 3)
                {
                    // Still the front end 5 s on: the Cross was not taken (a
                    // preview starting); again.
                    ++devStart().crossTries;
                    std::cout << "[devstart] Cross again" << std::endl;
                    press(kCross, now);
                    devStart().crossed = std::chrono::steady_clock::now();
                    devStart().crossedAt = now;
                    return;
                }
                if (mode == 3u)
                {
                    press(kCross, now, 6, 40); // a message box
                    return;
                }
                if (mode == 2u && word(rdram, kPlayerY) != kUnplaced && word(rdram, kLevelFrame) > 2u)
                {
                    devStart().placed = std::chrono::steady_clock::now();
                    std::cout << "[devstart] placed in level " << (word(rdram, kLevelIndex) & 0xFFu) << " (" << devStart().level
                              << "): load " << seconds(devStart().crossed) << " s from the level select's Cross" << std::endl;
                    devStart().step = devStart().skateRounds > 0 ? Step::Skating : Step::Placed;
                    devStart().skated = devStart().placed;
                }
                return;
            case Step::Skating:
                if (mode == 3u)
                {
                    press(kCross, now, 6, 40);
                    return;
                }
                if (!routineStep(now))
                {
                    devStart().held = devStart().pressed = 0;
                    apply();
                    devStart().skated = std::chrono::steady_clock::now();
                    std::cout << "[devstart] skated " << devStart().skateRounds << " round(s), " << seconds(devStart().placed) << " s"
                              << std::endl;
                    devStart().step = Step::Placed;
                }
                return;
            case Step::Placed:
                if (devStart().quitAfter >= 0.0 && seconds(devStart().skated) >= devStart().quitAfter)
                {
                    std::cout << "[devstart] done: stopping the runner" << std::endl;
                    devStart().step = Step::Done;
                    if (devStart().runtime)
                        devStart().runtime->requestStop();
                }
                return;
            case Step::Done:
                return;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        // The boot's values (ps2x::boot::text: the environment first, then
        // what the app gave this boot: the collection's Custom Freeplay
        // starts the game into its map this way).
        const char *level = ps2x::boot::text("PS2X_DEV_START_LEVEL");
        if (!level || !*level)
            return;
        devStart().level = level;
        if (const char *skater = ps2x::boot::text("PS2X_DEV_START_SKATER"); skater && *skater)
        {
            // "ask": the game's own skater select waits for the player.
            devStart().askSkater = std::strcmp(skater, "ask") == 0;
            devStart().skater = devStart().askSkater ? 0u : static_cast<uint32_t>(std::atoi(skater));
        }
        if (const char *skate = ps2x::boot::text("PS2X_DEV_START_SKATE"); skate && *skate)
            devStart().skateRounds = std::atoi(skate);
        if (const char *quit = ps2x::boot::text("PS2X_DEV_START_QUIT"); quit && *quit)
            devStart().quitAfter = std::atof(quit);
        devStart().runtime = &runtime;
        runtime.addVSyncHook([](uint8_t *rdram, uint64_t vsync) { onVsync(rdram, vsync); });
        std::cout << "[devstart] on: free skate level " << devStart().level << ", skater "
                  << (devStart().askSkater ? std::string("chosen by the player") : std::to_string(devStart().skater)) << std::endl;
    }
}

namespace aggressive_inline
{
    void installDevStart(PS2Runtime &runtime)
    {
        devstart::install(runtime);
    }
}
