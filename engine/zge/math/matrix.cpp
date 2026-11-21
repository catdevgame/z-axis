#include "matrix.h"

#include "angle.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "vec.h"
#include "zge/platform/platform.h"

#include "sym/zge/math/matrix.cpp.h"

namespace zge
{
    ee::Ptr<Matrix> matrixIdentity(ee::Ptr<Matrix> m)
    {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                m->m[row][column] = row == column ? 1.0f : 0.0f;
        return m;
    }
    READABLE_FUNCTION(matrixIdentity, Verified);

    Matrix *matrixClearTranslation(Matrix *m)
    {
        m->m[3][0] = 0.0f;
        m->m[3][1] = 0.0f;
        m->m[3][2] = 0.0f;
        return m;
    }
    READABLE_FUNCTION(matrixClearTranslation, Verified);

    Matrix *matrixClearRotation(Matrix *m)
    {
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                m->m[row][column] = row == column ? 1.0f : 0.0f;
        return m;
    }
    READABLE_FUNCTION(matrixClearRotation, Verified);

    ee::Ptr<Vec4> matrixTransformPoints(ee::Ptr<Vec4> dst, const Matrix *m, const Vec4 *src, int32_t count)
    {
        // dst is kept as the game's address (Ptr arithmetic), so the end
        // pointer stays in an uncached mirror if dst was in one.
        const auto &r = m->m;
        if (count <= 0)
            return dst;
        for (;;)
        {
            const ee::Float x = src->x, y = src->y, z = src->z;
            dst->x = (r[0][0] * x + r[1][0] * y) + (r[2][0] * z + r[3][0]);
            dst->y = (r[0][1] * x + r[1][1] * y) + (r[2][1] * z + r[3][1]);
            dst->z = (r[0][2] * x + r[1][2] * y) + (r[2][2] * z + r[3][2]);
            ++dst;
            ++src;
            if (--count <= 0)
                break;
        }
        return dst;
    }
    READABLE_FUNCTION(matrixTransformPoints, Verified);

    EulerAngles *matrixToEuler(EulerAngles *dst, const Matrix *m)
    {
        // For m = Rx * Ry * Z (row vectors): row X is (cy cz, cy sz, -sy),
        // so |cos y| is the length of its x and y.
        constexpr ee::Float kGimbalLockCos = std::bit_cast<float>(0x3A83126Fu); // 0.001
        const auto &r = m->m;
        const ee::Float cosY = ee::sqrt(r[0][0] * r[0][0] + r[0][1] * r[0][1]);
        // Built in a local (the original's stack), then copied to dst.
        EulerAngles angles;
        if (kGimbalLockCos < cosY)
        {
            angles.x = static_cast<short>(angleAtan2(r[1][2], r[2][2]));
            angles.y = static_cast<short>(angleAtan2(-r[0][2], cosY));
            angles.z = static_cast<short>(angleAtan2(r[0][1], r[0][0]));
        }
        else
        {
            angles.x = static_cast<short>(angleAtan2(-r[2][1], r[1][1]));
            angles.y = static_cast<short>(angleAtan2(-r[0][2], cosY));
            angles.z = 0;
        }
        *dst = angles;
        return dst;
    }
    READABLE_FUNCTION(matrixToEuler, Verified);

}

namespace zge
{
    // ---- products, transposes, the rigid inverse, a turn about y ---------------

    ee::Ptr<Matrix> matrixMultiply(ee::Ptr<Matrix> out, ee::Ptr<Matrix> a, ee::Ptr<Matrix> b)
    {
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 on the FPU, the three axes and the position only (the w column is left): a's words are read first,
            // b's again for each word written, so out may be a.
            ee::Float first[4][3];
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t column = 0; column < 3; ++column)
                    first[row][column] = a->m[row][column];
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t column = 0; column < 3; ++column)
                    out->m[row][column] = b->m[row][0] * first[0][column] + b->m[row][1] * first[1][column] +
                                          b->m[row][2] * first[2][column];
            for (uint32_t column = 0; column < 3; ++column)
                out->m[3][column] = first[3][column] + b->m[3][0] * first[0][column] + b->m[3][1] * first[1][column] +
                                    b->m[3][2] * first[2][column];
            return out;
        }
        aligned(out) = *b * *a;
        return out;
    }
    READABLE_FUNCTION_VU0(matrixMultiply, Verified);

    ee::Ptr<Matrix> matrixTranspose3(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m)
    {
        // The six off-diagonal words read first, so out may be m.
        const ee::Float xy = m->m[0][1], yx = m->m[1][0], xz = m->m[0][2];
        const ee::Float zx = m->m[2][0], yz = m->m[1][2], zy = m->m[2][1];
        if (!same(out, m))
        {
            out->m[0][0] = m->m[0][0];
            out->m[1][1] = m->m[1][1];
            out->m[2][2] = m->m[2][2];
        }
        out->m[0][1] = yx;
        out->m[0][2] = zx;
        out->m[1][0] = xy;
        out->m[2][0] = xz;
        out->m[2][1] = yz;
        out->m[1][2] = zy;
        return out;
    }
    READABLE_FUNCTION(matrixTranspose3, Verified);

    // transformByLoadedMatrix (the matrix unit; no caller in AI or DM2): point, its w taken
    // as 1, through the matrix loadVu0Matrix keeps in VU0. out, whose w is what the
    // transform left.
    ee::Ptr<Vec4> transformByLoadedMatrix(ee::Ptr<Vec4> out, ee::Ptr<Vec4> point)
    {
        aligned(out) = transformedByKeptMatrix(*point);
        return out;
    }
    READABLE_FUNCTION_VU0(transformByLoadedMatrix, Draft);

    // loadVu0Matrix (no caller in AI or DM2): m kept in VU0 for transformByLoadedMatrix. m.
    ee::Ptr<Matrix> loadVu0Matrix(ee::Ptr<Matrix> m)
    {
        keepMatrixInVu0(*m);
        return m;
    }
    READABLE_FUNCTION_VU0(loadVu0Matrix, Draft);

    ee::Ptr<Matrix> matrixInvertRigid(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m)
    {
        matrixTranspose3(out, m);
        // The new translation is the old one, negated, through the transposed
        // rotation. Its w is what the last vector operation left. `back` keeps
        // its place in the frame: its w is never set and stays what the stack held.
        const Vec4 &translation = m->translation();
        ee::Local<Vec4> back;
        back->x = -translation.x;
        back->y = -translation.y;
        back->z = -translation.z;
        aligned(out).translation() = rotated(*out, back);
        return out;
    }
    READABLE_FUNCTION_VU0(matrixInvertRigid, Verified);

    ee::Ptr<Matrix> matrixRotateY(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle)
    {
        if (!same(m, out))
            *out = *m;
        const ee::Float c = angleCos(angle);
        const ee::Float s = angleSin(static_cast<int32_t>(angle));
        const ee::Float minusS = -s;
        // Every row's x and z turned: x' = x cos + z sin, z' = z cos - x sin.
        auto &r = out->m;
        const ee::Float x0 = r[0][0], z0 = r[0][2], z1 = r[1][2], z3 = r[3][2];
        const ee::Float x1 = r[1][0], z2 = r[2][2], x2 = r[2][0], x3 = r[3][0];
        r[0][0] = c * x0 + s * z0;
        r[0][2] = minusS * x0 + c * z0;
        r[3][0] = c * x3 + s * z3;
        r[1][0] = c * x1 + s * z1;
        r[1][2] = minusS * x1 + c * z1;
        r[2][0] = c * x2 + s * z2;
        r[2][2] = minusS * x2 + c * z2;
        r[3][2] = minusS * x3 + c * z3;
        // The original hands back m, not out.
        return m;
    }
    READABLE_FUNCTION(matrixRotateY, Verified);

    // matrixRotateZ (kMatrixRotateZ): m copied to out (unless the same), then
    // turned about z by angle; m handed back.
    ee::Ptr<Matrix> matrixRotateZ(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle)
    {
        if (!same(m, out))
            *out = *m;
        const ee::Float c = angleCos(angle);
        const ee::Float s = angleSin(static_cast<int32_t>(angle));
        // Every row's x and y turned: x' = x cos - y sin, y' = x sin + y cos.
        auto &r = out->m;
        const ee::Float x0 = r[0][0], y0 = r[0][1], y1 = r[1][1], y2 = r[2][1];
        const ee::Float x1 = r[1][0], y3 = r[3][1], x2 = r[2][0], x3 = r[3][0];
        r[0][0] = c * x0 - s * y0;
        r[0][1] = s * x0 + c * y0;
        r[3][0] = c * x3 - s * y3;
        r[1][0] = c * x1 - s * y1;
        r[1][1] = s * x1 + c * y1;
        r[2][0] = c * x2 - s * y2;
        r[2][1] = s * x2 + c * y2;
        r[3][1] = s * x3 + c * y3;
        return m;
    }
    READABLE_FUNCTION(matrixRotateZ, Verified);

    // ---- the rest of zmathUtil.c: the x turn, the turns by Euler angles, the
    // translation, scaling and the module's start ---------------------------------

    namespace
    {
        inline ee::Global<Matrix, sym::g_identityMatrix> g_identityMatrix;

    }

    ee::Ptr<Matrix> matrixRotateX(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle)
    {
        if (!same(m, out))
            aligned(out) = aligned(m);
        const ee::Float c = angleCos(angle);
        const ee::Float s = angleSin(static_cast<int32_t>(angle));
        // Every row's y and z turned: y' = y cos - z sin, z' = y sin + z cos.
        auto &r = out->m;
        const ee::Float y0 = r[0][1], z0 = r[0][2], z1 = r[1][2], z2 = r[2][2];
        const ee::Float y1 = r[1][1], z3 = r[3][2], y2 = r[2][1], y3 = r[3][1];
        r[0][1] = c * y0 - s * z0;
        r[0][2] = s * y0 + c * z0;
        r[3][1] = c * y3 - s * z3;
        r[1][1] = c * y1 - s * z1;
        r[1][2] = s * y1 + c * z1;
        r[2][1] = c * y2 - s * z2;
        r[2][2] = s * y2 + c * z2;
        r[3][2] = s * y3 + c * z3;
        return out;
    }
    READABLE_FUNCTION(matrixRotateX, Verified);

    ee::Ptr<Matrix> matrixRotateXYZ(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, const EulerAngles *angles)
    {
        matrixRotateX(out, m, angles->x);
        matrixRotateY(out, out, angles->y);
        matrixRotateZ(out, out, angles->z);
        return out;
    }
    READABLE_FUNCTION(matrixRotateXYZ, Verified);

    ee::Ptr<Matrix> matrixTranslated(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, const Vec3 *offset)
    {
        if (!same(out, m))
            matrixCopyRotation(out, m);
        out->m[3][0] = m->m[3][0] + offset->x;
        out->m[3][1] = m->m[3][1] + offset->y;
        out->m[3][2] = m->m[3][2] + offset->z;
        return out;
    }
    READABLE_FUNCTION(matrixTranslated, Verified);

    ee::Ptr<Matrix> matrixScaling(ee::Ptr<Matrix> m, const Vec3 *scale)
    {
        auto &r = m->m;
        r[0][0] = scale->x;
        r[1][1] = scale->y;
        r[2][2] = scale->z;
        r[1][0] = 0.0f;
        r[1][2] = 0.0f;
        r[0][2] = 0.0f;
        r[2][1] = 0.0f;
        r[0][1] = 0.0f;
        r[2][0] = 0.0f;
        matrixClearTranslation(m.get());
        return m;
    }
    READABLE_FUNCTION(matrixScaling, Verified);

    void scaleMatrixRotation(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, ee::Float scale)
    {
        for (uint32_t column = 0; column < 3; ++column)
            for (uint32_t row = 0; row < 3; ++row)
                out->m[row][column] = m->m[row][column] * scale;
        if (!same(m, out))
            aligned(out).translation() = aligned(m).translation();
    }
    READABLE_FUNCTION(scaleMatrixRotation, Verified);

    namespace
    {
        inline ee::Global<Vec4, sym::worldUp> worldUp; // the world's up (y down)
    }

    // lookAtMatrix: the matrix at the eye looking at the target: row Z along
    // the view, X across it and the world's up, Y across both, T the eye.
    // Nothing keeps a roll. A game override wraps it (photo mode's roll).
    ee::Ptr<Matrix> lookAtMatrix(ee::Ptr<Matrix> matrix, ee::Ptr<Vec4> eye, ee::Ptr<Vec4> target)
    {
        Matrix &m = aligned(matrix);
        Vec4 &rowX = m.row(0), &rowY = m.row(1), &rowZ = m.row(2);
        rowZ = *target - *eye;
        rowZ = unit(rowZ);
        rowX = cross(rowZ, *worldUp);
        rowX = unit(rowX);
        rowY = cross(rowZ, rowX);
        m.translation() = aligned(eye);
        return matrix;
    }
    READABLE_FUNCTION(lookAtMatrix, Verified);

    void initMathUtilities()
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2's trigonometry is looked up: angle.cpp)
        {
            buildSineTable();
            buildArcSineTable();
            buildArcTangentTable();
        }
        matrixIdentity(g_identityMatrix.get());
    }
    READABLE_FUNCTION(initMathUtilities, Verified);

    void shutDownMathUtilities()
    {
    }
    READABLE_FUNCTION(shutDownMathUtilities, Verified);

    // reflectVector: a vector reflected off a surface of unit normal n:
    // v - 2 (v . n) n, into out's x, y and z. Returns out.
    ee::Ptr<Vec4> reflectVector(ee::Ptr<Vec4> out, ee::Ptr<Vec4> vector, ee::Ptr<Vec4> normal)
    {
        const ee::Float along = dot(*vector, *normal);
        const ee::Float twice = along + along;
        out->x = vector->x - twice * normal->x;
        out->y = vector->y - twice * normal->y;
        out->z = vector->z - twice * normal->z;
        return out;
    }
    READABLE_FUNCTION(reflectVector, Draft);

    namespace
    {
        // rotationFromTo: above this cosine the two directions count as one.
        constexpr ee::Float kSameDirection = ee::Float(0.999f);
    }

    // rotationFromTo (collisionQuery): the rotation that turns unit vector
    // from onto unit vector to, about their cross product's axis (the
    // identity when they are within kSameDirection): Rodrigues' form, cos +
    // (1 - cos) / |axis|^2 times the axis' products, the axis added or taken
    // off. The translation row is cleared; the w column is left as it was.
    ee::Ptr<Matrix> rotationFromTo(ee::Ptr<Matrix> matrix, ee::Ptr<Vec4> from, ee::Ptr<Vec4> to)
    {
        ee::Local<Vec4> axis;
        const ee::Float cosine = dot(*from, *to);
        if (kSameDirection < cosine)
            return matrixIdentity(matrix);
        *axis = cross(*from, *to);
        const ee::Float axisSquared = dot(*axis, *axis);
        const ee::Float x = axis->x, y = axis->y, z = axis->z;
        matrix->m[3][0] = ee::Float(0.0f);
        matrix->m[3][1] = ee::Float(0.0f);
        matrix->m[3][2] = ee::Float(0.0f);
        const ee::Float scale = (ee::Float(1.0f) - cosine) / axisSquared;
        const ee::Float scaledX = scale * x;
        const ee::Float scaledY = scale * y;
        matrix->m[0][2] = scaledX * z - y;
        matrix->m[1][2] = scaledY * z + x;
        matrix->m[0][1] = scaledX * y + z;
        matrix->m[2][2] = cosine + scale * z * z;
        matrix->m[0][0] = cosine + scaledX * x;
        matrix->m[1][1] = cosine + scaledY * y;
        matrix->m[1][0] = scaledX * y - z;
        matrix->m[2][0] = scaledX * z + y;
        matrix->m[2][1] = scaledY * z - x;
        return matrix;
    }
    READABLE_FUNCTION_VU0(rotationFromTo, Verified);

    // closestOnSegment (the double grind, the ground move, the faces'
    // tests): the point of the segment from start to end nearest `point`,
    // into out's x, y and z (its w is left as it was): start or end when the
    // point lies beyond that end, else the foot of the perpendicular,
    // measured back from end along the segment's unit direction. Returns
    // out.
    ee::Ptr<Vec4> closestOnSegment(ee::Ptr<Vec4> out, ee::Ptr<const Vec4> start, ee::Ptr<const Vec4> end, ee::Ptr<const Vec4> point)
    {
        ee::Local<Vec4> direction;
        ee::Local<Vec4> offset;
        *direction = *end - *start;
        *direction = unit(*direction);
        *offset = *start - *point;
        if (!(dot(*offset, *direction) < ee::Float(0.0f)))
        {
            aligned(out) = quadwordOf(start);
            return out;
        }
        *offset = *end - *point;
        const ee::Float along = dot(*offset, *direction);
        if (!(ee::Float(0.0f) < along))
        {
            aligned(out) = quadwordOf(end);
            return out;
        }
        out->x = end->x - direction->x * along;
        out->y = end->y - direction->y * along;
        out->z = end->z - direction->z * along;
        return out;
    }
    READABLE_FUNCTION_VU0(closestOnSegment, Verified);

    // transformPoint: ZGE2's point transform, out = matrix * point with the
    // translation (the engine's VU0 macro, vec.h transformed, as a function: ZGE2's
    // placeOnParent, drawScreenPrimitives and testNodeFaces call it, ZGE3 and ZGE4
    // write it inline; AI has no counterpart). Returns out.
    inline ee::Ptr<Vec4> transformPoint(ee::Ptr<Vec4> out, ee::Ptr<const Matrix> matrix, ee::Ptr<const Vec4> point)
    {
        *out = transformed(*matrix, *point);
        return out;
    }
    READABLE_FUNCTION_VU0_ONLY(dm2, transformPoint);
}
