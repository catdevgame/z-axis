#ifndef PS2_GENERATED_H
#define PS2_GENERATED_H

// What the recompiler's output includes first: the CPU state, the instruction macros, the runtime's calls. A game
// module's build (ZX_GAME_MODULE: game_module/, runtime/cmake/game_module.cmake, the app's first-run build) gets
// the module's view of the runtime, with no library header; anything else (the traced originals of zx-fncheck)
// gets the runtime's own headers.
#if defined(ZX_GAME_MODULE)
#include "game_module/module_runtime.h"
#include "ps2_runtime_macros.h"
#else
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#endif

#endif // PS2_GENERATED_H
