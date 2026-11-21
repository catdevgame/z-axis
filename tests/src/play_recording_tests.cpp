// Play recordings (runtime/play_recording.h): a session recorded on the game's
// thread replays the same values at the same reads, its checksums prove the
// state, a file cut short still replays, other threads stay live, and the
// app's setting gives a boot its recordings folder.
#include "MiniTest.h"

#include "collection/settings.h"
#include "runtime/play_recording.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace pr = play_recording;

    std::filesystem::path scratch(const char *name)
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "zaxis-play-recording-tests" / name;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // A boot whose BootConfig has these values (the environment left alone).
    void boot(std::initializer_list<std::pair<const char *, std::string>> values)
    {
        zx::BootConfig config;
        config.game = "ai";
        for (const auto &[name, value] : values)
            config.setOverEnvironment(name, value);
        zx::boot::begin(config);
    }

    struct Session
    {
        std::atomic<uint64_t> vsync{0};
        std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE, 0u);
        pr::BootInfo info(const std::filesystem::path &card)
        {
            pr::BootInfo boot;
            boot.game = "ai";
            boot.memcardSlot1 = card;
            boot.vsyncTick = &vsync;
            boot.rdram = ram.data();
            return boot;
        }
    };

    struct Reads
    {
        std::vector<uint8_t> pad;
        uint32_t hotkey = 0;
        std::time_t date = 0;
    };

    // `frames` vsyncs of play: a pad read and a shortcut read each, a date
    // now and then, and RAM that follows the pad (the game's state).
    // `live` makes the host's values (they differ between the two runs).
    std::vector<Reads> play(Session &s, uint64_t frames, int live)
    {
        std::vector<Reads> seen;
        for (uint64_t f = 1; f <= frames; ++f)
        {
            s.vsync.store(f);
            Reads r;
            uint8_t pad[18];
            for (int i = 0; i < 18; ++i)
                pad[i] = static_cast<uint8_t>(live * 31 + (i == 4 ? (f / 7) : 0xFF) + (i == 0 && f % 50 < 5 ? 0x40 : 0));
            pr::read(pr::Channel::Pad0, 0u, pad, sizeof(pad));
            r.pad.assign(pad, pad + 18);
            r.hotkey = pr::read(pr::Channel::Hotkey, static_cast<uint32_t>(live * 100 + f / 90), 3u);
            if (f % 40 == 0)
                r.date = pr::read(pr::Channel::Date, static_cast<std::time_t>(1700000000 + live * 1000 + f));
            // The game's state follows what it read.
            s.ram[0x100000 + f % 4096] = pad[0];
            s.ram[0x200000] = static_cast<uint8_t>(s.ram[0x200000] + pad[4]);
            pr::onVsync(s.ram.data(), f);
            pr::HostFrame frame;
            frame.vsync = f;
            frame.frameMs = 16.7 + (f == 77 ? 80.0 : 0.0);
            pr::onHostFrame(frame);
            seen.push_back(std::move(r));
        }
        return seen;
    }

    std::filesystem::path record(const std::filesystem::path &dir, Session &s, uint64_t frames,
                                 std::vector<Reads> &seen)
    {
        boot({{"ZX_RECORD", (dir / "rec").string()}});
        pr::beginBoot(s.info(dir / "mc0"));
        const std::filesystem::path path = pr::recordingPath();
        seen = play(s, frames, 1);
        pr::endBoot("test");
        zx::boot::end();
        return path;
    }
}

void register_play_recording_tests()
{
    MiniTest::Case("PlayRecording", [](TestCase &tc)
    {
        pr::setGameThread();

        tc.Run("off: reads pass through and nothing is written", [](TestCase &t)
        {
            boot({});
            Session s;
            pr::beginBoot(s.info({}));
            t.IsTrue(pr::mode() == pr::Mode::Off, "no ZX_RECORD or ZX_REPLAY: off");
            t.Equals(pr::read(pr::Channel::Hotkey, 7u, 1u), 7u, "the live value");
            t.IsTrue(pr::recordingPath().empty(), "no file");
            pr::endBoot("test");
            zx::boot::end();
        });

        tc.Run("a replay hands back the recorded values at the same reads, and its checksums match", [](TestCase &t)
        {
            const std::filesystem::path dir = scratch("roundtrip");
            {
                std::filesystem::create_directories(dir / "mc0" / "BASLUS-20327");
                std::ofstream(dir / "mc0" / "BASLUS-20327" / "save.bin", std::ios::binary) << "career";
            }
            Session recorded;
            std::vector<Reads> seen;
            const std::filesystem::path file = record(dir, recorded, 600, seen);
            t.IsTrue(std::filesystem::exists(file), "the recording: " + file.string());
            t.Equals(file.parent_path(), dir / "rec", "in ZX_RECORD's folder");
            t.Equals(pr::countIn(dir / "rec"), size_t{1}, "counted");

            Session replayed;
            boot({{"ZX_REPLAY", file.string()}});
            pr::beginBoot(replayed.info({}));
            t.IsTrue(pr::mode() == pr::Mode::Replay, "replaying");
            const std::vector<Reads> again = play(replayed, 600, 2);
            const pr::ReplayResults results = pr::replayResults();
            pr::endBoot("test");
            zx::boot::end();
            bool same = again.size() == seen.size();
            for (size_t i = 0; same && i < seen.size(); ++i)
                same = again[i].pad == seen[i].pad && again[i].hotkey == seen[i].hotkey && again[i].date == seen[i].date;
            t.IsTrue(same, "every read the recorded value, not the host's");
            t.Equals(results.checksumsMatched, uint64_t{10}, "a checksum every 60 vsyncs, each matching");
            t.Equals(results.checksumsDiffered, uint64_t{0}, "none differing");
            t.Equals(results.inputsMisplaced, uint64_t{0}, "every change at its recorded vsync");
            t.IsTrue(results.recordingEnded, "the file has its end");
            t.Equals(results.endVsync, uint64_t{600}, "to the last vsync");
            t.IsTrue(std::filesystem::file_size(file) < 32 * 1024, "small: " + std::to_string(std::filesystem::file_size(file)) + " bytes");
        });

        tc.Run("a different state is found at the next checksum; a stop vsync stops the replay", [](TestCase &t)
        {
            const std::filesystem::path dir = scratch("differs");
            Session recorded;
            std::vector<Reads> seen;
            const std::filesystem::path file = record(dir, recorded, 300, seen);
            Session replayed;
            boot({{"ZX_REPLAY", file.string()}, {"ZX_REPLAY_STOP", "200"}});
            pr::beginBoot(replayed.info({}));
            replayed.ram[0x1234567] = 1; // the 18th megabyte, not touched by play()
            for (uint64_t f = 1; f <= 300 && !pr::stopRequested(); ++f)
            {
                replayed.vsync.store(f);
                pr::onVsync(replayed.ram.data(), f);
                if (f == 200)
                    t.IsTrue(pr::stopRequested(), "stopped at ZX_REPLAY_STOP");
            }
            const pr::ReplayResults results = pr::replayResults();
            pr::endBoot("test");
            zx::boot::end();
            t.IsTrue(results.checksumsDiffered > 0u, "the state differs");
            t.Equals(results.firstDifferingVsync, uint64_t{60}, "first at the first checksum");
        });

        tc.Run("a file cut short (a crash) replays up to its last whole record", [](TestCase &t)
        {
            const std::filesystem::path dir = scratch("cut");
            Session recorded;
            std::vector<Reads> seen;
            const std::filesystem::path file = record(dir, recorded, 600, seen);
            std::filesystem::resize_file(file, std::filesystem::file_size(file) * 2 / 3);
            Session replayed;
            boot({{"ZX_REPLAY", file.string()}});
            pr::beginBoot(replayed.info({}));
            const pr::ReplayResults results = pr::replayResults();
            pr::endBoot("test");
            zx::boot::end();
            t.IsFalse(results.recordingEnded, "no end record");
            t.IsTrue(results.endVsync > 100u && results.endVsync < 600u, "ends where the file does: " + std::to_string(results.endVsync));
        });

        tc.Run("only the game's thread is recorded and replayed", [](TestCase &t)
        {
            const std::filesystem::path dir = scratch("threads");
            Session recorded;
            boot({{"ZX_RECORD", (dir / "rec").string()}});
            pr::beginBoot(recorded.info({}));
            recorded.vsync.store(1);
            t.Equals(pr::read(pr::Channel::Hotkey, 5u, 0u), 5u, "recorded");
            uint32_t other = 0;
            std::thread([&other]() { other = pr::read(pr::Channel::Hotkey, 9u, 0u); }).join();
            const std::filesystem::path file = pr::recordingPath();
            pr::endBoot("test");
            zx::boot::end();
            Session replayed;
            boot({{"ZX_REPLAY", file.string()}});
            pr::beginBoot(replayed.info({}));
            replayed.vsync.store(1);
            uint32_t otherReplay = 0;
            std::thread([&otherReplay]() { otherReplay = pr::read(pr::Channel::Hotkey, 11u, 0u); }).join();
            t.Equals(pr::read(pr::Channel::Hotkey, 6u, 0u), 5u, "the game's thread: the recorded value");
            t.Equals(otherReplay, 11u, "another thread: live");
            t.Equals(other, 9u, "another thread was live while recording");
            pr::endBoot("test");
            zx::boot::end();
        });

        tc.Run("the game's Settings page reads the registry as recorded: rows, names, values, texts", [](TestCase &t)
        {
            namespace st = zx::settings;
            // The host's state: the live run has a volume of 80 and a full
            // screen row; the replay's host 100 and no such row.
            auto host = [](int volume, bool fullscreen)
            {
                st::clear();
                st::Setting v;
                v.key = "audio.volume";
                v.label = "Master Volume";
                v.kind = st::Kind::Range;
                v.maxValue = 100;
                v.step = 5;
                v.unit = "%";
                v.current = [volume]() { return volume; };
                st::add(v);
                if (fullscreen)
                {
                    st::Setting f;
                    f.key = "display.fullscreen";
                    f.label = "Full Screen";
                    f.current = []() { return 1; };
                    st::add(f);
                }
            };
            struct Page
            {
                std::vector<std::string> keys;
                std::string volume, fullscreen, label;
                int value = 0;
                bool has = false;
            };
            auto page = []()
            {
                Page p;
                p.keys = st::keys("");
                p.volume = st::text("audio.volume");
                p.fullscreen = st::text("display.fullscreen");
                st::Setting setting;
                if (st::describe("display.fullscreen", setting))
                    p.label = setting.label;
                p.value = st::value("audio.volume");
                p.has = st::has("display.fullscreen");
                return p;
            };
            const std::filesystem::path dir = scratch("settings");
            Session recorded;
            host(80, true);
            boot({{"ZX_RECORD", (dir / "rec").string()}});
            pr::beginBoot(recorded.info({}));
            recorded.vsync.store(1);
            const Page live = page();
            recorded.vsync.store(2);
            st::set("audio.volume", 5); // a shorter text: "5%"
            const Page shorter = page();
            const std::filesystem::path file = pr::recordingPath();
            pr::endBoot("test");
            zx::boot::end();

            Session replayed;
            host(100, false);
            boot({{"ZX_REPLAY", file.string()}});
            pr::beginBoot(replayed.info({}));
            replayed.vsync.store(1);
            const Page again = page();
            replayed.vsync.store(2);
            const Page againShorter = page();
            std::string other;
            std::thread([&other]() { other = st::text("audio.volume"); }).join();
            pr::endBoot("test");
            zx::boot::end();
            st::clear();
            t.Equals(live.volume, std::string("80%"), "recorded: the live host's text");
            t.Equals(again.volume, live.volume, "replayed: the recorded text, not the replay host's");
            t.Equals(again.keys.size(), size_t{2}, "the recorded rows, the full screen row with them");
            t.Equals(again.fullscreen, std::string("On"), "a row the replay's host has not: its recorded text");
            t.Equals(again.label, std::string("Full Screen"), "and its name");
            t.IsTrue(again.has && again.value == 80, "its presence and the value as recorded");
            t.Equals(againShorter.volume, std::string("5%"), "a text of another length, whole");
            t.Equals(againShorter.volume, shorter.volume, "as recorded");
            t.Equals(other, std::string("100%"), "another thread: the live registry");
        });

        tc.Run("the app's setting: a recordings folder for the boot, off by default", [](TestCase &t)
        {
            zx::app::Settings settings;
            t.IsFalse(settings.recordPlay, "off by default");
            zx::app::AppPaths paths;
            paths.supportDir = "/support/games/ai";
            const auto &profile = zx::app::gameSettingsProfile("ai");
            zx::app::BootFolders folders;
            zx::BootConfig off = zx::app::gameBootConfig("ai", settings, paths, profile, folders, nullptr);
            t.IsTrue(off.find("ZX_RECORD") == nullptr, "off: no recording");
            settings.recordPlay = true;
            zx::BootConfig on = zx::app::gameBootConfig("ai", settings, paths, profile, folders, nullptr);
            t.IsTrue(on.find("ZX_RECORD") != nullptr, "on: the folder");
            if (on.find("ZX_RECORD"))
                t.Equals(std::filesystem::path(*on.find("ZX_RECORD")), pr::folderFor(paths.supportDir),
                         "the game's Recordings folder");
            t.IsTrue(zx::BootConfig::carries("ZX_REPLAY") && zx::BootConfig::carries("ZX_REPLAY_STOP"),
                     "a boot carries the replay's values");
        });
    });
}
