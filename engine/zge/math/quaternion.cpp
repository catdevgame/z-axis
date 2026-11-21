#include "quaternion.h"

#include "angle.h"
#include "matrix.h"
#include "vector.h"

#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "vec.h"
#include "ee/plain.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/math/quaternion.cpp.h"

namespace zge
{
    namespace
    {
        // A quaternion in the games' memory is four words wherever it is: DM2's
        // rider keeps some 4 bytes past a quadword, so a copy of the
        // whole goes through its bytes, never as one aligned quadword
        // (Quaternion's alignas(16) is the structs' layout, not a promise).
        // (A pointer made from the plain address: the compiler assumes no
        // alignment for it, as it would for the Quaternion type.)
        unsigned char *bytesOf(const Quaternion *q) { return reinterpret_cast<unsigned char *>(reinterpret_cast<std::uintptr_t>(q)); }
        void copyQuaternion(Quaternion *dst, const Quaternion *src)
        {
            std::memcpy(bytesOf(dst), bytesOf(src), sizeof(Quaternion));
        }
    }

    Quaternion *quaternionIdentity(Quaternion *q)
    {
        q->x = 0.0f;
        q->y = 0.0f;
        q->z = 0.0f;
        q->w = 1.0f;
        return q;
    }
    READABLE_FUNCTION(quaternionIdentity, Verified);

    // Both quaternions by their game addresses: a body the cut ragdoll leaves wild
    // is copied at the address it has (saveBodyState), which a host pointer would lose.
    ee::Ptr<Quaternion> quaternionCopy(ee::Ptr<Quaternion> dst, ee::Ptr<const Quaternion> src)
    {
        // Two unaligned 64-bit loads, then two stores (LDL/LDR, SDL/SDR).
        Quaternion copy;
        copyQuaternion(&copy, src);
        copyQuaternion(dst, &copy);
        return dst;
    }
    READABLE_FUNCTION(quaternionCopy, Verified);

    Quaternion *quaternionMultiply(Quaternion *dst, const Quaternion *a, const Quaternion *b)
    {
        // Built in the frame first, so dst may be a or b. It stays there: DM2 reads
        // the last product from the stack as a vertex's unset lane.
        ee::Local<Quaternion> product;
        Quaternion left, right;
        copyQuaternion(&left, a);
        copyQuaternion(&right, b);
        a = &left;
        b = &right;
        product->x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
        product->y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
        product->z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
        product->w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
        copyQuaternion(dst, &*product);
        return dst;
    }
    READABLE_FUNCTION(quaternionMultiply, Verified);

    Quaternion *quaternionFromAxisAngle(Quaternion *dst, const Vec3 *axis, Angle angle)
    {
        const Angle half = static_cast<Angle>(angle / 2); // towards zero
        const ee::Float s = angleSin(half);
        const ee::Float c = angleCos(half);
        dst->x = axis->x * s;
        dst->y = axis->y * s;
        dst->z = axis->z * s;
        dst->w = c;
        return dst;
    }
    READABLE_FUNCTION(quaternionFromAxisAngle, Verified);

    Angle quaternionToAxisAngle(Vec3 *axis, const Quaternion *q)
    {
        // |xyz| = sin(angle / 2) |q|, w = cos(angle / 2) |q|.
        axis->x = q->x;
        axis->y = q->y;
        axis->z = q->z;
        const ee::Float sinHalfLength = vec3Length(axis);
        if (sinHalfLength < kMathEpsilon)
        {
            // No rotation (or none to speak of): any axis will do.
            axis->x = 1.0f;
            axis->y = 0.0f;
            axis->z = 0.0f;
            return 0;
        }
        axis->x = q->x / sinHalfLength;
        axis->y = q->y / sinHalfLength;
        axis->z = q->z / sinHalfLength;
        const Angle half = static_cast<short>(angleAtan2(sinHalfLength, q->w));
        // Doubled by way of degrees, with the float rounding that goes with it.
        const ee::Float halfDegrees = ee::toFloat(half) * kAngleToDegrees;
        const Angle angle = static_cast<Angle>(ee::toInt((halfDegrees + halfDegrees) * kDegreesToAngle));
        return angle;
    }
    READABLE_FUNCTION(quaternionToAxisAngle, Verified);

    Quaternion *quaternionFromEuler(Quaternion *dst, const EulerAngles *angles)
    {
        // q = qz * qy * qx: the X rotation first, then Y, then Z. The half
        // angles go through radians and back (CVT.W.S, low 16 bits).
        const Angle halfX = radiansToAngle(ee::toFloat(angles->x) * kAngleToRadians * 0.5f);
        const Angle halfY = radiansToAngle(ee::toFloat(angles->y) * kAngleToRadians * 0.5f);
        const Angle halfZ = radiansToAngle(ee::toFloat(angles->z) * kAngleToRadians * 0.5f);
        const ee::Float cx = angleCos(halfX);
        const ee::Float cy = angleCos(halfY);
        const ee::Float cz = angleCos(halfZ);
        const ee::Float sx = angleSin(halfX);
        const ee::Float sy = angleSin(halfY);
        const ee::Float sz = angleSin(halfZ);

        const ee::Float sxcz = sx * cz;
        const ee::Float sxsz = sx * sz;
        const ee::Float cxsz = cx * sz;
        const ee::Float cxcz = cx * cz;
        dst->x = cy * sxcz - sy * cxsz;
        dst->y = cy * sxsz + sy * cxcz;
        dst->z = cy * cxsz - sy * sxcz;
        dst->w = cy * cxcz + sy * sxsz;
        return dst;
    }
    READABLE_FUNCTION(quaternionFromEuler, Verified);

    Matrix *quaternionToMatrix(Matrix *m, Quaternion *q)
    {
        const ee::Float length = quaternionNormalize(q);
        // 2 / |q|^2 would be the textbook scale for the q passed in; the
        // game uses its length before normalizing, on the normalized q
        // (the same while |q| is 1).
        const ee::Float scale = 0.0f < length ? 2.0f / length : 0.0f;

        const ee::Float x = q->x, y = q->y, z = q->z, w = q->w;
        const ee::Float xs = x * scale, ys = y * scale, zs = z * scale;
        const ee::Float xx = x * xs, yy = y * ys, zz = z * zs;
        const ee::Float xy = x * ys, xz = x * zs, yz = y * zs;
        const ee::Float wx = w * xs, wy = w * ys, wz = w * zs;

        auto &r = m->m;
        r[0][0] = 1.0f - (yy + zz);
        r[0][1] = xy + wz;
        r[0][2] = xz - wy;
        r[1][0] = xy - wz;
        r[1][1] = 1.0f - (xx + zz);
        r[1][2] = yz + wx;
        r[2][0] = xz + wy;
        r[2][1] = yz - wx;
        r[2][2] = 1.0f - (xx + yy);
        return m;
    }
    READABLE_FUNCTION(quaternionToMatrix, Verified);

    Quaternion *matrixToQuaternion(Quaternion *q, const Matrix *m)
    {
        const auto &r = m->m;
        // The squares of w, x, y and z from the diagonal; the largest one is
        // taken by its root (the most precise), the others from the
        // off-diagonal sums and differences.
        const ee::Float onePlus = r[0][0] + 1.0f;
        const ee::Float oneMinus = 1.0f - r[0][0];
        const ee::Float xx = (onePlus - r[1][1] - r[2][2]) / 4.0f;
        const ee::Float ww = (onePlus + r[1][1] + r[2][2]) / 4.0f;
        const ee::Float zz = (oneMinus - r[1][1] + r[2][2]) / 4.0f;
        const ee::Float yy = (oneMinus + r[1][1] - r[2][2]) / 4.0f;

        enum Largest
        {
            W,
            X,
            Y,
            Z
        } largest = W;
        ee::Float square = ww;
        if (square < xx)
        {
            square = xx;
            largest = X;
        }
        if (square < yy)
        {
            square = yy;
            largest = Y;
        }
        if (square < zz)
        {
            square = zz;
            largest = Z;
        }
        const ee::Float root = ee::sqrt(square);
        const ee::Float inverse = 1.0f / (root * 4.0f);
        // w from an off-diagonal difference, a - b. ZGE2 takes b - a and negates the product: the same but for
        // a zero's sign (-0 where ZGE3 has +0).
        auto wFrom = [&](ee::Float a, ee::Float b) -> ee::Float
        {
            if constexpr (game::readable::kIsDm2)
                return -((b - a) * inverse);
            else
                return (a - b) * inverse;
        };

        switch (largest)
        {
        case W:
            // Negated: w <= 0 and x, y, z the other way round.
            q->w = -root;
            q->x = (r[2][1] - r[1][2]) * inverse;
            q->y = (r[0][2] - r[2][0]) * inverse;
            q->z = (r[1][0] - r[0][1]) * inverse;
            break;
        case X:
            q->x = root;
            q->w = wFrom(r[1][2], r[2][1]);
            q->y = (r[0][1] + r[1][0]) * inverse;
            q->z = (r[0][2] + r[2][0]) * inverse;
            break;
        case Y:
            q->y = root;
            q->w = wFrom(r[2][0], r[0][2]);
            q->z = (r[1][2] + r[2][1]) * inverse;
            q->x = (r[1][0] + r[0][1]) * inverse;
            break;
        case Z:
            q->z = root;
            q->w = wFrom(r[0][1], r[1][0]);
            q->x = (r[2][0] + r[0][2]) * inverse;
            q->y = (r[2][1] + r[1][2]) * inverse;
            break;
        }
        return q;
    }
    READABLE_FUNCTION(matrixToQuaternion, Verified);

    Quaternion *quaternionConjugate(Quaternion *dst, const Quaternion *src)
    {
        dst->x = -src->x;
        dst->y = -src->y;
        dst->z = -src->z;
        dst->w = src->w;
        return dst;
    }
    READABLE_FUNCTION(quaternionConjugate, Verified);

    ee::Float quaternionNormalize(Quaternion *q)
    {
        // This one's result reaches further than q: the unit quaternion's w is
        // what the follow camera's next scaled vectors carry as their own w, and
        // only this call holds that feedback down (see updateCameraLookAt).
        if constexpr (game::readable::kIsDm2) // ZGE2 on the FPU: the length, then each part divided by it
        {
            const ee::Float length = ee::sqrt(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
            q->x = q->x / length;
            q->y = q->y / length;
            q->z = q->z / length;
            q->w = q->w / length;
            return length;
        }
        ee::Float length;
        *q = unit(*q, length);
        return length;
    }
    READABLE_FUNCTION_VU0(quaternionNormalize, Verified);

    ee::Float quaternionDot(const Quaternion *a, const Quaternion *b)
    {
        return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
    }
    READABLE_FUNCTION(quaternionDot, Verified);

    Quaternion *quaternionInverse(Quaternion *dst, Quaternion *src)
    {
        // conjugate(src / |src|) / |src|.
        const ee::Float length = quaternionNormalize(src);
        quaternionConjugate(dst, src);
        dst->x = dst->x / length;
        dst->y = dst->y / length;
        dst->z = dst->z / length;
        dst->w = dst->w / length;
        return dst;
    }
    READABLE_FUNCTION(quaternionInverse, Verified);

    Quaternion *quaternionSlerp(Quaternion *dst, const Quaternion *from, const Quaternion *to, ee::Float t)
    {
        ee::Float cosAngle = quaternionDot(from, to);
        // The short way round: interpolate towards -to instead.
        const bool flip = cosAngle < 0.0f;
        if (flip)
            cosAngle = -cosAngle;

        ee::Float fromWeight, toWeight;
        if (1.0f - cosAngle < kMathEpsilon)
        {
            // Nearly the same rotation: sin(angle) is too small to divide
            // by, interpolate linearly.
            fromWeight = 1.0f - t;
            toWeight = t;
        }
        else
        {
            // sin((1 - t) angle) / sin(angle) and sin(t angle) / sin(angle),
            // the angle going through the 16-bit trig functions.
            const ee::Float angle = ee::toFloat(static_cast<short>(angleAcos(cosAngle))) * kAngleToRadians;
            const ee::Float sinAngle = angleSin(radiansToAngle(angle));
            const ee::Float toAngle = t * angle;
            fromWeight = angleSin(radiansToAngle(angle - toAngle)) / sinAngle;
            toWeight = angleSin(radiansToAngle(toAngle)) / sinAngle;
        }
        if (flip)
            toWeight = -toWeight;

        dst->x = from->x * fromWeight + to->x * toWeight;
        dst->y = from->y * fromWeight + to->y * toWeight;
        dst->z = from->z * fromWeight + to->z * toWeight;
        dst->w = from->w * fromWeight + to->w * toWeight;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 leaves the blend as it is; ZGE3 added the normalising)
            quaternionNormalize(dst);
        return dst;
    }
    READABLE_FUNCTION(quaternionSlerp, Verified);

    Quaternion *quaternionNlerp(Quaternion *dst, const Quaternion *from, const Quaternion *to, ee::Float t)
    {
        const bool flip = quaternionDot(from, to) < 0.0f;
        const ee::Float fromWeight = 1.0f - t;
        const ee::Float toWeight = flip ? -t : t;
        dst->x = from->x * fromWeight + to->x * toWeight;
        dst->y = from->y * fromWeight + to->y * toWeight;
        dst->z = from->z * fromWeight + to->z * toWeight;
        dst->w = from->w * fromWeight + to->w * toWeight;
        quaternionNormalize(dst);
        return dst;
    }
    READABLE_FUNCTION(quaternionNlerp, Verified);

    Quaternion *quaternionAccumulate(Quaternion *dst, const Quaternion *a, const Quaternion *b)
    {
        // Subtract (add -b) when b is in the other hemisphere, unless a or b
        // is (nearly) zero and has no hemisphere: a zeroed accumulator
        // takes the first rotation as it is.
        bool subtract = false;
        if (quaternionDot(a, b) <= 0.0f)
            subtract = quaternionDot(a, a) >= kMathEpsilon &&
                       quaternionDot(b, b) >= kMathEpsilon;
        if (subtract)
        {
            dst->x = a->x - b->x;
            dst->y = a->y - b->y;
            dst->z = a->z - b->z;
            dst->w = a->w - b->w;
        }
        else
        {
            dst->x = a->x + b->x;
            dst->y = a->y + b->y;
            dst->z = a->z + b->z;
            dst->w = a->w + b->w;
        }
        return dst;
    }
    READABLE_FUNCTION(quaternionAccumulate, Verified);

    Quaternion *quaternionAdd(Quaternion *dst, const Quaternion *a, const Quaternion *b)
    {
        dst->x = a->x + b->x;
        dst->y = a->y + b->y;
        dst->z = a->z + b->z;
        dst->w = a->w + b->w;
        return dst;
    }
    READABLE_FUNCTION(quaternionAdd, Verified);

    Quaternion *quaternionAlign(Quaternion *dst, const Quaternion *q, const Quaternion *reference)
    {
        if (0.0f < quaternionDot(q, reference))
            quaternionCopy(dst, q);
        else
            quaternionScale(dst, q, ee::Float(-1.0f));
        return dst;
    }
    READABLE_FUNCTION(quaternionAlign, Verified);

    Quaternion *quaternionScale(Quaternion *dst, const Quaternion *src, ee::Float s)
    {
        dst->x = src->x * s;
        dst->y = src->y * s;
        dst->z = src->z * s;
        dst->w = src->w * s;
        return dst;
    }
    READABLE_FUNCTION(quaternionScale, Verified);

    // quaternionToEuler: a rotation's Euler angles, through its matrix.
    // Returns matrixToEuler's result (angles).
    ee::Ptr<EulerAngles> quaternionToEuler(ee::Ptr<EulerAngles> angles, ee::Ptr<Quaternion> rotation)
    {
        ee::Local<Matrix> matrix;
        quaternionToMatrix(matrix, rotation);
        return ee::Ptr<EulerAngles>(matrixToEuler(angles, matrix));
    }
    READABLE_FUNCTION(quaternionToEuler, Verified);

    namespace
    {
        // Below this the quaternion turns about no axis.
        constexpr ee::Float kNoAxis = ee::Float(1e-05f);
        constexpr ee::Float kHalfTurnDegrees = ee::Float(180.0f);
    }

    // quaternionToAxisTurns (was slerpVec3; nothing calls it in AI): the
    // quaternion's rotation as its unit axis times the turn in whole turns
    // (acos of w is half the angle: in degrees, over 180), into out's x, y
    // and z; (0, 0, 0, 1) when it has no axis. The sign follows the sine of
    // the half angle. Returns nothing.
    void quaternionToAxisTurns(ee::Ptr<Vec4> out, ee::Ptr<Quaternion> q)
    {
        const Angle halfAngle = angleAcos(q->w);
        const ee::Float turns = ee::toFloat(halfAngle) * kAngleToDegrees / kHalfTurnDegrees;
        out->x = q->x;
        out->y = q->y;
        out->z = q->z;
        const ee::Float length = vec3Length(&out->xyz());
        if (length < kNoAxis)
        {
            aligned(out) = kZero;
            return;
        }
        aligned(out) = *out * (ee::Float(1.0f) / length);
        if (angleSin(halfAngle) < ee::Float(0.0f))
        {
            out->x = -(out->x * turns);
            out->y = -(out->y * turns);
            out->z = -(out->z * turns);
            return;
        }
        out->x = out->x * turns;
        out->y = out->y * turns;
        out->z = out->z * turns;
    }
    READABLE_FUNCTION(quaternionToAxisTurns, Draft);

    // scriptStubZero: 0, and nothing else; the last function of the
    // quaternions' block. Was games/aggressive's
    // (script/anim_objects.cpp); the blocks say it is the engine's (layer2),
    // DM2 (PS2) has it word for word.
    uint32_t scriptStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(scriptStubZero, Draft);
}
