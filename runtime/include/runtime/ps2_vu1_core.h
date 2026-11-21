#ifndef PS2_VU1_CORE_H
#define PS2_VU1_CORE_H

// VU1's core: the state and pipelines the per-pair step works on (VU1Core), the step itself (src/lib/vu:
// ps2_vu1_step.h, the instructions in ps2_vu1_exec_lower.h and ps2_vu1_exec_upper.h, the pipelines in
// ps2_vu1_core.cpp), and what the core asks of whoever runs it (VU1Host). Freestanding (no library header, plain
// arrays): the runtime's interpreter (VU1Interpreter, runtime/ps2_vu1.h) is this core and its own parts, and a
// game module's ahead-of-time compiled microprograms (zx-vu1aot, src/lib/vu/ps2_vu1_aot.h) are compiled against
// it too and run on the interpreter's core, so both sides agree on its layout (vu1CoreLayout, checked at load:
// game_module/abi.h).

#include <stddef.h>
#include <stdint.h>

// The standard names the core uses: the library's in the runtime, the module's own subset in a game module.
#if defined(ZX_GAME_MODULE)
#include "game_module/freestanding_std.h"
#else
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>
#endif

#if defined(_MSC_VER)
#define PS2_VU1_ALWAYS_INLINE __forceinline
#else
#define PS2_VU1_ALWAYS_INLINE __attribute__((always_inline))
#endif

// In a game module the core is the module's own (its functions are compiled into it): a namespace of the
// module's, so modules linked into one program (ZX_GAME_MODULES=static) each keep theirs.
#if defined(ZX_GAME_MODULE)
#define PS2_VU1_CORE_CONCAT2(a, b) a##b
#define PS2_VU1_CORE_CONCAT(a, b) PS2_VU1_CORE_CONCAT2(a, b)
#define PS2_VU1_CORE_BEGIN inline namespace PS2_VU1_CORE_CONCAT(vu1_core_, ZX_GAME_MODULE_ID) {
#define PS2_VU1_CORE_END }
#else
#define PS2_VU1_CORE_BEGIN
#define PS2_VU1_CORE_END
#endif

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP
    uint64_t xgkickPackets; // debug statistic: PATH1 packets submitted

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

// What the core asks of the one running it, through plain function pointers (a module's core calls the runtime's):
// each gets `context`.
struct VU1Host
{
    void *context;
    // A PATH1 packet XGKICK completed (to the GIF, and to an observer).
    void (*packet)(void *context, const uint8_t *data, uint32_t size);
    // The pair at `pc` is about to issue, with the state as it sees it (the pair trace, VU1Core::tracingPairs).
    void (*tracePair)(void *context, uint32_t pc, const VU1State *state);
    // A reserved instruction, or a pipeline that cannot take a result (instruction: one of the codes the core
    // gives for those); the core stops the run.
    void (*reservedInstruction)(void *context, bool upper, uint32_t instruction);
};

// A microprogram compiled ahead of time (zx-vu1aot): runs `core` (a VU1Core) with `runContext` (its
// VU1Core::RunContext) until the run ends (true) or reaches a pc it has no code for (false: the interpreter goes on).
typedef bool (*VU1AotRun)(void *core, void *runContext);

PS2_VU1_CORE_BEGIN

struct VU1Core
{
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    struct RunContext
    {
        uint8_t *vuCode;
        uint32_t codeSize;
        uint8_t *vuData;
        uint32_t dataSize;
        uint64_t budgetEnd;
        bool programEnded;
    };
    enum class StepResult : uint8_t
    {
        Continue,
        Stop
    };

    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        VfAccess vfRead[2]{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    // Pre-decoded upper FMAC arithmetic (ADD/SUB/MUL/MADD/MSUB in all
    // forms, OPMULA/OPMSUB) for execFmacFast.
    struct FmacPlan
    {
        enum Kind : uint8_t
        {
            None = 0,
            Add,
            Sub,
            Mul,
            Madd,
            Msub,
            Opmula,
            Opmsub
        };
        enum Source : uint8_t
        {
            Broadcast = 0,
            Vector,
            QReg,
            IReg
        };
        Kind kind = None;
        Source source = Broadcast;
        bool toAcc = false;
        uint8_t bcLane = 0;
        uint8_t fs = 0, ft = 0, fd = 0, dest = 0;
        uint8_t vsLanes = 0, vtLanes = 0, accLanes = 0;
    };

    // execFmacFast specialized at compile time for one kind, source, dest
    // mask and target (VU1Interpreter::selectFmacFn), with the same results.
    using FmacFn = bool (*)(VU1Core &, const FmacPlan &);
    template <int Kind, int Source, int Dest, bool ToAcc>
    static bool fmacSpecialized(VU1Core &vu, const FmacPlan &plan);
    // The same, but the MAC/status result is not queued: its sticky bits are
    // ORed into `sticky` (ahead-of-time blocks, where nothing can see the
    // result before a later FMAC result that carries them, aotFoldSticky).
    template <int Kind, int Source, int Dest, bool ToAcc>
    static bool fmacSpecializedDefer(VU1Core &vu, const FmacPlan &plan, uint32_t &sticky);
    // The deferring FMAC's last one: queues its result with the sticky bits
    // collected so far (and clears them).
    template <int Kind, int Source, int Dest, bool ToAcc>
    static bool fmacSpecializedFold(VU1Core &vu, const FmacPlan &plan, uint32_t &sticky);
    enum : int
    {
        FlagQueue = 0,
        FlagDefer = 1,
        FlagFold = 2
    };
    template <int Kind, int Source, int Dest, bool ToAcc, int Mode>
    static bool fmacSpecializedImpl(VU1Core &vu, const FmacPlan &plan, uint32_t *sticky);
    template <int Mode>
    static bool fmacFlagResult(VU1Core &vu, uint32_t *sticky, uint32_t mac, uint32_t extraSticky);
    // Deferred sticky bits are collected raw (MAC bits | extra sticky << 16);
    // the status sticky bits they stand for:
    // (forced inline: in the ahead-of-time chunks, big functions, the compiler left it a call per fold)
    PS2_VU1_ALWAYS_INLINE static constexpr uint32_t deferredSticky(uint32_t collected)
    {
        return ((collected & 0x000Fu) != 0u ? 0x1u : 0u) | ((collected & 0x00F0u) != 0u ? 0x2u : 0u) |
               ((collected & 0x0F00u) != 0u ? 0x4u : 0u) | ((collected & 0xF000u) != 0u ? 0x8u : 0u) |
               ((collected >> 16) & 0xFu);
    }
    // Upper instructions without MAC/status results (MAX/MINI, ITOF/FTOI,
    // ABS, CLIP) specialized the same way; they use the plan's registers.
    enum UpperFastOp : int
    {
        UpperFastNone = 0,
        UpperFastMaxBc,
        UpperFastMiniBc,
        UpperFastMaxI,
        UpperFastMiniI,
        UpperFastMax,
        UpperFastMini,
        UpperFastItof,
        UpperFastFtoi,
        UpperFastAbs,
        UpperFastClip
    };
    // Op, dest and sub-field (broadcast lane or ITOF/FTOI scale) of an upper
    // instruction with a specialized handler (op UpperFastNone otherwise).
    static constexpr void upperFastKey(uint32_t upper, int &op, int &dest, int &sub)
    {
        const uint32_t code = upper & 0x3Fu;
        dest = static_cast<int>((upper >> 21) & 0xFu);
        sub = static_cast<int>(upper & 0x3u);
        op = UpperFastNone;
        if (code >= 0x10u && code <= 0x13u)
            op = UpperFastMaxBc;
        else if (code >= 0x14u && code <= 0x17u)
            op = UpperFastMiniBc;
        else if (code == 0x1Du)
            op = UpperFastMaxI;
        else if (code == 0x1Fu)
            op = UpperFastMiniI;
        else if (code == 0x2Bu)
            op = UpperFastMax;
        else if (code == 0x2Fu)
            op = UpperFastMini;
        else if (code >= 0x3Cu)
        {
            const uint32_t special = (upper & 0x3u) | ((upper >> 4) & 0x7Cu);
            if (special >= 0x10u && special <= 0x13u)
                op = UpperFastItof;
            else if (special >= 0x14u && special <= 0x17u)
                op = UpperFastFtoi;
            else if (special == 0x1Du)
                op = UpperFastAbs;
            else if (special == 0x1Fu)
                op = UpperFastClip;
        }
        if (op == UpperFastNone || op == UpperFastMaxI || op == UpperFastMiniI || op == UpperFastMax ||
            op == UpperFastMini || op == UpperFastAbs || op == UpperFastClip)
            sub = 0;
        if (op == UpperFastClip)
            dest = 0;
    }
    template <int Op, int Dest, int Sub>
    static bool upperFast(VU1Core &vu, const FmacPlan &plan);

    // execLower instantiated for one opcode (VU1Interpreter::selectLowerFn).
    using LowerFn = void (*)(VU1Core &, uint32_t, uint8_t *, uint32_t, uint32_t);
    template <uint32_t OpHi, uint32_t Funct, uint32_t Funct2>
    static void lowerOp(VU1Core &vu, uint32_t instr, uint8_t *vuData, uint32_t dataSize, uint32_t upperInstr);
    // Opcode fields of a lower instruction as execLower switches on them;
    // false for an opcode without its own instantiation.
    static constexpr bool lowerOpKey(uint32_t instr, uint32_t &opHi, uint32_t &funct, uint32_t &funct2)
    {
        opHi = (instr >> 25) & 0x7Fu;
        funct = 0u;
        funct2 = 0u;
        if (opHi != 0x40u)
            return true;
        funct = instr & 0x3Fu;
        if (funct >= 0x3Cu)
            funct2 = (instr & 0x3u) | ((instr >> 4) & 0x7Cu);
        return true;
    }

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;

        // Precomputed hazard and write-back data for the pair (from the
        // usages above), so issue only touches what the pair needs.
        enum : uint8_t
        {
            WaitFdiv = 1u,
            WaitEfu = 2u,
            WaitQ = 4u,
            WaitP = 8u,
            WaitXgkick = 16u
        };
        uint8_t vfReadCount = 0;
        uint8_t vfReadReg[4]{};
        uint8_t vfReadLanes[4]{};
        uint16_t viReadMask = 0;
        uint8_t accReadLanes = 0;
        uint8_t waitMask = 0;
        uint8_t upperWriteReg = 0, upperWriteLanes = 0, upperWriteLatency = 0;
        uint8_t lowerWriteReg = 0, lowerWriteLanes = 0, lowerWriteLatency = 0;
        uint8_t viWriteReg = 0, viWriteLatency = 0;
        uint8_t accWriteLanes = 0;
        bool upperNop = false; // NOP: execUpper has nothing to do
        bool lowerNop = false; // NOP / MOVE.0 vf0, vf0: execLower has nothing to do
        FmacPlan fmac{};
        FmacFn fmacFn = nullptr; // fmac.kind != None
        LowerFn lowerFn = nullptr; // !iBit && !lowerNop

        // Chain analysis (VU1Interpreter::analyzeChains). After at least
        // chainDepth pairs executed in sequence right before this one, only
        // the "near" reads can stall (every other read's last writer is at
        // least its latency back), and the writes need marking only if
        // chainMark.
        bool chainInfo = false;
        bool chainMark = true;
        uint8_t nearVfReadCount = 0;
        uint8_t nearVfReadReg[4]{};
        uint8_t nearVfReadLanes[4]{};
        uint16_t nearViReadMask = 0;
    };

    // A MAC/status or clip flag result, ready kFmacLatency cycles after
    // issue. MAC/status entries: value = mac | current status bits << 16 |
    // extra sticky bits << 20 (FMAC), or the sticky status bits (FSSET).
    // Clip entries: value = the clip register.
    struct FlagPipelineEntry
    {
        uint64_t issueCycle = 0;
        uint32_t value = 0;
        uint32_t writes = 0; // FlagWrites*
    };
    enum : uint32_t
    {
        FlagWritesMacStatus = 1u,
        FlagWritesSticky = 2u,
        FlagWritesClip = 4u,
        FlagStatusDropped = 0x100u, // a MAC/status entry whose status FSSET dropped
        FlagPrefixShift = 16u       // MAC/status entries: running OR of the sticky bits
    };
    static constexpr uint32_t kMaxFlagEntries = 64u;
    // MAC/status and clip results in issue order, one ring each (the fields
    // are independent, so applying each ring in order gives the same values
    // as one ring).
    struct FlagRing
    {
        FlagPipelineEntry entries[kMaxFlagEntries]{};
        uint32_t head = 0;
        uint32_t count = 0;
        // MAC/status ring: the OR of the sticky contributions of the FMAC
        // results queued since the ring was last empty (each entry keeps it
        // as of itself), so a batch of ready results applies as its last
        // one; entries that need the in-order loop (FSSET, dropped status)
        // are counted in `special`.
        uint32_t prefix = 0;
        uint32_t special = 0;
        // Entries left from an earlier run (budget stop): the status may
        // have been set in between, so they apply one by one.
        bool inOrder = false;
        bool ready(uint64_t cycle) const { return count != 0u && entries[head].issueCycle + 4u <= cycle; }
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    // A register write still inside its latency. Writes are applied at
    // issue; when a run stops on its cycle budget with writes in flight they
    // are rolled back and committed at their ready cycle by the next run,
    // so the stopped state matches the hardware pipeline.
    struct InFlightWrite
    {
        uint64_t readyCycle = 0;
        uint8_t kind = 0; // 0 VF, 1 VI, 2 ACC
        uint8_t reg = 0;
        uint8_t lanes = 0;
        uint32_t oldBits[4]{};
        uint32_t newBits[4]{};
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;
    static constexpr uint32_t kMaxWriteLatency = 4u; // VF/VI/ACC results
    static constexpr uint32_t kWriteLogSize = 16u;   // 4 cycles x 4 writes

    Unit m_unit = Unit::VU1;
    VU1State m_state{};
    VU1Host m_host{};
    // The pair trace is on (VU1Host::tracePair is called for every pair).
    bool m_tracingPairs = false;
    // The PATH1 packet being copied (XgkickPipeline::kBufferSize bytes, the runner's).
    uint8_t *m_xgkickPacket = nullptr;

    FlagRing m_macFlags; // MAC/status
    FlagRing m_clipFlags;
    ScalarPipelineEntry m_fdiv{};
    ScalarPipelineEntry m_efu[2]{};
    XgkickPipeline m_xgkick{};
    uint64_t m_xgkickSynced = 0; // PATH1 has run up to this cycle
    InFlightWrite m_writeLog[kWriteLogSize]{};
    uint32_t m_writeLogNext = 0;
    // Writes a budget stop rolled back, committed by the next run (the runner's storage: it adds them, the core
    // commits and drops them).
    InFlightWrite *m_deferredWrites = nullptr;
    uint32_t m_deferredWriteCount = 0;
    uint64_t m_vfReady[32][4]{};
    uint64_t m_viReady[16]{};
    uint64_t m_accReady[4]{};

    uint64_t m_cycle = 0;
    // Chain analysis: depth (0: off), pairs executed in sequence before the
    // current one, and the pc that continues the sequence.
    uint32_t m_chainDepth = 0;
    uint32_t m_seqPairs = 0;
    uint32_t m_expectedPc = ~0u;
    uint64_t m_pairsExecuted = 0;
    // Earliest readyCycle of any pending pipeline entry (~0 when none);
    // commitReadyPipelines does nothing before it.
    uint64_t m_nextCommitCycle = ~0ull;
    // Register and store results are written at issue; this is the cycle the
    // last of them would have committed. A finished run still waits for it
    // (the cycle count includes the write-back of the final instructions).
    uint64_t m_writesDoneCycle = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    bool m_fmacFastFlags = false;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    bool m_stopRequested = false;
    bool m_aotBlocksOff = false; // pair trace or ZX_VU1_AOT_BLOCKS=0
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    StepResult stepPair(const DecodedInstructionPair &decoded, bool cached, RunContext &rc);
    // The pair's instructions (upper, lower, I immediate), shared by the step
    // and ahead-of-time compiled blocks.
    // FixedVu1Data: data memory is VU1's 16 KiB (ahead-of-time blocks check
    // it on entry), so address masks are constants.
    // DeferFn: the pair's FMAC as fmacSpecializedDefer, sticky bits into *sticky.
    template <bool FixedVu1Data = false, auto DeferFn = nullptr>
    void execPairOps(const DecodedInstructionPair &decoded, RunContext &rc, uint32_t *sticky = nullptr);
    template <auto DeferFn>
    bool upperFastOp(const DecodedInstructionPair &decoded, uint32_t *sticky);
    // Adds sticky bits to the MAC/status result just queued in this cycle.
    void aotFoldSticky(uint32_t collected)
    {
        if (collected == 0u || m_macFlags.count == 0u)
            return;
        const uint32_t sticky = deferredSticky(collected);
        FlagPipelineEntry &entry = m_macFlags.entries[(m_macFlags.head + m_macFlags.count - 1u) % kMaxFlagEntries];
        m_macFlags.prefix |= sticky;
        entry.value |= sticky << 20;
        entry.writes |= sticky << FlagPrefixShift;
    }
    // Rare parts of the step, out of line so the per-pair code stays small.
    void tracePair();
    void stepBranch();
    void captureOldBits(const DecodedInstructionPair &decoded, int32_t oldVi, uint32_t (&oldBits)[4][4]);
    void stepProgramEnd(const DecodedInstructionPair &decoded, RunContext &rc, bool dHalt, bool tHalt);
    template <bool FixedVu1Data = false>
    void execLowerOf(const DecodedInstructionPair &decoded, RunContext &rc);

    void execUpper(uint32_t instr);
    // execUpper for the step: the FMAC fast paths fail rarely, and the
    // exact path would otherwise be inlined into every pair.
    void execUpperOutOfLine(uint32_t instr);
    static FmacPlan planFmac(uint32_t upper);
    bool execFmacFast(const FmacPlan &plan);
    bool finishFmacWithZeros(uint8_t kind, uint8_t dest, bool toAcc, uint8_t fd, const float *a, const float *b,
                             const float *acc, const uint32_t *resultBits, uint32_t extraSticky);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, uint32_t upperInstr);
    template <bool KnownOp, uint32_t KnownOpHi, uint32_t KnownFunct, uint32_t KnownFunct2>
    void execLowerImpl(uint32_t instr, uint8_t *vuData, uint32_t dataSize, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, double &result) const;
    uint8_t normalizeFmacExactResult(float &value, double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    // MAC/status/clip results are applied lazily: only when an instruction
    // reads them, when Q/P commit (they share the status register) and when
    // a run stops. Entries are applied in issue order up to the current
    // cycle, which gives the same values as committing every cycle.
    void applyReadyFlags()
    {
        if (m_macFlags.ready(m_cycle) || m_clipFlags.ready(m_cycle))
            applyReadyFlagsSlow();
    }
    void applyReadyFlagsSlow();
    // Queues a flag result issued this cycle; false when the ring is full.
    bool pushFlag(FlagRing &ring, uint32_t value, uint32_t writes);
    void applyMacFlagsInOrder(uint32_t count);
    bool queueFmacFlags(uint32_t mac, uint32_t extraSticky);
    void queueMacStatus(uint32_t mac, uint32_t status, uint32_t extraSticky);
    void suppressSameCycleFlags(bool status, bool clip);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void startXgkick(uint32_t qwordAddress);

    void logPairWrites(const DecodedInstructionPair &decoded, uint8_t writtenVi, const uint32_t (&oldBits)[4][4]);
    void supersedeDeferredWrites(const DecodedInstructionPair &decoded, uint8_t writtenVi);
    void commitDeferredWrites();
    void noteCommitCycle(uint64_t cycle)
    {
        if (cycle < m_nextCommitCycle)
            m_nextCommitCycle = cycle;
    }
    void commitReadyPipelines()
    {
        if (m_cycle >= m_nextCommitCycle)
            commitPipelinesSlow();
    }
    void commitPipelinesSlow();
    // m_state.cycles follows m_cycle at the end of a run (and for the pair
    // trace), not on every cycle.
    void advanceOneCycle()
    {
        ++m_cycle;
        commitReadyPipelines();
    }
    // PATH1 runs lazily: it only reads VU data memory, so its progress over
    // the cycles since m_xgkickSynced is caught up in one step before a
    // store, before anything reads its state (a waiting XGKICK, the end of
    // a run) and gives the same packets on the same cycles.
    void syncXgkick()
    {
        if (m_xgkick.active && m_cycle > m_xgkickSynced)
            progressXgkick(static_cast<uint32_t>(std::min<uint64_t>(m_cycle - m_xgkickSynced, 0x40000000u)));
        m_xgkickSynced = m_cycle;
    }
    void advanceTo(uint64_t targetCycle);

    // Ahead-of-time compiled blocks (ps2_vu1_aot.h, zx-vu1aot): straight-line
    // code for a basic block that keeps the issue cycles of its pairs in
    // locals and runs only the instructions, with the same results as
    // stepPair. A block runs only when no budget stop, trace or deferred
    // write can happen inside it (aotBlockEntryOk); anywhere else, and on
    // any rare event inside it, the per-pair code takes over (aotBlockBail).
    bool aotBlockEntryOk(const RunContext &rc, uint32_t maxCycles) const
    {
        return m_cycle + maxCycles < rc.budgetEnd && !m_aotBlocksOff && !m_stopRequested &&
               m_deferredWriteCount == 0u && !m_state.branchPending && !m_state.haltAfterDelaySlot && !m_state.ebit;
    }
    // Earliest cycle at which time passing has an effect (a commit), 0 when
    // every cycle needs the slow path (stop requested).
    uint64_t aotNextEvent() const
    {
        return m_stopRequested ? 0u : m_nextCommitCycle;
    }
    // Every cycle after `done` up to `target` passes (commits, PATH1).
    void aotSyncTo(uint64_t done, uint64_t target)
    {
        // The usual case, a DIV/SQRT/RSQRT result and nothing else to
        // commit: what commitPipelinesSlow does on Q's cycle, then the rest
        // of the cycles have nothing to do.
        if (m_fdiv.valid && m_fdiv.readyCycle <= target && m_fdiv.readyCycle > done && !m_efu[0].valid &&
            !m_efu[1].valid && m_deferredWriteCount == 0u)
        {
            m_cycle = m_fdiv.readyCycle;
            applyReadyFlags(); // flags first: Q's D/I bits go into the same status register
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
            m_nextCommitCycle = ~0ull;
            m_cycle = target;
            return;
        }
        m_cycle = done;
        advanceTo(target);
    }
    // The ready cycle of a pair's FDIV/EFU/PATH1 waits (and `ready`).
    uint64_t aotWaitReady(const DecodedInstructionPair &decoded, uint64_t ready);
    void aotMarkWrites(const DecodedInstructionPair &decoded, uint64_t cycle);
    // Leave a block before one of its pairs: the state the per-pair code
    // has there. Cycles up to `done` have passed; the step of the previous
    // pair ends at `next`. `marks` lists pairs (index into `pairs`) and
    // their issue cycles whose writes may still be pending.
    struct AotMark
    {
        uint32_t index;
        uint64_t cycle;
    };
    // stepPair for a pair the block code stopped at (out of line).
    void aotStepPairSlow(const DecodedInstructionPair &decoded, RunContext &rc);
    void aotBlockBail(const DecodedInstructionPair *pairs, const AotMark *marks, uint32_t markCount,
                      uint32_t executed, uint32_t pc, uint64_t done, uint64_t next);
    void progressXgkick(uint32_t cycles = 1u);
    // Cycles until PATH1 copies its next qword (one every two cycles).
    uint32_t cyclesToNextXgkickCopy() const { return m_xgkick.cycleCredit >= 1u ? 1u : 2u; }
    // Cycles until PATH1 has copied the rest of the current GIF tag (at
    // least one qword). PATH1 can only finish at a tag's end, so a wait for
    // it can step from tag end to tag end. Synced PATH1 only (credit 0 or 1).
    uint32_t cyclesToXgkickTagEnd() const
    {
        const uint32_t qwords = m_xgkick.currentTagEnd > m_xgkick.copiedBytes ? (m_xgkick.currentTagEnd - m_xgkick.copiedBytes) / 16u : 1u;
        return 2u * qwords - std::min(m_xgkick.cycleCredit, 1u);
    }
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded);
    uint64_t calculateNearReadyCycle(const DecodedInstructionPair &decoded);
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    float normalizeOperand(float value) const;
    void normalizeOperands(const float *in, float *out) const;
    bool fmacOperandsInSafeRange(uint8_t index, uint8_t dest, const float *vs, const float *vt,
                                 const float *acc, float q, float i) const;
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const { return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu; }
    int32_t readBranchVi(uint8_t reg) const
    {
        if (reg == 0u)
            return 0;
        if (m_viBranchBackupValid && m_viBranchBackupReg == reg)
            return m_viBranchBackupValue;
        return m_state.vi[reg];
    }
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue)
    {
        if (reg == 0u)
            return;
        m_viBranchBackupValue = oldValue;
        m_viBranchBackupReg = reg;
        m_viBranchBackupValid = true;
    }
    // Through the host, which says what it was; the run stops.
    void reportReservedInstruction(bool upper, uint32_t instruction)
    {
        m_host.reservedInstruction(m_host.context, upper, instruction);
        m_stopRequested = true;
    }
    float broadcast(const float *vf, uint8_t bc);
};

// The core's layout: its fields' offsets and the sizes of what it holds, hashed (FNV-1a). A game module's
// microprograms run on the runtime's core only when the module was compiled with the same layout.
#define PS2_VU1_CORE_FIELDS(X) \
    X(m_unit) X(m_state) X(m_host) X(m_tracingPairs) X(m_xgkickPacket) X(m_macFlags) X(m_clipFlags) X(m_fdiv) \
    X(m_efu) X(m_xgkick) X(m_xgkickSynced) X(m_writeLog) X(m_writeLogNext) X(m_deferredWrites) \
    X(m_deferredWriteCount) X(m_vfReady) X(m_viReady) X(m_accReady) X(m_cycle) X(m_chainDepth) X(m_seqPairs) \
    X(m_expectedPc) X(m_pairsExecuted) X(m_nextCommitCycle) X(m_writesDoneCycle) X(m_efuResourceReady) \
    X(m_workingClip) X(m_currentUpperInstruction) X(m_fmacFastFlags) X(m_viBranchBackupValue) \
    X(m_viBranchBackupReg) X(m_viBranchBackupValid) X(m_activeVuData) X(m_activeVuDataSize) X(m_stopRequested) \
    X(m_aotBlocksOff) X(m_pendingHaltD) X(m_pendingHaltT)
constexpr uint32_t vu1CoreLayout()
{
    uint32_t hash = 2166136261u;
    auto add = [&hash](size_t value) { hash = (hash ^ static_cast<uint32_t>(value)) * 16777619u; };
#define PS2_VU1_CORE_HASH_FIELD(field) add(offsetof(VU1Core, field));
    PS2_VU1_CORE_FIELDS(PS2_VU1_CORE_HASH_FIELD)
#undef PS2_VU1_CORE_HASH_FIELD
    add(sizeof(VU1Core));
    add(sizeof(VU1State));
    add(offsetof(VU1State, cycles));
    add(offsetof(VU1State, xgkickPackets));
    add(offsetof(VU1State, branchDelay));
    add(sizeof(VU1Host));
    add(sizeof(VU1Core::RunContext));
    add(offsetof(VU1Core::RunContext, budgetEnd));
    add(offsetof(VU1Core::RunContext, programEnded));
    add(sizeof(VU1Core::FlagRing));
    add(offsetof(VU1Core::FlagRing, head));
    add(offsetof(VU1Core::FlagRing, inOrder));
    add(sizeof(VU1Core::FlagPipelineEntry));
    add(sizeof(VU1Core::ScalarPipelineEntry));
    add(offsetof(VU1Core::ScalarPipelineEntry, valid));
    add(sizeof(VU1Core::InFlightWrite));
    add(offsetof(VU1Core::InFlightWrite, newBits));
    add(sizeof(VU1Core::XgkickPipeline));
    add(offsetof(VU1Core::XgkickPipeline, currentTagEop));
    return hash;
}
static_assert(__is_standard_layout(VU1Core), "VU1Core is shared with game modules: a standard-layout struct");

PS2_VU1_CORE_END

#endif // PS2_VU1_CORE_H
