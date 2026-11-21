#pragma once

// The records of Sony's libpkt (Runtime Library 2.4) as they sit in game
// memory: sceVif1Packet and sceGifPacket, the state of a DMA packet being
// built. The engine keeps them in its own structures and on the stack, and
// hands them to the library's functions, which stay the recompiled originals
// (sce/packet_calls.h calls them; not_ported.tsv). These describe the
// memory only: only the fields the engine reads are laid out.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace sce
{
    // sceVif1Packet: a VIF1 DMA packet being built.
    struct Vif1Packet
    {
        // Where the next word goes (a 32-bit pointer into the packet buffer).
        ee::Ptr<uint32_t> current; // +0x00
        ee::Ptr<uint32_t> bufferStart; // +0x04 [inferred] where the buffer starts (vif1PacketReset goes back to it)
        ee::Ptr<uint32_t> dmaTag;  // +0x08 the open DMA tag (sceVif1PkRet and the other tag writers)
        // The open VIF code (an UNPACK: sceVif1PkCloseUpkCode fills in its
        // NUM, byte 2, and clears this), 0 when none is open.
        ee::Ptr<uint32_t> vifCode; // +0x0C
        uint32_t unpackSizes;      // +0x10 the open UNPACK's element size and cycle (vif1PacketOpenUnpack)
        ee::Ptr<uint64_t> gifTag;  // +0x14 the open GIF tag (vif1PacketOpenGifTag; sceVif1PkCloseGifTag fills in NLOOP)
    };
    static_assert(offsetof(Vif1Packet, vifCode) == 0x0C && offsetof(Vif1Packet, gifTag) == 0x14);

    // sceGifPacket: a GIF DMA packet being built.
    struct GifPacket
    {
        ee::Ptr<uint32_t> current; // +0x00 where the next word goes
        ee::Ptr<uint32_t> start;   // +0x04 the buffer (the DMA chain's first tag)
        ee::Ptr<uint32_t> dmaTag;  // +0x08 the open DMA tag (its QWC filled in when it is closed), 0 none
        ee::Ptr<uint32_t> gifTag;  // +0x0C the open GIF tag (its NLOOP filled in when it is closed), 0 none
    };
    static_assert(offsetof(GifPacket, gifTag) == 0x0C);
}
