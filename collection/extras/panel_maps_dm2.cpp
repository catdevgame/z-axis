// Dave Mirra BMX 2's panel files (collection/extras/panel_maps.h), and the
// levels in DM2's format BMX XXX shares: a level's map from the faces its
// collision trees list, in one ground colour shaded by slope and height;
// its ProQuest goals from the executable's goal tables (the Xbox levels'
// from the DM2 Xbox extras' CAREER.BIN), each objective's targets the
// places its names find in the level; UI.pmap from the UI library's fonts
// and the executable's trick list.
#include "sym/collection/extras/panel_maps_dm2.cpp.h"
#include "panel_maps_parts.h"

#include "script_text.h"

#include <bitset>
#include <cctype>
#include <map>
#include <memory>

namespace zx::app::extras::panel_maps
{
    namespace
    {
        using dm2::f32At;
        using dm2::u16At;
        using dm2::u32At;

        constexpr uint32_t kFaceTwoSided = 0x4000;
        constexpr uint32_t kInstanceFlags = 0x12, kInstanceCollides = 0x2; // collides, and is no panorama (0x10)
        // DM2's map has no textures to average: its ridden ground in one colour.
        constexpr Rgb kGround = {150, 146, 138};

        // The last chunk's bytes of each tag among a part's (a dict of them).
        std::map<std::string, const Bytes *> streamsOf(const dm2::Chunk &part)
        {
            std::map<std::string, const Bytes *> out;
            for (const dm2::Chunk &k : part.kids)
                out[k.tag] = &k.data;
            return out;
        }

        const Bytes &stream(const std::map<std::string, const Bytes *> &streams, const char *tag)
        {
            const auto it = streams.find(tag);
            if (it == streams.end())
                throw PanelError(std::string("a level part without its ") + tag);
            return *it->second;
        }

        using Face = std::array<uint16_t, 4>; // flags, three vertices

        void readPart(const std::map<std::string, const Bytes *> &streams, std::vector<Vec3> &verts, std::vector<Face> &faces)
        {
            const Bytes &head = stream(streams, "TGPH");
            const size_t nv = u16At(head, 4), nf = u16At(head, 6);
            const Bytes &positions = stream(streams, "TGVP"), &corners = stream(streams, "TGFP");
            verts.resize(nv);
            for (size_t i = 0; i < nv; ++i)
                verts[i] = {f32At(positions, 12 * i), f32At(positions, 12 * i + 4), f32At(positions, 12 * i + 8)};
            faces.resize(nf);
            for (size_t i = 0; i < nf; ++i)
                for (size_t k = 0; k < 4; ++k)
                    faces[i][k] = u16At(corners, 8 * i + 2 * k);
        }

        const dm2::Chunk &objectHead(const dm2::Chunk &obj)
        {
            const dm2::Chunk *head = obj.find("TGHD");
            if (!head)
                throw PanelError("a level object without its head");
            return *head;
        }

        // The faces a collision tree (a TGCI record: a word, the tree's size,
        // the tree) lists in its leaves. A node is a kind byte (0: a leaf) and,
        // for a branch, its two children as offsets in words. DM2's leaves keep
        // their count at +2 and the indices inline from +4, and the record's
        // first word is small; BMX XXX's leaves are 8 bytes, the count at +2
        // and at +4 the offset, in halfwords from the tree's start, of the
        // indices after the nodes, and the record's first word is a name hash.
        std::bitset<65536> treeFaces(const Bytes &record)
        {
            const uint32_t first = u32At(record, 0), size = u32At(record, 4);
            const Bytes blob = dm2::sliceOf(record, 8, 8 + size_t(size));
            const bool offsetLeaves = first > 0xFFFF;
            std::bitset<65536> faces;
            std::vector<size_t> stack{0};
            std::vector<bool> seen(blob.size() + 1, false);
            while (!stack.empty())
            {
                const size_t at = stack.back();
                stack.pop_back();
                if (at + 8 > blob.size() || seen[at])
                    continue;
                seen[at] = true;
                if (blob[at] == 0)
                {
                    const size_t count = u16At(blob, at + 2);
                    const size_t start = offsetLeaves ? 2 * size_t(u32At(blob, at + 4)) : at + 4;
                    if (start + 2 * count <= blob.size())
                        for (size_t i = 0; i < count; ++i)
                            faces.set(u16At(blob, start + 2 * i));
                }
                else
                {
                    stack.push_back(4 * size_t(u16At(blob, at + 4)));
                    stack.push_back(4 * size_t(u16At(blob, at + 6)));
                }
            }
            return faces;
        }

        // The level's instances, placed in the world: an instance with a
        // parent (+0x44, the parent's id at +0x48; the decals) is placed in its
        // parent's space (DM2's rule: a parent of 0 is an id too).
        class Placer
        {
        public:
            explicit Placer(const Bytes &ins)
            {
                m_tree = dm2::parse(ins);
                dm2::walk(m_tree, [&](const dm2::Chunk &c) {
                    if (c.tag == "TGIH")
                        instances.push_back(&c.data);
                });
                for (const Bytes *d : instances)
                    m_byId[u32At(*d, 0x48)] = d;
            }

            Vec3 place(const Bytes *d, Vec3 v) const
            {
                for (int i = 0; i < 16; ++i)
                {
                    const dm2::Vec3 turned = dm2::quatRotate(rotation(*d), v);
                    v = {turned[0] + f32At(*d, 0x18), turned[1] + f32At(*d, 0x1C), turned[2] + f32At(*d, 0x20)};
                    const auto it = m_byId.find(u32At(*d, 0x44));
                    if (it == m_byId.end())
                        break;
                    d = it->second;
                }
                return v;
            }

            Vec3 turn(const Bytes *d, Vec3 n) const
            {
                for (int i = 0; i < 16; ++i)
                {
                    n = dm2::quatRotate(rotation(*d), n);
                    const auto it = m_byId.find(u32At(*d, 0x44));
                    if (it == m_byId.end())
                        break;
                    d = it->second;
                }
                return n;
            }

            std::vector<const Bytes *> instances;

        private:
            static dm2::Quat rotation(const Bytes &d) { return {f32At(d, 8), f32At(d, 12), f32At(d, 16), f32At(d, 20)}; }
            dm2::Chunks m_tree;
            std::unordered_map<uint32_t, const Bytes *> m_byId;
        };

        const dm2::Chunk &firstOf(const dm2::Chunks &tree, const char *what)
        {
            if (tree.empty() || !tree[0].container)
                throw PanelError(std::string("a level's ") + what + " that does not read");
            return tree[0];
        }

        // Every instance's collidable faces in world space: the faces its
        // object's collision tree (TGCI) lists, seen from above (a face counts
        // if its normal points up or it is two-sided). A part without face
        // normals (most of BMX XXX's) has them from its winding.
        std::vector<std::array<Vec3, 3>> collisionFaces(const Bytes &geoBytes, const Bytes &insBytes)
        {
            struct Listed
            {
                std::array<uint16_t, 3> corners;
                Vec3 normal;
                bool twoSided;
            };
            struct Object
            {
                std::vector<Vec3> verts;
                std::vector<Listed> faces;
            };
            std::unordered_map<uint32_t, Object> objects;
            const dm2::Chunks geo = dm2::parse(geoBytes);
            for (const dm2::Chunk &obj : firstOf(geo, "geometry").kids)
            {
                if (obj.tag != "TGEO")
                    continue;
                const dm2::Chunk &head = objectHead(obj);
                const dm2::Chunk *part = nullptr, *tree = nullptr;
                for (const dm2::Chunk &k : obj.kids)
                {
                    if (k.tag == "TGPT")
                        part = &k; // the last part
                    else if (k.tag == "TGCI" && !tree)
                        tree = &k;
                }
                if (!part || !tree)
                    continue;
                const auto streams = streamsOf(*part);
                Object o;
                std::vector<Face> faces;
                readPart(streams, o.verts, faces);
                std::vector<Vec3> normals;
                if (streams.count("TGFN"))
                {
                    const Bytes &n = *streams.at("TGFN");
                    for (size_t i = 0; i < faces.size(); ++i)
                        normals.push_back({f32At(n, 12 * i), f32At(n, 12 * i + 4), f32At(n, 12 * i + 8)});
                }
                else
                    for (const Face &f : faces)
                    {
                        if (std::max({f[1], f[2], f[3]}) >= o.verts.size())
                        {
                            normals.push_back({0.0, 0.0, 0.0});
                            continue;
                        }
                        const Vec3 &a = o.verts[f[1]], &b = o.verts[f[2]], &c = o.verts[f[3]];
                        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                        normals.push_back({uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx});
                    }
                const std::bitset<65536> listed = treeFaces(tree->data);
                for (size_t i = 0; i < faces.size(); ++i)
                    if (listed.test(i) && std::max({faces[i][1], faces[i][2], faces[i][3]}) < o.verts.size())
                        o.faces.push_back({{faces[i][1], faces[i][2], faces[i][3]}, normals[i], bool(faces[i][0] & kFaceTwoSided)});
                objects[u32At(head.data, 0x18)] = std::move(o);
            }
            const Placer placer(insBytes);
            std::vector<std::array<Vec3, 3>> out;
            std::vector<Vec3> world;
            for (const Bytes *d : placer.instances)
            {
                if ((u32At(*d, 0) & kInstanceFlags) != kInstanceCollides)
                    continue;
                const auto found = objects.find(u32At(*d, 0x40));
                if (found == objects.end())
                    continue;
                world.clear();
                for (const Vec3 &v : found->second.verts)
                    world.push_back(placer.place(d, v));
                for (const Listed &f : found->second.faces)
                    if (f.twoSided || placer.turn(d, f.normal)[1] < 0.0)
                        out.push_back({world[f.corners[0]], world[f.corners[1]], world[f.corners[2]]});
            }
            return out;
        }

        // Each face facing up, in the ground colour.
        std::vector<MapFace> groundFaces(const Bytes &geo, const Bytes &ins)
        {
            std::vector<MapFace> out;
            for (const auto &[a, b, c] : collisionFaces(geo, ins))
            {
                const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const double length = std::pow(nx * nx + ny * ny + nz * nz, 0.5);
                if (length == 0)
                    continue;
                if (ny > 0) // +y is down: the face's upward normal
                {
                    nx = -nx;
                    ny = -ny;
                    nz = -nz;
                }
                MapFace t;
                t.top = pyMin(pyMin(a[1], b[1]), c[1]);
                t.a = a;
                t.b = b;
                t.c = c;
                t.normal = {nx / length, ny / length, nz / length};
                t.colour[0] = kGround;
                out.push_back(t);
            }
            return out;
        }
    }

    GoalPlaces dm2GoalPlaces(const Bytes &geoBytes, const Bytes &insBytes)
    {
        // A face's or an edge's name is the tag word (+0) of its record in its
        // part's TGFT (one index a face) or TGFE (one a face edge: face * 3 +
        // k, from the face's vertex k to k + 1); an object's its instance's
        // (TGIH +0x4C, BMX XXX's at +0x50), at its origin. An edge gives both
        // its ends.
        struct Tagged
        {
            uint32_t word;
            PlaceKind kind;
            std::vector<Vec3> points;
        };
        std::unordered_map<uint32_t, std::vector<Tagged>> meshes;
        const dm2::Chunks geo = dm2::parse(geoBytes);
        for (const dm2::Chunk &obj : firstOf(geo, "geometry").kids)
        {
            if (obj.tag != "TGEO")
                continue;
            const dm2::Chunk &head = objectHead(obj);
            std::vector<Tagged> tagged;
            for (const dm2::Chunk &part : obj.kids)
            {
                if (part.tag != "TGPT")
                    continue;
                const auto streams = streamsOf(part);
                if (!streams.count("TGFT") && !streams.count("TGFE"))
                    continue;
                std::vector<Vec3> verts;
                std::vector<Face> faces;
                readPart(streams, verts, faces);
                const size_t nv = verts.size(), nf = faces.size();
                for (const auto &[tag, kind] : {std::make_pair("TGFT", PlaceFace), std::make_pair("TGFE", PlaceEdge)})
                {
                    if (!streams.count(tag))
                        continue;
                    // the tag buffer: a descriptor (bits 0-3 the index kind, 1 =
                    // u16; 4-13 the record size; 14-31 the count), the index
                    // count, the indices padded to 16 bytes, the records
                    const Bytes &tags = *streams.at(tag);
                    const uint32_t desc = u32At(tags, 0), count = u32At(tags, 4);
                    if ((desc & 0xF) != 1)
                        throw PanelError(formatted("a tag buffer of index kind %u", desc & 0xF));
                    const size_t size = (desc >> 4) & 0x3FF, records = desc >> 14;
                    const size_t indexBytes = (size_t(count) * 2 + 15) & ~size_t(15);
                    const size_t body = 8 + indexBytes;
                    if ((tags.size() > body ? tags.size() - body : 0) != size * records)
                        throw PanelError("a tag buffer of the wrong size");
                    for (size_t e = 0; e < count; ++e)
                    {
                        const uint16_t r = u16At(tags, 8 + 2 * e);
                        if (r == 0xFFFF || r >= records || size < 4)
                            continue;
                        const uint32_t word = u32At(tags, body + size_t(r) * size);
                        if (!word)
                            continue;
                        if (kind == PlaceFace && e < nf && std::max({faces[e][1], faces[e][2], faces[e][3]}) < nv)
                            tagged.push_back({word, kind, {verts[faces[e][1]], verts[faces[e][2]], verts[faces[e][3]]}});
                        else if (kind == PlaceEdge && e / 3 < nf)
                        {
                            const Face &face = faces[e / 3];
                            const size_t i0 = face[1 + e % 3], i1 = face[1 + (e + 1) % 3];
                            if (std::max(i0, i1) < nv)
                                tagged.push_back({word, kind, {verts[i0], verts[i1]}});
                        }
                    }
                }
            }
            meshes[u32At(head.data, 0x18)] = std::move(tagged);
        }
        const Placer placer(insBytes);
        GoalPlaces out;
        for (const Bytes *d : placer.instances)
        {
            std::string name = dm2::cText(*d, 0x4C);
            if (name.empty())
                name = dm2::cText(*d, 0x50);
            if (!name.empty())
                out[dm2::nameHash(name)][PlaceObject].push_back(placer.place(d, {0.0, 0.0, 0.0}));
            const auto found = meshes.find(u32At(*d, 0x40));
            if (found == meshes.end())
                continue;
            for (const Tagged &t : found->second)
            {
                std::vector<Vec3> world;
                for (const Vec3 &p : t.points)
                    world.push_back(placer.place(d, p));
                if (t.kind == PlaceFace)
                {
                    Vec3 centre{};
                    for (size_t i = 0; i < 3; ++i)
                    {
                        std::vector<double> v;
                        for (const Vec3 &p : world)
                            v.push_back(p[i]);
                        centre[i] = pythonMean(v);
                    }
                    world = {centre};
                }
                std::vector<Vec3> &into = out[t.word][t.kind];
                into.insert(into.end(), world.begin(), world.end());
            }
        }
        return out;
    }

    Bytes dm2FormatLevelFile(const Bytes &geo, const Bytes &ins, const std::string &level, const std::string &title,
                             const Bytes &goals, int goalCount, bool capitals, std::string &logLine)
    {
        // the level proper: the densest stretch of faces (DM2 keeps no start points in its DAT)
        const std::vector<MapFace> faces = groundFaces(geo, ins);
        const Box box = levelBox(faces, {});
        const Frame frame(box, pixelsPerUnit(box));
        MapPicture picture = rasterise(faces, frame);
        fillGaps(picture, frame.ppu);
        outline(picture);
        // In capitals, as DM2's menus show level names (its fonts have no small letters).
        Bytes file = levelFile(capitals ? pythonUpper(title) : title, frame, picture, goals);
        logLine = formatted("%s (%s): %d faces, box x %.0f .. %.0f z %.0f .. %.0f, %dx%d%s, %d goals, file %.1f MB",
                         level.c_str(), title.c_str(), int(faces.size()), box.x0, box.x1, box.z0, box.z1,
                         int(frame.width), int(frame.height), frame.turned ? " turned" : "", goalCount,
                         double(file.size()) / 1e6);
        return file;
    }

    namespace
    {
        // ---- ProQuest goals ------------------------------------------------------------

        // The goal tables (games/dm2/game/goals.h GoalDefinition): eight game
        // levels of five challenges of four goals, 0x160 bytes each; +0 the
        // objective count, +4 the respect it earns, +0x20 four objectives of
        // 0x50: a kind, four words. The level menu's entries' game levels
        // (g_levelMenuGameLevels, a byte each).
        constexpr uint32_t kGoalTables = sym::dm2::g_goalDefinitions, kGoalBytes = 0x160, kGameLevelBytes = 0x1B80;
        constexpr uint32_t kObjectiveAt = 0x20, kObjectiveBytes = 0x50;
        constexpr uint32_t kMenuGameLevels = sym::dm2::g_levelMenuGameLevels;
        // The level's NPCs (games/dm2/game/npcs.h NpcDefinition, g_npcDefinitions:
        // five a level of 0x14 bytes, four NPCs and the stand-in; the list
        // ends at an empty name): +4 its name, +0x10 the challenge whose goals
        // it gives, from which goal to which. CAREER.BIN keeps an Xbox
        // level's in its pros block.
        constexpr uint32_t kNpcTables = sym::dm2::g_npcDefinitions, kNpcLevelBytes = 0x64, kNpcBytes = 0x14, kNpcs = 4;
        constexpr uint32_t kNpcName = 0x04, kNpcRange = 0x10;
        // The challenges' names in the goals' text keys ("KSTRSP_GOAL_LEV%02d_%s_%d";
        // the rookie challenge's one goal "KSTRSP_GOAL_LEV%02d_ROOKIE") and on
        // the objectives screen (KSTRSP_OBJCTV_<title>).
        const char *const kChallengeKeys[] = {"ROOKIE", "EASY", "HARD", "XTRA", "EXPERT"};
        const char *const kChallengeTitles[] = {"ROOKIE", "AMATEUR", "PRO", "HARDCORE", "EXPERT"};
        // The level select's folders by menu value (Root.CurrentLevel); their
        // names are KSTR_LEVELNAMEMENU_<value>.
        const char *const kLevels[] = {"WOODWARD", "TRAINYAR", "SWAMPYTR", "COMMERCI",
                                       "GALLOON",  "CLOVERLE", "DEVILSPE", "AIRPORT"};
        // The Xbox levels (dm2_xbox.h's folders, the names the panel shows); they run in
        // Woodward Camp's slot (game level 2): their goals' texts are under
        // that level's keys (dave_mirra_xcareer.cpp).
        const std::pair<const char *, const char *> kXboxLevels[] = {{"VENICE", "Venice, Italy"}, {"GREENVIL", "Greenville, NC"}};
        constexpr int kXboxSlot = 2;
        // The panel's script words past the twenty goals' counts: an
        // objective met (dave_mirra_panel.cpp), four a goal.
        constexpr int64_t kObjectiveMetWords = 20;
        // The targets an objective keeps of each name (goals.h kObjectiveTargetsMost); an edge gives two points.
        constexpr size_t kObjectiveTargetsMost = 128;

        // Where an objective's names are (games/dm2/game/goals.cpp
        // findObjectiveTargets), by its kind: the words holding them and what
        // each names. Kinds 0, 4, 5 and 22 are anywhere.
        struct ObjectiveNames
        {
            std::vector<std::pair<uint32_t, PlaceKind>> names; // (word, what it names)
        };
        ObjectiveNames objectiveNames(uint32_t kind)
        {
            switch (kind)
            {
            case 1: return {{{3, PlaceFace}}};
            case 2: return {{{3, PlaceObject}}};
            case 3: return {{{3, PlaceEdge}}};
            case 6: return {{{1, PlaceFace}}};
            case 7: return {{{1, PlaceObject}}};
            case 8: return {{{0, PlaceEdge}}};
            case 9: return {{{0, PlaceObject}}};
            case 10: return {{{0, PlaceFace}}};
            case 11:
            case 12:
            case 13:
            case 23: return {{{0, PlaceObject}}};
            case 14:
            case 15:
            case 16:
            case 17: return {{{0, PlaceFace}, {1, PlaceFace}}};
            case 18: return {{{0, PlaceEdge}, {1, PlaceEdge}}};
            case 19: return {{{0, PlaceObject}, {1, PlaceObject}}};
            case 20: return {{{0, PlaceFace}, {1, PlaceEdge}}};
            case 21: return {{{0, PlaceFace}, {1, PlaceObject}}};
            default: return {};
            }
        }

        // The panel's look of a goal (GoalRecord type) by its objectives'
        // kinds (goals.h ObjectiveKind): a record of a run's points, grinds,
        // transfers and jumps off objects, else other.
        int64_t goalType(const std::vector<uint32_t> &kinds, const std::vector<std::array<uint32_t, 4>> &words)
        {
            if (!kinds.empty() && kinds[0] == 22 && words[0][0] == 1)
                return 1;
            for (uint32_t k : kinds)
                if (k == 8 || k == 9 || k == 18)
                    return 5;
            for (uint32_t k : kinds)
                if ((14 <= k && k <= 21) || k == 11 || k == 12)
                    return 7;
            return 0;
        }

        // A level's goal definitions: words by offset in its 0x1B80 bytes,
        // and the text a name field holds.
        class GoalTable
        {
        public:
            virtual ~GoalTable() = default;
            virtual uint32_t word(uint32_t offset) const = 0;
            virtual std::optional<std::string> name(uint32_t offset) const = 0;
        };

        // In the executable (a level's row of a table at `base`): names are pointers to texts.
        class ElfTable final : public GoalTable
        {
        public:
            ElfTable(const Elf &elf, uint32_t base) : m_elf(elf), m_base(base) {}
            uint32_t word(uint32_t offset) const override { return m_elf.word(m_base + offset); }
            std::optional<std::string> name(uint32_t offset) const override
            {
                const uint32_t pointer = word(offset);
                if (!pointer)
                    return std::nullopt;
                return m_elf.text(pointer);
            }

        private:
            const Elf &m_elf;
            uint32_t m_base;
        };

        // As CAREER.BIN keeps an Xbox level's: the same bytes, the names as
        // relocations (offset, string, how) to its strings.
        class CareerGoals final : public GoalTable
        {
        public:
            CareerGoals(Bytes body, const std::vector<std::array<uint32_t, 3>> &relocations,
                        const std::vector<std::string> &strings)
                : m_body(std::move(body))
            {
                for (const auto &r : relocations)
                {
                    if (r[1] >= strings.size())
                        throw PanelError("CAREER.BIN names a string it lacks");
                    m_names[r[0]] = strings[r[1]];
                }
            }
            uint32_t word(uint32_t offset) const override { return u32At(m_body, offset); }
            std::optional<std::string> name(uint32_t offset) const override
            {
                const auto it = m_names.find(offset);
                if (it == m_names.end())
                    return std::nullopt;
                return it->second;
            }

        private:
            Bytes m_body;
            std::unordered_map<uint32_t, std::string> m_names;
        };

        using Texts = std::unordered_map<uint32_t, std::string>;

        std::optional<std::string> lookup(const Texts &texts, const std::string &key)
        {
            const auto it = texts.find(dm2::nameHash(key));
            if (it == texts.end())
                return std::nullopt;
            return it->second;
        }

        // (first name's hash, places) of an objective whose definition is at
        // `at`: the targets the game finds for it as its challenge starts (its
        // names' faces, edges or objects), joined into places.
        std::pair<uint32_t, std::vector<std::pair<double, double>>> objectivePlaces(const GoalTable &goals, uint32_t at,
                                                                                    uint32_t kind, const GoalPlaces &places)
        {
            std::vector<Vec3> points;
            uint32_t first = 0;
            for (const auto &[word, found] : objectiveNames(kind).names)
            {
                const std::optional<std::string> name = goals.name(at + 4 + 4 * word);
                if (!name || name->empty())
                    continue;
                const uint32_t key = dm2::nameHash(*name);
                first = first ? first : key;
                // An objective keeps the first 128 of a name's targets, in the level's order.
                const auto it = places.find(key);
                if (it == places.end())
                    continue;
                const std::vector<Vec3> &of = it->second[found];
                const size_t most = kObjectiveTargetsMost * (found == PlaceEdge ? 2 : 1);
                points.insert(points.end(), of.begin(), of.begin() + long(std::min(most, of.size())));
            }
            return {first, joinPlaces(points)};
        }

        struct GoalChunks
        {
            Bytes bytes;
            int shown = 0, placed = 0, givers = 0, targets = 0;
        };

        // A level's NPC as the goals' givers: its name, and the goals of one
        // challenge it gives (npcWaitingFrame offers its range's first goal
        // not done).
        struct NpcGiver
        {
            std::string name;
            uint32_t challenge = 0, firstGoal = 0, lastGoal = 0;
        };

        // The level's NPCs from its row of the definitions (setupNpcs: the
        // four, up to the first with an empty name).
        std::vector<NpcGiver> npcGivers(const GoalTable &npcs)
        {
            std::vector<NpcGiver> out;
            for (uint32_t npc = 0; npc < kNpcs; ++npc)
            {
                const std::optional<std::string> name = npcs.name(npc * kNpcBytes + kNpcName);
                if (!name || name->empty())
                    break;
                const uint32_t range = npcs.word(npc * kNpcBytes + kNpcRange);
                out.push_back({*name, range & 0xFF, (range >> 8) & 0xFF, (range >> 16) & 0xFF});
            }
            return out;
        }

        // `format` with its one "%s" made `value`.
        std::string withValue(const std::string &format, const std::string &value)
        {
            const size_t at = format.find("%s");
            return at == std::string::npos ? format : format.substr(0, at) + value + format.substr(at + 2);
        }

        // " ".join(text.split()) of a text with its line breaks made spaces.
        std::string oneLine(std::string text)
        {
            text = script_text::replaceAll(script_text::replaceAll(std::move(text), "\r\n", " "), "\n", " ");
            std::string out;
            for (const std::string &word : script_text::splitWords(text))
                out += (out.empty() ? "" : " ") + word;
            return out;
        }

        // GOAL, TARG, GIVE and TEXT of a DM2 level: twenty goals, challenge by
        // challenge (as dave_mirra_panel.cpp reads their states), a slot with
        // no goal hidden (tier 3, never done); a goal of several objectives
        // counts them (script word = its slot: the objectives met), the rookie
        // challenge's run's points against the rider's score. Its title is the
        // goal's text, its text that again with its challenge and respect. Its
        // targets are its objectives' places, each done as its objective is
        // met. A goal an NPC gives is owned by its giver record (GIVE), named
        // by the goal's text key, as dave_mirra_panel.cpp names the NPC's
        // rider it tracks while the goal can be started (the record's place
        // is none: the rider rides); what starts it is the NPC screen's own
        // sending on ("You should go talk to %s." and "I think I saw him
        // %s." with the challenge's NPC location, the first of their
        // wordings). `gameLevel`: the number in the texts' keys.
        GoalChunks goalChunks(const GoalTable &goals, const Texts &names, int gameLevel, const GoalPlaces &places,
                              const std::vector<NpcGiver> &givers)
        {
            TextTable text;
            Bytes targets, data, give;
            int64_t targetCount = 0;
            GoalChunks out;
            for (int c = 0; c < 5; ++c)
            {
                const std::string key = kChallengeKeys[c];
                std::string challenge = lookup(names, std::string("KSTRSP_OBJCTV_") + kChallengeTitles[c])
                                            .value_or(lowerAscii(key) + " challenges:");
                while (!challenge.empty() && challenge.back() == ':')
                    challenge.pop_back();
                while (!challenge.empty() && challenge.back() == 's')
                    challenge.pop_back();
                for (int g = 0; g < 4; ++g)
                {
                    const int slot = c * 4 + g;
                    const uint32_t at = uint32_t(slot) * kGoalBytes;
                    const uint32_t count = goals.word(at) & 0xFF;
                    const uint32_t respect = goals.word(at + 4);
                    const std::string goalKey = c == 0 ? formatted("KSTRSP_GOAL_LEV%02d_%s", gameLevel, key.c_str())
                                                       : formatted("KSTRSP_GOAL_LEV%02d_%s_%d", gameLevel, key.c_str(), g);
                    const std::optional<std::string> words = count ? lookup(names, goalKey) : std::nullopt;
                    if (!words || words->empty() || !(1 <= count && count <= 4))
                    {
                        GoalFields hidden;
                        hidden.tier = 3;
                        putGoal(data, hidden);
                        continue;
                    }
                    std::vector<uint32_t> kinds;
                    std::vector<std::array<uint32_t, 4>> objectiveWords;
                    for (uint32_t o = 0; o < count; ++o)
                        kinds.push_back(goals.word(at + kObjectiveAt + o * kObjectiveBytes));
                    for (uint32_t o = 0; o < count; ++o)
                    {
                        std::array<uint32_t, 4> w{};
                        for (uint32_t i = 0; i < 4; ++i)
                            w[i] = goals.word(at + 0x24 + o * kObjectiveBytes + 4 * i);
                        objectiveWords.push_back(w);
                    }
                    const int64_t first = targetCount;
                    for (uint32_t o = 0; o < count; ++o)
                    {
                        const auto [name, spots] = objectivePlaces(goals, at + kObjectiveAt + o * kObjectiveBytes, kinds[o], places);
                        for (const auto &[x, z] : spots)
                        {
                            putTarget(targets, name, x, z, kObjectiveMetWords + slot * 4 + o, 1, kTargetPlace);
                            ++targetCount;
                        }
                    }
                    out.placed += targetCount > first ? 1 : 0;
                    // In capitals, as DM2's menus and their fonts have them.
                    const std::string title = pythonUpper(oneLine(*words));
                    GoalFields f;
                    f.title = text.add(title);
                    f.text = text.add(pythonUpper(title + " (" + challenge + ", " + std::to_string(respect) + " respect)"));
                    f.counterWord = count > 1 ? slot : -1;
                    f.counterTarget = count;
                    f.firstTarget = first;
                    f.targetCount = targetCount - first;
                    f.type = goalType(kinds, objectiveWords);
                    if (kinds[0] == 22 && objectiveWords[0][0] == 1)
                    {
                        f.scoreKind = 1;
                        f.scoreTarget = int64_t(objectiveWords[0][1]) + 1;
                    }
                    for (const NpcGiver &npc : givers)
                    {
                        if (npc.challenge != uint32_t(c) || uint32_t(g) < npc.firstGoal || uint32_t(g) > npc.lastGoal)
                            continue;
                        f.owner = dm2::nameHash(goalKey);
                        putBubble(give, f.owner, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), -1, 0);
                        ++out.givers;
                        const std::optional<std::string> talk = lookup(names, "KSTRSP_NPC_DISMISS_NPCNAME_0");
                        const std::optional<std::string> where = lookup(names, "KSTRSP_NPC_DISMISS_LOCATION_0");
                        const std::optional<std::string> place =
                            c > 0 ? lookup(names, formatted("KSTRSP_NPC_LOCATION_LEV%02d_%d", gameLevel, c - 1)) : std::nullopt;
                        if (talk && where && place)
                            f.activate = text.add(pythonUpper(oneLine(withValue(*talk, npc.name) + " " + withValue(*where, *place))));
                        break;
                    }
                    putGoal(data, f);
                    ++out.shown;
                }
            }
            Bytes goal;
            putU32(goal, 20);
            putBytes(goal, data);
            out.bytes = chunk("GOAL", goal);
            Bytes targ;
            putU32(targ, targetCount);
            putBytes(targ, targets);
            putBytes(out.bytes, chunk("TARG", targ));
            Bytes giveChunk;
            putU32(giveChunk, out.givers);
            putBytes(giveChunk, give);
            putBytes(out.bytes, chunk("GIVE", giveChunk));
            putBytes(out.bytes, chunk("TEXT", text.bytes));
            out.targets = int(targetCount);
            return out;
        }

        // ---- the trick list --------------------------------------------------------------

        // DM2's moves (games/dm2/overlay/screen_trick_list.cpp): 0x48 tricks,
        // then 0x56 modifiers (a modifier's move number is its number + 0x48),
        // each a name and its buttons in ButtonLib's glyphs, their kinds (u16;
        // 8: a move the characters start without, a trick of it a signature
        // move); and the trick list's categories by trick number, in its
        // order, as the panel's six families.
        constexpr uint32_t kTrickNames = sym::dm2::trickNames, kModifierNames = sym::dm2::modifierNames;
        constexpr uint32_t kTrickButtons = sym::dm2::trickButtonTexts, kModifierButtons = sym::dm2::modifierButtonTexts;
        constexpr uint32_t kMoveKinds = sym::dm2::g_trickKindsR;
        constexpr uint32_t kTricks = 0x48, kModifiers = 0x56, kFirstModifier = 0x48;
        constexpr uint32_t kMoveOffAtFirst = 0x8;
        const char *const kFamilies[] = {"AIR", "MODIFIERS", "STALLS", "GRINDS", "OTHERS", "SIGNATURE"};
        struct Category
        {
            int64_t family;
            uint32_t first, last; // tricks
        };
        constexpr Category kCategories[] = {{0, 0x00, 0x32}, {2, 0x3C, 0x42}, {3, 0x33, 0x3B}, {4, 0x43, 0x47}};

        Bytes trickChunks(const Elf &elf, int &rows)
        {
            auto kind = [&](uint32_t move) {
                const uint32_t address = kMoveKinds + 2 * move;
                return (elf.word(address & ~3u) >> (8 * (address & 2))) & 0xFFFF;
            };
            std::vector<Bytes> records, moves;
            auto add = [&](int64_t family, uint32_t move, uint32_t names, uint32_t buttons, uint32_t index, int64_t whileAble) {
                records.push_back(buttonsTrickRecord(family, elf.text(elf.word(buttons + 4 * index)),
                                                     upperAscii(elf.text(elf.word(names + 4 * index)))));
                Bytes m;
                putU8(m, move);
                putU8(m, whileAble);
                putU16(m, 0);
                moves.push_back(m);
            };
            for (uint32_t trick = kCategories[0].first; trick <= kCategories[0].last; ++trick)
                if (!(kind(trick) & kMoveOffAtFirst))
                    add(kCategories[0].family, trick, kTrickNames, kTrickButtons, trick, 0);
            for (uint32_t modifier = 0; modifier < kModifiers; ++modifier)
                if (!(kind(modifier + kFirstModifier) & kMoveOffAtFirst))
                    add(1, modifier + kFirstModifier, kModifierNames, kModifierButtons, modifier, 0);
            for (size_t c = 1; c < 4; ++c)
                for (uint32_t trick = kCategories[c].first; trick <= kCategories[c].last; ++trick)
                    if (!(kind(trick) & kMoveOffAtFirst))
                        add(kCategories[c].family, trick, kTrickNames, kTrickButtons, trick, 0);
            for (uint32_t trick = 0; trick < kTricks; ++trick)
                if (kind(trick) & kMoveOffAtFirst)
                    add(5, trick, kTrickNames, kTrickButtons, trick, 1);
            Bytes trik, move, fams;
            putU32(trik, int64_t(records.size()));
            for (const Bytes &r : records)
                putBytes(trik, r);
            putU32(move, int64_t(moves.size()));
            for (const Bytes &m : moves)
                putBytes(move, m);
            putU32(fams, 6);
            for (const char *f : kFamilies)
                putPadded(fams, f, 16, 16);
            Bytes out = chunk("TRIK", trik);
            putBytes(out, chunk("MOVE", move));
            putBytes(out, chunk("FAMS", fams));
            rows = int(records.size());
            return out;
        }

        // The panel's three fonts (panel.cpp: body, title, numbers) from DM2's
        // own UI library: its menus' small text, its overlay's headings and its
        // scores; and ButtonLib's PlayStation font for the trick table's
        // directions and buttons (a ButtonLib font by its source file's name).
        const std::vector<std::pair<std::string, std::string>> kUiFonts = {
            {"mnu_small", "fnt_14pt"}, {"ovr_sm3", "fnt_e_12pt"}, {"ovr_sm", "fnt_ovr_score"}, {"ps2_btn", "fnt_ps2buttons"}};

        void buildUi(const SourceFiles &disc, const Elf &elf, const std::filesystem::path &out, std::vector<std::string> &log)
        {
            int rows = 0;
            Bytes data;
            putText(data, "PMAP");
            putU32(data, 3); // panel_file.h kFileVersion
            putBytes(data, trickChunks(elf, rows));
            std::vector<std::string> found;
            forEachUiAsset(needFile(disc, "ASSETS/UILIB.ZAL"), [&](std::string name, const std::string &source, const Chunk &asset) {
                if (name == "button")
                    name = sourceStem(source);
                for (const Chunk &k : asset.kids)
                {
                    const auto font = std::find_if(kUiFonts.begin(), kUiFonts.end(), [&](const auto &f) { return f.first == name; });
                    if (k.tag != "UFNT" || font == kUiFonts.end() || std::count(found.begin(), found.end(), name))
                        continue;
                    found.push_back(name);
                    UiFont f = readUiFont(k);
                    if (name == "ps2_btn")
                        for (size_t g = 0; g < f.glyphs; ++g)
                        {
                            if (12 * g >= f.glyphRecords.size())
                                throw PanelError("a font with fewer glyphs than it says");
                            f.glyphRecords[12 * g] = panelDirectionGlyph(f.glyphRecords[12 * g]);
                        }
                    putBytes(data, fontChunk(font->second, f));
                }
            });
            writeMade(out / "UI.pmap", data);
            std::string missing;
            for (const auto &f : kUiFonts)
                if (!std::count(found.begin(), found.end(), f.first))
                    missing += " " + f.first;
            log.push_back(formatted("UI: %.1f MB, %d fonts, %d tricks%s", double(data.size()) / 1e6, int(found.size()), rows,
                                 missing.empty() ? "" : (", missing" + missing).c_str()));
        }

        // ---- the Xbox levels' career ------------------------------------------------------

        // CAREER.BIN (dm2_xbox_career.cpp writes it): "DM2XCAR1", u32 version,
        // u32 levels; each level a folder (16 bytes), its Xbox number and
        // blocks {u32 kind, size, relocations; the bytes; {u32 offset,
        // string, how}[relocations]}; the strings; the texts {s32 language,
        // s32 level, key, text}; the extra rewards.
        struct CareerLevel
        {
            std::string folder;
            uint32_t level = 0;
            std::optional<std::pair<Bytes, std::vector<std::array<uint32_t, 3>>>> goals; // the first goals block
            std::optional<std::pair<Bytes, std::vector<std::array<uint32_t, 3>>>> pros;  // the first pros block
        };
        // The blocks' kinds (dm2_xbox_career.cpp Kind): the goals, the local pros (the NPCs' definitions).
        constexpr uint32_t kCareerGoalsBlock = 1, kCareerProsBlock = 6;

        struct CareerLevelGoals
        {
            std::unique_ptr<CareerGoals> goals;
            Texts texts;
            std::vector<NpcGiver> givers;
        };
        struct Career
        {
            std::map<std::string, CareerLevelGoals> levels;
        };

        Career readCareer(const Bytes &data)
        {
            if (data.size() < 8 || std::memcmp(data.data(), "DM2XCAR1", 8) != 0)
                throw PanelError("CAREER.BIN is not one");
            const uint32_t count = u32At(data, 12);
            size_t at = 16;
            std::vector<CareerLevel> levels;
            for (uint32_t l = 0; l < count; ++l)
            {
                CareerLevel level;
                // (the folder's zeros after it dropped; a zero inside it is kept)
                const Bytes folder = dm2::sliceOf(data, at, at + 16);
                level.folder.assign(folder.begin(), folder.end());
                while (!level.folder.empty() && level.folder.back() == 0)
                    level.folder.pop_back();
                level.level = u32At(data, at + 16);
                const uint32_t blocks = u32At(data, at + 20);
                at += 24;
                for (uint32_t b = 0; b < blocks; ++b)
                {
                    const uint32_t kind = u32At(data, at), size = u32At(data, at + 4), relocations = u32At(data, at + 8);
                    at += 12;
                    Bytes body = dm2::sliceOf(data, at, at + size);
                    at += size;
                    std::vector<std::array<uint32_t, 3>> rel;
                    for (uint32_t i = 0; i < relocations; ++i)
                        rel.push_back({u32At(data, at + 12 * i), u32At(data, at + 12 * i + 4), u32At(data, at + 12 * i + 8)});
                    at += 12 * size_t(relocations);
                    if (kind == kCareerGoalsBlock && !level.goals)
                        level.goals = std::make_pair(std::move(body), std::move(rel));
                    else if (kind == kCareerProsBlock && !level.pros)
                        level.pros = std::make_pair(std::move(body), std::move(rel));
                }
                levels.push_back(std::move(level));
            }
            std::vector<std::string> strings;
            const uint32_t stringCount = u32At(data, at);
            at += 4;
            for (uint32_t i = 0; i < stringCount; ++i)
            {
                const uint32_t length = u32At(data, at);
                const Bytes text = dm2::sliceOf(data, at + 4, at + 4 + length);
                strings.emplace_back(text.begin(), text.end());
                at += 4 + size_t(length);
            }
            struct Text
            {
                int32_t language, level;
                std::string key, text;
            };
            std::vector<Text> texts;
            const uint32_t textCount = u32At(data, at);
            at += 4;
            for (uint32_t i = 0; i < textCount; ++i)
            {
                Text t;
                t.language = dm2::s32At(data, at);
                t.level = dm2::s32At(data, at + 4);
                const uint32_t keyLength = u32At(data, at + 8);
                const Bytes key = dm2::sliceOf(data, at + 12, at + 12 + keyLength);
                t.key.assign(key.begin(), key.end());
                at += 12 + size_t(keyLength);
                const uint32_t textLength = u32At(data, at);
                const Bytes text = dm2::sliceOf(data, at + 4, at + 4 + textLength);
                t.text.assign(text.begin(), text.end());
                at += 4 + size_t(textLength);
                texts.push_back(std::move(t));
            }
            const uint32_t extras = u32At(data, at);
            at += 4 + 16 * size_t(extras);
            if (at != data.size())
                throw PanelError("CAREER.BIN has bytes left over");
            Career out;
            for (CareerLevel &level : levels)
            {
                if (!level.goals)
                    continue;
                Texts english;
                for (const Text &t : texts)
                    if (t.language == 0 && t.level == int32_t(level.level) && !t.text.empty())
                        english[dm2::nameHash(t.key)] = t.text;
                std::vector<NpcGiver> givers;
                if (level.pros)
                    givers = npcGivers(CareerGoals(level.pros->first, level.pros->second, strings));
                out.levels[level.folder] = {std::make_unique<CareerGoals>(level.goals->first, level.goals->second, strings),
                                            std::move(english), std::move(givers)};
            }
            return out;
        }

        // An Xbox level's file: its map, and its goals from CAREER.BIN when it has them.
        void buildXboxLevel(const SourceFiles &xbox, const std::string &folder, const std::string &title, const Career &career,
                            const Texts &names, const std::filesystem::path &out, std::vector<std::string> &log)
        {
            const std::string path = folder + "/" + folder + ".";
            Bytes geo, ins;
            if (!xbox.readAny(path + "GEO", geo) || !xbox.readAny(path + "INS", ins))
                throw PanelError("the DM2 Xbox level " + folder + " is not whole");
            Bytes goals;
            int goalCount = 0;
            const auto it = career.levels.find(folder);
            if (it != career.levels.end())
            {
                Texts texts = names;
                for (const auto &[key, text] : it->second.texts)
                    texts[key] = text;
                const GoalChunks made = goalChunks(*it->second.goals, texts, kXboxSlot, dm2GoalPlaces(geo, ins), it->second.givers);
                goals = made.bytes;
                goalCount = made.shown;
                log.push_back(formatted("%s: %d goals, %d placed, %d givers, %d targets", folder.c_str(), made.shown, made.placed,
                                     made.givers, made.targets));
            }
            std::string line;
            writeMade(out / (folder + ".pmap"), dm2FormatLevelFile(geo, ins, folder, title, goals, goalCount, true, line));
            log.push_back(line);
        }

        std::string titleCase(const std::string &text)
        {
            std::string out = text;
            bool start = true;
            for (char &c : out)
            {
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (letter)
                    c = start ? char(std::toupper(uint8_t(c))) : char(std::tolower(uint8_t(c)));
                start = !letter;
            }
            return out;
        }
    }

    Bytes daveMirraMapFile(const SourceFiles &files, const std::string &folder, const std::string &title, std::vector<std::string> &log)
    {
        // The map alone: a FREERIDE row runs no goals (ProQuest's are the
        // executable's, for the row's own level).
        const std::string path = "ASSETS/LEVELS/" + folder + "/" + folder + ".";
        std::string line;
        Bytes file = dm2FormatLevelFile(needFile(files, path + "GEO"), needFile(files, path + "INS"), folder, title, {}, 0, true, line);
        log.push_back(line);
        return file;
    }

    void makeDaveMirraMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                           std::vector<std::string> &log)
    {
        const Elf elf(needFile(disc, "SLUS_201.59"));
        making("UI.pmap", [&] { buildUi(disc, elf, out, log); });
        const Texts names = dm2Strings(needFile(disc, "ASSETS/STRING.EUS"));
        for (uint32_t value = 0; value < 8; ++value)
        {
            const std::string level = kLevels[value];
            if (!wanted(extras, level))
                continue;
            const std::string title = lookup(names, formatted("KSTR_LEVELNAMEMENU_%u", value)).value_or(titleCase(level));
            const uint32_t entry = kMenuGameLevels + value;
            const uint32_t gameLevel = (elf.word(entry & ~3u) >> (8 * (entry & 3))) & 0xFF;
            making("the level " + level, [&] {
                const std::string path = "ASSETS/LEVELS/" + level + "/" + level + ".";
                const Bytes geo = needFile(disc, path + "GEO"), ins = needFile(disc, path + "INS");
                const GoalChunks goals = goalChunks(ElfTable(elf, kGoalTables + gameLevel * kGameLevelBytes), names, int(gameLevel),
                                                    dm2GoalPlaces(geo, ins), npcGivers(ElfTable(elf, kNpcTables + gameLevel * kNpcLevelBytes)));
                log.push_back(formatted("%s: %d goals, %d placed, %d givers, %d targets", level.c_str(), goals.shown, goals.placed,
                                     goals.givers, goals.targets));
                std::string line;
                writeMade(out / (level + ".pmap"), dm2FormatLevelFile(geo, ins, level, title, goals.bytes, goals.shown, true, line));
                log.push_back(line);
            });
        }
        if (!extras.dm2Xbox)
            return;
        Career career;
        Bytes careerBytes;
        if (extras.dm2Xbox->readAny("CAREER.BIN", careerBytes))
            making("CAREER.BIN", [&] { career = readCareer(careerBytes); });
        for (const auto &[folder, title] : kXboxLevels)
        {
            if (!extras.dm2Xbox->hasFolder(folder) || !wanted(extras, folder))
                continue;
            making(std::string("the level ") + folder, [&] { buildXboxLevel(*extras.dm2Xbox, folder, title, career, names, out, log); });
        }
    }
}
