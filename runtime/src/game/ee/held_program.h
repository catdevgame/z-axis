#pragma once

// The held table's expressions as the runtime works them out (ee/held.h Node): one place for each node's meaning,
// which the interpreter (ee/held.cpp run) and the programs readable_held_gen.py generates as code
// (readable_held_code.py: the build's readable_held_programs_*.cpp) both call, so the two give the same values and
// fail at the same nodes. Not game code: held.cpp and the generated programs alone include it.

#include "abi.h"
#include "ee.h"
#include "held.h"
#include "ps2_runtime_macros.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#define HELD_NOINLINE __declspec(noinline)
#define HELD_INLINE __forceinline
#else
#define HELD_NOINLINE __attribute__((noinline))
#define HELD_INLINE inline __attribute__((always_inline))
#endif

namespace ee::held::program
{
    inline uint64_t sx32(uint64_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
    inline uint64_t zx32(uint64_t v) { return static_cast<uint32_t>(v); }
    inline float asFloat(uint64_t bits) { return std::bit_cast<float>(static_cast<uint32_t>(bits)); }
    inline uint64_t bitsOf(float f) { return std::bit_cast<uint32_t>(f); }

    // What an expression is worked out with: the running function's frame and place, the call about to be made,
    // whether memory may be read (loads), where a load that cannot be read is said (unreadable, a check's), and
    // whether a recording answers the calls (replaying: zx-fncheck; a check's load is then not read).
    struct Context
    {
        const Frame &frame;
        const State &state;
        const CallRegs &regs;
        bool loads;
        bool *unreadable;
        bool replaying;
    };
    // What a case's values give (CaseValuesCode, ee/held.cpp caseValues): value[slot] for the slots in known,
    // the slots in cleared not known.
    struct Worked
    {
        uint64_t value[kSlots];
        uint32_t known = 0, cleared = 0;
    };

    // ---- the leaves: what the runtime has at the call --------------------------------------------------------

    // kEntry: the function's entry value of register r (GPR, or 32 + FPR).
    HELD_INLINE bool entry(const Context &c, int r, uint64_t &v)
    {
        if (r >= reg::a0 && r < reg::a0 + 8)
        {
            if (!(c.state.arguments & (1u << (r - reg::a0))))
                return false;
            v = c.state.argument[r - reg::a0];
        }
        else if (r >= 32 + 12 && r < 32 + 20)
        {
            if (!(c.state.arguments & (1u << (8 + r - 32 - 12))))
                return false;
            v = c.state.floatArgument[r - 32 - 12];
        }
        else if (r == reg::sp)
        {
            if (!c.frame.fnSp)
                return false;
            v = c.frame.fnSp;
        }
        else if (r == 28)
        {
            if (!c.frame.ctx)
                return false;
            v = gpr64(c.frame.ctx, 28);
        }
        else if (r == reg::ra)
        {
            v = tailReturn(c.frame);
            if (!v)
                return false;
        }
        else if (savedSlot(r) >= 0 && c.frame.ctx)
            v = entryRegister(c.frame, r);
        else
            return false;
        return true;
    }
    // kPrevious: what the previous call got in saved slot `slot`.
    HELD_INLINE bool previous(const Context &c, uint32_t slot, uint64_t &v)
    {
        const uint32_t bit = 1u << slot;
        if (slot >= static_cast<uint32_t>(kSlots) || !(c.state.known & bit))
            return false;
        if (c.state.entry & bit)
        {
            if (!c.frame.ctx)
                return false;
            v = entryRegister(c.frame, savedSlotReg(static_cast<int>(slot)));
        }
        else
            v = c.state.value[slot];
        return true;
    }
    // kResult: the previous call's v0 (0), v1 (1) or f0's bits (2).
    HELD_INLINE bool result(const Context &c, uint32_t which, uint64_t &v)
    {
        if (which > 2 || !(c.state.results & (1u << which)))
            return false;
        v = which == 0 ? c.state.v0 : which == 1 ? c.state.v1 : c.state.f0;
        return true;
    }
    // kArgument, kFloatArgument, kTarget: the call's.
    HELD_INLINE bool argument(const Context &c, uint32_t i, uint64_t &v)
    {
        if (i >= static_cast<uint32_t>(c.regs.intCount))
            return false;
        v = c.regs.a[i];
        return true;
    }
    HELD_INLINE bool floatArgument(const Context &c, uint32_t i, uint64_t &v)
    {
        if (i >= static_cast<uint32_t>(c.regs.floatCount))
            return false;
        v = bitsOf(c.regs.f[i]);
        return true;
    }
    HELD_INLINE bool target(const Context &c, uint64_t &v)
    {
        v = c.regs.target;
        return true;
    }
    // kEntryLoad, kReturnLoad: as read when the function was entered, when the last call returned.
    HELD_INLINE bool entryLoad(const Context &c, uint32_t i, uint64_t &v)
    {
        if (i >= static_cast<uint32_t>(kEntryLoads) || !(c.state.entryLoads & (1u << i)))
            return false;
        v = c.state.entryLoad[i];
        return true;
    }
    HELD_INLINE bool returnLoad(const Context &c, uint32_t i, uint64_t &v)
    {
        if (i >= static_cast<uint32_t>(kReturnLoads) || !(c.state.returnLoads & (1u << i)))
            return false;
        v = c.state.returnLoad[i];
        return true;
    }
    // kConst: the immediate, sign-extended.
    inline uint64_t constant(int32_t imm) { return static_cast<uint64_t>(static_cast<int64_t>(imm)); }

    // ---- kLoad: memory at an address, as the original loads it -----------------------------------------------

    // A load the original makes: RAM (any of its mirrors) or the scratchpad; nothing else (an I/O register is not
    // read for this).
    inline bool readMemory(uint32_t address, uint32_t bytes, uint64_t &out)
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        const bool ram = physical < PS2_RAM_SIZE && (address < 0x80000000u || address >= 0xA0000000u ||
                                                     (address & 0xE0000000u) == 0x80000000u);
        const bool scratch = (address & 0xFFFFC000u) == 0x70000000u;
        if (!(ram || scratch) || (address & (bytes - 1u)) != 0u)
            return false;
        // kseg2 and kseg3 (the TLB's, from 0xC0000000) are not RAM's mirrors: the runtime resolves an address
        // there that is not the scratchpad's to RAM's first bytes (getMemPtr), as ee::at did before the guest map
        // (which shows RAM there a 64 MB step at a time), and the table reads what it read then.
        const uint8_t *at = address >= 0xC0000000u && !scratch ? ee::at<uint8_t>(0u) : ee::at<uint8_t>(address);
        if (!at)
            return false;
        out = 0;
        std::memcpy(&out, at, bytes);
        return true;
    }
    // The load of LoadKind `kind` at `address` (the popped value + imm): false when memory may not be read here
    // (c.loads), when it is a word of the function's own frame outside its ee::Local objects (see State), when it
    // is a check's load in a replay (see held.h evaluate), or when it cannot be read (c.unreadable then set).
    HELD_NOINLINE bool load(const Context &c, uint32_t kind, uint32_t address, uint64_t &v);

    // ---- kAlu: the original's operations ----------------------------------------------------------------------

    // Operands popped: one for the forms with an immediate and the one-operand FPU forms, three for movn and movz
    // (source, condition, the old value), else two.
    constexpr int aluOperands(uint32_t op)
    {
        if (op == kMovn || op == kMovz)
            return 3;
        if (op >= kAddiu && op <= kDsra32)
            return 1;
        if (op >= kNegS && op <= kMfc1)
            return 1;
        return 2;
    }
    HELD_INLINE bool alu(uint32_t op, int64_t imm, uint64_t x, uint64_t y, uint64_t z, uint64_t &out)
    {
        const uint64_t ui = static_cast<uint64_t>(imm);
        switch (op)
        {
        case kAddu: out = sx32(static_cast<uint32_t>(x) + static_cast<uint32_t>(y)); return true;
        case kDaddu: out = x + y; return true;
        case kSubu: out = sx32(static_cast<uint32_t>(x) - static_cast<uint32_t>(y)); return true;
        case kDsubu: out = x - y; return true;
        case kAnd: out = x & y; return true;
        case kOr: out = x | y; return true;
        case kXor: out = x ^ y; return true;
        case kNor: out = ~(x | y); return true;
        case kSlt: out = static_cast<int64_t>(x) < static_cast<int64_t>(y) ? 1u : 0u; return true;
        case kSltu: out = x < y ? 1u : 0u; return true;
        case kSllv: out = sx32(static_cast<uint32_t>(x) << (y & 31u)); return true;
        case kSrlv: out = sx32(static_cast<uint32_t>(x) >> (y & 31u)); return true;
        case kSrav: out = sx32(static_cast<uint32_t>(static_cast<int32_t>(x) >> (y & 31u))); return true;
        case kDsllv: out = x << (y & 63u); return true;
        case kDsrlv: out = x >> (y & 63u); return true;
        case kDsrav: out = static_cast<uint64_t>(static_cast<int64_t>(x) >> (y & 63u)); return true;
        case kMovn: out = y != 0u ? x : z; return true;
        case kMovz: out = y == 0u ? x : z; return true;
        case kAddiu: out = sx32(static_cast<uint32_t>(x) + static_cast<uint32_t>(imm)); return true;
        case kDaddiu: out = x + ui; return true;
        case kAndi: out = x & (ui & 0xFFFFu); return true;
        case kOri: out = x | (ui & 0xFFFFu); return true;
        case kXori: out = x ^ (ui & 0xFFFFu); return true;
        case kSlti: out = static_cast<int64_t>(x) < imm ? 1u : 0u; return true;
        case kSltiu: out = x < ui ? 1u : 0u; return true;
        case kSll: out = sx32(static_cast<uint32_t>(x) << (imm & 31)); return true;
        case kSrl: out = sx32(static_cast<uint32_t>(x) >> (imm & 31)); return true;
        case kSra: out = sx32(static_cast<uint32_t>(static_cast<int32_t>(x) >> (imm & 31))); return true;
        case kDsll: out = x << (imm & 31); return true;
        case kDsrl: out = x >> (imm & 31); return true;
        case kDsra: out = static_cast<uint64_t>(static_cast<int64_t>(x) >> (imm & 31)); return true;
        case kDsll32: out = x << (32 + (imm & 31)); return true;
        case kDsrl32: out = x >> (32 + (imm & 31)); return true;
        case kDsra32: out = static_cast<uint64_t>(static_cast<int64_t>(x) >> (32 + (imm & 31))); return true;
        // The FPU as generated code runs it (ps2_runtime_macros.h's FPU_* without their trap hook).
        case kAddS: out = bitsOf(ps2_fadd(asFloat(x), asFloat(y))); return true;
        case kSubS: out = bitsOf(ps2_fsub(asFloat(x), asFloat(y))); return true;
        case kMulS: out = bitsOf(ps2_fmul(asFloat(x), asFloat(y))); return true;
        case kDivS: out = bitsOf(ps2_fdiv_nearest(asFloat(x), asFloat(y))); return true;
        case kMaxS: out = bitsOf(ps2_fmax(asFloat(x), asFloat(y))); return true;
        case kMinS: out = bitsOf(ps2_fmin(asFloat(x), asFloat(y))); return true;
        case kNegS: out = bitsOf(-asFloat(x)); return true;
        case kAbsS: out = bitsOf(std::fabs(asFloat(x))); return true;
        case kSqrtS: out = bitsOf(ps2_fsqrt_nearest(asFloat(x))); return true;
        case kCvtSW: out = bitsOf(ps2_cvt_s_w(static_cast<int32_t>(x))); return true;
        case kCvtWS: out = static_cast<uint32_t>(ps2_cvt_w_s(asFloat(x))); return true;
        case kMtc1: out = zx32(x); return true;
        case kCEqS: out = FPU_C_EQ_S(asFloat(x), asFloat(y)) ? 1u : 0u; return true;
        case kCLtS: out = FPU_C_LT_S(asFloat(x), asFloat(y)) ? 1u : 0u; return true;
        case kCLeS: out = FPU_C_LE_S(asFloat(x), asFloat(y)) ? 1u : 0u; return true;
        case kMfc1: out = sx32(x); return true;
        case kMultLo: case kMultHi:
        {
            const int64_t product = static_cast<int64_t>(static_cast<int32_t>(x)) * static_cast<int32_t>(y);
            out = sx32(static_cast<uint64_t>(op == kMultLo ? product : product >> 32));
            return true;
        }
        case kMultuLo: case kMultuHi:
        {
            const uint64_t product = static_cast<uint64_t>(static_cast<uint32_t>(x)) * static_cast<uint32_t>(y);
            out = sx32(op == kMultuLo ? product : product >> 32);
            return true;
        }
        case kDivLo: case kDivHi:
        {
            const int32_t dividend = static_cast<int32_t>(x), divisor = static_cast<int32_t>(y);
            int64_t lo, hi;
            if (divisor == 0)
            {
                lo = dividend < 0 ? 1 : -1;
                hi = dividend;
            }
            else if (divisor == -1 && dividend == INT32_MIN)
            {
                lo = INT32_MIN;
                hi = 0;
            }
            else
            {
                lo = dividend / divisor;
                hi = dividend % divisor;
            }
            out = static_cast<uint64_t>(op == kDivLo ? lo : hi);
            return true;
        }
        case kDivuLo: case kDivuHi:
        {
            const uint32_t dividend = static_cast<uint32_t>(x), divisor = static_cast<uint32_t>(y);
            if (divisor == 0)
                out = op == kDivuLo ? ~0ull : sx32(dividend);
            else
                out = sx32(op == kDivuLo ? dividend / divisor : dividend % divisor);
            return true;
        }
        default: return false;
        }
    }
}
