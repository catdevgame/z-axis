#include "xbox_models.h"

#include "geo_file.h"
#include "strips.h"
#include "texture_library.h"

#include "zge/render/ps2/packet_size.h"

#include "runtime/zaxis_xbox.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>
#include <unordered_set>

namespace zge::native
{
    namespace
    {
        uint32_t u32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                                  static_cast<uint8_t>(v >> 24)};
            out.insert(out.end(), b, b + 4);
        }
        bool tagIs(const uint8_t *p, const char *tag) { return std::memcmp(p, tag, 4) == 0; }

        struct Span
        {
            size_t at = 0, size = 0; // header offset, body size
        };

        // The MODL's chunks of a model file; false if it is not FMDL > MODL.
        bool modelChunks(const std::vector<uint8_t> &d, std::vector<Span> &out)
        {
            if (d.size() < 16 || !tagIs(&d[0], "FMDL") || !tagIs(&d[8], "MODL"))
                return false;
            const size_t end = 16 + u32(&d[12]);
            if (end > d.size())
                return false;
            for (size_t p = 16; p + 8 <= end;)
            {
                const size_t size = u32(&d[p + 4]);
                if (p + 8 + size > end)
                    return false;
                out.push_back({p, size});
                p += 8 + size;
            }
            return true;
        }

        // The one TGEO among a model's chunks (index), or -1.
        int onlyObject(const std::vector<uint8_t> &d, const std::vector<Span> &chunks)
        {
            int found = -1;
            for (size_t i = 0; i < chunks.size(); ++i)
                if (tagIs(&d[chunks[i].at], "TGEO"))
                {
                    if (found >= 0)
                        return -1;
                    found = static_cast<int>(i);
                }
            return found;
        }

        // Every MATI layer's texture id in a converted GEO (FORM > TGEO >
        // TMTB > MATI: the layer count at +0x2C, layers of 28 bytes from
        // +0x30, the texture id at +0x10), through `visit`.
        template <typename Visit>
        void forEachLayerTexture(std::vector<uint8_t> &geo, Visit visit)
        {
            std::vector<std::pair<size_t, size_t>> stack{{8, geo.size()}}; // past the FORM header
            while (!stack.empty())
            {
                auto [p, end] = stack.back();
                stack.pop_back();
                while (p + 8 <= end)
                {
                    const size_t size = u32(&geo[p + 4]);
                    const size_t body = p + 8;
                    if (body + size > end)
                        break;
                    if (tagIs(&geo[p], "TGEO") || tagIs(&geo[p], "TMTB"))
                        stack.push_back({body, body + size});
                    else if (tagIs(&geo[p], "MATI") && size >= 0x30)
                    {
                        const uint32_t layers = u32(&geo[body + 0x2C]);
                        for (uint32_t i = 0; i < layers && body + 0x30 + (i + 1) * 28 <= body + size; ++i)
                            visit(&geo[body + 0x30 + i * 28 + 0x10]);
                    }
                    p = body + size;
                }
            }
        }
    }

    namespace
    {
        void put16(std::vector<uint8_t> &out, uint16_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

        struct StripStats
        {
            uint32_t entriesBefore = 0, entriesAfter = 0, triangles = 0;
            bool sameTriangles = true;
        };

        // A TGFM chunk's body with each submesh's triangles stripped again
        // (strips.h); its group count through `groups`.
        std::vector<uint8_t> tightenGroups(const uint8_t *body, size_t size, uint32_t &groups, StripStats &stats)
        {
            struct Submesh
            {
                uint16_t header = 0; // submesh << 8 | mode
                std::vector<strips::Triangle> triangles;
            };
            std::vector<Submesh> submeshes;
            for (size_t at = 0; at + 4 <= size;)
            {
                const uint16_t header = u16(body + at), count = u16(body + at + 2);
                std::vector<uint16_t> entries(count);
                for (uint16_t i = 0; i < count && at + 4 + i * 2u + 2 <= size; ++i)
                    entries[i] = u16(body + at + 4 + i * 2u);
                stats.entriesBefore += count;
                auto it = std::find_if(submeshes.begin(), submeshes.end(),
                                       [&](const Submesh &m) { return (m.header >> 8) == (header >> 8); });
                if (it == submeshes.end())
                    it = submeshes.insert(submeshes.end(), Submesh{header, {}});
                strips::trianglesOf(entries, it->triangles);
                at += (count * 2u + 7u) & ~size_t(3);
            }
            std::vector<uint8_t> out;
            groups = 0;
            for (const Submesh &m : submeshes)
            {
                stats.triangles += static_cast<uint32_t>(m.triangles.size());
                std::vector<strips::Triangle> drawn;
                for (const std::vector<uint16_t> &group : strips::cutGroups(strips::makeStrips(m.triangles)))
                {
                    put16(out, m.header);
                    put16(out, static_cast<uint16_t>(group.size()));
                    for (uint16_t e : group)
                        put16(out, e);
                    if (group.size() & 1u)
                        put16(out, 0u);
                    strips::trianglesOf(group, drawn);
                    stats.entriesAfter += static_cast<uint32_t>(group.size());
                    ++groups;
                }
                // The same triangles, each in its winding.
                std::vector<strips::Triangle> a = m.triangles, b = drawn;
                for (auto &t : a)
                    t = strips::canonical(t);
                for (auto &t : b)
                    t = strips::canonical(t);
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                stats.sameTriangles = stats.sameTriangles && a == b;
            }
            return out;
        }

        // The chunks of a converted object (FORM > TGEO > TGPT) with every
        // part's TGFM stripped again and its TGPH group count set.
        std::vector<uint8_t> tightenChunks(const uint8_t *d, size_t size, StripStats &stats)
        {
            std::vector<uint8_t> out;
            size_t tgph = SIZE_MAX;
            uint32_t groups = 0;
            bool restripped = false;
            for (size_t p = 0; p + 8 <= size;)
            {
                const size_t n = u32(d + p + 4);
                if (p + 8 + n > size)
                    break;
                std::vector<uint8_t> body;
                if (tagIs(d + p, "FORM") || tagIs(d + p, "TGEO") || tagIs(d + p, "TGPT"))
                    body = tightenChunks(d + p + 8, n, stats);
                else if (tagIs(d + p, "TGFM"))
                {
                    body = tightenGroups(d + p + 8, n, groups, stats);
                    restripped = true;
                }
                else
                    body.assign(d + p + 8, d + p + 8 + n);
                if (tagIs(d + p, "TGPH"))
                    tgph = out.size() + 8;
                out.insert(out.end(), d + p, d + p + 4);
                put32(out, static_cast<uint32_t>(body.size()));
                out.insert(out.end(), body.begin(), body.end());
                p += 8 + n;
            }
            if (restripped && tgph != SIZE_MAX && tgph + 10 <= out.size())
            {
                out[tgph + 8] = static_cast<uint8_t>(groups);
                out[tgph + 9] = static_cast<uint8_t>(groups >> 8);
            }
            return out;
        }

        // The largest part's packet bytes and strip entries of a converted
        // object (packet_size.h), or false if a material's packet is more
        // than one DMA tag sends.
        bool packetSizes(const std::vector<uint8_t> &geo, uint32_t &largest, uint32_t &entries, std::string &why)
        {
            const auto objects = readGeoFile(geo);
            if (objects.size() != 1)
            {
                why = "the converted object does not read back";
                return false;
            }
            largest = entries = 0;
            for (size_t k = 0; k < objects[0].parts.size(); ++k)
            {
                const GeoFilePart &p = objects[0].parts[k];
                const bool skinned = !p.skinWeights.empty() || p.boneCount > 0; // a skinned render type
                const uint32_t perEntry = packetBytesPerEntry(skinned ? 0x08u : 0x04u);
                PacketSize whole(perEntry, 0);
                std::map<uint8_t, PacketSize> byMaterial;
                for (const GeoGroup &g : p.groups)
                {
                    whole.group(g.mode, g.material, static_cast<uint32_t>(g.entries.size()));
                    byMaterial.try_emplace(g.material, perEntry, 0)
                        .first->second.group(g.mode, g.material, static_cast<uint32_t>(g.entries.size()));
                }
                for (const auto &[material, size] : byMaterial)
                    if (size.bytes() / 16u > 0xFFFFu)
                    {
                        why = "part " + std::to_string(k) + " material " + std::to_string(material) +
                              ": its packet (" + std::to_string(size.bytes() / 1024u) +
                              " KB) is more than one DMA tag can send";
                        return false;
                    }
                largest = std::max(largest, whole.bytes());
                entries = std::max(entries, whole.entries());
            }
            return true;
        }
    }

    bool makeXboxModel(const std::string &relative, const std::vector<uint8_t> &ps2, const std::vector<uint8_t> &xbox,
                       std::vector<uint8_t> &out, XboxModelReport &report)
    {
        auto fail = [&](const std::string &why)
        {
            report.why = why;
            return false;
        };
        std::vector<Span> ps2Chunks, xboxChunks;
        if (!modelChunks(ps2, ps2Chunks) || !modelChunks(xbox, xboxChunks))
            return fail("not a model (FMDL > MODL)");
        const int ps2Object = onlyObject(ps2, ps2Chunks), xboxObject = onlyObject(xbox, xboxChunks);
        if (ps2Object < 0 || xboxObject < 0)
            return fail("not one object (TGEO) in each");

        // The same object: its parts and their bones.
        const auto ps2Geo = readGeoFile(ps2), xboxGeo = readGeoFile(xbox);
        if (ps2Geo.size() != 1 || xboxGeo.size() != 1 || ps2Geo[0].parts.size() != xboxGeo[0].parts.size())
            return fail("objects or parts differ");
        for (size_t k = 0; k < ps2Geo[0].parts.size(); ++k)
        {
            const GeoFilePart &a = ps2Geo[0].parts[k], &b = xboxGeo[0].parts[k];
            if (a.boneCount != b.boneCount || (a.flags & 0x0800) != (b.flags & 0x0800) ||
                a.materialCount != b.materialCount)
                return fail("part " + std::to_string(k) + ": bones, skinning or material count differ");
            report.ps2Vertices += a.vertexCount;
            report.xboxVertices += b.vertexCount;
        }

        // Xbox texture ids to the PS2 library's, by the pack's pairs.
        const auto ps2Textures = readTextureLibrary(ps2), xboxTextures = readTextureLibrary(xbox);
        std::map<uint32_t, uint32_t> toPs2;
        std::unordered_set<uint32_t> alpha; // Xbox ids whose PS2 texture has alpha (TZMP)
        std::vector<Rejected> rejected;
        for (const TexturePair &pair : pairTextures(relative, ps2Textures, xboxTextures, &rejected))
        {
            toPs2[pair.xbox->id] = pair.ps2->id;
            if (pair.ps2->kind == "TZMP")
                alpha.insert(pair.xbox->id);
        }
        // A PS2 texture with the same id as an Xbox one stands for itself
        // (textures both releases left alone).
        for (const LibraryTexture &t : ps2Textures)
            for (const LibraryTexture &x : xboxTextures)
                if (t.id == x.id && !toPs2.count(x.id))
                {
                    toPs2[x.id] = t.id;
                    if (t.kind == "TZMP")
                        alpha.insert(x.id);
                }
        // A pair whose Xbox picture the pack does not use (its colour key
        // drawn opaque, pictures that look different): the same texture by
        // name, so the material takes the PS2 texture and its PS2 picture.
        for (const Rejected &r : rejected)
            for (const LibraryTexture &t : ps2Textures)
                for (const LibraryTexture &x : xboxTextures)
                    if (t.name == r.ps2 && x.name == r.xbox && !toPs2.count(x.id))
                    {
                        toPs2[x.id] = t.id;
                        if (t.kind == "TZMP")
                            alpha.insert(x.id);
                        report.ps2Pictures.push_back(t.name + " (" + r.why + ")");
                    }

        // The Xbox TGEO in the PS2's form.
        const Span &xo = xboxChunks[static_cast<size_t>(xboxObject)];
        std::vector<uint8_t> geo;
        geo.insert(geo.end(), {'F', 'O', 'R', 'M'});
        put32(geo, static_cast<uint32_t>(8 + xo.size));
        geo.insert(geo.end(), xbox.begin() + static_cast<std::ptrdiff_t>(xo.at),
                   xbox.begin() + static_cast<std::ptrdiff_t>(xo.at + 8 + xo.size));
        zx::zaxis::xbox::GeometryReport converted;
        if (!zx::zaxis::xbox::convertGeometry(geo, &converted, &alpha))
            return fail("the Xbox object did not convert");
        // The VIF packet the game builds for each part (zge/render/packet.h):
        // the readable packet builder (buildPartPacket) makes its scratch buffer
        // as large as the packet, so only the packet format's own limits
        // apply: a material's DMA tag counts at most 0xFFFF quadwords, and
        // the builders count a part's strip entries in 13 bits (only kept,
        // never read back: reported, not refused).
        std::string why;
        if (!packetSizes(geo, report.packetBefore, report.entriesBefore, why))
            return fail(why);
        // The strips again from the triangles (strips.h): the converted
        // strips keep every degenerate triangle of the Xbox's D3D strips as
        // an entry that draws nothing.
        {
            StripStats stats;
            std::vector<uint8_t> tight = tightenChunks(geo.data(), geo.size(), stats);
            if (!stats.sameTriangles)
                return fail("the strips made again do not draw the same triangles");
            geo = std::move(tight);
            report.triangles = stats.triangles;
        }
        if (!packetSizes(geo, report.largestPacket, report.largestEntries, why))
            return fail(why);
        uint32_t ps2Entries = 0;
        if (!packetSizes(ps2, report.ps2Packet, ps2Entries, why))
            report.ps2Packet = 0;

        std::string missing;
        forEachLayerTexture(geo, [&](uint8_t *id)
                            {
                                ++report.materials;
                                const uint32_t x = u32(id);
                                if (x == 0)
                                    return; // no texture
                                auto it = toPs2.find(x);
                                if (it == toPs2.end())
                                {
                                    if (missing.empty())
                                        missing = std::to_string(x);
                                    return;
                                }
                                if (it->second != x)
                                    ++report.remapped;
                                std::memcpy(id, &it->second, 4);
                            });
        if (!missing.empty())
        {
            std::string why = "a material names Xbox texture " + missing + ", which has no PS2 pair";
            for (const Rejected &r : rejected)
                why += "; " + r.ps2 + " / " + r.xbox + ": " + r.why;
            return fail(why);
        }

        // The PS2 file with the converted object in place of its own.
        std::vector<uint8_t> body;
        for (size_t i = 0; i < ps2Chunks.size(); ++i)
        {
            if (static_cast<int>(i) == ps2Object)
                body.insert(body.end(), geo.begin() + 8, geo.end()); // the TGEO chunk, header included
            else
                body.insert(body.end(), ps2.begin() + static_cast<std::ptrdiff_t>(ps2Chunks[i].at),
                            ps2.begin() + static_cast<std::ptrdiff_t>(ps2Chunks[i].at + 8 + ps2Chunks[i].size));
        }
        out.clear();
        out.insert(out.end(), {'F', 'M', 'D', 'L'});
        put32(out, static_cast<uint32_t>(8 + body.size()));
        out.insert(out.end(), {'M', 'O', 'D', 'L'});
        put32(out, static_cast<uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        // Whatever followed the FMDL in the PS2 file.
        const size_t fmdlEnd = 8 + u32(&ps2[4]);
        if (fmdlEnd < ps2.size())
            out.insert(out.end(), ps2.begin() + static_cast<std::ptrdiff_t>(fmdlEnd), ps2.end());
        return true;
    }
}
