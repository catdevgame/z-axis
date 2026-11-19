#pragma once

// A part's VIF packet: what VU1 draws the part from (buildPartPacket builds it;
// the class renderers drawRigid / drawTerrain / drawSkinned send it).
//
// The packet is one DMA chain per material: a RET tag, then for each chunk
// of at most 60 strip entries of the material's groups a VIF block (an
// unpack header, the streams unpacked for those entries, MSCAL or MSCNT),
// padded to a quadword. A class builder writes it with libvifpk:
//   rigid (types 4, 0x18; buildRigid)   per chunk 24 header + UVs 16 + 8n
//                                         + colours 8 + 4n + positions 16 + 12n + 4
//   terrain (0x20; buildTerrain)          24 + UVs 16 + 4n + normals 8 + 8n
//                                         + positions 16 + 12n + 4
//   skinned (8, 0xC; buildSkinned)        24 + UVs 16 + 4n + positions and
//                                         weights 16 + 16n + normals or
//                                         colours 8 + 8n + 4
// (each stream's size is the one its writer skips when the stream is not
// rebuilt, so a rebuild in place keeps the layout).

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/render/part.h"

#include <cstdint>

namespace sce
{
    struct Vif1Packet;
}

namespace zge
{
    // buildPartPacket(part, whole, streams): builds the part's packet in the
    // buffer to build (bufferFlags bit 0). whole: the first build, into a
    // scratch buffer, then copied to an allocation of its exact size with
    // the material table after it; else the streams whose bits are set
    // (kPart* low bits, 0x7FF all) are rebuilt in the buffer's existing
    // allocation (one of the same size is made if there is none). Clears
    // the part's rebuild bits. Returns 1.
    //
    // The original builds a first packet in a fixed 160 KB scratch buffer
    // (malloc(0x28000)) that a larger part overruns (the Xbox release's
    // skaters); this one makes it as large as the part's packet when that is
    // more (packetBytes).
    int32_t buildPartPacket(ee::Ptr<ZgePart> part, int32_t whole, uint32_t streams);
    // ZGE2's buildPartPacket (ps2/part_packet.cpp), handed the node too; its drawNodeInView calls it by this name.
    int32_t buildPartPacketAndTable(ee::Ptr<Node> node, ee::Ptr<ZgePart> part, int32_t whole, uint32_t streams);

    // The original's scratch buffer for a first build.
    constexpr uint32_t kPacketScratchBytes = 0x28000u;

    // The bytes the class builder of the part's render type writes for its
    // packet, from the part's strip groups in RAM, with the packet starting
    // at an address `startAlignment` (0, 4, 8, 12) past a quadword; 0 for a
    // render type with no builder.
    uint32_t packetBytes(const ZgePart &part, uint32_t startAlignment);
}

namespace zge
{
    // (ps2/part_packet.cpp)
    void addGsRegister(sce::Vif1Packet *packet, uint32_t gsRegister, uint64_t value);
    void addGsRegister2(sce::Vif1Packet *packet, uint32_t gsRegister, uint64_t value);
}
