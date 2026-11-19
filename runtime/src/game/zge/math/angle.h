#pragma once

// 16-bit engine angles (zge::Angle) and the engine's trigonometry on them
// (zmathUtil.c, from floorOf on): thin wrappers around the C library's
// sinf, cosf, tanf, asinf, acosf, atanf and atan2f that convert between
// angles and radians, and floorf and ceilf.
//
// Readable code calls these with angleSin(...) so that the
// call shows up as the original's call does.

#include "types.h"

#include <bit>
#include "sym/zge/math/angle.h.h"
#include "ee/readable.h"

namespace zge
{
    // 2 pi / 65536 and 65536 / 2 pi (.lit4 constants of the trig wrappers).
    inline constexpr ee::Float kAngleToRadians = std::bit_cast<float>(0x38C90FDBu); // 9.58738e-05
    inline constexpr ee::Float kRadiansToAngle = std::bit_cast<float>(0x4622F983u); // 10430.378
    // 360 / 65536 and 65536 / 360.
    inline constexpr ee::Float kAngleToDegrees = std::bit_cast<float>(0x3BB40000u); // 0.0054931640625
    inline constexpr ee::Float kDegreesToAngle = std::bit_cast<float>(0x43360B61u); // 182.04445

    // The engine's "too small to divide by" (1e-5, several .lit4 copies).
    inline constexpr ee::Float kMathEpsilon = std::bit_cast<float>(0x3727C5ACu);

    // The C library (newlib's single-precision libm in the ELF; recompiled).
    inline constexpr uint32_t kCosf = sym::cosf;
    inline constexpr uint32_t kSinf = sym::sinf;
    inline constexpr uint32_t kAcosf = sym::acosf;
    inline constexpr uint32_t kAtan2f = sym::atan2f;
    inline constexpr uint32_t kAtanf = sym::atanf;
    inline constexpr uint32_t kCeilf = sym::ceilf;
    inline constexpr uint32_t kFloorf = sym::floorf;
    inline constexpr uint32_t kTanf = sym::tanf;
    inline constexpr uint32_t kAsinf = sym::asinf;

    // Radians to an angle as the compiled code does it: CVT.W.S (towards
    // zero, saturating), then the low 16 bits (so it wraps).
    inline Angle radiansToAngle(ee::Float radians) { return static_cast<Angle>(ee::toInt(radians * kRadiansToAngle)); }

    // angleSin: sinf(angle in radians).
    ee::Float angleSin(Angle angle);
    // angleCos: cosf(angle in radians).
    ee::Float angleCos(Angle angle);
    // angleAcos: acosf(x) as an angle.
    Angle angleAcos(ee::Float x);
    // angleAtan2: atan2f(y, x) as an angle.
    Angle angleAtan2(ee::Float y, ee::Float x);
    // angleAsin: asinf(x) as an angle.
    Angle angleAsin(ee::Float x);
    // angleTan: tanf(angle in radians).
    ee::Float angleTan(Angle angle);
    // angleAtan: atanf(x) as an angle.
    Angle angleAtan(ee::Float x);
    // floorOf, ceilingOf: floorf(x), ceilf(x).
    ee::Float floorOf(ee::Float x);
    ee::Float ceilingOf(ee::Float x);

    // ZGE2's trigonometry tables (angle.cpp), built by initMathUtilities: the sine (and so the cosine), the arc sine
    // and the arc tangent. ZGE3 calls the C library instead and has none.
    void buildSineTable();
    void buildArcSineTable();
    void buildArcTangentTable();
}
