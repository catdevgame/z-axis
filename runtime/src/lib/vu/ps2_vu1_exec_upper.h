#ifndef PS2_VU1_EXEC_UPPER_H
#define PS2_VU1_EXEC_UPPER_H

// Upper (FMAC) instruction execution, inline so that ahead-of-time compiled
// microprograms (ps2_vu1_aot.h) can specialize it for constant instructions.
// Ahead-of-time compiled code (ps2_vu1_aot.h) forces these inline so that
// constant instructions fold away the decoding.
#ifndef VU1_EXEC_INLINE
#define VU1_EXEC_INLINE inline
#endif
#ifndef VU1_FMAC_INLINE
#define VU1_FMAC_INLINE inline
#endif
#include "runtime/ps2_vu1_core.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_step.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#endif
namespace vu1upper
{
    inline int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    // MAX/MINI compare the raw words as sign-magnitude numbers (PCSX2 fp_max /
    // fp_min): no denormal flush or clamping, so integer data passes through
    // unchanged (games copy vertex flag words with MAX.w vfA, vfB, vfB).
    inline float vuMaxBits(float a, float b)
    {
        int32_t ia = 0, ib = 0;
        std::memcpy(&ia, &a, sizeof(ia));
        std::memcpy(&ib, &b, sizeof(ib));
        const int32_t r = (ia < 0 && ib < 0) ? std::min(ia, ib) : std::max(ia, ib);
        float out = 0.0f;
        std::memcpy(&out, &r, sizeof(out));
        return out;
    }

    inline float vuMinBits(float a, float b)
    {
        int32_t ia = 0, ib = 0;
        std::memcpy(&ia, &a, sizeof(ia));
        std::memcpy(&ib, &b, sizeof(ib));
        const int32_t r = (ia < 0 && ib < 0) ? std::max(ia, ib) : std::min(ia, ib);
        float out = 0.0f;
        std::memcpy(&out, &r, sizeof(out));
        return out;
    }
}

namespace vu1upper
{
    inline uint32_t bitsOf(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // Zero, or a magnitude in [2^-60, 2^61).
    inline bool inSafeRange(float value)
    {
        const uint32_t bits = bitsOf(value);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        return (bits & 0x7FFFFFFFu) == 0u || (exponent >= 67u && exponent <= 187u);
    }

    inline uint8_t unsafeLanes(const float *v)
    {
        return static_cast<uint8_t>((inSafeRange(v[0]) ? 0u : 8u) | (inSafeRange(v[1]) ? 0u : 4u) |
                                    (inSafeRange(v[2]) ? 0u : 2u) | (inSafeRange(v[3]) ? 0u : 1u));
    }
}

using namespace vu1upper;

inline bool VU1Core::fmacOperandsInSafeRange(uint8_t index, uint8_t dest, const float *vs, const float *vt,
                                             const float *acc, float q, float i) const
{
    // index: Special1 opcode or Special2 selector; both tables put the
    // arithmetic operations at the same indices.
    uint8_t vsLanes = dest;
    uint8_t vtLanes = 0u;
    uint8_t accLanes = 0u;
    bool usesQ = false;
    bool usesI = false;
    if (index <= 0x0Fu || (index >= 0x18u && index <= 0x1Bu))
    {
        vtLanes = static_cast<uint8_t>(8u >> (index & 3u));
        accLanes = (index >= 0x08u && index <= 0x0Fu) ? dest : 0u;
    }
    else if (index == 0x1Cu || index == 0x1Eu || (index >= 0x20u && index <= 0x27u))
    {
        usesQ = index == 0x1Cu || (index >= 0x20u && index <= 0x27u && (index & 2u) == 0u);
        usesI = !usesQ;
        accLanes = (index == 0x21u || index == 0x23u || index == 0x25u || index == 0x27u) ? dest : 0u;
    }
    else if (index == 0x28u || index == 0x29u || index == 0x2Au || index == 0x2Cu || index == 0x2Du)
    {
        vtLanes = dest;
        accLanes = (index == 0x29u || index == 0x2Du) ? dest : 0u;
    }
    else if (index == 0x2Eu)
    {
        vsLanes = 0xEu;
        vtLanes = 0xEu;
        // OPMSUB reads ACC; OPMULA (Special2) does not, checking it only
        // costs the fast path.
        accLanes = dest;
    }
    else
    {
        return false;
    }
    if ((unsafeLanes(vs) & vsLanes) != 0u || (unsafeLanes(vt) & vtLanes) != 0u ||
        (unsafeLanes(acc) & accLanes) != 0u)
        return false;
    if (usesQ && !inSafeRange(q))
        return false;
    if (usesI && !inSafeRange(i))
        return false;
    return true;
}

inline VU1Core::FmacPlan VU1Core::planFmac(uint32_t upper)
{
    FmacPlan plan;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const bool special = op >= 0x3Cu;
    const uint8_t index = special ? static_cast<uint8_t>((upper & 0x3u) | ((upper >> 4) & 0x7Cu)) : op;
    plan.toAcc = special;
    plan.fs = FS(upper);
    plan.ft = FT(upper);
    plan.fd = FD(upper);
    plan.dest = DEST(upper);
    if (index <= 0x0Fu || (index >= 0x18u && index <= 0x1Bu))
    {
        static constexpr FmacPlan::Kind kinds[4] = {FmacPlan::Add, FmacPlan::Sub, FmacPlan::Madd, FmacPlan::Msub};
        plan.kind = index >= 0x18u ? FmacPlan::Mul : kinds[index >> 2];
        plan.source = FmacPlan::Broadcast;
        plan.bcLane = static_cast<uint8_t>(index & 3u);
    }
    else if (index == 0x1Cu || index == 0x1Eu)
    {
        plan.kind = FmacPlan::Mul;
        plan.source = index == 0x1Cu ? FmacPlan::QReg : FmacPlan::IReg;
    }
    else if (index >= 0x20u && index <= 0x27u)
    {
        static constexpr FmacPlan::Kind kinds[4] = {FmacPlan::Add, FmacPlan::Madd, FmacPlan::Sub, FmacPlan::Msub};
        plan.kind = kinds[((index >> 2) & 1u) * 2u + (index & 1u)];
        plan.source = (index & 2u) ? FmacPlan::IReg : FmacPlan::QReg;
    }
    else if (index == 0x28u || index == 0x29u || index == 0x2Au || index == 0x2Cu || index == 0x2Du)
    {
        static constexpr FmacPlan::Kind kinds[6] = {FmacPlan::Add, FmacPlan::Madd, FmacPlan::Mul,
                                                   FmacPlan::None, FmacPlan::Sub, FmacPlan::Msub};
        plan.kind = kinds[index - 0x28u];
        plan.source = FmacPlan::Vector;
    }
    else if (index == 0x2Eu)
    {
        plan.kind = special ? FmacPlan::Opmula : FmacPlan::Opmsub;
        plan.source = FmacPlan::Vector;
    }
    if (plan.kind == FmacPlan::None || plan.dest == 0u)
    {
        plan.kind = FmacPlan::None;
        return plan;
    }

    const bool cross = plan.kind == FmacPlan::Opmula || plan.kind == FmacPlan::Opmsub;
    plan.vsLanes = cross ? 0xEu : plan.dest;
    if (plan.source == FmacPlan::Broadcast)
        plan.vtLanes = static_cast<uint8_t>(8u >> plan.bcLane);
    else if (plan.source == FmacPlan::Vector)
        plan.vtLanes = cross ? 0xEu : plan.dest;
    if (plan.kind == FmacPlan::Madd || plan.kind == FmacPlan::Msub || plan.kind == FmacPlan::Opmsub)
        plan.accLanes = plan.dest;
    return plan;
}

// The common case of execUpper for FMAC arithmetic. It only handles
// operands that are zero or in [2^-60, 2^61) and results that are normal
// non-zero numbers, where the result needs no clamping and the flags are the
// result signs (plus the factors' signs and zeros as sticky flags for
// product-sum forms); anything else returns false before touching state and
// goes through execUpper. Results use the same expressions as execUpper.
VU1_EXEC_INLINE bool VU1Core::execFmacFast(const FmacPlan &plan)
{
    const float *vs = m_state.vf[plan.fs];
    const float *vt = m_state.vf[plan.ft];
    const float *acc = m_state.acc;
    if ((unsafeLanes(vs) & plan.vsLanes) != 0u || (unsafeLanes(vt) & plan.vtLanes) != 0u ||
        (unsafeLanes(acc) & plan.accLanes) != 0u)
        return false;
    float b = 0.0f;
    if (plan.source == FmacPlan::Broadcast)
        b = vt[plan.bcLane];
    else if (plan.source == FmacPlan::QReg)
        b = m_state.q;
    else if (plan.source == FmacPlan::IReg)
        b = m_state.i;
    if (plan.source != FmacPlan::Vector && plan.source != FmacPlan::Broadcast && !inSafeRange(b))
        return false;

    float result[4];
    const bool vector = plan.source == FmacPlan::Vector;
    switch (plan.kind)
    {
    case FmacPlan::Add:
        if (vector)
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
        else
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + b;
        break;
    case FmacPlan::Sub:
        if (vector)
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
        else
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - b;
        break;
    case FmacPlan::Mul:
        if (vector)
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
        else
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * b;
        break;
    case FmacPlan::Madd:
        if (vector)
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * vt[c];
        else
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * b;
        break;
    case FmacPlan::Msub:
        if (vector)
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * vt[c];
        else
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * b;
        break;
    case FmacPlan::Opmula:
        result[0] = vs[1] * vt[2];
        result[1] = vs[2] * vt[0];
        result[2] = vs[0] * vt[1];
        result[3] = 0.0f;
        break;
    case FmacPlan::Opmsub:
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        break;
    default:
        return false;
    }

    uint32_t mac = 0u;
    uint32_t extraSticky = 0u;
    const bool productSum = plan.kind == FmacPlan::Madd || plan.kind == FmacPlan::Msub || plan.kind == FmacPlan::Opmsub;
    static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
    static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint32_t lane = 8u >> component;
        if ((plan.dest & lane) == 0u)
            continue;
        const uint32_t bits = bitsOf(result[component]);
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u || exponent == 0x7F800000u)
            return false; // zero or out of range: exact flags needed
        mac |= (bits >> 31) * (lane << 4);
        if (productSum)
        {
            const bool cross = plan.kind == FmacPlan::Opmsub;
            const uint32_t left = bitsOf(vs[cross ? crossLeft[component] : component]);
            const uint32_t right = bitsOf(vector ? vt[cross ? crossRight[component] : component] : b);
            extraSticky |= ((left ^ right) >> 30) & 0x2u;
            if ((left & 0x7FFFFFFFu) == 0u || (right & 0x7FFFFFFFu) == 0u)
                extraSticky |= 0x1u;
        }
    }

    float *target = plan.toAcc ? m_state.acc : m_state.vf[plan.fd];
    if (plan.dest & 0x8u)
        target[0] = result[0];
    if (plan.dest & 0x4u)
        target[1] = result[1];
    if (plan.dest & 0x2u)
        target[2] = result[2];
    if (plan.dest & 0x1u)
        target[3] = result[3];

    queueMacStatus(mac, (mac & 0xF0u) != 0u ? 0x2u : 0u, extraSticky);
    return true;
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
VU1_EXEC_INLINE void VU1Core::execUpper(uint32_t instr)
{
    m_currentUpperInstruction = instr;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    // MAX/MINI, ITOF and CLIP read the raw registers: skip the operand
    // normalization for them.
    const uint8_t index = op < 0x3Cu ? op : static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
    const bool rawOperands = op < 0x3Cu ? ((op >= 0x10u && op <= 0x17u) || op == 0x1Du || op == 0x1Fu || op == 0x2Bu || op == 0x2Fu)
                                        : ((index >= 0x10u && index <= 0x13u) || index == 0x1Fu);
    float normalizedVs[4]{};
    float normalizedVt[4]{};
    float normalizedAcc[4]{};
    float q = 0.0f;
    float i = 0.0f;
    if (!rawOperands)
    {
        normalizeOperands(m_state.vf[fs], normalizedVs);
        normalizeOperands(m_state.vf[ft], normalizedVt);
        normalizeOperands(m_state.acc, normalizedAcc);
        q = normalizeOperand(m_state.q);
        i = normalizeOperand(m_state.i);
        // FMAC operations whose operands all lie in [2^-60, 2^61) or are
        // zero cannot overflow or underflow, so their flags follow the sign
        // of the float result (see normalizeFmacResult).
        m_fmacFastFlags = fmacOperandsInSafeRange(index, dest, normalizedVs, normalizedVt, normalizedAcc, q, i);
    }
    const float *vs = normalizedVs;
    const float *vt = normalizedVt;
    const float *acc = normalizedAcc;
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        const float bc = m_state.vf[ft][op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vuMaxBits(m_state.vf[fs][c], bc);
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        const float bc = m_state.vf[ft][op & 3];
        for (int c = 0; c < 4; c++)
            result[c] = vuMinBits(m_state.vf[fs][c], bc);
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = vuMaxBits(m_state.vf[fs][c], m_state.i);
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = vuMinBits(m_state.vf[fs][c], m_state.i);
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = vuMaxBits(m_state.vf[fs][c], m_state.vf[ft][c]);
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = vuMinBits(m_state.vf[fs][c], m_state.vf[ft][c]);
        applyDest(vd, result, dest);
        return;

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = m_state.vf[ft];

        switch (specialOp)
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03: // ADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07: // SUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: // MADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F: // MSUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x10: // ITOF0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x11: // ITOF4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 16.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x12: // ITOF12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 4096.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x13: // ITOF15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 32768.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x14: // FTOI0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 1.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x15: // FTOI4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 16.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x16: // FTOI12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 4096.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x17: // FTOI15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 32768.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B: // MULAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1C: // MULAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(vtDest, result, dest);
            return;
        case 0x1E: // MULAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

            const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };

            uint32_t flags = 0u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
                flags |= 0x01u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
                flags |= 0x02u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
                flags |= 0x04u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
                flags |= 0x08u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
                flags |= 0x10u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
                flags |= 0x20u;
            queueClip(flags);
            return;
        }
        case 0x20: // ADDAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = 0.0f;
            applyFmacDestAcc(result, dest);
            return;
        case 0x2F:
        case 0x30: // NOP
            return;
        default:
            reportReservedInstruction(true, instr);
            return;
        }
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        reportReservedInstruction(true, instr);
        return;
    }
}


namespace vu1upper
{
#if defined(__aarch64__)
    // All-ones in the lanes of a dest mask (x = 8 ... w = 1).
    constexpr uint32_t laneOnes(uint32_t dest, uint32_t lane) { return (dest & (8u >> lane)) ? 0xFFFFFFFFu : 0u; }
    template <int Dest>
    inline uint32x4_t laneMask()
    {
        const uint32_t m[4] = {laneOnes(Dest, 0), laneOnes(Dest, 1), laneOnes(Dest, 2), laneOnes(Dest, 3)};
        return vld1q_u32(m);
    }
    // Lanes that are not zero and not in [2^-60, 2^61) (inSafeRange).
    inline uint32x4_t unsafeLaneBits(uint32x4_t bits)
    {
        const uint32x4_t magnitude = vandq_u32(bits, vdupq_n_u32(0x7FFFFFFFu));
        const uint32x4_t exponent = vandq_u32(vshrq_n_u32(bits, 23), vdupq_n_u32(0xFFu));
        const uint32x4_t inRange = vcleq_u32(vsubq_u32(exponent, vdupq_n_u32(67u)), vdupq_n_u32(120u));
        const uint32x4_t zero = vceqq_u32(magnitude, vdupq_n_u32(0u));
        return vmvnq_u32(vorrq_u32(inRange, zero));
    }
    inline bool anyLane(uint32x4_t v) { return vmaxvq_u32(v) != 0u; }
#endif
}

#if defined(__aarch64__)
namespace vu1upper
{
    // Sticky S/Z of the products of a product-sum (factors in range, see
    // calculateFmacProductSticky).
    inline uint32_t productSticky(uint32x4_t left, uint32x4_t right, uint32x4_t destMask)
    {
        uint32_t extraSticky = 0u;
        if (anyLane(vandq_u32(vshrq_n_u32(veorq_u32(left, right), 31), destMask)))
            extraSticky |= 0x2u;
        const uint32x4_t magnitudeMask = vdupq_n_u32(0x7FFFFFFFu);
        const uint32x4_t zeroFactor = vorrq_u32(vceqq_u32(vandq_u32(left, magnitudeMask), vdupq_n_u32(0u)),
                                                vceqq_u32(vandq_u32(right, magnitudeMask), vdupq_n_u32(0u)));
        if (anyLane(vandq_u32(zeroFactor, destMask)))
            extraSticky |= 0x1u;
        return extraSticky;
    }
}
#endif

#if !defined(__aarch64__) && defined(__SSE4_1__)
namespace vu1upper
{
    // The NEON helpers above with SSE4.1 (x86-64).
    constexpr uint32_t laneOnesSse(uint32_t dest, uint32_t lane) { return (dest & (8u >> lane)) ? 0xFFFFFFFFu : 0u; }
    template <int Dest>
    inline __m128i laneMaskSse()
    {
        return _mm_setr_epi32(static_cast<int>(laneOnesSse(Dest, 0)), static_cast<int>(laneOnesSse(Dest, 1)),
                              static_cast<int>(laneOnesSse(Dest, 2)), static_cast<int>(laneOnesSse(Dest, 3)));
    }
    inline __m128i unsafeLaneBitsSse(__m128i bits)
    {
        const __m128i magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i exponent = _mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(0xFF));
        const __m128i biased = _mm_sub_epi32(exponent, _mm_set1_epi32(67));
        // Unsigned biased <= 120.
        const __m128i inRange = _mm_cmpeq_epi32(_mm_min_epu32(biased, _mm_set1_epi32(120)), biased);
        const __m128i zero = _mm_cmpeq_epi32(magnitude, _mm_setzero_si128());
        return _mm_andnot_si128(_mm_or_si128(inRange, zero), _mm_set1_epi32(-1));
    }
    inline bool anyLaneSse(__m128i v) { return _mm_testz_si128(v, v) == 0; }
    // Movemask bits (x = bit 0) of a dest mask (x = 8).
    constexpr int destMoveMaskSse(int dest)
    {
        return ((dest >> 3) & 1) | ((dest >> 1) & 2) | ((dest << 1) & 4) | ((dest << 3) & 8);
    }
    // _mm_blend_epi16 immediate for the dest lanes (two 16-bit halves each).
    constexpr int blendMaskSse(int dest)
    {
        return ((dest & 8) ? 0x03 : 0) | ((dest & 4) ? 0x0C : 0) | ((dest & 2) ? 0x30 : 0) | ((dest & 1) ? 0xC0 : 0);
    }
    // Lanes outside zero and [2^-60, 2^61) as movemask bits.
    inline int unsafeMoveMaskSse(__m128i bits)
    {
        const __m128i magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i low = _mm_cmpgt_epi32(_mm_set1_epi32(67 << 23), magnitude);   // below 2^-60
        const __m128i high = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32((188 << 23) - 1)); // 2^61 and up
        const __m128i zero = _mm_cmpeq_epi32(magnitude, _mm_setzero_si128());
        return _mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(zero, _mm_or_si128(low, high))));
    }
    // MAC sign and zero bits of movemask lanes: x -> 0x80 / 0x8, ..., w -> 0x10 / 0x1.
    inline constexpr uint16_t kMacSignsSse[16] = {0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
                                                  0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0};
    inline constexpr uint16_t kMacZerosSse[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                                  0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
    inline uint32_t productStickySse(__m128i left, __m128i right, __m128i destMask)
    {
        uint32_t extraSticky = 0u;
        if (anyLaneSse(_mm_and_si128(_mm_srli_epi32(_mm_xor_si128(left, right), 31), destMask)))
            extraSticky |= 0x2u;
        const __m128i magnitudeMask = _mm_set1_epi32(0x7FFFFFFF);
        const __m128i zeroFactor = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(left, magnitudeMask), _mm_setzero_si128()),
                                                _mm_cmpeq_epi32(_mm_and_si128(right, magnitudeMask), _mm_setzero_si128()));
        if (anyLaneSse(_mm_and_si128(zeroFactor, destMask)))
            extraSticky |= 0x1u;
        return extraSticky;
    }
}
#endif

template <int Mode>
VU1_FMAC_INLINE bool VU1Core::fmacFlagResult(VU1Core &vu, uint32_t *sticky, uint32_t mac, uint32_t extraSticky)
{
    if constexpr (Mode == FlagDefer)
    {
        *sticky |= mac | (extraSticky << 16);
        return true;
    }
    else if constexpr (Mode == FlagFold)
    {
        const uint32_t collected = deferredSticky(*sticky);
        *sticky = 0u;
        return vu.queueFmacFlags(mac, extraSticky | collected);
    }
    else
        return vu.queueFmacFlags(mac, extraSticky);
}

template <int Kind, int Source, int Dest, bool ToAcc>
VU1_FMAC_INLINE bool VU1Core::fmacSpecialized(VU1Core &vu, const FmacPlan &plan)
{
    return fmacSpecializedImpl<Kind, Source, Dest, ToAcc, FlagQueue>(vu, plan, nullptr);
}

template <int Kind, int Source, int Dest, bool ToAcc>
VU1_FMAC_INLINE bool VU1Core::fmacSpecializedDefer(VU1Core &vu, const FmacPlan &plan, uint32_t &sticky)
{
    return fmacSpecializedImpl<Kind, Source, Dest, ToAcc, FlagDefer>(vu, plan, &sticky);
}

template <int Kind, int Source, int Dest, bool ToAcc>
VU1_FMAC_INLINE bool VU1Core::fmacSpecializedFold(VU1Core &vu, const FmacPlan &plan, uint32_t &sticky)
{
    return fmacSpecializedImpl<Kind, Source, Dest, ToAcc, FlagFold>(vu, plan, &sticky);
}

template <int Kind, int Source, int Dest, bool ToAcc, int Mode>
VU1_FMAC_INLINE bool VU1Core::fmacSpecializedImpl(VU1Core &vu, const FmacPlan &plan, uint32_t *sticky)
{
#if defined(__aarch64__)
    constexpr bool cross = Kind == FmacPlan::Opmula || Kind == FmacPlan::Opmsub;
    if constexpr (Kind == FmacPlan::None || Dest == 0 || (cross && Source != FmacPlan::Vector))
    {
        return vu.execFmacFast(plan);
    }
    else
    {
        constexpr int vsLanes = cross ? 0xE : Dest;
        constexpr bool productSum = Kind == FmacPlan::Madd || Kind == FmacPlan::Msub || Kind == FmacPlan::Opmsub;
        const uint32x4_t vsBits = vld1q_u32(reinterpret_cast<const uint32_t *>(vu.m_state.vf[plan.fs]));
        const uint32x4_t vtBits = vld1q_u32(reinterpret_cast<const uint32_t *>(vu.m_state.vf[plan.ft]));
        const uint32x4_t accBits = vld1q_u32(reinterpret_cast<const uint32_t *>(vu.m_state.acc));
        if (anyLane(vandq_u32(unsafeLaneBits(vsBits), laneMask<vsLanes>())))
            return false;
        if constexpr (Source == FmacPlan::Vector)
        {
            if (anyLane(vandq_u32(unsafeLaneBits(vtBits), laneMask<cross ? 0xE : Dest>())))
                return false;
        }
        if constexpr (productSum)
        {
            if (anyLane(vandq_u32(unsafeLaneBits(accBits), laneMask<Dest>())))
                return false;
        }
        float32x4_t b;
        if constexpr (Source == FmacPlan::Vector)
            b = vreinterpretq_f32_u32(vtBits);
        else
        {
            float scalar;
            if constexpr (Source == FmacPlan::Broadcast)
                scalar = vu.m_state.vf[plan.ft][plan.bcLane];
            else if constexpr (Source == FmacPlan::QReg)
                scalar = vu.m_state.q;
            else
                scalar = vu.m_state.i;
            if (!inSafeRange(scalar))
                return false;
            b = vdupq_n_f32(scalar);
        }

        float32x4_t a = vreinterpretq_f32_u32(vsBits);
        if constexpr (cross)
        {
            a = __builtin_shufflevector(a, a, 1, 2, 0, 3);
            b = __builtin_shufflevector(b, b, 2, 0, 1, 3);
        }
        const float32x4_t acc = vreinterpretq_f32_u32(accBits);
        float32x4_t result;
        if constexpr (Kind == FmacPlan::Add)
            result = vaddq_f32(a, b);
        else if constexpr (Kind == FmacPlan::Sub)
            result = vsubq_f32(a, b);
        else if constexpr (Kind == FmacPlan::Mul || Kind == FmacPlan::Opmula)
            result = vmulq_f32(a, b);
        else if constexpr (Kind == FmacPlan::Madd)
            result = vaddq_f32(acc, vmulq_f32(a, b));
        else
            result = vsubq_f32(acc, vmulq_f32(a, b));
        if constexpr (cross)
            result = vsetq_lane_f32(0.0f, result, 3);

        const uint32x4_t destMask = laneMask<Dest>();
        const uint32x4_t resultBits = vreinterpretq_u32_f32(result);
        const uint32x4_t exponent = vandq_u32(resultBits, vdupq_n_u32(0x7F800000u));
        const uint32x4_t badResult = vorrq_u32(vceqq_u32(exponent, vdupq_n_u32(0u)),
                                               vceqq_u32(exponent, vdupq_n_u32(0x7F800000u)));
        if (anyLane(vandq_u32(badResult, destMask)))
        {
            // Out of range: the exact path. Zero (or denormal) lanes: their
            // exact values and flags, out of line.
            if (anyLane(vandq_u32(vceqq_u32(exponent, vdupq_n_u32(0x7F800000u)), destMask)))
                return false;
            if constexpr (!productSum)
            {
                // ADD/SUB/MUL of operands in range: a result without an
                // exponent is an exact zero (never a denormal), its flags
                // are Z and its sign (finishFmacWithZeros gives the same).
                const uint32x4_t zero = vandq_u32(vceqq_u32(vandq_u32(resultBits, vdupq_n_u32(0x7FFFFFFFu)), vdupq_n_u32(0u)), destMask);
                if (!anyLane(vandq_u32(veorq_u32(badResult, zero), destMask)))
                {
                    const uint32_t signWeights[4] = {0x80u, 0x40u, 0x20u, 0x10u};
                    const uint32_t zeroWeights[4] = {0x8u, 0x4u, 0x2u, 0x1u};
                    const uint32_t mac = vaddvq_u32(vandq_u32(vmulq_u32(vshrq_n_u32(resultBits, 31), vld1q_u32(signWeights)), destMask)) |
                                         vaddvq_u32(vandq_u32(zero, vld1q_u32(zeroWeights)));
                    uint32_t *target = reinterpret_cast<uint32_t *>(ToAcc ? vu.m_state.acc : vu.m_state.vf[plan.fd]);
                    vst1q_u32(target, vbslq_u32(destMask, resultBits, vld1q_u32(target)));
                    return fmacFlagResult<Mode>(vu, sticky, mac, 0u);
                }
            }
            uint32_t extraSticky = 0u;
            if constexpr (productSum)
                extraSticky = productSticky(vreinterpretq_u32_f32(a), vreinterpretq_u32_f32(b), destMask);
            float aLanes[4], bLanes[4], accLanes[4];
            uint32_t resultLanes[4];
            vst1q_f32(aLanes, a);
            vst1q_f32(bLanes, b);
            vst1q_f32(accLanes, acc);
            vst1q_u32(resultLanes, resultBits);
            return vu.finishFmacWithZeros(static_cast<uint8_t>(Kind), static_cast<uint8_t>(Dest), ToAcc, plan.fd,
                                          aLanes, bLanes, accLanes, resultLanes, extraSticky);
        }

        const uint32_t signWeights[4] = {0x80u, 0x40u, 0x20u, 0x10u};
        const uint32_t mac = vaddvq_u32(vandq_u32(vmulq_u32(vshrq_n_u32(resultBits, 31), vld1q_u32(signWeights)), destMask));
        uint32_t extraSticky = 0u;
        if constexpr (productSum)
            extraSticky = productSticky(vreinterpretq_u32_f32(a), vreinterpretq_u32_f32(b), destMask);

        uint32_t *target = reinterpret_cast<uint32_t *>(ToAcc ? vu.m_state.acc : vu.m_state.vf[plan.fd]);
        vst1q_u32(target, vbslq_u32(destMask, resultBits, vld1q_u32(target)));

        return fmacFlagResult<Mode>(vu, sticky, mac, extraSticky);
    }
#elif defined(__SSE4_1__)
    // The NEON path above with SSE4.1: the same lanes, checks and results,
    // with lane masks as movemask bits (x = bit 0) and one branch for all
    // operand checks.
    constexpr bool cross = Kind == FmacPlan::Opmula || Kind == FmacPlan::Opmsub;
    if constexpr (Kind == FmacPlan::None || Dest == 0 || (cross && Source != FmacPlan::Vector))
    {
        return vu.execFmacFast(plan);
    }
    else
    {
        constexpr int vsLanes = cross ? 0xE : Dest;
        constexpr bool productSum = Kind == FmacPlan::Madd || Kind == FmacPlan::Msub || Kind == FmacPlan::Opmsub;
        constexpr int destBits = destMoveMaskSse(Dest);
        const __m128i vsBits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.fs]));
        const __m128i vtBits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.ft]));
        const __m128i accBits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.acc));
        int unsafe = unsafeMoveMaskSse(vsBits) & destMoveMaskSse(vsLanes);
        if constexpr (Source == FmacPlan::Vector)
            unsafe |= unsafeMoveMaskSse(vtBits) & destMoveMaskSse(cross ? 0xE : Dest);
        if constexpr (productSum)
            unsafe |= unsafeMoveMaskSse(accBits) & destBits;
        __m128 b;
        if constexpr (Source == FmacPlan::Vector)
            b = _mm_castsi128_ps(vtBits);
        else
        {
            float scalar;
            if constexpr (Source == FmacPlan::Broadcast)
                scalar = vu.m_state.vf[plan.ft][plan.bcLane];
            else if constexpr (Source == FmacPlan::QReg)
                scalar = vu.m_state.q;
            else
                scalar = vu.m_state.i;
            unsafe |= inSafeRange(scalar) ? 0 : 1;
            b = _mm_set1_ps(scalar);
        }
        if (unsafe != 0)
            return false;

        __m128 a = _mm_castsi128_ps(vsBits);
        if constexpr (cross)
        {
            a = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)); // y, z, x, w
            b = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2)); // z, x, y, w
        }
        const __m128 acc = _mm_castsi128_ps(accBits);
        __m128 result;
        [[maybe_unused]] __m128 product;
        if constexpr (Kind == FmacPlan::Add)
            result = _mm_add_ps(a, b);
        else if constexpr (Kind == FmacPlan::Sub)
            result = _mm_sub_ps(a, b);
        else if constexpr (Kind == FmacPlan::Mul || Kind == FmacPlan::Opmula)
            result = _mm_mul_ps(a, b);
        else if constexpr (Kind == FmacPlan::Madd)
        {
            product = _mm_mul_ps(a, b);
            result = _mm_add_ps(acc, product);
        }
        else
        {
            product = _mm_mul_ps(a, b);
            result = _mm_sub_ps(acc, product);
        }
        if constexpr (cross)
            result = _mm_blend_ps(result, _mm_setzero_ps(), 0x8);

        const __m128i resultBits = _mm_castps_si128(result);
        const __m128i exponent = _mm_and_si128(resultBits, _mm_set1_epi32(0x7F800000));
        const int infNan = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000)))) & destBits;
        const int noExponent = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()))) & destBits;
        // Sticky S/Z of the products (factors in range: a product is zero
        // only for a zero factor, and its sign is the factors' sign).
        uint32_t extraSticky = 0u;
        if constexpr (productSum && cross)
            extraSticky = productStickySse(_mm_castps_si128(a), _mm_castps_si128(b), laneMaskSse<Dest>()); // w is not range checked
        else if constexpr (productSum)
        {
            const __m128i productBits = _mm_castps_si128(product);
            extraSticky = ((_mm_movemask_ps(product) & destBits) != 0 ? 0x2u : 0u) |
                          ((_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(productBits, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()))) & destBits) != 0 ? 0x1u : 0u);
        }
        __m128i *target = reinterpret_cast<__m128i *>(ToAcc ? vu.m_state.acc : vu.m_state.vf[plan.fd]);
        if ((infNan | noExponent) != 0)
        {
            // Out of range: the exact path. Zero (or denormal) lanes: their
            // exact values and flags, out of line.
            if (infNan != 0)
                return false;
            if constexpr (!productSum)
            {
                // As in the NEON path: exact zeros, flags Z and sign.
                const int zeros = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(resultBits, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()))) & destBits;
                if (zeros == noExponent)
                {
                    const uint32_t mac = kMacSignsSse[_mm_movemask_ps(result) & destBits] | kMacZerosSse[zeros];
                    if constexpr (Dest == 0xF)
                        _mm_storeu_si128(target, resultBits);
                    else
                        _mm_storeu_si128(target, _mm_blend_epi16(_mm_loadu_si128(target), resultBits, blendMaskSse(Dest)));
                    return fmacFlagResult<Mode>(vu, sticky, mac, 0u);
                }
            }
            float aLanes[4], bLanes[4], accLanes[4];
            uint32_t resultLanes[4];
            _mm_storeu_ps(aLanes, a);
            _mm_storeu_ps(bLanes, b);
            _mm_storeu_ps(accLanes, acc);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(resultLanes), resultBits);
            return vu.finishFmacWithZeros(static_cast<uint8_t>(Kind), static_cast<uint8_t>(Dest), ToAcc, plan.fd,
                                          aLanes, bLanes, accLanes, resultLanes, extraSticky);
        }

        if constexpr (Dest == 0xF)
            _mm_storeu_si128(target, resultBits);
        else
            _mm_storeu_si128(target, _mm_blend_epi16(_mm_loadu_si128(target), resultBits, blendMaskSse(Dest)));
        const uint32_t mac = kMacSignsSse[_mm_movemask_ps(result) & destBits];
        return fmacFlagResult<Mode>(vu, sticky, mac, extraSticky);
    }
#else
    return vu.execFmacFast(plan);
#endif
}


template <int Op, int Dest, int Sub>
VU1_FMAC_INLINE bool VU1Core::upperFast(VU1Core &vu, const FmacPlan &plan)
{
#if !defined(__aarch64__) && defined(__SSE4_1__)
    // SSE4.1 versions of the loops below, with the same results.
    if constexpr (Op == UpperFastMaxBc || Op == UpperFastMiniBc || Op == UpperFastMaxI || Op == UpperFastMiniI ||
                  Op == UpperFastMax || Op == UpperFastMini)
    {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.fs]));
        __m128i b;
        if constexpr (Op == UpperFastMaxBc || Op == UpperFastMiniBc)
            b = _mm_set1_epi32(static_cast<int>(bitsOf(vu.m_state.vf[plan.ft][Sub])));
        else if constexpr (Op == UpperFastMaxI || Op == UpperFastMiniI)
            b = _mm_set1_epi32(static_cast<int>(bitsOf(vu.m_state.i)));
        else
            b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.ft]));
        // Sign-magnitude order: both negative -> the integer order reverses.
        const __m128i bothNegative = _mm_srai_epi32(_mm_and_si128(a, b), 31);
        __m128i result;
        if constexpr (Op == UpperFastMaxBc || Op == UpperFastMaxI || Op == UpperFastMax)
            result = _mm_blendv_epi8(_mm_max_epi32(a, b), _mm_min_epi32(a, b), bothNegative);
        else
            result = _mm_blendv_epi8(_mm_min_epi32(a, b), _mm_max_epi32(a, b), bothNegative);
        __m128i *target = reinterpret_cast<__m128i *>(vu.m_state.vf[plan.fd]);
        _mm_storeu_si128(target, _mm_blendv_epi8(_mm_loadu_si128(target), result, laneMaskSse<Dest>()));
        return true;
    }
    else if constexpr (Op == UpperFastItof)
    {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.fs]));
        __m128 result = _mm_cvtepi32_ps(a); // current rounding mode, as the scalar conversion
        if constexpr (Sub != 0)
            result = _mm_mul_ps(result, _mm_set1_ps(Sub == 1 ? 1.0f / 16.0f : (Sub == 2 ? 1.0f / 4096.0f : 1.0f / 32768.0f))); // exact
        __m128i *target = reinterpret_cast<__m128i *>(vu.m_state.vf[plan.ft]);
        _mm_storeu_si128(target, _mm_blendv_epi8(_mm_loadu_si128(target), _mm_castps_si128(result), laneMaskSse<Dest>()));
        return true;
    }
    else if constexpr (Op == UpperFastFtoi)
    {
        constexpr float scale = Sub == 0 ? 1.0f : (Sub == 1 ? 16.0f : (Sub == 2 ? 4096.0f : 32768.0f));
        __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu.m_state.vf[plan.fs]));
        // normalizeOperand: no exponent -> signed zero, exponent 255 -> signed max.
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        bits = _mm_blendv_epi8(bits, sign, _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
        bits = _mm_blendv_epi8(bits, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000)));
        // Scaling by a power of two is exact (overflow becomes infinity);
        // truncation, INT_MIN for every negative overflow, INT_MAX for the
        // positive ones (vuFloatToInt clamps the same way).
        const __m128 scaled = _mm_mul_ps(_mm_castsi128_ps(bits), _mm_set1_ps(scale));
        __m128i result = _mm_cvttps_epi32(scaled);
        result = _mm_blendv_epi8(result, _mm_set1_epi32(0x7FFFFFFF), _mm_castps_si128(_mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f))));
        __m128i *target = reinterpret_cast<__m128i *>(vu.m_state.vf[plan.ft]);
        _mm_storeu_si128(target, _mm_blendv_epi8(_mm_loadu_si128(target), result, laneMaskSse<Dest>()));
        return true;
    }
#endif
    // The same expressions as execUpper for these instructions.
    [[maybe_unused]] float result[4];
    [[maybe_unused]] const float *vs = vu.m_state.vf[plan.fs];
    if constexpr (Op == UpperFastMaxBc || Op == UpperFastMiniBc || Op == UpperFastMaxI || Op == UpperFastMiniI ||
                  Op == UpperFastMax || Op == UpperFastMini)
    {
        for (int c = 0; c < 4; c++)
        {
            float b;
            if constexpr (Op == UpperFastMaxBc || Op == UpperFastMiniBc)
                b = vu.m_state.vf[plan.ft][Sub];
            else if constexpr (Op == UpperFastMaxI || Op == UpperFastMiniI)
                b = vu.m_state.i;
            else
                b = vu.m_state.vf[plan.ft][c];
            if constexpr (Op == UpperFastMaxBc || Op == UpperFastMaxI || Op == UpperFastMax)
                result[c] = vuMaxBits(vs[c], b);
            else
                result[c] = vuMinBits(vs[c], b);
        }
        vu.applyDest(vu.m_state.vf[plan.fd], result, static_cast<uint8_t>(Dest));
        return true;
    }
    else if constexpr (Op == UpperFastItof)
    {
        for (int c = 0; c < 4; c++)
        {
            int32_t iv;
            std::memcpy(&iv, &vs[c], 4);
            if constexpr (Sub == 0)
                result[c] = static_cast<float>(iv);
            else if constexpr (Sub == 1)
                result[c] = static_cast<float>(iv) / 16.0f;
            else if constexpr (Sub == 2)
                result[c] = static_cast<float>(iv) / 4096.0f;
            else
                result[c] = static_cast<float>(iv) / 32768.0f;
        }
        vu.applyDest(vu.m_state.vf[plan.ft], result, static_cast<uint8_t>(Dest));
        return true;
    }
    else if constexpr (Op == UpperFastFtoi)
    {
        constexpr float scale = Sub == 0 ? 1.0f : (Sub == 1 ? 16.0f : (Sub == 2 ? 4096.0f : 32768.0f));
        for (int c = 0; c < 4; c++)
        {
            const int32_t iv = vuFloatToInt(vu.normalizeOperand(vs[c]), scale);
            std::memcpy(&result[c], &iv, 4);
        }
        vu.applyDest(vu.m_state.vf[plan.ft], result, static_cast<uint8_t>(Dest));
        return true;
    }
    else if constexpr (Op == UpperFastAbs)
    {
        for (int c = 0; c < 4; c++)
            result[c] = std::fabs(vu.normalizeOperand(vs[c]));
        vu.applyDest(vu.m_state.vf[plan.ft], result, static_cast<uint8_t>(Dest));
        return true;
    }
    else if constexpr (Op == UpperFastClip)
    {
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &vu.m_state.vf[plan.ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t orderedBits = 0;
            std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
            return orderedBits > limit;
        };
        uint32_t flags = 0u;
        if (exceedsClipPlane(vs[0], 0x00000000u))
            flags |= 0x01u;
        if (exceedsClipPlane(vs[0], 0x80000000u))
            flags |= 0x02u;
        if (exceedsClipPlane(vs[1], 0x00000000u))
            flags |= 0x04u;
        if (exceedsClipPlane(vs[1], 0x80000000u))
            flags |= 0x08u;
        if (exceedsClipPlane(vs[2], 0x00000000u))
            flags |= 0x10u;
        if (exceedsClipPlane(vs[2], 0x80000000u))
            flags |= 0x20u;
        vu.queueClip(flags);
        return true;
    }
    else
    {
        return false; // execUpper
    }
}

#endif
