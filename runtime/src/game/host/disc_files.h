#pragma once

// The host's services to the engine's disc device (zge/host/disc_device.cpp):
// the game's files, as the platform's file API, and the kernel's objects and
// waits a device function makes as the PS2 one does. One boot's state
// (zx::session): a game booted after another in the same process reads its
// own disc.
//
// The files: one open per file, <mod>/<path> in each mod folder (the last
// first), then the disc folder or image, then in-memory patches (AI's
// settings menus' UI scripts), by the path as the game gives it
// ("dvd0:\ASSETS\LEVELS\TUT\TUT.TXR" or the device's part of it).
// ZX_MODS_DIR: the mod folders in order (the app sets the game's Mods
// folder and each mod in it, collection/mods.h; zx::game_files::modsRoots
// reads the text). The trace (configure: ZX_TRACE_DISC) logs every
// open (path, size, where it came from) at 1, every read and seek too at 2;
// an open from anywhere but the disc, or of a file that is not there, is
// logged always.

#include "kernel.h"

#include <cstdint>
#include <memory>
#include <string>

namespace zx::game_files
{
    class GameFile;
}

namespace game::host::disc
{
    // What a game adds to the device (null: nothing). A disc file was opened
    // (the path as the game gave it, found or not).
    using FileOpened = void (*)(const std::string &path);
    // `bytes` of the file at `path` from `offset` were read to guest
    // `address`.
    using FileRead = void (*)(const std::string &path, uint64_t offset, uint32_t address, uint32_t bytes);
    struct Additions
    {
        FileOpened fileOpened = nullptr;
        FileRead fileRead = nullptr;
    };
    // The boot's trace level and its game's additions (before its first
    // open).
    void configure(int trace, const Additions &additions);

    // The files: the open ones by handle (never 0), each with its position.
    // The game's files are made at the first use (the device's init): the log
    // says where they come from.
    void prepare();
    bool exists(const std::string &path);
    // A handle, 0 when there is no such file.
    uint32_t open(const std::string &path);
    // False for a handle that is not open.
    bool close(uint32_t handle);
    bool isOpen(uint32_t handle);
    // Up to `bytes` from the handle's position to guest `destination`, clamped
    // at the end of the file; the position moves on by what was read. The
    // bytes read (0 for a handle that is not open).
    uint32_t read(uint32_t handle, uint32_t destination, uint32_t bytes);
    // The position `offset` from the start (origin 0), the position (1) or
    // the end (2), at most the end; false (the position as it was) for
    // another origin, a position before the start or a handle that is not
    // open.
    bool seek(uint32_t handle, int32_t offset, uint32_t origin);
    // 0 for a handle that is not open.
    uint32_t position(uint32_t handle);
    uint32_t size(uint32_t handle);

    // A file as the device serves it (Mods first), for the host's own readers.
    std::shared_ptr<zx::game_files::GameFile> openGameFile(const std::string &path);

    // A path the game gave (at most 256 characters, to its NUL).
    std::string guestPath(uint32_t address);
}

namespace game::host
{
    // ---- after the device function returns (host_form.h) -------------------

    // The completion block's callback (word 0, the function; word 1, its
    // argument) is called with the report {argument, subject, succeeded,
    // value} as the client's RPC end functions give it: in an invocation on
    // the caller's thread, the report below the invocation's stack top; the
    // caller then goes on at its return address. (The device's other waits
    // and the kernel's objects: host/kernel.h.)
    void reportToCallback(uint32_t completion, uint32_t subject, bool succeeded, uint32_t value);
}
