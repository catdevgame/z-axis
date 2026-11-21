#pragma once

// libgcc's routines that the engine, the game and the C library call by name (sce/libgcc.cpp): GCC 2.95's
// libgcc2.c 64-bit arithmetic and fp-bit's software doubles under their GOFAST names. A double travels as its
// 64-bit pattern (dp-bit), a float as an ee::Float.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace sce::libgcc
{
    uint64_t __muldi3(uint64_t u, uint64_t v);   // u * v
    uint64_t __udivdi3(uint64_t n, uint64_t dv); // n / dv
    uint64_t __umoddi3(uint64_t n, uint64_t dv); // n % dv
    uint64_t __floatdidf(int64_t u);             // (double) u
    ee::Float __floatdisf(int64_t u);            // (float) u (DM2 only: reach it from a step only DM2's instance keeps)
    int64_t __fixdfdi(uint64_t a);               // (long long) a
    uint64_t __fixunssfdi(ee::Float original);   // (unsigned long long) a float (DM2 only, as __floatdisf)

    uint64_t dpadd(uint64_t argA, uint64_t argB); // a + b
    uint64_t dpsub(uint64_t argA, uint64_t argB); // a - b
    uint64_t dpmul(uint64_t argA, uint64_t argB); // a * b
    uint64_t dpdiv(uint64_t argA, uint64_t argB); // a / b
    int32_t dpcmp(uint64_t argA, uint64_t argB);  // < 0, 0, > 0
    ee::Float dptofp(uint64_t arg);               // (float) a double
    uint64_t fptodp(ee::Float arg);               // (double) a float
    uint32_t fptoui(ee::Float arg);               // (unsigned) a float
    uint32_t dptoul(uint64_t argA);               // (unsigned long) a double
    int32_t dptoli(uint64_t argA);                // (int) a double
    uint64_t litodp(int32_t value);               // (double) an int

    // __main (libgcc2.c): main's first call, the global constructors once.
    int32_t __main();
}
