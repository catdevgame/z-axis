// The app's own update (collection/updater.h): a release's SHA256SUMS read,
// versions compared, a folder's files replaced and put back, and the whole
// update from a source folder to the restart, then rolled back, with the
// player's files beside the app untouched; going back to an older version,
// the root settings it does not read kept aside and put back after.
#include "MiniTest.h"

#include "collection/settings.h"
#include "collection/updater.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace col = zx::collection;
    namespace fs = std::filesystem;

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("zx-updater-" + std::to_string(now));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void write(const fs::path &file, const std::string &text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    std::string read(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // Every file below `folder` with its contents, but the update folder's.
    std::map<std::string, std::string> files(const fs::path &folder)
    {
        std::map<std::string, std::string> out;
        for (const auto &entry : fs::recursive_directory_iterator(folder))
            if (entry.is_regular_file())
            {
                const std::string rel = entry.path().lexically_relative(folder).generic_string();
                if (rel.rfind("update/", 0) != 0)
                    out[rel] = read(entry.path());
            }
        return out;
    }

    // A package of the Folder kind for these tests: its files one a line,
    // "path=contents", unpacked into a top folder as a release's zip has.
    std::string package(const std::map<std::string, std::string> &contents)
    {
        std::string text;
        for (const auto &[path, data] : contents)
            text += path + "=" + data + "\n";
        return text;
    }

    bool waitFor(col::Updater &updater, col::Updater::Stage stage)
    {
        for (int i = 0; i < 500; ++i)
        {
            if (updater.stage() == stage)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // A release folder: its packages and SHA256SUMS, as a release's dry run gives.
    void release(const fs::path &folder, const std::string &version, const std::map<std::string, std::string> &contents)
    {
        const std::string name = "Z-Axis-Collection-" + version + "-test.zip";
        write(folder / name, package(contents));
        write(folder / "SHA256SUMS", col::fileSha256(folder / name) + "  " + name + "\n");
    }

    col::UpdatePlatform folderPlatform(const fs::path &app, const fs::path &versionFile)
    {
        col::UpdatePlatform platform;
        platform.install = col::UpdatePlatform::Install::Folder;
        platform.packageSuffixes = {"-test-arm64.zip", "-test.zip"};
        platform.installed = app;
        platform.updateFolder = app / "update";
        platform.unpack = [](const fs::path &zip, const fs::path &into, std::string *)
        {
            std::istringstream lines(read(zip));
            std::string line;
            while (std::getline(lines, line))
                if (const size_t eq = line.find('='); eq != std::string::npos)
                    write(into / "Z-Axis Collection" / line.substr(0, eq), line.substr(eq + 1));
            return true;
        };
        platform.packageVersion = [](const fs::path &top) { return read(top / "version.txt"); };
        (void)versionFile;
        platform.relaunch = []() { return 1234L; };
        return platform;
    }
}

void register_updater_tests()
{
    MiniTest::Case("Updater", [](TestCase &tc)
    {
        tc.Run("SHA256SUMS read, names' versions, versions compared", [](TestCase &t)
        {
            const std::string a(64, 'a'), b(64, 'B');
            const auto sums = col::parseSha256Sums(a + "  Z-Axis-Collection-1.3.1-mac.zip\r\n" + "not a line\n" + b +
                                                   " *Z-Axis-Collection-1.3.1-android.apk\n" + a + "  ../escape.zip\n");
            t.Equals(sums.size(), size_t(2), "two lines are sums (a path is not a name)");
            t.Equals(sums[0].name, std::string("Z-Axis-Collection-1.3.1-mac.zip"), "the name, its CR gone");
            t.Equals(sums[1].sha256, std::string(64, 'b'), "the binary mark's line, lower case");
            t.Equals(col::versionInName("Z-Axis-Collection-1.3.1-mac.zip"), std::string("1.3.1"), "the version in a name");
            t.Equals(col::versionInName("Z-Axis-Collection-1.4.0.zip"), std::string("1.4.0"), "before the extension");
            t.Equals(col::versionInName("Z-Axis-Collection-mac.zip"), std::string(), "none");
            t.Equals(col::versionInName("Z-Axis-Collection-1.4.0-mac-arm64.zip"), std::string("1.4.0"), "1.4.0's names, the arch after");
            t.IsTrue(col::compareVersions("1.4.0", "1.3.1") > 0, "1.4.0 is newer than 1.3.1");
            t.IsTrue(col::compareVersions("1.3.10", "1.3.9") > 0, "by number, not by text");
            t.IsTrue(col::compareVersions("1.3.1", "1.3.1") == 0, "the same");
            t.IsTrue(col::compareVersions("dev", "1.0.0") < 0, "a build with no version is older than any");
        });

        tc.Run("a folder's files replaced, then put back, nothing else touched", [](TestCase &t)
        {
            TempDir dir;
            const fs::path app = dir.path / "app";
            write(app / "app.exe", "old exe");
            write(app / "same.dll", "same");
            write(app / "old-only.txt", "kept");
            write(app / "portable.txt", "");
            write(app / "Z-Axis Collection Data" / "settings.ini", "player's");
            const fs::path next = dir.path / "next";
            write(next / "app.exe", "new exe");
            write(next / "same.dll", "same");
            write(next / "fonts" / "new.ttf", "font");
            const auto before = files(app);
            col::FolderChange change;
            std::string error;
            t.IsTrue(col::replaceFiles(next, app, {}, dir.path / "undo", &change, &error), "replaced: " + error);
            t.Equals(change.written, 2, "the changed file and the new one");
            t.Equals(change.unchanged, 1, "the same one left");
            t.Equals(read(app / "app.exe"), std::string("new exe"), "the new exe in place");
            t.Equals(read(dir.path / "undo" / "files" / "app.exe"), std::string("old exe"), "the old one kept");
            t.Equals(read(app / "old-only.txt"), std::string("kept"), "a file the package lacks stays");
            t.Equals(read(app / "Z-Axis Collection Data" / "settings.ini"), std::string("player's"), "the player's folder untouched");
            t.Equals(col::addedFiles(dir.path / "undo"), std::vector<std::string>{"fonts/new.ttf"}, "the new file listed");

            t.IsTrue(col::replaceFiles(dir.path / "undo" / "files", app, col::addedFiles(dir.path / "undo"), dir.path / "redo",
                                       &change, &error),
                     "put back: " + error);
            t.Equals(change.removed, 1, "the new version's own file moved aside");
            t.IsTrue(files(app) == before, "the folder as it was, byte for byte");
            t.Equals(read(dir.path / "redo" / "files" / "app.exe"), std::string("new exe"), "the version rolled back from kept");
        });

        tc.Run("the Package way (Android): an update handed to the installer; no older version, no roll back", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "installed.apk", "1.4.0");
            col::UpdatePlatform platform;
            platform.install = col::UpdatePlatform::Install::Package;
            platform.packageSuffixes = {"-test.zip"};
            platform.installed = dir.path / "installed.apk";
            platform.updateFolder = dir.path / "update";
            std::string handed;
            platform.installPackage = [&handed](const fs::path &apk, std::function<void(std::string)> done, std::string *)
            {
                handed = read(apk);
                done("");
                return true;
            };
            const fs::path older = dir.path / "release-1.3.1", newer = dir.path / "release-1.4.1";
            release(older, "1.3.1", {{"app.exe", "1.3.1"}});
            release(newer, "1.4.1", {{"app.exe", "1.4.1"}});
            {
                col::Updater updater(platform, "1.4.0", older.string());
                t.IsFalse(updater.rollsBack(), "no roll back on Android");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Current), "an older release: not offered, this one stays");
                updater.rollBack();
                t.Equals(updater.takeNotice().rfind("No Roll Back Update here", 0), size_t(0), "Roll Back says Android has none");
            }
            {
                col::Updater updater(platform, "1.4.0", newer.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.1 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Handed), "handed to the installer");
                t.Equals(handed, package({{"app.exe", "1.4.1"}}), "the release's APK, checked");
                t.Equals(updater.previousVersion(), std::string(), "nothing kept");
                t.Equals(read(dir.path / "installed.apk"), std::string("1.4.0"), "the installed APK not touched");
            }
        });

        tc.Run("update from a source folder, restart, roll back; offline and damaged downloads change nothing", [](TestCase &t)
        {
            TempDir dir;
            const fs::path app = dir.path / "Programs" / "Z-Axis Collection";
            write(app / "app.exe", "1.3.1");
            write(app / "version.txt", "1.3.1");
            write(app / "portable.txt", "");
            write(app / "Z-Axis Collection Data" / "saves" / "card.bin", std::string("\0\1\2save", 7));
            const auto installed = files(app);

            // offline: a source that is not there
            {
                col::Updater updater(folderPlatform(app, {}), "1.3.1", (dir.path / "nowhere").string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Failed), "no source: failed");
                t.Equals(updater.summary(), std::string("No connection"), "said so");
                t.IsTrue(files(app) == installed, "nothing changed");
            }

            const fs::path source = dir.path / "release-1.4.0";
            release(source, "1.4.0", {{"app.exe", "1.4.0"}, {"version.txt", "1.4.0"}, {"new.dll", "lib"}});
            // damaged: the package is not the one SHA256SUMS names
            {
                const fs::path damaged = dir.path / "damaged";
                fs::create_directories(damaged);
                fs::copy(source / "SHA256SUMS", damaged / "SHA256SUMS");
                write(damaged / "Z-Axis-Collection-1.4.0-test.zip", "app.exe=evil\n");
                col::Updater updater(folderPlatform(app, {}), "1.3.1", damaged.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.0 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Failed), "the damaged download refused");
                t.IsTrue(files(app) == installed, "nothing changed");
                t.Equals(updater.previousVersion(), std::string(), "nothing kept");
            }

            {
                col::Updater updater(folderPlatform(app, {}), "1.3.1", source.string());
                t.Equals(updater.summary(), std::string("1.3.1"), "its version before a check");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.0 offered");
                t.Equals(updater.summary(), std::string("1.4.0: Enter to update"), "the row says what Enter does");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "in place: restart");
                t.Equals(read(app / "app.exe"), std::string("1.4.0"), "1.4.0 in place");
                t.Equals(read(app / "Z-Axis Collection Data" / "saves" / "card.bin"), std::string("\0\1\2save", 7),
                         "the player's save untouched");
                t.Equals(updater.previousVersion(), std::string("1.3.1"), "1.3.1 kept for Roll Back");
                t.IsTrue(!fs::exists(app / "update" / "download" / "Z-Axis-Collection-1.4.0-test.zip"), "the download let go");
            }
            {
                // (the restarted 1.4.0: the same source is its own version)
                col::Updater updater(folderPlatform(app, {}), "1.4.0", source.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Current), "up to date");
                updater.rollBack();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "rolled back: restart");
                t.IsTrue(files(app) == installed, "1.3.1's files as they were, byte for byte");
                t.Equals(updater.previousVersion(), std::string("1.4.0"), "1.4.0 kept in its turn");
            }
            {
                // (going back from a newer release: an older source is offered too)
                // (its app says 9.9.9: not the version its name gives)
                const fs::path older = dir.path / "release-1.2.0";
                release(older, "1.2.0", {{"app.exe", "1.2.0"}, {"version.txt", "9.9.9"}});
                col::Updater updater(folderPlatform(app, {}), "1.3.1", older.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.2.0 offered");
                t.Equals(updater.summary(), std::string("1.2.0: Enter to go back"), "as going back");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Failed), "a package whose app says another version is refused");
                t.IsTrue(files(app) == installed, "nothing changed");
            }
        });

        tc.Run("root settings added after a version: set aside for it, the rest byte for byte, then put back", [](TestCase &t)
        {
            const std::string settings = "# header\n\n[audio]\n# Volume.\nvolume = 70\n# Shuffle,\n# two lines.\nshuffle = true\n"
                                         "movie_volume = 90\n\n[general]\n# Practice.\npractice = true\nlog_file = true\n";
            const std::string older = "# header\n\n[audio]\n# Volume.\nvolume = 70\nmovie_volume = 90\n\n[general]\nlog_file = true\n";
            auto newKeys = [](const std::string &section, const std::string &key)
            { return (section == "audio" && key == "shuffle") || (section == "general" && key == "practice"); };
            const col::SettingsMove aside = col::setAsideSettings(settings, "", newKeys);
            t.Equals(aside.moved, std::vector<std::string>{"[audio] shuffle", "[general] practice"}, "the two new keys moved");
            t.Equals(aside.settings, older, "the rest as it was, byte for byte, each key's comment gone with it");
            t.IsTrue(aside.newer.find("[audio]\n# Shuffle,\n# two lines.\nshuffle = true\n") != std::string::npos,
                     "the side file has the key with its comment: " + aside.newer);
            t.IsTrue(aside.newer.find("[general]\n# Practice.\npractice = true\n") != std::string::npos, "and the other");
            const col::SettingsMove none = col::setAsideSettings(settings, "", [](const std::string &, const std::string &) { return false; });
            t.IsTrue(none.moved.empty() && none.settings == settings && none.newer.empty(), "none to move: nothing changes");

            // (the older version's own settings.ini, written anew at its quit, with the player's change there)
            const std::string rewritten = "[audio]\nvolume = 50\nmovie_volume = 90\n\n[general]\nlog_file = true\n";
            auto knowsShuffle = [](const std::string &section, const std::string &key) { return section == "audio" && key == "shuffle"; };
            const col::SettingsMove some = col::putBackSettings(rewritten, aside.newer, knowsShuffle);
            t.Equals(some.moved, std::vector<std::string>{"[audio] shuffle"}, "only the key this version reads put back");
            t.Equals(some.settings, std::string("[audio]\nvolume = 50\nmovie_volume = 90\n# Shuffle,\n# two lines.\nshuffle = true\n\n"
                                                "[general]\nlog_file = true\n"),
                     "at the end of its section, the older version's values kept");
            t.IsTrue(some.newer.find("practice = true") != std::string::npos && some.newer.find("shuffle") == std::string::npos,
                     "the other stays aside");
            const col::SettingsMove all = col::putBackSettings(some.settings, some.newer, newKeys);
            t.Equals(all.moved, std::vector<std::string>{"[general] practice"}, "then the other");
            t.Equals(all.newer, std::string(), "the side file empty");
            t.IsTrue(all.settings.find("[general]\nlog_file = true\n# Practice.\npractice = true\n") != std::string::npos,
                     "in its section: " + all.settings);

            // a side file's key set again: replaced, the others kept; one settings.ini sets already keeps its value
            const col::SettingsMove again = col::setAsideSettings("[audio]\nshuffle = false\n", aside.newer, newKeys);
            t.IsTrue(again.newer.find("shuffle = false") != std::string::npos && again.newer.find("shuffle = true") == std::string::npos,
                     "the newer value replaces the one kept");
            t.IsTrue(again.newer.find("practice = true") != std::string::npos, "the other kept");
            const col::SettingsMove both = col::putBackSettings("[audio]\nshuffle = true\n", "[audio]\nshuffle = false\n", newKeys);
            t.Equals(both.settings, std::string("[audio]\nshuffle = true\n"), "settings.ini's own value wins");
            t.Equals(both.newer, std::string(), "and the copy aside is let go");
            const col::SettingsMove noSection = col::putBackSettings("[audio]\nvolume = 1\n", aside.newer, newKeys);
            t.IsTrue(noSection.settings.find("\n[general]\n# Practice.\npractice = true\n") != std::string::npos,
                     "a section settings.ini lacks added: " + noSection.settings);
            const col::SettingsMove crlf = col::setAsideSettings("[audio]\r\nvolume = 1\r\nshuffle = true\r\n", "", newKeys);
            t.Equals(crlf.settings, std::string("[audio]\r\nvolume = 1\r\n"), "Windows line ends kept");
        });

        tc.Run("the root settings by version: 1.4.0's set aside for 1.3.1 and put back by 1.4.0, each value the player's",
               [](TestCase &t)
        {
            namespace app = zx::app;
            t.Equals(app::rootSettingSince("audio", "shuffle"), std::string("1.4.0"), "shuffle is 1.4.0's");
            t.Equals(app::rootSettingSince("general", "practice"), std::string("1.4.0"), "practice too");
            t.Equals(app::rootSettingSince("display", "map_window_on_top"), std::string("1.3.1"), "Always on Top 1.3.1's");
            t.Equals(app::rootSettingSince("audio", "volume"), std::string(), "the volume every release's");
            // (the keys 1.3.1 wrote in the root settings.ini: every other key the root has now needs its version, newer
            // than 1.3.1, or a roll back to 1.3.1 loses it)
            const std::vector<std::string> keys131 = {
                "display.fullscreen", "display.fullscreen_mode", "display.window_width", "display.window_height",
                "display.scaling", "display.widescreen", "display.aspect", "display.vsync", "display.variable_refresh",
                "display.frame_interpolation", "display.show_fps", "display.map_window", "display.map_window_x",
                "display.map_window_y", "display.map_window_width", "display.map_window_height", "display.map_window_on_top",
                "display.clean_top_screen", "display.xbox_textures", "display.xbox_movies", "display.xbox_skaters",
                "display.texture_dump", "graphics.renderer", "graphics.internal_resolution", "graphics.full_height",
                "graphics.render_path", "graphics.motion_blur", "graphics.anti_aliasing", "graphics.texture_filtering",
                "graphics.fog", "graphics.shadows", "graphics.depth_precision", "graphics.perspective_correct",
                "graphics.character_lighting", "graphics.depth_test", "audio.volume", "audio.mute_on_focus_loss",
                "audio.movie_volume", "host.disc", "general.pause_on_focus_loss", "general.skip_intro_movies",
                "general.log_file", "general.record_play", "general.custom_freeplay", "menu.intro_movie", "menu.menu_music",
                "menu.intro_turn"};
            app::Settings player;
            player.volume = 70;
            player.shuffleMusic = true;
            player.practice = true;
            player.mapWindowOnTop = true;
            const std::string now = app::formatSettings(player, app::everyOptionProfile(), app::SettingsScope::App);
            for (const std::string &key : app::settingsKeys(now))
            {
                const size_t dot = key.find('.');
                const std::string section = key.substr(0, dot), name = key.substr(dot + 1);
                const std::string since = app::rootSettingSince(section, name);
                const bool in131 = std::find(keys131.begin(), keys131.end(), key) != keys131.end();
                t.IsTrue(app::rootSettingKnown(section, name), key + " is this version's");
                t.IsTrue(in131 ? col::compareVersions(since, "1.3.1") <= 0 : col::compareVersions(since, "1.3.1") > 0,
                         key + (in131 ? ": 1.3.1 has it" : ": newer than 1.3.1, so its version is in the table"));
            }
            t.IsFalse(app::rootSettingKnown("general", "from_a_later_version"), "a later version's key is not this one's");

            TempDir dir;
            const fs::path file = dir.path / "settings.ini";
            write(file, now);
            col::setAsideNewerSettings(file, "1.4.0");
            t.IsFalse(fs::exists(col::newerSettingsFile(file)), "nothing newer than 1.4.0: no side file");
            t.Equals(read(file), now, "and settings.ini as it was");
            std::vector<std::string> log;
            col::setAsideNewerSettings(file, "1.3.1", &log);
            t.Equals(col::newerSettingsFile(file).string(), (dir.path / "settings.newer.ini").string(), "the side file's name");
            t.Equals(log.size(), size_t(1), "one line logged");
            const std::vector<std::string> left = app::settingsKeys(read(file));
            t.Equals(left.size(), keys131.size(), "settings.ini has only 1.3.1's keys");
            t.IsTrue(std::is_permutation(left.begin(), left.end(), keys131.begin(), keys131.end()), "1.3.1's own keys");
            t.Equals(app::parseSettings(read(file)).volume, 70, "the player's volume stays");
            t.IsTrue(app::parseSettings(read(file)).mapWindowOnTop, "and 1.3.1's Always on Top");
            // (1.3.1's quit: settings.ini written anew, its own keys only; then 1.4.0's start)
            {
                std::string written =
                    app::formatSettings(app::parseSettings(read(file)), app::everyOptionProfile(), app::SettingsScope::App);
                const size_t logFile = written.find("log_file = true");
                t.IsTrue(logFile != std::string::npos, "the log on by default");
                written.replace(logFile, 15, "log_file = false"); // (the player's change under 1.3.1)
                for (const char *gone : {"shuffle", "practice"})
                    if (const size_t at = written.find(std::string("\n") + gone + " = "); at != std::string::npos)
                        written.erase(at + 1, written.find('\n', at + 1) - at); // (1.3.1 writes neither)
                write(file, written);
            }
            col::putBackNewerSettings(file);
            t.IsFalse(fs::exists(col::newerSettingsFile(file)), "all put back: the side file gone");
            const app::Settings back = app::parseSettings(read(file));
            t.IsTrue(back.shuffleMusic && back.practice, "the player's shuffle and practice back");
            t.IsFalse(back.logFile, "the change made under 1.3.1 kept");

            // (to 1.3.0, then back to 1.3.1: Always on Top aside too, then back with 1.3.1, the rest left for 1.4.0)
            write(file, now);
            col::setAsideNewerSettings(file, "1.3.0");
            t.IsFalse(app::parseSettings(read(file)).mapWindowOnTop, "Always on Top aside for 1.3.0");
            const fs::path side = col::newerSettingsFile(file);
            const std::string aside = read(side);
            const col::SettingsMove at131 = col::putBackSettings(read(file), aside, [](const std::string &section, const std::string &key)
            { return col::compareVersions(zx::app::rootSettingSince(section, key), "1.3.1") <= 0; });
            t.Equals(at131.moved, std::vector<std::string>{"[display] map_window_on_top"}, "1.3.1 takes back only its own");
            t.IsTrue(app::parseSettings(at131.settings).mapWindowOnTop, "with the player's value");
            t.IsTrue(app::settingsKeys(at131.newer).size() == 2, "1.4.0's two still aside");
        });

        tc.Run("going back from the row: the older version's settings set aside at the quit; going up: none", [](TestCase &t)
        {
            TempDir dir;
            const fs::path app = dir.path / "Programs" / "Z-Axis Collection";
            write(app / "app.exe", "1.4.0");
            write(app / "version.txt", "1.4.0");
            write(app / "update" / "previous" / "version.txt", "1.3.1");
            write(app / "update" / "previous" / "files" / "app.exe", "1.3.1");
            write(app / "update" / "previous" / "files" / "version.txt", "1.3.1");
            write(app / "update" / "previous" / "added.txt", "");
            const fs::path settings = dir.path / "support" / "settings.ini";
            zx::app::Settings player;
            player.shuffleMusic = true;
            const std::string now = zx::app::formatSettings(player, zx::app::everyOptionProfile(), zx::app::SettingsScope::App);
            write(settings, now);
            const fs::path source = dir.path / "release-1.4.1";
            release(source, "1.4.1", {{"app.exe", "1.4.1"}, {"version.txt", "1.4.1"}});
            {
                col::Updater updater(folderPlatform(app, {}), "1.4.0", source.string());
                t.Equals(updater.restartVersion(), std::string(), "no restart yet");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.1 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "in place: restart");
                t.Equals(updater.restartVersion(), std::string("1.4.1"), "into 1.4.1");
                updater.relaunchAtQuit(settings);
                t.Equals(read(settings), now, "up: settings.ini untouched");
                t.IsFalse(fs::exists(col::newerSettingsFile(settings)), "no side file");
            }
            {
                col::Updater updater(folderPlatform(app, {}), "1.4.1", source.string());
                updater.rollBack();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "rolled back to 1.4.0");
                updater.relaunchAtQuit(settings);
                t.Equals(read(settings), now, "1.4.0 reads every key: untouched");
            }
            const fs::path older = dir.path / "release-1.3.1";
            release(older, "1.3.1", {{"app.exe", "1.3.1"}, {"version.txt", "1.3.1"}});
            {
                col::Updater updater(folderPlatform(app, {}), "1.4.0", older.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.3.1 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "going back to 1.3.1: restart");
                t.Equals(updater.restartVersion(), std::string("1.3.1"), "into 1.3.1");
                t.Equals(read(settings), now, "nothing moved before the quit (the app writes its settings first)");
                updater.relaunchAtQuit(settings);
                t.IsTrue(read(settings).find("shuffle") == std::string::npos, "1.4.0's shuffle out of settings.ini");
                t.IsTrue(read(col::newerSettingsFile(settings)).find("shuffle = true") != std::string::npos, "kept aside");
            }
            col::putBackNewerSettings(settings);
            t.IsTrue(zx::app::parseSettings(read(settings)).shuffleMusic, "1.4.0's start puts it back");
        });

        tc.Run("the version kept before: moved aside first, so an update that cannot do so changes nothing", [](TestCase &t)
        {
            TempDir dir;
            const fs::path app = dir.path / "Programs" / "Z-Axis Collection";
            write(app / "app.exe", "1.4.0");
            write(app / "version.txt", "1.4.0");
            write(app / "update" / "previous" / "version.txt", "1.3.1");
            write(app / "update" / "previous" / "files" / "app.exe", "1.3.1");
            write(app / "update" / "previous" / "added.txt", "");
            const auto installed = files(app);
            const fs::path source = dir.path / "release-1.4.1";
            release(source, "1.4.1", {{"app.exe", "1.4.1"}, {"version.txt", "1.4.1"}});
#if !defined(_WIN32)
            {
                // (what an earlier update could not remove, as a file still in use is on Windows: here a folder with
                // no write permission, so that 1.3.1's cannot be moved aside to its name)
                const fs::path stuck = app / "update" / "previous.old" / "in-use";
                write(stuck / "app.exe", "1.2.0");
                fs::permissions(stuck, fs::perms::owner_read | fs::perms::owner_exec);
                col::Updater updater(folderPlatform(app, {}), "1.4.0", source.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.1 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Failed), "refused: 1.3.1's kept files cannot be moved aside");
                t.IsTrue(files(app) == installed, "nothing changed");
                t.Equals(updater.previousVersion(), std::string("1.3.1"), "1.3.1 still kept");
                fs::permissions(stuck, fs::perms::owner_all);
            }
#endif
            {
                col::Updater updater(folderPlatform(app, {}), "1.4.0", source.string());
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Offered), "1.4.1 offered");
                updater.activate();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "in place: restart");
                t.Equals(read(app / "app.exe"), std::string("1.4.1"), "1.4.1 in place");
                t.Equals(updater.previousVersion(), std::string("1.4.0"), "1.4.0 kept in 1.3.1's place");
                t.IsFalse(fs::exists(app / "update" / "previous.old"), "1.3.1's let go once 1.4.1 is in place");
            }
            {
                col::Updater updater(folderPlatform(app, {}), "1.4.1", source.string());
                updater.rollBack();
                t.IsTrue(waitFor(updater, col::Updater::Stage::Restart), "rolled back: restart");
                t.IsTrue(files(app) == installed, "1.4.0's files as they were");
                t.Equals(updater.previousVersion(), std::string("1.4.1"), "1.4.1 kept in its turn");
                t.IsFalse(fs::exists(app / "update" / "previous.old"), "nothing left aside");
            }
        });
    });
}
