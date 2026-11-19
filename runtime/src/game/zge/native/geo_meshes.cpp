#include "geo_meshes.h"

#include "disc_files.h"

#include "ee/ee.h"
#include "runtime/ps2_boot_config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

// A part noted anew drops the host part its positions stream had of its own, and a clone's copied stream takes a
// copy of its source's (collision_data.cpp keeps those host parts and sets these hooks). The mesh reader only calls
// them, so it needs no collision code: the asset tools (ps2xNativeAssets) link it alone.
namespace zge::native
{
    namespace
    {
        std::atomic<OwnHostPartForget> g_ownHostPartForget{nullptr};
        std::atomic<OwnHostPartClone> g_ownHostPartClone{nullptr};
    }

    void setOwnHostPartHooks(OwnHostPartForget forget, OwnHostPartClone clone)
    {
        g_ownHostPartForget.store(forget, std::memory_order_release);
        g_ownHostPartClone.store(clone, std::memory_order_release);
    }
}

namespace zge::native
{
    namespace zx = ps2x::zaxis;

    bool packetKindOf(uint32_t renderType, zx::PacketKind &kind)
    {
        switch (renderType)
        {
        case 0x04:
        case 0x18:
            kind = zx::PacketKind::RigidPrelit;
            return true;
        case 0x20:
            kind = zx::PacketKind::Terrain;
            return true;
        case 0x08:
            kind = zx::PacketKind::Skinned;
            return true;
        case 0x0C:
            kind = zx::PacketKind::SkinnedPrelit;
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        // The packet's colour: every byte halved (0x80 = 1.0 for the GS).
        uint8_t halved(uint8_t c) { return static_cast<uint8_t>(c >> 1); }

        // A normal component as the packet builders quantize it (terrainNormalWord
        // terrain, skinnedNormalWord skinned): n * 32768 in EE floats, CVT.W.S
        // truncating, clamped to +-32767.
        int32_t normalFixed(float n)
        {
            int32_t fixed = ee::toInt(ee::Float(n) * ee::Float(32768.0f));
            if (fixed > 0x7FFF)
                fixed = 0x7FFF;
            if (!(fixed > -0x8000))
                fixed = -0x7FFF;
            return fixed;
        }

        // The high byte of a normal component as the terrain packet carries
        // it (the grass shells' direction).
        int8_t normalByte(const GeoFilePart &p, uint32_t v, int c)
        {
            if (!p.hasNormals || p.normalStride < 12)
                return 0;
            float f;
            std::memcpy(&f, &p.normalBytes[static_cast<size_t>(v) * p.normalStride + static_cast<size_t>(c) * 4u], 4);
            return static_cast<int8_t>(static_cast<int16_t>(normalFixed(f)) >> 8);
        }

        // A rigid vertex's normal as ZGE4's rigid packet carries it (rigidNormalWord): x, y, z fixed, z's low five
        // bits clear, w 0.
        void rigidNormal(const GeoFilePart &p, uint32_t v, int16_t out[4])
        {
            for (int c = 0; c < 3; ++c)
            {
                float f = 0.0f;
                if (p.hasNormals && p.normalStride >= 12)
                    std::memcpy(&f, &p.normalBytes[static_cast<size_t>(v) * p.normalStride + static_cast<size_t>(c) * 4u], 4);
                const int32_t fixed = normalFixed(f);
                out[c] = static_cast<int16_t>(c == 2 ? fixed & ~31 : fixed);
            }
            out[3] = 0;
        }
    }

    namespace
    {
        // A strip entry ends a triangle (the previous two and it) unless it
        // restarts the strip; the first two of a chunk never do.
        bool entryEndsTriangle(const GeoGroup &g, size_t chunkFirst, size_t entry)
        {
            return entry >= chunkFirst + 2 && (g.entries[entry] & 0x8000u) == 0;
        }

        // A skinned vertex as the skinned packet builders (addSkinnedPositions with
        // skinnedNormalWord / skinnedColourWord) carry it: in the position's low 7
        // mantissa bits per component the VU1 address of bone 0..3's matrix
        // ((bone - 1) * 4, 0x3F for none: withBone, the whole word), x's bit 0
        // whether the vertex ends a triangle, w 1.0; then the normal * 32768
        // (clamped) with the weights in sixteenths in its low 5 bits, or the
        // halved colour with the weights in bits 8-12 (prelit); bone 0's
        // weight is 16 less the others'. `record` is the vertex's skin weights
        // as the builder sends them (packetSkinWeights).
        void skinnedVertex(const GeoFilePart &p, uint32_t v, const uint8_t record[8], bool lit, bool uvsAsFloats,
                           bool endsTriangle, GSNativeVertex &out, GSNativeSkinWeights &weights)
        {
            const uint8_t *w = record;
            uint32_t address[4];
            uint8_t sixteenths[4];
            for (int c = 0; c < 4; ++c)
            {
                const uint32_t bone = w[c * 2];
                address[c] = bone ? (bone - 1u) << 2 : 0x3Fu;
                sixteenths[c] = static_cast<uint8_t>(w[c * 2 + 1] >> 4);
            }
            // Bone 0 takes what the others leave (both builders).
            sixteenths[0] = static_cast<uint8_t>(16 - (sixteenths[1] & 0x1F) - (sixteenths[2] & 0x1F) -
                                                 (sixteenths[3] & 0x1F));
            float position[4] = {out.pos[0], out.pos[1], out.pos[2], 1.0f};
            for (int c = 0; c < 4; ++c)
            {
                uint32_t bits;
                std::memcpy(&bits, &position[c], 4);
                bits = (bits & ~0x7Fu) | address[c];
                if (c == 0)
                    bits = (bits & ~1u) | (endsTriangle ? 1u : 0u);
                std::memcpy(&position[c], &bits, 4);
                weights.bone[c] = static_cast<uint8_t>(bits & 0x7Eu);
                weights.weight[c] = static_cast<uint8_t>(sixteenths[c] & 0x1Fu);
            }
            std::memcpy(out.pos, position, 12);
            if (uvsAsFloats)
            {
                out.st[0] = p.uvFloats[v * 2u];
                out.st[1] = p.uvFloats[v * 2u + 1u];
            }
            else
            {
                out.st[0] = static_cast<float>(p.uvs[v * 2u]) / 4096.0f;
                out.st[1] = static_cast<float>(p.uvs[v * 2u + 1u]) / 4096.0f;
            }
            for (int c = 0; c < 4; ++c)
            {
                uint16_t lane;
                if (lit)
                {
                    float n = 0.0f;
                    if (c < 3 && p.hasNormals && p.normalStride >= 12)
                        std::memcpy(&n, &p.normalBytes[static_cast<size_t>(v) * p.normalStride + c * 4u], 4);
                    // The low 5 bits carry the weight.
                    const int32_t fixed = normalFixed(n);
                    lane = static_cast<uint16_t>((static_cast<uint32_t>(fixed) & ~0x1Fu) | (sixteenths[c] & 0x1Fu));
                }
                else
                {
                    const uint8_t colour = p.hasColours ? halved(p.colours[v * 4u + c]) : 0;
                    lane = static_cast<uint16_t>(colour | (sixteenths[c] & 0x1Fu) << 8);
                }
                out.normal[c] = static_cast<int16_t>(lane);
                if (c < 3)
                    out.rgba[c] = lit ? 0 : static_cast<uint8_t>(lane & 0xFFu);
            }
            out.rgba[3] = 0x80;
        }
    }

    void packetSkinWeights(zx::SkinnedDrawForm::Weights form, uint8_t record[8])
    {
        using Weights = zx::SkinnedDrawForm::Weights;
        struct Bone
        {
            uint8_t &bone, &weight;
        };
        Bone b[4] = {{record[0], record[1]}, {record[2], record[3]}, {record[4], record[5]}, {record[6], record[7]}};
        const auto clear = [&](int first)
        {
            for (int i = first; i < 4; ++i)
                b[i].bone = b[i].weight = 0;
        };
        if (form == Weights::ThreeBones)
        {
            // ZGE4's limitSkinWeights(part, 3).
            clear(3);
            const uint32_t total = b[0].weight + b[3].weight + (b[2].weight + b[1].weight);
            const auto scaled = [total](uint32_t weight) { return total != 0 ? weight * 255u / total : 0u; };
            const uint32_t second = scaled(b[1].weight), third = scaled(b[2].weight), fourth = scaled(b[3].weight);
            b[1].weight = static_cast<uint8_t>(second);
            b[2].weight = static_cast<uint8_t>(third);
            b[3].weight = static_cast<uint8_t>(fourth);
            b[0].weight = static_cast<uint8_t>(~(fourth + (second + third)));
        }
        else if (form == Weights::LightBonesDropped)
        {
            // ZGE2's dropLightBones (a bone under 12/255 and every one after it out, the rest made 255 again).
            constexpr uint32_t kLeastWeight = 12;
            if (b[1].weight < kLeastWeight)
            {
                clear(1);
                b[0].weight = 0xFF;
            }
            else if (b[2].weight < kLeastWeight)
            {
                const int32_t sum = b[0].weight + b[1].weight;
                const uint8_t first = static_cast<uint8_t>(b[0].weight * 255 / sum);
                clear(2);
                b[0].weight = first;
                b[1].weight = static_cast<uint8_t>(~first);
            }
            else if (b[3].weight < kLeastWeight)
            {
                const int32_t sum = b[0].weight + b[1].weight + b[2].weight;
                const uint8_t first = static_cast<uint8_t>(b[0].weight * 255 / sum);
                const uint8_t second = static_cast<uint8_t>(b[1].weight * 255 / sum);
                b[0].weight = first;
                b[3].bone = b[3].weight = 0;
                b[1].weight = second;
                b[2].weight = static_cast<uint8_t>(~(first + second));
            }
            // Then three bones sent (addSkinnedPositionWords, skinnedNormalWords): the fourth not at all.
            clear(3);
        }
    }

    bool buildFileMesh(const GeoFilePart &p, zx::PacketKind kind, zx::RigidMesh &out, std::string *why,
                       const zx::SkinnedDrawForm &form)
    {
        auto fail = [why](const char *reason)
        {
            if (why)
                *why = reason;
            return false;
        };
        const bool terrain = kind == zx::PacketKind::Terrain;
        const bool skinned = kind == zx::PacketKind::Skinned || kind == zx::PacketKind::SkinnedPrelit;
        const bool lit = kind == zx::PacketKind::Skinned;
        const bool normals = kind == zx::PacketKind::RigidNormals;
        if (normals && (!p.hasNormals || p.normalStride < 12))
            return fail("no normals");
        if (skinned && p.skinWeights.size() < p.vertexCount * 8u)
            return fail("skinned without weights");
        if (p.positions.size() < p.vertexCount * 3u)
            return fail("no positions");
        if (!p.hasUvs)
            return fail("no UVs");
        for (const GeoGroup &g : p.groups)
            for (uint16_t entry : g.entries)
                if ((entry & 0x3FFFu) >= p.vertexCount)
                    return fail("strip entry past the vertices");

        // The runs as the builder's material table has them: a run starts each time the submesh changes from one
        // strip group to the next, at its submesh's entry (ZGE2's skinned builder: the next entry); an entry
        // started again keeps the later run (the earlier one's chain is not reached from the table).
        const bool inGroupOrder = skinned && form.materialsInGroupOrder;
        const bool uvsAsFloats = skinned && form.uvsAsFloats;
        if (uvsAsFloats && p.uvFloats.size() < p.vertexCount * 2u)
            return fail("no UV floats");
        struct Run
        {
            size_t firstGroup = 0, endGroup = 0;
            bool any = false;
        };
        std::vector<Run> runs(p.materialCount);
        {
            uint32_t filled = 0;
            for (size_t g = 0; g < p.groups.size();)
            {
                size_t end = g + 1;
                while (end < p.groups.size() && p.groups[end].material == p.groups[g].material)
                    ++end;
                const uint32_t entry = inGroupOrder ? filled++ : p.groups[g].material;
                if (entry >= runs.size())
                    runs.resize(entry + 1u);
                runs[entry] = Run{g, end, true};
                g = end;
            }
        }

        auto mesh = std::make_shared<GSNativeMesh>();
        mesh->id = GSNativeMesh::nextId();
        out.runs.assign(runs.size(), zx::RigidMesh::Run{});
        out.chunks = 0;
        for (size_t entry = 0; entry < runs.size(); ++entry)
        {
            out.runs[entry].first = static_cast<uint32_t>(mesh->vertices.size());
            for (size_t gi = runs[entry].firstGroup; runs[entry].any && gi < runs[entry].endGroup; ++gi)
            {
                const GeoGroup &g = p.groups[gi];
                // The builder sends a group in chunks of at most 60 entries,
                // each one a strip of its own.
                for (size_t first = 0; first < g.entries.size(); first += zx::kChunkEntries)
                {
                    const size_t count = std::min<size_t>(zx::kChunkEntries, g.entries.size() - first);
                    ++out.chunks;
                    for (size_t k = 2; k < count; ++k)
                    {
                        if (!entryEndsTriangle(g, first, first + k))
                            continue;
                        GSNativeVertex tv[3];
                        GSNativeSkinWeights tw[3];
                        for (int i = 0; i < 3; ++i)
                        {
                            const size_t at = first + k - 2 + static_cast<size_t>(i);
                            const uint32_t v = g.entries[at] & 0x3FFFu;
                            std::memcpy(tv[i].pos, &p.positions[v * 3u], 12);
                            if (skinned)
                            {
                                uint8_t record[8];
                                std::memcpy(record, &p.skinWeights[v * 8u], sizeof(record));
                                packetSkinWeights(form.weights, record);
                                skinnedVertex(p, v, record, lit, uvsAsFloats, entryEndsTriangle(g, first, at), tv[i],
                                              tw[i]);
                                continue;
                            }
                            tv[i].st[0] = static_cast<float>(p.uvs[v * 2u]) / 4096.0f;
                            tv[i].st[1] = static_cast<float>(p.uvs[v * 2u + 1u]) / 4096.0f;
                            const uint8_t *c = p.hasColours ? &p.colours[v * 4u] : nullptr;
                            if (terrain)
                            {
                                for (int a = 0; a < 3; ++a)
                                {
                                    const uint8_t low = c ? halved(c[a]) : 0;
                                    tv[i].normal[a] = static_cast<int16_t>(
                                        static_cast<uint16_t>(static_cast<uint8_t>(normalByte(p, v, a))) << 8 | low);
                                    tv[i].rgba[a] = low;
                                }
                                tv[i].rgba[3] = 0x80;
                            }
                            else if (normals)
                            {
                                // Lit from the normal by its node's lights; else VU1 draws the words' low bytes.
                                rigidNormal(p, v, tv[i].normal);
                                for (int a = 0; a < 4; ++a)
                                    tv[i].rgba[a] = static_cast<uint8_t>(tv[i].normal[a] & 0xFF);
                            }
                            else
                                for (int a = 0; a < 4; ++a)
                                    tv[i].rgba[a] = c ? halved(c[a]) : 0;
                        }
                        const float b[4] = {std::min({tv[0].st[0], tv[1].st[0], tv[2].st[0]}),
                                            std::min({tv[0].st[1], tv[1].st[1], tv[2].st[1]}),
                                            std::max({tv[0].st[0], tv[1].st[0], tv[2].st[0]}),
                                            std::max({tv[0].st[1], tv[1].st[1], tv[2].st[1]})};
                        for (int i = 0; i < 3; ++i)
                        {
                            std::memcpy(tv[i].stBounds, b, sizeof(b));
                            mesh->vertices.push_back(tv[i]);
                            if (skinned)
                                mesh->skin.push_back(tw[i]);
                        }
                    }
                }
            }
            out.runs[entry].count = static_cast<uint32_t>(mesh->vertices.size()) - out.runs[entry].first;
        }
        out.triangles = static_cast<uint32_t>(mesh->vertices.size() / 3u);
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (const GSNativeVertex &v : mesh->vertices)
            for (int c = 0; c < 3; ++c)
            {
                lo[c] = std::min(lo[c], v.pos[c]);
                hi[c] = std::max(hi[c], v.pos[c]);
            }
        if (mesh->vertices.empty())
            lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0.0f;
        std::memcpy(mesh->boundsMin, lo, sizeof(lo));
        std::memcpy(mesh->boundsMax, hi, sizeof(hi));
        out.mesh = std::move(mesh);
        return true;
    }

    namespace
    {
        struct NotedPart
        {
            std::string path; // normalized
            uint32_t chunkOffset = 0;
            std::shared_ptr<const GeoFilePart> part;
            uint32_t meshType = ~0u;
            // The disc file the data was read from and the TGPT chunk's offset
            // in it (memory streams: through the disc layer's reads).
            std::string discPath;
            uint64_t discOffset = 0;
            bool discKnown = false;
            // Streams the game made (notePartStreamsMade): `part` is them as read from RAM (notePartStreams),
            // not a file's.
            bool streamsMade = false;
            std::shared_ptr<const zx::RigidMesh> mesh;
        };

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<uint32_t, NotedPart> parts;
            // Files read, by path: their parts by TGPT offset.
            std::map<std::string, std::shared_ptr<std::map<uint32_t, std::shared_ptr<const GeoFilePart>>>> files;
            // A disc path's file as it was parsed: the game's reads of it then
            // (discFileReads) and its contents' FNV-1a. When the game reads
            // the path again and the contents differ (a game override serving
            // another file under it), it is parsed again; the old parts are
            // kept (host collision parts are keyed by their address).
            struct Parsed
            {
                uint64_t reads = 0;
                uint64_t hash = 0;
            };
            std::map<std::string, Parsed> parsed;
            std::vector<std::shared_ptr<std::map<uint32_t, std::shared_ptr<const GeoFilePart>>>> retired;
        };

        // Bumped whenever the noted parts change, and at each boot (the
        // collision layer's per-thread copies are dropped when it moves).
        std::atomic<uint64_t> g_notedEpoch{0};

        // The boot's noted parts and files read: keyed by guest addresses
        // and disc paths, so each game boot has its own (a game booted after
        // another in the same process: runtime/ps2_boot_config.h).
        Registry &registry()
        {
            static ps2x::boot::PerBoot<std::unique_ptr<Registry>> ofBoot(
                []
                {
                    g_notedEpoch.fetch_add(1, std::memory_order_acq_rel);
                    return std::make_unique<Registry>();
                });
            return *ofBoot.get();
        }

        uint64_t contentHash(const std::vector<uint8_t> &bytes)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : bytes)
                h = (h ^ b) * 1099511628211ull;
            return h;
        }

        std::shared_ptr<std::map<uint32_t, std::shared_ptr<const GeoFilePart>>> fileParts(Registry &r,
                                                                                          const std::string &path)
        {
            auto it = r.files.find(path);
            const uint64_t reads = discFileReads(path);
            std::vector<uint8_t> bytes;
            bool haveBytes = false;
            if (it != r.files.end())
            {
                auto known = r.parsed.find(path);
                if (known == r.parsed.end() || known->second.reads == reads)
                    return it->second;
                // Read again since: the same contents keep their parts.
                haveBytes = readGameFile(path, bytes);
                const uint64_t hash = haveBytes ? contentHash(bytes) : 0u;
                if (hash == known->second.hash)
                {
                    known->second.reads = reads;
                    return it->second;
                }
                r.retired.push_back(it->second);
            }
            auto parts = std::make_shared<std::map<uint32_t, std::shared_ptr<const GeoFilePart>>>();
            if (haveBytes || readGameFile(path, bytes))
            {
                r.parsed[path] = Registry::Parsed{reads, contentHash(bytes)};
                std::string error;
                for (GeoFileObject &object : readGeoFile(bytes, &error))
                    for (GeoFilePart &part : object.parts)
                        (*parts)[part.chunkOffset] = std::make_shared<const GeoFilePart>(std::move(part));
                if (!error.empty())
                    std::cout << "[native-meshes] " << path << ": " << error << std::endl;
            }
            r.files[path] = parts;
            return parts;
        }
    }

    void forgetCheckedPart(uint32_t partAddress);

    namespace
    {
        // A memory stream's path ("  ADDRESS:    SIZE", in hex): its address and size.
        bool memoryStream(const std::string &path, uint32_t &address, uint32_t &size)
        {
            const char *p = path.c_str();
            if (const char *colon = std::strchr(p, ':'); colon && colon - p == 4 && !std::strncmp(p, "rd00", 4))
                p = colon + 1; // zfile's device prefix
            char *end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 16);
            if (end == p || *end != ':')
                return false;
            const char *q = end + 1;
            const unsigned long n = std::strtoul(q, &end, 16);
            if (end == q || n == 0 || n > 0x2000000ul)
                return false;
            address = static_cast<uint32_t>(a);
            size = static_cast<uint32_t>(n);
            return true;
        }

        uint64_t fnv1a(const uint8_t *p, size_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; ++i)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }
    }

    namespace
    {
        std::atomic<uint64_t> g_parseNanos{0}, g_streamsParsed{0};
    }

    uint64_t geoParseNanos() { return g_parseNanos.load(); }
    uint64_t geoStreamsParsed() { return g_streamsParsed.load(); }

    uint64_t notedPartsEpoch()
    {
        registry(); // a new boot's registry moves the epoch
        return g_notedEpoch.load(std::memory_order_acquire);
    }

    void notePart(const uint8_t *rdram, ee::Address part, const std::string &gamePath, uint32_t chunkOffset)
    {
        notePart(rdram, part.address, gamePath, chunkOffset);
    }

    void notePart(const uint8_t *rdram, uint32_t partAddress, const std::string &gamePath, uint32_t chunkOffset)
    {
        forgetCheckedPart(partAddress);
        if (const OwnHostPartForget forget = g_ownHostPartForget.load(std::memory_order_acquire))
            forget(partAddress);
        g_notedEpoch.fetch_add(1, std::memory_order_acq_rel);
        std::string key;
        uint32_t address = 0;
        uint32_t size = 0;
        std::vector<uint8_t> stream;
        std::string discPath;
        uint64_t discOffset = 0;
        bool discKnown = false;
        if (memoryStream(gamePath, address, size))
        {
            discKnown = discOrigin(address, size, discPath, discOffset);
            discOffset += chunkOffset;
            const uint8_t *bytes = zx::GuestMemory{rdram}.bytes(address, size);
            if (!bytes)
                return;
            stream.assign(bytes, bytes + size);
            char text[64];
            std::snprintf(text, sizeof(text), "mem:%08x:%x:%016llx", address, size,
                          static_cast<unsigned long long>(fnv1a(bytes, size)));
            key = text;
        }
        else
        {
            key = normalizeGamePath(gamePath);
            discPath = key;
            discOffset = chunkOffset;
            discKnown = true;
        }
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!stream.empty() && !r.files.count(key))
        {
            const auto started = std::chrono::steady_clock::now();
            struct Timer
            {
                std::chrono::steady_clock::time_point started;
                ~Timer()
                {
                    g_parseNanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::steady_clock::now() - started)
                                                              .count());
                    ++g_streamsParsed;
                }
            } timer{started};
            auto parts = std::make_shared<std::map<uint32_t, std::shared_ptr<const GeoFilePart>>>();
            std::string error;
            for (GeoFileObject &object : readGeoFile(stream, &error))
                for (GeoFilePart &part : object.parts)
                    (*parts)[part.chunkOffset] = std::make_shared<const GeoFilePart>(std::move(part));
            static int debug = 0;
            if (std::getenv("PS2X_NATIVE_MESHES_DEBUG") && debug++ < 3)
            {
                std::cout << "[native-meshes] stream " << key << " error '" << error << "' parts at";
                for (const auto &[at, _] : *parts)
                    std::cout << " 0x" << std::hex << at << std::dec;
                std::cout << " noted 0x" << std::hex << chunkOffset << std::dec << " bytes";
                for (size_t i = 0; i < 32 && i < stream.size(); ++i)
                    std::cout << ' ' << std::hex << int(stream[i]) << std::dec;
                std::cout << std::endl;
            }
            r.files[key] = parts;
        }
        if (std::getenv("PS2X_NATIVE_MESHES_DEBUG"))
            std::cout << "[native-meshes] noted 0x" << std::hex << partAddress << std::dec << " " << key << " +0x"
                      << std::hex << chunkOffset << std::dec << " = "
                      << (discKnown ? discPath : std::string("(disc file not known)")) << " +0x" << std::hex
                      << discOffset << std::dec << std::endl;
        NotedPart &n = r.parts[partAddress];
        n = NotedPart{};
        n.path = key;
        n.chunkOffset = chunkOffset;
        n.discPath = discPath;
        n.discOffset = discOffset;
        n.discKnown = discKnown;
    }

    void notePartClone(uint32_t cloneAddress, uint32_t sourceAddress)
    {
        forgetCheckedPart(cloneAddress);
        if (const OwnHostPartClone clone = g_ownHostPartClone.load(std::memory_order_acquire))
            clone(cloneAddress, sourceAddress);
        g_notedEpoch.fetch_add(1, std::memory_order_acq_rel);
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(sourceAddress);
        if (std::getenv("PS2X_NATIVE_MESHES_DEBUG"))
            std::cout << "[native-meshes] clone 0x" << std::hex << cloneAddress << " of 0x" << sourceAddress << std::dec
                      << (it == r.parts.end() ? " (source not noted)" : "") << std::endl;
        if (it == r.parts.end())
        {
            r.parts.erase(cloneAddress);
            return;
        }
        NotedPart copy = it->second;
        copy.meshType = ~0u;
        copy.mesh.reset();
        r.parts[cloneAddress] = std::move(copy);
    }

    void notePartStreamsMade(ee::Ptr<zge::ZgePart> part) { notePartStreamsMade(part.address); }

    void notePartStreamsMade(uint32_t partAddress)
    {
        forgetCheckedPart(partAddress);
        if (const OwnHostPartForget forget = g_ownHostPartForget.load(std::memory_order_acquire))
            forget(partAddress);
        g_notedEpoch.fetch_add(1, std::memory_order_acq_rel);
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (std::getenv("PS2X_NATIVE_MESHES_DEBUG"))
            std::cout << "[native-meshes] streams made 0x" << std::hex << partAddress << std::dec << std::endl;
        NotedPart &n = r.parts[partAddress];
        n = NotedPart{};
        n.streamsMade = true;
    }

    bool partStreamsMade(uint32_t partAddress)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        return it != r.parts.end() && it->second.streamsMade;
    }

    void notePartStreams(uint32_t partAddress, std::shared_ptr<const GeoFilePart> streams)
    {
        forgetCheckedPart(partAddress);
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        if (it == r.parts.end() || !it->second.streamsMade)
            return;
        it->second.part = std::move(streams);
        it->second.meshType = ~0u;
        it->second.mesh.reset();
    }

    bool partDiscOrigin(uint32_t partAddress, std::string &path, uint64_t &chunkOffset)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        if (it == r.parts.end() || !it->second.discKnown)
            return false;
        path = it->second.discPath;
        chunkOffset = it->second.discOffset;
        return true;
    }

    std::shared_ptr<const GeoFilePart> filePartFor(uint32_t partAddress)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        if (it == r.parts.end())
            return nullptr;
        NotedPart &n = it->second;
        if (!n.part && !n.streamsMade)
        {
            const auto parts = fileParts(r, n.path);
            auto p = parts->find(n.chunkOffset);
            if (p != parts->end())
                n.part = p->second;
        }
        return n.part;
    }

    std::shared_ptr<const GeoFilePart> discFilePartFor(uint32_t partAddress)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        if (it == r.parts.end() || !it->second.discKnown)
            return nullptr;
        const auto parts = fileParts(r, it->second.discPath);
        auto p = parts->find(static_cast<uint32_t>(it->second.discOffset));
        return p != parts->end() ? p->second : nullptr;
    }

    std::shared_ptr<const zx::RigidMesh> fileMeshForPart(uint32_t partAddress, uint32_t renderType)
    {
        zx::PacketKind kind;
        if (!packetKindOf(renderType, kind))
            return nullptr;
        return fileMeshForPacketKind(partAddress, kind);
    }

    std::shared_ptr<const zx::RigidMesh> fileMeshForPacketKind(uint32_t partAddress, zx::PacketKind kind)
    {
        return fileMeshForPacketKind(partAddress, kind, zx::SkinnedDrawForm{});
    }

    std::shared_ptr<const zx::RigidMesh> fileMeshForPacketKind(uint32_t partAddress, zx::PacketKind kind,
                                                                const zx::SkinnedDrawForm &form)
    {
        const auto part = filePartFor(partAddress);
        if (!part)
            return nullptr;
        // The cache key: the kind and the form.
        const uint32_t renderType = static_cast<uint32_t>(kind) | 0x100u | static_cast<uint32_t>(form.weights) << 12 |
                                    (form.materialsInGroupOrder ? 0x10000u : 0u) | (form.uvsAsFloats ? 0x20000u : 0u);
        Registry &r = registry();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.parts.find(partAddress);
            if (it != r.parts.end() && it->second.meshType == renderType && it->second.part == part)
                return it->second.mesh;
        }
        auto mesh = std::make_shared<zx::RigidMesh>();
        if (!buildFileMesh(*part, kind, *mesh, nullptr, form))
            mesh.reset();
        else
            mesh->mesh->source = partAddress;
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.parts.find(partAddress);
        if (it != r.parts.end() && it->second.part == part)
        {
            it->second.meshType = renderType;
            it->second.mesh = mesh;
        }
        return mesh;
    }

    bool meshCheckEnabled()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_NATIVE_MESHES_CHECK");
            return v && *v && *v != '0';
        }();
        return on;
    }

    namespace
    {
        // The first field two vertices differ in, or nullptr.
        const char *vertexDifference(const GSNativeVertex &a, const GSNativeVertex &b)
        {
            if (std::memcmp(a.pos, b.pos, sizeof(a.pos)))
                return "position";
            if (std::memcmp(a.st, b.st, sizeof(a.st)))
                return "uv";
            if (std::memcmp(a.rgba, b.rgba, sizeof(a.rgba)))
                return "colour";
            if (std::memcmp(a.stBounds, b.stBounds, sizeof(a.stBounds)))
                return "uv bounds";
            if (std::memcmp(a.normal, b.normal, sizeof(a.normal)))
                return "normal";
            return nullptr;
        }

        struct CheckStats
        {
            std::mutex mutex;
            uint64_t parts = 0, same = 0, differ = 0, notNoted = 0, notModelled = 0, noPacket = 0;
            uint64_t discKnown = 0, discConfirmed = 0;
            std::map<std::string, uint64_t> reasons;
            int described = 0;
        };
        // The boot's mesh checks (PS2X_NATIVE_MESHES check mode), by part
        // address (ps2x::boot::PerBoot).
        struct Checks
        {
            CheckStats stats;
            std::mutex checkedMutex;
            std::unordered_map<uint32_t, bool> checked; // part -> checked since noted
        };
        Checks &checks()
        {
            static ps2x::boot::PerBoot<std::unique_ptr<Checks>> ofBoot([] { return std::make_unique<Checks>(); });
            return *ofBoot.get();
        }
    }

    void forgetCheckedPart(uint32_t partAddress)
    {
        Checks &c = checks();
        std::lock_guard<std::mutex> lock(c.checkedMutex);
        c.checked.erase(partAddress);
    }

    void checkPartMeshOnce(const uint8_t *rdram, ee::Ptr<zge::ZgePart> part, uint32_t renderType, zx::PacketKind kind,
                           const zx::SkinnedDrawForm &form)
    {
        checkPartMeshOnce(rdram, part.address, renderType, kind, form);
    }

    void notePartClone(ee::Ptr<zge::ZgePart> clone, ee::Ptr<zge::ZgePart> source)
    {
        notePartClone(clone.address, source.address);
    }

    void checkPartMeshOnce(const uint8_t *rdram, uint32_t partAddress, uint32_t renderType, zx::PacketKind kind,
                           const zx::SkinnedDrawForm &form)
    {
        {
            Checks &c = checks();
            std::lock_guard<std::mutex> lock(c.checkedMutex);
            bool &done = c.checked[partAddress];
            if (done)
                return;
            done = true;
        }
        checkPartMesh(rdram, partAddress, renderType, kind, form);
    }

    void checkPartMesh(const uint8_t *rdram, uint32_t partAddress, uint32_t renderType, zx::PacketKind kind,
                       const zx::SkinnedDrawForm &form)
    {
        const auto part = filePartFor(partAddress);
        zx::RigidMesh fromFile, fromPacket;
        std::string fileWhy, packetWhy;
        const bool skinned = kind == zx::PacketKind::Skinned || kind == zx::PacketKind::SkinnedPrelit;
        const bool haveFile = part && buildFileMesh(*part, kind, fromFile, &fileWhy, form);
        zx::GuestMemory memory{rdram};
        const bool havePacket =
            skinned ? zx::decodeSkinnedPart(memory, partAddress, fromPacket, &packetWhy, kind)
                    : zx::decodeRigidPart(memory, partAddress, fromPacket, &packetWhy, kind);

        // The part as the disc file has it, at the offset the disc layer's
        // reads give: the same streams and strips as the one parsed from RAM.
        bool discKnown = false, discSame = false;
        {
            std::string discPath;
            uint64_t discOffset = 0;
            if (part && partDiscOrigin(partAddress, discPath, discOffset))
            {
                discKnown = true;
                Registry &r = registry();
                std::lock_guard<std::mutex> rl(r.mutex);
                const auto parts = fileParts(r, discPath);
                auto it = parts->find(static_cast<uint32_t>(discOffset));
                if (it != parts->end())
                {
                    const GeoFilePart &a = *it->second, &b = *part;
                    discSame = a.positions == b.positions && a.uvs == b.uvs && a.colours == b.colours &&
                               a.normalBytes == b.normalBytes && a.skinWeights == b.skinWeights &&
                               a.groups.size() == b.groups.size();
                    for (size_t g = 0; discSame && g < a.groups.size(); ++g)
                        discSame = a.groups[g].material == b.groups[g].material &&
                                   a.groups[g].entries == b.groups[g].entries;
                }
            }
        }

        CheckStats &s = checks().stats;
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.parts;
        s.discKnown += discKnown;
        s.discConfirmed += discSame;
        std::string verdict;
        if (!part)
            ++s.notNoted, verdict = "not noted";
        else if (!havePacket)
            ++s.noPacket, verdict = "packet: " + packetWhy;
        else if (!haveFile)
            ++s.notModelled, ++s.reasons[fileWhy];
        else
        {
            // Compare run by run.
            std::string diff;
            if (fromFile.runs.size() != fromPacket.runs.size())
                diff = "material count";
            for (size_t k = 0; diff.empty() && k < fromFile.runs.size(); ++k)
            {
                const auto &a = fromFile.runs[k], &b = fromPacket.runs[k];
                if (a.count != b.count)
                {
                    diff = "material " + std::to_string(k) + ": " + std::to_string(a.count / 3) + " triangles from the file, " +
                           std::to_string(b.count / 3) + " in the packet";
                    break;
                }
                for (uint32_t i = 0; i < a.count; ++i)
                {
                    const char *field = vertexDifference(fromFile.mesh->vertices[a.first + i],
                                                         fromPacket.mesh->vertices[b.first + i]);
                    if (!field && skinned &&
                        std::memcmp(&fromFile.mesh->skin[a.first + i], &fromPacket.mesh->skin[b.first + i],
                                    sizeof(GSNativeSkinWeights)))
                        field = "bones";
                    if (field)
                    {
                        const GSNativeVertex &x = fromFile.mesh->vertices[a.first + i];
                        const GSNativeVertex &y = fromPacket.mesh->vertices[b.first + i];
                        char text[256];
                        std::snprintf(text, sizeof(text),
                                      "material %zu vertex %u: %s (file rgba %02x%02x%02x%02x n %04x %04x %04x st %g %g, "
                                      "packet rgba %02x%02x%02x%02x n %04x %04x %04x st %g %g)",
                                      k, i, field, x.rgba[0], x.rgba[1], x.rgba[2], x.rgba[3],
                                      static_cast<uint16_t>(x.normal[0]), static_cast<uint16_t>(x.normal[1]),
                                      static_cast<uint16_t>(x.normal[2]), x.st[0], x.st[1], y.rgba[0], y.rgba[1],
                                      y.rgba[2], y.rgba[3], static_cast<uint16_t>(y.normal[0]),
                                      static_cast<uint16_t>(y.normal[1]), static_cast<uint16_t>(y.normal[2]), y.st[0],
                                      y.st[1]);
                        diff = text;
                        break;
                    }
                }
            }
            if (diff.empty())
                ++s.same;
            else
            {
                ++s.differ;
                verdict = diff;
            }
        }
        // PS2X_NATIVE_MESHES_CHECK=2: a line for every part checked.
        static const bool everyPart = []
        {
            const char *v = std::getenv("PS2X_NATIVE_MESHES_CHECK");
            return v && std::atoi(v) >= 2;
        }();
        if (everyPart)
        {
            uint64_t entries = 0;
            uint64_t fileTriangles = 0;
            uint64_t packetTriangles = 0;
            if (part)
                for (const GeoGroup &g : part->groups)
                    entries += g.entries.size();
            for (const auto &run : fromFile.runs)
                fileTriangles += run.count / 3u;
            for (const auto &run : fromPacket.runs)
                packetTriangles += run.count / 3u;
            NotedPart noted;
            {
                Registry &r = registry();
                std::lock_guard<std::mutex> rl(r.mutex);
                auto it = r.parts.find(partAddress);
                if (it != r.parts.end())
                    noted = it->second;
            }
            std::cout << "[native-meshes] checked part 0x" << std::hex << partAddress << " type 0x" << renderType
                      << std::dec << " " << (noted.discKnown ? noted.discPath : noted.path) << ": "
                      << (part ? part->vertexCount : 0) << " vertices, " << entries << " strip entries, "
                      << fromFile.runs.size() << " materials; triangles " << fileTriangles << " from the file, "
                      << packetTriangles << " in the packet; " << (verdict.empty() ? "the same" : verdict)
                      << std::endl;
        }
        if (!verdict.empty() && s.described < 12)
        {
            ++s.described;
            NotedPart noted;
            {
                Registry &r = registry();
                std::lock_guard<std::mutex> rl(r.mutex);
                auto it = r.parts.find(partAddress);
                if (it != r.parts.end())
                    noted = it->second;
            }
            std::cout << "[native-meshes] part 0x" << std::hex << partAddress << " type 0x" << renderType << std::dec
                      << " (" << noted.path << " +0x" << std::hex << noted.chunkOffset << std::dec << "): " << verdict
                      << std::endl;
        }
        if (s.parts % 64u == 0u)
        {
            std::cout << "[native-meshes] check: " << s.parts << " parts, " << s.same << " the same from the file, "
                      << s.differ << " differ, " << s.notModelled << " not modelled";
            for (const auto &[why, n] : s.reasons)
                std::cout << " (" << why << ": " << n << ")";
            std::cout << ", " << s.notNoted << " not loaded by the readable loader, " << s.noPacket << " no packet; "
                      << s.discKnown << " with their disc file known, " << s.discConfirmed
                      << " the same as in it" << std::endl;
        }
    }
}
