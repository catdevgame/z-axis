#pragma once

// Vector arithmetic as the engine writes it: its macros, by value.
//
// Each one runs on VU0, as the original's inline sequence does: VU0's rounding,
// and what it leaves in VU0's registers is what the original leaves, so the next
// vector operation (in another function too) finds the lanes it finds on the PS2.
// Game code writes `wall.point + wall.normal * 0.5f` and owes VU0 nothing.
//
// The order of two operations is the order they run in: when it matters to a
// later stale lane, write them as two statements.
//
// w: the operations that work on x, y and z only (a scale, a transform, a
// rotation) hand back a w that is not the vector's. It is whatever the last
// operation left, and storing the whole result stores it, as the game does.
// Set w afterwards if it has to mean something.
//
// A vector in game memory is a typed reference: `zge::dot(*normal, point)` with
// `ee::Ptr<Vec4> normal`, or a field of a record, or an `ee::Local<Vec4>`.
//
// VU0 only loads whole quadwords, on 16-byte boundaries: a vector a little off
// one reads the aligned quadword it sits in, and every operation here does the
// same. Writing the result is ordinary C++ and cannot do that for you: where the
// destination may sit off a boundary, write `aligned(p) = ...`. Data of another
// type read as a vector goes through `quadwordOf`.

#include "ee/vu0.h"
#include "types.h"
#include "vu0_ops.h"

namespace zge
{
    // The aligned object a store or a quadword move at p really reaches.
    template <typename T>
    inline T &aligned(ee::Ptr<T> p) { return *ee::Ptr<T>(p.address & ~15u); }
    // The aligned quadword that holds *p, as a vector: what VU0 loads for p.
    template <typename T>
    inline Vec4 &quadwordOf(ee::Ptr<T> p) { return *ee::Ptr<Vec4>(p.address & ~15u); }

    namespace detail
    {
        // Where VU0 reads v. Guest addresses are masked, not host pointers: a
        // C++ local is aligned by its type, guest memory (the guest map: RAM, its
        // mirrors, the scratchpad) by the game's quadword.
        template <typename T>
        inline const T *loadedFrom(const T &v)
        {
            const uintptr_t offset = reinterpret_cast<uintptr_t>(&v) - reinterpret_cast<uintptr_t>(ee::g_ram);
            return ee::inGuestMap(&v) ? reinterpret_cast<const T *>(ee::g_ram + (offset & ~uintptr_t(15))) : &v;
        }
    }

    // A Vec3 as a direction: w 0.
    inline Vec4 direction(const Vec3 &v) { return Vec4{v.x, v.y, v.z, 0.0f}; }

    // (0, 0, 0, 1): VU0's constant register, which the engine stores to clear a
    // vector or to put a point at the origin. No VU0 state changes.
    inline constexpr Vec4 kZero{0.0f, 0.0f, 0.0f, 1.0f};

    inline Vec4 operator+(const Vec4 &a, const Vec4 &b)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.sum(detail::loadedFrom(a), detail::loadedFrom(b), &out);
        return out;
    }
    inline Vec4 operator-(const Vec4 &a, const Vec4 &b)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.difference(detail::loadedFrom(a), detail::loadedFrom(b), &out);
        return out;
    }
    // x, y and z scaled; w is what the last operation left.
    inline Vec4 operator*(const Vec4 &v, ee::Float s)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.scale(detail::loadedFrom(v), s, &out);
        return out;
    }
    // v scaled, straight after a dot product or a length. The engine's macro
    // brings the factor in through the integer register that result went out
    // by, so VU0's work register keeps the upper lanes of the result before it
    // (operator* has them 0).
    inline Vec4 scaledAfterMeasure(const Vec4 &v, ee::Float s)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.scale(&v, s, &out, vu.high(5));
        return out;
    }
    // v scaled, where the factor reaches VU0 through an integer register whose upper
    // half holds `carried`: those bits land in VU0's work register and stay there.
    inline Vec4 scaledCarrying(const Vec4 &v, ee::Float s, uint64_t carried)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.scale(detail::loadedFrom(v), s, &out, carried);
        return out;
    }
    // s taken off x, y and z; w is what the last operation left.
    inline Vec4 operator-(const Vec4 &v, ee::Float s)
    {
        vu0::Registers vu;
        Vec4 out;
        vu.less(detail::loadedFrom(v), s, &out);
        return out;
    }
    inline Vec4 &operator+=(Vec4 &a, const Vec4 &b) { return a = a + b; }
    inline Vec4 &operator-=(Vec4 &a, const Vec4 &b) { return a = a - b; }
    inline Vec4 &operator*=(Vec4 &v, ee::Float s) { return v = v * s; }

    inline ee::Float dot(const Vec4 &a, const Vec4 &b)
    {
        ee::vu0::Machine vu;
        return vu.dot(detail::loadedFrom(a), detail::loadedFrom(b));
    }
    inline ee::Float squaredLength(const Vec4 &v)
    {
        ee::vu0::Machine vu;
        return vu.squaredLength(detail::loadedFrom(v));
    }
    // v made length 1 (w 0).
    inline Vec4 unit(const Vec4 &v)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.normalize(detail::loadedFrom(v), &out);
        return out;
    }
    // The same, and v's length.
    inline Vec4 unit(const Vec4 &v, ee::Float &length)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        length = vu.normalize(detail::loadedFrom(v), &out);
        return out;
    }
    // q made length 1 over all four lanes, and its length.
    inline Quaternion unit(const Quaternion &q, ee::Float &length)
    {
        ee::vu0::Machine vu;
        Quaternion out;
        length = vu.normalize4(detail::loadedFrom(q), &out);
        return out;
    }
    // a x b (w 0).
    inline Vec4 cross(const Vec4 &a, const Vec4 &b)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.cross(detail::loadedFrom(a), detail::loadedFrom(b), &out);
        return out;
    }
    // Lane by lane.
    inline Vec4 abs(const Vec4 &v)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.absolute(detail::loadedFrom(v), &out);
        return out;
    }
    // Each of the four lanes of a times the same lane of b.
    inline Vec4 multiplied(const Vec4 &a, const Vec4 &b)
    {
        vu0::Registers vu;
        vu.load(4, detail::loadedFrom(a));
        vu.load(5, detail::loadedFrom(b));
        vu.mul(vu0::kXyzw, 6, 4, 5);
        Vec4 out;
        vu.store(6, &out);
        return out;
    }
    // The smaller and the larger of each of the four lanes.
    inline Vec4 minimum(const Vec4 &a, const Vec4 &b)
    {
        vu0::Registers vu;
        vu.load(4, detail::loadedFrom(a));
        vu.load(5, detail::loadedFrom(b));
        vu.minimum(6, 4, 5);
        Vec4 out;
        vu.store(6, &out);
        return out;
    }
    inline Vec4 maximum(const Vec4 &a, const Vec4 &b)
    {
        vu0::Registers vu;
        vu.load(4, detail::loadedFrom(a));
        vu.load(5, detail::loadedFrom(b));
        vu.maximum(6, 4, 5);
        Vec4 out;
        vu.store(6, &out);
        return out;
    }

    // Each lane of v brought up to limits' x and down to limits' y (VMAXx.xyzw,
    // then VMINIy.xyzw on the one register; no flags).
    inline Vec4 keptBetweenLanes(const Vec4 &v, const Vec4 &limits)
    {
        vu0::Registers vu;
        vu.load(4, detail::loadedFrom(v));
        vu.load(5, detail::loadedFrom(limits));
        vu.maximumBc(vu0::kXyzw, 4, 4, 5, vu0::kLaneX);
        vu.minimumBc(vu0::kXyzw, 4, 4, 5, vu0::kLaneY);
        Vec4 out;
        vu.store(4, &out);
        return out;
    }

    // Whether the box innerLow .. innerHigh lies within outerLow .. outerHigh
    // on x, y and z (faces touching count), as the engine's macro works it
    // out on VU0: the four corners loaded, the low corners' difference into
    // the accumulator, none of its lanes negative, then the high corners',
    // every lane negative or zero.
    inline bool boxWithin(const Vec4 &innerLow, const Vec4 &innerHigh, const Vec4 &outerLow, const Vec4 &outerHigh)
    {
        vu0::Registers vu;
        vu.load(4, detail::loadedFrom(innerLow));
        vu.load(5, detail::loadedFrom(outerLow));
        vu.load(6, detail::loadedFrom(innerHigh));
        vu.load(7, detail::loadedFrom(outerHigh));
        vu.subAcc(ee::vu0::kXyz, 4, 5);
        if (vu.negativeLanes() & ee::vu0::kXyz)
            return false;
        vu.subAcc(ee::vu0::kXyz, 6, 7);
        return ((vu.negativeLanes() | vu.zeroLanes()) & ee::vu0::kXyz) == ee::vu0::kXyz;
    }

    // A point through m: its x, y and z by the rows, plus the translation. p' = p * m
    // (types.h: row vectors), p.w taken as 1; w is what the last transform left.
    inline Vec4 transformed(const Matrix &m, const Vec4 &point)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.transform(detail::loadedFrom(m), detail::loadedFrom(point), &out);
        return out;
    }
    // A direction through m: the rotation alone, no translation.
    inline Vec4 rotated(const Matrix &m, const Vec4 &direction)
    {
        ee::vu0::Machine vu;
        Vec4 out;
        vu.rotate(detail::loadedFrom(m), detail::loadedFrom(direction), &out);
        return out;
    }
    // A point through a projection: all four lanes by the matrix, then x, y and z
    // divided by the w that gives. The result's w is that w.
    inline Vec4 projected(const Vec4 &point, const Matrix &m)
    {
        vu0::Registers vu;
        vu.load(8, detail::loadedFrom(point));
        for (int row = 0; row < 4; ++row)
            vu.load(4 + row, &detail::loadedFrom(m)->row(row));
        vu.mulAcc(vu0::kXyzw, 4, 8, vu0::kLaneX);
        vu.maddAcc(vu0::kXyzw, 5, 8, vu0::kLaneY);
        vu.maddAcc(vu0::kXyzw, 6, 8, vu0::kLaneZ);
        vu.madd(vu0::kXyzw, 9, 7, 8, vu0::kLaneW);
        vu.divideOne(9, vu0::kLaneW);
        vu.mulQ(vu0::kXyz, 9, 9);
        Vec4 out;
        vu.store(9, &out);
        return out;
    }
    // b's transform, then a's (row vectors): each of b's rows through a, the last
    // as a point. Every row keeps b's w.
    inline Matrix operator*(const Matrix &b, const Matrix &a)
    {
        vu0::Registers vu;
        vu.load(8, &detail::loadedFrom(b)->row(0));
        for (int row = 0; row < 4; ++row)
            vu.load(4 + row, &detail::loadedFrom(a)->row(row));
        for (int row = 1; row < 4; ++row)
            vu.load(8 + row, &detail::loadedFrom(b)->row(row));
        vu.rotate(8, 4, 8);
        vu.rotate(9, 4, 9);
        vu.rotate(10, 4, 10);
        vu.transform(11, 4, 7, 11);
        Matrix out;
        for (int row = 0; row < 4; ++row)
            vu.store(8 + row, &out.row(row));
        return out;
    }
    // The PS2 matrix unit (matrix.cpp's loadVu0Matrix and transformByLoadedMatrix): a
    // matrix's four rows kept in VU0 (vf28 .. vf31), then points through it with no load of
    // the matrix.
    inline void keepMatrixInVu0(const Matrix &m)
    {
        vu0::Registers vu;
        for (int row = 0; row < 4; ++row)
            vu.load(28 + row, &detail::loadedFrom(m)->row(row));
    }
    // A point through the kept matrix, its w taken as 1 (VMOVE.w from vf0's 1): x, y and z by
    // the rows, then the translation. w is what the last transform left.
    inline Vec4 transformedByKeptMatrix(const Vec4 &point)
    {
        vu0::Registers vu;
        Vec4 asPoint = *detail::loadedFrom(point);
        asPoint.w = ee::Float(1.0f);
        vu.load(8, &asPoint);
        vu.mulAcc(vu0::kXyz, 28, 8, vu0::kLaneX);
        vu.maddAcc(vu0::kXyz, 29, 8, vu0::kLaneY);
        vu.maddAcc(vu0::kXyz, 30, 8, vu0::kLaneZ);
        vu.madd(vu0::kXyz, 9, 31, 8, vu0::kLaneW);
        Vec4 out;
        vu.store(9, &out);
        return out;
    }
    // A matrix's row as a vector: rows X, Y and Z are the axes it turns onto.
    inline Vec4 &matrixRow(Matrix &m, uint32_t index) { return *reinterpret_cast<Vec4 *>(m.m[index]); }
    inline const Vec4 &matrixRow(const Matrix &m, uint32_t index) { return *reinterpret_cast<const Vec4 *>(m.m[index]); }
}
