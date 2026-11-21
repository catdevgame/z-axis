#pragma once

// The player's saves and settings in one file, to take them to another
// device or another copy of the app (the Android app's new id is a new app
// with a files folder of its own): a zip of what the player made in the
// support folder, by name (savedInTransfer: saves, memory cards, the
// settings files, a gamepad mapping file), and the same zip put back. The
// app reads and writes it through the window library's streams, so a
// document the system's file dialog gives (Android's content:// URIs) is as
// good as a path.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace zx::app
{
    struct TransferResult
    {
        bool ok = false;
        std::string message; // for the player
        size_t files = 0;
        uint64_t bytes = 0;
    };

    // The export's file, written at an offset (in order, but the zip's
    // writer may go back); false stops it.
    using TransferWrite = std::function<bool(uint64_t offset, const void *data, size_t size)>;
    // The import's file, read at an offset.
    using TransferRead = std::function<bool(uint64_t offset, void *data, size_t size)>;

    // Whether a support folder's file (its path there, '/'-separated) goes
    // into an export, and is taken from one: an allowlist.
    bool savedInTransfer(const std::string &path);

    TransferResult exportSaves(const std::filesystem::path &supportDir, const TransferWrite &write);
    // Every file of an export (of `size` bytes) into the support folder, over
    // the files of the same name there; a zip that is no export is refused.
    TransferResult importSaves(const std::filesystem::path &supportDir, uint64_t size, const TransferRead &read);
}
