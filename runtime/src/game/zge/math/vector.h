#pragma once

// ZGE3 vector helpers (the engine's math library around vec3SetOne).
//
// Most take a destination first and return it, so calls chain; the
// destination may be one of the sources. The Vec3 functions touch x, y and
// z only: a Vec4's w (or whatever follows a packed Vec3) is left alone.

#include "ee/abi.h"
#include "ee/readable.h"
#include "types.h"

namespace zge
{
    // LQ and SQ: the low bits of the addresses ignored.
    inline void copyQuadword(uint32_t to, uint32_t from) { *ee::at<ee::Qword>(to & ~0xFu) = *ee::at<ee::Qword>(from & ~0xFu); }

    // vec3SetOne: x = y = z = 1 (a unit scale). Returns v.
    Vec3 *vec3SetOne(Vec3 *v);

    // vec3ScaleAxes: dst = src scaled per axis by (sx, sy, sz). Returns dst.
    Vec3 *vec3ScaleAxes(Vec3 *dst, const Vec3 *src, ee::Float sx, ee::Float sy, ee::Float sz);

    // vec4Midpoint (VU0): the midpoint of a and b in x, y, z, (a + b) / 2;
    // w becomes a.w + b.w (the routine halves x, y and z only). Returns dst.
    Vec4 *vec4Midpoint(Vec4 *dst, const Vec4 *a, const Vec4 *b);

    // vec3Negate: dst = -src (sign flips, as NEG.S). Returns dst.
    Vec3 *vec3Negate(Vec3 *dst, const Vec3 *src);

    // vec3Length: the length of v, sqrt(x*x + y*y + z*z).
    ee::Float vec3Length(const Vec3 *v);

    // vec3Lerp: linear interpolation, dst = a + (b - a) * t. Returns dst.
    Vec3 *vec3Lerp(Vec3 *dst, const Vec3 *a, const Vec3 *b, ee::Float t);

    // vec3AddScalar: dst = src with add added to x, y and z. Returns dst.
    Vec3 *vec3AddScalar(Vec3 *dst, const Vec3 *src, ee::Float add);

    // vec3RotateX, vec3RotateY, vec3RotateZ: src rotated by angle about
    // the X, Y or Z axis (y' = y cos - z sin, z' = y sin + z cos for X, and
    // so on round). Only the two coordinates that change are written: the
    // axis coordinate of dst is left as it was, so these are meant for
    // dst == src (or a dst that already holds it). Returns dst.
    Vec3 *vec3RotateX(Vec3 *dst, const Vec3 *src, Angle angle);
    Vec3 *vec3RotateY(Vec3 *dst, const Vec3 *src, Angle angle);
    Vec3 *vec3RotateZ(Vec3 *dst, const Vec3 *src, Angle angle);

    // vec3RotateEuler: src rotated about X, then Y, then Z. The first rotation
    // goes from src to dst, the others work on dst; with dst != src, dst.x
    // is the old dst.x when the Y rotation reads it (see vec3RotateX).
    // Returns dst.
    Vec3 *vec3RotateEuler(Vec3 *dst, const Vec3 *src, const EulerAngles *angles);

    // lengthSquared (ZGE2 only, vector.cpp): x * x + y * y + z * z on the FPU; ZGE3 and ZGE4 square the vector inline.
    ee::Float lengthSquared(const Vec3 *v);
}
