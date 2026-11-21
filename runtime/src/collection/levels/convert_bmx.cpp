// BMX XXX's converter (collection/levels/convert.h): the model in BMX XXX's
// records (ZGE4) in The Bronx's level row, CHL_MULY. Ported from the
// tooling's bmx_level_convert.py, the reference.
#include "converter.h"

#include "collection/levels/text.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace zx::app::levels
{
    namespace
    {
        constexpr int kDefaultLaunch = 50;
        constexpr size_t kMaterialSound = 0x1C; // MATI: the tag record's first byte
        constexpr const char *kLibraries[] = {"ANIMS.ZAL", "CUTCOMP.ZAL", "CUTSCENE.ZAL", "MODELS.ZAL"};

        // BMX XXX's Globals record (game/bmx/game/level_globals.cpp: 0 the
        // ambient colour, 8 the fog colour, 12 the fog depth, 16 the far clip;
        // the fog's alpha in each colour's fourth byte).
        Bytes bmxGlobalsTags(const Globals &g)
        {
            ByteWriter w;
            for (const auto &colour : {g.light, g.clear, g.fog})
            {
                for (int c : colour)
                    w.u8(static_cast<uint8_t>(c));
                w.u8(static_cast<uint8_t>(g.fogAlpha));
            }
            w.f32(g.fogEnd - g.fogStart).f32(g.fogEnd);
            return w.take();
        }

        // BMX XXX's sky: AI's form, no fog (the stock competition levels'),
        // the far clip 360 m.
        const SkyForm kBmxSky = {kPartFlagsSky, kInstanceDecoration | kInstanceSky, 0, 240.0, 360.0, 360.0, &bmxGlobalsTags};

        // BMX XXX's props: the class data the sound name, a 64-byte text
        // (dynamicsObjectTouched plays "DynamicsObject.<it>" and the knock's
        // kind); another game's prop gets "default", dynamicsdef_beer's (The
        // Underground's). The instance's tags one zero word, as every stock
        // prop's.
        const DynamicsForm kBmxDynamics = {Engine::Zge4, kInstanceCollides, ByteWriter().text("default", 64).take(), Bytes(4, 0)};

        class BmxLevelConverter : public LevelConverter
        {
        public:
            BmxLevelConverter(const Model &model, const std::string &slot) : LevelConverter(model, slot, Engine::Zge4)
            {
                m_sky = &kBmxSky;
                m_dynamics = &kBmxDynamics;
                m_levelBytes = 8733620; // CHL_MNCO's, BMX XXX's largest stock level
                // BMX XXX's respawn takes the nearest spawn of any name
                // (findSpawnFace): its own are unnamed (name hash 0).
                m_respawnName = "";
                m_makesCamera = true;
            }

        protected:
            int surfaceOf(std::optional<int> material) override
            {
                const int i = LevelConverter::surfaceOf(material);
                Surface &s = m_surfaces[static_cast<size_t>(i)];
                if (s.bikeRead)
                    return i;
                s.bikeRead = true;
                TagMap tags;
                std::string base;
                if (material && *material >= 0)
                    std::tie(base, tags) = splitTags(m_model.materials[static_cast<size_t>(*material)].name);
                s.kind = 0;
                s.launch = kDefaultLaunch;
                for (const auto &[word, kind] : {std::pair<const char *, int>{"qp", 1}, {"spine", 2}, {"kicker", 3}, {"wall", 8}})
                {
                    auto it = tags.find(word);
                    if (it == tags.end())
                        continue;
                    s.kind = kind;
                    if (kind == 3 && it->second.hasText)
                    {
                        double value = 0.0;
                        if (!parseFloat(it->second.text, value))
                            throw LevelError("material " + s.name + ": #kicker=" + it->second.text + " is not a number");
                        s.launch = static_cast<int>(std::max(0.0, std::min(255.0, std::trunc(value))));
                    }
                }
                if (hasTag(tags, "slope"))
                    report.did("material " + s.name + ": #slope has no BMX XXX surface kind; its faces are ground");
                s.sound = hasTag(tags, "metal") ? 1 : hasTag(tags, "wood") ? 2 : 0;
                if (!hasTag(tags, "metal") && !hasTag(tags, "wood"))
                {
                    const std::string lower = lowerKey(base);
                    s.sound = lower == "metal" ? 1 : lower == "wood" ? 2 : 0;
                }
                return i;
            }

            void addEdges(const Node &node, const Mesh &mesh, const TagMap &tags) override
            {
                const TagValue &grind = tags.at("grind");
                const std::string word = grind.hasText ? grind.text : "ledge";
                int link;
                if (word == "ledge" || word == "rail" || word == "vpole" || word == "hpole")
                    link = 0;
                else if (word == "lip")
                    link = 1;
                else if (word == "spine")
                    link = 2;
                else
                    throw LevelError("node " + node.name + ": #grind=" + word + " is not one of ledge, rail, lip, spine, vpole, hpole");
                if (word == "vpole" || word == "hpole")
                    report.did("node " + node.name + ": BMX XXX has no poles; #grind=" + word + " is a plain grind edge");
                for (const Primitive &prim : mesh.primitives)
                {
                    std::vector<Vec3> world;
                    for (const Vec3 &p : prim.positions)
                    {
                        const Vec3 t = quatRotate(node.rotation, {p[0] * node.scale[0], p[1] * node.scale[1], p[2] * node.scale[2]});
                        world.push_back({t[0] + node.translation[0], -t[1] + -node.translation[1], -t[2] + -node.translation[2]});
                    }
                    for (const auto &[i, k] : prim.lines)
                        m_edgeMarks.push_back(
                            {world.at(static_cast<size_t>(i)), world.at(static_cast<size_t>(k)), link, BikeEdgeTag::kGrindable, 0, tagWordOf(tags)});
                }
            }

            GeoObject geoObject(const Piece &piece, uint32_t) override
            {
                // BMX XXX's ids are name hashes
                const uint32_t objectId = uniqueId(piece.name);
                const uint32_t partId = uniqueId(piece.name + "_part");
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
                    faceNormals.push_back(ByteWriter()
                                              .f32s(faceNormal(piece.positions[static_cast<size_t>(f[1])], piece.positions[static_cast<size_t>(f[2])],
                                                               piece.positions[static_cast<size_t>(f[3])]))
                                              .take());
                part.stream(StreamKind::Normals) = Stream::ofIndexed(IndexedStream::make(normals));
                part.stream(StreamKind::Colours) = Stream::ofIndexed(IndexedStream::make(colours));
                part.stream(StreamKind::Uvs) = Stream::ofIndexed(IndexedStream::make(uvs));
                part.stream(StreamKind::FaceNormals) = Stream::ofIndexed(IndexedStream::make(faceNormals));
                part.flags = piece.sky ? m_sky->partFlags : kPartFlags;
                part.materialCount = static_cast<uint8_t>(surfaces.size());
                // (surface kind, launch, tag word) per face: a record for each the faces
                // have (the word the objectives find faces by)
                using FaceKey = std::tuple<int, int, uint32_t>;
                std::vector<FaceKey> kinds;
                for (const auto &f : piece.faces)
                {
                    const Surface &s = m_surfaces[static_cast<size_t>(f[0])];
                    kinds.emplace_back(s.kind, s.launch, s.word);
                }
                const auto tagged = [](const FaceKey &k) { return std::get<0>(k) != 0 || std::get<2>(k) != 0; };
                std::set<FaceKey> usedSet;
                for (const auto &k : kinds)
                    if (tagged(k))
                        usedSet.insert(k);
                if (!usedSet.empty() && piece.collides)
                {
                    const std::vector<FaceKey> used(usedSet.begin(), usedSet.end());
                    TagBuffer buffer;
                    for (const auto &[kind, launch, word] : used)
                    {
                        BikeFaceTag t;
                        t.tag = word;
                        t.surfaceKind = static_cast<uint8_t>(kind);
                        t.launch = static_cast<uint8_t>(launch);
                        buffer.records.push_back(t.write());
                    }
                    for (const auto &k : kinds)
                        buffer.indices.push_back(tagged(k) ? static_cast<int>(std::find(used.begin(), used.end(), k) - used.begin()) : -1);
                    part.faceTags = std::move(buffer);
                }
                std::vector<Chunk> materials;
                for (int s : surfaces)
                {
                    const Surface &surface = m_surfaces[static_cast<size_t>(s)];
                    Chunk m = aiMaterial(surface.name, surface.texture, surface.twoSided, surface.layer);
                    m.data[kMaterialSound] = static_cast<uint8_t>(surface.sound);
                    materials.push_back(std::move(m));
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

            void tagEdges(Geometry &geo, const std::vector<Instance> &instances) override
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
                    {
                        const Vec3 t = quatRotate(inst.rotation, p);
                        world.push_back({t[0] + inst.position[0], t[1] + inst.position[1], t[2] + inst.position[2]});
                    }
                    TagBuffer buffer;
                    buffer.indices.assign(3 * part.faces.size(), -1);
                    std::map<std::pair<int, uint32_t>, int> indexOf; // the mark's (link kind, tag word) -> record
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
                            const std::pair<int, uint32_t> key{mark->kind, mark->word};
                            if (!indexOf.count(key))
                            {
                                indexOf[key] = static_cast<int>(buffer.records.size());
                                BikeEdgeTag t;
                                t.tag = mark->word;
                                t.linkKind = static_cast<uint8_t>(mark->kind);
                                t.flags = static_cast<uint8_t>(mark->flags);
                                buffer.records.push_back(t.write());
                            }
                            buffer.indices[3 * f + k] = indexOf[key];
                        }
                    if (!buffer.records.empty())
                    {
                        part.edgeTags = std::move(buffer);
                        tagged.insert(inst.geometryId);
                    }
                }
            }

            Bytes sceneFile(const Vec3 &low, const Vec3 &high) override { return SceneFile::make(m_slot, low, high).write(); }

        private:
            // A name's hash, the next one up if another object or part took
            // it: an id names one (two objects of one name, a #deco mesh and
            // its collidable one, get two ids).
            uint32_t uniqueId(const std::string &name)
            {
                uint32_t h = nameHash(name);
                while (m_ids.count(h) || h <= 0xFFFF)
                    h = h + 1;
                m_ids[h] = name;
                return h;
            }

            std::map<uint32_t, std::string> m_ids;
        };
    }

    Converted convertForBmx(const Model &model, const std::string &slot)
    {
        BmxLevelConverter conv(model, slot);
        std::map<std::string, Bytes> files = conv.build();
        conv.checkLevelBytes(files);
        if (!conv.report.errors.empty())
            throw LevelError(conv.report.errors.front());
        Converted out;
        const std::string folder = "ASSETS/LEVELS/" + slot + "/";
        for (auto &[ext, data] : files)
            out.files[folder + slot + "." + ext] = std::move(data);
        for (const char *library : kLibraries)
            out.files[folder + library] = emptyAssetLibrary();
        out.files[folder + "OBJCTV.TXT"] = emptyObjectivesFile();
        out.report = conv.report.text();
        return out;
    }
}
