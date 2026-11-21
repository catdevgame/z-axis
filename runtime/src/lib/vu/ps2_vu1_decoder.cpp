// VU0's and VU1's decoder (runtime/ps2_vu1.h): an instruction pair's usage of registers and pipelines, its hazard
// and write-back data and its specialized handlers, and the static chain analysis of a micro memory image. No state
// of an interpreter's: the interpreter's cache and zx-vu1aot (the generator, which runs on the build machine, also
// for a cross build: runtime/tools/vu1_aot_host) take it as it is.
#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_exec_upper.h"

#include <algorithm>
#include <cstring>
#include <vector>

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < sizeof(usage.vfRead) / sizeof(usage.vfRead[0]))
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

// Static hazard analysis of the decoded micro memory. A write issued at
// cycle c with latency l is ready at c + l, and the pair d places later in
// an unbroken sequence issues at c + d or later, so it cannot wait for that
// write when d >= l. With at least L (the largest write latency) pairs in
// sequence before a pair, only its reads whose last writer is fewer than
// its latency pairs back ("near") can stall. A write must still be marked
// in the ready arrays when a near read uses it, when it may be pending at a
// break of the sequence (taken branch, end of program, halt) within the
// next L pairs, or when the writer itself is not deep in a sequence (the
// interpreter checks that at run time).
uint32_t VU1Interpreter::analyzeChains(DecodedInstructionPair *pairs, uint32_t pairCount)
{
    uint32_t depth = 1;
    for (uint32_t i = 0; i < pairCount; ++i)
    {
        const DecodedInstructionPair &d = pairs[i];
        if (d.upperWriteReg != 0u)
            depth = std::max<uint32_t>(depth, d.upperWriteLatency);
        if (d.lowerWriteReg != 0u)
            depth = std::max<uint32_t>(depth, d.lowerWriteLatency);
        if (d.viWriteReg != 0u)
            depth = std::max<uint32_t>(depth, d.viWriteLatency);
    }
    if (depth > 8u)
        return 0u; // leave every pair on the full checks

    // Latency of pair w's write of VF lane (lane bit) of reg, 0 if none.
    auto vfWriteLatency = [&](uint32_t w, uint8_t reg, uint8_t lane) -> uint32_t
    {
        const DecodedInstructionPair &d = pairs[w];
        // markPairWrites marks lower, then upper: upper wins a shared lane.
        if (d.upperWriteReg == reg && (d.upperWriteLanes & lane) != 0u)
            return d.upperWriteLatency;
        if (d.lowerWriteReg == reg && (d.lowerWriteLanes & lane) != 0u)
            return d.lowerWriteLatency;
        return 0u;
    };
    std::vector<bool> nearWriter(pairCount, false);
    for (uint32_t i = 0; i < pairCount; ++i)
    {
        DecodedInstructionPair &d = pairs[i];
        d.nearVfReadCount = 0;
        std::fill(std::begin(d.nearVfReadReg), std::end(d.nearVfReadReg), uint8_t{0});
        std::fill(std::begin(d.nearVfReadLanes), std::end(d.nearVfReadLanes), uint8_t{0});
        d.nearViReadMask = 0;
        for (uint32_t index = 0; index < d.vfReadCount; ++index)
        {
            const uint8_t reg = d.vfReadReg[index];
            uint8_t nearLanes = 0;
            for (uint8_t lane = 8u; lane != 0u; lane >>= 1)
            {
                if ((d.vfReadLanes[index] & lane) == 0u)
                    continue;
                for (uint32_t k = 1; k <= depth && k <= i; ++k)
                {
                    const uint32_t latency = vfWriteLatency(i - k, reg, lane);
                    if (latency == 0u)
                        continue;
                    if (k < latency)
                    {
                        nearLanes |= lane;
                        nearWriter[i - k] = true;
                    }
                    break;
                }
            }
            if (nearLanes != 0u)
            {
                d.nearVfReadReg[d.nearVfReadCount] = reg;
                d.nearVfReadLanes[d.nearVfReadCount] = nearLanes;
                ++d.nearVfReadCount;
            }
        }
        for (uint32_t mask = d.viReadMask; mask != 0u; mask &= mask - 1u)
        {
            const uint8_t reg = static_cast<uint8_t>(std::countr_zero(mask));
            for (uint32_t k = 1; k <= depth && k <= i; ++k)
            {
                const DecodedInstructionPair &w = pairs[i - k];
                if (w.viWriteReg != reg)
                    continue;
                if (k < w.viWriteLatency)
                {
                    d.nearViReadMask |= static_cast<uint16_t>(1u << reg);
                    nearWriter[i - k] = true;
                }
                break;
            }
        }
    }
    // Pairs after which the next pair may not be the next one in memory.
    std::vector<bool> breakAfter(pairCount, false);
    for (uint32_t p = 0; p < pairCount; ++p)
    {
        const DecodedInstructionPair &d = pairs[p];
        bool brk = d.eBit || d.dBit || d.tBit || p + 1u == pairCount;
        if (p > 0u)
        {
            const DecodedInstructionPair &prev = pairs[p - 1u];
            brk = brk || prev.lowerUsage.pipeline == PipelineBranch || prev.eBit || prev.dBit || prev.tBit;
        }
        breakAfter[p] = brk;
    }
    for (uint32_t j = 0; j < pairCount; ++j)
    {
        DecodedInstructionPair &d = pairs[j];
        bool mark = nearWriter[j];
        for (uint32_t p = j; p < pairCount && p < j + depth && !mark; ++p)
            mark = breakAfter[p];
        d.chainMark = mark;
        d.chainInfo = true;
    }
    return depth;
}


VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper)
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(Unit unit, uint32_t lower)
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        // Selector = (bits 10:6 << 2) | bits 1:0, as in PCSX2's LowerOP_T3
        // tables: ESADD 70, ERSADD 71, ELENG 72, ERLENG 73, EATANxy 74,
        // EATANxz 75, ESUM 76, ESQRT 78, ERSQRT 79, ERCPR 7A, WAITP 7B,
        // ESIN 7C, EATAN 7D, EEXP 7E.
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x79:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7D:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x7C:
            usage.latency = 29u;
            break;
        case 0x7E:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(Unit unit, const uint8_t *vuCode, uint32_t pc)
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(unit, decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }

    for (const InstructionUsage *usage : {&decoded.upperUsage, &decoded.lowerUsage})
    {
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            if (access.reg == 0u || access.lanes == 0u)
                continue;
            uint32_t slot = 0;
            while (slot < decoded.vfReadCount && decoded.vfReadReg[slot] != access.reg)
                ++slot;
            if (slot == decoded.vfReadCount)
            {
                decoded.vfReadReg[slot] = access.reg;
                ++decoded.vfReadCount;
            }
            decoded.vfReadLanes[slot] |= access.lanes;
        }
        decoded.viReadMask |= static_cast<uint16_t>(usage->viRead & 0xFFFEu);
        decoded.accReadLanes |= usage->accRead;
    }
    const InstructionUsage &lower = decoded.lowerUsage;
    if (lower.pipeline == PipelineFdiv)
        decoded.waitMask |= DecodedInstructionPair::WaitFdiv;
    if (lower.pipeline == PipelineEfu)
        decoded.waitMask |= DecodedInstructionPair::WaitEfu;
    if (lower.waitQ)
        decoded.waitMask |= DecodedInstructionPair::WaitQ;
    if (lower.waitP)
        decoded.waitMask |= DecodedInstructionPair::WaitP;
    if (lower.pipeline == PipelineXgkick)
        decoded.waitMask |= DecodedInstructionPair::WaitXgkick;

    const InstructionUsage &upper = decoded.upperUsage;
    if (upper.vfWrite.reg != 0u)
    {
        decoded.upperWriteReg = upper.vfWrite.reg;
        decoded.upperWriteLanes = upper.vfWrite.lanes;
        decoded.upperWriteLatency = upper.vfLatency != 0u ? upper.vfLatency : upper.latency;
    }
    if (lower.vfWrite.reg != 0u && decoded.suppressedLowerVf != lower.vfWrite.reg)
    {
        decoded.lowerWriteReg = lower.vfWrite.reg;
        decoded.lowerWriteLanes = lower.vfWrite.lanes;
        decoded.lowerWriteLatency = lower.vfLatency != 0u ? lower.vfLatency : lower.latency;
    }
    if (lower.viWrite != 0u)
    {
        decoded.viWriteReg = static_cast<uint8_t>(std::countr_zero(lower.viWrite));
        decoded.viWriteLatency = lower.viLatency != 0u ? lower.viLatency : lower.latency;
    }
    decoded.accWriteLanes = upper.accWrite;
    const uint8_t upperOp = static_cast<uint8_t>(decoded.upper & 0x3Fu);
    const uint8_t upperSpecial = static_cast<uint8_t>((decoded.upper & 0x3u) | ((decoded.upper >> 4) & 0x7Cu));
    decoded.upperNop = upperOp >= 0x3Cu && (upperSpecial == 0x2Fu || upperSpecial == 0x30u);
    decoded.lowerNop = !decoded.iBit && (decoded.lower == 0u || decoded.lower == 0x8000033Cu);
    if (!decoded.upperUsage.reserved)
    {
        decoded.fmac = planFmac(decoded.upper);
        if (decoded.fmac.kind != FmacPlan::None)
            decoded.fmacFn = selectFmacFn(decoded.fmac);
        else if (!decoded.upperNop)
            decoded.fmacFn = selectUpperFastFn(decoded.upper);
    }
    if (!decoded.iBit && !decoded.lowerNop)
        decoded.lowerFn = selectLowerFn(decoded.lower);
    return decoded;
}

uint64_t VU1Interpreter::codeHash(const uint8_t *code, uint32_t size)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash ^= code[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}
