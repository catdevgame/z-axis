// Licence: ported from newlib 1.8.2's libm (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz), Sun's fdlibm:
// its math/ and common/ files carry the notice below.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunPro, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================

// libm: the single-precision functions of newlib 1.8.2's libm (Sun's fdlibm,
// libm/math/sf_*.c, ef_*.c, kf_*.c, wf_*.c and libm/common/sf_*.c), as the
// game links them (atanf-sceGsResetGraph, the same code in DM2 and BMX XXX),
// and the two double ones the float printing asks (math/s_isinf.c, s_isnan.c).
//
// Version pinned against the candidate releases (docs/READABLE.md, round
// 15): acosf and asinf set exc.arg1 = exc.arg2 = (double)x (added in 1.8.2:
// the fptodp calls), and __ieee754_asinf keeps the dangling else before its
// |x| < 2**-27 test that 1.9.0 braced (t is left unset on that path).
//
// Ported from that source: the names, structure and constants are newlib's;
// the order of each float operation is the compiled code's (gcc keeps the
// source's order except where it turned 2*x into x+x and folded constant
// sums), so every result is bit-identical under the EE's rounding (ee::Float).
// Constants are the source's (each one's bits in the comment); sums gcc
// folded at compile time (to nearest) are written as their folded bits,
// since adding them at run time on the EE (towards zero) can differ.
//

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libc.h"
#include "sce/libgcc.h"
#include "sce/libm.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/sce/libm.cpp.h"

namespace sce::libm
{
    // A float's mantissa, and the bit above it.
    constexpr int32_t kMantissaMask = (1 << 23) - 1, kImplicitBit = 1 << 23;

    namespace
    {
        using ee::Float;

        // _LIB_VERSION (common/s_lib_ver.c): which error convention the
        // wrappers follow; this build's is _POSIX_.
        enum LibVersion : int32_t
        {
            kIeee = -1, // _IEEE_: the wrappers return the kernel's result as it is
            kSvid,
            kXopen,
            kPosix, // _POSIX_: errno only, matherr is not asked
            kIsoc,
        };
        inline ee::Global<int32_t, sym::g_libVersion> g_libVersion; // _LIB_VERSION

        constexpr int32_t kEdom = 33; // errno EDOM
        constexpr int32_t kDomain = 1; // struct exception type DOMAIN

        // The wrappers' names for matherr (exc.name), in the game's .rodata.
        constexpr uint32_t kNameAcosf = sym::nameAcosf;  // "acosf"
        constexpr uint32_t kNameAsinf = sym::nameAsinf;  // "asinf"
        constexpr uint32_t kNameAtan2f = sym::nameAtan2f; // "atan2f"

        // struct exception (newlib's math.h), on the wrapper's stack; the
        // doubles are libgcc's soft-float bit patterns.
        struct Exception
        {
            int32_t type;
            uint32_t name;
            uint64_t arg1;
            uint64_t arg2;
            uint64_t retval;
            int32_t err;
        };
        static_assert(offsetof(Exception, arg1) == 0x08 && offsetof(Exception, retval) == 0x18 &&
                      offsetof(Exception, err) == 0x20, "struct exception as newlib lays it out");

        int32_t floatBits(Float x) { return static_cast<int32_t>(x.bits()); }
        Float floatFromBits(int32_t bits) { return Float::fromBits(static_cast<uint32_t>(bits)); }

        constexpr float one = 1.0f;
        constexpr float half = 0.5f;
        constexpr float huge = 1.0e30f;
        constexpr float tiny = 1.0e-30f;

        // ef_acos.c and ef_asin.c share these.
        constexpr float pi = 3.1415925026e+00f;      // 0x40490fda
        constexpr float pio2_hi = 1.5707962513e+00f; // 0x3fc90fda
        constexpr float pio2_lo = 7.5497894159e-08f; // 0x33a22168
        constexpr float pio4_hi = 7.8539818525e-01f; // 0x3f490fdb
        constexpr float pS0 = 1.6666667163e-01f;     // 0x3e2aaaab
        constexpr float pS1 = -3.2556581497e-01f;    // 0xbea6b090
        constexpr float pS2 = 2.0121252537e-01f;     // 0x3e4e0aa8
        constexpr float pS3 = -4.0055535734e-02f;    // 0xbd241146
        constexpr float pS4 = 7.9153501429e-04f;     // 0x3a4f7f04
        constexpr float pS5 = 3.4793309169e-05f;     // 0x3811ef08
        constexpr float qS1 = -2.4033949375e+00f;    // 0xc019d139
        constexpr float qS2 = 2.0209457874e+00f;     // 0x4001572d
        constexpr float qS3 = -6.8828397989e-01f;    // 0xbf303361
        constexpr float qS4 = 7.7038154006e-02f;     // 0x3d9dc62e

        // The rational approximation of asin(sqrt(z))/sqrt(z) - 1 both use:
        // p(z) / q(z), as gcc evaluates the source's nested sums.
        Float asinP(Float z) { return z * (z * (z * (z * (z * (z * Float(pS5) + Float(pS4)) + Float(pS3)) + Float(pS2)) + Float(pS1)) + Float(pS0)); }
        Float asinQ(Float z) { return z * (z * (z * (z * Float(qS4) + Float(qS3)) + Float(qS2)) + Float(qS1)) + Float(one); }

        // The wrappers' error path (wf_acos.c, wf_asin.c, wf_atan2.c, with
        // _LIB_VERSION not _IEEE_): fill a struct exception for a DOMAIN
        // error, let matherr see it unless _POSIX_, set errno, and return
        // exc.retval (0.0) as a float.
        Float domainError(int32_t version, uint32_t name, uint64_t arg1, uint64_t arg2);
    }

    // Declared ahead: their calls come before them.
    Float fabsf(Float x);
    int32_t isnanf(Float x);
    Float copysignf(Float x, Float y);
    int32_t matherr(ee::Address exception);
    Float atanf(Float x);
    Float __ieee754_acosf(Float x);
    Float __ieee754_asinf(Float x);
    Float __ieee754_atan2f(Float y, Float x);
    Float __ieee754_sqrtf(Float x);
    Float __kernel_cosf(Float x, Float y);
    Float __kernel_sinf(Float x, Float y, int32_t iy);
    Float __kernel_tanf(Float x, Float y, int32_t iy);
    int32_t __ieee754_rem_pio2f(Float x, ee::Address y);

    namespace
    {

        Float domainError(int32_t version, uint32_t name, uint64_t arg1, uint64_t arg2)
        {
            ee::Local<Exception> exc;
            exc->type = kDomain;
            exc->name = name;
            exc->err = 0;
            exc->arg1 = arg1;
            exc->arg2 = arg2;
            exc->retval = 0; // 0.0
            if (version == kPosix || !matherr(exc))
                *ee::Ptr<int32_t>(libc::__errno()) = kEdom;
            if (exc->err != 0)
                *ee::Ptr<int32_t>(libc::__errno()) = exc->err;
            return libgcc::dptofp(exc->retval);
        }
    }

    // atanf (sf_atan.c): argument reduction to |x| < 7/16 around atan(0.5),
    // atan(1), atan(1.5) or atan(inf), then an odd polynomial in x.
    Float atanf(Float x)
    {
        static constexpr float atanhi[] = {
            4.6364760399e-01f, // atan(0.5)hi 0x3eed6338
            7.8539812565e-01f, // atan(1.0)hi 0x3f490fda
            9.8279368877e-01f, // atan(1.5)hi 0x3f7b985e
            1.5707962513e+00f, // atan(inf)hi 0x3fc90fda
        };
        static constexpr float atanlo[] = {
            5.0121582440e-09f, // atan(0.5)lo 0x31ac3769
            3.7748947079e-08f, // atan(1.0)lo 0x33222168
            3.4473217170e-08f, // atan(1.5)lo 0x33140fb4
            7.5497894159e-08f, // atan(inf)lo 0x33a22168
        };
        static constexpr float aT[] = {
            3.3333334327e-01f,  // 0x3eaaaaab (the source's comment says ...aa; its decimal is ...ab)
            -2.0000000298e-01f, // 0xbe4ccccd
            1.4285714924e-01f,  // 0x3e124925
            -1.1111110449e-01f, // 0xbde38e38
            9.0908870101e-02f,  // 0x3dba2e6e
            -7.6918758452e-02f, // 0xbd9d8795
            6.6610731184e-02f,  // 0x3d886b35
            -5.8335702866e-02f, // 0xbd6ef16b
            4.9768779427e-02f,  // 0x3d4bda59
            -3.6531571299e-02f, // 0xbd15a221
            1.6285819933e-02f,  // 0x3c8569d7
        };

        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        int id;
        if (ix >= 0x50800000) // |x| >= 2^34
        {
            if (ix > 0x7F800000)
                return x + x; // NaN
            if (hx > 0)
                return Float(atanhi[3]) + Float(atanlo[3]);
            return -Float(atanhi[3]) - Float(atanlo[3]);
        }
        if (ix < 0x3EE00000) // |x| < 0.4375
        {
            if (ix < 0x31000000 && Float(huge) + x > Float(one)) // |x| < 2^-29: x, raising inexact
                return x;
            id = -1;
        }
        else
        {
            x = fabsf(x);
            if (ix < 0x3F980000) // |x| < 1.1875
            {
                if (ix < 0x3F300000) // 7/16 <= |x| < 11/16
                {
                    id = 0;
                    x = (x + x - Float(one)) / (x + Float(2.0f));
                }
                else // 11/16 <= |x| < 19/16
                {
                    id = 1;
                    x = (x - Float(one)) / (x + Float(one));
                }
            }
            else if (ix < 0x401C0000) // |x| < 2.4375
            {
                id = 2;
                x = (x - Float(1.5f)) / (x * Float(1.5f) + Float(one));
            }
            else // 2.4375 <= |x| < 2^66
            {
                id = 3;
                x = Float(-1.0f) / x;
            }
        }
        // end of argument reduction
        const Float z = x * x;
        const Float w = z * z;
        // the sum from i = 0 to 10 of aT[i] z**(i+1) as odd and even polynomials
        const Float s2 = w * (Float(aT[1]) + w * (Float(aT[3]) + w * (Float(aT[5]) + w * (Float(aT[7]) + w * Float(aT[9])))));
        const Float s1 = z * (Float(aT[0]) + w * (Float(aT[2]) + w * (Float(aT[4]) + w * (Float(aT[6]) + w * (Float(aT[8]) + w * Float(aT[10]))))));
        if (id < 0)
            return x - x * (s1 + s2);
        const Float r = Float(atanhi[id]) - (x * (s1 + s2) - Float(atanlo[id]) - x);
        return hx < 0 ? -r : r;
    }
    READABLE_FUNCTION(atanf, Verified);

    // ceilf (sf_ceil.c): clears the fraction bits, rounding up first.
    Float ceilf(Float x)
    {
        int32_t i0 = floatBits(x);
        const int32_t j0 = ((i0 >> 23) & 0xFF) - 0x7F;
        if (j0 >= 23)
        {
            if (j0 == 0x80)
                return x + x; // inf or NaN
            return x;
        }
        if (j0 < 0) // |x| < 1: 0 with x's sign, or 1 (raising inexact if x != 0)
        {
            if (Float(huge) + x > Float(0.0f))
            {
                if (i0 < 0)
                    i0 = static_cast<int32_t>(0x80000000u);
                else if (i0 != 0)
                    i0 = 0x3F800000;
            }
        }
        else
        {
            const int32_t i = kMantissaMask >> j0;
            if ((i0 & i) == 0)
                return x; // x is integral
            if (Float(huge) + x > Float(0.0f)) // raise inexact
            {
                if (i0 > 0)
                    i0 += kImplicitBit >> j0;
                i0 &= ~i;
            }
        }
        return floatFromBits(i0);
    }
    READABLE_FUNCTION(ceilf, Verified);

    // cosf (sf_cos.c): the kernel on [-pi/4, pi/4] after reducing x by
    // multiples of pi/2.
    Float cosf(Float x)
    {
        ee::Local<Float> y;
        const int32_t ix = floatBits(x) & 0x7FFFFFFF;
        auto leave = [](Float r)
        {
            return r;
        };
        if (ix <= 0x3F490FD8) // |x| ~< pi/4
            return leave(__kernel_cosf(x, Float(0.0f)));
        if (ix >= 0x7F800000) // cos(Inf or NaN) is NaN
            return leave(x - x);
        const int32_t n = __ieee754_rem_pio2f(x, y);
        switch (n & 3)
        {
        case 0:
            return leave(__kernel_cosf((&*y)[0], (&*y)[1]));
        case 1:
            return leave(-__kernel_sinf((&*y)[0], (&*y)[1], 1));
        case 2:
            return leave(-__kernel_cosf((&*y)[0], (&*y)[1]));
        default:
        {
            const Float r = __kernel_sinf((&*y)[0], (&*y)[1], 1);
            return r;
        }
        }
    }
    READABLE_FUNCTION(cosf, Verified);

    // fabsf (sf_fabs.c): clears the sign bit.
    Float fabsf(Float x)
    {
        return floatFromBits(floatBits(x) & 0x7FFFFFFF);
    }
    READABLE_FUNCTION(fabsf, Verified);

    // floorf (sf_floor.c): clears the fraction bits, rounding down first.
    Float floorf(Float x)
    {
        int32_t i0 = floatBits(x);
        const int32_t j0 = ((i0 >> 23) & 0xFF) - 0x7F;
        if (j0 >= 23)
        {
            if (j0 == 0x80)
                return x + x; // inf or NaN
            return x;
        }
        if (j0 < 0) // |x| < 1: 0 with x's sign, or -1 (raising inexact if x != 0)
        {
            if (Float(huge) + x > Float(0.0f))
            {
                if (i0 >= 0)
                    i0 = 0;
                else if ((i0 & 0x7FFFFFFF) != 0)
                    i0 = static_cast<int32_t>(0xBF800000u);
            }
        }
        else
        {
            const int32_t i = kMantissaMask >> j0;
            if ((i0 & i) == 0)
                return x; // x is integral
            if (Float(huge) + x > Float(0.0f)) // raise inexact
            {
                if (i0 < 0)
                    i0 += kImplicitBit >> j0;
                i0 &= ~i;
            }
        }
        return floatFromBits(i0);
    }
    READABLE_FUNCTION(floorf, Verified);

    // sinf (sf_sin.c): as cosf .
    Float sinf(Float x)
    {
        ee::Local<Float> y;
        const int32_t ix = floatBits(x) & 0x7FFFFFFF;
        auto leave = [](Float r)
        {
            return r;
        };
        if (ix <= 0x3F490FD8) // |x| ~< pi/4
            return leave(__kernel_sinf(x, Float(0.0f), 0));
        if (ix >= 0x7F800000) // sin(Inf or NaN) is NaN
            return leave(x - x);
        const int32_t n = __ieee754_rem_pio2f(x, y);
        switch (n & 3)
        {
        case 0:
            return leave(__kernel_sinf((&*y)[0], (&*y)[1], 1));
        case 1:
            return leave(__kernel_cosf((&*y)[0], (&*y)[1]));
        case 2:
            return leave(-__kernel_sinf((&*y)[0], (&*y)[1], 1));
        default:
            return leave(-__kernel_cosf((&*y)[0], (&*y)[1]));
        }
    }
    READABLE_FUNCTION(sinf, Verified);

    // tanf (sf_tan.c): the kernel with iy = 1 (tan) or -1 (-1/tan) by the
    // quadrant.
    Float tanf(Float x)
    {
        ee::Local<Float> y;
        const int32_t ix = floatBits(x) & 0x7FFFFFFF;
        if (ix <= 0x3F490FDA) // |x| ~< pi/4
        {
            const Float r = __kernel_tanf(x, Float(0.0f), 1);
            return r;
        }
        if (ix >= 0x7F800000) // tan(Inf or NaN) is NaN
            return x - x;
        const int32_t n = __ieee754_rem_pio2f(x, y);
        const Float r = __kernel_tanf((&*y)[0], (&*y)[1], 1 - ((n & 1) << 1)); // 1 even, -1 odd
        return r;
    }
    READABLE_FUNCTION(tanf, Verified);

    // acosf (wf_acos.c): the wrapper; |x| > 1 is a domain error.
    Float acosf(Float x)
    {
        const Float z = __ieee754_acosf(x);
        const int32_t version = *g_libVersion;
        if (version == kIeee || isnanf(x))
            return z;
        if (fabsf(x) > Float(one))
        {
            const uint64_t arg = libgcc::fptodp(x); // exc.arg1 = exc.arg2 = (double)x
            return domainError(version, kNameAcosf, arg, arg);
        }
        return z;
    }
    READABLE_FUNCTION(acosf, Verified);

    // asinf (wf_asin.c): the wrapper; |x| > 1 is a domain error.
    Float asinf(Float x)
    {
        const Float z = __ieee754_asinf(x);
        const int32_t version = *g_libVersion;
        if (version == kIeee || isnanf(x))
            return z;
        if (fabsf(x) > Float(one))
        {
            const uint64_t arg = libgcc::fptodp(x); // exc.arg1 = exc.arg2 = (double)x
            return domainError(version, kNameAsinf, arg, arg);
        }
        return z;
    }
    READABLE_FUNCTION(asinf, Verified);

    // atan2f (wf_atan2.c): the wrapper; atan2f(+-0, +-0) is a domain error.
    Float atan2f(Float y, Float x)
    {
        const Float z = __ieee754_atan2f(y, x);
        const int32_t version = *g_libVersion;
        if (version == kIeee || isnanf(x) || isnanf(y))
            return z;
        if (x == Float(0.0f) && y == Float(0.0f))
        {
            const uint64_t arg1 = libgcc::fptodp(y); // exc.arg1 = y
            const uint64_t arg2 = libgcc::fptodp(x); // exc.arg2 = x
            return domainError(version, kNameAtan2f, arg1, arg2);
        }
        return z;
    }
    READABLE_FUNCTION(atan2f, Verified);

    // __ieee754_acosf (ef_acos.c):
    //   |x| < 0.5:  acos(x) = pi/2 - (x + x*r(x*x)), r the asin approximation
    //   x < -0.5:   acos(x) = pi - 2 asin(sqrt((1+x)/2))
    //   x > 0.5:    acos(x) = 2 asin(sqrt((1-x)/2)), sqrt split as df + c
    Float __ieee754_acosf(Float x)
    {
        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        if (ix == 0x3F800000) // |x| == 1
        {
            if (hx > 0)
                return Float(0.0f); // acos(1) = 0
            return floatFromBits(0x40490FDB); // acos(-1) = pi + 2*pio2_lo (folded)
        }
        if (ix > 0x3F800000) // |x| > 1: NaN
            return (x - x) / (x - x);
        if (ix < 0x3F000000) // |x| < 0.5
        {
            if (ix <= 0x23000000) // |x| < 2**-57
                return floatFromBits(0x3FC90FDB); // pio2_hi + pio2_lo (folded)
            const Float z = x * x;
            const Float r = asinP(z) / asinQ(z);
            return Float(pio2_hi) - (x - (Float(pio2_lo) - x * r));
        }
        if (hx < 0) // x < -0.5
        {
            const Float z = (x + Float(one)) * Float(half);
            const Float q = asinQ(z);
            const Float p = asinP(z);
            const Float s = __ieee754_sqrtf(z);
            const Float r = p / q;
            const Float w = r * s - Float(pio2_lo);
            return Float(pi) - (s + w + (s + w)); // pi - 2*(s + w)
        }
        // x > 0.5
        const Float z = (Float(one) - x) * Float(half);
        const Float s = __ieee754_sqrtf(z);
        const Float df = floatFromBits(floatBits(s) & static_cast<int32_t>(0xFFFFF000u));
        const Float c = (z - df * df) / (s + df);
        const Float r = asinP(z) / asinQ(z);
        const Float w = r * s + c;
        return df + w + (df + w); // 2*(df + w)
    }
    READABLE_FUNCTION(__ieee754_acosf, Verified);

    // __ieee754_asinf (ef_asin.c):
    //   |x| < 0.5:  asin(x) = x + x*r(x*x)
    //   |x| >= 0.5: asin(x) = pi/2 - 2 asin(sqrt((1-|x|)/2)), near 1 directly, else with the
    //               square root split in two for the precision
    Float __ieee754_asinf(Float x)
    {
        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        if (ix == 0x3F800000) // asin(+-1) = +-pi/2, raising inexact
            return x * Float(pio2_hi) + x * Float(pio2_lo);
        if (ix > 0x3F800000) // |x| > 1: NaN
            return (x - x) / (x - x);
        if (ix < 0x3F000000) // |x| < 0.5
        {
            // 1.8.2's dangling else: below 2**-27 t stays unset when huge + x > one fails
            // (never: that returns x). The compiled code would then use whatever value the
            // caller left where it keeps t; here it is 0.
            Float t = Float(0.0f);
            if (ix < 0x32000000) // |x| < 2**-27
            {
                if (Float(huge) + x > Float(one))
                    return x; // x, raising inexact if x != 0
            }
            else
                t = x * x;
            const Float w = asinP(t) / asinQ(t);
            return x + x * w;
        }
        // 1 > |x| >= 0.5
        const Float t = (Float(one) - fabsf(x)) * Float(half);
        const Float q = asinQ(t);
        const Float p = asinP(t);
        const Float s = __ieee754_sqrtf(t);
        Float result;
        if (ix >= 0x3F79999A) // |x| > 0.975
        {
            const Float w = s + s * (p / q);
            result = Float(pio2_hi) - (w + w - Float(pio2_lo)); // pio2_hi - (2*(s + s*w) - pio2_lo)
        }
        else
        {
            const Float w = floatFromBits(floatBits(s) & static_cast<int32_t>(0xFFFFF000u));
            const Float c = (t - w * w) / (s + w);
            const Float r = p / q;
            const Float pp = (s + s) * r - (Float(pio2_lo) - (c + c)); // 2*s*r - (pio2_lo - 2*c)
            const Float qq = Float(pio4_hi) - (w + w);                  // pio4_hi - 2*w
            result = Float(pio4_hi) - (pp - qq);
        }
        return hx > 0 ? result : -result;
    }
    READABLE_FUNCTION(__ieee754_asinf, Verified);

    // __ieee754_atan2f (ef_atan2.c): atan(y/x) put in the quadrant of (x, y), with the
    // zeros and infinities by case.
    Float __ieee754_atan2f(Float y, Float x)
    {
        constexpr float pi_o_4 = 7.8539818525e-01f; // 0x3f490fdb
        constexpr float pi_o_2 = 1.5707963705e+00f; // 0x3fc90fdb
        constexpr float pi_lo = 1.5099578832e-07f;  // 0x34222168
        // Sums with tiny gcc folded (to nearest), as in the source's returns.
        const Float piPlusTiny = Float(pi);                          // pi + tiny
        const Float piO2PlusTiny = Float(pi_o_2);                    // pi_o_2 + tiny
        const Float piO4PlusTiny = Float(pi_o_4);                    // pi_o_4 + tiny
        const Float threePiO4PlusTiny = floatFromBits(0x4016CBE4);   // 3*pi_o_4 + tiny
        const Float zero = Float(0.0f);
        const Float minusZero = Float(-0.0f);                        // -zero

        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        const int32_t hy = floatBits(y);
        const int32_t iy = hy & 0x7FFFFFFF;
        if (ix > 0x7F800000 || iy > 0x7F800000) // x or y is NaN
            return x + y;
        if (hx == 0x3F800000) // x = 1.0
            return atanf(y);
        const int m = ((hy >> 31) & 1) | ((hx >> 30) & 2); // 2*sign(x) + sign(y)

        if (iy == 0) // y = 0
        {
            switch (m)
            {
            case 0:
            case 1: return y;                  // atan(+-0, +anything) = +-0
            case 2: return piPlusTiny;         // atan(+0, -anything) = pi
            case 3: return -piPlusTiny;        // atan(-0, -anything) = -pi
            }
        }
        const auto yInfinite = [&]
        {
            return hy < 0 ? -piO2PlusTiny : piO2PlusTiny;
        };
        if (ix == 0) // x = 0
            return yInfinite();
        if (ix == 0x7F800000) // x is INF
        {
            if (iy == 0x7F800000)
            {
                switch (m)
                {
                case 0: return piO4PlusTiny;        // atan(+INF, +INF)
                case 1: return -piO4PlusTiny;       // atan(-INF, +INF)
                case 2: return threePiO4PlusTiny;   // atan(+INF, -INF)
                case 3: return -threePiO4PlusTiny;  // atan(-INF, -INF)
                }
            }
            else
            {
                switch (m)
                {
                case 0: return zero;           // atan(+..., +INF)
                case 1: return minusZero;      // atan(-..., +INF)
                case 2: return piPlusTiny;     // atan(+..., -INF)
                case 3: return -piPlusTiny;    // atan(-..., -INF)
                }
            }
        }
        if (iy == 0x7F800000) // y is INF: the compiled code branches back to the x = 0 return
            return yInfinite();

        // y/x
        const int32_t k = (iy - ix) >> 23;
        Float z;
        if (k > 60) // |y/x| > 2**60
            z = floatFromBits(0x3FC90FDC); // pi_o_2 + 0.5*pi_lo (folded)
        else if (hx < 0 && k < -60) // |y|/x < -2**60
            z = Float(0.0f);
        else // safe to do y/x
        {
            const Float ratio = fabsf(y / x);
            z = atanf(ratio);
        }
        switch (m)
        {
        case 0: return z;                                         // atan(+, +)
        case 1: return floatFromBits(floatBits(z) ^ static_cast<int32_t>(0x80000000u)); // atan(-, +)
        case 2: return Float(pi) - (z - Float(pi_lo));            // atan(+, -)
        default: return z - Float(pi_lo) - Float(pi);             // atan(-, -)
        }
    }
    READABLE_FUNCTION(__ieee754_atan2f, Verified);

    int32_t __kernel_rem_pio2f(ee::Ptr<Float> x, ee::Ptr<Float> y, int32_t e0, int32_t nx, int32_t prec, ee::Ptr<int32_t> ipio2); // below

    // __ieee754_rem_pio2f (ef_rem_pio2.c): x - n*pi/2 as y[0] + y[1] (y at the address
    // given), returning n: by one term near pi/2, by up to three 17-bit pieces of pi/2
    // up to 2**7*pi/2, else by __kernel_rem_pio2f on 24-bit chunks of x.
    int32_t __ieee754_rem_pio2f(Float x, ee::Address yAddress)
    {
        // The table of the multiples of pi/2 with the last 8 bits cleared, for the quick
        // check of cancellation.
        static constexpr int32_t npio2_hw[] = {
            0x3fc90f00, 0x40490f00, 0x4096cb00, 0x40c90f00, 0x40fb5300, 0x4116cb00,
            0x412fed00, 0x41490f00, 0x41623100, 0x417b5300, 0x418a3a00, 0x4196cb00,
            0x41a35c00, 0x41afed00, 0x41bc7e00, 0x41c90f00, 0x41d5a000, 0x41e23100,
            0x41eec200, 0x41fb5300, 0x4203f200, 0x420a3a00, 0x42108300, 0x4216cb00,
            0x421d1400, 0x42235c00, 0x4229a500, 0x422fed00, 0x42363600, 0x423c7e00,
            0x4242c700, 0x42490f00,
        };
        constexpr uint32_t two_over_pi = sym::two_over_pi; // two_over_pi[], 24 bits of 2/pi per word, in .rodata
        constexpr float two8 = 2.5600000000e+02f;     // 0x43800000
        constexpr float invpio2 = 6.3661980629e-01f;  // 0x3f22f984: 24 bits of 2/pi
        constexpr float pio2_1 = 1.5707855225e+00f;   // 0x3fc90f80: first 17 bits of pi/2
        constexpr float pio2_1t = 1.0804334124e-05f;  // 0x37354443: pi/2 - pio2_1
        constexpr float pio2_2 = 1.0804273188e-05f;   // 0x37354400: second 17 bits of pi/2
        constexpr float pio2_2t = 6.0770999344e-11f;  // 0x2e85a308: pi/2 - (pio2_1 + pio2_2)
        constexpr float pio2_3 = 6.0770943833e-11f;   // 0x2e85a300: third 17 bits of pi/2
        constexpr float pio2_3t = 6.1232342629e-17f;  // 0x248d3132: pi/2 - (pio2_1 + pio2_2 + pio2_3)

        ee::Local<Float> tx;
        Float *const y = ee::Ptr<Float>(yAddress).get();
        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        const auto leave = [](int32_t value) {
            return value;
        };
        // y[] negated when x is, and -n, or n as it is.
        const auto signedResult = [&](int32_t value) {
            if (hx < 0)
            {
                y[0] = -y[0];
                y[1] = -y[1];
                return leave(-value);
            }
            return leave(value);
        };
        if (ix <= 0x3F490FD8) // |x| ~<= pi/4, no reduction
        {
            y[0] = x;
            y[1] = Float(0.0f);
            return leave(0);
        }
        if (ix < 0x4016CBE4) // |x| < 3pi/4: n = +-1
        {
            if (hx > 0)
            {
                Float z = x - Float(pio2_1);
                if ((ix & 0xFFFFFFF0) != 0x3FC90FD0) // 24+24 bit pi is enough
                {
                    y[0] = z - Float(pio2_1t);
                    y[1] = z - y[0] - Float(pio2_1t);
                }
                else // near pi/2: 24+24+24 bit pi
                {
                    z -= Float(pio2_2);
                    y[0] = z - Float(pio2_2t);
                    y[1] = z - y[0] - Float(pio2_2t);
                }
                return leave(1);
            }
            Float z = x + Float(pio2_1);
            if ((ix & 0xFFFFFFF0) != 0x3FC90FD0)
            {
                y[0] = z + Float(pio2_1t);
                y[1] = z - y[0] + Float(pio2_1t);
            }
            else
            {
                z += Float(pio2_2);
                y[0] = z + Float(pio2_2t);
                y[1] = z - y[0] + Float(pio2_2t);
            }
            return leave(-1);
        }
        if (ix <= 0x43490F80) // |x| ~<= 2^7*(pi/2), medium size
        {
            const Float t = fabsf(x);
            const int32_t n = ee::toInt(t * Float(invpio2) + Float(half));
            const Float fn = ee::toFloat(n);
            Float r = t - fn * Float(pio2_1);
            Float w = fn * Float(pio2_1t); // first round good to 40 bits
            if (n < 32 && (ix & static_cast<int32_t>(0xFFFFFF00u)) != npio2_hw[n - 1])
            {
                y[0] = r - w; // quick check: no cancellation
            }
            else
            {
                const int32_t j = ix >> 23;
                y[0] = r - w;
                int32_t i = j - ((floatBits(y[0]) >> 23) & 0xFF);
                if (i > 8) // second iteration, good to 57 bits
                {
                    Float t2 = r;
                    w = fn * Float(pio2_2);
                    r = t2 - w;
                    w = fn * Float(pio2_2t) - (t2 - r - w);
                    y[0] = r - w;
                    i = j - ((floatBits(y[0]) >> 23) & 0xFF);
                    if (i > 25) // third iteration, 74 bits
                    {
                        t2 = r;
                        w = fn * Float(pio2_3);
                        r = t2 - w;
                        w = fn * Float(pio2_3t) - (t2 - r - w);
                        y[0] = r - w;
                    }
                }
            }
            y[1] = r - y[0] - w;
            return signedResult(n);
        }
        // all other (large) arguments
        if (ix >= 0x7F800000) // inf or NaN
        {
            y[0] = y[1] = x - x;
            return leave(0);
        }
        // z = scalbn(|x|, ilogb(x) - 7), cut into three 8-bit chunks tx[]
        const int e0 = (ix >> 23) - 134;
        Float z = floatFromBits(ix - (e0 << 23));
        for (int i = 0;;) // one branch back
        {
            (&*tx)[i] = ee::toFloat(ee::toInt(z));
            z = (z - (&*tx)[i]) * Float(two8);
            if (++i == 2)
                break;
        }
        (&*tx)[2] = z;
        int nx = 3;
        if ((&*tx)[nx - 1] == Float(0.0f)) // skip the zero terms (the first test is the loop's entry)
        {
            --nx;
            for (;;)
            {
                if (!((&*tx)[nx - 1] == Float(0.0f)))
                    break;
                --nx;
            }
        }
        const int32_t n = __kernel_rem_pio2f(tx, ee::Ptr<Float>(yAddress), e0, nx, 2, ee::Ptr<int32_t>(two_over_pi));
        return signedResult(n);
    }
    READABLE_FUNCTION(__ieee754_rem_pio2f, Verified);

    // __ieee754_sqrtf (ef_sqrt.c): the bit-by-bit square root, rounded to nearest.
    Float __ieee754_sqrtf(Float x)
    {
        // The source's 0x3f000000: q's leading bit (the implicit one, at bit 23 once
        // halved) plus this gives the biased exponent 127 for m = 0.
        constexpr int32_t kHalfExponentBias = 126 << 23;
        int32_t ix = floatBits(x);
        if ((ix & 0x7F800000) == 0x7F800000) // Inf or NaN: sqrt(NaN) = NaN, sqrt(+inf) = +inf, sqrt(-inf) = NaN
            return x * x + x;
        if (ix <= 0)
        {
            const Float r = (ix & 0x7FFFFFFF) == 0 ? x : (x - x) / (x - x); // sqrt(+-0) = +-0, sqrt(-ve) = NaN
            return r;
        }
        // normalise x
        int32_t m = ix >> 23;
        if (m == 0) // subnormal x
        {
            int32_t i = 0;
            if ((ix & kImplicitBit) == 0)
            {
                for (;;)
                {
                    ix <<= 1;
                    ++i;
                    if ((ix & kImplicitBit) != 0)
                        break;
                }
            }
            m -= i - 1;
        }
        m -= 127; // unbias the exponent
        ix = (ix & kMantissaMask) | kImplicitBit;
        if (m & 1) // odd m: double x to make it even
            ix += ix;
        m >>= 1; // m = [m/2]
        // generate sqrt(x) bit by bit
        ix += ix;
        int32_t q = 0;
        int32_t s = 0;    // q = sqrt(x)
        uint32_t r = 1u << 24; // the moving bit, right to left
        for (;;) // 25 passes, 24 branches back
        {
            const int32_t t = s + static_cast<int32_t>(r);
            if (t <= ix)
            {
                s = t + static_cast<int32_t>(r);
                ix -= t;
                q += static_cast<int32_t>(r);
            }
            ix += ix;
            r >>= 1;
            if (r == 0)
                break;
        }
        if (ix != 0) // round to nearest (gcc folded the source's one - tiny and one + tiny tests)
            q += q & 1;
        ix = (q >> 1) + kHalfExponentBias;
        ix += m << 23;
        return floatFromBits(ix);
    }
    READABLE_FUNCTION(__ieee754_sqrtf, Verified);

    // __kernel_cosf (kf_cos.c): cos on [-pi/4, pi/4], x + y the reduced argument.
    Float __kernel_cosf(Float x, Float y)
    {
        constexpr float C1 = 4.1666667908e-02f;  // 0x3d2aaaab
        constexpr float C2 = -1.3888889225e-03f; // 0xbab60b61
        constexpr float C3 = 2.4801587642e-05f;  // 0x37d00d01
        constexpr float C4 = -2.7557314297e-07f; // 0xb493f27c
        constexpr float C5 = 2.0875723372e-09f;  // 0x310f74f6
        constexpr float C6 = -1.1359647598e-11f; // 0xad47d74e

        const int32_t ix = floatBits(x) & 0x7FFFFFFF;
        if (ix < 0x32000000 && ee::toInt(x) == 0) // |x| < 2**-27: 1, raising inexact
            return Float(one);
        const Float z = x * x;
        const Float r = z * (z * (z * (z * (z * (z * Float(C6) + Float(C5)) + Float(C4)) + Float(C3)) + Float(C2)) + Float(C1));
        if (ix < 0x3E99999A) // |x| < 0.3
            return Float(one) - (z * Float(half) - (z * r - x * y));
        // qx = |x|/4 (0.28125 above 0.78125)
        const Float qx = floatFromBits(ix > 0x3F480000 ? 0x3E900000 : ix - (1u << 24));
        const Float hz = z * Float(half) - qx;
        const Float a = Float(one) - qx;
        return a - (hz - (z * r - x * y));
    }
    READABLE_FUNCTION(__kernel_cosf, Verified);

    // __kernel_sinf (kf_sin.c): sin on [-pi/4, pi/4]; iy = 0 when y is 0.
    Float __kernel_sinf(Float x, Float y, int32_t iy)
    {
        constexpr float S1 = -1.6666667163e-01f; // 0xbe2aaaab
        constexpr float S2 = 8.3333337680e-03f;  // 0x3c088889
        constexpr float S3 = -1.9841270114e-04f; // 0xb9500d01
        constexpr float S4 = 2.7557314297e-06f;  // 0x3638ef1b
        constexpr float S5 = -2.5050759689e-08f; // 0xb2d72f34
        constexpr float S6 = 1.5896910177e-10f;  // 0x2f2ec9d3

        const int32_t ix = floatBits(x) & 0x7FFFFFFF;
        if (ix < 0x32000000 && ee::toInt(x) == 0) // |x| < 2**-27: x, raising inexact
            return x;
        const Float z = x * x;
        const Float v = z * x;
        const Float r = z * (z * (z * (z * Float(S6) + Float(S5)) + Float(S4)) + Float(S3)) + Float(S2);
        if (iy == 0)
            return x + v * (z * r + Float(S1));
        return x - (z * (y * Float(half) - v * r) - y - v * Float(S1));
    }
    READABLE_FUNCTION(__kernel_sinf, Verified);

    // __kernel_tanf (kf_tan.c): tan (iy = 1) or -1/tan (iy = -1) on [-pi/4, pi/4],
    // above 0.6744 by tan(pi/4 - x).
    Float __kernel_tanf(Float x, Float y, int32_t iy)
    {
        constexpr float pio4 = 7.8539812565e-01f;   // 0x3f490fda
        constexpr float pio4lo = 3.7748947079e-08f; // 0x33222168
        static constexpr float T[] = {
            3.3333334327e-01f,  // 0x3eaaaaab
            1.3333334029e-01f,  // 0x3e088889
            5.3968254477e-02f,  // 0x3d5d0dd1
            2.1869488060e-02f,  // 0x3cb327a4
            8.8632395491e-03f,  // 0x3c11371f
            3.5920790397e-03f,  // 0x3b6b6916
            1.4562094584e-03f,  // 0x3abede48
            5.8804126456e-04f,  // 0x3a1a26c8
            2.4646313977e-04f,  // 0x398137b9
            7.8179444245e-05f,  // 0x38a3f445
            7.1407252108e-05f,  // 0x3895c07a
            -1.8558637748e-05f, // 0xb79bae5f
            2.5907305826e-05f,  // 0x37d95384
        };

        const int32_t hx = floatBits(x);
        const int32_t ix = hx & 0x7FFFFFFF;
        if (ix < 0x31800000 && ee::toInt(x) == 0) // |x| < 2**-28, raising inexact
        {
            if ((ix | (iy + 1)) == 0)
                return Float(one) / fabsf(x);
            return iy == 1 ? x : Float(-1.0f) / x;
        }
        if (ix >= 0x3F2CA140) // |x| >= 0.6744: tan(pi/4 - |x|)
        {
            if (hx < 0)
            {
                x = -x;
                y = -y;
            }
            const Float z = Float(pio4) - x;
            const Float w = Float(pio4lo) - y;
            x = z + w;
            y = Float(0.0f);
        }
        const Float z = x * x;
        const Float w = z * z;
        // x^5 (T[1] + x^2 T[2] + ...) as x^5 (T[1] + x^4 T[3] + ... + x^20 T[11]) and
        // x^5 (x^2 (T[2] + x^4 T[4] + ... + x^22 T[12]))
        const Float odd = Float(T[1]) + w * (Float(T[3]) + w * (Float(T[5]) + w * (Float(T[7]) + w * (Float(T[9]) + w * Float(T[11])))));
        const Float even = z * (Float(T[2]) + w * (Float(T[4]) + w * (Float(T[6]) + w * (Float(T[8]) + w * (Float(T[10]) + w * Float(T[12]))))));
        const Float s = z * x;
        Float r = y + z * (s * (odd + even) + y);
        r += Float(T[0]) * s;
        const Float sum = x + r;
        if (ix >= 0x3F2CA140)
        {
            const Float v = ee::toFloat(iy);
            const Float d = x - (sum * sum / (sum + v) - r);
            return ee::toFloat(1 - ((hx >> 30) & 2)) * (v - (d + d)); // sign * (v - 2*(x - (w*w/(w+v) - r)))
        }
        if (iy == 1)
            return sum;
        // -1/(x + r), accurately: sum split as zh + (r - (zh - x)), -1/sum as t + ...
        const Float zh = floatFromBits(floatBits(sum) & static_cast<int32_t>(0xFFFFF000u));
        const Float v = r - (zh - x); // zh + v = r + x
        const Float a = Float(-1.0f) / sum;
        const Float t = floatFromBits(floatBits(a) & static_cast<int32_t>(0xFFFFF000u));
        const Float s1 = t * zh + Float(one);
        return t + a * (s1 + t * v);
    }
    READABLE_FUNCTION(__kernel_tanf, Verified);

    // matherr (common/s_matherr.c): the default handler, which changes nothing and
    // returns 0 (its test of arg1 != arg1 is a soft-float compare either way).
    int32_t matherr(ee::Address exception)
    {
        const uint64_t arg1 = ee::Ptr<Exception>(exception)->arg1;
        libgcc::dpcmp(arg1, arg1); // x->arg1 != x->arg1: NaN or not, the result is 0
        return 0;
    }
    READABLE_FUNCTION(matherr, Verified);

    // isnanf (math/sf_isnan.c): 1 for a NaN, else 0, without a compare.
    int32_t isnanf(Float x)
    {
        const int32_t ix = 0x7F800000 - (floatBits(x) & 0x7FFFFFFF);
        return static_cast<int32_t>(static_cast<uint32_t>(ix) >> 31);
    }
    READABLE_FUNCTION(isnanf, Verified);

    namespace
    {
        // EXTRACT_WORDS: a double's high and low words (it travels as its 64-bit pattern), and
        // the high word without its sign, with bit 0 set when anything is in the low word: the
        // exponent and fraction as one number that is 0x7FF00000 exactly for an infinity.
        int32_t magnitudeWord(uint64_t x)
        {
            const int32_t hx = static_cast<int32_t>(x >> 32) & 0x7FFFFFFF;
            const uint32_t lx = static_cast<uint32_t>(x);
            return hx | static_cast<int32_t>((lx | (0u - lx)) >> 31);
        }
    }

    // isinf (math/s_isinf.c): 1 for an infinity, else 0, without a compare. The float
    // printing (vfprintf's) is its only user.
    int32_t isinf(uint64_t x)
    {
        const int32_t hx = 0x7FF00000 - magnitudeWord(x);
        return 1 - static_cast<int32_t>(static_cast<uint32_t>(hx | -hx) >> 31);
    }
    READABLE_FUNCTION(isinf, Verified);

    // isnan (math/s_isnan.c): 1 for a NaN, else 0, without a compare.
    int32_t isnan(uint64_t x)
    {
        const int32_t hx = 0x7FF00000 - magnitudeWord(x);
        return static_cast<int32_t>(static_cast<uint32_t>(hx) >> 31);
    }
    READABLE_FUNCTION(isnan, Verified);

    // scalbnf (common/sf_scalbn.c): x * 2**n by the exponent bits.
    Float scalbnf(Float x, int32_t n)
    {
        constexpr float two25 = 3.355443200e+07f;   // 0x4c000000
        constexpr float twom25 = 2.9802322388e-08f; // 0x33000000
        constexpr int32_t kOverflowInt = 50000;

        int32_t ix = floatBits(x);
        int32_t k = (ix & 0x7F800000) >> 23; // the exponent
        if (k == 0) // 0 or subnormal x
        {
            if ((ix & 0x7FFFFFFF) == 0)
                return x; // +-0
            x *= Float(two25);
            ix = floatBits(x);
            k = ((ix & 0x7F800000) >> 23) - 25;
            if (n < -50000)
                return x * Float(tiny); // underflow
        }
        if (k == 0xFF)
            return x + x; // NaN or Inf
        k = k + n;
        if (k <= 0xFE)
        {
            if (k > 0) // a normal result
                return floatFromBits((ix & static_cast<int32_t>(0x807FFFFFu)) | (k << 23));
            if (k > -25) // a subnormal result
                return floatFromBits((ix & static_cast<int32_t>(0x807FFFFFu)) | ((k + 25) << 23)) * Float(twom25);
            if (n <= kOverflowInt) // underflow (n + k without integer overflow)
                return copysignf(Float(tiny), x) * Float(tiny);
        }
        return copysignf(Float(huge), x) * Float(huge); // overflow
    }
    READABLE_FUNCTION(scalbnf, Verified);

    // copysignf (common/sf_copysign.c): x with y's sign.
    Float copysignf(Float x, Float y)
    {
        return floatFromBits((floatBits(x) & 0x7FFFFFFF) | (floatBits(y) & static_cast<int32_t>(0x80000000u)));
    }
    READABLE_FUNCTION(copysignf, Verified);

    namespace
    {
        constexpr int32_t kInitialTerms[] = {4, 7, 9}; // init_jk
        constexpr float kPiOver2Parts[] = {          // PIo2: pi/2 in pieces of 8 bits
            1.5703125000e+00f, 4.5776367188e-04f, 2.5987625122e-05f, 7.5437128544e-08f,
            6.0026650317e-11f, 7.3896444519e-13f, 5.3845816694e-15f, 5.6378512969e-18f,
            8.3009228831e-20f, 3.2756352257e-22f, 6.3331015649e-25f};
        constexpr float kTwo8 = 2.5600000000e+02f, kTwon8 = 3.9062500000e-03f;

    }

    // kf_rem_pio2.c: x, in nx 8-bit pieces from 2 to the e0, reduced modulo pi/2: the remainder in y
    // (1, 2 or 3 floats by prec), the quadrant returned. ipio2 is the table of 2/pi's bits. The first
    // try may cancel to zero; then it recomputes with more bits of 2/pi.
    int32_t __kernel_rem_pio2f(ee::Ptr<Float> x, ee::Ptr<Float> y, int32_t e0, int32_t nx, int32_t prec, ee::Ptr<int32_t> ipio2)
    {
        int32_t iq[20];
        Float f[20], fq[20], q[20];
        const int32_t jk = kInitialTerms[prec];
        const int32_t jp = jk;
        const int32_t jx = nx - 1;
        int32_t jv = (e0 - 3) / 8;
        if (jv < 0)
            jv = 0;
        int32_t q0 = e0 - 8 * (jv + 1);
        {
            int32_t j = jv - jx;
            const int32_t m = jx + jk;
            for (int32_t i = 0; i <= m; ++i, ++j)
                f[i] = j < 0 ? Float(0.0f) : ee::toFloat(ipio2[j]);
        }
        const auto termOf = [&](int32_t i) {
            Float sum(0.0f);
            for (int32_t j = 0; j <= jx; ++j)
                sum += x[j] * f[jx + i - j];
            return sum;
        };
        for (int32_t i = 0; i <= jk; ++i)
            q[i] = termOf(i);

        int32_t jz = jk;
        int32_t n, ih;
        Float z;
        for (;;)
        {
            // Distills q[] into iq[], in reverse order.
            z = q[jz];
            int32_t i = 0;
            for (int32_t j = jz; j > 0; ++i, --j)
            {
                const Float fw = ee::toFloat(ee::toInt(Float(kTwon8) * z));
                iq[i] = ee::toInt(z - Float(kTwo8) * fw);
                z = q[j - 1] + fw;
            }
            // The integer part of z, in n.
            z = scalbnf(z, q0);
            z -= Float(8.0f) * floorf(z * Float(0.125f));
            n = ee::toInt(z);
            z -= ee::toFloat(n);
            ih = 0;
            if (q0 > 0)
            {
                const int32_t top = iq[jz - 1] >> (8 - q0);
                n += top;
                iq[jz - 1] -= top << (8 - q0);
                ih = iq[jz - 1] >> (7 - q0);
            }
            else if (q0 == 0)
                ih = iq[jz - 1] >> 8;
            else if (z >= Float(0.5f))
                ih = 2;
            if (ih > 0)
            {
                // q is above a half: 1 - q instead.
                n += 1;
                int32_t carry = 0;
                for (i = 0; i < jz; ++i)
                {
                    const int32_t j = iq[i];
                    if (carry == 0)
                    {
                        if (j != 0)
                        {
                            carry = 1;
                            iq[i] = 0x100 - j;
                        }
                    }
                    else
                        iq[i] = 0xFF - j;
                }
                if (q0 == 1)
                    iq[jz - 1] &= 0x7F;
                else if (q0 == 2)
                    iq[jz - 1] &= 0x3F;
                if (ih == 2)
                {
                    z = Float(1.0f) - z;
                    if (carry != 0)
                        z -= scalbnf(Float(1.0f), q0);
                }
            }
            if (z != Float(0.0f))
                break;
            int32_t j = 0;
            for (i = jz - 1; i >= jk; --i)
                j |= iq[i];
            if (j != 0)
                break;
            // Everything cancelled: more terms.
            int32_t k = 1;
            while (iq[jk - k] == 0)
                ++k;
            for (i = jz + 1; i <= jz + k; ++i)
            {
                f[jx + i] = ee::toFloat(ipio2[jv + i]);
                q[i] = termOf(i);
            }
            jz += k;
        }

        // Chops off zero terms, or splits a term too large.
        if (z == Float(0.0f))
        {
            jz -= 1;
            q0 -= 8;
            while (iq[jz] == 0)
            {
                --jz;
                q0 -= 8;
            }
        }
        else
        {
            z = scalbnf(z, -q0);
            if (z >= Float(kTwo8))
            {
                const Float fw = ee::toFloat(ee::toInt(Float(kTwon8) * z));
                iq[jz] = ee::toInt(z - Float(kTwo8) * fw);
                jz += 1;
                q0 += 8;
                iq[jz] = ee::toInt(fw);
            }
            else
                iq[jz] = ee::toInt(z);
        }

        // Back to floats, then by pi/2.
        Float fw = scalbnf(Float(1.0f), q0);
        for (int32_t i = jz; i >= 0; --i)
        {
            q[i] = fw * ee::toFloat(iq[i]);
            fw *= Float(kTwon8);
        }
        for (int32_t i = jz; i >= 0; --i)
        {
            fw = Float(0.0f);
            for (int32_t k = 0; k <= jp && k <= jz - i; ++k)
                fw += Float(kPiOver2Parts[k]) * q[i + k];
            fq[jz - i] = fw;
        }

        const auto sumDown = [&](int32_t from, int32_t to) {
            Float sum(0.0f);
            for (int32_t i = from; i >= to; --i)
                sum += fq[i];
            return sum;
        };
        const auto signedByIh = [&](Float value) { return ih == 0 ? value : -value; };
        if (prec == 0)
            y[0] = signedByIh(sumDown(jz, 0));
        else if (prec == 1 || prec == 2)
        {
            fw = sumDown(jz, 0);
            y[0] = signedByIh(fw);
            fw = fq[0] - fw;
            for (int32_t i = 1; i <= jz; ++i)
                fw += fq[i];
            y[1] = signedByIh(fw);
        }
        else if (prec == 3)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int32_t i = jz; i > pass; --i)
                {
                    fw = fq[i - 1] + fq[i];
                    fq[i] += fq[i - 1] - fw;
                    fq[i - 1] = fw;
                }
            }
            fw = sumDown(jz, 2);
            y[0] = signedByIh(fq[0]);
            y[1] = signedByIh(fq[1]);
            y[2] = signedByIh(fw);
        }
        return n & 7;
    }
    READABLE_FUNCTION(__kernel_rem_pio2f, Verified);
}
