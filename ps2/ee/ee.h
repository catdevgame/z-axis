#pragma once

// The Emotion Engine as the readable game code sees it: guest memory, the
// R5900's float arithmetic and pointers as the game stores them.
//
// Readable code works on the game's own data in EE RAM, through structs
// laid out like the game's (engine/zge/*.h). A struct's pointer fields are
// ee::Ptr<T> (a 32-bit EE address); its float fields are ee::Float, whose
// operators round like the EE FPU does (towards zero, with the R5900's
// adder; see ee_instructions.h), so a readable function gives the same
// bits as the recompiled one and the game's logic stays identical to the
// PS2's (games/aggressive/tools/logiccheck.py compares it with PCSX2 bit for bit).
//
// Host pointers into EE RAM are fine to hold for the length of a call;
// ee::at<T>(address) and ee::addressOf(pointer) convert between the two.

#include "flavour.h"
#include "ee_instructions.h"
#include "runtime/memory.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "runtime/lane_watch.h"

namespace ee
{
    // ---- guest memory ------------------------------------------------------

    // Base of the guest map (runtime/guest_map.h): the 4 GB of guest addresses, EE RAM at 0. The runtime's map in
    // the game (set by game::readable::install), the replay memory's in zx-fncheck.
    inline uint8_t *g_ram = nullptr;

#if ZX_GUEST_MAP_CHECK
    // A tool build's: the access at `address` resolved as before the guest map too, and told when the two differ
    // or it is outside RAM's own view (ee/guest_map_check.cpp).
    // (Not inlined: its return address is the readable function's.)
    void checkGuestAccess(uint32_t address) __attribute__((noinline));
#endif
    // The high half of an address as `lui` makes it, for the low half's sign: what the
    // original keeps in a register to reach a symbol: ee::high(sym::g_tuning). (consteval: a
    // number in the code from the first pass, so the object is the literal's.)
    consteval uint32_t high(uint32_t address) { return (address + 0x8000u) & 0xFFFF0000u; }
    // An address in the uncached mirror.
    consteval uint32_t uncached(uint32_t address) { return address | 0x20000000u; }

    // Host pointer to the T at an EE address: the guest map's base + address. The map puts RAM wherever the
    // generated code's loads take it (address & PS2_RAM_MASK), so main RAM wraps as they do, address 0 included
    // (the original's recompiled reads through a null object land in low RAM and go on: cov-ragdoll's
    // solveDynamicsIsland), the uncached mirrors and kseg0/kseg1 included; the scratchpad is mapped at its
    // addresses. I/O registers are not memory: ee::io (io.h) reaches them.
    template <typename T>
    inline T *at(uint32_t address)
    {
#if ZX_GUEST_MAP_CHECK
        checkGuestAccess(address);
#endif
#if ZX_LANE_WATCH_BUILD
        // A tool build's: the lane watch (runtime/lane_watch.h) counts the readable access by its return address
        // (the whole T: a field read through the pointer is not seen apart; 1 byte for a type not complete here).
        if (zx::lanes::g_watchOn)
        {
            if constexpr (requires { sizeof(T); })
                zx::lanes::watchReadable(address, static_cast<uint32_t>(sizeof(T)));
            else
                zx::lanes::watchReadable(address, 1u);
        }
#endif
        return reinterpret_cast<T *>(g_ram + address);
    }

    // Whether a host pointer is guest memory: in the guest map, at any guest address (a mirror's, the
    // scratchpad's) or the guard past 4 GB.
    inline bool inGuestMap(const void *pointer)
    {
        const uintptr_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(g_ram);
        return offset < guest_map::kSpace + guest_map::kGuard;
    }

    // The EE address of a host pointer into EE RAM (0 for nullptr): the guest address it was made from, a
    // mirror's or the scratchpad's included.
    inline uint32_t addressOf(const void *pointer)
    {
        if (!pointer)
            return 0u;
        return static_cast<uint32_t>(static_cast<const uint8_t *>(pointer) - g_ram);
    }

    // A guest pointer as a host one, for C++ that tests it: 0 is nullptr.
    template <typename T>
    inline T *pointerAt(uint32_t address)
    {
        return address != 0u ? at<T>(address) : nullptr;
    }

    // A pointer as the game stores it: a 32-bit EE address. Dereferences to
    // host memory, so `part->positions->data` reads like the game's C; a null
    // one reads low RAM as the generated code does. `if (ptr)`, get() and the
    // conversion to T * still say null.
    template <typename T>
    struct Ptr
    {
        uint32_t address = 0u;

        Ptr() = default;
        constexpr explicit Ptr(uint32_t a) : address(a) {}
        Ptr(T *pointer) : address(addressOf(pointer)) {}
        // A pointer to const from one to the same type, as T * converts to const T *.
        template <typename U>
            requires std::is_same_v<const U, T> && (!std::is_same_v<U, T>)
        constexpr Ptr(Ptr<U> other) : address(other.address) {}

        T *get() const { return pointerAt<T>(address); }
        T *operator->() const { return at<T>(address); }
        T &operator*() const { return *at<T>(address); }
        // Guest address arithmetic, wrapping in 32 bits as the original's sll
        // and addu do, then through at(): a uint32 index of -1 is the element
        // before, wherever the wrapped address lands, not a host pointer far
        // outside guest RAM (sampleTranslationKey's key -1 on an empty track).
        T &operator[](size_t i) const
        {
            return *at<T>(address + static_cast<uint32_t>(i) * static_cast<uint32_t>(sizeof(T)));
        }
        operator T *() const { return get(); }
        // Arithmetic on the EE address itself, as the game's C does: an
        // address in an uncached mirror (0x20000000, 0x30000000) stays in it,
        // which a round trip through a host pointer would lose.
        Ptr operator+(ptrdiff_t n) const { return Ptr(address + static_cast<uint32_t>(n * static_cast<ptrdiff_t>(sizeof(T)))); }
        Ptr operator-(ptrdiff_t n) const { return *this + (-n); }
        Ptr &operator+=(ptrdiff_t n) { return *this = *this + n; }
        Ptr &operator-=(ptrdiff_t n) { return *this = *this - n; }
        Ptr &operator++() { return *this += 1; }
        // Byte offset arithmetic as the game does it on raw addresses.
        template <typename U>
        Ptr<U> cast() const { return Ptr<U>(address); }
    };
    static_assert(sizeof(Ptr<int>) == 4, "EE pointers are 32 bits");

    // A global variable of the game at a fixed EE address:
    //     inline ee::Global<ee::Ptr<StringTable>, g_stringTable> g_stringTable;
    //     const StringTable *table = *g_stringTable;
    template <typename T, uint32_t Address>
    struct Global
    {
        static constexpr uint32_t address = Address;
        T &operator*() const { return *at<T>(Address); }
        T *operator->() const { return at<T>(Address); }
        T *get() const { return at<T>(Address); }
    };

    // ---- R5900 float arithmetic -------------------------------------------

    // A float in an EE FPU register or in the game's memory. Arithmetic
    // rounds as the EE does (ps2_fadd and friends: towards zero, operands
    // aligned like the R5900's adder; DIV.S and SQRT.S to nearest, as PCSX2
    // runs them), exponent-0 values read as zero, and there is no Inf or
    // NaN (overflow saturates to +-0x7FFFFFFF). Comparisons are the EE's
    // C.cond.S (ps2_to_double: an exponent-255 value is a number).
    //
    // The order of operations is the game's: a + b + c is (a + b) + c, as
    // the compiled code adds. Readable code is written in that order.
    class Float
    {
    public:
        constexpr Float() = default;
        constexpr Float(float v) : m_v(v) {}

        constexpr float raw() const { return m_v; }
        uint32_t bits() const { return std::bit_cast<uint32_t>(m_v); }
        static Float fromBits(uint32_t b) { return Float(std::bit_cast<float>(b)); }

        friend Float operator+(Float a, Float b) { return ps2_fadd(a.m_v, b.m_v); }
        friend Float operator-(Float a, Float b) { return ps2_fsub(a.m_v, b.m_v); }
        friend Float operator*(Float a, Float b) { return ps2_fmul(a.m_v, b.m_v); }
        // DIV.S (to nearest; a zero divisor gives +-max).
        friend Float operator/(Float a, Float b) { return ps2_fdiv_nearest(a.m_v, b.m_v); }
        // NEG.S flips the sign bit only.
        Float operator-() const { return -m_v; }
        Float &operator+=(Float b) { return *this = *this + b; }
        Float &operator-=(Float b) { return *this = *this - b; }
        Float &operator*=(Float b) { return *this = *this * b; }
        Float &operator/=(Float b) { return *this = *this / b; }

        friend bool operator<(Float a, Float b) { return ps2_to_double(a.m_v) < ps2_to_double(b.m_v); }
        friend bool operator<=(Float a, Float b) { return ps2_to_double(a.m_v) <= ps2_to_double(b.m_v); }
        friend bool operator>(Float a, Float b) { return b < a; }
        friend bool operator>=(Float a, Float b) { return b <= a; }
        friend bool operator==(Float a, Float b) { return ps2_to_double(a.m_v) == ps2_to_double(b.m_v); }
        friend bool operator!=(Float a, Float b) { return !(a == b); }

    private:
        float m_v = 0.0f;
    };
    static_assert(sizeof(Float) == 4 && std::is_trivially_copyable_v<Float>, "an EE float is 4 bytes");

    // SQRT.S (to nearest).
    inline Float sqrt(Float a) { return ps2_fsqrt_nearest(a.raw()); }
    // RSQRT.S: a / sqrt(b), both steps towards zero.
    inline Float rsqrt(Float a, Float b) { return ps2_frsqrt(a.raw(), b.raw()); }
    // ABS.S clears the sign bit only.
    inline Float abs(Float a) { return std::bit_cast<float>(a.bits() & 0x7FFFFFFFu); }
    inline Float max(Float a, Float b) { return ps2_fmax(a.raw(), b.raw()); }
    inline Float min(Float a, Float b) { return ps2_fmin(a.raw(), b.raw()); }
    // CVT.W.S (towards zero, saturating) and CVT.S.W.
    inline int32_t toInt(Float a) { return ps2_cvt_w_s(a.raw()); }
    inline Float toFloat(int32_t w) { return ps2_cvt_s_w(w); }
}
