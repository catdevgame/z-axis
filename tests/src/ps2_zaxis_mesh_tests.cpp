#include "MiniTest.h"
#include "runtime/gs/gs_record.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_zaxis_mesh.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ps2x::zaxis;

namespace
{
    void put32(std::vector<uint8_t> &ram, uint32_t at, uint32_t v)
    {
        std::memcpy(&ram[at], &v, 4);
    }

    // A rigid prelit part with one material whose packet is one chunk of
    // four vertices: two strip starts, then two triangles.
    void buildPart(std::vector<uint8_t> &ram, uint32_t part, uint32_t entries, uint32_t packet)
    {
        ram[part + part::kMaterialCount] = 1;
        ram[part + part::kBoneCount] = 0;
        put32(ram, part + part::kBuffers, 2u); // bit 1: buffer 1 is drawn
        put32(ram, part + part::kPacketEntries + 4u, entries);
        put32(ram, entries, packet);
        std::vector<uint32_t> w;
        // The chunk: GIF tag unpack in the tag's TTE words, then the rest.
        w.push_back(0x01000404u);                  // STCYCL 4,4
        w.push_back(0x6C018008u);                  // UNPACK V4-32 1 at 8 (FLG)
        w.insert(w.end(), {0x8004u, 0x300E0000u, 0x512u, 0u}); // GIF tag: 4 vertices, EOP
        w.push_back(0x20000000u);                  // STMASK
        w.push_back(0x10101010u);
        w.push_back(0x01000103u);                  // STCYCL 3,1
        w.push_back(0x7D048009u);                  // UNPACK V4-16 masked, 4
        const uint16_t uv[4][4] = {{0, 0, 0x1000, 0xC000}, {4096, 0, 0x1000, 0xC000}, {0, 2048, 0x1000, 0x4000}, {4096, 2048, 0x1000, 0x0020}};
        for (const auto &v : uv)
        {
            w.push_back(v[0] | (static_cast<uint32_t>(v[1]) << 16));
            w.push_back(v[2] | (static_cast<uint32_t>(v[3]) << 16));
        }
        w.push_back(0x01000103u);
        w.push_back(0x6E04800Au);                  // UNPACK V4-8, 4
        for (uint32_t i = 0; i < 4; ++i)
            w.push_back(0x7F404040u + i);
        w.push_back(0x01000103u);
        w.push_back(0x7804800Bu);                  // UNPACK V3-32 masked, 4
        const float pos[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
        for (const auto &p : pos)
            for (float f : p)
            {
                uint32_t b;
                std::memcpy(&b, &f, 4);
                w.push_back(b);
            }
        w.push_back(0x14000002u);                  // MSCAL 2
        while ((w.size() - 2u) % 4u)
            w.push_back(0u);
        const uint32_t qwc = static_cast<uint32_t>((w.size() - 2u) / 4u);
        put32(ram, packet, qwc | (6u << 28));     // RET
        put32(ram, packet + 4u, 0u);
        for (size_t i = 0; i < w.size(); ++i)
            put32(ram, packet + 8u + static_cast<uint32_t>(i) * 4u, w[i]);
    }
    uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    // A skinned part (type 8, 21 bones) with one material whose packet is
    // one chunk of four vertices as FUN_0010BFE0 builds it: UV V2-16 masked
    // at 9, position V4-32 at 10 (bone addresses in the low bits, bit 0 of
    // x: draw), the normal V4-16 at 11 (weights in the low 5 bits).
    void buildSkinnedPart(std::vector<uint8_t> &ram, uint32_t part, uint32_t entries, uint32_t packet, bool prelit)
    {
        ram[part + part::kMaterialCount] = 1;
        ram[part + part::kBoneCount] = 21;
        put32(ram, part + part::kBuffers, 0u); // buffer 0 is drawn
        put32(ram, part + part::kPacketEntries, entries);
        put32(ram, entries, packet);
        std::vector<uint32_t> w;
        w.push_back(0x01000404u);
        w.push_back(0x6C018008u);                  // UNPACK V4-32 1 at 8: the GIF tag
        w.insert(w.end(), {0x8004u, 0x300E0000u, 0x512u, 0u});
        w.push_back(0x20000000u);                  // STMASK: z, w from ROW
        w.push_back(0x50505050u);
        w.push_back(0x01000103u);                  // STCYCL 3,1
        w.push_back(0x75048009u);                  // UNPACK V2-16 masked, 4 at 9
        for (uint32_t i = 0; i < 4; ++i)
            w.push_back((i * 1024u) | (2048u << 16));
        w.push_back(0x20000000u);
        w.push_back(0x40404040u);
        w.push_back(0x01000103u);
        w.push_back(0x6C04800Au);                  // UNPACK V4-32, 4 at 10
        // Vertex i: bones 1 and 2 (addresses 0 and 4), x bit 0 set from
        // vertex 2 on except vertex 3 (a strip restart).
        for (uint32_t i = 0; i < 4; ++i)
        {
            const uint32_t draw = i == 2u ? 1u : 0u;
            w.push_back((fbits(static_cast<float>(i)) & ~0x7Fu) | 0u | draw);
            w.push_back((fbits(1.0f) & ~0x7Fu) | 4u);
            w.push_back((fbits(2.0f) & ~0x7Fu) | 0x3Fu);
            w.push_back((fbits(1.0f) & ~0x7Fu) | 0x3Fu);
        }
        w.push_back(0x01000103u);
        w.push_back(0x6D04800Bu);                  // UNPACK V4-16, 4 at 11
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (prelit) // colour halved, weights in bits 8-12: 12/16 and 4/16
            {
                w.push_back((0x0C00u | 0x40u) | ((0x0400u | 0x20u) << 16));
                w.push_back(0x10u | (0x0000u << 16));
            }
            else // normal (0, 0, 32767) & ~0x1F, weights 12 and 4
            {
                w.push_back(12u | (4u << 16));
                w.push_back((0x7FE0u | 0u) | (0u << 16));
            }
        }
        w.push_back(0x1400000Au);                  // MSCAL (the chunk program)
        while ((w.size() - 2u) % 4u)
            w.push_back(0u);
        const uint32_t qwc = static_cast<uint32_t>((w.size() - 2u) / 4u);
        put32(ram, packet, qwc | (6u << 28));
        put32(ram, packet + 4u, 0u);
        for (size_t i = 0; i < w.size(); ++i)
            put32(ram, packet + 8u + static_cast<uint32_t>(i) * 4u, w[i]);
    }
}

void register_ps2_zaxis_mesh_tests()
{
    MiniTest::Case("ZAxisMesh", [](TestCase &tc)
    {
        tc.Run("a part's built packet decodes to the triangles VU1 draws", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
            buildPart(ram, 0x1000u, 0x2000u, 0x3000u);
            RigidMesh mesh;
            std::string why;
            t.IsTrue(decodeRigidPart(GuestMemory{ram.data()}, 0x1000u, mesh, &why), "decoded: " + why);
            if (!mesh.mesh)
                return;
            t.Equals(mesh.chunks, 1u, "one chunk");
            t.Equals(mesh.triangles, 2u, "two triangles (the strip starts draw none)");
            t.Equals(mesh.runs.size(), size_t(1), "one material run");
            t.Equals(mesh.runs[0].count, 6u, "six vertices");
            const auto &v = mesh.mesh->vertices;
            t.Equals(v[2].pos[1], 1.0f, "triangle 0 ends at vertex 2");
            t.Equals(v[3].pos[0], 1.0f, "triangle 1 starts at vertex 1");
            t.Equals(v[2].st[1], 0.5f, "t = 2048 / 4096");
            t.Equals(v[5].rgba[0], uint8_t(0x43), "colour bytes as unpacked");
            t.Equals(v[0].stBounds[2], 1.0f, "triangle 0's s range ends at 1");
            t.Equals(v[0].stBounds[3], 0.5f, "triangle 0's t range ends at 0.5");
            t.Equals(mesh.mesh->boundsMax[1], 1.0f, "mesh box");
        });

        tc.Run("a skinned packet decodes with its bones and weights", [](TestCase &t)
        {
            for (int prelit = 0; prelit < 2; ++prelit)
            {
                std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
                buildSkinnedPart(ram, 0x1000u, 0x2000u, 0x3000u, prelit != 0);
                RigidMesh mesh;
                std::string why;
                const PacketKind kind = prelit ? PacketKind::SkinnedPrelit : PacketKind::Skinned;
                t.IsTrue(!decodeRigidPart(GuestMemory{ram.data()}, 0x1000u, mesh, &why), "not as a rigid part");
                t.IsTrue(decodeSkinnedPart(GuestMemory{ram.data()}, 0x1000u, mesh, &why, kind), "decoded: " + why);
                if (!mesh.mesh)
                    return;
                const auto &v = mesh.mesh->vertices;
                const auto &sk = mesh.mesh->skin;
                t.Equals(mesh.triangles, 1u, "one triangle: x bit 0 of vertex 2 only");
                t.Equals(sk.size(), v.size(), "a bone record per vertex");
                if (sk.size() < 3u)
                    return;
                t.Equals(sk[0].bone[0], uint8_t(0), "bone 0 address");
                t.Equals(sk[0].bone[1], uint8_t(4), "bone 1 address");
                t.Equals(sk[0].bone[2], uint8_t(0x3E), "no bone: 0x3F & 0x7E");
                t.Equals(sk[0].weight[0], uint8_t(12), "weight 0");
                t.Equals(sk[0].weight[1], uint8_t(4), "weight 1");
                t.Equals(v[1].st[0], 0.25f, "s = 1024 / 4096");
                t.Equals(v[1].st[1], 0.5f, "t = 2048 / 4096");
                if (prelit)
                    t.Equals(v[0].rgba[0], uint8_t(0x40), "colour: the low byte");
                else
                    t.Equals(v[0].normal[2], int16_t(0x7FE0), "normal as unpacked");
                t.Equals(v[2].rgba[3], uint8_t(0x80), "alpha 0x80");
            }
        });

        tc.Run("a skinned node's upload: palette, matrices, lights, program", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
            std::vector<uint32_t> w = {0u, 0u, 0x13000000u, 0x07000001u, 0x80000000u, 0x13000000u};
            w.push_back(0x6C080000u); // UNPACK V4-32 8 at 0: two bones
            for (uint32_t i = 0; i < 32u; ++i)
                w.push_back(fbits(static_cast<float>(i)));
            w.push_back(0x6C108000u); // UNPACK V4-32 16 at TOPS
            for (uint32_t i = 0; i < 64u; ++i)
                w.push_back(fbits(100.0f + static_cast<float>(i)));
            w.push_back(0x1400000Au); // MSCAL 0x0A
            w.push_back(0x14000000u); // MSCAL 0
            for (size_t i = 0; i < w.size(); ++i)
                put32(ram, 0x4000u + static_cast<uint32_t>(i) * 4u, w[i]);
            auto up = std::make_unique<SkinnedNodeUpload>();
            std::string why;
            const bool ok = readSkinnedUpload(GuestMemory{ram.data()}, 0x4000u, 0x4000u + static_cast<uint32_t>(w.size()) * 4u,
                                              *up, &why);
            t.IsTrue(ok, "read: " + why);
            t.Equals(up->paletteRows, 8u, "palette rows");
            t.Equals(up->palette[7][3], 31.0f, "palette's last word");
            t.Equals(up->screen[0][0], 100.0f, "screen matrix at TOPS");
            t.Equals(up->lightDir[0][0], 116.0f, "light directions at +4");
            t.Equals(up->lightColour[3][2], 146.0f, "ambient at +11");
            t.Equals(up->clip[3][3], 163.0f, "clip matrix at +12");
            t.Equals(up->program, 0x0Au, "MSCAL type + 2");
        });

        tc.Run("skinning and lights as VU1 0x08 computes them", [](TestCase &t)
        {
            GSNativeSkinning sk;
            // Bone at 0: identity, translation (1, 2, 3); bone at 4: scale 2.
            for (int r = 0; r < 3; ++r)
            {
                sk.palette[r][r] = 1.0f;
                sk.palette[4 + r][r] = 2.0f;
            }
            sk.palette[3][0] = 1.0f;
            sk.palette[3][1] = 2.0f;
            sk.palette[3][2] = 3.0f;
            sk.lit = true;
            sk.lightDir[2][0] = 1.0f;     // light 0 along +z
            sk.lightColour[0][0] = 0.5f;  // light 0 red 0.5
            sk.lightColour[3][1] = 0.25f; // ambient green 0.25
            sk.lightColour[3][2] = 3.0f;  // ambient blue past 255
            GSNativeVertex v;
            v.pos[0] = 1.0f;
            v.pos[1] = 1.0f;
            v.pos[2] = 1.0f;
            v.normal[2] = 16384; // 0.5
            GSNativeSkinWeights w;
            w.bone[0] = 0;
            w.bone[1] = 4;
            w.weight[0] = 8;
            w.weight[1] = 8;
            float p[3];
            uint8_t c[4];
            gsNativeSkinVertex(sk, v, w, p, c);
            t.Equals(p[0], 2.0f, "x = 0.5 * (1 + 1) + 0.5 * 2");
            t.Equals(p[2], 3.0f, "z = 0.5 * (1 + 3) + 0.5 * 2");
            t.Equals(c[0], uint8_t(32), "red: 128 * 0.5 * 0.5");
            t.Equals(c[1], uint8_t(32), "green: 128 * 0.25");
            t.Equals(c[2], uint8_t(255), "blue: at most 255");
            t.Equals(c[3], uint8_t(0x80), "alpha 0x80");
            sk.maxColour = 128.0f;
            gsNativeSkinVertex(sk, v, w, p, c);
            t.Equals(c[2], uint8_t(128), "blue: at most 128 with the Xbox's cap");
            t.Equals(c[0], uint8_t(32), "red below the cap unchanged");
            const float in[4] = {2.0f, -3.0f, 0.5f, 1.0f};
            t.Equals(gsNativeClipFlags(in), uint8_t(1u | 8u), "+x and -y outside");
            const float behind[4] = {0.0f, 0.0f, -0.1f, -1.0f};
            t.Equals(gsNativeClipFlags(behind), uint8_t(32u), "-z against max(w, 0)");
        });

        tc.Run("Xbox character lights: the Xbox's own constants from the PS2's upload", [](TestCase &t)
        {
            // The Ape at the Movie Lot start: the PS2's upload (VU1 TOPS + 4,
            // + 8) and what the Xbox gave its vertex shader for the same
            // frame (constants 176, 180, 184: L, dotted with the normal as -L).
            GSNativeSkinning sk;
            sk.lit = true;
            const float u[3][4] = {{0, 0, 1, 0}, {0, -1, 0, 0}, {-1, 0, 0, 0}};
            std::memcpy(sk.lightDir, u, sizeof(u));
            for (int i = 0; i < 3; ++i)
                sk.lightColour[i][0] = sk.lightColour[i][1] = sk.lightColour[i][2] = 0.3f;
            sk.lightColour[3][0] = sk.lightColour[3][1] = sk.lightColour[3][2] = 0.5f;
            t.IsTrue(xboxCharacterLights(sk), "the PS2's default lights are replaced");
            const float xbox[3][3] = {{0.2873f, 0.0f, 0.9578f}, {-0.2873f, -0.9578f, 0.0f}, {-0.5812f, 0.0f, -0.8137f}};
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                    t.IsTrue(std::fabs(sk.lightDir[k][c] + xbox[c][k]) < 1e-3f, "light direction as the Xbox's");
            t.Equals(sk.lightColour[1][2], 0.6f, "colours 0.6");
            t.Equals(sk.lightColour[3][0], 0.5f, "ambient 0.5");
            t.Equals(sk.maxColour, 128.0f, "the Xbox's cap");
            GSNativeSkinning own;
            own.lightColour[0][0] = 0.7f;
            t.IsTrue(!xboxCharacterLights(own), "a node's own lights are kept");
            t.Equals(own.maxColour, 255.0f, "and VU1's cap");
        });

        tc.Run("native meshes and draws round-trip through a GS trace's events", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
            buildSkinnedPart(ram, 0x1000u, 0x2000u, 0x3000u, false);
            RigidMesh mesh;
            std::string why;
            t.IsTrue(decodeSkinnedPart(GuestMemory{ram.data()}, 0x1000u, mesh, &why, PacketKind::Skinned), "decoded");
            if (!mesh.mesh)
                return;
            const std::vector<uint8_t> m = GSRecord::encodeNativeMesh(*mesh.mesh);
            const auto back = GSRecord::decodeNativeMesh(m.data(), static_cast<uint32_t>(m.size()));
            t.IsTrue(back != nullptr, "mesh decoded");
            if (!back)
                return;
            t.Equals(back->id, mesh.mesh->id, "id");
            t.Equals(back->vertices.size(), mesh.mesh->vertices.size(), "vertices");
            t.Equals(back->skin.size(), mesh.mesh->skin.size(), "bones");
            t.IsTrue(std::memcmp(back->vertices.data(), mesh.mesh->vertices.data(),
                                 back->vertices.size() * sizeof(GSNativeVertex)) == 0, "vertex bytes");
            t.IsTrue(GSRecord::decodeNativeMesh(m.data(), static_cast<uint32_t>(m.size() - 1u)) == nullptr, "cut short: refused");

            GSNativeDraw d;
            d.mesh = mesh.mesh;
            d.first = 0;
            d.count = 3;
            d.transform[3][1] = 7.0f;
            d.dropFlags = 0x3F;
            d.affineUV = true;
            d.uvFromNormal = true;
            d.alpha = 0x40;
            auto skin = std::make_shared<GSNativeSkinning>();
            skin->lit = true;
            skin->palette[127][3] = 5.0f;
            skin->lightColour[3][0] = 0.5f;
            d.skin = skin;
            const std::vector<uint8_t> e = GSRecord::encodeNativeDraw(d);
            GSNativeDraw r;
            uint64_t id = 0;
            t.IsTrue(GSRecord::decodeNativeDraw(e.data(), static_cast<uint32_t>(e.size()), r, id), "draw decoded");
            t.Equals(id, mesh.mesh->id, "mesh id");
            t.Equals(r.count, 3u, "count");
            t.Equals(r.transform[3][1], 7.0f, "transform");
            t.Equals(r.dropFlags, uint8_t(0x3F), "drop flags");
            t.IsTrue(r.affineUV && r.uvFromNormal, "flags");
            t.Equals(r.alpha, 0x40, "alpha");
            t.IsTrue(r.skin && r.skin->lit && r.skin->palette[127][3] == 5.0f && r.skin->lightColour[3][0] == 0.5f,
                     "skinning");
        });

        tc.Run("a clip volume's depth range from the screen and clip matrices", [](TestCase &t)
        {
            // A camera turned 30 degrees about y and moved; screen w' = view
            // z; clip z = (f + n) / (f - n) w' - 2 f n / (f - n): near 1.25,
            // far 600 (Dave Mirra BMX 2's).
            const float n = 1.25f, f = 600.0f, c = 0.8660254f, s = 0.5f;
            const float view[4][4] = {{c, 0, -s, 0}, {0, 1, 0, 0}, {s, 0, c, 0}, {10, -3, 40, 1}};
            const float a = (f + n) / (f - n), b = -2.0f * f * n / (f - n);
            float screen[4][4], clip[4][4];
            for (int r = 0; r < 4; ++r)
            {
                screen[r][0] = 300.0f * view[r][0] + 2048.0f * view[r][2];
                screen[r][1] = 300.0f * view[r][1] + 2048.0f * view[r][2];
                screen[r][2] = 5.0f * view[r][2] + 7.0f * view[r][3];
                screen[r][3] = view[r][2];
                clip[r][0] = 1.2f * view[r][0];
                clip[r][1] = 1.6f * view[r][1];
                clip[r][2] = a * view[r][2] + b * view[r][3];
                clip[r][3] = view[r][2];
            }
            float nearW = 0, farW = 0;
            t.IsTrue(clipDepthRange(screen, clip, nearW, farW), "a projection");
            t.IsTrue(std::fabs(nearW - n) < 1e-3f, "near 1.25: " + std::to_string(nearW));
            t.IsTrue(std::fabs(farW - f) < 0.1f, "far 600: " + std::to_string(farW));
            clip[0][2] += 3.0f; // z depending on x as well
            t.IsTrue(!clipDepthRange(screen, clip, nearW, farW), "not a projection along w");
        });

        tc.Run("other VIFcodes in a packet are refused", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0);
            buildPart(ram, 0x1000u, 0x2000u, 0x3000u);
            put32(ram, 0x3000u + 8u, 0x4A000000u);           // MPG as the first VIFcode
            RigidMesh mesh;
            std::string why;
            t.IsTrue(!decodeRigidPart(GuestMemory{ram.data()}, 0x1000u, mesh, &why), "refused");
        });
    });
}
