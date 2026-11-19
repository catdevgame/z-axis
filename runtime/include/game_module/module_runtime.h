#ifndef ZX_GAME_MODULE_RUNTIME_H
#define ZX_GAME_MODULE_RUNTIME_H

// The runtime as a game module's code sees it (PS2X_GAME_MODULE). Generated code calls the runtime as
// `runtime->dispatchGuestBranch(...)`, `ps2_stubs::sceGsSyncV(...)` and so on; here those calls go through the
// runtime's table (game_module/abi.h), so the module needs nothing of the runtime but this header. PS2Runtime names
// ZxRuntime here: the handle the runtime passes to every generated function, with the members generated code uses.
// Nothing is ever a ZxRuntime object: it stands for the runtime's own.

#if !defined(PS2X_GAME_MODULE)
#error "game_module/module_runtime.h is a game module's (PS2X_GAME_MODULE)"
#endif
#if defined(PS2_RUNTIME_H)
#error "a game module is compiled without the runtime's headers (ps2_runtime.h)"
#endif
#if defined(PS2_FUNCTION_LOG_TRACKER)
#error "PS2_FUNCTION_LOG_TRACKER: the function log is not in a game module (build the runner's generated code with the runtime's headers)"
#endif
#if !defined(PS2X_GAME_MODULE_ID)
#error "PS2X_GAME_MODULE_ID: the module's name (ai, dm2, bmx), set by the build"
#endif

#include "game_module/abi.h"
#include "game_module/freestanding_std.h"
#include "runtime/guest_cpu.h"
#include "runtime/ps2_address.h"
#include "ps2_call_list.h"

#if defined(_WIN32)
#define ZX_GAME_MODULE_HIDDEN
#else
#define ZX_GAME_MODULE_HIDDEN __attribute__((visibility("hidden")))
#endif

// A name of this module's own: zx_game_module_<name>_<id> (several modules link into one runner when static).
#define ZX_GAME_MODULE_CONCAT2(a, b) a##b
#define ZX_GAME_MODULE_CONCAT(a, b) ZX_GAME_MODULE_CONCAT2(a, b)
#define ZX_GAME_MODULE_NAME(name) ZX_GAME_MODULE_CONCAT(zx_game_module_##name##_, PS2X_GAME_MODULE_ID)

// The runtime's table, given to the module's entry (game_module/module_entry.cpp).
extern "C" ZX_GAME_MODULE_HIDDEN const ZxRuntimeApi *zx_game_module_api;
// What the module gives back: its function table, from the generated registration (ps2_generated::Registrar below).
extern "C" ZX_GAME_MODULE_HIDDEN ZxGameModule ZX_GAME_MODULE_NAME(code);

struct ZxRuntime
{
    enum class GuestBranchKind : uint32_t
    {
        DirectJump = ZX_BRANCH_DIRECT_JUMP,
        DirectCall = ZX_BRANCH_DIRECT_CALL,
        IndirectJump = ZX_BRANCH_INDIRECT_JUMP,
        IndirectCall = ZX_BRANCH_INDIRECT_CALL,
        Return = ZX_BRANCH_RETURN,
    };
    using RecompiledFunction = ZxGuestFunction;

    ZxRuntime() = delete;
    ZxRuntime(const ZxRuntime &) = delete;

    static bool isSpecialAddress(uint32_t address) { return Ps2IsSpecialAddress(address); }

    bool dispatchGuestBranch(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                             uint32_t fallthroughPc, GuestBranchKind kind, const char *debugName)
    {
        return zx_game_module_api->dispatchBranch(this, rdram, ctx, targetPc, sourcePc, fallthroughPc,
                                                  static_cast<uint32_t>(kind), debugName);
    }
    // (Not noexcept, unlike the runtime's: a call through the table that cannot throw needs no terminate handler,
    // which would be the C++ library's.)
    bool eeCheckpointDue(uint32_t cycles = 32u) { return zx_game_module_api->checkpointDue(this, cycles); }
    // As PS2Runtime::tailCall: the function in the table, `direct` when the table holds it (or nothing).
    void tailCall(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc, RecompiledFunction direct)
    {
        const ZxTailCallTable &table = *zx_game_module_api->tailCalls;
        const uint32_t slot = (targetPc - table.base) >> 2;
        const RecompiledFunction fn = slot < table.slotCount ? table.table[slot] : nullptr;
        if (fn == nullptr || fn == direct)
        {
            direct(rdram, ctx, this);
            return;
        }
        if (table.stats)
            zx_game_module_api->noteReplacedTailCall(sourcePc, targetPc);
        fn(rdram, ctx, this);
    }

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        return zx_game_module_api->load8(this, rdram, ctx, address);
    }
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        return zx_game_module_api->load16(this, rdram, ctx, address);
    }
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        return zx_game_module_api->load32(this, rdram, ctx, address);
    }
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        return zx_game_module_api->load64(this, rdram, ctx, address);
    }
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        __m128i value;
        zx_game_module_api->load128(this, rdram, ctx, address, &value);
        return value;
    }
    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t address, uint8_t value)
    {
        zx_game_module_api->store8(this, rdram, ctx, address, value);
    }
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t address, uint16_t value)
    {
        zx_game_module_api->store16(this, rdram, ctx, address, value);
    }
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t address, uint32_t value)
    {
        zx_game_module_api->store32(this, rdram, ctx, address, value);
    }
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t address, uint64_t value)
    {
        zx_game_module_api->store64(this, rdram, ctx, address, value);
    }
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t address, __m128i value)
    {
        zx_game_module_api->store128(this, rdram, ctx, address, &value);
    }

    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
    {
        zx_game_module_api->handleSyscall(this, rdram, ctx, encodedSyscallId);
    }
    void handleBreak(uint8_t *rdram, R5900Context *ctx) { zx_game_module_api->handleBreak(this, rdram, ctx); }
    void SignalException(R5900Context *ctx, PS2Exception exception)
    {
        zx_game_module_api->signalException(this, ctx, static_cast<uint32_t>(exception));
    }
    void clearLLBit(R5900Context *ctx) { zx_game_module_api->clearLLBit(this, ctx); }
    uint32_t readCop0Count() const { return zx_game_module_api->readCop0Count(this); }
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        zx_game_module_api->vu0StartMicroProgram(this, rdram, ctx, address);
    }
    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
    {
        zx_game_module_api->vu0ExecuteMicroProgram(this, rdram, ctx, address);
    }
};
using PS2Runtime = ZxRuntime;

// The kernel's calls and the libraries' functions the runtime replaces: by their index in ps2_call_list.h's lists.
enum ZxSyscallIndex : uint32_t
{
#define ZX_SYSCALL_INDEX(name) ZX_SYSCALL_##name,
    PS2_SYSCALL_LIST(ZX_SYSCALL_INDEX)
#undef ZX_SYSCALL_INDEX
    ZX_SYSCALL_COUNT
};
enum ZxStubIndex : uint32_t
{
#define ZX_STUB_INDEX(name) ZX_STUB_##name,
    PS2_STUB_LIST(ZX_STUB_INDEX)
#undef ZX_STUB_INDEX
    ZX_STUB_COUNT
};
namespace ps2_syscalls
{
#define ZX_SYSCALL_CALL(name)                                                    \
    inline void name(uint8_t *rdram, R5900Context *ctx, ZxRuntime *runtime)      \
    {                                                                            \
        zx_game_module_api->syscalls[ZX_SYSCALL_##name](rdram, ctx, runtime);    \
    }
    PS2_SYSCALL_LIST(ZX_SYSCALL_CALL)
#undef ZX_SYSCALL_CALL
}
namespace ps2_stubs
{
#define ZX_STUB_CALL(name)                                                       \
    inline void name(uint8_t *rdram, R5900Context *ctx, ZxRuntime *runtime)      \
    {                                                                            \
        zx_game_module_api->stubs[ZX_STUB_##name](rdram, ctx, runtime);          \
    }
    PS2_STUB_LIST(ZX_STUB_CALL)
#undef ZX_STUB_CALL
}

// The generated registration (register_functions.cpp) names the module's function table. Internal linkage: its
// constructor fills this module's descriptor, so the modules linked into one program (PS2X_GAME_MODULES=static)
// each keep their own copy; an inline one with external linkage would be merged into one, which would fill a
// single module's descriptor for all of them.
namespace ps2_generated
{
namespace
{
    struct Registrar
    {
        Registrar(const char *id, const char *elfName, uint32_t base, uint32_t end, uint32_t slotCount,
                  ZxGuestFunction *table)
        {
            ZxGameModule &code = ZX_GAME_MODULE_NAME(code);
            code.id = id;
            code.elfName = elfName;
            code.base = base;
            code.end = end;
            code.slotCount = slotCount;
            code.table = table;
        }
    };

    // An ahead-of-time compiled VU1 microprogram's place on the module's list (ps2_vu1_aot_register.h); internal
    // linkage for the same reason.
    struct Vu1Registrar
    {
        ZxVu1Program program;
        Vu1Registrar(uint64_t hash, bool (*run)(void *core, void *runContext)) : program{hash, run, nullptr}
        {
            ZxGameModule &code = ZX_GAME_MODULE_NAME(code);
            program.next = code.vu1Programs;
            code.vu1Programs = &program;
        }
    };
}
}
// AI's generated table has global names (the runtime's own when its code was linked in): the module's own here.
#define g_ps2RecompiledFunctionTableBase ZX_GAME_MODULE_NAME(tableBase)
#define g_ps2RecompiledFunctionTableEnd ZX_GAME_MODULE_NAME(tableEnd)
#define g_ps2RecompiledFunctionTableSlotCount ZX_GAME_MODULE_NAME(tableSlotCount)
#define g_ps2RecompiledFunctionTable ZX_GAME_MODULE_NAME(table)

// The function log (PS2_FUNCTION_LOG_TRACKER, _DEBUG stubs) is the runtime's alone.
#define PS_LOG_ENTRY(name) ((void)0)

#endif // ZX_GAME_MODULE_RUNTIME_H
