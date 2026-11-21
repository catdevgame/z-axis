#include "machine.h"
#include "runtime/memory.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;
PS2Machine::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};