#pragma once

// DDS pictures in texture packs, read as PCSX2 reads them
// (GSTextureReplacementLoaders.cpp's DDSLoader, PCSX2 v2.9.131), so a pack
// made for PCSX2 in DDS looks here as it does there:
//   - block formats, kept as they are (the GPU samples them): DXT1 or DX10
//     BC1_UNORM (71) as BC1; DXT2, DXT3 or BC2_UNORM (74) as BC2; DXT4,
//     DXT5 or BC3_UNORM (77) as BC3; BC7_UNORM (98) as BC7. Level 0 a
//     multiple of 4 texels on both axes.
//   - uncompressed 32-bit A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8 and 24-bit
//     R8G8B8 (the pixel format's flags, bit count and masks exactly these),
//     as RGBA8. Alpha is the file's (PCSX2's: a picture's alpha is the
//     PS2's, 0x80 opaque); without alpha it is 0xFF for X8R8G8B8 and R8G8B8
//     and 0x80 for X8B8G8R8, as PCSX2 sets them.
//   - its mip levels as the file has them (DDSD_MIPMAPCOUNT), each read
//     until one is cut short. PCSX2 makes no levels for a block format: a
//     block picture without them has level 0 alone.
//   - DDS_HEADER_FLAGS_PITCH with LINEARSIZE: dwPitchOrLinearSize is level
//     0's row pitch (PCSX2's reading of the pair).
// Volume and array pictures, other formats and sizes of 32768 or more are
// not read (a cube map's first face is, as PCSX2 reads it); here, as for
// PNG pictures, sizes above 4096 neither.
//
// Blocks are decoded (for the CPU renderer, and for a GPU without the
// format) by bcdec (NOTICE), as Direct3D specifies them and as the GPU
// PCSX2 draws them on samples them.

#include "runtime/ps2_texture_replace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zx::texture_dds
{
    // The file starts with "DDS ".
    bool isDds(const std::vector<uint8_t> &bytes);

    // A DDS file as a picture (block format or RGBA8, levels as above),
    // or false with *error why.
    bool readDds(const std::vector<uint8_t> &bytes, texture_replace::Picture &out, std::string *error = nullptr);

    // A level of a picture as RGBA8 texels (R first in memory, rows top
    // first, levelWidth x levelHeight): block formats decoded, RGBA8 copied.
    std::vector<uint8_t> rgbaLevel(const texture_replace::Picture &picture, uint32_t level);

    // The picture with every level as RGBA8 (the picture itself if it is).
    texture_replace::PicturePtr asRgba8(const texture_replace::PicturePtr &picture);
}
