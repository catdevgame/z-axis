#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include "ee_instructions.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using namespace ps2recomp;

namespace
{
    constexpr uint32_t kPlusMax = 0x7FFFFFFFu;
    constexpr uint32_t kMinusMax = 0xFFFFFFFFu;
    // 2^128: the host's +Inf pattern, an ordinary number on the PS2.
    constexpr uint32_t kTwoPow128 = 0x7F800000u;

    uint32_t bitsOf(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    float floatOf(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }

    Instruction makeCop1S(uint32_t function, uint8_t fd, uint8_t fs, uint8_t ft)
    {
        Instruction inst{};
        inst.opcode = OPCODE_COP1;
        inst.rs = COP1_S;
        inst.rt = ft;
        inst.rd = fs;
        inst.sa = fd;
        inst.function = function;
        return inst;
    }

    uint32_t laneBits(__m128 v, int lane)
    {
        float lanes[4];
        _mm_storeu_ps(lanes, v);
        return bitsOf(lanes[lane]);
    }

    struct HardwareVector
    {
        const char *op;
        uint32_t fs;
        uint32_t ft;
        uint32_t result;
        // 0: must match exactly.
        // 1: the hardware multiplier drops a bit in some products (1.0 * 0x3FFFFFFF = 0x3FFFFFFE) and its
        //    divide unit has its own rule; neither is emulated (nor in PCSX2), so the row differs in the last
        //    mantissa bit. (ADD.S/SUB.S round toward zero and match.)
        // 3: RSQRT.S as PCSX2 computes it, SQRTSS then DIVSS both rounded toward zero, which is one unit
        //    away from the hardware's divider here. We follow PCSX2, the reference for comparisons.
        int known;
    };

    const HardwareVector kHardwareVectors[] = {
#include "ps2_float_hw_vectors.inc"
    };

    uint32_t runHardwareOp(const HardwareVector &v)
    {
        const float fs = floatOf(v.fs);
        const float ft = floatOf(v.ft);
        const std::string op = v.op;
        if (op == "add")
            return bitsOf(FPU_ADD_S(fs, ft));
        if (op == "sub")
            return bitsOf(FPU_SUB_S(fs, ft));
        if (op == "mul")
            return bitsOf(FPU_MUL_S(fs, ft));
        if (op == "div")
            return bitsOf(FPU_DIV_S(fs, ft));
        if (op == "sqrt")
            return bitsOf(FPU_SQRT_S(fs));
        return bitsOf(FPU_RSQRT_S(fs, ft));
    }

    std::string describe(const HardwareVector &v, uint32_t got)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s %08x, %08x: PS2 gives %08x, got %08x", v.op, v.fs, v.ft, v.result, got);
        return text;
    }
}

void register_ps2_float_semantics_tests()
{
    MiniTest::Case("PS2FloatSemantics", [](TestCase &tc)
    {
        tc.Run("FPU results match the PS2 hardware results from ps2autotests", [](TestCase &t)
        {
            for (const HardwareVector &v : kHardwareVectors)
            {
                const uint32_t got = runHardwareOp(v);
                if (v.known == 1 || v.known == 3)
                {
                    const uint32_t distance = got > v.result ? got - v.result : v.result - got;
                    t.IsTrue(distance == 1u, describe(v, got));
                }
                else
                {
                    t.Equals(got, v.result, describe(v, got));
                }
            }
        });

        tc.Run("exponent 255 is an ordinary exponent, not Inf or NaN", [](TestCase &t)
        {
            const float twoPow128 = floatOf(kTwoPow128);
            t.Equals(bitsOf(FPU_ADD_S(twoPow128, 0.0f)), kTwoPow128, "2^128 + 0 should stay 2^128");
            t.Equals(bitsOf(FPU_MUL_S(twoPow128, 0.5f)), 0x7F000000u, "2^128 * 0.5 should be 2^127");
            t.Equals(bitsOf(FPU_MUL_S(floatOf(0x7F000000u), 2.0f)), kTwoPow128, "2^127 * 2 should be 2^128, not saturated");
            t.Equals(bitsOf(FPU_ADD_S(floatOf(0x7F000000u), floatOf(0x7F000000u))), kTwoPow128, "2^127 + 2^127 should be 2^128");
            t.Equals(bitsOf(FPU_SUB_S(floatOf(0x7FC00000u), twoPow128)), 0x7F000000u, "1.5*2^128 - 2^128 should be 2^127");
            t.Equals(bitsOf(FPU_DIV_S(floatOf(0x7FC00000u), 3.0f)), 0x7F000000u, "1.5*2^128 / 3 should be 2^127");
            t.Equals(bitsOf(FPU_SQRT_S(twoPow128)), 0x5F800000u, "sqrt(2^128) should be 2^64");
        });

        tc.Run("FPU results saturate at 0x7FFFFFFF and flush below 2^-126", [](TestCase &t)
        {
            const float maxf = floatOf(kPlusMax);
            const float minf = floatOf(kMinusMax);
            t.Equals(bitsOf(FPU_ADD_S(maxf, maxf)), kPlusMax, "max + max should saturate to +max");
            t.Equals(bitsOf(FPU_ADD_S(minf, minf)), kMinusMax, "-max + -max should saturate to -max");
            t.Equals(bitsOf(FPU_SUB_S(maxf, minf)), kPlusMax, "max - (-max) should saturate to +max");
            t.Equals(bitsOf(FPU_MUL_S(maxf, 2.0f)), kPlusMax, "max * 2 should saturate to +max");
            t.Equals(bitsOf(FPU_MUL_S(maxf, -2.0f)), kMinusMax, "max * -2 should saturate to -max");
            t.Equals(bitsOf(FPU_DIV_S(maxf, 0.5f)), kPlusMax, "max / 0.5 should saturate to +max");
            t.Equals(bitsOf(FPU_MUL_S(floatOf(0x00800000u), 0.5f)), 0x00000000u, "2^-126 * 0.5 should flush to +0");
            t.Equals(bitsOf(FPU_MUL_S(floatOf(0x80800000u), 0.5f)), 0x80000000u, "-2^-126 * 0.5 should flush to -0");
            t.Equals(bitsOf(FPU_MUL_S(floatOf(0x00000001u), 1.0f)), 0x00000000u, "a result with exponent 0 should flush to +0");
            t.Equals(bitsOf(FPU_ADD_S(1.0f, 2.0f)), bitsOf(3.0f), "ordinary addition should be unchanged");
            t.Equals(bitsOf(FPU_MUL_S(1.5f, 4.0f)), bitsOf(6.0f), "ordinary multiplication should be unchanged");
        });

        tc.Run("FPU DIV.S by zero gives +-max with the sign of fs xor ft", [](TestCase &t)
        {
            t.Equals(bitsOf(FPU_DIV_S(1.0f, 0.0f)), kPlusMax, "1/+0 should be +max");
            t.Equals(bitsOf(FPU_DIV_S(-1.0f, 0.0f)), kMinusMax, "-1/+0 should be -max");
            t.Equals(bitsOf(FPU_DIV_S(1.0f, -0.0f)), kMinusMax, "1/-0 should be -max");
            t.Equals(bitsOf(FPU_DIV_S(-1.0f, -0.0f)), kPlusMax, "-1/-0 should be +max");
            t.Equals(bitsOf(FPU_DIV_S(0.0f, 0.0f)), kPlusMax, "0/0 should be +max");
            t.Equals(bitsOf(FPU_DIV_S(1.0f, floatOf(0x00000001u))), kPlusMax, "an exponent-0 divisor should count as zero");
            t.Equals(bitsOf(FPU_DIV_S(6.0f, 3.0f)), bitsOf(2.0f), "ordinary division should be unchanged");
        });

        tc.Run("FPU RSQRT.S computes fs / sqrt(|ft|) and takes the sign of fs alone on a zero ft", [](TestCase &t)
        {
            t.Equals(bitsOf(FPU_RSQRT_S(4.0f, 16.0f)), bitsOf(1.0f), "4 / sqrt(16) should be 1");
            t.Equals(bitsOf(FPU_RSQRT_S(1.0f, 4.0f)), bitsOf(0.5f), "1 / sqrt(4) should be 0.5");
            t.Equals(bitsOf(FPU_RSQRT_S(1.0f, -4.0f)), bitsOf(0.5f), "the sign of the radicand should be ignored");
            t.Equals(bitsOf(FPU_RSQRT_S(2.0f, 0.0f)), kPlusMax, "x / sqrt(+0) should be +max");
            t.Equals(bitsOf(FPU_RSQRT_S(2.0f, -0.0f)), kPlusMax, "x / sqrt(-0) should be +max: the sign of ft is ignored");
            t.Equals(bitsOf(FPU_RSQRT_S(-2.0f, 0.0f)), kMinusMax, "-x / sqrt(+0) should be -max");
            t.Equals(bitsOf(FPU_RSQRT_S(-2.0f, -0.0f)), kMinusMax, "-x / sqrt(-0) should be -max");
        });

        tc.Run("FPU SQRT.S takes the square root of the magnitude", [](TestCase &t)
        {
            t.Equals(bitsOf(FPU_SQRT_S(16.0f)), bitsOf(4.0f), "sqrt(16) should be 4");
            t.Equals(bitsOf(FPU_SQRT_S(-16.0f)), bitsOf(4.0f), "sqrt(-16) should be 4");
            t.Equals(bitsOf(FPU_SQRT_S(-0.0f)), bitsOf(0.0f), "sqrt(-0) should be +0");
        });

        tc.Run("VU vector ADD/SUB/MUL follow the same rules in every lane", [](TestCase &t)
        {
            // _mm_set_ps lists lanes 3..0
            const __m128 a = _mm_set_ps(1.0f, floatOf(kTwoPow128), floatOf(kMinusMax), floatOf(kPlusMax));
            const __m128 b = _mm_set_ps(2.0f, 1.0f, floatOf(kMinusMax), floatOf(kPlusMax));

            const __m128 sum = PS2_VADD(a, b);
            t.Equals(laneBits(sum, 0), kPlusMax, "lane 0: max + max should saturate to +max");
            t.Equals(laneBits(sum, 1), kMinusMax, "lane 1: -max + -max should saturate to -max");
            t.Equals(laneBits(sum, 2), kTwoPow128, "lane 2: 2^128 + 1 should stay 2^128");
            t.Equals(laneBits(sum, 3), bitsOf(3.0f), "lane 3: 1 + 2 should be 3");

            const __m128 diff = PS2_VSUB(a, b);
            t.Equals(laneBits(diff, 0), bitsOf(0.0f), "lane 0: max - max should be 0");
            t.Equals(laneBits(diff, 2), kTwoPow128, "lane 2: 2^128 - 1 should stay 2^128");
            t.Equals(laneBits(diff, 3), bitsOf(-1.0f), "lane 3: 1 - 2 should be -1");

            const __m128 prod = PS2_VMUL(a, b);
            t.Equals(laneBits(prod, 0), kPlusMax, "lane 0: max * max should saturate to +max");
            t.Equals(laneBits(prod, 1), kPlusMax, "lane 1: -max * -max should saturate to +max");
            t.Equals(laneBits(prod, 2), kTwoPow128, "lane 2: 2^128 * 1 should stay 2^128");
            t.Equals(laneBits(prod, 3), bitsOf(2.0f), "lane 3: 1 * 2 should be 2");

            const __m128 tiny = _mm_set_ps(1.0f, floatOf(0x80800000u), floatOf(0x00800000u), 3.0f);
            const __m128 half = _mm_set1_ps(0.5f);
            const __m128 flushed = PS2_VMUL(tiny, half);
            t.Equals(laneBits(flushed, 0), bitsOf(1.5f), "lane 0: 3 * 0.5 should be 1.5");
            t.Equals(laneBits(flushed, 1), 0x00000000u, "lane 1: 2^-126 * 0.5 should flush to +0");
            t.Equals(laneBits(flushed, 2), 0x80000000u, "lane 2: -2^-126 * 0.5 should flush to -0");
            t.Equals(laneBits(flushed, 3), bitsOf(0.5f), "lane 3: 1 * 0.5 should be 0.5");
        });

        tc.Run("EE FPU and VU0 round toward zero like PCSX2's Chop mode", [](TestCase &t)
        {
            // 1 + (1 + 2^-23) * 2^-1 = 1.5 + 2^-24: round to nearest gives 1.5 + 2^-23 (ties to even), toward zero 1.5.
            const float half = floatOf(0x3F000001u);
            t.Equals(bitsOf(FPU_ADD_S(1.0f, half)), 0x3FC00000u, "ADD.S drops the half unit");
            t.Equals(bitsOf(FPU_ADD_S(-1.0f, -half)), 0xBFC00000u, "ADD.S toward zero for negative sums too");
            t.Equals(bitsOf(FPU_SUB_S(1.0f, -half)), 0x3FC00000u, "SUB.S drops the half unit");
            // The R5900 adder (and PCSX2's FPU_ADD_SUB) drops an addend whose exponent is 25 or more below
            // the other one, so 1 - 2^-60 is 1, not the float below 1 that IEEE round toward zero gives.
            t.Equals(bitsOf(FPU_ADD_S(1.0f, -0x1p-60f)), 0x3F800000u, "ADD.S: far smaller addend is dropped");
            t.Equals(bitsOf(FPU_ADD_S(1.0f, 0x1p-60f)), 0x3F800000u, "ADD.S: tiny positive addend leaves 1");
            t.Equals(bitsOf(FPU_SUB_S(-0x1p-60f, 1.0f)), 0xBF800000u, "SUB.S: far smaller minuend is dropped");
            // Exponent difference 2..24: the smaller operand keeps one bit below the larger one's last bit.
            // 1 - (2^-24 + 2^-25): IEEE chop gives 0x3F7FFFFE; the PS2 drops the 2^-25 bit first: 0x3F7FFFFF.
            t.Equals(bitsOf(FPU_SUB_S(1.0f, floatOf(0x33C00000u))), 0x3F7FFFFFu,
                     "SUB.S: bits beyond the guard bit of the smaller operand are dropped first");
            t.Equals(bitsOf(FPU_ADD_S(-1.0f, floatOf(0x33C00000u))), 0xBF7FFFFFu,
                     "ADD.S: the same with the operands' signs swapped");
            // Exponent difference 1: nothing is dropped.
            t.Equals(bitsOf(FPU_ADD_S(2.0f, floatOf(0x3F800001u))), bitsOf(3.0f), "ADD.S: difference 1 keeps every bit");
            // An exponent-0 operand is zero, as with PCSX2's denormals-are-zero.
            t.Equals(bitsOf(FPU_ADD_S(1.0f, floatOf(0x80000001u))), 0x3F800000u, "ADD.S ignores an exponent-0 operand");

            // (1 + 2^-23)^2 = 1 + 2^-22 + 2^-46: nearest and toward zero agree; (1 + 3 * 2^-23) * 1.5 does not.
            const float a = floatOf(0x3F800003u);
            t.Equals(bitsOf(FPU_MUL_S(a, 1.5f)), 0x3FC00004u, "MUL.S toward zero");
            t.Equals(bitsOf(FPU_MUL_S(-a, 1.5f)), 0xBFC00004u, "MUL.S toward zero, negative");
            // Stepping down from 2^-126 leaves exponent 0, which is flushed.
            t.Equals(bitsOf(FPU_MUL_S(floatOf(0x00800000u), floatOf(0x3F7FFFFFu))), 0x00000000u,
                     "MUL.S just below 2^-126 flushes to 0");

            // MADD.S rounds the product and the sum separately.
            const float madd = FPU_ADD_S(1.0f, FPU_MUL_S(a, 1.5f));
            t.Equals(bitsOf(madd), 0x40200002u, "MADD.S: two roundings toward zero");

            // CVT.S.W: 2^24 + 3 needs 25 bits; nearest gives 2^24 + 4, toward zero 2^24 + 2.
            t.Equals(bitsOf(FPU_CVT_S_W(16777219)), bitsOf(16777218.0f), "CVT.S.W toward zero");
            t.Equals(bitsOf(FPU_CVT_S_W(-16777219)), bitsOf(-16777218.0f), "CVT.S.W toward zero, negative");
            t.Equals(bitsOf(FPU_CVT_S_W(7)), bitsOf(7.0f), "CVT.S.W exact");

            // DIV.S and SQRT.S keep round to nearest (PCSX2's FPUDiv mode and its SQRT.S); VU0 DIV/SQRT chop.
            t.Equals(bitsOf(FPU_DIV_S(1.0f, 5.0f)), 0x3E4CCCCDu, "DIV.S rounds 1/5 to nearest");
            t.Equals(bitsOf(ps2_fdiv(1.0f, 5.0f)), 0x3E4CCCCCu, "VU0 DIV rounds 1/5 toward zero");
            t.Equals(bitsOf(FPU_SQRT_S(2.0f)), 0x3FB504F3u, "SQRT.S rounds sqrt(2) to nearest");
            t.Equals(bitsOf(ps2_fsqrt(2.0f)), 0x3FB504F3u, "VU0 SQRT: sqrt(2) toward zero is the same float");
            t.Equals(bitsOf(ps2_fsqrt(3.0f)), 0x3FDDB3D7u, "VU0 SQRT rounds sqrt(3) toward zero");
            t.Equals(bitsOf(FPU_SQRT_S(3.0f)), 0x3FDDB3D7u, "SQRT.S: sqrt(3) to nearest is the same float");
            t.Equals(bitsOf(FPU_SQRT_S(5.0f)), 0x400F1BBDu, "SQRT.S rounds sqrt(5) to nearest");
            t.Equals(bitsOf(ps2_fsqrt(5.0f)), 0x400F1BBCu, "VU0 SQRT rounds sqrt(5) toward zero");

            // Vector lanes.
            const __m128 va = _mm_set_ps(2.0f, -1.0f, 1.0f, 1.0f);
            const __m128 vb = _mm_set_ps(1.0f, -half, half, -0x1p-60f);
            const __m128 vsum = PS2_VADD(va, vb);
            t.Equals(laneBits(vsum, 0), 0x3F7FFFFFu, "VADD lane 0: 1 - 2^-60 toward zero");
            t.Equals(laneBits(vsum, 1), 0x3FC00000u, "VADD lane 1 toward zero");
            t.Equals(laneBits(vsum, 2), 0xBFC00000u, "VADD lane 2 toward zero");
            t.Equals(laneBits(vsum, 3), bitsOf(3.0f), "VADD lane 3 exact");
            const __m128 vdiff = PS2_VSUB(va, _mm_set_ps(1.0f, half, -half, 0x1p-60f));
            t.Equals(laneBits(vdiff, 0), 0x3F7FFFFFu, "VSUB lane 0 toward zero");
            t.Equals(laneBits(vdiff, 1), 0x3FC00000u, "VSUB lane 1 toward zero");
            t.Equals(laneBits(vdiff, 2), 0xBFC00000u, "VSUB lane 2 toward zero");
            const __m128 vprod = PS2_VMUL(_mm_set_ps(1.0f, -a, a, floatOf(0x00800000u)),
                                          _mm_set_ps(2.0f, 1.5f, 1.5f, floatOf(0x3F7FFFFFu)));
            t.Equals(laneBits(vprod, 0), 0x00000000u, "VMUL lane 0 just below 2^-126 flushes");
            t.Equals(laneBits(vprod, 1), 0x3FC00004u, "VMUL lane 1 toward zero");
            t.Equals(laneBits(vprod, 2), 0xBFC00004u, "VMUL lane 2 toward zero");
            t.Equals(laneBits(vprod, 3), bitsOf(2.0f), "VMUL lane 3 exact");
            // A lane taking the exponent-255 path keeps the other lanes chopped.
            const __m128 mixed = PS2_VADD(_mm_set_ps(1.0f, 1.0f, 1.0f, floatOf(kTwoPow128)),
                                          _mm_set_ps(half, half, half, 1.0f));
            t.Equals(laneBits(mixed, 0), kTwoPow128, "mixed lane 0: 2^128 + 1");
            t.Equals(laneBits(mixed, 1), 0x3FC00000u, "mixed lane 1 toward zero through the slow path");
        });

        tc.Run("FPU CVT.W.S truncates toward zero and saturates", [](TestCase &t)
        {
            t.Equals(FPU_CVT_W_S(2.75f), 2, "2.75 -> 2");
            t.Equals(FPU_CVT_W_S(-2.75f), -2, "-2.75 -> -2");
            t.Equals(FPU_CVT_W_S(0.5f), 0, "0.5 -> 0 (not rounded to nearest even)");
            t.Equals(FPU_CVT_W_S(1.5f), 1, "1.5 -> 1");
            t.Equals(FPU_CVT_W_S(3.0e9f), std::numeric_limits<int32_t>::max(), "3e9 saturates high");
            t.Equals(FPU_CVT_W_S(-3.0e9f), std::numeric_limits<int32_t>::min(), "-3e9 saturates low");
            t.Equals(FPU_CVT_W_S(floatOf(kPlusMax)), std::numeric_limits<int32_t>::max(), "+max saturates high");
            t.Equals(FPU_CVT_W_S(floatOf(kMinusMax)), std::numeric_limits<int32_t>::min(), "-max saturates low");
        });

        tc.Run("FPU C.LT/C.LE/C.EQ treat exponent 255 as a number", [](TestCase &t)
        {
            t.IsTrue(FPU_C_LT_S(1.0f, floatOf(kPlusMax)), "1 < +max");
            t.IsTrue(FPU_C_LT_S(floatOf(kMinusMax), 1.0f), "-max < 1");
            t.IsTrue(FPU_C_LE_S(floatOf(kPlusMax), floatOf(kPlusMax)), "+max <= +max");
            t.IsTrue(FPU_C_EQ_S(floatOf(kPlusMax), floatOf(kPlusMax)), "+max == +max");
            t.IsTrue(FPU_C_LT_S(floatOf(kTwoPow128), floatOf(kPlusMax)), "2^128 < +max");
            t.IsTrue(FPU_C_EQ_S(0.0f, -0.0f), "+0 == -0");
            // Older generated code calls the standard-MIPS names for the R5900's 0x34/0x36 encodings.
            t.IsTrue(FPU_C_OLT_S(1.0f, floatOf(kPlusMax)), "OLT: 1 < +max (0x7FFFFFFF is not NaN)");
            t.IsTrue(!FPU_C_OLT_S(floatOf(kPlusMax), 1.0f), "OLT: not +max < 1");
            t.IsTrue(FPU_C_OLT_S(floatOf(kMinusMax), -1.0f), "OLT: -max < -1");
            t.IsTrue(FPU_C_OLE_S(floatOf(kPlusMax), floatOf(kPlusMax)), "OLE: +max <= +max");
            t.IsTrue(FPU_C_OLE_S(1.0f, floatOf(kTwoPow128)), "OLE: 1 <= 2^128");
        });

        tc.Run("FPU MAX.S/MIN.S order exponent-255 values as numbers", [](TestCase &t)
        {
            t.Equals(bitsOf(FPU_MAX_S(1.0f, floatOf(kPlusMax))), kPlusMax, "max(1, +max)");
            t.Equals(bitsOf(FPU_MAX_S(floatOf(kPlusMax), 1.0f)), kPlusMax, "max(+max, 1)");
            t.Equals(bitsOf(FPU_MIN_S(1.0f, floatOf(kPlusMax))), bitsOf(1.0f), "min(1, +max)");
            t.Equals(bitsOf(FPU_MIN_S(floatOf(kPlusMax), 1.0f)), bitsOf(1.0f), "min(+max, 1)");
            t.Equals(bitsOf(FPU_MIN_S(-1.0f, floatOf(kMinusMax))), kMinusMax, "min(-1, -max)");
            t.Equals(bitsOf(FPU_MAX_S(floatOf(kTwoPow128), floatOf(kPlusMax))), kPlusMax, "max(2^128, +max)");
            t.Equals(bitsOf(FPU_MAX_S(-2.0f, 3.0f)), bitsOf(3.0f), "max(-2, 3)");
            t.Equals(bitsOf(FPU_MIN_S(-2.0f, 3.0f)), bitsOf(-2.0f), "min(-2, 3)");

            CodeGenerator gen({}, {});
            const std::string max = gen.translateInstruction(makeCop1S(COP1_S_MAX, 2, 3, 4));
            t.IsTrue(max.find("ctx->f[2] = FPU_MAX_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "MAX.S should use FPU_MAX_S but got: " + max);
            const std::string min = gen.translateInstruction(makeCop1S(COP1_S_MIN, 2, 3, 4));
            t.IsTrue(min.find("ctx->f[2] = FPU_MIN_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "MIN.S should use FPU_MIN_S but got: " + min);
        });

        tc.Run("VU0 macro MAX/MINI compare raw words as sign-magnitude numbers", [](TestCase &t)
        {
            const __m128 a = _mm_castsi128_ps(_mm_set_epi32(0x12345678, static_cast<int32_t>(0xBF800000u),
                                                            static_cast<int32_t>(kPlusMax), 0x3F800000));
            const __m128 b = _mm_castsi128_ps(_mm_set_epi32(0x00000007, static_cast<int32_t>(0xC0000000u),
                                                            0x3F800000, static_cast<int32_t>(kPlusMax)));
            const __m128 mx = PS2_VMAX(a, b);
            const __m128 mn = PS2_VMINI(a, b);
            t.Equals(laneBits(mx, 0), kPlusMax, "max(1, +max) lane 0");
            t.Equals(laneBits(mx, 1), kPlusMax, "max(+max, 1) lane 1");
            t.Equals(laneBits(mx, 2), 0xBF800000u, "max(-1, -2) lane 2");
            t.Equals(laneBits(mx, 3), 0x12345678u, "integer data passes through max");
            t.Equals(laneBits(mn, 0), 0x3F800000u, "min(1, +max) lane 0");
            t.Equals(laneBits(mn, 1), 0x3F800000u, "min(+max, 1) lane 1");
            t.Equals(laneBits(mn, 2), 0xC0000000u, "min(-1, -2) lane 2");
            t.Equals(laneBits(mn, 3), 0x00000007u, "integer data passes through min");

            CodeGenerator gen({}, {});
            Instruction inst{};
            inst.sa = 1;
            inst.rd = 2;
            inst.rt = 3;
            inst.vectorInfo.vectorField = 0xF;
            const std::string vmax = gen.translateVU_VMAX(inst);
            t.IsTrue(vmax.find("PS2_VMAX(ctx->vu0_vf[2], ctx->vu0_vf[3])") != std::string::npos,
                     "VMAX should use PS2_VMAX but got: " + vmax);
            t.IsTrue(vmax.find("_mm_max_ps") == std::string::npos, "VMAX must not use _mm_max_ps: " + vmax);
            const std::string vmini = gen.translateVU_VMINI(inst);
            t.IsTrue(vmini.find("PS2_VMINI(ctx->vu0_vf[2], ctx->vu0_vf[3])") != std::string::npos,
                     "VMINI should use PS2_VMINI but got: " + vmini);
        });

        tc.Run("C.LT.S and C.LE.S (funct 0x34/0x36 on the R5900) use the PS2 compare", [](TestCase &t)
        {
            CodeGenerator gen({}, {});
            const std::string lt = gen.translateInstruction(makeCop1S(COP1_S_C_OLT, 0, 3, 4));
            t.IsTrue(lt.find("FPU_C_LT_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "c.lt.s should compare fs < ft with FPU_C_LT_S but got: " + lt);
            const std::string le = gen.translateInstruction(makeCop1S(COP1_S_C_OLE, 0, 3, 4));
            t.IsTrue(le.find("FPU_C_LE_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "c.le.s should compare fs <= ft with FPU_C_LE_S but got: " + le);
        });

        tc.Run("DIV.S and RSQRT.S translate to the PS2 semantics helpers", [](TestCase &t)
        {
            CodeGenerator gen({}, {});

            // fd = f2, fs = f3, ft = f4 (all distinct so a wrong operand shows up)
            const std::string div = gen.translateInstruction(makeCop1S(COP1_S_DIV, 2, 3, 4));
            t.IsTrue(div.find("ctx->f[2] = FPU_DIV_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "DIV.S should write fs / ft to fd but got: " + div);
            t.IsTrue(div.find("INFINITY") == std::string::npos, "DIV.S must not produce IEEE infinity but got: " + div);

            const std::string rsqrt = gen.translateInstruction(makeCop1S(COP1_S_RSQRT, 2, 3, 4));
            t.IsTrue(rsqrt.find("ctx->f[2] = FPU_RSQRT_S(ctx->f[3], ctx->f[4])") != std::string::npos,
                     "RSQRT.S should compute fs / sqrt(ft) but got: " + rsqrt);
        });
    });
}
