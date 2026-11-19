#pragma once

// VIF codes (EE User's Manual 6.3, "VIFcode Reference"): the 32-bit commands
// VIF1 reads in front of the data it passes to VU1. A code is its command in
// bits 24-30 (bit 31 the interrupt bit), NUM in bits 16-23 (an UNPACK's
// quadwords, 256 as 0) and an immediate in bits 0-15.
//
// The codes are made as the game makes them: each field shifted into place
// and OR-ed, not masked to its width (as gs_registers.h's words).

#include <cstdint>

namespace zge::vif
{
    // The commands (CMD).
    constexpr uint32_t kNop = 0x00;
    constexpr uint32_t kStcycl = 0x01; // the cycle: WL quadwords written of every CL (immediate: CL bits 0-7, WL 8-15)
    constexpr uint32_t kOffset = 0x02; // the double buffer's second half, after BASE (VU1 quadwords)
    constexpr uint32_t kBase = 0x03;   // the double buffer's start (VU1 quadwords)
    constexpr uint32_t kItop = 0x04;   // the value VU1's program reads with XITOP
    constexpr uint32_t kMark = 0x07;   // a value the EE reads back (VIF1_MARK)
    constexpr uint32_t kFlushA = 0x13; // waits until VU1 has ended and every GIF path is idle
    constexpr uint32_t kMscal = 0x14;  // starts VU1's microprogram at the immediate
    constexpr uint32_t kMscnt = 0x17;  // continues the microprogram after its last stop
    constexpr uint32_t kStmask = 0x20; // the next word is the write mask
    constexpr uint32_t kStrow = 0x30;  // the next four words are the row registers

    // UNPACK's commands: 0x60, the masked bit (0x10), the vector's
    // components less one (bits 2-3) and its element size (bits 0-1: 0 32
    // bits, 1 16 bits, 2 8 bits).
    constexpr uint32_t kUnpackV2_32 = 0x64;
    constexpr uint32_t kUnpackV2_16 = 0x65;
    constexpr uint32_t kUnpackV3_32 = 0x68;
    constexpr uint32_t kUnpackV4_32 = 0x6C;
    constexpr uint32_t kUnpackV4_16 = 0x6D;
    constexpr uint32_t kUnpackV4_8 = 0x6E;
    constexpr uint32_t kUnpackMasked = 0x10; // the write mask (STMASK) applies

    // The interrupt bit: VIF1 interrupts the EE when it reaches the code.
    constexpr uint32_t kInterrupt = 1u << 31;
    // In an UNPACK's address (its immediate): relative to TOPS, the double
    // buffer's half VIF1 is writing (FLG).
    constexpr uint32_t kUnpackFromTops = 0x8000u;

    // A code: the command, the immediate and NUM.
    constexpr uint32_t code(uint32_t command, uint32_t immediate = 0, uint32_t num = 0)
    {
        return command << 24 | num << 16 | immediate;
    }

    constexpr uint32_t nop() { return code(kNop); }
    constexpr uint32_t flushA() { return code(kFlushA); }
    constexpr uint32_t mscnt() { return code(kMscnt); }
    constexpr uint32_t stmask() { return code(kStmask); }
    constexpr uint32_t strow() { return code(kStrow); }
    constexpr uint32_t mscal(uint32_t address) { return code(kMscal, address); }
    constexpr uint32_t mark(uint32_t value) { return code(kMark, value); }
    constexpr uint32_t base(uint32_t address) { return code(kBase, address); }
    constexpr uint32_t offset(uint32_t address) { return code(kOffset, address); }
    constexpr uint32_t itop(uint32_t value) { return code(kItop, value); }
    constexpr uint32_t stcycl(uint32_t cl, uint32_t wl) { return code(kStcycl, wl << 8 | cl); }
    // An UNPACK of `quadwords` to VU1 `address` (kUnpackFromTops in it:
    // relative to TOPS).
    constexpr uint32_t unpack(uint32_t command, uint32_t quadwords, uint32_t address)
    {
        return code(command, address, quadwords);
    }
    // A code's NUM: its bits, and an UNPACK's quadwords.
    constexpr uint32_t kNumBits = 0xFFu << 16;
    constexpr uint32_t unpackQuadwords(uint32_t unpackCode) { return unpackCode >> 16 & 0xFFu; }
}
