#pragma once

// A game's Xbox texture pack, made from the player's PS2 and Xbox discs: the
// Xbox release's picture for each PS2 texture it replaces, keyed by the PS2
// texture, and (Aggressive Inline) the Xbox Bink movie for each PS2 movie.
// Aggressive Inline's is made as tools/modding/xbox_pack.py makes it (the reference:
// the same bytes); DM2's and BMX XXX's the same way from their own Xbox
// libraries' forms (below). The game overrides read them (ZX_XBOX_PACK:
// Xbox Textures, Xbox Movies; game_overrides/zge_xbox_textures.h).
//
//   <out>/textures/<PS2 path>.xtp   one per PS2 file with replacements
//                                   (textures/ASSETS/LEVELS/CIVIC/CIVIC.TXR.xtp)
//   <out>/textures/report.txt       coverage per file, the pairs left out
//   <out>/movies/<PS2 path>.bik     the Xbox movie of each PS2 movie, as it is
//                                   (movies/ASSETS/MOVIES/CUTSCENE/CIVIC.PSS.bik)
//
// Both releases keep their textures in Z-Axis' IFF libraries: TXRC chunks
// with a TXRH header (u32 id, u16 w, u16 h, 12 zero bytes, the name at
// +0x14) and the picture, a PS2 TBMP/TZMP (a Windows BMP, 4 or 8 bits) or an
// Xbox TXPR (u32 0, then an XPR0 bundle with one D3D texture: DXT3 with its
// mip chain). The other games' Xbox libraries differ in form only: DM2's
// pictures are TDDS chunks (u32 0, then a DDS file: DXT3 with its mips for
// levels), BMX XXX's TXRCs are each packed in a ZLZO chunk (u32 the unpacked
// size, then LZO1X: collection/extras/lzo1x.h), and its level props'
// textures are in the levels' ZAL archives (options.archives), where the
// Xbox names mostly differ (the size dropped: "NDMBURNB_ID1128" ->
// "NDMBURNB_ID1"): there a PS2 texture with no Xbox picture by name is
// paired by picture, with the most alike Xbox picture of the archive made
// small (options.maxPictureDiff, maxPictureCover; report.txt lists each
// pair and each texture left out with its nearest).
//
// The Xbox files keep the PS2 names (sizes dropped from AI's skater names,
// IDn slots kept); a few renamed ones are paired by name below. A pair goes
// in when the Xbox picture is DXT3 and at least as large, and
// when both look alike (8x8 thumbnails: the mean RGB difference where both
// are opaque at most maxDiff, the mean difference of the opaque share at
// most maxCover), so a wrong name match never shows. Xbox pictures with
// opaque magenta texels (the PS2's colour key left in) are left out.
//
// The .xtp format is runtime/texture_replace.h's parsePack: the key is
// what the GS holds once the game has uploaded the texture, its size and
// the FNV-1a 64 of its indices (one byte a texel, rows top down).

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    struct XboxTexturePackOptions
    {
        double maxDiff = 48.0; // largest mean thumbnail difference of a pair (0-255)
        double maxCover = 0.5; // largest mean difference of the opaque share a thumbnail cell (0-1)
        bool movies = true;    // the Bink movies too
        bool archives = false; // the ZAL archives' textures too (BMX XXX's level props)
        // An archive's PS2 texture with no Xbox picture by name, paired by picture (32 x 32 at most):
        double maxPictureDiff = 10.0;  // largest mean RGB difference where both are opaque (0-255)
        double maxPictureCover = 0.05; // largest mean difference of the opaque share (0-1)
    };

    // Writes the pack into `out` (which should be empty). `log` gets the
    // summary lines (tools/modding/xbox_pack.py's, without the time taken). False
    // with *error when a file cannot be written.
    bool makeXboxTexturePack(const SourceFiles &ps2, const SourceFiles &xbox, const std::filesystem::path &out,
                             const XboxTexturePackOptions &options, std::vector<std::string> &log, std::string *error);
}
