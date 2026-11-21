#include "MiniTest.h"
#include "collection/extras/disc_image.h"
#include "collection/disc_finder.h"
#include "collection/extras/gamecube_disc.h"
#include "collection/extras/panel_maps.h"
#include "collection/extras/setup.h"
#include "collection/extras/source_files.h"
#include "collection/extras/xbox_disc.h"
#include "collection/extras/xbox_texture_pack.h"
#include "collection/settings.h"
#include "runtime/texture_replace.h"
#include "collection/extras/lzo1x.h"
#include "collection/extras/sequence_matcher.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// The extras' readers (collection/extras): hand-made Xbox and GameCube
// images, and a tiny RVZ made by the tooling's gc_disc.py reference (its
// bytes below: a 64 KiB GameCube disc in two zstd groups, the second packed:
// five bytes of data, then junk regenerated from a seed).

namespace
{
    namespace fs = std::filesystem;
    namespace ex = zx::app::extras;

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("zx-extras-" + std::to_string(now));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void write(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void le16(std::vector<uint8_t> &d, size_t at, uint16_t v)
    {
        d[at] = uint8_t(v);
        d[at + 1] = uint8_t(v >> 8);
    }
    void le32(std::vector<uint8_t> &d, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            d[at + size_t(i)] = uint8_t(v >> (8 * i));
    }
    void be32(std::vector<uint8_t> &d, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            d[at + size_t(i)] = uint8_t(v >> (24 - 8 * i));
    }

    // An XDVDFS entry: u16 left, u16 right (4-byte units), u32 sector, u32
    // size, u8 attributes, u8 name length, the name.
    void entry(std::vector<uint8_t> &d, size_t at, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
               uint8_t attributes, const std::string &name)
    {
        le16(d, at, left);
        le16(d, at + 2, right);
        le32(d, at + 4, sector);
        le32(d, at + 8, size);
        d[at + 12] = attributes;
        d[at + 13] = uint8_t(name.size());
        std::memcpy(d.data() + at + 14, name.data(), name.size());
    }

    // An XISO: default.xbe and media/a.txt.
    std::vector<uint8_t> xiso()
    {
        std::vector<uint8_t> d(37 * 2048, 0);
        std::memcpy(d.data() + 0x10000, "MICROSOFT*XBOX*MEDIA", 20);
        le32(d, 0x10000 + 20, 33); // the root table's sector
        le32(d, 0x10000 + 24, 64); // and size
        entry(d, 33 * 2048, 0, 7, 35, 4, 0, "default.xbe");
        entry(d, 33 * 2048 + 28, 0, 0, 34, 32, 0x10, "media");
        entry(d, 34 * 2048, 0, 0, 36, 3, 0, "a.txt");
        std::memcpy(d.data() + 35 * 2048, "XBEH", 4);
        std::memcpy(d.data() + 36 * 2048, "abc", 3);
        return d;
    }

    // A GCM: sub/f.bin and top.bin.
    std::vector<uint8_t> gcm()
    {
        std::vector<uint8_t> d(0x800, 0);
        std::memcpy(d.data(), "GTST01", 6);
        be32(d, 0x1C, 0xC2339F3D);
        std::memcpy(d.data() + 0x20, "Test", 4);
        const std::string names("sub\0f.bin\0top.bin\0", 18);
        be32(d, 0x420, 0x780); // main.dol
        be32(d, 0x424, 0x500); // the FST
        be32(d, 0x428, uint32_t(4 * 12 + names.size()));
        be32(d, 0x500, 0x01000000); // the root
        be32(d, 0x508, 4);
        be32(d, 0x50C, 0x01000000 | 0); // sub: parent 0, its entries end at 3
        be32(d, 0x514, 3);
        be32(d, 0x518, 4); // f.bin
        be32(d, 0x51C, 0x600);
        be32(d, 0x520, 4);
        be32(d, 0x524, 10); // top.bin
        be32(d, 0x528, 0x700);
        be32(d, 0x52C, 2);
        std::memcpy(d.data() + 0x530, names.data(), names.size());
        std::memcpy(d.data() + 0x600, "four", 4);
        std::memcpy(d.data() + 0x700, "hi", 2);
        return d;
    }

    const std::vector<uint8_t> kTestRvz = {
            0x52, 0x56, 0x5a, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc,
            0xa5, 0x1b, 0xd4, 0x29, 0xa6, 0x67, 0x2e, 0x3e, 0x65, 0x25, 0x19, 0x95, 0x40, 0xf8, 0x8a, 0xef,
            0xf9, 0x77, 0xed, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x02, 0xd3, 0x12, 0x28, 0x08, 0x96, 0x8e, 0x31, 0x42, 0x4f, 0x34, 0xe8, 0x47, 0xe9,
            0xd7, 0x6a, 0x2a, 0x6a, 0x22, 0x75, 0x9a, 0x15, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05,
            0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x80, 0x00, 0x47, 0x54, 0x53, 0x54, 0x30, 0x31, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xc2, 0x33, 0x9f, 0x3d, 0x54, 0x65, 0x73, 0x74, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x94, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xb2, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x28, 0xb5, 0x2f, 0xfd, 0x60, 0x00, 0x7f, 0x55, 0x08, 0x00, 0x04, 0x10,
            0x00, 0x07, 0x0e, 0x15, 0x1c, 0x23, 0x2a, 0x31, 0x38, 0x3f, 0x46, 0x4d, 0x54, 0x5b, 0x62, 0x69,
            0x70, 0x77, 0x7e, 0x85, 0x8c, 0x93, 0x9a, 0xa1, 0xa8, 0xaf, 0xb6, 0xbd, 0xc4, 0xcb, 0xd2, 0xd9,
            0xe0, 0xe7, 0xee, 0xf5, 0xfc, 0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49,
            0x50, 0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d, 0xa4, 0xab, 0xb2, 0xb9,
            0xc0, 0xc7, 0xce, 0xd5, 0xdc, 0xe3, 0xea, 0xf1, 0xf8, 0xff, 0x06, 0x0d, 0x14, 0x1b, 0x22, 0x29,
            0x30, 0x37, 0x3e, 0x45, 0x4c, 0x53, 0x5a, 0x61, 0x68, 0x6f, 0x76, 0x7d, 0x84, 0x8b, 0x92, 0x99,
            0xa0, 0xa7, 0xae, 0xb5, 0xbc, 0xc3, 0xca, 0xd1, 0xd8, 0xdf, 0xe6, 0xed, 0xf4, 0xfb, 0x02, 0x09,
            0x10, 0x17, 0x1e, 0x25, 0x2c, 0x33, 0x3a, 0x41, 0x48, 0x4f, 0x56, 0x5d, 0x64, 0x6b, 0x72, 0x79,
            0x80, 0x87, 0x8e, 0x95, 0x9c, 0xa3, 0xaa, 0xb1, 0xb8, 0xbf, 0xc6, 0xcd, 0xd4, 0xdb, 0xe2, 0xe9,
            0xf0, 0xf7, 0xfe, 0x05, 0x0c, 0x13, 0x1a, 0x21, 0x28, 0x2f, 0x36, 0x3d, 0x44, 0x4b, 0x52, 0x59,
            0x60, 0x67, 0x6e, 0x75, 0x7c, 0x83, 0x8a, 0x91, 0x98, 0x9f, 0xa6, 0xad, 0xb4, 0xbb, 0xc2, 0xc9,
            0xd0, 0xd7, 0xde, 0xe5, 0xec, 0xf3, 0xfa, 0x01, 0x08, 0x0f, 0x16, 0x1d, 0x24, 0x2b, 0x32, 0x39,
            0x40, 0x47, 0x4e, 0x55, 0x5c, 0x63, 0x6a, 0x71, 0x78, 0x7f, 0x86, 0x8d, 0x94, 0x9b, 0xa2, 0xa9,
            0xb0, 0xb7, 0xbe, 0xc5, 0xcc, 0xd3, 0xda, 0xe1, 0xe8, 0xef, 0xf6, 0xfd, 0x04, 0x0b, 0x12, 0x19,
            0x20, 0x27, 0x2e, 0x35, 0x3c, 0x43, 0x4a, 0x51, 0x58, 0x5f, 0x66, 0x6d, 0x74, 0x7b, 0x82, 0x89,
            0x90, 0x97, 0x9e, 0xa5, 0xac, 0xb3, 0xba, 0xc1, 0xc8, 0xcf, 0xd6, 0xdd, 0xe4, 0xeb, 0xf2, 0xf9,
            0x01, 0x00, 0x00, 0xfd, 0xfe, 0xc0, 0xbe, 0xa6, 0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x51, 0x89, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x05, 0x48, 0x45, 0x4c, 0x4c, 0x4f, 0x80, 0x00, 0x7f, 0xfb, 0x00, 0x01,
            0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
            0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
            0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41,
            0x42, 0x43, 0x00, 0x00, 0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x18, 0xad, 0x00, 0x00, 0x70, 0x00, 0x00,
            0x80, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x40, 0xf0,
            0x05, 0xb0, 0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x18, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x80,
            0x00, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8e, 0x80, 0x00, 0x00, 0x5a, 0x00,
            0x00, 0x00, 0x51,
    };

    std::string text(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }
}

namespace
{
    // ---- Z-Axis texture libraries, hand-made ---------------------------------

    using Bytes = std::vector<uint8_t>;

    Bytes chunk(const char *tag, const Bytes &body)
    {
        Bytes out(tag, tag + 4);
        out.resize(8);
        le32(out, 4, uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    Bytes operator+(Bytes a, const Bytes &b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    // TXRH: u32 id, u16 width, u16 height, 12 zero bytes, the name (32 bytes).
    Bytes txrh(const std::string &name, uint16_t width, uint16_t height)
    {
        Bytes h(0x34, 0);
        le32(h, 0, 0x1234u);
        le16(h, 4, width);
        le16(h, 6, height);
        std::memcpy(h.data() + 0x14, name.data(), name.size());
        return chunk("TXRH", h);
    }

    // A PS2 8-bit BMP (rows bottom up) of `indices` (top down), the palette
    // B, G, R, A with the PS2's alpha (0x80 opaque).
    Bytes bmp8(int width, int height, const Bytes &indices, const std::vector<std::array<uint8_t, 4>> &palette)
    {
        Bytes d(14 + 40, 0);
        d[0] = 'B';
        d[1] = 'M';
        le32(d, 10, uint32_t(14 + 40 + 1024));
        le32(d, 14, 40);
        le32(d, 18, uint32_t(width));
        le32(d, 22, uint32_t(height));
        le16(d, 26, 1);
        le16(d, 28, 8);
        for (size_t i = 0; i < 256; ++i)
        {
            const std::array<uint8_t, 4> c = i < palette.size() ? palette[i] : std::array<uint8_t, 4>{0, 0, 0, 0x80};
            d.insert(d.end(), c.begin(), c.end());
        }
        for (int y = height - 1; y >= 0; --y)
            d.insert(d.end(), indices.begin() + y * width, indices.begin() + (y + 1) * width);
        le32(d, 2, uint32_t(d.size()));
        return d;
    }

    // One DXT3 level of `width` x `height`, opaque: the left half `left`,
    // the right half `right` (RGB565).
    Bytes dxt3Halves(int width, int height, uint16_t left, uint16_t right)
    {
        Bytes d;
        for (int by = 0; by < height / 4; ++by)
            for (int bx = 0; bx < width / 4; ++bx)
            {
                Bytes block(16, 0xFF);
                le16(block, 8, bx < width / 8 ? left : right);
                le16(block, 10, 0);
                le32(block, 12, 0); // every texel colour 0
                d = d + block;
            }
        return d;
    }

    // DM2's Xbox picture: TDDS, u32 0, then a DDS file of one DXT3 level.
    Bytes tdds(int width, int height, const Bytes &pixels)
    {
        Bytes d(8 + 124, 0);
        std::memcpy(d.data() + 4, "DDS ", 4);
        le32(d, 8, 124);
        le32(d, 12, 0x1007); // caps, height, width, pixel format
        le32(d, 16, uint32_t(height));
        le32(d, 20, uint32_t(width));
        le32(d, 8 + 72, 32);
        le32(d, 8 + 76, 4); // DDPF_FOURCC
        std::memcpy(d.data() + 8 + 80, "DXT3", 4);
        return chunk("TDDS", d + pixels);
    }

    // AI's and BMX XXX's Xbox picture: TXPR, u32 0, then an XPR0 bundle with
    // one swizzled D3D texture (DXT3, one level).
    Bytes txpr(int width, int height, const Bytes &pixels)
    {
        const uint32_t headerSize = 0x30;
        Bytes d(4 + headerSize, 0);
        std::memcpy(d.data() + 4, "XPR0", 4);
        le32(d, 8, uint32_t(headerSize + pixels.size()));
        le32(d, 12, headerSize);
        le32(d, 16, 0x00040001u); // a texture resource
        le32(d, 20, 0);           // its data at the header's end
        auto log2 = [](int v)
        {
            uint32_t n = 0;
            while ((1 << n) < v)
                ++n;
            return n;
        };
        le32(d, 28, (0x0Eu << 8) | (1u << 16) | (log2(width) << 20) | (log2(height) << 24));
        le32(d, 36, 0xFFFFFFFFu);
        return chunk("TXPR", d + pixels);
    }

    // BMX XXX's form: a TXRC packed in a ZLZO chunk (u32 its size, then
    // LZO1X: here one literal run and the end marker).
    Bytes zlzo(const Bytes &packed)
    {
        Bytes body(4, 0);
        le32(body, 0, uint32_t(packed.size()));
        size_t rest = packed.size() - 3 - 15; // a long literal run: 0, zeros of 255, the rest
        body.push_back(0);
        while (rest > 255)
        {
            body.push_back(0);
            rest -= 255;
        }
        body.push_back(uint8_t(rest));
        body = body + packed;
        body = body + Bytes{0x11, 0x00, 0x00};
        return chunk("ZLZO", body);
    }

    Bytes library(const Bytes &chunks) { return chunk("FORM", chunks); }
}

void register_extras_tests()
{
    MiniTest::Case("Extras", [](TestCase &tc)
    {
        tc.Run("an XISO's files", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "x.iso", xiso());
            ex::XboxDisc disc;
            std::string error;
            t.IsTrue(disc.open(dir.path / "x.iso", &error), "opens: " + error);
            t.Equals(disc.files().size(), size_t(2), "two files");
            t.Equals(disc.files()[0].path, std::string("default.xbe"), "the root's file first");
            t.Equals(disc.files()[1].path, std::string("media/a.txt"), "then the folder's");
            std::vector<uint8_t> out;
            t.IsTrue(disc.readFile(*disc.find("MEDIA/A.TXT"), out), "found in any case");
            t.Equals(text(out), std::string("abc"), "its bytes");
        });

        tc.Run("not an Xbox image", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "x.iso", std::vector<uint8_t>(0x20000, 0));
            ex::XboxDisc disc;
            std::string error;
            t.IsFalse(disc.open(dir.path / "x.iso", &error), "refused");
            t.IsTrue(error.find("not an Xbox disc image") != std::string::npos, "says so: " + error);
        });

        tc.Run("a GCM's files", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "g.iso", gcm());
            ex::GameCubeDisc disc;
            std::string error;
            t.IsTrue(disc.open(dir.path / "g.iso", &error), "opens: " + error);
            t.Equals(disc.gameId(), std::string("GTST01"), "game id");
            t.Equals(disc.title(), std::string("Test"), "title");
            t.Equals(disc.files().size(), size_t(2), "two files");
            t.Equals(disc.files()[0].path, std::string("sub/f.bin"), "the folder's file");
            t.Equals(disc.files()[1].path, std::string("top.bin"), "the root's");
            std::vector<uint8_t> out;
            t.IsTrue(disc.readFile(disc.files()[0], out) && text(out) == "four", "its bytes");
        });

        tc.Run("an RVZ: zstd groups, packed junk, from a FILE*", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "t.rvz", kTestRvz);
            std::string error;
            std::FILE *file = std::fopen((dir.path / "t.rvz").string().c_str(), "rb");
            std::unique_ptr<ex::ImageFile> image = ex::ImageFile::adopt(file, "t.rvz", &error);
            t.IsNotNull(image.get(), "adopted: " + error);
            std::unique_ptr<ex::DiscImage> disc = ex::openDiscImage(std::move(image), &error);
            t.IsNotNull(disc.get(), "opens: " + error);
            if (!disc)
                return;
            t.Equals(disc->size(), uint64_t(0x10000), "the disc's size");
            uint8_t head[6], plain[4], data[5], junk[8];
            t.IsTrue(disc->read(0, 6, head) && std::memcmp(head, "GTST01", 6) == 0, "the header from header 2");
            t.IsTrue(disc->read(0x80, 4, plain), "group 0");
            t.IsTrue(plain[0] == 0x80 && plain[1] == 0x87 && plain[2] == 0x8E && plain[3] == 0x95, "its bytes (i * 7)");
            t.IsTrue(disc->read(0x8000, 5, data) && std::memcmp(data, "HELLO", 5) == 0, "group 1's data");
            const uint8_t expected[8] = {0x34, 0x64, 0x56, 0xb7, 0xf9, 0x5f, 0x37, 0xb1}; // gc_disc.py's junk
            t.IsTrue(disc->read(0x8005, 8, junk) && std::memcmp(junk, expected, 8) == 0, "its junk regenerated");
            t.IsFalse(disc->read(0xFFFF, 2, junk), "nothing past the end");
        });

        tc.Run("a damaged RVZ", [](TestCase &t)
        {
            TempDir dir;
            std::vector<uint8_t> bad = kTestRvz;
            bad[0x20] ^= 1; // inside header 1, under its hash
            write(dir.path / "t.rvz", bad);
            std::string error;
            t.IsNull(ex::openDiscImage(dir.path / "t.rvz", &error).get(), "refused");
            t.IsTrue(error.find("damaged") != std::string::npos, "says so: " + error);
        });

        tc.Run("patterns as gc_disc.py's", [](TestCase &t)
        {
            t.IsTrue(ex::pathMatches("assets/levels/TUT/Estate/tut.GEO", "assets/levels/tut/estate"), "a folder");
            t.IsTrue(ex::pathMatches("assets/string.zal", "ASSETS/STRING.ZAL"), "any case");
            t.IsTrue(ex::pathMatches("assets/levels/CHL_DAM/objctv.txt", "assets/levels/*/objctv.txt"), "a glob");
            t.IsTrue(ex::globMatches("levels/a/b/x.bni", "levels/*/*.bni"), "* crosses /");
            t.IsTrue(ex::globMatches("loading/tut/tut3.raw", "loading/tut/tut?.raw"), "?");
            t.IsTrue(ex::globMatches("a5", "a[0-9]") && !ex::globMatches("a5", "a[!0-9]"), "sets");
            t.IsFalse(ex::globMatches("levels/a.bin", "levels/*/*.bin"), "no folder, no match");
        });

        tc.Run("the walk order of the tooling", [](TestCase &t)
        {
            t.IsTrue(ex::sourceOrderLess("b.txt", "a/z.txt"), "a folder's files before its folders");
            t.IsTrue(ex::sourceOrderLess("a/z.txt", "b/a.txt"), "folders by name");
            t.IsTrue(ex::sourceOrderLess("B.txt", "a.txt"), "names as bytes (upper case first)");
        });

        tc.Run("difflib's opcodes", [](TestCase &t)
        {
            using Tag = ex::DiffOpcode::Tag;
            const std::vector<std::string> a = {"q", "a", "b", "c", "x", "y", "z", "a", "b"};
            const std::vector<std::string> b = {"a", "b", "c", "q", "x", "y", "a", "b", "z"};
            // SequenceMatcher(None, a, b, autojunk=False).get_opcodes() (CPython 3.15)
            const std::vector<ex::DiffOpcode> want = {{Tag::Delete, 0, 1, 0, 0}, {Tag::Equal, 1, 4, 0, 3},
                                                      {Tag::Insert, 4, 4, 3, 4}, {Tag::Equal, 4, 6, 4, 6},
                                                      {Tag::Delete, 6, 7, 6, 6}, {Tag::Equal, 7, 9, 6, 8},
                                                      {Tag::Insert, 9, 9, 8, 9}};
            const std::vector<ex::DiffOpcode> got = ex::diffOpcodes(a, b);
            bool same = got.size() == want.size();
            for (size_t i = 0; same && i < got.size(); ++i)
                same = got[i].tag == want[i].tag && got[i].aBegin == want[i].aBegin && got[i].aEnd == want[i].aEnd &&
                       got[i].bBegin == want[i].bBegin && got[i].bEnd == want[i].bEnd;
            t.IsTrue(same, "the Python's opcodes");
        });

        tc.Run("an extras disc checked by its executable", [](TestCase &t)
        {
            TempDir dir;
            write(dir.path / "x.iso", xiso());
            write(dir.path / "n.bin", std::vector<uint8_t>(4096, 7));
            ex::ExtrasSource other = ex::checkExtrasSource((dir.path / "x.iso").string(), "ai", ex::pathOpener());
            t.IsNull(other.disc, "an Xbox disc the app has no extras for");
            t.IsTrue(other.message.find("not a release of Aggressive Inline") != std::string::npos, "says so: " + other.message);
            ex::ExtrasSource none = ex::checkExtrasSource((dir.path / "n.bin").string(), "", ex::pathOpener());
            t.IsNull(none.disc, "not a disc");
            t.IsTrue(none.message.find("not an Xbox or GameCube") != std::string::npos, "says so: " + none.message);
            t.Equals(ex::extrasDiscsOf("bmx").size(), size_t(2), "BMX XXX: the Xbox and GameCube discs");
            t.Equals(ex::extrasDiscsOf("dm2").size(), size_t(1), "DM2: the Xbox disc");
        });

        tc.Run("the search tells Xbox and GameCube discs from PS2 discs and other files", [](TestCase &t)
        {
            TempDir dir;
            const fs::path roms = dir.path / "ROMs";
            write(roms / "xbox" / "game.iso", xiso());
            write(roms / "gamecube" / "game.gcm", gcm());
            write(roms / "ps2" / "notes.bin", std::vector<uint8_t>(70000, 3));
            const zx::app::DiscSearch search = zx::app::findDiscs({roms}, {}, ex::pathOpener());
            std::vector<std::string> names;
            for (const zx::app::FoundDisc &disc : search.discs)
            {
                names.push_back(disc.name);
                t.IsTrue(disc.kind == zx::app::FoundDisc::Kind::Extras, disc.name + " is an extras disc");
                t.IsNull(disc.extras, disc.name + ": none the app makes extras from (made-up executables)");
            }
            std::sort(names.begin(), names.end());
            std::string all;
            for (const std::string &name : names)
                all += " " + name;
            t.IsTrue(names == std::vector<std::string>{"game.gcm", "game.iso"}, "the two game discs, not the other file:" + all);
            t.IsTrue(search.extrasOf("ai").empty(), "no game's extras");
            const zx::app::DiscSearch noExtras = zx::app::findDiscs({roms}, {}, {});
            t.IsTrue(noExtras.discs.empty(), "with no opener, no extras discs looked for");
        });

        tc.Run("the boot reads the made extras before the bundled ones", [](TestCase &t)
        {
            TempDir dir;
            fs::create_directories(dir.path / "extras" / "xbox");
            fs::create_directories(dir.path / "bundle" / "xbox");
            fs::create_directories(dir.path / "bundle" / "estate");
            zx::app::Settings settings;
            zx::app::AppPaths paths;
            paths.supportDir = dir.path;
            zx::app::BootFolders folders;
            folders.resources = dir.path / "bundle";
            folders.extras = dir.path / "extras";
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const zx::BootConfig boot =
                zx::app::gameBootConfig("ai", settings, paths, zx::app::gameSettingsProfile("ai"), folders, noEnv);
            const std::string *xbox = boot.find("ZX_XBOX_DIR"), *estate = boot.find("ZX_ESTATE_DIR");
            t.IsTrue(xbox && *xbox == (dir.path / "extras" / "xbox").string(), "the setup's Xbox files");
            t.IsTrue(estate && *estate == (dir.path / "bundle" / "estate").string(), "a bundled one it did not make");
            t.IsNull(boot.find("ZX_XBOX_PACK"), "none of either: not set");
        });

        tc.Run("no panel maps from a disc with no levels", [](TestCase &t)
        {
            TempDir dir;
            fs::create_directories(dir.path / "disc" / "ASSETS" / "LEVELS");
            ex::FolderFiles disc;
            t.IsTrue(disc.open(dir.path / "disc", {"ASSETS"}), "the disc folder");
            std::vector<std::string> log;
            std::string error;
            t.IsFalse(ex::makePanelMaps(ex::PanelGame::AggressiveInline, disc, {}, dir.path / "panel", log, &error),
                      "refused");
            t.IsFalse(error.empty(), "says why: " + error);
        });

        tc.Run("a custom map's files over the disc, and its panel file refused without its level", [](TestCase &t)
        {
            TempDir dir;
            fs::create_directories(dir.path / "disc" / "ASSETS" / "LEVELS" / "MOVIE");
            std::ofstream(dir.path / "disc" / "ASSETS" / "LEVELS" / "MOVIE" / "MOVIE.TXT") << "disc";
            std::ofstream(dir.path / "disc" / "ASSETS" / "LEVELS" / "MOVIE" / "MOVIE.GEO") << "disc";
            ex::FolderFiles disc;
            t.IsTrue(disc.open(dir.path / "disc", {"ASSETS"}), "the disc folder");
            std::map<std::string, ex::Bytes> map;
            map["ASSETS/LEVELS/MOVIE/movie.geo"] = {'m', 'a', 'p'};
            map["ASSETS/LEVELS/MOVIE/MOVIE.INS"] = {'i'};
            const ex::LayeredFiles files(disc, std::move(map));
            ex::Bytes geo, txt, ins;
            t.IsTrue(files.readAny("assets/levels/movie/MOVIE.GEO", geo) && geo == ex::Bytes{'m', 'a', 'p'},
                     "the map's file wins over the disc's of that path (any case)");
            t.IsTrue(files.readAny("ASSETS/LEVELS/MOVIE/MOVIE.TXT", txt) && txt == ex::Bytes{'d', 'i', 's', 'c'} &&
                         files.readAny("ASSETS/LEVELS/MOVIE/MOVIE.INS", ins) && ins.size() == 1,
                     "the disc's other files, and the map's own");
            t.Equals(files.files().size(), size_t(3), "each path once");
            ex::Bytes out;
            std::vector<std::string> log;
            std::string error;
            const bool made = ex::makeMapPanelFile(ex::PanelGame::DaveMirra2, files, "WOODWARD", "Test", out, log, &error);
            t.IsTrue(!made && !error.empty(), "a level it does not have: refused, saying why: " + error);
        });

        tc.Run("LZO1X: literals, a match that overlaps itself, the end marker", [](TestCase &t)
        {
            // 3 literals (17 + 3), an M2 match of 6 bytes 3 back (0xA8 0x00),
            // the end (an M4 match at distance 0).
            const Bytes stream = {20, 'a', 'b', 'c', 0xA8, 0x00, 0x11, 0x00, 0x00, 0x00};
            Bytes out;
            t.IsTrue(ex::lzo1xDecompress(stream.data(), stream.size(), 64, out), "unpacked (a padding byte after the end left)");
            t.Equals(std::string(out.begin(), out.end()), std::string("abcabcabc"), "the match repeats the literals");
            t.IsFalse(ex::lzo1xDecompress(stream.data(), stream.size(), 8, out), "more than the size it may have: refused");
            t.IsFalse(ex::lzo1xDecompress(stream.data(), 7, 64, out), "cut short: refused");
            const Bytes before = {20, 'a', 'b', 'c', 0xA8, 0x01, 0x11, 0x00, 0x00};
            t.IsFalse(ex::lzo1xDecompress(before.data(), before.size(), 64, out), "a match before the start: refused");
        });

        tc.Run("DM2's and BMX XXX's Xbox pictures (TDDS, TXPR in ZLZO, a ZAL's) replace the PS2 ones", [](TestCase &t)
        {
            // The PS2 texture: 8 x 8, red on the left, blue on the right.
            Bytes indices(64);
            for (size_t i = 0; i < indices.size(); ++i)
                indices[i] = (i % 8) < 4 ? 1 : 2;
            const std::vector<std::array<uint8_t, 4>> palette = {{0, 0, 0, 0x80}, {0, 0, 255, 0x80}, {255, 0, 0, 0x80}};
            const Bytes ps2 = chunk("TXRC", txrh("wall", 8, 8) + chunk("TBMP", bmp8(8, 8, indices, palette)));
            const Bytes pixels = dxt3Halves(16, 16, 0xF800, 0x001F); // its Xbox picture at twice the size
            struct Form
            {
                const char *name;
                Bytes xbox;   // the Xbox library
                bool archive; // in a ZAL (BMX XXX's level props)
            };
            const Form forms[] = {
                {"DM2's TDDS", library(chunk("TXRC", txrh("wall", 8, 8) + tdds(16, 16, pixels))), false},
                {"BMX XXX's TXPR in a ZLZO", library(zlzo(chunk("TXRC", txrh("wall", 8, 8) + txpr(16, 16, pixels)))), false},
                {"BMX XXX's in a ZAL", library(zlzo(chunk("TXRC", txrh("wall", 8, 8) + txpr(16, 16, pixels)))), true},
            };
            for (const Form &form : forms)
            {
                TempDir dir;
                const char *file = form.archive ? "MODELS.ZAL" : "T.TXR";
                auto zal = [](const Bytes &entry)
                {
                    // ZDIR, DIDF (the count at 0x10), GRID, DLEN, OFST, then the entry.
                    Bytes d(0x18 + 3 * 12, 0);
                    std::memcpy(d.data(), "ZDIR", 4);
                    std::memcpy(d.data() + 8, "DIDF", 4);
                    le32(d, 0x10, 1);
                    std::memcpy(d.data() + 0x18, "GRID", 4);
                    std::memcpy(d.data() + 0x24, "DLEN", 4);
                    le32(d, 0x2C, uint32_t(entry.size()) | 0x60000000u);
                    std::memcpy(d.data() + 0x30, "OFST", 4);
                    le32(d, 0x38, uint32_t(d.size()));
                    return d + entry;
                };
                const Bytes ps2File = form.archive ? zal(chunk("FMDL", ps2)) : library(ps2);
                const Bytes xboxFile = form.archive ? zal(chunk("FMDL", form.xbox)) : form.xbox;
                write(dir.path / "ps2" / "ASSETS" / "LEVELS" / "T" / file, ps2File);
                write(dir.path / "xbox" / "media" / "assets" / "levels" / "t" / (std::string(file) == "T.TXR" ? "t.txr" : "models.zal"),
                      xboxFile);
                ex::FolderFiles ps2Disc, xboxDisc;
                t.IsTrue(ps2Disc.open(dir.path / "ps2", {"levels"}) && xboxDisc.open(dir.path / "xbox", {"levels"}),
                         std::string(form.name) + ": the folders");
                ex::XboxTexturePackOptions options;
                options.movies = false;
                options.archives = form.archive;
                std::vector<std::string> log;
                std::string error;
                t.IsTrue(ex::makeXboxTexturePack(ps2Disc, xboxDisc, dir.path / "pack", options, log, &error),
                         std::string(form.name) + ": made " + error);
                std::ifstream in(dir.path / "pack" / "textures" / "ASSETS" / "LEVELS" / "T" / (std::string(file) + ".xtp"),
                                 std::ios::binary);
                const Bytes xtp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::vector<zx::texture_replace::Entry> entries;
                t.IsTrue(zx::texture_replace::parsePack(xtp, entries, &error), std::string(form.name) + ": a pack " + error);
                t.Equals(entries.size(), size_t(1), std::string(form.name) + ": one picture");
                if (entries.size() != 1)
                    continue;
                t.Equals(entries[0].width, 8u, std::string(form.name) + ": keyed by the PS2 size");
                t.Equals(entries[0].indexHash, zx::texture_replace::indexHash(indices.data(), indices.size()),
                         std::string(form.name) + ": and its indices");
                t.Equals(entries[0].picture->width, 16u, std::string(form.name) + ": the Xbox picture's size");
                t.IsTrue(entries[0].picture->data == pixels, std::string(form.name) + ": its DXT3 as it is");
            }
        });

        tc.Run("BMX XXX's level props with other Xbox names are paired by picture, unlike ones left out", [](TestCase &t)
        {
            // The PS2 prop: 8 x 8, red on the left, blue on the right, its
            // name with the size ("wall128"); on the Xbox it is "wall" at
            // twice the size, next to "floor" (the halves the other way).
            Bytes indices(64);
            for (size_t i = 0; i < indices.size(); ++i)
                indices[i] = (i % 8) < 4 ? 1 : 2;
            const std::vector<std::array<uint8_t, 4>> palette = {{0, 0, 0, 0x80}, {0, 0, 255, 0x80}, {255, 0, 0, 0x80}};
            const Bytes ps2 = chunk("TXRC", txrh("wall128", 8, 8) + chunk("TBMP", bmp8(8, 8, indices, palette)));
            const Bytes wall = dxt3Halves(16, 16, 0xF800, 0x001F), floor = dxt3Halves(16, 16, 0x001F, 0xF800);
            const Bytes xboxWall = chunk("TXRC", txrh("wall", 16, 16) + txpr(16, 16, wall));
            const Bytes xboxFloor = chunk("TXRC", txrh("floor", 16, 16) + txpr(16, 16, floor));
            // A ZAL of one entry: ZDIR, DIDF (the count at 0x10), GRID, DLEN, OFST, then the entry.
            auto zal = [](const Bytes &entry)
            {
                Bytes d(0x18 + 3 * 12, 0);
                std::memcpy(d.data(), "ZDIR", 4);
                std::memcpy(d.data() + 8, "DIDF", 4);
                le32(d, 0x10, 1);
                std::memcpy(d.data() + 0x18, "GRID", 4);
                std::memcpy(d.data() + 0x24, "DLEN", 4);
                le32(d, 0x2C, uint32_t(entry.size()) | 0x60000000u);
                std::memcpy(d.data() + 0x30, "OFST", 4);
                le32(d, 0x38, uint32_t(d.size()));
                return d + entry;
            };
            struct Case
            {
                const char *name;
                Bytes xbox;  // the Xbox entry's textures (each in a ZLZO)
                bool paired; // the PS2 prop gets the Xbox "wall"
            };
            const Case cases[] = {
                {"the alike one among two", zlzo(xboxFloor) + zlzo(xboxWall), true},
                {"only an unlike one", zlzo(xboxFloor), false},
            };
            for (const Case &c : cases)
            {
                TempDir dir;
                write(dir.path / "ps2" / "ASSETS" / "LEVELS" / "T" / "MODELS.ZAL", zal(chunk("FMDL", library(ps2))));
                write(dir.path / "xbox" / "media" / "assets" / "levels" / "t" / "models.zal",
                      zal(chunk("FMDL", library(c.xbox))));
                ex::FolderFiles ps2Disc, xboxDisc;
                t.IsTrue(ps2Disc.open(dir.path / "ps2", {"levels"}) && xboxDisc.open(dir.path / "xbox", {"levels"}),
                         std::string(c.name) + ": the folders");
                ex::XboxTexturePackOptions options;
                options.movies = false;
                options.archives = true;
                std::vector<std::string> log;
                std::string error;
                t.IsTrue(ex::makeXboxTexturePack(ps2Disc, xboxDisc, dir.path / "pack", options, log, &error),
                         std::string(c.name) + ": made " + error);
                std::ifstream reportFile(dir.path / "pack" / "textures" / "report.txt");
                const std::string report((std::istreambuf_iterator<char>(reportFile)), std::istreambuf_iterator<char>());
                const fs::path xtpPath = dir.path / "pack" / "textures" / "ASSETS" / "LEVELS" / "T" / "MODELS.ZAL.xtp";
                if (!c.paired)
                {
                    t.IsFalse(fs::exists(xtpPath), std::string(c.name) + ": no pack for the file");
                    t.IsTrue(report.find("wall128 8x8: no Xbox picture alike, nearest floor 16x16") != std::string::npos,
                             std::string(c.name) + ": the report says so, with the nearest: " + report);
                    continue;
                }
                std::ifstream in(xtpPath, std::ios::binary);
                const Bytes xtp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::vector<zx::texture_replace::Entry> entries;
                t.IsTrue(zx::texture_replace::parsePack(xtp, entries, &error), std::string(c.name) + ": a pack " + error);
                t.Equals(entries.size(), size_t(1), std::string(c.name) + ": one picture");
                if (entries.size() != 1)
                    continue;
                t.Equals(entries[0].indexHash, zx::texture_replace::indexHash(indices.data(), indices.size()),
                         std::string(c.name) + ": keyed by the PS2 prop");
                t.IsTrue(entries[0].picture->data == wall, std::string(c.name) + ": the Xbox wall's picture");
                t.IsTrue(report.find("by picture: wall128 8x8 -> wall 16x16") != std::string::npos,
                         std::string(c.name) + ": in the report: " + report);
            }
        });

        // A game folder as 1.1.0 left it: its PS2 disc's files, its record of the extras discs ("# z-axis extras 1")
        // and its parts, made with no keys.
        const auto setUpBefore = [](const fs::path &dir, const std::string &record, std::initializer_list<const char *> parts)
        {
            write(dir / "disc" / "SYSTEM.CNF", {'B', 'O', 'O', 'T'});
            write(dir / "disc" / "ASSETS" / "LEVELS" / "A" / "A.TXT", {'a'});
            std::ofstream(dir / "disc.manifest", std::ios::binary) << "z-axis game files 1\ngame x\nelf SLUS_000.00\nsha256 ab12\n";
            std::ofstream(dir / "extras.manifest", std::ios::binary) << record;
            for (const char *part : parts)
                write(dir / "extras" / part / "made.txt", {'1', '.', '1'});
        };
        const auto read = [](const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };

        tc.Run("an update finds the parts whose maker changed since 1.1.0", [&](TestCase &t)
        {
            // (BMX XXX's content, Xbox textures and maps are made by makers at version 1)
            TempDir bmx;
            setUpBefore(bmx.path, "# z-axis extras 1\nxbox\t/discs/bmx-xbox.iso\ngamecube\t/discs/bmx-gc.rvz\n",
                        {"releases", "xbox-pack", "panel"});
            const ex::StaleExtras stale = ex::staleExtras("bmx", bmx.path);
            t.IsTrue(stale.parts == std::vector<std::string>({"releases", "xbox-pack", "panel"}), "the content, the textures and the maps");
            t.Equals(stale.discs.size(), size_t(2), "made from both discs");
            // (DM2's maps' maker is at 1: its maps alone, from no disc)
            TempDir dm2;
            setUpBefore(dm2.path, "# z-axis extras 1\nxbox\t/discs/dm2-xbox.iso\n", {"xbox-levels", "xbox-pack", "panel"});
            const ex::StaleExtras dm2Stale = ex::staleExtras("dm2", dm2.path);
            t.IsTrue(dm2Stale.parts == std::vector<std::string>({"panel"}), "DM2's maps alone");
            t.IsTrue(dm2Stale.discs.empty(), "no disc needed");
            // (AI's parts are all as 1.1.0 made them)
            TempDir ai;
            setUpBefore(ai.path, "# z-axis extras 1\nxbox\t/discs/ai-xbox.iso\n", {"xbox", "xbox-converted", "xbox-pack", "panel"});
            const ex::StaleExtras aiStale = ex::staleExtras("ai", ai.path);
            t.IsTrue(aiStale.parts.empty() && aiStale.unkeyed, "nothing for AI to make, but its keys to write");
            std::vector<ex::ExtrasSource> noSources;
            t.IsTrue(ex::makeExtras("ai", ai.path, noSources, {}, {}, {{"xbox", "/discs/ai-xbox.iso"}}).ok, "AI's keys written");
            t.IsTrue(ex::staleExtras("ai", ai.path, false).empty(), "then nothing for AI (made, as here, with no skaters)");
            // (a game not set up has nothing to update)
            TempDir none;
            t.IsTrue(ex::staleExtras("ai", none.path).empty(), "nothing without the PS2 disc's files");
        });

        tc.Run("a part whose disc does not open is kept, with its disc recorded", [&](TestCase &t)
        {
            TempDir bmx;
            setUpBefore(bmx.path, "# z-axis extras 1\nxbox\t/discs/bmx-xbox.iso\n", {"releases", "xbox-pack", "panel"});
            // (the maps as this version makes them, keyed: no disc but the PS2 one, which the test's is not)
            const std::string panelKey = "# z-axis extras part 1\npart panel\nmaker panel_maps_bmx 1\ninput ps2 ab12\n";
            std::ofstream(bmx.path / "extras" / "panel.key", std::ios::binary) << panelKey;
            std::vector<ex::ExtrasSource> none;
            const std::vector<ex::RecordedDisc> gone = {{"xbox", "/discs/bmx-xbox.iso"}};
            const ex::ExtrasResult r = ex::makeExtras("bmx", bmx.path, none, {}, {}, gone);
            t.IsTrue(r.ok, "nothing failed: " + r.message);
            t.IsTrue(r.made.empty(), "nothing made");
            t.IsTrue(r.kept == std::vector<std::string>({"releases", "xbox-pack"}), "the two parts kept for want of the disc");
            t.Equals(r.missing.size(), size_t(1), "the disc asked for");
            t.Equals(read(bmx.path / "extras" / "releases" / "made.txt"), std::string("1.1"), "the content as it was");
            t.Equals(read(bmx.path / "extras" / "xbox-pack" / "made.txt"), std::string("1.1"), "the textures as they were");
            t.IsFalse(fs::exists(bmx.path / "extras" / "releases.key"), "no key: still made by the version before");
            t.Equals(read(bmx.path / "extras" / "panel.key"), panelKey, "the maps, made as now, kept with their key");
            const std::vector<ex::RecordedDisc> recorded = ex::recordedExtras(bmx.path);
            t.IsTrue(recorded.size() == 1 && recorded[0].source == "/discs/bmx-xbox.iso", "the disc still recorded");
            t.IsTrue(ex::staleExtras("bmx", bmx.path).parts == std::vector<std::string>({"releases", "xbox-pack"}),
                     "and still to update, once the disc is back");
            // (the record the version after: no part is taken as 1.1.0's any more)
            t.IsTrue(read(bmx.path / "extras.manifest").rfind("# z-axis extras 2\n", 0) == 0, "the record's new head");
        });

        tc.Run("a part's key names its makers, its discs and the parts it reads", [&](TestCase &t)
        {
            TempDir ai;
            setUpBefore(ai.path, "# z-axis extras 1\nxbox\t/discs/ai-xbox.iso\n", {"xbox", "xbox-converted", "xbox-pack", "panel"});
            std::vector<ex::ExtrasSource> none;
            const ex::ExtrasResult r = ex::makeExtras("ai", ai.path, none, {}, {}, {{"xbox", "/discs/ai-xbox.iso"}});
            t.IsTrue(r.ok && r.made.empty(), "1.1.0's parts taken as they are: " + r.message);
            const std::string panel = read(ai.path / "extras" / "panel.key");
            t.IsTrue(panel.find("maker panel_maps_ai 0\n") != std::string::npos, "its maker: " + panel);
            t.IsTrue(panel.find("input ps2 ab12\n") != std::string::npos, "the PS2 disc's ELF");
            t.IsTrue(panel.find("after xbox-converted ") != std::string::npos && panel.find("after xbox ") != std::string::npos,
                     "the Zoo and the Xbox scripts it reads");
            const std::string pack = read(ai.path / "extras" / "xbox-pack.key");
            t.IsTrue(pack.find("input xbox f8cd57e2") != std::string::npos, "the Xbox disc, by its default.xbe: " + pack);
            // (a stray part no disc makes is taken away: Estate with no GameCube disc recorded)
            write(ai.path / "extras" / "estate" / "made.txt", {'x'});
            write(ai.path / "extras" / "xbox-pack.partial" / "half.txt", {'x'});
            ex::makeExtras("ai", ai.path, none, {}, {}, {{"xbox", "/discs/ai-xbox.iso"}});
            t.IsFalse(fs::exists(ai.path / "extras" / "estate"), "Estate taken away");
            t.IsFalse(fs::exists(ai.path / "extras" / "xbox-pack.partial"), "a stopped make's folder taken away");
            t.IsTrue(fs::exists(ai.path / "extras" / "xbox-pack" / "made.txt"), "the textures kept");
            // (a part with no key in this version's record is not 1.1.0's, the record edited on Windows too)
            fs::remove(ai.path / "extras" / "panel.key");
            const std::vector<std::string> unixLines = ex::staleExtras("ai", ai.path, false).parts;
            std::string record = read(ai.path / "extras.manifest"), crlf;
            for (char c : record)
                crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
            std::ofstream(ai.path / "extras.manifest", std::ios::binary | std::ios::trunc) << crlf;
            const std::vector<std::string> windowsLines = ex::staleExtras("ai", ai.path, false).parts;
            t.IsTrue(std::find(unixLines.begin(), unixLines.end(), "panel") != unixLines.end() && windowsLines == unixLines,
                     "the maps with no key to make again, with Windows' line ends as with none");
        });

        tc.Run("no extras without the PS2 disc", [](TestCase &t)
        {
            TempDir dir;
            std::vector<ex::ExtrasSource> sources;
            const ex::ExtrasResult r = ex::makeExtras("ai", dir.path, sources, {});
            t.IsFalse(r.ok, "refused");
            t.IsTrue(r.message.find("PS2 disc") != std::string::npos, "says why: " + r.message);
        });
    });
}
