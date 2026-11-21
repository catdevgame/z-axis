// Aggressive Inline's panel files (collection/extras/panel_maps.h): UI.pmap
// from the executable's trick tables, the string table and the UI library;
// a level's map from its collidable faces in their textures' mean colours,
// lit by their vertex colours, with what the skater cannot reach dimmed and
// the grind rails drawn; and its goals from its script (panel_goals.h).
#include "sym/collection/extras/panel_maps_ai.cpp.h"
#include "panel_goals.h"

#include <map>

namespace zx::app::extras::panel_maps
{
    namespace
    {
        // The career's levels (folder, display name): the game's own folder table.
        const std::vector<std::pair<std::string, std::string>> kLevels = {
            {"MOVIE", "Movie Lot"}, {"CIVIC", "Civic Center"}, {"INDUST", "Industrial"}, {"BRDWLK", "Boardwalk"},
            {"CANNERY", "Cannery"}, {"HANGER", "Airfield"},    {"MUSEUM", "Museum"},     {"TUT", "Tutorial"},
        };

        constexpr uint32_t kFaceCollides = 0x2000;     // a face the skater collides with (TGFP flags)
        constexpr uint32_t kInstanceCollides = 0x2;    // TGIH flags: the instance collides
        constexpr uint32_t kInstanceNoCollision = 0x10; // with 0x2 on panoramas: it does not
        constexpr uint16_t kSpawnClass = 0x46BE;       // a DataPoint's class: a spawn point

        bool contains(const std::string &text, std::string_view word) { return text.find(word) != std::string::npos; }

        // ---- textures: only their mean colours -------------------------------------

        // A texture's mean colour (r, g, b) and its alpha share.
        struct Albedo
        {
            double r = 128, g = 128, b = 128, alpha = 1.0;
        };

        // [start, stop) of a Python slice of `size` bytes.
        std::pair<size_t, size_t> pythonSlice(int64_t size, int64_t start, int64_t stop)
        {
            auto clip = [&](int64_t v) { return v < 0 ? std::max<int64_t>(0, size + v) : std::min(v, size); };
            const int64_t a = clip(start), b = clip(stop);
            return {size_t(a), size_t(std::max(a, b))};
        }

        // The mean colour of a TBMP / TZMP picture (an 8- or 4-bit BMP; a
        // TZMP's palette has alpha, a TBMP's leaves it 0), weighted by alpha.
        std::optional<Albedo> bmpMean(ByteView data)
        {
            if (!data.startsWith("BM"))
                return std::nullopt;
            const int64_t off = data.u32(10), head = data.u32(14);
            const int64_t width = data.s32(18);
            const int64_t height = std::abs(int64_t(data.s32(22)));
            const int64_t bpp = data.u16(28);
            int64_t colours = data.u32(46);
            if (colours == 0)
                colours = int64_t(1) << std::min<int64_t>(bpp, 62);
            if (bpp != 4 && bpp != 8)
                return std::nullopt;
            const ByteView palette = data.slice(size_t(14 + head), size_t(14 + head + colours * 4));
            const auto [from, to] = pythonSlice(int64_t(data.size), off, off + ((floorDiv(width * bpp, 8) + 3) & ~int64_t(3)) * height);
            int64_t counts[256] = {};
            for (size_t i = from; i < to; ++i)
            {
                const uint8_t v = data.data[i];
                if (bpp == 4)
                {
                    ++counts[v >> 4];
                    ++counts[v & 15];
                }
                else
                    ++counts[v];
            }
            bool hasAlpha = false;
            for (size_t i = 0; i < palette.size / 4; ++i)
                hasAlpha = hasAlpha || palette.data[i * 4 + 3];
            int64_t total = 0, weight = 0, r = 0, g = 0, b = 0;
            for (int64_t value = 0; value < 256; ++value)
            {
                const int64_t n = counts[value];
                if (n == 0 || size_t(value * 4 + 3) >= palette.size)
                    continue;
                const int64_t bb = palette.data[value * 4], gg = palette.data[value * 4 + 1],
                              rr = palette.data[value * 4 + 2], aa = palette.data[value * 4 + 3];
                int64_t a = hasAlpha ? aa : 128;
                if (rr > 200 && bb > 200 && gg < 70)
                    a = 0; // the art's colour key
                total += n;
                weight += n * a;
                r += n * a * rr;
                g += n * a * gg;
                b += n * a * bb;
            }
            if (!weight)
                return Albedo{128, 128, 128, 0.0};
            return Albedo{double(r) / double(weight), double(g) / double(weight), double(b) / double(weight),
                          double(weight) / (double(total) * 128.0)};
        }

        std::unordered_map<uint32_t, Albedo> textureMeans(ByteView txr)
        {
            std::unordered_map<uint32_t, Albedo> means;
            const Chunks tree = parseChunks(txr);
            if (tree.empty() || !tree[0].container)
                throw PanelError("a level's textures (TXR) that do not read");
            for (const Chunk &texture : tree[0].kids)
            {
                if (texture.tag != "TXRC")
                    continue;
                std::optional<uint32_t> ident;
                for (const Chunk &k : texture.kids)
                {
                    if (k.tag == "TXRH")
                    {
                        ident = k.data.u32(0);
                        if (contains(upperAscii(k.data.text(0x14)), "INVIS"))
                            means[*ident] = Albedo{0, 0, 0, 0.0}; // collision walls: nothing to see
                    }
                    else if ((k.tag == "TBMP" || k.tag == "TZMP") && ident && !means.count(*ident))
                        if (const std::optional<Albedo> mean = bmpMean(k.data))
                            means[*ident] = *mean;
                }
            }
            return means;
        }

        // ---- geometry ------------------------------------------------------------

        // The last chunk of each tag among a part's (a dict of them).
        std::map<std::string, const Chunk *> streamsOf(const Chunk &part)
        {
            std::map<std::string, const Chunk *> out;
            for (const Chunk &k : part.kids)
                out[k.tag] = &k;
            return out;
        }

        const Chunk &stream(const std::map<std::string, const Chunk *> &streams, const char *tag)
        {
            const auto it = streams.find(tag);
            if (it == streams.end())
                throw PanelError(std::string("a level part without its ") + tag);
            return *it->second;
        }

        using Face = std::array<uint16_t, 4>; // flags, three vertices

        std::vector<Face> facesOf(const Chunk &tgfp, size_t count)
        {
            std::vector<Face> out(count);
            for (size_t i = 0; i < count; ++i)
                for (size_t k = 0; k < 4; ++k)
                    out[i][k] = tgfp.data.u16(8 * i + 2 * k);
            return out;
        }

        std::vector<Vec3> vertsOf(const Chunk &tgvp, size_t count)
        {
            std::vector<Vec3> out(count);
            for (size_t i = 0; i < count; ++i)
                out[i] = {tgvp.data.f32(12 * i), tgvp.data.f32(12 * i + 4), tgvp.data.f32(12 * i + 8)};
            return out;
        }

        // An indexed stream (TIDA, TII8 | TII6) as the plain one: `count` elements of `stride`.
        Bytes expandIndexed(const Chunk &indexed, size_t count, size_t stride)
        {
            std::optional<ByteView> table;
            std::optional<std::vector<size_t>> indices;
            for (const Chunk &k : indexed.kids)
            {
                if (k.tag == "TIDA")
                    table = k.data;
                else if (k.tag == "TII8")
                    indices = std::vector<size_t>(k.data.data, k.data.data + k.data.size);
                else if (k.tag == "TII6")
                {
                    std::vector<size_t> v(k.data.size / 2);
                    for (size_t i = 0; i < v.size(); ++i)
                        v[i] = k.data.u16(2 * i);
                    indices = std::move(v);
                }
            }
            if (!table || !indices)
                throw PanelError("an indexed " + indexed.tag + " without its table or indices");
            if (indices->size() < count)
                throw PanelError("an indexed " + indexed.tag + " with too few indices");
            Bytes out(count * stride);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t at = (*indices)[i] * stride;
                if (at + stride > table->size)
                    throw PanelError("an indexed " + indexed.tag + " past its table");
                std::memcpy(out.data() + i * stride, table->data + at, stride);
            }
            return out;
        }

        // An object's collision part: its vertices, faces, vertex colours and materials' colours.
        struct Surface
        {
            std::vector<Vec3> verts;
            std::vector<Face> faces;
            Bytes colours; // RGBA a vertex (empty: none)
            std::vector<Albedo> albedo;
        };

        std::unordered_map<uint32_t, Surface> readObjects(const Chunks &geo, const std::unordered_map<uint32_t, Albedo> &means)
        {
            std::unordered_map<uint32_t, Surface> objects;
            for (const Chunk &obj : geo[0].kids)
            {
                if (obj.tag != "TGEO")
                    continue;
                const Chunk *head = obj.first("TGHD");
                if (!head)
                    throw PanelError("a level object without its head");
                const uint32_t ident = head->data.u32(0x18);
                // the most detailed part with collision faces (they share it, or the last part has them)
                const Chunk *best = nullptr;
                std::map<std::string, const Chunk *> streams;
                size_t nv = 0;
                std::vector<Face> faces;
                for (const Chunk &part : obj.kids)
                {
                    if (part.tag != "TGPT")
                        continue;
                    streams = streamsOf(part);
                    const Chunk &tgph = stream(streams, "TGPH");
                    nv = tgph.data.u16(4);
                    faces = facesOf(stream(streams, "TGFP"), tgph.data.u16(6));
                    if (std::any_of(faces.begin(), faces.end(), [](const Face &f) { return f[0] & kFaceCollides; }))
                    {
                        best = &part;
                        break;
                    }
                }
                if (!best)
                    continue;
                Surface s;
                s.verts = vertsOf(stream(streams, "TGVP"), nv);
                for (const Face &f : faces)
                    if (std::max({f[1], f[2], f[3]}) < nv)
                        s.faces.push_back(f);
                if (streams.count("TGVC"))
                    s.colours = streams["TGVC"]->data.bytes();
                else if (streams.count("TIVC"))
                    s.colours = expandIndexed(*streams["TIVC"], nv, 4);
                const Chunk *table = best->first("TMTB");
                if (!table)
                    table = obj.first("TMTB");
                if (table)
                    for (const Chunk &mat : table->kids)
                    {
                        if (mat.tag != "MATI")
                            continue;
                        const uint32_t layers = mat.data.u32(0x2C);
                        const uint32_t texture = layers ? mat.data.u32(0x40) : 0;
                        const auto it = means.find(texture);
                        s.albedo.push_back(it == means.end() ? Albedo{128, 128, 128, 1.0} : it->second);
                    }
                objects[ident] = std::move(s);
            }
            return objects;
        }

        // The level's instances, placed in the world: an instance with a
        // parent (+0x44, the parent's id; its own at +0x48: decals, statue
        // parts, animated pieces) is placed in its parent's space.
        class Placer
        {
        public:
            explicit Placer(const Bytes &ins)
            {
                m_tree = parseChunks(ins);
                walkChunks(m_tree, [&](const Chunk &c) {
                    if (c.tag == "TGIH")
                        instances.push_back(c.data);
                });
                for (const ByteView &d : instances)
                    m_byId[d.u32(0x48)] = d;
            }

            Vec3 place(ByteView d, Vec3 v) const
            {
                for (int i = 0; i < 16; ++i)
                {
                    const dm2::Vec3 turned = dm2::quatRotate({d.f32(8), d.f32(12), d.f32(16), d.f32(20)}, v);
                    v = {turned[0] + d.f32(0x18), turned[1] + d.f32(0x1C), turned[2] + d.f32(0x20)};
                    const uint32_t parent = d.u32(0x44);
                    const auto it = parent ? m_byId.find(parent) : m_byId.end();
                    if (it == m_byId.end())
                        break;
                    d = it->second;
                }
                return v;
            }

            static bool collides(ByteView d)
            {
                return (d.u32(0) & (kInstanceCollides | kInstanceNoCollision)) == kInstanceCollides;
            }
            static std::string name(ByteView d) { return d.text(0x50); }

            std::vector<ByteView> instances;

        private:
            Chunks m_tree;
            std::unordered_map<uint32_t, ByteView> m_byId;
        };

        // Left off the map: a roof hides what the level is about (the Movie
        // Lot's studio, the Airfield's hangars; ceilings face down and go with
        // the back faces), a shadow or a light's cone is no surface.
        bool hiddenInstance(const std::string &name)
        {
            const std::string lower = lowerAscii(name);
            for (const char *word : {"roof", "skylight", "ceiling", "shadow", "spotlight"})
                if (contains(lower, word))
                    return true;
            return false;
        }

        // Every collidable face seen from above (its colour: its three
        // corners', the material's lit by the vertex colours), and the
        // invisible walls, which draw nothing but bound the level.
        void worldFaces(const Chunks &geo, const Placer &placer, const std::unordered_map<uint32_t, Albedo> &means,
                        std::vector<MapFace> &faces, std::vector<std::array<Vec3, 3>> &barriers)
        {
            const std::unordered_map<uint32_t, Surface> objects = readObjects(geo, means);
            std::vector<Vec3> world;
            for (const ByteView &d : placer.instances)
            {
                if (!Placer::collides(d))
                    continue; // sky planes, fog cards and decals: what the skater never touches
                const auto found = objects.find(d.u32(0x40));
                if (found == objects.end() || hiddenInstance(Placer::name(d)))
                    continue;
                const Surface &s = found->second;
                world.clear();
                for (const Vec3 &v : s.verts)
                    world.push_back(placer.place(d, v));
                const std::string lower = lowerAscii(Placer::name(d));
                const bool water = contains(lower, "water") || contains(lower, "deathplane"); // the sea, and what kills under it
                for (const Face &f : s.faces)
                {
                    if (!(f[0] & kFaceCollides))
                        continue;
                    const Vec3 &a = world[f[1]], &b = world[f[2]], &c = world[f[3]];
                    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                    const double length = std::pow(nx * nx + ny * ny + nz * nz, 0.5);
                    if (length == 0)
                        continue;
                    const size_t material = f[0] & 0xFF;
                    const Albedo al = material < s.albedo.size() ? s.albedo[material] : Albedo{128, 128, 128, 1};
                    if (al.alpha == 0.0 && std::fabs(ny) < 0.5 * length)
                        barriers.push_back({a, b, c});
                    if (ny > 0)
                        continue; // +y is down: this face looks down
                    if (al.alpha < 0.55)
                        continue; // shadows and other see-through cards
                    // each corner's own colour (the material's, lit by the
                    // corner's vertex colour): the picture blends them across
                    // the face as the game draws it
                    MapFace t;
                    t.cornerColours = true;
                    for (size_t k = 0; k < 3; ++k)
                    {
                        if (!s.colours.empty())
                        {
                            const size_t i = size_t(f[1 + k]) * 4;
                            if (i + 2 >= s.colours.size())
                                throw PanelError("a level part with too few vertex colours");
                            t.colour[k] = {al.r * s.colours[i] / 128.0, al.g * s.colours[i + 1] / 128.0,
                                           al.b * s.colours[i + 2] / 128.0};
                        }
                        else
                            t.colour[k] = {al.r, al.g, al.b};
                    }
                    double mean[3];
                    for (size_t k = 0; k < 3; ++k)
                    {
                        const double v[3] = {t.colour[0][k], t.colour[1][k], t.colour[2][k]};
                        mean[k] = pythonSum(v, 3) / 3;
                    }
                    if (!water && pyMax(pyMax(mean[0], mean[1]), mean[2]) < 14)
                        continue; // unlit cards (shadows, the backs of things)
                    if (water)
                        t.cornerColours = false; // painted the water's colour
                    t.top = pyMin(pyMin(a[1], b[1]), c[1]);
                    t.a = a;
                    t.b = b;
                    t.c = c;
                    t.normal = {nx / length, ny / length, nz / length};
                    t.water = water;
                    faces.push_back(t);
                }
            }
        }

        // The level's spawn points (DataPoint: class +4, position +0x10).
        std::vector<Seed> spawnPoints(const Chunks &dat)
        {
            std::vector<Seed> out;
            walkChunks(dat, [&](const Chunk &c) {
                if (c.tag == "SDPT" && c.data.size >= 0x2C && c.data.u16(4) == kSpawnClass)
                    out.push_back({c.data.f32(0x10), c.data.f32(0x18), std::nullopt});
            });
            return out;
        }

        // Where the level puts its named things: the instances and data
        // points by name hash (the first of a name), their names where known
        // and their heights.
        struct Places
        {
            std::vector<uint32_t> order;
            std::unordered_map<uint32_t, std::pair<double, double>> at;
            std::unordered_map<uint32_t, double> heights;
            std::unordered_map<uint32_t, std::string> names;

            void add(uint32_t key, double x, double y, double z)
            {
                if (at.emplace(key, std::make_pair(x, z)).second)
                    order.push_back(key);
                heights.emplace(key, y);
            }
            std::string nameOf(uint32_t key) const
            {
                const auto it = names.find(key);
                return it == names.end() ? std::string() : it->second;
            }
            bool helper(uint32_t key) const { return contains(lowerAscii(nameOf(key)), "helper"); }
        };

        Places objectPlaces(const Placer &placer, const Chunks &dat)
        {
            Places p;
            for (const ByteView &d : placer.instances)
            {
                const std::string name = Placer::name(d);
                const uint32_t key = dm2::nameHash(name);
                p.names[key] = name;
                const Vec3 at = placer.place(d, {0.0, 0.0, 0.0});
                p.add(key, at[0], at[1], at[2]);
            }
            walkChunks(dat, [&](const Chunk &c) {
                if (c.tag == "SDPT" && c.data.size >= 0x2C)
                    p.add(c.data.u32(8), c.data.f32(0x10), c.data.f32(0x14), c.data.f32(0x18));
            });
            return p;
        }

        // Where the level puts things for the skater: what lies on a ledge
        // nothing rolls up to is in play too. Terrain's origins say nothing,
        // and the script's holders sit out of the way.
        std::vector<Seed> playSeeds(const Places &places)
        {
            std::vector<Seed> out;
            for (const uint32_t key : places.order)
            {
                const std::string name = lowerAscii(places.nameOf(key));
                if ((contains(name, "prop") || contains(name, "script") || contains(name, "npc")) &&
                    !contains(name, "helper") && !contains(name, "terrain"))
                {
                    const auto &[x, z] = places.at.at(key);
                    out.push_back({x, z, places.heights.at(key)});
                }
            }
            return out;
        }

        // Every grindable edge of the collidable instances in world space: an
        // edge is face * 3 + k of the object's part with the TGFE, from the
        // face's vertex k to k + 1, grindable when its record's flags (+9)
        // have bit 0.
        std::vector<std::array<Vec3, 2>> grindEdges(const Chunks &geo, const Placer &placer)
        {
            std::unordered_map<uint32_t, std::vector<std::array<Vec3, 2>>> objects;
            for (const Chunk &obj : geo[0].kids)
            {
                if (obj.tag != "TGEO")
                    continue;
                const Chunk *head = obj.first("TGHD");
                if (!head)
                    throw PanelError("a level object without its head");
                for (const Chunk &part : obj.kids)
                {
                    if (part.tag != "TGPT")
                        continue;
                    const auto streams = streamsOf(part);
                    if (!streams.count("TGFE"))
                        continue;
                    const Chunk &tgph = stream(streams, "TGPH");
                    const size_t nv = tgph.data.u16(4), nf = tgph.data.u16(6);
                    const std::vector<Vec3> verts = vertsOf(stream(streams, "TGVP"), nv);
                    const std::vector<Face> faces = facesOf(stream(streams, "TGFP"), nf);
                    // the tag buffer: a descriptor (bits 0-3 the index kind, 1 =
                    // u16; 4-13 the record size; 14-31 the count), the index
                    // count, the indices padded to 16 bytes, the records
                    const ByteView tags = stream(streams, "TGFE").data;
                    const uint32_t desc = tags.u32(0), count = tags.u32(4);
                    if ((desc & 0xF) != 1)
                        throw PanelError(formatted("a tag buffer of index kind %u", desc & 0xF));
                    const size_t size = (desc >> 4) & 0x3FF, records = desc >> 14;
                    const size_t indexBytes = (size_t(count) * 2 + 15) & ~size_t(15);
                    const ByteView body = tags.slice(8 + indexBytes);
                    if (body.size != size * records)
                        throw PanelError("a tag buffer of the wrong size");
                    std::vector<std::array<Vec3, 2>> edges;
                    for (size_t e = 0; e < count; ++e)
                    {
                        const uint16_t r = tags.u16(8 + 2 * e);
                        if (r == 0xFFFF)
                            continue;
                        if (r >= records || size < 10)
                            throw PanelError("a tag buffer's index past its records");
                        if (!(body.data[r * size + 9] & 1) || e / 3 >= nf)
                            continue;
                        const Face &face = faces[e / 3];
                        const size_t i0 = face[1 + e % 3], i1 = face[1 + (e + 1) % 3];
                        if (std::max(i0, i1) < nv)
                            edges.push_back({verts[i0], verts[i1]});
                    }
                    objects[head->data.u32(0x18)] = std::move(edges);
                }
            }
            std::vector<std::array<Vec3, 2>> out;
            for (const ByteView &d : placer.instances)
            {
                if (!Placer::collides(d))
                    continue;
                const auto found = objects.find(d.u32(0x40));
                if (found != objects.end())
                    for (const auto &[a, b] : found->second)
                        out.push_back({placer.place(d, a), placer.place(d, b)});
            }
            return out;
        }

        // ---- goals ---------------------------------------------------------------------

        struct GoalStats
        {
            int goals = 0, placed = 0, counted = 0, targets = 0;
        };

        // SCRP, GOAL, TARG, GIVE and TEXT for one level script. SCRP says which
        // script the set is for, as the game has it loaded: its variables'
        // bytes and each objective's name and description (Objective +0x18,
        // +0x1C: offsets in the script's strings). The PS2 disc's script and
        // the Xbox release's differ in one or the other wherever their goals do.
        Bytes goalChunks(const LevelScript &script, const Places &places, GoalStats &stats)
        {
            const std::vector<ScriptGoal> goals = analyseGoals(script);
            TextTable text;
            Bytes goalData, targetData;
            int64_t targetCount = 0;
            for (const ScriptGoal &g : goals)
            {
                const int64_t first = targetCount;
                GoalFields f;
                for (const auto &[hash, done] : g.targets)
                {
                    if (places.helper(hash) || !places.at.count(hash))
                        continue; // an out-of-the-way script holder, not a place
                    const auto &[x, z] = places.at.at(hash);
                    putTarget(targetData, hash, x, z, done ? done->word : -1, done ? done->value : 0, kTargetPlace);
                    ++targetCount;
                }
                const uint32_t owner = g.objective.owner;
                if (targetCount == first && places.at.count(owner) && !places.helper(owner))
                {
                    f.anchorX = places.at.at(owner).first;
                    f.anchorZ = places.at.at(owner).second;
                }
                f.title = text.add(g.objective.title);
                f.text = text.add(g.objective.text);
                f.owner = owner;
                if (g.counter)
                {
                    f.counterWord = g.counter->first;
                    f.counterTarget = g.counter->second;
                }
                f.firstTarget = first;
                f.targetCount = targetCount - first;
                f.type = g.objective.type;
                f.tier = g.objective.tier;
                f.locked = g.objective.locked;
                f.points = g.objective.points;
                if (g.score)
                {
                    f.scoreKind = g.score->first;
                    f.scoreTarget = g.score->second;
                }
                putGoal(goalData, f);
                ++stats.goals;
                stats.placed += (targetCount > first || f.anchorX == f.anchorX) ? 1 : 0;
                stats.counted += (g.counter || g.score) ? 1 : 0;
            }
            stats.targets = int(targetCount);
            Bytes key;
            putU32(key, int64_t(script.variableCount) * 4);
            putU32(key, int64_t(goals.size()));
            for (const ScriptGoal &g : goals)
            {
                putU32(key, g.objective.titleAt);
                putU32(key, g.objective.textAt);
            }
            Bytes out = chunk("SCRP", key);
            Bytes goal;
            putU32(goal, int64_t(goals.size()));
            putBytes(goal, goalData);
            putBytes(out, chunk("GOAL", goal));
            Bytes targ;
            putU32(targ, targetCount);
            putBytes(targ, targetData);
            putBytes(out, chunk("TARG", targ));
            const std::vector<NpcBubble> givers = npcBubbles(script);
            Bytes give;
            putU32(give, int64_t(givers.size()));
            for (const NpcBubble &b : givers)
            {
                const auto it = places.at.find(b.npc);
                const std::pair<double, double> at = it == places.at.end() ? std::make_pair(0.0, 0.0) : it->second;
                putBubble(give, b.npc, at.first, at.second, b.word, b.state);
            }
            putBytes(out, chunk("GIVE", give));
            putBytes(out, chunk("TEXT", text.bytes));
            return out;
        }

        // A level's files by extension; none when it has no such file.
        using LevelReader = std::function<std::optional<Bytes>(const std::string &ext)>;

        Bytes need(const LevelReader &read, const std::string &level, const std::string &ext)
        {
            std::optional<Bytes> data = read(ext);
            if (!data)
                throw PanelError("the level " + level + " has no " + ext + " file");
            return std::move(*data);
        }

        // The level's file. `xboxScript`: the Xbox release's .BIN of the
        // level (which the career scripts setting runs), for a second goal
        // set where it differs (a custom map's: its free skate script; the
        // log line still says Xbox).
        Bytes levelBytes(const LevelReader &read, const std::string &level, const std::string &title,
                         const std::optional<Bytes> &xboxScript, std::vector<std::string> &log)
        {
            const Bytes geoBytes = need(read, level, "GEO"), insBytes = need(read, level, "INS"),
                        txrBytes = need(read, level, "TXR"), datBytes = need(read, level, "DAT");
            const Chunks geo = parseChunks(geoBytes);
            if (geo.empty() || !geo[0].container)
                throw PanelError("the level " + level + "'s geometry does not read");
            const Chunks dat = parseChunks(datBytes);
            const Placer placer(insBytes);
            std::vector<MapFace> faces;
            std::vector<std::array<Vec3, 3>> barriers;
            worldFaces(geo, placer, textureMeans(txrBytes), faces, barriers);
            const Box box = levelBox(faces, spawnPoints(dat));
            const Frame frame(box, pixelsPerUnit(box));
            MapPicture picture = rasterise(faces, frame);
            const Places places = objectPlaces(placer, dat);
            std::vector<Seed> seeds = spawnPoints(dat);
            for (const Seed &s : playSeeds(places))
                seeds.push_back(s);
            const double inPlay = muteOutOfPlay(picture, frame, seeds, barriers);
            fillGaps(picture, frame.ppu);
            outline(picture);
            drawRails(picture, frame, grindEdges(geo, placer));
            log.push_back(formatted("%s: %d faces, box x %.0f .. %.0f z %.0f .. %.0f, %dx%d%s, %.0f%% in play", level.c_str(),
                                 int(faces.size()), box.x0, box.x1, box.z0, box.z1, int(frame.width), int(frame.height),
                                 frame.turned ? " turned" : "", inPlay * 100));
            GoalStats stats;
            Bytes goals;
            const std::optional<Bytes> script = read("BIN");
            if (script && !script->empty())
            {
                goals = goalChunks(LevelScript(*script), places, stats);
                if (xboxScript && !xboxScript->empty() && *xboxScript != *script)
                {
                    GoalStats xbox;
                    putBytes(goals, goalChunks(LevelScript(*xboxScript), places, xbox));
                    log.push_back(formatted("%s: the Xbox script's goals too: %d counted, %d targets", level.c_str(),
                                         xbox.counted, xbox.targets));
                }
            }
            Bytes file = levelFile(title, frame, picture, goals);
            log.push_back(formatted("%s: file %.1f MB, %d goals, %d placed, %d counted, %d targets", level.c_str(),
                                 double(file.size()) / 1e6, stats.goals, stats.placed, stats.counted, stats.targets));
            return file;
        }

        void buildLevel(const LevelReader &read, const std::string &level, const std::string &title,
                        const std::filesystem::path &out, const std::optional<Bytes> &xboxScript,
                        std::vector<std::string> &log)
        {
            writeMade(out / (level + ".pmap"), levelBytes(read, level, title, xboxScript, log));
        }

        // ---- UI.pmap ---------------------------------------------------------------------

        // The string table (ASSETS/STRING.EUS): u32 text bytes, u32 count,
        // {u32 name hash, u32 offset}[count]; a key it lacks is its own text.
        class StringTable
        {
        public:
            explicit StringTable(ByteView data)
            {
                const uint32_t count = data.u32(4);
                for (uint32_t i = 0; i < count; ++i)
                    m_texts[data.u32(8 + 8 * size_t(i))] = data.zeroEnded(data.u32(12 + 8 * size_t(i)));
            }
            std::string text(const std::string &key) const
            {
                const auto it = m_texts.find(dm2::nameHash(key));
                return it == m_texts.end() ? key : it->second;
            }

        private:
            std::unordered_map<uint32_t, std::string> m_texts;
        };

        // The pause menu's special trick list (inlinePausePopulateSpecialTrickList):
        // ten rows of an unlock bit, a name and its buttons, in three tables.
        constexpr uint32_t kSpecialBits = sym::specialTrickBits, kSpecialNames = sym::specialTrickNames, kSpecialButtons = sym::specialTrickPads;
        constexpr uint32_t kSpecialTricks = 10;

        Bytes specialTricks(const Elf &elf)
        {
            Bytes body;
            putU32(body, kSpecialTricks);
            for (uint32_t i = 0; i < kSpecialTricks; ++i)
            {
                putU32(body, elf.word(kSpecialBits + 4 * i));
                putPadded(body, elf.text(elf.word(kSpecialNames + 4 * i)), 47, 48);
                putPadded(body, elf.text(elf.word(kSpecialButtons + 4 * i)), 15, 16);
            }
            return chunk("SPEC", body);
        }

        // The trick tables: {create, unlock bit, pattern[2], mask[2]} with the
        // recent stick directions newest first (tricks.cpp TrickButtonEntry):
        // the air table's, the grind table's and the stall's and manual's. A
        // table names a trick only by the function that makes it: these are
        // those functions (air_tricks.cpp, trick_objects.cpp, stall_trick.cpp)
        // by the string table's keys of the tricks' texts.
        constexpr uint32_t kAirTable = sym::g_airTricks, kAirEntries = 55;
        constexpr uint32_t kGrindTable = sym::g_trickButtons, kGrindEntries = 25;
        constexpr uint32_t kStallTable = sym::g_stallTricks, kStallEntries = 4;
        constexpr uint32_t kManualTable = sym::g_manualTricks, kManualEntries = 2;
        constexpr uint32_t kTrickEntryBytes = 0x18;
        constexpr int64_t kButtonAir = 'x', kButtonGrind = 'y';
        enum Family : int64_t
        {
            FamilyGrab,
            FamilyFlip,
            FamilyCombo,
            FamilyGrind,
            FamilyOther,
            FamilySpecial,
        };

        // The air tricks' makers: the grabs' (0x70 bytes each), the flips'
        // and the special airs' (0x58), then the grab-and-flip combos' (0x78).
        constexpr uint32_t kGrabMakers = sym::newRocketGrab, kFlipMakers = sym::newFrontFlip, kSpecialAirMakers = sym::newSupermanLateFlip,
                           kComboMakers = sym::newRocketFrontFlip;
        const std::vector<std::string> kGrabKeys = {"Rocket", "LongDick", "StaleAir", "LookBackJudo",
                                                    "Abstract", "ParallelThat", "Method", "LuiKang"};
        // (Two flips share the text "Flatspin"; the rows say which way round, in the HUD's words for a spin.)
        const std::vector<std::string> kFlipKeys = {"FrontFlip", "HotFlip",   "Flatspin", "BioFlip",
                                                    "BackFlip",  "MistyFlip", "Flatspin", "McTwistFlip"};
        std::string flipSuffix(int64_t flip) { return flip == 2 ? " CW" : flip == 6 ? " CCW" : ""; }
        const std::vector<std::string> kSpecialAirKeys = {"SupermanLateFlip", "DoubleBackflip", "AbstractDoubleBackflip",
                                                          "Bio", "Corkscrew", "DoubleFlatspin", "OneFourFourZero"};
        const std::vector<std::string> kComboGrabKeys = {"Rocket", "StaleAir", "Abstract", "Method"};
        const std::unordered_map<uint32_t, std::string> kGrindKeys = {
            {sym::newSoul, "SoulGrind"},          {sym::newBackSlide, "BackslideGrind"},
            {sym::newMiszou, "MiszouGrind"},        {sym::newFishBrain, "FishBrainGrind"},
            {sym::newRoyale, "RoyaleGrind"},        {sym::newMakio, "MakioGrind"},
            {sym::newPornstar, "PornstarGrind"},      {sym::newFastslide, "FastslideGrind"},
            {sym::newTorqueSoul, "TorqueSoulGrind"},    {sym::newSoyale, "SoyaleGrind"},
            {sym::newX, "XGrind"},             {sym::newAcidSoul, "AcidSoulGrind"},
            {sym::newFreestyleFastslide, "FreestyleFastslideGrind"}, {sym::newFreestyleBackslide, "FreestyleBackslideGrind"},
            {sym::newFreestyleFishbrain, "FreestyleFishbrainGrind"}, {sym::newFreestyleMakio, "FreestyleMakioGrind"},
            {sym::newWonder, "WonderGrind"},        {sym::newSweatStance, "SweatStanceGrind"},
            {sym::newMistrial, "MistrialGrind"},      {sym::newUnity, "UnityGrind"},
            {sym::newSfCombo, "SamFogertyGrindCombo"}, {sym::newFmCombo, "FrankieMoralesGrindCombo"},
            {sym::newMsCombo, "MattSalernoGrindCombo"}, {sym::newDoubleGrindTrick, "DoubleGrind"},
            {sym::newHandGrindTrick, "HandGrind"}};
        const std::unordered_map<uint32_t, std::string> kStallKeys = {{sym::newAbstractHandPlant, "AbstractHandPlant"},
                                                                      {sym::newRocketHandPlant, "RocketHandPlant"},
                                                                      {sym::newMethodHandPlant, "MethodHandPlant"},
                                                                      {sym::newStaleHandPlant, "StaleHandPlant"}};
        const std::unordered_map<uint32_t, std::string> kManualKeys = {{sym::newSingleHeelManual, "SingleHeelManual"},
                                                                       {sym::newSingleToeManual, "SingleToeManual"}};

        // A list's element by a Python index (a negative one from the end).
        const std::string &listed(const std::vector<std::string> &list, int64_t index)
        {
            const int64_t at = index < 0 ? index + int64_t(list.size()) : index;
            if (at < 0 || at >= int64_t(list.size()))
                throw PanelError("a trick maker the trick list does not know");
            return list[size_t(at)];
        }

        const std::string &keyed(const std::unordered_map<uint32_t, std::string> &keys, uint32_t maker)
        {
            const auto it = keys.find(maker);
            if (it == keys.end())
                throw PanelError(formatted("a trick maker the trick list does not know (0x%08x)", maker));
            return it->second;
        }

        // (family, text) of an air table's maker.
        std::pair<int64_t, std::string> airTrickName(uint32_t create, const StringTable &strings)
        {
            if (kGrabMakers <= create && create < kFlipMakers)
                return {FamilyGrab, strings.text(listed(kGrabKeys, (create - kGrabMakers) / 0x70))};
            if (kFlipMakers <= create && create < kSpecialAirMakers)
            {
                const int64_t flip = (create - kFlipMakers) / 0x58;
                return {FamilyFlip, strings.text(listed(kFlipKeys, flip)) + flipSuffix(flip)};
            }
            if (kSpecialAirMakers <= create && create < kComboMakers)
                return {FamilySpecial, strings.text(listed(kSpecialAirKeys, (create - kSpecialAirMakers) / 0x58))};
            const int64_t index = floorDiv(int64_t(create) - kComboMakers, 0x78);
            const int64_t flip = floorMod(index, 8);
            return {FamilyCombo, strings.text(listed(kComboGrabKeys, floorDiv(index, 8))) + " " +
                                     strings.text(listed(kFlipKeys, flip)) + flipSuffix(flip)};
        }

        // The directions of a table entry, oldest first (the masks are made at start-up).
        Bytes trickInputs(const Elf &elf, uint32_t entry)
        {
            const uint32_t pattern = elf.word(entry + 8);
            Bytes out;
            for (int i = 0; i < 4; ++i)
                out.push_back(uint8_t(pattern >> (8 * i)));
            while (!out.empty() && out.back() == 0)
                out.pop_back();
            std::reverse(out.begin(), out.end());
            return out;
        }

        Bytes trickList(const Elf &elf, const StringTable &strings)
        {
            std::vector<Bytes> records;
            for (uint32_t i = 0; i < kAirEntries; ++i)
            {
                const uint32_t entry = kAirTable + i * kTrickEntryBytes;
                const auto [family, name] = airTrickName(elf.word(entry), strings);
                records.push_back(trickRecord(family, elf.word(entry + 4), trickInputs(elf, entry), kButtonAir, name));
            }
            for (uint32_t i = 0; i < kGrindEntries; ++i)
            {
                const uint32_t entry = kGrindTable + i * kTrickEntryBytes;
                const uint32_t unlock = elf.word(entry + 4);
                records.push_back(trickRecord(unlock ? FamilySpecial : FamilyGrind, unlock, trickInputs(elf, entry),
                                              kButtonGrind, strings.text(keyed(kGrindKeys, elf.word(entry)))));
            }
            for (uint32_t i = 0; i < kStallEntries; ++i)
            {
                const uint32_t entry = kStallTable + i * kTrickEntryBytes;
                records.push_back(trickRecord(FamilyOther, 0, trickInputs(elf, entry), kButtonGrind,
                                              strings.text(keyed(kStallKeys, elf.word(entry)))));
            }
            for (uint32_t i = 0; i < kManualEntries; ++i)
            {
                const uint32_t entry = kManualTable + i * kTrickEntryBytes;
                records.push_back(trickRecord(FamilyOther, 0, trickInputs(elf, entry), 0,
                                              strings.text(keyed(kManualKeys, elf.word(entry)))));
            }
            Bytes body;
            putU32(body, int64_t(records.size()));
            for (const Bytes &r : records)
                putBytes(body, r);
            return chunk("TRIK", body);
        }

        // The panel's fonts and icons, by their names in the UI library.
        const std::vector<std::string> kUiFonts = {"fnt_ovr_list", "fnt_ovr_obj", "fnt_ovr_score", "fnt_ovr_trick",
                                                   "fnt_14pt",     "fnt_e_12pt",  "fnt_ps2buttons"};
        const std::vector<std::string> kUiIcons = {
            "AttJump",       "AttSpeed",        "AttSpin",       "AttGrind",       "AttManual",     "AttFakie",
            "AttWallride",   "attrib_circ_full", "attrib_circ_empty", "level_lock", "arrow_up",      "arrow_dn",
            "arrow_lf",      "arrow_rt",        "head_taig",     "head_franky",    "head_jaren",    "head_matt",
            "head_shane",    "head_sven",       "head_chris",    "window_top_lf",  "Window_Top_mid", "window_top_rt",
            "window_side_lf", "window_side_mid", "window_btm_lf", "window_btm_mid", "scratch_score", "card",
            "KeyAirfield",   "KeyBoardwalk",    "KeyCannery",    "KeyCivic",       "KeyIndustrial", "KeyMovie",
            "KeyMuseum",     "KeyZoo"};

        bool among(const std::vector<std::string> &names, const std::string &name)
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        void buildUi(const SourceFiles &disc, const std::filesystem::path &out, std::vector<std::string> &log)
        {
            Bytes data;
            putText(data, "PMAP");
            putU32(data, 3); // panel_file.h kFileVersion
            const Elf elf(needFile(disc, "SLUS_203.27"));
            putBytes(data, specialTricks(elf));
            const Bytes strings = needFile(disc, "ASSETS/STRING.EUS");
            putBytes(data, trickList(elf, StringTable(strings)));
            const Bytes uilib = needFile(disc, "ASSETS/UILIB.ZAL");
            std::vector<std::string> found;
            forEachUiAsset(uilib, [&](const std::string &name, const std::string &, const Chunk &asset) {
                for (const Chunk &k : asset.kids)
                {
                    if (k.tag == "UFNT" && among(kUiFonts, name) && !among(found, name))
                    {
                        found.push_back(name);
                        putBytes(data, fontChunk(name, readUiFont(k)));
                    }
                    else if (k.tag == "UMTL" && among(kUiIcons, name) && !among(found, name))
                    {
                        found.push_back(name);
                        const Chunk *texture = k.first("TXRC");
                        if (!texture)
                            throw PanelError("a UI icon without its texture");
                        const RgbaPicture p = bmpRgba(pictureOf(*texture));
                        Bytes body;
                        putPadded(body, name, 31, 32);
                        putU32(body, p.width);
                        putU32(body, p.height);
                        putBytes(body, p.rgba);
                        putBytes(data, chunk("ICON", body));
                    }
                }
            });
            writeMade(out / "UI.pmap", data);
            std::string missing;
            for (const std::vector<std::string> *list : {&kUiFonts, &kUiIcons})
                for (const std::string &n : *list)
                    if (!among(found, n))
                        missing += " " + n;
            log.push_back(formatted("UI: %.1f MB, %d assets%s", double(data.size()) / 1e6, int(found.size()),
                                 missing.empty() ? "" : (", missing" + missing).c_str()));
        }

        // A folder's level as packaged: <STEM>.<EXT>.
        LevelReader folderLevel(const SourceFiles &files, const std::string &stem)
        {
            return [&files, stem](const std::string &ext) -> std::optional<Bytes> {
                Bytes data;
                if (!files.readAny(stem + "." + ext, data))
                    return std::nullopt;
                return data;
            };
        }
    }

    Bytes aggressiveInlineMapFile(const SourceFiles &files, const std::string &folder, const std::string &title,
                                  std::vector<std::string> &log)
    {
        const LevelReader read = [&files, folder](const std::string &ext) -> std::optional<Bytes> {
            Bytes data;
            if (!files.readAny("ASSETS/LEVELS/" + folder + "/" + folder + "." + ext, data))
                return std::nullopt;
            return data;
        };
        // The free skate script (<folder>T.BIN, which the slot runs) as a
        // second goal set, where it has goals of its own (an Estate's lessons).
        std::optional<Bytes> freeSkate;
        if (Bytes data; files.readAny("ASSETS/LEVELS/" + folder + "/" + folder + "T.BIN", data))
            freeSkate = std::move(data);
        if (freeSkate)
            try
            {
                return levelBytes(read, folder, title, freeSkate, log);
            }
            catch (const PanelError &e)
            {
                log.push_back(folder + ": the free skate script's goals not read (" + e.what() + ")");
            }
        return levelBytes(read, folder, title, std::nullopt, log);
    }

    void makeAggressiveInlineMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                                  std::vector<std::string> &log)
    {
        making("UI.pmap", [&] { buildUi(disc, out, log); });
        std::vector<std::pair<std::string, std::string>> levels;
        if (extras.levels.empty())
            levels = kLevels;
        else
            for (const std::string &level : extras.levels)
            {
                const auto it = std::find_if(kLevels.begin(), kLevels.end(), [&](const auto &l) { return l.first == level; });
                if (it == kLevels.end())
                    throw PanelError("Aggressive Inline has no level " + level);
                levels.push_back(*it);
            }
        for (const auto &[level, title] : levels)
        {
            const LevelReader read = [&disc, folder = level](const std::string &ext) -> std::optional<Bytes> {
                Bytes data;
                if (!disc.readAny("ASSETS/LEVELS/" + folder + "/" + folder + "." + ext, data))
                    return std::nullopt;
                return data;
            };
            // the Xbox release's script of the level, which the career scripts setting serves
            std::optional<Bytes> xbox;
            if (extras.xboxScripts)
            {
                Bytes data;
                if (extras.xboxScripts->readAny("levels/" + level + "/" + level + ".bin", data))
                    xbox = std::move(data);
            }
            making("the level " + level, [&] { buildLevel(read, level, title, out, xbox, log); });
        }
        if (extras.zoo)
            making("the Zoo", [&] { buildLevel(folderLevel(*extras.zoo, "ZOO"), "ZOO", "Zoo", out, std::nullopt, log); });
        if (extras.estate)
            making("the Estate",
                   [&] { buildLevel(folderLevel(*extras.estate, "TUT"), "ESTATE", "Estate", out, std::nullopt, log); });
    }
}
