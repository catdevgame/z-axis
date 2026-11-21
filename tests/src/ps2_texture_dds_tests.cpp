#include "MiniTest.h"
#include "runtime/ps2_texture_dds.h"
#include "runtime/ps2_texture_pack.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace dds = zx::texture_dds;
    namespace tr = zx::texture_replace;
    namespace tp = zx::texture_pack;

    constexpr uint32_t kFlagsTexture = 0x1007u;  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    constexpr uint32_t kFlagPitch = 0x8u;
    constexpr uint32_t kFlagMipmapCount = 0x20000u;
    constexpr uint32_t kFlagLinearSize = 0x80000u;
    constexpr uint32_t kFlagDepth = 0x800000u;

    constexpr uint32_t fourCC(const char (&s)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
    }

    void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

    // A DDS pixel format: flags, FourCC, bit count, R G B A masks.
    struct PixelFormat
    {
        uint32_t flags = 0, fourCC = 0, bits = 0, r = 0, g = 0, b = 0, a = 0;
    };
    PixelFormat blocks(const char (&code)[5]) { return {0x4u, fourCC(code)}; }
    const PixelFormat kA8R8G8B8{0x41u, 0, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u};
    const PixelFormat kX8R8G8B8{0x40u, 0, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0};
    const PixelFormat kA8B8G8R8{0x41u, 0, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u};
    const PixelFormat kX8B8G8R8{0x40u, 0, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0};
    const PixelFormat kR8G8B8{0x40u, 0, 24, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0};

    // A DDS file: its header, a DX10 header if dxgi is set (FourCC "DX10"),
    // then payload as it is.
    std::vector<uint8_t> ddsFile(uint32_t width, uint32_t height, uint32_t flags, uint32_t mipCount, PixelFormat pf,
                                 const std::vector<uint8_t> &payload, uint32_t dxgi = 0, uint32_t pitch = 0,
                                 uint32_t arraySize = 1)
    {
        std::vector<uint8_t> b(4 + 124 + (dxgi ? 20 : 0), 0);
        put32(b, 0, 0x20534444u);
        put32(b, 4, 124);
        put32(b, 8, flags);
        put32(b, 12, height);
        put32(b, 16, width);
        put32(b, 20, pitch);
        put32(b, 28, mipCount);
        put32(b, 76, 32);
        put32(b, 80, dxgi ? 0x4u : pf.flags);
        put32(b, 84, dxgi ? fourCC("DX10") : pf.fourCC);
        put32(b, 88, pf.bits);
        put32(b, 92, pf.r);
        put32(b, 96, pf.g);
        put32(b, 100, pf.b);
        put32(b, 104, pf.a);
        put32(b, 108, 0x1000u); // DDSCAPS_TEXTURE
        if (dxgi)
        {
            put32(b, 128, dxgi);
            put32(b, 132, 3); // TEXTURE2D
            put32(b, 140, arraySize);
        }
        b.insert(b.end(), payload.begin(), payload.end());
        return b;
    }

    // BC1 colour endpoints and indices: c0 > c1 picks the four-colour mode.
    std::vector<uint8_t> bc1Block(uint16_t c0, uint16_t c1, uint32_t indices)
    {
        std::vector<uint8_t> b(8);
        std::memcpy(b.data(), &c0, 2);
        std::memcpy(b.data() + 2, &c1, 2);
        std::memcpy(b.data() + 4, &indices, 4);
        return b;
    }

    std::vector<uint8_t> repeat(const std::vector<uint8_t> &block, size_t count)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i < count; ++i)
            out.insert(out.end(), block.begin(), block.end());
        return out;
    }

    // A BC7 mode 6 block (one subset, RGBA endpoints of 7 bits and a
    // p-bit each, 4-bit indices) with every index 0: each texel is endpoint
    // 0, (R0 << 1 | P0, G0 << 1 | P0, B0 << 1 | P0, A0 << 1 | P0).
    std::vector<uint8_t> bc7Mode6(uint32_t r0, uint32_t g0, uint32_t b0, uint32_t a0, uint32_t p0)
    {
        std::vector<uint8_t> b(16, 0);
        uint32_t at = 0;
        const auto bits = [&](uint32_t value, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i, ++at)
                if ((value >> i) & 1u)
                    b[at / 8u] |= static_cast<uint8_t>(1u << (at % 8u));
        };
        bits(1u << 6, 7);           // mode 6
        bits(r0, 7), bits(0x7F, 7); // R0, R1
        bits(g0, 7), bits(0x7F, 7); // G0, G1
        bits(b0, 7), bits(0x7F, 7); // B0, B1
        bits(a0, 7), bits(0x7F, 7); // A0, A1
        bits(p0, 1), bits(1, 1);    // P0, P1 (indices: all 0)
        return b;
    }

    uint32_t texel(const std::vector<uint8_t> &rgba, size_t i)
    {
        uint32_t v;
        std::memcpy(&v, rgba.data() + i * 4u, 4);
        return v;
    }

    struct TempDir
    {
        std::filesystem::path path;
        explicit TempDir(const char *what)
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / (std::string("zx-dds-") + what + "-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void writeFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

void register_ps2_texture_dds_tests()
{
    MiniTest::Case("TextureDds", [](TestCase &tc)
    {
        tc.Run("BC1: DXT1 and DX10 71, blocks kept, decoded with 1-bit alpha", [](TestCase &t)
        {
            // Red (0xF800) and blue (0x001F): index 0 red, 1 blue, 2 two
            // thirds red; then c0 <= c1: index 3 transparent black.
            const std::vector<uint8_t> four = bc1Block(0xF800, 0x001F, 0x00000024u); // texels 0..2: 0, 1, 2
            const std::vector<uint8_t> three = bc1Block(0x001F, 0xF800, 0x000000C0u); // texel 3: index 3
            std::vector<uint8_t> payload = four;
            payload.insert(payload.end(), three.begin(), three.end());
            tr::Picture p;
            std::string error;
            t.IsTrue(dds::readDds(ddsFile(8, 4, kFlagsTexture | kFlagLinearSize, 0, blocks("DXT1"), payload), p, &error),
                     "read: " + error);
            t.IsTrue(p.format == tr::Format::BC1 && p.width == 8u && p.height == 4u && p.mips == 1u, "BC1 8 x 4, one level");
            t.IsTrue(p.data == payload, "the blocks as they are");
            const std::vector<uint8_t> rgba = dds::rgbaLevel(p, 0);
            t.Equals(texel(rgba, 0), 0xFF0000FFu, "index 0: red, opaque");
            t.Equals(texel(rgba, 1), 0xFFFF0000u, "index 1: blue");
            t.Equals(texel(rgba, 2) & 0xFFu, 0xAAu, "index 2: two thirds of red");
            t.Equals(texel(rgba, 4 + 3), 0u, "c0 <= c1, index 3: transparent black");
            tr::Picture q;
            t.IsTrue(dds::readDds(ddsFile(8, 4, kFlagsTexture, 0, {}, payload, 71), q) && q.format == tr::Format::BC1 &&
                         q.data == payload,
                     "DX10 BC1_UNORM alike");
            t.IsFalse(dds::readDds(ddsFile(8, 4, kFlagsTexture, 0, {}, payload, 72), q), "BC1_UNORM_SRGB: not read (PCSX2's)");
        });

        tc.Run("BC2: DXT2, DXT3 and DX10 74, explicit alpha", [](TestCase &t)
        {
            std::vector<uint8_t> block(8, 0);
            block[0] = 0x8F; // texel 0 alpha 15, texel 1 alpha 8
            const std::vector<uint8_t> colour = bc1Block(0xF800, 0x001F, 0);
            block.insert(block.end(), colour.begin(), colour.end());
            for (const char *code : {"DXT2", "DXT3"})
            {
                tr::Picture p;
                const char name[5] = {code[0], code[1], code[2], code[3], 0};
                t.IsTrue(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, blocks(name), block), p) && p.format == tr::Format::BC2,
                         std::string(code) + ": BC2");
                const std::vector<uint8_t> rgba = dds::rgbaLevel(p, 0);
                t.Equals(texel(rgba, 0), 0xFF0000FFu, std::string(code) + ": alpha 15 is 255");
                t.Equals(texel(rgba, 1) >> 24, 0x88u, std::string(code) + ": alpha 8 is 0x88");
            }
            tr::Picture q;
            t.IsTrue(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, {}, block, 74), q) && q.format == tr::Format::BC2, "DX10 74");
        });

        tc.Run("BC3: DXT4, DXT5 and DX10 77, interpolated alpha", [](TestCase &t)
        {
            std::vector<uint8_t> block(8, 0);
            block[0] = 0x80; // alpha 0 = 0x80 (the PS2's opaque), every index 0
            block[1] = 0x00;
            const std::vector<uint8_t> colour = bc1Block(0x07E0, 0x0000, 0); // green
            block.insert(block.end(), colour.begin(), colour.end());
            for (const char *code : {"DXT4", "DXT5"})
            {
                tr::Picture p;
                const char name[5] = {code[0], code[1], code[2], code[3], 0};
                t.IsTrue(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, blocks(name), block), p) && p.format == tr::Format::BC3,
                         std::string(code) + ": BC3");
                t.Equals(texel(dds::rgbaLevel(p, 0), 15), 0x8000FF00u, std::string(code) + ": green, alpha 0x80");
            }
            tr::Picture q;
            t.IsTrue(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, {}, block, 77), q) && q.format == tr::Format::BC3, "DX10 77");
        });

        tc.Run("BC7: DX10 98 only, decoded as Direct3D specifies", [](TestCase &t)
        {
            const std::vector<uint8_t> block = bc7Mode6(127, 64, 0, 64, 0);
            tr::Picture p;
            t.IsTrue(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, {}, block, 98), p) && p.format == tr::Format::BC7 &&
                         p.data == block,
                     "BC7 kept");
            const std::vector<uint8_t> rgba = dds::rgbaLevel(p, 0);
            t.Equals(texel(rgba, 0), 0x800080FEu, "endpoint 0: (254, 128, 0, 128)");
            t.Equals(texel(rgba, 15), 0x800080FEu, "every texel");
            t.IsFalse(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, {}, block, 99), p), "BC7_UNORM_SRGB: not read");
            t.IsFalse(dds::readDds(ddsFile(4, 4, kFlagsTexture, 0, {}, block, 98, 0, 2), p), "an array: not read");
        });

        tc.Run("uncompressed: the five layouts PCSX2 reads, as R G B A", [](TestCase &t)
        {
            // One texel: bytes 0x11 0x22 0x33 0x44 (0x11 0x22 0x33 for 24 bits).
            const std::vector<uint8_t> four{0x11, 0x22, 0x33, 0x44}, three{0x11, 0x22, 0x33};
            struct Case
            {
                const char *name;
                PixelFormat pf;
                const std::vector<uint8_t> *texel;
                uint32_t rgba;
            };
            const Case cases[] = {
                {"A8R8G8B8 (B G R A)", kA8R8G8B8, &four, 0x44112233u},
                {"X8R8G8B8 (B G R X): alpha 0xFF", kX8R8G8B8, &four, 0xFF112233u},
                {"A8B8G8R8 (R G B A)", kA8B8G8R8, &four, 0x44332211u},
                {"X8B8G8R8 (R G B X): alpha 0x80", kX8B8G8R8, &four, 0x80332211u},
                {"R8G8B8 (B G R): alpha 0xFF", kR8G8B8, &three, 0xFF112233u},
            };
            for (const Case &c : cases)
            {
                tr::Picture p;
                t.IsTrue(dds::readDds(ddsFile(1, 1, kFlagsTexture, 0, c.pf, *c.texel), p) && p.format == tr::Format::RGBA8 &&
                             p.data.size() == 4u,
                         std::string(c.name) + ": read");
                t.Equals(texel(p.data, 0), c.rgba, c.name);
            }
            PixelFormat other = kA8R8G8B8;
            other.a = 0;
            tr::Picture p;
            t.IsFalse(dds::readDds(ddsFile(1, 1, kFlagsTexture, 0, other, four), p), "masks not exactly one of them: not read");
        });

        tc.Run("levels: the file's, until one is cut short", [](TestCase &t)
        {
            // BC1 8 x 8: levels of 4, 1, 1, 1 blocks.
            const std::vector<uint8_t> full = repeat(bc1Block(0xF800, 0x001F, 0), 7);
            tr::Picture p;
            t.IsTrue(dds::readDds(ddsFile(8, 8, kFlagsTexture | kFlagMipmapCount, 4, blocks("DXT1"), full), p) && p.mips == 4u &&
                         p.valid(),
                     "four levels to 1 x 1");
            t.Equals(dds::rgbaLevel(p, 3).size(), size_t{4}, "1 x 1 from its block's top left");
            t.Equals(texel(dds::rgbaLevel(p, 3), 0), 0xFF0000FFu, "red");
            t.IsTrue(dds::readDds(ddsFile(8, 8, kFlagsTexture | kFlagMipmapCount, 0, blocks("DXT1"), full), p) && p.mips == 4u,
                     "count 0: the whole chain (PCSX2's)");
            t.IsTrue(dds::readDds(ddsFile(8, 8, kFlagsTexture | kFlagMipmapCount, 4, blocks("DXT1"),
                                          std::vector<uint8_t>(full.begin(), full.begin() + 5 * 8)),
                                  p) &&
                         p.mips == 2u,
                     "cut short in level 2: levels 0 and 1");
            t.IsTrue(dds::readDds(ddsFile(8, 8, kFlagsTexture, 4, blocks("DXT1"), full), p) && p.mips == 1u,
                     "no MIPMAPCOUNT flag: level 0 alone");
            t.IsFalse(dds::readDds(ddsFile(8, 8, kFlagsTexture, 1, blocks("DXT1"), repeat(bc1Block(1, 0, 0), 3)), p),
                      "cut short in level 0: not read");
            t.IsFalse(dds::readDds(ddsFile(6, 8, kFlagsTexture, 1, blocks("DXT1"), full), p),
                      "a block format 6 wide: not read");
            t.IsFalse(dds::readDds(ddsFile(8, 8, kFlagsTexture | kFlagDepth, 1, blocks("DXT1"), full), p), "a volume: not read");
            t.IsFalse(dds::readDds(ddsFile(8192, 8, kFlagsTexture, 1, blocks("DXT1"), full), p), "over 4096: not read");
            // Uncompressed rows with a pitch (PITCH and LINEARSIZE both: PCSX2's
            // reading): 2 x 2 A8B8G8R8, 12 bytes a row.
            std::vector<uint8_t> rows(24, 0);
            rows[0] = 1, rows[4] = 2, rows[12] = 3, rows[16] = 4;
            t.IsTrue(dds::readDds(ddsFile(2, 2, kFlagsTexture | kFlagPitch | kFlagLinearSize, 1, kA8B8G8R8, rows, 0, 12), p) &&
                         p.data.size() == 16u && p.data[0] == 1 && p.data[4] == 2 && p.data[8] == 3 && p.data[12] == 4,
                     "rows read at the header's pitch, packed");
            t.IsTrue(dds::readDds(ddsFile(2, 2, kFlagsTexture | kFlagPitch, 1, kA8B8G8R8, rows, 0, 12), p) && p.data[8] == 0,
                     "PITCH alone: rows packed (the header's pitch not read)");
        });

        tc.Run("as RGBA8: every level decoded", [](TestCase &t)
        {
            tr::Picture p;
            dds::readDds(ddsFile(8, 8, kFlagsTexture | kFlagMipmapCount, 2, blocks("DXT1"), repeat(bc1Block(0xF800, 0x001F, 0), 5)),
                         p);
            const tr::PicturePtr src = std::make_shared<tr::Picture>(p);
            const tr::PicturePtr rgba = dds::asRgba8(src);
            t.IsTrue(rgba && rgba->format == tr::Format::RGBA8 && rgba->mips == 2u && rgba->valid(), "RGBA8, its two levels");
            t.Equals(texel(rgba->data, 0), 0xFF0000FFu, "level 0 red");
            t.Equals(texel(rgba->data, 64 + 15), 0xFF0000FFu, "level 1 red");
            t.IsTrue(dds::asRgba8(rgba) == rgba, "an RGBA8 picture is itself");
        });

        tc.Run("packs: .dds pictures indexed and read, CPU colours decoded", [](TestCase &t)
        {
            TempDir pack("pack");
            tp::Name a;
            a.textureHash = 0x5151;
            a.bits = 0x00u | (3u << 6) | (3u << 10); // CT32 8 x 8
            tp::Name b = a;
            b.textureHash = 0x6262;
            std::string dName = tp::fileName(a);
            dName.replace(dName.size() - 4, 4, ".dds");
            std::string eName = tp::fileName(b);
            eName.replace(eName.size() - 4, 4, ".DDS");
            // a: BC1 16 x 16 with levels 0 and 1; b: A8R8G8B8 4 x 4, no levels.
            writeFile(pack.path / dName, ddsFile(16, 16, kFlagsTexture | kFlagMipmapCount, 2, blocks("DXT1"),
                                                 repeat(bc1Block(0xF800, 0x001F, 0x55555555u), 20)));
            std::vector<uint8_t> blue;
            for (int i = 0; i < 16; ++i)
                blue.insert(blue.end(), {0xFF, 0x00, 0x00, 0x80}); // B G R A
            writeFile(pack.path / eName, ddsFile(4, 4, kFlagsTexture, 1, kA8R8G8B8, blue));
            tp::setPreload(false);
            tp::setFolders({pack.path});
            t.Equals(tp::stats().names, size_t{2}, "both indexed, the extension's case aside");
            tp::find(a);
            tp::find(b);
            tp::waitIdle();
            const tr::PicturePtr pa = tp::find(a), pb = tp::find(b);
            t.IsTrue(pa && pa->format == tr::Format::BC1 && pa->mips == 2u, "BC1 kept with the file's two levels");
            t.IsTrue(pb && pb->format == tr::Format::RGBA8 && pb->mips == 3u, "RGBA8: the rest of its chain made");
            if (pa)
            {
                const std::vector<uint32_t> c = tp::colorsAt(*pa, 8, 8);
                t.Equals(c[0], 0xFFFF0000u, "CPU: level 1's blue (every index 1)");
            }
            if (pb)
                t.Equals(tp::colorsAt(*pb, 4, 4)[5], 0x80FF0000u, "CPU: the uncompressed blue, alpha 0x80");
            t.Equals(tp::stats().failed, size_t{0}, "nothing failed");
            tp::setFolders({});
            tp::setPreload(true);
        });
    });
}
