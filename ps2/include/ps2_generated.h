#ifndef PS2_GENERATED_H
#define PS2_GENERATED_H

// What the recompiler's output includes first: the CPU state, the instruction macros, the runtime's calls. A game
// module's build (ZX_GAME_MODULE: game_module/, engine/game_module/game_module.cmake, the app's first-run build) gets
// the module's view of the runtime, with no library header; anything else (the traced originals of zx-fncheck)
// gets the runtime's own headers.
#if defined(ZX_GAME_MODULE)
#include "game_module/module_runtime.h"
#include "ee_instructions.h"
#else
#include "machine.h"
#include "ee_instructions.h"
#include "stubs.h"
#include "syscalls.h"
#endif

// Generated code names the machine as the recompiler writes it (recompiler/ is PS2Recomp's, untouched).
using PS2Runtime = PS2Machine;

#endif // PS2_GENERATED_H
