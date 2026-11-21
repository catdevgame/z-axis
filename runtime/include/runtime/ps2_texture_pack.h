#pragma once

// Texture packs: PNG or DDS pictures a player drops in that the renderers draw in
// place of the game's own textures, named the way PCSX2 names its texture
// dumps and replacements, so a pack made with PCSX2 (or with PCSX2's dump
// tools) works here unchanged and a dump made here works in PCSX2.
//
// A name is what the GS holds, never where (PCSX2's GSTextureCache::
// HashCacheKey, GSTextureReplacements' TextureName):
//   <texture hash>-<clut hash>-<bits>.png   a texture with a palette
//   <texture hash>-<bits>.png               a texture without one
//   ...-r<w>x<h>-<bits>.png                 a region of one (from CLAMP)
//   ...-mip<n>.png                          its mip level n (optional)
// with .png or .dds alike (a DDS holds its own mip levels: -mip<n> files
// are a PNG's; runtime/ps2_texture_dds.h reads DDS files as PCSX2 does).
// The texture hash is XXH3-64 of the GS memory blocks under the texture
// (the indices or colours read out when the texture is smaller than a
// block, has 16 or 24-bit texels or is a region), the CLUT hash XXH3-64 of
// the 16 or 256 palette entries as 32-bit colours, the bits the texture's
// PSM, TW and TH (and TEXA for 16 and 24-bit texels). A mipmapped texture
// is named by level 0 alone (PCSX2 with hardware mipmapping off) or by all
// its levels folded into the hash (PCSX2 with it on, for a draw that reaches
// every level): both names are looked up.
//
// Packs are folders (or folders inside a zip), lowest first: a later
// folder's picture of the same name wins. The pictures are indexed by name
// when the packs are set, read on loader threads (on first use, and all of
// them ahead when they fit the memory budget) and given to the renderers
// as texture_replace::Picture (RGBA8 with a mip chain, or a DDS's BC1, BC2,
// BC3 or BC7 blocks with the levels it has). The PS2 texture is
// drawn until its picture is ready. A picture's alpha is the PS2's (0x80 is
// opaque), as PCSX2's dumps have it.
//
// Nothing of the game sees a pack: its RAM, GS memory and logic are the
// same with or without one. With no pack and no dump folder every query
// here is a relaxed load of one flag.
//
// Thread-safe: set by the boot, asked by the GS threads, filled by loaders.

#include "runtime/ps2_texture_replace.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GSTexaReg;

namespace zx::texture_pack
{
    // ---- names -------------------------------------------------------------

    struct Name
    {
        uint64_t textureHash = 0;
        uint64_t clutHash = 0; // 0 for a texture without a palette
        // TEX0's PSM (6 bits), TW (4), TH (4), then TEXA's TA0 (8, from bit
        // 15), AEM (1) and TA1 (8) for 16 and 24-bit texels.
        uint32_t bits = 0;
        uint32_t regionWidth = 0, regionHeight = 0; // 0: the whole texture on that axis

        bool operator==(const Name &other) const = default;
        bool hasRegion() const { return regionWidth != 0 || regionHeight != 0; }
    };

    struct NameHash
    {
        size_t operator()(const Name &name) const;
    };

    // The rectangle of a texture PCSX2 treats as a texture of its own
    // (SourceRegion::Create): REGION_CLAMP narrower than the texture, or
    // REGION_REPEAT's cell. Bounds are [min, max) in texels.
    struct Region
    {
        bool hasX = false, hasY = false;
        int32_t minX = 0, maxX = 0, minY = 0, maxY = 0;

        int32_t width(uint32_t tw) const { return hasX ? maxX - minX : static_cast<int32_t>(tw); }
        int32_t height(uint32_t th) const { return hasY ? maxY - minY : static_cast<int32_t>(th); }
        // The same region at mip level `level` (SourceRegion::AdjustForMipmap).
        Region atLevel(uint32_t level) const;
    };
    // clamp: the CLAMP register as the GS holds it (WMS, WMT, MINU, MAXU,
    // MINV, MAXV from bit 0). twLog2 / thLog2: TEX0's TW and TH.
    Region regionOf(uint32_t twLog2, uint32_t thLog2, uint64_t clamp);

    // One level of a texture in GS memory.
    struct Level
    {
        uint32_t tbp = 0;   // in 256-byte blocks
        uint32_t tbw = 0;   // in 64 texels
        uint32_t twLog2 = 0, thLog2 = 0;
    };

    bool hasPalette(uint32_t psm);
    uint32_t nameBits(uint32_t psm, uint32_t twLog2, uint32_t thLog2, const GSTexaReg &texa);
    // XXH3-64 over the texture's bytes as PCSX2's HashTextureLevel feeds
    // them, one level after another (levels[0] is level 0; each level's
    // region is region.atLevel(k)). vram: the GS's 4 MiB local memory.
    uint64_t textureHash(const uint8_t *vram, uint32_t psm, const Level *levels, uint32_t levelCount, const Region &region,
                         const GSTexaReg &texa);
    // The names a draw's texture can have in a PCSX2 pack, in the order the
    // packs are searched. PCSX2 names a mipmapped draw by the levels from
    // its nearest one (LOD's floor, its base) to the last, folded together
    // with hardware mipmapping on (its default), and by the base level alone
    // with it off or where the draw only magnifies: each base level's both
    // names, the nearest first. A level 0 at block 0 (the frame buffer: DM2's
    // level textures, AI's distant models) is no base. The first name is
    // what dump mode writes. levels: levelCount levels from level 0 (1: not
    // mipmapped); clamp: the draw's CLAMP; clutHash: 0 without a palette.
    struct Candidate
    {
        Name name;
        uint32_t base = 0;   // the level the name starts at
        uint32_t levels = 1; // levels folded into its hash, from base
        Region region;       // its region at the base level (what its hash reads)
        Region region0;      // the part the picture covers, in level 0's texels
        bool hashed = false; // name.textureHash is set
    };
    // The names without their texture hashes (cheap: no GS memory read),
    // then hashCandidate for each one wanted.
    uint32_t candidateShapes(uint32_t psm, const Level *levels, uint32_t levelCount, uint64_t clamp, const GSTexaReg &texa,
                             uint64_t clutHash, Candidate out[8]);
    void hashCandidate(const uint8_t *vram, uint32_t psm, const Level *levels, const GSTexaReg &texa, Candidate &c);
    // Both: every name hashed (firstOnly: the first alone).
    uint32_t candidateNames(const uint8_t *vram, uint32_t psm, const Level *levels, uint32_t levelCount, uint64_t clamp,
                            const GSTexaReg &texa, uint64_t clutHash, Candidate out[8], bool firstOnly = false);
    // Some pack has a picture of this name's shape (bits and region, the
    // hashes aside): false says no picture without hashing anything.
    bool mayHave(const Name &shape);

    // The palette a draw reads as PCSX2's GSClut::Read32 makes it, from the
    // GS's CLUT buffer (512 halves: a 32-bit entry's low half at i, its high
    // half at i + 256). Returns the entry count (16 or 256; 0 for a texture
    // without a palette).
    uint32_t readClut(const uint16_t *clutBuffer, uint32_t psm, uint32_t cpsm, uint32_t csa, const GSTexaReg &texa,
                      uint32_t out[256]);
    uint64_t clutHash(const uint32_t *entries, uint32_t count);
    // The texture's colours as PCSX2's dump writes them (RGBA, R in the low
    // byte, the palette applied, TEXA for 16 and 24-bit texels): the region
    // (or the whole texture), row after row.
    std::vector<uint32_t> readColors(const uint8_t *vram, uint32_t psm, const Level &level, const Region &region,
                                     const GSTexaReg &texa, const uint32_t *clut);

    // "<texture>-<clut>-r<w>x<h>-<bits>[-mip<n>].png" as above.
    std::string fileName(const Name &name, uint32_t mip = 0);
    // A file name (no folder) in any of PCSX2's forms, with or without its
    // extension's case; *mip the level it holds (0 for the picture itself).
    std::optional<Name> parseFileName(std::string_view fileName, uint32_t *mip = nullptr);

    // ---- packs -------------------------------------------------------------

    // The boot's packs and dump folder: ZX_TEXTURE_PACKS (the folders,
    // game_files::kModsSeparator between them, lowest first: the collection
    // sets it from the game's mods) and ZX_TEXTURE_DUMP (a folder: dump
    // mode). Read again only when the boot changes. Renderers call it when
    // they are made.
    void configureFromBoot();
    // Replaces the packs (an empty list: none). Indexes the names at once.
    void setFolders(const std::vector<std::filesystem::path> &folders);
    // Dump mode into this folder (empty: off). Palette textures only unless
    // direct is set (direct textures include every movie frame).
    void setDumpFolder(const std::filesystem::path &folder, bool direct = false);

    // Some pack has pictures, or dump mode is on: the renderers name the
    // textures they draw. Cheap.
    bool active();
    bool hasPictures();
    bool dumping();
    bool dumpingDirect();

    // The picture of this name: null while it loads (queued now; the PS2
    // texture is drawn meanwhile) or when there is none. *known: the packs
    // have a picture of this name at all.
    texture_replace::PicturePtr find(const Name &name, bool *known = nullptr);
    // Changes whenever find() may answer differently (a picture loaded or
    // dropped, the packs set again). Starts at 1.
    uint64_t generation();

    // Dump mode: writes the texture as <dump folder>/<fileName(name)> once
    // (on a writer thread), unless a pack has it or the file is there.
    void dump(const Name &name, uint32_t width, uint32_t height, std::vector<uint32_t> colors);

    // Reads every picture ahead, up to the budget (on by default:
    // ZX_TEXTURE_PRELOAD=0 reads each at its first use only).
    void setPreload(bool on);
    // Bytes of pictures kept (texels and mips) before the least recently
    // used are dropped: 1 GiB on desktops, 256 MiB on Android.
    void setBudget(size_t bytes);
    size_t defaultBudget();
    // Blocks until every queued read is done (tests, measurements).
    void waitIdle();

    struct Stats
    {
        size_t folders = 0;
        size_t names = 0;        // pictures the packs have
        size_t loaded = 0;       // held in memory now
        size_t bytes = 0;        // their texels (RGBA8 or blocks) and mips
        size_t failed = 0;       // files that would not decode
        size_t requests = 0;     // find() calls that queued a read
        size_t dumped = 0;       // files written by dump mode
        double loadSeconds = 0;  // time the loaders spent reading and decoding
    };
    Stats stats();

    // ---- for the renderers ---------------------------------------------------

    // A picture at the PS2's size: box-filtered to width x height from its
    // nearest level at least as large (the CPU renderer draws at the PS2's
    // resolution; a block format's level decoded). RGBA, R in the low byte.
    std::vector<uint32_t> colorsAt(const texture_replace::Picture &picture, uint32_t width, uint32_t height);
}
