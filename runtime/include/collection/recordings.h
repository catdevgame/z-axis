#pragma once

// The player's recordings in the app (Settings > Recordings): each game's
// recordings listed with their game, level, date and length, and played back
// in the app exactly as they were recorded.
//
// A recording (runtime/play_recording.h) is one boot of one game: what the
// game read from the host, the boot's settings and the saves as they were at
// its start. Playing one back boots the game again from the same point: its
// recorded boot values, its saves copied into a folder of the playback's own
// (the player's saves are never read or written by it), and the recording
// handing the game every value it read. The window's own input stays live
// for pausing and leaving.
//
// Plain C++: the app (collection/app.cpp) lists, boots and shows; this file
// reads the recordings and makes the boot.

#include "runtime/play_recording.h"
#include "runtime/ps2_boot_config.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zx::collection
{
    // The three games' discs are the NTSC releases: 60000/1001 vsyncs a second.
    constexpr double kVsyncsPerSecond = 60000.0 / 1001.0;

    // One recording as the list shows it.
    struct RecordingEntry
    {
        std::filesystem::path file;
        std::string game;      // the collection's id ("ai", "dm2", "bmx")
        std::string started;   // as its header has it: "2026-11-20_14-05-33" (local time)
        std::string commit;    // the build that made it
        uint64_t vsyncs = 0;   // how long it is
        bool ended = false;    // false: cut short (a crash or a kill)
        bool fromApp = false;  // made by the app (not a runner given an ELF)
        std::string level;     // the level played longest, by its panel file's name ("" none)
        bool levelsNoted = false; // false: made before the app noted the levels
        int levels = 0;        // the levels it went to
    };

    // A recording's entry from its summary (play_recording::summarize).
    RecordingEntry recordingEntry(const std::filesystem::path &file, const play_recording::Summary &summary);
    // The recordings (*.zrec) in a game's folder, newest first; files that
    // are not recordings are left out.
    std::vector<RecordingEntry> listRecordings(const std::filesystem::path &folder);

    // How long, as a clock: "0:42", "12:34", "1:02:03".
    std::string lengthText(uint64_t vsyncs);
    // When it was made, from its header's started: "20 Nov 2026 14:05"
    // ("" when it cannot be read).
    std::string startedText(const std::string &started);
    // A level's name from the game's panel files (<folder>/<level>.pmap, its
    // NAME chunk), else the level's folder name as it is.
    std::string levelTitle(const std::filesystem::path &panelFolder, const std::string &level);

    // Where a playback keeps the recording's saves for its boot: a folder of
    // its own, laid out as a game's (collection/settings.h AppPaths).
    struct PlaybackFolders
    {
        std::filesystem::path root;
        std::filesystem::path memcardSlot1, memcardSlot2;
        std::filesystem::path savesDir;
        std::filesystem::path gameplayIni;
        std::filesystem::path controlsFile;
    };
    PlaybackFolders playbackFolders(const std::filesystem::path &root);

    // Empties the folders and writes the saves the recording holds into
    // them (card1/ and card2/ into the card slots, saves/ into the saves
    // folder, gameplay.ini), and a copy of the player's controls.ini (the
    // game's Controls page writes the one it is given). False and why when
    // the recording cannot be read or a file not written.
    bool restoreSaves(const std::filesystem::path &recording, const PlaybackFolders &folders,
                      const std::filesystem::path &controlsFile, std::string *error = nullptr);

    // The boot a playback runs. Every name a boot carries has the recorded
    // boot's value (none: none), but for:
    //   - the saves and controls: the playback's folders;
    //   - what only the player sees or keeps: the renderer, the screenshots'
    //     folder, the GPU pipelines' cache, the gamepad mappings, dumps;
    //   - the folders the app bundles, when the recorded one is not there
    //     now (the app moved): this boot's;
    //   - recording off, and the replay of the file.
    // `current` is the boot the app would give the game now.
    zx::BootConfig playbackBootConfig(const play_recording::Summary &recording, const std::filesystem::path &file,
                                        const zx::BootConfig &current, const PlaybackFolders &folders);
}
