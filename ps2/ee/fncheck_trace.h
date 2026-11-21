#pragma once

// What traced code (the recompiled originals built again with tracing,
// fncheck_traced.h) needs of fncheck: the active tracer, the I/O register
// hooks and the memory keys. Recording and checking themselves are in
// fncheck.h, which includes this.
//
// This header and those it includes are all the readable side that the
// traced originals see, so a change to the readable game's headers (ee.h,
// abi.h, readable.h) rebuilds only the readable code, not the six thousand
// traced originals.

#include "game_id.h"

#include "machine.h"

#include <cstdint>
#include <cstring>

class PS2Machine;

namespace fncheck
{
    // Memory is keyed by physical address: EE RAM at its physical address
    // (the uncached mirrors fold onto it), the scratchpad (16 KB at
    // the scratchpad) after RAM, at kScratchpadKey + its offset. Recordings,
    // their lines and the checker's replay memory use these keys.
    constexpr uint32_t kScratchpadKey = PS2_RAM_SIZE;
    constexpr uint32_t kMemoryKeys = PS2_RAM_SIZE + PS2_SCRATCHPAD_SIZE;
    inline uint32_t memoryKey(uint32_t address)
    {
        if (ps2IsScratchpadAddress(address))
            return kScratchpadKey + ps2ScratchpadOffset(address);
        return address & PS2_RAM_MASK;
    }
    // The host byte a key names: RAM in `rdram`, the scratchpad where the
    // runtime keeps it (ps2GetScratchpadHostPtr).
    inline uint8_t *keyPointer(uint8_t *rdram, uint32_t key)
    {
        if (key >= kScratchpadKey)
            return ps2GetScratchpadHostPtr() + (key - kScratchpadKey);
        return rdram + key;
    }

    // One access to an I/O register (an address PS2Machine::isSpecialAddress
    // takes) by the recorded function's own code, in order: the value read
    // or written, zero-extended (lo, then hi for a quadword).
    struct IoAccess
    {
        uint32_t address = 0;
        uint8_t size = 0;  // 1, 2, 4, 8 or 16 bytes
        uint8_t store = 0; // 1 a store, 0 a load
        uint16_t reserved = 0;
        uint64_t lo = 0, hi = 0;
    };
    static_assert(sizeof(IoAccess) == 24, "record layout");

    // Memory accesses and calls of traced code (fncheck_traced.h) go to the
    // active tracer, if any.
    class Tracer
    {
    public:
        virtual ~Tracer() = default;
        virtual void read(uint32_t address, uint32_t size) = 0;
        virtual void write(uint32_t address, uint32_t size) = 0;
        virtual void special(uint32_t address) = 0;
        virtual void beforeCall(uint8_t *rdram, R5900Context *ctx, uint32_t target) = 0;
        virtual void afterCall(uint8_t *rdram, R5900Context *ctx, bool returned) = 0;
        // The call just begun is a jump out of the function (a tail call);
        // `direct`: generated code makes it a direct C++ call (no dispatch).
        virtual void tailCall(bool direct) { (void)direct; }
        // EE cycles the traced code charged (its loops' safe points, its calls).
        virtual void cycles(uint32_t count) { (void)count; }
        // At a safe point of the traced code (a branch back, a call's
        // dispatch), after its cycles: whether a checkpoint is due there, so
        // that it unwinds as the game's does (zx-fncheck --safepoints; never
        // in the game).
        virtual bool safePointDue(uint32_t count) { (void)count; return false; }
        // An I/O register access of the traced code, with its value.
        virtual void io(const IoAccess &access) { (void)access; }
        // The traced code runs in the game (the recorder's): at every dispatch
        // it must do what the plain code does there, so a recorded run plays
        // as the unrecorded one; false in zx-fncheck's replays.
        virtual bool inGame() const { return false; }
    };
    inline thread_local Tracer *t_tracer = nullptr;

    // Where I/O register accesses go instead of the runtime: zx-fncheck
    // answers the loads from a recording and compares the stores (for the
    // traced original and for readable code, ee/io.h). Null in the game.
    class IoPort
    {
    public:
        virtual ~IoPort() = default;
        virtual void load(uint32_t address, uint32_t size, uint64_t &lo, uint64_t &hi) = 0;
        virtual void store(uint32_t address, uint32_t size, uint64_t lo, uint64_t hi) = 0;
    };
    inline thread_local IoPort *t_ioPort = nullptr;

    template <typename T>
    inline void ioSplit(const T &value, uint64_t &lo, uint64_t &hi)
    {
        uint64_t words[2] = {0, 0};
        std::memcpy(words, &value, sizeof(T));
        lo = words[0];
        hi = words[1];
    }
    template <typename T>
    inline T ioJoin(uint64_t lo, uint64_t hi)
    {
        const uint64_t words[2] = {lo, hi};
        T value;
        std::memcpy(&value, words, sizeof(T));
        return value;
    }

    // A load of an I/O register (`real` does it): from the IoPort if there
    // is one, else the runtime's, told to the tracer. The scratchpad is
    // memory however the code reaches it, the generated code's
    // runtime->LoadN at a constant address included: traced and replayed
    // as the READn macros do (drafter25: the recordings kept those accesses
    // in the I/O log and the others as memory, so a readable version had
    // to reach each byte the same way, ee::io or ee::at; readRecord turns
    // an older recording's scratchpad I/O into memory lines).
    template <typename T, typename Real>
    inline T ioLoad(uint32_t address, Real &&real)
    {
        if (ps2IsScratchpadAddress(address))
        {
            if (Tracer *t = t_tracer)
                t->read(address, static_cast<uint32_t>(sizeof(T)));
            T value;
            std::memcpy(&value, keyPointer(nullptr, memoryKey(address)), sizeof(T));
            return value;
        }
        IoAccess a;
        a.address = address;
        a.size = static_cast<uint8_t>(sizeof(T));
        if (IoPort *port = t_ioPort)
        {
            port->load(address, a.size, a.lo, a.hi);
            return ioJoin<T>(a.lo, a.hi);
        }
        const T value = real();
        if (Tracer *t = t_tracer)
        {
            ioSplit(value, a.lo, a.hi);
            t->io(a);
        }
        return value;
    }
    template <typename T, typename Real>
    inline void ioStore(uint32_t address, const T &value, Real &&real)
    {
        if (ps2IsScratchpadAddress(address))
        {
            if (Tracer *t = t_tracer)
                t->write(address, static_cast<uint32_t>(sizeof(T)));
            std::memcpy(keyPointer(nullptr, memoryKey(address)), &value, sizeof(T));
            return;
        }
        IoAccess a;
        a.address = address;
        a.size = static_cast<uint8_t>(sizeof(T));
        a.store = 1;
        ioSplit(value, a.lo, a.hi);
        if (IoPort *port = t_ioPort)
        {
            port->store(address, a.size, a.lo, a.hi);
            return;
        }
        if (Tracer *t = t_tracer)
            t->io(a);
        real();
    }

    inline void traceRead(uint32_t address, uint32_t size)
    {
        if (Tracer *t = t_tracer)
            t->read(address, size);
    }
    inline void traceWrite(uint32_t address, uint32_t size)
    {
        if (Tracer *t = t_tracer)
            t->write(address, size);
    }
    inline void traceSpecial(uint32_t address)
    {
        if (Tracer *t = t_tracer)
            t->special(address);
    }

    // A traced function's extent [start, end) (game.cmake reads it from the
    // generated file's "// Address:" line). A jump through a register to an
    // address inside the traced function (a jump table the recompiler did not
    // list, which generated code dispatches at run time: keepCameraEyeClear's) goes
    // on in the traced function there, instead of leaving it as a tail call.
    struct TracedExtent
    {
        TracedExtent(uint32_t start, uint32_t end, void (*traced)(uint8_t *, R5900Context *, PS2Machine *),
                     game::Game game = game::Game::Ai);
    };
    // The game whose traced functions run (whose extents tracedDispatch
    // looks in): set by installRecorder, and by zx-fncheck per recording.
    void setTracedGame(game::Game game);
    // A chunked function's chunks, as its traced original runs them in the
    // game (fncheck_traced.h runTracedChunks): where a recorded call of it
    // goes on (fncheck.cpp).
    bool tracedChunksKnown(uint32_t function);
    void noteTracedChunk(uint32_t function, uint32_t start, uint32_t end);
    // The end of the traced function's extent that starts at `start`, in the
    // traced game (0 if none is registered there).
    uint32_t tracedExtentEnd(uint32_t start);

    // PS2Machine::dispatchGuestBranch with the tracer told about calls.
    bool tracedDispatch(PS2Machine *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                        uint32_t fallthroughPc, int kind, const char *debugName);
}

namespace game::readable
{
    // The fncheck build of a recompiled function registers itself here
    // (ps2/ee/fncheck_traced.h), with the game whose generated code it was
    // built from (readable.cpp keeps them: tracedOriginal, ee/readable.h).
    struct TracedRegistration
    {
        TracedRegistration(uint32_t address, PS2Machine::RecompiledFunction traced, ::game::Game game = ::game::Game::Ai);
    };
}
