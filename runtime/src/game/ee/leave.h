#pragma once

// Registers a function leaves for its caller besides its result: the ones
// some caller reads after the call before it sets them (fncheck_tables.h
// ResultReader: a v1, an argument or temporary register, an FPU register
// other than f0), when the original sets them. A readable function names
// them just before it returns (after its last call: ee::call gives the
// context back as it was), so the caller finds the original's values:
//
//     ee::leave(ee::reg::v1, count);       // the original's v1 at its return
//     ee::leaveFloat(3, remainder);       // and its f3
//     return result;
//
// readable_draft.py writes them for the registers the generated callers
// read; ps2xFnCheck notes a readable function that leaves another value
// (regs_left). Off a readable call (no context) they do nothing.

#include "abi.h"

#include <type_traits>

namespace ee
{
    // A GPR as the original leaves it: 32-bit values sign-extended (as the
    // EE's 32-bit instructions write them), 64-bit ones whole, pointers as
    // their EE address.
    template <typename T>
    inline void leave(int r, T value)
    {
        Frame *frame = t_ee.frame;
        if (!frame || !frame->ctx)
            return;
        uint64_t bits;
        if constexpr (std::is_pointer_v<T>)
            bits = Abi<T>::toReg(value);
        else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
            bits = Abi<T>::toReg(value);
        else
            bits = Abi<uint32_t>::toReg(value.address); // ee::Ptr
        setGpr64(frame->ctx, r, bits);
    }

    // An FPU register as the original leaves it.
    inline void leaveFloat(int f, Float value)
    {
        if (Frame *frame = t_ee.frame; frame && frame->ctx)
            frame->ctx->f[f] = value.raw();
    }
}
