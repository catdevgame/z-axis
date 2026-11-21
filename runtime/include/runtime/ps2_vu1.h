#ifndef PS2_VU1_H
#define PS2_VU1_H

// VU0's and VU1's interpreter: the core the step runs on (VU1Core, runtime/ps2_vu1_core.h) with what the runtime
// adds to it: the decoder and its cache, the run loop, the packet observer and pair trace, and the ahead-of-time
// compiled microprograms (zx-vu1aot), the runner's own or a game module's, by micro memory hash.

#include "runtime/ps2_vu1_core.h"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class GS;
class PS2Memory;

class VU1Interpreter : private VU1Core
{
public:
    using VU1Core::Unit;

    explicit VU1Interpreter(Unit unit = Unit::VU1);
    VU1Interpreter(const VU1Interpreter &) = delete;
    VU1Interpreter &operator=(const VU1Interpreter &) = delete;

    void reset();

    // Cycle budget of one MSCAL/MSCNT run in the runtime. VU1 runs while the
    // VIF waits (MSCAL, MSCNT and FLUSH stall until the program ends), so a
    // run must reach its E bit: a program cut short and resumed by the next
    // MSCNT, or replaced by the next MSCAL, loses the rest of its work. The
    // near-plane clipper of big ground meshes (Aggressive Inline) needs well
    // over 2 x 65536 cycles. The limit only guards against code that never
    // ends (2^24 cycles: 0.11 s of VU1 time).
    static constexpr uint32_t kProgramCycleLimit = 1u << 24;

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // Called with every PATH1 packet XGKICK completes. With deliver=false the
    // packet goes only to the observer (offline replay, zx-vu1check).
    using PacketObserver = std::function<void(const uint8_t *data, uint32_t size)>;
    void setPacketObserver(PacketObserver observer, bool deliver = true)
    {
        m_packetObserver = std::move(observer);
        m_deliverPackets = deliver;
    }

    // Replace the architectural state and drop all pipeline state, as if the
    // previous run had ended with this state (corpus replay).
    void restoreState(const VU1State &state);
    // True when no pipeline (flags, Q/P, stores, writes, XGKICK) has work left.
    bool pipelinesIdle() const { return !pipelinesPending(); }
    // FNV-1a hash of micro memory: the key of ahead-of-time compiled code
    // (zx-vu1aot, ZX_VU1_AOT).
    static uint64_t codeHash(const uint8_t *code, uint32_t size);
    // True when the current micro memory runs ahead-of-time compiled code.
    bool usingAotCode() const { return m_aotProgram != nullptr; }
    // Instruction pairs issued so far (statistics).
    uint64_t pairsExecuted() const { return m_pairsExecuted; }

    // Debugging: called with the pc of every pair before it issues (after
    // any stall), with the state as the pair sees it.
    using PairTrace = std::function<void(uint32_t pc, const VU1State &state)>;
    void setPairTrace(PairTrace trace)
    {
        m_pairTrace = std::move(trace);
        m_tracingPairs = static_cast<bool>(m_pairTrace);
    }

    // Ahead-of-time compiled code for the micro memory with this hash: the runner's own (their sources register
    // themselves, ps2_vu1_aot.h) and game modules' (lib/game_module.cpp). A later one for a hash replaces the earlier.
    static void registerAotProgram(uint64_t hash, VU1AotRun program);

private:
    // The generator of ahead-of-time code uses the decoder and its analysis.
    friend struct VU1AotGenerator;

    // execFmacFast/upperFast/execLower instantiations chosen per decoded pair.
    static FmacFn selectFmacFn(const FmacPlan &plan);
    static FmacFn selectUpperFastFn(uint32_t upper);
    static LowerFn selectLowerFn(uint32_t instr);
    template <size_t... I>
    static constexpr std::array<FmacFn, sizeof...(I)> makeUpperFastTable(std::index_sequence<I...>)
    {
        // I = op * 64 + dest * 4 + sub
        return {{&upperFast<static_cast<int>(I / 64u), static_cast<int>((I / 4u) % 16u), static_cast<int>(I % 4u)>...}};
    }
    template <size_t... I>
    static constexpr std::array<FmacFn, sizeof...(I)> makeFmacTable(std::index_sequence<I...>)
    {
        // I = kind * 128 + source * 32 + dest * 2 + toAcc
        return {{&fmacSpecialized<static_cast<int>(I / 128u), static_cast<int>((I / 32u) % 4u),
                                  static_cast<int>((I / 2u) % 16u), (I % 2u) != 0u>...}};
    }
    template <size_t... I>
    static constexpr std::array<LowerFn, sizeof...(I)> makeLowerTable(std::index_sequence<I...>)
    {
        // I < 128: opHi (not 0x40); 128 + funct (< 0x3C); 192 + funct2.
        return {{&lowerOp<(I < 128u ? static_cast<uint32_t>(I) : 0x40u), (I >= 128u && I < 192u ? static_cast<uint32_t>(I - 128u) : (I >= 192u ? 0x3Cu : 0u)),
                          (I >= 192u ? static_cast<uint32_t>(I - 192u) : 0u)>...}};
    }

    // The core's host operations (VU1Host): this interpreter's observer, GIF and trace.
    static void hostPacket(void *context, const uint8_t *data, uint32_t size);
    static void hostTracePair(void *context, uint32_t pc, const VU1State *state);
    static void hostReservedInstruction(void *context, bool upper, uint32_t instruction);

    PacketObserver m_packetObserver;
    PairTrace m_pairTrace;
    bool m_deliverPackets = true;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    // Large buffers live outside the object so the per-pair state stays at
    // small offsets, and resetting the pipelines does not clear them.
    std::vector<DecodedInstructionPair> m_decodedCodeCache = std::vector<DecodedInstructionPair>(kMaxDecodedPairs);
    std::vector<uint8_t> m_xgkickPacketBuffer = std::vector<uint8_t>(XgkickPipeline::kBufferSize); // (the core's m_xgkickPacket)
    // The core's deferred writes (m_deferredWrites: its first m_deferredWriteCount), grown as a stop needs.
    std::vector<InFlightWrite> m_deferredWriteStorage = std::vector<InFlightWrite>(2u * kWriteLogSize);
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;
    DecodedInstructionPair m_scratchDecoded{};

    // Ahead-of-time compiled code for the current micro memory (or null).
    VU1AotRun m_aotProgram = nullptr;
    static VU1AotRun findAotProgram(uint64_t hash);
    static VU1AotRun aotRegistry(uint64_t hash, VU1AotRun add);
    void selectAotProgram(const uint8_t *vuCode, uint32_t codeSize);

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void resetScheduler();
    void rollBackInFlightWrites();
    void flushPipelines();

    // The decoder (ps2_vu1_decoder.cpp): no interpreter's state, so the generator runs it alone.
    static InstructionUsage decodeUpperUsage(uint32_t upper);
    static InstructionUsage decodeLowerUsage(Unit unit, uint32_t lower);
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    static DecodedInstructionPair decodeInstructionPair(Unit unit, const uint8_t *vuCode, uint32_t pc);
    // Marks the pairs' chain data (DecodedInstructionPair::chainInfo ...) and returns the chain depth (0: off).
    static uint32_t analyzeChains(DecodedInstructionPair *pairs, uint32_t pairCount);
    const DecodedInstructionPair *prepareDecodedCode(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);
};

#endif
