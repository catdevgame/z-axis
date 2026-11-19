// execLower is defined inline in ps2_vu1_exec_lower.h (included where it
// is called, so ahead-of-time compiled microprograms can specialize it).
#include "runtime/ps2_vu1.h"
#include "ps2_vu1_exec_lower.h"

VU1Interpreter::LowerFn VU1Interpreter::selectLowerFn(uint32_t instr)
{
    static const std::array<LowerFn, 320> table = makeLowerTable(std::make_index_sequence<320>{});
    uint32_t opHi = 0, funct = 0, funct2 = 0;
    lowerOpKey(instr, opHi, funct, funct2);
    if (opHi != 0x40u)
        return table[opHi];
    if (funct < 0x3Cu)
        return table[128u + funct];
    return table[192u + funct2];
}
