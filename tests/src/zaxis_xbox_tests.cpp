#include "MiniTest.h"
#include "runtime/zaxis_xbox.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace zx::zaxis::xbox;

namespace
{
    void put32(std::vector<uint8_t> &out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    std::vector<uint8_t> hex(const std::string &text)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < text.size(); i += 2)
            out.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
        return out;
    }

    // A TXPR payload: count, XPR0 header (0x40 bytes), the DXT3 data of an
    // 8x4 texture: left block red, opaque; right block blue, half alpha.
    std::vector<uint8_t> dxt3Texture()
    {
        std::vector<uint8_t> p;
        put32(p, 1u);
        p.insert(p.end(), {'X', 'P', 'R', '0'});
        put32(p, 0x40u + 32u);
        put32(p, 0x40u);
        put32(p, 0x00040001u);
        put32(p, 0u);
        put32(p, 0u);
        // DXT3 (0x0E), 1 mip level, log2 width 3 (8), log2 height 2 (4).
        put32(p, (2u << 24) | (3u << 20) | (1u << 16) | (0x0Eu << 8) | 0x29u);
        put32(p, 0u);
        p.resize(4u + 0x40u, 0xAD);
        auto block = [&](uint16_t colour, uint8_t alphaNibble)
        {
            for (int i = 0; i < 8; ++i)
                p.push_back(static_cast<uint8_t>(alphaNibble | (alphaNibble << 4)));
            p.push_back(static_cast<uint8_t>(colour));
            p.push_back(static_cast<uint8_t>(colour >> 8));
            p.push_back(static_cast<uint8_t>(colour));
            p.push_back(static_cast<uint8_t>(colour >> 8));
            put32(p, 0u); // every pixel colour 0
        };
        block(0xF800u, 0xF); // red
        block(0x001Fu, 0x8); // blue, alpha 136
        return p;
    }

    std::vector<uint8_t> library(const std::vector<uint8_t> &picture)
    {
        std::vector<uint8_t> entry;
        entry.insert(entry.end(), {'T', 'X', 'R', 'H'});
        put32(entry, 52u);
        put32(entry, 0x01234567u);
        entry.push_back(8);
        entry.push_back(0);
        entry.push_back(4);
        entry.push_back(0);
        entry.resize(8u + 0x14u, 0);
        const char name[] = "TESTTEX";
        entry.insert(entry.end(), name, name + sizeof(name));
        entry.resize(8u + 52u, 0);
        entry.insert(entry.end(), {'T', 'X', 'P', 'R'});
        put32(entry, static_cast<uint32_t>(picture.size()));
        entry.insert(entry.end(), picture.begin(), picture.end());
        std::vector<uint8_t> file{'F', 'O', 'R', 'M'};
        put32(file, static_cast<uint32_t>(8u + entry.size()));
        file.insert(file.end(), {'T', 'X', 'R', 'C'});
        put32(file, static_cast<uint32_t>(entry.size()));
        file.insert(file.end(), entry.begin(), entry.end());
        return file;
    }
}

void register_zaxis_xbox_tests()
{
    MiniTest::Case("ZAxisXbox", [](TestCase &tc)
    {
        tc.Run("DXT3 texture decode", [](TestCase &t)
        {
            const std::vector<uint8_t> txpr = dxt3Texture();
            Picture picture;
            std::string error;
            t.IsTrue(decodeTexture(txpr.data(), txpr.size(), picture, &error), "decoded: " + error);
            t.Equals(picture.width, 8, "width");
            t.Equals(picture.height, 4, "height");
            t.Equals(picture.rgba[0], 0xFF0000FFu, "red, opaque");
            t.Equals(picture.rgba[7], 0x88FF0000u, "blue, alpha 0x88");
            t.Equals(picture.rgba[3 * 8 + 4], 0x88FF0000u, "right block's last row");
            const std::vector<uint8_t> broken(txpr.begin(), txpr.begin() + 60);
            t.IsFalse(decodeTexture(broken.data(), broken.size(), picture, &error), "short data rejected");
        });

        tc.Run("palette of a picture with few colours is exact", [](TestCase &t)
        {
            Picture picture{4, 1, {0xFF0000FFu, 0xFF00FF00u, 0x80FF0000u, 0xFF0000FFu}};
            Palettized image;
            palettize(picture, true, image);
            t.Equals(image.palette.size(), size_t(256), "256 entries");
            for (size_t i = 0; i < 4; ++i)
                t.Equals(image.palette[image.index[i]], picture.rgba[i], "pixel " + std::to_string(i));
            Palettized opaque;
            palettize(picture, false, opaque);
            t.Equals(opaque.palette[opaque.index[2]], 0xFFFF0000u, "alpha dropped without alpha");
        });

        tc.Run("many colours: at most 256, close", [](TestCase &t)
        {
            Picture picture;
            picture.width = 64;
            picture.height = 64;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    picture.rgba.push_back(0xFF000000u | static_cast<uint32_t>(x * 4) | (static_cast<uint32_t>(y * 4) << 8) |
                                           (static_cast<uint32_t>((x + y) * 2) << 16));
            Palettized image;
            palettize(picture, false, image);
            int worst = 0;
            for (size_t i = 0; i < picture.rgba.size(); ++i)
                for (int c = 0; c < 3; ++c)
                {
                    const int a = static_cast<int>((picture.rgba[i] >> (8 * c)) & 0xFF);
                    const int b = static_cast<int>((image.palette[image.index[i]] >> (8 * c)) & 0xFF);
                    worst = std::max(worst, std::abs(a - b));
                }
            t.IsTrue(worst <= 24, "every channel within 24 of the original: " + std::to_string(worst));
        });

        tc.Run("shrink: box filter, alpha-weighted colour", [](TestCase &t)
        {
            Picture picture{4, 2, {0xFF0000FFu, 0x00FF0000u, 0xFF00FF00u, 0xFF00FF00u,
                                   0xFF0000FFu, 0x00FF0000u, 0xFF00FF00u, 0xFF00FF00u}};
            shrink(picture, 2);
            t.Equals(picture.width, 2, "width halved");
            t.Equals(picture.height, 1, "height halved");
            t.Equals(picture.rgba.size(), size_t(2), "pixels");
            t.Equals(picture.rgba[0] >> 24, 0x80u, "alpha averaged");
            t.IsTrue((picture.rgba[0] & 0xFFu) >= 0xFDu && ((picture.rgba[0] >> 16) & 0xFFu) <= 2u,
                     "colour from the opaque pixels only");
            t.Equals(picture.rgba[1], 0xFF00FF00u, "uniform block unchanged");
            Picture small{2, 2, {1u, 2u, 3u, 4u}};
            shrink(small, 128);
            t.Equals(small.width, 2, "nothing to do under the limit");
        });

        tc.Run("texture library to the PS2's", [](TestCase &t)
        {
            const std::vector<uint8_t> xbox = library(dxt3Texture());
            std::vector<uint8_t> ps2;
            std::vector<TextureReport> report;
            std::string error;
            t.IsTrue(convertTextureLibrary(xbox, ps2, &report, &error, 2), "converted: " + error);
            t.Equals(report.size(), size_t(1), "one texture");
            if (report.size() != 1)
                return;
            t.Equals(report[0].name, std::string("TESTTEX"), "name");
            t.IsTrue(report[0].alpha, "has alpha");
            t.IsTrue(report[0].error.empty(), "no error");
            t.IsTrue(std::memcmp(ps2.data(), "FORM", 4) == 0, "FORM");
            uint32_t formSize = 0;
            std::memcpy(&formSize, &ps2[4], 4);
            t.Equals(static_cast<size_t>(formSize) + 8u, ps2.size(), "FORM size");
            t.IsTrue(std::memcmp(&ps2[8], "TXRC", 4) == 0 && std::memcmp(&ps2[16], "TXRH", 4) == 0, "TXRC, TXRH");
            t.IsTrue(std::memcmp(&ps2[24], &xbox[24], 52) == 0, "header kept");
            std::vector<uint8_t> halved;
            std::vector<TextureReport> halvedReport;
            t.IsTrue(convertTextureLibrary(xbox, halved, &halvedReport, &error, 1, 4), "converted at most 4 a side");
            if (halvedReport.size() == 1)
            {
                t.Equals(halvedReport[0].width, 4, "width halved");
                t.Equals(halvedReport[0].sourceWidth, 8, "source width");
                uint16_t w = 0, h = 0;
                std::memcpy(&w, &halved[24 + 4], 2);
                std::memcpy(&h, &halved[24 + 6], 2);
                t.IsTrue(w == 4 && h == 2, "TXRH has the new size");
            }
            {
                // The Xbox picture for the renderer, keyed by the converted
                // texture's size and indices (top row first).
                std::vector<uint8_t> small;
                std::vector<TextureReport> smallReport;
                std::vector<HostTexture> host;
                t.IsTrue(convertTextureLibrary(xbox, small, &smallReport, &error, 1, 4, &host), "converted with pictures");
                t.Equals(host.size(), size_t(1), "one picture");
                if (host.size() == 1)
                {
                    const HostTexture &h = host[0];
                    t.IsTrue(h.dxt3 && h.pictureWidth == 8 && h.pictureHeight == 4 && h.mips == 1, "the Xbox DXT3 as it is");
                    const std::vector<uint8_t> txpr = dxt3Texture();
                    t.IsTrue(h.data.size() == 32u && std::memcmp(h.data.data(), txpr.data() + 4 + 0x40, 32) == 0,
                             "its two blocks");
                    t.IsTrue(h.width == 4 && h.height == 2 && h.name == "TESTTEX", "keyed by the converted size");
                    const uint8_t *bmp = &small[84];
                    uint32_t pixels = 0;
                    std::memcpy(&pixels, bmp + 10, 4);
                    std::vector<uint8_t> indices; // top row first: the BMP's rows reversed
                    for (int y = 1; y >= 0; --y)
                        indices.insert(indices.end(), bmp + pixels + y * 4, bmp + pixels + y * 4 + 4);
                    uint64_t fnv = 0xCBF29CE484222325ull;
                    for (uint8_t b : indices)
                        fnv = (fnv ^ b) * 0x100000001B3ull;
                    t.Equals(h.indexHash, fnv, "index hash of the converted indices");
                }
            }
            t.IsTrue(std::memcmp(&ps2[76], "TZMP", 4) == 0, "alpha texture as TZMP");
            uint32_t bmpSize = 0;
            std::memcpy(&bmpSize, &ps2[80], 4);
            t.Equals(bmpSize % 4u, 0u, "padded to 4");
            const uint8_t *bmp = &ps2[84];
            t.IsTrue(bmp[0] == 'B' && bmp[1] == 'M', "a BMP");
            t.Equals(static_cast<int>(bmp[28]), 8, "8 bits per pixel");
            // Bottom-up rows: the first stored row is the picture's last.
            const uint8_t index = bmp[54 + 1024 + 7];
            const uint8_t *entry = bmp + 54 + 4 * index;
            t.IsTrue(entry[0] == 0xFF && entry[2] == 0 && entry[3] == 0x88, "blue, alpha 0x88, BGRA");
        });

        tc.Run("texel budget: the largest made smaller first", [](TestCase &t)
        {
            // dm2_xbox.py's selftest case: a 256 and a 128 at most 128 a side.
            size_t texels = 0;
            std::vector<int> sides = budgetSides({{256, 256}, {128, 64}}, 128, 1u << 30, &texels);
            t.IsTrue(sides == std::vector<int>({128, 128}), "under the budget: as they are");
            t.Equals(texels, size_t(128 * 128 + 128 * 64), "their texels");
            sides = budgetSides({{256, 256}, {128, 64}, {256, 256}}, 128, 2u * 64u * 64u + 128u * 64u, &texels);
            t.IsTrue(sides == std::vector<int>({64, 128, 64}), "both largest halved, ties by order");
            t.Equals(texels, size_t(2 * 64 * 64 + 128 * 64), "within the budget");
            sides = budgetSides({{32, 32}}, 128, 1u, &texels);
            t.IsTrue(sides == std::vector<int>({16}), "never under 16 a side");
            t.Equals(texels, size_t(16 * 16), "what 16 a side leaves");
            sides = budgetSides({{8, 4}}, 0, 0u, &texels);
            t.IsTrue(sides == std::vector<int>({8}) && texels == 32u, "no limit, no budget: its own size");

            // A library over its budget: a 32 x 32 texture halved, its TXRH
            // and the Xbox picture's key follow.
            std::vector<uint8_t> txpr = dxt3Texture(); // 8 x 4: made 32 x 32 of its red block
            const std::vector<uint8_t> red(txpr.begin() + 4 + 0x40, txpr.begin() + 4 + 0x40 + 16);
            txpr.resize(4u + 0x40u);
            for (int b = 0; b < 64; ++b)
                txpr.insert(txpr.end(), red.begin(), red.end());
            const uint32_t format = (5u << 24) | (5u << 20) | (1u << 16) | (0x0Eu << 8) | 0x29u;
            std::memcpy(&txpr[4 + 24], &format, 4);
            const uint32_t size = 0x40u + 64u * 16u;
            std::memcpy(&txpr[4 + 4], &size, 4);
            std::vector<uint8_t> xbox = library(txpr);
            xbox[28] = 32; // the TXRH's width and height
            xbox[30] = 32;
            std::vector<uint8_t> ps2;
            std::vector<TextureReport> report;
            std::vector<HostTexture> host;
            std::string error;
            t.IsTrue(convertTextureLibrary(xbox, ps2, &report, &error, 1, 128, &host, 1024u), "within 1024: " + error);
            t.IsTrue(report.size() == 1 && report[0].width == 32 && report[0].error.empty(), "32 x 32 fits 1024 bytes");
            report.clear();
            host.clear();
            t.IsTrue(convertTextureLibrary(xbox, ps2, &report, &error, 1, 128, &host, 1023u), "within 1023: " + error);
            t.IsTrue(report.size() == 1 && report[0].width == 16 && report[0].height == 16, "halved to 16 x 16");
            t.IsTrue(host.size() == 1 && host[0].width == 16 && host[0].pictureWidth == 32, "the picture keyed by 16 x 16");
            uint16_t w = 0;
            std::memcpy(&w, &ps2[28], 2);
            t.Equals(static_cast<int>(w), 16, "TXRH has the new size");
        });

        tc.Run("material layers in the PS2 layout", [](TestCase &t)
        {
            struct Case
            {
                const char *xbox, *ps2;
            } cases[] = {
                // Textured, no clamp; untextured; clamp region 128 texels;
                // blended (flags 0x04). Seven words each.
                {"01000000" "0c000040" "0000f749" "0101001c" "44332211" "01000000" "01000000",
                 "1c000000" "00000040" "f082f749" "0101001c" "44332211" "01000000" "00000000"},
                {"01000000" "0c000000" "00000000" "00000000" "00000000" "01000000" "00000000",
                 "1c000000" "00000000" "f002fc49" "0131001c" "00000000" "00000000" "00000000"},
                {"01001000" "0cfc0140" "0000f749" "0101001c" "44332211" "01000000" "01000000",
                 "1cfc1f00" "00fc0140" "f082f749" "0101001c" "44332211" "01000000" "00000000"},
                {"01000400" "04000040" "0000f705" "01044014" "44332211" "01000000" "01000000",
                 "3c000000" "00000040" "f082f705" "01054014" "44332211" "01000000" "00000000"},
            };
            // Without render state (the Zoo): a decal (w3 bit 28 only, the
            // tree shadows) is blended without depth writes, not black.
            {
                std::vector<uint8_t> decal = hex("01150500" "0c000000" "00000000" "00000010" "44332211" "01000000" "01000000");
                convertLayer(decal.data(), true);
                t.IsTrue(decal == hex("3c000000" "00000040" "f082f709" "0101001c" "44332211" "01000000" "00000000"),
                         "decal: alpha blend 0x09, w3 0x1c000101");
                std::vector<uint8_t> plain = hex("01000000" "0c000000" "00000000" "00000000" "44332211" "01000000" "01000000");
                convertLayer(plain.data(), false);
                t.IsTrue(plain == hex("1c000000" "00000040" "f082f749" "0101001c" "44332211" "01000000" "00000000"),
                         "opaque layer: the PS2's common state");
            }
            // The Zoo's grass base (opaque, no alpha test, depth GEQUAL, as
            // the Tutorial's grass), a cut-out (blended with an alpha test),
            // and a mask texture (opaque).
            {
                std::vector<uint8_t> grass = hex("01010000" "0c000000" "00000000" "00000000" "44332211" "01000000" "01000000");
                convertLayer(grass.data(), true, true);
                t.IsTrue(grass == hex("1c000000" "00000040" "f082f749" "01010014" "44332211" "01000000" "00000000"),
                         "grass base: w3 0x14000101");
                std::vector<uint8_t> cutout = hex("01000000" "0c000000" "00000000" "00000000" "44332211" "01000000" "01000000");
                convertLayer(cutout.data(), true);
                t.IsTrue(cutout == hex("3c000000" "00000040" "f082f709" "0135041c" "44332211" "01000000" "00000000"),
                         "cut-out: alpha blend, alpha test GREATER 0x10");
            }
            for (const Case &c : cases)
            {
                std::vector<uint8_t> layer = hex(c.xbox);
                const std::vector<uint8_t> expected = hex(c.ps2);
                t.Equals(layer.size(), size_t(28), "28 bytes");
                convertLayer(layer.data());
                t.IsTrue(layer == expected, std::string("layer ") + c.xbox);
            }
        });
        tc.Run("Zoo grass materials: types 1 and 2 as the PS2's grass", [](TestCase &t)
        {
            // FORM { TGEO { TMTB { MATI x3 } } }: flags 0x8 (unblended base),
            // 0x408 (a blended dirt decal), 0x54 (caustics).
            auto mati = [](uint32_t flags, uint32_t hdr)
            {
                std::vector<uint8_t> m(160, 0);
                std::memcpy(&m[0], &flags, 4);
                const uint32_t one = 1, w1 = 0x0C, tex = 0x11223344;
                std::memcpy(&m[0x2C], &one, 4);
                std::memcpy(&m[0x30], &hdr, 4);
                std::memcpy(&m[0x34], &w1, 4);
                std::memcpy(&m[0x40], &tex, 4);
                return m;
            };
            auto chunk = [](const char *tag, const std::vector<uint8_t> &body)
            {
                std::vector<uint8_t> c(tag, tag + 4);
                const uint32_t size = static_cast<uint32_t>(body.size());
                c.insert(c.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
                c.insert(c.end(), body.begin(), body.end());
                return c;
            };
            std::vector<uint8_t> mats;
            for (auto m : {mati(0x8u, 0x201u), mati(0x408u, 0x50201u), mati(0x54u, 0x21501u)})
            {
                auto c = chunk("MATI", m);
                mats.insert(mats.end(), c.begin(), c.end());
            }
            std::vector<uint8_t> geo = chunk("FORM", chunk("TGEO", chunk("TMTB", mats)));
            t.Equals(convertMaterials(geo), 3L, "three materials");
            auto word = [&](size_t material, size_t offset)
            {
                uint32_t v = 0;
                std::memcpy(&v, &geo[8 + 8 + 8 + material * 168 + 8 + offset], 4);
                return v;
            };
            t.Equals(word(0, 0), 0x4u, "type 2 grass: the PS2's 0x4");
            t.Equals(word(0, 0x3C), 0x14000101u, "its base opaque, depth GEQUAL");
            t.Equals(word(1, 0), 0x400u, "a blended layer: no grass");
            t.Equals(word(2, 0), 0x0u, "caustics: the Xbox effect and its 0x4 dropped");
        });

        tc.Run("NPC model (FMDL) to the PS2's: texture, strips, materials", [](TestCase &t)
        {
            auto chunk = [](const char *tag, const std::vector<uint8_t> &body)
            {
                std::vector<uint8_t> c(tag, tag + 4);
                put32(c, static_cast<uint32_t>(body.size()));
                c.insert(c.end(), body.begin(), body.end());
                return c;
            };
            auto cat = [](std::initializer_list<std::vector<uint8_t>> parts)
            {
                std::vector<uint8_t> out;
                for (const auto &p : parts)
                    out.insert(out.end(), p.begin(), p.end());
                return out;
            };
            // The texture (the library's TXRC entry, id 0x01234567): its
            // right half clear (alpha 0x22), a cut-out like the willows'.
            std::vector<uint8_t> picture = dxt3Texture();
            std::fill(picture.begin() + 4 + 0x40 + 16, picture.begin() + 4 + 0x40 + 24, uint8_t{0x22});
            const std::vector<uint8_t> lib = library(picture);
            const std::vector<uint8_t> txrc(lib.begin() + 8, lib.end());
            // One object: a header, a skin chunk kept as it is, a strip of
            // 70 entries (two PS2 groups) and a material without render state
            // on that texture.
            std::vector<uint8_t> tgph(20, 0);
            tgph[8] = 1; // one group
            std::vector<uint8_t> fm;
            put32(fm, 0u | (70u << 16)); // submesh 0, 70 entries
            for (uint16_t i = 0; i < 70; ++i)
            {
                fm.push_back(static_cast<uint8_t>(i));
                fm.push_back(i < 2 ? 0x80 : 0x00);
            }
            std::vector<uint8_t> mati(160, 0);
            const uint32_t one = 1, hdr = 0x1u, w1 = 0x1Cu, tex = 0x01234567u;
            std::memcpy(&mati[0x2C], &one, 4);
            std::memcpy(&mati[0x30], &hdr, 4);
            std::memcpy(&mati[0x34], &w1, 4);
            std::memcpy(&mati[0x40], &tex, 4);
            const std::vector<uint8_t> mobo = chunk("MOBO", std::vector<uint8_t>(12, 0xAB));
            const std::vector<uint8_t> tgeo =
                chunk("TGEO", cat({chunk("TGPT", cat({chunk("TGPH", tgph), mobo, chunk("TGFM", fm)})),
                                   chunk("TMTB", chunk("MATI", mati))}));
            std::vector<uint8_t> zit = chunk("FMDL", chunk("MODL", cat({txrc, tgeo})));
            ModelReport report;
            std::string error;
            std::vector<HostTexture> host;
            t.IsTrue(convertModel(zit, &report, &error, 4, &host), "converted: " + error);
            t.IsTrue(std::memcmp(zit.data(), "FMDL", 4) == 0 && std::memcmp(&zit[8], "MODL", 4) == 0, "FMDL > MODL");
            uint32_t fmdl = 0, modl = 0;
            std::memcpy(&fmdl, &zit[4], 4);
            std::memcpy(&modl, &zit[12], 4);
            t.Equals(static_cast<size_t>(fmdl) + 8u, zit.size(), "FMDL size");
            t.Equals(static_cast<size_t>(modl) + 16u, zit.size(), "MODL size");
            t.Equals(report.textures.size(), size_t(1), "one texture");
            t.IsTrue(!report.textures.empty() && report.textures[0].cutout, "a cut-out");
            t.Equals(host.size(), size_t(1), "its Xbox picture for the renderer");
            t.IsTrue(std::memcmp(&zit[16], "TXRC", 4) == 0, "the texture first");
            const std::string bytes(zit.begin(), zit.end());
            t.IsTrue(bytes.find("TZMP") != std::string::npos && bytes.find("TXPR") == std::string::npos,
                     "the picture as an 8-bit BMP with alpha");
            t.Equals(report.geometry.groupsIn, 1L, "one Xbox strip");
            t.Equals(report.geometry.groupsOut, 2L, "cut into two PS2 groups");
            t.Equals(report.geometry.materials, 1L, "one material");
            const size_t ph = bytes.find("TGPH");
            uint32_t groups = 0;
            std::memcpy(&groups, &zit[ph + 8 + 8], 4);
            t.Equals(groups, 2u, "TGPH group count");
            t.IsTrue(bytes.find(std::string(mobo.begin(), mobo.end())) != std::string::npos, "other chunks kept");
            const size_t m = bytes.find("MATI") + 8;
            uint32_t w3 = 0;
            std::memcpy(&w3, &zit[m + 0x3C], 4);
            t.Equals(w3, 0x1C043501u, "the cut-out texture's layer alpha-tested, as the level's");
            std::vector<uint8_t> notModel = chunk("FORM", {});
            t.IsFalse(convertModel(notModel, nullptr, &error), "not a model");
        });
    });
}
