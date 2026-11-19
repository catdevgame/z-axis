// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdlib/strtod.c and
// dtoa.c carry the notice below.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// The author of this software is David M. Gay.
//
// Copyright (c) 1991 by AT&T.
//
// Permission to use, copy, modify, and distribute this software for any
// purpose without fee is hereby granted, provided that this entire notice
// is included in all copies of any software which is or includes a copy
// or modification of this software and in all copies of the supporting
// documentation for such software.
//
// THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
// WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR AT&T MAKES ANY
// REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
// OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.

// stdlib/strtod.c and dtoa.c, newlib 1.8.2: David Gay's correctly rounded decimal to double, and
// back (dtoa: the float printing's, dead in the game, so Draft). The same code in DM2 and BMX XXX.
// The EE has no double-precision hardware: a double travels as its 64-bit pattern with word 0 its
// high half, and every sum, product and comparison is a call into libgcc, in the order the
// compiled code makes them. strtod's jumps are the three stages below; dtoa's are its endings.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libc.h"
#include "sce/libgcc.h"

#include <cstdint>
#include "sym/sce/libc_strtod.cpp.h"

namespace sce::libc
{
    namespace
    {
        inline ee::Global<ee::Ptr<Reent>, sym::g_impurePtr> g_impurePtr;
        inline ee::Global<uint64_t, sym::g_doubleInfinity> g_doubleInfinity; // HUGE_VAL
        inline ee::Global<uint64_t[25], sym::g_tens> g_tens;                 // 1e0 .. 1e24
        inline ee::Global<uint64_t[5], sym::g_bigTens> g_bigTens;            // 1e16 .. 1e256
        inline ee::Global<uint64_t[5], sym::g_tinyTens> g_tinyTens;          // 1e-16 .. 1e-256
        inline ee::Global<uint64_t, sym::g_aadjBelowHalf> g_aadjBelowHalf;       // .4999999
        inline ee::Global<uint64_t, sym::g_aadjAboveHalf> g_aadjAboveHalf;       // .5000001
        inline ee::Global<uint64_t, sym::g_aadjBelowQuarter> g_aadjBelowQuarter; // .4999999 / 2

        constexpr int32_t kErange = 34;
        constexpr uint64_t kZero = 0ull, kOne = 0x3FF0000000000000ull, kMinusOne = 0xBFF0000000000000ull;
        constexpr uint64_t kHalf = 0x3FE0000000000000ull, kTwo = 0x4000000000000000ull;
        constexpr uint64_t kTwoTo32 = 0x41F0000000000000ull;
        constexpr int kDecimalDigits = 15;       // DBL_DIG
        constexpr int kLargestTablePower = 22;   // Ten_pmax
        constexpr int kMaxDecimalExponent = 308; // DBL_MAX_10_EXP
        constexpr int kBigTensCount = 5;         // n_bigtens
        constexpr int kPrecisionBits = 53;       // P
        constexpr int kMinimumExponent = -1022;  // Emin
        constexpr int kMaxExponent = 1024;       // DBL_MAX_EXP
        constexpr int kBias = 1023;              // Bias
        constexpr uint32_t kExponentMask = 0x7FF00000u; // Exp_mask
        constexpr uint32_t kExponentUnit = 1u << 20;    // Exp_msk1
        constexpr uint32_t kBoundaryMask = 0xFFFFFu;    // Bndry_mask
        constexpr uint32_t kBiggestHigh = 0x7FEFFFFFu;  // Big0
        constexpr uint32_t kBiggestLow = 0xFFFFFFFFu;   // Big1
        constexpr uint64_t kBiggest = static_cast<uint64_t>(kBiggestHigh) << 32 | kBiggestLow;
        constexpr uint64_t kSmallest = 1ull;            // Tiny0, Tiny1

        uint32_t word0(uint64_t d) { return static_cast<uint32_t>(d >> 32); }
        uint32_t word1(uint64_t d) { return static_cast<uint32_t>(d); }
        uint64_t withHigh(uint64_t d, uint32_t high) { return static_cast<uint64_t>(high) << 32 | word1(d); }

        void copyNumber(ee::Ptr<int32_t> to, ee::Ptr<int32_t> from, uint32_t bytes) { ee::game(sym::memcpy, to, from, bytes); }

        // An unsigned value as a double: the compiled code goes through the signed conversion.
        uint64_t doubleOf(uint32_t y)
        {
            const uint64_t d = libgcc::litodp(static_cast<int32_t>(y));
            return static_cast<int32_t>(y) < 0 ? libgcc::dpadd(d, kTwoTo32) : d;
        }
        uint64_t doubleOf(uint64_t z)
        {
            if (static_cast<int64_t>(z) >= 0)
                return libgcc::__floatdidf(static_cast<int64_t>(z));
            const uint64_t half = libgcc::__floatdidf(static_cast<int64_t>((z & 1u) | z >> 1));
            return libgcc::dpadd(half, half);
        }

        bool isDigit(int c) { return c >= '0' && c <= '9'; }
        bool isNonZeroDigit(int c) { return c > '0' && c <= '9'; }
    }

    uint64_t _strtod_r(ee::Ptr<Reent> reent, ee::Ptr<const char> s00, ee::Ptr<ee::Ptr<const char>> se)
    {
        ee::Local<int32_t> bbe;
        ee::Local<int32_t> bbbits;
        ee::Ptr<const char> s = s00;
        bool negative = false;
        uint64_t rv = kZero;

        const auto finish = [&]() -> uint64_t {
            if (se)
                *se = s;
            return negative ? libgcc::dpsub(kZero, rv) : rv;
        };
        const auto overflowed = [&]() {
            reent->errorNumber = kErange;
            rv = *g_doubleInfinity;
        };
        const auto underflowed = [&]() {
            rv = kZero;
            reent->errorNumber = kErange;
        };

        // The sign, past white space. A sign with nothing after it is no number.
        for (;; ++s)
        {
            const char c = *s;
            if (c == '-' || c == '+')
            {
                negative = c == '-';
                ++s;
                if (*s == '\0')
                {
                    s = s00;
                    return finish();
                }
                break;
            }
            if (c == '\0')
            {
                s = s00;
                return finish();
            }
            if (c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r' && c != ' ')
                break;
        }

        // The digits: the first 9 in y, the next 7 in z, the count in nd, nd0 before the point.
        int32_t nz0 = 0;
        if (*s == '0')
        {
            nz0 = 1;
            do
                ++s;
            while (*s == '0');
            if (*s == '\0')
                return finish();
        }
        ee::Ptr<const char> firstDigit = s;
        uint32_t y = 0u;
        uint64_t z = 0u;
        int32_t nd = 0, nf = 0, nz = 0;
        int c;
        for (; c = *s, isDigit(c); ++nd, ++s)
        {
            if (nd < 9)
                y = 10u * y + static_cast<uint32_t>(c) - '0';
            else if (nd < 16)
                z = 10u * z + static_cast<uint64_t>(static_cast<int64_t>(c)) - '0';
        }
        int32_t nd0 = nd;
        if (c == '.')
        {
            ++s;
            c = *s;
            if (nd == 0)
            {
                for (; c == '0'; c = *s)
                {
                    ++nz;
                    ++s;
                }
                if (isNonZeroDigit(c))
                {
                    firstDigit = s;
                    nf += nz;
                    nz = 0;
                }
            }
            for (; isDigit(c); ++s, c = *s)
            {
                ++nz;
                c -= '0';
                if (c != 0)
                {
                    nf += nz;
                    for (int32_t i = 1; i < nz; ++i)
                    {
                        if (nd++ < 9)
                            y *= 10u;
                        else if (nd <= kDecimalDigits + 1)
                            z *= 10u;
                    }
                    if (nd++ < 9)
                        y = 10u * y + static_cast<uint32_t>(c);
                    else if (nd <= kDecimalDigits + 1)
                        z = 10u * z + static_cast<uint64_t>(static_cast<int64_t>(c));
                    nz = 0;
                }
            }
        }

        // The exponent. Without digits there is no number.
        int64_t e = 0;
        if (c == 'e' || c == 'E')
        {
            if (nd == 0 && nz == 0 && nz0 == 0)
            {
                s = s00;
                return finish();
            }
            s00 = s;
            bool exponentNegative = false;
            ++s;
            c = *s;
            if (c == '-' || c == '+')
            {
                exponentNegative = c == '-';
                ++s;
                c = *s;
            }
            if (isDigit(c))
            {
                while (c == '0')
                {
                    ++s;
                    c = *s;
                }
                if (isNonZeroDigit(c))
                {
                    e = c - '0';
                    const ee::Ptr<const char> exponentStart = s;
                    for (;;)
                    {
                        ++s;
                        c = *s;
                        if (!isDigit(c))
                            break;
                        e = 10 * e + c - '0';
                    }
                    if (s.get() - exponentStart.get() > 8)
                        e = 9999999;
                    if (exponentNegative)
                        e = -e;
                }
                else
                    e = 0;
            }
            else
                s = s00;
        }
        if (nd == 0)
        {
            if (nz == 0 && nz0 == 0)
                s = s00;
            return finish();
        }

        // A first approximation, exact when the number is small enough.
        e -= nf;
        int32_t e1 = static_cast<int32_t>(e);
        if (nd0 == 0)
            nd0 = nd;
        const int32_t k = nd < kDecimalDigits + 1 ? nd : kDecimalDigits + 1;
        rv = doubleOf(y);
        if (k > 9)
        {
            const uint64_t scaled = libgcc::dpmul((*g_tens)[k - 9], rv);
            rv = libgcc::dpadd(scaled, doubleOf(z));
        }
        if (nd <= kDecimalDigits)
        {
            if (e == 0)
                return finish();
            if (e > 0)
            {
                if (e <= kLargestTablePower)
                {
                    rv = libgcc::dpmul((*g_tens)[e], rv);
                    return finish();
                }
                const int32_t spare = kDecimalDigits - nd;
                if (e <= kLargestTablePower + spare)
                {
                    e -= spare;
                    rv = libgcc::dpmul((*g_tens)[spare], rv);
                    rv = libgcc::dpmul((*g_tens)[e], rv);
                    return finish();
                }
            }
            else if (e >= -kLargestTablePower)
            {
                rv = libgcc::dpdiv(rv, (*g_tens)[-e]);
                return finish();
            }
        }
        e1 += nd - k;

        // Scales by the rest of the exponent, keeping clear of overflow and underflow on the way.
        if (e1 > 0)
        {
            const int32_t low = e1 & 15;
            if (low != 0)
                rv = libgcc::dpmul((*g_tens)[low], rv);
            e1 &= ~15;
            if (e1 != 0)
            {
                if (e1 > kMaxDecimalExponent)
                {
                    overflowed();
                    return finish();
                }
                e1 >>= 4;
                int32_t j = 0;
                for (; e1 > 1; ++j, e1 >>= 1)
                {
                    if ((e1 & 1) != 0)
                        rv = libgcc::dpmul((*g_bigTens)[j], rv);
                }
                rv = libgcc::dpmul((*g_bigTens)[j], withHigh(rv, word0(rv) - kPrecisionBits * kExponentUnit));
                const uint32_t exponent = word0(rv) & kExponentMask;
                if (exponent > kExponentUnit * (kMaxExponent + kBias - kPrecisionBits))
                {
                    overflowed();
                    return finish();
                }
                if (exponent > kExponentUnit * (kMaxExponent + kBias - 1 - kPrecisionBits))
                    rv = kBiggest;
                else
                    rv = withHigh(rv, word0(rv) + kPrecisionBits * kExponentUnit);
            }
        }
        else if (e1 < 0)
        {
            e1 = -e1;
            const int32_t low = e1 & 15;
            if (low != 0)
                rv = libgcc::dpdiv(rv, (*g_tens)[low]);
            e1 &= ~15;
            if (e1 != 0)
            {
                e1 >>= 4;
                if (e1 >= 1 << kBigTensCount)
                {
                    underflowed();
                    return finish();
                }
                int32_t j = 0;
                for (; e1 > 1; ++j, e1 >>= 1)
                {
                    if ((e1 & 1) != 0)
                        rv = libgcc::dpmul((*g_tinyTens)[j], rv);
                }
                const uint64_t before = rv;
                rv = libgcc::dpmul((*g_tinyTens)[j], rv);
                if (libgcc::dpcmp(rv, kZero) == 0)
                {
                    rv = libgcc::dpmul((*g_tinyTens)[j], libgcc::dpadd(before, before));
                    if (libgcc::dpcmp(rv, kZero) == 0)
                    {
                        underflowed();
                        return finish();
                    }
                    rv = kSmallest;
                }
            }
        }

        // Corrects rv against the digits, in exact arithmetic, until it is the nearest double.
        const ee::Ptr<Bigint> bd0 = _s2b(reent, firstDigit, nd0, nd, y);
        ee::Ptr<Bigint> bb, bd, bs, delta;
        for (;;)
        {
            bd = _Balloc(reent, bd0->k);
            copyNumber(&bd->sign, &bd0->sign, static_cast<uint32_t>(bd0->wordCount) * sizeof(int32_t) + 2u * sizeof(int32_t));
            bb = _d2b(reent, rv, bbe, bbbits);
            bs = _i2b(reent, 1);
            int32_t bb2, bb5, bd2, bd5;
            if (e >= 0)
            {
                bb2 = bb5 = 0;
                bd2 = bd5 = static_cast<int32_t>(e);
            }
            else
            {
                bb2 = bb5 = static_cast<int32_t>(-e);
                bd2 = bd5 = 0;
            }
            if (*bbe >= 0)
                bb2 += *bbe;
            else
                bd2 -= *bbe;
            int32_t bs2 = bb2;
            const int32_t topBit = *bbe + *bbbits - 1;
            const int32_t shift = topBit < kMinimumExponent ? *bbe + (kPrecisionBits - kMinimumExponent) : kPrecisionBits + 1 - *bbbits;
            bb2 += shift;
            bd2 += shift;
            int32_t common = bb2 < bd2 ? bb2 : bd2;
            if (common > bs2)
                common = bs2;
            if (common > 0)
            {
                bb2 -= common;
                bd2 -= common;
                bs2 -= common;
            }
            if (bb5 > 0)
            {
                bs = _pow5mult(reent, bs, bb5);
                const ee::Ptr<Bigint> product = _multiply(reent, bs, bb);
                _Bfree(reent, bb);
                bb = product;
            }
            if (bb2 > 0)
                bb = _lshift(reent, bb, bb2);
            if (bd5 > 0)
                bd = _pow5mult(reent, bd, bd5);
            if (bd2 > 0)
                bd = _lshift(reent, bd, bd2);
            if (bs2 > 0)
                bs = _lshift(reent, bs, bs2);
            delta = __mdiff(reent, bb, bd);
            const bool dsign = delta->sign != 0;
            delta->sign = 0;
            const int32_t order = __mcmp(delta, bs);
            const bool onBoundary = word1(rv) == 0u && (word0(rv) & kBoundaryMask) == 0u;
            const auto dropDown = [&]() { rv = withHigh(0u, ((word0(rv) & kExponentMask) - kExponentUnit) | kBoundaryMask) | 0xFFFFFFFFu; };
            if (order < 0)
            {
                // The error is less than half a unit in the last place.
                if (dsign || !onBoundary)
                    break;
                delta = _lshift(reent, delta, 1);
                if (__mcmp(delta, bs) > 0)
                    dropDown();
                break;
            }
            if (order == 0)
            {
                // Exactly half way.
                if (dsign)
                {
                    if ((word0(rv) & kBoundaryMask) == kBoundaryMask && word1(rv) == 0xFFFFFFFFu)
                    {
                        rv = withHigh(0u, (word0(rv) & kExponentMask) + kExponentUnit);
                        break;
                    }
                }
                else if (onBoundary)
                {
                    dropDown();
                    break;
                }
                if ((word1(rv) & 1u) == 0u)
                    break;
                if (dsign)
                    rv = libgcc::dpadd(_ulp(rv), rv);
                else
                {
                    rv = libgcc::dpsub(rv, _ulp(rv));
                    if (libgcc::dpcmp(rv, kZero) == 0)
                        underflowed();
                }
                break;
            }
            uint64_t aadj = _ratio(delta, bs);
            uint64_t aadj1;
            if (libgcc::dpcmp(aadj, kTwo) <= 0)
            {
                if (dsign)
                {
                    aadj = kOne;
                    aadj1 = kOne;
                }
                else if (!onBoundary)
                {
                    if (word1(rv) == 1u && word0(rv) == 0u)
                    {
                        underflowed();
                        break;
                    }
                    aadj = kOne;
                    aadj1 = kMinusOne;
                }
                else
                {
                    aadj = libgcc::dpcmp(aadj, kOne) < 0 ? kHalf : libgcc::dpmul(aadj, kHalf);
                    aadj1 = libgcc::dpsub(kZero, aadj);
                }
            }
            else
            {
                aadj = libgcc::dpmul(aadj, kHalf);
                aadj1 = dsign ? aadj : libgcc::dpsub(kZero, aadj);
            }
            const uint32_t before = word0(rv) & kExponentMask;
            bool mayStop = true;
            if (before == kExponentUnit * (kMaxExponent + kBias - 1))
            {
                const uint64_t saved = rv;
                rv = withHigh(rv, word0(rv) - kPrecisionBits * kExponentUnit);
                rv = libgcc::dpadd(libgcc::dpmul(aadj1, _ulp(rv)), rv);
                if ((word0(rv) & kExponentMask) >= kExponentUnit * (kMaxExponent + kBias - kPrecisionBits))
                {
                    if (saved == kBiggest)
                    {
                        overflowed();
                        break;
                    }
                    rv = kBiggest;
                    mayStop = false;
                }
                else
                    rv = withHigh(rv, word0(rv) + kPrecisionBits * kExponentUnit);
            }
            else
            {
                if (before <= (kPrecisionBits - 1) * kExponentUnit && libgcc::dpcmp(aadj, kOne) >= 0)
                {
                    aadj1 = libgcc::litodp(libgcc::dptoli(libgcc::dpadd(aadj, kHalf)));
                    if (!dsign)
                        aadj1 = libgcc::dpsub(kZero, aadj1);
                }
                rv = libgcc::dpadd(libgcc::dpmul(aadj1, _ulp(rv)), rv);
            }
            if (mayStop && before == (word0(rv) & kExponentMask))
            {
                // Can we stop? Only if the correction's own rounding cannot matter.
                aadj = libgcc::dpsub(aadj, libgcc::__floatdidf(libgcc::__fixdfdi(aadj)));
                if (dsign || word1(rv) != 0u || (word0(rv) & kBoundaryMask) != 0u)
                {
                    if (libgcc::dpcmp(aadj, *g_aadjBelowHalf) < 0 || libgcc::dpcmp(aadj, *g_aadjAboveHalf) > 0)
                        break;
                }
                else if (libgcc::dpcmp(aadj, *g_aadjBelowQuarter) < 0)
                    break;
            }
            _Bfree(reent, bb);
            _Bfree(reent, bd);
            _Bfree(reent, bs);
            _Bfree(reent, delta);
        }
        _Bfree(reent, bb);
        _Bfree(reent, bd);
        _Bfree(reent, bs);
        _Bfree(reent, bd0);
        _Bfree(reent, delta);
        return finish();
    }
    READABLE_FUNCTION(_strtod_r, Verified);

    // ---- stdlib/dtoa.c ----------------------------------------------------------------------

    namespace
    {
        // The estimate of log10 (d) from d's exponent and the first-order Taylor series of
        // log10 about 1.5: (d2 - 1.5) * slope + offset + exponent * log10 (2).
        inline ee::Global<uint64_t, sym::g_dtoaLogSlope> g_dtoaLogSlope;   // 0.289529654602168
        inline ee::Global<uint64_t, sym::g_dtoaLogOffset> g_dtoaLogOffset; // 0.1760912590558, log10 (1.5) rounded up
        inline ee::Global<uint64_t, sym::g_log10Of2> g_log10Of2;           // 0.301029995663981
        constexpr uint64_t kOneAndHalf = 0x3FF8000000000000ull, kFive = 0x4014000000000000ull;
        constexpr uint64_t kSeven = 0x401C000000000000ull, kTen = 0x4024000000000000ull;
        constexpr uint32_t kSignBit = 0x80000000u;      // Sign_bit
        constexpr uint32_t kFractionHigh = 0xFFFFFu;    // Frac_mask1, Bndry_mask: the fraction's bits in word 0
        constexpr uint32_t kExponentOfOne = 0x3FF00000u; // Exp_11
        constexpr int kExponentShift = 20;              // Exp_shift1
        constexpr int kQuickMax = 14;                   // Quick_max: the most digits the quick estimate tries
        constexpr int kIntMax = 14;                     // Int_max
        constexpr int kBletch = 0x10;                   // Bletch
        constexpr int kLog2P = 1;                       // Log2P
        constexpr int32_t kNotANumberPoint = 9999;      // *decpt for an infinity or a NaN

        ee::Ptr<char> charactersOf(ee::Ptr<Bigint> b) { return reinterpret_cast<char *>(b.get()); }

        // A word of a number, moving on (the source's *x++).
        uint32_t takeWord(ee::Ptr<uint32_t> &at)
        {
            const uint32_t word = *at;
            ++at;
            return word;
        }
        // Storeinc: a word from its two halves, moving on.
        void storeHalves(ee::Ptr<uint32_t> &at, uint32_t high, uint32_t low)
        {
            *at = high << 16 | (low & 0xFFFFu);
            ++at;
        }
    }

    // quorem: the next decimal digit: how many times S goes into b (at most 9), with b left the
    // remainder. dtoa shifts S so its top word has 4 leading zero bits, which keeps the first
    // estimate, top word by top word plus one, at or below the true quotient and close to it.
    int32_t quorem(ee::Ptr<Bigint> b, ee::Ptr<Bigint> S)
    {
        int32_t n = S->wordCount;
        if (b->wordCount < n)
            return 0;
        ee::Ptr<uint32_t> sx = S->x;
        const ee::Ptr<uint32_t> sxe = sx + --n;
        ee::Ptr<uint32_t> bx = b->x;
        ee::Ptr<uint32_t> bxe = bx + n;
        uint32_t q = *bxe / (*sxe + 1u); // ensure q <= true quotient

        // b -= multiple * S, a half word at a time.
        const auto subtractMultiple = [&](uint32_t multiple) {
            int32_t borrow = 0;
            uint32_t carry = 0u;
            do
            {
                const uint32_t si = takeWord(sx);
                const uint32_t ys = (si & 0xFFFFu) * multiple + carry;
                const uint32_t zs = (si >> 16) * multiple + (ys >> 16);
                carry = zs >> 16;
                const int32_t y = static_cast<int32_t>((*bx & 0xFFFFu) - (ys & 0xFFFFu)) + borrow;
                borrow = y >> 16;
                const int32_t z = static_cast<int32_t>((*bx >> 16) - (zs & 0xFFFFu)) + borrow;
                borrow = z >> 16;
                storeHalves(bx, static_cast<uint32_t>(z), static_cast<uint32_t>(y));
            } while (sx.get() <= sxe.get());
        };
        // b's count of words without the zero ones at the top.
        const auto dropTopZeroes = [&]() {
            bx = b->x;
            for (;;)
            {
                bxe -= 1;
                if (!(bxe.get() > bx.get() && *bxe == 0u))
                    break;
                --n;
            }
            b->wordCount = n;
        };

        if (q != 0u)
        {
            subtractMultiple(q);
            if (*bxe == 0u)
                dropTopZeroes();
        }
        if (__mcmp(b, S) >= 0)
        {
            ++q;
            bx = b->x;
            sx = S->x;
            subtractMultiple(1u);
            bx = b->x;
            bxe = bx + n;
            if (*bxe == 0u)
                dropTopZeroes();
        }
        return static_cast<int32_t>(q);
    }
    READABLE_FUNCTION(quorem, Draft);

    // _dtoa_r: David Gay's double to decimal digits. The digits of value (its sign to *sign, the
    // decimal point's place to *decpt, the end of the digits to *rve), in the _reent's result
    // buffer, with no trailing zeroes. mode 0 and 1: the shortest string that reads back as value
    // (1 with Steele and White's stopping rule); 2: max (1, ndigits) significant digits; 3: ndigits
    // past the decimal point, which can be negative; 4 to 9 as 2 and 3 by other routes. An
    // infinity or a NaN is "Infinity" or "NaN" with *decpt 9999. The source's jumps are the
    // endings below: each path ends in one, and the endings share their tails.
    ee::Ptr<char> _dtoa_r(ee::Ptr<Reent> reent, uint64_t value, int32_t mode, int32_t ndigits, ee::Ptr<int32_t> decpt, ee::Ptr<int32_t> sign,
                           ee::Ptr<ee::Ptr<char>> rve)
    {
        ee::Local<int32_t> be;
        ee::Local<int32_t> bbits;
        uint64_t d = value;

        // The last call's buffer goes back.
        if (reent->dtoaResult)
        {
            reent->dtoaResult->k = reent->dtoaResultK;
            reent->dtoaResult->maxWords = 1 << reent->dtoaResultK;
            _Bfree(reent, reent->dtoaResult);
            reent->dtoaResult = ee::Ptr<Bigint>();
        }
        if ((word0(d) & kSignBit) != 0u)
        {
            *sign = 1;
            d = withHigh(d, word0(d) & ~kSignBit); // clear the sign bit
        }
        else
            *sign = 0;
        if ((word0(d) & kExponentMask) == kExponentMask)
        {
            *decpt = kNotANumberPoint;
            const bool infinity = word1(d) == 0u && (word0(d) & kFractionHigh) == 0u;
            const ee::Ptr<char> text(infinity ? sym::g_textInfinity : sym::g_textNaN2);
            if (rve)
                *rve = text + (text[3] != '\0' ? 8 : 3);
            return text;
        }
        if (libgcc::dpcmp(d, kZero) == 0)
        {
            *decpt = 1;
            const ee::Ptr<char> text(sym::g_textZeroDigit2);
            if (rve)
                *rve = text + 1;
            return text;
        }

        ee::Ptr<Bigint> b = _d2b(reent, d, be, bbits);
        int32_t i = static_cast<int32_t>(word0(d) >> kExponentShift & (kExponentMask >> kExponentShift));
        uint64_t d2;
        bool denormal;
        if (i != 0)
        {
            d2 = withHigh(d, (word0(d) & kFractionHigh) | kExponentOfOne);
            i -= kBias;
            denormal = false;
        }
        else
        {
            // d is denormalized: its bits as a whole number, scaled.
            i = *bbits + *be + (kBias + (kPrecisionBits - 1) - 1);
            const uint32_t x = i > 32 ? word0(d) << (64 - i) | word1(d) >> (i - 32) : word1(d) << (32 - i);
            d2 = doubleOf(x);
            d2 = withHigh(d2, word0(d2) - 31 * kExponentUnit); // adjust exponent
            i -= (kBias + (kPrecisionBits - 1) - 1) + 1;
            denormal = true;
        }

        // k = floor (log10 (d)), from the estimate, which errs on the high side.
        uint64_t ds = libgcc::dpsub(d2, kOneAndHalf);
        ds = libgcc::dpmul(ds, *g_dtoaLogSlope);
        ds = libgcc::dpadd(ds, *g_dtoaLogOffset);
        ds = libgcc::dpadd(ds, libgcc::dpmul(libgcc::litodp(i), *g_log10Of2));
        int32_t k = libgcc::dptoli(ds);
        if (libgcc::dpcmp(ds, kZero) < 0 && libgcc::dpcmp(ds, libgcc::litodp(k)) != 0)
            k--; // want k = floor (ds)
        bool kCheck = true;
        if (k >= 0 && k <= kLargestTablePower)
        {
            if (libgcc::dpcmp(d, (*g_tens)[k]) < 0)
                k--;
            kCheck = false;
        }
        int32_t b2, s2, b5, s5;
        const int32_t j = *bbits - i - 1;
        if (j >= 0)
        {
            b2 = 0;
            s2 = j;
        }
        else
        {
            b2 = -j;
            s2 = 0;
        }
        if (k >= 0)
        {
            b5 = 0;
            s5 = k;
            s2 += k;
        }
        else
        {
            b2 -= k;
            b5 = -k;
            s5 = 0;
        }
        if (mode < 0 || mode > 9)
            mode = 0;
        bool tryQuick = true;
        if (mode > 5)
        {
            mode -= 4;
            tryQuick = false;
        }
        bool leftright = true;
        int32_t ilim = 0, ilim1 = 0;
        switch (mode)
        {
        case 0:
        case 1:
            ilim = ilim1 = -1;
            i = 18;
            ndigits = 0;
            break;
        case 2:
            leftright = false;
            [[fallthrough]];
        case 4:
            if (ndigits <= 0)
                ndigits = 1;
            ilim = ilim1 = i = ndigits;
            break;
        case 3:
            leftright = false;
            [[fallthrough]];
        case 5:
            i = ndigits + k + 1;
            ilim = i;
            ilim1 = i - 1;
            if (i <= 0)
                i = 1;
            break;
        }

        // The result's buffer: a number block big enough for i characters.
        reent->dtoaResultK = 0;
        for (uint32_t bytes = sizeof(uint32_t); sizeof(Bigint) - sizeof(uint32_t) + bytes <= static_cast<uint32_t>(i); bytes <<= 1)
            reent->dtoaResultK++;
        reent->dtoaResult = _Balloc(reent, reent->dtoaResultK);
        const ee::Ptr<char> s0 = charactersOf(reent->dtoaResult);
        ee::Ptr<char> s = s0;
        ee::Ptr<Bigint> S, mhi, mlo;
        const auto put = [&s](int32_t c) {
            *s = static_cast<char>(c);
            ++s;
        };

        // How a path ends: Done is the source's ret1 (only b to give back), Digits its ret (S,
        // mhi and mlo too); the others do their part first.
        enum class Ending
        {
            NotYet,      // the quick estimate gave up (fast_failed)
            Done,        // ret1
            Digits,      // ret
            BumpUp,      // bump_up: the last digit up, carrying through nines
            NoDigits,    // no_digits: fcvt style, nothing at this precision
            OneDigit,    // one_digit: a 1 at the next power of ten
            RoundNineUp, // round_9_up: a 9 that rounds up
            RoundOff,    // roundoff: the last digit up, carrying
        };

        // Try to get by with floating point when few digits are asked for.
        const auto quickDigits = [&]() -> Ending {
            int32_t power = 0;
            int32_t ieps = 2; // conservative
            if (k > 0)
            {
                ds = (*g_tens)[k & 0xF];
                int32_t big = k >> 4;
                if ((big & kBletch) != 0)
                {
                    big &= kBletch - 1;
                    d = libgcc::dpdiv(d, (*g_bigTens)[kBigTensCount - 1]);
                    ieps++;
                }
                for (; big != 0; big >>= 1, power++)
                {
                    if ((big & 1) != 0)
                    {
                        ieps++;
                        ds = libgcc::dpmul(ds, (*g_bigTens)[power]);
                    }
                }
                d = libgcc::dpdiv(d, ds);
            }
            else if (const int32_t j1 = -k; j1 != 0)
            {
                d = libgcc::dpmul(d, (*g_tens)[j1 & 0xF]);
                for (int32_t big = j1 >> 4; big != 0; big >>= 1, power++)
                {
                    if ((big & 1) != 0)
                    {
                        ieps++;
                        d = libgcc::dpmul(d, (*g_bigTens)[power]);
                    }
                }
            }
            if (kCheck && libgcc::dpcmp(d, kOne) < 0 && ilim > 0)
            {
                if (ilim1 <= 0)
                    return Ending::NotYet;
                ilim = ilim1;
                k--;
                d = libgcc::dpmul(d, kTen);
                ieps++;
            }
            uint64_t eps = libgcc::dpadd(libgcc::dpmul(libgcc::litodp(ieps), d), kSeven);
            eps = withHigh(eps, word0(eps) - (kPrecisionBits - 1) * kExponentUnit);
            if (ilim == 0)
            {
                S = mhi = ee::Ptr<Bigint>();
                d = libgcc::dpsub(d, kFive);
                if (libgcc::dpcmp(d, eps) > 0)
                    return Ending::OneDigit;
                if (libgcc::dpcmp(d, libgcc::dpsub(kZero, eps)) < 0)
                    return Ending::NoDigits;
                return Ending::NotYet;
            }
            if (leftright)
            {
                // Steele and White: only the digits needed.
                eps = libgcc::dpsub(libgcc::dpdiv(kHalf, (*g_tens)[ilim - 1]), eps);
                for (int32_t n = 0;;)
                {
                    const int32_t L = libgcc::dptoli(d);
                    d = libgcc::dpsub(d, libgcc::litodp(L));
                    put('0' + L);
                    if (libgcc::dpcmp(d, eps) < 0)
                        return Ending::Done;
                    if (libgcc::dpcmp(libgcc::dpsub(kOne, d), eps) < 0)
                        return Ending::BumpUp;
                    if (++n >= ilim)
                        break;
                    eps = libgcc::dpmul(eps, kTen);
                    d = libgcc::dpmul(d, kTen);
                }
            }
            else
            {
                // ilim digits, then fix them up.
                eps = libgcc::dpmul(eps, (*g_tens)[ilim - 1]);
                for (int32_t n = 1;; n++, d = libgcc::dpmul(d, kTen))
                {
                    const int32_t L = libgcc::dptoli(d);
                    d = libgcc::dpsub(d, libgcc::litodp(L));
                    put('0' + L);
                    if (n == ilim)
                    {
                        if (libgcc::dpcmp(d, libgcc::dpadd(kHalf, eps)) > 0)
                            return Ending::BumpUp;
                        if (libgcc::dpcmp(d, libgcc::dpsub(kHalf, eps)) < 0)
                        {
                            do
                                s -= 1;
                            while (*s == '0');
                            ++s;
                            return Ending::Done;
                        }
                        break;
                    }
                }
            }
            return Ending::NotYet;
        };

        // A whole number below 1e15: its digits by floating point.
        const auto smallIntegerDigits = [&]() -> Ending {
            ds = (*g_tens)[k];
            if (ndigits < 0 && ilim <= 0)
            {
                S = mhi = ee::Ptr<Bigint>();
                if (ilim < 0 || libgcc::dpcmp(d, libgcc::dpmul(kFive, ds)) <= 0)
                    return Ending::NoDigits;
                return Ending::OneDigit;
            }
            for (int32_t n = 1;; n++)
            {
                const int32_t L = libgcc::dptoli(libgcc::dpdiv(d, ds));
                d = libgcc::dpsub(d, libgcc::dpmul(libgcc::litodp(L), ds));
                put('0' + L);
                if (n == ilim)
                {
                    d = libgcc::dpadd(d, d);
                    if (libgcc::dpcmp(d, ds) > 0 || (libgcc::dpcmp(d, ds) == 0 && (L & 1) != 0))
                        return Ending::BumpUp;
                    break;
                }
                d = libgcc::dpmul(d, kTen);
                if (libgcc::dpcmp(d, kZero) == 0)
                    break;
            }
            return Ending::Done;
        };

        // The general case, in multiple precision: b / S gives the digits; mhi and mlo are the
        // margins within which a shorter string still reads back as d.
        const auto bigDigits = [&]() -> Ending {
            int32_t m2 = b2;
            int32_t m5 = b5;
            mhi = mlo = ee::Ptr<Bigint>();
            int32_t shift = 0;
            if (leftright)
            {
                if (mode < 2)
                    shift = denormal ? *be + (kBias + (kPrecisionBits - 1) - 1 + 1) : 1 + kPrecisionBits - *bbits;
                else
                {
                    int32_t fives = ilim - 1;
                    if (m5 >= fives)
                        m5 -= fives;
                    else
                    {
                        fives -= m5;
                        s5 += fives;
                        b5 += fives;
                        m5 = 0;
                    }
                    if ((shift = ilim) < 0)
                    {
                        m2 -= shift;
                        shift = 0;
                    }
                }
                b2 += shift;
                s2 += shift;
                mhi = _i2b(reent, 1);
            }
            if (m2 > 0 && s2 > 0)
            {
                shift = m2 < s2 ? m2 : s2;
                b2 -= shift;
                m2 -= shift;
                s2 -= shift;
            }
            if (b5 > 0)
            {
                if (leftright)
                {
                    if (m5 > 0)
                    {
                        mhi = _pow5mult(reent, mhi, m5);
                        const ee::Ptr<Bigint> b1 = _multiply(reent, mhi, b);
                        _Bfree(reent, b);
                        b = b1;
                    }
                    if (const int32_t rest = b5 - m5; rest != 0)
                        b = _pow5mult(reent, b, rest);
                }
                else
                    b = _pow5mult(reent, b, b5);
            }
            S = _i2b(reent, 1);
            if (s5 > 0)
                S = _pow5mult(reent, S, s5);

            // Is d a normalized power of 2? Then the margin below is half the one above. The source
            // sets spec_case in modes 0 and 1 only, and the left-right modes 4 and 5 read it too: there
            // it is what an earlier call left in its word of the frame (a bug the game keeps; its only
            // caller, cvt, asks modes 2 and 3).
            ee::Local<int32_t> specialCase;
            if (mode < 2)
            {
                *specialCase = 0;
                if (word1(d) == 0u && (word0(d) & kFractionHigh) == 0u && (word0(d) & kExponentMask) != 0u)
                {
                    b2 += kLog2P;
                    s2 += kLog2P;
                    *specialCase = 1;
                }
            }

            // Shift so the divisor has 4 leading zero bits: quorem's estimate needs it.
            shift = ((s5 != 0 ? 32 - _hi0bits(ee::Ptr<uint32_t>(S->x)[S->wordCount - 1]) : 1) + s2) & 0x1F;
            if (shift != 0)
                shift = 32 - shift;
            if (shift > 4)
            {
                shift -= 4;
                b2 += shift;
                m2 += shift;
                s2 += shift;
            }
            else if (shift < 4)
            {
                shift += 28;
                b2 += shift;
                m2 += shift;
                s2 += shift;
            }
            if (b2 > 0)
                b = _lshift(reent, b, b2);
            if (s2 > 0)
                S = _lshift(reent, S, s2);
            if (kCheck && __mcmp(b, S) < 0)
            {
                k--;
                b = _multadd(reent, b, 10, 0); // we botched the k estimate
                if (leftright)
                    mhi = _multadd(reent, mhi, 10, 0);
                ilim = ilim1;
            }
            if (ilim <= 0 && mode > 2)
            {
                if (ilim < 0)
                    return Ending::NoDigits;
                S = _multadd(reent, S, 5, 0);
                if (__mcmp(b, S) <= 0)
                    return Ending::NoDigits;
                return Ending::OneDigit;
            }

            int32_t dig = 0;
            const bool evenValue = (word1(d) & 1u) == 0u;
            if (leftright)
            {
                if (m2 > 0)
                    mhi = _lshift(reent, mhi, m2);
                mlo = mhi;
                if (*specialCase != 0)
                {
                    mhi = _Balloc(reent, mhi->k);
                    copyNumber(&mhi->sign, &mlo->sign, static_cast<uint32_t>(mlo->wordCount) * sizeof(int32_t) + 2u * sizeof(int32_t));
                    mhi = _lshift(reent, mhi, kLog2P);
                }
                for (int32_t n = 1;; n++)
                {
                    dig = quorem(b, S) + '0';
                    // Do we yet have the shortest decimal string that will round to d?
                    const int32_t belowLow = __mcmp(b, mlo);
                    const ee::Ptr<Bigint> delta = __mdiff(reent, S, mhi);
                    int32_t aboveHigh = delta->sign != 0 ? 1 : __mcmp(b, delta);
                    _Bfree(reent, delta);
                    if (aboveHigh == 0 && mode == 0 && evenValue)
                    {
                        if (dig == '9')
                            return Ending::RoundNineUp;
                        if (belowLow > 0)
                            dig++;
                        put(dig);
                        return Ending::Digits;
                    }
                    if (belowLow < 0 || (belowLow == 0 && mode == 0 && evenValue))
                    {
                        if (aboveHigh > 0)
                        {
                            b = _lshift(reent, b, 1);
                            aboveHigh = __mcmp(b, S);
                            if ((aboveHigh > 0 || (aboveHigh == 0 && (dig & 1) != 0)) && dig++ == '9')
                                return Ending::RoundNineUp;
                        }
                        put(dig);
                        return Ending::Digits;
                    }
                    if (aboveHigh > 0)
                    {
                        if (dig == '9') // possible if n == 1
                            return Ending::RoundNineUp;
                        put(dig + 1);
                        return Ending::Digits;
                    }
                    put(dig);
                    if (n == ilim)
                        break;
                    b = _multadd(reent, b, 10, 0);
                    if (mlo.get() == mhi.get())
                        mlo = mhi = _multadd(reent, mhi, 10, 0);
                    else
                    {
                        mlo = _multadd(reent, mlo, 10, 0);
                        mhi = _multadd(reent, mhi, 10, 0);
                    }
                }
            }
            else
            {
                for (int32_t n = 1;; n++)
                {
                    dig = quorem(b, S) + '0';
                    put(dig);
                    if (n >= ilim)
                        break;
                    b = _multadd(reent, b, 10, 0);
                }
            }

            // Round off the last digit.
            b = _lshift(reent, b, 1);
            const int32_t half = __mcmp(b, S);
            if (half > 0 || (half == 0 && (dig & 1) != 0))
                return Ending::RoundOff;
            do
                s -= 1;
            while (*s == '0');
            ++s;
            return Ending::Digits;
        };

        Ending ending = Ending::NotYet;
        if (ilim >= 0 && ilim <= kQuickMax && tryQuick)
        {
            const uint64_t savedD = d;
            const int32_t savedK = k, savedIlim = ilim;
            ending = quickDigits();
            if (ending == Ending::NotYet)
            {
                s = s0;
                d = savedD;
                k = savedK;
                ilim = savedIlim;
            }
        }
        if (ending == Ending::NotYet && *be >= 0 && k <= kIntMax)
            ending = smallIntegerDigits();
        if (ending == Ending::NotYet)
            ending = bigDigits();

        // The last digit up, carrying through nines; past the first, the number gains a digit.
        const auto bumpUp = [&]() {
            for (;;)
            {
                s -= 1;
                if (*s != '9')
                    break;
                if (s.get() == s0.get())
                {
                    k++;
                    *s = '0';
                    break;
                }
            }
            ++*s;
            ++s;
        };
        const auto roundOff = [&]() {
            for (;;)
            {
                s -= 1;
                if (*s != '9')
                {
                    ++*s;
                    ++s;
                    return;
                }
                if (s.get() == s0.get())
                {
                    k++;
                    put('1');
                    return;
                }
            }
        };
        const auto freeNumbers = [&]() {
            _Bfree(reent, S);
            if (mhi)
            {
                if (mlo && mlo.get() != mhi.get())
                    _Bfree(reent, mlo);
                _Bfree(reent, mhi);
            }
        };
        switch (ending)
        {
        case Ending::BumpUp:
            bumpUp();
            break;
        case Ending::NoDigits:
            k = -1 - ndigits;
            freeNumbers();
            break;
        case Ending::OneDigit:
            put('1');
            k++;
            freeNumbers();
            break;
        case Ending::RoundNineUp:
            put('9');
            roundOff();
            freeNumbers();
            break;
        case Ending::RoundOff:
            roundOff();
            freeNumbers();
            break;
        case Ending::Digits:
            freeNumbers();
            break;
        case Ending::Done:
        case Ending::NotYet:
            break;
        }
        _Bfree(reent, b);
        *s = '\0';
        *decpt = k + 1;
        if (rve)
            *rve = s;
        return s0;
    }
    READABLE_FUNCTION(_dtoa_r, Verified);
}
