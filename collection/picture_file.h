#pragma once

// A picture file a player gives the collection (a custom map's preview) as
// the menu draws it: a BMP (readBmpImage), a PNG or a JPEG. PNG and JPEG are
// decoded by the window library's own image code (SDL 3.4's, which carries
// stb_image 2.32): SDL_LoadPNG_IO, and SDL_ConvertPixels from its MJPG
// format, the one way SDL opens its JPEG decoder to callers.

#include "collection/menu.h"

#include <cstdint>
#include <vector>

namespace zx::collection
{
    // The most pixels a PNG or a JPEG read has, 4096 by 4096 (64 MB as the
    // menu's RGBA): the picture is made at the size its header says before
    // it decodes, and a header can say 65535 by 65535 (17 GB) in a file of a
    // few bytes. A preview is drawn a few hundred pixels across. (A BMP's
    // pixels are in its file, so its size is the file's.)
    constexpr uint64_t kMostPixels = 4096u * 4096u;

    // False for a file of none of these kinds, one that does not decode, or
    // a PNG or JPEG of more than kMostPixels.
    bool readPictureFile(const std::vector<uint8_t> &file, Image &out);

    // A JPEG's size from its frame header (the first SOFn segment): false
    // for a file that is no JPEG or has no frame header.
    bool jpegSize(const std::vector<uint8_t> &file, int &width, int &height);
}
