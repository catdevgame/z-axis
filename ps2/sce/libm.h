#pragma once

// libm's functions the C library and the engine call by name (sce/libm.cpp, newlib 1.8.2's libm): a double travels
// as its 64-bit pattern, a float as the EE's (ee::Float).

#include "ee/ee.h"

#include <cstdint>

namespace sce::libm
{
    int32_t isinf(uint64_t x); // 1 if x is an infinity
    int32_t isnan(uint64_t x); // 1 if x is not a number
    ee::Float asinf(ee::Float x);  // the arc sine, radians
    ee::Float atanf(ee::Float x);  // the arc tangent, radians
    ee::Float floorf(ee::Float x); // the largest whole number not above x
}
