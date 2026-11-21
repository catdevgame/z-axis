#ifndef ZX_GAME_MODULE_FREESTANDING_STD_H
#define ZX_GAME_MODULE_FREESTANDING_STD_H

// A game module is compiled with no library headers (-nostdlibinc: the app builds it on the player's machine, which
// has no SDK). Generated code, the EE's macros (ee_instructions.h) and VU1's core (vu1/vu1_core.h) use a
// few standard names; here they are, on the compiler's builtins, with the standard's meaning (the compiler still knows
// the C functions by name: it may call memcpy, floorf and the like, which the module gets from the runtime,
// game_module/module_entry.cpp).

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    void *memcpy(void *destination, const void *source, size_t size);
    void *memmove(void *destination, const void *source, size_t size);
    void *memset(void *destination, int value, size_t size);
    float fabsf(float x);
    float sqrtf(float x);
    float floorf(float x);
    float ceilf(float x);
    float roundf(float x);
    float truncf(float x);
    double fabs(double x);
    double sqrt(double x);
}

// <cmath>'s classification in C++ form (generated code and the macros call isnan on floats).
inline bool isnan(float x) { return __builtin_isnan(x); }
inline bool isnan(double x) { return __builtin_isnan(x); }

namespace std
{
    using ::memcpy;
    using ::memmove;
    using ::memset;

    inline float sqrt(float x) { return __builtin_sqrtf(x); }
    inline double sqrt(double x) { return __builtin_sqrt(x); }
    inline float fabs(float x) { return __builtin_fabsf(x); }
    inline double fabs(double x) { return __builtin_fabs(x); }
    inline bool signbit(float x) { return __builtin_signbit(x); }
    inline bool signbit(double x) { return __builtin_signbit(x); }
    inline bool isnan(float x) { return __builtin_isnan(x); }
    inline bool isnan(double x) { return __builtin_isnan(x); }

    template <class To, class From>
    constexpr To bit_cast(const From &from) noexcept
    {
        static_assert(sizeof(To) == sizeof(From), "bit_cast: sizes differ");
        return __builtin_bit_cast(To, from);
    }

    // For the unsigned types the macros (and VU1's core) count in.
    constexpr int countl_zero(uint32_t x) noexcept { return x == 0u ? 32 : __builtin_clz(x); }
    constexpr int countl_zero(uint64_t x) noexcept { return x == 0u ? 64 : __builtin_clzll(x); }
    template <class T>
    constexpr int countr_zero(T x) noexcept
    {
        static_assert(T(-1) > T(0), "countr_zero: an unsigned type");
        return x == 0u ? static_cast<int>(sizeof(T) * 8u)
                       : (sizeof(T) <= 4u ? __builtin_ctz(static_cast<uint32_t>(x)) : __builtin_ctzll(x));
    }

    using nullptr_t = decltype(nullptr);
    template <class A, class B>
    inline constexpr bool is_same_v = __is_same(A, B);

    template <class T>
    constexpr const T &min(const T &a, const T &b) { return b < a ? b : a; }
    template <class T>
    constexpr const T &max(const T &a, const T &b) { return a < b ? b : a; }

    template <class T>
    struct numeric_limits;
    template <>
    struct numeric_limits<int32_t>
    {
        static constexpr int32_t min() noexcept { return INT32_MIN; }
        static constexpr int32_t max() noexcept { return INT32_MAX; }
    };
    template <>
    struct numeric_limits<float>
    {
        static constexpr float min() noexcept { return __FLT_MIN__; } // the smallest normal
        static constexpr float max() noexcept { return __FLT_MAX__; }
    };
}

#endif // ZX_GAME_MODULE_FREESTANDING_STD_H
