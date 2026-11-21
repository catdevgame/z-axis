// Z-Axis meshes as native GS meshes: see runtime/ps2_zaxis_mesh.h.
#include "runtime/ps2_zaxis_mesh.h"

#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

uint64_t GSNativeMesh::nextId()
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace zx::zaxis
{
    const uint8_t *GuestMemory::bytes(uint32_t address, size_t n) const
    {
        if (!rdram || n == 0u)
            return nullptr;
        const uint8_t *first = getConstMemPtr(rdram, address);
        const uint8_t *last = getConstMemPtr(rdram, address + static_cast<uint32_t>(n - 1u));
        return first && last == first + (n - 1u) ? first : nullptr;
    }

    bool GuestMemory::u8(uint32_t address, uint8_t &out) const
    {
        const uint8_t *p = bytes(address, 1);
        if (!p)
            return false;
        out = *p;
        return true;
    }

    bool GuestMemory::u16(uint32_t address, uint16_t &out) const
    {
        const uint8_t *p = bytes(address, 2);
        if (!p)
            return false;
        std::memcpy(&out, p, 2);
        return true;
    }

    bool GuestMemory::u32(uint32_t address, uint32_t &out) const
    {
        const uint8_t *p = bytes(address, 4);
        if (!p)
            return false;
        std::memcpy(&out, p, 4);
        return true;
    }

    bool GuestMemory::f32(uint32_t address, float &out) const
    {
        const uint8_t *p = bytes(address, 4);
        if (!p)
            return false;
        std::memcpy(&out, p, 4);
        return true;
    }

    namespace
    {
        // The VIF stream of a DMA chain as VIF1 gets it with CHCR.TTE: each
        // tag's upper 64 bits, then its data.
        bool chainStream(const GuestMemory &m, uint32_t address, std::vector<uint8_t> &out, std::string &why)
        {
            for (int budget = 4096; budget > 0; --budget)
            {
                const uint8_t *tag = m.bytes(address, 16);
                if (!tag)
                {
                    why = "packet not in memory";
                    return false;
                }
                uint64_t lo = 0;
                std::memcpy(&lo, tag, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>((lo >> 32) & 0x7FFFFFF0u);
                out.insert(out.end(), tag + 8, tag + 16);
                uint32_t data = address + 16u, next = data + qwc * 16u;
                bool end = false;
                switch (id)
                {
                case 1: // cnt
                    break;
                case 2: // next
                    next = addr;
                    break;
                case 3: // ref
                case 4: // refs
                    data = addr;
                    next = address + 16u;
                    break;
                case 6: // ret
                case 7: // end
                    end = true;
                    break;
                default: // refe, call: not what the builder makes
                    why = "packet has other DMA tags";
                    return false;
                }
                if (qwc)
                {
                    const uint8_t *p = m.bytes(data, qwc * 16u);
                    if (!p)
                    {
                        why = "packet not in memory";
                        return false;
                    }
                    out.insert(out.end(), p, p + qwc * 16u);
                }
                if (end)
                    return true;
                address = next;
            }
            why = "packet chain too long";
            return false;
        }
    }

    bool decodeMaterialPacket(const GuestMemory &m, uint32_t packet, std::vector<GSNativeVertex> &out, uint32_t &chunks,
                              std::string *why, PacketKind kind, std::vector<GSNativeSkinWeights> *skin)
    {
        const bool terrain = kind == PacketKind::Terrain;
        const bool skinned = kind == PacketKind::Skinned || kind == PacketKind::SkinnedPrelit;
        const bool normals = kind == PacketKind::RigidNormals;
        if (skinned && !skin)
        {
            if (why)
                *why = "no bone output";
            return false;
        }
        std::string reason;
        auto fail = [&](const char *r)
        {
            if (why)
                *why = r;
            return false;
        };
        std::vector<uint8_t> vif;
        if (!chainStream(m, packet, vif, reason))
        {
            if (why)
                *why = reason;
            return false;
        }
        uint32_t cl = 1, wl = 1;
        // The chunk being unpacked.
        uint32_t vertices = 0;
        std::vector<int16_t> uv;       // s, t, z, w per vertex (terrain: s, t)
        std::vector<uint8_t> colour;   // RGBA
        std::vector<int16_t> packed;   // terrain: normal high bytes, colour low bytes, flags << 8 in w; RigidNormals: the normal
        std::vector<float> position;   // x, y, z (skinned: x, y, z, w)
        auto emit = [&]() -> bool
        {
            if (vertices == 0u && uv.empty())
                return true;
            const uint32_t uvStride = terrain || skinned ? 2u : 4u;
            const uint32_t posStride = skinned ? 4u : 3u;
            if (uv.size() != vertices * uvStride || position.size() != vertices * posStride ||
                (terrain || skinned || normals ? packed.size() != vertices * 4u : colour.size() != vertices * 4u))
                return false;
            ++chunks;
            auto bits = [&](uint32_t v, int c)
            {
                uint32_t b = 0;
                std::memcpy(&b, &position[v * posStride + static_cast<uint32_t>(c)], 4);
                return b;
            };
            for (uint32_t k = 2; k < vertices; ++k)
            {
                if (skinned)
                {
                    // x's bit 0: the vertex ends a triangle (else ADC).
                    if ((bits(k, 0) & 1u) == 0u)
                        continue;
                }
                else
                {
                    const uint16_t flags = static_cast<uint16_t>(terrain ? packed[k * 4u + 3u] : uv[k * 4u + 3u]);
                    if (flags & 0x8000u)
                        continue;
                }
                GSNativeVertex tv[3];
                GSNativeSkinWeights tw[3];
                for (int i = 0; i < 3; ++i)
                {
                    const uint32_t v = k - 2u + static_cast<uint32_t>(i);
                    std::memcpy(tv[i].pos, &position[v * posStride], 12);
                    // ITOF12: exact.
                    tv[i].st[0] = static_cast<float>(uv[v * uvStride]) / 4096.0f;
                    tv[i].st[1] = static_cast<float>(uv[v * uvStride + 1u]) / 4096.0f;
                    if (skinned)
                    {
                        const bool lit = kind == PacketKind::Skinned;
                        for (int c = 0; c < 4; ++c)
                        {
                            const uint16_t lane = static_cast<uint16_t>(packed[v * 4u + c]);
                            tv[i].normal[c] = packed[v * 4u + c];
                            tw[i].bone[c] = static_cast<uint8_t>(bits(v, c) & 0x7Eu);
                            tw[i].weight[c] = static_cast<uint8_t>(lit ? lane & 0x1Fu : (lane >> 8) & 0x1Fu);
                            if (c < 3)
                                tv[i].rgba[c] = lit ? 0 : static_cast<uint8_t>(lane & 0xFFu);
                        }
                        tv[i].rgba[3] = 0x80;
                    }
                    else if (terrain)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            tv[i].normal[c] = packed[v * 4u + c];
                            tv[i].rgba[c] = static_cast<uint8_t>(packed[v * 4u + c] & 0xFF);
                        }
                        tv[i].rgba[3] = 0x80;
                    }
                    else if (normals)
                    {
                        // Lit by its node's lights from the normal; else VU1 draws the words' low bytes as colour.
                        for (int c = 0; c < 4; ++c)
                        {
                            tv[i].normal[c] = packed[v * 4u + c];
                            tv[i].rgba[c] = static_cast<uint8_t>(packed[v * 4u + c] & 0xFF);
                        }
                    }
                    else
                        std::memcpy(tv[i].rgba, &colour[v * 4u], 4);
                }
                const float b[4] = {std::min({tv[0].st[0], tv[1].st[0], tv[2].st[0]}),
                                    std::min({tv[0].st[1], tv[1].st[1], tv[2].st[1]}),
                                    std::max({tv[0].st[0], tv[1].st[0], tv[2].st[0]}),
                                    std::max({tv[0].st[1], tv[1].st[1], tv[2].st[1]})};
                for (int i = 0; i < 3; ++i)
                {
                    std::memcpy(tv[i].stBounds, b, sizeof(b));
                    out.push_back(tv[i]);
                    if (skinned)
                        skin->push_back(tw[i]);
                }
            }
            vertices = 0;
            uv.clear();
            colour.clear();
            packed.clear();
            position.clear();
            return true;
        };
        size_t pos = 0;
        while (pos + 4u <= vif.size())
        {
            uint32_t cmd = 0;
            std::memcpy(&cmd, &vif[pos], 4);
            pos += 4;
            const uint8_t op = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
            const uint32_t num = (cmd >> 16) & 0xFFu;
            const uint32_t imm = cmd & 0xFFFFu;
            if ((op & 0x60u) == 0x60u)
            {
                const uint32_t vn = (op >> 2) & 3u, vl = op & 3u;
                const uint32_t n = num ? num : 256u;
                const uint32_t vectorBits = vl == 3u ? 16u : (vn + 1u) * (32u >> vl);
                // Vectors read: all of them unless WL > CL (fill mode).
                uint32_t read = n;
                if (wl > cl && wl)
                    read = (n / wl) * cl + std::min(n % wl, cl);
                const size_t bytes = ((static_cast<size_t>(read) * vectorBits + 31u) / 32u) * 4u;
                if (pos + bytes > vif.size())
                    return fail("packet cut short");
                const uint8_t *d = &vif[pos];
                if (vn == 3u && vl == 0u && n == 1u && (imm & 0x3FFu) == (terrain ? 0x28u : 8u))
                {
                    // The chunk's GIF tag (at 8, the terrain program's at 0x28).
                    if (!emit())
                        return fail("chunk incomplete");
                    uint32_t tag = 0;
                    std::memcpy(&tag, d, 4);
                    vertices = tag & 0x7FFFu;
                }
                else if (skinned && vn == 1u && vl == 1u && read == n)
                {
                    const size_t at = uv.size();
                    uv.resize(at + n * 2u);
                    std::memcpy(&uv[at], d, n * 4u);
                }
                else if (skinned && vn == 3u && vl == 0u && read == n)
                {
                    const size_t at = position.size();
                    position.resize(at + n * 4u);
                    std::memcpy(&position[at], d, n * 16u);
                }
                else if (skinned && vn == 3u && vl == 1u && read == n)
                {
                    const size_t at = packed.size();
                    packed.resize(at + n * 4u);
                    std::memcpy(&packed[at], d, n * 8u);
                }
                else if (normals && vn == 3u && vl == 1u && (op & 0x10u) == 0u && read == n)
                {
                    // The normals' V4-16 (unmasked; the UVs' is masked).
                    const size_t at = packed.size();
                    packed.resize(at + n * 4u);
                    std::memcpy(&packed[at], d, n * 8u);
                }
                else if (!terrain && vn == 3u && vl == 1u && read == n)
                {
                    const size_t at = uv.size();
                    uv.resize(at + n * 4u);
                    std::memcpy(&uv[at], d, n * 8u);
                }
                else if (terrain && vn == 1u && vl == 1u && read == n)
                {
                    const size_t at = uv.size();
                    uv.resize(at + n * 2u);
                    std::memcpy(&uv[at], d, n * 4u);
                }
                else if (terrain && vn == 3u && vl == 1u && read == n)
                {
                    const size_t at = packed.size();
                    packed.resize(at + n * 4u);
                    std::memcpy(&packed[at], d, n * 8u);
                }
                else if (!terrain && vn == 3u && vl == 2u && read == n)
                    colour.insert(colour.end(), d, d + n * 4u);
                else if (vn == 2u && vl == 0u && read == n)
                {
                    const size_t at = position.size();
                    position.resize(at + n * 3u);
                    std::memcpy(&position[at], d, n * 12u);
                }
                else
                    return fail("packet has other unpacks");
                pos += bytes;
                continue;
            }
            switch (op)
            {
            case 0x00: // NOP
            case 0x02: // OFFSET
            case 0x03: // BASE
            case 0x04: // ITOP
            case 0x05: // STMOD
            case 0x06: // MSKPATH3
            case 0x07: // MARK
            case 0x10: // FLUSHE
            case 0x11: // FLUSH
            case 0x13: // FLUSHA
                break;
            case 0x01: // STCYCL
                cl = imm & 0xFFu;
                wl = (imm >> 8) & 0xFFu;
                break;
            case 0x14: // MSCAL
            case 0x15: // MSCALF
            case 0x17: // MSCNT
                if (!emit())
                    return fail("chunk incomplete");
                break;
            case 0x20: // STMASK
                pos += 4;
                break;
            case 0x30: // STROW
            case 0x31: // STCOL
                pos += 16;
                break;
            default:
                return fail("packet has other VIFcodes");
            }
        }
        if (!emit())
            return fail("chunk incomplete");
        return true;
    }

    namespace
    {
        bool decodePart(const GuestMemory &m, uint32_t p, RigidMesh &out, std::string *why, PacketKind kind)
        {
            auto fail = [why](const char *reason)
            {
                if (why)
                    *why = reason;
                return false;
            };
            const bool skinned = kind == PacketKind::Skinned || kind == PacketKind::SkinnedPrelit;
            uint8_t materials = 0, bones = 0;
            uint32_t buffers = 0, entries = 0;
            if (!m.u8(p + part::kMaterialCount, materials) || !m.u8(p + part::kBoneCount, bones) ||
                !m.u32(p + part::kBuffers, buffers))
                return fail("part not in memory");
            if (bones && !skinned)
                return fail("bones");
            const uint32_t drawn = (buffers >> 1) & 1u;
            if (!m.u32(p + part::kPacketEntries + drawn * 4u, entries) || !entries)
                return fail("no packet");
            auto mesh = std::make_shared<GSNativeMesh>();
            mesh->id = GSNativeMesh::nextId();
            mesh->source = p;
            out.runs.assign(materials, RigidMesh::Run{});
            out.chunks = 0;
            for (uint32_t k = 0; k < materials; ++k)
            {
                uint32_t packet = 0;
                if (!m.u32(entries + k * kPacketEntryBytes, packet))
                    return fail("packet entries not in memory");
                out.runs[k].first = static_cast<uint32_t>(mesh->vertices.size());
                if (packet && !decodeMaterialPacket(m, packet, mesh->vertices, out.chunks, why, kind,
                                                    skinned ? &mesh->skin : nullptr))
                    return false;
                out.runs[k].count = static_cast<uint32_t>(mesh->vertices.size()) - out.runs[k].first;
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
    }

    bool decodeRigidPart(const GuestMemory &m, uint32_t p, RigidMesh &out, std::string *why, PacketKind kind)
    {
        if (kind == PacketKind::Skinned || kind == PacketKind::SkinnedPrelit)
        {
            if (why)
                *why = "skinned";
            return false;
        }
        return decodePart(m, p, out, why, kind);
    }

    bool decodeSkinnedPart(const GuestMemory &m, uint32_t p, RigidMesh &out, std::string *why, PacketKind kind)
    {
        if (kind != PacketKind::Skinned && kind != PacketKind::SkinnedPrelit)
        {
            if (why)
                *why = "not skinned";
            return false;
        }
        return decodePart(m, p, out, why, kind);
    }

    bool clipDepthRange(const float screen[4][4], const float clip[4][4], float &nearW, float &farW)
    {
        int r = 0;
        for (int k = 1; k < 3; ++k)
            if (std::fabs(screen[k][3]) > std::fabs(screen[r][3]))
                r = k;
        if (std::fabs(screen[r][3]) < 1e-6f)
            return false;
        const float a = clip[r][2] / screen[r][3];
        const float b = clip[3][2] - a * screen[3][3];
        // The other rows must agree (z = a w' + b throughout).
        for (int k = 0; k < 3; ++k)
            if (std::fabs(clip[k][2] - a * screen[k][3]) > 1e-3f * (std::fabs(clip[k][2]) + std::fabs(a * screen[k][3]) + 1e-3f))
                return false;
        if (std::fabs(a + 1.0f) < 1e-9f || std::fabs(1.0f - a) < 1e-9f)
            return false;
        const float w1 = -b / (a + 1.0f), w2 = b / (1.0f - a);
        nearW = std::min(w1, w2);
        farW = std::max(w1, w2);
        return nearW > 0.0f && farW > nearW;
    }

    bool readSkinnedUpload(const GuestMemory &m, uint32_t from, uint32_t to, SkinnedNodeUpload &out, std::string *why)
    {
        auto fail = [why](const char *reason)
        {
            if (why)
                *why = reason;
            return false;
        };
        if (to < from || to - from > 0x10000u)
            return fail("payload range");
        const uint8_t *vif = m.bytes(from, to - from);
        if (!vif && to > from)
            return fail("payload not in memory");
        const size_t size = to - from;
        bool palette = false, block = false;
        size_t pos = 0;
        while (pos + 4u <= size)
        {
            uint32_t cmd = 0;
            std::memcpy(&cmd, vif + pos, 4);
            pos += 4;
            const uint8_t op = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
            const uint32_t num = (cmd >> 16) & 0xFFu;
            const uint32_t imm = cmd & 0xFFFFu;
            if ((op & 0x60u) == 0x60u)
            {
                const uint32_t vn = (op >> 2) & 3u, vl = op & 3u;
                const uint32_t n = num ? num : 256u;
                if (vn != 3u || vl != 0u || (op & 0x10u))
                    return fail("payload has other unpacks");
                if (pos + n * 16u > size)
                    return fail("payload cut short");
                const uint8_t *d = vif + pos;
                pos += n * 16u;
                const uint32_t addr = imm & 0x3FFu;
                if ((imm & 0x8000u) == 0u && addr == 0u && !palette)
                {
                    if (n > static_cast<uint32_t>(GSNativeSkinning::kPaletteRows))
                        return fail("palette too long");
                    std::memcpy(out.palette, d, n * 16u);
                    out.paletteRows = n;
                    palette = true;
                }
                else if ((imm & 0x8000u) != 0u && addr == 0u && n == 16u && !block)
                {
                    std::memcpy(out.screen, d, 64);
                    std::memcpy(out.lightDir, d + 64, 64);
                    std::memcpy(out.lightColour, d + 128, 64);
                    std::memcpy(out.clip, d + 192, 64);
                    block = true;
                }
                else
                    return fail("payload has other unpacks");
                continue;
            }
            switch (op)
            {
            case 0x00: // NOP
            case 0x01: // STCYCL
            case 0x02: // OFFSET
            case 0x03: // BASE
            case 0x04: // ITOP
            case 0x05: // STMOD
            case 0x06: // MSKPATH3
            case 0x07: // MARK
            case 0x10: // FLUSHE
            case 0x11: // FLUSH
            case 0x13: // FLUSHA
                break;
            case 0x14: // MSCAL
            case 0x15: // MSCALF
                if (imm != 0u)
                {
                    // The node's program: its inputs are all there.
                    out.program = imm;
                    if (!palette || !block)
                        return fail(!block ? "no node block" : "no palette");
                    return true;
                }
                break;
            case 0x17: // MSCNT
                break;
            case 0x20: // STMASK
                pos += 4;
                break;
            case 0x30: // STROW
            case 0x31: // STCOL
                pos += 16;
                break;
            default:
                return fail("payload has other VIFcodes");
            }
        }
        if (!palette || !block)
            return fail(!block ? "no node block" : "no palette");
        return true;
    }

    bool xboxCharacterLights(GSNativeSkinning &skin)
    {
        auto is = [](const float *v, float x) { return v[0] == x && v[1] == x && v[2] == x; };
        if (!is(skin.lightColour[0], 0.3f) || !is(skin.lightColour[1], 0.3f) || !is(skin.lightColour[2], 0.3f) ||
            !is(skin.lightColour[3], 0.5f))
            return false;
        static const float kXbox[3][3] = {{1.0f, 0.0f, -0.3f}, {0.0f, -1.0f, 0.3f}, {-0.7f, 0.0f, 0.5f}};
        float m[3][3]; // m[k][c]: component k of Xbox light c, normalised
        for (int c = 0; c < 3; ++c)
        {
            const float *l = kXbox[c];
            const float n = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            for (int k = 0; k < 3; ++k)
                m[k][c] = l[k] / n;
        }
        float u[3][4];
        std::memcpy(u, skin.lightDir, sizeof(u));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                skin.lightDir[r][c] = (u[r][0] * m[0][c] + u[r][1] * m[1][c]) + u[r][2] * m[2][c];
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                skin.lightColour[i][k] = 0.6f;
        skin.maxColour = 128.0f;
        return true;
    }
}
