// A level's instances, scene points, scene file and DM2's texture library
// (collection/levels/scene.h).
#include "collection/levels/scene.h"

#include <algorithm>
#include <cmath>

namespace ps2x::app::levels
{
    const uint32_t kSpawnPoint = nameHash("Spawn");
    const uint32_t kGlobalsPoint = nameHash("Globals");
    const uint32_t kPlayerOne = nameHash("PlayerOne");
    const uint32_t kPlayerTwo = nameHash("PlayerTwo");
    const uint32_t kCameraPoint = nameHash("CameraPos");
    const uint32_t kIdleCamera = nameHash("idle");

    namespace
    {
        constexpr const char *kSceneNames[] = {"ZFTX", "ZFGE", "ZFIS", "ZFSD"};
        constexpr const char *kSceneExtensions[] = {"TXR", "GEO", "INS", "DAT"};

        Tag tagOf(const std::string &text) { return {text[0], text[1], text[2], text[3]}; }

        Bytes paddedName(const std::string &text)
        {
            Bytes data(text.begin(), text.end());
            data.push_back(0);
            return paddedTo(std::move(data), 4);
        }
    }

    // ---- instances --------------------------------------------------------------

    Instance Instance::read(const Bytes &data, std::optional<Bytes> tags, Engine engine)
    {
        const size_t size = engine == Engine::Zge2 ? 0x6C : 0x70;
        if (data.size() != size)
            throw LevelError("TGIH of " + std::to_string(data.size()) + " bytes");
        Instance i;
        i.flags = readU32(data, 0);
        i.level = readU8(data, 4);
        i.materialSet = readU8(data, 5);
        if (readU16(data, 6))
            throw LevelError("TGIH with its padding set");
        i.rotation = readFloats<4>(data, 8);
        i.position = readFloats<3>(data, 0x18);
        i.worldMin = readFloats<3>(data, 0x24);
        i.worldMax = readFloats<3>(data, 0x30);
        i.nameHash = readU32(data, 0x3C);
        i.geometryId = readU32(data, 0x40);
        i.parent = readU32(data, 0x44);
        i.instanceId = readU32(data, 0x48);
        if (engine == Engine::Zge2)
            i.name = cString(data, 0x4C, 32);
        else
        {
            i.region = readU32(data, 0x4C);
            i.name = cString(data, 0x50, 32);
        }
        i.tags = std::move(tags);
        return i;
    }

    Bytes Instance::write(Engine engine) const
    {
        ByteWriter w;
        w.u32(flags).u8(level).u8(materialSet).u16(0).f32s(rotation).f32s(position).f32s(worldMin).f32s(worldMax);
        w.u32(nameHash).u32(geometryId).u32(parent).u32(instanceId);
        if (engine == Engine::Zge2)
        {
            if (region)
                throw LevelError("instance " + name + ": DM2's TGIH has no region word");
        }
        else
            w.u32(region);
        w.text(name, 32);
        return w.take();
    }

    Instance Instance::make(const std::string &name, const GeoHeader &object, const Vec3 &position, const Quat &rotation,
                            bool collides, std::optional<Bytes> tags)
    {
        Instance i;
        std::vector<Vec3> world;
        for (double x : {object.boundsMin[0], object.boundsMax[0]})
            for (double y : {object.boundsMin[1], object.boundsMax[1]})
                for (double z : {object.boundsMin[2], object.boundsMax[2]})
                {
                    const Vec3 turned = quatRotate(rotation, {x, y, z});
                    world.push_back({turned[0] + position[0], turned[1] + position[1], turned[2] + position[2]});
                }
        for (int a = 0; a < 3; ++a)
        {
            i.worldMin[a] = world[0][a];
            i.worldMax[a] = world[0][a];
            for (const Vec3 &p : world)
            {
                i.worldMin[a] = std::min(i.worldMin[a], p[a]);
                i.worldMax[a] = std::max(i.worldMax[a], p[a]);
            }
        }
        i.flags = collides ? kCollides : 0;
        i.rotation = rotation;
        i.position = position;
        i.geometryId = object.id;
        i.instanceId = levels::nameHash(name);
        i.name = name;
        i.nameHash = levels::nameHash(name);
        i.tags = std::move(tags);
        return i;
    }

    Instances Instances::read(const Bytes &data, Engine engine)
    {
        const std::vector<Chunk> tree = parseChunks(data);
        if (tree.size() != 1 || tree[0].tag != tag("FORM"))
            throw LevelError("an INS is one FORM");
        Instances out;
        for (const Chunk &g : tree[0].kids)
        {
            if (g.tag != tag("TGIN"))
                throw LevelError("a " + tagText(g.tag) + " chunk in an INS");
            const bool plain = g.kids.size() == 1 && g.kids[0].tag == tag("TGIH");
            const bool tagged = g.kids.size() == 2 && g.kids[0].tag == tag("TGIH") && g.kids[1].tag == tag("TGIT");
            if (!plain && !tagged)
                throw LevelError("a TGIN of other chunks than TGIH, TGIT");
            out.instances.push_back(
                Instance::read(g.kids[0].data, tagged ? std::optional<Bytes>(g.kids[1].data) : std::nullopt, engine));
        }
        return out;
    }

    Bytes Instances::write(Engine engine) const
    {
        std::vector<Chunk> kids;
        for (const Instance &i : instances)
        {
            std::vector<Chunk> parts = {Chunk::leaf(tag("TGIH"), i.write(engine))};
            if (i.tags)
                parts.push_back(Chunk::leaf(tag("TGIT"), *i.tags));
            kids.push_back(Chunk::node(tag("TGIN"), std::move(parts)));
        }
        return serializeChunks({Chunk::node(tag("FORM"), std::move(kids))});
    }

    // ---- scene points and zones -----------------------------------------------------

    ScenePoint ScenePoint::read(const Bytes &data, Engine engine)
    {
        ScenePoint p;
        const uint32_t header = readU32(data, 0);
        p.kind = readU32(data, 4);
        p.name = readU32(data, 8);
        if (engine == Engine::Zge2)
        {
            p.index = readU32(data, 12);
            if (header != kHeader)
                throw LevelError("SDPT header of " + std::to_string(header) + " bytes");
        }
        else
        {
            p.index = readU16(data, 12);
            if (header != kHeader || readU16(data, 14))
                throw LevelError("SDPT header of " + std::to_string(header) + " bytes (or its padding set)");
        }
        p.position = readFloats<3>(data, 0x10);
        p.orientation = readFloats<4>(data, 0x1C);
        p.tags = slice(data, header);
        return p;
    }

    Bytes ScenePoint::write(Engine engine) const
    {
        ByteWriter w;
        w.u32(kHeader).u32(kind).u32(name);
        if (engine == Engine::Zge2)
            w.u32(index);
        else
            w.u16(static_cast<uint16_t>(index)).u16(0);
        w.f32s(position).f32s(orientation).bytes(tags);
        return w.take();
    }

    ScenePoint ScenePoint::spawn(const std::string &name, const Vec3 &position, const Quat &orientation, Bytes tags)
    {
        ScenePoint p;
        p.kind = kSpawnPoint;
        p.name = nameHash(name);
        p.position = position;
        p.orientation = orientation;
        p.tags = std::move(tags);
        return p;
    }

    ViewRegion ViewRegion::read(const Bytes &data)
    {
        ViewRegion r;
        r.mesh = readU32(data, 0);
        r.low = readFloats<3>(data, 4);
        r.high = readFloats<3>(data, 0x10);
        const uint32_t count = readU32(data, 0x1C);
        size_t first;
        if (data.size() == 0x24 + 4u * count)
        {
            r.flag = readU32(data, 0x20);
            first = 0x24;
        }
        else if (data.size() == 0x20 + 4u * count)
        {
            r.flag = std::nullopt;
            first = 0x20;
        }
        else
            throw LevelError("PVSO of the wrong size for its zones");
        for (uint32_t i = 0; i < count; ++i)
            r.hidden.push_back(readU32(data, first + 4 * i));
        return r;
    }

    Bytes ViewRegion::write() const
    {
        ByteWriter w;
        w.u32(mesh).f32s(low).f32s(high).u32(static_cast<uint32_t>(hidden.size()));
        if (flag)
            w.u32(*flag);
        for (uint32_t h : hidden)
            w.u32(h);
        return w.take();
    }

    SceneData SceneData::read(const Bytes &data, Engine engine)
    {
        const std::vector<Chunk> tree = parseChunks(data);
        if (tree.size() != 1 || tree[0].tag != tag("FORM"))
            throw LevelError("a DAT is one FORM");
        SceneData out;
        for (const Chunk &k : tree[0].kids)
        {
            out.order.push_back(k.tag);
            if (k.tag == tag("SDPT"))
                out.points.push_back(ScenePoint::read(k.data, engine));
            else if (k.tag == tag("ZPVS"))
            {
                std::vector<ViewRegion> regions;
                for (const Chunk &p : k.kids)
                {
                    if (p.tag != tag("PVSO"))
                        throw LevelError("a ZPVS with a " + tagText(p.tag));
                    regions.push_back(ViewRegion::read(p.data));
                }
                out.regions = std::move(regions);
            }
            else
                out.other.push_back(k);
        }
        return out;
    }

    Bytes SceneData::write(Engine engine) const
    {
        if (engine == Engine::Zge2 && regions)
            for (const ViewRegion &r : *regions)
                if (r.flag)
                    throw LevelError("DM2 reads PVSO's older body only");
        std::vector<Tag> sequence = order;
        std::vector<Chunk> others = other;
        if (order.empty())
        {
            sequence.assign(points.size(), tag("SDPT"));
            sequence.push_back(tag("ZLIT"));
            if (regions)
                sequence.push_back(tag("ZPVS"));
            others = {Chunk::leaf(tag("ZLIT"), {})};
        }
        std::vector<Chunk> kids;
        size_t point = 0, next = 0;
        for (const Tag &t : sequence)
        {
            if (t == tag("SDPT"))
            {
                if (point >= points.size())
                    throw LevelError("a DAT's order names more points than it has");
                kids.push_back(Chunk::leaf(tag("SDPT"), points[point++].write(engine)));
            }
            else if (t == tag("ZPVS"))
            {
                std::vector<Chunk> zones;
                for (const ViewRegion &r : *regions)
                    zones.push_back(Chunk::leaf(tag("PVSO"), r.write()));
                kids.push_back(Chunk::node(tag("ZPVS"), std::move(zones)));
            }
            else
            {
                if (next >= others.size())
                    throw LevelError("a DAT's order names more chunks than it has");
                kids.push_back(others[next++]);
            }
        }
        return serializeChunks({Chunk::node(tag("FORM"), std::move(kids))});
    }

    // ---- the scene file ---------------------------------------------------------------

    SceneFile SceneFile::read(const Bytes &data)
    {
        const std::vector<Chunk> tree = parseChunks(data);
        if (tree.size() != 1 || tree[0].tag != tag("FSCN") || tree[0].kids.size() != 1 || tree[0].kids[0].tag != tag("TERR"))
            throw LevelError("a ZIT is FSCN { TERR }");
        const std::vector<Chunk> &kids = tree[0].kids[0].kids;
        if (kids.size() != 5 || kids[0].tag != tag("TEHD"))
            throw LevelError("a TERR of other chunks than TEHD and the four names");
        for (int n = 0; n < 4; ++n)
            if (kids[1 + n].tag != tagOf(kSceneNames[n]))
                throw LevelError("a TERR of other chunks than TEHD and the four names");
        if (kids[0].data.size() != 0x30)
            throw LevelError("a TEHD of " + std::to_string(kids[0].data.size()) + " bytes");
        SceneFile z;
        z.cell = readFloats<3>(kids[0].data, 0);
        z.low = readFloats<3>(kids[0].data, 12);
        z.high = readFloats<3>(kids[0].data, 24);
        for (int k = 0; k < 3; ++k)
            z.counts[k] = readU32(kids[0].data, 36 + 4 * k);
        for (int n = 0; n < 4; ++n)
        {
            const std::string text = cString(kids[1 + n].data, 0, kids[1 + n].data.size());
            if (kids[1 + n].data != paddedName(text))
                throw LevelError("a " + std::string(kSceneNames[n]) + " name not zero-padded to 4");
            z.names[kSceneNames[n]] = text;
        }
        return z;
    }

    Bytes SceneFile::write() const
    {
        ByteWriter head;
        head.f32s(cell).f32s(low).f32s(high).u32(counts[0]).u32(counts[1]).u32(counts[2]);
        std::vector<Chunk> kids = {Chunk::leaf(tag("TEHD"), head.take())};
        for (const char *n : kSceneNames)
            kids.push_back(Chunk::leaf(tagOf(n), paddedName(names.at(n))));
        return serializeChunks({Chunk::node(tag("FSCN"), {Chunk::node(tag("TERR"), std::move(kids))})});
    }

    SceneFile SceneFile::make(const std::string &level, const Vec3 &low, const Vec3 &high, double cell)
    {
        SceneFile z;
        z.cell = {cell, cell, cell};
        z.low = low;
        z.high = high;
        for (int k = 0; k < 3; ++k)
            z.counts[k] = static_cast<uint32_t>(std::max(1.0, std::nearbyint((high[k] - low[k]) / cell)));
        for (int n = 0; n < 4; ++n)
            z.names[kSceneNames[n]] = level + "." + kSceneExtensions[n];
        return z;
    }

    // ---- DM2's texture library ----------------------------------------------------------

    LibraryTexture LibraryTexture::read(const Chunk &chunk)
    {
        if (chunk.tag != tag("TXRC") || chunk.kids.empty() || chunk.kids[0].tag != tag("TXRH"))
            throw LevelError("a texture is TXRC { TXRH, TBMP ... }");
        const Bytes &head = chunk.kids[0].data;
        if (head.size() != 0x34 || std::any_of(head.begin() + 0x0C, head.begin() + 0x14, [](uint8_t b) { return b != 0; }))
            throw LevelError("TXRH of the wrong size or with its padding set");
        LibraryTexture t;
        t.id = readU32(head, 0);
        t.width = readU16(head, 4);
        t.height = readU16(head, 6);
        t.reserved08 = readU16(head, 8);
        t.reserved0a = readU16(head, 10);
        t.name = cString(head, 0x14, 32);
        for (size_t i = 1; i < chunk.kids.size(); ++i)
        {
            const Chunk &k = chunk.kids[i];
            if (k.tag != tag("TBMP") || k.data.size() < 6 || k.data[0] != 'B' || k.data[1] != 'M')
                throw LevelError("a " + tagText(k.tag) + " level in a texture");
            const uint32_t size = readU32(k.data, 2);
            if (k.data.size() != ((size + 3u) & ~3u) ||
                std::any_of(k.data.begin() + size, k.data.end(), [](uint8_t b) { return b != 0; }))
                throw LevelError("a TBMP of the wrong size for its BMP");
            t.levels.push_back(slice(k.data, 0, size));
        }
        return t;
    }

    Chunk LibraryTexture::chunk() const
    {
        ByteWriter head;
        head.u32(id).u16(width).u16(height).u16(reserved08).u16(reserved0a).zeros(8).text(name, 32);
        std::vector<Chunk> kids = {Chunk::leaf(tag("TXRH"), head.take())};
        for (const Bytes &bmp : levels)
            kids.push_back(Chunk::leaf(tag("TBMP"), paddedTo(bmp, 4)));
        return Chunk::node(tag("TXRC"), std::move(kids));
    }

    TextureLibrary TextureLibrary::read(const Bytes &data)
    {
        const std::vector<Chunk> tree = parseChunks(data);
        if (tree.size() != 1 || tree[0].tag != tag("FORM"))
            throw LevelError("a TXR is one FORM");
        TextureLibrary lib;
        for (const Chunk &c : tree[0].kids)
            lib.textures.push_back(LibraryTexture::read(c));
        return lib;
    }

    Bytes TextureLibrary::write() const
    {
        std::vector<Chunk> kids;
        for (const LibraryTexture &t : textures)
            kids.push_back(t.chunk());
        return serializeChunks({Chunk::node(tag("FORM"), std::move(kids))});
    }

    // ---- the small files ------------------------------------------------------------------

    Bytes emptyLevelScript()
    {
        // Version 2.70, as the Zoo prototype's 60-byte ZOOT.BIN: one object
        // with one empty state, one variable word.
        constexpr uint32_t kState = 0x04588EA5, kObject = 0x0049A229;
        ByteWriter w;
        w.f32(2.70).u32(9).u32(1).u32(0).u32(0);
        for (uint32_t word : {kState, kObject, 3u, 0xFFFFFFFFu, 0u, 0u, kState, 5u, 0xFFFFFFFFu, 0xFFFFFFFFu})
            w.u32(word);
        return w.take();
    }

    Bytes emptyAssetLibrary()
    {
        // ZDIR { DIDF (no entries, the unread word 0), GRID, DLEN, OFST }.
        const Chunk didf = Chunk::leaf(tag("DIDF"), Bytes(8, 0));
        const Bytes directory = serializeChunks({didf, Chunk::leaf(tag("GRID"), {}), Chunk::leaf(tag("DLEN"), {}), Chunk::leaf(tag("OFST"), {})});
        ByteWriter w;
        w.bytes(reinterpret_cast<const uint8_t *>("ZDIR"), 4).u32(static_cast<uint32_t>(directory.size())).bytes(directory);
        return w.take();
    }

    Bytes emptyObjectivesFile() { return {'\r', '\n'}; }
}
