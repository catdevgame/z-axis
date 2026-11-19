#pragma once

// VU0 macro-mode arithmetic (COP2: VADD, VMUL, VSQRT, VDIV ...) for
// readable code that replaces the game's hand-written VU0 routines.
//
// VU0 rounds towards zero like the EE FPU, but its adder has no R5900
// operand alignment (ps2_fpu_align_addends), and VDIV / VSQRT round
// towards zero too, where the FPU's DIV.S / SQRT.S round to nearest. So
// a VU0 sum or quotient can differ from the ee::Float one in the last
// bit: readable code uses these where the original used VU0.
//
// One lane at a time, through the same helpers the recompiled code uses
// (ps2_vadd etc. in ps2_runtime_macros.h). Only the values are modelled:
// the VU0 registers (VF, Q) and the MAC / status flags are not written, so
// a readable replacement leaves them as they were. That is fine where the
// next VU0 user loads its own operands (the game's C code cannot read them;
// its VU0 routines start with LQC2 / QMTC2).
//
// A function whose callers read what it leaves in VU0 (a lane of a
// register it wrote, Q) says what it leaves with ee::vu0::Leave and is
// registered with READABLE_FUNCTION_VU0: ps2xFnCheck then compares its VU0
// registers, Q and the MAC and status flags with the recorded ones.
//
// MicroLane does the same for a VU0 microprogram's arithmetic (a VCALLMS),
// whose values and flags the VU interpreter works out its own way.

#include "abi.h"
#include "ee.h"

#include <cmath>
#include <limits>

namespace ee::vu0
{
    namespace detail
    {
        inline Float lane0(__m128 v) { return Float(_mm_cvtss_f32(v)); }
        inline __m128 splat(Float a) { return _mm_set1_ps(a.raw()); }
    }

    // VADD / VSUB / VMUL on one lane.
    inline Float add(Float a, Float b) { return detail::lane0(ps2_vadd(detail::splat(a), detail::splat(b))); }
    inline Float sub(Float a, Float b) { return detail::lane0(ps2_vsub(detail::splat(a), detail::splat(b))); }
    inline Float mul(Float a, Float b) { return detail::lane0(ps2_vmul(detail::splat(a), detail::splat(b))); }
    // VSQRT Q = sqrt(|a|), VDIV Q = a / b (towards zero; a zero divisor
    // gives +-max).
    inline Float sqrt(Float a) { return Float(ps2_fsqrt(a.raw())); }
    inline Float div(Float a, Float b) { return Float(ps2_fdiv(a.raw(), b.raw())); }

    // One lane of a VU0 microprogram's FMAC arithmetic (VADD, VSUB, VMUL and the multiply-adds a program started
    // with VCALLMS runs), as the runtime's VU interpreter does it, with the flags its instructions set in that lane.
    // In micro mode VU0 reads an operand of exponent 0 as 0 and one of exponent 255 as the largest float; a result
    // past the largest float is the largest, one under the smallest normal (or 0) a 0 of its sign, and the rest is
    // VU0's own rounding (add, sub, mul). The flags come from each result's exact value: zero, sign, underflow and
    // overflow; a multiply-add's product adds its own to the sticky ones. last() is this lane's part of the MAC
    // flags after its last instruction (Z 1, S 2, U 4, O 8), sticky() everything its instructions set.
    class MicroLane
    {
    public:
        Float mul(Float a, Float b)
        {
            a = operand(a);
            b = operand(b);
            return result(exact(a) * exact(b), vu0::mul(a, b));
        }
        Float add(Float a, Float b)
        {
            a = operand(a);
            b = operand(b);
            return result(exact(a) + exact(b), vu0::add(a, b));
        }
        Float sub(Float a, Float b)
        {
            a = operand(a);
            b = operand(b);
            return result(exact(a) - exact(b), vu0::sub(a, b));
        }
        // VMADD and VMSUB: sum + a b, sum - a b (sum in ACC).
        Float multiplyAdd(Float sum, Float a, Float b)
        {
            sum = operand(sum);
            a = operand(a);
            b = operand(b);
            const double product = exact(a) * exact(b);
            m_sticky |= flagsOf(product);
            return result(exact(sum) + product, vu0::add(sum, vu0::mul(a, b)));
        }
        Float multiplySubtract(Float sum, Float a, Float b)
        {
            sum = operand(sum);
            a = operand(a);
            b = operand(b);
            const double product = exact(a) * exact(b);
            m_sticky |= flagsOf(product);
            return result(exact(sum) - product, vu0::sub(sum, vu0::mul(a, b)));
        }

        uint32_t last() const { return m_last; }
        uint32_t sticky() const { return m_sticky; }

    private:
        static constexpr uint32_t kZero = 1u, kSign = 2u, kUnderflow = 4u, kOverflow = 8u;
        static constexpr uint32_t kLargest = 0x7F7FFFFFu;

        static Float operand(Float v)
        {
            const uint32_t b = v.bits();
            const uint32_t exponent = (b >> 23) & 0xFFu;
            if (exponent == 0u)
                return Float::fromBits(b & 0x80000000u);
            if (exponent == 0xFFu)
                return Float::fromBits((b & 0x80000000u) | kLargest);
            return v;
        }
        static double exact(Float v) { return static_cast<double>(v.raw()); }
        static uint32_t flagsOf(double r)
        {
            uint32_t flags = std::signbit(r) ? kSign : 0u;
            const double magnitude = std::fabs(r);
            if (magnitude == 0.0)
                flags |= kZero;
            else if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
                flags |= kOverflow;
            else if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
                flags |= kUnderflow | kZero;
            return flags;
        }
        Float result(double exactResult, Float rounded)
        {
            m_last = flagsOf(exactResult);
            m_sticky |= m_last;
            const uint32_t sign = std::signbit(exactResult) ? 0x80000000u : 0u;
            if (m_last & kOverflow)
                return Float::fromBits(sign | kLargest);
            if (m_last & kZero)
                return Float::fromBits(sign);
            return rounded;
        }

        uint32_t m_last = 0;
        uint32_t m_sticky = 0;
    };

    // Scaffolding (vu0-argument-upper-half): docs/SCAFFOLDING.md says why and what removes it.
    // QMTC2 moves all 128 bits of an integer register into a VU0 register. A function that hands VU0 its first
    // argument that way (BMX XXX's angleSin and angleCos) gives it the upper half of the argument's register too,
    // whatever the last 128-bit move through it left there, as its caller left it: the VU0 lanes the argument does
    // not set.
    inline uint64_t firstArgumentUpperHalf() { return callerRegisterHigh(reg::a0); }

    // One VU0 macro instruction of a draft (the drafter's fold_vu_statement):
    // the recompiled statement as a call, a whole register's arithmetic
    // through VU0's rounding, the result's `dest` lanes kept, the MAC and
    // status flags set.
    enum Dest : uint32_t // an instruction's destination lanes (x = 8)
    {
        kW = 1u, kZ, kZw, kY, kYw, kYz, kYzw, kX, kXw, kXz, kXzw, kXy, kXyw, kXyz, kXyzw,
    };
    inline __m128 add(__m128 a, __m128 b) { return PS2_VADD(a, b); }
    inline __m128 sub(__m128 a, __m128 b) { return PS2_VSUB(a, b); }
    inline __m128 mul(__m128 a, __m128 b) { return PS2_VMUL(a, b); }
    // VMULx and the like: one lane in all four.
    template <int Lane>
    inline __m128 lane(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(Lane, Lane, Lane, Lane)); }
    inline __m128 keptLanes(__m128 into, __m128 result, uint32_t dest)
    {
        const __m128i mask = _mm_setr_epi32((dest & 8u) ? -1 : 0, (dest & 4u) ? -1 : 0, (dest & 2u) ? -1 : 0, (dest & 1u) ? -1 : 0);
        return _mm_blendv_ps(into, result, _mm_castsi128_ps(mask));
    }
    inline void setVf(R5900Context *ctx, int reg, uint32_t dest, __m128 result)
    {
        ctx->vu0_vf[reg] = keptLanes(ctx->vu0_vf[reg], result, dest);
        ps2_vu0_fmac_flags(ctx, result, dest);
    }
    inline void setAcc(R5900Context *ctx, uint32_t dest, __m128 result)
    {
        ctx->vu0_acc = keptLanes(ctx->vu0_acc, result, dest);
        ps2_vu0_fmac_flags(ctx, result, dest);
    }

    // A VU0 register's four lanes.
    struct Lanes
    {
        Float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    };

    // The game's inline VU0 sequences (the engine's vector macros; always
    // the same registers), done on the running context's VU0 registers: each
    // step writes the lanes its instruction writes and sets the MAC and
    // status flags as the generated code does (ps2_vu0_fmac_flags), so what
    // the function leaves in VU0 (registers, ACC, Q, flags) is the
    // original's. Values go through VU0's own rounding (ps2_vadd and the
    // rest). For readable code whose original inlines them and whose callers
    // (or later code) read a lane they leave. Outside a readable call (no
    // context) they work on a thread's scratch registers (made once a
    // thread, so a Machine costs nothing to make in the hot routines; it
    // used to zero a context of its own, 1.6 KB, at every construction).
    class Machine
    {
    public:
        Machine() : m_ctx(t_ee.frame && t_ee.frame->ctx ? t_ee.frame->ctx : scratch()) {}
        Machine(const Machine &) = delete;
        Machine &operator=(const Machine &) = delete;

        __m128 vf(int reg) const { return m_ctx->vu0_vf[reg]; }
        Float q() const { return Float(m_ctx->vu0_q); }

        // LQC2 / SQC2.
        void load(int reg, const void *quadword) { m_ctx->vu0_vf[reg] = _mm_loadu_ps(static_cast<const float *>(quadword)); }
        void store(int reg, void *quadword) const { _mm_storeu_ps(static_cast<float *>(quadword), m_ctx->vu0_vf[reg]); }

        // LQC2 vf7 row 3, vf8 v, vf4 .. vf6 rows 0 .. 2; VMULAw.xyz ACC
        // vf7 vf0w (the translation, by vf0's w: 1); VMADDAx.xyz,
        // VMADDAy.xyz ACC; VMADDz.xyz vf9; SQC2 vf9 to `out`: v (xyz) by the
        // matrix, w is vf9's.
        void transform(const void *matrix, const void *v, void *out)
        {
            const uint8_t *m = static_cast<const uint8_t *>(matrix);
            load(7, m + 0x30);
            load(8, v);
            load(4, m + 0x00);
            load(5, m + 0x10);
            load(6, m + 0x20);
            __m128 &acc = m_ctx->vu0_acc;
            acc = fmac(ps2_vmul(vf(7), lane<3>(vf(0))), kXyz, acc);
            acc = fmac(ps2_vadd(acc, ps2_vmul(vf(4), lane<0>(vf(8)))), kXyz, acc);
            acc = fmac(ps2_vadd(acc, ps2_vmul(vf(5), lane<1>(vf(8)))), kXyz, acc);
            set(9, ps2_vadd(acc, ps2_vmul(vf(6), lane<2>(vf(8)))), kXyz);
            store(9, out);
        }

        // LQC2 vf4 a, vf5 b; VMUL.xyz vf5; VADDy.x, VADDz.x vf5: a.b (xyz),
        // handed out as QMFC2 + MTC1 do (vf5's x).
        Float dot(const void *a, const void *b)
        {
            load(4, a);
            load(5, b);
            set(5, ps2_vmul(vf(4), vf(5)), kXyz);
            set(5, ps2_vadd(vf(5), lane<1>(vf(5))), kX);
            set(5, ps2_vadd(vf(5), lane<2>(vf(5))), kX);
            return Float(_mm_cvtss_f32(vf(5)));
        }

        // LQC2 vf4 v; its length as dot (vf5), VSQRT Q, VADDq.x vf5 vf0 Q,
        // VDIV Q vf0w vf5x, VSUB.xyzw vf6 vf0 vf0, VMULq.xyz vf6 vf4 Q; SQC2
        // vf6 to `out` (w 0): v made unit length. Returns the length (vf5's
        // x, the QMFC2 the sequence makes).
        Float normalize(const void *v, void *out)
        {
            load(4, v);
            set(5, ps2_vmul(vf(4), vf(4)), kXyz);
            set(5, ps2_vadd(vf(5), lane<1>(vf(5))), kX);
            set(5, ps2_vadd(vf(5), lane<2>(vf(5))), kX);
            m_ctx->vu0_q = ps2_fsqrt(_mm_cvtss_f32(vf(5)));
            set(5, ps2_vadd(vf(0), _mm_set1_ps(m_ctx->vu0_q)), kX);
            m_ctx->vu0_q = ps2_fdiv(_mm_cvtss_f32(lane<3>(vf(0))), _mm_cvtss_f32(vf(5)));
            set(6, ps2_vsub(vf(0), vf(0)), kXyzw);
            const Float length(_mm_cvtss_f32(vf(5)));
            set(6, ps2_vmul(vf(4), _mm_set1_ps(m_ctx->vu0_q)), kXyz);
            store(6, out);
            return length;
        }

        // normalize on all four lanes (VMUL.xyzw; VADDw.x too; VMULq.xyzw): a
        // quaternion made unit length, in vf6 as well as in `out`.
        Float normalize4(const void *v, void *out)
        {
            load(4, v);
            set(5, ps2_vmul(vf(4), vf(4)), kXyzw);
            set(5, ps2_vadd(vf(5), lane<1>(vf(5))), kX);
            set(5, ps2_vadd(vf(5), lane<2>(vf(5))), kX);
            set(5, ps2_vadd(vf(5), lane<3>(vf(5))), kX);
            m_ctx->vu0_q = ps2_fsqrt(_mm_cvtss_f32(vf(5)));
            set(5, ps2_vadd(vf(0), _mm_set1_ps(m_ctx->vu0_q)), kX);
            m_ctx->vu0_q = ps2_fdiv(_mm_cvtss_f32(lane<3>(vf(0))), _mm_cvtss_f32(vf(5)));
            set(6, ps2_vsub(vf(0), vf(0)), kXyzw);
            const Float length(_mm_cvtss_f32(vf(5)));
            set(6, ps2_vmul(vf(4), _mm_set1_ps(m_ctx->vu0_q)), kXyzw);
            store(6, out);
            return length;
        }

        // LQC2 vf8 v, vf4 .. vf6 rows 0 .. 2; VMULAx.xyz ACC, VMADDAy.xyz
        // ACC, VMADDz.xyz vf9; SQC2 vf9 to `out`: v (xyz) by the matrix's
        // rotation (no translation), w is vf9's.
        void rotate(const void *matrix, const void *v, void *out)
        {
            const uint8_t *m = static_cast<const uint8_t *>(matrix);
            load(8, v);
            load(4, m + 0x00);
            load(5, m + 0x10);
            load(6, m + 0x20);
            __m128 &acc = m_ctx->vu0_acc;
            acc = fmac(ps2_vmul(vf(4), lane<0>(vf(8))), kXyz, acc);
            acc = fmac(ps2_vadd(acc, ps2_vmul(vf(5), lane<1>(vf(8)))), kXyz, acc);
            set(9, ps2_vadd(acc, ps2_vmul(vf(6), lane<2>(vf(8)))), kXyz);
            store(9, out);
        }

        // LQC2 vf4 a, vf5 b; VSUB.xyzw vf6; SQC2 vf6 to `out`: a - b.
        void difference(const void *a, const void *b, void *out)
        {
            load(4, a);
            load(5, b);
            set(6, ps2_vsub(vf(4), vf(5)), kXyzw);
            store(6, out);
        }

        // LQC2 vf4 a, vf5 b; VADD.xyzw vf6; SQC2 vf6 to `out`: a + b.
        void sum(const void *a, const void *b, void *out)
        {
            load(4, a);
            load(5, b);
            set(6, ps2_vadd(vf(4), vf(5)), kXyzw);
            store(6, out);
        }

        // LQC2 vf4 v; MFC1 + QMTC2 vf5 (x = s, y its sign, zw `t0High`: the
        // upper half of the EE's t0, which QMTC2 moves too); VMULx.xyz vf6
        // vf4 vf5x; SQC2 vf6 to `out`: v (xyz) times s, w is vf6's.
        void scale(const void *v, Float s, void *out, uint64_t t0High = 0)
        {
            load(4, v);
            const uint64_t low = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s.bits())));
            m_ctx->vu0_vf[5] = _mm_castsi128_ps(_mm_set_epi64x(static_cast<int64_t>(t0High), static_cast<int64_t>(low)));
            set(6, ps2_vmul(vf(4), lane<0>(vf(5))), kXyz);
            store(6, out);
        }

        // LQC2 vf4 v; VABS.xyzw vf6 vf4 (no flags): |v|, lane by lane, as the
        // SQC2 that follows hands it out.
        Lanes absolute(const void *v)
        {
            load(4, v);
            const __m128 result = _mm_and_ps(vf(4), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
            m_ctx->vu0_vf[6] = result;
            alignas(16) float lanes[4];
            _mm_store_ps(lanes, result);
            return {Float(lanes[0]), Float(lanes[1]), Float(lanes[2]), Float(lanes[3])};
        }

        // The same with the SQC2: |v| to `out`.
        void absolute(const void *v, void *out)
        {
            load(4, v);
            m_ctx->vu0_vf[6] = _mm_and_ps(vf(4), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
            store(6, out);
        }

        // LQC2 vf4 a, vf5 b; VOPMULA.xyz ACC vf4 vf5; VOPMSUB.xyz vf6 vf5
        // vf4; VSUB.w vf6 vf6 vf6; SQC2 vf6 to `out`: a x b (w 0).
        void cross(const void *a, const void *b, void *out)
        {
            load(4, a);
            load(5, b);
            __m128 &acc = m_ctx->vu0_acc;
            acc = fmac(ps2_vmul(yzx(vf(4)), zxy(vf(5))), kXyz, acc);
            set(6, ps2_vsub(acc, ps2_vmul(yzx(vf(5)), zxy(vf(4)))), kXyz);
            set(6, ps2_vsub(vf(6), vf(6)), kW);
            store(6, out);
        }

        // LQC2 vf4 v; VMUL.xyz vf5 vf4 vf4; VADDy.x, VADDz.x vf5: v.v (xyz),
        // as QMFC2 + MTC1 hand it out (vf5's x).
        Float squaredLength(const void *v)
        {
            load(4, v);
            set(5, ps2_vmul(vf(4), vf(4)), kXyz);
            set(5, ps2_vadd(vf(5), lane<1>(vf(5))), kX);
            set(5, ps2_vadd(vf(5), lane<2>(vf(5))), kX);
            return Float(_mm_cvtss_f32(vf(5)));
        }

        // A register's upper half (QMFC2 into t0 moves it to t0's upper 64 bits).
        uint64_t high(int reg) const
        {
            return static_cast<uint64_t>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(_mm_castps_si128(vf(reg)), _mm_castps_si128(vf(reg)))));
        }

        // The sequences on EE addresses (LQC2 / SQC2 ignore the low four bits).
        static void *at(uint32_t address) { return ee::at<uint8_t>(address & ~15u); }
        void transform(uint32_t matrix, uint32_t v, uint32_t out) { transform(at(matrix), at(v), at(out)); }
        void rotate(uint32_t matrix, uint32_t v, uint32_t out) { rotate(at(matrix), at(v), at(out)); }
        Float dot(uint32_t a, uint32_t b) { return dot(at(a), at(b)); }
        Float normalize(uint32_t v, uint32_t out) { return normalize(at(v), at(out)); }
        void difference(uint32_t a, uint32_t b, uint32_t out) { difference(at(a), at(b), at(out)); }
        void sum(uint32_t a, uint32_t b, uint32_t out) { sum(at(a), at(b), at(out)); }
        void scale(uint32_t v, Float s, uint32_t out, uint64_t t0High = 0) { scale(at(v), s, at(out), t0High); }
        void cross(uint32_t a, uint32_t b, uint32_t out) { cross(at(a), at(b), at(out)); }
        Float squaredLength(uint32_t v) { return squaredLength(at(v)); }

    private:
        enum : uint32_t // an instruction's destination lanes (x = 8)
        {
            kX = 0x8,
            kW = 0x1,
            kXyz = 0xE,
            kXyzw = 0xF,
        };
        template <int Lane>
        static __m128 lane(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(Lane, Lane, Lane, Lane)); }
        // VOPMULA / VOPMSUB's operand orders (w kept).
        static __m128 yzx(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); }
        static __m128 zxy(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); }

        // An FMAC result: its flags, and the `dest` lanes of `into` replaced.
        __m128 fmac(__m128 result, uint32_t dest, __m128 into)
        {
            ps2_vu0_fmac_flags(m_ctx, result, dest);
            const __m128i mask = _mm_setr_epi32((dest & 8u) ? -1 : 0, (dest & 4u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                                (dest & 1u) ? -1 : 0);
            return _mm_blendv_ps(into, result, _mm_castsi128_ps(mask));
        }
        void set(int reg, __m128 result, uint32_t dest) { m_ctx->vu0_vf[reg] = fmac(result, dest, m_ctx->vu0_vf[reg]); }

        static R5900Context *scratch()
        {
            static thread_local R5900Context context = []
            {
                R5900Context c{};
                c.vu0_vf[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
                return c;
            }();
            return &context;
        }

        R5900Context *m_ctx;
    };

    // For readable code that works a macro sequence's result out by value
    // (VADD, VSUB, VMULx, the cross product, on vf4 and vf5 into vf6; a
    // transform into vf9) rather than on a Machine: the whole result, stale w
    // included, as the sequence's SQC2 stored it, left in the register. The
    // next xyz-only sequence keeps that w and its SQC2 stores it, in another
    // function too (abdiff1). Flags and vf4, vf5 are not modelled.
    template <typename V>
    inline const V &leftInVf(int reg, const V &result)
    {
        static_assert(sizeof(V) == 16);
        if (t_ee.frame && t_ee.frame->ctx)
            t_ee.frame->ctx->vu0_vf[reg] = _mm_loadu_ps(reinterpret_cast<const float *>(&result));
        return result;
    }
    template <typename V>
    inline V leftInVf6(const V &result) { return leftInVf(6, result); }
    template <typename V>
    inline V leftInVf9(const V &result) { return leftInVf(9, result); }

    // What a readable function leaves in VU0, written into the running
    // context (the game's, or ps2xFnCheck's replay): registers, Q, and the
    // flags each of the original's FMAC instructions sets, in its order
    // (fmac: the result before the dest mask, dest as the xyzw field, x = 8;
    // ps2_vu0_fmac_flags). Outside a readable call (no context) nothing.
    class Leave
    {
    public:
        Leave() : m_ctx(t_ee.frame ? t_ee.frame->ctx : nullptr) {}

        Lanes vf(int reg) const
        {
            Lanes l;
            if (m_ctx)
            {
                alignas(16) float lanes[4];
                _mm_store_ps(lanes, m_ctx->vu0_vf[reg]);
                l = {Float(lanes[0]), Float(lanes[1]), Float(lanes[2]), Float(lanes[3])};
            }
            return l;
        }
        void vf(int reg, const Lanes &l)
        {
            if (m_ctx)
                m_ctx->vu0_vf[reg] = _mm_setr_ps(l.x.raw(), l.y.raw(), l.z.raw(), l.w.raw());
        }
        void q(Float value)
        {
            if (m_ctx)
                m_ctx->vu0_q = value.raw();
        }
        void fmac(const Lanes &result, uint32_t dest)
        {
            if (m_ctx)
                ps2_vu0_fmac_flags(m_ctx, _mm_setr_ps(result.x.raw(), result.y.raw(), result.z.raw(), result.w.raw()),
                                   dest);
        }
        void acc(const Lanes &l)
        {
            if (m_ctx)
                m_ctx->vu0_acc = _mm_setr_ps(l.x.raw(), l.y.raw(), l.z.raw(), l.w.raw());
        }
        // A microprogram's flags, from its lanes x .. w (MicroLane): the MAC flags its last instruction set, the
        // status's Z S U O from them, and its sticky bits joined by everything the lanes set (D and I kept).
        void microFlags(const MicroLane (&lanes)[4])
        {
            if (!m_ctx)
                return;
            uint32_t mac = 0u, current = 0u, sticky = 0u;
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                const uint32_t bit = 8u >> lane;
                const uint32_t last = lanes[lane].last();
                for (uint32_t flag = 0; flag < 4u; ++flag) // Z, S, U, O: a nibble each
                    if (last & (1u << flag))
                        mac |= bit << (4u * flag);
                current |= last;
                sticky |= lanes[lane].sticky();
            }
            m_ctx->vu0_mac_flags = mac;
            m_ctx->vu0_status = static_cast<uint16_t>((m_ctx->vu0_status & 0xFF0u) | current | ((current | sticky) << 6));
        }

    private:
        R5900Context *m_ctx;
    };
}

