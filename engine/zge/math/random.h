#pragma once

// The engine's random numbers and its math set-up (random.cpp).

#include <cstdint>

namespace zge
{
    // randomNumber: the next number of the engine's generator (one seed for
    // the whole game).
    uint32_t randomNumber();

    // initMath, shutdownMath: the math module's start and end (1: done).
    uint32_t initMath();
    uint32_t shutdownMath();
}
