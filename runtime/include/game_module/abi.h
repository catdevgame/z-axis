#ifndef ZX_GAME_MODULE_ABI_H
#define ZX_GAME_MODULE_ABI_H

// The boundary between the runtime and a game module: the code recompiled from one game's ELF (generated functions,
// their table), built on its own, by the workshop's build or by the app from the player's disc, and loaded at boot.
//
// One table goes each way, plain C, no library type in either:
//   ZxRuntimeApi   what the module calls in the runtime (branches, the EE scheduler's checkpoints, memory outside
//                  RAM, the CPU's exceptions, the kernel's and libraries' replacements, the few C library functions
//                  the compiler may call), filled by the runtime (lib/game_module.cpp);
//   ZxGameModule   what the module gives back: its function table, the ELF it was recompiled from, and its VU1
//                  microprograms compiled ahead of time (ps2xVu1Aot's code, run on the runtime's VU1 core,
//                  runtime/ps2_vu1_core.h).
// A module exports one function, zx_game_module (zx_game_module_<id> when linked into the runner,
// PS2X_GAME_MODULES=static), that takes the runtime's table and returns its own, or null when it was built for another
// version of this boundary. ZX_GAME_MODULE_ABI changes with any change here, and with the order of the kernel and
// library lists (ps2_call_list.h), which the module calls by index.

#include <stddef.h>
#include <stdint.h>

#define ZX_GAME_MODULE_ABI 2u

#ifdef __cplusplus
extern "C" {
#endif

struct R5900Context;
// The runtime as a module sees it: a handle passed back on every call (the runtime's PS2Runtime).
typedef struct ZxRuntime ZxRuntime;
// A generated function: guest RAM, the EE's registers, the runtime.
typedef void (*ZxGuestFunction)(uint8_t *rdram, struct R5900Context *ctx, ZxRuntime *runtime);

// A VU1 microprogram compiled ahead of time, for the micro memory image with this hash (VU1Interpreter::codeHash): run
// on the runtime's VU1 core (a VU1Core and its run's context, runtime/ps2_vu1_core.h's VU1AotRun). The module's
// programs are a list.
typedef struct ZxVu1Program
{
    uint64_t hash;
    bool (*run)(void *core, void *runContext);
    const struct ZxVu1Program *next;
} ZxVu1Program;

// The function table tail calls read (the runtime's PS2Runtime::TailCallTable): the active game's, kept up to date
// by the runtime as functions are replaced (readable versions, game overrides).
typedef struct ZxTailCallTable
{
    uint32_t base;
    uint32_t slotCount;
    ZxGuestFunction *table;
    bool stats; // PS2X_TAILCALL_STATS: tail calls reaching a replaced function are counted (noteReplacedTailCall)
} ZxTailCallTable;

// PS2Runtime::GuestBranchKind.
enum ZxBranchKind
{
    ZX_BRANCH_DIRECT_JUMP = 0,
    ZX_BRANCH_DIRECT_CALL = 1,
    ZX_BRANCH_INDIRECT_JUMP = 2,
    ZX_BRANCH_INDIRECT_CALL = 3,
    ZX_BRANCH_RETURN = 4,
};

typedef struct ZxRuntimeApi
{
    uint32_t abi;              // ZX_GAME_MODULE_ABI
    uint32_t size;             // sizeof(ZxRuntimeApi)
    uint32_t cpuContextSize;   // sizeof(R5900Context)
    uint32_t cpuContextLayout; // zxCpuContextLayout(): its fields' offsets, hashed
    uint32_t vu1CoreSize;      // sizeof(VU1Core)
    uint32_t vu1CoreLayout;    // vu1CoreLayout(): its fields' offsets, hashed

    // Control flow: a call, jump or return that leaves the function (PS2Runtime::dispatchGuestBranch, false when
    // the guest thread unwinds); a checkpoint at a loop's back edge (PS2Runtime::eeCheckpointDue); tail calls.
    bool (*dispatchBranch)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t targetPc,
                           uint32_t sourcePc, uint32_t fallthroughPc, uint32_t kind, const char *debugName);
    bool (*checkpointDue)(ZxRuntime *runtime, uint32_t cycles);
    const ZxTailCallTable *tailCalls;
    void (*noteReplacedTailCall)(uint32_t sourcePc, uint32_t targetPc);

    // Loads and stores outside RAM (PS2Runtime::Load8.., Store8..: registers, scratchpad, VU memory, the BIOS); the
    // 128-bit ones through 16 bytes at `value`.
    uint8_t (*load8)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);
    uint16_t (*load16)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);
    uint32_t (*load32)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);
    uint64_t (*load64)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);
    void (*load128)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address, void *value);
    void (*store8)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address, uint8_t value);
    void (*store16)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address, uint16_t value);
    void (*store32)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address, uint32_t value);
    void (*store64)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address, uint64_t value);
    void (*store128)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address,
                     const void *value);

    // The CPU's own events: SYSCALL (its encoded number), BREAK, an exception (PS2Exception), SC's reservation,
    // COP0 Count, a VU0 microprogram (VCALLMS: started; executed to its end).
    void (*handleSyscall)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t encodedSyscallId);
    void (*handleBreak)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx);
    void (*signalException)(ZxRuntime *runtime, struct R5900Context *ctx, uint32_t exception);
    void (*clearLLBit)(ZxRuntime *runtime, struct R5900Context *ctx);
    uint32_t (*readCop0Count)(const ZxRuntime *runtime);
    void (*vu0StartMicroProgram)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);
    void (*vu0ExecuteMicroProgram)(ZxRuntime *runtime, uint8_t *rdram, struct R5900Context *ctx, uint32_t address);

    // The kernel's calls and the libraries' functions the runtime replaces, in the order of PS2_SYSCALL_LIST and
    // PS2_STUB_LIST (ps2_call_list.h).
    const ZxGuestFunction *syscalls;
    uint32_t syscallCount;
    const ZxGuestFunction *stubs;
    uint32_t stubCount;

    // The C library functions the compiler may call for the module's code (a module links no library).
    void *(*memcpy)(void *destination, const void *source, size_t size);
    void *(*memmove)(void *destination, const void *source, size_t size);
    void *(*memset)(void *destination, int value, size_t size);
    float (*floorf)(float x);
    float (*ceilf)(float x);
    float (*roundf)(float x);
    float (*truncf)(float x);
    double (*floor)(double x);
    double (*ceil)(double x);
    double (*round)(double x);
    double (*trunc)(double x);
} ZxRuntimeApi;

typedef struct ZxGameModule
{
    uint32_t abi;              // ZX_GAME_MODULE_ABI
    uint32_t size;             // sizeof(ZxGameModule)
    uint32_t cpuContextSize;   // as compiled into the module
    uint32_t cpuContextLayout; // as compiled into the module
    const char *id;            // the recompiler's namespace: "" (AI's, the runtime's default code), "dm2", "bmx"
    const char *elfName;       // the ELF's file name the code was recompiled from
    uint32_t base;             // the function table: the first address,
    uint32_t end;              // one past the last,
    uint32_t slotCount;        // its slots (one per 4 bytes),
    ZxGuestFunction *table;    // and the functions
    uint32_t vu1CoreSize;      // as compiled into the module
    uint32_t vu1CoreLayout;    // as compiled into the module
    const ZxVu1Program *vu1Programs; // the VU1 microprograms compiled ahead of time (none: null)
} ZxGameModule;

typedef const ZxGameModule *(*ZxGameModuleEntry)(const ZxRuntimeApi *api);

#ifdef __cplusplus
}
#endif

#endif // ZX_GAME_MODULE_ABI_H
