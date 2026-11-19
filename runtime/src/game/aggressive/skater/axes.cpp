// The skater's axes made square again (readable round gamehand4).

#include "skater.h"

#include "ee/readable.h"
#include "zge/math/vec.h"
#include "sym/aggressive/skater/axes.cpp.h"

namespace aggressive::skater
{
    // orthonormalize (kOrthonormalize): forward = unit(up x right), then
    // right = unit(forward x up); up is kept. All on VU0 (the cross
    // products' w 0): its callers go on with what it leaves there, so it
    // is registered as a VU0 function.
    void orthonormalize(ee::Ptr<zge::Vec4> forward, ee::Ptr<zge::Vec4> right, ee::Ptr<const zge::Vec4> up)
    {
        *forward = zge::cross(*up, *right);
        *forward = zge::unit(*forward);
        *right = zge::cross(*forward, *up);
        *right = zge::unit(*right);
    }
    READABLE_FUNCTION_VU0(orthonormalize, Verified);

    // (a, b): b / a x g_radiansToDegrees.
    ee::Float scaledRatio(ee::Float denominator, ee::Float numerator)
    {
        return numerator / denominator * ee::Float(57.295776f);
    }
    READABLE_FUNCTION(scaledRatio, Draft);
}
