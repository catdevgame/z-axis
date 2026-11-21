#pragma once

// Native textures: the Xbox release's pictures for the PS2 textures, read
// from the Xbox files when the game loads a texture library, instead of a
// pack made at build time (tools/xbox_pack.py makes the same entries; see
// texture_library.h). The renderer gets them through
// runtime/ps2_texture_replace.h like the pack's: the game's RAM and logic
// never see them.
//
// The readable texture loader (zge/assets/txr.cpp) tells it which file a
// texture came from. A worker reads the PS2 file and its Xbox counterpart
// (same path below assets, any case; ZX_XBOX_DIR), pairs them and sets
// the source "native:<path>". Files a pack already covers are left to the
// pack. ZX_TRACE_NATIVE_TEXTURES=1 logs every file and every rejected pair.
//
// Off unless ZX_NATIVE_TEXTURES=1 (developers): the game never converts
// another release's files at run time; the app's textures come from the
// pack the packaging step makes (tools/xbox_pack.py). This is the pack's
// rules on the host, kept as the check of texture_library.cpp against it
// (zx-native-assets) and for experiments.

#include "texture_library.h"

#include "runtime/ps2_texture_replace.h"

#include <string>
#include <vector>

namespace zge::native
{
    struct FileTextures
    {
        std::vector<zx::texture_replace::Entry> entries; // PS2 mip levels included
        size_t ps2Textures = 0;
        size_t pairs = 0;
        std::vector<Rejected> rejected;
    };

    // The replacement entries of one PS2 library (`relativePath` below
    // assets, e.g. "levels/TUT/TUT.TXR") from both files' bytes.
    FileTextures texturesForFile(const std::string &relativePath, const std::vector<uint8_t> &ps2File,
                                 const std::vector<uint8_t> &xboxFile);

    // The game is loading a texture from the file it opened as `gamePath`.
    void noteTextureFile(const std::string &gamePath);
}
