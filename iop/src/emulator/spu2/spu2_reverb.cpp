// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Ported from PCSX2 pcsx2/SPU2/Reverb.cpp and ReverbResample.cpp
// (commit 091ab687). The resampling filters use PCSX2's portable
// "_reference" implementations instead of the SSE/AVX ones; the SIMD
// versions use rounding/saturating 16-bit multiplies, so results can differ
// from PCSX2 by a least-significant bit.

#include "spu2_internal.h"

#include <algorithm>

namespace zx::spu2
{
    namespace
    {
        constexpr u32 NUM_TAPS = 39;
        // 39 tap filter, the 0's could be optimized out
        constexpr std::array<s16, 48> filter_down_coefs = {
            -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0,
            -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0,
            35, 0, -10, 0, 2, 0, -1};

        constexpr std::array<s16, 48> make_up_coefs()
        {
            std::array<s16, 48> ret = {};
            for (u32 i = 0; i < NUM_TAPS; i++)
            {
                const s32 v = filter_down_coefs[i] * 2;
                ret[i] = static_cast<s16>(v < INT16_MIN ? INT16_MIN : (v > INT16_MAX ? INT16_MAX : v));
            }
            return ret;
        }

        constexpr std::array<s16, 48> filter_up_coefs = make_up_coefs();
    }

    s32 ReverbDownsample(V_Core &core, bool right)
    {
        const int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
        s32 out = 0;

        for (u32 i = 0; i < NUM_TAPS; i++)
        {
            out += core.RevbDownBuf[right][index + i] * filter_down_coefs[i];
        }

        out >>= 15;

        return clamp_mix(out);
    }

    StereoOut32 ReverbUpsample(V_Core &core)
    {
        const int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
        s32 l = 0, r = 0;

        for (u32 i = 0; i < NUM_TAPS; i++)
        {
            l += core.RevbUpBuf[0][index + i] * filter_up_coefs[i];
            r += core.RevbUpBuf[1][index + i] * filter_up_coefs[i];
        }

        l >>= 15;
        r >>= 15;

        return {clamp_mix(l), clamp_mix(r)};
    }

    s32 V_Core::RevbGetIndexer(s32 offset)
    {
        const u32 start = EffectsStartA & 0x3f'ffff;
        const u32 end = (EffectsEndA & 0x3f'ffff) | 0xffff;

        u32 x = ((Sys->Cycles >> 1) + offset) % ((end - start) + 1);

        x += start;

        return x & 0xf'ffff;
    }

    StereoOut32 V_Core::DoReverb(StereoOut32 Input)
    {
        if (EffectsStartA >= EffectsEndA)
        {
            return StereoOut32();
        }

        s16 *const mem = Sys->spu2mem.data();
        V_Core *const cores = Sys->Cores;

        Input = clamp_mix(Input);

        RevbDownBuf[0][RevbSampleBufPos] = static_cast<s16>(Input.Left);
        RevbDownBuf[1][RevbSampleBufPos] = static_cast<s16>(Input.Right);
        RevbDownBuf[0][RevbSampleBufPos | 64] = static_cast<s16>(Input.Left);
        RevbDownBuf[1][RevbSampleBufPos | 64] = static_cast<s16>(Input.Right);

        const bool R = Sys->Cycles & 1;

        // Calculate the read/write addresses we'll be needing for this session of reverb.

        const u32 same_src = RevbGetIndexer(R ? Revb.SAME_R_SRC : Revb.SAME_L_SRC);
        const u32 same_dst = RevbGetIndexer(R ? Revb.SAME_R_DST : Revb.SAME_L_DST);
        const u32 same_prv = RevbGetIndexer(R ? Revb.SAME_R_DST - 1 : Revb.SAME_L_DST - 1);

        const u32 diff_src = RevbGetIndexer(R ? Revb.DIFF_L_SRC : Revb.DIFF_R_SRC);
        const u32 diff_dst = RevbGetIndexer(R ? Revb.DIFF_R_DST : Revb.DIFF_L_DST);
        const u32 diff_prv = RevbGetIndexer(R ? Revb.DIFF_R_DST - 1 : Revb.DIFF_L_DST - 1);

        const u32 comb1_src = RevbGetIndexer(R ? Revb.COMB1_R_SRC : Revb.COMB1_L_SRC);
        const u32 comb2_src = RevbGetIndexer(R ? Revb.COMB2_R_SRC : Revb.COMB2_L_SRC);
        const u32 comb3_src = RevbGetIndexer(R ? Revb.COMB3_R_SRC : Revb.COMB3_L_SRC);
        const u32 comb4_src = RevbGetIndexer(R ? Revb.COMB4_R_SRC : Revb.COMB4_L_SRC);

        const u32 apf1_src = RevbGetIndexer(R ? (Revb.APF1_R_DST - Revb.APF1_SIZE) : (Revb.APF1_L_DST - Revb.APF1_SIZE));
        const u32 apf1_dst = RevbGetIndexer(R ? Revb.APF1_R_DST : Revb.APF1_L_DST);
        const u32 apf2_src = RevbGetIndexer(R ? (Revb.APF2_R_DST - Revb.APF2_SIZE) : (Revb.APF2_L_DST - Revb.APF2_SIZE));
        const u32 apf2_dst = RevbGetIndexer(R ? Revb.APF2_R_DST : Revb.APF2_L_DST);

        // Optimized IRQ testing: all buffer addresses are within the effects area.
        for (int i = 0; i < 2; i++)
        {
            if (FxEnable && cores[i].IRQEnable && ((cores[i].IRQA >= EffectsStartA) && (cores[i].IRQA <= EffectsEndA)))
            {
                const u32 a = cores[i].IRQA;
                if ((a == same_src) || (a == diff_src) ||
                    (a == same_dst) || (a == diff_dst) ||
                    (a == same_prv) || (a == diff_prv) ||
                    (a == comb1_src) || (a == comb2_src) ||
                    (a == comb3_src) || (a == comb4_src) ||
                    (a == apf1_dst) || (a == apf1_src) ||
                    (a == apf2_dst) || (a == apf2_src))
                {
                    Sys->SetIrqCall(i);
                }
            }
        }

        // Reverb algorithm pretty much directly ripped from http://drhell.web.fc2.com/ps1/
        // minus the 35 step FIR which just seems to break things.

        s32 in, same, diff, apf1, apf2, out;

        const auto MUL = [](s32 x, s32 y)
        { return (x * y) >> 15; };

        in = MUL(R ? Revb.IN_COEF_R : Revb.IN_COEF_L, ReverbDownsample(*this, R));

        same = MUL(Revb.IIR_VOL, in + MUL(Revb.WALL_VOL, mem[same_src]) - mem[same_prv]) + mem[same_prv];
        diff = MUL(Revb.IIR_VOL, in + MUL(Revb.WALL_VOL, mem[diff_src]) - mem[diff_prv]) + mem[diff_prv];

        out = MUL(Revb.COMB1_VOL, mem[comb1_src]) + MUL(Revb.COMB2_VOL, mem[comb2_src]) +
              MUL(Revb.COMB3_VOL, mem[comb3_src]) + MUL(Revb.COMB4_VOL, mem[comb4_src]);

        apf1 = out - MUL(Revb.APF1_VOL, mem[apf1_src]);
        out = mem[apf1_src] + MUL(Revb.APF1_VOL, apf1);
        apf2 = out - MUL(Revb.APF2_VOL, mem[apf2_src]);
        out = mem[apf2_src] + MUL(Revb.APF2_VOL, apf2);

        // According to no$psx the effects always run but don't always write back
        if (FxEnable)
        {
            mem[same_dst] = static_cast<s16>(clamp_mix(same));
            mem[diff_dst] = static_cast<s16>(clamp_mix(diff));
            mem[apf1_dst] = static_cast<s16>(clamp_mix(apf1));
            mem[apf2_dst] = static_cast<s16>(clamp_mix(apf2));
        }

        out = clamp_mix(out);

        RevbUpBuf[R][RevbSampleBufPos] = static_cast<s16>(out);
        RevbUpBuf[!R][RevbSampleBufPos] = 0;

        RevbUpBuf[R][RevbSampleBufPos | 64] = static_cast<s16>(out);
        RevbUpBuf[!R][RevbSampleBufPos | 64] = 0;

        RevbSampleBufPos = (RevbSampleBufPos + 1) & 63;

        return ReverbUpsample(*this);
    }
}
