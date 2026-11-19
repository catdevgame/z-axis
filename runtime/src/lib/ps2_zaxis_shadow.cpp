#include "runtime/ps2_zaxis_shadow.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>

namespace ps2x::zaxis
{
    namespace
    {
        bool fail(std::string *why, const char *reason)
        {
            if (why)
                *why = reason;
            return false;
        }

        constexpr uint32_t kVuQwords = 1024;
        constexpr uint32_t kProgramKick = 0x24;   // XTOP, XGKICK: a ready GIF packet
        constexpr uint32_t kProgramVolume = 0x1C; // the volume
        constexpr uint32_t kFirstPoint = 0x20;    // the silhouette's VU1 address (after ITOP)

        // The chain from `chain` as VIF1 reads it: per tag its upper half
        // (VIFcodes) and its data, up to the RET (or END) that ends it.
        bool chainStream(const GuestMemory &m, uint32_t chain, std::vector<uint8_t> &out, std::string *why)
        {
            uint32_t at = chain & 0x0FFFFFF0u;
            for (int tags = 0; tags < 64; ++tags)
            {
                const uint8_t *tag = m.bytes(at, 16);
                if (!tag)
                    return fail(why, "tag not in memory");
                uint64_t lo = 0;
                std::memcpy(&lo, tag, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>((lo >> 32) & 0x7FFFFFF0u);
                out.insert(out.end(), tag + 8, tag + 16);
                if (out.size() + qwc * 16u > 0x40000u)
                    return fail(why, "chain too long");
                const uint32_t dataAt = (id == 0u || id == 3u || id == 4u) ? addr : at + 16u;
                if (qwc)
                {
                    const uint8_t *d = m.bytes(dataAt, qwc * 16u);
                    if (!d)
                        return fail(why, "data not in memory");
                    out.insert(out.end(), d, d + qwc * 16u);
                }
                switch (id)
                {
                case 1: // cnt
                    at = at + 16u + qwc * 16u;
                    break;
                case 2: // next
                    at = addr;
                    break;
                case 3: // ref
                case 4: // refs
                    at = at + 16u;
                    break;
                case 0: // refe
                case 6: // ret
                case 7: // end
                    return true;
                default:
                    return fail(why, "a call in the chain");
                }
            }
            return fail(why, "too many tags");
        }

        struct Projected
        {
            float s[3];
            uint32_t q[4];
        };

        // VU1 0x1C's transform of a point: to the camera (row 3 + x row 0
        // + y row 1 + z row 2, in that order), z at least the near clip, to
        // the screen, divided by w, clamped, FTOI4.
        Projected project(const ShadowVolumePacket &p, const float v[3])
        {
            float c[3];
            for (int k = 0; k < 3; ++k)
            {
                float acc = p.view[3][k];
                acc += p.view[0][k] * v[0];
                acc += p.view[1][k] * v[1];
                c[k] = acc + p.view[2][k] * v[2];
            }
            c[2] = std::max(c[2], p.nearClip);
            float h[4];
            for (int k = 0; k < 4; ++k)
            {
                float acc = p.screen[3][k];
                acc += p.screen[0][k] * c[0];
                acc += p.screen[1][k] * c[1];
                h[k] = acc + p.screen[2][k] * c[2];
            }
            const float q = 1.0f / h[3];
            Projected out{};
            for (int k = 0; k < 3; ++k)
            {
                float s = h[k] * q;
                s = std::max(s, 0.0f);
                if (k < 2)
                    s = std::min(s, 4095.0f);
                out.s[k] = s;
                out.q[k] = static_cast<uint32_t>(static_cast<int32_t>(s * 16.0f));
            }
            out.q[3] = 0x3F800000u; // w: the float 1.0 (FTOI4 writes x y z only)
            return out;
        }

        void put(std::vector<uint8_t> &gif, const void *data, size_t n)
        {
            const uint8_t *b = static_cast<const uint8_t *>(data);
            gif.insert(gif.end(), b, b + n);
        }
    }

    size_t gifPacketBytes(const uint8_t *data, size_t size)
    {
        size_t at = 0;
        for (int tags = 0; tags < 256; ++tags)
        {
            if (at + 16u > size)
                return 0;
            uint64_t tag = 0;
            std::memcpy(&tag, data + at, 8);
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
            const bool eop = (tag >> 15) & 1u;
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
            if (nreg == 0)
                nreg = 16;
            size_t bytes = 0;
            if (flg == 0)
                bytes = size_t{nloop} * nreg * 16u;
            else if (flg == 1)
                bytes = (size_t{nloop} * nreg * 8u + 15u) & ~size_t{15};
            else
                bytes = size_t{nloop} * 16u;
            at += 16u + bytes;
            if (at > size)
                return 0;
            if (eop)
                return at;
        }
        return 0;
    }

    bool readShadowVolumePacket(const GuestMemory &m, uint32_t chain, ShadowVolumePacket &out, std::string *why)
    {
        std::vector<uint8_t> vif;
        if (!chainStream(m, chain, vif, why))
            return false;
        // VU1's data memory as the packet fills it: the absolute unpacks,
        // and the ones from TOPS (each kick's GIF packet) apart.
        thread_local std::vector<uint8_t> vu, top;
        vu.assign(kVuQwords * 16u, 0);
        top.assign(kVuQwords * 16u, 0);
        std::bitset<kVuQwords> written;
        bool topWritten = false, volume = false;
        int kicks = 0;
        uint32_t itop = 0;
        ShadowVolumePacket p;
        size_t pos = 0;
        const size_t size = vif.size();
        while (pos + 4u <= size)
        {
            uint32_t cmd = 0;
            std::memcpy(&cmd, vif.data() + pos, 4);
            pos += 4;
            const uint8_t op = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
            const uint32_t num = (cmd >> 16) & 0xFFu;
            const uint32_t imm = cmd & 0xFFFFu;
            if ((op & 0x60u) == 0x60u)
            {
                const uint32_t vn = (op >> 2) & 3u, vl = op & 3u;
                const uint32_t n = num ? num : 256u;
                if (vn != 3u || vl != 0u || (op & 0x10u))
                    return fail(why, "an unpack other than V4-32");
                if (pos + n * 16u > size)
                    return fail(why, "an unpack cut short");
                const bool fromTops = (imm & 0x8000u) != 0u;
                const uint32_t addr = imm & 0x3FFu;
                for (uint32_t k = 0; k < n; ++k)
                {
                    const uint32_t a = (addr + k) & (kVuQwords - 1u);
                    std::memcpy((fromTops ? top : vu).data() + 16u * a, vif.data() + pos + 16u * k, 16);
                    if (fromTops)
                        topWritten = true;
                    else
                        written.set(a);
                }
                pos += n * 16u;
                continue;
            }
            switch (op)
            {
            case 0x00: // NOP
            case 0x01: // STCYCL
            case 0x02: // OFFSET
            case 0x03: // BASE
            case 0x05: // STMOD
            case 0x06: // MSKPATH3
            case 0x07: // MARK
            case 0x10: // FLUSHE
            case 0x11: // FLUSH
            case 0x13: // FLUSHA
                break;
            case 0x04: // ITOP
                itop = imm & 0x3FFu;
                break;
            case 0x20: // STMASK
                pos += 4;
                break;
            case 0x30: // STROW
            case 0x31: // STCOL
                pos += 16;
                break;
            case 0x14: // MSCAL
            case 0x15: // MSCALF
                if (imm == kProgramKick)
                {
                    if (!topWritten)
                        return fail(why, "a kick of nothing");
                    const size_t bytes = gifPacketBytes(top.data(), top.size());
                    if (!bytes)
                        return fail(why, "a kick that is not a GIF packet");
                    std::vector<uint8_t> &dst = volume ? p.after : p.before;
                    if (!dst.empty() || kicks >= 2)
                        return fail(why, "more GS state kicks than two");
                    dst.assign(top.begin(), top.begin() + static_cast<long>(bytes));
                    ++kicks;
                    topWritten = false;
                }
                else if (imm == kProgramVolume)
                {
                    if (volume || p.before.empty())
                        return fail(why, "a second volume, or one before its GS state");
                    for (uint32_t k = 0; k <= 14u; ++k)
                        if (!written[(itop + k) & (kVuQwords - 1u)])
                            return fail(why, "the volume's parameters not all there");
                    const auto q = [&](uint32_t k) { return vu.data() + 16u * ((itop + k) & (kVuQwords - 1u)); };
                    std::memcpy(&p.nearClip, q(0), 4);
                    std::memcpy(&p.pointCount, q(0) + 4, 4);
                    std::memcpy(&p.scale, q(0) + 8, 4);
                    std::memcpy(p.light, q(1), 16);
                    std::memcpy(p.colourTag, q(2), 16);
                    std::memcpy(p.colour[0], q(3), 16);
                    std::memcpy(p.colour[1], q(5), 16);
                    std::memcpy(p.triangleTag, q(6), 16);
                    for (int r = 0; r < 4; ++r)
                    {
                        std::memcpy(p.view[r], q(7u + r), 16);
                        std::memcpy(p.screen[r], q(11u + r), 16);
                    }
                    if (std::memcmp(q(2), q(4), 16) != 0)
                        return fail(why, "two RGBAQ tags");
                    if (p.pointCount % 2u || kFirstPoint + p.pointCount > kVuQwords)
                        return fail(why, "an odd point count");
                    for (uint32_t k = 0; k < p.pointCount; ++k)
                    {
                        std::array<float, 4> v;
                        std::memcpy(v.data(), q(kFirstPoint + k), 16);
                        p.points.push_back(v);
                    }
                    volume = true;
                }
                else
                    return fail(why, "another microprogram");
                break;
            case 0x17: // MSCNT
                return fail(why, "MSCNT");
            case 0x50: // DIRECT
            case 0x51: // DIRECTHL
                return fail(why, "DIRECT GS data");
            default:
                return fail(why, "an unknown VIFcode");
            }
        }
        if (!volume || p.after.empty())
            return fail(why, "no volume, or no GS state after it");
        p.itop = itop;
        out = std::move(p);
        return true;
    }

    size_t shadowVolumeGif(const ShadowVolumePacket &p, const std::vector<std::array<float, 3>> &points,
                           std::vector<uint8_t> &gif)
    {
        const Projected light = project(p, p.light);
        // Each edge's triangle once (the program does it in each pass).
        struct Triangle
        {
            Projected a, b;
            bool firstPass;
        };
        std::vector<Triangle> triangles;
        triangles.reserve(points.size() / 2u);
        for (size_t e = 0; e + 1u < points.size(); e += 2)
        {
            float ext[2][3];
            for (int k = 0; k < 2; ++k)
                for (int c = 0; c < 3; ++c)
                {
                    const float v = points[e + k][c];
                    ext[k][c] = v - (p.light[c] - v) * p.scale;
                }
            Triangle t{project(p, ext[0]), project(p, ext[1]), false};
            // (A' - B') x (L - B'), z (OPMULA / OPMSUB); the MAC's z sign.
            const float d1x = t.a.s[0] - t.b.s[0], d1y = t.a.s[1] - t.b.s[1];
            const float d2x = light.s[0] - t.b.s[0], d2y = light.s[1] - t.b.s[1];
            t.firstPass = std::signbit(d1x * d2y - d2x * d1y);
            triangles.push_back(t);
        }
        gif.reserve(gif.size() + 64u + triangles.size() * 64u);
        for (int pass = 0; pass < 2; ++pass)
        {
            put(gif, p.colourTag, 16);
            put(gif, p.colour[pass], 16);
            for (const Triangle &t : triangles)
            {
                if (t.firstPass != (pass == 0))
                    continue;
                put(gif, p.triangleTag, 16);
                put(gif, light.q, 16);
                put(gif, t.a.q, 16);
                put(gif, t.b.q, 16);
            }
        }
        return triangles.size();
    }
}
