#ifndef PS2_VU1_AOT_REGISTER_H
#define PS2_VU1_AOT_REGISTER_H

// How an ahead-of-time compiled microprogram (ps2xVu1Aot's dispatcher source, ps2_vu1_aot.h) makes itself known:
//   in the runner, to the interpreter's registry (VU1Interpreter::registerAotProgram) before main;
//   in a game module (PS2X_GAME_MODULE), on the module's list (ZxGameModule::vu1Programs, game_module/abi.h), which the
//   runtime registers when it loads the module (lib/game_module.cpp).
// Either way the interpreter runs it whenever micro memory has this hash.
//
//   VU1_AOT_REGISTER(HASH)   after the definition of VU1AotProgram<HASH>::run (HASH: the image's, a 64-bit literal)

#include "ps2_vu1_aot.h"

// The program as the registry calls it (VU1AotRun): the core and its run's context.
template <uint64_t Hash>
bool vu1AotEntry(void *core, void *runContext)
{
    return VU1AotProgram<Hash>::run(*static_cast<VU1Core *>(core), *static_cast<VU1Core::RunContext *>(runContext));
}

#if defined(PS2X_GAME_MODULE)
#include "game_module/module_runtime.h"
#define VU1_AOT_REGISTER(hash) static const ps2_generated::Vu1Registrar kVu1Registered(hash, &vu1AotEntry<hash>);
#else
#include "runtime/ps2_vu1.h"
#define VU1_AOT_REGISTER(hash)                                                                                       \
    static const bool kVu1Registered = (VU1Interpreter::registerAotProgram(hash, &vu1AotEntry<hash>), true);
#endif

#endif
