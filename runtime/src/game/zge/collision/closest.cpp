// Closest points on a segment and on a triangle (zge/collision/closest.h).

#include "closest.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include "sym/zge/collision/closest.cpp.h"

namespace zge
{
    using ee::Float;

    // ---- a segment ------------------------------------------------------------

    Vec4 closestOnSegment(const Vec4 &a, const Vec4 &b, const Vec4 &point)
    {
        using ee::vu0::add;
        using ee::vu0::mul;
        using ee::vu0::sub;
        const auto difference = [](const Vec4 &x, const Vec4 &y) { return Vec3{sub(x.x, y.x), sub(x.y, y.y), sub(x.z, y.z)}; };
        const auto dotOf = [](const Vec3 &x, const Vec3 &y) { return add(add(mul(x.x, y.x), mul(x.y, y.y)), mul(x.z, y.z)); };

        // b - a over its length (VU0's square root and divide).
        const Vec3 delta = difference(b, a);
        const Float length = add(0.0f, ee::vu0::sqrt(dotOf(delta, delta)));
        const Float inverse = ee::vu0::div(1.0f, length);
        const Vec3 along = {mul(delta.x, inverse), mul(delta.y, inverse), mul(delta.z, inverse)};

        if (!(dotOf(difference(a, point), along) < 0.0f))
            return a;
        const Float bPast = dotOf(difference(b, point), along);
        if (!(0.0f < bPast))
            return b;
        return {sub(b.x, mul(along.x, bPast)), sub(b.y, mul(along.y, bPast)), sub(b.z, mul(along.z, bPast)), b.w};
    }

    // closestPointOnSegment: see closest.h.
    void closestPointOnSegment(const Vec4 *a, const Vec4 *b, const Vec4 *direction, const Vec4 *point, Vec4 *out)
    {
        // The direction and the vector being worked on live in the original's frame, so what they hold at the
        // return stays in the dead stack, where later code reads it as stale stack (a w lane of DM2's).
        ee::Local<Vec4> along;
        ee::Local<Vec4> working;
        if (direction)
            *along = *direction;
        else
        {
            *working = *b - *a;
            *along = unit(*working);
        }

        // How far a and b are past the point along the direction.
        *working = *a - *point;
        if (!(dot(*working, *along) < 0.0f))
        {
            *out = *a;
            return;
        }
        *working = *b - *point;
        const Float bPast = dot(*working, *along);
        if (!(0.0f < bPast))
        {
            *out = *b;
            return;
        }
        // Between: from b back to the point's foot. The scale keeps b - point's w, so the foot's w is
        // b.w - (b.w - point.w).
        *working = *along * bPast;
        *out = *b - *working;
    }
    READABLE_FUNCTION_VU0(closestPointOnSegment, Verified);

    // ---- a triangle -------------------------------------------------------------

    namespace
    {
        // The triangle's edges and the point's offset live in the frame in every version (each is stored there as
        // it is made, and a later function's unset local can read them there); ZGE2 squares them through its
        // lengthSquared, which takes the vector's address, ZGE3 and ZGE4 inline on VU0.
        struct VectorOnStack : ee::Local<Vec4>
        {
            VectorOnStack(const Vec4 &value) { **this = value; }
        };
        using SquaredVector = VectorOnStack;
        Float squaredLengthOf(const Vec4 &v)
        {
            if constexpr (game::readable::kIsDm2)
                return lengthSquared(&v.xyz());
            else
                return squaredLength(v);
        }
        constexpr uint64_t kZeroDouble = 0u; // 0.0 as a double
    }

    // closestPointOnTriangle: see closest.h.
    void closestPointOnTriangle(const Vec4 *point, const Vec4 *v0, const Vec4 *v1, const Vec4 *v2, Vec4 *out,
                                ee::Float *squared)
    {
        // The triangle as v0 + s edge0 + t edge1, the point at v0 - toV0;
        // the quadratic |v0 + s edge0 + t edge1 - point|^2 =
        //   a s^2 + 2 b s t + c t^2 + 2 d s + 2 e t + f.
        const SquaredVector edge0 = *v1 - *v0;
        const SquaredVector edge1 = *v2 - *v0;
        const SquaredVector toV0 = *v0 - *point;
        const Float a = squaredLengthOf(edge0);
        const Float b = dot(edge0, edge1);
        const Float c = squaredLengthOf(edge1);
        const Float d = dot(edge0, toV0);
        const Float e = dot(edge1, toV0);
        const Float f = squaredLengthOf(toV0);

        // The unconstrained minimum's s and t, times det; then the region
        // it falls in decides which edge or corner is nearest (the FPU from
        // here on).
        const Float det = ee::abs(a * c - b * b);
        Float s = b * e - c * d;
        Float t = b * d - a * e;
        enum class Where
        {
            Inside,     // or on the edge s + t = 1: s and t as they are
            S1,         // the corner v1
            SOnEdge0,   // a point of edge0 (t = 0)
            T1,         // the corner v2
            TOnEdge1,   // a point of edge1 (s = 0)
            V0,         // the corner v0
        } where = Where::Inside;
        // Nearest on edge0 (t = 0) from the corner v0's side.
        auto onEdge0 = [&]()
        {
            if (a <= -d)
            {
                s = 1.0f;
                where = Where::S1;
            }
            else
            {
                s = -d / a;
                where = Where::SOnEdge0;
            }
        };
        // Nearest on edge1 (s = 0), when e says it is not v0. ZGE2 tests e against a double 0 where t is
        // negative too (region 4), against a float one elsewhere; ZGE3 against a float one.
        auto onEdge1 = [&](bool eAgainstDouble)
        {
            if (eAgainstDouble ? 0 <= sce::libgcc::dpcmp(sce::libgcc::fptodp(e), kZeroDouble) : 0.0f <= e)
            {
                t = 0.0f;
                where = Where::V0;
            }
            else if (c <= -e)
            {
                t = 1.0f;
                where = Where::T1;
            }
            else
            {
                t = -e / c;
                where = Where::TOnEdge1;
            }
        };
        // Nearest on the edge s + t = 1: s = numer / denom, unless past v1.
        auto onFarEdgeS = [&](Float numer)
        {
            const Float denom = (a - (b + b)) + c;
            if (!(denom <= numer))
            {
                s = numer / denom;
                t = 1.0f - s;
            }
            else
            {
                s = 1.0f;
                t = 0.0f;
                where = Where::S1;
            }
        };

        if (s + t <= det)
        {
            if (s < 0.0f)
            {
                if (t < 0.0f && d < 0.0f)
                {
                    // Region 4, nearer edge0.
                    t = 0.0f;
                    onEdge0();
                }
                else
                {
                    // Regions 3 and 4 nearer edge1.
                    s = 0.0f;
                    onEdge1(game::readable::kIsDm2 && t < 0.0f);
                }
            }
            else if (t < 0.0f)
            {
                // Region 5.
                t = 0.0f;
                if (0.0f <= d)
                {
                    s = 0.0f;
                    where = Where::V0;
                }
                else
                    onEdge0();
            }
            else
            {
                // Region 0: inside.
                const Float inverse = 1.0f / det;
                t = t * inverse;
                s = s * inverse;
            }
        }
        else if (s < 0.0f)
        {
            // Region 2: the far edge or edge1.
            const Float near = b + d, far = c + e;
            if (near < far)
                onFarEdgeS(far - near);
            else
            {
                s = 0.0f;
                if (far <= 0.0f)
                {
                    t = 1.0f;
                    where = Where::T1;
                }
                else if (0.0f <= e)
                {
                    t = 0.0f;
                    where = Where::V0;
                }
                else
                {
                    // Not past v2 (c + e > 0 says so already).
                    t = -e / c;
                    where = Where::TOnEdge1;
                }
            }
        }
        else if (t < 0.0f)
        {
            // Region 6: the far edge or edge0.
            const Float near = b + e, far = a + d;
            t = 0.0f;
            if (near < far)
            {
                const Float numer = far - near;
                const Float denom = (a - (b + b)) + c;
                if (!(denom <= numer))
                {
                    t = numer / denom;
                    s = 1.0f - t;
                }
                else
                {
                    t = 1.0f;
                    s = 0.0f;
                    where = Where::T1;
                }
            }
            else if (far <= 0.0f)
            {
                s = 1.0f;
                where = Where::S1;
            }
            else if (0.0f <= d)
            {
                s = 0.0f;
                where = Where::V0;
            }
            else
            {
                s = -d / a;
                where = Where::SOnEdge0;
            }
        }
        else
        {
            // Region 1: the far edge.
            const Float numer = ((c + e) - b) - d;
            if (numer <= 0.0f)
            {
                t = 1.0f;
                s = 0.0f;
                where = Where::T1;
            }
            else
                onFarEdgeS(numer);
        }
        if (squared)
        {
            // The quadratic at (s, t), worked out as each case can.
            switch (where)
            {
            case Where::S1: *squared = (a + (d + d)) + f; break;
            case Where::SOnEdge0: *squared = d * s + f; break;
            case Where::T1: *squared = (c + (e + e)) + f; break;
            case Where::TOnEdge1: *squared = e * t + f; break;
            case Where::V0: *squared = f; break;
            case Where::Inside:
                *squared = s * ((a * s + b * t) + (d + d)) + t * ((b * s + c * t) + (e + e)) + f;
                break;
            }
        }
        if (out)
        {
            // v0 + s edge0 + t edge1 (the two scaled edges in the frame too, w as VU0 left it).
            const VectorOnStack alongEdge0 = edge0 * s;
            const VectorOnStack alongEdge1 = edge1 * t;
            const Vec4 offset = *alongEdge0 + *alongEdge1;
            *out = offset + *v0;
        }
    }
    READABLE_FUNCTION_VU0(closestPointOnTriangle, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_streamChannelsOpen> g_streamChannelsOpen; // g_streamChannelsOpen; its name is a low-confidence guess (readable_names.tsv)
    }

}
