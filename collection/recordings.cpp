// The player's recordings in the app (collection/recordings.h): the list's
// entries and texts, the saves a playback starts from, and its boot.
#include "collection/recordings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace zx::collection
{
    namespace
    {
        // The names a playback takes from this boot, not the recorded one:
        // what only the player sees or keeps, and the developers' switches of
        // this run (dumps, logs, scripts of the window's input).
        const std::set<std::string> &currentNames()
        {
            static const std::set<std::string> names = {
                "ZX_GS_BACKEND", "ZX_GS_SCALE", "ZX_GS_FULL_HEIGHT", "ZX_GAMEPAD_MAPPINGS",
                "ZX_SCREENSHOT_DIR", "ZX_GS_PIPELINE_CACHE", "ZX_TEXTURE_DUMP", "ZX_PAD_SCRIPT",
                "ZX_PAD_SCRIPT_CLOCK", "ZX_HOST_INPUT", "ZX_HOST_INPUT_SCRIPT", "ZX_DUMP_WHEN",
                "ZX_DUMP_PREFIX", "ZX_DUMP_EXTENDED", "ZX_DUMP_AT_FRAME", "ZX_SHOT_AT", "ZX_STATE_LOG",
                "ZX_STATE_LOG_FILE", "ZX_PAD_DUMP", "ZX_PAD_LOG",
            };
            return names;
        }

        // The folders the app bundles (its Resources): the recorded one is
        // the same files unless the app has moved since.
        const std::set<std::string> &bundledNames()
        {
            static const std::set<std::string> names = {"ZX_XBOX_DIR",          "ZX_XBOX_PACK",
                                                        "ZX_XBOX_CONVERTED",    "ZX_ESTATE_DIR",
                                                        "ZX_DM2_XBOX_LEVELS_DIR", "ZX_BMX_RELEASES_DIR"};
            return names;
        }

        bool writeFile(const std::filesystem::path &path, const std::string &bytes)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(out);
        }

        // A file name inside a recording, made safe to write under a folder:
        // no root, no "..".
        bool safeRelative(const std::string &name, std::filesystem::path &out)
        {
            const std::filesystem::path path(name);
            if (name.empty() || path.is_absolute() || path.has_root_name())
                return false;
            for (const std::filesystem::path &part : path)
                if (part == "..")
                    return false;
            out = path;
            return true;
        }
    }

    RecordingEntry recordingEntry(const std::filesystem::path &file, const play_recording::Summary &summary)
    {
        RecordingEntry entry;
        entry.file = file;
        entry.game = summary.value("game");
        entry.started = summary.value("started");
        entry.commit = summary.value("commit");
        entry.vsyncs = summary.endVsync;
        entry.ended = summary.ended;
        entry.fromApp = summary.value("front_end") == "app";
        // The level it was in longest: each "level" event starts a stretch
        // that the next one (or the end) ends.
        std::map<std::string, uint64_t> time;
        std::set<std::string> seen;
        std::string level;
        uint64_t from = 0;
        for (const play_recording::Summary::Event &event : summary.events)
        {
            if (event.kind != "level")
                continue;
            entry.levelsNoted = true;
            if (!level.empty())
                time[level] += event.vsync - std::min(event.vsync, from);
            level = event.text;
            from = event.vsync;
            if (!level.empty())
                seen.insert(level);
        }
        if (!level.empty())
            time[level] += summary.endVsync - std::min(summary.endVsync, from);
        uint64_t longest = 0;
        for (const auto &[name, vsyncs] : time)
            if (entry.level.empty() || vsyncs > longest)
            {
                entry.level = name;
                longest = vsyncs;
            }
        entry.levels = static_cast<int>(seen.size());
        return entry;
    }

    std::vector<RecordingEntry> listRecordings(const std::filesystem::path &folder)
    {
        // Each file is read whole once: its entry is kept by its size and
        // time for the next listing (an hour of play is a few megabytes).
        struct Known
        {
            bool read = false;
            uintmax_t size = 0;
            std::filesystem::file_time_type written{};
            std::optional<RecordingEntry> entry; // none: not a recording
        };
        static std::mutex mutex;
        static std::map<std::filesystem::path, Known> known;
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<RecordingEntry> out;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            std::error_code fileEc;
            if (it->path().extension() != ".zrec" || !it->is_regular_file(fileEc))
                continue;
            const uintmax_t size = it->file_size(fileEc);
            const std::filesystem::file_time_type written = it->last_write_time(fileEc);
            Known &file = known[it->path()];
            if (!file.read || file.size != size || file.written != written)
            {
                file.read = true;
                file.size = size;
                file.written = written;
                play_recording::Summary summary;
                file.entry = play_recording::summarize(it->path(), summary)
                                 ? std::optional<RecordingEntry>(recordingEntry(it->path(), summary))
                                 : std::nullopt;
            }
            if (file.entry)
                out.push_back(*file.entry);
        }
        // Newest first: the header's time reads in order as text; the file's
        // name breaks a tie (two in one second: "-2").
        std::sort(out.begin(), out.end(), [](const RecordingEntry &a, const RecordingEntry &b)
                  { return a.started != b.started ? a.started > b.started : a.file.filename() > b.file.filename(); });
        return out;
    }

    std::string lengthText(uint64_t vsyncs)
    {
        const uint64_t seconds = static_cast<uint64_t>(std::floor(static_cast<double>(vsyncs) / kVsyncsPerSecond));
        char text[32];
        if (seconds >= 3600u)
            std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600u),
                          static_cast<unsigned long long>(seconds / 60u % 60u), static_cast<unsigned long long>(seconds % 60u));
        else
            std::snprintf(text, sizeof(text), "%llu:%02llu", static_cast<unsigned long long>(seconds / 60u),
                          static_cast<unsigned long long>(seconds % 60u));
        return text;
    }

    std::string startedText(const std::string &started)
    {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (std::sscanf(started.c_str(), "%d-%d-%d_%d-%d-%d", &year, &month, &day, &hour, &minute, &second) < 5 || month < 1 ||
            month > 12)
            return {};
        static const std::array<const char *, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char text[48];
        std::snprintf(text, sizeof(text), "%d %s %d %02d:%02d", day, months[static_cast<size_t>(month - 1)], year, hour, minute);
        return text;
    }

    std::string levelTitle(const std::filesystem::path &panelFolder, const std::string &level)
    {
        // collection/panel_file.h: "PMAP", the version, then chunks of a tag
        // and a size, each padded to 4 bytes; NAME is the level's name.
        std::ifstream in(panelFolder / (level + ".pmap"), std::ios::binary);
        char magic[8] = {};
        if (level.empty() || !in.read(magic, sizeof(magic)) || std::memcmp(magic, "PMAP", 4) != 0)
            return level;
        for (int chunk = 0; chunk < 64; ++chunk)
        {
            char tag[4] = {};
            uint8_t sizeBytes[4] = {};
            if (!in.read(tag, sizeof(tag)) || !in.read(reinterpret_cast<char *>(sizeBytes), sizeof(sizeBytes)))
                break;
            const uint32_t size = static_cast<uint32_t>(sizeBytes[0]) | static_cast<uint32_t>(sizeBytes[1]) << 8 |
                                  static_cast<uint32_t>(sizeBytes[2]) << 16 | static_cast<uint32_t>(sizeBytes[3]) << 24;
            if (std::memcmp(tag, "NAME", 4) == 0)
            {
                std::string name(std::min<uint32_t>(size, 256u), '\0');
                if (!in.read(name.data(), static_cast<std::streamsize>(name.size())))
                    break;
                name.resize(std::strlen(name.c_str()));
                return name.empty() ? level : name;
            }
            in.seekg(static_cast<std::streamoff>((size + 3u) & ~3u), std::ios::cur);
        }
        return level;
    }

    PlaybackFolders playbackFolders(const std::filesystem::path &root)
    {
        PlaybackFolders folders;
        folders.root = root;
        folders.memcardSlot1 = root / "memcards" / "slot1";
        folders.memcardSlot2 = root / "memcards" / "slot2";
        folders.savesDir = root / "Saves";
        folders.gameplayIni = root / "gameplay.ini";
        folders.controlsFile = root / "controls.ini";
        return folders;
    }

    bool restoreSaves(const std::filesystem::path &recording, const PlaybackFolders &folders,
                      const std::filesystem::path &controlsFile, std::string *error)
    {
        std::error_code ec;
        std::filesystem::remove_all(folders.root, ec);
        for (const std::filesystem::path &dir : {folders.memcardSlot1, folders.memcardSlot2, folders.savesDir})
            std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            if (error)
                *error = folders.root.string() + ": " + ec.message();
            return false;
        }
        std::string failed;
        const bool read = play_recording::readFiles(recording, [&](const std::string &name, const std::string &bytes)
        {
            std::filesystem::path target;
            if (name == "gameplay.ini")
                target = folders.gameplayIni;
            else
            {
                const size_t slash = name.find('/');
                const std::string label = name.substr(0, slash);
                std::filesystem::path rest;
                if (slash == std::string::npos || !safeRelative(name.substr(slash + 1), rest))
                    return;
                if (label == "card1")
                    target = folders.memcardSlot1 / rest;
                else if (label == "card2")
                    target = folders.memcardSlot2 / rest;
                else if (label == "saves")
                    target = folders.savesDir / rest;
                else
                    return;
            }
            if (!writeFile(target, bytes) && failed.empty())
                failed = target.string();
        });
        if (!read)
        {
            if (error)
                *error = recording.filename().string() + " is not a recording";
            return false;
        }
        if (!controlsFile.empty() && std::filesystem::exists(controlsFile, ec))
            std::filesystem::copy_file(controlsFile, folders.controlsFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (!failed.empty())
        {
            if (error)
                *error = "cannot write " + failed;
            return false;
        }
        return true;
    }

    zx::BootConfig playbackBootConfig(const play_recording::Summary &recording, const std::filesystem::path &file,
                                        const zx::BootConfig &current, const PlaybackFolders &folders)
    {
        zx::BootConfig config;
        config.game = current.game;
        std::map<std::string, std::string> recorded;
        for (const auto &[key, value] : recording.header)
            if (key.rfind("boot.", 0) == 0)
                recorded.emplace(key.substr(5), value);
        std::error_code ec;
        for (const std::string &name : zx::BootConfig::names())
        {
            const std::string *now = current.find(name);
            const auto it = recorded.find(name);
            const bool keepCurrent =
                currentNames().count(name) != 0 ||
                (bundledNames().count(name) != 0 && (it == recorded.end() || !std::filesystem::is_directory(it->second, ec)));
            if (keepCurrent)
            {
                if (now && current.overEnvironment(name))
                    config.setOverEnvironment(name, *now);
                else if (now)
                    config.set(name, *now);
            }
            else if (it != recorded.end())
                config.set(name, it->second);
        }
        // The saves and controls: the playback's own folders.
        config.set("ZX_SAVES_DIR", folders.savesDir.string());
        config.set("ZX_CONTROLS_INI", folders.controlsFile.string());
        if (recorded.count("ZX_GAMEPLAY_INI"))
            config.set("ZX_GAMEPLAY_INI", folders.gameplayIni.string());
        else
            config.erase("ZX_GAMEPLAY_INI");
        // Not recorded again (the player's Record Play, or the environment's),
        // and the file replayed to its end (a developer's ZX_REPLAY_STOP
        // still stops it sooner).
        config.setOverEnvironment("ZX_RECORD", "");
        config.erase("ZX_RECORD_CHECKSUMS");
        config.erase("ZX_REPLAY_STOP");
        config.setOverEnvironment("ZX_REPLAY", file.string());
        return config;
    }
}
