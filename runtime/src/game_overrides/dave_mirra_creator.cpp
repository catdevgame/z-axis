// Dave Mirra Freestyle BMX 2 (SLUS-20159): Create Player, the menu the game
// has the code for but never shows, behind [extras] create_player
// (PS2X_DM2_CREATE_PLAYER=1; off by default).
//
// What the game has (game/dm2/menu/player_creator.cpp): the creator's start
// (bmxPlayerCreatorInitialize), which reads ASSETS\CREATEPL.DAT and
// registers three script functions (InitializePlayerCreatorMenu,
// HarvestPlayerCreatorMenu, ApplyPlayerCreatorTexturesToModel) on a root
// widget "PlayerCreator", and the created rider's model (newCreatedRiderModel)
// that newRider makes for characters 17 .. 20 once the creator is started.
// Nothing calls the start; no release's UILIB.ZAL has the PlayerCreator
// screen or the libraries its functions load, and CREATEPL.DAT names files
// (S:\bmx2\riders\createarider\CRIDER01-09.ZIT, about 150 BMPs) that never
// shipped. UILIB's MainMenu keeps an orphan action, OldCreatRider.
//
// What this supplies, from the game's own data:
// - the start, called where the front end registers its script functions
//   (introStateEnter, once);
// - CREATEPL.DAT for the parts below: one outfit, one tone, three sections
//   (head, body, legs) of three geometry options each, one texture sub-part
//   an option (FACE, SHIRT, PANTS), every rider's matching texture a choice;
// - UILIB's G<o><h><b><l>.ZIF (a model: the head's faces from one rider's
//   street model, the body's from another, the legs' from a third: the
//   riders share one skeleton and one texture layout, ID1 the head, ID2 the
//   shirt, gloves and arms, ID3 the trousers and shoes) and T<o><t><s><g>.ZIF
//   (a section option's choices, "T<o><t><s><g><part><choice>"), made from
//   RIDERS/*/PLAYER.ZIT and PLAYERC.ZIT when UILIB.ZAL is first read;
// - the PlayerCreator screen (creator.zsc, loaded with the main menu) in the
//   look of the Options frames, and CREATE RIDER on the main menu: four
//   slots (the created characters 17 .. 20), a row for each section and each
//   sub-part, RIDE (Free Ride with that rider, to the level select) and DONE.
//   The rows drive the game's own functions: InitializePlayerCreatorMenu
//   fills the screen's variables, HarvestPlayerCreatorMenu takes them back
//   into the character's record, ApplyPlayerCreatorTexturesToModel dresses
//   the model;
// - the libraries a created rider's model is made from kept loaded while the
//   rider lives (the game's maker lets them go as it returns; its textures
//   and geometry stay in them).
// The choices are the character's record (RiderCharacter +0xBE), so the game
// save keeps them (native saves).

#include "../game/ee/game_id.h"
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_creator.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zaxis_text.h"
#include "runtime/ps2_zaxis_ui.h"
#include "zge_host.h"
#include "zge_settings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <cmath>
#include <string>
#include <vector>

namespace game::readable
{
    bool runsInGame(uint32_t address, game::Game game);       // src/game/ee/readable.h
    bool switchedOnAtBoot(uint32_t address, game::Game game); // src/game/ee/readable.h
}

namespace dave_mirra::creator
{
    namespace
    {
        namespace zx = ps2x::zaxis;
        namespace gf = ps2x::game_files;

        constexpr int kRegA0 = 4, kRegA1 = 5, kRegSp = 29, kRegRa = 31;

        // Where a MATI names its texture (the texture's name hash).
        constexpr size_t kMaterialTexture = 0x3C;

        // ---- chunk files ------------------------------------------------------

        constexpr uint32_t fourcc(const char (&t)[5])
        {
            return uint32_t(uint8_t(t[0])) | uint32_t(uint8_t(t[1])) << 8 | uint32_t(uint8_t(t[2])) << 16 |
                   uint32_t(uint8_t(t[3])) << 24;
        }

        uint32_t u32(const std::vector<uint8_t> &d, size_t at)
        {
            uint32_t v = 0;
            std::memcpy(&v, &d[at], 4);
            return v;
        }
        uint16_t u16(const std::vector<uint8_t> &d, size_t at)
        {
            uint16_t v = 0;
            std::memcpy(&v, &d[at], 2);
            return v;
        }
        void put32(std::vector<uint8_t> &d, size_t at, uint32_t v) { std::memcpy(&d[at], &v, 4); }
        void put16(std::vector<uint8_t> &d, size_t at, uint16_t v) { std::memcpy(&d[at], &v, 2); }

        // A chunk: its tag and its body's place in the file.
        struct Chunk
        {
            uint32_t tag = 0;
            size_t body = 0, size = 0;
        };

        std::vector<Chunk> chunksIn(const std::vector<uint8_t> &d, size_t begin, size_t end)
        {
            std::vector<Chunk> out;
            while (begin + 8 <= end)
            {
                const Chunk c{u32(d, begin), begin + 8, u32(d, begin + 4)};
                if (c.body + c.size > end)
                    return {};
                out.push_back(c);
                begin = c.body + c.size;
            }
            return out;
        }

        const Chunk *firstOf(const std::vector<Chunk> &chunks, uint32_t tag)
        {
            for (const Chunk &c : chunks)
                if (c.tag == tag)
                    return &c;
            return nullptr;
        }

        // A chunk written: tag, size, body; the body padded to 4 bytes and
        // the size counting the padding (the engine's reader steps by the size).
        void writeChunk(std::vector<uint8_t> &out, uint32_t tag, const std::vector<uint8_t> &body)
        {
            const size_t at = out.size();
            const size_t padded = (body.size() + 3u) & ~size_t(3);
            out.resize(at + 8);
            put32(out, at, tag);
            put32(out, at + 4, static_cast<uint32_t>(padded));
            out.insert(out.end(), body.begin(), body.end());
            out.resize(at + 8 + padded, 0);
        }
        std::vector<uint8_t> bytesOf(const std::vector<uint8_t> &d, const Chunk &c)
        {
            return {d.begin() + static_cast<ptrdiff_t>(c.body), d.begin() + static_cast<ptrdiff_t>(c.body + c.size)};
        }
        // The whole chunk, header and body.
        std::vector<uint8_t> wholeOf(const std::vector<uint8_t> &d, const Chunk &c)
        {
            return {d.begin() + static_cast<ptrdiff_t>(c.body - 8), d.begin() + static_cast<ptrdiff_t>(c.body + c.size)};
        }

        uint32_t nameHash(const std::string &name) { return ps2x::zaxis_text::keyHash(name); }

        // ---- the riders' models ------------------------------------------------

        // A texture of a rider model: its TXRC chunk whole, and which of the
        // three the riders' layout makes it (1 head, 2 shirt, 3 trousers).
        struct RiderTexture
        {
            std::string name;
            uint32_t hash = 0;
            int id = 0;
            std::vector<uint8_t> txrc;
        };

        // A rider model's skinned part, its streams per vertex and its strip
        // groups per material; the rest of the model as it is.
        struct SkinnedPart
        {
            std::vector<uint8_t> header;              // TGPH
            std::vector<uint8_t> bones;               // MOBO
            std::vector<std::vector<uint8_t>> pivots; // TGPB, in order
            uint32_t vertices = 0, faces = 0;
            uint32_t normalStride = 0;
            std::vector<uint8_t> positions, normals, colours, uvs, weights; // TGVP TGVN TGVC TGVU TGVI
            std::vector<uint8_t> faceRecords, faceNormals;                  // TGFP (8 a face) TGFN (12)
            struct Group
            {
                uint8_t mode = 0, material = 0;
                std::vector<uint16_t> entries;
            };
            std::vector<Group> groups;
        };

        struct RiderModel
        {
            std::string source; // "DMIRRA/PLAYER.ZIT"
            std::vector<uint8_t> file;
            std::vector<RiderTexture> textures;
            // MODL's chunks, the TGEO's and the material table's.
            std::vector<Chunk> modl, tgeo;
            std::vector<std::vector<uint8_t>> materials; // MATI bodies
            int materialOfId[4] = {-1, -1, -1, -1};      // ID 1 .. 3 -> material index
            size_t skinnedIndex = 0;                     // the TGEO child that is the skinned part
            SkinnedPart part;
            std::vector<uint8_t> pivotKey; // the bones' pivots: models that share them share a skeleton
            // Each layout part's texture places (its vertices' UVs in 1/64 steps).
            std::set<uint32_t> uvPlaces[4];
        };

        // "MIRRASTREETID3" -> 3, "Harkin_CompD1v1" -> 1, "RYANSTREET_ID3" -> 3.
        int textureLayoutId(const std::string &name)
        {
            static const std::regex id(R"(I?D([123]))", std::regex::icase);
            int found = 0;
            for (auto it = std::sregex_iterator(name.begin(), name.end(), id); it != std::sregex_iterator(); ++it)
                found = (*it)[1].str()[0] - '0';
            return found;
        }

        bool readPart(const std::vector<uint8_t> &d, const Chunk &tgpt, SkinnedPart &p)
        {
            for (const Chunk &c : chunksIn(d, tgpt.body, tgpt.body + tgpt.size))
            {
                if (c.tag == fourcc("TGPH"))
                {
                    p.header = bytesOf(d, c);
                    p.vertices = u16(d, c.body + 4);
                    p.faces = u16(d, c.body + 6);
                }
                else if (c.tag == fourcc("MOBO"))
                    p.bones = bytesOf(d, c);
                else if (c.tag == fourcc("TGPB"))
                    p.pivots.push_back(bytesOf(d, c));
                else if (c.tag == fourcc("TGVP"))
                    p.positions = bytesOf(d, c);
                else if (c.tag == fourcc("TGVN"))
                    p.normals = bytesOf(d, c);
                else if (c.tag == fourcc("TGVC"))
                    p.colours = bytesOf(d, c);
                else if (c.tag == fourcc("TGVU"))
                    p.uvs = bytesOf(d, c);
                else if (c.tag == fourcc("TGVI"))
                    p.weights = bytesOf(d, c);
                else if (c.tag == fourcc("TGFP"))
                    p.faceRecords = bytesOf(d, c);
                else if (c.tag == fourcc("TGFN"))
                    p.faceNormals = bytesOf(d, c);
                else if (c.tag == fourcc("TGFM"))
                {
                    for (size_t at = c.body; at + 4 <= c.body + c.size;)
                    {
                        SkinnedPart::Group g;
                        g.mode = d[at];
                        g.material = d[at + 1];
                        const uint16_t count = u16(d, at + 2);
                        for (uint16_t i = 0; i < count; ++i)
                            g.entries.push_back(u16(d, at + 4 + i * 2u));
                        p.groups.push_back(std::move(g));
                        at += (count * 2u + 7u) & ~size_t(3);
                    }
                }
                else
                    return false; // a stream this does not know: not a model to take apart
            }
            const uint32_t n = p.vertices;
            if (p.header.size() < 20 || !n || p.positions.size() != n * 12u || p.colours.size() != n * 4u ||
                p.uvs.size() != n * 8u || p.weights.size() != n * 8u || p.normals.size() % n ||
                p.faceRecords.size() != p.faces * 8u || p.faceNormals.size() != p.faces * 12u)
                return false;
            p.normalStride = static_cast<uint32_t>(p.normals.size() / n);
            return true;
        }

        std::optional<RiderModel> readRiderModel(const std::string &source, std::vector<uint8_t> file)
        {
            RiderModel m;
            m.source = source;
            m.file = std::move(file);
            const std::vector<uint8_t> &d = m.file;
            const std::vector<Chunk> top = chunksIn(d, 0, d.size());
            const Chunk *fmdl = firstOf(top, fourcc("FMDL"));
            if (!fmdl)
                return std::nullopt;
            const std::vector<Chunk> model = chunksIn(d, fmdl->body, fmdl->body + fmdl->size);
            const Chunk *modl = firstOf(model, fourcc("MODL"));
            if (!modl)
                return std::nullopt;
            m.modl = chunksIn(d, modl->body, modl->body + modl->size);
            for (const Chunk &c : m.modl)
            {
                if (c.tag != fourcc("TXRC"))
                    continue;
                const std::vector<Chunk> texture = chunksIn(d, c.body, c.body + c.size);
                const Chunk *head = firstOf(texture, fourcc("TXRH"));
                if (!head || head->size < 0x34)
                    return std::nullopt;
                RiderTexture t;
                const char *name = reinterpret_cast<const char *>(&d[head->body + 0x14]);
                t.name.assign(name, strnlen(name, 0x20));
                t.hash = u32(d, head->body);
                t.id = textureLayoutId(t.name);
                t.txrc = wholeOf(d, c);
                m.textures.push_back(std::move(t));
            }
            const Chunk *tgeo = firstOf(m.modl, fourcc("TGEO"));
            if (!tgeo || m.textures.size() != 3)
                return std::nullopt;
            m.tgeo = chunksIn(d, tgeo->body, tgeo->body + tgeo->size);
            bool found = false;
            for (size_t i = 0; i < m.tgeo.size(); ++i)
            {
                const Chunk &c = m.tgeo[i];
                if (c.tag == fourcc("TGPT") && !found)
                {
                    m.skinnedIndex = i;
                    found = readPart(d, c, m.part);
                    if (!found)
                        return std::nullopt;
                }
                if (c.tag == fourcc("TMTB"))
                    for (const Chunk &mati : chunksIn(d, c.body, c.body + c.size))
                        if (mati.tag == fourcc("MATI") && mati.size >= kMaterialTexture + 4)
                            m.materials.push_back(bytesOf(d, mati));
            }
            if (!found || m.materials.size() != 3)
                return std::nullopt;
            // Each of the three layout textures is one material's.
            for (int material = 0; material < 3; ++material)
            {
                const uint32_t hash = u32(m.materials[material], kMaterialTexture);
                for (const RiderTexture &t : m.textures)
                    if (t.hash == hash && t.id)
                        m.materialOfId[t.id] = material;
            }
            for (int id = 1; id <= 3; ++id)
                if (m.materialOfId[id] < 0)
                    return std::nullopt;
            for (const std::vector<uint8_t> &pivot : m.part.pivots)
                m.pivotKey.insert(m.pivotKey.end(), pivot.begin(), pivot.end());
            for (int id = 1; id <= 3; ++id)
                for (const SkinnedPart::Group &g : m.part.groups)
                {
                    if (g.material != m.materialOfId[id])
                        continue;
                    for (uint16_t e : g.entries)
                    {
                        const uint32_t v = e & 0x3FFFu;
                        float uv[2];
                        std::memcpy(uv, &m.part.uvs[v * 8u], 8);
                        const auto step = [](float f) { return static_cast<uint32_t>(static_cast<int32_t>(std::lround(f * 64.0f)) & 0xFFFF); };
                        m.uvPlaces[id].insert(step(uv[0]) << 16 | step(uv[1]));
                    }
                }
            return m;
        }

        // ---- the parts the creator offers ----------------------------------------

        // The sections (CREATEPL.DAT's order), the layout texture each one
        // dresses, its texture's name in the made models (TEXTURE_NAME) and
        // its sub-part (SUB_PART_ID: the choice's byte in the character's
        // record is the sub-part + 4).
        struct Section
        {
            const char *title;
            int layoutId;
            const char *textureName;
            int subPart;
            const char *subPartTitle;
            // The models whose part is a geometry option, first found first
            // (folder and file under ASSETS/RIDERS).
            std::array<const char *, 6> sources;
        };
        constexpr int kSections = 3;
        constexpr int kOptionsMost = 3; // CREATEPL.DAT's parser takes fewer than 4
        constexpr int kChoicesMost = 64; // and fewer than 65 choices
        const Section kSectionList[kSections] = {
            {"Head", 1, "01HD01", 1, "Face",
             {"DMIRRA/PLAYER.ZIT", "AMISHBOY/PLAYER.ZIT", "MDIAS/PLAYER.ZIT", "TMCMURRA/PLAYER.ZIT", "CMACKAY/PLAYER.ZIT",
              "TMIRRA/PLAYER.ZIT"}},
            {"Body", 2, "01ST01", 2, "Shirt",
             {"LRAMSDEL/PLAYER.ZIT", "JGARCIA/PLAYER.ZIT", "TMCMURRA/PLAYER.ZIT", "AMISHBOY/PLAYER.ZIT", "DMIRRA/PLAYER.ZIT",
              "CMACKAY/PLAYER.ZIT"}},
            {"Legs", 3, "01PT01", 5, "Pants",
             {"AMISHBOY/PLAYER.ZIT", "LRAMSDEL/PLAYER.ZIT", "TMCMURRA/PLAYER.ZIT", "DMIRRA/PLAYER.ZIT", "CMACKAY/PLAYER.ZIT",
              "JGARCIA/PLAYER.ZIT"}},
        };
        constexpr int kOutfit = 0, kTone = 0;
        // The texture-naming chain's first sub-part (its one choice dresses
        // nothing) and the name after the last section's.
        constexpr int kChainSubPart = 3;
        constexpr const char *kChainEnd = "01END01";
        constexpr int kFirstCreated = 17, kCreatedSlots = 4; // the characters 17 .. 20
        // A character's record (RiderCharacter, 0x3C8 bytes) keeps its look at
        // +0xBE: 13 bytes an outfit (the tone, each section's geometry option,
        // each sub-part's choice at the sub-part + 4).
        constexpr uint32_t kCharacterBytes = 0x3C8, kLookBytes = 0xBE, kLookPerOutfit = 13;
        // A choice's picture: the rider's texture at half its side (the riders'
        // own NPC models do the same), so a section's library is small.
        constexpr int kChoiceShrink = 2;

        // What the creator is made of, from the disc's riders: each section's
        // geometry options, each with the textures that fit it (the riders'
        // textures of that part whose models map the part's faces to the same
        // places of the picture: the riders do not share one mapping).
        struct Option
        {
            const RiderModel *model = nullptr;
            std::vector<const RiderTexture *> choices;
            std::vector<std::string> sources; // "DMIRRA/PLAYER.ZIT MIRRASTREETID1"
        };
        struct Parts
        {
            bool made = false;
            std::vector<RiderModel> models; // every rider model read
            std::array<std::vector<Option>, kSections> options;
        };

        // ASSETS/RIDERS/<folder>/<file> for the characters 0 .. 16, as SCENE.TXT's
        // "RiderNN" models name them.
        std::vector<std::string> riderFolders()
        {
            std::vector<std::string> folders;
            std::vector<uint8_t> text;
            const std::shared_ptr<gf::GameFile> file = zge_host::openGameFile("SCENE.TXT");
            if (!file || !gf::readAll(*file, text))
                return folders;
            // A MODEL's NAME "RiderNN" and, after it, its FILE line (comments after either).
            const std::string scene(text.begin(), text.end());
            static const std::regex name(R"re(^\s*NAME\s+"Rider(\d\d)")re");
            static const std::regex fileLine(R"re(^\s*FILE\s+"assets\\riders\\([A-Za-z0-9_]+)\\Player\.Zit")re", std::regex::icase);
            int number = -1;
            size_t at = 0;
            while (at < scene.size())
            {
                const size_t end = std::min(scene.find('\n', at), scene.size());
                const std::string line = scene.substr(at, end - at);
                at = end + 1;
                std::smatch m;
                if (std::regex_search(line, m, name))
                    number = std::stoi(m[1].str());
                else if (number >= 0 && std::regex_search(line, m, fileLine))
                {
                    if (folders.size() < size_t(number) + 1)
                        folders.resize(size_t(number) + 1);
                    std::string folder = m[1].str();
                    std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                    folders[size_t(number)] = folder;
                    number = -1;
                }
                else if (line.find("NAME") != std::string::npos)
                    number = -1;
            }
            return folders;
        }

        // The texture at 1/kChoiceShrink of its side: its first picture's
        // texels taken a step apart (an 8-bit picture keeps its palette); a
        // picture of another depth stays whole. One picture, no smaller ones.
        std::vector<uint8_t> shrunkTexture(const std::vector<uint8_t> &txrc, const std::string &name)
        {
            const std::vector<Chunk> kids = chunksIn(txrc, 8, txrc.size());
            const Chunk *head = firstOf(kids, fourcc("TXRH"));
            const Chunk *picture = nullptr;
            for (const Chunk &c : kids)
                if (c.tag == fourcc("TBMP") || c.tag == fourcc("TZMP"))
                {
                    picture = &c;
                    break;
                }
            if (!head || !picture || head->size < 0x34 || picture->size < 54)
                return {};
            std::vector<uint8_t> bmp = bytesOf(txrc, *picture);
            const uint32_t pixels = u32(bmp, 0x0A);
            const int32_t width = static_cast<int32_t>(u32(bmp, 0x12)), height = static_cast<int32_t>(u32(bmp, 0x16));
            const uint16_t bits = u16(bmp, 0x1C);
            const uint32_t stride = ((static_cast<uint32_t>(width) * bits + 31u) / 32u) * 4u;
            if (bits == 8 && width % kChoiceShrink == 0 && height % kChoiceShrink == 0 &&
                pixels + stride * static_cast<uint32_t>(height) <= bmp.size())
            {
                const int32_t w = width / kChoiceShrink, h = height / kChoiceShrink;
                const uint32_t newStride = ((static_cast<uint32_t>(w) * 8u + 31u) / 32u) * 4u;
                std::vector<uint8_t> out(bmp.begin(), bmp.begin() + pixels);
                out.resize(pixels + newStride * static_cast<uint32_t>(h), 0);
                for (int32_t y = 0; y < h; ++y)
                    for (int32_t x = 0; x < w; ++x)
                        out[pixels + static_cast<uint32_t>(y) * newStride + static_cast<uint32_t>(x)] =
                            bmp[pixels + static_cast<uint32_t>(y * kChoiceShrink) * stride + static_cast<uint32_t>(x * kChoiceShrink)];
                put32(out, 0x02, static_cast<uint32_t>(out.size()));
                put32(out, 0x12, static_cast<uint32_t>(w));
                put32(out, 0x16, static_cast<uint32_t>(h));
                put32(out, 0x22, newStride * static_cast<uint32_t>(h));
                bmp = std::move(out);
            }
            // As the UI's own libraries keep a picture: TZMP, its palette's
            // alpha whole (the models' TBMP leave it 0 and draw opaque).
            const uint32_t paletteAt = 14u + u32(bmp, 0x0E);
            for (uint32_t at = paletteAt + 3u; at < pixels && at < bmp.size(); at += 4u)
                bmp[at] = 0xFF;
            std::vector<uint8_t> header = bytesOf(txrc, *head);
            put32(header, 0, nameHash(name));
            put16(header, 4, static_cast<uint16_t>(u32(bmp, 0x12)));
            put16(header, 6, static_cast<uint16_t>(u32(bmp, 0x16)));
            std::fill(header.begin() + 0x14, header.begin() + 0x34, 0);
            std::memcpy(&header[0x14], name.data(), std::min<size_t>(name.size(), 0x1F));
            std::vector<uint8_t> body;
            writeChunk(body, fourcc("TXRH"), header);
            writeChunk(body, fourcc("TZMP"), bmp);
            std::vector<uint8_t> out;
            writeChunk(out, fourcc("TXRC"), body);
            return out;
        }

        // A texture chunk under another name (its hash with it), pictures as they are.
        std::vector<uint8_t> renamedTexture(const std::vector<uint8_t> &txrc, const std::string &name)
        {
            std::vector<uint8_t> body;
            for (const Chunk &c : chunksIn(txrc, 8, txrc.size()))
            {
                std::vector<uint8_t> b = bytesOf(txrc, c);
                if (c.tag == fourcc("TXRH") && b.size() >= 0x34)
                {
                    put32(b, 0, nameHash(name));
                    std::fill(b.begin() + 0x14, b.begin() + 0x34, 0);
                    std::memcpy(&b[0x14], name.data(), std::min<size_t>(name.size(), 0x1F));
                }
                writeChunk(body, c.tag, b);
            }
            std::vector<uint8_t> out;
            writeChunk(out, fourcc("TXRC"), body);
            return out;
        }

        Parts makeParts()
        {
            Parts parts;
            const std::vector<std::string> folders = riderFolders();
            std::map<std::string, size_t> bySource;
            parts.models.reserve(folders.size() * 2);
            for (const std::string &folder : folders)
                for (const char *file : {"PLAYER.ZIT", "PLAYERC.ZIT"})
                {
                    if (folder.empty())
                        continue;
                    const std::string source = folder + "/" + file;
                    std::vector<uint8_t> bytes;
                    const std::shared_ptr<gf::GameFile> f = zge_host::openGameFile("ASSETS/RIDERS/" + source);
                    if (!f || !gf::readAll(*f, bytes))
                        continue;
                    if (std::optional<RiderModel> m = readRiderModel(source, std::move(bytes)))
                    {
                        bySource[source] = parts.models.size();
                        parts.models.push_back(std::move(*m));
                    }
                    else
                        std::cerr << "[create-player] " << source << ": not a rider model this can take apart" << std::endl;
                }
            if (parts.models.empty())
                return parts;
            // Geometry: models whose bones sit where the first section's first
            // model's do (one skeleton), so their parts meet. Choices: every
            // model's texture of the part whose faces map like the option's
            // (most of the same texture places used), the option's own first,
            // each picture once.
            const RiderModel *skeleton = nullptr;
            for (int s = 0; s < kSections; ++s)
                for (const char *source : kSectionList[s].sources)
                {
                    const auto at = bySource.find(source);
                    if (at == bySource.end() || parts.options[s].size() >= kOptionsMost)
                        continue;
                    const RiderModel &m = parts.models[at->second];
                    if (!skeleton)
                        skeleton = &m;
                    if (m.pivotKey != skeleton->pivotKey || m.part.bones.size() != skeleton->part.bones.size())
                        continue;
                    Option option;
                    option.model = &m;
                    const int id = kSectionList[s].layoutId;
                    const std::set<uint32_t> &mine = m.uvPlaces[id];
                    std::vector<std::vector<uint8_t>> seen;
                    std::vector<const RiderModel *> order{&m};
                    for (const RiderModel &other : parts.models)
                        if (&other != &m)
                            order.push_back(&other);
                    for (const RiderModel *other : order)
                    {
                        const std::set<uint32_t> &theirs = other->uvPlaces[id];
                        size_t shared = 0;
                        for (uint32_t place : theirs)
                            shared += mine.count(place);
                        const size_t all = mine.size() + theirs.size() - shared;
                        if (!all || shared * 4 < all * 3) // under three quarters of the places in common
                            continue;
                        for (const RiderTexture &t : other->textures)
                        {
                            if (t.id != id || option.choices.size() >= kChoicesMost ||
                                std::find(seen.begin(), seen.end(), t.txrc) != seen.end())
                                continue;
                            seen.push_back(t.txrc);
                            option.choices.push_back(&t);
                            option.sources.push_back(other->source + " " + t.name);
                        }
                    }
                    parts.options[s].push_back(std::move(option));
                }
            parts.made = true;
            for (int s = 0; s < kSections; ++s)
                parts.made = parts.made && !parts.options[s].empty();
            return parts;
        }

        // ---- the made models ---------------------------------------------------

        // The skinned part of `from`'s faces of one material, appended to `to`
        // as material `material`: the vertices they use, the strip groups,
        // and the collision faces whose corners are all among them.
        void takePart(const SkinnedPart &from, int fromMaterial, uint8_t material, SkinnedPart &to)
        {
            std::map<uint16_t, uint16_t> remap;
            const auto vertex = [&](uint16_t v) {
                const auto at = remap.find(v);
                if (at != remap.end())
                    return at->second;
                const uint16_t index = static_cast<uint16_t>(to.vertices++);
                remap[v] = index;
                const auto copy = [&](std::vector<uint8_t> &out, const std::vector<uint8_t> &in, size_t size) {
                    out.insert(out.end(), in.begin() + static_cast<ptrdiff_t>(v * size),
                               in.begin() + static_cast<ptrdiff_t>((v + 1) * size));
                };
                copy(to.positions, from.positions, 12);
                copy(to.normals, from.normals, from.normalStride);
                copy(to.colours, from.colours, 4);
                copy(to.uvs, from.uvs, 8);
                copy(to.weights, from.weights, 8);
                return index;
            };
            constexpr uint16_t kVertexMask = 0x3FFF;
            for (const SkinnedPart::Group &g : from.groups)
            {
                if (g.material != fromMaterial)
                    continue;
                SkinnedPart::Group out{g.mode, material, {}};
                for (uint16_t e : g.entries)
                    out.entries.push_back(static_cast<uint16_t>((e & ~kVertexMask) | vertex(e & kVertexMask)));
                to.groups.push_back(std::move(out));
            }
            for (uint32_t f = 0; f < from.faces; ++f)
            {
                const size_t at = f * 8u;
                const uint16_t corners[3] = {u16(from.faceRecords, at + 2), u16(from.faceRecords, at + 4),
                                             u16(from.faceRecords, at + 6)};
                if (!remap.count(corners[0]) || !remap.count(corners[1]) || !remap.count(corners[2]))
                    continue;
                std::vector<uint8_t> record(from.faceRecords.begin() + static_cast<ptrdiff_t>(at),
                                            from.faceRecords.begin() + static_cast<ptrdiff_t>(at + 8));
                for (int k = 0; k < 3; ++k)
                    put16(record, 2 + k * 2, remap[corners[k]]);
                to.faceRecords.insert(to.faceRecords.end(), record.begin(), record.end());
                to.faceNormals.insert(to.faceNormals.end(), from.faceNormals.begin() + static_cast<ptrdiff_t>(f * 12u),
                                      from.faceNormals.begin() + static_cast<ptrdiff_t>(f * 12u + 12u));
                ++to.faces;
            }
        }

        std::vector<uint8_t> writePart(const SkinnedPart &p)
        {
            std::vector<uint8_t> out;
            std::vector<uint8_t> header = p.header;
            put16(header, 4, static_cast<uint16_t>(p.vertices));
            put16(header, 6, static_cast<uint16_t>(p.faces));
            put16(header, 8, static_cast<uint16_t>(p.groups.size()));
            writeChunk(out, fourcc("TGPH"), header);
            writeChunk(out, fourcc("MOBO"), p.bones);
            writeChunk(out, fourcc("TGVP"), p.positions);
            writeChunk(out, fourcc("TGVN"), p.normals);
            writeChunk(out, fourcc("TGVC"), p.colours);
            writeChunk(out, fourcc("TGVU"), p.uvs);
            writeChunk(out, fourcc("TGVI"), p.weights);
            for (const std::vector<uint8_t> &pivot : p.pivots)
                writeChunk(out, fourcc("TGPB"), pivot);
            writeChunk(out, fourcc("TGFP"), p.faceRecords);
            writeChunk(out, fourcc("TGFN"), p.faceNormals);
            std::vector<uint8_t> groups;
            for (const SkinnedPart::Group &g : p.groups)
            {
                const size_t at = groups.size();
                groups.resize(at + 4 + ((g.entries.size() * 2u + 3u) & ~size_t(3)), 0);
                groups[at] = g.mode;
                groups[at + 1] = g.material;
                put16(groups, at + 2, static_cast<uint16_t>(g.entries.size()));
                for (size_t i = 0; i < g.entries.size(); ++i)
                    put16(groups, at + 4 + i * 2, g.entries[i]);
            }
            writeChunk(out, fourcc("TGFM"), groups);
            return out;
        }

        // A model of the head's part of `head`, the body's of `body` and the
        // legs' of `legs`; the rest (the shadow part, bounds, the instance) the
        // body model's. Its textures are the three parts' own, named as the
        // creator's sub-parts (TEXTURE_NAME), so the creator's choices find them.
        std::vector<uint8_t> composedModel(const std::array<const RiderModel *, kSections> &from)
        {
            const RiderModel &base = *from[1];
            const std::vector<uint8_t> &d = base.file;
            SkinnedPart part;
            part.header = base.part.header;
            part.bones = base.part.bones;
            part.pivots = base.part.pivots;
            for (int s = 0; s < kSections; ++s)
                takePart(from[s]->part, from[s]->materialOfId[kSectionList[s].layoutId], static_cast<uint8_t>(s), part);
            std::vector<uint8_t> tgeo;
            for (size_t i = 0; i < base.tgeo.size(); ++i)
            {
                const Chunk &c = base.tgeo[i];
                if (i == base.skinnedIndex)
                    writeChunk(tgeo, fourcc("TGPT"), writePart(part));
                else if (c.tag == fourcc("TMTB"))
                {
                    std::vector<uint8_t> table;
                    for (int s = 0; s < kSections; ++s)
                    {
                        std::vector<uint8_t> material = from[s]->materials[from[s]->materialOfId[kSectionList[s].layoutId]];
                        put32(material, kMaterialTexture, nameHash(kSectionList[s].textureName));
                        writeChunk(table, fourcc("MATI"), material);
                    }
                    writeChunk(tgeo, fourcc("TMTB"), table);
                }
                else
                {
                    const std::vector<uint8_t> whole = wholeOf(d, c);
                    tgeo.insert(tgeo.end(), whole.begin(), whole.end());
                }
            }
            std::vector<uint8_t> modl;
            for (int s = 0; s < kSections; ++s)
                for (const RiderTexture &t : from[s]->textures)
                    if (t.id == kSectionList[s].layoutId)
                    {
                        const std::vector<uint8_t> renamed = renamedTexture(t.txrc, kSectionList[s].textureName);
                        modl.insert(modl.end(), renamed.begin(), renamed.end());
                    }
            for (const Chunk &c : base.modl)
            {
                if (c.tag == fourcc("TXRC"))
                    continue;
                if (c.tag == fourcc("TGEO"))
                    writeChunk(modl, fourcc("TGEO"), tgeo);
                else
                {
                    const std::vector<uint8_t> whole = wholeOf(d, c);
                    modl.insert(modl.end(), whole.begin(), whole.end());
                }
            }
            std::vector<uint8_t> out;
            writeChunk(out, fourcc("MODL"), modl);
            return out;
        }

        // ---- UI libraries ---------------------------------------------------------

        // HLIB and HAST: the artist's path, the name, the kind.
        std::vector<uint8_t> assetHead(const std::string &path, const std::string &name, uint32_t kind)
        {
            std::vector<uint8_t> head(0xA4, 0);
            std::memcpy(head.data(), path.data(), std::min<size_t>(path.size(), 0x7F));
            std::memcpy(head.data() + 0x80, name.data(), std::min<size_t>(name.size(), 0x1F));
            put32(head, 0xA0, kind);
            return head;
        }
        constexpr uint32_t kLibraryKind = 0x00010001u, kMaterialKind = 0, kSceneKind = 4;
        // A material's head as the game's own libraries have it.
        const std::vector<uint8_t> kMaterialHead = {0x11, 0xC1, 0xCC, 0xCC};

        std::vector<uint8_t> library(const std::string &name, const std::vector<std::vector<uint8_t>> &assets)
        {
            std::vector<uint8_t> body;
            writeChunk(body, fourcc("HLIB"), assetHead("S:\\bmx2\\riders\\createarider\\" + name + ".zif", name, kLibraryKind));
            for (const std::vector<uint8_t> &asset : assets)
                writeChunk(body, fourcc("UAST"), asset);
            std::vector<uint8_t> out;
            writeChunk(out, fourcc("ULIB"), body);
            return out;
        }

        // G<o><h><b><l>: the model of those geometry options.
        std::string geometryName(int h, int b, int l)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "G%d%d%d%d", kOutfit, h, b, l);
            return name;
        }
        // T<o><t><s><g>: a section option's choices; its assets "<lib><part><choice>".
        std::string choicesName(int section, int option)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "T%d%d%d%d", kOutfit, kTone, section, option);
            return name;
        }

        // ---- CREATEPL.DAT ---------------------------------------------------------

        std::string creatorData(const Parts &parts)
        {
            std::string out;
            const auto line = [&](int depth, const std::string &text) { out += std::string(size_t(depth), '\t') + text + "\r\n"; };
            line(0, "// Create Player: the parts the port makes from the riders' models (dave_mirra_creator.cpp).");
            line(0, "#define FACE_SUB_PART\t\t1");
            line(0, "#define SHIRT_SUB_PART\t\t2");
            line(0, "#define PANTS_SUB_PART\t\t5");
            line(0, "#define CHAIN_SUB_PART\t\t3");
            line(0, "#define kbmxSponsorNone\t\t0");
            line(0, "");
            line(0, "OUTFITS BEGIN");
            line(1, "NUMBER_OF_OUTFIT_TYPES 1");
            line(1, "//Street outfit");
            line(1, "BEGIN");
            line(2, "NUMBER_OF_TEXTURE_TONES 1");
            line(2, "NUMBER_OF_GEOMETRY_SECTIONS " + std::to_string(kSections));
            static const char *const kSubPartDefine[] = {"$FACE_SUB_PART", "$SHIRT_SUB_PART", "$PANTS_SUB_PART"};
            for (int s = 0; s < kSections; ++s)
            {
                line(2, std::string("//") + kSectionList[s].title + " section");
                line(2, "BEGIN");
                line(3, "NUMBER_OF_GEOMETRY_OPTIONS " + std::to_string(parts.options[s].size()));
                for (const Option &option : parts.options[s])
                {
                    line(3, "//" + option.model->source);
                    line(3, "BEGIN");
                    // The game's setModelTexture names the texture it hands on by
                    // the sub-part before (a kept bug, player_creator.cpp), so
                    // each sub-part names the next one's material: a first
                    // sub-part only the chain's start, then each section's own.
                    const auto subPart = [&](const std::string &textureName, const std::string &id,
                                             const std::vector<std::string> &sources) {
                        line(4, "BEGIN");
                        line(5, "TEXTURE_NAME " + textureName);
                        line(5, "SUB_PART_ID " + id);
                        line(5, "NUMBER_OF_CHOICES " + std::to_string(sources.size()));
                        line(5, "GROUP_ID BEGIN");
                        for (size_t c = 0; c < sources.size(); ++c)
                            line(6, "$kbmxSponsorNone");
                        line(5, "END");
                        line(5, "FILE_NAMES BEGIN");
                        line(6, "TEXTURE_TONE BEGIN");
                        for (const std::string &source : sources)
                        {
                            std::string path = "ASSETS\\RIDERS\\" + source.substr(0, source.find(' '));
                            std::replace(path.begin(), path.end(), '/', '\\');
                            line(7, path + "\\" + source.substr(source.find(' ') + 1));
                        }
                        line(6, "END");
                        line(5, "END");
                        line(4, "END");
                    };
                    const bool first = s == 0;
                    line(4, std::string("NUMBER_OF_SUB_PARTS ") + (first ? "2" : "1"));
                    if (first)
                        subPart(kSectionList[0].textureName, "$CHAIN_SUB_PART", {option.sources.front()});
                    subPart(s + 1 < kSections ? kSectionList[s + 1].textureName : kChainEnd, kSubPartDefine[s], option.sources);
                    line(3, "END");
                }
                line(2, "END");
            }
            line(2, "FILE_NAMES BEGIN");
            for (size_t h = 0; h < parts.options[0].size(); ++h)
                for (size_t b = 0; b < parts.options[1].size(); ++b)
                    for (size_t l = 0; l < parts.options[2].size(); ++l)
                        line(3, "UILIB:" + geometryName(int(h), int(b), int(l)) + ".ZIF");
            line(2, "END");
            line(1, "END");
            line(0, "END");
            return out;
        }

        // ---- the screen ------------------------------------------------------------

        // creator.zsc: Root.PlayerCreator, the variables the game's creator
        // functions read and set, a frame of rows in the look of the Options
        // frames (the Common library's frame materials, mnu_small in the menus'
        // grey and gold), and the model (PlayerModel), dancing as the rider
        // select's does. Its pad goes to the pcCreator functions.
        constexpr int kRows = 9;
        constexpr float kRowTop = 40.0f, kRowPitch = 30.0f;
        const char *const kRowTitles[kRows] = {"Rider", "Head", "Body", "Legs", "Face", "Shirt", "Pants", "Ride", "Done"};
        constexpr int kRowSlot = 0, kRowFirstGeometry = 1, kRowFirstTexture = 4, kRowRide = 7, kRowDone = 8;

        const char *const kScreenHead = R"ZSC(///////////////////////////
// UIBuilder Generated File (Create Player: dave_mirra_creator.cpp)

////////////////////////////
// Main Object
OBJECT "guiScreenSpace" "PlayerCreator"
{
	//////////////////////////////
	// Property
	OnCreate	"PLAY DeclareA\nPLAY DeclareB\nPLAY DeclareC\nSET Frame.Visible {0}\nSET PlayerModel.Visible {0}\nCALL bmxPlayerCreatorInitialize"
	Visible	0
	Scale	1.000000 1.000000 1.000000
	//////////////////////////////
	// Action
	ACTION "Script" "DeclareA"
	{
		StartDelay	0
		Script	"DECLARE String ActiveMenu\nDECLARE Sint32 Slot\nDECLARE Sint32 OutfitType\nDECLARE Sint32 NumberOfOptions\nDECLARE Sint32 GeometryOption0\nDECLARE Sint32 GeometryOption1\nDECLARE Sint32 GeometryOption2\nDECLARE Sint32 TextureOption1\nDECLARE Sint32 TextureOption2\nDECLARE Sint32 TextureOption3\nDECLARE Sint32 TextureOption5\nDECLARE String Next"
	}
	ACTION "Script" "DeclareB"
	{
		StartDelay	0
		Script	"@DECLARES@"
	}
	ACTION "Script" "DeclareC"
	{
		StartDelay	0
		Script	"DECLARE String ModelLib\nDECLARE String ModelPath\nDECLARE String ModelAsset\nDECLARE String HeadLib\nDECLARE String HeadPath\nDECLARE String BodyLib\nDECLARE String BodyPath\nDECLARE String LegsLib\nDECLARE String LegsPath\nDECLARE String OldModel\nDECLARE String OldHead\nDECLARE String OldBody\nDECLARE String OldLegs"
	}
	ACTION "Script" "Init"
	{
		StartDelay	0
		Script	"SET Visible {1}\nSET Frame.Visible {1}\nSET Root.CommonUI.TopHeader.Title.Text {\"Create Rider\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Caption.Text {\"Back\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Text {\"3\"}\nPLAY Root.CommonUI.UnlockUser\nPLAY Root.CommonUI.DisplayButton1\nCALL pcCreatorOpen\nPLAY ReadLook\nPLAY ShowModel\nSET Frame.KeySniffer.Active {1}\nSETFOCUS"
	}
	ACTION "Script" "ReadLook"
	{
		StartDelay	0
		Script	"SET ActiveMenu {\"Texture\"}\nCALL InitializePlayerCreatorMenu\nCALL pcCreatorTextures\nSET ActiveMenu {\"Geometry\"}\nCALL InitializePlayerCreatorMenu\nCALL pcCreatorGeometry"
	}
	ACTION "Script" "ShowModel"
	{
		StartDelay	0
		Script	"STOP PlayerModel.Dance\nSET PlayerModel.Visible {0}\nSET PlayerModel.AssetName {\"\"}\nUNLOADLIB *OldModel\nUNLOADLIB *OldHead\nUNLOADLIB *OldBody\nUNLOADLIB *OldLegs\nSET OldModel {\"\"}\nSET OldHead {\"\"}\nSET OldBody {\"\"}\nSET OldLegs {\"\"}\nLOADLIB *HeadLib *HeadPath HeadLoaded"
	}
	ACTION "Script" "HeadLoaded"
	{
		StartDelay	0
		Script	"SET OldHead HeadLib\nLOADLIB *BodyLib *BodyPath BodyLoaded"
	}
	ACTION "Script" "BodyLoaded"
	{
		StartDelay	0
		Script	"SET OldBody BodyLib\nLOADLIB *LegsLib *LegsPath LegsLoaded"
	}
	ACTION "Script" "LegsLoaded"
	{
		StartDelay	0
		Script	"SET OldLegs LegsLib\nLOADLIB *ModelLib *ModelPath ModelLoaded"
	}
	ACTION "Script" "ModelLoaded"
	{
		StartDelay	0
		Script	"SET OldModel ModelLib\nSET PlayerModel.Visible {1}\nSET PlayerModel.AssetName ModelAsset\nPLAY PlayerModel.Dance\nCALL ApplyPlayerCreatorTexturesToModel"
	}
	ACTION "Script" "Step"
	{
		StartDelay	0
		Script	"GOTO Next\n:Slot\nPLAY ReadLook\nPLAY ShowModel\nGOTO End\n:Geometry\nSET ActiveMenu {\"Geometry\"}\nCALL HarvestPlayerCreatorMenu\nPLAY ReadLook\nPLAY ShowModel\nGOTO End\n:Texture\nSET ActiveMenu {\"Texture\"}\nCALL HarvestPlayerCreatorMenu\nCALL ApplyPlayerCreatorTexturesToModel\nGOTO End\n:Ride\nPLAY Leave\nSET Root.MainMenu.PlayerCount {1}\nSET Root.MainMenu.PlayerIndex {1}\nSET Root.CurrentRideType {3}\nSET Root.CurrentRiderCount {1}\nSET Root.CurrentRider1 Slot\nSET Root.CurrentRiderOutfit1 {0}\nSET Root.CurrentBike1 {0}\nPLAY Root.MainMenu.SelectLevel.Init\nGOTO End\n:Done\nPLAY Leave\nPLAY Root.MainMenu.MainMenuFrame.Init\n:End"
	}
	ACTION "Script" "Leave"
	{
		StartDelay	0
		Script	"KILLFOCUS\nSET Frame.KeySniffer.Active {0}\nSTOP PlayerModel.Dance\nSET PlayerModel.Visible {0}\nSET PlayerModel.AssetName {\"\"}\nUNLOADLIB *OldModel\nUNLOADLIB *OldHead\nUNLOADLIB *OldBody\nUNLOADLIB *OldLegs\nSET OldModel {\"\"}\nSET OldHead {\"\"}\nSET OldBody {\"\"}\nSET OldLegs {\"\"}\nSET Frame.Visible {0}\nSET Visible {0}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Caption.Text {\"Select\"}\nSET Root.CommonUI.BottomHeader.Corner.BottomLine.Button1.Text {\"1\"}\nPLAY Root.CommonUI.LockUser"
	}
	OBJECT "guiFrame" "Frame"
	{
		//////////////////////////////
		// Property
		Visible	0
		Position	16.000000 70.000000 0.000000
		Scale	1.000000 1.000000 1.000000
		Alignment	0
		Material_Top	"Common.top"
		Material_TopRight	"Common.topright"
		Material_Center	"Common.center"
		Material_Right	"Common.right"
		Material_Bottom	"Common.bottom"
		Material_BottomRight	"Common.botright"
		Width	280
		Height	310
		OBJECT "guiImage" "Highlight"
		{
			//////////////////////////////
			// Property
			Visible	1
			Position	140.000000 40.000000 0.000000
			Scale	1.000000 1.000000 1.000000
			Alignment	17
			TopLeftColor	238 181 44 56
			BottomLeftColor	238 181 44 56
			TopRightColor	238 181 44 56
			BottomRightColor	238 181 44 56
			Width	264
			Height	26
			UScale	1
			VScale	1
		}
@ROWS@		OBJECT "guiKeySniffer" "KeySniffer"
		{
			//////////////////////////////
			// Property
			OnCreate	"SET Active {0}"
			Visible	1
			Scale	1.000000 1.000000 1.000000
			Active	0
			Flags	3
			CurCtrl	0
			LeftAxisX	0
			LeftAxisY	0
			RightAxisX	0
			RightAxisY	0
			OnUpKey	"CALL pcCreatorUp\nPLAY Root.PlayerCreator.Step"
			OnDownKey	"CALL pcCreatorDown\nPLAY Root.PlayerCreator.Step"
			OnLeftKey	"CALL pcCreatorLeft\nPLAY Root.PlayerCreator.Step"
			OnRightKey	"CALL pcCreatorRight\nPLAY Root.PlayerCreator.Step"
			OnFire4	"CALL pcCreatorAccept\nPLAY Root.PlayerCreator.Step"
			OnFire1	"CALL pcCreatorBack\nPLAY Root.PlayerCreator.Step"
			OnStart	"CALL pcCreatorBack\nPLAY Root.PlayerCreator.Step"
		}
	}
	MODEL "NotUsed" "PlayerModel"
	{
		//////////////////////////////
		// Property
		OnCreate	"SET AssetName {\"\"}"
		Visible	0
		Position	1.000000 0.900000 4.000000
		Scale	1.000000 1.000000 1.000000
		//////////////////////////////
		// Action
		ACTION "Animate" "Dance"
		{
			StartDelay	0
			OnStart	"SET Orientation {0 0 0 1}\nSET Position {1.0 0.9 4}"
			StartAnimation	"PlayerMotions.interface00_s"
			LoopAnimation	"PlayerMotions.interface00_l"
			LoopMode	1
			LoopTime	10000
			Speed	1
		}
	}
}
)ZSC";

        std::string screenScript()
        {
            std::string declares;
            // The thirteen MaxOption a sub-part's id reaches (clearCreatorMaxOptions sets them all).
            for (int i = 0; i < 13; ++i)
                declares += std::string(i ? "\\n" : "") + "DECLARE Sint32 MaxOption" + std::to_string(i);
            std::string rows;
            for (int i = 0; i < kRows; ++i)
                for (int column = 0; column < 2; ++column)
                {
                    char label[768];
                    std::snprintf(label, sizeof(label),
                                  "\t\tOBJECT \"guiLabel\" \"%s%d\"\n\t\t{\n"
                                  "\t\t\t//////////////////////////////\n\t\t\t// Property\n"
                                  "\t\t\tVisible\t1\n\t\t\tPosition\t%.6f %.6f 0.000000\n"
                                  "\t\t\tScale\t0.800000 0.800000 1.000000\n\t\t\tAlignment\t%d\n"
                                  "\t\t\tSpacing\t1.250000\n\t\t\tTracking\t1\n"
                                  "\t\t\tCharset\t\"Common.mnu_small\"\n"
                                  "\t\t\tColor\t%s\n\t\t\tText\t\"%s\"\n\t\t}\n",
                                  column == 0 ? "Name" : "Value", i, column == 0 ? 20.0 : 260.0,
                                  static_cast<double>(kRowTop + static_cast<float>(i) * kRowPitch), column == 0 ? 16 : 18,
                                  column == 0 ? "192 192 192 255" : "238 181 44 255", column == 0 ? kRowTitles[i] : "");
                    rows += label;
                }
            std::string text = kScreenHead;
            text.replace(text.find("@DECLARES@"), 10, declares);
            text.replace(text.find("@ROWS@"), 6, rows);
            std::string crlf;
            for (char c : text)
            {
                if (c == '\n')
                    crlf += '\r';
                crlf += c;
            }
            return crlf;
        }

        // The main menu's CREATE RIDER: after Multiplayer (Exit to Collection,
        // when there, moves one down). Its action opens the screen.
        constexpr const char *kItem = "Create Rider";
        bool patchMainMenu(std::string &text)
        {
            std::string edited = text;
            zx::Span menu = zx::findObject(edited, "MainMenu/MainMenuFrame/MainMenu");
            if (!menu.found())
                return false;
            const bool exitLine = edited.find("\\nExit to Collection", menu.begin) != std::string::npos;
            if (exitLine)
            {
                // Exit to Collection's action: one place on.
                zx::Span exitAction = zx::findObject(edited, "MainMenu/MainMenuFrame/MainMenu/7");
                if (!exitAction.found() || !zx::replaceIn(edited, exitAction, "ACTION \"Script\" \"7\"", "ACTION \"Script\" \"8\""))
                    return false;
                menu = zx::findObject(edited, "MainMenu/MainMenuFrame/MainMenu");
            }
            // The line's index is 7 either way (after Multiplayer, 6).
            const std::string action = "ACTION \"Script\" \"7\"\n{\n\tStartDelay\t0\n\tScript\t\"PLAY .DeInit\\nPLAY "
                                       "Root.PlayerCreator.Init\"\n}\n";
            edited.insert(zx::objectEnd(edited, menu), zx::indentBlock(action, zx::indentAt(edited, menu.begin) + 1));
            menu = zx::findObject(edited, "MainMenu/MainMenuFrame/MainMenu");
            const std::string line = std::string("\\n") + kItem;
            bool ok = zx::replaceIn(edited, menu, "\\nMultiplayer", "\\nMultiplayer" + line);
            // The screen: a second top object of the script, made under the
            // root with the main menu (the game's creator functions find it there).
            edited += screenScript();
            zx::Span frame = zx::findObject(edited, "MainMenu/MainMenuFrame");
            ok = ok && zx::replaceIn(edited, frame, "\\n!Multiplayer", "\\n!Multiplayer" + line) &&
                 zx::replaceIn(edited, frame, "\\nCool Stuff\\nMultiplayer", "\\nCool Stuff\\nMultiplayer" + line);
            // The frame one line taller.
            for (const char *from : {"\tHeight\t300\r\n", "\tHeight\t270\r\n"})
            {
                frame = zx::findObject(edited, "MainMenu/MainMenuFrame");
                const std::string height = from;
                const int value = std::stoi(height.substr(8));
                if (zx::replaceIn(edited, frame, height, "\tHeight\t" + std::to_string(value + 30) + "\r\n"))
                    break;
            }
            ok = ok && cheats::patchLevelSelectBack(edited);
            if (!ok)
                return false;
            text = std::move(edited);
            return true;
        }

        // ---- what is made, once a boot ---------------------------------------------

        struct State
        {
            std::mutex mutex;
            bool tried = false;
            Parts parts;
            // Guest memory of our own: names and values handed to the game's
            // functions, and the stack they run on (the scratch's top).
            uint32_t scratch = 0;
            // The screen.
            uint32_t screen = 0;
            int row = 0, slot = 0;
            int geometry[kSections] = {}, geometryMost[kSections] = {};
            int texture[kSections] = {}, textureMost[kSections] = {};
            // The libraries a created rider's model was made from, held while
            // the rider can live (until the front end's next exit), and how
            // many of the maker's unloads are still to keep.
            std::vector<std::string> riderLibraries;
            int keepUnloads = 0;
            // The slot a RIDE left from: Back from the level select comes
            // back to it (until the front end's exit, the ride's level start).
            int rideSlot = -1;
            bool on = false;
            PS2Runtime::RecompiledFunction frontEndExit = nullptr, newCreatedRiderModel = nullptr, unloadLibrary = nullptr,
                                           checkCheat = nullptr;
        };
        State &state()
        {
            static ps2x::boot::PerBoot<std::unique_ptr<State>> ofBoot([] { return std::make_unique<State>(); });
            return *ofBoot.get();
        }
        constexpr uint32_t kScratchBytes = 0x4000, kScratchStrings = 0x800;

        const Parts &parts()
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.mutex);
            if (!st.tried)
            {
                st.tried = true;
                st.parts = makeParts();
                std::cout << "[create-player] " << st.parts.models.size() << " rider models read;";
                for (int s = 0; s < kSections; ++s)
                {
                    std::cout << " " << kSectionList[s].title << ":";
                    for (const Option &option : st.parts.options[s])
                        std::cout << " " << option.model->source << " (" << option.choices.size() << ")";
                    std::cout << ";";
                }
                std::cout << (st.parts.made ? " made" : " not made") << std::endl;
            }
            return st.parts;
        }

        bool patchCreatorData(std::vector<uint8_t> &bytes)
        {
            const Parts &p = parts();
            if (!p.made)
                return false;
            const std::string text = creatorData(p);
            bytes.assign(text.begin(), text.end());
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            const Parts &p = parts();
            if (!p.made)
                return false;
            std::vector<zx::ArchiveEntry> entries;
            if (!zx::readArchive(bytes, entries))
                return false;
            bool menu = false;
            for (zx::ArchiveEntry &entry : entries)
            {
                if (zx::scriptName(entry) != "MainMenu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (!patchMainMenu(text))
                {
                    std::cerr << "[create-player] MainMenu is not the expected script: no Create Player" << std::endl;
                    return false;
                }
                entry.data.assign(text.begin(), text.end());
                menu = true;
            }
            if (!menu)
                return false;
            constexpr uint32_t kEntryFlags = 6;
            const auto add = [&](const std::string &file, std::vector<uint8_t> data) {
                zx::ArchiveEntry entry;
                entry.nameHash = nameHash(file);
                entry.flags = kEntryFlags;
                entry.data = std::move(data);
                entries.push_back(std::move(entry));
            };
            size_t models = 0, choices = 0;
            for (size_t h = 0; h < p.options[0].size(); ++h)
                for (size_t b = 0; b < p.options[1].size(); ++b)
                    for (size_t l = 0; l < p.options[2].size(); ++l)
                    {
                        const std::string name = geometryName(int(h), int(b), int(l));
                        std::vector<uint8_t> asset;
                        writeChunk(asset, fourcc("HAST"), assetHead("S:\\bmx2\\riders\\createarider\\" + name + ".zit", name, kSceneKind));
                        writeChunk(asset, fourcc("UMDL"), composedModel({p.options[0][h].model, p.options[1][b].model, p.options[2][l].model}));
                        add(name + ".ZIF", library(name, {asset}));
                        ++models;
                    }
            for (int s = 0; s < kSections; ++s)
                for (size_t g = 0; g < p.options[s].size(); ++g)
                {
                    const std::string name = choicesName(s, int(g));
                    std::vector<std::vector<uint8_t>> assets;
                    const auto choiceAsset = [&](int subPart, int choice, const RiderTexture &t, const std::string &source) {
                        char suffix[8];
                        std::snprintf(suffix, sizeof(suffix), "%02d%02d", subPart, choice);
                        const std::string texture = name + suffix;
                        std::vector<uint8_t> material;
                        writeChunk(material, fourcc("HMTL"), kMaterialHead);
                        const std::vector<uint8_t> txrc = shrunkTexture(t.txrc, texture);
                        material.insert(material.end(), txrc.begin(), txrc.end());
                        std::vector<uint8_t> asset;
                        writeChunk(asset, fourcc("HAST"), assetHead(source, texture, kMaterialKind));
                        writeChunk(asset, fourcc("UMTL"), material);
                        assets.push_back(std::move(asset));
                    };
                    if (s == 0)
                        choiceAsset(kChainSubPart, 0, *p.options[s][g].choices.front(), p.options[s][g].sources.front());
                    for (size_t c = 0; c < p.options[s][g].choices.size(); ++c)
                        choiceAsset(kSectionList[s].subPart, int(c), *p.options[s][g].choices[c], p.options[s][g].sources[c]);
                    add(name + ".ZIF", library(name, assets));
                    ++choices;
                }
            bytes = zx::writeArchive(entries);
            std::cout << "[create-player] UILIB: the PlayerCreator screen, " << models << " models, " << choices
                      << " choice libraries; the main menu's " << kItem << std::endl;
            return true;
        }

        // ---- the screen's functions ------------------------------------------------
        //
        // The screen's own actions do the work on the game's side (the
        // creator's functions, the libraries' loads); these keep the row, the
        // counts and the labels, set the values the actions read, and name
        // the next one (Next: Slot, Geometry, Texture, Ride, Done or nothing).
        // They call the game only for what never waits (a variable read, SET).

        uint32_t stackBelow(R5900Context *ctx) { return (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu; }

        // A string in our scratch memory (one of a few places).
        uint32_t guestString(uint8_t *rdram, int place, const std::string &text)
        {
            const uint32_t at = state().scratch - kScratchBytes + static_cast<uint32_t>(place) * 0x100u;
            zge_host::settings::putString(rdram, at, text.substr(0, 0xFF));
            return at;
        }

        uint32_t findScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t root = zge_host::settings::readWord(rdram, sym::dm2::g_uiRoot);
            uint32_t node = 0;
            if (root)
                runtime->callGuestFunction(rdram, ctx, sym::dm2::findNode, {root, guestString(rdram, 1, "PlayerCreator")},
                                           state().scratch, &node);
            return node;
        }

        // A Sint32 variable of the screen, as the game's copyVariable reads it.
        int readVariable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &name)
        {
            State &st = state();
            const uint32_t value = st.scratch - kScratchBytes + kScratchStrings - 0x10u;
            zge_host::settings::putString(rdram, value, std::string(3, '\0'));
            runtime->callGuestFunction(rdram, ctx, sym::dm2::copyVariable, {st.screen, guestString(rdram, 0, name), value},
                                       st.scratch);
            return static_cast<int>(zge_host::settings::readWord(rdram, value));
        }

        void runLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::vector<std::string> &lines)
        {
            zge_host::settings::runScript(rdram, ctx, runtime, state().screen, lines);
        }

        std::string braced(const std::string &text) { return "{\"" + text + "\"}"; }

        void showRows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, std::vector<std::string> lines = {})
        {
            const State &st = state();
            const auto value = [&](int row, const std::string &text) {
                lines.push_back("SET Frame.Value" + std::to_string(row) + ".Text " + braced(text));
            };
            const auto count = [](int at, int most) { return "< " + std::to_string(at + 1) + "/" + std::to_string(most) + " >"; };
            value(kRowSlot, count(st.slot, kCreatedSlots));
            for (int s = 0; s < kSections; ++s)
            {
                value(kRowFirstGeometry + s, count(st.geometry[s], std::max(st.geometryMost[s], 1)));
                value(kRowFirstTexture + s, count(st.texture[s], std::max(st.textureMost[s], 1)));
            }
            char at[96];
            std::snprintf(at, sizeof(at), "SET Frame.Highlight.Position {140.0 %.1f 0.0}",
                          static_cast<double>(kRowTop + static_cast<float>(st.row) * kRowPitch));
            lines.push_back(at);
            runLines(rdram, ctx, runtime, lines);
        }

        enum class Key
        {
            Open,     // the screen shown: its first row, the first slot
            Textures, // InitializePlayerCreatorMenu has put the textures' values up
            Geometry, // and the geometry's
            Up,
            Down,
            Left,
            Right,
            Accept,
            Back
        };

        void key(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Key k)
        {
            State &st = state();
            if (k == Key::Open)
            {
                st.screen = findScreen(rdram, ctx, runtime);
                // Back from a ride's level select: the rider it was, on RIDE.
                const bool back = st.rideSlot >= 0;
                st.row = back ? kRowRide : 0;
                st.slot = back ? st.rideSlot : 0;
                st.rideSlot = -1;
                runLines(rdram, ctx, runtime,
                         {"SET Slot {" + std::to_string(kFirstCreated + st.slot) + "}", "SET OutfitType {0}"});
                return;
            }
            if (!st.screen)
                return;
            if (k == Key::Textures)
            {
                for (int s = 0; s < kSections; ++s)
                {
                    const std::string part = std::to_string(kSectionList[s].subPart);
                    st.texture[s] = readVariable(rdram, ctx, runtime, "TextureOption" + part);
                    st.textureMost[s] = readVariable(rdram, ctx, runtime, "MaxOption" + part);
                }
                return;
            }
            if (k == Key::Geometry)
            {
                std::vector<std::string> lines;
                for (int s = 0; s < kSections; ++s)
                {
                    st.geometry[s] = readVariable(rdram, ctx, runtime, "GeometryOption" + std::to_string(s));
                    st.geometryMost[s] = readVariable(rdram, ctx, runtime, "MaxOption" + std::to_string(s));
                    static const char *const kLib[kSections] = {"Head", "Body", "Legs"};
                    const std::string name = choicesName(s, st.geometry[s]);
                    lines.push_back(std::string("SET ") + kLib[s] + "Lib " + braced(name));
                    lines.push_back(std::string("SET ") + kLib[s] + "Path " + braced("UILIB:" + name + ".ZIF"));
                }
                const std::string model = geometryName(st.geometry[0], st.geometry[1], st.geometry[2]);
                lines.push_back("SET ModelLib " + braced(model));
                lines.push_back("SET ModelPath " + braced("UILIB:" + model + ".ZIF"));
                lines.push_back("SET ModelAsset " + braced(model + "." + model));
                return showRows(rdram, ctx, runtime, lines);
            }
            const auto step = [](int value, int most, int by) { return most > 0 ? (value + by + most) % most : 0; };
            std::string next = "None";
            std::vector<std::string> lines;
            switch (k)
            {
            case Key::Up:
            case Key::Down:
                st.row = step(st.row, kRows, k == Key::Up ? -1 : 1);
                break;
            case Key::Left:
            case Key::Right:
            {
                const int by = k == Key::Left ? -1 : 1;
                if (st.row == kRowSlot)
                {
                    st.slot = step(st.slot, kCreatedSlots, by);
                    lines.push_back("SET Slot {" + std::to_string(kFirstCreated + st.slot) + "}");
                    next = "Slot";
                }
                else if (st.row >= kRowFirstGeometry && st.row < kRowFirstGeometry + kSections)
                {
                    // The game takes the new geometry (and starts its textures over).
                    const int s = st.row - kRowFirstGeometry;
                    st.geometry[s] = step(st.geometry[s], st.geometryMost[s], by);
                    for (int i = 0; i < kSections; ++i)
                        lines.push_back("SET GeometryOption" + std::to_string(i) + " {" + std::to_string(st.geometry[i]) + "}");
                    next = "Geometry";
                }
                else if (st.row >= kRowFirstTexture && st.row < kRowFirstTexture + kSections)
                {
                    const int s = st.row - kRowFirstTexture;
                    st.texture[s] = step(st.texture[s], st.textureMost[s], by);
                    for (int i = 0; i < kSections; ++i)
                        lines.push_back("SET TextureOption" + std::to_string(kSectionList[i].subPart) + " {" +
                                        std::to_string(st.texture[i]) + "}");
                    next = "Texture";
                }
                break;
            }
            case Key::Accept:
                if (st.row == kRowRide)
                    next = "Ride";
                else if (st.row == kRowDone)
                    next = "Done";
                break;
            case Key::Back:
                next = "Done";
                break;
            default:
                break;
            }
            lines.push_back("SET Next " + braced(next));
            if (next == "Ride")
            {
                st.rideSlot = st.slot;
                std::cout << "[create-player] ride: character " << kFirstCreated + st.slot << std::endl;
            }
            showRows(rdram, ctx, runtime, lines);
        }

        template <Key K>
        void screenFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            key(rdram, ctx, runtime, K);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // bmxPlayerCreatorInitialize as a script function (the screen's
        // OnCreate calls it): the game's own, run as its caller's tail call.
        void creatorStart(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::cout << "[create-player] bmxPlayerCreatorInitialize" << std::endl;
            ctx->pc = sym::dm2::bmxPlayerCreatorInitialize;
        }

        // ---- the game's own points -------------------------------------------------

        // frontEndExit: the screen goes with the main menu; the libraries a
        // created rider's model was made from (the level before) let go.
        void frontEndExitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            if (const uint32_t screen = findScreen(rdram, ctx, runtime))
                runtime->callGuestFunction(rdram, ctx, sym::dm2::deleteSceneObject, {screen}, stackBelow(ctx));
            st.screen = 0;
            for (const std::string &name : st.riderLibraries)
                runtime->callGuestFunction(rdram, ctx, sym::dm2::unloadUiLibraryByName, {guestString(rdram, 2, name)},
                                           stackBelow(ctx));
            st.riderLibraries.clear();
            st.rideSlot = -1;
            st.frontEndExit(rdram, ctx, runtime);
        }

        // bmxCheckCheat (a script native, the main menu's cheat keys): the
        // game's run, then what it opened for every one of the game's riders
        // the created riders get (cheats::shareWithCreatedRiders).
        constexpr uint32_t kRiderRecordsBytes = kCharacterBytes * cheats::kRiderRecords;
        void checkCheatHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            std::vector<uint8_t> before(kRiderRecordsBytes);
            if (const uint8_t *records = getConstMemPtr(rdram, sym::dm2::g_riderCharacters))
                std::memcpy(before.data(), records, before.size());
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, st.checkCheat, sym::dm2::bmxCheckCheat,
                                       {getRegU32(ctx, kRegA0), getRegU32(ctx, kRegA1)}, getRegU32(ctx, kRegSp), &v0);
            setReturnU32(ctx, v0);
            ctx->pc = getRegU32(ctx, kRegRa);
            uint8_t *records = getMemPtr(rdram, sym::dm2::g_riderCharacters);
            if (!records || std::memcmp(records, before.data(), before.size()) == 0)
                return;
            if (const size_t given = cheats::shareWithCreatedRiders(records, before.data()))
                std::cout << "[create-player] a cheat: the created riders get what it opened for every rider (" << given
                          << " things)" << std::endl;
        }

        // newCreatedRiderModel makes the model from its libraries and lets
        // them go (its geometry's after the clone, the choices' after the
        // textures are set): those four unloads are kept until the front
        // end's next exit, so the model keeps what it was made of.
        void newCreatedRiderModelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.keepUnloads = 1 + kSections;
            std::cout << "[create-player] character " << kFirstCreated + getRegU32(ctx, kRegA0) << "'s model" << std::endl;
            st.newCreatedRiderModel(rdram, ctx, runtime);
        }

        void unloadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            if (st.keepUnloads > 0)
            {
                --st.keepUnloads;
                st.riderLibraries.push_back(zge_host::settings::getString(rdram, getRegU32(ctx, kRegA0), 0x20));
                setReturnU32(ctx, 1u);
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            st.unloadLibrary(rdram, ctx, runtime);
        }
    }

    bool install(PS2Runtime &runtime)
    {
        const char *on = ps2x::boot::text("PS2X_DM2_CREATE_PLAYER");
        if (!on || on[0] != '1')
            return false;
        // A created rider's model is the readable newRider's (game/dm2/rider/rider_state.cpp): the original makes
        // none for characters 17 .. 20, and a ride with such a rider hangs at END RUN. With the readable newRider
        // off (PS2X_READABLE=off, -dm2:newRider) the menu is not added: the game as it shipped.
        if (!game::readable::runsInGame(sym::dm2::newRider, game::Game::Dm2))
        {
            std::cout << "[create-player] off: created riders need the readable newRider (PS2X_READABLE turns it off; "
                         "the original makes no model for them)"
                      << std::endl;
            return false;
        }
        State &st = state();
        st.scratch = runtime.reserveAsyncCallbackStack(kScratchBytes, 16u);
        if (!st.scratch || !zge_host::settings::installScriptRunner(runtime, sym::dm2::runScript))
        {
            std::cerr << "[create-player] scratch memory not reserved" << std::endl;
            return false;
        }
        static constexpr struct
        {
            const char *name;
            PS2Runtime::RecompiledFunction fn;
        } kFunctions[] = {
            {"bmxPlayerCreatorInitialize", &creatorStart},
            {"pcCreatorOpen", &screenFunction<Key::Open>},         {"pcCreatorTextures", &screenFunction<Key::Textures>},
            {"pcCreatorGeometry", &screenFunction<Key::Geometry>}, {"pcCreatorUp", &screenFunction<Key::Up>},
            {"pcCreatorDown", &screenFunction<Key::Down>},         {"pcCreatorLeft", &screenFunction<Key::Left>},
            {"pcCreatorRight", &screenFunction<Key::Right>},       {"pcCreatorAccept", &screenFunction<Key::Accept>},
            {"pcCreatorBack", &screenFunction<Key::Back>},
        };
        for (const auto &function : kFunctions)
            if (!zge_host::addScriptFunction(runtime, scriptFunctions(), function.name, function.fn))
                return false;
        const auto hook = [&](uint32_t at, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction fn) {
            return (original = runtime.lookupFunction(at)) && runtime.replaceFunction(at, fn);
        };
        if (!hook(sym::dm2::frontEndExit, st.frontEndExit, &frontEndExitHook) ||
            !hook(sym::dm2::newCreatedRiderModel, st.newCreatedRiderModel, &newCreatedRiderModelHook) ||
            !hook(sym::dm2::unloadUiLibraryByName, st.unloadLibrary, &unloadHook) ||
            !hook(sym::dm2::bmxCheckCheat, st.checkCheat, &checkCheatHook))
        {
            std::cerr << "[create-player] hooks not installed" << std::endl;
            return false;
        }
        ps2x::disc_patch::registerPatch("CREATEPL.DAT", &patchCreatorData);
        ps2x::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        st.on = true;
        std::cout << "[create-player] on: the main menu's " << kItem << std::endl;
        return true;
    }
}

namespace dave_mirra::cheats
{
    uint32_t ridersCovered() { return creator::state().on ? kRiderRecords : kGameCheatRiders; }
}

namespace dave_mirra
{
    bool installCreatePlayer(PS2Runtime &runtime) { return creator::install(runtime); }
    bool createPlayerCanRun() { return game::readable::switchedOnAtBoot(sym::dm2::newRider, game::Game::Dm2); }
}
