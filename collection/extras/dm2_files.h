#pragma once

// Dave Mirra BMX 2's files as the DM2 extras' converters read and write them
// (dm2_xbox_levels.cpp, dm2_xbox_themes.cpp, dm2_xbox_career.cpp), as the
// tooling's dm2_level.py does: chunk trees (its CONTAINERS), the UI
// libraries' trees, ZDIR archives, BMPs, the median cut palette and the
// quaternion rotation, each computed in the same order as the Python so the
// files are the same bytes.

#include "collection/extras/source_files.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zx::app::extras::dm2
{
    // A DM2 file that does not read as the converter expects (a chunk past
    // its parent, a texture with no DDS, a level with no start point).
    class Dm2Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // ---- little-endian fields ---------------------------------------------

    inline uint16_t u16At(const Bytes &d, size_t at)
    {
        if (at + 2 > d.size())
            throw Dm2Error("a field past the end of a record");
        return uint16_t(d[at] | (d[at + 1] << 8));
    }
    inline uint32_t u32At(const Bytes &d, size_t at)
    {
        if (at + 4 > d.size())
            throw Dm2Error("a field past the end of a record");
        return uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
    }
    inline int32_t s32At(const Bytes &d, size_t at) { return int32_t(u32At(d, at)); }
    inline int16_t s16At(const Bytes &d, size_t at) { return int16_t(u16At(d, at)); }
    // A float field widened to a double (the Python computes in doubles).
    inline double f32At(const Bytes &d, size_t at)
    {
        const uint32_t bits = u32At(d, at);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    inline void putU16(Bytes &d, size_t at, uint16_t v)
    {
        d[at] = uint8_t(v);
        d[at + 1] = uint8_t(v >> 8);
    }
    inline void putU32(Bytes &d, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            d[at + size_t(i)] = uint8_t(v >> (8 * i));
    }
    // A double narrowed to the float a file holds (struct.pack("<f")).
    inline void putF32(Bytes &d, size_t at, double v)
    {
        const float f = float(v);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        putU32(d, at, bits);
    }
    inline void appendU16(Bytes &d, uint16_t v)
    {
        d.push_back(uint8_t(v));
        d.push_back(uint8_t(v >> 8));
    }
    inline void appendU32(Bytes &d, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            d.push_back(uint8_t(v >> (8 * i)));
    }
    inline void appendBytes(Bytes &d, const Bytes &more) { d.insert(d.end(), more.begin(), more.end()); }
    inline void appendText(Bytes &d, std::string_view text) { d.insert(d.end(), text.begin(), text.end()); }
    // text and zeros to `size` bytes (bytes.ljust: never cut).
    inline void appendPadded(Bytes &d, std::string_view text, size_t size)
    {
        appendText(d, text);
        for (size_t i = text.size(); i < size; ++i)
            d.push_back(0);
    }
    inline Bytes sliceOf(const Bytes &d, size_t from, size_t to = SIZE_MAX)
    {
        from = std::min(from, d.size());
        to = std::max(from, std::min(to, d.size()));
        return Bytes(d.begin() + long(from), d.begin() + long(to));
    }
    // The bytes before the first zero.
    inline std::string cText(const Bytes &d, size_t from, size_t to = SIZE_MAX)
    {
        const Bytes s = sliceOf(d, from, to);
        return std::string(s.begin(), std::find(s.begin(), s.end(), uint8_t(0)));
    }

    // The engine's name hash (a case-insensitive ELF hash).
    uint32_t nameHash(std::string_view name);

    // ---- chunk trees --------------------------------------------------------

    struct Chunk
    {
        std::string tag;
        Bytes data;
        std::vector<Chunk> kids;
        bool container = false;

        static Chunk node(std::string tag, std::vector<Chunk> kids)
        {
            Chunk c;
            c.tag = std::move(tag);
            c.kids = std::move(kids);
            c.container = true;
            return c;
        }
        static Chunk leaf(std::string tag, Bytes data)
        {
            Chunk c;
            c.tag = std::move(tag);
            c.data = std::move(data);
            return c;
        }
        size_t size() const;
        // The first child with that tag, or null.
        Chunk *find(std::string_view t);
        const Chunk *find(std::string_view t) const;
    };
    using Chunks = std::vector<Chunk>;

    // dm2_level.py parse: a level file's chunks (FORM, TGEO, TGPT, TMTB,
    // TGIN, TXRC, FSCN, TERR, ZPVS, ZDYN read inside). Throws Dm2Error on a
    // chunk past its parent or bytes left over.
    Chunks parse(const Bytes &data, size_t start = 0, size_t end = SIZE_MAX);
    // dm2_xbox.py parse_ui: a UI library's (ULIB, UAST, UMTL, TXRC read
    // inside; bytes after the last whole chunk dropped).
    Chunks parseUi(const Bytes &data, size_t start = 0, size_t end = SIZE_MAX);
    Bytes serialize(const Chunks &chunks);
    // Every chunk, each before its children.
    void walk(Chunks &chunks, const std::function<void(Chunk &)> &visit);
    void walk(const Chunks &chunks, const std::function<void(const Chunk &)> &visit);

    // ---- archives and pictures ------------------------------------------------

    // The entries of a ZDIR archive (.ZAL). Throws Dm2Error when it is not one.
    std::vector<Bytes> zalEntries(const Bytes &data);

    using Rgba = std::array<int, 4>;
    struct Picture
    {
        int width = 0, height = 0;
        std::vector<std::vector<Rgba>> rows; // top row first
    };
    // A 4- or 8-bit BMP; DM2's palettes leave alpha 0 (opaque: read as 255).
    Picture decodeBmp(const Bytes &data, bool opaque);

    struct Palette
    {
        std::vector<Rgba> colours;
        std::map<Rgba, int> index; // each colour's entry
    };
    // At most `boxes` colours for the colours' counts: the median cut by the
    // widest channel, ties ordered by the whole colour (every host makes the
    // same palette).
    Palette medianCut(const std::map<Rgba, long long> &counts, size_t boxes);

    // An 8-bit BMP (bottom-up rows, a BGRA palette of 256) of index rows.
    Bytes bmp8(const std::vector<std::vector<uint8_t>> &rows, const std::vector<Rgba> &palette);

    // ---- geometry ---------------------------------------------------------------

    using Vec3 = std::array<double, 3>;
    using Quat = std::array<double, 4>; // x, y, z, w
    // v rotated by q, the products and sums in dm2_level.py's order.
    Vec3 quatRotate(const Quat &q, const Vec3 &v);

    // The level files' records' sizes and names.
    constexpr size_t kMaterialSize = 0x9C; // a MATI
    constexpr uint32_t kSpawnClass = 0x005846BE; // "Spawn"
}
