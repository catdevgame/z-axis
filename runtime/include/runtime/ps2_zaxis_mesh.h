#pragma once

// Z-Axis engine (ZGE) meshes as the game holds them in RAM, made into
// native GS meshes (runtime/gs/gs_native.h). Aggressive Inline
// (SLUS-20327); layouts from the game's loaders and its VIF packet builders.
//
// A level mesh is a "part" (the GEO file's TGPT chunk at run time, 0x68
// bytes):
//
//   part +0x04  positions   stream of float4 (w = 1)
//   part +0x14  UVs         indexed stream: s16 pairs, 4096 = 1.0
//   part +0x18  colours     indexed stream: RGBA8
//   part +0x24  groups      TGFM strips; freed once the packet is built
//   part +0x34  u16 vertex count, +0x38 u16 group count, +0x3C u8 bone
//               count, +0x3D u8 material count, +0x3E u16 flags
//   part +0x50/+0x54  per packet buffer: the built VIF packet's material
//               entries ({u32 packet (0: none), u16 VU address, u16}, one
//               per material), +0x64 bit 1 the buffer being drawn
//   part +0x60  u32: the render type in the top 6 bits (4 rigid prelit,
//               0x18 sky, 0x20 lit by normals, 8 / 0xC skinned)
//
// The packet (built once, on the part's first draw, by FUN_00109178 /
// FUN_0010CBE0 from the streams and the groups) is what VU1 is given, and it
// stays in RAM while the part does: per material, a DMA chain (one RET tag
// whose upper 64 bits carry two VIFcodes, CHCR.TTE) of chunks, each one
// strip of at most 60 vertices (FUN_00108E70: a group is cut there, every
// chunk a new strip):
//
//   UNPACK V4-32 1 at 8      the GIF tag (NLOOP = vertices, EOP)
//   STMASK, STCYCL 1 of 3    every vertex 3 qwords: UV, colour, position
//   UNPACK V4-16 (masked)    s, t times 4096 (ITOF12), z from ROW (1.0),
//                            w: 0x8000 no triangle (strip start), 0x4000 /
//                            0x20 winding (only the clipper culls by it)
//   UNPACK V4-8              RGBA, every byte halved (0x80 = 1.0)
//   UNPACK V3-32             x, y, z
//   MSCAL 2 / MSCNT          the microprogram draws the strip
//
// The prelit microprograms (VU1 0x04, 0x06, 0x1E) draw vertex k >= 2 of a
// chunk as the triangle (k - 2, k - 1, k) unless its w has 0x8000.

#include "runtime/gs/gs_native.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zx::zaxis
{
    // Field offsets of the run-time part (TGPT).
    namespace part
    {
        constexpr uint32_t kPositions = 0x04;
        constexpr uint32_t kNormals = 0x10;   // stream (null: the skinned packet carries colours)
        constexpr uint32_t kUvs = 0x14;
        constexpr uint32_t kColours = 0x18;
        constexpr uint32_t kBones = 0x1C;     // skinned: {bone record *list, s32 cursor, u16 count}; record +0x10 offset, +0x20 u16 pose index
        constexpr uint32_t kSkinWeights = 0x20; // skinned: 8 bytes per vertex {bone, weight << 4} x 4 (bone 0: none)
        constexpr uint32_t kGroups = 0x24;
        constexpr uint32_t kVertexCount = 0x34;
        constexpr uint32_t kGroupCount = 0x38;
        constexpr uint32_t kBoneCount = 0x3C;
        constexpr uint32_t kMaterialCount = 0x3D;
        constexpr uint32_t kFlags = 0x3E;
        constexpr uint32_t kPacketEntries = 0x50; // [2]: per buffer
        constexpr uint32_t kRenderType = 0x60;    // >> 26
        constexpr uint32_t kBuffers = 0x64;       // bit 1: the buffer drawn
        constexpr uint32_t kStreamData = 0x10;
        constexpr uint32_t kStreamIndexFlags = 0x01; // bit 0: 16-bit indices
        constexpr uint32_t kStreamIndexTable = 0x08;
    }
    // A material entry of the built packet: {u32 packet (0: none), u16 VU
    // address, u16}.
    constexpr uint32_t kPacketEntryBytes = 8;

    // Render types (part +0x60 >> 26).
    enum class RenderType : uint8_t
    {
        RigidPrelit = 0x04,
        Skinned = 0x08,
        SkinnedPrelit = 0x0C,
        Sky = 0x18,
        Lit = 0x20,
    };

    // The packet builder's chunk: entries per strip sent to VU1 (its double
    // buffers of 0x1E0 qwords).
    constexpr uint32_t kChunkEntries = 60;

    // Reads guest memory (the runtime's RAM, or a test's buffer).
    struct GuestMemory
    {
        const uint8_t *rdram = nullptr;
        // A pointer to n bytes at address, or nullptr if they are not all
        // in memory.
        const uint8_t *bytes(uint32_t address, size_t n) const;
        bool u8(uint32_t address, uint8_t &out) const;
        bool u16(uint32_t address, uint16_t &out) const;
        bool u32(uint32_t address, uint32_t &out) const;
        bool f32(uint32_t address, float &out) const;
    };

    // The two packet layouts the builders make (3 qwords per vertex):
    // - RigidPrelit (FUN_0010CBE0, types 4 / 0x18): UV V4-16 (s, t, ROW,
    //   flags), colour V4-8 (halved), position V3-32.
    // - RigidNormals (ZGE4's buildRigid for a part with normals): the same
    //   but a V4-16 of the normal * 32768 (truncated, clamped to +-32767;
    //   z's low 5 bits clear; w 0, rigidNormalWord) where the colour goes.
    //   With its node's lights the program lights the vertex from it
    //   (GSNativeLights); without, it draws the words' low bytes as the
    //   colour.
    // - Terrain (FUN_0010B2B8, type 0x20, "lit by normals"; recomp
    //   stage4 notes): UV V2-16 (s, t), a V4-16 of the normal's high byte
    //   and the halved colour's low byte per x/y/z and the flags << 8 in
    //   w (0x80 strip start), position V3-32. Its microprogram draws the
    //   colour bytes with alpha 0x80 (no lighting); the normal moves grass
    //   shells.
    // - Skinned (FUN_0010BFE0, types 8 / 0xC): UV V2-16 masked (s, t; z, w
    //   from ROW: 1.0) at 9, position V4-32 at 10, a V4-16 at 11. The
    //   position's low 7 mantissa bits carry, per component x, y, z, w (w
    //   1.0 otherwise), the VU1 address of bone 0..3's matrix ((bone - 1) *
    //   4, 0x3F for none) and in x's bit 0 whether the vertex ends a
    //   triangle (FUN_0010B958). The V4-16 is the normal * 32767 with the
    //   low 5 bits the bone weights in sixteenths (type 8, FUN_0010B6B8:
    //   x 16 - the others, y, z, w), or the halved colour with the weights
    //   in bits 8-12 (type 0xC, FUN_0010B7F8).
    enum class PacketKind : uint8_t
    {
        RigidPrelit,
        Terrain,
        Skinned,       // type 8: normals
        SkinnedPrelit, // type 0xC: colours
        // ZGE4's rigid packet of a part with normals (BMX XXX's buildRigid, types 4 / 0x18): the normal where the
        // colour goes.
        RigidNormals,
    };

    // What a game's rigid packet builder (buildRigid) and clipping program make, where the engine versions differ.
    struct RigidDrawForm
    {
        // The rigid clipping program's triangles (ZGE3's and ZGE4's NearSplit, ZGE2's NearSplitAndFar).
        GSNativeClip clipped = GSNativeClip::Near;
        // ZGE4 (BMX XXX): a part with normals is sent them where its colours go (PacketKind::RigidNormals), lit
        // from them when its node has lights of its own (GSNativeLights).
        bool normalsForColours = false;
    };

    // What a game's skinned packet builder (buildSkinned) does with the part's skin weights, materials and UVs, and
    // its lit skinned microprogram with the alpha, where the engine versions differ: a native draw made on the host
    // from the part's streams follows it to give what VU1 draws.
    struct SkinnedDrawForm
    {
        enum class Weights : uint8_t
        {
            // ZGE3 (Aggressive Inline): the four bones as the stream has them.
            AsStream,
            // ZGE4 (BMX XXX): limitSkinWeights(part, 3) first: the fourth bone cleared, the others' weights scaled
            // to 255 over their total, the first taking what the other two leave.
            ThreeBones,
            // ZGE2 (Dave Mirra 2): dropLightBones first (a bone under 12/255 and every one after it dropped, the
            // rest scaled to 255), then three bones sent, the first weighing what the other two leave.
            LightBonesDropped,
        };
        Weights weights = Weights::AsStream;
        // ZGE2: the packet's material table filled in the order the submeshes come (a new entry each time the
        // submesh changes from one strip group to the next), where ZGE3 and ZGE4 start a submesh's strips at its
        // own entry.
        bool materialsInGroupOrder = false;
        // ZGE2: the UVs sent as the stream's floats (V2-32), where ZGE3 and ZGE4 send them to 1/4096 (V2-16).
        bool uvsAsFloats = false;
        // ZGE2's lit skinned program (DM2's VU1 0x118) lights the alpha as the colour channels, from the lights'
        // w (GSNativeSkinning::alphaFromLights); ZGE3's and ZGE4's give 0x80.
        bool alphaFromLights = false;
        // The game's Xbox release lights its characters with the lights xboxCharacterLights puts in place of the
        // PS2's default ones (Character Lighting: Xbox): Aggressive Inline's and BMX XXX's, whose XBEs hold the same
        // light block and whose PS2 default directions are the world axes. Dave Mirra 2's PS2 default directions
        // are (1, 0, 0.3), (0, 1, 0.3), (0, 0, 1), not the axes, and its XBE has no such block: its riders keep the
        // PS2's lights.
        bool xboxLights = false;
    };

    // A rigid prelit part made into a native mesh: its triangles, the
    // vertices of material k's packet at [runs[k].first, + runs[k].count).
    struct RigidMesh
    {
        struct Run
        {
            uint32_t first = 0;
            uint32_t count = 0;
        };
        std::shared_ptr<GSNativeMesh> mesh;
        std::vector<Run> runs; // one per material (count 0: none)
        uint32_t chunks = 0;
        uint32_t triangles = 0;
    };

    // Decodes the packet a part is drawn with (its buffer +0x64 bit 1) into
    // the triangles VU1 would draw, one run per material. False (and why)
    // for a packet that is not chunks as above.
    bool decodeRigidPart(const GuestMemory &memory, uint32_t partAddress, RigidMesh &out, std::string *why = nullptr,
                         PacketKind kind = PacketKind::RigidPrelit);

    // One material's packet: its chunks' triangles appended to `out` (and,
    // skinned, their bones to `skin`).
    bool decodeMaterialPacket(const GuestMemory &memory, uint32_t packetAddress, std::vector<GSNativeVertex> &out,
                              uint32_t &chunks, std::string *why = nullptr, PacketKind kind = PacketKind::RigidPrelit,
                              std::vector<GSNativeSkinWeights> *skin = nullptr);

    // A skinned part (types 8 / 0xC) made into a native mesh with bones
    // (GSNativeMesh::skin): as decodeRigidPart.
    bool decodeSkinnedPart(const GuestMemory &memory, uint32_t partAddress, RigidMesh &out, std::string *why,
                           PacketKind kind);

    // What the skinned renderer FUN_0010C250 uploads to VU1 for a node, in
    // the payload of the DMA CALL tag it opens (after the VU state, the
    // texture request): the bone palette, then 16 qwords at TOPS.
    struct SkinnedNodeUpload
    {
        // UNPACK V4-32 at 0: per bone of the part's bone list (part +0x1C),
        // 4 qwords from the node's pose (node +0x1BC, 0xC0 per bone):
        // rotation rows 0-2 (w 0) and the translation less the bone's
        // offset turned by the rotation (w 1.0).
        uint32_t paletteRows = 0;
        float palette[GSNativeSkinning::kPaletteRows][4] = {};
        // UNPACK V4-32 at TOPS (the 16 qwords VU1 0x158 / 0x5A0 load):
        float screen[4][4] = {};      // +0: object -> GS screen (world x *0x231CC4)
        float lightDir[4][4] = {};    // +4: light directions as columns (row k: component k of lights 0-2); row 3 unused
        float lightColour[4][4] = {}; // +8: colours of lights 0-2, the ambient (*0x27AB20, or node +0x160 +0x40)
        float clip[4][4] = {};        // +12: object -> clip volume (world x *0x231CCC)
        uint32_t program = 0;         // the MSCAL: type + 2 (0x0A lit, 0x0E prelit)
    };
    // Reads it from the VIF stream at `from` (the CALL tag's upper 64 bits)
    // up to the first microprogram run (MSCAL with an address), at most to
    // `to`. False (and why) for a payload not of that form.
    bool readSkinnedUpload(const GuestMemory &memory, uint32_t from, uint32_t to, SkinnedNodeUpload &out,
                           std::string *why = nullptr);

    // The depth range of a view's clip volume in screen w (view depth), for a
    // GPU that clips natively what a clipping microprogram clipped against
    // it (Dave Mirra BMX 2's VU1 0x06: |x|, |y|, |z| <= w of world x clip).
    // The clip matrix's z column is a w' + b (w' the screen matrix's w
    // column, b on the translation row): -w <= a w + b <= w gives the near
    // and far planes. False if the matrices are not of that form.
    bool clipDepthRange(const float screen[4][4], const float clip[4][4], float &nearW, float &farW);

    // The Xbox release's character lights in place of the PS2's (gs_quality
    // Character Lighting). The PS2's default block (0x2732F0: directions the
    // world axes, colours 0.3, ambient 0.5) reaches VU1 as U = -directions x
    // world (columns: the lights in object space, dotted with the normal).
    // The Xbox's (0x26E450 in its RAM, its vertex shader constants 176..185:
    // read from xemu's push buffer, the Ape at the Movie Lot start) has the
    // directions (1, 0, -0.3), (0, -1, 0.3), (-0.7, 0, 0.5), colours 0.6
    // (an ambient of 0.5: the Ape's back halves with them at 0), and the
    // same transform (its shader dots the normal with
    // -L x world^-1), so its lights in object space are U times the
    // normalised directions as columns; its vertex colours saturate at 1.0
    // (maxColour 128; VU1 keeps up to 255). Skinning with other lights (a
    // node's own light block) is left as it is. True if changed. Only for a
    // game whose PS2 default directions are the world axes and whose Xbox
    // release has these lights (SkinnedDrawForm::xboxLights): with other
    // directions U is not the world turned into object space, and the
    // lights would come out as neither release's.
    bool xboxCharacterLights(GSNativeSkinning &skin);
}
