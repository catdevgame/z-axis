#include "vector.h"

#include "angle.h"

#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "vec.h"
#include "ee/plain.h"
#include "sym/zge/math/vector.cpp.h"

namespace zge
{
    Vec3 *vec3SetOne(Vec3 *v)
    {
        v->x = 1.0f;
        v->y = 1.0f;
        v->z = 1.0f;
        return v;
    }
    READABLE_FUNCTION(vec3SetOne, Verified);

    Vec3 *vec3ScaleAxes(Vec3 *dst, const Vec3 *src, ee::Float sx, ee::Float sy, ee::Float sz)
    {
        dst->x = src->x * sx;
        dst->y = src->y * sy;
        dst->z = src->z * sz;
        return dst;
    }
    READABLE_FUNCTION(vec3ScaleAxes, Verified);

    Vec4 *vec4Midpoint(Vec4 *dst, const Vec4 *a, const Vec4 *b)
    {
        // The sum of all four lanes, then x, y and z halved: w keeps the sum.
        *dst = *a + *b;
        *dst *= 0.5f;
        return dst;
    }
    READABLE_FUNCTION(vec4Midpoint, Verified);

    Vec3 *vec3Negate(Vec3 *dst, const Vec3 *src)
    {
        dst->x = -src->x;
        dst->y = -src->y;
        dst->z = -src->z;
        return dst;
    }
    READABLE_FUNCTION(vec3Negate, Verified);

    ee::Float vec3Length(const Vec3 *v)
    {
        return ee::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    }
    READABLE_FUNCTION(vec3Length, Verified);

    // lengthSquared: x * x + y * y + z * z, on the FPU. ZGE2 has it as a function its camera, collision and
    // rider code call; ZGE3 and ZGE4 square the vector inline.
    inline ee::Float lengthSquared(const Vec3 *v)
    {
        return v->x * v->x + v->y * v->y + v->z * v->z;
    }
    READABLE_FUNCTION_ONLY(dm2, lengthSquared);

    Vec3 *vec3Lerp(Vec3 *dst, const Vec3 *a, const Vec3 *b, ee::Float t)
    {
        dst->x = a->x + (b->x - a->x) * t;
        dst->y = a->y + (b->y - a->y) * t;
        dst->z = a->z + (b->z - a->z) * t;
        return dst;
    }
    READABLE_FUNCTION(vec3Lerp, Verified);

    // The plane rotation the three axis rotations share: (a, b) turned by
    // angle, a' = a cos - b sin, b' = a sin + b cos.
    namespace
    {
        struct PlaneRotation
        {
            ee::Float a, b;
        };
        PlaneRotation rotatePlane(ee::Float a, ee::Float b, Angle angle)
        {
            const ee::Float c = angleCos(angle);
            const ee::Float s = angleSin(angle);
            return {a * c - b * s, a * s + b * c};
        }
    }

    Vec3 *vec3RotateX(Vec3 *dst, const Vec3 *src, Angle angle)
    {
        const PlaneRotation r = rotatePlane(src->y, src->z, angle);
        dst->z = r.b;
        dst->y = r.a;
        return dst;
    }
    READABLE_FUNCTION(vec3RotateX, Verified);

    Vec3 *vec3RotateY(Vec3 *dst, const Vec3 *src, Angle angle)
    {
        // z' = x sin + z cos, x' = x cos - z sin.
        const PlaneRotation r = rotatePlane(src->x, src->z, angle);
        dst->z = r.b;
        dst->x = r.a;
        return dst;
    }
    READABLE_FUNCTION(vec3RotateY, Verified);

    Vec3 *vec3RotateZ(Vec3 *dst, const Vec3 *src, Angle angle)
    {
        const PlaneRotation r = rotatePlane(src->x, src->y, angle);
        dst->y = r.b;
        dst->x = r.a;
        return dst;
    }
    READABLE_FUNCTION(vec3RotateZ, Verified);

    Vec3 *vec3RotateEuler(Vec3 *dst, const Vec3 *src, const EulerAngles *angles)
    {
        vec3RotateX(dst, src, angles->x);
        vec3RotateY(dst, dst, angles->y);
        vec3RotateZ(dst, dst, angles->z);
        return dst;
    }
    READABLE_FUNCTION(vec3RotateEuler, Verified);

    Vec3 *vec3AddScalar(Vec3 *dst, const Vec3 *src, ee::Float add)
    {
        dst->x = src->x + add;
        dst->y = src->y + add;
        dst->z = src->z + add;
        return dst;
    }
    READABLE_FUNCTION(vec3AddScalar, Verified);
}
