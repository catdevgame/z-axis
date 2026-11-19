#ifndef PS2_VU1_STEP_H
#define PS2_VU1_STEP_H

// The interpreter's per-pair step and the hazard bookkeeping it uses, inline
// so that ahead-of-time compiled microprograms (ps2_vu1_aot.h) run the same
// code with constant pairs.
#include "runtime/ps2_vu1_core.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#endif

#if defined(_MSC_VER)
#define VU1_FORCE_INLINE __forceinline
#else
#define VU1_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace vu1step
{
    template <typename T>
    inline void copyBits(uint32_t (&out)[4], const T *values, uint32_t count)
    {
        std::memcpy(out, values, sizeof(uint32_t) * count);
    }
}
using vu1step::copyBits;

namespace vu1step
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}
using vu1step::laneForComponent;

// Small helpers the step and the instruction code call for every pair.
inline void VU1Core::normalizeOperands(const float *in, float *out) const
{
    uint32_t bits[4];
    std::memcpy(bits, in, sizeof(bits));
    std::memcpy(out, in, sizeof(bits));
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint32_t exponent = bits[component] & 0x7F800000u;
        if (exponent == 0u || exponent == 0x7F800000u)
            out[component] = normalizeOperand(in[component]);
    }
}

inline float VU1Core::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

namespace vu1step
{
    // All-ones lanes of a dest mask (x = 8 is lane 0).
    alignas(16) inline constexpr uint32_t kDestLaneMasks[16][4] = {
        {0, 0, 0, 0}, {0, 0, 0, ~0u}, {0, 0, ~0u, 0}, {0, 0, ~0u, ~0u},
        {0, ~0u, 0, 0}, {0, ~0u, 0, ~0u}, {0, ~0u, ~0u, 0}, {0, ~0u, ~0u, ~0u},
        {~0u, 0, 0, 0}, {~0u, 0, 0, ~0u}, {~0u, 0, ~0u, 0}, {~0u, 0, ~0u, ~0u},
        {~0u, ~0u, 0, 0}, {~0u, ~0u, 0, ~0u}, {~0u, ~0u, ~0u, 0}, {~0u, ~0u, ~0u, ~0u}};
    alignas(16) inline constexpr float kVf0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
}

// The register as one 16-byte store (a later vector load of it forwards
// from the store; four lane stores would not), bits copied unchanged.
inline void VU1Core::applyDest(float *dst, const float *result, uint8_t dest)
{
#if defined(__aarch64__)
    const uint32x4_t mask = vld1q_u32(vu1step::kDestLaneMasks[dest & 0xFu]);
    vst1q_u32(reinterpret_cast<uint32_t *>(dst), vbslq_u32(mask, vld1q_u32(reinterpret_cast<const uint32_t *>(result)),
                                                             vld1q_u32(reinterpret_cast<const uint32_t *>(dst))));
#elif defined(__SSE4_1__)
    const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i *>(vu1step::kDestLaneMasks[dest & 0xFu]));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst),
                     _mm_blendv_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(dst)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i *>(result)), mask));
#else
    if (dest & 0x8u)
        std::memcpy(&dst[0], &result[0], 4);
    if (dest & 0x4u)
        std::memcpy(&dst[1], &result[1], 4);
    if (dest & 0x2u)
        std::memcpy(&dst[2], &result[2], 4);
    if (dest & 0x1u)
        std::memcpy(&dst[3], &result[3], 4);
#endif
}

inline void VU1Core::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

VU1_FORCE_INLINE bool VU1Core::pushFlag(FlagRing &ring, uint32_t value, uint32_t writes)
{
    // All flag results have the FMAC latency, so each ring is a FIFO in
    // issue order (entries of one pair: upper first).
    if (ring.count >= kMaxFlagEntries) [[unlikely]]
    {
        applyReadyFlags();
        if (ring.count >= kMaxFlagEntries)
            return false;
    }
    FlagPipelineEntry &entry = ring.entries[(ring.head + ring.count) % kMaxFlagEntries];
    ++ring.count;
    entry.issueCycle = m_cycle;
    entry.value = value;
    entry.writes = writes;
    return true;
}

// The MAC/status result of an FMAC instruction (status: its Z/S/U/O bits).
VU1_FORCE_INLINE void VU1Core::queueMacStatus(uint32_t mac, uint32_t status, uint32_t extraSticky)
{
    m_macFlags.prefix |= (status | extraSticky) & 0xFu;
    if (!pushFlag(m_macFlags, (mac & 0xFFFFu) | ((status & 0xFu) << 16) | ((extraSticky & 0xFu) << 20),
                  FlagWritesMacStatus | (m_macFlags.prefix << FlagPrefixShift)))
        reportReservedInstruction(true, 0xFFFFFFFFu);
}

// MAC/status result of an FMAC instruction (status: S and Z from the MAC).
VU1_FORCE_INLINE bool VU1Core::queueFmacFlags(uint32_t mac, uint32_t extraSticky)
{
    queueMacStatus(mac, ((mac & 0xF0u) != 0u ? 0x2u : 0u) | ((mac & 0x0Fu) != 0u ? 0x1u : 0u), extraSticky);
    return true;
}

inline void VU1Core::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    // Written at issue: the next cycle's PATH1 read and any later load see it
    // exactly as with a one-cycle commit (nothing else reads data memory
    // between).
    if (!m_activeVuData || address + 16u > m_activeVuDataSize)
        return;
    if (m_xgkick.active)
        syncXgkick(); // PATH1 reads what was there before this cycle

    uint32_t lanes[4]{};
    std::memcpy(lanes, m_activeVuData + address, sizeof(lanes));
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
            lanes[component] = words[component];
    }
    std::memcpy(m_activeVuData + address, lanes, sizeof(lanes));
    m_writesDoneCycle = std::max(m_writesDoneCycle, m_cycle + 1u);
}


// FDIV, EFU and PATH1 waits of a pair (decoded.waitMask).
VU1_FORCE_INLINE uint64_t VU1Core::aotWaitReady(const DecodedInstructionPair &decoded, uint64_t ready)
{
    const uint8_t wait = decoded.waitMask;
    if ((wait & (DecodedInstructionPair::WaitFdiv | DecodedInstructionPair::WaitQ)) && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (wait & DecodedInstructionPair::WaitEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (wait & DecodedInstructionPair::WaitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    // A second XGKICK waits until PATH1 is idle, which only changes on
    // a qword copy.
    if ((wait & DecodedInstructionPair::WaitXgkick) && m_xgkick.active)
    {
        syncXgkick();
        if (m_xgkick.active)
            ready = std::max(ready, m_cycle + cyclesToXgkickTagEnd());
    }
    return ready;
}

VU1_FORCE_INLINE uint64_t VU1Core::calculatePairReadyCycle(const DecodedInstructionPair &decoded)
{
    uint64_t ready = m_cycle;
    for (uint32_t index = 0; index < decoded.vfReadCount; ++index)
    {
        const uint64_t (&lanes)[4] = m_vfReady[decoded.vfReadReg[index]];
        const uint8_t mask = decoded.vfReadLanes[index];
        if (mask & 0x8u)
            ready = std::max(ready, lanes[0]);
        if (mask & 0x4u)
            ready = std::max(ready, lanes[1]);
        if (mask & 0x2u)
            ready = std::max(ready, lanes[2]);
        if (mask & 0x1u)
            ready = std::max(ready, lanes[3]);
    }
    for (uint32_t mask = decoded.viReadMask; mask != 0u; mask &= mask - 1u)
        ready = std::max(ready, m_viReady[std::countr_zero(mask)]);
    if (const uint8_t acc = decoded.accReadLanes)
    {
        if (acc & 0x8u)
            ready = std::max(ready, m_accReady[0]);
        if (acc & 0x4u)
            ready = std::max(ready, m_accReady[1]);
        if (acc & 0x2u)
            ready = std::max(ready, m_accReady[2]);
        if (acc & 0x1u)
            ready = std::max(ready, m_accReady[3]);
    }

    if (decoded.waitMask)
        ready = aotWaitReady(decoded, ready);
    return ready;
}

VU1_FORCE_INLINE uint64_t VU1Core::calculateNearReadyCycle(const DecodedInstructionPair &decoded)
{
    uint64_t ready = m_cycle;
    for (uint32_t index = 0; index < decoded.nearVfReadCount; ++index)
    {
        const uint64_t (&lanes)[4] = m_vfReady[decoded.nearVfReadReg[index]];
        const uint8_t mask = decoded.nearVfReadLanes[index];
        if (mask & 0x8u)
            ready = std::max(ready, lanes[0]);
        if (mask & 0x4u)
            ready = std::max(ready, lanes[1]);
        if (mask & 0x2u)
            ready = std::max(ready, lanes[2]);
        if (mask & 0x1u)
            ready = std::max(ready, lanes[3]);
    }
    for (uint32_t mask = decoded.nearViReadMask; mask != 0u; mask &= mask - 1u)
        ready = std::max(ready, m_viReady[std::countr_zero(mask)]);
    // ACC results forward in one cycle: a read in a chain never waits.
    if (decoded.waitMask)
        ready = aotWaitReady(decoded, ready);
    return ready;
}

VU1_FORCE_INLINE void VU1Core::markPairWrites(const DecodedInstructionPair &decoded)
{
    aotMarkWrites(decoded, m_cycle);
}

// The ready cycles of the writes of a pair issued at `cycle`.
VU1_FORCE_INLINE void VU1Core::aotMarkWrites(const DecodedInstructionPair &decoded, uint64_t cycle)
{
    auto markVf = [this](uint8_t reg, uint8_t lanes, uint64_t ready)
    {
        uint64_t (&slot)[4] = m_vfReady[reg];
        if (lanes & 0x8u)
            slot[0] = ready;
        if (lanes & 0x4u)
            slot[1] = ready;
        if (lanes & 0x2u)
            slot[2] = ready;
        if (lanes & 0x1u)
            slot[3] = ready;
        m_writesDoneCycle = std::max(m_writesDoneCycle, ready);
    };
    if (decoded.lowerWriteReg != 0u)
        markVf(decoded.lowerWriteReg, decoded.lowerWriteLanes, cycle + decoded.lowerWriteLatency);
    if (decoded.upperWriteReg != 0u)
        markVf(decoded.upperWriteReg, decoded.upperWriteLanes, cycle + decoded.upperWriteLatency);
    if (decoded.viWriteReg != 0u)
    {
        m_viReady[decoded.viWriteReg] = cycle + decoded.viWriteLatency;
        m_writesDoneCycle = std::max(m_writesDoneCycle, m_viReady[decoded.viWriteReg]);
    }
    if (const uint8_t acc = decoded.accWriteLanes)
    {
        const uint64_t ready = cycle + kAccForwardLatency;
        if (acc & 0x8u)
            m_accReady[0] = ready;
        if (acc & 0x4u)
            m_accReady[1] = ready;
        if (acc & 0x2u)
            m_accReady[2] = ready;
        if (acc & 0x1u)
            m_accReady[3] = ready;
        m_writesDoneCycle = std::max(m_writesDoneCycle, ready);
    }
}

// The pair's lower instruction: its own instantiation when it has one.
template <bool FixedVu1Data>
VU1_FORCE_INLINE void VU1Core::execLowerOf(const DecodedInstructionPair &decoded, RunContext &rc)
{
    const uint32_t dataSize = FixedVu1Data ? 0x4000u : rc.dataSize;
    if (decoded.lowerFn)
        decoded.lowerFn(*this, decoded.lower, rc.vuData, dataSize, decoded.upper);
    else
        execLower(decoded.lower, rc.vuData, dataSize, decoded.upper);
}

#if defined(_MSC_VER)
#define VU1_NOINLINE __declspec(noinline)
#else
#define VU1_NOINLINE __attribute__((noinline))
#endif

VU1_NOINLINE inline void VU1Core::tracePair()
{
    m_state.cycles = m_cycle;
    applyReadyFlags();
    m_host.tracePair(m_host.context, m_state.pc, &m_state);
}

VU1_NOINLINE inline void VU1Core::captureOldBits(const DecodedInstructionPair &decoded, int32_t oldVi,
                                                        uint32_t (&oldBits)[4][4])
{
    if (decoded.upperUsage.vfWrite.reg != 0u)
        copyBits(oldBits[0], m_state.vf[decoded.upperUsage.vfWrite.reg], 4u);
    if (decoded.lowerUsage.vfWrite.reg != 0u)
        copyBits(oldBits[1], m_state.vf[decoded.lowerUsage.vfWrite.reg], 4u);
    copyBits(oldBits[2], &oldVi, 1u);
    copyBits(oldBits[3], m_state.acc, 4u);
}

// A taken branch: count down its delay slot, then jump.
VU1_NOINLINE inline void VU1Core::stepBranch()
{
    if (m_state.branchDelay == 0u)
    {
        m_state.pc = m_state.branchTarget & microAddressMask();
        m_state.branchPending = false;
    }
    else
    {
        --m_state.branchDelay;
    }
}

// The end of a program (E bit, its delay slot, D/T stops), after the pc update.
VU1_NOINLINE inline void VU1Core::stepProgramEnd(const DecodedInstructionPair &decoded, RunContext &rc,
                                                        bool dHalt, bool tHalt)
{
    const bool haltBit = dHalt || tHalt;
    const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;
    if (m_state.haltAfterDelaySlot)
    {
        m_state.stoppedByD = m_pendingHaltD;
        m_state.stoppedByT = m_pendingHaltT;
        rc.programEnded = true;
    }
    else if (m_state.ebit)
        rc.programEnded = true;
    else if (haltBit && !haltBranch)
    {
        m_state.stoppedByD = dHalt;
        m_state.stoppedByT = tHalt;
        rc.programEnded = true;
    }
    else if (decoded.eBit)
        m_state.ebit = true;
    else if (haltBranch)
    {
        m_state.haltAfterDelaySlot = true;
        m_pendingHaltD = dHalt;
        m_pendingHaltT = tHalt;
    }
}

// The pair's upper instruction through its fast path, false when it needs
// execUpper (DeferFn: the FMAC as fmacSpecializedDefer/Fold).
template <auto DeferFn>
VU1_FORCE_INLINE bool VU1Core::upperFastOp(const DecodedInstructionPair &decoded, uint32_t *sticky)
{
    if constexpr (std::is_same_v<decltype(DeferFn), std::nullptr_t>)
    {
        static_cast<void>(sticky);
        return decoded.fmacFn && decoded.fmacFn(*this, decoded.fmac);
    }
    else
        return DeferFn(*this, decoded.fmac, *sticky);
}

template <bool FixedVu1Data, auto DeferFn>
VU1_FORCE_INLINE void VU1Core::execPairOps(const DecodedInstructionPair &decoded, RunContext &rc, uint32_t *sticky)
{
    if (decoded.iBit)
    {
        if (!decoded.upperNop && !upperFastOp<DeferFn>(decoded, sticky))
            execUpperOutOfLine(decoded.upper);
        float immediate = 0.0f;
        std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
        m_state.i = normalizeOperand(immediate);
    }
    else if (decoded.upperVfShadowReg != 0u)
    {
        float oldVf[4]{};
        float upperVf[4]{};
        std::memcpy(oldVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(oldVf));
        if (!upperFastOp<DeferFn>(decoded, sticky))
            execUpperOutOfLine(decoded.upper);
        std::memcpy(upperVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(upperVf));
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    oldVf,
                    sizeof(oldVf));
        execLowerOf<FixedVu1Data>(decoded, rc);
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    upperVf,
                    sizeof(upperVf));
    }
    else
    {
        if (!decoded.upperNop && !upperFastOp<DeferFn>(decoded, sticky))
            execUpperOutOfLine(decoded.upper);
        if (!decoded.lowerNop)
            execLowerOf<FixedVu1Data>(decoded, rc);
    }
}

// One pass of the run loop for the selected pair (the loop condition, the
// pipeline commit and the pc bounds check come first, in the caller). Shared
// by the interpreter and ahead-of-time compiled microprograms (ps2_vu1_aot.h),
// which call it with constant pairs so the compiler can specialize it.
VU1_FORCE_INLINE VU1Core::StepResult VU1Core::stepPair(const DecodedInstructionPair &decoded, bool cached,
                                                                    RunContext &rc)
{
    if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
    {
        reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
        return StepResult::Stop;
    }

    // Chain analysis: deep in a sequence only near reads can stall.
    m_seqPairs = m_state.pc == m_expectedPc ? m_seqPairs + (m_seqPairs < 255u ? 1u : 0u) : 0u;
    m_expectedPc = m_state.pc + 8u;
    const bool inChain = decoded.chainInfo && cached && m_seqPairs >= m_chainDepth &&
                         m_cycle + 16u < rc.budgetEnd;
    uint64_t readyCycle = inChain ? calculateNearReadyCycle(decoded) : calculatePairReadyCycle(decoded);
    while (readyCycle > m_cycle)
    {
        if (readyCycle >= rc.budgetEnd)
        {
            advanceTo(rc.budgetEnd);
            break;
        }
        advanceTo(readyCycle);
        readyCycle = inChain ? calculateNearReadyCycle(decoded) : calculatePairReadyCycle(decoded);
    }
    if (m_cycle >= rc.budgetEnd)
        return StepResult::Stop;

    // Register and memory results are written at issue. Every reader of
    // a VF/VI/ACC lane stalls until the writer's latency has passed
    // (calculatePairReadyCycle), all VF writes have the same latency, and
    // stores become visible to PATH1 on the next cycle either way, so
    // this is the same as committing them later. Only the lower half of
    // a pair must not see the upper half's result (upperVfShadowReg).
    if (m_tracingPairs) [[unlikely]]
        tracePair();

    uint8_t writtenVi = 0u;
    int32_t oldVi = 0;
    if (decoded.lowerUsage.viWrite != 0u)
    {
        writtenVi = static_cast<uint8_t>(std::countr_zero(decoded.lowerUsage.viWrite));
        oldVi = m_state.vi[writtenVi];
    }
    // Writes of the last few cycles before the budget runs out are logged
    // so a budget stop can hide the ones still in flight.
    const bool logWrites = m_cycle + kMaxWriteLatency >= rc.budgetEnd;
    uint32_t oldBits[4][4]; // only read when logWrites
    if (logWrites) [[unlikely]]
        captureOldBits(decoded, oldVi, oldBits);

    execPairOps(decoded, rc);

    m_viBranchBackupValid = false;
    if (decoded.lowerUsage.viWrite != 0u)
        m_state.vi[writtenVi] = static_cast<int16_t>(m_state.vi[writtenVi]);
    if (m_deferredWriteCount != 0u) [[unlikely]]
        supersedeDeferredWrites(decoded, writtenVi);
    if (logWrites) [[unlikely]]
        logPairWrites(decoded, writtenVi, oldBits);

    if (!inChain || decoded.chainMark)
        markPairWrites(decoded);
    ++m_pairsExecuted;
    if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
        recordViWriteForBranch(writtenVi, oldVi);

    std::memcpy(m_state.vf[0], vu1step::kVf0, sizeof(vu1step::kVf0));
    m_state.vi[0] = 0;

    uint32_t nextPc = m_state.pc + 8u;
    if (nextPc >= rc.codeSize)
        nextPc = 0u;
    m_state.pc = nextPc;

    if (m_state.branchPending) [[unlikely]]
        stepBranch();

    const bool dHalt = decoded.dBit && m_state.dBitEnabled;
    const bool tHalt = decoded.tBit && m_state.tBitEnabled;
    if (m_state.haltAfterDelaySlot | m_state.ebit | dHalt | tHalt | decoded.eBit) [[unlikely]]
        stepProgramEnd(decoded, rc, dHalt, tHalt);

    advanceOneCycle();
    return rc.programEnded ? StepResult::Stop : StepResult::Continue;
}

#endif
