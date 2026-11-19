// A level's geometry records (collection/levels/geometry.h).
#include "collection/levels/geometry.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ps2x::app::levels
{
    namespace
    {
        struct StreamTags
        {
            Tag plain;
            Tag indexed;
            size_t stride;
            bool perFace;
        };
        // In the order the readers try them (ai_level_write.py STREAMS).
        constexpr StreamTags kStreams[] = {
            {tag("TGVN"), tag("TIVN"), 12, false}, // normals
            {tag("TGVC"), tag("TIVC"), 4, false},  // colours
            {tag("TGVU"), tag("TIVU"), 8, false},  // uvs
            {tag("TGFN"), tag("TIFN"), 12, true},  // face normals
        };

        size_t edgeRecordSize(Engine engine) { return engine == Engine::Zge3 ? EdgeTag::kSize : BikeEdgeTag::kSize; }
        size_t faceRecordSize(Engine engine) { return engine == Engine::Zge3 ? FaceTag::kSize : BikeFaceTag::kSize; }
        constexpr size_t kCornerRecordSize = 4;

        // The exporters' chunk order for a part made from scratch.
        std::vector<Tag> defaultOrder(Engine engine)
        {
            switch (engine)
            {
            case Engine::Zge2:
                return {tag("TGPH"), tag("TGVP"), tag("TGVN"), tag("TGVC"), tag("TGVU"), tag("TGFP"), tag("TGFN"), tag("TGFM"),
                        tag("TGFE"), tag("TGFT")};
            case Engine::Zge4:
                return {tag("TGPH"), tag("TIVC"), tag("TGVC"), tag("TIVN"), tag("TGVN"), tag("TIVU"), tag("TGVU"), tag("TIFN"),
                        tag("TGFN"), tag("TGVP"), tag("TGFP"), tag("TGFM"), tag("TGFV"), tag("TGFE"), tag("TGFT")};
            case Engine::Zge3:
            default:
                return {tag("TGPH"), tag("TGVP"), tag("TIVN"), tag("TGVN"), tag("TIVC"), tag("TGVC"), tag("TIVU"), tag("TGVU"),
                        tag("TGFP"), tag("TIFN"), tag("TGFN"), tag("TGFM"), tag("TGFE"), tag("TGFT")};
            }
        }

        // ---- collision trees ------------------------------------------------

        std::unique_ptr<FaceTreeNode> readOffsetNode(const Bytes &blob, size_t at, int depth)
        {
            if (depth > 64)
                throw LevelError("collision tree deeper than 64");
            auto node = std::make_unique<FaceTreeNode>();
            node->axis = readU8(blob, at);
            node->leftover = readU8(blob, at + 1);
            if (node->axis == 0)
            {
                const uint16_t count = readU16(blob, at + 2);
                const uint32_t offset = readU32(blob, at + 4);
                for (uint16_t i = 0; i < count; ++i)
                    node->faces.push_back(readU16(blob, offset * 2u + 2u * i));
                return node;
            }
            if (node->axis > 3)
                throw LevelError("collision tree node of type " + std::to_string(node->axis));
            node->plane = readS16(blob, at + 2);
            node->greater = readOffsetNode(blob, readU16(blob, at + 4) * 4u, depth + 1);
            node->lesser = readOffsetNode(blob, readU16(blob, at + 6) * 4u, depth + 1);
            return node;
        }

        std::unique_ptr<FaceTreeNode> readInlineNode(const Bytes &blob, size_t at, int depth)
        {
            if (depth > 64)
                throw LevelError("collision tree deeper than 64");
            auto node = std::make_unique<FaceTreeNode>();
            node->axis = readU8(blob, at);
            node->leftover = readU8(blob, at + 1);
            if (node->axis == 0)
            {
                const uint16_t count = readU16(blob, at + 2);
                for (uint16_t i = 0; i < count; ++i)
                    node->faces.push_back(readU16(blob, at + 4 + 2u * i));
                if (count & 1)
                    node->tail = slice(blob, at + 4 + 2u * count, 2);
                return node;
            }
            if (node->axis > 3)
                throw LevelError("collision tree node of type " + std::to_string(node->axis));
            node->plane = readS16(blob, at + 2);
            node->greater = readInlineNode(blob, readU16(blob, at + 4) * 4u, depth + 1);
            node->lesser = readInlineNode(blob, readU16(blob, at + 6) * 4u, depth + 1);
            return node;
        }

        void preorder(const FaceTreeNode *n, std::vector<const FaceTreeNode *> &out)
        {
            out.push_back(n);
            if (!n->leaf())
            {
                preorder(n->greater.get(), out);
                preorder(n->lesser.get(), out);
            }
        }

        // ZGE2's inline form, the greater side first.
        void emitInline(const FaceTreeNode *n, Bytes &out)
        {
            const size_t at = out.size();
            ByteWriter w;
            if (n->leaf())
            {
                w.u8(0).u8(n->leftover).u16(static_cast<uint16_t>(n->faces.size()));
                for (uint16_t f : n->faces)
                    w.u16(f);
                if (n->faces.size() & 1)
                {
                    Bytes tail = n->tail.empty() ? Bytes{0, 0} : n->tail;
                    tail.resize(2, 0);
                    w.bytes(tail);
                }
                out.insert(out.end(), w.data().begin(), w.data().end());
                return;
            }
            w.u8(n->axis).u8(n->leftover).s16(n->plane).u16(0).u16(0);
            out.insert(out.end(), w.data().begin(), w.data().end());
            emitInline(n->greater.get(), out);
            const size_t lesser = out.size();
            emitInline(n->lesser.get(), out);
            if (lesser / 4 > 0xFFFF)
                throw LevelError("collision tree of " + std::to_string(out.size()) + " bytes: past the u16 word offsets");
            const uint16_t greaterWord = static_cast<uint16_t>((at + 8) / 4), lesserWord = static_cast<uint16_t>(lesser / 4);
            out[at + 4] = static_cast<uint8_t>(greaterWord);
            out[at + 5] = static_cast<uint8_t>(greaterWord >> 8);
            out[at + 6] = static_cast<uint8_t>(lesserWord);
            out[at + 7] = static_cast<uint8_t>(lesserWord >> 8);
        }

        void collectLeaves(const FaceTreeNode *n, std::set<int> &faces)
        {
            if (n->leaf())
            {
                faces.insert(n->faces.begin(), n->faces.end());
                return;
            }
            collectLeaves(n->greater.get(), faces);
            collectLeaves(n->lesser.get(), faces);
        }

        using Bounds = std::array<std::array<double, 2>, 3>; // per axis: low, high

        std::unique_ptr<FaceTreeNode> buildNode(const std::vector<int> &members, int level, const std::map<int, Bounds> &boxes)
        {
            if (members.size() > CollisionTree::kLeafFaces && level < CollisionTree::kDepth)
            {
                std::array<std::array<double, 2>, 3> extent{};
                for (int a = 0; a < 3; ++a)
                {
                    double lo = boxes.at(members[0])[a][0], hi = boxes.at(members[0])[a][1];
                    for (int f : members)
                    {
                        lo = std::min(lo, boxes.at(f)[a][0]);
                        hi = std::max(hi, boxes.at(f)[a][1]);
                    }
                    extent[a] = {lo, hi};
                }
                // The longest axis (the first of equals, as max() keeps).
                int axis = 0;
                for (int a = 1; a < 3; ++a)
                    if (extent[a][1] - extent[a][0] > extent[axis][1] - extent[axis][0])
                        axis = a;
                std::vector<double> centres;
                for (int f : members)
                    centres.push_back((boxes.at(f)[axis][0] + boxes.at(f)[axis][1]) / 2);
                std::sort(centres.begin(), centres.end());
                const double plane = std::nearbyint(centres[centres.size() / 2] * 100);
                if (plane >= -32768 && plane <= 32767)
                {
                    const double at = plane / 100.0;
                    std::vector<int> low, high;
                    for (int f : members)
                        if (boxes.at(f)[axis][0] <= at)
                            low.push_back(f);
                    for (int f : members)
                        if (boxes.at(f)[axis][1] >= at)
                            high.push_back(f);
                    if (low.size() < members.size() && high.size() < members.size())
                    {
                        auto node = std::make_unique<FaceTreeNode>();
                        node->axis = static_cast<uint8_t>(axis + 1);
                        node->plane = static_cast<int16_t>(plane);
                        node->greater = buildNode(high, level + 1, boxes);
                        node->lesser = buildNode(low, level + 1, boxes);
                        return node;
                    }
                }
            }
            auto leaf = std::make_unique<FaceTreeNode>();
            std::vector<int> sorted = members;
            std::sort(sorted.begin(), sorted.end());
            for (int f : sorted)
                leaf->faces.push_back(static_cast<uint16_t>(f));
            return leaf;
        }
    }

    Vec3 quatRotate(const Quat &q, const Vec3 &v)
    {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
        return {v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)};
    }

    Quat quatMultiply(const Quat &a, const Quat &b)
    {
        const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
        const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
        return {aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw,
                aw * bw - ax * bx - ay * by - az * bz};
    }

    // ---- faces and strips -----------------------------------------------------

    Face Face::make(int material, int a, int b, int c, bool collides, bool twoSided)
    {
        Face f;
        f.flags = static_cast<uint16_t>((material & 0xFF) | 0x300 | (collides ? kCollides : 0) | (twoSided ? kTwoSided : 0));
        f.vertices = {static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint16_t>(c)};
        return f;
    }

    std::vector<StripGroup> StripGroup::readAll(const Bytes &data)
    {
        std::vector<StripGroup> groups;
        size_t at = 0;
        while (at + 4 <= data.size())
        {
            const uint16_t head = readU16(data, at), count = readU16(data, at + 2);
            StripGroup g;
            g.submesh = static_cast<uint8_t>(head >> 8);
            g.mode = static_cast<uint8_t>(head & 0xFF);
            for (uint16_t i = 0; i < count; ++i)
                g.entries.push_back(readU16(data, at + 4 + 2u * i));
            groups.push_back(std::move(g));
            at += (count * 2u + 7u) & ~3u;
        }
        if (at != data.size())
            throw LevelError("TGFM with stray bytes");
        return groups;
    }

    Bytes StripGroup::writeAll(const std::vector<StripGroup> &groups)
    {
        ByteWriter w;
        for (const StripGroup &g : groups)
        {
            if (g.entries.size() > kGroupEntries)
                throw LevelError("a strip group of " + std::to_string(g.entries.size()) + " entries (at most 60)");
            const size_t start = w.size();
            w.u16(static_cast<uint16_t>((g.submesh << 8) | g.mode)).u16(static_cast<uint16_t>(g.entries.size()));
            for (uint16_t e : g.entries)
                w.u16(e);
            while ((w.size() - start) % 4)
                w.u8(0);
        }
        return w.take();
    }

    std::vector<StripGroup> StripGroup::fromStrip(int submesh, const std::vector<int> &strip, int mode, size_t maxEntries)
    {
        std::vector<uint16_t> entries;
        for (size_t k = 0; k < strip.size(); ++k)
        {
            const int v = strip[k];
            if (k < 2 || strip[k - 2] == strip[k - 1] || strip[k - 1] == v || strip[k - 2] == v)
                entries.push_back(static_cast<uint16_t>(v | kNoTriangle));
            else
                entries.push_back(static_cast<uint16_t>(v | ((k & 1) ? kFlipped : 0)));
        }
        std::vector<StripGroup> groups;
        maxEntries = std::max<size_t>(maxEntries, 4);
        size_t k = 0;
        while (k < entries.size())
        {
            StripGroup g;
            g.submesh = static_cast<uint8_t>(submesh);
            g.mode = static_cast<uint8_t>(mode);
            if (k >= 2)
            {
                g.entries.push_back(static_cast<uint16_t>((entries[k - 2] & 0x3FFF) | kNoTriangle));
                g.entries.push_back(static_cast<uint16_t>((entries[k - 1] & 0x3FFF) | kNoTriangle));
            }
            while (k < entries.size() && g.entries.size() < maxEntries)
                g.entries.push_back(entries[k++]);
            groups.push_back(std::move(g));
        }
        return groups;
    }

    // ---- tag buffers and records ----------------------------------------------

    TagBuffer TagBuffer::read(const Bytes &data, size_t recordSize)
    {
        const uint32_t desc = readU32(data, 0), count = readU32(data, 4);
        const uint32_t kind = desc & 0xF, size = (desc >> 4) & 0x3FF, records = desc >> 14;
        if (kind != 1)
            throw LevelError("tag buffer of index kind " + std::to_string(kind));
        if (size != recordSize)
            throw LevelError("tag records of " + std::to_string(size) + " bytes (" + std::to_string(recordSize) + " expected)");
        const size_t indexBytes = (count * 2u + 15u) & ~15u;
        if (data.size() < 8 + indexBytes || data.size() - 8 - indexBytes != size_t(size) * records)
            throw LevelError("tag buffer of the wrong size for its records");
        TagBuffer out;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint16_t r = readU16(data, 8 + 2u * i);
            out.indices.push_back(r == 0xFFFF ? -1 : r);
        }
        for (uint32_t i = 0; i < records; ++i)
            out.records.push_back(slice(data, 8 + indexBytes + size_t(i) * size, size));
        return out;
    }

    Bytes TagBuffer::write() const
    {
        const uint32_t size = records.empty() ? 0 : static_cast<uint32_t>(records[0].size());
        ByteWriter w;
        w.u32(static_cast<uint32_t>(records.size() << 14) | (size << 4) | 1u).u32(static_cast<uint32_t>(indices.size()));
        for (int i : indices)
            w.u16(i < 0 ? 0xFFFF : static_cast<uint16_t>(i));
        while ((w.size() - 8) % 16)
            w.u8(0);
        for (const Bytes &r : records)
            w.bytes(r);
        return w.take();
    }

    EdgeTag EdgeTag::read(const Bytes &r)
    {
        EdgeTag t;
        t.eventId = readU32(r, 0);
        t.element = readU32(r, 4);
        t.kind = readU8(r, 8);
        t.flags = readU8(r, 9);
        t.byte0a = readU8(r, 10);
        t.material = readU8(r, 11);
        t.launchMin = readF32(r, 12);
        t.launchMax = readF32(r, 16);
        t.launchSpeed = readF32(r, 20);
        return t;
    }

    Bytes EdgeTag::write() const
    {
        ByteWriter w;
        w.u32(eventId).u32(element).u8(kind).u8(flags).u8(byte0a).u8(material).f32(launchMin).f32(launchMax).f32(launchSpeed);
        return w.take();
    }

    FaceTag FaceTag::read(const Bytes &r)
    {
        FaceTag t;
        t.eventKind = readU32(r, 0);
        t.nearestEdge = readU32(r, 4);
        t.face = readU32(r, 8);
        t.kind = readU8(r, 12);
        t.leftover = {readU8(r, 13), readU8(r, 14), readU8(r, 15)};
        t.gravity = readF32(r, 16);
        t.launchCap = readF32(r, 20);
        t.launchPush = readF32(r, 24);
        return t;
    }

    Bytes FaceTag::write() const
    {
        ByteWriter w;
        w.u32(eventKind).u32(nearestEdge).u32(face).u8(kind).u8(leftover[0]).u8(leftover[1]).u8(leftover[2]);
        w.f32(gravity).f32(launchCap).f32(launchPush);
        return w.take();
    }

    BikeEdgeTag BikeEdgeTag::read(const Bytes &r)
    {
        BikeEdgeTag t;
        t.tag = readU32(r, 0);
        t.linkKind = readU8(r, 4);
        t.flags = readU8(r, 5);
        t.grindSide = readU8(r, 6);
        t.wallKind = readU8(r, 7);
        return t;
    }

    Bytes BikeEdgeTag::write() const
    {
        ByteWriter w;
        w.u32(tag).u8(linkKind).u8(flags).u8(grindSide).u8(wallKind);
        return w.take();
    }

    BikeFaceTag BikeFaceTag::read(const Bytes &r)
    {
        BikeFaceTag t;
        t.tag = readU32(r, 0);
        t.link = readU32(r, 4);
        t.surfaceKind = readU8(r, 8);
        t.launch = readU8(r, 9);
        t.leftover = {readU8(r, 10), readU8(r, 11)};
        return t;
    }

    Bytes BikeFaceTag::write() const
    {
        ByteWriter w;
        w.u32(tag).u32(link).u8(surfaceKind).u8(launch).u8(leftover[0]).u8(leftover[1]);
        return w.take();
    }

    // ---- collision trees ----------------------------------------------------------

    CollisionTree CollisionTree::read(const Bytes &data, Engine engine)
    {
        const uint32_t partId = readU32(data, 0), size = readU32(data, 4);
        const Bytes blob = slice(data, 8, size);
        if (blob.size() != size || data.size() != ((8u + size + 3u) & ~3u))
            throw LevelError("TGCI of the wrong size for its tree");
        CollisionTree tree;
        tree.partId = partId;
        tree.root = engine == Engine::Zge2 ? readInlineNode(blob, 0, 0) : readOffsetNode(blob, 0, 0);
        if (engine == Engine::Zge2 && slice(tree.write(engine), 8) != slice(data, 8))
            throw LevelError("TGCI not in the exporter's layout (preorder, greater side first)");
        return tree;
    }

    Bytes CollisionTree::write(Engine engine) const
    {
        if (engine == Engine::Zge2)
        {
            Bytes out;
            emitInline(root.get(), out);
            ByteWriter w;
            w.u32(partId).u32(static_cast<uint32_t>(out.size())).bytes(out);
            return w.take();
        }
        std::vector<const FaceTreeNode *> order;
        preorder(root.get(), order);
        std::map<const FaceTreeNode *, size_t> index;
        for (size_t i = 0; i < order.size(); ++i)
            index[order[i]] = i;
        if (order.size() * 2 > 0xFFFF)
            throw LevelError("collision tree of " + std::to_string(order.size()) + " nodes: past the u16 word offsets");
        ByteWriter nodes, lists;
        size_t at = 8 * order.size();
        for (const FaceTreeNode *n : order)
        {
            if (n->leaf())
            {
                nodes.u8(0).u8(n->leftover).u16(static_cast<uint16_t>(n->faces.size())).u32(static_cast<uint32_t>(at / 2));
                for (uint16_t f : n->faces)
                    lists.u16(f);
                at += 2 * n->faces.size();
            }
            else
                nodes.u8(n->axis).u8(n->leftover).s16(n->plane).u16(static_cast<uint16_t>(2 * index[n->greater.get()]))
                    .u16(static_cast<uint16_t>(2 * index[n->lesser.get()]));
        }
        Bytes tree = nodes.take();
        const Bytes listBytes = lists.take();
        tree.insert(tree.end(), listBytes.begin(), listBytes.end());
        ByteWriter w;
        w.u32(partId).u32(static_cast<uint32_t>(tree.size())).bytes(tree).padTo(4);
        return w.take();
    }

    std::set<int> CollisionTree::faces() const
    {
        std::set<int> out;
        collectLeaves(root.get(), out);
        return out;
    }

    std::optional<CollisionTree> CollisionTree::build(uint32_t partId, const std::vector<Vec3> &positions, const std::vector<Face> &faces)
    {
        std::map<int, Bounds> boxes;
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const Face &f = faces[i];
            if (!(f.flags & Face::kCollides))
                continue;
            if (*std::max_element(f.vertices.begin(), f.vertices.end()) >= positions.size())
                continue;
            Bounds b{};
            for (int a = 0; a < 3; ++a)
            {
                double lo = positions[f.vertices[0]][a], hi = lo;
                for (uint16_t v : f.vertices)
                {
                    lo = std::min(lo, positions[v][a]);
                    hi = std::max(hi, positions[v][a]);
                }
                b[a] = {lo, hi};
            }
            boxes[static_cast<int>(i)] = b;
        }
        if (boxes.empty())
            return std::nullopt;
        std::vector<int> members;
        for (const auto &[f, b] : boxes)
            members.push_back(f);
        CollisionTree tree;
        tree.partId = partId;
        tree.root = buildNode(members, 0, boxes);
        return tree;
    }

    // ---- streams ----------------------------------------------------------------------

    IndexedStream IndexedStream::fromChunk(const Chunk &chunk, size_t stride, size_t count)
    {
        const Bytes *table = nullptr, *raw = nullptr;
        bool wide = false;
        for (const Chunk &k : chunk.kids)
            if (k.tag == tag("TIDA"))
            {
                if (k.data.size() % stride)
                    throw LevelError(tagText(chunk.tag) + " table not a multiple of its stride");
                table = &k.data;
            }
            else if (k.tag == tag("TII8") || k.tag == tag("TII6"))
            {
                wide = k.tag == tag("TII6");
                raw = &k.data;
            }
        if (!table || !raw)
            throw LevelError(tagText(chunk.tag) + " without its table or indices");
        const size_t width = wide ? 2 : 1;
        if (raw->size() != ((count * width + 3) & ~size_t(3)))
            throw LevelError(tagText(chunk.tag) + ": the wrong number of index bytes");
        IndexedStream s;
        s.wide = wide;
        for (size_t at = 0; at < table->size(); at += stride)
            s.table.push_back(slice(*table, at, stride));
        for (size_t i = 0; i < count; ++i)
            s.indices.push_back(wide ? readU16(*raw, 2 * i) : readU8(*raw, i));
        return s;
    }

    Chunk IndexedStream::write(Tag t) const
    {
        ByteWriter raw;
        for (uint16_t i : indices)
            wide ? raw.u16(i) : raw.u8(static_cast<uint8_t>(i));
        raw.padTo(4);
        Bytes joined;
        for (const Bytes &e : table)
            joined.insert(joined.end(), e.begin(), e.end());
        return Chunk::node(t, {Chunk::leaf(tag("TIDA"), std::move(joined)), Chunk::leaf(wide ? tag("TII6") : tag("TII8"), raw.take())});
    }

    Bytes IndexedStream::expand() const
    {
        Bytes out;
        for (uint16_t i : indices)
        {
            if (i >= table.size())
                throw LevelError("an index past its stream's table");
            out.insert(out.end(), table[i].begin(), table[i].end());
        }
        return out;
    }

    IndexedStream IndexedStream::make(const std::vector<Bytes> &elements)
    {
        IndexedStream s;
        std::map<Bytes, uint16_t> where;
        for (const Bytes &e : elements)
        {
            auto it = where.find(e);
            if (it == where.end())
            {
                it = where.emplace(e, static_cast<uint16_t>(s.table.size())).first;
                s.table.push_back(e);
            }
            s.indices.push_back(it->second);
        }
        s.wide = s.table.size() > 256;
        return s;
    }

    Stream Stream::ofPlain(Bytes bytes)
    {
        Stream s;
        s.plain = std::move(bytes);
        return s;
    }

    Stream Stream::ofIndexed(IndexedStream table)
    {
        Stream s;
        s.indexed = true;
        s.table = std::move(table);
        return s;
    }

    // ---- parts ------------------------------------------------------------------------

    Part Part::read(const Chunk &chunk, Engine engine)
    {
        const Chunk *head = chunk.find(tag("TGPH"));
        if (!head || head->data.size() != 0x14)
            throw LevelError("TGPT without a 20-byte TGPH");
        const Bytes &h = head->data;
        Part p;
        p.flags = readU16(h, 0);
        const uint16_t vertexCount = readU16(h, 4), faceCount = readU16(h, 6), groupCount = readU16(h, 8);
        p.reserved = readU8(h, 10);
        p.boneCount = readU8(h, 12);
        p.materialCount = readU8(h, 14);
        p.id = readU32(h, 16);
        if (readU16(h, 2) || readU8(h, 11) || readU8(h, 13) || readU8(h, 15))
            throw LevelError("TGPH with its padding set");
        for (const Chunk &k : chunk.kids)
        {
            p.order.push_back(k.tag);
            if (k.tag == tag("TGPH"))
                continue;
            if (k.tag == tag("TGVP"))
            {
                if (k.data.size() != 12u * vertexCount)
                    throw LevelError("TGVP of the wrong size for its vertices");
                p.positions.clear();
                for (size_t i = 0; i < vertexCount; ++i)
                    p.positions.push_back(readFloats<3>(k.data, 12 * i));
            }
            else if (k.tag == tag("TGFP"))
            {
                if (k.data.size() != 8u * faceCount)
                    throw LevelError("TGFP of the wrong size for its faces");
                p.faces.clear();
                for (size_t i = 0; i < faceCount; ++i)
                {
                    Face f;
                    f.flags = readU16(k.data, 8 * i);
                    f.vertices = {readU16(k.data, 8 * i + 2), readU16(k.data, 8 * i + 4), readU16(k.data, 8 * i + 6)};
                    p.faces.push_back(f);
                }
            }
            else if (k.tag == tag("TGFM"))
            {
                p.groups = StripGroup::readAll(k.data);
                if (p.groups.size() != groupCount)
                    throw LevelError("TGPH counts other strip groups than its TGFM has");
            }
            else if (k.tag == tag("TGFE"))
                p.edgeTags = TagBuffer::read(k.data, edgeRecordSize(engine));
            else if (k.tag == tag("TGFT"))
                p.faceTags = TagBuffer::read(k.data, faceRecordSize(engine));
            else
            {
                bool typed = false;
                for (size_t s = 0; s < 4 && !typed; ++s)
                {
                    const size_t count = kStreams[s].perFace ? faceCount : vertexCount;
                    if (k.tag == kStreams[s].plain)
                    {
                        if (k.data.size() != kStreams[s].stride * count)
                            throw LevelError(tagText(k.tag) + " of the wrong size for its elements");
                        p.streams[s] = Stream::ofPlain(k.data);
                        typed = true;
                    }
                    else if (k.tag == kStreams[s].indexed)
                    {
                        p.streams[s] = Stream::ofIndexed(IndexedStream::fromChunk(k, kStreams[s].stride, count));
                        typed = true;
                    }
                }
                if (!typed)
                {
                    if (engine == Engine::Zge4 && k.tag == tag("TGFV"))
                        p.cornerTags = TagBuffer::read(k.data, kCornerRecordSize);
                    else
                        p.other.push_back(k);
                }
            }
        }
        return p;
    }

    Chunk Part::write(Engine engine) const
    {
        if (engine == Engine::Zge2)
        {
            for (size_t s = 0; s < 4; ++s)
                if (streams[s] && streams[s]->indexed)
                    throw LevelError("part " + std::to_string(id) + ": DM2 reads no indexed streams");
            for (const StripGroup &g : groups)
                for (uint16_t e : g.entries)
                    if (e & StripGroup::kFlipped)
                        throw LevelError("part " + std::to_string(id) + ": a strip entry with 0x4000 (DM2's strips have no winding flag)");
        }
        // The chunks by tag, in the order ai_level_write.py's chunks() makes them.
        std::vector<std::pair<Tag, Chunk>> chunks;
        auto put = [&chunks](Chunk c)
        {
            for (auto &[t, existing] : chunks)
                if (t == c.tag)
                {
                    existing = std::move(c);
                    return;
                }
            const Tag t = c.tag;
            chunks.emplace_back(t, std::move(c));
        };
        {
            ByteWriter head;
            head.u16(flags).u16(0).u16(static_cast<uint16_t>(positions.size())).u16(static_cast<uint16_t>(faces.size()))
                .u16(static_cast<uint16_t>(groups.size()));
            head.u8(reserved).u8(0).u8(boneCount).u8(0).u8(materialCount).u8(0).u32(id);
            put(Chunk::leaf(tag("TGPH"), head.take()));
        }
        {
            ByteWriter w;
            for (const Vec3 &p : positions)
                w.f32s(p);
            put(Chunk::leaf(tag("TGVP"), w.take()));
        }
        {
            ByteWriter w;
            for (const Face &f : faces)
                w.u16(f.flags).u16(f.vertices[0]).u16(f.vertices[1]).u16(f.vertices[2]);
            put(Chunk::leaf(tag("TGFP"), w.take()));
        }
        put(Chunk::leaf(tag("TGFM"), StripGroup::writeAll(groups)));
        for (size_t s = 0; s < 4; ++s)
            if (streams[s])
                put(streams[s]->indexed ? streams[s]->table.write(kStreams[s].indexed) : Chunk::leaf(kStreams[s].plain, streams[s]->plain));
        if (edgeTags)
            put(Chunk::leaf(tag("TGFE"), edgeTags->write()));
        if (faceTags)
            put(Chunk::leaf(tag("TGFT"), faceTags->write()));
        for (const Chunk &k : other)
            put(k);
        if (engine == Engine::Zge4 && cornerTags)
            put(Chunk::leaf(tag("TGFV"), cornerTags->write()));
        auto has = [&chunks](const Tag &t)
        { return std::any_of(chunks.begin(), chunks.end(), [&t](const auto &c) { return c.first == t; }); };
        std::vector<Tag> sequence;
        for (const Tag &t : order.empty() ? defaultOrder(engine) : order)
            if (has(t))
                sequence.push_back(t);
        for (const auto &[t, c] : chunks)
            if (std::find(sequence.begin(), sequence.end(), t) == sequence.end())
                sequence.push_back(t);
        std::vector<Chunk> kids;
        for (const Tag &t : sequence)
            for (const auto &[ct, c] : chunks)
                if (ct == t)
                {
                    kids.push_back(c);
                    break;
                }
        return Chunk::node(tag("TGPT"), std::move(kids));
    }

    void Part::unwind()
    {
        for (StripGroup &g : groups)
            for (uint16_t &e : g.entries)
                e = static_cast<uint16_t>(e & ~StripGroup::kFlipped);
    }

    LevelOfDetail LevelOfDetail::read(const Bytes &data)
    {
        if (data.size() != 12)
            throw LevelError("TGLO of " + std::to_string(data.size()) + " bytes");
        LevelOfDetail lod;
        lod.distance = readF32(data, 0);
        lod.word04 = readU32(data, 4);
        lod.partId = readU32(data, 8);
        return lod;
    }

    Bytes LevelOfDetail::write() const
    {
        ByteWriter w;
        w.f32(distance).u32(word04).u32(partId);
        return w.take();
    }

    GeoHeader GeoHeader::read(const Bytes &data)
    {
        if (data.size() != 0x40)
            throw LevelError("TGHD of " + std::to_string(data.size()) + " bytes");
        GeoHeader h;
        h.boundsMin = readFloats<3>(data, 0);
        h.boundsMax = readFloats<3>(data, 0xC);
        h.id = readU32(data, 0x18);
        h.name = cString(data, 0x1C, 32);
        h.nameHash = readU32(data, 0x3C);
        return h;
    }

    Bytes GeoHeader::write() const
    {
        ByteWriter w;
        w.f32s(boundsMin).f32s(boundsMax).u32(id).text(name, 32).u32(nameHash);
        return w.take();
    }

    GeoHeader GeoHeader::make(const Vec3 &boundsMin, const Vec3 &boundsMax, uint32_t id, const std::string &name,
                              std::optional<uint32_t> nameHash)
    {
        GeoHeader h;
        h.boundsMin = boundsMin;
        h.boundsMax = boundsMax;
        h.id = id;
        h.name = nameHash ? name : name.substr(0, kNameChars);
        h.nameHash = nameHash ? *nameHash : levels::nameHash(name);
        return h;
    }

    const uint32_t kDynamicsObjectClass = nameHash("DynamicsObject");

    // ---- objects ----------------------------------------------------------------------

    GeoObject GeoObject::read(const Chunk &chunk, Engine engine)
    {
        const Chunk *head = chunk.find(tag("TGHD"));
        if (!head)
            throw LevelError("TGEO without TGHD");
        GeoObject o;
        o.header = GeoHeader::read(head->data);
        const std::vector<Chunk> &kids = chunk.kids;
        for (size_t i = 0; i < kids.size(); ++i)
        {
            const Chunk &k = kids[i];
            if (k.tag == tag("TGHD"))
                continue;
            if (k.tag == tag("TGTD"))
                o.typeData = k.data;
            else if (k.tag == tag("TGFR"))
                o.frame = k.data;
            else if (k.tag == tag("TGCI"))
                o.collision = CollisionTree::read(k.data, engine);
            else if (k.tag == tag("TGPT"))
            {
                ObjectPart part{Part::read(k, engine), std::nullopt, std::nullopt};
                while (i + 1 < kids.size() && (kids[i + 1].tag == tag("TMTB") || kids[i + 1].tag == tag("TGLO")))
                {
                    ++i;
                    if (kids[i].tag == tag("TMTB"))
                        part.materials = kids[i];
                    else
                        part.lod = LevelOfDetail::read(kids[i].data);
                }
                o.parts.push_back(std::move(part));
            }
            else
                throw LevelError("a " + tagText(k.tag) + " chunk in a TGEO");
        }
        return o;
    }

    Chunk GeoObject::write(Engine engine) const
    {
        std::vector<Chunk> kids;
        kids.push_back(Chunk::leaf(tag("TGHD"), header.write()));
        if (typeData)
            kids.push_back(Chunk::leaf(tag("TGTD"), *typeData));
        kids.push_back(Chunk::leaf(tag("TGFR"), frame));
        for (const ObjectPart &p : parts)
        {
            kids.push_back(p.part.write(engine));
            if (p.lod)
                kids.push_back(Chunk::leaf(tag("TGLO"), p.lod->write()));
            if (p.materials)
                kids.push_back(*p.materials);
        }
        if (collision)
            kids.push_back(Chunk::leaf(tag("TGCI"), collision->write(engine)));
        return Chunk::node(tag("TGEO"), std::move(kids));
    }

    const ObjectPart *GeoObject::collisionPart() const
    {
        if (collision)
            for (const ObjectPart &p : parts)
                if (p.part.id == collision->partId)
                    return &p;
        return parts.empty() ? nullptr : &parts.back();
    }

    Geometry Geometry::read(const Bytes &data, Engine engine)
    {
        const std::vector<Chunk> tree = parseChunks(data);
        if (tree.size() != 1 || tree[0].tag != tag("FORM"))
            throw LevelError("a GEO is one FORM");
        Geometry g;
        for (size_t i = 0; i < tree[0].kids.size(); ++i)
        {
            const Chunk &k = tree[0].kids[i];
            if (k.tag == tag("TGEO"))
                g.objects.push_back(GeoObject::read(k, engine));
            else
                g.other.emplace_back(i, k);
        }
        return g;
    }

    Bytes Geometry::write(Engine engine) const
    {
        std::vector<Chunk> kids;
        for (const GeoObject &o : objects)
            kids.push_back(o.write(engine));
        std::vector<std::pair<size_t, Chunk>> sorted = other;
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[i, k] : sorted)
            kids.insert(kids.begin() + static_cast<ptrdiff_t>(std::min(i, kids.size())), k);
        return serializeChunks({Chunk::node(tag("FORM"), std::move(kids))});
    }
}
