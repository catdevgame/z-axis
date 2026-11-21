#pragma once

// The R5900 calling convention for readable functions.
//
// A readable function is ordinary C++ with the game function's signature:
//
//     const char *stringTableLookup(uint32_t id);
//     Vec4 *vec4NegateXyz(Vec4 *dst, const Vec4 *src);
//
// ee::entry<&fn> adapts it to the recompiled code's calling convention
// (a RecompiledFunction): it reads the arguments from the registers the
// game passes them in, runs fn and puts the result where the game expects
// it. The game's EABI (64-bit, o64-like): integer and pointer arguments in
// a0-a7 (r4-r11), float arguments in f12-f19, each kind counted on its
// own; results in v0 (integers and pointers) or f0 (floats). A 32-bit
// value is kept sign-extended in its 64-bit register, unsigned too.
// Registers other than the result are left as they were: the ones the
// caller may rely on (s0-s7, fp, gp, sp, ra) are unchanged, as the
// original's epilogue restores them; the others are dead after a call.
//
// A readable function calls other game functions with ee::call (by
// address: the recompiled function, or whatever replaced it) or
// game::call<&fn> (another readable function; game/ee/readable.h).

#include "flavour.h"
#include "ee.h"
#include "held.h"
#include "markers.h"

#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <utility>

class PS2Runtime;

namespace ee
{
    namespace reg
    {
        constexpr int v0 = 2, v1 = 3, a0 = 4, sp = 29, ra = 31;
        constexpr int f0 = 0, f12 = 12;
        // The rest by name, for ee::leftByCall.
        constexpr int at = 1, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14,
                      t7 = 15, t8 = 24, t9 = 25;
    }
    constexpr int kIntArgRegs = 8;   // a0-a7
    constexpr int kFloatArgRegs = 8; // f12-f19

    inline uint64_t gpr64(const R5900Context *ctx, int r) { return r == 0 ? 0u : static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[r])); }
    // Sets the low 64 bits of a GPR (the upper 64 are kept, as the EE's 64-bit
    // instructions do).
    inline void setGpr64(R5900Context *ctx, int r, uint64_t value)
    {
        if (r != 0)
            Ps2SetGprLow64(ctx, r, _mm_cvtsi64_si128(static_cast<int64_t>(value)));
    }

    // A whole 128-bit GPR: the quadword the original loads with LQ and hands
    // to a callee in an argument register (sceVif1PkAddData128).
    struct Qword
    {
        uint64_t low = 0, high = 0;
    };
    inline Qword loadQword(uint32_t address)
    {
        Qword q;
        std::memcpy(&q, at<uint8_t>(address), 16);
        return q;
    }
    inline void setGpr128(R5900Context *ctx, int r, Qword value)
    {
        if (r != 0)
            ctx->r[r] = _mm_set_epi64x(static_cast<int64_t>(value.high), static_cast<int64_t>(value.low));
    }
    inline uint64_t gprHigh64(const R5900Context *ctx, int r)
    {
        return r == 0 ? 0u : static_cast<uint64_t>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(ctx->r[r], ctx->r[r])));
    }

    // Caller-saved registers as the last call left them (the callee's
    // temporaries): what an original reads after a call without setting the
    // register first (holdEnter hands skaterStat the a3 that orthonormalize
    // left; trafficCarVelocity's QMTC2 after a call moves t0's upper half as
    // the callee left it). Every ee::call and ee::tailCall sets them, from the
    // guest context where the callee returned (under zx-fncheck, from the
    // recording); a readable callee called directly (game::call) does not
    // model its temporaries and leaves them as they were.
    struct LeftRegisters
    {
        uint64_t low[32] = {}, high[32] = {};
        float f[32] = {};
    };

    // ---- how a C++ type travels in registers ------------------------------

    template <typename T, typename = void>
    struct Abi;

    // Integers, enums and bool: an integer register, 32-bit values
    // sign-extended to 64 bits.
    template <typename T>
    struct Abi<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>>
    {
        static constexpr bool isFloat = false;
        static T fromReg(uint64_t r)
        {
            if constexpr (std::is_same_v<T, bool>)
                return (r & 0xFFFFFFFFu) != 0u;
            else
                return static_cast<T>(r);
        }
        static uint64_t toReg(T v)
        {
            if constexpr (sizeof(T) <= 4)
                return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
            else
                return static_cast<uint64_t>(v);
        }
    };

    // Pointers into EE RAM: the EE address in an integer register.
    template <typename T>
    struct Abi<T *, void>
    {
        static constexpr bool isFloat = false;
        static T *fromReg(uint64_t r) { return pointerAt<T>(static_cast<uint32_t>(r)); }
        static uint64_t toReg(const T *p) { return Abi<uint32_t>::toReg(addressOf(p)); }
    };
    template <typename T>
    struct Abi<Ptr<T>, void>
    {
        static constexpr bool isFloat = false;
        static Ptr<T> fromReg(uint64_t r) { return Ptr<T>(static_cast<uint32_t>(r)); }
        static uint64_t toReg(Ptr<T> p) { return Abi<uint32_t>::toReg(p.address); }
    };

    // A quadword: a whole 128-bit integer register (call arguments only).
    template <>
    struct Abi<Qword, void>
    {
        static constexpr bool isFloat = false;
    };

    // Floats: an FPU register.
    template <>
    struct Abi<float, void>
    {
        static constexpr bool isFloat = true;
        static float fromReg(float f) { return f; }
        static float toReg(float v) { return v; }
    };
    template <>
    struct Abi<Float, void>
    {
        static constexpr bool isFloat = true;
        static Float fromReg(float f) { return Float(f); }
        static float toReg(Float v) { return v.raw(); }
    };

    // How a function's result is compared by zx-fncheck.
    enum class ReturnKind : uint8_t
    {
        Void,    // nothing (v0 is dead)
        Int32,   // v0, low 32 bits (sign-extended)
        Int64,   // v0, 64 bits
        Pointer, // v0, an EE address
        Float,   // f0
    };

    template <typename R>
    constexpr ReturnKind returnKindOf()
    {
        if constexpr (std::is_void_v<R>)
            return ReturnKind::Void;
        else if constexpr (std::is_pointer_v<R>)
            return ReturnKind::Pointer;
        else if constexpr (Abi<R>::isFloat)
            return ReturnKind::Float;
        else if constexpr (sizeof(R) > 4)
            return ReturnKind::Int64;
        else
            return ReturnKind::Int32;
    }

    template <typename F>
    struct Signature;
    template <typename R, typename... P>
    struct Signature<R (*)(P...)>
    {
        using Return = R;
        static constexpr ReturnKind returnKind = returnKindOf<R>();
    };

    // ---- the register image of a call -------------------------------------

    // What a caller hands over and gets back. A readable caller sets only
    // the arguments it passes (intCount, floatCount); zx-fncheck compares
    // those with the recorded call.
    // VU0's vector registers as a callee left them (ee::callKeepingVu0).
    struct Vu0Registers
    {
        __m128 vf[32];
    };

    // VU0's macro-mode state: what one recompiled function leaves for the
    // next, as on the EE. A call from readable code keeps the callee's (the
    // original's next callee, or its own code after the call, sees them).
    struct Vu0State
    {
        __m128 vf[32];
        uint16_t vi[16];
        float q, p, i;
        __m128 r, acc;
        uint16_t status;
        uint32_t macFlags, clipFlags, clipFlags2;

        static Vu0State of(const R5900Context &ctx)
        {
            Vu0State s;
            std::memcpy(s.vf, ctx.vu0_vf, sizeof(s.vf));
            std::memcpy(s.vi, ctx.vi, sizeof(s.vi));
            s.q = ctx.vu0_q;
            s.p = ctx.vu0_p;
            s.i = ctx.vu0_i;
            s.r = ctx.vu0_r;
            s.acc = ctx.vu0_acc;
            s.status = ctx.vu0_status;
            s.macFlags = ctx.vu0_mac_flags;
            s.clipFlags = ctx.vu0_clip_flags;
            s.clipFlags2 = ctx.vu0_clip_flags2;
            return s;
        }
        void into(R5900Context &ctx) const
        {
            std::memcpy(ctx.vu0_vf, vf, sizeof(vf));
            std::memcpy(ctx.vi, vi, sizeof(vi));
            ctx.vu0_q = q;
            ctx.vu0_p = p;
            ctx.vu0_i = i;
            ctx.vu0_r = r;
            ctx.vu0_acc = acc;
            ctx.vu0_status = status;
            ctx.vu0_mac_flags = macFlags;
            ctx.vu0_clip_flags = clipFlags;
            ctx.vu0_clip_flags2 = clipFlags2;
        }
    };

    struct CallRegs
    {
        uint32_t target = 0;
        uint64_t a[kIntArgRegs] = {};
        float f[kFloatArgRegs] = {};
        int intCount = 0;
        int floatCount = 0;
        uint64_t v0 = 0, v1 = 0;
        float f0 = 0.0f;
        // The original's last act is a jump to the callee (a tail call),
        // which generated code makes as a direct C++ call: no dispatch, so
        // no EE cycles and no safe point (ee::tailCall).
        bool tail = false;
        // Arguments passed as whole 128-bit registers (ee::Qword): bit i for
        // a_i, its upper half in aHigh[i].
        uint32_t qwordArgs = 0;
        uint64_t aHigh[kIntArgRegs] = {};
        // The original's return address for this call (its jal's + 8), when
        // the caller published it (ee::calleeSaves's ra): a guest callee gets
        // it in ra, as in the original, and returns through it (callGuest,
        // callSuspending); 0: the runtime's own return slot (drafter25).
        uint32_t returnAddress = 0;
        // Made by game::call / game::tailCall for a readable callee (under
        // zx-fncheck, where it is answered from the recording): its
        // signature is all a direct caller gives it, so an argument register
        // its original reads that the call leaves out is noted apart
        // (direct_left; drafter25: freeLoadedFile's signature without the a0
        // it hands on).
        bool direct = false;
        // A kernel call (ee::kernelCall): no jal, so no dispatch to charge.
        bool kernel = false;
        // Integer arguments after the eighth: the original's caller stores each
        // in its frame's lowest slots, 8 bytes apart from the callee's sp up
        // (detail::storeStackWord wrote them as the call was made).
        int stackCount = 0;
        // Sets the argument registers in ctx.
        void into(R5900Context *ctx) const;
    };

    // Who carries out a readable function's calls. Null in the game: the
    // call runs (callGuest). zx-fncheck answers them from a recording.
    struct CallHandler
    {
        virtual ~CallHandler() = default;
        virtual void call(CallRegs &regs) = 0;
        // Cycles the readable code charged (zx-fncheck counts them).
        virtual void cycles(uint64_t count) { (void)count; }
        // The --safepoints interruptions so far in this replay (zx-fncheck:
        // after one, a self-call's return goes on as the scheduler's dispatch
        // of the return address has it; leaveSelfCall, SliceEmu).
        virtual uint32_t interruptions() const { return 0; }
        // A function calling itself. Generated code runs that call inline, so a recording holds the calls made inside
        // it and none for the call itself: zx-fncheck says true when this call of `target` is one, and the caller
        // runs the body directly (game::detail::Route), its calls answered and checked in the recording's order.
        virtual bool callsItself(uint32_t target) const
        {
            (void)target;
            return false;
        }
        // A tail call of `target` that is a jump inside the function replayed: generated code runs a `j` into its
        // own extent as a goto, so a recording holds the calls made after it as the function's own and none for the
        // jump (BMX's ghostRideMove, which releases its frame and jumps into the riderless bike's move). zx-fncheck
        // says true when `target` lies in the replayed function's extent and the recording's next call is not one
        // of it; the caller then runs the body there directly (game::tail, game::tailCall), its calls answered.
        virtual bool jumpsInside(uint32_t target) const
        {
            (void)target;
            return false;
        }
        // The running function read a register of the context as its caller
        // left it (ee::callerRegister, ee::callerFloat: GPR r, or 32 + FPR)
        // or as its last callee left it (ee::leftByCall: `left`). A replay
        // keeps the recorded registers there whoever called the function, so
        // zx-fncheck notes a read that a direct caller (game::call) would
        // not answer.
        virtual void registerRead(int r, bool left)
        {
            (void)r;
            (void)left;
        }
    };
    // The build's flavour (runtime/CMakeLists.txt ZX_DEV_CHECKS): true in the dev build, false in the app's.
    inline constexpr bool kDevChecks = ZX_DEV_CHECKS != 0;

    struct Frame;
    struct FrameRow;
    struct Suspendable;

    // The host thread that runs guest code (the game's EE thread, zx-fncheck's replay thread): what its readable
    // functions and the calls between them share, one object per thread. One thread_local object, so a function
    // that reaches several of these looks it up once (in a loaded library, Android's dynamic TLS resolves each
    // thread_local variable with a call of its own).
    struct ThreadState
    {
        Frame *frame = nullptr;             // the readable function running now (set by ee::entry)
        // Who carries out its calls: null in the game (they run), zx-fncheck's or a unit test's while it replays
        // a call. The app's flavour has no checker, so there it is a constant null: every test of it is decided
        // when the code compiles, and a call reads no thread-local for it.
#if ZX_DEV_CHECKS
        CallHandler *callHandler = nullptr;
#else
        static constexpr CallHandler *callHandler = nullptr;
#endif
        Suspendable *suspendable = nullptr; // the fiber of the one running on a fiber (suspend.cpp)
        const uint64_t *suspendableResumes = nullptr; // its count of resumes (fiberResumes), set with it
        bool tailNext = false;              // game::tail's mark on the next call (takeTailNext)
        // A readable callee called directly (game::call): its call's arguments, for its frame's saves and the
        // held-register table (ee::enterFrame, ee/held.h).
        const CallRegs *directArguments = nullptr;
        LeftRegisters left; // the caller-saved registers as the last call left them (ee::leftByCall)

        // abi.cpp's: the safe-point marks and the hooks of ee/markers.h, the last call's results.
        // (The checker's hooks: zx-fncheck sets them; in the app's flavour they are constant nulls.)
#if ZX_DEV_CHECKS
        MarkerHook markerHook = nullptr;
        SafePointHook safePointHook = nullptr;
#else
        static constexpr MarkerHook markerHook = nullptr;
        static constexpr SafePointHook safePointHook = nullptr;
#endif
        uint32_t dispatchedMark = 0; // the entry block an ee::jumpDispatch(target, true) marked
        uint64_t directReturns = 0; // returns through the scheduler leaveDirectCall charged
        uint64_t v0 = 0, v1 = 0;    // the last call's results, taken where the callee returned
        float f0 = 0.0f;
        Vu0State vu0 = {};         // VU0 as that callee left it
        uint32_t guestReturn = 0;  // the return address callGuest's next call gives its callee (0: the sentinel)
        bool selfCallFrame = false; // enterFrame for a self-call's inner call (no argument saves)

        // suspend.cpp's
        uint32_t kernelNumber = 0;          // the kernel call ee::kernelCall makes
        Suspendable *leaving = nullptr;     // the function that left a fiber last
        Suspendable *fiberCaller = nullptr; // a fiber's caller while a callee runs on that fiber (callOnFiber)

        // held.cpp's: the held-register table's rows of the frame rows entered last (held::rowOf).
        struct HeldRow
        {
            const FrameRow *layout = nullptr;
            const held::Function *function = nullptr;
        };
        static constexpr uint32_t kHeldRows = 256;
        HeldRow heldRows[kHeldRows] = {};
        // abi.cpp's: the return addresses ee::siteReturn found for a frame-table row (its index + 1; 0: none) and a
        // target, the last ones asked for.
        struct SiteReturn
        {
            uint32_t row = 0, target = 0, returnAddress = 0;
        };
        static constexpr uint32_t kSiteReturns = 1024;
        SiteReturn siteReturns[kSiteReturns] = {};
    };
    constinit inline thread_local ThreadState t_ee;

    inline void keepLeft(const R5900Context &ctx)
    {
        LeftRegisters &left = t_ee.left;
        // The register file in one copy, each register its low half, then its upper one (gpr64, gprHigh64), split
        // into the two lists; r0 reads 0. (Register by register, each through the vector extracts, it was one
        // of the costs of every call: perf6.)
        uint64_t words[64];
        static_assert(sizeof(words) == sizeof(ctx.r));
        std::memcpy(words, ctx.r, sizeof(words));
        for (int r = 0; r < 32; ++r)
        {
            left.low[r] = words[2 * r];
            left.high[r] = words[2 * r + 1];
        }
        left.low[0] = 0u;
        left.high[0] = 0u;
        std::memcpy(left.f, ctx.f, sizeof(left.f));
    }

    inline uint64_t leftByCall(int r)
    {
        if (t_ee.callHandler)
            t_ee.callHandler->registerRead(r, true);
        return t_ee.left.low[r];
    }
    inline uint64_t leftByCallHigh(int r) { return t_ee.left.high[r]; }
    inline float leftByCallFloat(int f)
    {
        if (t_ee.callHandler)
            t_ee.callHandler->registerRead(32 + f, true);
        return t_ee.left.f[f];
    }

    // Stack a callee gets below a readable function's sp when the function
    // has no StackFrame (the original's frame would be there; the area is
    // dead after the return).
    constexpr uint32_t kCalleeStackGap = 0x40u;

    // The original's callee-saved registers (s0-s7, gp, fp, ra, f20-f31) as
    // a model: a readable function keeps its values in C++ locals, so at its
    // calls the context still holds its entry values, and a callee's
    // register saves (sd s0, 0(sp) ...) would write those instead of the
    // caller's working values: other dead stack than the original's
    // (drafter21: strcmp reading past "Sint32" at career tick
    // 1,102 into initNode's save of its caller's s0). Slots: s0-s7 0-7,
    // gp 8, fp 9, ra 10, f20-f31 11-22.
    constexpr int kSavedSlots = 23;
    // (Each register's slot, -1 for none, in a table: the runtime asks it for registers it knows only then, at
    // every frame's register saves.)
    struct SavedSlotTable
    {
        int8_t slot[64];
        constexpr SavedSlotTable() : slot{}
        {
            for (int reg = 0; reg < 64; ++reg)
                slot[reg] = static_cast<int8_t>(reg >= 16 && reg <= 23 ? reg - 16
                                              : reg == 28              ? 8
                                              : reg == 30              ? 9
                                              : reg == 31              ? 10
                                              : reg >= 52 && reg <= 63 ? reg - 52 + 11
                                                                       : -1);
        }
    };
    inline constexpr SavedSlotTable kSavedSlotTable{};
    constexpr int savedSlot(int reg) { return reg >= 0 && reg < 64 ? kSavedSlotTable.slot[reg] : -1; }
    constexpr int savedSlotReg(int slot)
    {
        return slot < 8 ? slot + 16 : slot == 8 ? 28 : slot == 9 ? 30 : slot == 10 ? 31 : slot - 11 + 52;
    }
    struct SavedRegs
    {
        uint32_t mask = 0; // slots set
        uint64_t value[kSavedSlots]; // (meaningful in mask's slots only)
        SavedRegs() {} // (not zeroed: every call makes some of these)
    };

    struct FrameRow;

    // The readable function running now (set by ee::entry).
    struct Frame
    {
        uint8_t *rdram = nullptr;
        R5900Context *ctx = nullptr;
        PS2Runtime *runtime = nullptr;
        uint32_t sp = 0;
        uint32_t ra = 0;
        uint32_t calleeSp = 0; // the stack ee::call gives callees
        uint32_t stackTop = 0; // below the innermost StackFrame (sp without one)
        // Where the original would be in the safe-point model (see "Safe
        // points" below): how it goes on after an unwind.
        uint8_t mode = 0;          // kWhole or kBlocks
        bool paid = false;         // the next ee::block's dispatch is paid already
        bool callUnwound = false;  // the last call came back through the scheduler
        uint32_t slice = 0;        // the entry block the original is in (the last ee::block)
        uint32_t directDepth = 0;  // readable callees running in it (game::call)
        // ee::calleeSaves: what the caller published for its next call, and
        // the model of the running function's entry registers (a readable
        // callee called directly, a self-call: its caller's published
        // values; unset slots are the context's).
        SavedRegs next;
        SavedRegs view;
        // The running readable function (the one called directly, game::call,
        // while it runs): its sp as the original's, and the original's frame
        // (the frame table; null: not in it). ee::enterFrame.
        uint32_t fnSp = 0;
        const FrameRow *layout = nullptr;
        // The running function's ee::Local objects so far, and where the next
        // free one goes (ee/plain.h).
        uint32_t localCount = 0, localNext = 0;
        // Where the running function is in its original's calls, and what
        // they got in the callee-saved registers (ee/held.h): the place of the
        // function the frame was made for (frameHeld), or while a readable
        // callee called directly runs with a place of its own, that one, in
        // its FunctionScope (calleeHeld; ee::enterFrame). A copy of a frame is
        // made only with no callee running (null).
        held::State frameHeld;
        held::State *calleeHeld = nullptr;
        held::State &held() { return calleeHeld ? *calleeHeld : frameHeld; }
        const held::State &held() const { return calleeHeld ? *calleeHeld : frameHeld; }
    };

    // A register as the caller left it, which the original reads without
    // setting (a function that returns the v0 it was called with, a value
    // handed on in a register the signature does not name). The context
    // keeps the caller's registers while a readable function runs, but the
    // argument registers of the calls it has made (ee::call sets them).
    // A callee-saved one comes from the entry registers' model where it has
    // one (Frame::view: a readable callee called directly, a self-call).
    inline uint64_t callerRegister(int r)
    {
        if (!t_ee.frame)
            return 0u;
        if (t_ee.callHandler)
            t_ee.callHandler->registerRead(r, false);
        const int slot = savedSlot(r);
        if (slot >= 0 && (t_ee.frame->view.mask & (1u << slot)))
            return t_ee.frame->view.value[slot];
        return gpr64(t_ee.frame->ctx, r);
    }
    // The upper half of a register as the caller left it, which QMTC2 hands on
    // to VU0 (zge's vec4Midpoint: the lanes it does not set). Only a register
    // no direct call models: the context's.
    inline uint64_t callerRegisterHigh(int r) { return t_ee.frame ? gprHigh64(t_ee.frame->ctx, r) : 0u; }
    inline float callerFloat(int f)
    {
        if (!t_ee.frame)
            return 0.0f;
        if (t_ee.callHandler)
            t_ee.callHandler->registerRead(32 + f, false);
        const int slot = savedSlot(32 + f);
        if (slot >= 0 && (t_ee.frame->view.mask & (1u << slot)))
            return std::bit_cast<float>(static_cast<uint32_t>(t_ee.frame->view.value[slot]));
        return t_ee.frame->ctx->f[f];
    }

    // Locals on the guest stack, for a readable function that hands their
    // address to a callee: laid out as the original's stack frame (`size`
    // bytes below the caller's sp), so the calls get the same addresses.
    // Callees called meanwhile get their stack below it, as in the original.
    // A readable function called directly by another (game::call) shares
    // its caller's Frame; its StackFrame goes below the caller's, where the
    // original's own frame would be.
    //
    //     ee::StackFrame frame(0xA0);           // the original's addiu sp, sp, -0xA0
    //     char *name = frame.at<char>(0x00);    // its local at sp+0
    // A register save of the original (sd, sw or swc1 of a callee-saved
    // register's entry value, ra's too, into its frame): StackFrame::saveEntry.
    struct EntrySave
    {
        uint8_t reg;     // GPR 0-31, or 32 + n for FPR n
        uint8_t bytes;   // 4, 8 or 16 (sq: the register's upper half too)
        uint16_t offset; // in the frame
    };

    // ---- the original's stack frames ---------------------------------------
    //
    // The frame table (written at build time by tools/fncheck_traced_gen.py
    // from the generated code's prologues: each function's addiu sp, sp,
    // -size and its stores of callee-saved and argument registers' entry
    // values, an argument it keeps in its frame) gives
    // every readable function the original's frame without code of its own:
    // ee::entry (through the table), game::call (directly), game::tailCall
    // and ee::selfCall enter it with ee::enterFrame, which puts its callees'
    // stack where the original's is (sp - size; kCalleeStackGap for a
    // function not in the table or without a frame) and writes the
    // original's register saves there, so the dead stack it leaves below its
    // caller's sp is the original's (drafter21: stringSetText's callees ran
    // 0x60 above the original's, and 540 hand-written functions wrote none
    // of their saves). A tail call releases the caller's frame first (the
    // callee runs at the caller's sp, as after the original's epilogue).
    // A StackFrame inside still places locals and callees below its own
    // size. ZX_READABLE_FRAMES=0: none of this (the model before round 22:
    // callees kCalleeStackGap below a table entry's sp, a direct callee in
    // its caller's place).
    struct FrameRow
    {
        uint32_t game;    // ee/game_id.h number
        uint32_t address; // the function's start
        uint32_t size;    // its frame (0: none)
        uint32_t first;   // its saves: the table's saves [first, first + count)
        uint32_t count;
        uint32_t firstLocal; // its locals' places: the table's locals [firstLocal, firstLocal + localCount)
        uint32_t localCount;
    };
    // The generated table registers itself at static initialisation (a build
    // without it, the unit tests, runs with an empty one).
    struct FrameTableRegistration
    {
        FrameTableRegistration(const FrameRow *rows, uint32_t count, const EntrySave *saves, const uint16_t *locals);
    };
    // The function at `address` of game `game`, or null. abi.cpp.
    const FrameRow *frameLayout(uint32_t game, uint32_t address);
    const EntrySave *frameSaves(const FrameRow &row);
    // A row's place in the frame table: its index + 1, 0 for a row not of the table (one made by hand).
    uint32_t frameRowNumber(const FrameRow *row);
    // The table's rows: [frameRow(1), frameRow(frameRowCount())].
    uint32_t frameRowCount();
    const FrameRow &frameRow(uint32_t number);
    // ZX_READABLE_FRAMES (default 1).
    bool framesOn();
    // ZX_READABLE_LEAN (default 0; the app's boot sets 1 for AI, gameBootConfig): the proof scaffolding the game can
    // run without. 1 is the held-register table off (held::on): about a third of the EE thread in AI's and BMX
    // XXX's levels (lean1). Its values only make the callee-saved registers a guest callee gets, and so the saves
    // it writes into dead stack, the original's; what they change is the RAM words listed in lean1's report (the
    // bits a guest function reads from a register it never set, records copied from stack nobody wrote). The
    // A/B, the replay and the logic checks run with it 0.
    // ZX_READABLE_LEAN=held,saves,registers: those parts alone. `saves`: none of the original's register saves
    // written into the frames (enterFrame, StackFrame::saveEntry); the original's code times itself by such
    // stack (a quadword strcmp past its NUL), so EE time and with it the heap's layout change (AI's career).
    // `registers`: guest callees get the context's callee-saved registers (applySaves). abi.cpp.
    constexpr uint32_t kLeanHeld = 1u, kLeanSaves = 2u, kLeanRegisters = 4u;
    // The boot's switches, one word: the lean parts, kBootHeldOn (the held-register table on: held::on) and
    // kBootSwitchesRead. readBootSwitches() reads them at each boot (game::readable::install: a collection plays
    // its games in one process, and the app sets ZX_READABLE_LEAN per game, gameBootConfig); a run that installs
    // nothing (zx-fncheck, the unit tests) reads them at its first use. Every call asks: a plain load after.
    constexpr uint32_t kBootHeldOn = 1u << 30, kBootSwitchesRead = 1u << 31;
    inline std::atomic<uint32_t> g_bootSwitches{0u};
    uint32_t readBootSwitches();
    inline uint32_t bootSwitches()
    {
        const uint32_t switches = g_bootSwitches.load(std::memory_order_relaxed);
        return (switches & kBootSwitchesRead) ? switches : readBootSwitches();
    }
    inline uint32_t leanParts() { return bootSwitches() & (kLeanHeld | kLeanSaves | kLeanRegisters); }
    inline bool lean(uint32_t part) { return (bootSwitches() & part) != 0u; }
    // Each function's call targets (a jal's, or 0 for a jalr linking ra)
    // with the return address of the one site that calls it, 0 when several
    // do, from the generated code (the frame table's source,
    // tools/fncheck_traced_gen.py): a call from readable code that publishes
    // no ra (hand-written code) gives its callee that one (ee::siteReturn;
    // drafter25).
    struct CallSite
    {
        uint32_t game, function, target, returnAddress;
    };
    struct CallSiteRegistration
    {
        CallSiteRegistration(const CallSite *rows, uint32_t count);
    };
    // The return address of the only call of `target` from the frame-table
    // function `row`; for a target it never calls directly, its only indirect
    // call's (a function pointer); 0 when it has several or none. abi.cpp.
    uint32_t siteReturn(const FrameRow &row, uint32_t target);
    // A registered readable function's row, by its host function
    // (readable.cpp); layoutOf<Fn>() keeps it.
    const FrameRow *hostLayout(const void *host);
    template <auto Fn>
    const FrameRow *layoutOf()
    {
        static const FrameRow *const layout = hostLayout(reinterpret_cast<const void *>(Fn));
        return layout;
    }
    // What enterFrame changed, for leaveFrame. Kept in place while the
    // function runs (the frame may point at its held place): not copied.
    struct FunctionScope
    {
        const FrameRow *layout = nullptr;
        uint32_t fnSp = 0, stackTop = 0, calleeSp = 0;
        bool entered = false;
        uint32_t localCount = 0, localNext = 0;
        // The callee's place in its original's calls, when it has a row and
        // its caller a place of its own to go back to (heldOwn: then
        // Frame::calleeHeld points here, and was outerHeld); else the callee
        // works in the caller's place, whose function (heldFunction) is put
        // back after it.
        held::State held;
        held::State *outerHeld = nullptr;
        const held::Function *heldFunction = nullptr;
        uint32_t heldFlags = 0; // (its flags, held::State::flags)
        bool heldOwn = false;

        FunctionScope() = default;
        FunctionScope(const FunctionScope &) = delete;
        FunctionScope &operator=(const FunctionScope &) = delete;
    };
    // Enters `layout`'s function at sp `sp` in `frame`: Frame::fnSp, stackTop
    // (a StackFrame goes below it) and calleeSp (sp - size, else sp -
    // kCalleeStackGap), and the original's register saves at sp - size + offset
    // from the entry registers' model (Frame::view) where it has a value, else
    // the context; an argument register's from the call's arguments for a
    // readable callee called directly (ee::t_ee.directArguments; a self-call's,
    // not known, none). Nothing with ZX_READABLE_FRAMES=0. abi.cpp. `thread`: this
    // thread's state (t_ee), from a caller that has it.
    void enterFrame(FunctionScope &scope, Frame &frame, const FrameRow *layout, uint32_t sp, ThreadState &thread = t_ee);
    void leaveFrame(Frame &frame, const FunctionScope &scope);
    // A readable callee called directly (game::call: at the caller's callee
    // sp) or as a tail call (game::tailCall: at the caller's own sp, its
    // frame released) in the running Frame.
    void enterDirectFrame(FunctionScope &scope, const FrameRow *layout, bool tail);
    void leaveDirectFrame(const FunctionScope &scope);

    // A register's value as the original holds it: a 32-bit value sign-
    // extended (as lw and the 32-bit ALU leave it), a float's bits.
    struct CalleeSave
    {
        uint8_t reg;    // GPR 0-31, or 32 + n for FPR n
        uint64_t value;
        template <typename T>
        CalleeSave(int r, T v) : reg(static_cast<uint8_t>(r)), value(bitsOf(v))
        {
        }

    private:
        template <typename T>
        static uint64_t bitsOf(T v)
        {
            if constexpr (std::is_same_v<std::decay_t<T>, Float>)
                return v.bits();
            else if constexpr (std::is_same_v<std::decay_t<T>, float>)
                return std::bit_cast<uint32_t>(v);
            else if constexpr (std::is_enum_v<T>)
                return bitsOf(static_cast<std::underlying_type_t<T>>(v));
            else if constexpr (std::is_integral_v<T> && sizeof(T) <= 4)
                return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
            else if constexpr (std::is_integral_v<T>)
                return static_cast<uint64_t>(v);
            else
                return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v))));
        }
    };
    // Before a call: the callee-saved registers the original has changed
    // by then, with their values (readable_draft.py emits it at every call
    // where one differs from the entry value), and ra, the jal's return
    // address (the drafter gives it at every call but a tail call). A guest
    // call gets them in the context, restored after it, but ra: the callee
    // gets the original's return address and returns through it (the table
    // slot there takes it back to the readable caller: suspend.cpp), and ra
    // stays so after the call, as the original's callee leaves it; a
    // readable callee called directly or a self-call gets them as its entry
    // registers' model (Frame::view), which its saveEntry writes, and ra is
    // the published one after it returns. A call without a published ra
    // returns through the runtime's resume slot, its callee's ra (and so its
    // register saves and the context after it) the slot's address, not the
    // original's (drafter24: paths6's movie functions left a heap pointer in
    // three stack words of the card thread and main). abi.cpp.
    void calleeSaves(std::initializer_list<CalleeSave> saves);
    // ZX_READABLE_RETURNS (default 1): guest calls return through the
    // published ra as above. 0: every guest call through the runtime's slots
    // (the model before drafter25).
    // (Inline: every placed call asks.)
    inline bool returnsOn()
    {
        static const bool on = []
        {
            const char *v = std::getenv("ZX_READABLE_RETURNS");
            return !(v && v[0] == '0');
        }();
        return on;
    }
    // The return address published for the next call (Frame::next's ra), 0
    // if none (or ZX_READABLE_RETURNS=0).
    uint32_t publishedReturn(const Frame &frame);
    // The return address of a call to `target` from the running function:
    // the published one, else the call-site table's (siteReturn, the
    // running function's frame row: Frame::layout), 0 if neither knows.
    uint32_t callReturn(const Frame &frame, uint32_t target);
    // Before a readable callee called directly (game::call) or a self-call
    // without a published ra: the call-site table's, published (so the
    // callee's saves and ra after it are the original's).
    void publishSiteReturn(uint32_t target);
    // A tail call's: the running function's own return address (the
    // original's epilogue put ra back): the entry ra, or for a readable
    // function called directly the one its caller published; 0 if unknown.
    uint32_t tailReturn(const Frame &frame);
    // A callee-saved register's value at the running function's entry: the
    // entry registers' model (Frame::view) where it has one, else the context.
    uint64_t entryRegister(const Frame &frame, int reg);
    // A readable callee called directly or a self-call has returned: ra as
    // the original's epilogue leaves it, the return address its caller
    // published (in `view`, the callee's entry model), else as it is.
    void returnedTo(const Frame &frame, const SavedRegs &view);
    // Before a call of a readable function made directly (ee::selfCall, the
    // drafter's), the caller-saved registers the callee reads as its caller
    // left them (ee::callerRegister: not arguments, so not set by the call):
    // into the context, where the original has them at its jal (drafter24).
    void passRegisters(std::initializer_list<CalleeSave> regs);
    // (callGuest and callSuspending) The published registers into ctx for
    // a guest call; restoreSaves puts the context's own back after it.
    struct KeptRegs
    {
        uint32_t mask = 0;
        __m128i gpr[kSavedSlots];
        float fpr[kSavedSlots];
    };
    KeptRegs applySaves(Frame &frame, R5900Context *ctx);
    void restoreSaves(const KeptRegs &kept, R5900Context *ctx);

    class StackFrame
    {
    public:
        explicit StackFrame(uint32_t size)
            : m_frame(t_ee.frame), m_savedCalleeSp(m_frame ? m_frame->calleeSp : 0u),
              m_savedTop(m_frame ? m_frame->stackTop : 0u), m_base(m_frame ? m_frame->stackTop - size : 0u)
        {
            if (m_frame)
            {
                m_frame->calleeSp = m_base;
                m_frame->stackTop = m_base;
            }
        }
        ~StackFrame()
        {
            if (m_frame)
            {
                m_frame->calleeSp = m_savedCalleeSp;
                m_frame->stackTop = m_savedTop;
            }
        }
        StackFrame(const StackFrame &) = delete;
        StackFrame &operator=(const StackFrame &) = delete;

        uint32_t address(uint32_t offset = 0) const { return m_base + offset; }
        template <typename T>
        T *at(uint32_t offset) const { return ee::at<T>(m_base + offset); }

        // The original's register saves, written as it writes them: the
        // dead stack it leaves below its caller's sp is then the original's
        // (drafter20: strcmp compares quadwords and reads past a
        // string's 0 into it, so stale bytes there cost it 128 EE cycles).
        // The values: the entry registers' model (Frame::view, what a
        // readable caller published with calleeSaves) where it has one, else
        // the context's (a function entered through the table: its entry
        // registers; a guest call from a readable caller has the caller's
        // published values in them). abi.cpp.
        void saveEntry(std::initializer_list<EntrySave> saves) const;

    private:
        Frame *m_frame;
        uint32_t m_savedCalleeSp;
        uint32_t m_savedTop;
        uint32_t m_base;
    };

    // Runs regs.target with regs' arguments on the current frame's context
    // (stack below the readable function's sp) and fills v0, v1 and f0.
    // EE safe points are held off meanwhile (PS2Runtime::callGuestFunction):
    // the callee runs to completion, so it must not wait. abi.cpp.
    void callGuest(CallRegs &regs);

    // A readable function whose callees can wait (READABLE_FUNCTION_WAITS)
    // runs on its own host fiber (suspend.cpp). Its calls go out as real
    // guest calls: the fiber leaves, the guest thread continues at the
    // callee with ra at a resume slot in the function table, and when the
    // callee returns there (after any waits and thread switches in between)
    // the fiber is entered again with the results. Non-null while such a
    // function runs.
    struct Suspendable;
    void callSuspending(CallRegs &regs);
    // At a safe point with a checkpoint due: the fiber leaves and the guest
    // thread unwinds to the scheduler, which enters it again later.
    void yieldSafePoint();
    // Runs body(frame) on a fiber for the function entered with ctx.
    void runSuspendable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, void (*body)(Frame &frame),
                        const FrameRow *layout = nullptr);
    // Finds the free table slots for the resume addresses in the table in use
    // (at each install), and has the runtime drop what still waits when its
    // run stops (dropWaiting).
    bool installSuspendableCalls(PS2Runtime &runtime);
    // The functions waiting for a callee's return now (on their fibers).
    size_t waitingFunctions();
    // The run stopped (PS2Runtime::addStoppedHook): every function still
    // waiting is dropped with its fiber, and the return slots set up in the
    // table in use get back what they had.
    void dropWaiting(PS2Runtime &runtime);
    // `entry` is a readable function's table entry (ee::entry or
    // ee::entrySuspendable): a guest call of it from a fiber runs on that
    // fiber. suspend.cpp.
    void noteReadableEntry(PS2Runtime::RecompiledFunction entry);
    // `entry` is the table entry of a library function the runtime replaces with host code that neither waits
    // nor calls the game (game::readable::LibraryLeaf): a guest call of it from a fiber runs on that fiber,
    // through the same dispatch. Each install starts the list again (clearLibraryLeaves). suspend.cpp.
    void noteLibraryLeaf(PS2Runtime::RecompiledFunction entry);
    void clearLibraryLeaves();
    // ZX_STACK_POISON_STORES, ZX_STACK_POISON_FRAMES (abi.cpp): the table's
    // functions get their fill at every entry through the function table. Before
    // the readable functions take their slots (readable.cpp), so `off` and the
    // defaults fill alike.
    void installStackFill(PS2Runtime &runtime);

    // SYSCALL with `number` in v1: the arguments are the registers as the
    // function's caller left them (a kernel call's wrapper: addiu v1, N;
    // syscall; jr ra), the result is v0. It leaves a fiber as a guest call
    // does, so a kernel call that waits suspends the function (suspend.cpp);
    // under zx-fncheck it is a call of target 0, which no recording holds.
    int32_t kernelCall(uint32_t number);

    namespace detail
    {
        // The `index`th integer argument after the eighth of the call `regs`
        // is being made up for: `bytes` (4 or 8) of `word` at the callee's
        // sp + 8 * index, in the running function's frame (the sp callGuest
        // gives the callee: Frame::calleeSp, a tail call's the function's
        // own). Stops where the frame has no room for it. abi.cpp.
        void storeStackWord(const CallRegs &regs, int index, uint64_t word, uint32_t bytes);
        // An argument this model has no place for (a ninth float, a 128-bit
        // word after the eighth register): stops with the call's target.
        [[noreturn]] void noPlaceForArgument(const CallRegs &regs, const char *what);
        // The bytes the original stores for an argument on the stack: sw for
        // a 32-bit value or an address, sd for a 64-bit one; a variadic
        // word says its own (ee::VarArg).
        template <typename T>
        uint32_t stackWordBytes(const T &value)
        {
            if constexpr (requires { value.stackBytes(); })
                return value.stackBytes();
            else
                return sizeof(T) > 4u ? 8u : 4u;
        }

        template <typename T>
        void pushArg(CallRegs &regs, T value)
        {
            using A = Abi<std::decay_t<T>>;
            if constexpr (requires { value.leftOut(); }) // a variadic function's word (ee::VarArg, ee/plain.h)
            {
                if (value.leftOut())
                    return; // a word the call does not name: its register stays as the call left it
            }
            if constexpr (std::is_same_v<std::decay_t<T>, Qword>)
            {
                if (regs.intCount >= kIntArgRegs)
                    noPlaceForArgument(regs, "a 128-bit argument after the eighth register");
                regs.qwordArgs |= 1u << regs.intCount;
                regs.aHigh[regs.intCount] = value.high;
                regs.a[regs.intCount++] = value.low;
            }
            else if constexpr (A::isFloat)
            {
                if (regs.floatCount >= kFloatArgRegs)
                    noPlaceForArgument(regs, "a ninth float argument");
                regs.f[regs.floatCount++] = A::toReg(value);
            }
            else if (regs.intCount < kIntArgRegs)
                regs.a[regs.intCount++] = A::toReg(value);
            else // a0-t3 are taken: the caller's stack
                storeStackWord(regs, regs.stackCount++, A::toReg(value), stackWordBytes(value));
        }

        // An argument's register as pushArg sets it, for the held-register
        // table's view of a readable callee called directly (ee/held.h): the
        // registers only (pushArg stores the words after the eighth).
        template <typename T>
        void registerArg(CallRegs &regs, const T &value)
        {
            using A = Abi<std::decay_t<T>>;
            if constexpr (requires { value.leftOut(); })
            {
                if (value.leftOut())
                    return;
            }
            if constexpr (std::is_same_v<std::decay_t<T>, Qword>)
            {
                if (regs.intCount < kIntArgRegs)
                    regs.a[regs.intCount++] = value.low;
            }
            else if constexpr (A::isFloat)
            {
                if (regs.floatCount < kFloatArgRegs)
                    regs.f[regs.floatCount++] = A::toReg(value);
            }
            else if (regs.intCount < kIntArgRegs)
                regs.a[regs.intCount++] = A::toReg(value);
        }

        template <typename R>
        R takeResult(const CallRegs &regs)
        {
            if constexpr (std::is_void_v<R>)
                return;
            else if constexpr (Abi<R>::isFloat)
                return Abi<R>::fromReg(regs.f0);
            else
                return Abi<R>::fromReg(regs.v0);
        }

        template <typename T>
        T readArg(const R5900Context *ctx, int &nextInt, int &nextFloat)
        {
            using A = Abi<T>;
            if constexpr (std::is_same_v<T, Qword>)
            {
                const int r = reg::a0 + nextInt++;
                return Qword{gpr64(ctx, r), gprHigh64(ctx, r)};
            }
            else if constexpr (A::isFloat)
                return A::fromReg(ctx->f[reg::f12 + nextFloat++]);
            else
                return A::fromReg(gpr64(ctx, reg::a0 + nextInt++));
        }

        template <typename R>
        void writeResult(R5900Context *ctx, R value)
        {
            using A = Abi<R>;
            if constexpr (A::isFloat)
                ctx->f[reg::f0] = A::toReg(value);
            else
                setGpr64(ctx, reg::v0, A::toReg(value));
        }

        template <auto Fn, typename R, typename... P>
        void invoke(R (*)(P...), R5900Context *ctx)
        {
            int nextInt = 0, nextFloat = 0;
            // Braced initialisation reads the arguments left to right.
            std::tuple<std::decay_t<P>...> args{readArg<std::decay_t<P>>(ctx, nextInt, nextFloat)...};
            if constexpr (std::is_void_v<R>)
                std::apply(Fn, args);
            else
                writeResult<R>(ctx, std::apply(Fn, args));
        }
    }

    // Emulated EE time. Generated code charges 32 cycles at every backward
    // branch (a loop's safe point) and 8 for every call it dispatches;
    // emulated time decides when vsyncs and interrupts fall, so a readable
    // function charges what the original would (zx-fncheck compares them).
    // abi.cpp.
    void chargeCycles(uint64_t cycles);

    // ---- safe points --------------------------------------------------------
    //
    // A recompiled function checks at every branch back (32 cycles) and at
    // every call it dispatches (8) whether a checkpoint is due (a vsync, a
    // timer, the end of a time slice); if one is, it unwinds to the EE
    // scheduler, which handles the event and later resumes the guest thread
    // at the pc it left, through the function table. A loop head or a call's
    // return address maps either to the function itself (its switch: the rest
    // runs as before, kWhole) or to one of the recompiler's entry blocks
    // (entry_XXXXXXXX: slices of the function from one block start to the
    // next; the rest of that call then runs slice by slice through the
    // scheduler, kBlocks: 8 cycles and a check at each slice it enters, 32 and
    // a check at a branch back to the slice's own start, nothing at other
    // branches back). A frame that unwound returns to its caller through the
    // scheduler too (8 and a check at the return address).
    //
    // A readable function on a fiber (ee/suspend.cpp: every readable
    // function in the game while ZX_READABLE_SAFEPOINTS is on) does the
    // same: at the original's safe points it checks, and when a checkpoint is
    // due its fiber leaves (the guest thread unwinds to the scheduler as the
    // original does) and is entered again from the scheduler; from there it
    // charges what the original charges in the mode it is in. Drafts mark the
    // entry blocks (ee::block) and their branches back (ee::loopBack), so
    // they follow the original in both modes; a hand-written function
    // (ee::chargeLoop) gets the checks at its loops and calls and the whole
    // function's charges (exact when its loops are one entry block each).
    // Under zx-fncheck, and with the setting off, only the charges remain:
    // the model of a call that runs straight through.
    constexpr uint8_t kWhole = 0, kBlocks = 1;
    // ZX_READABLE_SAFEPOINTS (default 1): readable functions in the game
    // honour checkpoints as above. 0: they run each call through, safe
    // points held off (the model before; abi.cpp).
    bool safePointsOn();
    // A branch back the original checks at (32 cycles), without the entry
    // block model (hand-written functions).
    void safePoint(uint32_t cycles);
    // A loop the original runs `iterations` times (one backward branch each).
    void chargeLoop(uint64_t iterations);
    // A draft's branch back to `head`, the start of an entry block.
    void loopBack(uint32_t head);
    // A branch back to an address the function table maps to the whole
    // function (or to its start); `slice` is the entry block it lies in (0:
    // none).
    void loopBackWhole(uint32_t target, uint32_t slice);
    // The start of an entry block (afterCall: it is the return address of
    // the call just made).
    void block(uint32_t leader, bool afterCall = false);
    // A branch target inside entry block `slice` that the table maps to the
    // whole function: reached from another slice it is a dispatch of the
    // function.
    void blockWhole(uint32_t target, uint32_t slice);
    // Around a readable function called directly (game::call): the dispatch
    // (8 cycles and a check), the callee's own mode, and after it, if the
    // call unwound meanwhile, the return through the scheduler.
    // What a call's published values covered of the caller's entry-register model (Frame::view): its mask before
    // and the slots they replaced, with their values before (the rest of the model the call leaves as it is).
    struct ViewOverlay
    {
        uint32_t mask = 0;     // Frame::view's mask before
        uint32_t replaced = 0; // the slots the published values replaced
        uint64_t value[kSavedSlots]; // (their values before: meaningful in `replaced`'s slots only)
        ViewOverlay() {} // (not zeroed)
    };
    struct DirectCall
    {
        uint8_t mode;
        bool paid, callUnwound;
        uint32_t slice;
        uint64_t resumes;
        ViewOverlay view; // the caller's entry-register model, as far as the call changes it (Frame::view)
    };
    DirectCall enterDirectCall(bool tail);
    void leaveDirectCall(const DirectCall &saved);
    // A readable callee called directly (game::call: game/ee/readable.h directAt), its bookkeeping in one place, in
    // the order the pieces above make it: the original's callee-saved registers at the call (held::beforeCall), the
    // site's return address (publishSiteReturn), the dispatch (enterDirectCall), the callee's frame with `regs` as
    // its arguments (enterDirectFrame); after it, the frame left (leaveDirectFrame), the return (leaveDirectCall) and
    // the call's results (held::afterCall: `known`, bits 0-2, v0, v1, f0). One look-up of the thread's state each.
    struct DirectCallScope
    {
        DirectCall call;
        FunctionScope frame;
        ThreadState *thread = nullptr; // (the call's thread, looked up once: a fiber runs on the thread it was made on)
    };
    void enterDirectCallee(DirectCallScope &scope, uint32_t address, const CallRegs &regs, const FrameRow *layout);
    void leaveDirectCallee(const DirectCallScope &scope, uint64_t v0, uint32_t f0, uint8_t known);
    // Times the running fiber was entered again from the scheduler (an
    // unwind in it); 0 off a fiber.
    inline uint64_t fiberResumes(const ThreadState &thread = t_ee)
    {
        return thread.suspendableResumes ? *thread.suspendableResumes : 0u;
    }
    // Whether the fiber's last resume was the return of a tail call (made
    // by a readable function called directly, game::call) and nothing has
    // resumed it since; asking clears it. That return came back through
    // the scheduler's dispatch of the resume slot, which is the original's
    // dispatch of the caller's return address: leaveDirectCall charges no
    // second return (suspend.cpp).
    bool takeTailReturn(ThreadState &thread = t_ee);

    // A function calling itself as generated code compiles it (zge's
    // checkFaceTree, cachedMatrix): the jal to its own start becomes a jump
    // there (no dispatch, no host recursion: the guest frames stack up in
    // one C++ call), so the inner call's return, a jr to the address after
    // the jal, leaves the C++ function: the guest thread unwinds to the
    // scheduler, which dispatches that address (8 cycles and a check; the
    // table gives it to the whole function), and every caller up the chain
    // returns through the scheduler as well. From an entry block (after an
    // interrupted loop or call) the call of itself is the slice's jal
    // instead: dispatched (8 and a check) into the whole function, its
    // return in place unless the inner call unwound.
    //
    //     ee::selfCall([&] { walkFaceTree(tree, next); });
    //
    // The readable version calls itself in C++ inside selfCall. Under
    // zx-fncheck the inner return's dispatch is charged (8) and, with
    // --safepoints, is a safe point, as its replay of the original has it
    // (drafter20); with safe points off only the cycles are charged.
    // When the return address starts an entry block (an entry_ function in
    // the table: setSkaterScore's), the scheduler's dispatch goes
    // on there slice by slice: selfCall(RETURN, inner) models that (the
    // ee::block(RETURN) after it charges nothing more). abi.cpp.
    struct SelfCall
    {
        DirectCall saved;
        bool dispatched; // from an entry block: a dispatched call
        uint32_t interruptions; // (zx-fncheck: CallHandler::interruptions at the call)
        SavedRegs view;  // the caller's Frame::view (a self-call's callee is modelled from its published values)
        FunctionScope scope; // the inner call's frame (below the caller's, as the original's jal)
    };
    void enterSelfCall(SelfCall &call);
    void leaveSelfCall(const SelfCall &call, uint32_t returnBlock = 0);
    template <typename F>
    void selfCall(F &&inner)
    {
        SelfCall call{};
        enterSelfCall(call);
        inner();
        leaveSelfCall(call);
    }
    template <typename F>
    void selfCall(uint32_t returnBlock, F &&inner)
    {
        SelfCall call{};
        enterSelfCall(call);
        inner();
        leaveSelfCall(call, returnBlock);
    }

    // game::tail (game/ee/readable.h) marks the call it makes as the original's
    // tail jump (a `j`, not a `jal`): the first call made after it is that call,
    // whatever makes it (a readable callee's entry, ee::call, ee::game, a
    // FunctionPtr), and takes the mark (takeTailNext). game::tail clears a mark
    // no call took when its callee returns, so it never reaches a later call
    // (frame1: BMX givePathManObjective's tail through a plain ee::game helper
    // was made as a jal, and the mark left set made riderActivatesPathMan's next
    // call a tail call, its callee's frame over its caller's).
    inline bool takeTailNext() { return std::exchange(t_ee.tailNext, false); }

    // Calls the game function at an EE address (recompiled or replaced)
    // and returns its result as R.
    namespace detail
    {
        // ee::call and ee::tailCall; `direct`: made by game::call (or
        // game::tailCall) for a readable callee answered from the recording
        // (zx-fncheck), whose signature is all its callers can give it.
        template <typename R, typename... A>
        R callWith(uint32_t target, bool tail, bool direct, A... args)
        {
            if (takeTailNext())
                tail = true; // (game::tail's call: before the stack words, which a tail call puts at its own sp)
            CallRegs regs;
            regs.target = target;
            regs.tail = tail;
            regs.direct = direct;
            (pushArg(regs, args), ...);
            // What the original holds in its callee-saved registers at this call
            // (ee/held.h).
            if (!tail && t_ee.frame && t_ee.frame->held().function)
                held::beforeCall(*t_ee.frame, regs);
            if (t_ee.callHandler)
            {
                if (!tail)
                    chargeCycles(8u); // the dispatch, as the original's call charges it
                t_ee.callHandler->call(regs);
            }
            else if (t_ee.suspendable)
                callSuspending(regs);
            else
                callGuest(regs);
            if (!tail && t_ee.frame && held::readsAfterCall(t_ee.frame->held()))
                held::afterCall(*t_ee.frame, regs.v0, regs.v1, std::bit_cast<uint32_t>(regs.f0), 7u);
            return takeResult<R>(regs);
        }
    }

    template <typename R = void, typename... A>
    R call(uint32_t target, A... args)
    {
        return detail::callWith<R>(target, false, false, args...);
    }

    // A call the original makes as its tail call (`j` to a function's
    // start), which generated code compiles to a direct C++ call: the same
    // as call, but without the dispatch's 8 EE cycles (and safe point) a
    // `jal` costs. Use it for the original's tail calls, so the EE time is
    // the game's.
    template <typename R = void, typename... A>
    R tailCall(uint32_t target, A... args)
    {
        return detail::callWith<R>(target, true, false, args...);
    }

    // ee::call that also hands back the VU0 registers the callee left, for a
    // readable function that goes on using a lane its original never
    // reloaded after the call (a VMULx.xyz keeps the destination's w, then
    // an SQC2 stores all four lanes). Every call keeps the callee's VU0
    // state in the context (Vu0State); this copies the vector registers out.
    // Exact while the callee is recompiled code: readable functions do not
    // model VU0 registers.
    template <typename R = void, typename... A>
    R callKeepingVu0(Vu0Registers &vu0, uint32_t target, A... args)
    {
        auto copyOut = [&vu0]
        {
            if (const Frame *frame = t_ee.frame; frame && frame->ctx)
                std::memcpy(vu0.vf, frame->ctx->vu0_vf, sizeof(vu0.vf));
        };
        if constexpr (std::is_void_v<R>)
        {
            call<void>(target, args...);
            copyOut();
        }
        else
        {
            R result = call<R>(target, args...);
            copyOut();
            return result;
        }
    }

    namespace detail
    {
        // The body of a suspendable function: arguments from the registers
        // at entry, the result into the context current at the end (the
        // guest thread's context may be another object after a wait).
        template <auto Fn, typename R, typename... P>
        void invokeSuspendable(R (*)(P...), Frame &frame)
        {
            int nextInt = 0, nextFloat = 0;
            std::tuple<std::decay_t<P>...> args{readArg<std::decay_t<P>>(frame.ctx, nextInt, nextFloat)...};
            if constexpr (std::is_void_v<R>)
                std::apply(Fn, args);
            else
            {
                R result = std::apply(Fn, args);
                writeResult<R>(frame.ctx, result);
            }
        }
        template <auto Fn>
        void suspendableBody(Frame &frame) { invokeSuspendable<Fn>(Fn, frame); }
    }

    // The RecompiledFunction for a readable function whose callees can wait:
    // as entry<Fn>, on a fiber (see Suspendable). Under zx-fncheck, whose
    // calls are answered from a recording, it runs directly.
    template <auto Fn>
    void entrySuspendable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // The RecompiledFunction for a readable function: arguments from
    // registers, the result to v0 / f0, then back to the caller (pc = ra).
    template <auto Fn>
    void entryDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sp = static_cast<uint32_t>(gpr64(ctx, reg::sp));
        // (all 64 bits: a host-made call's is its sentinel, sign-extended)
        const uint64_t entryRa = gpr64(ctx, reg::ra);
        Frame frame{rdram, ctx, runtime, sp, static_cast<uint32_t>(entryRa), sp - kCalleeStackGap, sp};
        FunctionScope scope; // (a frame of its own: its place is the frame's)
        enterFrame(scope, frame, layoutOf<Fn>(), sp);
        Frame *outer = t_ee.frame;
        t_ee.frame = &frame;
        detail::invoke<Fn>(Fn, ctx);
        t_ee.frame = outer;
        // The jr ra: ra as the epilogue put it back (a call leaves it at its
        // return address).
        setGpr64(ctx, reg::ra, entryRa);
        ctx->pc = frame.ra;
    }

    // Whether a readable function entered now runs on a fiber (safe points
    // on, in the game, the resume slot installed). suspend.cpp.
    bool fiberEntry(const PS2Runtime *runtime);

    template <auto Fn>
    void entry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (fiberEntry(runtime))
            runSuspendable(rdram, ctx, runtime, &detail::suspendableBody<Fn>, layoutOf<Fn>());
        else
            entryDirect<Fn>(rdram, ctx, runtime);
    }

    template <auto Fn>
    void entrySuspendable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (t_ee.callHandler || !runtime)
            entryDirect<Fn>(rdram, ctx, runtime);
        else
            runSuspendable(rdram, ctx, runtime, &detail::suspendableBody<Fn>, layoutOf<Fn>());
    }
}
