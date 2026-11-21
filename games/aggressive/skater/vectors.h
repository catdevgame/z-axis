#pragma once

// The skater code's vector moves as the original makes them: a quadword
// load and store (lq / sq ignore the low four address bits), and the upper
// doubleword such a load leaves in t0, which the engine's VU0 sequences
// move into vf5 with QMTC2 (ee::vu0::Machine::scale's t0High).

#include "ee/ee.h"

#include <cstring>

namespace aggressive::skater
{
    inline void copyQuadword(uint32_t to, uint32_t from)
    {
        std::memcpy(ee::at<uint8_t>(to & ~0xFu), ee::at<uint8_t>(from & ~0xFu), 16);
    }
    inline uint64_t highDoubleword(uint32_t vector)
    {
        return *ee::at<uint64_t>((vector & ~0xFu) + 8);
    }
    // VU0's constant register vf0, (0, 0, 0, 1), stored at `to` (the
    // original's SQC2 vf0: a cleared vector, w 1).
    inline void storeVf0(R5900Context *ctx, uint32_t to)
    {
        _mm_storeu_si128(ee::at<__m128i>(to & ~0xFu), _mm_castps_si128(ctx->vu0_vf[0]));
    }
}
