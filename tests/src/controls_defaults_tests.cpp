// A player's controls.ini and the defaults that changed since it was written
// (collection/settings.h updateControlsDefaults, runtime/host_input.h
// kControlsRevision and GameControls::changedDefaults).
#include "MiniTest.h"
#include "collection/settings.h"
#include "runtime/host_input.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace zx::app;
namespace hi = ps2_host_input;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("zx-controls-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    const std::string kRevisionLines = "# The revision of the defaults this file was written with: when a later\n"
                                       "# version changes a default, the app gives it to you unless you had\n"
                                       "# changed that control yourself. Leave this line as it is.\n"
                                       "controls_version = " + std::to_string(hi::kControlsRevision) + "\n\n";

    // A game whose defaults changed at the current revision: Cross gained K,
    // the mouse went from stick to look, and Circle is a control it did not
    // have before.
    hi::GameControls changedGame()
    {
        hi::GameControls game;
        game.id = "test";
        game.title = "Test";
        game.defaultIni = "# Test controls\n\n" + kRevisionLines +
                          "[buttons]\ncross    = SPACE, K\nsquare   = J\ncircle   = F\n\n[mouse]\nmode = look\n";
        game.changedDefaults = {{hi::kControlsRevision, "buttons", "cross", {"SPACE"}},
                                {hi::kControlsRevision, "mouse", "mode", {"stick"}}};
        return game;
    }

    // The file that game wrote before: Cross on Space alone, the mouse on the
    // stick, no Circle.
    const std::string kOldFile = "# Test controls\n\n[buttons]\ncross    = SPACE\nsquare   = J\n\n[mouse]\nmode = stick\n";

    std::string replaced(std::string text, const std::string &from, const std::string &to)
    {
        const size_t at = text.find(from);
        return at == std::string::npos ? text : text.replace(at, from.size(), to);
    }

    // Each game's default controls as "[section] key = value" lines (no
    // comments, values as compared), hashed (FNV-1a): what a change of a
    // default changes.
    uint64_t defaultsFingerprint(const hi::GameControls &game)
    {
        uint64_t hash = 1469598103934665603ull;
        std::istringstream in(game.defaultIni);
        std::string line, section;
        while (std::getline(in, line))
        {
            line = line.substr(0, line.find_first_of("#;"));
            std::string compact;
            for (char c : line)
                if (c != ' ' && c != '\t' && c != '\r')
                    compact += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (compact.empty())
                continue;
            if (compact.front() == '[')
            {
                section = compact;
                continue;
            }
            for (char c : section + compact + "\n")
                hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        }
        return hash;
    }
}

void register_controls_defaults_tests()
{
    MiniTest::Case("ControlsDefaults", [](TestCase &tc)
    {
        tc.Run("an untouched file of an earlier version gets the defaults that changed", [](TestCase &t)
        {
            const hi::GameControls game = changedGame();
            std::vector<std::string> log;
            const std::string updated = updateControlsDefaults(kOldFile, game, &log);
            t.Equals(updated,
                     "# Test controls\n\n" + kRevisionLines +
                         "[buttons]\ncross    = SPACE, K\nsquare   = J\n\n[mouse]\nmode = look\n",
                     "the two new defaults, the revision line, the rest as it was");
            t.Equals(log.size(), size_t{2}, "two changes logged");
            t.Equals(updateControlsDefaults(updated, game), updated, "once only: the next start changes nothing");
        });

        tc.Run("a control the player rebound keeps their binding", [](TestCase &t)
        {
            const hi::GameControls game = changedGame();
            const std::string mine = replaced(kOldFile, "cross    = SPACE\n", "cross    = space, MOUSE_LEFT # mine\n");
            std::vector<std::string> log;
            const std::string updated = updateControlsDefaults(mine, game, &log);
            t.IsTrue(updated.find("cross    = space, MOUSE_LEFT # mine\n") != std::string::npos, "their Cross kept");
            t.IsTrue(updated.find("mode = look\n") != std::string::npos, "the mouse they never set gets the new default");
            t.IsTrue(log.size() == 2u && log[0].find("kept") != std::string::npos, "the kept one logged as theirs");
            // The same default in another case and spacing is the default, not theirs.
            const std::string spaced = replaced(kOldFile, "cross    = SPACE\n", "Cross=space\n");
            t.IsTrue(updateControlsDefaults(spaced, game).find("Cross= SPACE, K\n") != std::string::npos,
                     "\"Cross=space\" is the earlier default");
            // Set back to the earlier default after the update: theirs from then on.
            const std::string setBack = replaced(updateControlsDefaults(kOldFile, game), "mode = look", "mode = stick");
            t.Equals(updateControlsDefaults(setBack, game), setBack, "a control set back after the update stays");
        });

        tc.Run("a control added since takes its default", [](TestCase &t)
        {
            const hi::GameControls game = changedGame();
            const std::string updated = updateControlsDefaults(kOldFile, game);
            t.IsTrue(updated.find("circle") == std::string::npos, "the file is not given lines it did not have");
            hi::HostInputConfig config = hi::HostInputConfig::defaults(game.defaultIni);
            std::vector<std::string> warnings;
            config.applyIni(updated, &warnings);
            t.IsTrue(warnings.empty(), "the updated file parses without warnings (the revision line included)");
            t.Equals(hi::formatBindings(config.buttons[hi::kCircle]), std::string("F"), "Circle on its default");
            t.Equals(hi::formatBindings(config.buttons[hi::kCross]), std::string("SPACE, K"), "Cross on its new default");
            t.Equals(hi::formatBindings(config.buttons[hi::kSquare]), std::string("J"), "Square as it was");
            t.IsTrue(config.mouseCamera == hi::MouseCamera::Look, "the mouse on its new default");
        });

        tc.Run("the file's line ends, a later revision's file and a missing one", [](TestCase &t)
        {
            const hi::GameControls game = changedGame();
            std::string crlf;
            for (char c : kOldFile)
                crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
            const std::string updated = updateControlsDefaults(crlf, game);
            size_t lone = 0;
            for (size_t i = 0; i < updated.size(); ++i)
                lone += updated[i] == '\n' && (i == 0 || updated[i - 1] != '\r');
            t.IsTrue(lone == 0 && updated.find("controls_version = ") != std::string::npos &&
                         updated.find("mode = look\r\n") != std::string::npos,
                     "a CRLF file stays CRLF");
            const std::string later = "controls_version = " + std::to_string(hi::kControlsRevision + 1) + "\n" + kOldFile;
            t.Equals(updateControlsDefaults(later, game), later, "a newer app's file is left alone");
            const std::string garbled = "controls_version = two\n" + kOldFile;
            t.IsTrue(updateControlsDefaults(garbled, game).find("controls_version = " + std::to_string(hi::kControlsRevision) +
                                                                "\n") != std::string::npos,
                     "an unreadable revision is the first, and rewritten");

            TempDir dir;
            const std::filesystem::path file = dir.path / "games" / "bmx" / "controls.ini";
            t.IsFalse(updateControlsFile(file, "bmx"), "no file: nothing done");
            t.IsFalse(std::filesystem::exists(file), "and none made (the game writes its defaults)");
        });

        tc.Run("BMX XXX: a 1.2.0 controls.ini gets mouse-look", [](TestCase &t)
        {
            const hi::GameControls &bmx = hi::gameControls("bmx");
            // 1.2.0 wrote the defaults with the mouse on the stick and no revision line.
            std::string old = replaced(bmx.defaultIni, kRevisionLines, "");
#if defined(__ANDROID__)
            t.IsTrue(old.find("mode = stick\n") != std::string::npos, "Android's default is still stick");
#else
            old = replaced(old, "mode = look\n", "mode = stick\n");
#endif
            t.IsTrue(old != bmx.defaultIni && old.find("controls_version") == std::string::npos, "an unversioned file");
            TempDir dir;
            const std::filesystem::path file = dir.path / "controls.ini";
            writeTextFile(file, old);
            std::vector<std::string> log;
            t.IsTrue(updateControlsFile(file, "bmx", &log), "rewritten");
            std::string text;
            t.IsTrue(readTextFile(file, text), "read back");
            t.Equals(text, bmx.defaultIni, "the untouched file is today's defaults, byte for byte");
            t.IsFalse(updateControlsFile(file, "bmx"), "once only");
#if !defined(__ANDROID__)
            t.IsTrue(log.size() == 2u && log[0] == "[mouse] mode: stick -> look (the new default)", "the change logged");
#endif
        });

        tc.Run("every game's defaults are those of kControlsRevision", [](TestCase &t)
        {
            // A changed default reaches the players who have a controls.ini only
            // through changedDefaults: one that changes here without an entry
            // there never does. Changing a default: raise kControlsRevision,
            // list the control's earlier default in the game's changedDefaults
            // at that revision, then put the new fingerprints here. A control
            // added (a key no file had: the bottom_screen shortcut, 1.4.0)
            // reaches every file as it is, its default where the file lacks
            // it: new fingerprints, no revision.
            for (const hi::GameControls *game : hi::allGameControls())
            {
                for (const hi::ChangedDefault &change : game->changedDefaults)
                    t.IsTrue(change.revision > 1 && change.revision <= hi::kControlsRevision && !change.earlier.empty(),
                             game->id + ": [" + change.section + "] " + change.key + " changed at a revision there is");
                t.IsTrue(game->defaultIni.find("\ncontrols_version = " + std::to_string(hi::kControlsRevision) + "\n") !=
                             std::string::npos,
                         game->id + ": its defaults say the revision");
            }
#if !defined(__ANDROID__)
            // (Controls added, as the soundtrack's previous_track, next_track and shuffle (F5, F6, F7; chords
            // L3+L1, L3+R1, none), practice_point (F2, L3+L2) and bottom_screen (M, L3+R2), are no changed default:
            // a file without their lines takes the defaults; only the fingerprints change, no revision.)
            const std::vector<std::pair<std::string, uint64_t>> fingerprints = {
                {"ai", 12975978041597033363ull},
                {"dm2", 705358251272615454ull},
                {"bmx", 5699358797554886172ull},
            };
            t.Equals(hi::kControlsRevision, 2, "the revision the fingerprints are of");
            for (const auto &[id, want] : fingerprints)
            {
                const uint64_t got = defaultsFingerprint(hi::gameControls(id));
                t.IsTrue(got == want, id + "'s default controls changed (fingerprint " + std::to_string(got) +
                                          "): give the change a ChangedDefault and a new kControlsRevision");
            }
#endif
        });
    });
}
