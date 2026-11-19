// Licence: ported from GCC 2.95.2 (https://ftp.gnu.org/gnu/gcc/gcc-2.95.2/): libgcc2.c and
// config/fp-bit.c are under the GPL, version 2 or (at your option) any later version, with
// the runtime exception below; this port takes the later version, GPL-3.0.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// Copyright (C) 1989, 92-98, 1999 Free Software Foundation, Inc.
//
// This file is part of GNU CC.
//
// GNU CC is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2, or (at your option)
// any later version.
//
// GNU CC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GNU CC; see the file COPYING.  If not, write to
// the Free Software Foundation, 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.
//
// As a special exception, if you link this library with other files,
//    some of which are compiled with GCC, to produce an executable,
//    this library does not by itself cause the resulting executable
//    to be covered by the GNU General Public License.
//    This exception does not however invalidate any other reasons why
//    the executable file might be covered by the GNU General Public License.
//
// This is a software floating point library which can be used instead of
//    the floating point routines in libgcc1.c for targets without hardware
//    floating point.
//  Copyright (C) 1994, 1995, 1996, 1997, 1998 Free Software Foundation, Inc.
//
// This file is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option) any
// later version.
//
// In addition to the permissions in the GNU General Public License, the
// Free Software Foundation gives you unlimited permission to link the
// compiled version of this file with other programs, and to distribute
// those programs without any restriction coming from the use of this
// file.  (The General Public License restrictions do apply in other
// respects; for example, they cover modification of the file, and
// distribution when not linked into another program.)
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.
//
// As a special exception, if you link this library with other files,
//    some of which are compiled with GCC, to produce an executable,
//    this library does not by itself cause the resulting executable
//    to be covered by the GNU General Public License.
//    This exception does not however invalidate any other reasons why
//    the executable file might be covered by the GNU General Public License.

// libgcc: the compiler's support routines as the game links them
// (__divdi3-_calloc_r, __fixdfdi; the same code in DM2 and BMX XXX):
// libgcc2.c's 64-bit integer arithmetic (the R5900 has no 64-bit multiply
// or divide) and config/fp-bit.c's software floating point, built twice by
// config/mips/t-elf as dp-bit.c (double) and fp-bit.c (float, FLOAT) with
// US_SOFTWARE_GOFAST, which gives the GOFAST names (dpadd, fpadd, dptofp ...).
//
// Version pinned against the candidate sources (docs/READABLE.md, round
// 15): fp-bit.c is GCC 2.95's (the text of 2.95.2 and 2.95.3 is the same):
// pack_d's denormal path shifts without the rounding GCC 3.0 added, the
// whole object has no usi_to_float (3.0 has one), and with one EE change:
// unpack_d reads a zero exponent as zero whatever the fraction (the form
// GCC 3.0 calls NO_DENORMALS; the R5900's FPU has no denormals). So the
// SDK 2.4 toolchain's ee-gcc 2.96, a Cygnus tree between 2.95 and 3.0, whose
// source is not on disk: unpack_d (__unpack_d, __unpack_f) is 2.95's with that
// one change, as the machine code has it.
//
// The rest ported from the 2.95.2 source: fp_number_type and the routines keep fp-bit's
// names and order; the doubles travel as their 64-bit patterns in integer
// registers (dp-bit), the floats in FPU registers (the EE has single
// precision). The compiled code's branches back (32 EE cycles each, as the
// generated code charges them) are charged where it takes them.
//
// __negdf2, the float routines past fptodp and libgcc2.c's __do_global_dtors
// are ported too, as Draft: nothing calls them in any of the three games' PS2
// builds. __moddi3 is reached only through what the runtime replaces.
// libgcc2.c's __main, main's first call, is here too (the game links it apart from
// the rest, after the start-up code's helpers).
// __divdi3 is the runtime's in the game (config.toml's stubs: no recompiled
// function to check it against), so it stays Draft.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/sce/libgcc.cpp.h"

namespace sce::libgcc
{
    namespace
    {
        // fp_class_type
        enum FpClass : int32_t
        {
            kSnan,
            kQnan,
            kZero,
            kNumber,
            kInfinity,
        };

        // fp_number_type for dp-bit (fractype UDItype) and fp-bit (USItype).
        struct FpNumberD
        {
            int32_t fpClass;
            uint32_t sign;
            int32_t normalExp;
            uint32_t _0c;
            uint64_t fraction;
        };
        static_assert(sizeof(FpNumberD) == 0x18 && offsetof(FpNumberD, fraction) == 0x10, "fp_number_type, double");
        struct FpNumberF
        {
            int32_t fpClass;
            uint32_t sign;
            int32_t normalExp;
            uint32_t fraction;
        };
        static_assert(sizeof(FpNumberF) == 0x10, "fp_number_type, float");

        bool isNan(int32_t c) { return static_cast<uint32_t>(c) < kZero; }

        // dp-bit's parameters (52 fraction bits, 8 guard bits).
        namespace d
        {
            constexpr int kFracBits = 52;
            constexpr int kExpBits = 11;
            constexpr int kNGards = 8;
            constexpr int kExpBias = 1023;
            constexpr int kExpMax = 0x7FF;
            constexpr int kFracNBits = 64;
            constexpr int kNormalExpMin = -kExpBias + 1;
            constexpr uint64_t kQuietNan = 0x8000000000000ull;
            constexpr uint64_t kImplicit1 = 1ull << (kFracBits + kNGards);
            constexpr uint64_t kImplicit2 = 1ull << (kFracBits + 1 + kNGards);
            constexpr uint64_t kGardMask = 0xFF, kGardMsb = 0x80, kGardRound = 0x7F;
        }
        // fp-bit's (23 fraction bits, 7 guard bits).
        namespace f
        {
            constexpr int kFracBits = 23;
            constexpr int kExpBits = 8;
            constexpr int kNGards = 7;
            constexpr int kExpBias = 127;
            constexpr int kExpMax = 0xFF;
            constexpr int kFracNBits = 32;
            constexpr int kNormalExpMin = -kExpBias + 1;
            constexpr uint32_t kQuietNan = (1u << 20);
            constexpr uint32_t kImplicit1 = 1u << (kFracBits + kNGards);
            constexpr uint32_t kImplicit2 = 1u << (kFracBits + 1 + kNGards);
            constexpr uint32_t kGardMask = 0x7F, kGardMsb = 0x40, kGardRound = 0x3F;
        }
        // double to float: the bits between the two fractions' guard positions
        constexpr int kFDBitOff = (d::kFracBits + d::kNGards) - (f::kFracBits + f::kNGards);

        constexpr uint32_t kTheNanDf = sym::theNanDf; // __thenan_df, the NaN nan() returns

        constexpr uint32_t kMaxUsiInt = 0xFFFFFFFFu;
    }

    uint64_t __pack_d(ee::Address source);
    void __unpack_d(ee::Address source, ee::Address destination);
    void __unpack_f(ee::Address source, ee::Address destination);
    uint32_t _fpadd_parts(ee::Address a, ee::Address b, ee::Address tmp);
    int32_t __fpcmp_parts_d(ee::Address a, ee::Address b);
    ee::Float __pack_f(ee::Address source);
    ee::Float __make_fp(int32_t fpClass, uint32_t sign, int32_t exp, uint32_t fraction);
    uint64_t __make_dp(int32_t fpClass, uint32_t sign, int32_t exp, uint64_t fraction);

    // ---- libgcc2.c ------------------------------------------------------------------

    // __muldi3: the low 64 bits of a product, from 32-bit halves (umul_ppmm of the
    // low words plus both cross products in the high word).
    uint64_t __muldi3(uint64_t u, uint64_t v)
    {
        return u * v;
    }
    READABLE_FUNCTION(__muldi3, Verified);

    // __udivdi3, __umoddi3: libgcc2.c's __udivmoddi4 (GCC 2.95.2, inlined into each), a long
    // division on 32-bit halves (UWtype) with longlong.h's C fallbacks: __udiv_qrnnd_c (the
    // MIPS port has no udiv_qrnnd, so UDIV_NEEDS_NORMALIZATION is 1), count_leading_zeros,
    // umul_ppmm and sub_ddmmss. Every branch of the source is here so a zero divisor goes
    // where the original goes: the deliberate `d0 = 1 / d0` (break 7 after the divu in the
    // compiled code), and on the R5900 a divu by zero traps nothing and leaves LO = 0xFFFFFFFF
    // (the quotient the recompiled divu gives too), so the division then runs on with
    // d0 = 0xFFFFFFFF: n / 0xFFFFFFFF, not the host's n / 0. (cov4: with a plain n / d, the
    // profiler's cycle tables came out wrong in the A/B from the first dump: something divides
    // by a zero elapsed time.)
    namespace
    {
        using UWtype = uint32_t;
        constexpr int kWTypeSize = 32;
        constexpr UWtype kLlB = UWtype{1} << (kWTypeSize / 2);                   // __ll_B
        constexpr UWtype llLowpart(UWtype t) { return t & (kLlB - 1); }         // __ll_lowpart
        constexpr UWtype llHighpart(UWtype t) { return t >> (kWTypeSize / 2); } // __ll_highpart

        // The R5900's divu: a zero divisor is no trap, LO is 0xFFFFFFFF (the recompiler's divu).
        constexpr UWtype eeDivu(UWtype a, UWtype b) { return b != 0 ? a / b : 0xFFFFFFFFu; }

        // count_leading_zeros (the __clz_tab fallback's value).
        int countLeadingZeros(UWtype x)
        {
            int n = 0;
            for (UWtype bit = UWtype{1} << (kWTypeSize - 1); bit != 0 && (x & bit) == 0; bit >>= 1)
                ++n;
            return n;
        }

        // umul_ppmm: the 64-bit product of two UWtypes in two halves.
        void umulPpmm(UWtype &w1, UWtype &w0, UWtype u, UWtype v)
        {
            const uint64_t p = static_cast<uint64_t>(u) * v;
            w1 = static_cast<UWtype>(p >> kWTypeSize);
            w0 = static_cast<UWtype>(p);
        }

        // sub_ddmmss: (sh, sl) = (ah, al) - (bh, bl) with the borrow.
        void subDdmmss(UWtype &sh, UWtype &sl, UWtype ah, UWtype al, UWtype bh, UWtype bl)
        {
            const UWtype x = al - bl;
            sh = ah - bh - (x > al ? 1u : 0u);
            sl = x;
        }

        // __udiv_qrnnd_c: (q, r) of the two-word n1:n0 by the normalized d (its top bit set).
        void udivQrnnd(UWtype &q, UWtype &r, UWtype n1, UWtype n0, UWtype d)
        {
            const UWtype d1 = llHighpart(d);
            const UWtype d0 = llLowpart(d);

            UWtype r1 = n1 % d1;
            UWtype q1 = n1 / d1;
            UWtype m = q1 * d0;
            r1 = r1 * kLlB | llHighpart(n0);
            if (r1 < m)
            {
                q1--, r1 += d;
                if (r1 >= d) // i.e. no carry when adding to r1
                    if (r1 < m)
                        q1--, r1 += d;
            }
            r1 -= m;

            UWtype r0 = r1 % d1;
            UWtype q0 = r1 / d1;
            m = q0 * d0;
            r0 = r0 * kLlB | llLowpart(n0);
            if (r0 < m)
            {
                q0--, r0 += d;
                if (r0 >= d)
                    if (r0 < m)
                        q0--, r0 += d;
            }
            r0 -= m;

            q = q1 * kLlB | q0;
            r = r0;
        }

        // __udivmoddi4 (libgcc2.c): the quotient of n by d; the remainder into *rp when given.
        uint64_t udivmoddi4(uint64_t n, uint64_t d, uint64_t *rp)
        {
            UWtype d0 = static_cast<UWtype>(d), d1 = static_cast<UWtype>(d >> kWTypeSize);
            UWtype n0 = static_cast<UWtype>(n), n1 = static_cast<UWtype>(n >> kWTypeSize), n2;
            UWtype q0, q1;
            int b, bm;
            UWtype rrLow = 0, rrHigh = 0;

            if (d1 == 0)
            {
                if (d0 > n1)
                {
                    // 0q = nn / 0D
                    bm = countLeadingZeros(d0);
                    if (bm != 0)
                    {
                        // Normalize, i.e. make the most significant bit of the denominator set.
                        d0 = d0 << bm;
                        n1 = (n1 << bm) | (n0 >> (kWTypeSize - bm));
                        n0 = n0 << bm;
                    }
                    udivQrnnd(q0, n0, n1, n0, d0);
                    q1 = 0;
                    // Remainder in n0 >> bm.
                }
                else
                {
                    // qq = NN / 0d
                    if (d0 == 0)
                        d0 = eeDivu(1, d0); // Divide intentionally by zero.
                    bm = countLeadingZeros(d0);
                    if (bm == 0)
                    {
                        // From (n1 >= d0) /\ (the most significant bit of d0 is set), conclude (the most
                        // significant bit of n1 is set) /\ (the leading quotient digit q1 = 1). This special
                        // case is necessary, not an optimization. (Shifts counts of W_TYPE_SIZE are undefined.)
                        n1 -= d0;
                        q1 = 1;
                    }
                    else
                    {
                        // Normalize.
                        b = kWTypeSize - bm;
                        d0 = d0 << bm;
                        n2 = n1 >> b;
                        n1 = (n1 << bm) | (n0 >> b);
                        n0 = n0 << bm;
                        udivQrnnd(q1, n1, n2, n1, d0);
                    }
                    // n1 != d0...
                    udivQrnnd(q0, n0, n1, n0, d0);
                    // Remainder in n0 >> bm.
                }
                if (rp != nullptr)
                {
                    rrLow = n0 >> bm;
                    rrHigh = 0;
                }
            }
            else
            {
                if (d1 > n1)
                {
                    // 00 = nn / DD
                    q0 = 0;
                    q1 = 0;
                    // Remainder in n1n0.
                    if (rp != nullptr)
                    {
                        rrLow = n0;
                        rrHigh = n1;
                    }
                }
                else
                {
                    // 0q = NN / dd
                    bm = countLeadingZeros(d1);
                    if (bm == 0)
                    {
                        // From (n1 >= d1) /\ (the most significant bit of d1 is set), conclude (the most
                        // significant bit of n1 is set) /\ (the quotient digit q0 = 0 or 1). This special
                        // case is necessary, not an optimization. The condition on the next line takes
                        // advantage of that n1 >= d1 (true due to program flow).
                        if (n1 > d1 || n0 >= d0)
                        {
                            q0 = 1;
                            subDdmmss(n1, n0, n1, n0, d1, d0);
                        }
                        else
                            q0 = 0;
                        q1 = 0;
                        if (rp != nullptr)
                        {
                            rrLow = n0;
                            rrHigh = n1;
                        }
                    }
                    else
                    {
                        UWtype m1, m0;
                        // Normalize.
                        b = kWTypeSize - bm;
                        d1 = (d1 << bm) | (d0 >> b);
                        d0 = d0 << bm;
                        n2 = n1 >> b;
                        n1 = (n1 << bm) | (n0 >> b);
                        n0 = n0 << bm;
                        udivQrnnd(q0, n1, n2, n1, d1);
                        umulPpmm(m1, m0, q0, d0);
                        if (m1 > n1 || (m1 == n1 && m0 > n0))
                        {
                            q0--;
                            subDdmmss(m1, m0, m1, m0, d1, d0);
                        }
                        q1 = 0;
                        // Remainder in (n1n0 - m1m0) >> bm.
                        if (rp != nullptr)
                        {
                            subDdmmss(n1, n0, n1, n0, m1, m0);
                            rrLow = (n1 << b) | (n0 >> bm);
                            rrHigh = n1 >> bm;
                        }
                    }
                }
            }
            if (rp != nullptr)
                *rp = (static_cast<uint64_t>(rrHigh) << kWTypeSize) | rrLow;
            return (static_cast<uint64_t>(q1) << kWTypeSize) | q0;
        }
    }

    uint64_t __udivdi3(uint64_t n, uint64_t dv)
    {
        return udivmoddi4(n, dv, nullptr);
    }
    READABLE_FUNCTION(__udivdi3, Verified);

    // __divdi3: libgcc2.c: the magnitudes through __udivmoddi4 (inlined here too), the
    // sign of the quotient from the operands'.
    int64_t __divdi3(int64_t u, int64_t v)
    {
        const bool negative = (u < 0) != (v < 0);
        const uint64_t uu = u < 0 ? 0ull - static_cast<uint64_t>(u) : static_cast<uint64_t>(u);
        const uint64_t vv = v < 0 ? 0ull - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        const uint64_t w = udivmoddi4(uu, vv, nullptr);
        return static_cast<int64_t>(negative ? 0ull - w : w);
    }

    // __umoddi3: libgcc2.c: __udivmoddi4's remainder.
    uint64_t __umoddi3(uint64_t n, uint64_t dv)
    {
        uint64_t w;
        (void)udivmoddi4(n, dv, &w);
        return w;
    }
    READABLE_FUNCTION(__umoddi3, Verified);

    // ---- dp-bit.c --------------------------------------------------------------------

    // pack_d: an fp_number_type to the double's bits, rounding the guard bits to even.
    uint64_t __pack_d(ee::Address source)
    {
        const FpNumberD *const src = ee::Ptr<FpNumberD>(source).get();
        uint64_t fraction = src->fraction;
        const uint32_t sign = src->sign;
        int exp = 0;

        if (isNan(src->fpClass))
        {
            exp = d::kExpMax;
            fraction |= d::kQuietNan;
        }
        else if (src->fpClass == kInfinity)
        {
            exp = d::kExpMax;
            fraction = 0;
        }
        else if (src->fpClass == kZero)
            fraction = 0;
        else if (fraction == 0)
            exp = 0;
        else if (src->normalExp < d::kNormalExpMin)
        {
            // Too small for the exponent: a denormal, the fraction shifted right.
            const int shift = d::kNormalExpMin - src->normalExp;
            exp = 0;
            fraction = shift > d::kFracNBits - d::kNGards ? 0 : fraction >> shift;
            fraction >>= d::kNGards;
        }
        else if (src->normalExp > d::kExpBias)
        {
            exp = d::kExpMax;
            fraction = 0;
        }
        else
        {
            exp = src->normalExp + d::kExpBias;
            // Half way between two numbers: choose the one with an even lsb.
            if ((fraction & d::kGardMask) == d::kGardMsb)
            {
                if (fraction & (1u << d::kNGards))
                    fraction += d::kGardRound + 1;
            }
            else
                fraction += d::kGardRound; // round up
            if (fraction >= d::kImplicit2)
            {
                fraction >>= 1;
                exp += 1;
            }
            fraction >>= d::kNGards;
        }
        // FLOAT_BIT_ORDER_MISMATCH (little-endian MIPS): the union's bit fields
        return (fraction & ((1ull << d::kFracBits) - 1)) | (static_cast<uint64_t>(exp & d::kExpMax) << d::kFracBits) |
               (static_cast<uint64_t>(sign & 1u) << 63);
    }
    READABLE_FUNCTION(__pack_d, Verified);

    // unpack_d: a double's bits to an fp_number_type: its sign, and its class, with the
    // exponent unbiased and the fraction moved up past the guard bits under the implicit 1
    // for a number, the fraction kept for a NaN. GCC 2.95's text but for one EE change: a
    // zero exponent is zero whatever the fraction (the R5900 has no denormals), where 2.95
    // shifts a denormal's fraction up to its implicit 1.
    void __unpack_d(ee::Address source, ee::Address destination)
    {
        const uint64_t valueRaw = *ee::Ptr<const uint64_t>(source);
        FpNumberD *const dst = ee::Ptr<FpNumberD>(destination).get();
        const uint64_t fraction = valueRaw & ((1ull << d::kFracBits) - 1ull);
        const int exp = static_cast<int>(valueRaw >> d::kFracBits) & ((1 << d::kExpBits) - 1);
        const int sign = static_cast<int>(valueRaw >> (d::kFracBits + d::kExpBits)) & 1;

        dst->sign = static_cast<uint32_t>(sign);
        if (exp == 0)
        {
            // Looks like 0: zero, denormal or not.
            dst->fpClass = kZero;
        }
        else if (exp == d::kExpMax)
        {
            // Huge exponent: infinity with a zero fraction, else a NaN, the fraction kept.
            if (fraction == 0)
                dst->fpClass = kInfinity;
            else
            {
                dst->fpClass = (fraction & d::kQuietNan) != 0 ? kQnan : kSnan;
                dst->fraction = fraction;
            }
        }
        else
        {
            // Nothing strange about this number.
            dst->normalExp = exp - d::kExpBias;
            dst->fpClass = kNumber;
            dst->fraction = (fraction << d::kNGards) | d::kImplicit1;
        }
    }
    READABLE_FUNCTION(__unpack_d, Verified);

    // _fpadd_parts: a + b into tmp (or a, b or the NaN for the special cases); returns the
    // result's address.
    uint32_t _fpadd_parts(ee::Address aAddress, ee::Address bAddress, ee::Address tmpAddress)
    {
        const FpNumberD *const a = ee::Ptr<FpNumberD>(aAddress).get();
        const FpNumberD *const b = ee::Ptr<FpNumberD>(bAddress).get();
        FpNumberD *const tmp = ee::Ptr<FpNumberD>(tmpAddress).get();

        // The compiled code returns a and b from blocks near the top; the returns below it
        // reach them by branching back.
        const auto returnABack = [&] {   return aAddress; };
        const auto returnBBack = [&] {   return bAddress; };

        if (isNan(a->fpClass))
            return aAddress;
        if (isNan(b->fpClass))
            return bAddress;
        if (a->fpClass == kInfinity)
        {
            // Adding infinities with opposite signs yields a NaN.
            if (b->fpClass == kInfinity && a->sign != b->sign)
                return kTheNanDf;
            return returnABack();
        }
        if (b->fpClass == kInfinity)
            return returnBBack();
        if (b->fpClass == kZero)
        {
            if (a->fpClass == kZero)
            {
                *tmp = *a;
                tmp->sign = a->sign & b->sign;
                return tmpAddress;
            }
            return returnABack();
        }
        if (a->fpClass == kZero)
            return returnBBack();

        // Two numbers: shift the smaller and increment its exponent till they're the same.
        int aExp = a->normalExp;
        int bExp = b->normalExp;
        uint64_t aFraction = a->fraction;
        uint64_t bFraction = b->fraction;
        const auto lshift = [](uint64_t x) { return (x & 1) | (x >> 1); }; // LSHIFT: keeps a sticky bit
        int diff = aExp - bExp;
        if (diff < 0)
            diff = -diff;
        if (diff < d::kFracNBits)
        {
            // One bit at a time (the source's own "Optimize." note); only one of the two loops runs.
            if (aExp > bExp)
            {
                for (;;)
                {
                    ++bExp;
                    bFraction = lshift(bFraction);
                    if (!(aExp > bExp))
                        break;
                }
            }
            if (bExp > aExp)
            {
                for (;;)
                {
                    ++aExp;
                    aFraction = lshift(aFraction);
                    if (!(bExp > aExp))
                        break;
                }
            }
        }
        else if (aExp > bExp) // something's up: choose the biggest
        {
            bExp = aExp;
            bFraction = 0;
        }
        else
        {
            aExp = bExp;
            aFraction = 0;
        }

        if (a->sign != b->sign)
        {
            const int64_t tfraction = a->sign ? static_cast<int64_t>(bFraction - aFraction) : static_cast<int64_t>(aFraction - bFraction);
            tmp->normalExp = aExp;
            if (tfraction >= 0)
            {
                tmp->fraction = static_cast<uint64_t>(tfraction);
                tmp->sign = 0;
            }
            else
            {
                tmp->fraction = static_cast<uint64_t>(-tfraction);
                tmp->sign = 1;
            }
            // and renormalise it
            if (tmp->fraction < d::kImplicit1 && tmp->fraction)
            {
                for (;;)
                {
                    tmp->fraction <<= 1;
                    tmp->normalExp--;
                    if (!(tmp->fraction < d::kImplicit1 && tmp->fraction))
                        break;
                }
            }
        }
        else
        {
            tmp->sign = a->sign;
            tmp->normalExp = aExp;
            tmp->fraction = aFraction + bFraction;
        }
        tmp->fpClass = kNumber;
        // The fraction is added: shift down to renormalise the number.
        if (tmp->fraction >= d::kImplicit2)
        {
            tmp->fraction = lshift(tmp->fraction);
            tmp->normalExp++;
        }
        return tmpAddress;
    }
    // Draft, not Tested: 2 of the 2,861 recorded calls differ, and the recording is what is
    // wrong. The compiled code tests tfraction's sign with bltz, which on the R5900 reads
    // all 64 bits; the recompiler's bltz (and blez, bgtz, bgez: ps2xRecomp's
    // control_flow_emitter.cpp) reads only the low 32, so a negative difference whose low
    // word is zero (-1.59 + 1.5 at the same exponent, mash-6 vsync 3881) keeps sign 0 and
    // the unnegated fraction there. This follows the source and the hardware.
    READABLE_FUNCTION(_fpadd_parts, Verified);

    // dpadd (add): a + b.
    uint64_t dpadd(uint64_t argA, uint64_t argB)
    {
        ee::Local<uint8_t[0x20]> destination;
        ee::Local<uint8_t[0x20]> destination2;
        ee::Local<uint8_t[0x20]> tmpAddress;
        ee::Local<uint64_t> source;
        ee::Local<uint64_t> source2;
        *source = argA;
        *source2 = argB;
        __unpack_d(source, destination);
        __unpack_d(source2, destination2);
        const uint32_t result = _fpadd_parts(destination, destination2, tmpAddress);
        return __pack_d(result);
    }
    READABLE_FUNCTION(dpadd, Verified);

    // dpsub (sub): a + -b.
    uint64_t dpsub(uint64_t argA, uint64_t argB)
    {
        ee::Local<uint8_t[0x20]> destination;
        ee::Local<FpNumberD> destination2;
        ee::Local<uint8_t[0x20]> tmpAddress;
        ee::Local<uint64_t> source;
        ee::Local<uint64_t> source2;
        *source = argA;
        *source2 = argB;
        __unpack_d(source, destination);
        __unpack_d(source2, destination2);
        destination2->sign ^= 1;
        const uint32_t result = _fpadd_parts(destination, destination2, tmpAddress);
        return __pack_d(result);
    }
    READABLE_FUNCTION(dpsub, Verified);

    // __fpcmp_parts_d: -1, 0 or 1 as a < b, a == b, a > b; 1 if either is a NaN (unordered).
    int32_t __fpcmp_parts_d(ee::Address aAddress, ee::Address bAddress)
    {
        const FpNumberD *const a = ee::Ptr<FpNumberD>(aAddress).get();
        const FpNumberD *const b = ee::Ptr<FpNumberD>(bAddress).get();
        // The original's entry blocks (ee/abi.h, safe points) are marked
        // where it enters them; the returns marked "back" reach a shared
        // return above them by a branch back.
        const auto back = [](int32_t r)
        {
            return r;
        };
        const auto at = [](int32_t r)
        {
            return r;
        };

        if (isNan(a->fpClass) || isNan(b->fpClass))
            return at(1); // how to indicate unordered compare?
        if (a->fpClass == kInfinity && b->fpClass == kInfinity)
            return static_cast<int32_t>(b->sign - a->sign); // +inf > -inf, but +inf == +inf
        if (a->fpClass == kInfinity)
            return at(a->sign ? -1 : 1);
        if (b->fpClass == kInfinity)
            return b->sign ? 1 : -1;
        if (a->fpClass == kZero && b->fpClass == kZero)
            return 0;
        if (a->fpClass == kZero)
            return at(b->sign ? 1 : -1);
        if (b->fpClass == kZero)
            return back(a->sign ? -1 : 1);
        // now both are normal
        if (a->sign != b->sign) // opposite signs
            return at(a->sign ? -1 : 1);
        // same sign: exponents?
        if (a->normalExp > b->normalExp)
            return back(a->sign ? -1 : 1);
        if (a->normalExp < b->normalExp)
            return at(a->sign ? 1 : -1);
        // same exponents: check size
        if (a->fraction > b->fraction)
            return back(a->sign ? -1 : 1);
        if (a->fraction < b->fraction)
            return back(a->sign ? 1 : -1);
        return 0; // after all that, they're equal
    }
    READABLE_FUNCTION(__fpcmp_parts_d, Verified);

    // dpcmp (compare): __fpcmp_parts of the unpacked arguments.
    int32_t dpcmp(uint64_t argA, uint64_t argB)
    {
        ee::Local<uint8_t[0x20]> destination;
        ee::Local<uint8_t[0x20]> destination2;
        ee::Local<uint64_t> source;
        ee::Local<uint64_t> source2;
        *source = argA;
        *source2 = argB;
        __unpack_d(source, destination);
        __unpack_d(source2, destination2);
        return __fpcmp_parts_d(destination, destination2);
    }
    READABLE_FUNCTION(dpcmp, Verified);

    // __make_dp: pack_d of the parts.
    uint64_t __make_dp(int32_t fpClass, uint32_t sign, int32_t exp, uint64_t fraction)
    {
        ee::Local<FpNumberD> in;
        in->fpClass = fpClass;
        in->sign = sign;
        in->normalExp = exp;
        in->fraction = fraction;
        return __pack_d(in);
    }
    READABLE_FUNCTION(__make_dp, Verified);

    // dptofp (df_to_sf): a double to a float; the lowest guard bit set when nonzero bits
    // are dropped.
    ee::Float dptofp(uint64_t arg)
    {
        ee::Local<FpNumberD> in;
        ee::Local<uint64_t> source;
        *source = arg;
        __unpack_d(source, in);
        uint32_t sffrac = static_cast<uint32_t>(in->fraction >> kFDBitOff);
        if ((in->fraction & ((1u << kFDBitOff) - 1)) != 0)
            sffrac |= 1;
        return __make_fp(in->fpClass, in->sign, in->normalExp, sffrac);
    }
    READABLE_FUNCTION(dptofp, Verified);

    // __negdf2 (negate): -a, by flipping the sign of the unpacked number (flip_sign).
    uint64_t __negdf2(uint64_t arg)
    {
        ee::Local<FpNumberD> a;
        ee::Local<uint64_t> source;
        *source = arg;
        __unpack_d(source, a);
        a->sign = !a->sign;
        return __pack_d(a);
    }
    READABLE_FUNCTION(__negdf2, Draft);

    // ---- fp-bit.c (FLOAT) ------------------------------------------------------------

    // pack_d for floats: the result in an FPU register.
    ee::Float __pack_f(ee::Address source)
    {
        const FpNumberF *const src = ee::Ptr<FpNumberF>(source).get();
        uint32_t fraction = src->fraction;
        const uint32_t sign = src->sign;
        int exp = 0;

        if (isNan(src->fpClass))
        {
            exp = f::kExpMax;
            fraction |= f::kQuietNan;
        }
        else if (src->fpClass == kInfinity)
        {
            exp = f::kExpMax;
            fraction = 0;
        }
        else if (src->fpClass == kZero)
            fraction = 0;
        else if (fraction == 0)
            exp = 0;
        else if (src->normalExp < f::kNormalExpMin)
        {
            const int shift = f::kNormalExpMin - src->normalExp;
            exp = 0;
            fraction = shift > f::kFracNBits - f::kNGards ? 0 : fraction >> shift;
            fraction >>= f::kNGards;
        }
        else if (src->normalExp > f::kExpBias)
        {
            exp = f::kExpMax;
            fraction = 0;
        }
        else
        {
            exp = src->normalExp + f::kExpBias;
            if ((fraction & f::kGardMask) == f::kGardMsb)
            {
                if (fraction & (1u << f::kNGards))
                    fraction += f::kGardRound + 1;
            }
            else
                fraction += f::kGardRound;
            if (fraction >= f::kImplicit2)
            {
                fraction >>= 1;
                exp += 1;
            }
            fraction >>= f::kNGards;
        }
        return ee::Float::fromBits((fraction & ((1u << f::kFracBits) - 1)) | (static_cast<uint32_t>(exp & f::kExpMax) << f::kFracBits) |
                                   ((sign & 1u) << 31));
    }
    READABLE_FUNCTION(__pack_f, Verified);

    // unpack_d for floats (__unpack_f): as __unpack_d, on the float's 32 bits.
    void __unpack_f(ee::Address source, ee::Address destination)
    {
        const uint32_t valueRaw = *ee::Ptr<const uint32_t>(source);
        FpNumberF *const dst = ee::Ptr<FpNumberF>(destination).get();
        const uint32_t fraction = valueRaw & ((1u << f::kFracBits) - 1u);
        const int exp = static_cast<int>(valueRaw >> f::kFracBits) & ((1 << f::kExpBits) - 1);
        const int sign = static_cast<int>(valueRaw >> (f::kFracBits + f::kExpBits)) & 1;

        dst->sign = static_cast<uint32_t>(sign);
        if (exp == 0)
            dst->fpClass = kZero;
        else if (exp == f::kExpMax)
        {
            if (fraction == 0)
                dst->fpClass = kInfinity;
            else
            {
                dst->fpClass = (fraction & f::kQuietNan) != 0 ? kQnan : kSnan;
                dst->fraction = fraction;
            }
        }
        else
        {
            dst->normalExp = exp - f::kExpBias;
            dst->fpClass = kNumber;
            dst->fraction = (fraction << f::kNGards) | f::kImplicit1;
        }
    }
    READABLE_FUNCTION(__unpack_f, Verified);

    // fptoui (float_to_usi): a float to an unsigned int, saturating.
    uint32_t fptoui(ee::Float arg)
    {
        ee::Local<FpNumberF> a;
        ee::Local<ee::Float> source;
        *source = arg;
        __unpack_f(source, a);
        // The original's entry blocks (ee/abi.h, safe points): return 0
        //, the sign test, MAX_USI_INT, the
        // shifts, the return.
        if (a->fpClass == kZero || isNan(a->fpClass))
            return 0;
        if (a->sign) // a negative number
            return 0;
        if (a->fpClass == kInfinity) // get reasonable MAX_USI_INT...
            return kMaxUsiInt;
        if (a->normalExp < 0) // a number, but a small one (the compiled code branches back to a return 0)
            return 0;
        if (a->normalExp > 31)
            return kMaxUsiInt;
        if (a->normalExp > f::kFracBits + f::kNGards)
            return a->fraction << (a->normalExp - (f::kFracBits + f::kNGards));
        return a->fraction >> ((f::kFracBits + f::kNGards) - a->normalExp);
    }
    READABLE_FUNCTION(fptoui, Verified);

    // __make_fp: pack_d of the parts.
    ee::Float __make_fp(int32_t fpClass, uint32_t sign, int32_t exp, uint32_t fraction)
    {
        ee::Local<FpNumberF> in;
        in->fpClass = fpClass;
        in->sign = sign;
        in->normalExp = exp;
        in->fraction = fraction;
        return __pack_f(in);
    }
    READABLE_FUNCTION(__make_fp, Verified);

    // fptodp (sf_to_df): a float to a double.
    uint64_t fptodp(ee::Float arg)
    {
        ee::Local<FpNumberF> in;
        ee::Local<ee::Float> source;
        *source = arg;
        __unpack_f(source, in);
        return __make_dp(in->fpClass, in->sign, in->normalExp, static_cast<uint64_t>(in->fraction) << kFDBitOff);
    }
    READABLE_FUNCTION(fptodp, Verified);

    // The rest of fp-bit.c for floats. Nothing calls these in any of the three games' PS2
    // builds (the game's floats are the FPU's), so they stay Draft.
    namespace
    {
        using FloatParts = ee::Ptr<FpNumberF>;
        bool isInfinity(FloatParts p) { return p->fpClass == kInfinity; }
        bool isZero(FloatParts p) { return p->fpClass == kZero; }
        FloatParts floatNotANumber() { return FloatParts(sym::g_theNanSf); } // nan (): __thenan_sf
        uint32_t lshift(uint32_t x) { return (x & 1u) | (x >> 1); }       // LSHIFT: keeps a sticky bit

        // The guard bits rounded: on the exact halfway value, up from an odd number or when
        // anything is left below.
        uint32_t roundedToEven(uint32_t value, bool anythingBelow)
        {
            if ((value & f::kGardMask) == f::kGardMsb && ((value & (1u << f::kNGards)) != 0u || anythingBelow))
                value += f::kGardRound + 1u;
            return value;
        }

        constexpr int32_t kMaxSiIntF = 0x7FFFFFFF;
        constexpr int32_t kMinSiIntF = -kMaxSiIntF - 1;
    }

    // _fpadd_parts: a + b into tmp (or a, b or the NaN for the special cases).
    FloatParts _fpadd_parts_sf(FloatParts a, FloatParts b, FloatParts tmp)
    {
        if (isNan(a->fpClass))
            return a;
        if (isNan(b->fpClass))
            return b;
        if (isInfinity(a))
        {
            // Adding infinities with opposite signs yields a NaN.
            if (isInfinity(b) && a->sign != b->sign)
                return floatNotANumber();
            return a;
        }
        if (isInfinity(b))
            return b;
        if (isZero(b))
        {
            if (isZero(a))
            {
                *tmp = *a;
                tmp->sign = a->sign & b->sign;
                return tmp;
            }
            return a;
        }
        if (isZero(a))
            return b;

        // Two numbers: shift the smaller and increment its exponent till they're the same.
        int aExp = a->normalExp;
        int bExp = b->normalExp;
        uint32_t aFraction = a->fraction;
        uint32_t bFraction = b->fraction;
        int diff = aExp - bExp;
        if (diff < 0)
            diff = -diff;
        if (diff < f::kFracNBits)
        {
            // One bit at a time (the source's own "Optimize." note).
            while (aExp > bExp)
            {
                ++bExp;
                bFraction = lshift(bFraction);
            }
            while (bExp > aExp)
            {
                ++aExp;
                aFraction = lshift(aFraction);
            }
        }
        else if (aExp > bExp) // something's up: choose the biggest
        {
            bExp = aExp;
            bFraction = 0;
        }
        else
        {
            aExp = bExp;
            aFraction = 0;
        }

        if (a->sign != b->sign)
        {
            const int32_t tfraction = a->sign ? static_cast<int32_t>(bFraction - aFraction) : static_cast<int32_t>(aFraction - bFraction);
            tmp->sign = tfraction < 0 ? 1u : 0u;
            tmp->normalExp = aExp;
            tmp->fraction = tfraction < 0 ? 0u - static_cast<uint32_t>(tfraction) : static_cast<uint32_t>(tfraction);
            // and renormalise it
            while (tmp->fraction < f::kImplicit1 && tmp->fraction != 0u)
            {
                tmp->fraction <<= 1;
                tmp->normalExp--;
            }
        }
        else
        {
            tmp->sign = a->sign;
            tmp->normalExp = aExp;
            tmp->fraction = aFraction + bFraction;
        }
        tmp->fpClass = kNumber;
        // The fraction is added: shift down to renormalise the number.
        if (tmp->fraction >= f::kImplicit2)
        {
            tmp->fraction = lshift(tmp->fraction);
            tmp->normalExp++;
        }
        return tmp;
    }
    READABLE_FUNCTION(_fpadd_parts_sf, Draft);

    // fpadd (add): a + b.
    ee::Float fpadd(ee::Float argA, ee::Float argB)
    {
        ee::Local<FpNumberF> a;
        ee::Local<FpNumberF> b;
        ee::Local<FpNumberF> tmp;
        ee::Local<ee::Float> sourceA;
        ee::Local<ee::Float> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_f(sourceA, a);
        __unpack_f(sourceB, b);
        return __pack_f(_fpadd_parts_sf(a, b, tmp));
    }
    READABLE_FUNCTION(fpadd, Draft);

    // fpsub (sub): a + -b.
    ee::Float fpsub(ee::Float argA, ee::Float argB)
    {
        ee::Local<FpNumberF> a;
        ee::Local<FpNumberF> b;
        ee::Local<FpNumberF> tmp;
        ee::Local<ee::Float> sourceA;
        ee::Local<ee::Float> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_f(sourceA, a);
        __unpack_f(sourceB, b);
        b->sign ^= 1u;
        return __pack_f(_fpadd_parts_sf(a, b, tmp));
    }
    READABLE_FUNCTION(fpsub, Draft);

    namespace
    {
        // _fpmul_parts, inlined in fpmul: the two 32-bit fractions multiplied to 64 bits
        // (the source's FLOAT case), the top half renormalised and rounded.
        FloatParts multiplyParts(FloatParts a, FloatParts b, FloatParts tmp)
        {
            if (isNan(a->fpClass))
            {
                a->sign = a->sign != b->sign;
                return a;
            }
            if (isNan(b->fpClass))
            {
                b->sign = a->sign != b->sign;
                return b;
            }
            if (isInfinity(a))
            {
                if (isZero(b))
                    return floatNotANumber();
                a->sign = a->sign != b->sign;
                return a;
            }
            if (isInfinity(b))
            {
                if (isZero(a))
                    return floatNotANumber();
                b->sign = a->sign != b->sign;
                return b;
            }
            if (isZero(a))
            {
                a->sign = a->sign != b->sign;
                return a;
            }
            if (isZero(b))
            {
                b->sign = a->sign != b->sign;
                return b;
            }

            const uint64_t answer = static_cast<uint64_t>(a->fraction) * b->fraction;
            uint32_t high = static_cast<uint32_t>(answer >> 32);
            uint32_t low = static_cast<uint32_t>(answer);

            tmp->normalExp = a->normalExp + b->normalExp;
            tmp->sign = a->sign != b->sign;
            tmp->normalExp += 2; // the source's own "???????"
            constexpr uint32_t kFracHigh = 0x80000000u;
            while (high >= f::kImplicit2)
            {
                tmp->normalExp++;
                if ((high & 1u) != 0u)
                {
                    low >>= 1;
                    low |= kFracHigh;
                }
                high >>= 1;
            }
            while (high < f::kImplicit1)
            {
                tmp->normalExp--;
                high <<= 1;
                if ((low & kFracHigh) != 0u)
                    high |= 1u;
                low <<= 1;
            }
            tmp->fraction = roundedToEven(high, low != 0u);
            tmp->fpClass = kNumber;
            return tmp;
        }

        // _fpdiv_parts, inlined in fpdiv: long division, a bit at a time.
        FloatParts divideParts(FloatParts a, FloatParts b)
        {
            if (isNan(a->fpClass))
                return a;
            if (isNan(b->fpClass))
                return b;
            a->sign ^= b->sign;
            if (isInfinity(a) || isZero(a))
            {
                if (a->fpClass == b->fpClass)
                    return floatNotANumber();
                return a;
            }
            if (isInfinity(b))
            {
                a->fraction = 0u;
                a->normalExp = 0;
                return a;
            }
            if (isZero(b))
            {
                a->fpClass = kInfinity;
                return a;
            }
            a->normalExp -= b->normalExp;
            uint32_t numerator = a->fraction;
            const uint32_t denominator = b->fraction;
            if (numerator < denominator)
            {
                // The fraction will be less than 1.0.
                numerator *= 2u;
                --a->normalExp;
            }
            uint32_t quotient = 0u;
            for (uint32_t bit = f::kImplicit1; bit != 0u; bit >>= 1)
            {
                if (numerator >= denominator)
                {
                    quotient |= bit;
                    numerator -= denominator;
                }
                numerator *= 2u;
            }
            a->fraction = roundedToEven(quotient, numerator != 0u);
            return a;
        }
    }

    // fpmul (multiply): a * b.
    ee::Float fpmul(ee::Float argA, ee::Float argB)
    {
        ee::Local<FpNumberF> a;
        ee::Local<FpNumberF> b;
        ee::Local<FpNumberF> tmp;
        ee::Local<ee::Float> sourceA;
        ee::Local<ee::Float> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_f(sourceA, a);
        __unpack_f(sourceB, b);
        return __pack_f(multiplyParts(a, b, tmp));
    }
    READABLE_FUNCTION(fpmul, Draft);

    // fpdiv (divide): a / b.
    ee::Float fpdiv(ee::Float argA, ee::Float argB)
    {
        ee::Local<FpNumberF> a;
        ee::Local<FpNumberF> b;
        ee::Local<ee::Float> sourceA;
        ee::Local<ee::Float> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_f(sourceA, a);
        __unpack_f(sourceB, b);
        return __pack_f(divideParts(a, b));
    }
    READABLE_FUNCTION(fpdiv, Draft);

    // __fpcmp_parts: -1, 0 or 1 as a < b, a == b, a > b; 1 if either is a NaN (unordered).
    int32_t __fpcmp_parts_f(FloatParts a, FloatParts b)
    {
        if (isNan(a->fpClass) || isNan(b->fpClass))
            return 1; // how to indicate unordered compare?
        if (isInfinity(a) && isInfinity(b))
            return static_cast<int32_t>(b->sign - a->sign); // +inf > -inf, but +inf == +inf
        if (isInfinity(a))
            return a->sign ? -1 : 1;
        if (isInfinity(b))
            return b->sign ? 1 : -1;
        if (isZero(a) && isZero(b))
            return 0;
        if (isZero(a))
            return b->sign ? 1 : -1;
        if (isZero(b))
            return a->sign ? -1 : 1;
        // now both are normal
        if (a->sign != b->sign) // opposite signs
            return a->sign ? -1 : 1;
        // same sign: exponents?
        if (a->normalExp > b->normalExp)
            return a->sign ? -1 : 1;
        if (a->normalExp < b->normalExp)
            return a->sign ? 1 : -1;
        // same exponents: check size
        if (a->fraction > b->fraction)
            return a->sign ? -1 : 1;
        if (a->fraction < b->fraction)
            return a->sign ? 1 : -1;
        return 0; // after all that, they're equal
    }
    READABLE_FUNCTION(__fpcmp_parts_f, Draft);

    // fpcmp (compare): __fpcmp_parts of the unpacked arguments.
    int32_t fpcmp(ee::Float argA, ee::Float argB)
    {
        ee::Local<FpNumberF> a;
        ee::Local<FpNumberF> b;
        ee::Local<ee::Float> sourceA;
        ee::Local<ee::Float> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_f(sourceA, a);
        __unpack_f(sourceB, b);
        return __fpcmp_parts_f(a, b);
    }
    READABLE_FUNCTION(fpcmp, Draft);

    // sitofp (si_to_float): an int as a float.
    ee::Float sitofp(int32_t value)
    {
        ee::Local<FpNumberF> in;
        in->fpClass = kNumber;
        in->sign = value < 0 ? 1u : 0u;
        if (value == 0)
            in->fpClass = kZero;
        else
        {
            in->normalExp = f::kFracBits + f::kNGards;
            if (in->sign)
            {
                // The smallest int has no positive int to negate to.
                if (value == kMinSiIntF)
                    return ee::Float(-2147483648.0f);
                in->fraction = 0u - static_cast<uint32_t>(value);
            }
            else
                in->fraction = static_cast<uint32_t>(value);
            while (in->fraction < (1u << (f::kFracBits + f::kNGards)))
            {
                in->fraction <<= 1;
                in->normalExp -= 1;
            }
        }
        return __pack_f(in);
    }
    READABLE_FUNCTION(sitofp, Draft);

    // fptosi (float_to_si): a float to an int, saturating; the fraction cut towards zero.
    int32_t fptosi(ee::Float arg)
    {
        ee::Local<FpNumberF> a;
        ee::Local<ee::Float> source;
        *source = arg;
        __unpack_f(source, a);
        if (isZero(a))
            return 0;
        if (isNan(a->fpClass))
            return 0;
        if (isInfinity(a)) // get reasonable MAX_SI_INT...
            return a->sign ? kMinSiIntF : kMaxSiIntF;
        if (a->normalExp < 0) // a number, but a small one
            return 0;
        if (a->normalExp > 30)
            return a->sign ? kMinSiIntF : kMaxSiIntF;
        const int32_t magnitude = static_cast<int32_t>(a->fraction >> ((f::kFracBits + f::kNGards) - a->normalExp));
        return a->sign ? -magnitude : magnitude;
    }
    READABLE_FUNCTION(fptosi, Draft);

    // __negsf2 (negate): -a, by flipping the sign of the unpacked number (flip_sign).
    ee::Float __negsf2(ee::Float arg)
    {
        ee::Local<FpNumberF> a;
        ee::Local<ee::Float> source;
        *source = arg;
        __unpack_f(source, a);
        a->sign = !a->sign;
        return __pack_f(a);
    }
    READABLE_FUNCTION(__negsf2, Draft);

    namespace
    {
        using Parts = ee::Ptr<FpNumberD>;
        bool isInfinity(Parts p) { return p->fpClass == kInfinity; }
        bool isZero(Parts p) { return p->fpClass == kZero; }
        Parts notANumber() { return Parts(kTheNanDf); }

        // Rounds a result to its guard bits: on the exact halfway value, up from an odd number
        // or when anything is left below.
        uint64_t roundedToEven(uint64_t value, bool anythingBelow)
        {
            if ((value & d::kGardMask) == d::kGardMsb && ((value & (1ull << d::kNGards)) != 0u || anythingBelow))
                value += d::kGardRound + 1u;
            return value;
        }

        // _fpmul_parts, inlined in dpmul. Its 64 by 64 bit multiply is the source's long form.
        Parts multiplyParts(Parts a, Parts b, Parts tmp)
        {
            const uint32_t differ = a->sign != b->sign;
            if (isNan(a->fpClass))
            {
                a->sign = differ;
                return a;
            }
            if (isNan(b->fpClass))
            {
                b->sign = differ;
                return b;
            }
            if (isInfinity(a))
            {
                if (isZero(b))
                    return notANumber();
                a->sign = differ;
                return a;
            }
            if (isInfinity(b))
            {
                if (isZero(a))
                    return notANumber();
                b->sign = differ;
                return b;
            }
            if (isZero(a))
            {
                a->sign = differ;
                return a;
            }
            if (isZero(b))
            {
                b->sign = differ;
                return b;
            }
            const uint64_t nl = a->fraction & 0xFFFFFFFFu;
            const uint64_t nh = a->fraction >> 32;
            const uint64_t ml = b->fraction & 0xFFFFFFFFu;
            const uint64_t mh = b->fraction >> 32;
            const uint64_t lowLow = __muldi3(ml, nl);
            const uint64_t middle1 = __muldi3(mh, nl);
            const uint64_t middle2 = __muldi3(ml, nh);
            const uint64_t highHigh = __muldi3(mh, nh);
            uint64_t high = 0u;
            const uint64_t middle = middle1 + middle2;
            if (middle < middle1)
                high += 0x100000000ull;
            const uint64_t middleLow = middle << 32;
            uint64_t low = lowLow + middleLow;
            if (low < lowLow)
                ++high;
            high += (middle >> 32) + highHigh;

            tmp->normalExp = a->normalExp + b->normalExp + 4;
            tmp->sign = differ;
            constexpr uint64_t kLowTop = 1ull << 63;
            while (high >= d::kImplicit2)
            {
                ++tmp->normalExp;
                if ((high & 1u) != 0u)
                {
                    low >>= 1;
                    low |= kLowTop;
                }
                high >>= 1;
            }
            while (high < d::kImplicit1)
            {
                --tmp->normalExp;
                high <<= 1;
                if ((low & kLowTop) != 0u)
                    high |= 1u;
                low <<= 1;
            }
            tmp->fraction = roundedToEven(high, low != 0u);
            tmp->fpClass = kNumber;
            return tmp;
        }

        // _fpdiv_parts, inlined in dpdiv: long division, a bit at a time.
        Parts divideParts(Parts a, Parts b)
        {
            if (isNan(a->fpClass))
                return a;
            if (isNan(b->fpClass))
                return b;
            a->sign ^= b->sign;
            if (isInfinity(a) || isZero(a))
            {
                if (a->fpClass == b->fpClass)
                    return notANumber();
                return a;
            }
            if (isInfinity(b))
            {
                a->fraction = 0u;
                a->normalExp = 0;
                return a;
            }
            if (isZero(b))
            {
                a->fpClass = kInfinity;
                return a;
            }
            a->normalExp -= b->normalExp;
            uint64_t numerator = a->fraction;
            const uint64_t denominator = b->fraction;
            if (numerator < denominator)
            {
                numerator *= 2u;
                --a->normalExp;
            }
            uint64_t quotient = 0u;
            for (uint64_t bit = d::kImplicit1; bit != 0u; bit >>= 1)
            {
                if (numerator >= denominator)
                {
                    quotient |= bit;
                    numerator -= denominator;
                }
                numerator *= 2u;
            }
            a->fraction = roundedToEven(quotient, numerator != 0u);
            return a;
        }

        constexpr int kFractionTop = d::kFracBits + d::kNGards;
        constexpr int32_t kMaxSiInt = 0x7FFFFFFF;
    }

    // dpmul (multiply): a * b.
    uint64_t dpmul(uint64_t argA, uint64_t argB)
    {
        ee::Local<uint8_t[0x20]> a;
        ee::Local<uint8_t[0x20]> b;
        ee::Local<uint8_t[0x20]> tmp;
        ee::Local<uint64_t> sourceA;
        ee::Local<uint64_t> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_d(sourceA, a);
        __unpack_d(sourceB, b);
        return __pack_d(multiplyParts(Parts(a.address()), Parts(b.address()), Parts(tmp.address())));
    }
    READABLE_FUNCTION(dpmul, Verified);

    // dpdiv (divide): a / b.
    uint64_t dpdiv(uint64_t argA, uint64_t argB)
    {
        ee::Local<uint8_t[0x20]> a;
        ee::Local<uint8_t[0x20]> b;
        ee::Local<uint64_t> sourceA;
        ee::Local<uint64_t> sourceB;
        *sourceA = argA;
        *sourceB = argB;
        __unpack_d(sourceA, a);
        __unpack_d(sourceB, b);
        return __pack_d(divideParts(Parts(a.address()), Parts(b.address())));
    }
    READABLE_FUNCTION(dpdiv, Verified);

    // litodp (si_to_float): an int as a double.
    uint64_t litodp(int32_t value)
    {
        constexpr uint64_t kMinusTwoTo31 = 0xC1E0000000000000ull;
        ee::Local<FpNumberD> in;
        in->fpClass = kNumber;
        in->sign = value < 0;
        if (value == 0)
            in->fpClass = kZero;
        else
        {
            in->normalExp = kFractionTop;
            if (in->sign != 0u)
            {
                if (value == static_cast<int32_t>(0x80000000u))
                    return kMinusTwoTo31;
                in->fraction = static_cast<uint64_t>(static_cast<int64_t>(-value));
            }
            else
                in->fraction = static_cast<uint64_t>(static_cast<int64_t>(value));
            while (in->fraction < d::kImplicit1)
            {
                in->fraction <<= 1;
                in->normalExp -= 1;
            }
        }
        return __pack_d(in);
    }
    READABLE_FUNCTION(litodp, Verified);

    // dptoli (float_to_si): a double as an int, towards zero; out of range gives the nearest int.
    int32_t dptoli(uint64_t argA)
    {
        ee::Local<uint8_t[0x20]> parts;
        ee::Local<uint64_t> source;
        *source = argA;
        __unpack_d(source, parts);
        const Parts a(parts.address());
        if (isZero(a) || isNan(a->fpClass))
            return 0;
        const int32_t limit = a->sign != 0u ? -kMaxSiInt - 1 : kMaxSiInt;
        if (isInfinity(a))
            return limit;
        if (a->normalExp < 0)
            return 0;
        if (a->normalExp > 30)
            return limit;
        const int32_t whole = static_cast<int32_t>(a->fraction >> (kFractionTop - a->normalExp));
        return a->sign != 0u ? -whole : whole;
    }
    READABLE_FUNCTION(dptoli, Verified);

    // dptoul (float_to_usi): as an unsigned int.
    uint32_t dptoul(uint64_t argA)
    {
        ee::Local<uint8_t[0x20]> parts;
        ee::Local<uint64_t> source;
        *source = argA;
        __unpack_d(source, parts);
        const Parts a(parts.address());
        if (isZero(a) || isNan(a->fpClass) || a->sign != 0u)
            return 0u;
        if (isInfinity(a))
            return kMaxUsiInt;
        if (a->normalExp < 0)
            return 0u;
        if (a->normalExp > 31)
            return kMaxUsiInt;
        if (a->normalExp > kFractionTop)
            return static_cast<uint32_t>(a->fraction << (a->normalExp - kFractionTop));
        return static_cast<uint32_t>(a->fraction >> (kFractionTop - a->normalExp));
    }
    READABLE_FUNCTION(dptoul, Verified);

    namespace
    {
        constexpr uint64_t kZeroDouble = 0u;
        constexpr uint64_t kTwoTo16 = 0x40F0000000000000ull, kTwoTo32 = 0x41F0000000000000ull;
        constexpr uint64_t kTwoToMinus32 = 0x3DF0000000000000ull;
    }

    // libgcc2.c's __floatdidf: the high word, scaled up in two steps, plus the low word unsigned.
    uint64_t __floatdidf(int64_t u)
    {
        uint64_t d = litodp(static_cast<int32_t>(u >> 32));
        d = dpmul(d, kTwoTo16);
        d = dpmul(d, kTwoTo16);
        const int32_t low = static_cast<int32_t>(u);
        uint64_t lowDouble = litodp(low);
        if (low < 0)
            lowDouble = dpadd(lowDouble, kTwoTo32);
        return dpadd(d, lowDouble);
    }
    READABLE_FUNCTION(__floatdidf, Verified);

    // libgcc2.c's __floatdisf (DM2 links it, for ZGE2's cyclesToTime; AI and BMX XXX do not): worked in double
    // so the high word keeps its precision, as __floatdidf, then rounded to single. A value of 2^53 or more in
    // magnitude, whose low bits the double would round away, first keeps them as one bit below the single's
    // rounding place (REP_BIT, 1 << (64 - 53)), so the result is rounded once.
    inline ee::Float __floatdisf(int64_t u)
    {
        constexpr int64_t kDoubleExact = int64_t(1) << 53;  // 1 << DF_SIZE
        constexpr uint32_t kRepresentBit = 1u << (64 - 53); // REP_BIT
        if (!(-kDoubleExact < u && u < kDoubleExact))
        {
            if (static_cast<uint32_t>(u) & (kRepresentBit - 1u))
                u |= kRepresentBit;
        }
        uint64_t f = litodp(static_cast<int32_t>(u >> 32));
        f = dpmul(f, kTwoTo16);
        f = dpmul(f, kTwoTo16);
        const int32_t low = static_cast<int32_t>(u);
        uint64_t lowDouble = litodp(low);
        if (low < 0)
            lowDouble = dpadd(lowDouble, kTwoTo32);
        return dptofp(dpadd(f, lowDouble));
    }
    READABLE_FUNCTION_ONLY(dm2, __floatdisf);

    // libgcc2.c's __fixunsdfdi: the high word from a / 2^32, then what is left, which can be negative.
    uint64_t __fixunsdfdi(uint64_t a)
    {
        if (dpcmp(a, kZeroDouble) < 0)
            return 0u;
        const uint64_t high = static_cast<uint64_t>(dptoul(dpmul(a, kTwoToMinus32))) << 32;
        uint64_t highDouble;
        if (static_cast<int64_t>(high) >= 0)
            highDouble = __floatdidf(static_cast<int64_t>(high));
        else
        {
            const uint64_t half = __floatdidf(static_cast<int64_t>((high & 1u) | high >> 1));
            highDouble = dpadd(half, half);
        }
        const uint64_t rest = dpsub(a, highDouble);
        if (dpcmp(rest, kZeroDouble) < 0)
            return high - dptoul(dpsub(kZeroDouble, rest));
        return high + dptoul(rest);
    }
    READABLE_FUNCTION(__fixunsdfdi, Verified);

    int64_t __fixdfdi(uint64_t a)
    {
        if (dpcmp(a, kZeroDouble) < 0)
            return -static_cast<int64_t>(__fixunsdfdi(dpsub(kZeroDouble, a)));
        return static_cast<int64_t>(__fixunsdfdi(a));
    }
    READABLE_FUNCTION(__fixdfdi, Verified);

    // libgcc2.c's __fixunssfdi (DM2 links it, for ZGE2's screen rectangles' depth and the shadow buffer's TEX0
    // width; AI and BMX XXX do not): the float widened to a double, which loses no bits, then __fixunsdfdi's
    // steps on it (libgcc2.c has them twice, as here): the high word from a / 2^32, then what is left.
    inline uint64_t __fixunssfdi(ee::Float original)
    {
        const uint64_t a = fptodp(original);
        if (dpcmp(a, kZeroDouble) < 0)
            return 0u;
        const uint64_t high = static_cast<uint64_t>(dptoul(dpmul(a, kTwoToMinus32))) << 32;
        uint64_t highDouble;
        if (static_cast<int64_t>(high) >= 0)
            highDouble = __floatdidf(static_cast<int64_t>(high));
        else
        {
            const uint64_t half = __floatdidf(static_cast<int64_t>((high & 1u) | high >> 1));
            highDouble = dpadd(half, half);
        }
        const uint64_t rest = dpsub(a, highDouble);
        if (dpcmp(rest, kZeroDouble) < 0)
            return high - dptoul(dpsub(kZeroDouble, rest));
        return high + dptoul(rest);
    }
    READABLE_FUNCTION_ONLY(dm2, __fixunssfdi);

    // ---- libgcc2.c, L__main ------------------------------------------------------------

    namespace
    {
        // The source's static p: the next of __DTOR_LIST__'s destructors to run, from its
        // second word on (the first holds the count); the list ends at a zero.
        inline ee::Global<ee::Ptr<ee::FunctionPtr<void()>>, sym::g_dtorList> g_nextDestructor;
    }

    // __do_global_dtors: runs the global destructors in turn. The pointer moves past each one
    // before it runs, so a destructor that exits does not run twice. Nothing calls it in any
    // of the three games.
    void __do_global_dtors()
    {
        ee::Ptr<ee::FunctionPtr<void()>> &next = *g_nextDestructor;
        while (*next)
        {
            ++next;
            (*(next - 1))();
        }
    }
    READABLE_FUNCTION(__do_global_dtors, Draft);

    namespace
    {
        // The source's static initialized: 0 until __main has run the constructors.
        inline ee::Global<int32_t, sym::g_mainInitialized> g_mainInitialized;
        namespace tail
        {
            int32_t __do_global_ctors() { return ee::tailCall<int32_t>(sym::__do_global_ctors); }
        }
    }

    // __main (libgcc2.c, L__main): main's first call. Runs the global constructors once, so a
    // recursive main does not run them again. The source's __main returns nothing; the compiled
    // one leaves 1 as its result when it has run before, and that is kept.
    int32_t __main()
    {
        if (*g_mainInitialized != 0)
            return 1;
        *g_mainInitialized = 1;
        return tail::__do_global_ctors();
    }
    READABLE_FUNCTION(__main, Verified);
}
