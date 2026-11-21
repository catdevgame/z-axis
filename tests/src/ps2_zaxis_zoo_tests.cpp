#include "MiniTest.h"
#include "runtime/ps2_texture_replace.h"
#include "runtime/ps2_zaxis_zoo.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace zoo = zx::zaxis::zoo;
namespace tr = zx::texture_replace;

namespace
{
    void put32(std::vector<uint8_t> &out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void putF(std::vector<uint8_t> &out, float f)
    {
        uint32_t v = 0;
        std::memcpy(&v, &f, 4);
        put32(out, v);
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

    std::vector<uint8_t> form(const std::vector<std::vector<uint8_t>> &chunks)
    {
        std::vector<uint8_t> payload;
        for (const auto &c : chunks)
            payload.insert(payload.end(), c.begin(), c.end());
        return chunk("FORM", payload);
    }

    // A data point: the 0x2C-byte header (class at +4, name at +8, id,
    // position) and the class's fields (an NPC's type at +0x2C).
    std::vector<uint8_t> point(uint16_t kind, uint32_t name, uint32_t id, float x, float y, float z,
                               std::vector<uint8_t> fields = {0, 0, 0, 0})
    {
        std::vector<uint8_t> p;
        put32(p, 0x2Cu);
        put32(p, kind);
        put32(p, name);
        put32(p, id);
        putF(p, x);
        putF(p, y);
        putF(p, z);
        p.resize(0x2C, 0u);
        p.insert(p.end(), fields.begin(), fields.end());
        return chunk("SDPT", p);
    }

    // A geometry object: TGEO > TGHD (name at +0x1C of 0x40 bytes) and a
    // material using a texture.
    std::vector<uint8_t> object(const std::string &name, uint32_t texture)
    {
        std::vector<uint8_t> head(0x40, 0u);
        std::memcpy(&head[0x1C], name.data(), name.size());
        std::vector<uint8_t> mati(0x30 + 28, 0u);
        const uint32_t layers = 1u;
        std::memcpy(&mati[0x2C], &layers, 4);
        std::memcpy(&mati[0x30 + 0x10], &texture, 4);
        std::vector<uint8_t> body = chunk("TGHD", head);
        const std::vector<uint8_t> m = chunk("MATI", mati);
        body.insert(body.end(), m.begin(), m.end());
        return chunk("TGEO", body);
    }

    // A texture library entry: TXRC > TXRH (id first) and a picture.
    std::vector<uint8_t> texture(uint32_t id)
    {
        std::vector<uint8_t> head(52, 0u);
        std::memcpy(head.data(), &id, 4);
        std::vector<uint8_t> body = chunk("TXRH", head);
        const std::vector<uint8_t> bmp = chunk("TBMP", std::vector<uint8_t>(8, static_cast<uint8_t>(id)));
        body.insert(body.end(), bmp.begin(), bmp.end());
        return chunk("TXRC", body);
    }
}

void register_ps2_zaxis_zoo_tests()
{
    MiniTest::Case("ZAxisZoo", [](TestCase &tc)
    {
        tc.Run("texture packs written as parsePack reads them", [](TestCase &t)
        {
            auto picture = [](const std::string &name, uint8_t fill)
            {
                auto p = std::make_shared<tr::Picture>();
                p->width = 8;
                p->height = 8;
                p->mips = 4;
                p->format = tr::Format::BC2;
                p->name = name;
                p->data.assign(112, fill);
                return p;
            };
            std::vector<tr::Entry> entries;
            entries.push_back({16, 16, 0x1111, picture("WALL", 1)});
            entries.push_back({8, 8, 0x2222, entries[0].picture});           // the same picture, another key
            entries.push_back({16, 16, 0x3333, picture("WALL", 1)});        // alike: written once
            entries.push_back({16, 16, 0x4444, picture("FLOOR", 1)});       // same data, another name
            entries.push_back({32, 32, 0x5555, picture(std::string(40, 'N'), 2)});
            const std::vector<uint8_t> pack = tr::writePack(entries, 8u);
            t.Equals(pack.size(), size_t(16 + 64 * 5 + 112 * 3), "header, five entries, three pictures");
            std::vector<tr::Entry> back;
            std::string error;
            size_t skipped = 9;
            t.IsTrue(tr::parsePack(pack, back, &error, &skipped), "parsed: " + error);
            t.IsTrue(back.size() == 5u && skipped == 0u, "every entry");
            if (back.size() != 5u)
                return;
            for (size_t i = 0; i < 5; ++i)
                t.IsTrue(back[i].width == entries[i].width && back[i].height == entries[i].height &&
                             back[i].indexHash == entries[i].indexHash,
                         "key " + std::to_string(i));
            t.IsTrue(back[0].picture == back[1].picture && back[0].picture == back[2].picture, "one picture");
            t.IsTrue(back[3].picture != back[0].picture && back[3].picture->name == "FLOOR", "its own name");
            t.IsTrue(back[4].picture->name == std::string(32, 'N'), "a long name cut at 32");
            t.IsTrue(back[4].picture->data == entries[4].picture->data && back[4].picture->mips == 4u &&
                         back[4].picture->format == tr::Format::BC2,
                     "data, levels, format");
            t.Equals(static_cast<unsigned>(pack[16 + 4]), 8u, "8-bit indices");
        });

        tc.Run("split screen's data points", [](TestCase &t)
        {
            constexpr uint16_t spawn = 0x46BEu, npc = 0x5343u;
            const uint32_t playerOne = zoo::engineHash("playerone"), respawn = zoo::engineHash("respawn");
            const uint32_t willowType = zoo::engineHash("NPC_Willow");
            std::vector<uint8_t> type(4);
            std::memcpy(type.data(), &willowType, 4);
            const std::vector<uint8_t> level = form({
                point(spawn, playerOne, 0, -13.1f, -3.9f, 103.1f),
                point(spawn, respawn, 0, 0.0f, -4.0f, 50.0f),      // the free skate start
                point(spawn, respawn, 1, 162.3f, -8.0f, -3.0f),    // over the east boulevard: an egg
                point(spawn, respawn, 2, -10.0f, -8.0f, 100.0f),   // by a start: none
                point(spawn, respawn, 3, 500.0f, -8.0f, 500.0f),   // no ground known: none
                point(npc, zoo::engineHash("willow"), 0, 1.0f, 2.0f, 3.0f, type),
                point(npc, zoo::engineHash("willow"), 1, 7.0f, 2.0f, 3.0f, type),
                point(npc, zoo::engineHash("willow_a"), 0, 1.0f, 2.0f, 3.0f, type), // sways over the first
                chunk("ZPVS", std::vector<uint8_t>(16, 0xEE)),
            });
            zoo::SplitScreenDataReport report;
            const std::vector<uint8_t> dat = zoo::splitScreenData(level, zoo::Options{}, &report);
            t.IsTrue(report.playerTwo && report.eggs == 1u && report.twins == 1u, "player two, one egg, one twin");
            t.Equals(static_cast<size_t>(get32(dat, 4)) + 8u, dat.size(), "FORM size");
            // Points in order; the new ones after the last point, before ZPVS.
            std::vector<size_t> at;
            for (size_t p = 8; p + 8 <= dat.size(); p += 8 + get32(dat, p + 4))
                at.push_back(p);
            t.Equals(at.size(), size_t(11), "nine chunks and two points");
            if (at.size() != 11u)
                return;
            t.IsTrue(std::memcmp(&dat[at[8]], "SDPT", 4) == 0 && get32(dat, at[8] + 16) == zoo::engineHash("playertwo"),
                     "player two");
            float p2[3];
            std::memcpy(p2, &dat[at[8] + 8 + 0x10], 12);
            t.IsTrue(p2[0] == 0.0f && p2[2] == 50.0f, "at the free skate start");
            t.IsTrue(get32(dat, at[9] + 12) == 0x5362u && dat[at[9] + 8 + 0x44] == 2u, "an egg spot (bS, type 2)");
            float egg[3];
            std::memcpy(egg, &dat[at[9] + 8 + 0x10], 12);
            t.IsTrue(egg[0] == 162.3f && egg[1] == 4.61f && egg[2] == -3.0f, "on the ground below the respawn point");
            t.IsTrue(std::memcmp(&dat[at[10]], "ZPVS", 4) == 0, "the rest after");
            t.Equals(get32(dat, at[5] + 12) & 0xFFFFu, 0u, "the static willow under the swaying one has no object");
            t.Equals(get32(dat, at[6] + 12) & 0xFFFFu, 0x5343u, "the other static willow kept");
            t.Equals(get32(dat, at[7] + 12) & 0xFFFFu, 0x5343u, "the swaying one kept");

            zoo::Options developer;
            developer.keepWillowTwins = true;
            developer.splitScreenEggs = "1,2,3;4,5,6";
            const std::vector<uint8_t> dev = zoo::splitScreenData(level, developer, &report);
            t.IsTrue(report.eggs == 2u && report.twins == 0u, "developer eggs and twins");
        });

        tc.Run("split screen's script", [](TestCase &t)
        {
            std::vector<uint8_t> bin;
            putF(bin, 2.7f);
            put32(bin, 0);
            put32(bin, 0);
            put32(bin, 0);
            put32(bin, 8); // strings
            bin.insert(bin.end(), {'c', 'a', 'm', 0, 0x1E, 0, 0, 0});
            put32(bin, 0x0Au);
            put32(bin, zoo::engineHash("playCameraAnim")); // pushed, not called
            put32(bin, 0x1Eu);
            put32(bin, zoo::engineHash("playCameraAnim"));
            put32(bin, 0x1Eu);
            put32(bin, zoo::engineHash("boxEnter"));
            std::vector<uint8_t> script = bin;
            t.Equals(zoo::splitScreenScript(script), size_t(1), "one call");
            t.Equals(get32(script, 0x14 + 8 + 12), zoo::engineHash("getTheVisibleTimer"), "made harmless");
            t.Equals(get32(script, 0x14 + 8 + 4), zoo::engineHash("playCameraAnim"), "a push left alone");
            t.Equals(get32(script, 0x14 + 8 + 20), zoo::engineHash("boxEnter"), "other calls left alone");
        });

        tc.Run("split screen's props", [](TestCase &t)
        {
            const std::vector<uint8_t> level = form({object("Bench", 3), object("PropDef_Balloon", 4)});
            const std::vector<uint8_t> donorGeo =
                form({object("Tree", 9), object("PropDef_Balloon", 5), object("PropDef_EggBig", 7)});
            const std::vector<uint8_t> donorTxr = form({texture(9), texture(7), texture(5)});
            std::vector<uint8_t> geo = level;
            t.Equals(zoo::addSplitScreenProps(geo, true, level, donorGeo, nullptr), size_t(1), "one prop");
            t.Equals(static_cast<size_t>(get32(geo, 4)) + 8u, geo.size(), "FORM size");
            const std::string g(geo.begin(), geo.end());
            t.IsTrue(g.find("PropDef_EggBig") != std::string::npos && g.find("Tree") == std::string::npos,
                     "the egg, not the donor's own objects");
            std::vector<uint8_t> txr = form({texture(3), texture(4)});
            t.Equals(zoo::addSplitScreenProps(txr, false, level, donorGeo, &donorTxr), size_t(1), "its texture");
            t.Equals(txr.size(), form({texture(3), texture(4), texture(7)}).size(), "texture 7 added");
            t.IsTrue(txr == form({texture(3), texture(4), texture(7)}), "after the level's");
        });

        tc.Run("objects hidden by name", [](TestCase &t)
        {
            // hideObjects reads the name at TGHD +0x24 (a developer switch).
            std::vector<uint8_t> head(0x40, 0u);
            std::memcpy(&head[0x24], "Rock", 4);
            std::vector<uint8_t> tgph(16, 0u);
            const uint32_t groups = 5;
            std::memcpy(&tgph[8], &groups, 4);
            std::vector<uint8_t> body = chunk("TGHD", head);
            const std::vector<uint8_t> part = chunk("TGPT", chunk("TGPH", tgph));
            body.insert(body.end(), part.begin(), part.end());
            std::vector<uint8_t> geo = form({chunk("TGEO", body)});
            t.Equals(zoo::hideObjects(geo, "Tree,Rock"), size_t(1), "one part");
            const size_t ph = std::string(geo.begin(), geo.end()).find("TGPH");
            t.Equals(get32(geo, ph + 8 + 8), 0u, "no groups");
        });

        tc.Run("the engine's name hash", [](TestCase &t)
        {
            t.Equals(zoo::engineHash("NPC_KelpA"), 0x093FD201u, "the Xbox table's kelp type");
            t.Equals(zoo::engineHash("npc_willow"), 0x04B945D7u, "case-insensitive");
        });
    });
}
