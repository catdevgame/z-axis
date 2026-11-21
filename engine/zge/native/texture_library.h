#pragma once

// Z-Axis texture libraries read on the host, both releases (native asset
// loading, first step). A TXR file, and the texture part of a ZIT model or
// UI archive, is a tree of IFF chunks (tag, u32 size, body, padded to 4)
// holding "TXRC" entries: a "TXRH" header (u32 id, u16 width, u16 height,
// 12 zero bytes, the name at +0x14) and the picture,
//   PS2:  "TBMP" (opaque) or "TZMP" (with alpha): a Windows BMP, 4 or 8
//         bits per texel, a 256-colour palette, rows bottom-up; smaller
//         mip levels may follow as more TBMP chunks;
//   Xbox: "TXPR": a u32 count and an XPR0 resource whose D3D texture is
//         DXT3 with a full mip chain for levels, skaters and editor pieces.
//
// pairTextures() finds, for a PS2 library, the Xbox pictures that stand for
// its textures, as tools/modding/xbox_pack.py does at build time (same names, the
// same rules), so the game's loader can take them straight from the Xbox
// files (native_textures.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace zge::native
{
    // A PS2 picture as the GS will hold it: one index per texel, top row
    // first (4-bit indices unpacked).
    struct Ps2Picture
    {
        int width = 0, height = 0;
        int bits = 0;                 // 4 or 8
        std::vector<uint8_t> indices; // width * height
        uint32_t palette[256] = {};   // R | G << 8 | B << 16
    };

    // An Xbox DXT3 (BC2) picture: every mip level, mip 0 first.
    struct XboxPicture
    {
        int format = 0; // D3D format byte; 0x0E is DXT3
        int width = 0, height = 0, mips = 0;
        std::vector<uint8_t> levels;
    };

    struct LibraryTexture
    {
        std::string name;
        uint32_t id = 0;  // TXRH +0: what materials name the texture by
        std::string kind; // TBMP, TZMP, TXPR, TTGA
        Ps2Picture ps2;   // TBMP / TZMP with 4 or 8 bits
        std::vector<Ps2Picture> ps2Mips; // the smaller levels that follow it
        XboxPicture xbox; // TXPR
        bool readable = false; // the picture could be read
    };

    // Every texture entry in a file, in order (nested FORMs and the model
    // and archive containers included).
    std::vector<LibraryTexture> readTextureLibrary(const std::vector<uint8_t> &file);

    // Bytes of each DXT3 level of a w x h picture.
    size_t dxt3LevelBytes(int width, int height);

    struct TexturePair
    {
        const LibraryTexture *ps2 = nullptr;
        const LibraryTexture *xbox = nullptr;
        std::string how; // "name", "renamed"
    };
    struct Rejected
    {
        std::string ps2, xbox, why;
    };

    // The pairs that pass, for one file both releases have (`relativePath`:
    // below the assets folder, any case, e.g. "levels/tut/tut.txr").
    std::vector<TexturePair> pairTextures(const std::string &relativePath, const std::vector<LibraryTexture> &ps2,
                                          const std::vector<LibraryTexture> &xbox,
                                          std::vector<Rejected> *rejected = nullptr);

    // FNV-1a 64 of the indices: the key a replacement is found by
    // (runtime/texture_replace.h).
    uint64_t indexHash(const Ps2Picture &picture);
}
