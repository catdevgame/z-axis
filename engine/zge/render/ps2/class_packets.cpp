// The class packet builders (zge/render/packet.h), the part mesh's strict encoder: a part's VIF packet as
// each render class lays it out for its microprograms. buildPartPacket
// (ps2/part_packet.cpp) calls the part's class builder, which walks the part's strip
// groups in chunks of at most 60 entries: for each chunk a GIF tag (a
// triangle strip: ST, RGBAQ, XYZ2), then the streams the class unpacks to
// VU1 one after the other (VU addresses 8/40 on: tag, then one stream a
// quadword further each), then MSCAL 2 (a material's first chunk) or MSCNT.
// A material's first chunk starts a DMA chain of its own (startMaterialChain:
// the material table's entry). The streams not to rebuild are skipped by
// their size, so a rebuild in place keeps the layout.
//
//   terrain: GIF tag 40, UVs 41 (V2-16), normals 42 (V4-16: the normal in
//            the high bytes, the colour halved in the low ones, the strip
//            and edge flags in w), positions 43 (V3-32)
//   skinned: GIF tag 8, UVs 9 (V2-16), positions 10 (V4-32: x, y, z to
//            1/128 with the first three bones' indices in the low bits, w
//            the fourth's), normals or colours 11 (V4-16: with the weights)
//   rigid:   GIF tag 8, UVs 9 (V4-16: s, t, 1, the strip flags), colours 10
//            (V4-8, halved), positions 11 (V3-32)
//

#include "zge/render/packet.h"
#include "zge/render/part.h"
#include "zge/render/renderer.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/collision/collision.h"
#include "zge/math/vector.h"
#include "zge/math/vec.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/ps2/class_packets.cpp.h"

namespace zge
{
    namespace
    {
        // The packet the builders write (a sceVif1Packet in the renderer's
        // globals).
        inline ee::Global<sce::Vif1Packet, sym::g_buildPacket> g_buildPacket;
        using Packet = ee::Ptr<sce::Vif1Packet>;
        ee::Ptr<uint8_t> writePosition(Packet packet) { return packet->current.cast<uint8_t>(); }
        using Group = ee::Ptr<StripGroup>;
        constexpr Packet kBuildPacket(sym::g_buildPacket);

        // The terrain normals' edge test: the last two vertices' positions
        // and four bits a vertex of whether it lies far from the one before
        // (bit 0 past 0.5, bit 1 past 4), shifted up a nibble a vertex.
        inline ee::Global<Vec4, sym::g_terrainPosition> g_terrainPosition;
        inline ee::Global<Vec4, sym::g_terrainPreviousPosition> g_terrainPreviousPosition;
        inline ee::Global<uint32_t, sym::g_terrainEdgeBits> g_terrainEdgeBits;
        constexpr uint32_t kEdgePastHalf = 0x1u, kEdgePastFour = 0x2u;
        // Any of the last three vertices' bits, and what the normal word says of it.
        constexpr uint32_t kAnyEdgePastHalf = 0x111u, kAnyEdgePastFour = 0x222u;
        constexpr uint16_t kWordEdgePastHalf = 0x2000u, kWordEdgePastFour = 0x1000u;
        // The skinned positions' bone weight w, from the material set up
        // (its low seven bits cleared).
        inline ee::Global<uint32_t, sym::g_skinnedPositionW> g_skinnedPositionW;


        // The streams' cycle: one quadword written in three (CL 3, WL 1).
        constexpr uint32_t kOneInThree = vif::stcycl(3, 1);
        // The microprogram started (a material's first chunk) or continued.
        constexpr uint32_t kStartProgram = vif::mscal(2), kContinueProgram = vif::mscnt();
        // The masked UNPACKs (the masked fields filled from the row).
        constexpr uint32_t kUnpackV2_16Masked = vif::kUnpackV2_16 | vif::kUnpackMasked;
        constexpr uint32_t kUnpackV3_32Masked = vif::kUnpackV3_32 | vif::kUnpackMasked;
        constexpr uint32_t kUnpackV4_16Masked = vif::kUnpackV4_16 | vif::kUnpackMasked;
        constexpr uint32_t kUnpackV2_32Masked = vif::kUnpackV2_32 | vif::kUnpackMasked; // (ZGE2's terrain UVs: its float pairs)
        // The write masks: which fields the row fills.
        constexpr uint32_t kMaskW = 0x40404040u;      // positions: w
        constexpr uint32_t kMaskZw = 0x50505050u;     // V2 UVs: z and w
        constexpr uint32_t kMaskZ = 0x10u * 0x01010101u; // the rigid UVs: z

        constexpr uint32_t unpackCode(uint32_t format, uint32_t count, uint32_t vuAddress)
        {
            return vif::unpack(format, count, vuAddress | vif::kUnpackFromTops);
        }

        // A chunk's GIF tag: NLOOP entries, EOP, PRIM a triangle strip with
        // Gouraud shading and texture (PRE clear), three registers: ST, RGBAQ,
        // XYZ2. ZGE2's tag has no PRIM bits (its microprogram sets them).
        constexpr uint64_t kStripPrim = game::readable::perGame(gs::kTriangleStrip | gs::kGouraud | gs::kTextured, 0u,
                                                                gs::kTriangleStrip | gs::kGouraud | gs::kTextured);
        constexpr uint64_t kStripTagRegisters = gs::registerList({gs::kSt, gs::kRgbaq, gs::kXyz2});

        // A strip group's mode (StripGroup::submeshMode bits 0-1).
        enum StripMode : uint32_t
        {
            kModeStrip = 0,   // an entry a vertex
            kModeUnsent = 1,  // none of its streams sent (named from how the writers skip it)
            kModeList = 2,    // three entries a triangle
        };
        uint32_t modeOf(Group group) { return group->submeshMode & 0x3u; }
        uint32_t submeshOf(Group group) { return group->submeshMode >> 8; }
        // The group after this one in the groups stream.
        Group nextGroup(Group group) { return (group.cast<uint8_t>() + StripGroup::strideFor(group->count)).cast<StripGroup>(); }
        // The entry `first` of a group (its indices after the two-halfword head).
        ee::Ptr<uint16_t> entryCursor(Group group, uint32_t first) { return group.cast<uint16_t>() + (2 + first); }
        // Which streams a builder rebuilds: a stream not rebuilt is skipped by
        // its size, so a rebuild in place keeps the layout.
        enum RebuildBits : uint32_t
        {
            kRebuildPositions = 0x01,
            kRebuildColours = 0x02,
            kRebuildUvs = 0x04,
            kRebuildNormals = 0x40,
        };

        // A vertex position as a stream holds it and as the packet takes it.
        struct alignas(16) PositionWords
        {
            uint32_t x, y, z, w;
        };
        static_assert(sizeof(PositionWords) == 16);
        // A V4-16 element: a normal or colour word, 16 bits a lane.
        struct alignas(16) VertexWord
        {
            uint16_t x, y, z, w;
            uint64_t doubleword() const { return x | uint64_t(y) << 16 | uint64_t(z) << 32 | uint64_t(w) << 48; }
        };
        // Each channel shifted right by `colourShift` (1: halved).
        void shiftedRight(VertexColour &colour, uint32_t colourShift)
        {
            colour.red >>= colourShift;
            colour.green >>= colourShift;
            colour.blue >>= colourShift;
            colour.alpha >>= colourShift;
        }
        // The colour shift ZGE4's builders hand the colour writers (halved), which ZGE2's and ZGE3's writers have
        // no parameter for: they halve.
        constexpr uint32_t kBuilderColourShift = 1;

        // A vertex's four bones: index and weight, in the skin weights stream.
        struct VertexBones
        {
            struct
            {
                uint8_t bone, weight;
            } bones[4];
            // A bone's weight in 16ths (the stream holds it << 4).
            uint32_t sixteenths(int bone) const { return bones[bone].weight >> 4; }
        };
        static_assert(sizeof(VertexBones) == 8);

        // A word of each width appended to the packet (several appends have
        // identical code in the original, so each is named by its symbol).
        void addCode(Packet packet, uint32_t code) { sce::vif1PacketAddCode(packet, code); }
        void addCallCode(Packet packet, uint32_t code) { sce::vif1PacketAddCallCode(packet, code); }
        void addWord(Packet packet, uint32_t word) { sce::vif1PacketAddWord(packet, word); }
        void addDword(Packet packet, uint64_t dword) { sce::vif1PacketAddDword2(packet, dword); }

        // A position word with a bone's index less one in bits 2-6 (0x3F: none).
        uint32_t withBone(uint32_t word, uint32_t bone) { return bone != 0 ? word | (bone - 1) << 2 : word | 0x3Fu; }

        void skipPacket(Packet packet, uint32_t bytes) { packet->current = (packet->current.cast<uint8_t>() + bytes).cast<uint32_t>(); }

        // A normal component to 16 bits (clamped to +-32767), the low bits
        // cleared for what goes in them.
        uint16_t packedNormal(ee::Float component, int32_t lowMask)
        {
            int32_t value = ee::toInt(component);
            value = 32767 < value ? 32767 : value;
            return static_cast<uint16_t>((-32768 >= value ? -32767 : value) & lowMask);
        }
    }

    namespace
    {
        void initPacket(Packet packet, uint32_t buffer) { sce::vif1PacketInit(packet, buffer); }
        namespace tail
        {
            void resetPacket(Packet packet) { ee::tailCall(sym::vif1PacketReset, packet); }
            void closeDmaTag(Packet packet) { ee::tailCall(sym::vif1PacketCloseDmaTag, packet); }
        }
    }

    // ---- the chunks' frame -------------------------------------------------

    // beginBuildPacket: the build packet started in `buffer` (sceVif1PkInit,
    // then sceVif1PkReset: the original's tail call).
    void beginBuildPacket(Packet packet, uint32_t buffer)
    {
        initPacket(packet, buffer);
        tail::resetPacket(packet);
    }
    READABLE_FUNCTION(beginBuildPacket, Verified);

    // startMaterialChain (registered here: no draft): a material's DMA chain
    // started: the packet aligned to a quadword and a RET tag opened; where
    // the chain starts.
    ee::Ptr<uint8_t> startMaterialChain(Packet packet)
    {
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint8_t> start = writePosition(packet); // where the chain starts
        sce::vif1PacketRet(packet, 0);
        return start;
    }
    READABLE_FUNCTION(startMaterialChain, Verified);

    // finishBuildPacket: the build packet ended: aligned to a quadword, its DMA
    // tag closed (the original's tail call).
    void finishBuildPacket(Packet packet)
    {
        sce::vif1PacketAlign(packet, 2, 0);
        tail::closeDmaTag(packet);
    }
    READABLE_FUNCTION(finishBuildPacket, Verified);

    // addGifTag and addTerrainGifTag (the terrain's copy, no result): a
    // chunk's GIF tag for `entries` vertices unpacked to `vuAddress` (24
    // bytes skipped when not rebuilt); the next VU address.
    namespace
    {
        void addGifTagWords(Packet packet, uint32_t entries, uint32_t vuAddress, uint32_t endOfPacket)
        {
            sce::vif1PacketOpenUnpack(packet, vuAddress & 0xFFFFu | vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);
            packet->gifTag = packet->current.cast<uint64_t>();
            addDword(packet, gs::GifTag{.loops = entries, .endOfPacket = endOfPacket, .prim = kStripPrim, .registers = 3}.bits());
            addDword(packet, kStripTagRegisters);
            sce::vif1PacketCloseUnpack(packet);
        }
    }

    // (`firstChunk`: a pointer the builders pass, read nowhere. ZGE2 has no `streams`: it always writes the tag.)
    int32_t addGifTag(Packet packet, uint32_t entries, uint32_t vuAddress, uint32_t endOfPacket, uint32_t /*firstChunk*/, uint32_t streams)
    {
        if (streams == 0 && !game::readable::kIsDm2)
        {
            skipPacket(packet, 0x18);
            return vuAddress + 1;
        }
        addGifTagWords(packet, entries, vuAddress, endOfPacket);
        return vuAddress + 1;
    }
    READABLE_FUNCTION(addGifTag, Verified);

    void addTerrainGifTag(Packet packet, uint32_t entries, uint32_t vuAddress, uint32_t endOfPacket, uint32_t /*firstChunk*/, uint32_t streams)
    {
        if (streams == 0)
        {
            skipPacket(packet, 0x18);
            return;
        }
        addGifTagWords(packet, entries, vuAddress, endOfPacket);
    }
    READABLE_FUNCTION(addTerrainGifTag, Verified);

    // addMicroprogramCall and addTerrainMicroprogramCall (the terrain's copy): a chunk's end:
    // MSCAL 2 for a material's first chunk (*firstChunk, cleared), MSCNT
    // for the others (4 bytes skipped when not rebuilt), the packet aligned
    // to a quadword (the original's tail call).
    namespace
    {
        namespace tail
        {
            void vif1PacketAlign(Packet packet, uint32_t bit, uint32_t pos) { ee::tailCall(sym::vif1PacketAlign, packet, bit, pos); }
        }

        void addMicroprogramCallWords(Packet packet, ee::Ptr<uint32_t> firstChunk, uint32_t streams)
        {
            const uint32_t code = *firstChunk != 0 ? kStartProgram : kContinueProgram;
            if (streams == 0 && !game::readable::kIsDm2) // (DM2 always rebuilds; it has no such argument)
                skipPacket(packet, 4);
            else
                addCode(packet, code);
            *firstChunk = 0;
            tail::vif1PacketAlign(packet, 2, 0);
        }
    }

    void addMicroprogramCall(Packet packet, ee::Ptr<uint32_t> firstChunk, uint32_t streams)
    {
        addMicroprogramCallWords(packet, firstChunk, streams);
    }
    READABLE_FUNCTION(addMicroprogramCall, Verified);

    void addTerrainMicroprogramCall(Packet packet, ee::Ptr<uint32_t> firstChunk, uint32_t streams)
    {
        addMicroprogramCallWords(packet, firstChunk, streams);
    }
    READABLE_FUNCTION(addTerrainMicroprogramCall, Verified);

    // chunkEntries: the entries of a group's next chunk from `done` (at
    // most `most`; a list's whole triangles), the triangles they make added
    // to *triangles (a strip's: an entry each, less its restarts, which the
    // position writers take off).
    uint32_t chunkEntries(Group group, uint32_t done, ee::Ptr<uint32_t> triangles, uint32_t most)
    {
        const uint32_t left = group->count - done;
        const uint32_t mode = modeOf(group);
        uint32_t entries = left < most ? left : most;
        if (mode != kModeUnsent)
        {
            if (mode < 2)
                *triangles = *triangles + entries;
            else
            {
                if (mode != kModeList)
                    return entries;
                const uint32_t partial = entries % 3u;
                entries = entries - partial;
                *triangles = *triangles + entries / 3u;
            }
        }
        return entries;
    }
    READABLE_FUNCTION(chunkEntries, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t addCode(Packet packet, uint32_t code) { return ee::tailCall<int32_t>(sym::vif1PacketAddCode, packet, code); }
        }
    }

    // addMarkCode (no caller in this build): a MARK code of `value` >> 9
    // (the original's tail call).
    int32_t addMarkCode(Packet packet, uint32_t value)
    {
        return tail::addCode(packet, vif::mark(value >> 9));
    }
    READABLE_FUNCTION(addMarkCode, Draft);

    namespace
    {
        // A material first met: its DMA chain starts here and its table entry
        // says so.
        void startMaterial(ee::Ptr<PacketMaterial> table, uint32_t material, ee::Ptr<uint32_t> materialStart)
        {
            const ee::Ptr<uint8_t> start = startMaterialChain(kBuildPacket);
            *materialStart = 1;
            PacketMaterial &entry = table[material];
            entry.start = start;
            entry.gsBlockAddress = 0;
        }

        // The packet ended, the part's type word told how many entries and
        // triangles it holds. The packet's bytes.
        int32_t finishPartPacket(ee::Ptr<ZgePart> part, uint32_t sent, uint32_t triangles)
        {
            finishBuildPacket(kBuildPacket);
            part->typeWord = part->typeWord & ~kTypeWordEntries & ~kTypeWordTriangles | sent & 0x1FFFu | (triangles & 0x1FFFu) << 13;
            return bytesFrom(g_buildPacket->bufferStart, g_buildPacket->current);
        }
    }

    // ---- terrain -----------------------------------------------------------

    // terrainNormalWord: a terrain vertex's normal word: the normal to 16 bits
    // a component (its high byte kept) with the vertex's colour shifted right
    // by `colourShift` (ZGE4's parameter; ZGE2 and ZGE3 halve it) in the low
    // bytes (red, green, blue in x, y, z, alpha in w); w also says a restart
    // (0x8000), not flipped (0x4000), and whether an edge to the last
    // vertices is long (0x2000 past 0.5, 0x1000 past 4, from the edge bits
    // of the last three).
    void terrainNormalWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t entry READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
    {
        ee::Local<Float4> normal;
        ee::Local<VertexColour> colour;
        ee::Local<VertexWord> word;
        const uint32_t vertex = entry & kStripVertexMask;
        const bool restart = (entry & kStripNoTriangle) != 0;
        if (partStream(part, StreamKind::Normals) != 0)
            vertexNormal(part, vertex, normal);
        else
            vec3SetOne(ee::Ptr<Vec3>(normal.address()).get());
        const ee::Float scale = ee::Float(32768.0f);
        word->w = 0;
        word->x = packedNormal(normal->x * scale, ~0xFF);
        word->y = packedNormal(normal->y * scale, ~0xFF);
        word->z = packedNormal(normal->z * scale, ~0xFF);

        vertexColour(part, vertex, colour);
        word->x |= colour->red >> colourShift;
        word->y |= colour->green >> colourShift;
        word->z |= colour->blue >> colourShift;
        word->w |= colour->alpha >> colourShift;
        if (restart)
            word->w |= kStripNoTriangle;
        if ((entry & kStripFlipped) == 0)
            word->w |= kStripFlipped;

        // Each vertex shifts the last ones' edge bits up a nibble and adds its own.
        *g_terrainEdgeBits = *g_terrainEdgeBits << 4;
        *g_terrainPreviousPosition = *g_terrainPosition;
        vertexPosition(part, vertex, ee::Ptr<Vec4>(g_terrainPosition.get()));
        const Vec4 step = *g_terrainPreviousPosition - *g_terrainPosition;
        const ee::Float squared = squaredLength(step);
        uint32_t bits = *g_terrainEdgeBits | (ee::Float(0.25f) < squared ? kEdgePastHalf : 0u);
        if (ee::Float(16.0f) < squared)
            bits |= kEdgePastFour;
        *g_terrainEdgeBits = bits;
        if (!restart)
        {
            if (bits & kAnyEdgePastHalf)
                word->w |= kWordEdgePastHalf;
            if (bits & kAnyEdgePastFour)
                word->w |= kWordEdgePastFour;
        }
        addDword(packet, word->doubleword());
    }
    READABLE_FUNCTION(terrainNormalWord, Verified);

    // terrainUvWord (terrain) and skinnedUvWord (skinned, word for word): a
    // vertex's UV word: s and t to 1/4096 in 16 bits each.
    namespace
    {
        void uvWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t entry)
        {
            ee::Local<Float4> uv;
            vertexUv(part, entry & kStripVertexMask, uv);
            const uint32_t s = static_cast<uint32_t>(fixedUv(uv->x)) & 0xFFFFu;
            addWord(packet, s | static_cast<uint32_t>(fixedUv(uv->y)) << 16);
        }
    }

    void terrainUvWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t entry) { uvWord(packet, part, entry); }
    READABLE_FUNCTION(terrainUvWord, Verified);

    void skinnedUvWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t entry) { uvWord(packet, part, entry); }
    READABLE_FUNCTION(skinnedUvWord, Verified);

    // addTerrainPositions: a chunk's terrain positions (V3-32, w from the row: 16
    // + 12 bytes an entry skipped when not rebuilt); a strip's restarts
    // taken off *triangles.
    void addTerrainPositions(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                             uint32_t streams, ee::Ptr<uint32_t> triangles)
    {
        ee::Local<PositionWords> position;
        if ((streams & kRebuildPositions) == 0)
        {
            skipPacket(packet, 0x10 + entries * 0xC);
            return;
        }
        const ee::Ptr<PositionWords> positions = streamRecords<PositionWords>(part->positions);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskW);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV3_32Masked, entries, vuAddress));
        if (modeOf(group) != kModeStrip)
            return;
        uint32_t counted = *triangles;
        do
        {
            const uint32_t entry = *cursor;
            ++cursor;
            const PositionWords &source = positions[entry & kStripVertexMask];
            if (entry & kStripNoTriangle)
                --counted;
            --entries;
            position->x = source.x;
            position->y = source.y;
            position->z = source.z;
            sce::vif1PacketAddWords(packet, position, 3);
        } while (entries != 0);
        *triangles = counted;
    }
    READABLE_FUNCTION(addTerrainPositions, Verified);

    // addTerrainNormals: a chunk's terrain normals (V4-16: terrainNormalWord per
    // entry, given ZGE4's colour shift; a list's triangles three entries at a
    // time; 8 + 8 bytes an entry skipped when not rebuilt), the edge test
    // reset first.
    void addTerrainNormals(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                           uint32_t streams READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
    {
        if ((streams & kRebuildNormals) == 0)
        {
            skipPacket(packet, 8 + (entries << 3));
            return;
        }
        *g_terrainPosition = kZero;
        *g_terrainPreviousPosition = kZero;
        *g_terrainEdgeBits = 0;
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_16, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                terrainNormalWord(packet, part, entry READABLE_PARAM_ONLY(bmx, colourShift));
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t entry = *cursor;
                    ++cursor;
                    terrainNormalWord(packet, part, entry READABLE_PARAM_ONLY(bmx, colourShift));
                }
            }
        }
    }
    READABLE_FUNCTION(addTerrainNormals, Verified);

    // addTerrainUvs: a chunk's terrain UVs (V2-16, z and w from the row: a
    // strip's terrainUvWord per entry, a list's the stream's words as they
    // are; 16 + 4 bytes an entry skipped when not rebuilt).
    void addTerrainUvs(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                       uint32_t streams)
    {
        if ((streams & kRebuildUvs) == 0)
        {
            skipPacket(packet, 0x10 + (entries << 2));
            return;
        }
        const ee::Ptr<uint32_t> uvs = streamRecords<uint32_t>(part->uvs);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskZw);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV2_16Masked, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                terrainUvWord(packet, part, entry & kStripVertexMask);
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = *cursor;
                    ++cursor;
                    sce::vif1PacketAddWords(packet, ee::Ptr<const uint32_t>(uvs + vertex), 2);
                }
            }
        }
    }

    READABLE_FUNCTION(addTerrainUvs, Verified);

    // buildTerrain: a terrain part's packet built in `buffer`, its
    // materials' chains in `materials` (PacketMaterial per material), the
    // `streams` bits to rebuild; the entries and triangles counted into its
    // type word. The packet's bytes.
    int32_t buildTerrain(ee::Ptr<ZgePart> part, uint32_t buffer, uint32_t materials, uint32_t streams)
    {
        ee::Local<uint32_t> triangles;
        ee::Local<uint32_t> firstChunk;
        ee::Local<uint32_t> materialStart;
        const ee::Ptr<PacketMaterial> table(materials);
        uint32_t material = 255;
        uint32_t sent = 0;
        *firstChunk = 1;
        *triangles = 0;
        partStream(part, StreamKind::Normals);
        beginBuildPacket(kBuildPacket, buffer);
        Group group = streamRecords<StripGroup>(part->groups);
        for (int32_t groupsLeft = part->groupCount; groupsLeft != 0; --groupsLeft)
        {
            uint32_t done = 0;
            do
            {
                *materialStart = 0;
                const uint32_t chunk = chunkEntries(group, done, triangles, 60);
                sent += chunk;
                if (submeshOf(group) != material)
                {
                    material = submeshOf(group);
                    startMaterial(table, material, materialStart);
                }
                addTerrainGifTag(kBuildPacket, chunk, 40, 1, firstChunk.address(), streams);
                addTerrainUvs(kBuildPacket, 41, chunk, done, group, part, streams);
                addTerrainNormals(kBuildPacket, 42, chunk, done, group, part, streams READABLE_PARAM_ONLY(bmx, kBuilderColourShift));
                addTerrainPositions(kBuildPacket, 43, chunk, done, group, part, streams, triangles);
                done += chunk;
                addTerrainMicroprogramCall(kBuildPacket, materialStart, streams);
            } while (done < group->count);
            group = nextGroup(group);
        }
        return finishPartPacket(part, sent, *triangles);
    }
    READABLE_FUNCTION(buildTerrain, Verified);

    // ---- skinned -----------------------------------------------------------

    // skinnedNormalWord: a skinned vertex's normal word: the normal to 16 bits a
    // component (the low five bits cleared) with the second, third and
    // fourth bones' weights (in 1/16ths) in y, z and w's low bits, and the
    // first's (16 less the others) in x's.
    void skinnedNormalWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex)
    {
        ee::Local<Float4> normal;
        ee::Local<VertexWord> word;
        const VertexBones &bones = streamRecords<VertexBones>(part->skinWeights)[vertex];
        vertexNormal(part, vertex, normal);
        const ee::Float scale = ee::Float(32768.0f);
        const uint32_t x = packedNormal(normal->x * scale, -32);
        const uint32_t y = packedNormal(normal->y * scale, -32);
        const uint32_t z = packedNormal(normal->z * scale, -32);
        const uint32_t second = bones.sixteenths(1), third = bones.sixteenths(2), fourth = bones.sixteenths(3);
        word->x = static_cast<uint16_t>(x | 0x10u - (second + third + fourth));
        word->y = static_cast<uint16_t>(y | second);
        word->z = static_cast<uint16_t>(z | third);
        word->w = static_cast<uint16_t>(fourth);
        addDword(packet, word->doubleword());
    }

    // skinnedNormalWords: ZGE2's skinnedNormalWord: the vertex's normal as
    // its three floats (ZGE2's normals have no index table), the low five
    // bits of each made a bone's weight in 1/16ths: the second's in y, the
    // third's in z and the first's (16 less those) in x (ZGE2 weighs three
    // bones a vertex).
    void skinnedNormalWords(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex)
    {
        constexpr uint32_t kWeightBits = 0x1Fu;
        ee::Local<uint32_t[3]> words;
        const ee::Ptr<ee::Float> normal = streamRecords<ee::Float>(part->normals) + 3 * vertex;
        const VertexBones &bones = streamRecords<VertexBones>(part->skinWeights)[vertex];
        const uint32_t second = bones.sixteenths(1), third = bones.sixteenths(2);
        words[0] = normal[0].bits() & ~kWeightBits;
        words[1] = (normal[1].bits() & ~kWeightBits) | second;
        words[2] = (normal[2].bits() & ~kWeightBits) | third;
        words[0] = words[0] | (0x10u - (second + third));
        sce::vif1PacketAddWords(packet, words, 3);
    }
    READABLE_FUNCTION_OWN(skinnedNormalWord, Verified, dm2, skinnedNormalWords);

    // skinnedColourWord: a skinned vertex's colour word: its colour shifted
    // right by `colourShift` (ZGE4's parameter; ZGE2 and ZGE3 halve it), 16
    // bits a channel, with the four bones' weights (as skinnedNormalWord's)
    // in each channel's high byte.
    void skinnedColourWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
    {
        ee::Local<VertexColour> colour;
        ee::Local<VertexWord> word;
        const VertexBones &bones = streamRecords<VertexBones>(part->skinWeights)[vertex];
        vertexColour(part, vertex, colour);
        const uint32_t second = bones.sixteenths(1), third = bones.sixteenths(2), fourth = bones.sixteenths(3);
        const uint32_t first = 0x10u - (fourth + (second + third));
        word->x = static_cast<uint16_t>(colour->red >> colourShift | (first & 0xFFu) << 8);
        word->y = static_cast<uint16_t>(colour->green >> colourShift | second << 8);
        word->z = static_cast<uint16_t>(colour->blue >> colourShift | third << 8);
        word->w = static_cast<uint16_t>(colour->alpha >> colourShift | fourth << 8);
        addDword(packet, word->doubleword());
    }
    READABLE_FUNCTION(skinnedColourWord, Verified);

    // addSkinnedPositions: a chunk's skinned positions (V4-32: x, y, z to 1/128
    // with the first three bones' indices less one (0x3F: none) in their
    // low bits, w g_skinnedPositionW's with the fourth's; bit 0 of x: a
    // triangle ends here; 16 + 16 bytes an entry skipped when not rebuilt);
    // a strip's restarts taken off *triangles. Its next VU address.
    uint32_t addSkinnedPositions(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                                 uint32_t streams, ee::Ptr<uint32_t> triangles)
    {
        ee::Local<PositionWords> position;
        if ((streams & kRebuildPositions) == 0)
        {
            skipPacket(packet, 0x10 + (entries << 4));
            return vuAddress;
        }
        const ee::Ptr<PositionWords> positions = streamRecords<PositionWords>(part->positions);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        const ee::Ptr<VertexBones> skin = streamRecords<VertexBones>(part->skinWeights);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskW);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_32, entries, vuAddress));
        if (modeOf(group) != kModeStrip)
            return vuAddress;
        // The last three entries' restarts, a bit each (6 to start: the first
        // two make no triangle).
        uint32_t restarts = 6;
        uint32_t counted = *triangles;
        do
        {
            const uint32_t entry = *cursor;
            ++cursor;
            if (entry & kStripNoTriangle)
            {
                --counted;
                restarts |= 0x2u;
            }
            const uint32_t vertex = entry & kStripVertexMask;
            const PositionWords &source = positions[vertex];
            const VertexBones &bones = skin[vertex];
            position->x = withBone(source.x & ~0x7Fu, bones.bones[0].bone);
            position->y = withBone(source.y & ~0x7Fu, bones.bones[1].bone);
            position->z = withBone(source.z & ~0x7Fu, bones.bones[2].bone);
            position->w = withBone(*g_skinnedPositionW & ~0x7Fu, bones.bones[3].bone);
            restarts >>= 1;
            if (restarts != 0)
                position->x &= ~0x1u;
            else
                position->x |= 0x1u; // a triangle ends here
            ++vuAddress;
            --entries;
            sce::vif1PacketAddQword(packet, std::bit_cast<ee::Qword>(*position));
        } while (entries != 0);
        *triangles = counted;
        return vuAddress;
    }

    namespace
    {
        // ZGE2 weighs three bones a vertex at most and drops a bone weighing
        // under 12/255: the first such and every bone after it are taken
        // out (index and weight 0) and the weights left made to sum to 255
        // again, the last taking the remainder. In the stream itself.
        void dropLightBones(VertexBones &bones)
        {
            constexpr uint32_t kLeastWeight = 12;
            auto &b = bones.bones;
            const auto dropFrom = [&](int first) {
                for (int i = first; i < 4; ++i)
                    b[i] = {0, 0};
            };
            if (b[1].weight < kLeastWeight)
            {
                dropFrom(1);
                b[0].weight = 0xFF;
            }
            else if (b[2].weight < kLeastWeight)
            {
                const int32_t sum = b[0].weight + b[1].weight;
                const uint8_t first = static_cast<uint8_t>(b[0].weight * 255 / sum);
                dropFrom(2);
                b[0].weight = first;
                b[1].weight = static_cast<uint8_t>(~first);
            }
            else if (b[3].weight < kLeastWeight)
            {
                const int32_t sum = b[0].weight + b[1].weight + b[2].weight;
                const uint8_t first = static_cast<uint8_t>(b[0].weight * 255 / sum);
                const uint8_t second = static_cast<uint8_t>(b[1].weight * 255 / sum);
                b[0].weight = first;
                b[3] = {0, 0};
                b[1].weight = second;
                b[2].weight = static_cast<uint8_t>(~(first + second));
            }
        }
    }

    // addSkinnedPositionWords: ZGE2's addSkinnedPositions: a chunk's skinned
    // positions as three words (V3-32, w from the row): x, y, z to 1/128
    // with the first three bones' indices less one (0x3F: none) in their
    // low bits, the vertex's light bones dropped first (dropLightBones);
    // bit 0 of x: a triangle ends here; 16 + 12 bytes an entry skipped when
    // not rebuilt; a strip's restarts taken off *triangles. Its next VU
    // address.
    uint32_t addSkinnedPositionWords(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                                     ee::Ptr<ZgePart> part, uint32_t streams, ee::Ptr<uint32_t> triangles)
    {
        constexpr uint32_t kPositionWordsBytes = 12;
        ee::Local<uint32_t[3]> words;
        if ((streams & kRebuildPositions) == 0)
        {
            skipPacket(packet, 0x10 + entries * kPositionWordsBytes);
            return vuAddress;
        }
        const ee::Ptr<PositionWords> positions = streamRecords<PositionWords>(part->positions);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        const ee::Ptr<VertexBones> skin = streamRecords<VertexBones>(part->skinWeights);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskW);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV3_32Masked, entries, vuAddress));
        if (modeOf(group) != kModeStrip)
            return vuAddress;
        // The last three entries' restarts, a bit each (6 to start: the first
        // two make no triangle).
        uint32_t restarts = 6;
        uint32_t counted = *triangles;
        do
        {
            uint32_t vertex = *cursor;
            ++cursor;
            if (vertex & kStripNoTriangle)
            {
                --counted;
                restarts |= 0x2u;
                vertex &= ~kStripNoTriangle;
            }
            VertexBones &bones = skin[vertex];
            const PositionWords &source = positions[vertex];
            dropLightBones(bones);
            words[0] = withBone(source.x & ~0x7Fu, bones.bones[0].bone);
            words[1] = withBone(source.y & ~0x7Fu, bones.bones[1].bone);
            words[2] = withBone(source.z & ~0x7Fu, bones.bones[2].bone);
            restarts >>= 1;
            if (restarts != 0)
                words[0] = words[0] & ~0x1u;
            else
                words[0] = words[0] | 0x1u; // a triangle ends here
            ++vuAddress;
            --entries;
            sce::vif1PacketAddWords(packet, words, 3);
        } while (entries != 0);
        *triangles = counted;
        return vuAddress;
    }
    READABLE_FUNCTION_OWN(addSkinnedPositions, Verified, dm2, addSkinnedPositionWords);

    // addSkinnedNormals (normals: skinnedNormalWord) and addSkinnedColours (colours:
    // skinnedColourWord, given ZGE4's colour shift): a chunk's skinned normals
    // or colours (V4-16; 8 + 8 bytes an entry skipped when not rebuilt). The
    // normals take a list's triangles three entries at a time; the colours
    // only strips. Their next VU address.
    uint32_t addSkinnedNormals(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                               uint32_t streams)
    {
        if ((streams & kRebuildNormals) == 0)
        {
            skipPacket(packet, 8 + (entries << 3));
            return vuAddress;
        }
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_16, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                skinnedNormalWord(packet, part, entry & kStripVertexMask);
                --entries;
            } while (entries != 0);
            return vuAddress;
        }
        if (mode != kModeList)
            return vuAddress;
        for (; entries >= 3u; entries -= 3)
        {
            for (int corner = 0; corner < 3; ++corner)
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                skinnedNormalWord(packet, part, entry);
            }
        }
        return vuAddress;
    }

    // addSkinnedNormalWords: ZGE2's addSkinnedNormals: each entry's
    // skinnedNormalWord (ZGE2's: three words, V3-32, w from the row); 16 +
    // 12 bytes an entry skipped when not rebuilt. Their next VU address.
    uint32_t addSkinnedNormalWords(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                                   ee::Ptr<ZgePart> part, uint32_t streams)
    {
        constexpr uint32_t kNormalWordsBytes = 12;
        if ((streams & kRebuildNormals) == 0)
        {
            skipPacket(packet, 0x10 + entries * kNormalWordsBytes);
            return vuAddress;
        }
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskW);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV3_32Masked, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                skinnedNormalWord(packet, part, entry & ~kStripNoTriangle); // (ZGE2 clears the restart bit alone)
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t entry = *cursor;
                    ++cursor;
                    ++vuAddress;
                    skinnedNormalWord(packet, part, entry);
                }
        }
        return vuAddress;
    }
    READABLE_FUNCTION_OWN(addSkinnedNormals, Verified, dm2, addSkinnedNormalWords);

    uint32_t addSkinnedColours(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                               uint32_t streams READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
    {
        if ((streams & kRebuildColours) == 0)
        {
            skipPacket(packet, 8 + (entries << 3));
            return vuAddress;
        }
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_16, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode >= 2)
            return vuAddress;
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                skinnedColourWord(packet, part, entry & kStripVertexMask READABLE_PARAM_ONLY(bmx, colourShift));
                --entries;
            } while (entries != 0);
        }
        return vuAddress;
    }
    READABLE_FUNCTION(addSkinnedColours, Verified);

    // addSkinnedUvs: a chunk's skinned UVs (as addTerrainUvs, with
    // skinnedUvWord; 16 + 4 bytes an entry skipped). Its next VU address.
    uint32_t addSkinnedUvs(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                           uint32_t streams)
    {
        if ((streams & kRebuildUvs) == 0)
        {
            skipPacket(packet, 0x10 + (entries << 2));
            return vuAddress;
        }
        const ee::Ptr<uint32_t> uvs = streamRecords<uint32_t>(part->uvs);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskZw);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV2_16Masked, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                skinnedUvWord(packet, part, entry & kStripVertexMask);
                --entries;
            } while (entries != 0);
            return vuAddress;
        }
        if (mode != kModeList)
            return vuAddress;
        for (; entries >= 3u; entries -= 3)
        {
            for (int corner = 0; corner < 3; ++corner)
            {
                const uint32_t vertex = *cursor;
                ++cursor;
                ++vuAddress;
                sce::vif1PacketAddWords(packet, ee::Ptr<const uint32_t>(uvs + vertex), 2);
            }
        }
        return vuAddress;
    }

    // addSkinnedUvPairs: ZGE2's addSkinnedUvs: each entry's vertex's UV as
    // the stream's two floats (V2-32, z and w from the row; ZGE2's UV
    // stream is float pairs), strips and lists alike; 16 + 8 bytes an entry
    // skipped when not rebuilt. The VU address past the entries sent.
    uint32_t addSkinnedUvPairs(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                               ee::Ptr<ZgePart> part, uint32_t streams)
    {
        constexpr uint32_t kUvPairBytes = 8;
        if ((streams & kRebuildUvs) == 0)
        {
            skipPacket(packet, 0x10 + entries * kUvPairBytes);
            return vuAddress;
        }
        const ee::Ptr<uint8_t> uvs = streamRecords<uint8_t>(part->uvs);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskZw);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV2_32Masked, entries, vuAddress));
        const auto addUv = [&](uint32_t vertex) {
            sce::vif1PacketAddWords(packet, uvs + vertex * kUvPairBytes, 2);
            ++vuAddress;
        };
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                addUv(entry & ~kStripNoTriangle); // (ZGE2 clears the restart bit alone)
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = *cursor;
                    ++cursor;
                    addUv(vertex);
                }
        }
        return vuAddress;
    }
    READABLE_FUNCTION_OWN(addSkinnedUvs, Verified, dm2, addSkinnedUvPairs);

    // limitSkinWeights (ZGE4 only): each vertex of a skinned part keeps its first `bones` bones (at least one; the
    // others cleared with their weights) and has its weights scaled to 255 together: the second to the fourth each
    // times 255 over the vertex's total, the first what is left. 4 leaves the weights as they are. ZGE2 and ZGE3
    // send the stream's weights as they are.
    inline void limitSkinWeights(ee::Ptr<ZgePart> part, uint32_t bones)
    {
        if (bones == 4)
            return;
        ee::Ptr<VertexBones> vertex = streamRecords<VertexBones>(part->skinWeights);
        for (int32_t index = 0; index < part->vertexCount; ++index, ++vertex)
        {
            for (uint32_t bone = 1; bone < 4; ++bone)
                if (bones <= bone)
                    vertex->bones[bone] = {0, 0};
            const uint32_t total = vertex->bones[0].weight + vertex->bones[3].weight +
                                   (vertex->bones[2].weight + vertex->bones[1].weight);
            // (DIVU: the original traps on a vertex with no weight, BREAK 7.)
            const auto scaled = [total](uint32_t weight) { return total != 0 ? weight * 255u / total : 0u; };
            const uint32_t second = scaled(vertex->bones[1].weight);
            const uint32_t third = scaled(vertex->bones[2].weight);
            const uint32_t fourth = scaled(vertex->bones[3].weight);
            vertex->bones[1].weight = static_cast<uint8_t>(second);
            vertex->bones[2].weight = static_cast<uint8_t>(third);
            vertex->bones[3].weight = static_cast<uint8_t>(fourth);
            vertex->bones[0].weight = static_cast<uint8_t>(~(fourth + (second + third)));
        }
    }
    READABLE_FUNCTION_ONLY(bmx, limitSkinWeights);

    namespace
    {
        // ZGE4 builds a skinned packet with three bones a vertex (limitSkinWeights).
        constexpr uint32_t kSkinnedPacketBones = 3;
    }

    // buildSkinned: a skinned part's packet built (as buildTerrain's: GIF
    // tag 8, UVs 9, positions 10, normals 11 when the part has normals,
    // else colours). The packet's bytes. ZGE4 limits the part's skin weights
    // to three bones a vertex first. ZGE2 fills the material table in the
    // order the submeshes come and sends normals always (never colours).
    int32_t buildSkinned(ee::Ptr<ZgePart> part, uint32_t buffer, uint32_t materials, uint32_t streams)
    {
        ee::Local<uint32_t> triangles;
        ee::Local<uint32_t> firstChunk;
        ee::Local<uint32_t> materialStart;
        const ee::Ptr<PacketMaterial> table(materials);
        uint32_t material = 255;
        uint32_t filled = 0; // ZGE2's: the material table's entries filled
        uint32_t sent = 0;
        *firstChunk = 1;
        *triangles = 0;
        if constexpr (game::readable::kIsBmx)
            limitSkinWeights(part, kSkinnedPacketBones);
        beginBuildPacket(kBuildPacket, buffer);
        Group group = streamRecords<StripGroup>(part->groups);
        for (int32_t groupsLeft = part->groupCount; groupsLeft != 0; --groupsLeft)
        {
            uint32_t done = 0;
            do
            {
                *materialStart = 0;
                const uint32_t chunk = chunkEntries(group, done, triangles, 60);
                const uint32_t submesh = submeshOf(group);
                sent += chunk;
                if (submesh != material)
                {
                    material = submesh;
                    startMaterial(table, game::readable::kIsDm2 ? filled++ : material, materialStart);
                }
                addGifTag(kBuildPacket, chunk, 8, 1, firstChunk.address(), streams);
                addSkinnedUvs(kBuildPacket, 9, chunk, done, group, part, streams);
                addSkinnedPositions(kBuildPacket, 10, chunk, done, group, part, streams, triangles);
                if constexpr (game::readable::kIsDm2) // (ZGE2 sends normals always)
                    addSkinnedNormals(kBuildPacket, 11, chunk, done, group, part, streams);
                else if (part->normals)
                    addSkinnedNormals(kBuildPacket, 11, chunk, done, group, part, streams);
                else
                    addSkinnedColours(kBuildPacket, 11, chunk, done, group, part, streams READABLE_PARAM_ONLY(bmx, kBuilderColourShift));
                done += chunk;
                addMicroprogramCall(kBuildPacket, materialStart, streams);
            } while (done < group->count);
            group = nextGroup(group);
        }
        return finishPartPacket(part, sent, *triangles);
    }
    READABLE_FUNCTION(buildSkinned, Verified);

    // ---- rigid -------------------------------------------------------------

    // addRigidPositions: a chunk's rigid positions (V3-32, w from the row; a
    // part with bones takes them past its vertices, its second set, but not in
    // ZGE4; 16 + 12 bytes an entry skipped when not rebuilt). The triangle
    // count stays as it is: a rigid strip's restarts are not taken off.
    void addRigidPositions(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                           uint32_t streams, ee::Ptr<uint32_t> /*triangles*/)
    {
        if ((streams & kRebuildPositions) == 0)
        {
            skipPacket(packet, 0x10 + entries * 0xC);
            return;
        }
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        ee::Ptr<PositionWords> positions = streamRecords<PositionWords>(part->positions);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskW);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV3_32Masked, entries, vuAddress));
        if constexpr (!game::readable::kIsBmx)
            if (part->boneCount != 0)
                positions += part->vertexCount;
        if (modeOf(group) != kModeStrip)
            return;
        do
        {
            const uint32_t entry = *cursor;
            ++cursor;
            --entries;
            sce::vif1PacketAddWords(packet, ee::Ptr<const uint32_t>(&positions[entry & kStripVertexMask].x), 3);
        } while (entries != 0);
    }
    READABLE_FUNCTION(addRigidPositions, Verified);

    // addRigidColours: a chunk's rigid colours (V4-8, each channel shifted
    // right by `colourShift`, ZGE4's parameter, where ZGE2 and ZGE3 halve it:
    // a strip's from vertexColour, a list's from the colours stream directly;
    // 8 + 4 bytes an entry skipped when not rebuilt). ZGE2 returns the next
    // VU address and takes a strip's colours from the stream too.
    READABLE_RESULT_ONLY(dm2, uint32_t) addRigidColours(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                                                        ee::Ptr<ZgePart> part, uint32_t streams READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
    {
        ee::Local<VertexColour> colour;
        ee::Local<VertexColour> listColour;
        if ((streams & kRebuildColours) == 0)
        {
            skipPacket(packet, 8 + (entries << 2));
            return READABLE_RETURN_ONLY(dm2, vuAddress);
        }
        const ee::Ptr<VertexColour> colours = streamRecords<VertexColour>(part->colours);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_8, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                ++vuAddress;
                if constexpr (game::readable::kIsDm2)
                    *colour = colours[entry & kStripVertexMask];
                else
                    vertexColour(part, entry & kStripVertexMask, colour);
                --entries;
                shiftedRight(colour, colourShift);
                sce::vif1PacketAddWords(packet, colour, 1);
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = *cursor;
                    ++cursor;
                    ++vuAddress;
                    *listColour = colours[vertex];
                    shiftedRight(listColour, colourShift);
                    sce::vif1PacketAddWords(packet, listColour, 1);
                }
            }
        }
        return READABLE_RETURN_ONLY(dm2, vuAddress);
    }
    READABLE_FUNCTION(addRigidColours, Verified);

    // rigidNormalWord (ZGE4 only): a rigid vertex's normal word: the normal to 16 bits a component (z's low five
    // bits clear), w 0. ZGE2 and ZGE3 send a rigid part no normals.
    inline void rigidNormalWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex)
    {
        ee::Local<Float4> normal;
        ee::Local<VertexWord> word;
        vertexNormal(part, vertex, normal);
        const ee::Float scale = ee::Float(32768.0f);
        word->w = 0;
        word->x = packedNormal(normal->x * scale, -1);
        word->y = packedNormal(normal->y * scale, -1);
        word->z = packedNormal(normal->z * scale, -32);
        addDword(packet, word->doubleword());
    }
    READABLE_FUNCTION_ONLY(bmx, rigidNormalWord);

    // addRigidNormals (ZGE4 only): a chunk's rigid normals (V4-16, rigidNormalWord), where buildRigid sends a part
    // with normals in place of its colours; a strip's only (a list sends none); 8 + 8 bytes an entry skipped when
    // not rebuilt. Its next VU address.
    inline uint32_t addRigidNormals(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                                    ee::Ptr<ZgePart> part, uint32_t streams)
    {
        if ((streams & kRebuildNormals) == 0)
        {
            skipPacket(packet, 8 + (entries << 3));
            return vuAddress;
        }
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(vif::kUnpackV4_16, entries, vuAddress));
        if (modeOf(group) != kModeStrip)
            return vuAddress;
        do
        {
            const uint32_t entry = *cursor;
            ++cursor;
            ++vuAddress;
            rigidNormalWord(packet, part, entry & kStripVertexMask);
            --entries;
        } while (entries != 0);
        return vuAddress;
    }
    READABLE_FUNCTION_ONLY(bmx, addRigidNormals);

    // addRigidUvs: a chunk's rigid UVs (V4-16, z from the row: a strip's s
    // and t to 1/4096, 1.0, the strip flags (a restart, flipped 0x20 or
    // not 0x4000); a list's the stream's words; 16 + 8 bytes an entry
    // skipped when not rebuilt).
    void addRigidUvs(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group, ee::Ptr<ZgePart> part,
                     uint32_t streams)
    {
        ee::Local<Float4> uv;
        ee::Local<VertexWord> word;
        if ((streams & kRebuildUvs) == 0)
        {
            skipPacket(packet, 0x10 + (entries << 3));
            return;
        }
        const ee::Ptr<uint32_t> uvs = streamRecords<uint32_t>(part->uvs);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskZ);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV4_16Masked, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                vertexUv(part, entry & kStripVertexMask, uv);
                const ee::Float t = uv->y;
                word->x = static_cast<uint16_t>(fixedUv(uv->x));
                word->y = static_cast<uint16_t>(fixedUv(t));
                word->z = 0x1000;
                word->w = static_cast<uint16_t>(entry & kStripNoTriangle | ((entry & kStripFlipped) != 0 ? 0x20u : 0x4000u));
                sce::vif1PacketAddGsData(packet, word, 1);
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = *cursor;
                    ++cursor;
                    sce::vif1PacketAddWords(packet, ee::Ptr<const uint32_t>(uvs + vertex), 2);
                }
            }
        }
    }

    // addRigidUvPairs: ZGE2's addRigidUvs: a strip's UVs from the stream's
    // float pairs (ZGE2's UV stream) to 1/4096, 1.0 and the entry itself as
    // the fourth halfword (V4-16, z from the row); a list's the stream's
    // two words as they are; 16 + 8 bytes an entry skipped when not rebuilt.
    // The VU address past the entries sent.
    uint32_t addRigidUvPairs(Packet packet, uint32_t vuAddress, uint32_t entries, uint32_t first, Group group,
                             ee::Ptr<ZgePart> part, uint32_t streams)
    {
        constexpr uint32_t kUvPairBytes = 8;
        ee::Local<VertexWord> word;
        if ((streams & kRebuildUvs) == 0)
        {
            skipPacket(packet, 0x10 + entries * kUvPairBytes);
            return vuAddress;
        }
        const ee::Ptr<uint8_t> uvs = streamRecords<uint8_t>(part->uvs);
        ee::Ptr<uint16_t> cursor = entryCursor(group, first);
        addCode(packet, vif::stmask());
        addCallCode(packet, kMaskZ);
        addCode(packet, kOneInThree);
        addCode(packet, unpackCode(kUnpackV4_16Masked, entries, vuAddress));
        const uint32_t mode = modeOf(group);
        if (mode == kModeStrip)
        {
            do
            {
                const uint32_t entry = *cursor;
                ++cursor;
                const ee::Ptr<ee::Float> uv = (uvs + (entry & ~kStripNoTriangle) * kUvPairBytes).cast<ee::Float>();
                word->x = static_cast<uint16_t>(fixedUv(uv[0]));
                word->w = static_cast<uint16_t>(entry);
                word->z = 0x1000;
                word->y = static_cast<uint16_t>(fixedUv(uv[1]));
                sce::vif1PacketAddGsData(packet, word, 1);
                ++vuAddress;
                --entries;
            } while (entries != 0);
        }
        else if (mode == kModeList)
        {
            for (; entries >= 3u; entries -= 3)
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertex = *cursor;
                    ++cursor;
                    sce::vif1PacketAddWords(packet, uvs + vertex * kUvPairBytes, 2);
                    ++vuAddress;
                }
        }
        return vuAddress;
    }
    READABLE_FUNCTION_OWN(addRigidUvs, Verified, dm2, addRigidUvPairs);

    // buildRigid: a rigid part's packet built (as buildTerrain's: GIF tag
    // 8, UVs 9, colours 10, positions 11, each writer at the tag's next VU
    // address on). The packet's bytes.
    int32_t buildRigid(ee::Ptr<ZgePart> part, uint32_t buffer, uint32_t materials, uint32_t streams)
    {
        ee::Local<uint32_t> triangles;
        ee::Local<uint32_t> firstChunk;
        ee::Local<uint32_t> materialStart;
        const ee::Ptr<PacketMaterial> table(materials);
        uint32_t material = 255;
        uint32_t filled = 0; // ZGE2's: the material table's entries filled
        uint32_t sent = 0;
        *firstChunk = 1;
        *triangles = 0;
        beginBuildPacket(kBuildPacket, buffer);
        Group group = streamRecords<StripGroup>(part->groups);
        for (int32_t groupsLeft = part->groupCount; groupsLeft != 0; --groupsLeft)
        {
            uint32_t done = 0;
            do
            {
                *materialStart = 0;
                const uint32_t chunk = chunkEntries(group, done, triangles, 60);
                const uint32_t submesh = submeshOf(group);
                sent += chunk;
                if (submesh != material)
                {
                    material = submesh;
                    // ZGE2 fills the table's entries in the order the submeshes come; ZGE3 and ZGE4 use the
                    // submesh's own entry.
                    startMaterial(table, game::readable::kIsDm2 ? filled++ : material, materialStart);
                }
                const uint32_t vuAddress = static_cast<uint32_t>(addGifTag(kBuildPacket, chunk, 8, 1, firstChunk.address(), streams));
                addRigidUvs(kBuildPacket, vuAddress, chunk, done, group, part, streams);
                if constexpr (game::readable::kIsBmx)
                {
                    // ZGE4 sends a part's normals, when it has them, where its colours go.
                    if (part->normals)
                        addRigidNormals(kBuildPacket, vuAddress + 1, chunk, done, group, part, streams);
                    else
                        addRigidColours(kBuildPacket, vuAddress + 1, chunk, done, group, part, streams READABLE_PARAM_ONLY(bmx, kBuilderColourShift));
                }
                else
                    addRigidColours(kBuildPacket, vuAddress + 1, chunk, done, group, part, streams READABLE_PARAM_ONLY(bmx, kBuilderColourShift));
                addRigidPositions(kBuildPacket, vuAddress + 2, chunk, done, group, part, streams, triangles);
                done += chunk;
                addMicroprogramCall(kBuildPacket, materialStart, streams);
            } while (done < group->count);
            group = nextGroup(group);
        }
        return finishPartPacket(part, sent, *triangles);
    }
    READABLE_FUNCTION(buildRigid, Verified);
}
