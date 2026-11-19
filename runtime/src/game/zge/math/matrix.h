#pragma once

// ZGE3 matrix helpers (types.h: row vectors, rows X, Y, Z and T).

#include "types.h"
#include "ee/readable.h"

namespace zge
{
    // matrixIdentity: the identity matrix. Returns m (as given: an uncached
    // mirror's address stays one; BMX XXX passes them).
    ee::Ptr<Matrix> matrixIdentity(ee::Ptr<Matrix> m);

    // matrixClearTranslation: no translation: T's x, y and z = 0 (T.w is left alone).
    // Returns m.
    Matrix *matrixClearTranslation(Matrix *m);

    // matrixClearRotation: no rotation: the 3x3 part becomes the identity; the
    // translation row and the w column are left alone. Returns m.
    Matrix *matrixClearRotation(Matrix *m);

    // matrixTransformPoints: count points (Vec4 strides) transformed by m, p' = p * m
    // with p.w taken as 1; dst.w is left alone. Returns the end of dst
    // (dst + count; dst itself when count <= 0).
    ee::Ptr<Vec4> matrixTransformPoints(ee::Ptr<Vec4> dst, const Matrix *m, const Vec4 *src, int32_t count);

    // matrixScale: out = m with each rotation row's x, y and z scaled by
    // scale's (zge/utilities.cpp). Returns out.
    Matrix *matrixScale(Matrix *out, const Matrix *m, const Vec3 *scale);

    // matrixToEuler: the Euler angles of m's rotation, the inverse of
    // quaternionFromEuler / vec3RotateEuler's X-then-Y-then-Z order. Near
    // gimbal lock (|cos y| <= 0.001) z is taken as 0 and x carries the
    // rest. Returns dst.
    EulerAngles *matrixToEuler(EulerAngles *dst, const Matrix *m);

    // matrixMultiply: out = b * a (row vectors: b's transform, then a's), on
    // VU0. Each row of out keeps b's w. Returns out. Its callers read the
    // VU0 registers it leaves (READABLE_FUNCTION_VU0).
    ee::Ptr<Matrix> matrixMultiply(ee::Ptr<Matrix> out, ee::Ptr<Matrix> a, ee::Ptr<Matrix> b);

    // matrixTranspose3: the 3x3 rotation of m transposed into out (out may be
    // m); out's translation row and w column are left alone. Returns out.
    ee::Ptr<Matrix> matrixTranspose3(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m);

    // matrixInvertRigid: the inverse of a rotation and translation: the rotation
    // transposed, the translation -T by it. Returns out.
    ee::Ptr<Matrix> matrixInvertRigid(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m);

    // matrixRotateY: m turned about y by angle into out (out may be m).
    // Returns m (as the original does).
    ee::Ptr<Matrix> matrixRotateY(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle);
    // matrixRotateZ: the same about z.
    ee::Ptr<Matrix> matrixRotateZ(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle);

    // matrixRotateX: m copied to out (unless the same), then turned about x by
    // angle. Returns out (where the y and z turns hand back m).
    ee::Ptr<Matrix> matrixRotateX(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, Angle angle);
    // matrixRotateXYZ: m turned about x, then y, then z by the angles (into
    // out, then out in place). Returns out.
    ee::Ptr<Matrix> matrixRotateXYZ(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, const EulerAngles *angles);
    // matrixTranslated: out = m's rotation (copied unless the same) with m's
    // translation plus offset. Returns out.
    ee::Ptr<Matrix> matrixTranslated(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, const Vec3 *offset);
    // matrixScaling: m a scaling: scale's x, y and z on the diagonal, the rest
    // of the rotation 0, no translation (its w column left alone). Returns m.
    ee::Ptr<Matrix> matrixScaling(ee::Ptr<Matrix> m, const Vec3 *scale);
    // scaleMatrixRotation: out's rotation = m's times scale, out's translation m's
    // (copied unless the same).
    void scaleMatrixRotation(ee::Ptr<Matrix> out, ee::Ptr<Matrix> m, ee::Float scale);
    // initMathUtilities, shutDownMathUtilities: the math module's start (the shared
    // identity matrix made) and shutdown (nothing).
    void initMathUtilities();
    void shutDownMathUtilities();
    // closestOnSegment: the point of segment start .. end nearest `point`, into
    // out (the registered function; collision/closest.h's of the same name is
    // the host's). Returns out.
    ee::Ptr<Vec4> closestOnSegment(ee::Ptr<Vec4> out, ee::Ptr<const Vec4> start, ee::Ptr<const Vec4> end, ee::Ptr<const Vec4> point);

    // transformPoint (ZGE2 only, matrix.cpp): out = matrix * point with the translation; ZGE3 and ZGE4 inline it.
    ee::Ptr<Vec4> transformPoint(ee::Ptr<Vec4> out, ee::Ptr<const Matrix> matrix, ee::Ptr<const Vec4> point);
}
