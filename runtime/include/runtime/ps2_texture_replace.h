#pragma once

// Host-side replacement pictures for GS textures: the GPU renderer draws a
// replacement wherever the game draws the texture it stands for, and the
// game itself (its RAM, its GS memory, its logic) never sees it. Used for
// the Xbox release's textures (Aggressive Inline, a pack made at build time
// from the user's Xbox disc); a texture pack feature can use the same.
//
// A replacement is keyed by what the GS holds: the texture's size in texels
// (TW x TH) and the FNV-1a 64 hash of its indices, one byte per texel in
// row order (T8 and T4 alike, 4-bit indices unpacked). Keying on content
// rather than on a GS address survives the game moving textures around and
// never shows a replacement on a texture that is not the one it was made
// for. Indexed (T8/T4) textures are keyed by their indices; 32/24/16-bit
// ones ("rgb" entries: movie frames shown at a higher resolution) by the
// FNV-1a 64 of the R, G, B bytes of each texel of every row but the last
// (alpha left out; Aggressive Inline's movie upload stops 8 texels short
// of the end, so the last row is never what the game wrote).
//
// Pictures come in sources (one per pack file): setSource replaces the
// source's previous entries; when two sources have the same key, the one
// set last wins (the level being loaded beats an older level). Sources
// set longest ago are dropped when the pictures exceed the memory budget.
//
// Thread-safe: sources are set by loader threads, looked up by the GS.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::texture_replace
{
    enum class Format : uint8_t
    {
        BC2 = 1,   // DXT3: 16 bytes per 4x4 block, explicit 4-bit alpha
        RGBA8 = 2, // 4 bytes per texel, R G B A
    };

    struct Picture
    {
        uint32_t width = 0, height = 0;
        uint32_t mips = 1; // levels in data, mip 0 first, each tightly packed
        Format format = Format::BC2;
        std::vector<uint8_t> data;
        std::string name; // for logs

        static size_t levelBytes(Format format, uint32_t width, uint32_t height);
        // Size of a level (1 x 1 at least), its offset in data.
        uint32_t levelWidth(uint32_t level) const;
        uint32_t levelHeight(uint32_t level) const;
        size_t levelOffset(uint32_t level) const;
        size_t levelSize(uint32_t level) const;
        // data holds every level it claims.
        bool valid() const;
    };
    using PicturePtr = std::shared_ptr<const Picture>;

    struct Entry
    {
        uint32_t width = 0, height = 0; // the GS texture's TW x TH
        uint64_t indexHash = 0; // indexHash() or, for rgb, rgbHash()
        PicturePtr picture;
        bool rgb = false;
    };

    // FNV-1a 64 of the indices.
    uint64_t indexHash(const uint8_t *indices, size_t count);
    // FNV-1a 64 of bytes 0-2 (R, G, B) of each texel (R | G << 8 | B << 16 |
    // A << 24) of rows 0 .. height - 2 of a width x height picture.
    uint64_t rgbHash(const uint32_t *texels, uint32_t width, uint32_t height);
    // Some source has rgb entries (the GPU renderer hashes RGB textures only then).
    bool hasRgbEntries();
    // Some source is set (cheap; for callers that skip work without one).
    bool hasSources();
    // The index hash of a texture as the GS will hold it, from the texels the
    // game uploads (a row after another, top row first): psm 0x13 (T8, a
    // byte each) or 0x14 (T4, two a byte, the first in the low nibble).
    // 0 for other formats.
    uint64_t indexHashOfUpload(const uint8_t *texels, uint32_t width, uint32_t height, uint32_t psm);

    void setSource(const std::string &source, std::vector<Entry> entries);
    void removeSource(const std::string &source);
    bool hasSource(const std::string &source);
    void clear();

    // The picture for a texture, or null (none, or replacements off).
    PicturePtr find(uint32_t width, uint32_t height, uint64_t indexHash, bool rgb = false);

    // Changes whenever find() may answer differently (a source set or
    // removed, replacements switched on or off). Starts at 1.
    uint64_t generation();

    // On by default; off makes find() answer null for indexed entries
    // (textures; sources are kept). Rgb entries (movie frames, which have
    // their own setting) are found either way.
    void setEnabled(bool enabled);
    bool enabled();

    // Bytes of pictures kept before the oldest sources are dropped
    // (default 512 MiB).
    void setBudget(size_t bytes);

    struct Stats
    {
        size_t sources = 0;
        size_t entries = 0;
        size_t bytes = 0; // picture data held
    };
    Stats stats();

    // Pack files (".xtp", made by a build tool): little-endian,
    //   header  16 bytes: "PXTP", u32 version (1), u32 entry count, u32 0
    //   entries 64 bytes each:
    //     u16 TW, u16 TH, u8 index bits (4 or 8), u8 0, u16 0,
    //     u64 index hash,
    //     u16 width, u16 height, u8 mips, u8 format (Format), u16 0,
    //     u32 data offset (from the file start), u32 data size,
    //     char name[32] (NUL padded)
    //   then the picture data.
    // Entries that do not add up are skipped and counted in `skipped`.
    bool parsePack(const std::vector<uint8_t> &bytes, std::vector<Entry> &out, std::string *error = nullptr,
                   size_t *skipped = nullptr);
    // A pack of these entries (index bits 4 or 8 for all; rgb entries are
    // not written). A picture used by several entries, or pictures alike in
    // name, size, levels, format and data, are written once and come back
    // from parsePack as one picture. Names longer than 32 bytes are cut.
    std::vector<uint8_t> writePack(const std::vector<Entry> &entries, uint8_t indexBits = 8u);
}
