#ifndef PS2_GUEST_CPU_H
#define PS2_GUEST_CPU_H

// The EE's state as generated code sees it: R5900Context (registers, COP0, COP1, VU0 macro mode) and the helpers
// that read and write it alone. Freestanding (no library header): a game module (game_module/) is compiled against it
// as the runtime is, so both sides agree on the layout (game_module/abi.h checks it at load).

#include <stddef.h>
#include <stdint.h>
// (A game module takes the x86 intrinsics from immintrin.h on Windows too: clang's intrin.h needs the C library's
// setjmp.h.)
#if defined(_MSC_VER) && !defined(PS2X_GAME_MODULE)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif

enum PS2Exception
{
    EXCEPTION_TLB_REFILL = 0x02,          // TLB refill/load exception
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,  // Address error on load
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05, // Address error on store
    EXCEPTION_SYSCALL = 0x08,             // SYSCALL instruction
    EXCEPTION_BREAKPOINT = 0x09,          // BREAK instruction
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C, // From MIPS spec
    EXCEPTION_TRAP = 0x0D,             // Trap instruction condition met
};

// PS2 CPU context (R5900)
struct alignas(16) R5900Context
{
    // General Purpose Registers (128-bit)
    __m128i r[32]; // Main registers

    // Control registers
    uint32_t pc;         // Program counter
    uint64_t insn_count; // Instruction counter
    uint64_t hi, lo;     // HI/LO registers for mult/div results
    uint64_t hi1, lo1;   // Secondary HI/LO registers for MULT1/DIV1
    uint32_t sa;         // Shift amount register

    // VU0 registers (when used in macro mode)
    __m128 vu0_vf[32];        // VU0 vector float registers
    uint16_t vi[16];          // VU0 vector integer registers
    float vu0_q;              // VU0 Q register (quotient)
    float vu0_p;              // VU0 P register (EFU result)
    float vu0_i;              // VU0 I register (integer value)
    __m128 vu0_r;             // VU0 R register
    __m128 vu0_acc;           // VU0 ACC accumulator register
    uint16_t vu0_status;      // VU0 status register
    uint32_t vu0_mac_flags;   // VU0 MAC flags
    uint32_t vu0_clip_flags;  // VU0 clipping flags
    uint32_t vu0_clip_flags2; // VU0 clipping flags
    uint32_t vu0_cmsar0;      // VU0 microprogram start address
    uint32_t vu0_cmsar1;      // VU0 microprogram start address
    uint32_t vu0_cmsar2;      // VU0 microprogram start address
    uint32_t vu0_cmsar3;      // VU0 microprogram start address
    uint32_t vu0_vpu_stat;
    uint32_t vu0_vpu_stat2; // extra VPU status (used by CR_VPU_STAT2)
    uint32_t vu0_vpu_stat3; // extra VPU status 3
    uint32_t vu0_vpu_stat4; // extra VPU status 4
    uint32_t vu0_tpc;       // TPC (VU0 PC)
    uint32_t vu0_tpc2;      // second TPC
    uint32_t vu0_fbrst;     // VIF/VU reset register
    uint32_t vu0_fbrst2;    // FBRST2
    uint32_t vu0_fbrst3;    // FBRST3
    uint32_t vu0_fbrst4;    // FBRST4
    uint32_t vu0_itop;
    uint32_t vu0_top;
    uint32_t vu0_info;
    uint32_t vu0_xitop; // VU0 XITOP - input ITOP for VIF/VU sync
    uint32_t vu0_pc;

    float vu0_cf[4]; // VU0 FMAC control floating-point registers

    // COP0 System control registers
    uint32_t cop0_index;
    uint32_t cop0_random;
    uint32_t cop0_entrylo0;
    uint32_t cop0_entrylo1;
    uint32_t cop0_context;
    uint32_t cop0_pagemask;
    uint32_t cop0_wired;
    uint32_t cop0_badvaddr;
    uint32_t cop0_count;
    uint32_t cop0_entryhi;
    uint32_t cop0_compare;
    uint32_t cop0_status;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
    uint32_t cop0_prid;
    uint32_t cop0_config;
    uint32_t cop0_badpaddr;
    uint32_t cop0_debug;
    uint32_t cop0_perf;
    uint32_t cop0_taglo;
    uint32_t cop0_taghi;
    uint32_t cop0_errorepc;

    // LL/SC reservation state (not part of COP0 Status bits).
    uint32_t llbit;
    uint32_t lladdr;

    // Delay slot state tracking
    bool in_delay_slot;
    uint32_t branch_pc;

    // COP2 control registers (VU0 integer + control)
    uint32_t cop2_ccr[32];

    // FPU registers (COP1)
    float f[32];
    float f_acc;    // FPU accumulator
    uint32_t fcr31; // Control/status register

    R5900Context()
    {
        __builtin_memset(static_cast<void *>(this), 0, sizeof(*this));

        // Initialize VU0 registers
        vu0_q = 1.0f; // Q register usually initialized to 1.0

        // Reset COP0 registers
        cop0_random = 47; // Start at maximum value
        // Status as the EE kernel leaves it at handoff. IE (bit 0) and EIE
        // (bit 16) are separate enables and guest code reads both; libkernel's
        // StartThread refuses to run while IE is clear.
        cop0_status = 0x00010001; // EIE | IE
        cop0_prid = 0x00002e20; // CPU ID for R5900

        in_delay_slot = false;
        branch_pc = 0;
    }

    ~R5900Context() = default;
};

// The layout of R5900Context as a number (each field's offset, and the size, hashed): a game module built against
// another layout is refused at load (game_module/abi.h).
#define PS2_CPU_CONTEXT_FIELDS(X) \
    X(r) \
    X(pc) \
    X(insn_count) \
    X(hi) \
    X(lo) \
    X(hi1) \
    X(lo1) \
    X(sa) \
    X(vu0_vf) \
    X(vi) \
    X(vu0_q) \
    X(vu0_p) \
    X(vu0_i) \
    X(vu0_r) \
    X(vu0_acc) \
    X(vu0_status) \
    X(vu0_mac_flags) \
    X(vu0_clip_flags) \
    X(vu0_clip_flags2) \
    X(vu0_cmsar0) \
    X(vu0_cmsar1) \
    X(vu0_cmsar2) \
    X(vu0_cmsar3) \
    X(vu0_vpu_stat) \
    X(vu0_vpu_stat2) \
    X(vu0_vpu_stat3) \
    X(vu0_vpu_stat4) \
    X(vu0_tpc) \
    X(vu0_tpc2) \
    X(vu0_fbrst) \
    X(vu0_fbrst2) \
    X(vu0_fbrst3) \
    X(vu0_fbrst4) \
    X(vu0_itop) \
    X(vu0_top) \
    X(vu0_info) \
    X(vu0_xitop) \
    X(vu0_pc) \
    X(vu0_cf) \
    X(cop0_index) \
    X(cop0_random) \
    X(cop0_entrylo0) \
    X(cop0_entrylo1) \
    X(cop0_context) \
    X(cop0_pagemask) \
    X(cop0_wired) \
    X(cop0_badvaddr) \
    X(cop0_count) \
    X(cop0_entryhi) \
    X(cop0_compare) \
    X(cop0_status) \
    X(cop0_cause) \
    X(cop0_epc) \
    X(cop0_prid) \
    X(cop0_config) \
    X(cop0_badpaddr) \
    X(cop0_debug) \
    X(cop0_perf) \
    X(cop0_taglo) \
    X(cop0_taghi) \
    X(cop0_errorepc) \
    X(llbit) \
    X(lladdr) \
    X(in_delay_slot) \
    X(branch_pc) \
    X(cop2_ccr) \
    X(f) \
    X(f_acc) \
    X(fcr31)
constexpr uint32_t ps2CpuContextLayout()
{
    uint32_t hash = 2166136261u;
#define PS2_CPU_CONTEXT_HASH_FIELD(field) hash = (hash ^ static_cast<uint32_t>(offsetof(R5900Context, field))) * 16777619u;
    PS2_CPU_CONTEXT_FIELDS(PS2_CPU_CONTEXT_HASH_FIELD)
#undef PS2_CPU_CONTEXT_HASH_FIELD
    return (hash ^ static_cast<uint32_t>(sizeof(R5900Context))) * 16777619u;
}

// VU0 macro mode: MAC and status flags of an FMAC result, as PCSX2's
// VU_MACx_UPDATE / VU_STAT_UPDATE. res is the result before the dest mask,
// dest the xyzw field (x = 8). MAC: per written lane Z (bits 0-3), S (4-7),
// U (8-11), O (12-15), x in the high bit of each nibble; unwritten lanes are
// clear. Status: Z S U O in bits 0-3, ORed into the sticky bits 6-9; I/D and
// their sticky bits are kept. Results reach here already flushed and clamped
// (ps2_vadd etc.), so underflow reads as zero and overflow is not reported.
inline void ps2_vu0_fmac_flags(R5900Context *ctx, __m128 res, uint32_t dest)
{
    alignas(16) uint32_t bits[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(bits), _mm_castps_si128(res));
    uint32_t mac = 0u;
    for (uint32_t lane = 0; lane < 4u; ++lane)
    {
        const uint32_t laneBit = 8u >> lane;
        if ((dest & laneBit) == 0u)
            continue;
        const uint32_t b = bits[lane];
        const uint32_t exponent = (b >> 23) & 0xFFu;
        if ((b & 0x80000000u) != 0u)
            mac |= laneBit << 4;
        if ((b & 0x7FFFFFFFu) == 0u)
            mac |= laneBit;
        else if (exponent == 0u)
            mac |= laneBit | (laneBit << 8);
        else if (exponent == 0xFFu)
            mac |= laneBit << 12;
    }
    const uint32_t current = ((mac & 0x000Fu) ? 0x1u : 0u) | ((mac & 0x00F0u) ? 0x2u : 0u) |
                             ((mac & 0x0F00u) ? 0x4u : 0u) | ((mac & 0xF000u) ? 0x8u : 0u);
    ctx->vu0_mac_flags = mac;
    ctx->vu0_status = static_cast<uint16_t>((ctx->vu0_status & 0xFF0u) | current | (current << 6));
}

inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    // Check if reg is valid (0-31)
    if (reg < 0 || reg > 31)
        return 0;
    if (reg == 0)
        return 0;
    return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 0));
}

inline void setReturnU32(R5900Context *ctx, uint32_t value)
{
    // R5900 sign-extends 32-bit results into 64-bit GPR, even for unsigned values.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value))); // $v0
}

inline void setReturnS32(R5900Context *ctx, int32_t value)
{
    // Signed 32-bit return should be sign-extended when observed as 64-bit.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value)); // $v0
}

inline void setReturnU64(R5900Context *ctx, uint64_t value)
{
    // Keep both conventions: full 64-bit value in $v0 and high 32-bit in $v1.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    ctx->r[3] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<uint32_t>(value >> 32)));
}

inline void ps2TraceGuestWrite(uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t size,
                               uint64_t valueLo,
                               uint64_t valueHi,
                               const char *op,
                               const R5900Context *ctx)
{
    (void)rdram;
    (void)guestAddr;
    (void)size;
    (void)valueLo;
    (void)valueHi;
    (void)op;
    (void)ctx;
    // TODO we dont need this anymore so on next release it will be deleted
}

inline void ps2TraceGuestRangeWrite(uint8_t *rdram,
                                    uint32_t guestAddr,
                                    uint32_t size,
                                    const char *op,
                                    const R5900Context *ctx)
{
    (void)rdram;
    (void)guestAddr;
    (void)size;
    (void)op;
    (void)ctx;
    // TODO we dont need this anymore so on next release it will be deleted
}

#endif // PS2_GUEST_CPU_H
