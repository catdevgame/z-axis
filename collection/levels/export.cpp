// A level as a model in the converters' terms (collection/levels/export.h),
// ported from the tooling's zge_level_gltf.py (the reference).
#include "collection/levels/export.h"

#include "collection/levels/scene.h"
#include "collection/levels/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace zx::app::levels
{
    namespace
    {
        // What differs between the three games' level files for the export.
        struct GameForm
        {
            Engine engine;
            size_t matiTexture;     // the texture id's offset in a MATI's one layer
            size_t matiLayer;       // the first layer's four state words
            size_t flagsSize;       // the MATI's flags word (bit 1: drawn on both sides)
            int matiSound;          // the material sound byte's offset, -1 when the game does not name its values
            bool colourKey;         // TBMP texels of magenta are transparent (DM2)
        };

        GameForm formOf(Engine engine)
        {
            switch (engine)
            {
            case Engine::Zge2:
                return {engine, 0x3C, 0x2C, 2, -1, true};
            case Engine::Zge4:
                return {engine, 0x40, 0x30, 4, 0x1C, false};
            case Engine::Zge3:
            default:
                return {engine, 0x40, 0x30, 4, -1, false};
            }
        }

        constexpr Rgba kKey = {255, 0, 255, 255};
        constexpr uint32_t kInstanceShown = 0x01;
        constexpr uint32_t kInstanceSky = 0x10;
        constexpr uint32_t kInstanceMirrored = 0x400;

        // The spawns a game reads by name and what each is in the model
        // (#spawn=<role>): the starts, and the points a fallen player is put
        // back at (AI's "Respawn", DM2's "Player", BMX XXX's any but the
        // starts and paintball's). Empty: a name only a game's scripts or
        // modes read (AI's lesson points, BMX XXX's sniper points: namedPoints).
        // The named points a game's scripts or modes read, carried by name
        // (#spawn=<name>: the converters write each for its own game,
        // convert.cpp scriptSpawnGame): AI's lesson points and wickedTree,
        // BMX XXX's paintball points.
        const std::map<uint32_t, std::string> &namedPoints()
        {
            static const std::map<uint32_t, std::string> points = []
            {
                std::map<uint32_t, std::string> out;
                for (const ScriptSpawns &game : scriptSpawns())
                    for (const std::string &name : game.names)
                        out.emplace(nameHash(name), name);
                return out;
            }();
            return points;
        }

        std::string spawnRole(Engine engine, uint32_t name)
        {
            static const uint32_t respawn = nameHash("Respawn"), player = nameHash("Player");
            static const uint32_t shooter = nameHash("sniper_shooter"), target = nameHash("sniper_target");
            if (name == kPlayerOne)
                return "PlayerOne";
            if (name == kPlayerTwo)
                return "PlayerTwo";
            if ((engine == Engine::Zge3 && name == respawn) || (engine == Engine::Zge2 && name == player) ||
                (engine == Engine::Zge4 && name != shooter && name != target))
                return "respawn";
            return {};
        }

        // What DM2 adds to an idle camera point's position for its eye (its
        // level-point camera stands 5 m above it; y grows down).
        constexpr double kDm2CameraLift = -5.0;

        Vec3 toModel(const Vec3 &p) { return {p[0], -p[1], -p[2]}; }
        Quat quatToModel(const Quat &q) { return {q[0], -q[1], -q[2], q[3]}; }

        std::vector<double> unpackFloats(const Bytes &data, size_t at, size_t count)
        {
            std::vector<double> out;
            for (size_t i = 0; i < count; ++i)
                out.push_back(readF32(data, at + 4 * i));
            return out;
        }

        // A part's stream as its bytes (an indexed one expanded), or none
        // when it is missing or shorter than `count` elements.
        std::optional<Bytes> streamBytes(const Part &part, StreamKind kind, size_t count, size_t stride)
        {
            const std::optional<Stream> &s = part.stream(kind);
            if (!s)
                return std::nullopt;
            Bytes data = s->bytes();
            if (data.size() < count * stride)
                return std::nullopt;
            return data;
        }

        // A texture level's BMP as RGBA rows, top first: 4 and 8 bits (a
        // TBMP opaque, its palette's alpha 0; a TZMP its palette's alpha), 24
        // and 32 bits uncompressed; DM2's magenta its key.
        Image decodeBmp(const Bytes &data, bool colourKey, bool withAlpha)
        {
            if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
                throw LevelError("not a BMP");
            const uint16_t bpp = readU16(data, 28);
            const uint32_t pixels = readU32(data, 10);
            const int32_t width = readS32(data, 18), height = readS32(data, 22);
            const int rowsCount = std::abs(height);
            Image img;
            img.width = width;
            img.height = rowsCount;
            if (bpp == 4 || bpp == 8)
            {
                const uint32_t head = readU32(data, 14);
                uint32_t colours = readU32(data, 46);
                if (!colours)
                    colours = 1u << bpp;
                std::vector<Rgba> palette;
                for (uint32_t i = 0; i < colours; ++i)
                {
                    const size_t at = 14 + head + 4 * i;
                    palette.push_back({readU8(data, at + 2), readU8(data, at + 1), readU8(data, at),
                                       static_cast<uint8_t>(withAlpha ? readU8(data, at + 3) : 255)});
                }
                const size_t stride = (static_cast<size_t>(width) * bpp + 31) / 32 * 4;
                for (int y = 0; y < rowsCount; ++y)
                {
                    const size_t at = pixels + static_cast<size_t>(height > 0 ? rowsCount - 1 - y : y) * stride;
                    for (int x = 0; x < width; ++x)
                    {
                        const uint8_t byte = readU8(data, at + (bpp == 8 ? static_cast<size_t>(x) : static_cast<size_t>(x >> 1)));
                        const uint8_t index = bpp == 8 ? byte : (x % 2 == 0 ? byte >> 4 : byte & 15);
                        if (index >= palette.size())
                            throw LevelError("a BMP pixel past its palette");
                        img.pixels.push_back(palette[index]);
                    }
                }
            }
            else if (bpp == 24 || bpp == 32)
            {
                const size_t step = bpp / 8;
                const size_t stride = (static_cast<size_t>(width) * bpp / 8 + 3) & ~size_t(3);
                for (int y = 0; y < rowsCount; ++y)
                {
                    const size_t at = pixels + static_cast<size_t>(height > 0 ? rowsCount - 1 - y : y) * stride;
                    for (int x = 0; x < width; ++x)
                    {
                        const size_t p = at + step * static_cast<size_t>(x);
                        img.pixels.push_back({readU8(data, p + 2), readU8(data, p + 1), readU8(data, p),
                                              static_cast<uint8_t>(bpp == 32 && withAlpha ? readU8(data, p + 3) : 255)});
                    }
                }
            }
            else
                throw LevelError(std::to_string(bpp) + "-bit BMP");
            if (colourKey && !withAlpha)
                for (Rgba &px : img.pixels)
                    if (px[0] == kKey[0] && px[1] == kKey[1] && px[2] == kKey[2])
                        px = {0, 0, 0, 0};
            return img;
        }

        // AI's FaceTag kinds -> the converters' tags: 1 quarter pipe, 2 spine,
        // 3 slope, 8 wall ride.
        const char *aiSurface(int kind)
        {
            switch (kind)
            {
            case 1: return "qp";
            case 2: return "spine";
            case 3: return "slope";
            case 8: return "wall";
            default: return nullptr;
            }
        }

        // The bike games' surface kinds: 1 quarter pipe, 2 spine, 3 kicker,
        // and BMX XXX's 8, a wall.
        const char *bikeSurface(Engine engine, int kind)
        {
            switch (kind)
            {
            case 1: return "qp";
            case 2: return "spine";
            case 3: return "kicker";
            case 8: return engine == Engine::Zge4 ? "wall" : nullptr;
            default: return nullptr;
            }
        }

        // AI's EdgeTag kinds -> #grind words (any other grindable kind a ledge).
        const char *aiGrind(int kind)
        {
            switch (kind)
            {
            case 1: case 9: return "lip";
            case 2: return "spine";
            case 3: return "vpole";
            case 4: return "hpole";
            default: return "ledge";
            }
        }

        // AI's edge material -> the grind sound: 1 wood, 2 and 8 metal.
        std::optional<std::string> aiEdgeSound(int material)
        {
            if (material == 1)
                return "wood";
            if (material == 2 || material == 8)
                return "metal";
            return std::nullopt;
        }

        const char *bikeGrind(int linkKind)
        {
            switch (linkKind)
            {
            case 1: return "lip";
            case 2: return "spine";
            default: return "ledge";
            }
        }

        std::string hexBytes(const Bytes &data)
        {
            static const char digits[] = "0123456789abcdef";
            std::string out;
            for (uint8_t b : data)
            {
                out.push_back(digits[b >> 4]);
                out.push_back(digits[b & 15]);
            }
            return out;
        }

        using Tags = std::vector<std::string>;

        struct MaterialLook
        {
            bool bothSides = false;
            std::optional<Bytes> state; // the first layer's 16 bytes when it blends or tests alpha
        };

        struct MaterialLayer
        {
            std::optional<uint32_t> texture;
            std::optional<std::string> sound;
        };

        class Export
        {
        public:
            Export(Engine engine, const LevelFiles &files) : m_form(formOf(engine)), m_files(files) {}

            Model run();

        private:
            const Bytes &file(const char *ext) const
            {
                auto it = m_files.find(ext);
                if (it == m_files.end())
                    throw LevelError(std::string("the level has no ") + ext);
                return it->second;
            }

            void readTextures();
            std::optional<int> texture(uint32_t id);
            std::string imageName(const std::string &name) const;
            MaterialLook materialLook(const Chunk &mati) const;
            MaterialLayer materialLayer(const Chunk &mati) const;
            int material(std::optional<uint32_t> id, const Tags &tags);
            Tags surfaceTags(const std::optional<Bytes> &record);
            std::optional<int> meshOf(const std::string &name, const Part &part, const std::optional<Chunk> &table,
                                      const std::vector<int> &faces, bool withTags, MeshExtras extras);
            void grindEdges(const Part &part, const std::optional<Chunk> &table, const Vec3 &position, const Quat &rotation);

            GameForm m_form;
            const LevelFiles &m_files;
            ModelBuilder m_builder;
            struct LibraryEntry
            {
                std::string name;
                std::vector<const Chunk *> levels;
            };
            std::vector<Chunk> m_txr;
            std::map<uint32_t, LibraryEntry> m_library;
            std::map<uint32_t, std::optional<int>> m_textures;
            std::map<std::pair<std::optional<uint32_t>, Tags>, int> m_materials;
            // (grind word, sound) -> segments, in the model's axes
            // the grind edges by (#grind word, sound, tag word)
            std::map<std::tuple<std::string, std::string, uint32_t>, std::vector<std::pair<Vec3, Vec3>>> m_edges;
        };

        void Export::readTextures()
        {
            m_txr = parseChunks(file("TXR"));
            if (m_txr.size() != 1 || m_txr[0].tag != tag("FORM"))
                throw LevelError("a TXR is one FORM");
            for (const Chunk &chunk : m_txr[0].kids)
            {
                if (chunk.kids.empty())
                    throw LevelError("a texture without its header");
                const Bytes &head = chunk.kids[0].data;
                LibraryEntry entry;
                entry.name = cString(head, 0x14, 0x20);
                for (size_t i = 1; i < chunk.kids.size(); ++i)
                    entry.levels.push_back(&chunk.kids[i]);
                m_library[readU32(head, 0)] = std::move(entry);
            }
        }

        std::optional<int> Export::texture(uint32_t id)
        {
            if (auto it = m_textures.find(id); it != m_textures.end())
                return it->second;
            const LibraryEntry &entry = m_library.at(id);
            std::optional<int> index;
            if (!entry.levels.empty() && (entry.levels[0]->tag == tag("TBMP") || entry.levels[0]->tag == tag("TZMP")))
            {
                const Bytes &data = entry.levels[0]->data;
                const uint32_t size = readU32(data, 2);
                try
                {
                    Image img = decodeBmp(slice(data, 0, size), m_form.colourKey, entry.levels[0]->tag == tag("TZMP"));
                    char fallback[16];
                    std::snprintf(fallback, sizeof fallback, "tex%08x", id);
                    index = m_builder.image(imageName(entry.name.empty() ? fallback : entry.name), img.width, img.height,
                                            std::move(img.pixels));
                }
                catch (const LevelError &)
                {
                    // a texture the export cannot decode is left out
                }
            }
            m_textures[id] = index;
            return index;
        }

        std::string Export::imageName(const std::string &name) const
        {
            std::string base;
            for (char c : name)
                base.push_back(isAlnumLatin1(static_cast<uint8_t>(c)) || c == '_' || c == '-' ? c : '_');
            if (base.empty())
                base = "texture";
            std::string out = base;
            for (int n = 2; m_builder.model().images.end() != std::find_if(m_builder.model().images.begin(), m_builder.model().images.end(),
                                                                         [&out](const Image &i) { return i.name == out; });
                 ++n)
                out = base + "_" + std::to_string(n);
            return out;
        }

        MaterialLook Export::materialLook(const Chunk &mati) const
        {
            const Bytes &data = mati.data;
            uint32_t flags = 0;
            for (size_t i = 0; i < m_form.flagsSize && i < data.size(); ++i)
                flags |= uint32_t(data[i]) << (8 * i);
            MaterialLook look;
            look.bothSides = flags & 2;
            const Bytes state = slice(data, m_form.matiLayer, 16);
            if (state.size() < 16)
                return look;
            const bool blends = readU32(state, 0) & 0x20;     // ABE
            const bool tests = readU32(state, 12) & (1u << 10); // ATE
            if (blends || tests)
                look.state = state;
            return look;
        }

        MaterialLayer Export::materialLayer(const Chunk &mati) const
        {
            const Bytes &data = mati.data;
            MaterialLayer layer;
            if (data.size() >= m_form.matiTexture + 4)
                layer.texture = readU32(data, m_form.matiTexture);
            if (!layer.texture || !m_library.count(*layer.texture))
            {
                layer.texture.reset();
                for (size_t at = 0; at + 3 < data.size(); at += 4)
                    if (m_library.count(readU32(data, at)))
                    {
                        layer.texture = readU32(data, at);
                        break;
                    }
            }
            if (m_form.matiSound >= 0 && data.size() > static_cast<size_t>(m_form.matiSound))
            {
                const uint8_t sound = data[static_cast<size_t>(m_form.matiSound)];
                if (sound == 1)
                    layer.sound = "metal";
                else if (sound == 2)
                    layer.sound = "wood";
            }
            return layer;
        }

        int Export::material(std::optional<uint32_t> id, const Tags &tags)
        {
            const auto key = std::make_pair(id, tags);
            if (auto it = m_materials.find(key); it != m_materials.end())
                return it->second;
            const std::optional<int> tex = id ? texture(*id) : std::nullopt;
            std::string name = id ? m_library.at(*id).name : std::string("untextured");
            for (const std::string &t : tags)
                name += "#" + t;
            const Colour colour = tex ? Colour{1.0, 1.0, 1.0, 1.0} : Colour{0.5, 0.5, 0.5, 1.0};
            const int index = m_builder.material(std::move(name), colour, tex);
            m_materials[key] = index;
            return index;
        }

        // The converters' tags for a face's surface kind (none for ground).
        Tags kindTags(Engine engine, const Bytes &record)
        {
            if (engine == Engine::Zge3)
            {
                const FaceTag t = FaceTag::read(record);
                const char *word = aiSurface(t.kind);
                if (!word)
                    return {};
                if (std::string(word) == "slope")
                    return {"slope=" + formatG(roundTo(t.gravity, 4))};
                return {word};
            }
            const BikeFaceTag t = BikeFaceTag::read(record);
            const char *word = bikeSurface(engine, t.surfaceKind);
            if (!word)
                return {};
            const std::string w = word;
            if (w == "kicker")
                return {"kicker=" + std::to_string(t.launch), "launch=" + std::to_string(t.launch)};
            if (w == "qp")
                return {"qp", "launch=" + std::to_string(t.launch)};
            return {w};
        }

        // A tag word as the converters' #tag=0x... has it.
        std::string tagWordText(uint32_t word)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08x", word);
            return text;
        }

        // The converters' tags for a face's tag record: its surface kind,
        // then its tag word (the word the level's goals and events find the
        // face by) as #tag=0x...
        Tags Export::surfaceTags(const std::optional<Bytes> &record)
        {
            if (!record)
                return {};
            Tags tags = kindTags(m_form.engine, *record);
            const uint32_t word = m_form.engine == Engine::Zge3 ? FaceTag::read(*record).eventKind : BikeFaceTag::read(*record).tag;
            if (word)
                tags.push_back("tag=" + tagWordText(word));
            return tags;
        }

        std::optional<int> Export::meshOf(const std::string &name, const Part &part, const std::optional<Chunk> &table,
                                          const std::vector<int> &faces, bool withTags, MeshExtras extras)
        {
            const size_t count = part.positions.size();
            const std::optional<Bytes> normals = streamBytes(part, StreamKind::Normals, count, 12);
            const std::optional<Bytes> uvs = streamBytes(part, StreamKind::Uvs, count, 8);
            const std::optional<Bytes> colours = streamBytes(part, StreamKind::Colours, count, 4);
            std::vector<MaterialLayer> layers;
            std::vector<MaterialLook> looks;
            if (table)
                for (const Chunk &k : table->kids)
                    if (k.tag == tag("MATI"))
                    {
                        layers.push_back(materialLayer(k));
                        looks.push_back(materialLook(k));
                    }
            std::vector<std::optional<Bytes>> records(part.faces.size());
            if (withTags && part.faceTags)
                for (size_t f = 0; f < part.faceTags->indices.size() && f < part.faces.size(); ++f)
                    if (part.faceTags->indices[f] >= 0)
                        records[f] = part.faceTags->records.at(static_cast<size_t>(part.faceTags->indices[f]));
            // faces by material, in the order the materials first appear
            std::vector<std::pair<int, std::vector<int>>> byMaterial;
            for (int f : faces)
            {
                const Face &face = part.faces[static_cast<size_t>(f)];
                const auto &v = face.vertices;
                if (std::max({v[0], v[1], v[2]}) >= count || v[0] == v[1] || v[1] == v[2] || v[0] == v[2])
                    continue;
                const size_t m = static_cast<size_t>(face.material());
                const MaterialLayer layer = m < layers.size() ? layers[m] : MaterialLayer{};
                Tags tags = withTags ? surfaceTags(records[static_cast<size_t>(f)]) : Tags{};
                const MaterialLook look = m < looks.size() ? looks[m] : MaterialLook{};
                if (look.bothSides)
                    tags.push_back("2sided");
                else if (face.flags & Face::kTwoSided)
                    tags.push_back("2sided=collide");
                if (look.state)
                    tags.push_back("layer=" + hexBytes(*look.state));
                if (layer.sound)
                    tags.push_back(*layer.sound);
                const int mat = material(layer.texture, tags);
                auto it = std::find_if(byMaterial.begin(), byMaterial.end(), [mat](const auto &e) { return e.first == mat; });
                if (it == byMaterial.end())
                {
                    byMaterial.emplace_back(mat, std::vector<int>{});
                    it = byMaterial.end() - 1;
                }
                it->second.push_back(f);
            }
            std::vector<Primitive> prims;
            for (const auto &[mat, fs] : byMaterial)
            {
                Primitive p;
                std::vector<Vec3> ns;
                std::vector<Vec2> us;
                std::vector<Colour> cs;
                std::map<int, int> remap;
                for (int f : fs)
                {
                    std::array<int, 3> tri{};
                    for (int k = 0; k < 3; ++k)
                    {
                        const int v = part.faces[static_cast<size_t>(f)].vertices[static_cast<size_t>(k)];
                        auto it = remap.find(v);
                        if (it == remap.end())
                        {
                            it = remap.emplace(v, static_cast<int>(p.positions.size())).first;
                            p.positions.push_back(toModel(part.positions[static_cast<size_t>(v)]));
                            if (normals)
                                ns.push_back(toModel(readFloats<3>(*normals, 12 * static_cast<size_t>(v))));
                            if (uvs)
                                us.push_back(readFloats<2>(*uvs, 8 * static_cast<size_t>(v)));
                            if (colours)
                            {
                                Colour c{};
                                for (int ch = 0; ch < 4; ++ch)
                                    c[static_cast<size_t>(ch)] = (*colours)[4 * static_cast<size_t>(v) + static_cast<size_t>(ch)] / (ch == 3 ? 255.0 : 128.0);
                                cs.push_back(c);
                            }
                            else
                                cs.push_back({1.0, 1.0, 1.0, 1.0});
                        }
                        tri[static_cast<size_t>(k)] = it->second;
                    }
                    p.triangles.push_back(tri);
                }
                if (!ns.empty())
                    p.normals = std::move(ns);
                if (!us.empty())
                    p.uvs = std::move(us);
                p.colours = std::move(cs);
                p.material = mat;
                prims.push_back(std::move(p));
            }
            if (prims.empty())
                return std::nullopt;
            return m_builder.mesh(name, std::move(prims), std::move(extras));
        }

        void Export::grindEdges(const Part &part, const std::optional<Chunk> &table, const Vec3 &position, const Quat &rotation)
        {
            if (!part.edgeTags)
                return;
            std::vector<Vec3> world;
            for (const Vec3 &p : part.positions)
            {
                const Vec3 turned = quatRotate(rotation, p);
                world.push_back(toModel({turned[0] + position[0], turned[1] + position[1], turned[2] + position[2]}));
            }
            std::vector<std::optional<std::string>> sounds;
            if (table)
                for (const Chunk &k : table->kids)
                    if (k.tag == tag("MATI"))
                        sounds.push_back(materialLayer(k).sound);
            const TagBuffer &tags = *part.edgeTags;
            for (size_t f = 0; f < part.faces.size(); ++f)
            {
                const Face &face = part.faces[f];
                for (size_t k = 0; k < 3; ++k)
                {
                    const size_t at = 3 * f + k;
                    if (at >= tags.indices.size() || tags.indices[at] < 0)
                        continue;
                    const Bytes &record = tags.records.at(static_cast<size_t>(tags.indices[at]));
                    std::string word;
                    std::optional<std::string> sound;
                    uint32_t tagWord = 0; // the word the level's goals find the edge by
                    if (m_form.engine == Engine::Zge3)
                    {
                        const EdgeTag r = EdgeTag::read(record);
                        if (!(r.flags & EdgeTag::kGrindable) && r.kind != 3)
                            continue; // (a word on an edge not ground is left out)
                        word = aiGrind(r.kind);
                        sound = aiEdgeSound(r.material);
                        tagWord = r.eventId;
                    }
                    else
                    {
                        const BikeEdgeTag r = BikeEdgeTag::read(record);
                        if (!r.grindable())
                            continue;
                        word = bikeGrind(r.linkKind);
                        sound = static_cast<size_t>(face.material()) < sounds.size() ? sounds[static_cast<size_t>(face.material())]
                                                                                    : std::nullopt;
                        tagWord = r.tag;
                    }
                    const uint16_t a = face.vertices[k], b = face.vertices[(k + 1) % 3];
                    if (std::max(a, b) < world.size())
                        m_edges[{word, sound.value_or(""), tagWord}].emplace_back(world[a], world[b]);
                }
            }
        }

        Model Export::run()
        {
            readTextures();
            const Engine engine = m_form.engine;
            const Geometry geo = Geometry::read(file("GEO"), engine);
            const Instances ins = Instances::read(file("INS"), engine);
            const SceneData dat = SceneData::read(file("DAT"), engine);
            std::map<uint32_t, const GeoObject *> objects;
            for (const GeoObject &o : geo.objects)
                objects[o.header.id] = &o;
            std::map<uint32_t, const Instance *> byId;
            for (const Instance &i : ins.instances)
                byId[i.instanceId] = &i;
            // The dynamics definitions (each ZDYN's chunks after its DDEF), by
            // the name hash their DDEF holds: a prop object's.
            std::map<uint32_t, std::vector<Chunk>> definitions;
            for (const auto &[place, k] : geo.other)
                if (k.tag == tag("ZDYN") && !k.kids.empty() && k.kids[0].tag == tag("DDEF"))
                    definitions.emplace(readU32(k.kids[0].data, 0), std::vector<Chunk>(k.kids.begin() + 1, k.kids.end()));
            // A dynamics prop: an object of the DynamicsObject type with its definition.
            auto isProp = [&](const GeoObject &obj)
            {
                return obj.typeData && obj.typeData->size() >= 4 && readU32(*obj.typeData, 0) == kDynamicsObjectClass &&
                       definitions.count(obj.header.nameHash);
            };
            // The mesh's extras: the stock name hash where the name field cut
            // the name, and a prop's definition with its class data and the
            // instance's tags (those of the instance its mesh is made for).
            auto objectExtras = [&](const GeoObject &obj, const Instance &inst)
            {
                MeshExtras extras;
                if (obj.header.nameHash != nameHash(obj.header.name))
                    extras.nameHash = obj.header.nameHash;
                if (isProp(obj))
                    extras.dynamics = DynamicsDefinition{definitions.at(obj.header.nameHash), engine, slice(*obj.typeData, 4),
                                                         inst.tags.value_or(Bytes{})};
                return extras;
            };

            std::map<std::pair<uint32_t, std::string>, std::optional<int>> meshes; // (object id, role)
            for (const Instance &inst : ins.instances)
            {
                auto found = objects.find(inst.geometryId);
                if (found == objects.end() || found->second->parts.empty())
                    continue;
                const GeoObject &obj = *found->second;
                const bool shown = inst.flags & kInstanceShown;
                const bool sky = inst.flags & kInstanceSky;
                const bool collides = !sky && (inst.flags & Instance::kCollides);
                if (!shown && !collides)
                    continue;
                (void)kInstanceMirrored; // placed unmirrored, as the reference does
                const ObjectPart *drawn = &obj.parts[0];
                for (const ObjectPart &p : obj.parts)
                    if (p.lod)
                    {
                        drawn = &p;
                        break;
                    }
                const ObjectPart *hit = collides ? obj.collisionPart() : nullptr;
                // the world transform: the instance in its parents' space
                Vec3 position = inst.position;
                Quat rotation = inst.rotation;
                const Instance *parent = inst.parent ? (byId.count(inst.parent) ? byId[inst.parent] : nullptr) : nullptr;
                for (int depth = 0; depth < 16 && parent; ++depth)
                {
                    const Vec3 turned = quatRotate(parent->rotation, position);
                    position = {turned[0] + parent->position[0], turned[1] + parent->position[1], turned[2] + parent->position[2]};
                    rotation = quatMultiply(parent->rotation, rotation);
                    parent = parent->parent ? (byId.count(parent->parent) ? byId[parent->parent] : nullptr) : nullptr;
                }
                std::string nodeName = inst.name;
                if (nodeName.empty())
                    nodeName = obj.header.name;
                if (nodeName.empty())
                {
                    char text[16];
                    std::snprintf(text, sizeof text, "inst%08x", inst.instanceId);
                    nodeName = text;
                }
                struct Role
                {
                    const char *role;
                    const ObjectPart *part;
                    std::vector<int> faces;
                    bool withTags;
                };
                auto allFaces = [](const ObjectPart *p)
                {
                    std::vector<int> fs(p->part.faces.size());
                    for (size_t i = 0; i < fs.size(); ++i)
                        fs[i] = static_cast<int>(i);
                    return fs;
                };
                auto facesWhere = [](const ObjectPart *p, bool collide)
                {
                    std::vector<int> fs;
                    for (size_t i = 0; i < p->part.faces.size(); ++i)
                        if (bool(p->part.faces[i].flags & Face::kCollides) == collide)
                            fs.push_back(static_cast<int>(i));
                    return fs;
                };
                std::vector<Role> roles;
                if (isProp(obj) && collides)
                    roles.push_back({"dynamics", drawn, allFaces(drawn), true}); // its body collides for it in the game
                else if (sky)
                    roles.push_back({"sky", drawn, allFaces(drawn), false});
                else if (!hit || hit != drawn)
                {
                    if (shown)
                        roles.push_back({"deco", drawn, allFaces(drawn), false});
                    if (hit)
                        roles.push_back({"hidden", hit, facesWhere(hit, true), true});
                }
                else
                {
                    std::vector<int> loose = facesWhere(hit, false);
                    roles.push_back({shown ? "solid" : "hidden", hit, facesWhere(hit, true), true});
                    if (!loose.empty() && shown)
                        roles.push_back({"deco", hit, std::move(loose), false});
                }
                for (const Role &r : roles)
                {
                    const auto key = std::make_pair(obj.header.id, std::string(r.role));
                    if (!meshes.count(key))
                    {
                        std::string meshName = obj.header.name;
                        if (meshName.empty())
                        {
                            char text[16];
                            std::snprintf(text, sizeof text, "obj%08x", obj.header.id);
                            meshName = text;
                        }
                        meshes[key] = r.faces.empty() ? std::nullopt
                                                      : meshOf(meshName, r.part->part, r.part->materials, r.faces, r.withTags, objectExtras(obj, inst));
                    }
                    if (!meshes[key])
                        continue;
                    const std::string role = r.role;
                    const std::string suffix = role == "sky"        ? "#sky"
                                               : role == "deco"     ? "#deco"
                                               : role == "hidden"   ? "#hidden"
                                               : role == "dynamics" ? "#dynamics"
                                                                    : "";
                    m_builder.node(nodeName + suffix, meshes[key], toModel(position), quatToModel(rotation));
                }
                if (hit)
                    grindEdges(hit->part, hit->materials, position, rotation);
            }
            // the grind edges, a line mesh per word and sound (sorted by both)
            for (const auto &[key, segments] : m_edges)
            {
                const std::string &word = std::get<0>(key), &sound = std::get<1>(key);
                const uint32_t tagWord = std::get<2>(key);
                Primitive p;
                std::map<Vec3, int> index;
                for (const auto &[a, b] : segments)
                {
                    std::array<int, 2> ends{};
                    int e = 0;
                    for (const Vec3 &pt : {a, b})
                    {
                        auto it = index.find(pt);
                        if (it == index.end())
                        {
                            it = index.emplace(pt, static_cast<int>(p.positions.size())).first;
                            p.positions.push_back(pt);
                        }
                        ends[static_cast<size_t>(e++)] = it->second;
                    }
                    p.lines.push_back(ends);
                }
                // (a tag word: in the names as zge_level_gltf's export writes it, _<8 hex> and #tag=0x<8 hex>)
                const std::string named = tagWord ? "_" + tagWordText(tagWord).substr(2) : std::string();
                const std::string meshName = "grind_" + word + (sound.empty() ? "" : "_" + sound) + named;
                std::vector<Primitive> prims;
                prims.push_back(std::move(p));
                const int mesh = m_builder.mesh(meshName, std::move(prims));
                m_builder.node(meshName + "#grind=" + word + (sound.empty() ? "" : "#" + sound) + (tagWord ? "#tag=" + tagWordText(tagWord) : ""),
                               mesh);
            }
            for (const ScenePoint &p : dat.points)
            {
                if (p.kind != kSpawnPoint)
                    continue;
                std::string role = spawnRole(m_form.engine, p.name);
                if (const auto named = namedPoints().find(p.name); role.empty() && named != namedPoints().end())
                    role = named->second; // a named point: carried by its name
                if (role.empty())
                    continue; // a name no game's scripts are known to read
                m_builder.node((role == "respawn" ? std::string("Respawn") : role) + "#spawn=" + role, std::nullopt, toModel(p.position),
                               quatToModel(p.orientation));
            }
            std::vector<const ScenePoint *> cameras;
            for (const ScenePoint &p : dat.points)
                if (p.kind == kCameraPoint && p.name == kIdleCamera && p.index > 0)
                    cameras.push_back(&p);
            std::stable_sort(cameras.begin(), cameras.end(), [](const ScenePoint *a, const ScenePoint *b) { return a->index < b->index; });
            const double lift = m_form.engine == Engine::Zge2 ? kDm2CameraLift : 0.0;
            for (size_t n = 0; n < cameras.size(); ++n)
            {
                const ScenePoint &p = *cameras[n];
                const Vec3 eye = {p.position[0], p.position[1] + lift, p.position[2]};
                const int fov = !p.tags.empty() && p.tags[0] ? p.tags[0] : 60;
                m_builder.node("Idle" + std::to_string(n + 1) + "#camera#fov=" + std::to_string(fov), std::nullopt, toModel(eye),
                               quatToModel(p.orientation));
            }
            return std::move(m_builder.model());
        }
    }

    Model exportLevel(Engine engine, const LevelFiles &files)
    {
        Export ex(engine, files);
        return ex.run();
    }
}
