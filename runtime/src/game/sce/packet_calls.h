#pragma once

// Calls of Sony's libpkt (Runtime Library 2.4), the VIF1 and GIF packet
// builders the engine writes its DMA chains with. The game links the library
// statically and it runs as the recompiled original: no source of it is on
// disk, so it is not written here (not_ported.tsv;
// config.game.toml recompiles sceVif1Pk* and sceGifPk* on purpose: an HLE
// version kept the packet differently and broke every per-frame chain).
// Each call is the game function by its name (docs/READABLE.md: a recompiled
// callee is one typed line over ee::game). The SDK name, where the ELF or the
// signature scan gives it, is in brackets; the rest are not settled.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "sce/packet_records.h"

#include <cstdint>
#include "sym/sce/packet_calls.h.h"

namespace sce
{
    using Vif1PacketPtr = ee::Ptr<Vif1Packet>;
    using GifPacketPtr = ee::Ptr<GifPacket>;

    // ---- the VIF1 packet (libvifpk) ----------------------------------------------------------

    // A packet over `buffer`; empty again, from its start.
    inline void vif1PacketInit(Vif1PacketPtr packet, ee::Address buffer) { ee::game(sym::vif1PacketInit, packet, buffer); }
    inline void vif1PacketReset(Vif1PacketPtr packet) { ee::game(sym::vif1PacketReset, packet); }
    // Pads the packet to a quadword with zero words and closes the open DMA tag (its QWC: the
    // quadwords after it); the write position.
    inline uint32_t vif1PacketCloseDmaTag(Vif1PacketPtr packet) { return ee::game<uint32_t>(sym::vif1PacketCloseDmaTag, packet); }
    // DMA tags, each closing the open one: [sceVif1PkCnt] "cnt" (the data follows), [sceVif1PkNext]
    // "next" to the chain at `address`, [sceVif1PkCall] "call" to the chain at `address`,
    // [sceVif1PkRet] "ret" (back to the caller's chain), "end"; `flags` goes into the tag's first word.
    inline void vif1PacketCnt(Vif1PacketPtr packet, uint32_t flags) { ee::game(sym::vif1PacketCnt, packet, flags); }
    inline void vif1PacketNext(Vif1PacketPtr packet, ee::Address address, uint32_t flags) { ee::game(sym::vif1PacketNext, packet, address, flags); }
    inline void vif1PacketCall(Vif1PacketPtr packet, ee::Address address, uint32_t flags) { ee::game(sym::vif1PacketCall, packet, address, flags); }
    inline void vif1PacketRet(Vif1PacketPtr packet, uint32_t flags) { ee::game(sym::vif1PacketRet, packet, flags); }
    inline void vif1PacketEnd(Vif1PacketPtr packet, uint32_t flags) { ee::game(sym::vif1PacketEnd, packet, flags); }
    // [sceVif1PkRef] A "ref" tag (`size` quadwords at `address`) with two VIF codes; it stays closed.
    inline void vif1PacketRef(Vif1PacketPtr packet, ee::Address address, uint32_t size, uint32_t code0, uint32_t code1, uint32_t flags)
    {
        ee::game(sym::vif1PacketRef, packet, address, size, code0, code1, flags);
    }
    // [sceVif1PkAlign] Zero words up to the next position `pos` words into a block of 2^(bit + 2)
    // bytes (at least one word).
    inline void vif1PacketAlign(Vif1PacketPtr packet, uint32_t bit, uint32_t pos) { ee::game(sym::vif1PacketAlign, packet, bit, pos); }

    // [sceVif1PkOpenUpkCode] STCYCL cl, wl, then an UNPACK to VU1 address `address` in `mode`, its NUM
    // left open; [sceVif1PkCloseUpkCode] fills it in from the data written since.
    inline void vif1PacketOpenUnpack(Vif1PacketPtr packet, uint32_t address, uint32_t mode, uint32_t cycle, uint32_t write)
    {
        ee::game(sym::vif1PacketOpenUnpack, packet, address, mode, cycle, write);
    }
    inline void vif1PacketCloseUnpack(Vif1PacketPtr packet) { ee::game(sym::vif1PacketCloseUnpack, packet); }
    // [sceVif1PkOpenDirectCode] A VIF DIRECT code (with the interrupt bit if asked) at the next word 3
    // of a quadword, its size left open; the close fills it in (the quadwords written since).
    inline void vif1PacketOpenDirect(Vif1PacketPtr packet, uint32_t interrupt) { ee::game(sym::vif1PacketOpenDirect, packet, interrupt); }
    inline void vif1PacketCloseDirect(Vif1PacketPtr packet) { ee::game(sym::vif1PacketCloseDirect, packet); }
    // A GIF tag (a whole 128-bit register) at the write position, left open; [sceVif1PkCloseGifTag]
    // fills in its NLOOP from the data after it, then zero words to a quadword.
    inline void vif1PacketOpenGifTag(Vif1PacketPtr packet, ee::Qword tag) { ee::game(sym::vif1PacketOpenGifTag, packet, tag); }
    inline void vif1PacketCloseGifTag(Vif1PacketPtr packet) { ee::game(sym::vif1PacketCloseGifTag, packet); }

    // One word appended: the library has the same code three times; the engine adds VIF codes with
    // the first, the microprogram calls (MSCAL) with the second, data words with the third.
    inline void vif1PacketAddCode(Vif1PacketPtr packet, uint32_t code) { ee::game(sym::vif1PacketAddCode, packet, code); }
    inline void vif1PacketAddCallCode(Vif1PacketPtr packet, uint32_t code) { ee::game(sym::vif1PacketAddCallCode, packet, code); }
    inline void vif1PacketAddWord(Vif1PacketPtr packet, uint32_t word) { ee::game(sym::vif1PacketAddWord, packet, word); }
    // `count` words; a doubleword (two copies of the same code); a quadword handed in a 128-bit
    // register; `count` quadwords of data (the engine's every block of data, about 45,000 a frame).
    inline void vif1PacketAddWords(Vif1PacketPtr packet, ee::Address words, uint32_t count) { ee::game(sym::vif1PacketAddWords, packet, words, count); }
    inline void vif1PacketAddDword(Vif1PacketPtr packet, uint64_t value) { ee::game(sym::vif1PacketAddDword, packet, value); }
    inline void vif1PacketAddDword2(Vif1PacketPtr packet, uint64_t value) { ee::game(sym::vif1PacketAddDword2, packet, value); }
    inline void vif1PacketAddQword(Vif1PacketPtr packet, ee::Qword value) { ee::game(sym::vif1PacketAddQword, packet, value); }
    inline void vif1PacketAddQwords(Vif1PacketPtr packet, ee::Address data, int32_t count) { ee::game(sym::vif1PacketAddQwords, packet, data, count); }
    // [sceVif1PkAddGsAD] An A+D pair: the register's value, then its address (a quadword);
    // [sceVif1PkAddGsDataN] `count` doublewords.
    inline void vif1PacketAddGsAd(Vif1PacketPtr packet, uint32_t address, uint64_t value) { ee::game(sym::vif1PacketAddGsAd, packet, address, value); }
    inline void vif1PacketAddGsData(Vif1PacketPtr packet, ee::Address data, uint32_t count) { ee::game(sym::vif1PacketAddGsData, packet, data, count); }

    // ---- the GIF packet (libgifpk) -----------------------------------------------------------

    // [sceGifPkInit] A packet over `buffer`; [sceGifPkReset] empty again, from its start;
    // [sceGifPkTerminate] pads to a quadword with zero words and closes the open DMA tag (its QWC),
    // the write position; the quadwords written.
    inline void gifPacketInit(GifPacketPtr packet, ee::Address buffer) { ee::game(sym::gifPacketInit, packet, buffer); }
    inline void gifPacketReset(GifPacketPtr packet) { ee::game(sym::gifPacketReset, packet); }
    inline uint32_t gifPacketTerminate(GifPacketPtr packet) { return ee::game<uint32_t>(sym::gifPacketTerminate, packet); }
    inline uint32_t gifPacketSize(GifPacketPtr packet) { return ee::game<uint32_t>(sym::gifPacketSize, packet); }
    // DMA tags, each closing the open one: [sceGifPkCnt] "cnt", [sceGifPkCall] "call" to the chain at
    // `address`, [sceGifPkEnd] "end", "ref" (`size` quadwords at `address`, it stays closed),
    // [sceGifPkRet] "ret"; word2 and word3 are the tag's upper half, `flags` goes into its first word.
    inline void gifPacketCnt(GifPacketPtr packet, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(sym::gifPacketCnt, packet, word2, word3, flags); }
    inline void gifPacketCall(GifPacketPtr packet, ee::Address address, uint32_t word2, uint32_t word3, uint32_t flags)
    {
        ee::game(sym::gifPacketCall, packet, address, word2, word3, flags);
    }
    inline void gifPacketEnd(GifPacketPtr packet, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(sym::gifPacketEnd, packet, word2, word3, flags); }
    inline void gifPacketRef(GifPacketPtr packet, ee::Address address, uint32_t size, uint32_t word2, uint32_t word3, uint32_t flags)
    {
        ee::game(sym::gifPacketRef, packet, address, size, word2, word3, flags);
    }
    inline void gifPacketRet(GifPacketPtr packet, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(sym::gifPacketRet, packet, word2, word3, flags); }
    // One doubleword.
    inline void gifPacketAddDword(GifPacketPtr packet, uint64_t value) { ee::game(sym::gifPacketAddDword, packet, value); }
    // A GIF tag on a quadword, left open; the close counts what was added since into its NLOOP and
    // zeroes the gap to the next quadword; a GS register write in A+D form (the value, then the
    // register's address).
    inline void sceGifPkOpenGifTag(GifPacketPtr packet, ee::Qword tag) { ee::game(sym::sceGifPkOpenGifTag, packet, tag); }
    inline void sceGifPkCloseGifTag(GifPacketPtr packet) { ee::game(sym::sceGifPkCloseGifTag, packet); }
    inline void sceGifPkAddGsAD(GifPacketPtr packet, uint64_t address, uint64_t value) { ee::game(sym::sceGifPkAddGsAD, packet, address, value); }
}
