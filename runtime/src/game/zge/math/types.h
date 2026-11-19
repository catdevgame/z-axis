#pragma once

// The ZGE3 engine's math types, as they sit in EE RAM.
//
// Vectors are four floats (16-byte aligned in the game's structs, so VU0
// can load them with LQC2); a 3D point or direction keeps w (1 for points,
// 0 or unused for directions). Matrices are row-major with row vectors:
// p' = p * M, rows X, Y, Z (the axes) and T (the translation), as VU1 and
// the node structs use them. Quaternions are
// x, y, z, w.
//
// Fields are ee::Float, so arithmetic on them rounds like the EE FPU.

#include "ee/ee.h"
#include "ee/address_compare.h"
#include "ee/mirror.h"

#include <cstddef>

namespace zge
{
    // Pointers compared by address, as the game compares them (ee/address_compare.h).
    using ee::same;
    using ee::before;
    // The bytes between two places in one buffer (ee/mirror.h).
    using ee::bytesFrom;

    struct Vec3
    {
        ee::Float x, y, z;
    };
    static_assert(sizeof(Vec3) == 12);

    struct alignas(16) Vec4
    {
        ee::Float x, y, z, w;

        // The x, y and z of this vector, for the routines that take a Vec3.
        Vec3 &xyz() { return *reinterpret_cast<Vec3 *>(this); }
        const Vec3 &xyz() const { return *reinterpret_cast<const Vec3 *>(this); }
    };
    static_assert(sizeof(Vec4) == 16);
    static_assert(offsetof(Vec4, x) == 0x0 && offsetof(Vec4, y) == 0x4 && offsetof(Vec4, z) == 0x8 &&
                  offsetof(Vec4, w) == 0xC);

    struct alignas(16) Quaternion
    {
        ee::Float x, y, z, w;
    };
    static_assert(sizeof(Quaternion) == 16);

    // Rows X, Y, Z, T; m[row][column].
    struct alignas(16) Matrix
    {
        ee::Float m[4][4];

        // A whole row: 0, 1 and 2 are the axes, 3 the translation.
        Vec4 &row(int index) { return *reinterpret_cast<Vec4 *>(m[index]); }
        const Vec4 &row(int index) const { return *reinterpret_cast<const Vec4 *>(m[index]); }
        Vec4 &translation() { return row(3); }
        const Vec4 &translation() const { return row(3); }
    };
    static_assert(sizeof(Matrix) == 64);

    // An angle as the engine stores most of them: 16 bits for a full turn
    // (0x4000 = 90 degrees), wrapping like the integer does.
    using Angle = int16_t;

    // Rotations about X, Y and Z (the node structs' and the matrix to
    // Euler conversion's layout).
    struct EulerAngles
    {
        Angle x, y, z;
    };
    static_assert(sizeof(EulerAngles) == 6);
}
