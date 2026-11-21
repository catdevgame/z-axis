#pragma once

// The function table guest code dispatches through: the generated set
// chosen at ELF load (ps2_generated::select), as overrides changed it
// (PS2Machine::replaceFunction). (machine.cpp)
//
// A boot's changes are its own: when its runtime goes (~PS2Machine) every
// slot it replaced, in every table, gets its generated entry back, so the
// next boot in the process installs on the table as generated. Its hooks'
// originals (lookupFunction before replaceFunction) are then the generated
// functions, not the last boot's hooks; select() at the ELF load puts back
// a table chosen again too, but it runs only when the process has several
// games' code (a lone game module, Android's with one game set up, has none
// to choose).

#include "machine.h"

#include <cstddef>
#include <cstdint>

namespace zx::function_table
{
    struct View
    {
        uint32_t base = 0;
        uint32_t end = 0;
        uint32_t slotCount = 0;
        PS2Machine::RecompiledFunction *table = nullptr;
    };
    View active();

    // Every slot replaced since its table was generated or last put back,
    // in every table, back as generated (a boot's end); how many there were.
    size_t restoreGenerated();
}
