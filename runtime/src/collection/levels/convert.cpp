// A model made into a level (collection/levels/convert.h): Aggressive
// Inline's converter, which the other two games' build on (levels/
// convert_dm2.cpp, convert_bmx.cpp). Ported from the tooling's
// ai_level_convert.py, the reference.
#include "converter.h"

#include "collection/levels/arithmetic.h"
#include "collection/levels/text.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace ps2x::app::levels
{
    const uint32_t kTerrainClass = nameHash("Terrain");

    namespace
    {
        constexpr double kCell = 50.0; // TEHD: the collision world's cell, every stock level's
        constexpr size_t kNameBytes = 31;
        constexpr double kQuarterPipeLaunchPush = 4.0; // TUT's quarter pipes
        constexpr double kEdgeTolerance = 0.01;
        constexpr double kMarkCell = 4.0;
        // LOCAL's one material's layer: triangle strips, Gouraud, no blending,
        // wrap both ways, RGBA modulate, mipmaps by LOD, Z test greater-or-equal.
        constexpr std::array<uint32_t, 4> kOpaqueLayerState = {0x0000001C, 0x40000000, 0x49F782F0, 0x1C000101};
        constexpr uint8_t kMaterialColours[12] = {0xe5, 0xe5, 0xe5, 0xff, 0xf3, 0xf3, 0xf3, 0xff, 0xf3, 0xf3, 0xf3, 0xff};

        Vec3 toGame(const Vec3 &p) { return {p[0], -p[1], -p[2]}; }
        Quat quatToGame(const Quat &q) { return {q[0], -q[1], -q[2], q[3]}; }
        Vec3 plus(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

        // AI's Globals record (aggressive/camera.cpp applyLevelCameraSettings: 4 the
        // clear colour, 8 the fog colour, 12 the fog alpha as a float, 16 the fog
        // start, 20 its end, 24 the far clip).
        Bytes aiGlobalsTags(const Globals &g)
        {
            ByteWriter w;
            w.u32(0);
            w.u8(static_cast<uint8_t>(g.clear[0])).u8(static_cast<uint8_t>(g.clear[1])).u8(static_cast<uint8_t>(g.clear[2])).u8(0x80);
            w.u8(static_cast<uint8_t>(g.fog[0])).u8(static_cast<uint8_t>(g.fog[1])).u8(static_cast<uint8_t>(g.fog[2])).u8(static_cast<uint8_t>(g.fogAlpha));
            w.f32(static_cast<double>(g.fogAlpha)).f32(g.fogStart).f32(g.fogEnd).f32(g.farClip);
            return w.take();
        }

        // AI's sky: the panorama part's render class 3, the instance a decoration
        // with the sky bit; the fog's alpha the stock levels' 24, from 240 to 360 m.
        const SkyForm kAiSky = {kPartFlagsSky, kInstanceDecoration | kInstanceSky, 24, 240.0, 360.0, 1000.0, &aiGlobalsTags};

        // AI's props: the class's tag bytes 4 and 5 are the first and later
        // impact sounds (createDynamicsObject: + 0x40 in the level's bank),
        // the Civic Center's DynamicsDef_BoxA's 1 and 1 (its other two bytes
        // are not read: zero here); the instance's tags one zero word.
        const DynamicsForm kAiDynamics = {Engine::Zge3, kInstanceCollides, {1, 1, 0, 0}, Bytes(4, 0)};

        std::string withNumber(const std::string &name, size_t n) { return name + "_" + std::to_string(n); }
    }

    // ---- tags and names -------------------------------------------------------------------

    std::pair<std::string, TagMap> splitTags(const std::string &name)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t hash = name.find('#', start);
            parts.push_back(name.substr(start, hash == std::string::npos ? std::string::npos : hash - start));
            if (hash == std::string::npos)
                break;
            start = hash + 1;
        }
        TagMap tags;
        for (size_t i = 1; i < parts.size(); ++i)
        {
            const std::string &t = parts[i];
            if (t.empty())
                continue;
            const size_t equals = t.find('=');
            const std::string key = lowerKey(trimmed(t.substr(0, equals)));
            const std::string value = equals == std::string::npos ? std::string() : t.substr(equals + 1);
            tags[key] = value.empty() ? TagValue{} : TagValue{true, trimmed(value)};
        }
        return {trimmed(parts[0]), tags};
    }

    bool hasTag(const TagMap &tags, const char *key) { return tags.count(key) != 0; }

    std::string distinctName(const std::string &name, std::set<std::string> &used)
    {
        const std::string base = name.empty() ? "object" : name;
        std::string out = base;
        for (size_t n = 2; used.count(upperKey(out)); ++n)
            out = withNumber(base, n);
        used.insert(upperKey(out));
        return out;
    }

    std::string shortName(const std::string &name, std::set<std::string> &used)
    {
        std::string base = name.substr(0, kNameBytes);
        if (base.empty())
            base = "object";
        std::string out = base;
        for (size_t n = 2; used.count(upperKey(out)); ++n)
        {
            const std::string tail = "_" + std::to_string(n);
            out = base.substr(0, kNameBytes - tail.size()) + tail;
        }
        used.insert(upperKey(out));
        return out;
    }

    bool parseFloat(const std::string &text, double &out)
    {
        const std::string t = trimmed(text);
        if (t.empty())
            return false;
        char *end = nullptr;
        errno = 0;
        out = std::strtod(t.c_str(), &end);
        return end == t.c_str() + t.size();
    }

    bool parseInt(const std::string &text, long long &out)
    {
        const std::string t = trimmed(text);
        if (t.empty())
            return false;
        size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
        if (i == t.size())
            return false;
        for (size_t k = i; k < t.size(); ++k)
            if (t[k] < '0' || t[k] > '9')
                return false;
        out = std::strtoll(t.c_str(), nullptr, 10);
        return true;
    }

    double floorDiv(double x, double y)
    {
        // CPython's float floor division (floatobject.c _float_div_mod).
        double mod = std::fmod(x, y);
        double div = (x - mod) / y;
        if (mod != 0.0)
        {
            if ((y < 0) != (mod < 0))
            {
                mod += y;
                div -= 1.0;
            }
        }
        if (div != 0.0)
        {
            double floored = std::floor(div);
            if (div - floored > 0.5)
                floored += 1.0;
            return floored;
        }
        return std::copysign(0.0, x / y);
    }

    // ---- materials ---------------------------------------------------------------------------

    std::array<uint32_t, 4> layerState(const std::array<uint32_t, 4> &base, const std::optional<Bytes> &given)
    {
        std::array<uint32_t, 4> words = base;
        if (!given)
            return words;
        const uint32_t g0 = readU32(*given, 0), g1 = readU32(*given, 4), g2 = readU32(*given, 8), g3 = readU32(*given, 12);
        words[0] = (words[0] & ~0x20u) | (g0 & 0x20u);
        words[1] = (words[1] & ~(1u << 30)) | (g1 & (1u << 30));
        const uint32_t keep2 = 0x3u | (0x3Fu << 25);
        words[2] = (words[2] & ~keep2) | (g2 & keep2);
        words[3] = g3;
        return words;
    }

    Chunk aiMaterial(const std::string &name, const std::string &texture, bool twoSided, const std::optional<Bytes> &layer)
    {
        ByteWriter w;
        w.u32(twoSided ? 0x2 : 0).u32(nameHash(name)).bytes(kMaterialColours, sizeof kMaterialColours).f32(0.1).f32(1.0).zeros(16).u32(1);
        for (uint32_t word : layerState(kOpaqueLayerState, layer))
            w.u32(word);
        w.u32(nameHash(texture)).u32(0).u32(0).zeros(28 * 3);
        return Chunk::leaf(tag("MATI"), w.take());
    }

    namespace
    {
        // A #layer=<32 hex digits> as its 16 bytes (Python's bytes.fromhex:
        // whitespace between the bytes allowed).
        std::optional<Bytes> layerOf(const TagMap &tags)
        {
            auto it = tags.find("layer");
            if (it == tags.end() || !it->second.hasText)
                return std::nullopt;
            const std::string &text = it->second.text;
            Bytes out;
            size_t i = 0;
            auto hex = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            while (i < text.size())
            {
                if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\v' || text[i] == '\f')
                {
                    ++i;
                    continue;
                }
                if (i + 1 >= text.size() || hex(text[i]) < 0 || hex(text[i + 1]) < 0)
                    return std::nullopt;
                out.push_back(static_cast<uint8_t>(hex(text[i]) * 16 + hex(text[i + 1])));
                i += 2;
            }
            if (out.size() != 16)
                return std::nullopt;
            return out;
        }
    }

    // ---- grind marks -------------------------------------------------------------------------------

    MarkIndex::MarkIndex(const std::vector<EdgeMark> &marks) : m_marks(marks)
    {
        for (size_t n = 0; n < marks.size(); ++n)
        {
            const EdgeMark &m = marks[n];
            std::array<long long, 3> low{}, high{};
            for (int k = 0; k < 3; ++k)
            {
                low[k] = static_cast<long long>(floorDiv(std::min(m.a[k], m.b[k]) - kEdgeTolerance, kMarkCell));
                high[k] = static_cast<long long>(floorDiv(std::max(m.a[k], m.b[k]) + kEdgeTolerance, kMarkCell));
            }
            for (long long x = low[0]; x <= high[0]; ++x)
                for (long long y = low[1]; y <= high[1]; ++y)
                    for (long long z = low[2]; z <= high[2]; ++z)
                        m_cells[{x, y, z}].push_back(n);
        }
    }

    std::vector<const EdgeMark *> MarkIndex::near(const Vec3 &p) const
    {
        std::vector<const EdgeMark *> out;
        auto it = m_cells.find({static_cast<long long>(floorDiv(p[0], kMarkCell)), static_cast<long long>(floorDiv(p[1], kMarkCell)),
                                static_cast<long long>(floorDiv(p[2], kMarkCell))});
        if (it != m_cells.end())
            for (size_t n : it->second)
                out.push_back(&m_marks[n]);
        return out;
    }

    bool onSegment(const Vec3 &p, const EdgeMark &mark)
    {
        const Vec3 &a = mark.a, &b = mark.b;
        const Vec3 ab = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double length2 = sumOf({ab[0] * ab[0], ab[1] * ab[1], ab[2] * ab[2]});
        if (length2 < 1e-12)
            return false;
        double t = sumOf({(p[0] - a[0]) * ab[0], (p[1] - a[1]) * ab[1], (p[2] - a[2]) * ab[2]}) / length2;
        if (t < -kEdgeTolerance || t > 1 + kEdgeTolerance)
            return false;
        t = std::max(0.0, std::min(1.0, t));
        const Vec3 q = {a[0] + t * ab[0], a[1] + t * ab[1], a[2] + t * ab[2]};
        return distanceOf(p, q) <= kEdgeTolerance;
    }

    // ---- the converter --------------------------------------------------------------------------------

    LevelConverter::LevelConverter(const Model &model, std::string slot, Engine engine)
        : m_engine(engine), m_sky(&kAiSky), m_dynamics(&kAiDynamics), m_model(model), m_slot(upperKey(slot))
    {
    }

    Texture *LevelConverter::texture(const std::string &name)
    {
        for (Texture &t : m_textures)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    int LevelConverter::surfaceOf(std::optional<int> materialIndex)
    {
        const int index = materialIndex.value_or(-1);
        for (size_t i = 0; i < m_surfaces.size(); ++i)
            if (m_surfaces[i].source == index)
                return static_cast<int>(i);
        std::string name = "default";
        TagMap tags;
        Colour base{1.0, 1.0, 1.0, 1.0};
        std::optional<int> image;
        if (index >= 0)
        {
            const Material &m = m_model.materials.at(static_cast<size_t>(index));
            std::tie(name, tags) = splitTags(m.name);
            base = m.baseColour;
            image = m.texture;
        }
        std::string textureName;
        if (image)
        {
            const Image &img = m_model.images.at(static_cast<size_t>(*image));
            textureName = texture(img.name) ? img.name : shortName(splitTags(img.name).first, m_names);
            if (!texture(textureName))
                m_textures.push_back(Texture{textureName, img.width, img.height, img.pixels});
        }
        else
        {
            textureName = shortName(name + "_colour", m_names);
            Rgba px{};
            for (int c = 0; c < 4; ++c)
                px[static_cast<size_t>(c)] = static_cast<uint8_t>(std::nearbyint(std::min(1.0, std::max(0.0, base[static_cast<size_t>(c)])) * 255));
            m_textures.push_back(Texture{textureName, 8, 8, std::vector<Rgba>(64, px)});
        }
        const std::string full = index >= 0 ? m_model.materials[static_cast<size_t>(index)].name : name;
        Surface s;
        s.name = full.substr(0, kNameBytes);
        s.texture = textureName;
        auto twoSided = tags.find("2sided");
        s.twoSided = twoSided != tags.end() && !(twoSided->second.hasText && twoSided->second.text == "collide");
        s.facesTwoSided = twoSided != tags.end();
        s.baseColour = base;
        s.layer = layerOf(tags);
        for (const auto &[word, kind] : {std::pair<const char *, int>{"qp", 1}, {"spine", 2}, {"slope", 3}, {"wall", 8}})
        {
            auto it = tags.find(word);
            if (it == tags.end())
                continue;
            s.kind = kind;
            if (kind == 3)
            {
                double g = 1.0;
                if (!it->second.hasText)
                    g = 1.0; // float(True)
                else if (!parseFloat(it->second.text, g))
                    g = 1.0;
                s.gravity = g;
            }
        }
        s.source = index;
        m_surfaces.push_back(std::move(s));
        return static_cast<int>(m_surfaces.size()) - 1;
    }

    void LevelConverter::readModel()
    {
        for (const Node &node : m_model.nodes)
        {
            auto [name, tags] = splitTags(node.name);
            const Mesh *mesh = node.mesh ? &m_model.meshes.at(static_cast<size_t>(*node.mesh)) : nullptr;
            if (mesh)
            {
                // the mesh's tags under the node's own
                TagMap merged = splitTags(mesh->name).second;
                for (const auto &[k, v] : tags)
                    merged[k] = v;
                tags = std::move(merged);
            }
            if (hasTag(tags, "zone"))
                continue; // an exported level has no zones
            if (auto spawn = tags.find("spawn"); spawn != tags.end())
            {
                std::string spawnName = spawn->second.hasText ? spawn->second.text : name;
                const std::string key = lowerKey(spawnName);
                if (key == "playerone")
                    spawnName = "PlayerOne";
                else if (key == "playertwo" && !m_hasPlayerTwo)
                {
                    report.did("spawn " + spawnName + " has no meaning in this game: left out");
                    continue;
                }
                else if (key == "playertwo")
                    spawnName = "PlayerTwo";
                else if (key == "respawn")
                    spawnName = m_respawnName;
                spawns.push_back({spawnName, toGame(node.translation), quatToGame(node.rotation)});
                continue;
            }
            if (tags.count("camera"))
            {
                Camera c;
                c.position = toGame(node.translation);
                c.rotation = quatToGame(node.rotation);
                double fov = 0;
                if (auto it = tags.find("fov"); it != tags.end() && it->second.hasText && parseFloat(it->second.text, fov) && std::isfinite(fov))
                    c.fov = static_cast<int>(std::min(179.0, std::max(1.0, std::trunc(fov))));
                cameras.push_back(c);
                continue;
            }
            if (!mesh)
                continue;
            const bool lines = std::any_of(mesh->primitives.begin(), mesh->primitives.end(), [](const Primitive &p) { return !p.lines.empty(); });
            if (lines && hasTag(tags, "grind"))
                addEdges(node, *mesh, tags);
            if (std::any_of(mesh->primitives.begin(), mesh->primitives.end(), [](const Primitive &p) { return !p.triangles.empty(); }))
                addObject(name, node, *mesh, tags);
        }
    }

    void LevelConverter::addEdges(const Node &node, const Mesh &mesh, const TagMap &tags)
    {
        const TagValue &grind = tags.at("grind");
        const std::string word = grind.hasText ? grind.text : "ledge";
        int kind;
        if (word == "ledge" || word == "rail")
            kind = 0;
        else if (word == "lip")
            kind = 1;
        else if (word == "spine")
            kind = 2;
        else if (word == "vpole")
            kind = 3;
        else if (word == "hpole")
            kind = 4;
        else
            throw LevelError("node " + node.name + ": #grind=" + word + " is not one of ledge, rail, lip, spine, vpole, hpole");
        int sound = 0;
        for (const auto &[w, s] : {std::pair<const char *, int>{"concrete", 0}, {"wood", 1}, {"metal", 2}})
            if (hasTag(tags, w))
            {
                sound = s;
                break;
            }
        const int flags = kind == 3 ? 0 : EdgeTag::kGrindable; // a vertical pole is its own search's
        for (const Primitive &prim : mesh.primitives)
        {
            std::vector<Vec3> world;
            for (const Vec3 &p : prim.positions)
                world.push_back(plus(toGame(quatRotate(node.rotation, {p[0] * node.scale[0], p[1] * node.scale[1], p[2] * node.scale[2]})),
                                     toGame(node.translation)));
            for (const auto &[i, k] : prim.lines)
                m_edgeMarks.push_back({world.at(static_cast<size_t>(i)), world.at(static_cast<size_t>(k)), kind, flags, sound});
        }
    }

    void LevelConverter::addObject(const std::string &name, const Node &node, const Mesh &mesh, const TagMap &tags)
    {
        const bool sky = hasTag(tags, "sky");
        const bool dynamics = hasTag(tags, "dynamics") && !sky;
        const bool collides = !hasTag(tags, "deco") && !sky;
        const Vec3 scale = {roundTo(node.scale[0], 6), roundTo(node.scale[1], 6), roundTo(node.scale[2], 6)};
        const PieceKey key{*node.mesh, scale, collides, sky, dynamics};
        size_t entry = 0;
        while (entry < m_pieces.size() && !(m_pieces[entry].first == key))
            ++entry;
        if (entry == m_pieces.size())
        {
            std::string meshName = splitTags(mesh.name).first;
            if (meshName.empty())
                meshName = name;
            std::vector<Piece> pieces = makePieces(meshName, mesh, scale, collides, sky);
            if (mesh.extras.nameHash && pieces.size() == 1)
                pieces[0].nameHash = mesh.extras.nameHash;
            if (dynamics)
            {
                if (pieces.size() > 1)
                    throw LevelError("dynamics prop " + meshName + " cut into " + std::to_string(pieces.size()) +
                                     " pieces: one prop is one object (at most 1024 faces, 16384 vertices)");
                const std::optional<DynamicsDefinition> &definition = mesh.extras.dynamics;
                if (!definition || definition->chunks.empty())
                    throw LevelError("node " + node.name + ": #dynamics needs its definition, the mesh's extras \"dynamics\" with its "
                                     "ZDYN chunks (a stock prop's export has them)");
                if (std::none_of(definition->chunks.begin(), definition->chunks.end(), [](const Chunk &c) { return c.tag == tag("DBDY"); }))
                    throw LevelError("node " + node.name + ": the dynamics definition has no body (DBDY)");
                pieces[0].dynamics = definition;
            }
            m_pieces.emplace_back(key, std::move(pieces));
        }
        Placement pl;
        pl.nodeName = name;
        pl.pieces = entry;
        pl.position = toGame(node.translation);
        pl.rotation = quatToGame(node.rotation);
        pl.collides = collides;
        pl.hidden = hasTag(tags, "hidden");
        pl.sky = sky;
        pl.dynamics = dynamics;
        m_placements.push_back(std::move(pl));
    }

    std::vector<Piece> LevelConverter::makePieces(const std::string &name, const Mesh &mesh, const Vec3 &scale, bool collides, bool sky)
    {
        Piece whole;
        whole.name = name;
        whole.collides = collides;
        whole.sky = sky;
        std::vector<std::optional<Vec3>> normals;
        // equal vertices welded, so strips run on: (position, normal, uv, colour)
        struct VertexKey
        {
            Vec3 position;
            std::optional<Vec3> normal;
            Vec2 uv;
            Rgba colour;
            bool operator<(const VertexKey &o) const
            {
                if (position != o.position)
                    return position < o.position;
                if (normal.has_value() != o.normal.has_value())
                    return !normal.has_value();
                if (normal && *normal != *o.normal)
                    return *normal < *o.normal;
                if (uv != o.uv)
                    return uv < o.uv;
                return colour < o.colour;
            }
        };
        std::map<VertexKey, int> shared;
        for (const Primitive &prim : mesh.primitives)
        {
            if (prim.triangles.empty())
                continue;
            const int surface = surfaceOf(prim.material);
            std::vector<int> index;
            for (size_t i = 0; i < prim.positions.size(); ++i)
            {
                const Vec3 &p = prim.positions[i];
                const Vec3 position = toGame({p[0] * scale[0], p[1] * scale[1], p[2] * scale[2]});
                std::optional<Vec3> normal;
                if (prim.normals)
                {
                    const Vec3 &raw = (*prim.normals)[i];
                    const Vec3 n = {raw[0] / (scale[0] != 0.0 ? scale[0] : 1), raw[1] / (scale[1] != 0.0 ? scale[1] : 1),
                                    raw[2] / (scale[2] != 0.0 ? scale[2] : 1)};
                    double length = std::sqrt(sumOf({n[0] * n[0], n[1] * n[1], n[2] * n[2]}));
                    if (length == 0.0)
                        length = 1.0;
                    normal = toGame({n[0] / length, n[1] / length, n[2] / length});
                }
                const Vec2 uv = prim.uvs ? (*prim.uvs)[i] : Vec2{0.0, 0.0};
                const Colour c = prim.colours ? (*prim.colours)[i] : Colour{1.0, 1.0, 1.0, 1.0};
                // 0x80 is 1.0 on the PS2; the stock levels go up to 0xFF. The
                // alpha's full is 0xFF (every stock vertex of the three games):
                // at 0x80 a blended material drew at half its alpha.
                Rgba colour{};
                for (int k = 0; k < 4; ++k)
                    colour[static_cast<size_t>(k)] = static_cast<uint8_t>(
                        std::min(255.0, std::max(0.0, std::nearbyint(c[static_cast<size_t>(k)] * (k == 3 ? 0xFF : 0x80)))));
                const VertexKey vk{position, normal, uv, colour};
                // A NaN equals nothing, itself neither: a vertex with one is
                // never welded (as the reference's dict of tuples has it).
                bool nan = std::isnan(uv[0]) || std::isnan(uv[1]);
                for (int k = 0; k < 3; ++k)
                    nan = nan || std::isnan(position[static_cast<size_t>(k)]) || (normal && std::isnan((*normal)[static_cast<size_t>(k)]));
                auto it = nan ? shared.end() : shared.find(vk);
                if (it == shared.end())
                {
                    const int fresh = static_cast<int>(whole.positions.size());
                    if (!nan)
                        it = shared.emplace(vk, fresh).first;
                    whole.positions.push_back(position);
                    normals.push_back(normal);
                    whole.uvs.push_back(uv);
                    whole.colours.push_back(colour);
                    index.push_back(fresh);
                    continue;
                }
                index.push_back(it->second);
            }
            for (const auto &[a, b, c] : prim.triangles)
            {
                const int ia = index.at(static_cast<size_t>(a)), ib = index.at(static_cast<size_t>(b)), ic = index.at(static_cast<size_t>(c));
                if (ia != ib && ib != ic && ia != ic)
                    whole.faces.push_back({surface, ia, ib, ic});
            }
        }
        if (std::any_of(normals.begin(), normals.end(), [](const std::optional<Vec3> &n) { return !n; }))
        {
            std::vector<std::vector<Vec3>> faceNormals(whole.positions.size());
            for (const auto &f : whole.faces)
            {
                const Vec3 fn = faceNormal(whole.positions[static_cast<size_t>(f[1])], whole.positions[static_cast<size_t>(f[2])],
                                           whole.positions[static_cast<size_t>(f[3])]);
                for (int k = 1; k < 4; ++k)
                    faceNormals[static_cast<size_t>(f[static_cast<size_t>(k)])].push_back(fn);
            }
            for (size_t i = 0; i < normals.size(); ++i)
                if (!normals[i])
                {
                    Vec3 acc = {0.0, -1.0, 0.0};
                    if (!faceNormals[i].empty())
                        for (int k = 0; k < 3; ++k)
                        {
                            std::vector<double> parts;
                            for (const Vec3 &fn : faceNormals[i])
                                parts.push_back(fn[static_cast<size_t>(k)]);
                            acc[static_cast<size_t>(k)] = sumOf(parts);
                        }
                    double length = std::sqrt(sumOf({acc[0] * acc[0], acc[1] * acc[1], acc[2] * acc[2]}));
                    if (length == 0.0)
                        length = 1.0;
                    normals[i] = Vec3{acc[0] / length, acc[1] / length, acc[2] / length};
                }
        }
        for (const std::optional<Vec3> &n : normals)
            whole.normals.push_back(*n);
        std::vector<Piece> pieces = cut(whole);
        if (sky && pieces.size() > 1)
            throw LevelError("sky " + name + " has " + std::to_string(whole.faces.size()) + " faces: one object holds at most 1024");
        if (pieces.size() > 1)
            report.did("object " + name + " cut into " + std::to_string(pieces.size()) + " pieces: " + std::to_string(whole.faces.size()) +
                       " faces over 1024 an object");
        std::set<std::string> used;
        for (size_t i = 0; i < pieces.size(); ++i)
            pieces[i].name = distinctName(i == 0 ? name : withNumber(name, i + 1), used);
        return pieces;
    }

    std::vector<Piece> LevelConverter::cut(const Piece &piece)
    {
        if (piece.faces.size() <= kFacesPerObject && piece.positions.size() <= kVerticesPerPart)
            return {piece};
        std::vector<Vec3> centres;
        for (const auto &f : piece.faces)
        {
            Vec3 c{};
            for (int k = 0; k < 3; ++k)
                c[static_cast<size_t>(k)] = (piece.positions[static_cast<size_t>(f[1])][static_cast<size_t>(k)] +
                                             piece.positions[static_cast<size_t>(f[2])][static_cast<size_t>(k)] +
                                             piece.positions[static_cast<size_t>(f[3])][static_cast<size_t>(k)]) /
                                            3;
            centres.push_back(c);
        }
        int axis = 0;
        double widest = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            double lo = centres[0][static_cast<size_t>(k)], hi = lo;
            for (const Vec3 &c : centres)
            {
                lo = std::min(lo, c[static_cast<size_t>(k)]);
                hi = std::max(hi, c[static_cast<size_t>(k)]);
            }
            if (k == 0 || hi - lo > widest)
            {
                widest = hi - lo;
                axis = k;
            }
        }
        std::vector<size_t> order(piece.faces.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return centres[a][static_cast<size_t>(axis)] < centres[b][static_cast<size_t>(axis)]; });
        const size_t middle = order.size() / 2;
        std::vector<Piece> out;
        for (int h = 0; h < 2; ++h)
        {
            Piece half;
            half.name = piece.name;
            half.collides = piece.collides;
            half.sky = piece.sky;
            std::map<int, int> remap;
            for (size_t n = h == 0 ? 0 : middle; n < (h == 0 ? middle : order.size()); ++n)
            {
                const auto &f = piece.faces[order[n]];
                std::array<int, 4> face{f[0], 0, 0, 0};
                for (int k = 1; k < 4; ++k)
                {
                    const int v = f[static_cast<size_t>(k)];
                    auto it = remap.find(v);
                    if (it == remap.end())
                    {
                        it = remap.emplace(v, static_cast<int>(half.positions.size())).first;
                        half.positions.push_back(piece.positions[static_cast<size_t>(v)]);
                        half.normals.push_back(piece.normals[static_cast<size_t>(v)]);
                        half.uvs.push_back(piece.uvs[static_cast<size_t>(v)]);
                        half.colours.push_back(piece.colours[static_cast<size_t>(v)]);
                    }
                    face[static_cast<size_t>(k)] = it->second;
                }
                half.faces.push_back(face);
            }
            for (Piece &p : cut(half))
                out.push_back(std::move(p));
        }
        return out;
    }

    std::optional<SkyLook> LevelConverter::skyLookOf(const std::vector<const Piece *> &pieces)
    {
        std::vector<std::pair<double, std::array<int, 3>>> corners; // (height, colour)
        for (const Piece *piece : pieces)
            for (const auto &f : piece->faces)
            {
                const Texture *t = texture(m_surfaces[static_cast<size_t>(f[0])].texture);
                for (int k = 1; k < 4; ++k)
                {
                    const size_t v = static_cast<size_t>(f[static_cast<size_t>(k)]);
                    const Vec2 &uv = piece->uvs[v];
                    auto wrap = [](double value, int size)
                    {
                        long long i = static_cast<long long>(std::floor(value * size)) % size;
                        return static_cast<int>(i < 0 ? i + size : i);
                    };
                    const Rgba &texel = t->at(wrap(uv[0], t->width), wrap(uv[1], t->height));
                    const Rgba &shade = piece->colours[v];
                    std::array<int, 3> c{};
                    for (int ch = 0; ch < 3; ++ch)
                        c[static_cast<size_t>(ch)] = std::min(255, texel[static_cast<size_t>(ch)] * shade[static_cast<size_t>(ch)] / 0x80);
                    corners.emplace_back(-piece->positions[v][1], c);
                }
            }
        if (corners.empty())
            return std::nullopt;
        double low = corners[0].first, high = low;
        for (const auto &c : corners)
        {
            low = std::min(low, c.first);
            high = std::max(high, c.first);
        }
        const double centre = (low < 0 && 0 < high) ? (low + high) / 2 : std::max(low, 0.0);
        std::vector<std::array<int, 3>> top, horizon, all;
        for (const auto &[h, c] : corners)
        {
            all.push_back(c);
            if (h >= high - (high - centre) / 4)
                top.push_back(c);
            if (centre <= h && h <= centre + (high - centre) / 4)
                horizon.push_back(c);
        }
        auto mean = [&all](const std::vector<std::array<int, 3>> &cs)
        {
            const std::vector<std::array<int, 3>> &use = cs.empty() ? all : cs;
            std::array<int, 3> out{};
            for (int k = 0; k < 3; ++k)
            {
                long long sum = 0;
                for (const auto &c : use)
                    sum += c[static_cast<size_t>(k)];
                out[static_cast<size_t>(k)] = static_cast<int>(static_cast<double>(sum) / static_cast<double>(use.size()));
            }
            return out;
        };
        return SkyLook{mean(top), mean(horizon)};
    }

    void LevelConverter::useSkyLook(const SkyLook &look)
    {
        Globals g;
        g.clear = look.zenith;
        g.fog = look.horizon;
        g.fogAlpha = m_sky->fogAlpha;
        g.fogStart = m_sky->fogStart;
        g.fogEnd = m_sky->fogEnd;
        g.farClip = m_sky->farClip;
        m_globals = g;
    }

    ScenePoint LevelConverter::globalsPoint()
    {
        ScenePoint p;
        p.kind = kGlobalsPoint;
        p.name = 0;
        p.position = {0.0, 0.0, 0.0};
        p.tags = m_sky->tags(*m_globals);
        return p;
    }

    void LevelConverter::worldBox(const std::vector<Instance> &instances, Vec3 &low, Vec3 &high) const
    {
        for (int k = 0; k < 3; ++k)
        {
            double lo = instances[0].worldMin[static_cast<size_t>(k)], hi = instances[0].worldMax[static_cast<size_t>(k)];
            for (const Instance &i : instances)
            {
                lo = std::min(lo, i.worldMin[static_cast<size_t>(k)]);
                hi = std::max(hi, i.worldMax[static_cast<size_t>(k)]);
            }
            low[static_cast<size_t>(k)] = std::floor(lo / kCell) * kCell - kCell;
            high[static_cast<size_t>(k)] = std::ceil(hi / kCell) * kCell + kCell;
        }
    }

    GeoHeader LevelConverter::objectHeader(const Piece &piece, uint32_t objectId)
    {
        Vec3 low = piece.positions[0], high = low;
        for (const Vec3 &p : piece.positions)
            for (size_t k = 0; k < 3; ++k)
            {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        return GeoHeader::make(low, high, objectId, piece.name, piece.nameHash);
    }

    Bytes LevelConverter::dynamicsClassData(const DynamicsDefinition &definition) const
    {
        return definition.game == m_dynamics->game ? definition.classData : m_dynamics->classData;
    }

    Bytes LevelConverter::dynamicsInstanceTags(const DynamicsDefinition &definition) const
    {
        return definition.game == m_dynamics->game && !definition.instanceTags.empty() ? definition.instanceTags : m_dynamics->instanceTags;
    }

    GeoObject LevelConverter::geoObject(const Piece &piece, uint32_t objectId)
    {
        const uint32_t partId = objectId + 1;
        std::set<int> surfaceSet;
        for (const auto &f : piece.faces)
            surfaceSet.insert(f[0]);
        const std::vector<int> surfaces(surfaceSet.begin(), surfaceSet.end());
        std::map<int, int> local;
        for (size_t i = 0; i < surfaces.size(); ++i)
            local[surfaces[i]] = static_cast<int>(i);
        Part part;
        part.id = partId;
        part.positions = piece.positions;
        for (const auto &f : piece.faces)
        {
            const Surface &s = m_surfaces[static_cast<size_t>(f[0])];
            part.faces.push_back(Face::make(local[f[0]], f[1], f[2], f[3], piece.collides, s.facesTwoSided || s.twoSided));
        }
        for (int s : surfaces)
        {
            std::vector<std::array<int, 3>> tris;
            for (const auto &f : piece.faces)
                if (f[0] == s)
                    tris.push_back({f[1], f[2], f[3]});
            for (StripGroup &g : packStrips(local[s], stripify(tris)))
                part.groups.push_back(std::move(g));
        }
        if (piece.positions.size() > kVerticesPerPart)
            throw LevelError(piece.name + ": " + std::to_string(piece.positions.size()) + " vertices in one part");
        std::vector<Bytes> normals, colours, uvs, faceNormals;
        for (const Vec3 &n : piece.normals)
            normals.push_back(ByteWriter().f32s(n).take());
        for (const Rgba &c : piece.colours)
            colours.push_back({c[0], c[1], c[2], c[3]});
        for (const Vec2 &uv : piece.uvs)
            uvs.push_back(ByteWriter().f32s(uv).take());
        for (const auto &f : piece.faces)
            faceNormals.push_back(ByteWriter().f32s(faceNormal(piece.positions[static_cast<size_t>(f[1])], piece.positions[static_cast<size_t>(f[2])],
                                                              piece.positions[static_cast<size_t>(f[3])]))
                                      .take());
        part.stream(StreamKind::Normals) = Stream::ofIndexed(IndexedStream::make(normals));
        part.stream(StreamKind::Colours) = Stream::ofIndexed(IndexedStream::make(colours));
        part.stream(StreamKind::Uvs) = Stream::ofIndexed(IndexedStream::make(uvs));
        part.stream(StreamKind::FaceNormals) = Stream::ofIndexed(IndexedStream::make(faceNormals));
        part.flags = piece.sky ? m_sky->partFlags : kPartFlags;
        part.materialCount = static_cast<uint8_t>(surfaces.size());
        std::vector<int> kinds;
        for (const auto &f : piece.faces)
            kinds.push_back(m_surfaces[static_cast<size_t>(f[0])].kind);
        std::set<int> kindSet;
        for (int k : kinds)
            if (k)
                kindSet.insert(k);
        if (!kindSet.empty() && piece.collides)
        {
            const std::vector<int> sorted(kindSet.begin(), kindSet.end());
            TagBuffer buffer;
            for (int k : sorted)
            {
                double gravity = 0.0;
                bool any = false;
                for (const auto &f : piece.faces)
                {
                    const Surface &s = m_surfaces[static_cast<size_t>(f[0])];
                    if (s.kind == k)
                    {
                        gravity = any ? std::max(gravity, s.gravity) : s.gravity;
                        any = true;
                    }
                }
                FaceTag t;
                t.kind = static_cast<uint8_t>(k);
                t.gravity = k == 3 ? gravity : 0.0;
                t.launchPush = k == 1 ? kQuarterPipeLaunchPush : 0.0;
                buffer.records.push_back(t.write());
            }
            for (int k : kinds)
                buffer.indices.push_back(k ? static_cast<int>(std::find(sorted.begin(), sorted.end(), k) - sorted.begin()) : -1);
            part.faceTags = std::move(buffer);
        }
        std::vector<Chunk> materials;
        for (int s : surfaces)
        {
            const Surface &surface = m_surfaces[static_cast<size_t>(s)];
            materials.push_back(aiMaterial(surface.name, surface.texture, surface.twoSided, surface.layer));
        }
        GeoObject obj;
        obj.header = objectHeader(piece, objectId);
        obj.frame = {0x00, 0x00, 0x01, 0x00};
        obj.typeData = ByteWriter().u32(kTerrainClass).take();
        LevelOfDetail lod;
        lod.distance = kLodDistance;
        lod.partId = partId;
        if (piece.collides)
            obj.collision = CollisionTree::build(partId, part.positions, part.faces);
        obj.parts.push_back({std::move(part), lod, Chunk::node(tag("TMTB"), std::move(materials))});
        return obj;
    }

    void LevelConverter::tagEdges(Geometry &geo, const std::vector<Instance> &instances)
    {
        if (m_edgeMarks.empty())
            return;
        const MarkIndex index(m_edgeMarks);
        std::map<uint32_t, GeoObject *> objects;
        for (GeoObject &o : geo.objects)
            objects[o.header.id] = &o;
        std::set<uint32_t> tagged;
        for (const Instance &inst : instances)
        {
            if (!(inst.flags & Instance::kCollides) || tagged.count(inst.geometryId))
                continue;
            GeoObject &obj = *objects.at(inst.geometryId);
            Part &part = obj.parts[0].part;
            std::vector<Vec3> world;
            for (const Vec3 &p : part.positions)
                world.push_back(plus(quatRotate(inst.rotation, p), inst.position));
            // one record per tagged face edge, as the stock levels have it
            TagBuffer buffer;
            buffer.indices.assign(3 * part.faces.size(), -1);
            for (size_t f = 0; f < part.faces.size(); ++f)
                for (size_t k = 0; k < 3; ++k)
                {
                    const Vec3 &a = world[part.faces[f].vertices[k]], &b = world[part.faces[f].vertices[(k + 1) % 3]];
                    const EdgeMark *mark = nullptr;
                    for (const EdgeMark *m : index.near(a))
                        if (onSegment(a, *m) && onSegment(b, *m))
                        {
                            mark = m;
                            break;
                        }
                    if (!mark)
                        continue;
                    buffer.indices[3 * f + k] = static_cast<int>(buffer.records.size());
                    EdgeTag t;
                    t.kind = static_cast<uint8_t>(mark->kind);
                    t.flags = static_cast<uint8_t>(mark->flags);
                    t.material = static_cast<uint8_t>(mark->sound);
                    buffer.records.push_back(t.write());
                }
            if (!buffer.records.empty())
            {
                report.say(obj.header.name + ": " + std::to_string(buffer.records.size()) + " grind face edges");
                part.edgeTags = std::move(buffer);
                tagged.insert(inst.geometryId);
            }
        }
    }

    Quat lookRotation(const Vec3 &d)
    {
        const double h = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        const double cosYaw = d[2] / h;
        const double cy = std::sqrt(std::max(0.0, (1.0 + cosYaw) / 2.0));
        const double sy = std::copysign(std::sqrt(std::max(0.0, (1.0 - cosYaw) / 2.0)), d[0]);
        const double cx = std::sqrt(std::max(0.0, (1.0 + h) / 2.0));
        const double sx = std::copysign(std::sqrt(std::max(0.0, (1.0 - h) / 2.0)), -d[1]);
        return {cy * sx, sy * cx, -sy * sx, cy * cx};
    }

    namespace
    {
        // A made idle camera: this far behind the PlayerOne spawn and above
        // it (m), looking at a point this far above the spawn.
        constexpr double kStartCameraBack = 8.0;
        constexpr double kStartCameraUp = 3.0;
        constexpr double kStartCameraLookUp = 1.0;
    }

    std::optional<Camera> LevelConverter::startCamera() const
    {
        auto start = std::find_if(spawns.begin(), spawns.end(), [](const Spawn &s) { return nameHash(s.name) == kPlayerOne; });
        if (start == spawns.end())
            return std::nullopt;
        const Vec3 &p = start->position;
        const double x = start->rotation[0], y = start->rotation[1], z = start->rotation[2], w = start->rotation[3];
        double fx = 2.0 * (x * z + w * y), fz = 1.0 - 2.0 * (x * x + y * y); // the spawn's +z, turned
        const double n = std::sqrt(fx * fx + fz * fz);
        if (n > 1e-6)
        {
            fx = fx / n;
            fz = fz / n;
        }
        else
        {
            fx = 0.0;
            fz = 1.0;
        }
        Camera c;
        c.position = {p[0] - kStartCameraBack * fx, p[1] - kStartCameraUp, p[2] - kStartCameraBack * fz}; // (y grows down)
        const Vec3 d = {p[0] - c.position[0], p[1] - kStartCameraLookUp - c.position[1], p[2] - c.position[2]};
        const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        c.rotation = lookRotation({d[0] / length, d[1] / length, d[2] / length});
        return c;
    }

    std::vector<ScenePoint> LevelConverter::cameraPoints()
    {
        std::vector<Camera> all = cameras;
        if (all.empty() && m_makesCamera)
            if (std::optional<Camera> made = startCamera())
            {
                all.push_back(*made);
                char text[160];
                std::snprintf(text, sizeof text, "no camera in the model: an idle camera made behind and above the PlayerOne spawn, at (%.2f, %.2f, %.2f)",
                              made->position[0], made->position[1], made->position[2]);
                report.did(text);
            }
        std::vector<ScenePoint> points;
        for (size_t i = 0; i < all.size(); ++i)
        {
            ScenePoint p;
            p.kind = kCameraPoint;
            p.name = kIdleCamera;
            p.position = {all[i].position[0], all[i].position[1] - m_cameraLift, all[i].position[2]};
            p.orientation = all[i].rotation;
            p.index = static_cast<uint32_t>(i + 1);
            p.tags = {static_cast<uint8_t>(all[i].fov), 0, 0, 0};
            points.push_back(std::move(p));
        }
        if (!points.empty())
            report.say("idle cameras: " + std::to_string(points.size()));
        return points;
    }

    std::vector<ScenePoint> LevelConverter::spawnAndCameraPoints()
    {
        std::vector<ScenePoint> points;
        for (const Spawn &s : spawns)
            points.push_back(ScenePoint::spawn(s.name, s.position, s.rotation, m_spawnTags));
        if (std::none_of(points.begin(), points.end(), [](const ScenePoint &p) { return p.name == kPlayerOne; }))
            report.error("no PlayerOne spawn");
        for (ScenePoint &p : cameraPoints())
            points.push_back(std::move(p));
        return points;
    }

    SceneData LevelConverter::sceneData()
    {
        SceneData dat;
        dat.points = spawnAndCameraPoints();
        if (m_globals)
            dat.points.push_back(globalsPoint());
        return dat;
    }

    Bytes LevelConverter::textureFile()
    {
        std::vector<const Texture *> all;
        for (const Texture &t : m_textures)
            all.push_back(&t);
        return writeTextureFile(all);
    }

    Bytes LevelConverter::sceneFile(const Vec3 &low, const Vec3 &high)
    {
        std::string lower;
        for (char c : m_slot)
            lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        return SceneFile::make(lower, low, high, kCell).write();
    }

    void LevelConverter::checkLevelBytes(const std::map<std::string, Bytes> &files)
    {
        size_t total = 0;
        for (const char *ext : {"GEO", "INS", "DAT", "TXR"})
            total += files.at(ext).size();
        report.say("level files: " + std::to_string(total) + " bytes (budget " + std::to_string(m_levelBytes) +
                   ", the game's largest stock level's)");
        if (total > m_levelBytes)
        {
            // (the reason the menu shows: one short line)
            char text[96];
            std::snprintf(text, sizeof text, "too big for its memory (level files %.1f MB, %.1f at most)", total / 1e6, m_levelBytes / 1e6);
            report.error(text);
        }
    }

    void LevelConverter::budgets(const Geometry &geo, const std::vector<Instance> &instances)
    {
        size_t parts = 0;
        for (const GeoObject &o : geo.objects)
            parts += o.parts.size();
        size_t texels = 0;
        for (const Texture &t : m_textures)
            texels += t.texels();
        report.say(m_slot + ": " + std::to_string(geo.objects.size()) + " objects, " + std::to_string(parts) + " parts, " +
                   std::to_string(instances.size()) + " instances, " + std::to_string(m_textures.size()) + " textures, " +
                   std::to_string(texels / 1024) + " KB of texels");
        if (parts > m_partsInView)
            report.error("too many parts to draw (" + std::to_string(parts) + ", " + std::to_string(m_partsInView) + " at most)");
        if (texels > static_cast<size_t>(m_texelBudgetKb) * 1024)
            report.error("too many texels (" + std::to_string(texels / 1024) + " KB, " + std::to_string(m_texelBudgetKb) + " at most)");
    }

    std::map<std::string, Bytes> LevelConverter::build()
    {
        readModel();
        if (m_placements.empty())
            throw LevelError("the model has no triangle mesh under a node");
        if (!m_globals)
        {
            // a sky of the model's own: the level's clear colour and fog from what it shows
            std::vector<const Piece *> skyPieces;
            for (size_t e = 0; e < m_pieces.size(); ++e)
                if (std::any_of(m_placements.begin(), m_placements.end(), [e](const Placement &pl) { return pl.sky && pl.pieces == e; }))
                    for (const Piece &p : m_pieces[e].second)
                        skyPieces.push_back(&p);
            if (const std::optional<SkyLook> look = skyLookOf(skyPieces))
                useSkyLook(*look);
        }
        std::vector<Texture *> textures;
        for (Texture &t : m_textures)
            textures.push_back(&t);
        fitTextures(textures, report, m_textureSide, m_texelBudgetKb);
        Geometry geo;
        std::vector<std::vector<size_t>> objectsOf(m_pieces.size()); // each entry's objects, by index into geo.objects
        std::vector<std::pair<uint32_t, Chunk>> definitions;          // the props' ZDYN by name hash, in the order made
        uint32_t nextId = m_firstObjectId;
        for (size_t e = 0; e < m_pieces.size(); ++e)
            for (const Piece &piece : m_pieces[e].second)
            {
                GeoObject obj = geoObject(piece, nextId);
                nextId += 2;
                if (piece.dynamics)
                {
                    // the prop's type and class data, its definition named
                    // by the object's name hash (what the game's lookup compares)
                    obj.typeData = ByteWriter().u32(kDynamicsObjectClass).bytes(dynamicsClassData(*piece.dynamics)).take();
                    const uint32_t hash = obj.header.nameHash;
                    if (std::none_of(definitions.begin(), definitions.end(), [hash](const auto &d) { return d.first == hash; }))
                    {
                        std::vector<Chunk> kids = {Chunk::leaf(tag("DDEF"), ByteWriter().u32(hash).take())};
                        kids.insert(kids.end(), piece.dynamics->chunks.begin(), piece.dynamics->chunks.end());
                        definitions.emplace_back(hash, Chunk::node(tag("ZDYN"), std::move(kids)));
                    }
                }
                geo.objects.push_back(std::move(obj));
                objectsOf[e].push_back(geo.objects.size() - 1);
            }
        // The definitions after every object, as the stock GEOs have them
        // (FORM { TGEO ..., ZDYN ... }).
        for (size_t n = 0; n < definitions.size(); ++n)
            geo.other.emplace_back(geo.objects.size() + n, std::move(definitions[n].second));
        if (!definitions.empty())
        {
            std::string props;
            for (const GeoObject &o : geo.objects)
                if (o.typeData && readU32(*o.typeData, 0) == kDynamicsObjectClass)
                {
                    char hash[16];
                    std::snprintf(hash, sizeof hash, "%08X", o.header.nameHash);
                    props += (props.empty() ? "" : ", ") + o.header.name + " (" + hash + ")";
                }
            report.say(std::to_string(definitions.size()) + " dynamics props: " + props);
        }
        std::vector<Instance> instances;
        std::vector<size_t> skies; // (index into instances)
        std::vector<size_t> skyObjects;
        for (const Placement &pl : m_placements)
            for (size_t n = 0; n < objectsOf[pl.pieces].size(); ++n)
            {
                const GeoObject &obj = geo.objects[objectsOf[pl.pieces][n]];
                const std::string name = shortName(n == 0 ? pl.nodeName : withNumber(pl.nodeName, n + 1), m_names);
                Instance inst = Instance::make(name, obj.header, pl.position, pl.rotation, pl.collides, ByteWriter().u32(kTerrainClass).take());
                inst.flags = pl.collides ? kInstanceCollides : kInstanceDecoration;
                if (pl.hidden)
                    inst.flags &= ~kInstanceShown;
                if (pl.sky)
                {
                    inst.flags = m_sky->instanceFlags;
                    skies.push_back(instances.size());
                    skyObjects.push_back(objectsOf[pl.pieces][n]);
                }
                if (pl.dynamics)
                {
                    inst.flags = m_dynamics->instanceFlags;
                    inst.tags = dynamicsInstanceTags(*m_pieces[pl.pieces].second[n].dynamics);
                }
                instances.push_back(std::move(inst));
            }
        // A sky sits at the level's centre (the game moves it to the camera
        // each frame: its place only sets its box).
        std::vector<Instance> others;
        for (const Instance &i : instances)
            if (!(i.flags & kInstanceSky))
                others.push_back(i);
        if (!skies.empty() && !others.empty())
        {
            Vec3 low{}, high{};
            worldBox(others, low, high);
            const Vec3 centre = {(low[0] + high[0]) / 2, (low[1] + high[1]) / 2, (low[2] + high[2]) / 2};
            for (size_t s = 0; s < skies.size(); ++s)
            {
                Instance &inst = instances[skies[s]];
                Instance moved = Instance::make(inst.name, geo.objects[skyObjects[s]].header, centre, inst.rotation, false, inst.tags);
                moved.flags = inst.flags;
                inst = std::move(moved);
            }
        }
        tagEdges(geo, instances);
        Instances ins;
        ins.instances = instances;
        SceneData dat = sceneData();
        Vec3 low{}, high{};
        worldBox(instances, low, high);
        std::map<std::string, Bytes> files;
        files["GEO"] = geo.write(m_engine);
        files["INS"] = ins.write(m_engine);
        files["DAT"] = dat.write(m_engine);
        files["TXR"] = textureFile();
        files["ZIT"] = sceneFile(low, high);
        budgets(geo, instances);
        return files;
    }

    // ---- the entry ------------------------------------------------------------------------------------------

    namespace
    {
        Converted convertForAi(const Model &model)
        {
            LevelConverter conv(model, "TUT", Engine::Zge3);
            std::map<std::string, Bytes> files = conv.build();
            conv.checkLevelBytes(files);
            if (!conv.report.errors.empty())
                throw LevelError(conv.report.errors.front());
            Converted out;
            const std::string folder = "ASSETS/LEVELS/TUT/";
            for (auto &[ext, data] : files)
                out.files[folder + "TUT." + ext] = std::move(data);
            out.files[folder + "TUT.BIN"] = emptyLevelScript();
            out.files[folder + "TUTT.BIN"] = emptyLevelScript();
            out.files[folder + "MOTIONS.ZAL"] = emptyAssetLibrary();
            out.report = conv.report.text();
            return out;
        }
    }

    Converted convertModel(const Model &model, Engine target)
    {
        switch (target)
        {
        case Engine::Zge2:
            return convertForDm2(model);
        case Engine::Zge4:
            return convertForBmx(model);
        case Engine::Zge3:
        default:
            return convertForAi(model);
        }
    }
}
