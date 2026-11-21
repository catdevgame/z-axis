// The player's recordings in the app (collection/recordings.h,
// collection/playback.h): a recording's summary and its entry on the
// Recordings page, the saves a playback starts from, the boot it runs (which
// replays the recording exactly), the page and the shell's Play, and the
// playback's controls and second screen strip.
#include "MiniTest.h"

#include "collection/menu.h"
#include "collection/playback.h"
#include "collection/recordings.h"
#include "runtime/play_recording.h"
#include "runtime/boot_config.h"
#include "runtime/host_input.h"
#include "runtime/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace col = zx::collection;
    namespace pr = play_recording;

    std::filesystem::path scratch(const char *name)
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "zaxis-recordings-tests" / name;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    void writeText(const std::filesystem::path &file, const std::string &text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    std::string readText(const std::filesystem::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    struct Session
    {
        std::atomic<uint64_t> vsync{0};
        std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE, 0u);
    };

    // `frames` vsyncs of play on the game's thread: a pad read a vsync (the
    // host's value `live`), RAM that follows it, the levels the app notes.
    std::vector<uint8_t> play(Session &s, uint64_t frames, uint8_t live)
    {
        std::vector<uint8_t> seen;
        for (uint64_t f = 1; f <= frames; ++f)
        {
            s.vsync.store(f);
            uint8_t pad[18] = {};
            pad[2] = static_cast<uint8_t>(live + f / 20);
            pr::read(pr::Channel::Pad0, 0u, pad, sizeof(pad));
            seen.push_back(pad[2]);
            s.ram[0x300000 + f % 1024] = pad[2];
            if (f == 10)
                pr::noteEvent("level", "IND");
            if (f == 100)
                pr::noteEvent("level", "");
            if (f == 130)
                pr::noteEvent("level", "WHS");
            pr::onVsync(s.ram.data(), f);
        }
        return seen;
    }

    col::MenuInput key(const char *name) { return col::menuInput({ps2_host_input::hostCodeByName(name)}, 0); }
}

void register_recordings_tests()
{
    MiniTest::Case("Recordings", [](TestCase &tc)
    {
        pr::setGameThread();

        tc.Run("a recording's summary, its entry, its saves restored, and a playback's boot that replays it exactly", [](TestCase &t)
        {
            const std::filesystem::path dir = scratch("playback");
            writeText(dir / "games" / "ai" / "memcards" / "slot1" / "BASLUS-20327" / "save.bin", "career");
            writeText(dir / "games" / "ai" / "Saves" / "Career.sav", "native");
            writeText(dir / "games" / "ai" / "gameplay.ini", "[camera]\n");
            writeText(dir / "games" / "ai" / "controls.ini", "[keys]\n");

            // Recorded: the app's boot, Record Play on, French.
            zx::BootConfig recordedBoot;
            recordedBoot.game = "ai";
            recordedBoot.setOverEnvironment("ZX_RECORD", (dir / "games" / "ai" / "Recordings").string());
            recordedBoot.setOverEnvironment("ZX_SAVES_DIR", (dir / "games" / "ai" / "Saves").string());
            recordedBoot.setOverEnvironment("ZX_GAMEPLAY_INI", (dir / "games" / "ai" / "gameplay.ini").string());
            recordedBoot.setOverEnvironment("ZX_LANGUAGE", "fr");
            zx::boot::begin(recordedBoot);
            Session recorded;
            pr::BootInfo info;
            info.game = "ai";
            info.fromApp = true;
            info.memcardSlot1 = dir / "games" / "ai" / "memcards" / "slot1";
            info.memcardSlot2 = dir / "games" / "ai" / "memcards" / "slot2";
            info.vsyncTick = &recorded.vsync;
            info.rdram = recorded.ram.data();
            pr::beginBoot(info);
            const std::filesystem::path file = pr::recordingPath();
            const std::vector<uint8_t> seen = play(recorded, 400, 1);
            pr::endBoot("test");
            zx::boot::end();

            pr::Summary summary;
            t.IsTrue(pr::summarize(file, summary), "summarized: " + file.string());
            t.Equals(summary.value("game"), std::string("ai"), "its game");
            t.Equals(summary.value("commit"), std::string(pr::buildCommit()), "made by this build");
            t.Equals(summary.value("boot.ZX_LANGUAGE"), std::string("fr"), "its boot's values");
            t.Equals(summary.endVsync, uint64_t{400}, "its length");
            t.IsTrue(summary.ended, "its end record");
            t.Equals(summary.events.size(), size_t{3}, "the three level events");
            t.IsTrue(std::find(summary.files.begin(), summary.files.end(), "card1/BASLUS-20327/save.bin") != summary.files.end() &&
                         std::find(summary.files.begin(), summary.files.end(), "saves/Career.sav") != summary.files.end() &&
                         std::find(summary.files.begin(), summary.files.end(), "gameplay.ini") != summary.files.end(),
                     "the saves at its boot");

            const col::RecordingEntry entry = col::recordingEntry(file, summary);
            t.Equals(entry.level, std::string("WHS"), "the level played longest (270 vsyncs against 90)");
            t.Equals(entry.levels, 2, "two levels");
            t.IsTrue(entry.levelsNoted && entry.fromApp && entry.ended, "noted, the app's, ended");
            const std::vector<col::RecordingEntry> listed = col::listRecordings(dir / "games" / "ai" / "Recordings");
            t.IsTrue(listed.size() == 1 && listed[0].file == file, "listed");

            // The playback's saves: the recording's, in a folder of its own; what was there goes.
            const col::PlaybackFolders folders = col::playbackFolders(dir / "games" / "ai" / "Playback");
            writeText(folders.savesDir / "stale.sav", "old");
            writeText(dir / "games" / "ai" / "Saves" / "Career.sav", "played on since");
            std::string error;
            t.IsTrue(col::restoreSaves(file, folders, dir / "games" / "ai" / "controls.ini", &error), "restored: " + error);
            t.Equals(readText(folders.memcardSlot1 / "BASLUS-20327" / "save.bin"), std::string("career"), "the card's save");
            t.Equals(readText(folders.savesDir / "Career.sav"), std::string("native"), "the save as it was at the boot");
            t.IsFalse(std::filesystem::exists(folders.savesDir / "stale.sav"), "nothing else");
            t.Equals(readText(folders.gameplayIni), std::string("[camera]\n"), "gameplay.ini");
            t.Equals(readText(folders.controlsFile), std::string("[keys]\n"), "a copy of the player's controls");
            t.Equals(readText(dir / "games" / "ai" / "Saves" / "Career.sav"), std::string("played on since"),
                     "the player's save untouched");

            // The boot: the recorded values, the playback's folders, the player's renderer, no recording.
            zx::BootConfig now;
            now.game = "ai";
            now.set("ZX_LANGUAGE", "en");
            now.set("ZX_ZOO", "1");
            now.setOverEnvironment("ZX_GS_BACKEND", "gpu");
            now.set("ZX_SCREENSHOT_DIR", (dir / "shots").string());
            now.set("ZX_RECORD", (dir / "games" / "ai" / "Recordings").string());
            const zx::BootConfig boot = col::playbackBootConfig(summary, file, now, folders);
            t.IsTrue(boot.find("ZX_LANGUAGE") && *boot.find("ZX_LANGUAGE") == "fr", "the recorded language");
            t.IsTrue(boot.find("ZX_ZOO") == nullptr, "a value the recorded boot had not: none");
            t.IsTrue(boot.find("ZX_GS_BACKEND") && *boot.find("ZX_GS_BACKEND") == "gpu" && boot.overEnvironment("ZX_GS_BACKEND"),
                     "the player's renderer, over the environment as the app gives it");
            t.IsTrue(boot.find("ZX_SCREENSHOT_DIR") && *boot.find("ZX_SCREENSHOT_DIR") == (dir / "shots").string(),
                     "the player's screenshots folder");
            t.IsTrue(boot.find("ZX_SAVES_DIR") && *boot.find("ZX_SAVES_DIR") == folders.savesDir.string() &&
                         boot.find("ZX_GAMEPLAY_INI") && *boot.find("ZX_GAMEPLAY_INI") == folders.gameplayIni.string() &&
                         boot.find("ZX_CONTROLS_INI") && *boot.find("ZX_CONTROLS_INI") == folders.controlsFile.string(),
                     "the playback's saves, gameplay.ini and controls");
            t.IsTrue(boot.find("ZX_RECORD") && boot.find("ZX_RECORD")->empty() && boot.overEnvironment("ZX_RECORD"),
                     "not recorded again");
            t.IsTrue(boot.find("ZX_REPLAY") && *boot.find("ZX_REPLAY") == file.string(), "the recording replayed");

            // Played back with that boot, from the restored card: every read the recorded one, every state check equal.
            zx::boot::begin(boot);
            Session replayed;
            pr::BootInfo again = info;
            again.memcardSlot1 = folders.memcardSlot1;
            again.memcardSlot2 = folders.memcardSlot2;
            again.vsyncTick = &replayed.vsync;
            again.rdram = replayed.ram.data();
            pr::beginBoot(again);
            t.IsTrue(pr::mode() == pr::Mode::Replay, "replaying");
            t.IsTrue(pr::recordingPath().empty(), "and not recording");
            std::vector<uint8_t> shown;
            for (uint64_t f = 1; f <= 400 && !pr::stopRequested(); ++f)
            {
                replayed.vsync.store(f);
                uint8_t pad[18] = {};
                pad[2] = static_cast<uint8_t>(9 + f); // the host's live value: not what the game reads
                pr::read(pr::Channel::Pad0, 0u, pad, sizeof(pad));
                shown.push_back(pad[2]);
                replayed.ram[0x300000 + f % 1024] = pad[2];
                pr::onVsync(replayed.ram.data(), f);
            }
            const pr::ReplayResults results = pr::replayResults();
            pr::endBoot("test");
            zx::boot::end();
            t.IsTrue(shown == seen, "the recorded reads");
            t.Equals(results.checksumsMatched, uint64_t{6}, "a state check every 60 vsyncs, each equal");
            t.Equals(results.checksumsDiffered, uint64_t{0}, "none differing");
            t.Equals(results.vsync, uint64_t{400}, "where it got to: its end");
        });

        tc.Run("the page's texts: length, date, the level's name", [](TestCase &t)
        {
            t.Equals(col::lengthText(0), std::string("0:00"), "nothing");
            t.Equals(col::lengthText(6000), std::string("1:40"), "6000 vsyncs: 100.1 s");
            t.Equals(col::lengthText(223157), std::string("1:02:03"), "over an hour");
            t.Equals(col::startedText("2026-11-20_14-05-33"), std::string("20 Nov 2026 14:05"), "the date and time");
            t.Equals(col::startedText("soon"), std::string(), "a date it cannot read: none");
            const std::filesystem::path dir = scratch("level-title");
            {
                std::string pmap("PMAP\x03\x00\x00\x00", 8);
                pmap += std::string("MAPI\x04\x00\x00\x00\x01\x02\x03\x04", 12);
                pmap += std::string("NAME\x0b\x00\x00\x00Industrial\x00\x00", 20);
                writeText(dir / "IND.pmap", pmap);
            }
            t.Equals(col::levelTitle(dir, "IND"), std::string("Industrial"), "its panel file's NAME");
            t.Equals(col::levelTitle(dir, "WHS"), std::string("WHS"), "no panel file: the folder's name");
        });

        tc.Run("Settings > Recordings: a row's Play plays it; its end comes back to the row", [](TestCase &t)
        {
            col::Shell shell(3);
            shell.start();
            col::SettingsRow page;
            page.kind = col::SettingsRow::Page;
            page.label = "Recordings";
            page.opens = col::MenuPage::Recordings;
            const std::vector<col::SettingsRow> settings = {page};
            const col::Layout layout = col::layout(960, 720, 3);
            shell.showPage(col::MenuPage::Settings);
            shell.input(key("Enter"), layout, settings, col::pageLayout(960, 720, settings.size()));
            t.IsTrue(shell.page() == col::MenuPage::Recordings, "the row opens the page");

            std::vector<col::SettingsRow> rows;
            col::SettingsRow heading;
            heading.kind = col::SettingsRow::Heading;
            heading.label = "Dave Mirra BMX 2";
            rows.push_back(heading);
            for (int i = 0; i < 2; ++i)
            {
                col::SettingsRow row;
                row.kind = col::SettingsRow::Action;
                row.label = "Woodward";
                row.activate = [i]()
                {
                    col::MenuAction action;
                    action.kind = col::MenuAction::Play;
                    action.game = 1;
                    action.recording = i;
                    return action;
                };
                rows.push_back(row);
            }
            const col::PageLayout recordings = col::pageLayout(960, 720, rows.size());
            shell.input(key("Down"), layout, rows, recordings);
            const col::MenuAction action = shell.input(key("Enter"), layout, rows, recordings);
            t.IsTrue(action.kind == col::MenuAction::Play && action.game == 1 && action.recording == 1, "the second row's Play");
            t.IsTrue(shell.play(action.game, action.recording), "played");
            t.IsTrue(shell.state() == col::State::Game && shell.game() == 1 && shell.playback() == 1, "the game runs it");
            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.IsTrue(shell.state() == col::State::Menu && shell.page() == col::MenuPage::Recordings && shell.chosenRow(rows) == 2,
                     "back on the page, on its row");
            t.Equals(shell.playback(), -1, "no playback");
            t.Equals(shell.lastGame(), -1, "a playback is not a game played");
            shell.input(key("Escape"), layout, rows, recordings);
            t.IsTrue(shell.page() == col::MenuPage::Settings && shell.pageRow() == 0, "Back: Settings, on the Recordings row");
            shell.input(key("Escape"), layout, settings, col::pageLayout(960, 720, settings.size()));
            shell.boot(0);
            t.Equals(shell.playback(), -1, "a game booted plays nothing back");
        });

        tc.Run("the playback's controls: pause, resume, leave; its line", [](TestCase &t)
        {
            namespace hi = ps2_host_input;
            const int space = hi::hostCodeByName("Space"), escape = hi::hostCodeByName("Escape"), enter = hi::hostCodeByName("Enter");
            auto button = [](int b) { return static_cast<uint16_t>(1u << b); };
            t.IsTrue(col::playbackCommand({}, button(hi::kStart), false) == col::PlaybackCommand::Pause, "Start pauses");
            t.IsTrue(col::playbackCommand({space}, 0, false) == col::PlaybackCommand::Pause, "so does Space");
            t.IsTrue(col::playbackCommand({escape}, 0, false) == col::PlaybackCommand::Pause, "and Escape");
            t.IsTrue(col::playbackCommand({}, button(hi::kCross), false) == col::PlaybackCommand::None, "Cross does nothing while it plays");
            t.IsTrue(col::playbackCommand({}, button(hi::kCross), true) == col::PlaybackCommand::Resume, "paused: Cross goes on");
            t.IsTrue(col::playbackCommand({enter}, 0, true) == col::PlaybackCommand::Resume, "Enter too");
            t.IsTrue(col::playbackCommand({}, button(hi::kStart), true) == col::PlaybackCommand::Resume, "Start too");
            t.IsTrue(col::playbackCommand({}, button(hi::kCircle), true) == col::PlaybackCommand::Leave, "Circle leaves");
            t.IsTrue(col::playbackCommand({escape}, 0, true) == col::PlaybackCommand::Leave, "Escape leaves");
            t.Equals(col::playbackPosition(6000, 12000), std::string("1:40 / 3:20"), "where it is");
            t.Equals(col::playbackLine(6000, 12000, false, col::HintDevice::PlayStation), std::string("Playback 1:40 / 3:20   Start Pause"),
                     "playing, a pad");
            t.Equals(col::playbackLine(6000, 12000, true, col::HintDevice::Xbox), std::string("Paused 1:40 / 3:20   A Resume   B Leave"),
                     "paused, an Xbox pad");
            t.Equals(col::playbackLine(6000, 12000, true, col::HintDevice::Keyboard),
                     std::string("Paused 1:40 / 3:20   Enter Resume   Esc Leave"), "paused, the keyboard");
            t.Equals(col::playbackLine(6000, 12000, false, col::HintDevice::Touch), std::string("Playback 1:40 / 3:20"), "touch: no buttons");
        });

        tc.Run("the second screen's strip: at the foot, buttons for a thumb, its taps", [](TestCase &t)
        {
            const col::StripLayout layout = col::stripLayout(1240, 1080);
            t.IsTrue(layout.strip.y + layout.strip.height == 1080 && layout.strip.width == 1240, "a band across the foot");
            for (const col::Rect &button : {layout.pause, layout.leave})
                t.IsTrue(button.width >= 120 && button.height >= 120 && button.x >= 0 && button.x + button.width <= 1240 &&
                             button.y >= layout.strip.y && button.y + button.height <= 1080,
                         "a button in the band, 120 pixels each way at least");
            t.IsTrue(layout.pause.x + layout.pause.width <= layout.leave.x && layout.line.x + layout.line.width <= layout.pause.x,
                     "the line, Pause, Leave, left to right");
            const col::StripLayout small = col::stripLayout(620, 540);
            t.IsTrue(small.pause.height >= 120 && small.leave.width >= 120, "a smaller screen: still for a thumb");

            col::PlaybackStrip &strip = col::PlaybackStrip::shared();
            t.IsFalse(strip.press(600, 1050, 1240, 1080), "none shown: the panel's touch");
            strip.show(col::StripView{"0:10 / 1:00", false});
            const float px = layout.pause.x + 10.0f, py = layout.pause.y + 10.0f;
            t.IsFalse(strip.press(600, 200, 1240, 1080), "above the band: the panel's");
            t.IsTrue(strip.press(px, py, 1240, 1080) && strip.release(px, py, 1240, 1080), "on Pause: the strip's");
            const float lx = layout.leave.x + 10.0f, ly = layout.leave.y + 10.0f;
            t.IsTrue(strip.press(px, py, 1240, 1080) && strip.release(lx, ly, 1240, 1080), "down on one, up on another");
            std::vector<col::PlaybackCommand> taps = strip.takeTaps();
            t.IsTrue(taps.size() == 1 && taps[0] == col::PlaybackCommand::Pause, "one tap: Pause");
            strip.show(col::StripView{"0:10 / 1:00", true});
            strip.press(px, py, 1240, 1080);
            strip.release(px, py, 1240, 1080);
            strip.press(lx, ly, 1240, 1080);
            strip.release(lx, ly, 1240, 1080);
            taps = strip.takeTaps();
            t.IsTrue(taps.size() == 2 && taps[0] == col::PlaybackCommand::Resume && taps[1] == col::PlaybackCommand::Leave,
                     "paused: Resume, then Leave");
            col::Canvas band(1240, layout.strip.height);
            t.IsTrue(strip.draw(band, 1240, 1080), "drawn while shown");
            strip.show(std::nullopt);
            t.IsFalse(strip.draw(band, 1240, 1080), "not once gone");
            t.IsTrue(strip.takeTaps().empty(), "no taps kept");
        });
    });
}
