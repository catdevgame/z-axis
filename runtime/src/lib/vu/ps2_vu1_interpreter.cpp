// VU0's and VU1's interpreter (runtime/ps2_vu1.h): the decoder and its static analysis, the run loop, what the core
// asks of its host, and the ahead-of-time compiled microprograms by micro memory hash.
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_exec_lower.h"
#include "ps2_vu1_exec_upper.h"
#include "ps2_vu1_step.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#include <xmmintrin.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <ps2_log.h>



VU1Interpreter::VU1Interpreter(Unit unit)
{
    m_unit = unit;
    m_host = VU1Host{this, &hostPacket, &hostTracePair, &hostReservedInstruction};
    m_xgkickPacket = m_xgkickPacketBuffer.data();
    m_deferredWrites = m_deferredWriteStorage.data();
    reset();
}

void VU1Interpreter::hostPacket(void *context, const uint8_t *data, uint32_t size)
{
    VU1Interpreter &vu = *static_cast<VU1Interpreter *>(context);
    if (vu.m_packetObserver)
        vu.m_packetObserver(data, size);
    if (vu.m_deliverPackets)
    {
        if (vu.m_activeMemory)
            vu.m_activeMemory->submitGifPacket(GifPathId::Path1, data, size);
        else if (vu.m_activeGs)
            vu.m_activeGs->processGIFPacket(data, size);
    }
}

void VU1Interpreter::hostTracePair(void *context, uint32_t pc, const VU1State *state)
{
    static_cast<VU1Interpreter *>(context)->m_pairTrace(pc, *state);
}

void VU1Interpreter::hostReservedInstruction(void *context, bool upper, uint32_t instruction)
{
    const VU1Interpreter &vu = *static_cast<const VU1Interpreter *>(context);
    RUNTIME_ERROR(
        "[VU" << (vu.m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << vu.m_cycle
              << " pc=0x" << std::hex << vu.m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
}

void VU1Interpreter::resetScheduler()
{
    m_macFlags.head = m_macFlags.count = 0;
    m_macFlags.prefix = m_macFlags.special = 0;
    m_macFlags.inOrder = false;
    m_clipFlags.head = m_clipFlags.count = 0;
    m_fdiv = {};
    std::fill(std::begin(m_efu), std::end(m_efu), ScalarPipelineEntry{});
    m_xgkick = {};
    std::fill(&m_vfReady[0][0], &m_vfReady[0][0] + 32 * 4, 0ull);
    std::fill(std::begin(m_viReady), std::end(m_viReady), 0ull);
    std::fill(std::begin(m_accReady), std::end(m_accReady), 0ull);
    m_nextCommitCycle = ~0ull;
    m_writesDoneCycle = 0;
    std::fill(std::begin(m_writeLog), std::end(m_writeLog), InFlightWrite{});
    m_writeLogNext = 0;
    m_deferredWriteCount = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

void VU1Interpreter::restoreState(const VU1State &state)
{
    m_state = state;
    m_cycle = state.cycles;
    resetScheduler();
}

void VU1Interpreter::rollBackInFlightWrites()
{
    // Newest first, so a lane written twice ends with its value from before
    // both writes.
    std::vector<InFlightWrite> inFlight;
    const uint32_t count = std::min(m_writeLogNext, kWriteLogSize);
    for (uint32_t back = 1; back <= count; ++back)
    {
        const InFlightWrite &entry = m_writeLog[(m_writeLogNext - back) % kWriteLogSize];
        if (entry.readyCycle <= m_cycle || entry.lanes == 0u)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if (entry.kind == 1u)
            {
                if (component == 0u)
                    std::memcpy(&m_state.vi[entry.reg], &entry.oldBits[0], sizeof(uint32_t));
                continue;
            }
            if ((entry.lanes & laneForComponent(component)) == 0u)
                continue;
            float *target = entry.kind == 0u ? &m_state.vf[entry.reg][component] : &m_state.acc[component];
            std::memcpy(target, &entry.oldBits[component], sizeof(uint32_t));
        }
        inFlight.push_back(entry);
    }
    std::fill(std::begin(m_writeLog), std::end(m_writeLog), InFlightWrite{});
    m_writeLogNext = 0;
    // (after the ones a stop before left, newest last; the core's storage grown when a stop needs more)
    const size_t needed = m_deferredWriteCount + inFlight.size();
    if (m_deferredWriteStorage.size() < needed)
    {
        m_deferredWriteStorage.resize(needed);
        m_deferredWrites = m_deferredWriteStorage.data();
    }
    std::copy(inFlight.rbegin(), inFlight.rend(), m_deferredWrites + m_deferredWriteCount);
    m_deferredWriteCount = static_cast<uint32_t>(needed);
    for (uint32_t index = 0; index < m_deferredWriteCount; ++index)
        noteCommitCycle(m_deferredWrites[index].readyCycle);
}

void VU1Interpreter::flushPipelines()
{
    for (;;)
    {
        syncXgkick();
        applyReadyFlags();
        if (!pipelinesPending())
            break;
        // The run ends on the cycle PATH1 finishes, which can only be a
        // cycle that copies a qword: step from copy to copy.
        if (m_xgkick.active)
            advanceTo(m_cycle + cyclesToXgkickTagEnd());
        else
        {
            uint64_t target = m_nextCommitCycle;
            if (m_cycle < m_writesDoneCycle)
                target = std::min(target, m_writesDoneCycle);
            if (m_macFlags.count != 0u)
                target = std::min(target, m_macFlags.entries[m_macFlags.head].issueCycle + kFmacLatency);
            if (m_clipFlags.count != 0u)
                target = std::min(target, m_clipFlags.entries[m_clipFlags.head].issueCycle + kFmacLatency);
            advanceTo(target);
        }
    }
}



void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(m_unit, vuCode, i * 8u);
    m_chainDepth = analyzeChains(m_decodedCodeCache.data(), pairCount);
    selectAotProgram(vuCode, codeSize);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

const VU1Interpreter::DecodedInstructionPair *VU1Interpreter::prepareDecodedCode(
    const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory)
{
    // Micro memory only changes between runs (VIF MPG, EE writes), so the
    // cache is checked once per run.
    const bool trackedCode = memory != nullptr &&
                             ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                              (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedCode)
        return nullptr;

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    return m_decodedCodeCache.data();
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}


void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_seqPairs = 0;
    m_expectedPc = ~0u;
    static const bool aotBlocks = []()
    {
        const char *value = std::getenv("PS2X_VU1_AOT_BLOCKS");
        return !(value && value[0] == '0');
    }();
    m_aotBlocksOff = !aotBlocks || m_tracingPairs;
    if (m_macFlags.count != 0u)
        m_macFlags.inOrder = true;
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    // Round toward zero: set where the thread is not rounding so already, and put back after, but on the VIF1
    // worker, which runs VU1 code and integer work alone (VIF codes, the GIF's packets): it keeps the VU's rounding
    // from its first run on, and the runs after it find it set.
    const bool keepVuRounding = memory && memory->onVu1Worker();
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // (on x86-64 only MXCSR matters (no x87 math), and the MSVC CRT's fegetround/fesetround cost more than a
    // short run)
    const unsigned int previousRounding = _mm_getcsr() & 0x6000u;
    if (previousRounding != 0x6000u)
        _mm_setcsr(_mm_getcsr() | 0x6000u);
#else
    const int previousRoundingMode = std::fegetround();
    const bool setVuRounding = previousRoundingMode != FE_TOWARDZERO && std::fesetround(FE_TOWARDZERO) == 0;
#endif
    RunContext rc{vuCode, codeSize, vuData, dataSize, m_cycle + maxCycles, false};
    const DecodedInstructionPair *decodedCode = prepareDecodedCode(vuCode, codeSize, memory);
    // Ahead-of-time code for this micro memory runs the loop until it ends,
    // or hands back to the interpreter (a pc it has no code for).
    if (decodedCode && m_aotProgram)
        commitReadyPipelines();
    const bool handled = decodedCode && m_aotProgram && m_aotProgram(static_cast<VU1Core *>(this), &rc);
    while (!handled && m_cycle < rc.budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        const DecodedInstructionPair *decodedPair = nullptr;
        if (decodedCode && (m_state.pc & 7u) == 0u && m_state.pc / 8u < kMaxDecodedPairs)
            decodedPair = &decodedCode[m_state.pc / 8u];
        else
        {
            m_scratchDecoded = decodeInstructionPair(m_unit, vuCode, m_state.pc);
            decodedPair = &m_scratchDecoded;
        }
        if (stepPair(*decodedPair, decodedPair != &m_scratchDecoded, rc) == StepResult::Stop)
            break;
    }
    const bool programEnded = rc.programEnded;
    syncXgkick();

    if (!programEnded)
    {
        rollBackInFlightWrites();
        applyReadyFlags();
    }
    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (previousRounding != 0x6000u && !keepVuRounding)
        _mm_setcsr((_mm_getcsr() & ~0x6000u) | previousRounding);
#else
    if (setVuRounding && !keepVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
#endif
}

// Registry of ahead-of-time compiled code by micro memory hash. Filled by
// static initializers of the generated sources, read when code changes.
void VU1Interpreter::registerAotProgram(uint64_t hash, VU1AotRun program)
{
    static_cast<void>(aotRegistry(hash, program));
}

VU1AotRun VU1Interpreter::findAotProgram(uint64_t hash)
{
    return aotRegistry(hash, nullptr);
}

VU1AotRun VU1Interpreter::aotRegistry(uint64_t hash, VU1AotRun add)
{
    static std::mutex mutex;
    static std::unordered_map<uint64_t, VU1AotRun> programs;
    std::lock_guard<std::mutex> lock(mutex);
    if (add)
        programs[hash] = add;
    const auto it = programs.find(hash);
    return it == programs.end() ? nullptr : it->second;
}

// PS2X_VU1_AOT=0 runs everything on the interpreter. PS2X_VU1_CODE_DUMP=<dir>
// writes every micro memory image VU1 runs as <dir>/<hash>.vu1code (input for
// ps2xVu1Aot).
void VU1Interpreter::selectAotProgram(const uint8_t *vuCode, uint32_t codeSize)
{
    m_aotProgram = nullptr;
    if (m_unit != Unit::VU1 || codeSize != 0x4000u)
        return;
    static const bool enabled = []()
    {
        const char *value = std::getenv("PS2X_VU1_AOT");
        return !(value && value[0] == '0');
    }();
    static const char *dumpDir = std::getenv("PS2X_VU1_CODE_DUMP");
    const uint64_t hash = codeHash(vuCode, codeSize);
    if (enabled)
        m_aotProgram = findAotProgram(hash);
    static std::mutex seenMutex;
    static std::unordered_set<uint64_t> seen;
    {
        std::lock_guard<std::mutex> lock(seenMutex);
        if (!seen.insert(hash).second)
            return;
    }
    std::fprintf(stderr, "[vu1] micro memory %016llx: %s\n", static_cast<unsigned long long>(hash),
                 m_aotProgram ? "ahead-of-time code" : (enabled ? "interpreter (no ahead-of-time code)" : "interpreter (PS2X_VU1_AOT=0)"));
    if (dumpDir && dumpDir[0])
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.vu1code", static_cast<unsigned long long>(hash));
        const std::filesystem::path path = std::filesystem::path(dumpDir) / name;
        if (FILE *f = std::fopen(path.string().c_str(), "wb"))
        {
            std::fwrite(vuCode, 1, codeSize, f);
            std::fclose(f);
        }
    }
}
