// Dave Mirra BMX 2's converter (collection/levels/convert.h): the model in
// DM2's records (ZGE2) in a FREERIDE level row's slot (WOODWARD, Woodward
// Camp's), the level's files replaced. Ported from the tooling's
// dm2_level_convert.py convert --level WOODWARD (the reference).
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
        constexpr int kQuarterPipeLaunch = 70; // Editobj_qpipe_qpipe4
        constexpr int kKickerLaunch = 60;      // Editobj_kicker_2
        constexpr int kGroundLaunch = 50;      // every untagged stock face's byte 9
        constexpr Rgba kKey = {255, 0, 255, 255}; // the colour key: DM2's textures have no alpha
        // DM2's material: flags 0, the three colours, the two unread floats, the
        // tag record (its first byte the surface sound), one layer of the stock
        // levels' opaque state naming the texture.
        constexpr uint8_t kMaterialColours[12] = {0xe5, 0xe5, 0xe5, 0xff, 0xff, 0xff, 0xff, 0xff, 0x19, 0x19, 0x19, 0xff};
        constexpr std::array<uint32_t, 4> kMaterialLayerState = {0x0000001C, 0x40000000, 0x09F782F0, 0x1C000101};
        constexpr size_t kMaterialBytes = 0x9C;

        // DM2's Globals record (game/dm2/level/level_globals.cpp: 0 the scene
        // light, 4 the clear colour, 8 the fog colour, 12 the fog start, 16 the
        // far clip).
        Bytes dm2GlobalsTags(const Globals &g)
        {
            ByteWriter w;
            for (int c : g.light)
                w.u8(static_cast<uint8_t>(c));
            w.u8(0);
            for (int c : g.clear)
                w.u8(static_cast<uint8_t>(c));
            w.u8(0);
            for (int c : g.fog)
                w.u8(static_cast<uint8_t>(c));
            w.u8(static_cast<uint8_t>(g.fogAlpha));
            w.f32(g.fogStart).f32(g.farClip);
            return w.take();
        }

        // DM2's sky: part flags 0 (a sky node's rigid part is the sky), the
        // stock panorama's instance 0x33 without the collides bit; no fog
        // planes; the park's far clip 500 m.
        const SkyForm kDm2Sky = {0, 0x31, 0, 240.0, 360.0, 500.0, &dm2GlobalsTags};

        // DM2's props: the class data the type word alone; the instance's tag
        // word 1 its object sound (startObjectSound: 1 to 16, 0 none;
        // Woodward's trash can 11, cones 12, ball 8). Another game's prop has
        // none: DM2's sounds are numbered samples of the level's bank, and no
        // other game's prop says which.
        const DynamicsForm kDm2Dynamics = {Engine::Zge2, 0x23, {}, Bytes(8, 0)};

        Chunk dm2Material(const std::string &texture, bool twoSided, const std::optional<Bytes> &layer)
        {
            ByteWriter w;
            w.u16(twoSided ? 0x2 : 0).bytes(kMaterialColours, sizeof kMaterialColours).zeros(2).f32(0.25).f32(1.0).u8(0).zeros(7).zeros(8);
            w.u32(1);
            for (uint32_t word : layerState(kMaterialLayerState, layer))
                w.u32(word);
            w.u32(nameHash(texture)).zeros(8);
            w.zeros(kMaterialBytes - w.size());
            return Chunk::leaf(tag("MATI"), w.take());
        }

        class Dm2LevelConverter : public LevelConverter
        {
        public:
            Dm2LevelConverter(const Model &model, const std::string &slot) : LevelConverter(model, slot, Engine::Zge2)
            {
                m_texelBudgetKb = 2560;
                m_textureSide = 128;
                m_partsInView = 682; // Devil's Peak, DM2's most instanced parts
                m_levelBytes = 10068316; // Woodward Camp's, DM2's largest stock level
                m_sky = &kDm2Sky;
                m_dynamics = &kDm2Dynamics;
                // DM2's respawns are its "Player" spawns (respawnAtStart: the
                // ground under the nearest); its multiplayer takes turns, so it
                // has no second start. Its level-point camera (with the level
                // camera points option) stands 5 m above a camera point.
                m_respawnName = "Player";
                m_hasPlayerTwo = false;
                m_makesCamera = true;
                m_cameraLift = -5.0;
                m_spawnTags = Bytes(4, 0);
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
                if (material && *material >= 0)
                    tags = splitTags(m_model.materials[static_cast<size_t>(*material)].name).second;
                s.bikeKind = 0;
                for (const auto &[word, kind] : {std::pair<const char *, int>{"qp", 1}, {"spine", 2}, {"kicker", 3}})
                    if (hasTag(tags, word))
                    {
                        s.bikeKind = kind;
                        break;
                    }
                const int fallback = s.bikeKind == 1 ? kQuarterPipeLaunch : s.bikeKind == 3 ? kKickerLaunch : kGroundLaunch;
                s.launch = fallback;
                if (auto it = tags.find("launch"); it != tags.end())
                {
                    long long value = 0;
                    if (!it->second.hasText)
                        s.launch = 1; // int(True)
                    else if (parseInt(it->second.text, value))
                        s.launch = static_cast<int>(value);
                }
                for (const char *word : {"wall", "slope"})
                    if (hasTag(tags, word))
                        report.did("material " + s.name + ": #" + word + " has no DM2 form: its faces are ground");
                return i;
            }

            GeoObject geoObject(const Piece &piece, uint32_t objectId) override
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
                ByteWriter normals, colours, uvs, faceNormals;
                for (const Vec3 &n : piece.normals)
                    normals.f32s(n);
                for (const Rgba &c : piece.colours)
                    colours.u8(c[0]).u8(c[1]).u8(c[2]).u8(c[3]);
                for (const Vec2 &uv : piece.uvs)
                    uvs.f32s(uv);
                for (const auto &f : piece.faces)
                    faceNormals.f32s(faceNormal(piece.positions[static_cast<size_t>(f[1])], piece.positions[static_cast<size_t>(f[2])],
                                                piece.positions[static_cast<size_t>(f[3])]));
                part.stream(StreamKind::Normals) = Stream::ofPlain(normals.take());
                part.stream(StreamKind::Colours) = Stream::ofPlain(colours.take());
                part.stream(StreamKind::Uvs) = Stream::ofPlain(uvs.take());
                part.stream(StreamKind::FaceNormals) = Stream::ofPlain(faceNormals.take());
                part.flags = piece.sky ? m_sky->partFlags : 0;
                part.materialCount = static_cast<uint8_t>(surfaces.size());
                part.unwind();
                // (surface kind, launch, tag word) per face: a record for each
                // the faces have (the word the gaps and goals find faces by)
                using FaceKey = std::tuple<int, int, uint32_t>;
                std::vector<FaceKey> kinds;
                for (const auto &f : piece.faces)
                {
                    const Surface &s = m_surfaces[static_cast<size_t>(f[0])];
                    kinds.emplace_back(s.bikeKind, s.launch, s.word);
                }
                const auto tagged = [](const FaceKey &k) { return std::get<0>(k) != 0 || std::get<2>(k) != 0; };
                if (piece.collides && std::any_of(kinds.begin(), kinds.end(), tagged))
                {
                    std::set<FaceKey> recordSet;
                    for (const auto &k : kinds)
                        if (tagged(k))
                            recordSet.insert(k);
                    const std::vector<FaceKey> records(recordSet.begin(), recordSet.end());
                    TagBuffer buffer;
                    for (const auto &[kind, launch, word] : records)
                    {
                        BikeFaceTag t;
                        t.tag = word;
                        t.surfaceKind = static_cast<uint8_t>(kind);
                        t.launch = static_cast<uint8_t>(launch);
                        buffer.records.push_back(t.write());
                    }
                    for (const auto &k : kinds)
                        buffer.indices.push_back(tagged(k) ? static_cast<int>(std::find(records.begin(), records.end(), k) - records.begin()) : -1);
                    part.faceTags = std::move(buffer);
                }
                std::vector<Chunk> materials;
                for (int s : surfaces)
                {
                    const Surface &surface = m_surfaces[static_cast<size_t>(s)];
                    materials.push_back(dm2Material(surface.texture, surface.twoSided, surface.layer));
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
                    std::map<std::pair<int, uint32_t>, int> indexOf; // (link kind, tag word) -> record
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
                            const int link = mark->kind == 1 ? 1 : mark->kind == 2 ? 2 : 0;
                            const std::pair<int, uint32_t> key{link, mark->word};
                            if (!indexOf.count(key))
                            {
                                indexOf[key] = static_cast<int>(buffer.records.size());
                                BikeEdgeTag t;
                                t.tag = mark->word;
                                t.linkKind = static_cast<uint8_t>(link);
                                t.flags = BikeEdgeTag::kGrindable;
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

            SceneData sceneData() override
            {
                SceneData dat;
                dat.points = spawnAndCameraPoints();
                if (m_globals)
                    dat.points.push_back(globalsPoint());
                return dat;
            }

            Bytes textureFile() override
            {
                TextureLibrary lib;
                for (const Texture &t : m_textures)
                {
                    // texels with alpha under 128 become the key
                    std::vector<Rgba> keyed;
                    bool anyKey = false;
                    for (const Rgba &px : t.pixels)
                    {
                        const Rgba p = px[3] < 128 ? kKey : Rgba{px[0], px[1], px[2], 255};
                        anyKey = anyKey || (p[0] == kKey[0] && p[1] == kKey[1] && p[2] == kKey[2]);
                        keyed.push_back(p);
                    }
                    std::vector<Rgba> palette;
                    std::vector<uint8_t> indices;
                    palettize(keyed, false, palette, indices);
                    if (anyKey && std::find(palette.begin(), palette.end(), kKey) == palette.end())
                        throw LevelError("texture " + t.name + ": the key colour was merged in its palette");
                    LibraryTexture lt;
                    lt.id = nameHash(t.name);
                    lt.width = static_cast<uint16_t>(t.width);
                    lt.height = static_cast<uint16_t>(t.height);
                    lt.name = t.name;
                    lt.levels.push_back(dm2Bmp8(t.width, t.height, indices, palette));
                    lib.textures.push_back(std::move(lt));
                }
                return lib.write();
            }

            Bytes sceneFile(const Vec3 &low, const Vec3 &high) override { return SceneFile::make(m_slot, low, high, 50.0).write(); }
        };
    }
    Converted convertForDm2(const Model &model, const std::string &slot)
    {
        Dm2LevelConverter conv(model, slot);
        std::map<std::string, Bytes> files = conv.build();
        conv.checkLevelBytes(files);
        if (!conv.report.errors.empty())
            throw LevelError(conv.report.errors.front());
        Converted out;
        const std::string folder = "ASSETS/LEVELS/" + slot + "/";
        for (auto &[ext, data] : files)
            out.files[folder + slot + "." + ext] = std::move(data);
        out.report = conv.report.text();
        return out;
    }
}
