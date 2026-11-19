// The R5900's conditionals on 64-bit registers (branch64 round).
//
// The compare-with-zero branches (BLEZ, BGTZ, BLTZ, BGEZ, their likely and
// and-link forms) test the whole 64-bit register, as BEQ / BNE, SLT / SLTI /
// SLTU / SLTIU, MOVZ / MOVN and the traps do (PCSX2's interpreter: SD[0]).
// These tests generate each instruction from its encoding, then evaluate the
// emitted condition with the runtime's own register macros on values whose
// low word and doubleword disagree in sign, against the R5900's rule.
#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/types.h"
#include "ps2_runtime_macros.h"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <functional>
#include <regex>
#include <string>
#include <vector>

using namespace ps2recomp;

namespace
{
    constexpr uint32_t kBase = 0x00100000u;

    // Low word and doubleword with different signs (or zero against nonzero).
    const std::array<uint64_t, 8> kValues = {
        0xFFFFFFFF00000000ull, // negative, low word 0 (the soft-double case at 0x22179C)
        0x0000000100000000ull, // positive, low word 0
        0x0000000080000000ull, // positive, low word negative
        0xFFFFFFFF7FFFFFFFull, // negative, low word positive
        0x0000000000000000ull,
        0x0000000000000001ull,
        0xFFFFFFFFFFFFFFFFull,
        0x8000000000000000ull,
    };

    Instruction decode(uint32_t raw, uint32_t address = kBase)
    {
        R5900Decoder decoder;
        return decoder.decodeInstruction(address, raw);
    }

    uint32_t regimm(uint32_t rt, uint32_t rs, uint16_t offset)
    {
        return (OPCODE_REGIMM << 26) | (rs << 21) | (rt << 16) | offset;
    }

    uint32_t itype(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t imm)
    {
        return (opcode << 26) | (rs << 21) | (rt << 16) | imm;
    }

    uint32_t rtype(uint32_t function, uint32_t rs, uint32_t rt, uint32_t rd)
    {
        return (OPCODE_SPECIAL << 26) | (rs << 21) | (rt << 16) | (rd << 11) | function;
    }

    // The branch, a nop delay slot and two more nops (the target).
    std::string generateBranch(uint32_t raw)
    {
        Function func;
        func.name = "branch64";
        func.start = kBase;
        func.end = kBase + 16u;
        func.isRecompiled = true;
        std::vector<Instruction> body = {decode(raw, kBase), decode(0, kBase + 4u), decode(0, kBase + 8u),
                                         decode(0, kBase + 12u)};
        CodeGenerator gen({}, {});
        return gen.generateFunction(func, body, false);
    }

    // The emitted `const bool branch_taken_0x... = (EXPR);` condition.
    std::string branchCondition(const std::string &generated)
    {
        static const std::regex re(R"(const bool branch_taken_0x[0-9a-f]+ = \((.*)\);)");
        std::smatch m;
        return std::regex_search(generated, m, re) ? m[1].str() : std::string();
    }

    // Evaluates `GPR_S64|GPR_S32(ctx, N) OP 0` with the runtime's macros.
    bool evalZeroCompare(const std::string &expr, R5900Context *ctx, bool &ok)
    {
        static const std::regex re(R"(^GPR_(S64|S32|U64|U32)\(ctx, (\d+)\) (<=|<|>=|>|==|!=) 0$)");
        std::smatch m;
        ok = std::regex_match(expr, m, re);
        if (!ok)
            return false;
        const int reg = std::stoi(m[2].str());
        int64_t v = 0;
        if (m[1] == "S64")
            v = GPR_S64(ctx, reg);
        else if (m[1] == "S32")
            v = GPR_S32(ctx, reg);
        else if (m[1] == "U64")
            v = static_cast<int64_t>(GPR_U64(ctx, reg) != 0);
        else
            v = static_cast<int64_t>(GPR_U32(ctx, reg) != 0);
        const std::string op = m[3].str();
        if (op == "<=") return v <= 0;
        if (op == "<") return v < 0;
        if (op == ">=") return v >= 0;
        if (op == ">") return v > 0;
        if (op == "==") return v == 0;
        return v != 0;
    }

    void setGpr(R5900Context &ctx, int reg, uint64_t lo, uint64_t hi = 0)
    {
        ctx.r[reg] = _mm_set_epi64x(static_cast<long long>(hi), static_cast<long long>(lo));
    }

    uint64_t lane(const R5900Context &ctx, int reg, int which)
    {
        alignas(16) uint64_t lanes[2];
        _mm_store_si128(reinterpret_cast<__m128i *>(lanes), ctx.r[reg]);
        return lanes[which];
    }

    struct BranchCase
    {
        const char *name;
        uint32_t raw;
        std::function<bool(int64_t)> rule; // the R5900's (64-bit) condition
        bool links;
        bool likely;
    };
}

void register_ps2_branch64_tests()
{
    MiniTest::Case("Branch64", [](TestCase &tc)
                   {
        const uint32_t a0 = 4;
        const std::vector<BranchCase> cases = {
            {"BLEZ", itype(OPCODE_BLEZ, a0, 0, 2), [](int64_t v) { return v <= 0; }, false, false},
            {"BGTZ", itype(OPCODE_BGTZ, a0, 0, 2), [](int64_t v) { return v > 0; }, false, false},
            {"BLEZL", itype(OPCODE_BLEZL, a0, 0, 2), [](int64_t v) { return v <= 0; }, false, true},
            {"BGTZL", itype(OPCODE_BGTZL, a0, 0, 2), [](int64_t v) { return v > 0; }, false, true},
            {"BLTZ", regimm(REGIMM_BLTZ, a0, 2), [](int64_t v) { return v < 0; }, false, false},
            {"BGEZ", regimm(REGIMM_BGEZ, a0, 2), [](int64_t v) { return v >= 0; }, false, false},
            {"BLTZL", regimm(REGIMM_BLTZL, a0, 2), [](int64_t v) { return v < 0; }, false, true},
            {"BGEZL", regimm(REGIMM_BGEZL, a0, 2), [](int64_t v) { return v >= 0; }, false, true},
            {"BLTZAL", regimm(REGIMM_BLTZAL, a0, 2), [](int64_t v) { return v < 0; }, true, false},
            {"BGEZAL", regimm(REGIMM_BGEZAL, a0, 2), [](int64_t v) { return v >= 0; }, true, false},
            {"BLTZALL", regimm(REGIMM_BLTZALL, a0, 2), [](int64_t v) { return v < 0; }, true, true},
            {"BGEZALL", regimm(REGIMM_BGEZALL, a0, 2), [](int64_t v) { return v >= 0; }, true, true},
        };

        for (const BranchCase &bc : cases)
        {
            tc.Run(std::string(bc.name) + " tests the 64-bit register", [bc](TestCase &t) {
                const std::string generated = generateBranch(bc.raw);
                const std::string cond = branchCondition(generated);
                t.IsTrue(!cond.empty(), std::string(bc.name) + ": a branch condition is emitted");
                t.IsTrue(cond.find("GPR_S64(ctx, 4)") != std::string::npos,
                         std::string(bc.name) + ": the condition reads the doubleword, got: " + cond);
                for (uint64_t value : kValues)
                {
                    R5900Context ctx{};
                    setGpr(ctx, 4, value, 0x1234567890ABCDEFull);
                    bool parsed = false;
                    const bool taken = evalZeroCompare(cond, &ctx, parsed);
                    t.IsTrue(parsed, std::string(bc.name) + ": condition parses: " + cond);
                    t.Equals(taken, bc.rule(static_cast<int64_t>(value)),
                             std::string(bc.name) + fmt::format(": taken for 0x{:016X}", value));
                }
            });
        }

        tc.Run("the and-link forms link whether or not they branch, the likely ones too", [cases](TestCase &t) {
            for (const BranchCase &bc : cases)
            {
                if (!bc.links)
                    continue;
                const std::string generated = generateBranch(bc.raw);
                const size_t cond = generated.find("const bool branch_taken_");
                const size_t link = generated.find("SET_GPR_U32(ctx, 31, 0x100008u);");
                const size_t test = generated.find("if (branch_taken_");
                t.IsTrue(cond != std::string::npos && link != std::string::npos && test != std::string::npos,
                         std::string(bc.name) + ": condition, link and test are emitted");
                t.IsTrue(cond < link && link < test,
                         std::string(bc.name) + ": $ra is written after the condition is read and before the taken test");
            }
        });

        tc.Run("a likely branch not taken skips its delay slot", [](TestCase &t) {
            Function func;
            func.name = "likely64";
            func.start = kBase;
            func.end = kBase + 16u;
            func.isRecompiled = true;
            // bltzl a0, +2 ; addiu v0, zero, 7 (delay slot)
            std::vector<Instruction> body = {decode(regimm(REGIMM_BLTZL, 4, 2), kBase),
                                             decode(itype(OPCODE_ADDIU, 0, 2, 7), kBase + 4u),
                                             decode(0, kBase + 8u), decode(0, kBase + 12u)};
            CodeGenerator gen({}, {});
            const std::string generated = gen.generateFunction(func, body, false);
            const size_t test = generated.find("if (branch_taken_");
            const size_t slot = generated.find("SET_GPR_S32(ctx, 2,");
            t.IsTrue(test != std::string::npos && slot != std::string::npos && test < slot,
                     "the delay slot sits inside the taken test");
        });

        tc.Run("BEQ and BNE compare the doublewords", [](TestCase &t) {
            const std::string beq = branchCondition(generateBranch(itype(OPCODE_BEQ, 4, 5, 2)));
            const std::string bne = branchCondition(generateBranch(itype(OPCODE_BNE, 4, 5, 2)));
            const std::string beql = branchCondition(generateBranch(itype(OPCODE_BEQL, 4, 5, 2)));
            const std::string bnel = branchCondition(generateBranch(itype(OPCODE_BNEL, 4, 5, 2)));
            t.Equals(beq, std::string("GPR_U64(ctx, 4) == GPR_U64(ctx, 5)"), "BEQ");
            t.Equals(bne, std::string("GPR_U64(ctx, 4) != GPR_U64(ctx, 5)"), "BNE");
            t.Equals(beql, std::string("GPR_U64(ctx, 4) == GPR_U64(ctx, 5)"), "BEQL");
            t.Equals(bnel, std::string("GPR_U64(ctx, 4) != GPR_U64(ctx, 5)"), "BNEL");
            // Equal low words, different high words: not equal on the R5900.
            R5900Context ctx{};
            setGpr(ctx, 4, 0x0000000100000005ull);
            setGpr(ctx, 5, 0x0000000000000005ull);
            R5900Context *const c = &ctx;
            t.IsTrue(GPR_U64(c, 4) != GPR_U64(c, 5), "the doublewords differ where the low words agree");
        });

        tc.Run("SLT, SLTI, SLTU and SLTIU compare the doublewords", [](TestCase &t) {
            CodeGenerator gen({}, {});
            const std::string slt = gen.translateInstruction(decode(rtype(SPECIAL_SLT, 4, 5, 2)));
            const std::string sltu = gen.translateInstruction(decode(rtype(SPECIAL_SLTU, 4, 5, 2)));
            const std::string slti = gen.translateInstruction(decode(itype(OPCODE_SLTI, 4, 2, 0xFFFF)));
            const std::string sltiu = gen.translateInstruction(decode(itype(OPCODE_SLTIU, 4, 2, 0xFFFF)));
            t.IsTrue(slt.find("GPR_S64(ctx, 4) < (int64_t)GPR_S64(ctx, 5)") != std::string::npos, "SLT: " + slt);
            t.IsTrue(sltu.find("GPR_U64(ctx, 4) < (uint64_t)GPR_U64(ctx, 5)") != std::string::npos, "SLTU: " + sltu);
            t.IsTrue(slti.find("GPR_S64(ctx, 4) < (int64_t)(int32_t)4294967295") != std::string::npos, "SLTI: " + slti);
            t.IsTrue(sltiu.find("GPR_U64(ctx, 4) < (uint64_t)(int64_t)(int32_t)4294967295") != std::string::npos,
                     "SLTIU (the immediate sign-extended to 64 bits, then compared unsigned): " + sltiu);
        });

        tc.Run("MOVZ and MOVN test the doubleword and move only the low 64 bits", [](TestCase &t) {
            CodeGenerator gen({}, {});
            const std::string movz = gen.translateInstruction(decode(rtype(SPECIAL_MOVZ, 2, 3, 1)));
            const std::string movn = gen.translateInstruction(decode(rtype(SPECIAL_MOVN, 2, 3, 1)));
            t.Equals(movz, std::string("if (GPR_U64(ctx, 3) == 0) SET_GPR_U64(ctx, 1, GPR_U64(ctx, 2));"), "MOVZ");
            t.Equals(movn, std::string("if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 1, GPR_U64(ctx, 2));"), "MOVN");

            // Executed with the runtime's macros: rt's low word is zero but its doubleword is not.
            R5900Context ctx{};
            R5900Context *const c = &ctx;
            setGpr(ctx, 1, 0x1111111111111111ull, 0xAAAAAAAAAAAAAAAAull);
            setGpr(ctx, 2, 0x2222222222222222ull, 0xBBBBBBBBBBBBBBBBull);
            setGpr(ctx, 3, 0x0000000100000000ull);
            if (GPR_U64(c, 3) == 0) SET_GPR_U64(c, 1, GPR_U64(c, 2));
            t.Equals(lane(ctx, 1, 0), 0x1111111111111111ull, "MOVZ does not move when only the low word is zero");
            if (GPR_U64(c, 3) != 0) SET_GPR_U64(c, 1, GPR_U64(c, 2));
            t.Equals(lane(ctx, 1, 0), 0x2222222222222222ull, "MOVN moves rs's low doubleword");
            t.Equals(lane(ctx, 1, 1), 0xAAAAAAAAAAAAAAAAull, "MOVN keeps rd's upper doubleword");
        });

        tc.Run("the traps compare the doublewords", [](TestCase &t) {
            CodeGenerator gen({}, {});
            const std::string tge = gen.translateInstruction(decode(rtype(SPECIAL_TGE, 4, 5, 0)));
            const std::string tltu = gen.translateInstruction(decode(rtype(SPECIAL_TLTU, 4, 5, 0)));
            const std::string teqi = gen.translateInstruction(decode(regimm(REGIMM_TEQI, 4, 1)));
            t.IsTrue(tge.find("GPR_S64(ctx, 4) >= GPR_S64(ctx, 5)") != std::string::npos, "TGE: " + tge);
            t.IsTrue(tltu.find("GPR_U64(ctx, 4) < GPR_U64(ctx, 5)") != std::string::npos, "TLTU: " + tltu);
            t.IsTrue(teqi.find("GPR_S64(ctx, 4) == (int64_t)(int32_t)1") != std::string::npos, "TEQI: " + teqi);
        });

        tc.Run("BC1F and BC1T test the FPU condition bit 23", [](TestCase &t) {
            const uint32_t bc1f = (OPCODE_COP1 << 26) | (COP1_BC << 21) | (COP1_BC_BCF << 16) | 2u;
            const uint32_t bc1tl = (OPCODE_COP1 << 26) | (COP1_BC << 21) | (COP1_BC_BCTL << 16) | 2u;
            t.Equals(branchCondition(generateBranch(bc1f)), std::string("!(ctx->fcr31 & 0x800000)"), "BC1F");
            t.Equals(branchCondition(generateBranch(bc1tl)), std::string("(ctx->fcr31 & 0x800000)"), "BC1TL");
        });

        tc.Run("PABSW and PABSH take rt and saturate the most negative lane", [](TestCase &t) {
            CodeGenerator gen({}, {});
            // PABSW rd=1, rt=3 (rs field 0): MMI1 sa=1, function MMI_MMI1.
            const uint32_t pabsw = (OPCODE_MMI << 26) | (0u << 21) | (3u << 16) | (1u << 11) | (MMI1_PABSW << 6) | MMI_MMI1;
            const uint32_t pabsh = (OPCODE_MMI << 26) | (0u << 21) | (3u << 16) | (1u << 11) | (MMI1_PABSH << 6) | MMI_MMI1;
            const std::string w = gen.translateInstruction(decode(pabsw));
            const std::string h = gen.translateInstruction(decode(pabsh));
            t.Equals(w, std::string("SET_GPR_VEC(ctx, 1, PS2_PABSW(GPR_VEC(ctx, 3)));"), "PABSW");
            t.Equals(h, std::string("SET_GPR_VEC(ctx, 1, PS2_PABSH(GPR_VEC(ctx, 3)));"), "PABSH");

            alignas(16) uint32_t words[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(words),
                            PS2_PABSW(_mm_set_epi32(5, -7, INT32_MIN, 0)));
            t.Equals(words[0], 0u, "PABSW 0");
            t.Equals(words[1], 0x7FFFFFFFu, "PABSW of 0x80000000 is 0x7FFFFFFF");
            t.Equals(words[2], 7u, "PABSW -7");
            t.Equals(words[3], 5u, "PABSW 5");
            alignas(16) uint16_t halves[8];
            _mm_store_si128(reinterpret_cast<__m128i *>(halves),
                            PS2_PABSH(_mm_set_epi16(1, -1, INT16_MIN, 0, 2, -2, INT16_MIN, 3)));
            t.Equals(halves[0], static_cast<uint16_t>(3), "PABSH 3");
            t.Equals(halves[1], static_cast<uint16_t>(0x7FFF), "PABSH of 0x8000 is 0x7FFF");
            t.Equals(halves[2], static_cast<uint16_t>(2), "PABSH -2");
            t.Equals(halves[5], static_cast<uint16_t>(0x7FFF), "PABSH of 0x8000 is 0x7FFF (high lanes)");
            t.Equals(halves[6], static_cast<uint16_t>(1), "PABSH -1");
        });
    });
}
