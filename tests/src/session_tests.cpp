// A game boot's session (runtime/session.h): the game overrides' state
// made fresh for every boot, what they registered with the host undone when
// the game is left, the game's own shutdown hooks; and the owner-tagged
// registrations it undoes (disc patches, game file providers).
#include "MiniTest.h"

#include "game_overrides.h"
#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/disc_patch.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
    std::vector<std::string> g_events;

    struct Counter
    {
        int value = 0;
        std::vector<int> items;
    };

    struct First
    {
        ~First() { g_events.push_back("first destroyed"); }
    };

    struct Second
    {
        ~Second() { g_events.push_back("second destroyed"); }
    };

    int g_hookRuns = 0;
    void countingHook(PS2Machine &) { ++g_hookRuns; }

    std::vector<uint8_t> bytesOf(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }
}

void register_session_tests()
{
    MiniTest::Case("Session", [](TestCase &tc)
    {
        tc.Run("state is one per session, made fresh after it ends", [](TestCase &t)
        {
            zx::session::end();
            Counter &a = zx::session::state<Counter>();
            a.value = 7;
            a.items.push_back(1);
            t.IsTrue(&zx::session::state<Counter>() == &a, "the same object within a session");
            t.Equals(zx::session::state<Counter>().value, 7, "and its value");
            const uint64_t before = zx::session::number();
            zx::session::end();
            t.IsTrue(zx::session::number() != before, "the session number changes");
            Counter &b = zx::session::state<Counter>();
            t.Equals(b.value, 0, "a new session's state is value-initialized");
            t.IsTrue(b.items.empty(), "with nothing kept");
            zx::session::end();
        });

        tc.Run("end: the end work first (states still there), then states, last made first", [](TestCase &t)
        {
            zx::session::end();
            g_events.clear();
            zx::session::state<First>();
            zx::session::state<Second>();
            zx::session::state<Counter>().value = 3;
            zx::session::onEnd([]() { g_events.push_back("undo a"); });
            zx::session::onEnd([]()
                                 { g_events.push_back("undo b sees " + std::to_string(zx::session::state<Counter>().value)); });
            zx::session::end();
            const std::vector<std::string> expected{"undo b sees 3", "undo a", "second destroyed", "first destroyed"};
            t.IsTrue(g_events == expected, "undo work last added first, then states last made first");
            g_events.clear();
            zx::session::end();
            t.IsTrue(g_events.empty(), "nothing runs twice");
        });

        tc.Run("a boot begins and ends a session", [](TestCase &t)
        {
            zx::boot::begin(zx::BootConfig{});
            zx::session::state<Counter>().value = 5;
            zx::boot::begin(zx::BootConfig{});
            t.Equals(zx::session::state<Counter>().value, 0, "a new boot: a new session");
            zx::session::state<Counter>().value = 6;
            zx::boot::end();
            t.Equals(zx::session::state<Counter>().value, 0, "the boot's end ends it");
            zx::session::end();
        });

        tc.Run("the game's shutdown hooks are its session's", [](TestCase &t)
        {
            PS2Machine runtime; // its boot begins a session
            g_hookRuns = 0;
            zx::session::addShutdownHook(&countingHook);
            zx::session::addShutdownHook(&countingHook);
            ps2_game_overrides::runShutdownHooks(runtime);
            t.Equals(g_hookRuns, 1, "run by runShutdownHooks, once per function");
            zx::session::end();
            ps2_game_overrides::runShutdownHooks(runtime);
            t.Equals(g_hookRuns, 1, "dropped when the session ends");
        });

        tc.Run("a settings row of the session is removed when it ends", [](TestCase &t)
        {
            zx::session::end();
            zx::settings::Setting row;
            row.key = "test.session_row";
            row.label = "Session Row";
            zx::session::addSetting(row);
            t.IsTrue(zx::settings::has("test.session_row"), "added");
            zx::session::end();
            t.IsFalse(zx::settings::has("test.session_row"), "removed");
        });

        tc.Run("disc patches by owner: one owner's taken back, the others' kept and the file made again", [](TestCase &t)
        {
            namespace dp = zx::disc_patch;
            dp::clearPatches();
            dp::registerPatch("uilib.zal", [](std::vector<uint8_t> &bytes) { bytes.push_back('a'); return true; }, "game");
            dp::registerPatch("UILIB.ZAL", [](std::vector<uint8_t> &bytes) { bytes.push_back('b'); return true; }, "other");
            dp::registerPatch("scene.txt", [](std::vector<uint8_t> &bytes) { bytes.push_back('s'); return true; }, "game");
            const auto read = [](std::vector<uint8_t> &bytes) { bytes = bytesOf("x"); return true; };
            auto both = dp::contents("ASSETS/UILIB.ZAL", read);
            t.IsTrue(both && *both == bytesOf("xab"), "in registration order");
            dp::removePatches("game");
            auto other = dp::contents("ASSETS/UILIB.ZAL", read);
            t.IsTrue(other && *other == bytesOf("xb"), "the other owner's patch alone, made again");
            t.IsFalse(dp::hasPatch("SCENE.TXT"), "a file with no patch left has none");
            dp::removePatches("other");
            t.IsFalse(dp::hasPatch("UILIB.ZAL"), "all gone");
            dp::clearPatches();
        });

        tc.Run("game file providers removed by name", [](TestCase &t)
        {
            namespace gf = zx::game_files;
            gf::clearProviders();
            const auto made = [](const std::string &text)
            {
                return [text](const std::string &) { return std::make_shared<const std::vector<uint8_t>>(bytesOf(text)); };
            };
            gf::registerProvider("ASSETS/A/", made("game"), "game");
            gf::registerProvider("ASSETS/B/", made("other"), "other");
            gf::GameFileSystem files({{}, nullptr, {}});
            t.IsTrue(files.open("ASSETS/A/FILE.BIN") != nullptr, "the game's provider answers");
            gf::removeProviders("game");
            t.IsTrue(files.open("ASSETS/A/FILE.BIN") == nullptr, "removed");
            t.IsTrue(files.open("ASSETS/B/FILE.BIN") != nullptr, "the other kept");
            gf::clearProviders();
        });
    });
}
