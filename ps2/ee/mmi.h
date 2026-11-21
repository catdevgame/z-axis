#pragma once

// The R5900's parallel instructions on whole 128-bit registers, as generated
// code runs them (PS2_* in ee_instructions.h), for drafts of the SDK's
// colour conversion and string functions (readable_draft.py). Each takes and
// returns the register as ee::Qword; `hiLo` is the 64-bit HI:LO that MADD adds to.
//
//     const ee::Qword sum = ee::mmi::paddh(a, b);
//     ee::mmi::setShiftBytes(offset);                  // mtsab offset, 0
//     const ee::Qword bytes = ee::mmi::qfsrv(hi, lo);

#include "abi.h"

#include <cstdint>

namespace ee::mmi
{
    inline __m128i vec(Qword q) { return _mm_set_epi64x(static_cast<int64_t>(q.high), static_cast<int64_t>(q.low)); }
    inline Qword qword(__m128i v)
    {
        Qword q;
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&q), v);
        return q;
    }

#define EE_MMI_BINARY(NAME, FORM) \
    inline Qword NAME(Qword rs, Qword rt) { return qword(FORM(vec(rs), vec(rt))); }
    EE_MMI_BINARY(paddh, PS2_PADDH)
    EE_MMI_BINARY(psubb, PS2_PSUBB)
    EE_MMI_BINARY(psubw, PS2_PSUBW)
    EE_MMI_BINARY(pand, PS2_PAND)
    EE_MMI_BINARY(por, PS2_POR)
    EE_MMI_BINARY(pnor, PS2_PNOR)
    EE_MMI_BINARY(pxor, PS2_PXOR)
    EE_MMI_BINARY(pcgth, PS2_PCGTH)
    EE_MMI_BINARY(pmaxh, PS2_PMAXH)
    EE_MMI_BINARY(pminh, PS2_PMINH)
    EE_MMI_BINARY(pcpyld, PS2_PCPYLD)
    EE_MMI_BINARY(pcpyud, _mm_unpackhi_epi64)
    EE_MMI_BINARY(pextlb, PS2_PEXTLB)
    EE_MMI_BINARY(pextub, PS2_PEXTUB)
    EE_MMI_BINARY(pextlw, PS2_PEXTLW)
    EE_MMI_BINARY(pextuw, PS2_PEXTUW)
    EE_MMI_BINARY(ppacb, PS2_PPACB)
#undef EE_MMI_BINARY

    inline Qword psrlh(Qword rt, int bits) { return qword(_mm_srli_epi16(vec(rt), bits)); }
    inline Qword psrah(Qword rt, int bits) { return qword(_mm_srai_epi16(vec(rt), bits)); }
    inline Qword psllh(Qword rt, int bits) { return qword(_mm_slli_epi16(vec(rt), bits)); }
    // PCPYH: each halfword's lowest repeated in its doubleword.
    inline Qword pcpyh(Qword rt)
    {
        const uint64_t l = rt.low & 0xFFFFu, h = rt.high & 0xFFFFu;
        return Qword{l * 0x0001000100010001ull, h * 0x0001000100010001ull};
    }

    // The funnel shift amount (SA), in bits.
    inline uint32_t shift() { return t_ee.frame->ctx->sa; }
    inline void setShift(uint32_t value) { t_ee.frame->ctx->sa = value & 0x7Fu; }                  // MTSA
    inline void setShiftBytes(uint32_t value) { t_ee.frame->ctx->sa = (value & 0xFu) << 3; }       // MTSAB
    inline void setShiftHalfwords(uint32_t value) { t_ee.frame->ctx->sa = (value & 0x7u) << 4; }   // MTSAH
    inline Qword qfsrv(Qword rs, Qword rt) { return qword(PS2_QFSRV(vec(rs), vec(rt), shift() & 0x7Fu)); }

    // HI:LO as one 64-bit value.
    inline int64_t hiLo(int32_t hi, int32_t lo)
    {
        return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo));
    }
}
