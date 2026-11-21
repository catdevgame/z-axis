#pragma once

// A game function held as data, a callback or a table's entry, with what it
// takes and gives: `pool->allocate(bytes, alignment)` is the original's jump
// through the pointer. 32 bits, as the game stores it; 0 is none.

#include "plain.h"

#include <cstdint>

namespace ee
{
    template <typename Signature>
    struct FunctionPtr;
    template <typename R, typename... A>
    struct FunctionPtr<R(A...)>
    {
        uint32_t address = 0u;

        FunctionPtr() = default;
        constexpr FunctionPtr(uint32_t a) : address(a) {}
        // The same function under another signature (a visit typed for the records handed where the tree's
        // visit is taken): the one word, as the game stores it.
        template <typename Other>
        constexpr explicit FunctionPtr(FunctionPtr<Other> other) : address(other.address) {}

        explicit operator bool() const { return address != 0u; }
        bool operator==(const FunctionPtr &) const = default;
        R operator()(A... args) const { return game<R>(address, args...); }
        // The original's tail jump through the pointer.
        R tail(A... args) const { return tailCall<R>(address, detail::passArgument(args)...); }
    };
    static_assert(sizeof(FunctionPtr<void()>) == 4, "game function pointers are 32 bits");

    template <typename Signature>
    struct Abi<FunctionPtr<Signature>, void>
    {
        static constexpr bool isFloat = false;
        static FunctionPtr<Signature> fromReg(uint64_t r) { return FunctionPtr<Signature>(static_cast<uint32_t>(r)); }
        static uint64_t toReg(FunctionPtr<Signature> f) { return Abi<uint32_t>::toReg(f.address); }
    };
}
