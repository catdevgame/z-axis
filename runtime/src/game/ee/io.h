#pragma once

// I/O registers from readable code: the EE's hardware registers (DMA
// channels, VIF, GIF, INTC) and the other addresses
// the runtime handles itself (PS2Runtime::isSpecialAddress), which are not
// RAM, so not ee::at.
//
//     const uint32_t stat = ee::io::load<uint32_t>(kVif1Stat);
//     ee::io::store<uint32_t>(kD2Qwc, 0);
//     ee::io::enableInterrupts();                    // EI
//
// In the game they go to the runtime (PS2Runtime::LoadN / StoreN, as the
// generated code's accesses do, with the same side effects: a store to a
// channel's CHCR starts it). ps2xFnCheck answers the loads from the
// recording's I/O log and compares the stores with it, in order
// (fncheck::IoPort; the recorder keeps the traced original's accesses,
// fncheck.h Record::io).

#include "abi.h"
#include "plain.h"
#include "fncheck.h"
#include "ps2_runtime.h"
#include "runtime/ps2_address.h"

#include <cstdint>

namespace ee::io
{
    template <typename T>
    T load(uint32_t address)
    {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "an I/O load");
        // The scratchpad is memory (its bytes the same whether reached
        // through here or ee::at; the recordings keep it so: drafter25).
        if (ps2IsScratchpadAddress(address))
            return *at<T>(address);
        if (fncheck::IoPort *port = fncheck::t_ioPort)
        {
            uint64_t lo = 0, hi = 0;
            port->load(address, sizeof(T), lo, hi);
            return fncheck::ioJoin<T>(lo, hi);
        }
        // (Not told to a recorder's tracer: a recording keeps the traced
        // original's own accesses only.)
        return [&]() -> T {
            const Frame *frame = t_ee.frame;
            PS2Runtime *runtime = frame->runtime;
            if constexpr (sizeof(T) == 1)
                return static_cast<T>(runtime->Load8(frame->rdram, frame->ctx, address));
            else if constexpr (sizeof(T) == 2)
                return static_cast<T>(runtime->Load16(frame->rdram, frame->ctx, address));
            else if constexpr (sizeof(T) == 4)
                return static_cast<T>(runtime->Load32(frame->rdram, frame->ctx, address));
            else
                return static_cast<T>(runtime->Load64(frame->rdram, frame->ctx, address));
        }();
    }

    template <typename T>
    void store(uint32_t address, T value)
    {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "an I/O store");
        if (ps2IsScratchpadAddress(address))
        {
            *at<T>(address) = value;
            return;
        }
        if (fncheck::IoPort *port = fncheck::t_ioPort)
        {
            uint64_t lo = 0, hi = 0;
            fncheck::ioSplit(value, lo, hi);
            port->store(address, sizeof(T), lo, hi);
            return;
        }
        {
            const Frame *frame = t_ee.frame;
            PS2Runtime *runtime = frame->runtime;
            if constexpr (sizeof(T) == 1)
                runtime->Store8(frame->rdram, frame->ctx, address, static_cast<uint8_t>(value));
            else if constexpr (sizeof(T) == 2)
                runtime->Store16(frame->rdram, frame->ctx, address, static_cast<uint16_t>(value));
            else if constexpr (sizeof(T) == 4)
                runtime->Store32(frame->rdram, frame->ctx, address, static_cast<uint32_t>(value));
            else
                runtime->Store64(frame->rdram, frame->ctx, address, static_cast<uint64_t>(value));
        }
    }

    // LQ and SQ of an I/O register: one 16-byte access (a FIFO takes a
    // quadword at once, not two doublewords).
    inline Qword loadQuadword(uint32_t address)
    {
        Qword value;
        if (fncheck::IoPort *port = fncheck::t_ioPort)
        {
            port->load(address, 16u, value.low, value.high);
            return value;
        }
        const Frame *frame = t_ee.frame;
        const __m128i bits = frame->runtime->Load128(frame->rdram, frame->ctx, address);
        std::memcpy(&value, &bits, 16);
        return value;
    }

    inline void storeQuadword(uint32_t address, Qword value)
    {
        if (fncheck::IoPort *port = fncheck::t_ioPort)
        {
            port->store(address, 16u, value.low, value.high);
            return;
        }
        __m128i bits;
        std::memcpy(&bits, &value, 16);
        const Frame *frame = t_ee.frame;
        frame->runtime->Store128(frame->rdram, frame->ctx, address, bits);
    }

    // An address that is an I/O register to readable code: one the runtime
    // handles itself (PS2Runtime::isSpecialAddress) but the scratchpad, which
    // ee::at reaches as memory (as the recorder keeps it).
    inline bool isRegister(uint32_t address)
    {
        return Ps2IsSpecialAddress(address) && !ps2IsScratchpadAddress(address);
    }

    // Memory through a pointer that may be an I/O register (a DMA channel's
    // registers handed in by address, as libdma does): decided at run time,
    // as the generated code's accesses are.
    template <typename T>
    T loadAny(uint32_t address)
    {
        if (isRegister(address))
            return load<T>(address);
        return *at<T>(address);
    }
    template <typename T>
    void storeAny(uint32_t address, T value)
    {
        if (isRegister(address))
            store<T>(address, value);
        else
            *at<T>(address) = value;
    }

    // EI: interrupts enabled (COP0 Status.EIE, as generated code sets it;
    // ps2xFnCheck compares the status a function leaves).
    inline void enableInterrupts() { t_ee.frame->ctx->cop0_status |= 0x10000u; }
    // DI.
    inline void disableInterrupts() { t_ee.frame->ctx->cop0_status &= ~0x10000u; }

    // MFC0 rt, Count: the EE's cycle counter, what the recompiled instruction
    // reads (PS2Runtime::readCop0Count: the scheduler's EE cycle plus the
    // bias an MTC0 Count set), kept in the context as it does. It charges
    // nothing; the value depends on the cycles charged so far, so read it
    // where the original does relative to its loops and calls (ps2xFnCheck
    // starts Count at 0 for both replays: readCycleCounter's timer).
    //
    //     const uint32_t now = ee::io::cop0Count();   // mfc0 $a0, Count
    inline uint32_t cop0Count()
    {
        const Frame *frame = t_ee.frame;
        frame->ctx->cop0_count = frame->runtime->readCop0Count();
        return frame->ctx->cop0_count;
    }
    // MFPS / MFPC (COP0 register 25, the performance counters: performanceCounter0
    // MFPC 0, performanceCounter1 MFPC 1): the recompiled code reads the one word it
    // keeps for register 25 (R5900Context::cop0_perf) whichever counter is
    // named, so this does too.
    inline uint32_t cop0Perf() { return t_ee.frame->ctx->cop0_perf; }
    // MFC0 rt, Status (the word EI and DI change).
    inline uint32_t cop0Status() { return t_ee.frame->ctx->cop0_status; }
    // The CPU's own registers, for the few places that are about them: what an
    // interrupt handler keeps of the interrupted code.
    inline uint32_t globalPointerRegister() { return static_cast<uint32_t>(callerRegister(28)); }
    inline Float floatRegister(int f) { return Float(callerFloat(f)); }
    inline void setFloatRegister(int f, Float value) { t_ee.frame->ctx->f[f] = value.raw(); }

    // A stack argument of the running function, the ninth on: what its caller
    // left at its sp.
    template <typename T>
    T stackArgument(uint32_t index)
    {
        return *at<T>(t_ee.frame->fnSp + index * 8u);
    }
    // The original's jump to itself: nothing more happens on this thread, time
    // goes on.
    [[noreturn]] void hang();

    // An address as a number, as C code casts a pointer.
    template <typename T>
    uint32_t numberOf(Ptr<T> pointer) { return pointer.address; }
    inline uint32_t numberOf(Address address) { return address.address; }
    // MFC0 of any other register, the word the recompiled instruction reads
    // (ps2xRecomp cop0_translator.cpp; the kernel's and libpc's few uses).
    inline uint32_t cop0Register(int rd)
    {
        const R5900Context *ctx = t_ee.frame->ctx;
        switch (rd)
        {
        case 0: return ctx->cop0_index;
        case 1: return ctx->cop0_random;
        case 2: return ctx->cop0_entrylo0;
        case 3: return ctx->cop0_entrylo1;
        case 4: return ctx->cop0_context;
        case 5: return ctx->cop0_pagemask;
        case 6: return ctx->cop0_wired;
        case 8: return ctx->cop0_badvaddr;
        case 10: return ctx->cop0_entryhi;
        case 11: return ctx->cop0_compare;
        case 13: return ctx->cop0_cause;
        case 14: return ctx->cop0_epc;
        case 15: return ctx->cop0_prid;
        case 16: return ctx->cop0_config;
        case 23: return ctx->cop0_badpaddr;
        case 24: return ctx->cop0_debug;
        case 28: return ctx->cop0_taglo;
        case 29: return ctx->cop0_taghi;
        case 30: return ctx->cop0_errorepc;
        default: return 0u;
        }
    }
    // MTC0, with the masks the recompiled instruction has.
    inline void setCop0Register(int rd, uint32_t value)
    {
        const Frame *frame = t_ee.frame;
        R5900Context *ctx = frame->ctx;
        switch (rd)
        {
        case 0: ctx->cop0_index = value & 0x3Fu; break;
        case 2: ctx->cop0_entrylo0 = value & 0x3FFFFFFFu; break;
        case 3: ctx->cop0_entrylo1 = value & 0x3FFFFFFFu; break;
        case 4: ctx->cop0_context = (ctx->cop0_context & 0xFF800000u) | (value & 0x7FFFFFu); break;
        case 5: ctx->cop0_pagemask = value & 0x01FFE000u; break;
        case 6:
            ctx->cop0_wired = value & 0x3Fu;
            ctx->cop0_random = 47u;
            break;
        case 9:
            frame->runtime->writeCop0Count(value);
            ctx->cop0_count = value;
            break;
        case 10: ctx->cop0_entryhi = value & 0xC00000FFu; break;
        case 11:
            ctx->cop0_compare = value;
            ctx->cop0_cause &= ~0x8000u;
            break;
        case 12: ctx->cop0_status = value & 0xFF57FFFFu; break;
        case 13: ctx->cop0_cause = (ctx->cop0_cause & ~0x00000300u) | (value & 0x00000300u); break;
        case 14: ctx->cop0_epc = value; break;
        case 16: ctx->cop0_config = (ctx->cop0_config & ~0x7u) | (value & 0x7u); break;
        case 24: ctx->cop0_debug = value; break;
        case 25: ctx->cop0_perf = value; break;
        case 28: ctx->cop0_taglo = value; break;
        case 29: ctx->cop0_taghi = value; break;
        case 30: ctx->cop0_errorepc = value; break;
        default: break; // (read-only: Random, BadVAddr, PRId, BadPAddr)
        }
    }
}
