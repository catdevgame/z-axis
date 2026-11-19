#pragma once

// Scaffolding (t0-leftover): docs/SCAFFOLDING.md says why and what removes it.
//
// The leftover the engine's scale macro carries into VU0 beside its factor
// (vec.h scaledCarrying), where it is not a vector the code can name.
//
// The macro hands the factor to VU0 through an integer register, all 128 bits
// of it: the factor is the low word, and the upper half is whatever the last
// 128-bit move through that register left. That lands in the z and w lanes of
// VU0's work register, and later vector code (in another function too) can
// read it there: in DM2 a skinned model's vertices carry it on.
//
// Where a known vector made that move (a quadword copy of it in the same
// function), pass the vector's upper half instead (AI's upperHalf). In DM2's
// rider nothing does: its only 128-bit moves through the register are the
// library's (a VU0 matrix copy's third row, the string routines' scratch),
// reached through callbacks, so what lands is what the code before left, from
// one of two places:
//
//   LeftBy::caller    as this function's caller left it (nothing since has
//                     changed it: the calls in between leave it alone)
//   LeftBy::lastCall  as the last call this function made left it
//
// Pass the one the original's code path has, so the lanes are the original's.

#include "ee/abi.h"

#include <cstdint>

namespace zge
{
    enum class LeftBy
    {
        caller,
        lastCall,
    };

    // The upper half the scale macro's register holds here, as `by` left it.
    inline uint64_t scaleLeftover(LeftBy by)
    {
        return by == LeftBy::caller ? ee::callerRegisterHigh(ee::reg::t0) : ee::leftByCallHigh(ee::reg::t0);
    }
}
