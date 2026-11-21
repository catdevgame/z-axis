#pragma once

// ZGE3 quaternions (quaternionIdentity-scriptStubZero): rotations of the skaters'
// bones, the animation blender and the cameras.
//
// A quaternion is x, y, z, w (w the real part). Functions take the
// destination first and return it; the destination may be a source.
// Angles are 16-bit engine angles (angle.h).
//
// Surprises kept from the original:
//   - quaternionNormalize normalizes in place AND returns the old length;
//     quaternionToMatrix and quaternionInverse call it on their source,
//     so they normalize the caller's quaternion as a side effect.
//   - matrixToQuaternion returns -q (w <= 0) when w is the largest
//     component; q and -q are the same rotation.

#include "types.h"
#include "ee/readable.h"

namespace zge
{
    // quaternionIdentity: the identity rotation (0, 0, 0, 1). Returns q.
    Quaternion *quaternionIdentity(Quaternion *q);

    // quaternionCopy: dst = src (the bits, as a 16-byte copy). Returns dst.
    ee::Ptr<Quaternion> quaternionCopy(ee::Ptr<Quaternion> dst, ee::Ptr<const Quaternion> src);

    // quaternionMultiply: the Hamilton product dst = a * b (b's rotation first,
    // then a's, for column vectors). Returns dst.
    Quaternion *quaternionMultiply(Quaternion *dst, const Quaternion *a, const Quaternion *b);

    // quaternionFromAxisAngle: the rotation by angle about a unit axis. Returns dst.
    Quaternion *quaternionFromAxisAngle(Quaternion *dst, const Vec3 *axis, Angle angle);

    // quaternionToAxisAngle: the axis (unit length; (1, 0, 0) for no rotation) and
    // the angle of q's rotation; returns the angle. q need not be unit
    // length.
    Angle quaternionToAxisAngle(Vec3 *axis, const Quaternion *q);

    // quaternionFromEuler: the rotation by Euler angles (x, y, z).
    Quaternion *quaternionFromEuler(Quaternion *dst, const EulerAngles *angles);

    // quaternionToMatrix: the 3x3 rotation part of m from q (row 3 and column 3
    // are left alone). Normalizes q in place first; a zero q gives the
    // identity. Returns m.
    Matrix *quaternionToMatrix(Matrix *m, Quaternion *q);

    // quaternionToEuler: a rotation's Euler angles, through its matrix
    // (matrixToEuler). Returns angles.
    ee::Ptr<EulerAngles> quaternionToEuler(ee::Ptr<EulerAngles> angles, ee::Ptr<Quaternion> rotation);

    // matrixToQuaternion: the rotation of m's 3x3 part as a quaternion. Returns q.
    Quaternion *matrixToQuaternion(Quaternion *q, const Matrix *m);

    // quaternionConjugate: the conjugate, (-x, -y, -z, w). Returns dst.
    Quaternion *quaternionConjugate(Quaternion *dst, const Quaternion *src);

    // quaternionNormalize (VU0): scales q to unit length in place and returns its
    // length before. A zero q stays zero.
    ee::Float quaternionNormalize(Quaternion *q);

    // quaternionDot: the 4D dot product.
    ee::Float quaternionDot(const Quaternion *a, const Quaternion *b);

    // quaternionInverse: dst = the inverse, conjugate(src) / |src|^2. Normalizes
    // src in place. Returns dst.
    Quaternion *quaternionInverse(Quaternion *dst, Quaternion *src);

    // quaternionSlerp: spherical interpolation from `from` (t = 0) to `to`
    // (t = 1) the short way round, normalized. Returns dst.
    Quaternion *quaternionSlerp(Quaternion *dst, const Quaternion *from, const Quaternion *to, ee::Float t);

    // quaternionNlerp: linear interpolation the short way round, normalized
    // (cheaper than slerp). Returns dst.
    Quaternion *quaternionNlerp(Quaternion *dst, const Quaternion *from, const Quaternion *to, ee::Float t);

    // quaternionAccumulate: dst = a + b, or a - b when b points into the other
    // hemisphere (an accumulator for blending rotations; a or b near zero
    // always adds). Returns dst.
    Quaternion *quaternionAccumulate(Quaternion *dst, const Quaternion *a, const Quaternion *b);

    // quaternionAdd: dst = a + b. Returns dst.
    Quaternion *quaternionAdd(Quaternion *dst, const Quaternion *a, const Quaternion *b);

    // quaternionAlign: dst = q or -q, whichever is in reference's hemisphere.
    // Returns dst.
    Quaternion *quaternionAlign(Quaternion *dst, const Quaternion *q, const Quaternion *reference);

    // quaternionScale: dst = src * s, all four components. Returns dst.
    Quaternion *quaternionScale(Quaternion *dst, const Quaternion *src, ee::Float s);
}
