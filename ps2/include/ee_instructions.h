#ifndef PS2_EE_INSTRUCTIONS_H
#define PS2_EE_INSTRUCTIONS_H
// The EE's instructions as generated code writes them: registers, arithmetic, FPU, MMI, VU0 macro mode, loads and
// stores. The runtime's readable code uses them too. In a game module (ZX_GAME_MODULE, game_module/) they are
// compiled with no library header, against the module's view of the runtime (game_module/module_runtime.h).
#if defined(ZX_GAME_MODULE)
#include "game_module/module_runtime.h"
#else
#include <cstdint>
#include <cmath>
#include <limits>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "machine.h"
#endif
#include "runtime/lane_watch.h"

// The runtime's float checks (ZX_FLOAT_CENSUS, ZX_FPU_TRAP builds): where generated code reports to.
void zx_float_census(int kind, uint32_t a, uint32_t b, uint32_t r);
void zx_fpu_trap_report(const char *op, uint32_t a, uint32_t b, uint32_t r);

// In a game module the helpers below are the module's own (an inline namespace: found as if declared here, linked
// apart from the runtime's copies, which name the library's std:: where the module's name the builtins).
#if defined(ZX_GAME_MODULE)
inline namespace game_module
{
#endif

// The lane watch (runtime/lane_watch.h, tools/checks/wlane.py): in a ZX_LANE_WATCH_BUILD build every guest load
// and store below tells the watch its bytes; the app's and the gate's builds compile it to nothing.
#ifndef ZX_LANE_WATCH_BUILD
#define ZX_LANE_WATCH_BUILD 0
#endif
#if ZX_LANE_WATCH_BUILD
#define ZX_LANE_WATCH(addr, size, write)                                              \
    do                                                                                  \
    {                                                                                   \
        if (zx::lanes::g_watchOn)                                                     \
            zx::lanes::watchAccess((addr), (size), (write), ctx->pc);                 \
    } while (0)
// The differential's hooks (lane_watch.h, ZX_LANE_DIFF): a load before it reads, a store after it wrote (the
// bytes it left), and in a wlane_branches.py copy of the generated code each conditional branch's direction and each
// register jump's target.
#define ZX_LANE_DIFF_LOAD(addr, size)                                                  \
    do                                                                                  \
    {                                                                                   \
        if (zx::lanes::g_diffOn)                                                      \
            zx::lanes::diffAccess(rdram, (addr), (size), false, ctx->pc);             \
    } while (0)
#define ZX_LANE_DIFF_STORED(addr, size)                                                \
    do                                                                                  \
    {                                                                                   \
        if (zx::lanes::g_diffOn)                                                      \
            zx::lanes::diffAccess(rdram, (addr), (size), true, ctx->pc);              \
    } while (0)
#define ZX_LANE_BRANCH(pc, taken) (zx::lanes::g_diffOn ? zx::lanes::diffBranch((pc), (taken)) : (taken))
#define ZX_LANE_JUMP(pc, target) (zx::lanes::g_diffOn ? zx::lanes::diffJump((pc), (target)) : (target))
#else
#define ZX_LANE_WATCH(addr, size, write) \
    do                                     \
    {                                      \
    } while (0)
#define ZX_LANE_DIFF_LOAD(addr, size) \
    do                                  \
    {                                   \
    } while (0)
#define ZX_LANE_DIFF_STORED(addr, size) \
    do                                    \
    {                                     \
    } while (0)
#define ZX_LANE_BRANCH(pc, taken) (taken)
#define ZX_LANE_JUMP(pc, target) (target)
#endif

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// PS2 float semantics (EE FPU and VU0 macro mode). The PS2 format has no Inf, NaN or denormals:
//   - exponent 255 is an ordinary exponent, so 0x7F800000 is 2^128 and the largest value is 0x7FFFFFFF;
//   - exponent 0 is zero whatever the mantissa holds;
//   - a result of 2^129 or more saturates to +-0x7FFFFFFF, one below 2^-126 becomes +-0;
//   - x/0 gives +-0x7FFFFFFF.
// A host float cannot hold the exponent-255 values, so anything that touches them is computed in double.
// That path is rare and kept out of line; the inline part is the host operation and a range check.
// An operand with exponent 0 and a non-zero mantissa (a host denormal) is zero, as on the hardware and in
// PCSX2 (FPU/VU0 "DenormalsAreZero"): ADD/SUB/MUL and the vector operations clear such operands first.
// Checked against the hardware results in ps2autotests (tests/cpu/ee_fpu).
//
// Rounding: the EE FPU and VU0 round toward zero, and PCSX2 runs both in "Chop" (round toward zero) mode by
// default, except EE FPU DIV.S ("FPUDiv" mode, round to nearest by default) and SQRT.S (always nearest in
// PCSX2); see ps2_fdiv_nearest. RSQRT.S is SQRTSS + DIVSS in Chop mode in PCSX2. The host stays in round-to-nearest; each helper takes the host result r and, when the exact result
// is nearer to zero than r, steps r one unit toward zero (ps2_chop). The exact comparison is TwoSum for
// ADD/SUB and the double-precision result for MUL/DIV/SQRT/CVT (exact for MUL and CVT.S.W; for DIV and SQRT
// the double result can only equal r when the exact result does). Only generated EE code uses these helpers,
// so VU1, GS, IOP and runtime float code keep the host's rounding and their results do not change.
// Define PS2_EE_FLOAT_ROUND_NEAREST to get the host's round-to-nearest results back (for comparisons).
// Not emulated: the hardware multiplier also drops a bit in some products (1.0 * 0x3FFFFFFF = 0x3FFFFFFE),
// which PCSX2 does not do either.
#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

constexpr uint32_t PS2_FLOAT_MAX_BITS = 0x7FFFFFFFu;
constexpr uint32_t PS2_FLOAT_EXP_MASK = 0x7F800000u;
constexpr uint32_t PS2_FLOAT_SIGN_MASK = 0x80000000u;

#if defined(PS2_EE_FLOAT_ROUND_NEAREST)
constexpr bool PS2_EE_FLOAT_CHOP = false;
#else
constexpr bool PS2_EE_FLOAT_CHOP = true;
#endif

// Round-toward-zero conversion of a double in the PS2 range: the float mantissa is the top 23 bits of the
// double mantissa. Covers exponent 255 (2^128 to 2^129) as well.
inline uint32_t ps2_double_to_bits_chop(double d)
{
    const uint64_t b = std::bit_cast<uint64_t>(d);
    const uint32_t sign = static_cast<uint32_t>(b >> 32) & PS2_FLOAT_SIGN_MASK;
    const uint32_t exponent = static_cast<uint32_t>((b >> 52) & 0x7FFu) - 1023u + 127u;
    return sign | (exponent << 23) | static_cast<uint32_t>((b >> 29) & 0x7FFFFFu);
}

// True when a host float operation cannot be trusted with x: exponent 255, or exponent 0 with a mantissa.
inline bool ps2_fspecial(float x)
{
    const uint32_t t = std::bit_cast<uint32_t>(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}

inline double ps2_to_double(float x)
{
    uint32_t b = std::bit_cast<uint32_t>(x);
    const uint32_t exponent = b & PS2_FLOAT_EXP_MASK;
    if (exponent == 0u)
        return static_cast<double>(std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK));
    if (exponent == PS2_FLOAT_EXP_MASK)
        return static_cast<double>(std::bit_cast<float>(b - 0x00800000u)) * 2.0;
    return static_cast<double>(x);
}

inline float ps2_from_double(double d, bool chop = PS2_EE_FLOAT_CHOP)
{
    const double magnitude = std::fabs(d);
    const uint32_t sign = std::signbit(d) ? PS2_FLOAT_SIGN_MASK : 0u;
    if (magnitude >= 0x1p129)
        return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
    if (magnitude < 0x1p-126)
        return std::bit_cast<float>(sign);
    if (chop)
        return std::bit_cast<float>(ps2_double_to_bits_chop(d));
    if (magnitude >= 0x1p128)
    {
        // Convert one exponent lower, then put the exponent back as 255.
        const uint32_t b = std::bit_cast<uint32_t>(static_cast<float>(d * 0.5));
        if ((b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK)
            return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
        return std::bit_cast<float>(b + 0x00800000u);
    }
    // A value that rounds up to 2^128 comes out as the host's Inf pattern, which is 2^128 on the PS2.
    return static_cast<float>(d);
}

// r is a normal host result that is one unit too far from zero when `away` is set. Stepping down from
// +-2^-126 leaves exponent 0, which the PS2 flushes to +-0.
inline float ps2_chop(float r, bool away)
{
    uint32_t b = std::bit_cast<uint32_t>(r) - (away ? 1u : 0u);
    if ((b & PS2_FLOAT_EXP_MASK) == 0u)
        b &= PS2_FLOAT_SIGN_MASK;
    return std::bit_cast<float>(b);
}

// An exponent-0 value as +-0 (the hardware reads it as zero; PCSX2 runs with denormals-are-zero).
inline float ps2_daz(float x)
{
    const uint32_t b = std::bit_cast<uint32_t>(x);
    return (b & PS2_FLOAT_EXP_MASK) == 0u ? std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK) : x;
}

// TwoSum: e = (a + b) - r exactly, for r the round-to-nearest sum. r is too far from zero when e is
// non-zero and of the other sign. An exponent-0 operand counts as zero here, so the tiny host value it
// added to r (if any) does not step r.
inline float ps2_chop_sum(float a, float b, float r)
{
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    a = ps2_daz(a);
    b = ps2_daz(b);
    const float bb = r - a;
    const float e = (a - (r - bb)) + (b - bb);
    const uint32_t eb = std::bit_cast<uint32_t>(e);
    return ps2_chop(r, (eb & 0x7FFFFFFFu) != 0u && ((eb ^ std::bit_cast<uint32_t>(r)) & PS2_FLOAT_SIGN_MASK) != 0u);
}

// exact is the exact result, or one that is on the same side of every float as the exact result.
inline float ps2_chop_exact(double exact, float r)
{
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    return ps2_chop(r, std::fabs(static_cast<double>(r)) > std::fabs(exact));
}

PS2_FLOAT_NOINLINE inline float ps2_fadd_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a + b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return ps2_chop_sum(a, b, r);
    return ps2_from_double(ps2_to_double(a) + ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fsub_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a - b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return ps2_chop_sum(a, -b, r);
    return ps2_from_double(ps2_to_double(a) - ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fmul_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a * b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return ps2_chop_exact(static_cast<double>(a) * static_cast<double>(b), r);
    return ps2_from_double(ps2_to_double(a) * ps2_to_double(b));
}

// A zero divisor gives +-max with the sign of fs xor ft.
PS2_FLOAT_NOINLINE inline float ps2_fdiv_slow(float a, float b, bool chop = PS2_EE_FLOAT_CHOP)
{
    const uint32_t ua = std::bit_cast<uint32_t>(a);
    const uint32_t ub = std::bit_cast<uint32_t>(b);
    if ((ub & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>(((ua ^ ub) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    const float r = a / b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return chop ? ps2_chop_exact(static_cast<double>(a) / static_cast<double>(b), r) : r;
    return ps2_from_double(ps2_to_double(a) / ps2_to_double(b), chop);
}

// sqrt(|ft|); the result is never negative, sqrt(-0) is +0.
PS2_FLOAT_NOINLINE inline float ps2_fsqrt_slow(float a, bool chop = PS2_EE_FLOAT_CHOP)
{
    if ((std::bit_cast<uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u)
        return 0.0f;
    return ps2_from_double(std::sqrt(std::fabs(ps2_to_double(a))), chop);
}

// fs / sqrt(|ft|). The sign of ft is ignored everywhere, so a zero ft gives +-max with the sign of fs alone.
PS2_FLOAT_NOINLINE inline float ps2_frsqrt_slow(float a, float b)
{
    if ((std::bit_cast<uint32_t>(b) & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>((std::bit_cast<uint32_t>(a) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    // The root is rounded to a float before the division, as on the hardware.
    const float root = ps2_from_double(std::sqrt(std::fabs(ps2_to_double(b))));
    return ps2_from_double(ps2_to_double(a) / static_cast<double>(root));
}

// True when a, b and the host result r are all normal numbers, so r is also the PS2 result.
inline bool ps2_fall_normal(float a, float b, float r)
{
    // Doubling drops the sign; adding one exponent step wraps exponent 255 round to 0 next to exponent 0.
    const uint32_t sa = std::bit_cast<uint32_t>(a) * 2u + 0x01000000u;
    const uint32_t sb = std::bit_cast<uint32_t>(b) * 2u + 0x01000000u;
    const uint32_t sr = std::bit_cast<uint32_t>(r) * 2u + 0x01000000u;
    uint32_t lowest = sa < sb ? sa : sb;
    lowest = lowest < sr ? lowest : sr;
    return lowest >= 0x02000000u;
}

// The same test on the result alone. An exponent-255 operand always leaves exponent 255 in the host result
// of an addition, subtraction or multiplication (Inf or NaN), so this finds those operands as well.
inline bool ps2_fnormal(float r)
{
    return (std::bit_cast<uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

// The R5900 adder has no guard bits: the operand with the smaller exponent loses the mantissa bits that are
// shifted out beyond one extra bit, and is gone entirely (only its sign stays) when the exponents differ by 25
// or more. PCSX2 emulates this for EE FPU ADD.S/SUB.S/MADD.S/MSUB.S (iFPU.cpp FPU_ADD_SUB, enabled by
// FPU_CORRECT_ADD_SUB) by masking that operand before an IEEE add in Chop mode; this does the same.
// VU0 (and PCSX2's microVU) do not do this, so the vector helpers below do not use it.
inline void ps2_fpu_align_addends(float &a, float &b)
{
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return;
    uint32_t ua = std::bit_cast<uint32_t>(a);
    uint32_t ub = std::bit_cast<uint32_t>(b);
    const int diff = static_cast<int>((ua >> 23) & 0xFFu) - static_cast<int>((ub >> 23) & 0xFFu);
    if (diff >= 25)
        ub &= PS2_FLOAT_SIGN_MASK;
    else if (diff > 1)
        ub &= 0xFFFFFFFFu << (diff - 1);
    else if (diff <= -25)
        ua &= PS2_FLOAT_SIGN_MASK;
    else if (diff < -1)
        ua &= 0xFFFFFFFFu << (-diff - 1);
    a = std::bit_cast<float>(ua);
    b = std::bit_cast<float>(ub);
}

// Census, only with -DZX_FLOAT_CENSUS (a separate dev build; devdep1): every EE FPU and VU0 macro-mode operation
// is counted by what sets it apart from the host's IEEE arithmetic (rounding toward zero, the R5900 adder's
// operand alignment, denormals read as zero, an overflow or special operand), zx_float_census in
// state_probe.cpp; the totals print at exit. Reads the operands as given and the result as returned.
#if defined(ZX_FLOAT_CENSUS)
enum Ps2FloatCensusKind : int { kCensusAdd, kCensusSub, kCensusMul, kCensusDiv, kCensusSqrt, kCensusCvtWS, kCensusCvtSW,
                                kCensusVecAdd, kCensusVecSub, kCensusVecMul };
inline float ps2_census_f(int kind, float a, float b, float r)
{
    zx_float_census(kind, std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b), std::bit_cast<uint32_t>(r));
    return r;
}
inline int32_t ps2_census_i(int kind, float a, int32_t r)
{
    zx_float_census(kind, std::bit_cast<uint32_t>(a), 0u, static_cast<uint32_t>(r));
    return r;
}
inline float ps2_census_fi(int kind, int32_t w, float r)
{
    zx_float_census(kind, static_cast<uint32_t>(w), 0u, std::bit_cast<uint32_t>(r));
    return r;
}
#define PS2_CENSUS_F(kind, a, b, r) ps2_census_f(kind, a, b, r)
#define PS2_CENSUS_I(kind, a, r) ps2_census_i(kind, a, r)
#define PS2_CENSUS_FI(kind, w, r) ps2_census_fi(kind, w, r)
#define PS2_CENSUS_V(op, a, b, r) ps2_census_v(op, a, b, r)
#else
#define PS2_CENSUS_F(kind, a, b, r) (r)
#define PS2_CENSUS_I(kind, a, r) (r)
#define PS2_CENSUS_FI(kind, w, r) (r)
#define PS2_CENSUS_V(op, a, b, r) (r)
#endif

inline float ps2_fadd(float a0, float b0)
{
    float a = ps2_daz(a0);
    float b = ps2_daz(b0);
    ps2_fpu_align_addends(a, b);
    const float r = a + b;
    return PS2_CENSUS_F(kCensusAdd, a0, b0, ps2_fnormal(r) ? ps2_chop_sum(a, b, r) : ps2_fadd_slow(a, b));
}

inline float ps2_fsub(float a0, float b0)
{
    float a = ps2_daz(a0);
    float b = ps2_daz(b0);
    ps2_fpu_align_addends(a, b);
    const float r = a - b;
    return PS2_CENSUS_F(kCensusSub, a0, b0, ps2_fnormal(r) ? ps2_chop_sum(a, -b, r) : ps2_fsub_slow(a, b));
}

inline float ps2_fmul(float a0, float b0)
{
    const float a = ps2_daz(a0);
    const float b = ps2_daz(b0);
    const float r = a * b;
    // The double product of two floats is exact.
    return PS2_CENSUS_F(kCensusMul, a0, b0,
                        ps2_fnormal(r) ? ps2_chop_exact(static_cast<double>(a) * static_cast<double>(b), r) : ps2_fmul_slow(a, b));
}

// EE FPU DIV.S and SQRT.S. PCSX2 runs DIV.S in its separate "FPUDiv" rounding mode (round to nearest by
// default) and forces round to nearest for SQRT.S, so these keep the host result. The slow path for
// special operands rounds to nearest too.
inline float ps2_fdiv_nearest(float a, float b)
{
    const float r = a / b;
    return PS2_CENSUS_F(kCensusDiv, a, b, ps2_fall_normal(a, b, r) ? r : ps2_fdiv_slow(a, b, false));
}

inline float ps2_fsqrt_nearest(float a)
{
    const float r = std::sqrt(a);
    return PS2_CENSUS_F(kCensusSqrt, a, a, ps2_fall_normal(a, a, r) ? r : ps2_fsqrt_slow(a, false));
}

// VU0 DIV/SQRT (and the EE FPU's RSQRT.S, see ps2_frsqrt) round toward zero like the other operations.
inline float ps2_fdiv(float a, float b)
{
    const float r = a / b;
    return ps2_fall_normal(a, b, r) ? ps2_chop_exact(static_cast<double>(a) / static_cast<double>(b), r)
                                    : ps2_fdiv_slow(a, b);
}

inline float ps2_fsqrt(float a)
{
    const float r = std::sqrt(a);
    return ps2_fall_normal(a, a, r) ? ps2_chop_exact(std::sqrt(static_cast<double>(a)), r) : ps2_fsqrt_slow(a);
}

inline float ps2_frsqrt(float a, float b)
{
    // As on the hardware (and in PCSX2), the root is rounded to a float before the division.
    const float root = std::sqrt(b);
    const float r = a / root;
    if (!ps2_fall_normal(a, b, r))
        return ps2_frsqrt_slow(a, b);
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    const float choppedRoot = ps2_chop_exact(std::sqrt(static_cast<double>(b)), root);
    const float q = a / choppedRoot;
    return ps2_chop_exact(static_cast<double>(a) / static_cast<double>(choppedRoot), q);
}

// CVT.W.S: the EE FPU always rounds toward zero, and |x| >= 2^31 (including exponent 255)
// saturates to 0x7FFFFFFF or 0x80000000 by sign.
inline int32_t ps2_cvt_w_s(float a)
{
    const uint32_t b = std::bit_cast<uint32_t>(a);
    if ((b & PS2_FLOAT_EXP_MASK) <= 0x4E800000u)
        return PS2_CENSUS_I(kCensusCvtWS, a, static_cast<int32_t>(a));
    return PS2_CENSUS_I(kCensusCvtWS, a,
                        (b & PS2_FLOAT_SIGN_MASK) ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max());
}

// CVT.S.W rounds toward zero too (|w| > 2^24 loses low bits); the double value of an int32 is exact.
inline float ps2_cvt_s_w(int32_t w)
{
    const float r = static_cast<float>(w);
    return PS2_CENSUS_FI(kCensusCvtSW, w, w == 0 ? r : ps2_chop_exact(static_cast<double>(w), r));
}

inline float ps2_cvt_s_l(int64_t l)
{
    const float r = static_cast<float>(l);
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    if (l == 0 || std::fabs(r) >= 0x1p63f)
        return r;
    // r is an integer here; compare in int64 to stay exact.
    const int64_t back = static_cast<int64_t>(r);
    return ps2_chop(r, l > 0 ? back > l : back < l);
}

enum class Ps2VecOp
{
    Add,
    Sub,
    Mul
};

// Lane by lane through the scalar helpers; only reached when a result lane has exponent 255.
PS2_FLOAT_NOINLINE inline __m128 ps2_vslow(__m128 a, __m128 b, Ps2VecOp op)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    for (int i = 0; i < 4; ++i)
    {
        switch (op)
        {
        case Ps2VecOp::Add:
            lr[i] = ps2_fadd_slow(la[i], lb[i]);
            break;
        case Ps2VecOp::Sub:
            lr[i] = ps2_fsub_slow(la[i], lb[i]);
            break;
        default:
            lr[i] = ps2_fmul_slow(la[i], lb[i]);
            break;
        }
    }
    return _mm_load_ps(lr);
}

// host is the host result of a op b and r the same rounded toward zero. As in ps2_fnormal, testing host alone
// finds every lane that needs the double path. Results with exponent 0 are flushed to +-0.
#if defined(ZX_FLOAT_CENSUS)
inline __m128 ps2_census_v(Ps2VecOp op, __m128 a, __m128 b, __m128 r)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    _mm_store_ps(lr, r);
    const int kind = op == Ps2VecOp::Add ? kCensusVecAdd : op == Ps2VecOp::Sub ? kCensusVecSub : kCensusVecMul;
    for (int i = 0; i < 4; ++i)
        zx_float_census(kind, std::bit_cast<uint32_t>(la[i]), std::bit_cast<uint32_t>(lb[i]), std::bit_cast<uint32_t>(lr[i]));
    return r;
}
#endif
inline __m128 ps2_vfinish(__m128 a, __m128 b, __m128 r, __m128 host, Ps2VecOp op)
{
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i hexp = _mm_and_si128(_mm_castps_si128(host), expMask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(hexp, expMask)) != 0)
        return PS2_CENSUS_V(op, a, b, ps2_vslow(a, b, op));
    const __m128i rexp = _mm_and_si128(_mm_castps_si128(r), expMask);
    const __m128i flush = _mm_and_si128(_mm_cmpeq_epi32(rexp, _mm_setzero_si128()), _mm_set1_epi32(0x7FFFFFFF));
    return PS2_CENSUS_V(op, a, b, _mm_castsi128_ps(_mm_andnot_si128(flush, _mm_castps_si128(r))));
}

// Round toward zero, lane by lane: TwoSum for a + b (pass -b for a subtraction).
inline __m128 ps2_vchop_sum(__m128 a, __m128 b, __m128 r)
{
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    const __m128 bb = _mm_sub_ps(r, a);
    const __m128 e = _mm_add_ps(_mm_sub_ps(a, _mm_sub_ps(r, bb)), _mm_sub_ps(b, bb));
    const __m128i ei = _mm_castps_si128(e);
    const __m128i ri = _mm_castps_si128(r);
    const __m128i nonzero = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(ei, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()),
                                          _mm_set1_epi32(-1));
    const __m128i otherSign = _mm_srai_epi32(_mm_xor_si128(ei, ri), 31);
    // away lanes are -1: adding them steps the magnitude down by one unit.
    __m128i stepped = _mm_add_epi32(ri, _mm_and_si128(nonzero, otherSign));
    const __m128i zeroExp = _mm_cmpeq_epi32(_mm_and_si128(stepped, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    stepped = _mm_andnot_si128(_mm_and_si128(zeroExp, _mm_set1_epi32(0x7FFFFFFF)), stepped);
    return _mm_castsi128_ps(stepped);
}

inline __m128 ps2_vchop_mul(__m128 a, __m128 b, __m128 r)
{
    if constexpr (!PS2_EE_FLOAT_CHOP)
        return r;
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    _mm_store_ps(lr, r);
    for (int i = 0; i < 4; ++i)
        lr[i] = ps2_chop_exact(static_cast<double>(la[i]) * static_cast<double>(lb[i]), lr[i]);
    return _mm_load_ps(lr);
}

// Lanes with exponent 0 become +-0 (denormals are zero).
inline __m128 ps2_vdaz(__m128 x)
{
    const __m128i xi = _mm_castps_si128(x);
    const __m128i zeroExp = _mm_cmpeq_epi32(_mm_and_si128(xi, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_andnot_si128(_mm_and_si128(zeroExp, _mm_set1_epi32(0x7FFFFFFF)), xi));
}

inline __m128 ps2_vadd(__m128 a, __m128 b)
{
    a = ps2_vdaz(a);
    b = ps2_vdaz(b);
    const __m128 r = _mm_add_ps(a, b);
    return ps2_vfinish(a, b, ps2_vchop_sum(a, b, r), r, Ps2VecOp::Add);
}

inline __m128 ps2_vsub(__m128 a, __m128 b)
{
    a = ps2_vdaz(a);
    b = ps2_vdaz(b);
    const __m128 r = _mm_sub_ps(a, b);
    return ps2_vfinish(a, b, ps2_vchop_sum(a, _mm_xor_ps(b, _mm_set1_ps(-0.0f)), r), r, Ps2VecOp::Sub);
}

inline __m128 ps2_vmul(__m128 a, __m128 b)
{
    a = ps2_vdaz(a);
    b = ps2_vdaz(b);
    const __m128 r = _mm_mul_ps(a, b);
    return ps2_vfinish(a, b, ps2_vchop_mul(a, b, r), r, Ps2VecOp::Mul);
}


// Debug aid, only with -DZX_FPU_TRAP (a separate build): every EE FPU / VU0 macro ADD, SUB, MUL, DIV, SQRT
// and RSQRT result whose exponent is 0xF0 or more (|x| >= 2^113) is reported with the generated function it
// came from (zx_fpu_trap_report in state_probe.cpp, filtered by ZX_FPU_TRAP_FROM / _LIMIT).
#if defined(ZX_FPU_TRAP)
inline float zx_fpu_trap(const char *op, float a, float b, float r)
{
    if (((std::bit_cast<uint32_t>(r) >> 23) & 0xFFu) >= 0xF0u)
        zx_fpu_trap_report(op, std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b), std::bit_cast<uint32_t>(r));
    return r;
}
inline __m128 zx_vtrap(const char *op, __m128 a, __m128 b, __m128 r)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    _mm_store_ps(lr, r);
    for (int i = 0; i < 4; ++i)
        zx_fpu_trap(op, la[i], lb[i], lr[i]);
    return r;
}
#define PS2_FTRAP(op, a, b, r) zx_fpu_trap(op, (float)(a), (float)(b), r)
#define PS2_VTRAP(op, a, b, r) zx_vtrap(op, (__m128)(a), (__m128)(b), r)
#else
#define PS2_FTRAP(op, a, b, r) (r)
#define PS2_VTRAP(op, a, b, r) (r)
#endif

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) PS2_VTRAP("vadd", a, b, ps2_vadd((__m128)(a), (__m128)(b)))
#define PS2_VSUB(a, b) PS2_VTRAP("vsub", a, b, ps2_vsub((__m128)(a), (__m128)(b)))
#define PS2_VMUL(a, b) PS2_VTRAP("vmul", a, b, ps2_vmul((__m128)(a), (__m128)(b)))
// VU0 macro MAX/MINI compare the raw words as sign-magnitude numbers, like VU1 (vu1_upper.cpp) and
// PCSX2's microVU (MIN_MAX_PS without the min/max hack): no clamping or denormal flush, exponent-255 values
// order as numbers, and integer data in a lane passes through unchanged. _mm_max_ps/_mm_min_ps would treat
// exponent 255 as NaN.
inline int32_t ps2_vminmax_lane(int32_t a, int32_t b, bool max)
{
    const bool bothNegative = a < 0 && b < 0;
    return (max != bothNegative) ? (a > b ? a : b) : (a < b ? a : b);
}
inline __m128 ps2_vminmax(__m128 a, __m128 b, bool max)
{
    alignas(16) int32_t la[4], lb[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(la), _mm_castps_si128(a));
    _mm_store_si128(reinterpret_cast<__m128i *>(lb), _mm_castps_si128(b));
    for (int i = 0; i < 4; ++i)
        la[i] = ps2_vminmax_lane(la[i], lb[i], max);
    return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(la)));
}
#define PS2_VMAX(a, b) ps2_vminmax((__m128)(a), (__m128)(b), true)
#define PS2_VMINI(a, b) ps2_vminmax((__m128)(a), (__m128)(b), false)
#define PS2_VDIV(a, b) _mm_div_ps((__m128)(a), (__m128)(b))
#define PS2_VMULQ(a, q) _mm_mul_ps((__m128)(a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#if ZX_LANE_WATCH_BUILD
// The generated code's accesses at addresses it knows are RAM (a global): watched and in the differential too (the
// READ*/WRITE* macros below call Ps2Fast* themselves).
#define FAST_READ8(addr) ([&]() -> uint8_t {               \
    const uint32_t _faddr = (uint32_t)(addr);              \
    ZX_LANE_WATCH(_faddr, 1u, false);                  \
    ZX_LANE_DIFF_LOAD(_faddr, 1u);                     \
    return Ps2FastRead8(rdram, _faddr); }())
#define FAST_READ16(addr) ([&]() -> uint16_t {               \
    const uint32_t _faddr = (uint32_t)(addr);              \
    ZX_LANE_WATCH(_faddr, 2u, false);                  \
    ZX_LANE_DIFF_LOAD(_faddr, 2u);                     \
    return Ps2FastRead16(rdram, _faddr); }())
#define FAST_READ32(addr) ([&]() -> uint32_t {               \
    const uint32_t _faddr = (uint32_t)(addr);              \
    ZX_LANE_WATCH(_faddr, 4u, false);                  \
    ZX_LANE_DIFF_LOAD(_faddr, 4u);                     \
    return Ps2FastRead32(rdram, _faddr); }())
#define FAST_READ64(addr) ([&]() -> uint64_t {               \
    const uint32_t _faddr = (uint32_t)(addr);              \
    ZX_LANE_WATCH(_faddr, 8u, false);                  \
    ZX_LANE_DIFF_LOAD(_faddr, 8u);                     \
    return Ps2FastRead64(rdram, _faddr); }())
#define FAST_READ128(addr) ([&]() -> __m128i {               \
    const uint32_t _faddr = (uint32_t)(addr);              \
    ZX_LANE_WATCH(_faddr, 16u, false);                  \
    ZX_LANE_DIFF_LOAD(_faddr, 16u);                     \
    return Ps2FastRead128(rdram, _faddr); }())
#define FAST_WRITE8(addr, val)                              \
    do                                                     \
    {                                                      \
        const uint32_t _faddr = (uint32_t)(addr);          \
        ZX_LANE_WATCH(_faddr, 1u, true);               \
        Ps2FastWrite8(rdram, _faddr, (uint8_t)(val));                 \
        ZX_LANE_DIFF_STORED(_faddr, 1u);               \
    } while (0)
#define FAST_WRITE16(addr, val)                              \
    do                                                     \
    {                                                      \
        const uint32_t _faddr = (uint32_t)(addr);          \
        ZX_LANE_WATCH(_faddr, 2u, true);               \
        Ps2FastWrite16(rdram, _faddr, (uint16_t)(val));                 \
        ZX_LANE_DIFF_STORED(_faddr, 2u);               \
    } while (0)
#define FAST_WRITE32(addr, val)                              \
    do                                                     \
    {                                                      \
        const uint32_t _faddr = (uint32_t)(addr);          \
        ZX_LANE_WATCH(_faddr, 4u, true);               \
        Ps2FastWrite32(rdram, _faddr, (uint32_t)(val));                 \
        ZX_LANE_DIFF_STORED(_faddr, 4u);               \
    } while (0)
#define FAST_WRITE64(addr, val)                              \
    do                                                     \
    {                                                      \
        const uint32_t _faddr = (uint32_t)(addr);          \
        ZX_LANE_WATCH(_faddr, 8u, true);               \
        Ps2FastWrite64(rdram, _faddr, (uint64_t)(val));                 \
        ZX_LANE_DIFF_STORED(_faddr, 8u);               \
    } while (0)
#define FAST_WRITE128(addr, val)                              \
    do                                                     \
    {                                                      \
        const uint32_t _faddr = (uint32_t)(addr);          \
        ZX_LANE_WATCH(_faddr, 16u, true);               \
        Ps2FastWrite128(rdram, _faddr, (val));                 \
        ZX_LANE_DIFF_STORED(_faddr, 16u);               \
    } while (0)
#else
#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))
#endif

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    ZX_LANE_WATCH(_addr, 1u, false);                     \
    ZX_LANE_DIFF_LOAD(_addr, 1u);                       \
    return PS2Machine::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : Ps2FastRead8(rdram, _addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    ZX_LANE_WATCH(_addr, 2u, false);                     \
    ZX_LANE_DIFF_LOAD(_addr, 2u);                       \
    return PS2Machine::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : Ps2FastRead16(rdram, _addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    ZX_LANE_WATCH(_addr, 4u, false);                     \
    ZX_LANE_DIFF_LOAD(_addr, 4u);                       \
    return PS2Machine::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : Ps2FastRead32(rdram, _addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    ZX_LANE_WATCH(_addr, 8u, false);                     \
    ZX_LANE_DIFF_LOAD(_addr, 8u);                       \
    return PS2Machine::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : Ps2FastRead64(rdram, _addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    ZX_LANE_WATCH(_addr, 16u, false);                    \
    ZX_LANE_DIFF_LOAD(_addr, 16u);                       \
    return PS2Machine::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : Ps2FastRead128(rdram, _addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        ZX_LANE_WATCH(_addr, 1u, true);                                     \
        if (PS2Machine::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            Ps2FastWrite8(rdram, _addr, (uint8_t)(val));                                            \
        }                                                                            \
        ZX_LANE_DIFF_STORED(_addr, 1u);                                       \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        ZX_LANE_WATCH(_addr, 2u, true);                                       \
        if (PS2Machine::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            Ps2FastWrite16(rdram, _addr, (uint16_t)(val));                                             \
        }                                                                              \
        ZX_LANE_DIFF_STORED(_addr, 2u);                                       \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        ZX_LANE_WATCH(_addr, 4u, true);                                       \
        if (PS2Machine::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            Ps2FastWrite32(rdram, _addr, (uint32_t)(val));                                             \
        }                                                                              \
        ZX_LANE_DIFF_STORED(_addr, 4u);                                       \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        ZX_LANE_WATCH(_addr, 8u, true);                                       \
        if (PS2Machine::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            Ps2FastWrite64(rdram, _addr, (uint64_t)(val));                                             \
        }                                                                              \
        ZX_LANE_DIFF_STORED(_addr, 8u);                                       \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        ZX_LANE_WATCH(_addr, 16u, true);                                    \
        __m128i _value = (val);                                                      \
        if (PS2Machine::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            Ps2FastWrite128(rdram, _addr, _value);                                         \
        }                                                                            \
        ZX_LANE_DIFF_STORED(_addr, 16u);                                      \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Add with Signed Saturation Word (PADDSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b)
{

    __m128i sum = _mm_add_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, sum),
                                     _mm_xor_si128(b, sum));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, sum));
    #endif
}
#define PS2_PADDSW(a, b) ps2_paddsw((__m128i)(a), (__m128i)(b))

// Packed Subtract with Signed Saturation Word (PSUBSW)
inline __m128i ps2_psubsw(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, b),
                                     _mm_xor_si128(a, diff));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, diff));
    #endif

}
#define PS2_PSUBSW(a, b) ps2_psubsw((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
// PABSW / PABSH saturate: the most negative lane (0x80000000, 0x8000) gives
// 0x7FFFFFFF / 0x7FFF on the R5900 (PCSX2's _PABSW / _PABSH), where the
// host's abs leaves it negative.
#define PS2_PABSW(a) _mm_min_epu32(_mm_abs_epi32((__m128i)(a)), _mm_set1_epi32(0x7FFFFFFF))
#define PS2_PABSH(a) _mm_min_epu16(_mm_abs_epi16((__m128i)(a)), _mm_set1_epi16(0x7FFF))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
#define PS2_PSLLVW(a, b) _mm_custom_sllv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRLVW(a, b) _mm_custom_srlv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRAVW(a, b) _mm_custom_srav_epi32((__m128i)(a), (__m128i)(b))

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// FPU (COP1) operations
#define FPU_SET_ACC(ctx, res) (ctx->f_acc = res)
#define FPU_ADD_S(a, b) PS2_FTRAP("add", a, b, ps2_fadd((float)(a), (float)(b)))
#define FPU_SUB_S(a, b) PS2_FTRAP("sub", a, b, ps2_fsub((float)(a), (float)(b)))
#define FPU_MUL_S(a, b) PS2_FTRAP("mul", a, b, ps2_fmul((float)(a), (float)(b)))
#define FPU_DIV_S(a, b) PS2_FTRAP("div", a, b, ps2_fdiv_nearest((float)(a), (float)(b)))
#define FPU_RSQRT_S(a, b) PS2_FTRAP("rsqrt", a, b, ps2_frsqrt((float)(a), (float)(b)))
#define FPU_SQRT_S(a) PS2_FTRAP("sqrt", a, a, ps2_fsqrt_nearest((float)(a)))
#define FPU_ABS_S(a) fabsf((float)(a))
// EE FPU MAX.S/MIN.S order the operands as PS2 numbers (exponent 255 is a number, exponent 0 is zero); on a
// tie the result is ft, as with the MAXSS/MINSS PCSX2 uses. std::max/std::min would see exponent 255 as NaN.
inline float ps2_fmax(float fs, float ft)
{
    return ps2_to_double(fs) > ps2_to_double(ft) ? fs : ft;
}
inline float ps2_fmin(float fs, float ft)
{
    return ps2_to_double(fs) < ps2_to_double(ft) ? fs : ft;
}
#define FPU_MAX_S(a, b) ps2_fmax((float)(a), (float)(b))
#define FPU_MIN_S(a, b) ps2_fmin((float)(a), (float)(b))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ps2_cvt_s_w((int32_t)(a))
#define FPU_CVT_S_L(a) ps2_cvt_s_l((int64_t)(a))
#define FPU_CVT_W_S(a) ps2_cvt_w_s((float)(a))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_EQ_S(a, b) (ps2_to_double((float)(a)) == ps2_to_double((float)(b)))
#define FPU_C_UEQ_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
// The R5900 encodes C.LT.S as funct 0x34 and C.LE.S as 0x36, the standard MIPS C.OLT/C.OLE slots (its
// FPU only has C.F, C.EQ, C.LT and C.LE). Code generated before the translator named them that way still
// calls FPU_C_OLT_S/FPU_C_OLE_S, so these compare like FPU_C_LT_S/FPU_C_LE_S: a host compare would see an
// exponent-255 value such as the x/0 result 0x7FFFFFFF as NaN, and 1 < +max would be false.
#define FPU_C_OLT_S(a, b) FPU_C_LT_S(a, b)
#define FPU_C_ULT_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLE_S(a, b) FPU_C_LE_S(a, b)
#define FPU_C_ULE_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SEQ_S(a, b) ((float)(a) == (float)(b))
#define FPU_C_NGL_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LT_S(a, b) (ps2_to_double((float)(a)) < ps2_to_double((float)(b)))
#define FPU_C_NGE_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LE_S(a, b) (ps2_to_double((float)(a)) <= ps2_to_double((float)(b)))
#define FPU_C_NGT_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(0, 3, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)


#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                               \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(uint32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#if defined(ZX_GAME_MODULE)
} // inline namespace game_module
#endif

#endif // PS2_EE_INSTRUCTIONS_H
