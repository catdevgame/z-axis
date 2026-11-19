#pragma once

// VU0 macro-mode instructions one at a time, on the running context's VU0
// registers, for the engine's hand-written VU0 routines whose callers read
// what they leave (READABLE_FUNCTION_VU0) or that are written in their
// original's register order: the multiply-accumulate chains of a matrix
// product (VMULA / VMADDA / VMADD by one lane of a register), VMINI / VMAX.
// Each FMAC writes the lanes of its destination field and sets the MAC and
// status flags as the generated code does; VMINI and VMAX set none.
// ee::vu0::Machine (ee/vu0.h) has the engine's fixed inline sequences;
// this is the general form those are made of (dot and transformPoint are
// Machine's dot and transform). Outside a readable call (no context) it
// works on a thread's scratch registers, as Machine does.

#include "ee/abi.h"
#include "ee/vu0.h"

namespace zge::vu0
{
    // An instruction's destination field (x = 8, as the encoding has it).
    enum Dest : uint32_t
    {
        kX = 0x8,
        kXy = 0xC,
        kXyz = 0xE,
        kXyzw = 0xF,
    };

    // A broadcast lane (the bc field).
    enum Lane : int
    {
        kLaneX = 0,
        kLaneY = 1,
        kLaneZ = 2,
        kLaneW = 3,
    };

    class Registers
    {
    public:
        Registers() : m_ctx(ee::t_ee.frame && ee::t_ee.frame->ctx ? ee::t_ee.frame->ctx : scratch()) {}
        Registers(const Registers &) = delete;
        Registers &operator=(const Registers &) = delete;

        // LQC2 / SQC2 on an EE address (the low four bits ignored).
        void load(int reg, uint32_t address) { m_ctx->vu0_vf[reg] = _mm_loadu_ps(ee::at<float>(address & ~15u)); }
        void store(int reg, uint32_t address) const { _mm_storeu_ps(ee::at<float>(address & ~15u), m_ctx->vu0_vf[reg]); }
        // The same on a vector or matrix row held by reference.
        void load(int reg, const void *quadword) { m_ctx->vu0_vf[reg] = _mm_loadu_ps(static_cast<const float *>(quadword)); }
        void store(int reg, void *quadword) const { _mm_storeu_ps(static_cast<float *>(quadword), m_ctx->vu0_vf[reg]); }

        // VMULAbc.dest ACC, vf[a], vf[b]bc
        void mulAcc(uint32_t dest, int a, int b, Lane bc)
        {
            m_ctx->vu0_acc = fmac(ps2_vmul(vf(a), lane(vf(b), bc)), dest, m_ctx->vu0_acc);
        }
        // VMADDAbc.dest ACC, vf[a], vf[b]bc
        void maddAcc(uint32_t dest, int a, int b, Lane bc)
        {
            m_ctx->vu0_acc = fmac(ps2_vadd(m_ctx->vu0_acc, ps2_vmul(vf(a), lane(vf(b), bc))), dest, m_ctx->vu0_acc);
        }
        // VMSUBAbc.dest ACC, vf[a], vf[b]bc
        void msubAcc(uint32_t dest, int a, int b, Lane bc)
        {
            m_ctx->vu0_acc = fmac(ps2_vsub(m_ctx->vu0_acc, ps2_vmul(vf(a), lane(vf(b), bc))), dest, m_ctx->vu0_acc);
        }
        // VSUBA.dest ACC, vf[a], vf[b]
        void subAcc(uint32_t dest, int a, int b) { m_ctx->vu0_acc = fmac(ps2_vsub(vf(a), vf(b)), dest, m_ctx->vu0_acc); }
        // The MAC flags the last FMAC left, as a destination field (x = 8):
        // the lanes it made negative, and those it made zero.
        uint32_t negativeLanes() const { return (m_ctx->vu0_mac_flags >> 4) & 0xFu; }
        uint32_t zeroLanes() const { return m_ctx->vu0_mac_flags & 0xFu; }
        // VMAXbc.dest vf[d], vf[a], vf[b]bc (no flags).
        void maximumBc(uint32_t dest, int d, int a, int b, Lane bc)
        {
            const __m128 result = ps2_vminmax(vf(a), lane(vf(b), bc), true);
            const __m128i mask = _mm_setr_epi32((dest & 8u) ? -1 : 0, (dest & 4u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                                (dest & 1u) ? -1 : 0);
            m_ctx->vu0_vf[d] = _mm_blendv_ps(m_ctx->vu0_vf[d], result, _mm_castsi128_ps(mask));
        }
        // VMINIbc.dest vf[d], vf[a], vf[b]bc (no flags).
        void minimumBc(uint32_t dest, int d, int a, int b, Lane bc)
        {
            const __m128 result = ps2_vminmax(vf(a), lane(vf(b), bc), false);
            const __m128i mask = _mm_setr_epi32((dest & 8u) ? -1 : 0, (dest & 4u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                                (dest & 1u) ? -1 : 0);
            m_ctx->vu0_vf[d] = _mm_blendv_ps(m_ctx->vu0_vf[d], result, _mm_castsi128_ps(mask));
        }
        // CFC2 vi16: the status flags as the last FMAC left them (bit 1, S:
        // a lane of its destination field negative).
        uint32_t status() const { return m_ctx->vu0_status; }
        // VMULbc.dest vf[d], vf[a], vf[b]bc
        void mulBc(uint32_t dest, int d, int a, int b, Lane bc)
        {
            m_ctx->vu0_vf[d] = fmac(ps2_vmul(vf(a), lane(vf(b), bc)), dest, m_ctx->vu0_vf[d]);
        }
        // VMADDbc.dest vf[d], vf[a], vf[b]bc
        void madd(uint32_t dest, int d, int a, int b, Lane bc)
        {
            m_ctx->vu0_vf[d] = fmac(ps2_vadd(m_ctx->vu0_acc, ps2_vmul(vf(a), lane(vf(b), bc))), dest, m_ctx->vu0_vf[d]);
        }
        // VDIV Q, vf0w, vf[d]bc: Q = 1 / the lane (VU0's divide, towards zero).
        void divideOne(int d, Lane bc) { m_ctx->vu0_q = ps2_fdiv(_mm_cvtss_f32(lane(vf(0), kLaneW)), _mm_cvtss_f32(lane(vf(d), bc))); }
        // VMULq.dest vf[d], vf[a], Q
        void mulQ(uint32_t dest, int d, int a) { m_ctx->vu0_vf[d] = fmac(ps2_vmul(vf(a), _mm_set1_ps(m_ctx->vu0_q)), dest, m_ctx->vu0_vf[d]); }
        // VMUL.dest vf[d], vf[a], vf[b]; VADDbc.dest vf[d], vf[a], vf[b]bc
        void mul(uint32_t dest, int d, int a, int b) { m_ctx->vu0_vf[d] = fmac(ps2_vmul(vf(a), vf(b)), dest, m_ctx->vu0_vf[d]); }
        // VADD.dest / VSUB.dest vf[d], vf[a], vf[b]
        void add(uint32_t dest, int d, int a, int b) { m_ctx->vu0_vf[d] = fmac(ps2_vadd(vf(a), vf(b)), dest, m_ctx->vu0_vf[d]); }
        void sub(uint32_t dest, int d, int a, int b) { m_ctx->vu0_vf[d] = fmac(ps2_vsub(vf(a), vf(b)), dest, m_ctx->vu0_vf[d]); }
        // The engine's vector sum and difference (LQC2 vf4 a, vf5 b; VADD /
        // VSUB.xyzw vf6; SQC2 vf6 to out), as ee::vu0::Machine's.
        void sum(uint32_t a, uint32_t b, uint32_t out)
        {
            load(4, a);
            load(5, b);
            add(kXyzw, 6, 4, 5);
            store(6, out);
        }
        void difference(uint32_t a, uint32_t b, uint32_t out)
        {
            load(4, a);
            load(5, b);
            sub(kXyzw, 6, 4, 5);
            store(6, out);
        }
        // The engine's vector scale (LQC2 vf4 v; MFC1 + QMTC2 vf5: x = s,
        // y its sign, z and w `t0High`, the upper half of the EE's t0 that
        // QMTC2 moves too; VMULx.xyz vf6, vf4, vf5x; SQC2 vf6 to out): v
        // times s, w is vf6's.
        void scale(uint32_t v, ee::Float s, uint32_t out, uint64_t t0High = 0)
        {
            load(4, v);
            const uint64_t low = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s.bits())));
            m_ctx->vu0_vf[5] = _mm_castsi128_ps(_mm_set_epi64x(static_cast<int64_t>(t0High), static_cast<int64_t>(low)));
            m_ctx->vu0_vf[6] = fmac(ps2_vmul(vf(4), lane(vf(5), kLaneX)), kXyz, vf(6));
            store(6, out);
        }
        // The same with VSUBx.xyz: v less s on each of x, y and z.
        void less(uint32_t v, ee::Float s, uint32_t out, uint64_t t0High)
        {
            load(4, v);
            const uint64_t low = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s.bits())));
            m_ctx->vu0_vf[5] = _mm_castsi128_ps(_mm_set_epi64x(static_cast<int64_t>(t0High), static_cast<int64_t>(low)));
            m_ctx->vu0_vf[6] = fmac(ps2_vsub(vf(4), lane(vf(5), kLaneX)), kXyz, vf(6));
            store(6, out);
        }
        void less(const void *v, ee::Float s, void *out)
        {
            load(4, v);
            const uint64_t low = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(s.bits())));
            m_ctx->vu0_vf[5] = _mm_castsi128_ps(_mm_set_epi64x(0, static_cast<int64_t>(low)));
            m_ctx->vu0_vf[6] = fmac(ps2_vsub(vf(4), lane(vf(5), kLaneX)), kXyz, vf(6));
            store(6, out);
        }
        // The engine's rotation of a vector by a matrix's 3x3 (LQC2 vf8 v,
        // vf4 .. vf6 rows 0 .. 2; rotate(9, 4, 8); SQC2 vf9 to out).
        void rotateVector(uint32_t matrix, uint32_t v, uint32_t out)
        {
            load(8, v);
            for (int row = 0; row < 3; ++row)
                load(4 + row, matrix + 0x10u * row);
            rotate(9, 4, 8);
            store(9, out);
        }
        void addBc(uint32_t dest, int d, int a, int b, Lane bc)
        {
            m_ctx->vu0_vf[d] = fmac(ps2_vadd(vf(a), lane(vf(b), bc)), dest, m_ctx->vu0_vf[d]);
        }
        // A register's x, as QMFC2 + MTC1 hand it to the FPU.
        ee::Float x(int reg) const { return ee::Float(_mm_cvtss_f32(vf(reg))); }
        // The engine's dot product (LQC2 vf4 a, vf5 b; VMUL.xyz vf5; VADDy.x,
        // VADDz.x vf5): a . b over x, y and z.
        ee::Float dot(uint32_t a, uint32_t b)
        {
            load(4, a);
            load(5, b);
            mul(kXyz, 5, 4, 5);
            addBc(kX, 5, 5, 5, kLaneY);
            addBc(kX, 5, 5, 5, kLaneZ);
            return x(5);
        }
        // The engine's squared length (LQC2 vf4 v; VMUL.xyz vf5, vf4, vf4;
        // VADDy.x, VADDz.x vf5): v . v over x, y and z.
        ee::Float squaredLength(uint32_t v)
        {
            load(4, v);
            mul(kXyz, 5, 4, 4);
            addBc(kX, 5, 5, 5, kLaneY);
            addBc(kX, 5, 5, 5, kLaneZ);
            return x(5);
        }
        // The engine's point transform (LQC2 vf7 row 3, vf8 v, vf4 .. vf6 rows
        // 0 .. 2; then transform(9, 4, 7, 8); SQC2 vf9 to out).
        void transformPoint(uint32_t matrix, uint32_t v, uint32_t out)
        {
            load(7, matrix + 0x30u);
            load(8, v);
            for (int row = 0; row < 3; ++row)
                load(4 + row, matrix + 0x10u * row);
            transform(9, 4, 7, 8);
            store(9, out);
        }

        // VMINI.xyzw / VMAX.xyzw vf[d], vf[a], vf[b] (no flags).
        void minimum(int d, int a, int b) { m_ctx->vu0_vf[d] = ps2_vminmax(vf(a), vf(b), false); }
        void maximum(int d, int a, int b) { m_ctx->vu0_vf[d] = ps2_vminmax(vf(a), vf(b), true); }

        // Three VMULA / VMADDA / VMADD steps: vf[d].xyz = vf[v] (xyz) by the
        // rotation in vf[r0], vf[r0 + 1], vf[r0 + 2] (row vectors).
        void rotate(int d, int r0, int v)
        {
            mulAcc(kXyz, r0, v, kLaneX);
            maddAcc(kXyz, r0 + 1, v, kLaneY);
            madd(kXyz, d, r0 + 2, v, kLaneZ);
        }
        // The same with the translation in vf[t] first (VMULAw by vf0's w,
        // 1): vf[d].xyz = vf[v] as a point by the matrix.
        void transform(int d, int r0, int t, int v)
        {
            mulAcc(kXyz, t, 0, kLaneW);
            maddAcc(kXyz, r0, v, kLaneX);
            maddAcc(kXyz, r0 + 1, v, kLaneY);
            madd(kXyz, d, r0 + 2, v, kLaneZ);
        }

    private:
        __m128 vf(int reg) const { return m_ctx->vu0_vf[reg]; }
        static __m128 lane(__m128 v, Lane bc)
        {
            switch (bc)
            {
            case kLaneX: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
            case kLaneY: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
            case kLaneZ: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
            default: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
            }
        }
        // An FMAC result: its flags, and the `dest` lanes of `into` replaced.
        __m128 fmac(__m128 result, uint32_t dest, __m128 into)
        {
            ps2_vu0_fmac_flags(m_ctx, result, dest);
            const __m128i mask = _mm_setr_epi32((dest & 8u) ? -1 : 0, (dest & 4u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                                (dest & 1u) ? -1 : 0);
            return _mm_blendv_ps(into, result, _mm_castsi128_ps(mask));
        }

        // The registers outside a readable call (made once a thread, so a
        // Registers costs nothing to make in the hot routines).
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
}
