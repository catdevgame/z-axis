#pragma once

// The engine's cubic Bezier curves (curve.cpp): four control points, Vec4
// strides, x, y and z used.

#include "types.h"

namespace zge
{
    // bezierPoint, bezierTangent: the point at t (0 .. 1) on the curve p[0] ..
    // p[3], and its derivative there (t clamped to the curve).
    void bezierPoint(ee::Float t, const Vec4 *p, Vec3 *out);
    void bezierTangent(ee::Float t, const Vec4 *p, Vec3 *out);

    // subdivideBezierLength: the curve's length, the halves measured again
    // with half the tolerance while the control polygon and the chord differ
    // by more than it. bezierLengthDefault: to the default tolerance.
    ee::Float subdivideBezierLength(ee::Ptr<Vec4> curve, ee::Float tolerance);
    ee::Float bezierLengthDefault(ee::Ptr<Vec4> curve);

    // nearestOnBezierSteps: the parameter of the curve's point nearest point,
    // from `steps` chords (*distanceOut, when given, the distance);
    // nearestOnBezier: over ten.
    ee::Float nearestOnBezierSteps(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> point, uint32_t steps, ee::Ptr<ee::Float> distanceOut);
    ee::Float nearestOnBezier(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> point, ee::Ptr<ee::Float> distanceOut);

    // splitBezier: the curve split at its middle: each half's two inner
    // control points and the point both share.
    void splitBezier(ee::Ptr<Vec4> curve, ee::Ptr<Vec4> leftInner, ee::Ptr<Vec4> rightInner, ee::Ptr<Vec4> middle);
}
