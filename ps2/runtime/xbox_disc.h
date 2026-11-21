#pragma once

// Files of the Xbox release of the same game that the port reads as they
// are (Aggressive Inline: string tables, fonts, scripts, speech, loading
// screens). The app never reads a disc image: the build (platform/mac/make-app.sh,
// platform/windows/make-windows.ps1) extracts the user's Xbox image once and bundles
// the files the port uses, like the PS2 disc's; what needs another format
// (the Zoo, the Ape) is converted there too (zx-xbox-convert), not here.
//
// The folder is ZX_XBOX_DIR (the app sets it to Resources/xbox; for
// developer runs, any extracted copy): the disc root (default.xbe and
// media/), its media folder or the assets folder, any case.
//
// Paths are relative to media/assets, '/' separated, any case:
// "levels/ZOO/zoo.TXR". All functions are safe to call from any thread.
// Nothing read from it is ever written anywhere.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2_xbox_disc
{
    // Finds the folder (see above). Idempotent: later calls return the
    // first result. False if there is none or it holds no assets folder.
    bool init();
    bool available();
    bool exists(const std::string &path);
    bool readFile(const std::string &path, std::vector<uint8_t> &out);
    // The file's path on the host (a movie the host's player opens itself:
    // engine/host/movies.h setHostMovie), or "" when there is none.
    std::string hostPath(const std::string &path);
    // Names in a directory (files and folders), with their case on the
    // disc; empty if there is no such directory.
    std::vector<std::string> list(const std::string &dir);
    // What was opened, for logs, or why there is nothing.
    std::string description();

    // Uses this folder instead of ZX_XBOX_DIR (tests, tools); drops
    // whatever was open. False if it holds no assets folder.
    bool openPath(const std::string &path);
    // Drops the folder; init() looks again (tests).
    void reset();
}
