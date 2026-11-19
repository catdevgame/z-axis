#pragma once

// The function table guest code dispatches through: the generated set
// chosen at ELF load (ps2_generated::select), as overrides changed it
// (PS2Runtime::replaceFunction). (ps2_runtime.cpp)

#include "ps2_runtime.h"

#include <cstdint>

namespace ps2x::function_table
{
    struct View
    {
        uint32_t base = 0;
        uint32_t end = 0;
        uint32_t slotCount = 0;
        PS2Runtime::RecompiledFunction *table = nullptr;
    };
    View active();
}
