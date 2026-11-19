#pragma once

// Builds a recompiled function a second time with its memory accesses and
// calls reported to fncheck's tracer (game/ee/fncheck.h). game/game.cmake
// writes one source per registered function:
//
//     #include "game/ee/fncheck_traced.h"
//     #define FUN_00148408_0x148408 fncheck_traced_00148408
//     #include "blocks/FUN_00148408_0x148408.cpp"   // the generated file, with block markers
//     FNCHECK_TRACED(address, fncheck_traced_00148408)
//
// and for another game's function (its generated code namespaced, the
// file defining it inside `namespace dm2`):
//
//     #define FUN_001a33f0_0x1a33f0 fncheck_traced_dm2_001a33f0
//     #include "blocks/FUN_001a33f0_0x1a33f0.cpp"   // from DM2's generated-ns, with block markers
//     FNCHECK_TRACED_IN(dm2, sym::riderFrame, dm2::fncheck_traced_riderFrame)
//
// The included file is the generated one with a block marker at the start
// of each basic block (blocks/<stem>.cpp, tools/fncheck_traced_gen.py; path
// coverage, fncheck_build.h); the generated code is otherwise unchanged; only
// the macros it is written in are:
// loads and stores (READ*/WRITE*, FAST_*) tell the tracer first, and its
// calls go through TracedRuntime::dispatchGuestBranch (PS2Runtime is
// renamed to it for the included file), which tells the tracer around
// each call. Without an active tracer the function runs as the original.

#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include <ps2_recompiled_functions.h>
#include <ps2_recompiled_stubs.h>

// (fncheck_trace.h, not fncheck.h or readable.h: the traced originals see
// none of the readable game's headers, so a change there leaves them be)
#include "fncheck_build.h"
#include "fncheck_trace.h"
#include "runtime/ee_scheduler.h"

namespace fncheck
{
    inline void tailCallTo(uint32_t target, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // Set when traced code leaves by a dispatch (a call that did not come
    // back, a jump, a return, a tail call) or at a due checkpoint, cleared by
    // a call that came back: how runTracedChunks (below) tells a chunk that
    // went on to another of its function's chunks from one that left.
    inline thread_local bool t_chunkLeft = false;
    // What a traced function's tail calls name as their recompiled target
    // (game.cmake): TracedRuntime::tailCall never calls it.
    inline void tailCallPlaceholder(uint8_t *, R5900Context *, PS2Runtime *) {}

    // Never constructed: generated code gets the real runtime, seen
    // through this type so that its calls are traced.
    struct TracedRuntime : PS2Runtime
    {
        // A backward branch's safe point: counted, then the runtime's.
        bool eeCheckpointDue(uint32_t cycles = 32u) noexcept
        {
            if (Tracer *tracer = t_tracer)
            {
                tracer->cycles(cycles);
                if (tracer->safePointDue(cycles))
                    return t_chunkLeft = true; // unwinds (ps2xFnCheck --safepoints)
            }
            const bool due = PS2Runtime::eeCheckpointDue(cycles);
            if (due)
                t_chunkLeft = true;
            return due;
        }

        bool dispatchGuestBranch(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                                 uint32_t fallthroughPc, GuestBranchKind kind, const char *debugName)
        {
            const bool back = tracedDispatch(this, rdram, ctx, targetPc, sourcePc, fallthroughPc,
                                             static_cast<int>(kind), debugName);
            t_chunkLeft = !back;
            return back;
        }

        // I/O registers (the macros' accesses to special addresses, and the
        // generated code's direct runtime->LoadN / StoreN at constant I/O
        // addresses): kept with their values (fncheck::ioLoad / ioStore), or
        // answered by ps2xFnCheck.
#define FNCHECK_IO_ACCESSORS(N, T)                                                                                 \
        T Load##N(uint8_t *rdram, R5900Context *ctx, uint32_t address)                                             \
        {                                                                                                          \
            return ioLoad<T>(address, [&] { return PS2Runtime::Load##N(rdram, ctx, address); });                   \
        }                                                                                                          \
        void Store##N(uint8_t *rdram, R5900Context *ctx, uint32_t address, T value)                                \
        {                                                                                                          \
            ioStore<T>(address, value, [&] { PS2Runtime::Store##N(rdram, ctx, address, value); });                 \
        }
        FNCHECK_IO_ACCESSORS(8, uint8_t)
        FNCHECK_IO_ACCESSORS(16, uint16_t)
        FNCHECK_IO_ACCESSORS(32, uint32_t)
        FNCHECK_IO_ACCESSORS(64, uint64_t)
        FNCHECK_IO_ACCESSORS(128, __m128i)
#undef FNCHECK_IO_ACCESSORS

        // A tail call (PS2Runtime::tailCall): the tracer's.
        void tailCall(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t, RecompiledFunction)
        {
            tailCallTo(targetPc, rdram, ctx, this);
        }
    };

    // Generated code compiles a jump to a known function start (a tail call)
    // as a call of the function in the table (PS2Runtime::tailCall; code
    // generated before round 11: a direct call, which game.cmake redirects
    // by a macro). In a traced build those calls come here: while recording or checking, the tail call is the tracer's (the
    // function returns with pc at the target, and the recorder or checker
    // runs or answers it); otherwise the function in the table runs, as the
    // dispatcher would.
    inline void tailCallTo(uint32_t target, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        t_chunkLeft = true;
        if (Tracer *tracer = t_tracer)
        {
            // (No dispatch charged: in the game this is a direct C++ call.)
            tracer->beforeCall(rdram, ctx, target);
            tracer->tailCall(true);
            ctx->pc = target;
            return;
        }
        runtime->lookupFunction(target)(rdram, ctx, runtime);
    }

    template <void (*Traced)(uint8_t *, R5900Context *, TracedRuntime *)>
    void tracedEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Traced(rdram, ctx, static_cast<TracedRuntime *>(runtime));
    }

    // A function the recompiler wrote as several entry_ chunks (one that the
    // Ghidra map has inside another function's range: AI's bindTexture,
    // entry_1049d0 to entry_104abc), each returning with pc at the next one
    // (a fall through, or a branch into another chunk), which the scheduler
    // dispatches in the game. Its traced original is the whole function
    // (game.cmake, FNCHECK_TRACED_CHUNKS): the chunk that holds pc, then the
    // chunk at pc for as long as a chunk ends at the start of one of the
    // function's chunks without having left (t_chunkLeft), each such step
    // the scheduler's dispatch of the chunk (its cycles and its checkpoint: a
    // safe point). A chunk left any other way (its return, a jump or tail
    // call out, a call that did not come back, a due checkpoint, the hop's
    // included) ends it there, as a whole function's
    // traced code ends, and the recorder and ps2xFnCheck go on from that pc
    // as they do for any traced original (a resume point comes back here, to
    // the chunk that holds it).
    struct TracedChunk
    {
        uint32_t start, end; // [start, end), in address order, each starting where the one before ends
        void (*run)(uint8_t *, R5900Context *, TracedRuntime *);
    };
    inline void runTracedChunks(const TracedChunk *chunks, size_t count, uint8_t *rdram, R5900Context *ctx,
                                TracedRuntime *runtime)
    {
        const TracedChunk *chunk = &chunks[0];
        for (size_t i = 0; i < count; ++i)
            if (ctx->pc >= chunks[i].start && ctx->pc < chunks[i].end)
                chunk = &chunks[i];
        const bool outer = t_chunkLeft;
        for (;;)
        {
            t_chunkLeft = false;
            chunk->run(rdram, ctx, runtime);
            if (t_chunkLeft)
                return; // (left: stays set for a jump table's dispatch that ran this, tracedDispatch)
            const TracedChunk *next = nullptr;
            for (size_t i = 0; i < count && !next; ++i)
                if (ctx->pc == chunks[i].start)
                    next = &chunks[i];
            if (!next)
            {
                t_chunkLeft = outer;
                return;
            }
            // The scheduler's dispatch of the next chunk (EeScheduler's slice dispatch: its 8 cycles and a
            // checkpoint): due there, the function unwinds with pc at the chunk, as the game's does, and is resumed
            // there (ps2xFnCheck --safepoints: an interruption at the hop).
            if (runtime->eeCheckpointDue(EeScheduler::kGuestDispatchCycles))
                return;
            chunk = next;
        }
    }
}

#undef FAST_READ8
#undef FAST_READ16
#undef FAST_READ32
#undef FAST_READ64
#undef FAST_READ128
#undef FAST_WRITE8
#undef FAST_WRITE16
#undef FAST_WRITE32
#undef FAST_WRITE64
#undef FAST_WRITE128
#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128

#define FAST_READ8(addr) ([&]() -> uint8_t { const uint32_t _fa = (uint32_t)(addr); ::fncheck::traceRead(_fa, 1u); return Ps2FastRead8(rdram, _fa); }())
#define FAST_READ16(addr) ([&]() -> uint16_t { const uint32_t _fa = (uint32_t)(addr); ::fncheck::traceRead(_fa, 2u); return Ps2FastRead16(rdram, _fa); }())
#define FAST_READ32(addr) ([&]() -> uint32_t { const uint32_t _fa = (uint32_t)(addr); ::fncheck::traceRead(_fa, 4u); return Ps2FastRead32(rdram, _fa); }())
#define FAST_READ64(addr) ([&]() -> uint64_t { const uint32_t _fa = (uint32_t)(addr); ::fncheck::traceRead(_fa, 8u); return Ps2FastRead64(rdram, _fa); }())
#define FAST_READ128(addr) ([&]() -> __m128i { const uint32_t _fa = (uint32_t)(addr); ::fncheck::traceRead(_fa, 16u); return Ps2FastRead128(rdram, _fa); }())

#define FAST_WRITE8(addr, val) (::fncheck::traceWrite((uint32_t)(addr), 1u), Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val)))
#define FAST_WRITE16(addr, val) (::fncheck::traceWrite((uint32_t)(addr), 2u), Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val)))
#define FAST_WRITE32(addr, val) (::fncheck::traceWrite((uint32_t)(addr), 4u), Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val)))
#define FAST_WRITE64(addr, val) (::fncheck::traceWrite((uint32_t)(addr), 8u), Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val)))
#define FAST_WRITE128(addr, val) (::fncheck::traceWrite((uint32_t)(addr), 16u), Ps2FastWrite128(rdram, (uint32_t)(addr), (val)))

#define FNCHECK_READ(ADDR, LOAD, FAST, TYPE)                                                                             \
    ([&]() -> TYPE {                                                                                               \
        uint32_t _addr = (uint32_t)(ADDR);                                                                         \
        if (ps2IsScratchpadAddress(_addr))                                                                         \
        {                                                                                                          \
            ::fncheck::traceRead(_addr, (uint32_t)sizeof(TYPE));                                                   \
            TYPE _v;                                                                                               \
            std::memcpy(&_v, ::fncheck::keyPointer(rdram, ::fncheck::memoryKey(_addr)), sizeof(TYPE));             \
            return _v;                                                                                             \
        }                                                                                                          \
        if (PS2Runtime::isSpecialAddress(_addr))                                                                   \
            return runtime->LOAD(rdram, ctx, _addr); /* TracedRuntime: kept with its value */                      \
        return FAST(_addr);                                                                                        \
    }())

#define READ8(addr) FNCHECK_READ(addr, Load8, FAST_READ8, uint8_t)
#define READ16(addr) FNCHECK_READ(addr, Load16, FAST_READ16, uint16_t)
#define READ32(addr) FNCHECK_READ(addr, Load32, FAST_READ32, uint32_t)
#define READ64(addr) FNCHECK_READ(addr, Load64, FAST_READ64, uint64_t)
#define READ128(addr) FNCHECK_READ(addr, Load128, FAST_READ128, __m128i)

#define FNCHECK_WRITE(ADDR, STORE, FAST, VALUE, TYPE)                                                                         \
    do                                                                                                             \
    {                                                                                                              \
        uint32_t _addr = (ADDR);                                                                                   \
        auto _value = (VALUE);                                                                                     \
        if (ps2IsScratchpadAddress(_addr))                                                                         \
        {                                                                                                          \
            const TYPE _typed = (TYPE)(_value);                                                                    \
            ::fncheck::traceWrite(_addr, (uint32_t)sizeof(TYPE));                                                  \
            std::memcpy(::fncheck::keyPointer(rdram, ::fncheck::memoryKey(_addr)), &_typed, sizeof(TYPE));         \
        }                                                                                                          \
        else if (PS2Runtime::isSpecialAddress(_addr))                                                              \
            runtime->STORE(rdram, ctx, _addr, _value); /* TracedRuntime: kept with its value */                    \
        else                                                                                                       \
            FAST(_addr, _value);                                                                                   \
    } while (0)

#define WRITE8(addr, val) FNCHECK_WRITE(addr, Store8, FAST_WRITE8, (val), uint8_t)
#define WRITE16(addr, val) FNCHECK_WRITE(addr, Store16, FAST_WRITE16, (val), uint16_t)
#define WRITE32(addr, val) FNCHECK_WRITE(addr, Store32, FAST_WRITE32, (val), uint32_t)
#define WRITE64(addr, val) FNCHECK_WRITE(addr, Store64, FAST_WRITE64, (val), uint64_t)
#define WRITE128(addr, val) FNCHECK_WRITE(addr, Store128, FAST_WRITE128, (val), __m128i)

// From here on the included generated function sees the traced runtime.
#define PS2Runtime ::fncheck::TracedRuntime

// (a name of its own in each use: PS2X_TRACED_UNITY puts many in one translation unit)
#define FNCHECK_CONCAT2(a, b) a##b
#define FNCHECK_CONCAT(a, b) FNCHECK_CONCAT2(a, b)
#define FNCHECK_UNIQUE(name) FNCHECK_CONCAT(name, __COUNTER__)
#define FNCHECK_TRACED(ADDRESS, SYMBOL)                                                                            \
    static ::game::readable::TracedRegistration FNCHECK_UNIQUE(g_fncheckTraced)(ADDRESS, &::fncheck::tracedEntry<&SYMBOL>);
#define FNCHECK_TRACED_EXTENT(ADDRESS, END, SYMBOL)                                                                \
    static ::fncheck::TracedExtent FNCHECK_UNIQUE(g_fncheckTracedExtent)(ADDRESS, END, &::fncheck::tracedEntry<&SYMBOL>);

// A function of several entry_ chunks (runTracedChunks): SYMBOL runs them,
// each chunk given as {start, end, &its traced name}.
#define FNCHECK_TRACED_CHUNKS(SYMBOL, ...)                                                                         \
    namespace                                                                                                      \
    {                                                                                                              \
        const ::fncheck::TracedChunk FNCHECK_CONCAT(SYMBOL, _chunks)[] = {__VA_ARGS__};                            \
    }                                                                                                              \
    void SYMBOL(uint8_t *rdram, R5900Context *ctx, ::fncheck::TracedRuntime *runtime)                              \
    {                                                                                                              \
        ::fncheck::runTracedChunks(FNCHECK_CONCAT(SYMBOL, _chunks),                                                \
                                   sizeof(FNCHECK_CONCAT(SYMBOL, _chunks)) / sizeof(::fncheck::TracedChunk), rdram, \
                                   ctx, runtime);                                                                  \
    }

// Another game's (GAME: dm2, ee/game_id.h), built from its namespaced
// generated code (game.cmake): SYMBOL is in the game's namespace.
#define FNCHECK_TRACED_IN(GAME, ADDRESS, SYMBOL)                                                                   \
    static ::game::readable::TracedRegistration FNCHECK_UNIQUE(g_fncheckTraced)(ADDRESS, &::fncheck::tracedEntry<&SYMBOL>, \
                                                                                ::game::games::GAME);
#define FNCHECK_TRACED_EXTENT_IN(GAME, ADDRESS, END, SYMBOL)                                                       \
    static ::fncheck::TracedExtent FNCHECK_UNIQUE(g_fncheckTracedExtent)(ADDRESS, END, &::fncheck::tracedEntry<&SYMBOL>, \
                                                                         ::game::games::GAME);
