// The engine's cubic Bezier curve point and tangent (bezierPoint, bezierTangent
// in the math block; DM2 and BMX XXX have them word for word): the camera
// paths, the movers' tracks and the path followers' paths are such curves.

#include "curve.h"

#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sym/zge/math/curve.cpp.h"

namespace zge
{
    // bezierPoint: the point at t (0 .. 1) on the curve with control
    // points p[0] .. p[3] (Vec4 strides; x, y and z): Bernstein weights
    // (1-t)^3, 3t(1-t)^2, 3t^2(1-t), t^3, summed from the last point back.
    void bezierPoint(ee::Float t, const Vec4 *p, Vec3 *out)
    {
        const ee::Float threeT = t * 3.0f;
        const ee::Float u = ee::Float(1.0f) - t;
        const ee::Float w3 = t * t * t;
        const ee::Float uu = u * u;
        const ee::Float w2 = threeT * t * u;
        const ee::Float w1 = threeT * uu;
        const ee::Float w0 = uu * u;
        out->x = p[3].x * w3 + p[2].x * w2 + p[1].x * w1 + p[0].x * w0;
        out->y = p[3].y * w3 + p[2].y * w2 + p[1].y * w1 + p[0].y * w0;
        out->z = p[3].z * w3 + p[2].z * w2 + p[1].z * w1 + p[0].z * w0;
    }
    READABLE_FUNCTION(bezierPoint, Verified);

    namespace
    {
        // The range bezierTangent clamps t to [inferred: 0 and 1].
        inline ee::Global<ee::Float, sym::g_tangentLowT> g_tangentLowT;
        inline ee::Global<ee::Float, sym::g_tangentHighT> g_tangentHighT;
    }

    // bezierTangent: the curve's derivative at t (clamped to the range
    // above): the Bernstein weights' derivatives -3(1-t)^2, 3(1-t)^2 -
    // 6t(1-t), 6t(1-t) - 3t^2, 3t^2, summed from the last point back.
    void bezierTangent(ee::Float t, const Vec4 *p, Vec3 *out)
    {
        if (t <= *g_tangentLowT)
            t = *g_tangentLowT;
        if (*g_tangentHighT <= t)
            t = *g_tangentHighT;
        const ee::Float u = ee::Float(1.0f) - t;
        const ee::Float d3 = t * (t * 3.0f);
        const ee::Float tu6 = t * (u * 6.0f);
        const ee::Float uu3 = u * (u * 3.0f);
        const ee::Float d2 = tu6 - d3;
        const ee::Float d1 = uu3 - tu6;
        const ee::Float d0 = -uu3;
        out->x = p[3].x * d3 + p[2].x * d2 + p[1].x * d1 + p[0].x * d0;
        out->y = p[3].y * d3 + p[2].y * d2 + p[1].y * d1 + p[0].y * d0;
        out->z = p[3].z * d3 + p[2].z * d2 + p[1].z * d1 + p[0].z * d0;
    }
    READABLE_FUNCTION(bezierTangent, Verified);

    namespace
    {
        // nearestOnBezierSteps' first best distance: the largest float.
        inline ee::Global<ee::Float, sym::g_bezierLengthFloatMax> g_largestFloat;
        // bezierLengthDefault's tolerance (on the squared difference).
        constexpr ee::Float kDefaultTolerance = ee::Float(1e-05f);

        // A count as a float, as the compiled code converts an unsigned value.
        ee::Float unsignedToFloat(uint32_t count)
        {
            if (static_cast<int32_t>(count) >= 0)
                return ee::toFloat(static_cast<int32_t>(count));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((count & 1u) | (count >> 1)));
            return half + half;
        }
    }

    // subdivideBezierLength: the curve's length: the sum of its control
    // polygon's three sides, unless that differs from the chord (first point
    // to last) by more than tolerance allows (tolerance < difference
    // squared): then the curve is split in two at its middle and each half
    // measured the same way with half the tolerance.
    ee::Float subdivideBezierLength(ee::Ptr<Vec4> curve, ee::Float tolerance)
    {
        ee::Local<Vec4> side;
        ee::Local<Vec4[4]> left;
        ee::Local<Vec4[4]> right;
        *side = curve[1] - curve[0];
        const ee::Float first = vec3Length(&side->xyz());
        *side = curve[2] - curve[1];
        const ee::Float polygon = first + vec3Length(&side->xyz());
        *side = curve[3] - curve[2];
        const ee::Float sides = polygon + vec3Length(&side->xyz());
        *side = curve[3] - curve[0];
        const ee::Float difference = sides - vec3Length(&side->xyz());
        if (!(tolerance < difference * difference))
            return sides;
        splitBezier(curve, &left[1], &right[1], &left[3]);
        left[0] = quadwordOf(curve);
        right[3] = quadwordOf(curve + 3);
        right[0] = left[3];
        const ee::Float halfTolerance = tolerance * ee::Float(0.5f);
        // Each half measured by this function itself (a typed call by name: recursion).
        const ee::Float leftLength = subdivideBezierLength(ee::Ptr<Vec4>(left), halfTolerance);
        const ee::Float rightLength = subdivideBezierLength(ee::Ptr<Vec4>(right), halfTolerance);
        return leftLength + rightLength;
    }
    READABLE_FUNCTION(subdivideBezierLength, Verified);

    // nearestOnBezierSteps (was bezierLength): the curve's parameter nearest
    // point, from `steps` chords: for each chord, from the point at t back to
    // the one before, the nearest of its end at t, its start (t less a step)
    // or a point along it; *distanceOut (when given) the nearest distance.
    // As the original works it, the point along a chord is measured from its
    // start in the chord's backward direction, and its parameter added to t.
    ee::Float nearestOnBezierSteps(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> point, uint32_t steps,
                                    ee::Ptr<ee::Float> distanceOut)
    {
        ee::Local<Vec4> previous;
        ee::Local<Vec4> here;
        ee::Local<Vec4> back;
        ee::Local<Vec4> toPoint;
        ee::Local<Vec4> along;
        const ee::Float step = ee::Float(1.0f) / unsignedToFloat(steps);
        *previous = quadwordOf(curve);
        ee::Float nearestT = ee::Float(0.0f);
        ee::Float nearest = *g_largestFloat;
        ee::Float t = ee::Float(0.0f) + step;
        for (uint32_t i = 0; i < steps; ++i)
        {
            bezierPoint(t, curve, &here->xyz());
            *back = *previous - *here;
            const ee::Float length = vec3Length(&back->xyz());
            *back = *back * (ee::Float(1.0f) / length);
            *toPoint = *here - *point;
            ee::Float candidateT = t;
            if (dot(*toPoint, *back) < ee::Float(0.0f))
            {
                *toPoint = *previous - *point;
                const ee::Float past = dot(*toPoint, *back);
                if (ee::Float(0.0f) < past)
                {
                    along->x = previous->x + back->x * past;
                    along->y = previous->y + back->y * past;
                    along->z = previous->z + back->z * past;
                    candidateT = t + past / length * step;
                    *toPoint = *along - *point;
                }
                else
                    candidateT = t - step;
            }
            const ee::Float distance = vec3Length(&toPoint->xyz());
            if (distance < nearest)
            {
                nearest = distance;
                nearestT = candidateT;
            }
            *previous = *here;
            t = t + step;
        }
        if (distanceOut)
            *distanceOut = nearest;
        return nearestT;
    }
    READABLE_FUNCTION(nearestOnBezierSteps, Verified);

    // splitBezier: the curve split at its middle (de Casteljau, halves):
    // leftInner[0], leftInner[1] the left half's inner control points,
    // rightInner[0], rightInner[1] the right half's, middle the point both
    // share. Only x, y and z.
    void splitBezier(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> leftInner, ee::Ptr<Vec4> rightInner, ee::Ptr<Vec4> middle)
    {
        constexpr ee::Float kHalf = ee::Float(0.5f);
        vec3Lerp(&leftInner[0].xyz(), &curve[0].xyz(), &curve[1].xyz(), kHalf);
        vec3Lerp(&rightInner[1].xyz(), &curve[2].xyz(), &curve[3].xyz(), kHalf);
        vec3Lerp(&middle->xyz(), &curve[1].xyz(), &curve[2].xyz(), kHalf);
        vec3Lerp(&leftInner[1].xyz(), &leftInner[0].xyz(), &middle->xyz(), kHalf);
        vec3Lerp(&rightInner[0].xyz(), &rightInner[1].xyz(), &middle->xyz(), kHalf);
        game::tail(vec3Lerp, &middle->xyz(), &leftInner[1].xyz(), &rightInner[0].xyz(), kHalf); // the original's tail call
    }
    READABLE_FUNCTION(splitBezier, Verified);

    // bezierLengthDefault: the curve's length to the default tolerance.
    ee::Float bezierLengthDefault(ee::Ptr<Vec4> curve)
    {
        return subdivideBezierLength(curve, kDefaultTolerance);
    }
    READABLE_FUNCTION(bezierLengthDefault, Verified);

    // nearestOnBezier (was estimateBezierLength): nearestOnBezierSteps over
    // ten chords.
    ee::Float nearestOnBezier(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> point, ee::Ptr<ee::Float> distanceOut)
    {
        return nearestOnBezierSteps(curve, point, 10, distanceOut);
    }
    READABLE_FUNCTION(nearestOnBezier, Verified);
}
