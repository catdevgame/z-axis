// The generator of ahead-of-time compiled VU1 microprograms (runtime/ps2_vu1_aot_generator.h, ps2_vu1_aot.h).
#include "runtime/ps2_vu1_aot_generator.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

// The generator's key, set by the build (runtime/CMakeLists.txt). Without it (a compile outside the build: a syntax
// check) it is empty, and code made with this generator is refused (generatorKey).
#if !defined(ZX_VU1_GENERATOR_KEY)
#define ZX_VU1_GENERATOR_KEY ""
#endif

struct VU1AotGenerator
{
    using Interp = VU1Interpreter;
    using Core = VU1Core;

    // Chunk sources per image, always this many (vu1_aot_<hash>_c00.cpp ..., the ones past the image's chunks
    // empty), so the build names them before they are made (runtime/CMakeLists.txt): a chunk is at least 64 of
    // the image's 2048 pairs, the last aside.
    static constexpr uint32_t kChunkFiles = 32u;

    static std::string hex(uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llXu", static_cast<unsigned long long>(value));
        return text;
    }

    template <size_t N>
    static std::string bytes(const uint8_t (&values)[N])
    {
        std::string out = "{";
        for (size_t i = 0; i < N; ++i)
            out += (i ? ", " : "") + std::to_string(values[i]);
        return out + "}";
    }

    static std::string usage(const Interp::InstructionUsage &u)
    {
        std::ostringstream out;
        out << std::boolalpha << "{.vfRead = {{" << int(u.vfRead[0].reg) << ", " << int(u.vfRead[0].lanes) << "}, {"
            << int(u.vfRead[1].reg) << ", " << int(u.vfRead[1].lanes) << "}}"
            << ", .vfWrite = {" << int(u.vfWrite.reg) << ", " << int(u.vfWrite.lanes) << "}"
            << ", .vfReadCount = " << int(u.vfReadCount) << ", .viRead = " << u.viRead
            << ", .viWrite = " << u.viWrite << ", .accRead = " << int(u.accRead)
            << ", .accWrite = " << int(u.accWrite) << ", .latency = " << int(u.latency)
            << ", .vfLatency = " << int(u.vfLatency) << ", .viLatency = " << int(u.viLatency)
            << ", .pipeline = static_cast<VU1Core::Pipeline>(" << int(u.pipeline) << ")"
            << ", .waitQ = " << u.waitQ << ", .waitP = " << u.waitP << ", .readsClip = " << u.readsClip
            << ", .writesClip = " << u.writesClip << ", .delaysNextBranchRead = " << u.delaysNextBranchRead
            << ", .reserved = " << u.reserved << "}";
        return out.str();
    }

    static std::string plan(const Interp::FmacPlan &p)
    {
        std::ostringstream out;
        out << std::boolalpha << "{.kind = static_cast<VU1Core::FmacPlan::Kind>(" << int(p.kind) << ")"
            << ", .source = static_cast<VU1Core::FmacPlan::Source>(" << int(p.source) << ")"
            << ", .toAcc = " << p.toAcc << ", .bcLane = " << int(p.bcLane) << ", .fs = " << int(p.fs)
            << ", .ft = " << int(p.ft) << ", .fd = " << int(p.fd) << ", .dest = " << int(p.dest)
            << ", .vsLanes = " << int(p.vsLanes) << ", .vtLanes = " << int(p.vtLanes)
            << ", .accLanes = " << int(p.accLanes) << "}";
        return out.str();
    }

    static std::string pair(const Interp::DecodedInstructionPair &d)
    {
        std::ostringstream out;
        out << std::boolalpha << "{.lower = " << hex(d.lower) << ", .upper = " << hex(d.upper)
            << ",\n         .lowerUsage = " << usage(d.lowerUsage)
            << ",\n         .upperUsage = " << usage(d.upperUsage)
            << ",\n         .iBit = " << d.iBit << ", .eBit = " << d.eBit << ", .mBit = " << d.mBit
            << ", .dBit = " << d.dBit << ", .tBit = " << d.tBit
            << ", .upperVfShadowReg = " << int(d.upperVfShadowReg)
            << ", .suppressedLowerVf = " << int(d.suppressedLowerVf)
            << ", .vfReadCount = " << int(d.vfReadCount) << ", .vfReadReg = " << bytes(d.vfReadReg)
            << ", .vfReadLanes = " << bytes(d.vfReadLanes) << ", .viReadMask = " << d.viReadMask
            << ", .accReadLanes = " << int(d.accReadLanes) << ", .waitMask = " << int(d.waitMask)
            << ", .upperWriteReg = " << int(d.upperWriteReg) << ", .upperWriteLanes = " << int(d.upperWriteLanes)
            << ", .upperWriteLatency = " << int(d.upperWriteLatency)
            << ", .lowerWriteReg = " << int(d.lowerWriteReg) << ", .lowerWriteLanes = " << int(d.lowerWriteLanes)
            << ", .lowerWriteLatency = " << int(d.lowerWriteLatency)
            << ", .viWriteReg = " << int(d.viWriteReg) << ", .viWriteLatency = " << int(d.viWriteLatency)
            << ", .accWriteLanes = " << int(d.accWriteLanes) << ", .upperNop = " << d.upperNop
            << ", .lowerNop = " << d.lowerNop << ",\n         .fmac = " << plan(d.fmac) << ", .fmacFn = ";
        if (d.fmacFn && d.fmac.kind != Interp::FmacPlan::None)
            out << "&VU1Core::fmacSpecialized<" << int(d.fmac.kind) << ", " << int(d.fmac.source) << ", "
                << int(d.fmac.dest) << ", " << (d.fmac.toAcc ? "true" : "false") << ">";
        else if (d.fmacFn)
        {
            int op = 0, dest = 0, sub = 0;
            VU1Interpreter::upperFastKey(d.upper, op, dest, sub);
            out << "&VU1Core::upperFast<" << op << ", " << dest << ", " << sub << ">";
        }
        else
            out << "nullptr";
        out << ", .lowerFn = ";
        if (d.lowerFn)
        {
            uint32_t opHi = 0, funct = 0, funct2 = 0;
            VU1Interpreter::lowerOpKey(d.lower, opHi, funct, funct2);
            out << "&VU1Core::lowerOp<" << opHi << "u, " << funct << "u, " << funct2 << "u>";
        }
        else
            out << "nullptr";
        out << ",\n         .chainInfo = " << d.chainInfo << ", .chainMark = " << d.chainMark
            << ", .nearVfReadCount = " << int(d.nearVfReadCount) << ", .nearVfReadReg = " << bytes(d.nearVfReadReg)
            << ", .nearVfReadLanes = " << bytes(d.nearVfReadLanes) << ", .nearViReadMask = " << d.nearViReadMask
            << "}";
        return out.str();
    }

    // ------------------------------------------------------------------
    // Blocks: straight-line code for basic blocks (see ps2_vu1_aot.h).

    static bool isBranch(const Interp::DecodedInstructionPair &d)
    {
        return !d.iBit && d.lowerUsage.pipeline == Interp::PipelineBranch;
    }

    // Target of a pc-relative branch (B, BAL, IBxx); false for JR/JALR.
    static bool branchTarget(const Interp::DecodedInstructionPair &d, uint32_t pc, uint32_t &target)
    {
        const uint32_t opHi = (d.lower >> 25) & 0x7Fu;
        if (opHi == 0x24u || opHi == 0x25u)
            return false;
        const int32_t imm = static_cast<int16_t>(static_cast<int32_t>(d.lower << 21) >> 21);
        target = static_cast<uint32_t>(static_cast<int32_t>(pc) + 8 + imm * 8) & 0x3FFFu;
        return true;
    }

    static bool hasWrites(const Interp::DecodedInstructionPair &d)
    {
        return d.lowerWriteReg != 0u || d.upperWriteReg != 0u || d.viWriteReg != 0u || d.accWriteLanes != 0u;
    }

    // Pairs whose step does nothing but pass a cycle.
    static bool idlePair(const Interp::DecodedInstructionPair &d)
    {
        return d.upperNop && d.lowerNop && !d.iBit && !d.eBit && !d.dBit && !d.tBit && d.waitMask == 0u;
    }

    struct BlockPlan
    {
        std::vector<bool> bad;      // never inside a block
        std::vector<bool> leader;   // a block starts here
        std::vector<int32_t> end;   // last pair of the block starting here (-1: none)
        uint32_t maxLatency = 1;
    };

    static BlockPlan planBlocks(const std::vector<Interp::DecodedInstructionPair> &pairs, uint32_t pairCount,
                                const std::vector<uint32_t> &chunkEnd)
    {
        BlockPlan plan;
        plan.bad.assign(pairCount, false);
        plan.leader.assign(pairCount, false);
        plan.end.assign(pairCount, -1);
        for (uint32_t i = 0; i < pairCount; ++i)
        {
            const auto &d = pairs[i];
            if (d.upperWriteReg != 0u)
                plan.maxLatency = std::max<uint32_t>(plan.maxLatency, d.upperWriteLatency);
            if (d.lowerWriteReg != 0u)
                plan.maxLatency = std::max<uint32_t>(plan.maxLatency, d.lowerWriteLatency);
            if (d.viWriteReg != 0u)
                plan.maxLatency = std::max<uint32_t>(plan.maxLatency, d.viWriteLatency);
            bool bad = d.upperUsage.reserved || d.lowerUsage.reserved || d.dBit || d.tBit;
            bad = bad || (d.eBit && isBranch(d));
            if (i > 0u)
            {
                const auto &prev = pairs[i - 1u];
                bad = bad || (isBranch(prev) && (isBranch(d) || d.eBit));
                bad = bad || (prev.eBit && (isBranch(d) || d.eBit));
            }
            plan.bad[i] = bad;
        }
        for (uint32_t i = 0; i < pairCount; ++i)
        {
            const auto &d = pairs[i];
            if ((i > 0u && chunkEnd[i - 1u] == i) || i == 0u || plan.bad[i] || (i > 0u && plan.bad[i - 1u]))
                plan.leader[i] = true;
            uint32_t target = 0;
            if (isBranch(d) && branchTarget(d, i * 8u, target) && target / 8u < pairCount)
                plan.leader[target / 8u] = true;
            // After a branch's delay slot or an E bit's delay slot.
            if ((isBranch(d) || d.eBit) && i + 2u < pairCount)
                plan.leader[i + 2u] = true;
        }
        for (uint32_t s = 0; s < pairCount; ++s)
        {
            if (!plan.leader[s] || plan.bad[s])
                continue;
            int32_t e = -1;
            for (uint32_t i = s; i < chunkEnd[s]; ++i)
            {
                if (plan.bad[i] || (i != s && plan.leader[i] && !(isBranch(pairs[i - 1u]) || pairs[i - 1u].eBit)))
                    break;
                if (isBranch(pairs[i]) || pairs[i].eBit)
                {
                    // The block ends with the delay slot, or before this pair.
                    if (i + 1u < chunkEnd[s] && !plan.bad[i + 1u])
                        e = static_cast<int32_t>(i + 1u);
                    break;
                }
                e = static_cast<int32_t>(i);
            }
            plan.end[s] = e;
        }
        return plan;
    }

    // Lower-bound bookkeeping of the issue cycles of a block: pair k issues
    // at base[seg] + off; base[j + 1] >= base[j] + step[j].
    struct Timing
    {
        std::vector<uint32_t> seg, off;
        std::vector<uint32_t> step; // per segment, to the next
        uint32_t segments = 1;      // s0 = the entry cycle
    };

    static std::string cyc(const Timing &t, uint32_t k)
    {
        std::string out = "s" + std::to_string(t.seg[k]);
        if (t.off[k] != 0u)
            out += " + " + std::to_string(t.off[k]) + "u";
        return out;
    }

    static std::string markList(const std::vector<Interp::DecodedInstructionPair> &pairs, uint32_t first,
                                uint32_t s, uint32_t k, uint32_t latency, const Timing &t, uint32_t &count)
    {
        std::string out;
        count = 0;
        for (uint32_t w = (k > latency ? k - latency : 0u); w < k; ++w)
        {
            if (!hasWrites(pairs[s + w]))
                continue;
            out += (count ? ", " : "") + std::string("{") + std::to_string(s + w - first) + "u, " + cyc(t, w) + "}";
            ++count;
        }
        return out;
    }

    // Leave the block before pair k: z<k> takes `done` (cycles passed so
    // far), `next` (the cycle the previous step ended at) and whether pair
    // k still runs (stop requested during its stall).
    static void emitBail(std::ostringstream &out, const std::vector<Interp::DecodedInstructionPair> &pairs,
                         uint32_t first, uint32_t s, uint32_t k, uint32_t latency, const Timing &t,
                         const std::string &done, const std::string &next, bool stepThenStop)
    {
        uint32_t count = 0;
        const std::string marks = markList(pairs, first, s, k, latency, t, count);
        const uint32_t pc = (s + k) * 8u;
        out << "        {\n";
        if (count)
            out << "            const VU1Core::AotMark m[] = {" << marks << "};\n";
        char line[256];
        std::snprintf(line, sizeof(line), "            vu.aotBlockBail(kPairs, %s, %uu, %uu, 0x%04Xu, %s, %s);\n",
                      count ? "m" : "nullptr", count, k, pc, done.c_str(), next.c_str());
        out << line;
        if (stepThenStop)
        {
            out << "            vu.aotStepPairSlow(kPairs[" << s + k - first << "], rc);\n"
                << "            return true;\n";
        }
        else
        {
            std::snprintf(line, sizeof(line), "            goto p%04X;\n", pc);
            out << line;
        }
        out << "        }\n";
    }

    static void emitBlock(std::ostringstream &out, const std::vector<Interp::DecodedInstructionPair> &pairs,
                          const BlockPlan &plan, uint32_t first, uint32_t end, uint32_t s,
                          const std::function<std::string(uint32_t)> &label)
    {
        const uint32_t e = static_cast<uint32_t>(plan.end[s]);
        const uint32_t n = e - s + 1u;
        const uint32_t L = plan.maxLatency;
        // FDIV and EFU waits last at most their longest latency (RSQRT 13,
        // EATAN 54 cycles): with that much budget left on entry no wait but
        // XGKICK's can reach the end of the budget.
        constexpr uint32_t kMaxScalarWait = 64u;
        uint32_t waitBound = 0;
        for (uint32_t k = 0; k < n; ++k)
            if (pairs[s + k].waitMask & ~Interp::DecodedInstructionPair::WaitXgkick)
                waitBound += kMaxScalarWait;
        char line[512];
        // e<pc>: from the end of another block, where no branch, halt or
        // deferred write can be pending: only the budget and a stop remain.
        const uint32_t margin = 4u * n + 24u + waitBound;
        std::snprintf(line, sizeof(line),
                      "e%04X:\n    if (vu.m_cycle + %uu >= rc.budgetEnd || vu.m_stopRequested)\n        goto p%04X;\n    goto q%04X;\n"
                      "b%04X:\n    if (!vu.aotBlockEntryOk(rc, %uu))\n        goto p%04X;\nq%04X:\n    {\n",
                      s * 8u, margin, s * 8u, s * 8u, s * 8u, margin, s * 8u, s * 8u);
        out << line;
        out << "        const uint64_t s0 = vu.m_cycle;\n        uint64_t ev = vu.aotNextEvent();\n";

        // MAC/status results nothing can see on their own: an FMAC result is
        // only observed by a MAC/status flag instruction, at a wait pair
        // (which may hand over to the per-pair code) or after the block. Up
        // to the last FMAC L at least 4 pairs before such a point (so L is
        // ready there), the FMACs before L only OR their sticky bits into a
        // local that L's result takes over (fmacSpecializedDefer,
        // aotFoldSticky): every observer sees L applied, and with it
        // everything before.
        std::vector<int> flagRole(n, 0); // 1: defer, 2: fold (L)
        {
            auto isFmac = [&](uint32_t k)
            {
                const auto &d = pairs[s + k];
                return d.fmac.kind != Interp::FmacPlan::None && d.fmacFn && !d.upperNop;
            };
            auto observes = [&](uint32_t k)
            {
                const auto &d = pairs[s + k];
                if (d.waitMask & Interp::DecodedInstructionPair::WaitXgkick)
                    return true;
                if (!d.iBit && !d.lowerNop)
                {
                    const uint32_t opHi = (d.lower >> 25) & 0x7Fu;
                    if ((opHi >= 0x14u && opHi <= 0x1Bu) || !d.lowerFn)
                        return true;
                }
                return false;
            };
            uint32_t segStart = 0;
            for (uint32_t k = 0; k <= n; ++k)
            {
                if (k < n && !observes(k))
                    continue;
                std::vector<uint32_t> fmacs;
                for (uint32_t f = segStart; f < k && f + 4u <= k; ++f)
                    if (isFmac(f))
                        fmacs.push_back(f);
                if (fmacs.size() >= 2u)
                {
                    for (size_t i = 0; i + 1u < fmacs.size(); ++i)
                        flagRole[fmacs[i]] = 1;
                    flagRole[fmacs.back()] = 2;
                }
                segStart = k;
            }
        }
        if (std::find(flagRole.begin(), flagRole.end(), 1) != flagRole.end())
            out << "        uint32_t sticky = 0u;\n";
        Timing t;
        t.seg.assign(n, 0);
        t.off.assign(n, 0);
        t.step.assign(1, 0);
        std::string done = "s0"; // cycles passed up to here
        bool recordedPrev = false;
        for (uint32_t k = 0; k < n; ++k)
        {
            const auto &d = pairs[s + k];
            const uint32_t idx = s + k - first;
            const uint32_t pc = (s + k) * 8u;
            std::snprintf(line, sizeof(line), "        // %04X\n", pc);
            out << line;

            // Issue cycle: the previous pair's next cycle, the in-block
            // writers of what it reads, and (first pairs) earlier writers.
            const uint32_t cur = k == 0u ? 0u : t.seg[k - 1u];
            uint32_t best = k == 0u ? 0u : t.off[k - 1u] + 1u;
            std::vector<std::string> dynamic;
            auto addTerm = [&](uint32_t w, uint32_t latency)
            {
                if (k - w >= latency)
                    return; // at least `latency` pairs later
                const uint32_t ws = t.seg[w];
                const uint32_t wo = t.off[w] + latency;
                if (ws == cur)
                {
                    best = std::max(best, wo);
                    return;
                }
                uint32_t delta = 0;
                for (uint32_t j = ws; j < cur; ++j)
                    delta += t.step[j];
                const std::string term = "s" + std::to_string(ws) + " + " + std::to_string(wo) + "u";
                if (wo > delta && wo - delta > best && std::find(dynamic.begin(), dynamic.end(), term) == dynamic.end())
                    dynamic.push_back(term);
            };
            for (uint32_t r = 0; r < d.vfReadCount; ++r)
            {
                const uint8_t reg = d.vfReadReg[r];
                for (uint32_t lane = 0; lane < 4u; ++lane)
                {
                    const uint8_t bit = static_cast<uint8_t>(8u >> lane);
                    if ((d.vfReadLanes[r] & bit) == 0u)
                        continue;
                    bool found = false;
                    for (uint32_t w = k; w-- > 0u && !found;)
                    {
                        const auto &wd = pairs[s + w];
                        if (wd.upperWriteReg == reg && (wd.upperWriteLanes & bit))
                        {
                            addTerm(w, wd.upperWriteLatency);
                            found = true;
                        }
                        else if (wd.lowerWriteReg == reg && (wd.lowerWriteLanes & bit))
                        {
                            addTerm(w, wd.lowerWriteLatency);
                            found = true;
                        }
                    }
                    if (!found && k + 1u < L)
                        dynamic.push_back("vu.m_vfReady[" + std::to_string(reg) + "][" + std::to_string(lane) + "]");
                }
            }
            for (uint32_t mask = d.viReadMask; mask != 0u; mask &= mask - 1u)
            {
                const uint32_t reg = static_cast<uint32_t>(std::countr_zero(mask));
                bool found = false;
                for (uint32_t w = k; w-- > 0u && !found;)
                {
                    if (pairs[s + w].viWriteReg == reg)
                    {
                        addTerm(w, pairs[s + w].viWriteLatency);
                        found = true;
                    }
                }
                if (!found && k + 1u < L)
                    dynamic.push_back("vu.m_viReady[" + std::to_string(reg) + "]");
            }
            // ACC: one-cycle forwarding, never a stall in sequence.

            const std::string prevNext = k == 0u ? std::string("s0") : "(" + cyc(t, k - 1u) + " + 1u)";
            const bool waits = d.waitMask != 0u;
            if (waits)
            {
                // The stall loop of stepPair, after the previous step's
                // last cycle.
                const uint32_t seg = t.segments++;
                t.step.push_back(0);
                t.step[cur] = best;
                out << "        uint64_t s" << seg << ";\n        {\n";
                if (k != 0u)
                {
                    out << "            const uint64_t next = " << prevNext << ";\n"
                        << "            if (next >= ev)\n            {\n"
                        << "                vu.aotSyncTo(" << done << ", next);\n"
                        << "                if (vu.m_stopRequested)\n                {\n";
                    std::ostringstream bail;
                    emitBail(bail, pairs, first, s, k, L, t, "next", "next", false);
                    out << bail.str() << "                }\n            }\n            else\n                vu.m_cycle = next;\n";
                }
                std::string ready = "vu.m_cycle";
                std::string terms = "vu.m_cycle";
                if (best > 0u || k != 0u)
                    terms += ", s" + std::to_string(cur) + " + " + std::to_string(best) + "u";
                for (const std::string &term : dynamic)
                    terms += ", " + term;
                out << "            const uint64_t reads = vu1AotMax(" << terms << ");\n"
                    << "            for (uint64_t ready = vu.aotWaitReady(kPairs[" << idx << "], reads); ready > vu.m_cycle;\n"
                    << "                 ready = vu.aotWaitReady(kPairs[" << idx << "], reads))\n            {\n"
                    << "                if (ready + " << 4u * (n - k) + 24u << "u >= rc.budgetEnd)\n";
                {
                    std::ostringstream bail;
                    emitBail(bail, pairs, first, s, k, L, t, "vu.m_cycle", "vu.m_cycle", false);
                    out << bail.str();
                }
                out << "                vu.advanceTo(ready);\n            }\n"
                    << "            if (vu.m_stopRequested)\n";
                {
                    std::ostringstream bail;
                    emitBail(bail, pairs, first, s, k, L, t, "vu.m_cycle", "vu.m_cycle", true);
                    out << bail.str();
                }
                out << "            s" << seg << " = vu.m_cycle;\n        }\n";
                t.seg[k] = seg;
                t.off[k] = 0;
                done = "s" + std::to_string(seg);
            }
            else if (!dynamic.empty())
            {
                const uint32_t seg = t.segments++;
                t.step.push_back(0);
                t.step[cur] = best;
                out << "        const uint64_t s" << seg << " = vu1AotMax(s" << cur << " + " << best << "u";
                for (const std::string &term : dynamic)
                    out << ", " << term;
                out << ");\n";
                t.seg[k] = seg;
                t.off[k] = 0;
            }
            else
            {
                t.seg[k] = cur;
                t.off[k] = best;
            }

            const bool record = d.lowerUsage.viWrite != 0u && d.lowerUsage.delaysNextBranchRead;
            const bool clear = !record && (k == 0u || recordedPrev);
            if (idlePair(d) && !isBranch(d))
            {
                if (clear)
                    out << "        vu.m_viBranchBackupValid = false;\n";
                recordedPrev = record;
                continue;
            }
            const std::string c = cyc(t, k);
            if (!waits)
            {
                out << "        if (" << c << " >= ev) [[unlikely]]\n        {\n"
                    << "            vu.aotSyncTo(" << done << ", " << c << ");\n"
                    << "            ev = vu.aotNextEvent();\n"
                    << "            if (vu.m_stopRequested)\n";
                std::ostringstream bail;
                emitBail(bail, pairs, first, s, k, L, t, c, c, true);
                out << bail.str() << "        }\n";
                done = c;
            }
            out << "        vu.m_cycle = " << c << ";\n";
            if (isBranch(d))
            {
                std::snprintf(line, sizeof(line), "        vu.m_state.pc = 0x%04Xu;\n", pc);
                out << line;
            }
            const uint8_t viReg = d.lowerUsage.viWrite != 0u ? static_cast<uint8_t>(std::countr_zero(d.lowerUsage.viWrite)) : 0u;
            if (record)
                out << "        const int32_t old" << k << " = vu.m_state.vi[" << int(viReg) << "];\n";
            if (flagRole[k] != 0)
                out << "        vu.execPairOps<true, &VU1Core::fmacSpecialized" << (flagRole[k] == 1 ? "Defer" : "Fold") << "<" << int(d.fmac.kind) << ", "
                    << int(d.fmac.source) << ", " << int(d.fmac.dest) << ", " << (d.fmac.toAcc ? "true" : "false")
                    << ">>(kPairs[" << idx << "], rc, &sticky);\n";
            else
                out << "        vu.execPairOps<true>(kPairs[" << idx << "], rc);\n";
            if (flagRole[k] == 2)
                out << "        vu.aotFoldSticky(sticky); // when its result took the exact path\n        sticky = 0u;\n";
            if (d.lowerUsage.viWrite != 0u)
                out << "        vu.m_state.vi[" << int(viReg) << "] = static_cast<int16_t>(vu.m_state.vi[" << int(viReg) << "]);\n";
            if (record)
                out << "        vu.recordViWriteForBranch(" << int(viReg) << ", old" << k << ");\n";
            else if (clear)
                out << "        vu.m_viBranchBackupValid = false;\n";
            recordedPrev = record;
            const bool upperVf0 = !d.upperNop && (((d.upper >> 6) & 0x1Fu) == 0u || ((d.upper >> 16) & 0x1Fu) == 0u);
            const bool lowerVf0 = !d.iBit && !d.lowerNop && ((d.lower >> 16) & 0x1Fu) == 0u;
            const bool lowerVi0 = !d.iBit && !d.lowerNop && (((d.lower >> 16) & 0xFu) == 0u || ((d.lower >> 6) & 0xFu) == 0u);
            if (upperVf0 || lowerVf0)
                out << "        std::memcpy(vu.m_state.vf[0], vu1step::kVf0, sizeof(vu1step::kVf0));\n";
            if (lowerVi0)
                out << "        vu.m_state.vi[0] = 0;\n";
            if (isBranch(d))
                out << "        if (vu.m_state.branchPending)\n            vu.m_state.branchDelay = 0u;\n";
            if (d.eBit)
                out << "        vu.m_state.ebit = true;\n";
            const bool generic = (!d.iBit && !d.lowerNop && !d.lowerFn) || (!d.upperNop && !d.fmacFn);
            const Interp::Pipeline pipe = d.lowerUsage.pipeline;
            if (generic || waits || pipe == Interp::PipelineFdiv || pipe == Interp::PipelineEfu ||
                pipe == Interp::PipelineXgkick)
                out << "        ev = vu.aotNextEvent();\n";
            if (generic && k + 1u < n)
            {
                // A reserved instruction: stop after this pair.
                out << "        if (vu.m_stopRequested)\n";
                std::ostringstream bail;
                emitBail(bail, pairs, first, s, k + 1u, L, t, done, "(" + c + " + 1u)", false);
                out << bail.str();
            }
        }

        // The last step's cycle, the writes still in flight, the pc.
        const std::string last = cyc(t, n - 1u);
        out << "        {\n            const uint64_t next = " << last << " + 1u;\n"
            << "            if (next >= ev)\n                vu.aotSyncTo(" << done << ", next);\n"
            << "            else\n                vu.m_cycle = next;\n        }\n";
        for (uint32_t w = (n > L ? n - L : 0u); w < n; ++w)
        {
            if (hasWrites(pairs[s + w]))
                out << "        vu.aotMarkWrites(kPairs[" << s + w - first << "], " << cyc(t, w) << ");\n";
        }
        out << "        vu.m_pairsExecuted += " << n << "u;\n"
            << "        vu.m_seqPairs = 0u;\n        vu.m_expectedPc = ~0u;\n";
        const uint32_t fall = ((e + 1u) * 8u) & 0x3FFFu;
        std::snprintf(line, sizeof(line), "        vu.m_state.pc = 0x%04Xu;\n", fall);
        out << line;
        const auto &tail = pairs[e];
        const auto &branch = pairs[e > 0u ? e - 1u : 0u];
        if (n >= 2u && branch.eBit)
        {
            out << "        rc.programEnded = true;\n        return true;\n    }\n";
            return;
        }
        auto jumpTo = [&](uint32_t pcTarget) -> std::string
        {
            const uint32_t i = pcTarget / 8u;
            if (i >= first && i < end)
            {
                std::string to = label(i);
                if (to[0] == 'b')
                    to[0] = 'e';
                return "goto " + to + ";";
            }
            return "goto dispatch;";
        };
        if (n >= 2u && isBranch(branch))
        {
            out << "        if (vu.m_state.branchPending)\n        {\n"
                << "            vu.m_state.pc = vu.m_state.branchTarget & 0x3FFFu;\n"
                << "            vu.m_state.branchPending = false;\n";
            uint32_t target = 0;
            if (branchTarget(branch, (e - 1u) * 8u, target))
                out << "            " << jumpTo(target) << "\n";
            else
                out << "            goto dispatch;\n";
            out << "        }\n";
        }
        (void)tail;
        out << "        " << (fall / 8u == e + 1u ? jumpTo(fall) : std::string("goto dispatch;")) << "\n    }\n";
    }

    static bool write(const std::vector<uint8_t> &code, const std::filesystem::path &dir,
                      std::vector<std::filesystem::path> &written, std::string &error, uint32_t &blocks,
                      uint32_t &pairsInBlocks)
    {
        // (the interpreter's decoding and chain analysis, as its cache has them: rebuildDecodedCodeCache's, with
        // no interpreter)
        const uint64_t hash = VU1Interpreter::codeHash(code.data(), static_cast<uint32_t>(code.size()));
        const uint32_t pairCount = static_cast<uint32_t>(code.size() / 8u);
        std::vector<Interp::DecodedInstructionPair> decoded(pairCount);
        for (uint32_t i = 0; i < pairCount; ++i)
            decoded[i] = VU1Interpreter::decodeInstructionPair(VU1Interpreter::Unit::VU1, code.data(), i * 8u);
        VU1Interpreter::analyzeChains(decoded.data(), pairCount);
        for (uint32_t i = 0; i < pairCount; ++i)
        {
            const auto &d = decoded[i];
            if (d.lowerFn && VU1Interpreter::selectLowerFn(d.lower) != d.lowerFn)
            {
                error = "pair " + std::to_string(i) + ": lowerFn does not match its instruction";
                return false;
            }
            if (d.fmac.kind == Interp::FmacPlan::None && !d.upperNop && VU1Interpreter::selectUpperFastFn(d.upper) != d.fmacFn)
            {
                error = "pair " + std::to_string(i) + ": upper fast path does not match its instruction";
                return false;
            }
            if (d.fmac.kind != Interp::FmacPlan::None && VU1Interpreter::selectFmacFn(d.fmac) != d.fmacFn)
            {
                error = "pair " + std::to_string(i) + ": fmacFn does not match its plan";
                return false;
            }
        }

        // Chunks of about 128 pairs, split where no branch jumps across, so
        // loops stay inside one chunk function (entering one costs a
        // prologue, and on Windows x64 saving XMM6-15).
        std::vector<uint32_t> chunkStarts;
        {
            // Cost of a split before pair p: the branches across it, weighted
            // by how often they likely run (short loops are inner loops).
            std::vector<double> cost(pairCount + 1u, 0.0);
            for (uint32_t b = 0; b < pairCount; ++b)
            {
                uint32_t target = 0;
                const auto &d = decoded[b];
                if (!isBranch(d) || !branchTarget(d, b * 8u, target))
                    continue;
                const uint32_t from = b + 1u, to = target / 8u;
                const uint32_t lo = std::min(from, to) + 1u, hi = std::min(std::max(from, to), pairCount);
                for (uint32_t p = lo; p <= hi; ++p)
                    cost[p] += 1.0 / static_cast<double>(hi - lo + 1u);
            }
            uint32_t s = 0;
            while (s < pairCount)
            {
                chunkStarts.push_back(s);
                if (s + (std::getenv("ZX_VU1_AOT_FIXED_CHUNKS") ? 128u : 192u) >= pairCount)
                    break;
                uint32_t best = s + 128u;
                static const bool fixed = std::getenv("ZX_VU1_AOT_FIXED_CHUNKS") != nullptr;
                for (uint32_t p = s + 64u; !fixed && p <= s + 192u && p < pairCount; ++p)
                {
                    const auto distance = [&](uint32_t q) { return q > s + 128u ? q - (s + 128u) : (s + 128u) - q; };
                    if (cost[p] < cost[best] || (cost[p] == cost[best] && distance(p) < distance(best)))
                        best = p;
                }
                s = best;
            }
        }
        const uint32_t chunkCount = static_cast<uint32_t>(chunkStarts.size());
        if (chunkCount > kChunkFiles)
        {
            error = std::to_string(chunkCount) + " chunks: more than the " + std::to_string(kChunkFiles) + " chunk sources";
            return false;
        }
        std::vector<uint32_t> chunkEnd(pairCount), chunkOf(pairCount);
        for (uint32_t c = 0; c < chunkCount; ++c)
        {
            const uint32_t end = c + 1u < chunkCount ? chunkStarts[c + 1u] : pairCount;
            for (uint32_t i = chunkStarts[c]; i < end; ++i)
            {
                chunkEnd[i] = end;
                chunkOf[i] = c;
            }
        }
        char stem[64];
        std::snprintf(stem, sizeof(stem), "vu1_aot_%016llx", static_cast<unsigned long long>(hash));
        const std::string type = "VU1AotProgram<" + hex(hash) + "ll>";
        std::error_code made;
        std::filesystem::create_directories(dir, made);
        auto save = [&](const std::string &name, const std::string &text)
        {
            const std::filesystem::path path = dir / name;
            std::ofstream file(path);
            file << text;
            if (!file)
            {
                error = "cannot write " + path.string();
                return false;
            }
            written.push_back(path);
            return true;
        };

        // The program type, shared by the dispatcher and the chunks.
        std::ostringstream header;
        header << "// Generated by zx-vu1aot from VU1 micro memory " << hex(hash) << ". Do not edit.\n"
               << "#pragma once\n#include \"ps2_vu1_aot.h\"\n\n"
               << "template <>\nstruct " << type << "\n{\n"
               << "    static bool run(VU1Core &vu, VU1Core::RunContext &rc);\n";
        for (uint32_t c = 0; c < chunkCount; ++c)
            header << "    static bool chunk" << c << "(VU1Core &vu, VU1Core::RunContext &rc);\n";
        header << "};\n";
        if (!save(std::string(stem) + ".h", header.str()))
            return false;

        std::ostringstream master;
        master << "// Generated by zx-vu1aot from VU1 micro memory " << hex(hash) << ". Do not edit.\n"
               << "#include \"" << stem << ".h\"\n#include \"ps2_vu1_aot_register.h\"\n\n"
               << "bool " << type << "::run(VU1Core &vu, VU1Core::RunContext &rc)\n{\n"
               << "    using Chunk = bool (*)(VU1Core &, VU1Core::RunContext &);\n"
               << "    static constexpr Chunk kChunks[" << chunkCount << "] = {";
        for (uint32_t c = 0; c < chunkCount; ++c)
            master << (c ? ", " : "") << "&chunk" << c;
        master << "};\n    static constexpr uint8_t kChunkOf[" << pairCount << "] = {";
        for (uint32_t i = 0; i < pairCount; ++i)
            master << (i == 0u ? "\n        " : (i % 32u ? ", " : ",\n        ")) << chunkOf[i];
        master << "};\n    // The blocks take VU1's code and data sizes as constants.\n"
               << "    if (rc.codeSize != 0x4000u || rc.dataSize != 0x4000u)\n        return false;\n"
               << "    for (;;)\n    {\n"
               << "        const uint32_t pc = vu.m_state.pc;\n"
               << "        if ((pc & 7u) != 0u || pc >= " << pairCount * 8u << "u)\n"
               << "            return false;\n"
               << "        if (kChunks[kChunkOf[pc / 8u]](vu, rc))\n"
               << "            return true;\n    }\n}\n\n"
               << "VU1_AOT_REGISTER(" << hex(hash) << "ll)\n";
        if (!save(std::string(stem) + ".cpp", master.str()))
            return false;

        const std::vector<Interp::DecodedInstructionPair> pairs(decoded.begin(),
                                                               decoded.begin() + pairCount);
        static const bool blocksOn = []()
        {
            const char *value = std::getenv("ZX_VU1_AOT_BLOCKS");
            return !(value && value[0] == '0');
        }();
        const BlockPlan blockPlan = planBlocks(pairs, pairCount, chunkEnd);
        auto hasBlock = [&](uint32_t i) { return blocksOn && blockPlan.end[i] >= 0; };
        auto label = [&](uint32_t i)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%c%04X", hasBlock(i) ? 'b' : 'p', i * 8u);
            return std::string(text);
        };
        uint32_t blockCount = 0, blockPairs = 0;
        for (uint32_t c = 0; c < chunkCount; ++c)
        {
            const uint32_t first = chunkStarts[c];
            const uint32_t end = c + 1u < chunkCount ? chunkStarts[c + 1u] : pairCount;
            std::ostringstream out;
            out << "// Generated by zx-vu1aot from VU1 micro memory " << hex(hash) << ". Do not edit.\n"
                << "#include \"" << stem << ".h\"\n\n"
                << "bool " << type << "::chunk" << c << "(VU1Core &vu, VU1Core::RunContext &rc)\n{\n"
                << "    static constexpr VU1Core::DecodedInstructionPair kPairs[" << end - first << "] = {\n";
            for (uint32_t i = first; i < end; ++i)
            {
                char pcText[16];
                std::snprintf(pcText, sizeof(pcText), "%04X", i * 8u);
                out << "        /* " << pcText << " */ " << pair(decoded[i]) << ",\n";
            }
            out << "    };\ndispatch:\n    switch (vu.m_state.pc)\n    {\n";
            for (uint32_t i = first; i < end; ++i)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "    case 0x%04X:\n        goto %s;\n", i * 8u, label(i).c_str());
                out << line;
            }
            out << "    default:\n        return false;\n    }\n";
            for (uint32_t i = first; i < end; ++i)
            {
                if (!hasBlock(i))
                    continue;
                emitBlock(out, pairs, blockPlan, first, end, i, label);
                ++blockCount;
                blockPairs += static_cast<uint32_t>(blockPlan.end[i]) - i + 1u;
            }
            for (uint32_t i = first; i < end; ++i)
            {
                char line[128];
                if (i + 1u < end)
                    std::snprintf(line, sizeof(line), "p%04X:\n    VU1_AOT_PAIR(%u, 0x%04Xu, %s)\n", i * 8u, i - first,
                                  (i + 1u) * 8u, label(i + 1u).c_str());
                else
                    std::snprintf(line, sizeof(line), "p%04X:\n    VU1_AOT_LAST_PAIR(%u)\n", i * 8u, i - first);
                out << line;
            }
            out << "}\n";
            char name[96];
            std::snprintf(name, sizeof(name), "%s_c%02u.cpp", stem, c);
            if (!save(name, out.str()))
                return false;
        }
        // The rest of the kChunkFiles sources, empty: the build knows the file names before they are made.
        for (uint32_t c = chunkCount; c < kChunkFiles; ++c)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "%s_c%02u.cpp", stem, c);
            if (!save(name, "// Generated by zx-vu1aot from VU1 micro memory " + hex(hash) + ": no chunk " +
                                std::to_string(c) + ". Do not edit.\n"))
                return false;
        }
        blocks = blockCount;
        pairsInBlocks = blockPairs;
        return true;
    }
};


namespace zx::vu1aot
{
    const char *generatorKey() { return ZX_VU1_GENERATOR_KEY; }

    bool writeProgram(const std::vector<uint8_t> &image, const std::filesystem::path &dir, Written &written,
                      std::string &error)
    {
        if (generatorKey()[0] == '\0')
        {
            error = "this program was built without the VU1 generator's key (ZX_VU1_GENERATOR_KEY)";
            return false;
        }
        if (image.size() != 0x4000u)
        {
            error = "not a 16 KiB micro memory image";
            return false;
        }
        written = {};
        written.hash = VU1Interpreter::codeHash(image.data(), static_cast<uint32_t>(image.size()));
        return VU1AotGenerator::write(image, dir, written.files, error, written.blocks, written.pairsInBlocks);
    }
}
