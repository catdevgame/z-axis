#include "MiniTest.h"
#include "runtime/ps2_texture_replace.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace tr = zx::texture_replace;

    tr::PicturePtr picture(uint32_t w, uint32_t h, uint32_t mips, const std::string &name, uint8_t fill = 0)
    {
        auto p = std::make_shared<tr::Picture>();
        p->width = w;
        p->height = h;
        p->mips = mips;
        p->name = name;
        p->data.assign(p->levelOffset(mips), fill);
        return p;
    }

    void put16(std::vector<uint8_t> &b, size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
    void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
    void put64(std::vector<uint8_t> &b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

    // A pack with one entry: TW x TH texture `hash` -> a w x h BC2 picture with `mips` levels.
    std::vector<uint8_t> pack(uint16_t tw, uint16_t th, uint64_t hash, uint16_t w, uint16_t h, uint8_t mips,
                              uint32_t dataSize, const char *name)
    {
        std::vector<uint8_t> b(16 + 64, 0);
        std::memcpy(b.data(), "PXTP", 4);
        put32(b, 4, 1);
        put32(b, 8, 1);
        put16(b, 16, tw);
        put16(b, 18, th);
        b[20] = 8;
        put64(b, 24, hash);
        put16(b, 32, w);
        put16(b, 34, h);
        b[36] = mips;
        b[37] = 1; // BC2
        put32(b, 40, 80);
        put32(b, 44, dataSize);
        std::strncpy(reinterpret_cast<char *>(b.data() + 48), name, 31);
        b.resize(80 + dataSize, 0xAB);
        return b;
    }
}

void register_ps2_texture_replace_tests()
{
    MiniTest::Case("TextureReplace", [](TestCase &tc)
    {
        tc.Run("index hash is FNV-1a 64", [](TestCase &t)
        {
            t.IsTrue(tr::indexHash(nullptr, 0) == 0xCBF29CE484222325ull, "empty");
            const uint8_t a = 'a';
            t.IsTrue(tr::indexHash(&a, 1) == 0xAF63DC4C8601EC8Cull, "\"a\"");
            const uint32_t texels[4] = {0x80000061u, 0xFF000000u, 0x12345678u, 0x9ABCDEF0u};
            const uint8_t rgb[6] = {0x61, 0, 0, 0, 0, 0};
            t.IsTrue(tr::rgbHash(texels, 2, 2) == tr::indexHash(rgb, 6), "rgb: R, G, B of all rows but the last");
            t.IsTrue(tr::rgbHash(texels, 2, 1) == tr::indexHash(nullptr, 0), "one row: nothing hashed");
        });

        tc.Run("index hash of the texels the game uploads", [](TestCase &t)
        {
            const uint8_t t8[4] = {1, 2, 3, 4};
            t.Equals(tr::indexHashOfUpload(t8, 2, 2, 0x13u), tr::indexHash(t8, 4), "T8: a byte a texel");
            // T4: two texels a byte, the first in the low nibble (the GS's order).
            const uint8_t t4[2] = {0x21, 0x43};
            t.Equals(tr::indexHashOfUpload(t4, 2, 2, 0x14u), tr::indexHash(t8, 4), "T4: low nibble first");
            t.Equals(tr::indexHashOfUpload(t8, 2, 2, 0x00u), uint64_t(0), "not indexed: none");
        });

        tc.Run("BC2 levels", [](TestCase &t)
        {
            tr::PicturePtr p = picture(256, 64, 9, "p");
            t.IsTrue(p->levelSize(0) == 64u * 16u * 16u, "mip 0: 64 x 16 blocks");
            t.IsTrue(p->levelWidth(8) == 1u && p->levelHeight(8) == 1u && p->levelSize(8) == 16u, "1 x 1 is one block");
            t.IsTrue(p->levelSize(7) == 16u, "2 x 1 is one block");
            t.IsTrue(p->valid(), "complete chain");
            auto bad = std::make_shared<tr::Picture>(*p);
            bad->data.resize(bad->data.size() - 1);
            t.IsFalse(bad->valid(), "short data");
            bad = std::make_shared<tr::Picture>(*p);
            bad->mips = 10;
            t.IsFalse(bad->valid(), "more levels than the size has");
        });

        tc.Run("sources: newest wins, removal, switch, budget", [](TestCase &t)
        {
            tr::clear();
            tr::setEnabled(true);
            tr::setBudget(size_t(512) << 20);
            const uint64_t g0 = tr::generation();
            tr::setSource("LEVEL_A", {{64, 64, 1, picture(256, 256, 1, "a1")}, {32, 32, 2, picture(128, 128, 1, "a2")}});
            t.IsTrue(tr::generation() != g0, "generation changes");
            t.IsTrue(tr::find(64, 64, 1) && tr::find(64, 64, 1)->name == "a1", "found");
            t.IsFalse(static_cast<bool>(tr::find(64, 32, 1)), "the size is part of the key");
            tr::setSource("LEVEL_B", {{64, 64, 1, picture(256, 256, 1, "b1")}});
            t.IsTrue(tr::find(64, 64, 1)->name == "b1", "the file loaded last wins");
            t.IsTrue(tr::find(32, 32, 2)->name == "a2", "others stay");
            tr::setSource("LEVEL_A", {{64, 64, 1, picture(256, 256, 1, "a1")}});
            t.IsTrue(tr::find(64, 64, 1)->name == "a1", "set again: newest again");
            t.IsFalse(static_cast<bool>(tr::find(32, 32, 2)), "a source's old entries are replaced");
            tr::removeSource("LEVEL_A");
            t.IsTrue(tr::find(64, 64, 1)->name == "b1", "removed: the other one shows");
            tr::setEnabled(false);
            t.IsFalse(static_cast<bool>(tr::find(64, 64, 1)), "off");
            tr::setEnabled(true);
            t.IsTrue(static_cast<bool>(tr::find(64, 64, 1)), "on again");

            tr::clear();
            tr::setBudget(90000);
            tr::setSource("ONE", {{8, 8, 1, picture(128, 128, 1, "one")}});   // 16 KB
            tr::setSource("TWO", {{8, 8, 2, picture(256, 256, 1, "two")}});   // 64 KB
            tr::setSource("THREE", {{8, 8, 3, picture(128, 128, 1, "three")}}); // 16 KB: over
            t.IsFalse(tr::hasSource("ONE"), "oldest dropped over the budget");
            t.IsTrue(tr::hasSource("TWO") && tr::hasSource("THREE"), "newer kept");
            t.IsTrue(tr::stats().bytes == 81920u, "bytes held");
            tr::clear();
            tr::setBudget(size_t(512) << 20);
        });

        tc.Run("pack files", [](TestCase &t)
        {
            std::vector<tr::Entry> out;
            std::string error;
            size_t skipped = 9;
            // 8 x 8 BC2 with 4 levels: 64 + 16 + 16 + 16 bytes.
            t.IsTrue(tr::parsePack(pack(16, 16, 0x1234, 8, 8, 4, 112, "WALL"), out, &error, &skipped), "parsed");
            t.IsTrue(out.size() == 1u && skipped == 0u, "one entry");
            t.IsTrue(out[0].width == 16u && out[0].height == 16u && out[0].indexHash == 0x1234u, "key");
            t.IsTrue(out[0].picture->width == 8u && out[0].picture->mips == 4u && out[0].picture->name == "WALL",
                     "picture");
            t.IsTrue(out[0].picture->data.size() == 112u && out[0].picture->data[0] == 0xAB, "data");
            {
                // Two entries on the same data (a texture and its PS2 mip level): one picture.
                std::vector<uint8_t> two = pack(16, 16, 0x1234, 8, 8, 4, 112, "WALL");
                std::vector<uint8_t> second(two.begin() + 16, two.begin() + 80);
                put16(second, 0, 8);
                put16(second, 2, 8);
                put64(second, 8, 0x5678);
                two.insert(two.begin() + 80, second.begin(), second.end());
                put32(two, 8, 2);
                put32(two, 40, 144);
                put32(two, 104, 144);
                std::vector<tr::Entry> both;
                t.IsTrue(tr::parsePack(two, both, &error, &skipped) && both.size() == 2u, "two entries");
                t.IsTrue(both.size() == 2u && both[0].picture == both[1].picture && both[1].width == 8u,
                         "shared picture, own key");
            }
            out.clear();
            t.IsTrue(tr::parsePack(pack(16, 16, 1, 8, 8, 4, 100, "SHORT"), out, &error, &skipped), "file is fine");
            t.IsTrue(out.empty() && skipped == 1u, "a picture without all its levels is skipped");
            std::vector<uint8_t> bad = pack(16, 16, 1, 8, 8, 1, 64, "X");
            put32(bad, 44, 4096);
            out.clear();
            t.IsTrue(tr::parsePack(bad, out, &error, &skipped) && out.empty() && skipped == 1u, "data past the end skipped");
            t.IsFalse(tr::parsePack(std::vector<uint8_t>(8, 0), out, &error), "too short");
            std::vector<uint8_t> version = pack(16, 16, 1, 8, 8, 1, 64, "X");
            put32(version, 4, 2);
            t.IsFalse(tr::parsePack(version, out, &error), "unknown version");
            std::vector<uint8_t> count = pack(16, 16, 1, 8, 8, 1, 64, "X");
            put32(count, 8, 1000);
            t.IsFalse(tr::parsePack(count, out, &error), "entry table past the end");
        });
    });
}
