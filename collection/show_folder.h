#pragma once

// A folder of the collection's (Mods, Maps, a game's Recordings) shown in the
// system's file browser: Finder on macOS, Explorer on Windows, through the
// window library's SDL_OpenURL. Android has no browser that may look into
// the app's own files (its file URIs are not handed to other apps), so
// there, as wherever the browser does not open, the caller says where the
// folder is instead.

#include <filesystem>
#include <string>

namespace zx::collection
{
    // The folder as a file URL: "file://" and its path, the parts between
    // slashes (Windows' too: "file:///D:/..."), every byte but letters,
    // digits, "-._~/" and a drive's ":" written as %XX (a space as %20), as
    // RFC 8089 file URLs are. A relative path is taken from the folder the
    // app runs in.
    std::string fileUrl(const std::filesystem::path &folder);

    // Makes the folder when it is not there yet and shows it. False when
    // the system did not open it (Android; no file browser).
    bool showFolder(const std::filesystem::path &folder);

    // The line that says where a folder is, when showFolder did not
    // open it: "Mods: /path/to/Mods".
    std::string folderNotice(const std::string &name, const std::filesystem::path &folder);
}
