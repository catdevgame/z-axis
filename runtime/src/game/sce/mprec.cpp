// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdlib/mprec.c and
// mprec.h carry the notice below.
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

// stdlib/mprec.c, newlib 1.8.2: David Gay's multiple precision integers, which strtod and the
// float printing use. The same code in DM2 and BMX XXX. A double travels as its 64-bit pattern
// (the EE has no double-precision hardware), with word 0 its high half.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libgcc.h"

#include <cstdint>
#include "sym/sce/mprec.cpp.h"

namespace sce::libc
{
    namespace
    {
        constexpr int kKmax = 15;                       // _Kmax: free lists of sizes up to 2 to the 15 words
        constexpr uint32_t kExponentMask = 0x7FF00000u; // Exp_mask
        constexpr uint32_t kExponentOne = 0x3FF00000u;  // Exp_1
        constexpr uint32_t kExponentUnit = 1u << 20;    // Exp_msk1
        constexpr uint32_t kFractionMask = 0xFFFFFu;    // Frac_mask
        constexpr int kExponentShift = 20;              // Exp_shift
        constexpr int kPrecisionBits = 53;              // P
        constexpr int kBias = 1023;                     // Bias
        constexpr int kExponentBits = 11;               // Ebits

        uint32_t word0(uint64_t d) { return static_cast<uint32_t>(d >> 32); }
        uint32_t word1(uint64_t d) { return static_cast<uint32_t>(d); }
        uint64_t makeDouble(uint32_t high, uint32_t low) { return static_cast<uint64_t>(high) << 32 | low; }

        ee::Ptr<uint32_t> wordsOf(ee::Ptr<Bigint> b) { return b->x; }

        // The words after a walk: the next word, moving on. A word at a time reads as the source's *x++.
        uint32_t next(ee::Ptr<uint32_t> &at)
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

        template <typename T>
        ee::Ptr<T> callocR(ee::Ptr<Reent> reent, uint32_t count, uint32_t bytes) { return ee::game<ee::Ptr<T>>(sym::_calloc_r, reent, count, bytes); }
        void memcpy(ee::Ptr<int32_t> to, ee::Ptr<int32_t> from, uint32_t bytes) { ee::game(sym::memcpy, to, from, bytes); }

        inline ee::Global<uint64_t[25], sym::g_tens> g_tens; // tens[]: 1e0 .. 1e24
        constexpr uint64_t kOne = 0x3FF0000000000000ull, kTen = 0x4024000000000000ull;
    }

    // Balloc: a number of 2 to the k words, from its free list or the heap. NULL if the list's
    // table cannot be had; a block that cannot be had is NULL too.
    ee::Ptr<Bigint> _Balloc(ee::Ptr<Reent> reent, int32_t k)
    {
        if (!reent->bigintFreeList)
        {
            reent->bigintFreeList = callocR<ee::Ptr<Bigint>>(reent, sizeof(uint32_t), kKmax + 1);
            if (!reent->bigintFreeList)
                return ee::Ptr<Bigint>();
        }
        ee::Ptr<Bigint> result = reent->bigintFreeList[k];
        if (result)
            reent->bigintFreeList[k] = result->next;
        else
        {
            const int32_t words = 1 << k;
            result = callocR<Bigint>(reent, 1, sizeof(Bigint) + (words - 1) * sizeof(uint32_t));
            if (!result)
                return ee::Ptr<Bigint>();
            result->k = k;
            result->maxWords = words;
        }
        result->wordCount = 0;
        result->sign = 0;
        return result;
    }
    READABLE_FUNCTION(_Balloc, Verified);

    // Bfree: back on the free list of its size.
    void _Bfree(ee::Ptr<Reent> reent, ee::Ptr<Bigint> v)
    {
        if (v)
        {
            v->next = reent->bigintFreeList[v->k];
            reent->bigintFreeList[v->k] = v;
        }
    }
    READABLE_FUNCTION(_Bfree, Verified);

    // multadd: b * m + a, in b if it fits.
    ee::Ptr<Bigint> _multadd(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t m, int32_t a)
    {
        int32_t wds = b->wordCount;
        ee::Ptr<uint32_t> x = wordsOf(b);
        int32_t i = 0;
        do
        {
            const uint32_t xi = *x;
            const uint32_t y = (xi & 0xFFFFu) * static_cast<uint32_t>(m) + static_cast<uint32_t>(a);
            const uint32_t z = (xi >> 16) * static_cast<uint32_t>(m) + (y >> 16);
            a = static_cast<int32_t>(z >> 16);
            *x = (z << 16) + (y & 0xFFFFu);
            ++x;
        } while (++i < wds);
        if (a != 0)
        {
            if (wds >= b->maxWords)
            {
                const ee::Ptr<Bigint> larger = _Balloc(reent, b->k + 1);
                // Bcopy: the sign, the count and the words.
                memcpy(&larger->sign, &b->sign, static_cast<uint32_t>(b->wordCount) * sizeof(int32_t) + 2u * sizeof(int32_t));
                _Bfree(reent, b);
                b = larger;
            }
            wordsOf(b)[wds] = static_cast<uint32_t>(a);
            b->wordCount = wds + 1;
        }
        return b;
    }
    READABLE_FUNCTION(_multadd, Verified);

    // s2b: the nd decimal digits at s (nd0 of them before the point) as a number, y9 being the
    // value of the first nine.
    ee::Ptr<Bigint> _s2b(ee::Ptr<Reent> reent, ee::Ptr<const char> s, int32_t nd0, int32_t nd, uint32_t y9)
    {
        const int32_t x = (nd + 8) / 9;
        int32_t k = 0;
        for (int32_t y = 1; x > y; y <<= 1)
            ++k;
        ee::Ptr<Bigint> b = _Balloc(reent, k);
        wordsOf(b)[0] = y9;
        b->wordCount = 1;
        int32_t i = 9;
        if (9 < nd0)
        {
            s += 9;
            do
            {
                b = _multadd(reent, b, 10, *s - '0');
                ++s;
            } while (++i < nd0);
            ++s;
        }
        else
            s += 10;
        for (; i < nd; ++i)
        {
            b = _multadd(reent, b, 10, *s - '0');
            ++s;
        }
        return b;
    }
    READABLE_FUNCTION(_s2b, Verified);

    // hi0bits: the zero bits above x's highest set bit; 32 for zero.
    int32_t _hi0bits(uint32_t x)
    {
        int32_t k = 0;
        if ((x & 0xFFFF0000u) == 0u)
        {
            k = 16;
            x <<= 16;
        }
        if ((x & 0xFF000000u) == 0u)
        {
            k += 8;
            x <<= 8;
        }
        if ((x & 0xF0000000u) == 0u)
        {
            k += 4;
            x <<= 4;
        }
        if ((x & 0xC0000000u) == 0u)
        {
            k += 2;
            x <<= 2;
        }
        if ((x & 0x80000000u) == 0u)
        {
            ++k;
            if ((x & 0x40000000u) == 0u)
                return 32;
        }
        return k;
    }
    READABLE_FUNCTION(_hi0bits, Verified);

    // lo0bits: shifts *y down past its low zero bits, and says how many; 32 for zero.
    int32_t _lo0bits(ee::Ptr<uint32_t> y)
    {
        uint32_t x = *y;
        if ((x & 7u) != 0u)
        {
            if ((x & 1u) != 0u)
                return 0;
            if ((x & 2u) != 0u)
            {
                *y = x >> 1;
                return 1;
            }
            *y = x >> 2;
            return 2;
        }
        int32_t k = 0;
        if ((x & 0xFFFFu) == 0u)
        {
            k = 16;
            x >>= 16;
        }
        if ((x & 0xFFu) == 0u)
        {
            k += 8;
            x >>= 8;
        }
        if ((x & 0xFu) == 0u)
        {
            k += 4;
            x >>= 4;
        }
        if ((x & 3u) == 0u)
        {
            k += 2;
            x >>= 2;
        }
        if ((x & 1u) == 0u)
        {
            ++k;
            x >>= 1;
            if (x == 0u)
                return 32;
        }
        *y = x;
        return k;
    }
    READABLE_FUNCTION(_lo0bits, Verified);

    // i2b: an int as a number.
    ee::Ptr<Bigint> _i2b(ee::Ptr<Reent> reent, int32_t value)
    {
        const ee::Ptr<Bigint> b = _Balloc(reent, 1);
        wordsOf(b)[0] = static_cast<uint32_t>(value);
        b->wordCount = 1;
        return b;
    }
    READABLE_FUNCTION(_i2b, Verified);

    // mult: a * b, schoolbook, a half word of b at a time.
    ee::Ptr<Bigint> _multiply(ee::Ptr<Reent> reent, ee::Ptr<Bigint> a, ee::Ptr<Bigint> b)
    {
        if (a->wordCount < b->wordCount)
        {
            const ee::Ptr<Bigint> swap = a;
            a = b;
            b = swap;
        }
        int32_t k = a->k;
        const int32_t wa = a->wordCount;
        const int32_t wb = b->wordCount;
        int32_t wc = wa + wb;
        if (wc > a->maxWords)
            ++k;
        const ee::Ptr<Bigint> c = _Balloc(reent, k);
        {
            const ee::Ptr<uint32_t> end = wordsOf(c) + wc;
            for (ee::Ptr<uint32_t> x = wordsOf(c); x.get() < end.get(); ++x)
                *x = 0u;
        }
        const ee::Ptr<uint32_t> xa = wordsOf(a);
        const ee::Ptr<uint32_t> xae = xa + wa;
        ee::Ptr<uint32_t> xb = wordsOf(b);
        const ee::Ptr<uint32_t> xbe = xb + wb;
        ee::Ptr<uint32_t> xc0 = wordsOf(c);
        for (; xb.get() < xbe.get(); ++xb, ++xc0)
        {
            uint32_t y = *xb & 0xFFFFu;
            if (y != 0u)
            {
                ee::Ptr<uint32_t> x = xa;
                ee::Ptr<uint32_t> xc = xc0;
                uint32_t carry = 0u;
                do
                {
                    const uint32_t z = (*x & 0xFFFFu) * y + (*xc & 0xFFFFu) + carry;
                    carry = z >> 16;
                    const uint32_t z2 = (next(x) >> 16) * y + (*xc >> 16) + carry;
                    carry = z2 >> 16;
                    storeHalves(xc, z2, z);
                } while (x.get() < xae.get());
                *xc = carry;
            }
            y = *xb >> 16;
            if (y != 0u)
            {
                ee::Ptr<uint32_t> x = xa;
                ee::Ptr<uint32_t> xc = xc0;
                uint32_t carry = 0u;
                uint32_t z2 = *xc;
                do
                {
                    const uint32_t z = (*x & 0xFFFFu) * y + (*xc >> 16) + carry;
                    carry = z >> 16;
                    storeHalves(xc, z, z2);
                    z2 = (next(x) >> 16) * y + (*xc & 0xFFFFu) + carry;
                    carry = z2 >> 16;
                } while (x.get() < xae.get());
                *xc = z2;
            }
        }
        ee::Ptr<uint32_t> xc = wordsOf(c) + wc;
        while (wc > 0)
        {
            xc -= 1;
            if (*xc != 0u)
                break;
            --wc;
        }
        c->wordCount = wc;
        return c;
    }
    READABLE_FUNCTION(_multiply, Verified);

    // pow5mult: b * 5 to the k, keeping the powers 625, 625 squared ... on the _reent.
    ee::Ptr<Bigint> _pow5mult(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t k)
    {
        static constexpr int32_t kSmallPowers[3] = {5, 25, 125};
        const int32_t i = k & 3;
        if (i != 0)
            b = _multadd(reent, b, kSmallPowers[i - 1], 0);
        k >>= 2;
        if (k == 0)
            return b;
        ee::Ptr<Bigint> p5 = reent->powersOf5;
        if (!p5)
        {
            p5 = _i2b(reent, 625);
            reent->powersOf5 = p5;
            p5->next = ee::Ptr<Bigint>();
        }
        for (;;)
        {
            if ((k & 1) != 0)
            {
                const ee::Ptr<Bigint> product = _multiply(reent, b, p5);
                _Bfree(reent, b);
                b = product;
            }
            k >>= 1;
            if (k == 0)
                break;
            ee::Ptr<Bigint> nextPower = p5->next;
            if (!nextPower)
            {
                nextPower = _multiply(reent, p5, p5);
                p5->next = nextPower;
                nextPower->next = ee::Ptr<Bigint>();
            }
            p5 = nextPower;
        }
        return b;
    }
    READABLE_FUNCTION(_pow5mult, Verified);

    // lshift: b << k, as a new number; b is freed.
    ee::Ptr<Bigint> _lshift(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t k)
    {
        const int32_t n = k >> 5;
        int32_t newK = b->k;
        int32_t n1 = n + b->wordCount + 1;
        for (int32_t i = b->maxWords; n1 > i; i <<= 1)
            ++newK;
        const ee::Ptr<Bigint> shifted = _Balloc(reent, newK);
        ee::Ptr<uint32_t> x1 = wordsOf(shifted);
        for (int32_t i = 0; i < n; ++i)
        {
            *x1 = 0u;
            ++x1;
        }
        ee::Ptr<uint32_t> x = wordsOf(b);
        const ee::Ptr<uint32_t> xe = x + b->wordCount;
        k &= 0x1F;
        if (k != 0)
        {
            const int32_t rest = 32 - k;
            uint32_t z = 0u;
            do
            {
                *x1 = *x << k | z;
                ++x1;
                z = next(x) >> rest;
            } while (x.get() < xe.get());
            *x1 = z;
            if (z != 0u)
                ++n1;
        }
        else
        {
            do
            {
                *x1 = next(x);
                ++x1;
            } while (x.get() < xe.get());
        }
        shifted->wordCount = n1 - 1;
        _Bfree(reent, b);
        return shifted;
    }
    READABLE_FUNCTION(_lshift, Verified);

    // cmp: below, at or above 0 as a is below, equal to or above b.
    int32_t __mcmp(ee::Ptr<Bigint> a, ee::Ptr<Bigint> b)
    {
        const int32_t j = b->wordCount;
        const int32_t difference = a->wordCount - j;
        if (difference != 0)
            return difference;
        const ee::Ptr<uint32_t> xa0 = wordsOf(a);
        ee::Ptr<uint32_t> xa = xa0 + j;
        ee::Ptr<uint32_t> xb = wordsOf(b) + j;
        for (;;)
        {
            xa -= 1;
            xb -= 1;
            if (*xa != *xb)
                return *xa < *xb ? -1 : 1;
            if (xa.get() <= xa0.get())
                break;
        }
        return 0;
    }
    READABLE_FUNCTION(__mcmp, Verified);

    // diff: |a - b|, its sign in the number's.
    ee::Ptr<Bigint> __mdiff(ee::Ptr<Reent> reent, ee::Ptr<Bigint> a, ee::Ptr<Bigint> b)
    {
        int32_t order = __mcmp(a, b);
        if (order == 0)
        {
            const ee::Ptr<Bigint> zero = _Balloc(reent, 0);
            zero->wordCount = 1;
            wordsOf(zero)[0] = 0u;
            return zero;
        }
        if (order < 0)
        {
            const ee::Ptr<Bigint> swap = a;
            a = b;
            b = swap;
            order = 1;
        }
        else
            order = 0;
        const ee::Ptr<Bigint> c = _Balloc(reent, a->k);
        c->sign = order;
        int32_t wa = a->wordCount;
        ee::Ptr<uint32_t> xa = wordsOf(a);
        const ee::Ptr<uint32_t> xae = xa + wa;
        const int32_t wb = b->wordCount;
        ee::Ptr<uint32_t> xb = wordsOf(b);
        const ee::Ptr<uint32_t> xbe = xb + wb;
        ee::Ptr<uint32_t> xc = wordsOf(c);
        int32_t borrow = 0;
        do
        {
            const int32_t y = static_cast<int32_t>((*xa & 0xFFFFu) - (*xb & 0xFFFFu)) + borrow;
            borrow = y >> 16;
            const int32_t z = static_cast<int32_t>((next(xa) >> 16) - (next(xb) >> 16)) + borrow;
            borrow = z >> 16;
            storeHalves(xc, static_cast<uint32_t>(z), static_cast<uint32_t>(y));
        } while (xb.get() < xbe.get());
        while (xa.get() < xae.get())
        {
            const int32_t y = static_cast<int32_t>(*xa & 0xFFFFu) + borrow;
            borrow = y >> 16;
            const int32_t z = static_cast<int32_t>(next(xa) >> 16) + borrow;
            borrow = z >> 16;
            storeHalves(xc, static_cast<uint32_t>(z), static_cast<uint32_t>(y));
        }
        for (;;)
        {
            xc -= 1;
            if (*xc != 0u)
                break;
            --wa;
        }
        c->wordCount = wa;
        return c;
    }
    READABLE_FUNCTION(__mdiff, Verified);

    // ulp: the value of the last bit of x.
    uint64_t _ulp(uint64_t x)
    {
        int32_t l = static_cast<int32_t>((word0(x) & kExponentMask) - (kPrecisionBits - 1) * kExponentUnit);
        if (l > 0)
            return makeDouble(static_cast<uint32_t>(l), 0u);
        l = -l >> kExponentShift;
        if (l < kExponentShift)
            return makeDouble(0x80000u >> l, 0u);
        l -= kExponentShift;
        return makeDouble(0u, l >= 31 ? 1u : 1u << (31 - l));
    }
    READABLE_FUNCTION(_ulp, Verified);

    // b2d: the number's top 53 bits as a double in [1, 2), and through e the exponent that goes with it.
    uint64_t _b2d(ee::Ptr<Bigint> a, ee::Ptr<int32_t> e)
    {
        const ee::Ptr<uint32_t> xa0 = wordsOf(a);
        ee::Ptr<uint32_t> xa = xa0 + a->wordCount;
        const auto previous = [&xa, xa0]() -> uint32_t {
            if (xa.get() <= xa0.get())
                return 0u;
            xa -= 1;
            return *xa;
        };
        xa -= 1;
        uint32_t y = *xa;
        int32_t k = _hi0bits(y);
        *e = 32 - k;
        if (k < kExponentBits)
        {
            const uint32_t high = kExponentOne | y >> (kExponentBits - k);
            const uint32_t w = previous();
            return makeDouble(high, y << (32 - kExponentBits + k) | w >> (kExponentBits - k));
        }
        const uint32_t z = previous();
        k -= kExponentBits;
        if (k != 0)
        {
            const uint32_t high = kExponentOne | y << k | z >> (32 - k);
            y = previous();
            return makeDouble(high, z << k | y >> (32 - k));
        }
        return makeDouble(kExponentOne | y, z);
    }
    READABLE_FUNCTION(_b2d, Verified);

    // d2b: a double as a whole number; through e and bits its exponent and how many bits it has.
    ee::Ptr<Bigint> _d2b(ee::Ptr<Reent> reent, uint64_t d, ee::Ptr<int32_t> e, ee::Ptr<int32_t> bits)
    {
        const ee::Ptr<Bigint> b = _Balloc(reent, 1);
        const ee::Ptr<uint32_t> x = wordsOf(b);
        ee::Local<uint32_t> y;
        ee::Local<uint32_t> z;
        *z = word0(d) & kFractionMask;
        const int32_t de = static_cast<int32_t>((word0(d) & 0x7FFFFFFFu) >> kExponentShift);
        if (de != 0)
            *z |= kExponentUnit;
        *y = word1(d);
        int32_t k;
        int32_t i;
        if (*y != 0u)
        {
            k = _lo0bits(y);
            if (k != 0)
            {
                x[0] = *y | *z << (32 - k);
                *z >>= k;
            }
            else
                x[0] = *y;
            x[1] = *z;
            i = x[1] != 0u ? 2 : 1;
            b->wordCount = i;
        }
        else
        {
            k = _lo0bits(z);
            x[0] = *z;
            i = 1;
            b->wordCount = 1;
            k += 32;
        }
        if (de != 0)
        {
            *e = de - kBias - (kPrecisionBits - 1) + k;
            *bits = kPrecisionBits - k;
        }
        else
        {
            *e = de - kBias - (kPrecisionBits - 1) + 1 + k;
            *bits = 32 * i - _hi0bits(x[i - 1]);
        }
        return b;
    }
    READABLE_FUNCTION(_d2b, Verified);

    // ratio: a / b as a double.
    uint64_t _ratio(ee::Ptr<Bigint> a, ee::Ptr<Bigint> b)
    {
        ee::Local<int32_t> ka;
        ee::Local<int32_t> kb;
        uint64_t da = _b2d(a, ka);
        uint64_t db = _b2d(b, kb);
        int32_t k = *ka - *kb + 32 * (a->wordCount - b->wordCount);
        if (k > 0)
            da += static_cast<uint64_t>(static_cast<uint32_t>(k) * kExponentUnit) << 32;
        else
        {
            k = -k;
            db += static_cast<uint64_t>(static_cast<uint32_t>(k) * kExponentUnit) << 32;
        }
        return libgcc::dpdiv(da, db);
    }
    READABLE_FUNCTION(_ratio, Verified);

    // mprec_log10: 10 to the dig as a double: from the table up to 1e23, else multiplied out.
    uint64_t _mprec_log10(int32_t dig)
    {
        uint64_t v = kOne;
        if (dig < 24)
            return (*g_tens)[dig];
        while (dig > 0)
        {
            v = libgcc::dpmul(v, kTen);
            dig--;
        }
        return v;
    }
    READABLE_FUNCTION(_mprec_log10, Draft);
}
