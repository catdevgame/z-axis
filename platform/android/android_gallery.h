#pragma once

// The Android build's screenshots in the system's Gallery
// (android_gallery.cpp, the Java side Gallery.java).

#include <filesystem>
#include <string>

namespace zx_android
{
    // Copies the PNG at `file` into the shared pictures, in `folder` (a
    // relative path: "Pictures/Z-Axis Collection/Aggressive Inline"), named
    // as the file is. Its content URI, empty when it could not be added (or
    // before the activity installed the Gallery: Gallery.install).
    std::string addToGallery(const std::filesystem::path &file, const std::string &folder);
}
