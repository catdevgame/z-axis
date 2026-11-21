#include "geo_file.h"

#include "ee/ee.h"
#include "zge/render/fixed_point.h"

#include <cstring>

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
        uint16_t u16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        bool tagIs(const uint8_t *p, const char *tag) { return std::memcmp(p, tag, 4) == 0; }

        struct Chunk
        {
            const uint8_t *tag;
            size_t body, size;
        };

        // The chunks in [begin, end): false if one runs past the end.
        bool chunks(const std::vector<uint8_t> &d, size_t begin, size_t end, std::vector<Chunk> &out)
        {
            for (size_t p = begin; p + 8 <= end;)
            {
                const uint32_t size = u32(&d[p + 4]);
                if (p + 8 + size > end)
                    return false;
                out.push_back({&d[p], p + 8, size});
                p += 8 + size;
            }
            return true;
        }

        // The loader's float -> s16 (ITOF12 back): multiply in EE floats,
        // CVT.W.S truncating (zge/assets/geo.cpp uvFixed).
        int16_t uvFixed(float value) { return static_cast<int16_t>(fixedUv(ee::Float(value))); }

        // An indexed stream's pieces (TIDA table, TII8 / TII6 indices).
        struct Indexed
        {
            const uint8_t *table = nullptr;
            size_t tableBytes = 0;
            std::vector<uint32_t> indices;
        };

        bool readIndexed(const std::vector<uint8_t> &d, const Chunk &c, Indexed &out)
        {
            std::vector<Chunk> children;
            if (!chunks(d, c.body, c.body + c.size, children))
                return false;
            for (const Chunk &k : children)
            {
                if (tagIs(k.tag, "TIDA"))
                {
                    out.table = &d[k.body];
                    out.tableBytes = k.size;
                }
                else if (tagIs(k.tag, "TII8"))
                    for (size_t i = 0; i < k.size; ++i)
                        out.indices.push_back(d[k.body + i]);
                else if (tagIs(k.tag, "TII6"))
                    for (size_t i = 0; i + 2 <= k.size; i += 2)
                        out.indices.push_back(u16(&d[k.body + i]));
            }
            return out.table != nullptr;
        }

        bool readPart(const std::vector<uint8_t> &d, const Chunk &tgpt, size_t chunkOffset, GeoFilePart &part,
                      std::string &why)
        {
            part.chunkOffset = static_cast<uint32_t>(chunkOffset);
            std::vector<Chunk> children;
            if (!chunks(d, tgpt.body, tgpt.body + tgpt.size, children))
            {
                why = "TGPT chunks do not add up";
                return false;
            }
            bool header = false;
            for (const Chunk &c : children)
            {
                const uint8_t *b = &d[c.body];
                if (tagIs(c.tag, "TGPH") && c.size >= 20)
                {
                    header = true;
                    part.flags = u16(b);
                    part.vertexCount = u16(b + 4);
                    part.faceCount = u16(b + 6);
                    part.groupCount = u16(b + 8);
                    part.boneCount = b[0x0C];
                    part.materialCount = b[0x0E];
                    part.partId = u32(b + 0x10);
                }
                if (!header)
                    continue;
                const size_t n = part.vertexCount;
                if (tagIs(c.tag, "TGVP") && c.size >= n * 12u)
                {
                    part.positions.resize(n * 3u);
                    std::memcpy(part.positions.data(), b, n * 12u);
                }
                else if (tagIs(c.tag, "TGVU") && c.size >= n * 8u)
                {
                    part.uvs.resize(n * 2u);
                    part.uvFloats.resize(n * 2u);
                    for (size_t i = 0; i < n * 2u; ++i)
                    {
                        float f;
                        std::memcpy(&f, b + i * 4u, 4);
                        part.uvs[i] = uvFixed(f);
                        part.uvFloats[i] = f;
                    }
                    part.hasUvs = true;
                }
                else if (tagIs(c.tag, "TGVC") && c.size >= n * 4u)
                {
                    part.colours.assign(b, b + n * 4u);
                    part.hasColours = true;
                }
                else if (tagIs(c.tag, "TGVI") && c.size >= n * 8u)
                    part.skinWeights.assign(b, b + n * 8u);
                else if (tagIs(c.tag, "TGVN") && n && c.size >= n)
                {
                    part.normalStride = static_cast<uint32_t>(c.size / n);
                    part.normalBytes.assign(b, b + n * part.normalStride);
                    part.hasNormals = true;
                }
                else if (tagIs(c.tag, "TIVU") || tagIs(c.tag, "TIVC") || tagIs(c.tag, "TIVN"))
                {
                    Indexed ix;
                    if (!readIndexed(d, c, ix) || ix.indices.size() < n)
                    {
                        why = "indexed stream incomplete";
                        return false;
                    }
                    if (tagIs(c.tag, "TIVU"))
                    {
                        part.uvs.resize(n * 2u);
                        part.uvFloats.resize(n * 2u);
                        for (size_t v = 0; v < n; ++v)
                            for (size_t k = 0; k < 2; ++k)
                            {
                                const size_t at = (static_cast<size_t>(ix.indices[v]) * 2u + k) * 4u;
                                if (at + 4 > ix.tableBytes)
                                {
                                    why = "UV index past its table";
                                    return false;
                                }
                                float f;
                                std::memcpy(&f, ix.table + at, 4);
                                part.uvs[v * 2u + k] = uvFixed(f);
                                part.uvFloats[v * 2u + k] = f;
                            }
                        part.hasUvs = true;
                    }
                    else if (tagIs(c.tag, "TIVC"))
                    {
                        part.colours.resize(n * 4u);
                        for (size_t v = 0; v < n; ++v)
                        {
                            const size_t at = static_cast<size_t>(ix.indices[v]) * 4u;
                            if (at + 4 > ix.tableBytes)
                            {
                                why = "colour index past its table";
                                return false;
                            }
                            std::memcpy(&part.colours[v * 4u], ix.table + at, 4);
                        }
                        part.hasColours = true;
                    }
                    else
                    {
                        // The table's element size: the table over its
                        // highest index + 1.
                        uint32_t top = 0;
                        for (size_t v = 0; v < n; ++v)
                            top = std::max(top, ix.indices[v]);
                        const uint32_t stride = static_cast<uint32_t>(ix.tableBytes / (top + 1u));
                        part.normalStride = stride;
                        part.normalBytes.resize(n * stride);
                        for (size_t v = 0; v < n; ++v)
                            std::memcpy(&part.normalBytes[v * stride], ix.table + ix.indices[v] * stride, stride);
                        part.hasNormals = true;
                    }
                }
                else if (tagIs(c.tag, "TGFP") && c.size >= part.faceCount * 8u)
                    part.faces.assign(b, b + part.faceCount * 8u);
                else if (tagIs(c.tag, "TGFN") && c.size >= part.faceCount * 12u)
                {
                    part.faceNormals.resize(part.faceCount * 3u);
                    std::memcpy(part.faceNormals.data(), b, part.faceCount * 12u);
                    part.hasFaceNormals = true;
                }
                else if (tagIs(c.tag, "TIFN"))
                {
                    Indexed ix;
                    if (!readIndexed(d, c, ix) || ix.indices.size() < part.faceCount)
                    {
                        why = "indexed face normals incomplete";
                        return false;
                    }
                    part.faceNormals.resize(part.faceCount * 3u);
                    for (size_t f = 0; f < part.faceCount; ++f)
                    {
                        const size_t at = static_cast<size_t>(ix.indices[f]) * 12u;
                        if (at + 12 > ix.tableBytes)
                        {
                            why = "face normal index past its table";
                            return false;
                        }
                        std::memcpy(&part.faceNormals[f * 3u], ix.table + at, 12);
                    }
                    part.hasFaceNormals = true;
                }
                else if ((tagIs(c.tag, "TGFT") || tagIs(c.tag, "TGFE")) && c.size >= 8)
                {
                    // A buffer stream: an 8-byte descriptor, then the body
                    // the game keeps (zge/assets/geo.cpp loadBufferStreamChunk).
                    std::vector<uint8_t> body(b + 8, b + c.size);
                    if (tagIs(c.tag, "TGFT"))
                    {
                        part.faceTags = std::move(body);
                        part.hasFaceTags = true;
                        part.faceTagsDescriptor = u32(b);
                        part.faceTagsCount = u32(b + 4);
                    }
                    else
                    {
                        part.faceEdges = std::move(body);
                        part.hasFaceEdges = true;
                        part.faceEdgesDescriptor = u32(b);
                        part.faceEdgesCount = u32(b + 4);
                    }
                }
                else if (tagIs(c.tag, "TGFM"))
                {
                    for (size_t at = 0; at + 4 <= c.size;)
                    {
                        GeoGroup g;
                        g.mode = static_cast<uint8_t>(b[at] & 3u);
                        g.material = b[at + 1];
                        const uint16_t count = u16(b + at + 2);
                        if (at + 4 + count * 2u > c.size)
                        {
                            why = "TGFM group past its chunk";
                            return false;
                        }
                        for (uint16_t i = 0; i < count; ++i)
                            g.entries.push_back(u16(b + at + 4 + i * 2u));
                        part.groups.push_back(std::move(g));
                        at += (count * 2u + 7u) & ~size_t(3); // header and entries, padded to 4
                    }
                }
            }
            if (!header)
            {
                why = "TGPT without TGPH";
                return false;
            }
            return true;
        }

        // A TGEO's body: its header, parts and materials.
        GeoFileObject readObject(const std::vector<uint8_t> &d, size_t begin, size_t end, size_t chunkOffset,
                                 std::string &why)
        {
            GeoFileObject object;
            object.chunkOffset = static_cast<uint32_t>(chunkOffset);
            std::vector<Chunk> children;
            if (!chunks(d, begin, end, children) && why.empty())
                why = "TGEO chunks do not add up";
            auto collision = std::make_shared<GeoCollisionChunks>();
            for (const Chunk &k : children)
            {
                if (tagIs(k.tag, "TGFR") && k.size >= 4)
                {
                    collision->hasFrame = true;
                    std::memcpy(collision->frame, &d[k.body], 4);
                }
                else if (tagIs(k.tag, "TGCI") && k.size >= 8)
                {
                    // {u32 part id, u32 bytes, the tree} (loadCollisionTreeChunk).
                    const uint32_t bytes = u32(&d[k.body + 4]);
                    if (8u + static_cast<size_t>(bytes) <= k.size)
                    {
                        collision->hasTree = true;
                        collision->treePartId = u32(&d[k.body]);
                        collision->tree.assign(&d[k.body + 8], &d[k.body + 8] + bytes);
                    }
                    else if (why.empty())
                        why = "TGCI tree past its chunk";
                }
                if (tagIs(k.tag, "TGHD") && k.size >= 0x3C)
                    object.name.assign(reinterpret_cast<const char *>(&d[k.body + 0x1C]),
                                       strnlen(reinterpret_cast<const char *>(&d[k.body + 0x1C]), 32));
                else if (tagIs(k.tag, "TGPT"))
                {
                    GeoFilePart part;
                    std::string partWhy;
                    if (readPart(d, k, k.body - 8, part, partWhy))
                        object.parts.push_back(std::move(part));
                    else if (why.empty())
                        why = partWhy;
                }
            }
            object.collision = collision;
            for (GeoFilePart &part : object.parts)
                part.objectCollision = collision;
            return object;
        }

        void readGeo(const std::vector<uint8_t> &d, size_t begin, size_t end, std::vector<GeoFileObject> &out,
                     std::string &why, int depth)
        {
            if (depth > 8)
                return;
            std::vector<Chunk> list;
            if (!chunks(d, begin, end, list) && why.empty())
                why = "chunks do not add up";
            for (const Chunk &c : list)
            {
                if (tagIs(c.tag, "TGEO"))
                    out.push_back(readObject(d, c.body, c.body + c.size, c.body - 8, why));
                else if (tagIs(c.tag, "FORM") || tagIs(c.tag, "FMDL") || tagIs(c.tag, "MODL") ||
                         // An asset library (ZGE2's .zif: ULIB, each asset a UAST with its HAST header, a model's
                         // UMDL around its MODL; DM2's BikeUI.zit in com\motions.zif).
                         tagIs(c.tag, "ULIB") || tagIs(c.tag, "UAST") || tagIs(c.tag, "UMDL"))
                {
                    // A FORM may start with its type tag (TXRC libraries do);
                    // a GEO's starts with its first TGEO.
                    size_t body = c.body;
                    if (tagIs(c.tag, "FORM") && c.size >= 12 && !tagIs(&d[c.body], "TGEO"))
                        body += 4;
                    readGeo(d, body, c.body + c.size, out, why, depth + 1);
                }
            }
        }
    }

    std::vector<GeoFileObject> readGeoFile(const std::vector<uint8_t> &file, std::string *error)
    {
        std::vector<GeoFileObject> out;
        std::string why;
        if (file.size() >= 8 && tagIs(file.data(), "TGHD"))
            // A TGEO's body on its own: what the game parses from memory
            // (it reads a level's objects into RAM one by one).
            out.push_back(readObject(file, 0, file.size(), 0, why));
        else
            readGeo(file, 0, file.size(), out, why, 0);
        if (error)
            *error = why;
        return out;
    }
}
