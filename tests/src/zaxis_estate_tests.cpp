#include "MiniTest.h"
#include "runtime/texture_replace.h"
#include "runtime/zaxis_estate.h"
#include "runtime/zaxis_ui.h"

#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace estate = zx::zaxis::estate;

namespace
{
    void put32(std::vector<uint8_t> &out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void put16(std::vector<uint8_t> &out, uint16_t v)
    {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    }
    uint32_t get32(const std::vector<uint8_t> &d, size_t at)
    {
        uint32_t v = 0;
        std::memcpy(&v, &d[at], 4);
        return v;
    }
    std::vector<uint8_t> chunk(const char *tag, const std::vector<uint8_t> &payload)
    {
        std::vector<uint8_t> out(tag, tag + 4);
        put32(out, static_cast<uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    // A BMP of w x h (bottom-up), bpp 4/8 (palette of 2^bpp grey entries)
    // or 24 (pixels as given, BGR), rows padded to 4 bytes.
    std::vector<uint8_t> bmp(int w, int h, int bpp, const std::vector<uint8_t> &rowsBottomUp)
    {
        const uint32_t colours = bpp <= 8 ? (1u << bpp) : 0u;
        const uint32_t offset = 14u + 40u + colours * 4u;
        const uint32_t stride = ((static_cast<uint32_t>(w) * bpp + 31u) / 32u) * 4u;
        std::vector<uint8_t> out = {'B', 'M'};
        put32(out, offset + stride * h);
        put32(out, 0);
        put32(out, offset);
        put32(out, 40);
        put32(out, static_cast<uint32_t>(w));
        put32(out, static_cast<uint32_t>(h));
        put16(out, 1);
        put16(out, static_cast<uint16_t>(bpp));
        put32(out, 0);
        put32(out, stride * h);
        put32(out, 0xB12);
        put32(out, 0xB12);
        put32(out, colours);
        put32(out, 0);
        for (uint32_t i = 0; i < colours; ++i)
        {
            const uint8_t g = static_cast<uint8_t>(i * 255u / (colours - 1u));
            out.insert(out.end(), {g, g, g, 0});
        }
        out.insert(out.end(), rowsBottomUp.begin(), rowsBottomUp.end());
        while (out.size() % 4u)
            out.push_back(0);
        return out;
    }

    std::vector<uint8_t> header(const char *name, uint16_t w, uint16_t h)
    {
        std::vector<uint8_t> out;
        put32(out, 0x12345678u);
        put16(out, w);
        put16(out, h);
        out.resize(0x14, 0);
        const size_t at = out.size();
        out.resize(52, 0);
        std::memcpy(&out[at], name, std::strlen(name));
        return out;
    }

    std::vector<uint8_t> library(const std::vector<std::vector<uint8_t>> &entries)
    {
        std::vector<uint8_t> body;
        for (const auto &e : entries)
            body.insert(body.end(), e.begin(), e.end());
        return chunk("FORM", body);
    }

    std::vector<uint8_t> entry(const char *name, uint16_t w, uint16_t h, const std::vector<uint8_t> &picture,
                               const char *tag = "TBMP")
    {
        std::vector<uint8_t> inner = chunk("TXRH", header(name, w, h));
        const std::vector<uint8_t> pic = chunk(tag, picture);
        inner.insert(inner.end(), pic.begin(), pic.end());
        return chunk("TXRC", inner);
    }

    void putf(std::vector<uint8_t> &out, float f)
    {
        uint32_t v = 0;
        std::memcpy(&v, &f, 4);
        put32(out, v);
    }

    // A one-object GEO: a quad of half size `half` at height y (+y down),
    // its material's texture hash 0x12345678 (header()'s), prelit 1.0.
    std::vector<uint8_t> quadObject(uint32_t id, float half, float y)
    {
        std::vector<uint8_t> tghd(0x40, 0u);
        std::memcpy(&tghd[0x18], &id, 4);
        std::vector<uint8_t> tgph;
        put16(tgph, 1u); // rigid
        put16(tgph, 0u);
        put16(tgph, 4u); // vertices
        put16(tgph, 2u);
        put16(tgph, 1u);
        tgph.resize(20u, 0u);
        std::vector<uint8_t> tgvp, tgvu, tgvc, tgfm;
        const float corners[4][2] = {{-half, -half}, {half, -half}, {-half, half}, {half, half}};
        for (const auto &c : corners)
        {
            putf(tgvp, c[0]);
            putf(tgvp, y);
            putf(tgvp, c[1]);
            putf(tgvu, 0.25f);
            putf(tgvu, 0.25f);
            tgvc.insert(tgvc.end(), {128, 128, 128, 128});
        }
        tgfm.insert(tgfm.end(), {0, 0}); // a strip, material 0
        put16(tgfm, 4u);
        for (uint16_t e : {0x8000u, 0x8001u, 2u, 3u})
            put16(tgfm, e);
        std::vector<uint8_t> part = chunk("TGPH", tgph);
        for (const auto &c : {chunk("TGVP", tgvp), chunk("TGVU", tgvu), chunk("TGVC", tgvc), chunk("TGFM", tgfm)})
            part.insert(part.end(), c.begin(), c.end());
        std::vector<uint8_t> mati(160u, 0u);
        const uint32_t layers = 1u, texture = 0x12345678u;
        std::memcpy(&mati[0x2C], &layers, 4);
        std::memcpy(&mati[0x40], &texture, 4);
        std::vector<uint8_t> object = chunk("TGHD", tghd);
        for (const auto &c : {chunk("TGPT", part), chunk("TMTB", chunk("MATI", mati))})
            object.insert(object.end(), c.begin(), c.end());
        return chunk("TGEO", object);
    }

    std::vector<uint8_t> instance(const char *name, uint32_t object, uint32_t id)
    {
        std::vector<uint8_t> tgih(0x70u, 0u);
        const uint32_t flags = 0x61u;
        const float w = 1.0f;
        std::memcpy(&tgih[0], &flags, 4);
        std::memcpy(&tgih[0x14], &w, 4); // quaternion (0, 0, 0, 1)
        std::memcpy(&tgih[0x40], &object, 4);
        std::memcpy(&tgih[0x48], &id, 4);
        std::memcpy(&tgih[0x50], name, std::strlen(name));
        return chunk("TGIN", chunk("TGIH", tgih));
    }

    // A level script with one object and state whose "enter" makes the
    // given setObjectVisible(target, visible, keep 1) calls.
    std::vector<uint8_t> script(uint32_t object, uint32_t state, const std::vector<std::pair<uint32_t, uint32_t>> &calls)
    {
        std::vector<uint32_t> code = {object, 4u, 0xFFFFFFFFu, 0u, 0u, 0u, state, 6u, 0xFFFFFFFFu, 0u,
                                      0x004A38A2u, 4u, 0xFFFFFFFFu, 0u};
        for (const auto &[target, visible] : calls)
            code.insert(code.end(), {0x0Au, 1u, 0x0Au, visible, 0x0Au, target, 0x1Eu, 0x098C3505u});
        code.insert(code.end(), {0x1F40u, 0u});
        std::vector<uint8_t> out;
        put32(out, 0x402CCCCDu); // 2.7
        put32(out, static_cast<uint32_t>(code.size()));
        put32(out, 2u);
        put32(out, 4u);
        put32(out, 8u);
        out.insert(out.end(), 8u + 4u + 8u, 0u); // strings, data, variables
        for (uint32_t w : code)
            put32(out, w);
        return out;
    }
}

void register_zaxis_estate_tests()
{
    MiniTest::Case("ZAxisEstate", [](TestCase &tc)
    {
        tc.Run("a 24-bit picture made 8-bit, the rest byte for byte", [](TestCase &t)
        {
            // 8-bit 4x2 (rows bottom-up: 4 5 6 7 then 0 1 2 3), 24-bit 2x2
            // of three colours, 4-bit 4x2.
            const std::vector<uint8_t> e8 = entry("EIGHT", 4, 2, bmp(4, 2, 8, {4, 5, 6, 7, 0, 1, 2, 3}));
            const std::vector<uint8_t> rgb = {0, 0, 255, 0, 255, 0, 0, 0,     // bottom: red, green (BGR), pad
                                              255, 0, 0, 0, 0, 255, 0, 0};    // top: blue, red, pad
            const std::vector<uint8_t> e24 = entry("TRUE", 2, 2, bmp(2, 2, 24, rgb));
            const std::vector<uint8_t> e4 = entry("FOUR", 4, 2, bmp(4, 2, 4, {0x12, 0x34, 0, 0, 0x56, 0x78, 0, 0}));
            std::vector<uint8_t> txr = library({e8, e24, e4});
            estate::TextureReport report;
            std::string error;
            t.IsTrue(estate::palettizeLibrary(txr, &report, &error), "palettized: " + error);
            t.Equals(report.textures, size_t(3), "three textures");
            t.Equals(report.pictures, size_t(3), "three pictures");
            t.Equals(report.converted, size_t(1), "one made 8-bit");
            t.Equals(get32(txr, 4), static_cast<uint32_t>(txr.size() - 8u), "the FORM's size");
            // The first and last entries as they were.
            t.IsTrue(std::equal(e8.begin(), e8.end(), txr.begin() + 8), "the 8-bit entry unchanged");
            t.IsTrue(std::equal(e4.rbegin(), e4.rend(), txr.rbegin()), "the 4-bit entry unchanged");
            // The converted one: TXRC, TXRH as it was, then an 8-bit TBMP.
            const size_t at = 8u + e8.size();
            t.IsTrue(std::memcmp(&txr[at], "TXRC", 4) == 0, "TXRC");
            const uint32_t entrySize = get32(txr, at + 4u);
            t.Equals(static_cast<size_t>(at + 8u + entrySize + e4.size()), txr.size(), "the TXRC's size");
            t.IsTrue(std::equal(e24.begin() + 8, e24.begin() + 8 + 60, txr.begin() + static_cast<std::ptrdiff_t>(at + 8u)),
                     "the TXRH as it was");
            const size_t picture = at + 8u + 60u;
            t.IsTrue(std::memcmp(&txr[picture], "TBMP", 4) == 0, "still a TBMP");
            const size_t b = picture + 8u;
            t.Equals(static_cast<int>(txr[b + 28]), 8, "8 bits per pixel");
            // Each pixel's palette colour is its colour (three colours fit).
            auto colour = [&](int x, int yTop)
            {
                const uint32_t offset = get32(txr, b + 10);
                const int row = 1 - yTop; // bottom-up
                const uint8_t index = txr[b + offset + static_cast<size_t>(row) * 4u + static_cast<size_t>(x)];
                const size_t pal = b + 54u + index * 4u;
                return std::vector<int>{txr[pal + 2], txr[pal + 1], txr[pal], txr[pal + 3]};
            };
            t.IsTrue(colour(0, 1) == std::vector<int>{255, 0, 0, 0}, "bottom left red, palette alpha 0");
            t.IsTrue(colour(1, 1) == std::vector<int>{0, 255, 0, 0}, "bottom right green");
            t.IsTrue(colour(0, 0) == std::vector<int>{0, 0, 255, 0}, "top left blue");
            t.IsTrue(colour(1, 0) == std::vector<int>{255, 0, 0, 0}, "top right red");
            // A second run changes nothing.
            std::vector<uint8_t> again = txr;
            t.IsTrue(estate::palettizeLibrary(again, &report, &error) && again == txr && report.converted == 0u,
                     "nothing left to convert");
        });

        tc.Run("texture keys: level 0's indices top down, as the GS holds them", [](TestCase &t)
        {
            const std::vector<uint8_t> e8 = entry("EIGHT", 4, 2, bmp(4, 2, 8, {4, 5, 6, 7, 0, 1, 2, 3}));
            const std::vector<uint8_t> e4 = entry("FOUR", 4, 2, bmp(4, 2, 4, {0x12, 0x34, 0, 0, 0x56, 0x78, 0, 0}));
            const std::vector<estate::TextureKey> keys = estate::textureKeys(library({e8, e4}));
            t.Equals(keys.size(), size_t(2), "two keys");
            if (keys.size() != 2u)
                return;
            const uint8_t top8[] = {0, 1, 2, 3, 4, 5, 6, 7};
            const uint8_t top4[] = {5, 6, 7, 8, 1, 2, 3, 4}; // high nibble first, rows reversed
            t.Equals(keys[0].indexHash, zx::texture_replace::indexHash(top8, 8), "8-bit");
            t.Equals(keys[1].indexHash, zx::texture_replace::indexHash(top4, 8), "4-bit");
            t.IsTrue(keys[0].width == 4u && keys[0].height == 2u && keys[0].name == "EIGHT", "size and name");
            const std::vector<estate::TextureKey> back = estate::readTextureKeys(estate::writeTextureKeys(keys));
            t.IsTrue(back.size() == 2u && back[1].indexHash == keys[1].indexHash && back[1].name == "FOUR" &&
                         back[0].width == 4u,
                     "written and read back");
        });

        tc.Run("visibility rules: the career script's show and hide calls the free skate one lacks", [](TestCase &t)
        {
            const std::vector<uint8_t> career = script(0x111u, 0x222u, {{0x333u, 0u}, {0x444u, 0u}});
            const std::vector<uint8_t> freeSkate = script(0x111u, 0x222u, {{0x444u, 1u}});
            const std::vector<estate::VisibilityRule> rules = estate::borrowedVisibility(career, freeSkate);
            t.Equals(rules.size(), size_t(1), "one call: the target the free skate script never shows or hides");
            if (rules.size() == 1u)
                t.IsTrue(rules[0].object == 0x111u && rules[0].state == 0x222u && rules[0].target == 0x333u &&
                             !rules[0].visible && rules[0].keep,
                         "object, state, target, hide, keep");
            const std::vector<uint8_t> otherState = script(0x111u, 0x555u, {});
            t.IsTrue(estate::borrowedVisibility(career, otherState).empty(), "none for a state the script has not");
            const std::vector<estate::VisibilityRule> back =
                estate::readVisibilityRules(estate::writeVisibilityRules(rules));
            t.IsTrue(back.size() == 1u && back[0].target == 0x333u && !back[0].visible, "written and read back");
        });

        tc.Run("renderPicture: the level as it starts, framed on its grounds", [](TestCase &t)
        {
            // A grey ground (texel 128, prelit 1.0) and a lesson prop above it
            // (hidden at the start: not drawn).
            std::vector<uint8_t> objects = quadObject(1u, 10.0f, 0.0f), prop = quadObject(2u, 2.0f, -3.0f);
            objects.insert(objects.end(), prop.begin(), prop.end());
            const std::vector<uint8_t> geo = chunk("FORM", objects);
            std::vector<uint8_t> placed = instance("Terrain_Ground_1", 1u, 1u), propPlaced = instance("ScriptObj_box_L5", 2u, 2u);
            placed.insert(placed.end(), propPlaced.begin(), propPlaced.end());
            const std::vector<uint8_t> ins = chunk("FORM", placed);
            const std::vector<uint8_t> txr =
                library({entry("GREY", 4, 4, bmp(4, 4, 8, std::vector<uint8_t>(16, 128)))});
            estate::Picture picture;
            estate::PictureReport report;
            std::string error;
            t.IsTrue(estate::renderPicture(geo, ins, txr, 32, picture, &report, &error), "drawn: " + error);
            t.Equals(picture.width, 32, "32 wide");
            t.Equals(report.instances, size_t(1), "the lesson prop left out");
            t.Equals(report.texturedTriangles, size_t(2), "the ground's two triangles textured");
            if (picture.rgba.size() == 32u * 32u)
            {
                const uint32_t centre = picture.rgba[16u * 32u + 16u], corner = picture.rgba[0];
                t.Equals(centre & 0xFFFFFFu, 0x808080u, "the ground's grey in the middle");
                t.IsTrue((corner & 0xFFFFFFu) != 0x808080u, "the sky in a corner");
            }
            t.IsFalse(estate::renderPicture({1, 2, 3, 4}, ins, txr, 32, picture, nullptr, &error), "no GEO: no picture");
        });

        tc.Run("iconFont: the level font's format, two glyphs on a 256 x 64 page", [](TestCase &t)
        {
            estate::Picture picture;
            picture.width = picture.height = 16;
            picture.rgba.assign(256u, 0xFF3060A0u);
            const std::vector<uint8_t> font = estate::iconFont(picture, 'n', 'N', "fnt_lvl_icons_estate.zmp");
            t.IsTrue(font.size() > 64u && std::memcmp(font.data(), "UFNT", 4) == 0, "a UFNT");
            t.Equals(static_cast<size_t>(get32(font, 4)) + 8u, font.size(), "its size");
            t.IsTrue(std::memcmp(&font[8], "HFNT", 4) == 0, "the HFNT first");
            const size_t hfnt = 16u;
            t.Equals(static_cast<int>(font[hfnt]), 11, "line height 11");
            t.Equals(static_cast<int>(font[hfnt + 2] | font[hfnt + 3] << 8), 2, "two glyphs");
            t.Equals(static_cast<int>(font[hfnt + 4] | font[hfnt + 5] << 8), 1, "one page");
            t.Equals(static_cast<int>(font[hfnt + 6] | font[hfnt + 7] << 8), 256, "256 wide");
            t.Equals(static_cast<int>(font[hfnt + 8] | font[hfnt + 9] << 8), 64, "64 high");
            const size_t glyphs = hfnt + 6u + 40u;
            t.IsTrue(font[glyphs] == 'n' && font[glyphs + 12] == 'N', "n and N");
            const std::string text(font.begin(), font.end());
            const size_t tzmp = text.find("TZMP");
            t.IsTrue(tzmp != std::string::npos && text.compare(tzmp + 8, 2, "BM") == 0, "the page a BMP");
            if (tzmp != std::string::npos)
            {
                t.Equals(get32(font, tzmp + 8 + 18), 256u, "page BMP 256 wide");
                t.Equals(static_cast<int>(font[tzmp + 8 + 28]), 8, "8-bit");
            }
            t.IsTrue(font == estate::iconFont(picture, 'n', 'N', "fnt_lvl_icons_estate.zmp"), "the same bytes again");
        });

        tc.Run("stillLibrary: a FrontEnd still asset addAssets takes", [](TestCase &t)
        {
            estate::Picture picture;
            picture.width = 64;
            picture.height = 32;
            for (int i = 0; i < 64 * 32; ++i)
                picture.rgba.push_back(0xFF000000u | static_cast<uint32_t>(i * 7));
            std::vector<uint8_t> library = estate::stillLibrary(picture, "still_bkg_01");
            const std::vector<uint8_t> still = estate::stillLibrary(picture, estate::kStillName);
            t.IsTrue(std::memcmp(still.data(), "ULIB", 4) == 0 && get32(still, 4) + 8u == still.size(), "a ULIB");
            const std::string text(still.begin(), still.end());
            t.IsTrue(text.find("estate_still.bmp") != std::string::npos && text.find("TBMP") != std::string::npos,
                     "its source and an 8-bit picture");
            const std::vector<std::string> added = zx::zaxis::addAssets(library, still, {estate::kStillName});
            t.IsTrue(added.size() == 1u && added[0] == estate::kStillName, "added to a library with still_bkg_01");
            t.IsTrue(zx::zaxis::addAssets(library, still, {estate::kStillName}).empty(), "not twice");
        });

        tc.Run("build: the files the game loads, or a reason", [](TestCase &t)
        {
            std::map<std::string, std::vector<uint8_t>> disc;
            std::vector<uint8_t> zit;
            for (const char *name : {"tut.TXR", "tut.GEO", "tut.INS", "tut.DAT"})
                zit.insert(zit.end(), name, name + std::strlen(name) + 1);
            disc["tut.ZIT"] = zit;
            disc["tut.TXR"] = library({entry("EIGHT", 4, 2, bmp(4, 2, 8, {4, 5, 6, 7, 0, 1, 2, 3}))});
            for (const char *name : {"tut.GEO", "tut.INS", "tut.DAT", "TUT.bin", "TUTT.bin", "motions.zal"})
                disc[name] = {1, 2, 3, 4};
            const estate::Reader reader = [&disc](const std::string &name, std::vector<uint8_t> &out)
            {
                auto it = disc.find(name);
                if (it == disc.end())
                    return false;
                out = it->second;
                return true;
            };
            estate::Made made;
            std::ostringstream log;
            std::string error;
            t.IsTrue(estate::build(reader, made, log, &error), "built: " + error);
            t.Equals(made.files.size(), size_t(10), "eight files, TEXTURES.TXT and VISIBILITY.TXT (no icon: no picture)");
            t.IsTrue(log.str().find("no icon") != std::string::npos, "the missing icon said");
            t.IsTrue(made.files.count("TUTT.BIN") && made.files["TUTT.BIN"] == disc["TUTT.bin"], "TUTT.BIN as it is");
            t.IsTrue(made.files["TUT.TXR"] == disc["tut.TXR"], "an 8-bit library as it is");
            t.IsTrue(!made.files["TEXTURES.TXT"].empty(), "the texture keys");
            disc.erase("TUTT.bin");
            t.IsFalse(estate::build(reader, made, log, &error), "a missing file");
            t.IsTrue(error.find("TUTT.bin") != std::string::npos, "named: " + error);
            disc["TUTT.bin"] = {1};
            disc["tut.ZIT"] = {'x', 0};
            t.IsFalse(estate::build(reader, made, log, &error), "a ZIT that names other files");
        });
    });
}
