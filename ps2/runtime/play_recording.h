#pragma once

// Play recordings: a session of the player's own play, written as it goes,
// that replays here frame for frame.
//
// A recording is one boot of one game (the app's "Record Play"
// setting, or ZX_RECORD=<folder> for any run: <folder>/<game>-<date>.zrec).
// It holds what the game was given that a person or the host decided:
//   - every value the game's thread read from the host's input (each pad
//     port's state as the game saw it after the keyboard, mouse, touch and
//     gamepad mapping, mouse-look, the shortcut keys, the prompt device, the
//     wall clock and file dates, the picture's aspect, the settings registry
//     as the game's own Settings pages read it), each with the order
//     of the read on its channel, so a replay hands back the same values at
//     the same reads;
//   - what the boot was: the build's commit, the game, its ELF's hash, the
//     boot's settings (BootConfig and the process's switches), the device;
//   - the saves as they were at boot (a copy of every file of the memory
//     card folders, the save folder and gameplay.ini, with their hashes);
//   - checksums of EE RAM (one per megabyte) every 60 vsyncs, so a replay
//     proves it is the same game state;
//   - telemetry on the same vsync clock: each host frame's time, the game,
//     VIF1, GS and host threads' CPU time, presents, the pacer's drops;
//   - events: settings changed from the app's menus, the level the game is
//     in (the app's, for its list of recordings), the end of the run (a
//     file without one ended in a crash or a kill).
// Nothing is shown to the player and nothing is asked of them: moments are
// found from the telemetry afterwards (tools/checks/replay.py events).
//
// ZX_REPLAY=<file> replays one: the game's thread reads the recorded
// values instead of the host's, and the checksums are compared as the replay
// reaches them ("[replay] state ..." lines, a summary at the end).
// ZX_REPLAY_STOP=<vsync> stops the run there (a chunk: boot to a moment).
// Run it unpaced, with the host's input off; replay.py sets it all up. The
// app's Settings > Recordings plays one back in the app the same way, paced,
// the window's own input live (collection/recordings.h).
//
// Off (neither set), every call returns at once and changes nothing.
//
// The file: "ZAXREC1\n", then records of a type byte, a length (LEB128)
// and the payload; each record is whole on its own, so a file cut short by
// a crash reads up to its last whole record. Numbers are LEB128 unless
// said. Layout in play_recording.cpp and replay.py.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace play_recording
{
    // What the game's thread reads from the host, one stream each.
    enum class Channel : uint8_t
    {
        Pad0 = 0,          // port 0's pad data, bytes 2..19 (buttons, sticks, pressures)
        Pad1 = 1,          // port 1's
        PortConnected = 2, // whether a pad is in port 1 (PSPadBackend::portConnected)
        Hotkey = 3,        // a shortcut's press count (PSPadBackend::hotkeyPresses), by shortcut
        MouseLook = 4,     // a mouse-look camera frame (PSPadBackend::mouseLookFrame)
        KeyRecent = 5,     // PSPadBackend::keyPressedWithin
        PromptDevice = 6,  // PSPadBackend::activeDevice
        PromptGeneration = 7,
        FreeInput = 8,     // photo mode's input (PSPadBackend::takeFreeInput)
        Date = 9,          // a wall clock time or a file's date the game is given
        ViewAspect = 10,   // PS2Machine::wideAspect
        Widescreen = 11,   // PS2Machine::widescreen
        Random = 12,       // libc rand() as the game calls it
        // The view settings and the render path the game overrides write into
        // the game's RAM from (the far clip, the LOD ends, the blur packet):
        DrawDistance = 13, // PS2Machine::drawDistance (the app's setting)
        MotionBlur = 14,   // PS2Machine::motionBlur
        NativeDraws = 15,  // GSFrontend::nativeDrawsOnBackend (the GPU renderer's own draws)
        // The settings registry as the game's thread reads it
        // (runtime/settings.h: the Settings pages' rows, names, values
        // and texts are the host's state), one stream per read and setting.
        Setting = 16,
        // Clean Top Screen's pieces left off the top screen
        // (panel::hiddenOnTop, as a game's widget-draw hook reads it), by piece.
        TopScreenHud = 17,
        Count
    };

    enum class Mode
    {
        Off,
        Record,
        Replay
    };
    Mode mode();
    inline bool active() { return mode() != Mode::Off; }

    // This build's commit, as a recording's header has it ("commit="): a
    // recording replays exactly on the build that made it.
    const char *buildCommit();

    // What a boot is, for the recording's header and its copy of the saves.
    struct BootInfo
    {
        std::string game; // "ai", "dm2", "bmx"
        // Booted by the app's front end (its menu, folders and settings; a
        // replay goes through it too), not a runner given an ELF.
        bool fromApp = false;
        std::string elfPath;
        std::filesystem::path memcardSlot1;
        std::filesystem::path memcardSlot2;
        std::filesystem::path discRoot;
        const std::atomic<uint64_t> *vsyncTick = nullptr;
        const uint8_t *rdram = nullptr;
    };
    // PS2Machine::loadELFImage: the ELF as loaded, for the header's elf_hash
    // (the app loads the disc's ELF from memory: no file at its path).
    void noteElfImage(const uint8_t *bytes, size_t size);
    // PS2Machine::run, before the game's thread starts: opens the recording
    // (ZX_RECORD) or loads the replay (ZX_REPLAY) of this boot.
    void beginBoot(const BootInfo &info);
    // PS2Machine::run, when the game has stopped: the end record, the replay's summary.
    void endBoot(const char *why);

    // The game's thread says it is (PS2Machine::run): only its reads are
    // recorded and replayed; the window's (the overlay, the menus) stay live.
    void setGameThread();
    bool onGameThread();

    // A read on the game's thread: recording, notes `bytes`; replaying,
    // overwrites them with the recorded value of this read. Elsewhere, or
    // off, leaves them alone. `key` separates streams of one channel (the
    // shortcut's number for Hotkey).
    void read(Channel channel, uint32_t key, void *bytes, size_t size);
    // A read of a text: as read(), the replay's text at the recorded length.
    void readText(Channel channel, uint32_t key, std::string &text);
    template <typename T>
    T read(Channel channel, T value, uint32_t key = 0)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (mode() != Mode::Off)
            read(channel, key, &value, sizeof(value));
        return value;
    }

    // The EE thread at each vsync (before the guest sees it): checksums,
    // writing out, a replay's stop.
    void onVsync(const uint8_t *rdram, uint64_t tick);
    // The replay reached ZX_REPLAY_STOP or the recording's end, or a
    // recording run was sent SIGTERM or SIGINT: the host loop stops the run
    // (PS2Machine::run), and the file gets its end record.
    bool stopRequested();

    // The host loop after each present.
    struct HostFrame
    {
        uint64_t vsync = 0;
        double frameMs = 0.0;   // since the previous present
        double gameCpuMs = 0.0; // CPU time of each thread since the previous present
        double vif1CpuMs = 0.0;
        double gsCpuMs = 0.0;
        double hostCpuMs = 0.0;
        uint32_t flips = 0;       // the game's display flips since the previous present
        double pacerDroppedMs = 0.0;
    };
    void onHostFrame(const HostFrame &frame);

    // A setting changed (runtime/settings.h): from the app's menus
    // (fromGame false) the change reaches the game at a host frame, not at
    // a read of the game's, so the replay applies it at the vsync it came
    // and says so; from the game's own Settings page it follows the pad.
    void onSettingChanged(const std::string &key, int value, bool fromGame);

    // The recording being written ("" when none), and a replay's results so
    // far (the summary endBoot prints).
    std::filesystem::path recordingPath();
    struct ReplayResults
    {
        uint64_t checksumsMatched = 0;
        uint64_t checksumsDiffered = 0;
        uint64_t firstDifferingVsync = 0;
        uint64_t inputsMisplaced = 0; // a read that changed the value came at another vsync than recorded
        uint64_t endVsync = 0;        // the recording's last vsync
        bool recordingEnded = false;  // false: the file was cut short (a crash or a kill)
        uint64_t vsync = 0;           // the vsync the replay is at now
    };
    ReplayResults replayResults();

    // Something went wrong in the run (the game's thread stopped on an
    // error): an event at the vsync, for the moments a replay is asked for.
    void noteError(const std::string &text);

    // Something the app knows of the run, for the list of the player's
    // recordings: an event at the vsync, of `kind` ("level": the level the
    // game is in now, by its panel file's name; "" none). Recording only.
    void noteEvent(const std::string &kind, const std::string &text);

    // A recording read without replaying it (the app's list of the player's
    // recordings): its header's lines, how far it goes, whether it has its
    // end record, its events and the names of the files it holds. False for
    // a file that is not a recording.
    struct Summary
    {
        std::vector<std::pair<std::string, std::string>> header; // "key=value" lines, in order
        uint64_t endVsync = 0; // the end record's vsync, else the last vsync in the file
        bool ended = false;    // false: cut short (a crash or a kill)
        struct Event
        {
            uint64_t vsync = 0;
            std::string kind, text;
        };
        std::vector<Event> events;
        std::vector<std::string> files; // "card1/...", "saves/...", "gameplay.ini"

        // The header's value for key ("" none): "game", "started", "commit", "boot.ZX_...".
        std::string value(const std::string &key) const;
    };
    bool summarize(const std::filesystem::path &file, Summary &out);
    // The files a recording holds (the saves as they were at its boot), by
    // name, in order. False for a file that is not a recording.
    bool readFiles(const std::filesystem::path &file,
                   const std::function<void(const std::string &name, const std::string &bytes)> &each);

    // Where the app keeps a game's recordings, and how many are there (the
    // settings' Recordings line).
    std::filesystem::path folderFor(const std::filesystem::path &supportDir);
    size_t countIn(const std::filesystem::path &folder);
}
