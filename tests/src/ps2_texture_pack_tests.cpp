#include "MiniTest.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_texture_pack.h"
#include "test_zip_builder.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    namespace tp = zx::texture_pack;

    struct TempDir
    {
        std::filesystem::path path;
        explicit TempDir(const char *what)
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / (std::string("zx-texpack-") + what + "-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    // 256 bytes i * 7 + 3 (the reference vectors below are XXH3-64 0.8.3's
    // of these, from xxhash.h itself).
    std::vector<uint8_t> pattern()
    {
        std::vector<uint8_t> b(256);
        for (int i = 0; i < 256; ++i)
            b[i] = static_cast<uint8_t>(i * 7 + 3);
        return b;
    }

    // CLAMP: WMS, WMT and the four 10-bit fields.
    uint64_t clampOf(uint32_t wms, uint32_t wmt, uint32_t minU, uint32_t maxU, uint32_t minV, uint32_t maxV)
    {
        return static_cast<uint64_t>(wms) | (static_cast<uint64_t>(wmt) << 2) | (static_cast<uint64_t>(minU) << 4) |
               (static_cast<uint64_t>(maxU) << 14) | (static_cast<uint64_t>(minV) << 24) | (static_cast<uint64_t>(maxV) << 34);
    }
}

void register_ps2_texture_pack_tests()
{
    MiniTest::Case("TexturePack", [](TestCase &tc)
    {
        GSMem::InitLookupTables();

        tc.Run("CLUT hash is XXH3-64 0.8.3", [](TestCase &t)
        {
            t.Equals(tp::clutHash(nullptr, 0), 0x2D06800538D394C2ull, "empty input");
            const std::vector<uint8_t> b = pattern();
            uint32_t entries[64];
            std::memcpy(entries, b.data(), 256);
            t.Equals(tp::clutHash(entries, 64), 0x3C38817F6D79C0DAull, "256 bytes");
            t.Equals(tp::clutHash(entries, 16), 0x287EB1FA9E4BE2C1ull, "64 bytes");
        });

        tc.Run("whole blocks are hashed as GS memory holds them", [](TestCase &t)
        {
            std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
            const std::vector<uint8_t> b = pattern();
            std::memcpy(vram.data(), b.data(), 256);
            const GSTexaReg texa{};
            // T8 16 x 16 at block 0: exactly block 0.
            tp::Level t8{0, 1, 4, 4};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_T8, &t8, 1, tp::Region{}, texa), 0x3C38817F6D79C0DAull, "T8 16x16");
            // T4 32 x 16 and CT32 8 x 8: block 0 too.
            tp::Level t4{0, 1, 5, 4};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_T4, &t4, 1, tp::Region{}, texa), 0x3C38817F6D79C0DAull, "T4 32x16");
            tp::Level c32{0, 1, 3, 3};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_CT32, &c32, 1, tp::Region{}, texa), 0x3C38817F6D79C0DAull,
                     "CT32 8x8");
            // T8 32 x 16: blocks 0 and 1 (the PSMT8 page's first row is 0 1 4 5 ...).
            std::vector<uint8_t> two(512);
            for (int i = 0; i < 512; ++i)
                two[i] = vram[i] = static_cast<uint8_t>(i * 13 + 1);
            uint32_t words[128];
            std::memcpy(words, two.data(), 512);
            tp::Level t8w{0, 1, 5, 4};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_T8, &t8w, 1, tp::Region{}, texa), tp::clutHash(words, 128),
                     "T8 32x16: blocks 0, 1");
        });

        tc.Run("small, 16-bit and region textures are hashed as read out", [](TestCase &t)
        {
            std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
            // A T8 8 x 8 (smaller than its 16 x 16 block): its indices row by row.
            std::vector<uint8_t> rows;
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                {
                    const uint8_t v = static_cast<uint8_t>(x * 31 + y * 5 + 7);
                    GSMem::WriteP8(vram.data(), 64, 1, x, y, v);
                    rows.push_back(v);
                }
            // (64 bytes = 16 words: the same XXH3 as the CLUT hash of them)
            uint32_t words[16];
            std::memcpy(words, rows.data(), 64);
            const GSTexaReg texa{};
            tp::Level small{64, 1, 3, 3};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_T8, &small, 1, tp::Region{}, texa), tp::clutHash(words, 16),
                     "T8 8x8 by its indices");

            // CT16 16 x 8 (one block, but 16-bit): read out as RGBA with TEXA.
            GSTexaReg texa16;
            texa16.ta0 = 0x20;
            texa16.ta1 = 0x70;
            texa16.aem = true;
            std::vector<uint32_t> colors;
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 16; ++x)
                {
                    const uint32_t c = (x == 0 && y == 0) ? 0u : ((x * 3u) | (y << 5) | ((x + y) << 10) | ((x & 1u) << 15));
                    GSMem::WriteCT16(vram.data(), 128, 1, x, y, c);
                    uint32_t a = (c & 0x8000u) ? texa16.ta1 : texa16.ta0;
                    if (c == 0u)
                        a = 0u; // AEM
                    colors.push_back(((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (a << 24));
                }
            tp::Level c16{128, 1, 4, 3};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_CT16, &c16, 1, tp::Region{}, texa16),
                     tp::clutHash(colors.data(), static_cast<uint32_t>(colors.size())), "CT16 by RGBA with TEXA");
            t.Equals(tp::nameBits(GS_PSM_CT16, 4, 3, texa16), 0x02u | (4u << 6) | (3u << 10) | (0x20u << 15) | (1u << 23) | (0x70u << 24),
                     "CT16's bits carry TEXA");
            t.Equals(tp::nameBits(GS_PSM_T8, 7, 6, texa16), 0x13u | (7u << 6) | (6u << 10), "T8's do not");

            // A region of a T8 32 x 32: its rows from the region's left.
            for (uint32_t y = 0; y < 32; ++y)
                for (uint32_t x = 0; x < 32; ++x)
                    GSMem::WriteP8(vram.data(), 256, 1, x, y, (x ^ (y * 3)) & 0xFFu);
            tp::Region r = tp::regionOf(5, 5, clampOf(2, 0, 4, 19, 0, 0));
            t.IsTrue(r.hasX && !r.hasY && r.minX == 4 && r.maxX == 20, "REGION_CLAMP 4..19 is the region [4, 20)");
            std::vector<uint8_t> part;
            for (uint32_t y = 0; y < 32; ++y)
                for (uint32_t x = 4; x < 20; ++x)
                    part.push_back(static_cast<uint8_t>((x ^ (y * 3)) & 0xFFu));
            std::vector<uint32_t> partWords(part.size() / 4);
            std::memcpy(partWords.data(), part.data(), part.size());
            tp::Level big{256, 1, 5, 5};
            t.Equals(tp::textureHash(vram.data(), GS_PSM_T8, &big, 1, r, texa),
                     tp::clutHash(partWords.data(), static_cast<uint32_t>(partWords.size())), "T8 region by its indices");
        });

        tc.Run("a mipmapped texture's names, nearest base first", [](TestCase &t)
        {
            std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
            for (size_t i = 0; i < vram.size(); ++i)
                vram[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
            const GSTexaReg texa{};
            // T8 64 x 64 with two more levels.
            tp::Level levels[3] = {{1024, 1, 6, 6}, {2048, 1, 5, 5}, {2304, 1, 4, 4}};
            tp::Candidate c[8];
            t.Equals(tp::candidateNames(vram.data(), GS_PSM_T8, levels, 1, 0, texa, 0x77, c), 1u, "not mipmapped: one name");
            t.Equals(c[0].name.textureHash, tp::textureHash(vram.data(), GS_PSM_T8, levels, 1, tp::Region{}, texa), "level 0");
            t.Equals(c[0].name.clutHash, 0x77ull, "with the palette's hash");
            const uint32_t n = tp::candidateNames(vram.data(), GS_PSM_T8, levels, 3, 0, texa, 0x77, c);
            t.Equals(n, 4u, "level 0 alone, 0..2, 1..2, 2 alone");
            t.Equals(c[1].name.textureHash, tp::textureHash(vram.data(), GS_PSM_T8, levels, 3, tp::Region{}, texa), "0..2");
            t.Equals(c[2].base, 1u, "then from level 1");
            t.Equals(c[2].name.bits, 0x13u | (5u << 6) | (5u << 10), "named by level 1's size");
            t.Equals(c[3].name.textureHash, tp::textureHash(vram.data(), GS_PSM_T8, levels + 2, 1, tp::Region{}, texa), "2 alone");
            // A level 0 at block 0 (the frame buffer) is no base.
            levels[0].tbp = 0;
            const uint32_t m = tp::candidateNames(vram.data(), GS_PSM_T8, levels, 3, 0, texa, 0x77, c);
            t.IsTrue(m == 3u && c[0].base == 1u, "from level 1: alone, 1..2, 2 alone");
            t.Equals(tp::candidateNames(vram.data(), GS_PSM_T8, levels, 3, 0, texa, 0x77, c, true), 1u, "the dump's name only");
        });

        tc.Run("regions from CLAMP as PCSX2 makes them", [](TestCase &t)
        {
            const tp::Region none = tp::regionOf(6, 6, clampOf(0, 1, 0, 0, 0, 0));
            t.IsTrue(!none.hasX && !none.hasY, "REPEAT / CLAMP: none");
            const tp::Region whole = tp::regionOf(6, 6, clampOf(3, 3, 63, 0, 63, 0));
            t.IsTrue(!whole.hasX && !whole.hasY, "REGION_REPEAT over the whole texture: none");
            const tp::Region cell = tp::regionOf(6, 6, clampOf(3, 0, 15, 32, 0, 0));
            t.IsTrue(cell.hasX && cell.minX == 32 && cell.maxX == 48, "REGION_REPEAT MSK 15 FIX 32: [32, 48)");
            const tp::Region full = tp::regionOf(6, 6, clampOf(2, 2, 0, 63, 0, 63));
            t.IsTrue(!full.hasX && !full.hasY, "REGION_CLAMP over the whole texture: none");
            const tp::Region l1 = cell.atLevel(1);
            t.IsTrue(l1.minX == 16 && l1.maxX == 24, "the cell at level 1");
        });

        tc.Run("the palette as PCSX2 reads it", [](TestCase &t)
        {
            std::vector<uint16_t> clut(512);
            for (uint32_t i = 0; i < 256; ++i)
            {
                clut[i] = static_cast<uint16_t>(i * 3);
                clut[i + 256] = static_cast<uint16_t>(0x8000u | i);
            }
            uint32_t out[256];
            GSTexaReg texa{};
            t.Equals(tp::readClut(clut.data(), GS_PSM_T8, GS_PSM_CT32, 0, texa, out), 256u, "T8: 256 entries");
            t.Equals(out[5], 15u | ((0x8000u | 5u) << 16), "a 32-bit entry from both halves");
            t.Equals(tp::readClut(clut.data(), GS_PSM_T4, GS_PSM_CT32, 2, texa, out), 16u, "T4: 16 entries");
            t.Equals(out[0], 96u | ((0x8000u | 32u) << 16), "from CSA 2");
            // T8 with CSA: each run of 16 clamps at the last (GTA's dirt).
            tp::readClut(clut.data(), GS_PSM_T8, GS_PSM_CT32, 15, texa, out);
            t.Equals(out[0x31] & 0xFFFFu, (240u + 1u) * 3u & 0xFFFFu, "CSA 15: clamped to entry 240");
            // 16-bit: TEXA, AEM for 0.
            clut[0] = 0;
            clut[1] = 0x801F;
            texa.ta0 = 0x10;
            texa.ta1 = 0x80;
            texa.aem = true;
            tp::readClut(clut.data(), GS_PSM_T4, GS_PSM_CT16, 0, texa, out);
            t.Equals(out[0], 0u, "AEM: 0 is transparent");
            t.Equals(out[1], 0xF8u | (0x80u << 24), "the alpha bit picks TA1");
        });

        tc.Run("file names are PCSX2's", [](TestCase &t)
        {
            tp::Name n;
            n.textureHash = 0x00ab12;
            n.clutHash = 0xdeadbeef00112233ull;
            n.bits = 0x13u | (7u << 6) | (6u << 10);
            t.Equals(tp::fileName(n), std::string("ab12-deadbeef00112233-000019d3.png"), "palette");
            t.Equals(tp::fileName(n, 2), std::string("ab12-deadbeef00112233-000019d3-mip2.png"), "a mip level");
            n.regionWidth = 16;
            t.Equals(tp::fileName(n), std::string("ab12-deadbeef00112233-r16x0-000019d3.png"), "a region");
            tp::Name direct;
            direct.textureHash = 0x1234567890abcdefull;
            direct.bits = 0x02u | (5u << 6) | (5u << 10) | (0x80u << 24);
            t.Equals(tp::fileName(direct), std::string("1234567890abcdef-80001542.png"), "no palette");

            uint32_t mip = 9;
            auto p = tp::parseFileName("ab12-deadbeef00112233-000019d3.PNG", &mip);
            t.IsTrue(p && p->textureHash == 0xab12u && p->clutHash == 0xdeadbeef00112233ull && p->bits == 0x19d3u && mip == 0,
                     "parse: palette");
            p = tp::parseFileName("ab12-deadbeef00112233-r16x0-000019d3-mip3.png", &mip);
            t.IsTrue(p && p->regionWidth == 16 && p->regionHeight == 0 && mip == 3, "parse: region, mip");
            p = tp::parseFileName("1234567890abcdef-80001542.dds", &mip);
            t.IsTrue(p && p->textureHash == 0x1234567890abcdefull && p->clutHash == 0 && p->bits == 0x80001542u,
                     "parse: no palette");
            p = tp::parseFileName("ab12-deadbeef-000059d3.png");
            t.IsTrue(p && p->bits == 0x19d3u, "parse: the old TCC bit dropped");
            t.IsFalse(tp::parseFileName("readme.png").has_value(), "not a name");
            t.IsFalse(tp::parseFileName("ab12-zz-000019d3.png").has_value(), "not hex");
            t.IsFalse(tp::parseFileName("ab12-000019d3").has_value(), "no extension");
        });

        tc.Run("a picture at the PS2's size is box-filtered", [](TestCase &t)
        {
            zx::texture_replace::Picture p;
            p.width = 4;
            p.height = 2;
            p.mips = 1;
            p.format = zx::texture_replace::Format::RGBA8;
            p.data.assign(4u * 2u * 4u, 0);
            for (uint32_t i = 0; i < 8; ++i)
                p.data[i * 4u] = static_cast<uint8_t>(i * 10); // R
            const std::vector<uint32_t> c = tp::colorsAt(p, 2, 1);
            t.Equals(c.size(), size_t{2}, "2 x 1");
            t.Equals(c[0] & 0xFFu, (0u + 10u + 40u + 50u + 2u) / 4u, "left: the mean of its 2 x 2");
            t.Equals(c[1] & 0xFFu, (20u + 30u + 60u + 70u + 2u) / 4u, "right");
        });

        tc.Run("packs: dump, index, read, a later pack wins, mips", [](TestCase &t)
        {
            TempDir low("low"), high("high"), dumps("dump");
            tp::Name a;
            a.textureHash = 0x1111;
            a.clutHash = 0x2222;
            a.bits = 0x13u | (3u << 6) | (3u << 10);
            tp::Name b = a;
            b.textureHash = 0x3333;
            // Dump mode writes the pictures (8 x 8, two colours).
            tp::setDumpFolder(dumps.path);
            t.IsTrue(tp::active() && tp::dumping(), "dump mode on");
            tp::dump(a, 8, 8, std::vector<uint32_t>(64, 0x80102030u));
            tp::dump(a, 8, 8, std::vector<uint32_t>(64, 0x80FFFFFFu)); // again: not written twice
            tp::dump(b, 8, 8, std::vector<uint32_t>(64, 0x40405060u));
            tp::waitIdle();
            t.Equals(tp::stats().dumped, size_t{2}, "two files");
            t.IsTrue(std::filesystem::exists(dumps.path / tp::fileName(a)), "named as PCSX2 names it");
            tp::setDumpFolder({});
            // The low pack has a and b, the high one a again.
            std::filesystem::create_directories(low.path / "sub");
            std::filesystem::copy_file(dumps.path / tp::fileName(a), low.path / "sub" / tp::fileName(a));
            std::filesystem::copy_file(dumps.path / tp::fileName(b), low.path / tp::fileName(b));
            std::filesystem::copy_file(dumps.path / tp::fileName(b), high.path / tp::fileName(a));
            // and a's level 1 (4 x 4), b's a level of the wrong size.
            std::filesystem::copy_file(dumps.path / tp::fileName(b), high.path / tp::fileName(a, 1));
            tp::setPreload(false);
            tp::setFolders({low.path, high.path});
            t.Equals(tp::stats().names, size_t{2}, "two names");
            tp::Name shape = a;
            shape.textureHash = 0x9999;
            t.IsTrue(tp::mayHave(shape), "a name of a's shape may be there (bits and region alike)");
            shape.bits ^= 1u << 6;
            t.IsFalse(tp::mayHave(shape), "another size: no picture, nothing to hash");
            bool known = false;
            t.IsTrue(tp::find(a, &known) == nullptr && known, "first use: queued, the PS2 texture meanwhile");
            tp::Name c = a;
            c.textureHash = 0x4444;
            t.IsTrue(tp::find(c, &known) == nullptr && !known, "an unknown name");
            tp::waitIdle();
            auto pa = tp::find(a, &known);
            t.IsTrue(pa != nullptr, "read");
            if (pa)
            {
                t.Equals(pa->width, 8u, "8 x 8");
                t.Equals(pa->mips, 4u, "a chain to 1 x 1");
                t.Equals(static_cast<uint32_t>(pa->data[0]), 0x60u, "the later pack's picture (R of 0x40405060)");
                t.Equals(static_cast<uint32_t>(pa->data[3]), 0x40u, "its alpha as the PS2's");
                // level 1 is 4 x 4, the file 8 x 8: made from level 0 instead.
                t.Equals(static_cast<uint32_t>(pa->data[pa->levelOffset(1)]), 0x60u, "level 1 made from level 0");
            }
            // A pack inside a zip mod, read in place.
            {
                std::ifstream in(dumps.path / tp::fileName(b), std::ios::binary);
                const std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const std::filesystem::path zip = low.path / "hd.zip";
                // (with its manifest beside the Textures folder: a zip whose files all lie in one folder is
                // read from inside it)
                t.IsTrue(test_zip::write(zip, {{"mod.ini", "[mod]\nid = hd\n"}, {"Textures/ai/walls/" + tp::fileName(c), png}}),
                         "zip written");
                tp::setFolders({zip / "Textures" / "ai"});
                t.Equals(tp::stats().names, size_t{1}, "the zip's picture indexed");
                tp::find(c, &known);
                tp::waitIdle();
                auto pz = tp::find(c, &known);
                t.IsTrue(pz && pz->width == 8u && pz->data[0] == 0x60u, "read from the zip");
            }
            const uint64_t g = tp::generation();
            tp::setBudget(1u << 20);
            tp::setFolders({});
            t.IsFalse(tp::active(), "no packs, no dump: off");
            t.IsTrue(tp::generation() != g, "a new generation");
            t.IsTrue(tp::find(a) == nullptr, "nothing found");
            tp::setBudget(tp::defaultBudget());
            tp::setPreload(true);
        });
    });
}
