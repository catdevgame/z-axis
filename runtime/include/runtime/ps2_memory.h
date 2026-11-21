#ifndef PS2_MEMORY_H
#define PS2_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <iostream>
#include <memory>
#include <condition_variable>
#include <mutex>

#include "gs/ps2_gif_arbiter.h"
#include "runtime/guest_map.h"
#include "ps2_async_stage.h"
#include "runtime/ps2_memory_map.h"
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif

class GS;
// A packet registered with PS2Memory::setVif1Substitute (ps2_memory.cpp).
struct Vif1SubstituteState;

inline constexpr uint32_t PS2_FIO_O_RDONLY = 0x0001;
inline constexpr uint32_t PS2_FIO_O_WRONLY = 0x0002;
inline constexpr uint32_t PS2_FIO_O_RDWR = 0x0003;
inline constexpr uint32_t PS2_FIO_O_NBLOCK = 0x0010;
inline constexpr uint32_t PS2_FIO_O_APPEND = 0x0100;
inline constexpr uint32_t PS2_FIO_O_CREAT = 0x0200;
inline constexpr uint32_t PS2_FIO_O_TRUNC = 0x0400;
inline constexpr uint32_t PS2_FIO_O_EXCL = 0x0800;
inline constexpr uint32_t PS2_FIO_O_NOWAIT = 0x8000;

inline constexpr uint32_t PS2_FIO_SEEK_SET = 0;
inline constexpr uint32_t PS2_FIO_SEEK_CUR = 1;
inline constexpr uint32_t PS2_FIO_SEEK_END = 2;

inline constexpr uint32_t PS2_FIO_S_IFDIR = 0x1000;
inline constexpr uint32_t PS2_FIO_S_IFREG = 0x2000;


inline std::atomic<uint8_t *> &ps2ScratchpadHostPtrStorage()
{
    static std::atomic<uint8_t *> ptr{nullptr};
    return ptr;
}

inline void ps2SetScratchpadHostPtr(uint8_t *ptr)
{
    ps2ScratchpadHostPtrStorage().store(ptr, std::memory_order_relaxed);
}

inline uint8_t *ps2GetScratchpadHostPtr()
{
    return ps2ScratchpadHostPtrStorage().load(std::memory_order_relaxed);
}

inline bool ps2IsScratchpadAddress(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return true;
    }

    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        return lower >= PS2_SCRATCHPAD_BASE &&
               lower < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);
    }

    return false;
}

inline uint32_t ps2ScratchpadOffset(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return addr - PS2_SCRATCHPAD_BASE;
    }

    const uint32_t lower = addr & 0x7FFFFFFFu;
    return lower - PS2_SCRATCHPAD_BASE;
}

inline bool ps2ResolveGuestPointer(uint32_t addr, uint32_t &offset, bool &scratch)
{
    if (ps2IsScratchpadAddress(addr))
    {
        scratch = true;
        offset = ps2ScratchpadOffset(addr);
        return true;
    }

    uint32_t phys = 0;
    if (addr < 0x20000000u)
    {
        phys = addr;
    }
    else if ((addr >= 0x20000000u && addr < 0x40000000u) ||
             (addr >= 0x80000000u && addr < 0xC0000000u))
    {
        phys = addr & 0x1FFFFFFFu;
    }

    if (phys >= PS2_RAM_SIZE)
    {
        phys &= PS2_RAM_MASK;
    }

    scratch = false;
    offset = phys;
    return true;
}
inline uint8_t *getMemPtr(uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

inline const uint8_t *getConstMemPtr(const uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        const uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

// PS2 GS (Graphics Synthesizer) registers
struct GSRegisters
{
    uint64_t pmode;    // Pixel mode
    uint64_t smode1;   // Sync mode 1
    uint64_t smode2;   // Sync mode 2
    uint64_t srfsh;    // Refresh control
    uint64_t synch1;   // Synchronization control 1
    uint64_t synch2;   // Synchronization control 2
    uint64_t syncv;    // Synchronization control V
    uint64_t dispfb1;  // Display buffer 1
    uint64_t display1; // Display area 1
    uint64_t dispfb2;  // Display buffer 2
    uint64_t display2; // Display area 2
    uint64_t extbuf;   // External buffer
    uint64_t extdata;  // External data
    uint64_t extwrite; // External write
    uint64_t bgcolor;  // Background color
    std::atomic<uint64_t> csr;
    std::atomic<uint64_t> vsyncTick;
    uint64_t imr;      // Interrupt mask
    uint64_t busdir;   // Bus direction
    uint64_t siglblid; // Signal label ID
};
static_assert(sizeof(GSRegisters) == (20u * sizeof(uint64_t)), "GSRegisters layout changed unexpectedly");
static_assert(alignof(GSRegisters) == alignof(uint64_t), "GSRegisters alignment must remain 64-bit");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "GS CSR atomic must be lock-free on all supported targets");

// PS2 VIF (VPU Interface) registers
struct VIFRegisters
{
    uint32_t stat;   // Status
    uint32_t fbrst;  // VIF Force Break
    uint32_t err;    // Error status
    uint32_t mark;   // Interrupt control
    uint32_t cycle;  // Transfer mode
    uint32_t mode;   // Mode control
    uint32_t num;    // Data amount counter
    uint32_t mask;   // Data mask
    uint32_t code;   // VIFcode
    uint32_t itops;  // ITOP save
    uint32_t base;   // Base address
    uint32_t ofst;   // Offset
    uint32_t tops;   // TOPS
    uint32_t itop;   // ITOP
    uint32_t top;    // TOP
    uint32_t row[4]; // Transfer row data
    uint32_t col[4]; // Transfer column data
};
static_assert(sizeof(VIFRegisters) == (23u * sizeof(uint32_t)), "VIFRegisters layout changed unexpectedly");

// PS2 DMA registers
struct DMARegisters
{
    uint32_t chcr; // Channel control
    uint32_t madr; // Memory address
    uint32_t qwc;  // Quadword count
    uint32_t tadr; // Tag address
    uint32_t asr0; // Address stack 0
    uint32_t asr1; // Address stack 1
    uint32_t sadr; // Source address
};
static_assert(sizeof(DMARegisters) == (7u * sizeof(uint32_t)), "DMARegisters layout changed unexpectedly");

struct JumpTable
{
    uint32_t address = 0;          // Base address of the jump table
    uint32_t baseRegister = 0;     // Register used for index
    std::vector<uint32_t> targets; // Jump targets
};

class PS2Memory
{
public:
    PS2Memory();
    ~PS2Memory();

    PS2Memory(const PS2Memory &) = delete;
    PS2Memory &operator=(const PS2Memory &) = delete;
    PS2Memory(PS2Memory &&) = delete;
    PS2Memory &operator=(PS2Memory &&) = delete;

    // Initialize memory
    bool initialize(size_t ramSize = PS2_RAM_SIZE);
    // VU0's micro and data memory alone, zeroed, and VIF0's registers, for a host that has none of the rest of the
    // machine: zx-fncheck, whose replays start VU0 microprograms (VCALLMS) after it sends the game's own upload
    // through processVIF0Data. initialize() makes them with everything else.
    void initializeVu0Memory();
    // VU0's code and data memory (initialized) set to these copies of theirs, PS2_VU0_CODE_SIZE and
    // PS2_VU0_DATA_SIZE bytes: the code's generation moves on if it changes (decoded microprograms are made again).
    void setVu0Memory(const uint8_t *code, const uint8_t *data);

    // Memory access methods
    uint8_t *getRDRAM() { return m_rdram; }
    uint8_t *getScratchpad() { return m_scratchpad; }
    uint8_t *getIOPRAM() { return iop_ram; }
    uint64_t dmaStartCount() const { return m_dmaStartCount.load(std::memory_order_relaxed); }
    uint64_t gifCopyCount() const { return m_gifCopyCount.load(std::memory_order_relaxed); }
    uint64_t gsWriteCount() const { return m_gsWriteCount.load(std::memory_order_relaxed); }
    uint64_t vifWriteCount() const { return m_vifWriteCount.load(std::memory_order_relaxed); }
    uint64_t getVU0CodeGeneration() const { return m_vu0CodeGeneration.load(std::memory_order_relaxed); }
    uint64_t getVU1CodeGeneration() const { return m_vu1CodeGeneration.load(std::memory_order_relaxed); }

    // Read/write memory
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    uint64_t read64(uint32_t address);
    __m128i read128(uint32_t address);

    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    void write64(uint32_t address, uint64_t value);
    void write128(uint32_t address, __m128i value);

    // TLB handling
    uint32_t translateAddress(uint32_t virtualAddress);
    bool tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const;
    bool tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid);
    int32_t tlbProbe(uint32_t vpn) const;
    size_t tlbEntryCount() const { return m_tlbEntries.size(); }

    // Hardware register interface
    bool writeIORegister(uint32_t address, uint32_t value);
    uint32_t readIORegister(uint32_t address);

    // EE timers advance from the scheduler's emulated EE-cycle clock. The
    // returned mask uses bits 0..3 for newly raised TIM0..TIM3 interrupts.
    // (Inline: it runs at every charge of the scheduler; with no timer counting and no GIF FIFO count to
    // drain it has nothing to do.)
    uint32_t advanceEeTimers(uint64_t eeCycles) noexcept
    {
        return (m_eeTimersCounting != 0u || m_gifFqcLive) ? advanceCountingEeTimers(eeCycles) : 0u;
    }
    [[nodiscard]] uint64_t cyclesUntilNextEeTimerInterrupt() const noexcept;
    void resetEeTimers() noexcept;

    using GifPacketCallback = std::function<void(const uint8_t *, uint32_t)>;
    void setGifPacketCallback(GifPacketCallback cb) { m_gifPacketCallback = std::move(cb); }
    void setGifArbiter(GifArbiter *arbiter) { m_gifArbiter = arbiter; }

    using Vu1MscalCallback = std::function<void(uint32_t startPC, uint32_t top, uint32_t itop)>;
    void setVu1MscalCallback(Vu1MscalCallback cb) { m_vu1MscalCallback = std::move(cb); }
    using Vu1MscntCallback = std::function<void(uint32_t top, uint32_t itop)>;
    void setVu1MscntCallback(Vu1MscntCallback cb) { m_vu1MscntCallback = std::move(cb); }

    // Asynchronous graphics (ZX_VU1_ASYNC): VIF1 streams, VU1 runs, the
    // GIF arbiter and PATH3 run in order on one worker, the GS packet
    // processing they feed on a second one. The EE drains them (syncVif1 /
    // syncGraphics) before it observes anything they change, so it sees the
    // same state as with synchronous processing.
    void setAsyncGraphics(bool enabled);
    bool asyncGraphics() const { return m_asyncGraphics; }
    void stopAsyncGraphics();
    using GsPacketSink = std::function<void(const uint8_t *, uint32_t)>;
    // Where the GIF arbiter's output goes (the GS frontend).
    void setGsPacketSink(GsPacketSink sink) { m_gsPacketSink = std::move(sink); }
    // Called on the GS worker when its queue runs dry (async graphics).
    void setGsIdleSink(std::function<void()> sink) { m_gsIdleSink = std::move(sink); }
    void deliverGifPacket(const uint8_t *data, uint32_t sizeBytes);
    // Asked on the EE thread before VIF1 work is queued: false runs it
    // synchronously (VU1 D/T stops, which write the EE context's VPU-STAT).
    void setVu1AsyncAllowed(std::function<bool()> allowed) { m_vu1AsyncAllowed = std::move(allowed); }
    enum SyncReason : uint32_t
    {
        SyncVifReg = 0,
        SyncGifReg,
        SyncVuMem,
        SyncGsPriv,
        SyncGsHle,
        SyncGifChain,
        SyncSynchronous,
        SyncOther,
        SyncReasonCount
    };
    void syncVif1(SyncReason reason = SyncOther) const;
    void syncGraphics(SyncReason reason = SyncOther) const;
    bool onVu1Worker() const { return m_vifStage && m_vifStage->isWorkerThread(); }
    bool onGsWorker() const { return m_gsStage && m_gsStage->isWorkerThread(); }
    // Runs fn on the GS worker after all GS work queued so far (VIF1/VU1
    // output included), or at once without the workers. For GS HLE calls the
    // EE does not read back (buffer swaps). At most one such call is in
    // flight: the next one waits for the previous to finish, so the EE runs
    // at most one frame ahead of the GS.
    void queueGsWork(std::function<void()> fn);
    // Runs fn on the GS worker in stream order: after all GS work queued so
    // far (VIF1/VU1 output included) and before anything queued later, or
    // at once without the workers. Never waits. For side-band notes a game
    // override hands the renderer (GS::noteTextureObjects).
    void postGsSideband(std::function<void()> fn);
    // Drains that had to wait, per reason (statistics).
    uint64_t syncWaits(SyncReason reason) const { return m_syncWaits[reason].load(std::memory_order_relaxed); }

    // ---- Native draws: VU1 work substituted in the VIF1 stream ----------
    //
    // A game override that draws a mesh natively (runtime/gs/gs_native.h)
    // registers the DMA packet the game built for it: the CALL tag at
    // tagAddress and the chain it calls. When VIF1's chain reaches that tag
    // (EE thread, as the DMA reads it), the VIFcodes of the packet still go
    // to VIF1 and keep every register, the EE-side scan and the interrupts
    // exactly as they are; only its microprogram runs (MSCAL, MSCALF, MSCNT)
    // do not start VU1. In place of the first one, `begin` (called there, on
    // the EE thread, after any interrupt handler before it has run) returns
    // the GS-side work, which runs in stream order where VU1's output would
    // have gone (PS2Memory::postGsSideband). Game RAM and EE timing do not
    // change; VU1's memory and registers are never read back by the game.
    struct Vif1Substitute
    {
        // The DMA tag expected at the address: ID and ADDR are compared
        // (QWC is filled in later by the game's packet library).
        uint64_t tag = 0;
        // Empty result: VU1 runs as usual for this packet.
        std::function<std::function<void()>()> begin;
    };
    // Replaces what is registered at the address (EE thread).
    void setVif1Substitute(uint32_t tagAddress, Vif1Substitute substitute);
    void clearVif1Substitute(uint32_t tagAddress);
    struct Vif1SubstituteStats
    {
        uint64_t packets = 0;      // reached and taken
        uint64_t declined = 0;     // begin() returned nothing
        uint64_t mismatched = 0;   // another tag at the address (stale)
        uint64_t runsSkipped = 0;  // MSCAL/MSCALF/MSCNT not run (EE side)
        uint64_t unpacksSkipped = 0; // UNPACKs whose data was not written (EE side marks)
        uint64_t drawsPosted = 0;  // GS work posted (worker side)
        uint64_t drawsLost = 0;    // GS work never reached (stream dropped)
    };
    Vif1SubstituteStats vif1SubstituteStats() const;
    // MSCAL/MSCALF/MSCNT VIFcodes of substituted packets carry these in
    // NUM (ignored by those VIFcodes): no VU1 run; Begin also posts the
    // GS work whose sequence number is in IMMEDIATE.
    static constexpr uint8_t kVif1NativeSkip = 0xB0;
    static constexpr uint8_t kVif1NativeBegin = 0xB1;
    // UNPACKs of a substituted packet after its first run carry this in
    // IMMEDIATE (a bit UNPACK does not use): their data is not written.
    static constexpr uint32_t kVif1NativeUnpackSkip = 0x2000u;
    // Native draws made from the engine's draw records (render_path =
    // records, zge/host/class_draws.cpp): no packet of VU1 work is built for
    // them. In its place the packet gets one VIF NOP, the draw's anchor:
    // NUM kVif1NativeAnchor (NOP ignores NUM and IMMEDIATE), IMMEDIATE the
    // slot addVif1NativeAnchor gave. When the EE-side scan reaches it (EE
    // thread, after any interrupt handler before it has run), `begin` is
    // called and the GS work it returns runs there in stream order, as a
    // substituted packet's does. A slot is taken once; one never reached is
    // replaced when its number comes round again.
    static constexpr uint8_t kVif1NativeAnchor = 0xB2;
    using Vif1NativeBegin = std::function<std::function<void()>()>;
    uint16_t addVif1NativeAnchor(Vif1NativeBegin begin);
    static uint32_t vif1NativeAnchorCode(uint16_t slot)
    {
        return (static_cast<uint32_t>(kVif1NativeAnchor) << 16) | slot; // NOP (0x00), NUM, IMMEDIATE
    }
    struct Vif1AnchorStats
    {
        uint64_t added = 0, reached = 0, declined = 0, replaced = 0;
    };
    Vif1AnchorStats vif1AnchorStats() const;
    // Worker CPU time so far in ms (statistics; 0 without the workers).
    double vif1WorkerCpuMs() const { return m_vifStage ? m_vifStage->cpuMilliseconds() : 0.0; }
    double gsWorkerCpuMs() const { return m_gsStage ? m_gsStage->cpuMilliseconds() : 0.0; }

    uint8_t *getVU1Code() { return m_vu1Code; }
    const uint8_t *getVU1Code() const { return m_vu1Code; }
    uint8_t *getVU1Data() { return m_vu1Data; }
    const uint8_t *getVU1Data() const { return m_vu1Data; }
    uint8_t *getVU0Code() { return m_vu0Code; }
    const uint8_t *getVU0Code() const { return m_vu0Code; }
    uint8_t *getVU0Data() { return m_vu0Data; }
    const uint8_t *getVU0Data() const { return m_vu0Data; }

    bool isPath3Masked() const { return m_path3Masked; }
    void flushMaskedPath3Packets(bool drainImmediately = true);

    void submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately = true, bool path2DirectHl = false);
    void processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount);
    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr);
    bool tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr);
    void processVIF0Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF0Data(const uint8_t *data, uint32_t sizeBytes);
    void processVIF1Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF1Data(const uint8_t *data, uint32_t sizeBytes);
    // Tests: UNPACK always through the general per-lane loop (no fast path).
    static inline bool s_vif1GeneralUnpack = false;
    // Debug (ZX_VIF1_PREFETCH=1): read a whole VIF1 transfer at the kick,
    // as before chains were fetched as VIF1 consumes them, so the part behind
    // an interrupt stall does not see what the handler wrote.
    static inline bool s_vif1PrefetchChain = false;
    void processPendingTransfers();
    std::vector<uint32_t> consumeCompletedDmacCauses();
    // INTC causes raised by devices (VIF1 interrupt bit), for the EE scheduler.
    std::vector<uint32_t> consumePendingIntcCauses();
    [[nodiscard]] bool isVif1StalledOnInterrupt() const noexcept { return m_vif1InterruptStalled; }

    int pollDmaRegisters();

    // Track code modifications for self-modifying code
    void registerCodeRegion(uint32_t start, uint32_t end);
    bool isCodeAddress(uint32_t address) const;
    bool isCodeModified(uint32_t address, uint32_t size);
    void clearModifiedFlag(uint32_t address, uint32_t size);

    // GS register accessors
    GSRegisters &gs() { return gs_regs; }
    const GSRegisters &gs() const { return gs_regs; }
    uint8_t *getGSVRAM() { return m_gsVRAM; }
    const uint8_t *getGSVRAM() const { return m_gsVRAM; }
    bool hasSeenGifCopy() const { return m_seenGifCopy; }
    // DISPFB1 or DISPFB2 set (by the EE, the HLE's display environment or a
    // game's override), with the times it changed counted. The registers'
    // values alone repeat when a double-buffered game swaps twice between two
    // host frames (A, B, A). Each change is a new game frame for the window:
    // the display change sink (GS::capturePresentationFrame) takes its
    // picture on the GS worker, in stream order, behind the frame's drawing:
    // after the GS work that swapped (an HLE buffer swap), or at the next
    // VBlank for a change the EE made itself (displayAtVBlank: by then it
    // has written the rest of the display registers too).
    void setDisplayBuffer(uint64_t &dispfb, uint64_t value)
    {
        if (dispfb != value)
        {
            m_displayChanges.fetch_add(1u, std::memory_order_relaxed);
            if (onGsWorker())
                m_displayChangedInGsWork = true;
            else
                m_displayChangedByEe.store(true, std::memory_order_relaxed);
        }
        dispfb = value;
    }
    uint64_t displayChanges() const { return m_displayChanges.load(std::memory_order_relaxed); }
    // Where display changes go (on the GS worker, or at once without it).
    void setDisplayChangeSink(std::function<void()> sink) { m_displayChangeSink = std::move(sink); }
    // EE thread, at every VBlank start: hands a change the EE made since the
    // last one to the sink.
    void displayAtVBlank()
    {
        if (m_displayChangeSink && m_displayChangedByEe.exchange(false, std::memory_order_relaxed))
            postGsSideband(m_displayChangeSink);
    }
    // Main RAM (PS2_RAM_SIZE) and the scratchpad (16 KB): both in the guest map (runtime/guest_map.h), RAM at
    // its base, the scratchpad at 0x70000000 from it.
    guest_map::Map m_guestMap;
    uint8_t *m_rdram;
    uint8_t *m_scratchpad;

    // IOP RAM (2MB)
    uint8_t *iop_ram;

    bool m_seenGifCopy;
    std::atomic<uint64_t> m_dmaStartCount{0};
    std::atomic<uint64_t> m_gifCopyCount{0};
    std::atomic<uint64_t> m_gsWriteCount{0};
    std::atomic<uint64_t> m_vifWriteCount{0};
    std::atomic<uint64_t> m_vu0CodeGeneration{0};
    std::atomic<uint64_t> m_vu1CodeGeneration{0};
    std::atomic<uint64_t> m_displayChanges{0}; // setDisplayBuffer
    std::function<void()> m_displayChangeSink;
    bool m_displayChangedInGsWork = false;           // GS worker: by the queued work running
    std::atomic<bool> m_displayChangedByEe{false};    // since the last VBlank
    // I/O registers
    std::unordered_map<uint32_t, uint32_t> m_ioRegisters;
    bool m_gifFqcLive = false; // GIF_STAT's FQC field is set and no EE time has passed since (advanceEeTimers)

    // Registers
    GSRegisters gs_regs;
    uint8_t *m_gsVRAM;
    VIFRegisters vif0_regs;
    VIFRegisters vif1_regs;
    DMARegisters dma_regs[10]; // 10 DMA channels

    // TLB entries
    struct TLBEntry
    {
        uint32_t vpn;
        uint32_t pfn;
        uint32_t mask;
        bool valid;
    };

    std::vector<TLBEntry> m_tlbEntries;

    bool m_asyncGraphics = false;
    std::unique_ptr<Ps2AsyncStage> m_vifStage;
    std::unique_ptr<Ps2AsyncStage> m_gsStage;
    GsPacketSink m_gsPacketSink;
    std::function<void()> m_gsIdleSink;
    std::function<bool()> m_vu1AsyncAllowed;
    // GS packets from the VIF1 worker, handed to the GS stage in batches.
    std::vector<uint8_t> m_gsBatch;
    std::vector<uint32_t> m_gsBatchSizes;
    mutable std::array<std::atomic<uint64_t>, SyncReasonCount> m_syncWaits{};
    uint64_t m_gsWorkPosted = 0;
    std::atomic<uint64_t> m_gsWorkDone{0};
    std::mutex m_gsWorkMutex;
    std::condition_variable m_gsWorkCv;
    void flushGsBatch();
    bool vif1AsyncNow();
    void postVif1Work(std::function<void()> work);
    // EE-side VIF1 state as it will be once the worker has processed what
    // was queued so far (scanVif1Stream): the registers and PATH3 mask the
    // VIFcodes set (not the unpacked data or VU1 results), the pending PATH2
    // image data and a stall due before the next command. VIF1/GIF register
    // reads are answered from it without waiting for the worker.
    VIFRegisters m_vif1Shadow{};
    bool m_path3MaskedShadow = false;
    uint32_t m_vif1ScanPendingImageQwc = 0u;
    bool m_vif1ScanStallAfter = false;
    void resyncVif1Shadow(bool reset);
    bool writeVif1RegisterQueued(uint32_t address, uint32_t value);
    static uint32_t vif1UnpackBytes(uint32_t cmd, uint32_t cycle);
    // With `incomplete`, a VIFcode whose data runs past the end is left
    // unscanned (the return value is where it starts, *incomplete is set),
    // so the scan can go on when more of the stream has been fetched.
    // `mscals`: positions (from data) of the MSCAL/MSCALF/MSCNT VIFcodes
    // and of the native draws' anchors (NOP, NUM kVif1NativeAnchor)
    // scanned, for native draws.
    uint32_t scanVif1Stream(const uint8_t *data, uint32_t sizeBytes, bool &stalls, bool *incomplete = nullptr,
                            std::vector<uint32_t> *mscals = nullptr);
    // Native draws: substituted packets by tag address (EE thread), the GS
    // work begun on the EE in stream order (taken by processVIF1Data).
    std::unordered_map<uint32_t, std::shared_ptr<Vif1SubstituteState>> m_vif1Substitutes;
    // Native draws' anchors by slot (EE thread; addVif1NativeAnchor).
    std::unordered_map<uint16_t, Vif1NativeBegin> m_vif1Anchors;
    uint16_t m_vif1AnchorNext = 0;
    Vif1AnchorStats m_vif1AnchorStats; // (under m_vif1SubstituteStatsMutex)
    struct Vif1NativeWork
    {
        uint16_t sequence = 0;
        std::function<void()> work;
    };
    std::mutex m_vif1NativeMutex;
    std::deque<Vif1NativeWork> m_vif1NativeQueue;
    uint16_t m_vif1NativeSequence = 0;
    mutable std::mutex m_vif1SubstituteStatsMutex;
    Vif1SubstituteStats m_vif1SubstituteStats;
    void substituteVif1Runs(uint8_t *stream, const std::vector<uint32_t> &mscals,
                            const std::vector<std::pair<size_t, std::shared_ptr<Vif1SubstituteState>>> &owners);
    void runVif1NativeWork(uint16_t sequence);
    // Async mode, EE thread: cut a VIF1 stream at an interrupt stall as
    // processVIF1Data would (the stall state lives on the EE) and add what
    // the worker has to process to `out`.
    void queueVif1Stream(std::vector<uint8_t> bytes, std::vector<std::vector<uint8_t>> &out);
    // The interrupt-bit VIFcode ended the last stream of a transfer.
    void finishVif1Queue();
    void beginVif1OnEe();
    void resyncVif1Scan();
    void finishVif1Async();

    GifPacketCallback m_gifPacketCallback;
    GifArbiter *m_gifArbiter = nullptr;
    Vu1MscalCallback m_vu1MscalCallback;
    Vu1MscntCallback m_vu1MscntCallback;

    uint8_t *m_vu0Code = nullptr;
    uint8_t *m_vu0Data = nullptr;
    uint8_t *m_vu1Code = nullptr;
    uint8_t *m_vu1Data = nullptr;
    bool m_path3Masked = false;
    uint32_t m_vif1PendingPath2ImageQwc = 0u;
    bool m_vif1PendingPath2DirectHl = false;
    // VIFcode interrupt bit: VIF1 stops after that command, raises INTC 5 and
    // waits for VIF1_FBRST.STC. The unprocessed stream is kept here and the
    // VIF1 DMA stays busy until it has been consumed.
    bool m_vif1StallAfterCommand = false;
    bool m_vif1InterruptStalled = false;
    std::vector<uint8_t> m_vif1StalledStream;
    std::vector<uint32_t> m_pendingIntcCauses;
    void resumeVif1AfterInterrupt();
    void finishVif1Dma();
    std::vector<std::vector<uint8_t>> m_path3MaskedFifo;

    struct PendingTransfer
    {
        bool fromScratchpad = false;
        uint32_t srcAddr = 0;
        uint32_t qwc = 0;
        std::vector<uint8_t> chainData;
    };
    std::vector<PendingTransfer> m_pendingGifTransfers;
    std::vector<PendingTransfer> m_pendingVif0Transfers;

    // A DMA transfer as the DMAC reads it: chain tags and their data are
    // fetched from memory as the transfer progresses. VIF1 transfers are
    // fetched only as far as VIF1 consumes them, so the part behind an
    // interrupt stall is read after the interrupt handler ran (it patches
    // data the rest of the chain REFs: the game's texture handler writes
    // TEX0 into material blocks). GIF and VIF0 transfers are read whole at
    // the kick (fetchAllDma).
    struct DmaSource
    {
        std::vector<uint8_t> literal; // bytes not from memory (VIF1 FIFO, an interpreter's stalled tail), sent first
        size_t literalPos = 0;
        uint8_t tagWords[8] = {};     // CHCR.TTE: the DMAtag's upper 64 bits, sent before the tag's data
        uint32_t tagWordsLeft = 0;
        uint32_t dataAddr = 0;        // current data block (guest address) and the bytes still to send
        uint32_t dataBytes = 0;
        bool chain = false;           // chain mode with tags left at tadr
        bool tte = false;
        bool tie = false;
        uint32_t channel = 0;         // channel registers this source updates (0: none)
        uint32_t chcr = 0, tadr = 0, asr0 = 0, asr1 = 0, asp = 0, lastTagUpper = 0;
        // A guard against a chain that loops forever, not a hardware limit
        // (the DMAC follows any number of tags). It was 4096, which a dense
        // level reaches: Dave Mirra BMX 2 running Aggressive Inline's Movie
        // Lot sends 5,498 tags a frame, and the rider, drawn last, was cut.
        int tagBudget = 1 << 20;
        // Native draws (setVif1Substitute): the substituted packet this
        // source is reading, the call depth its CALL started from, and
        // whether its RET has been read (it ends after that tag's data).
        std::shared_ptr<Vif1SubstituteState> sub;
        uint32_t subAsp = 0;
        bool subEnding = false;
        std::shared_ptr<Vif1SubstituteState> literalSub; // the packet `literal` came from
    };
    enum class DmaPiece : uint8_t
    {
        Literal,
        TagWords,
        Data
    };
    // Appends the next piece of `src` to `out`; false at its end.
    bool fetchDmaPiece(DmaSource &src, std::vector<uint8_t> &out, DmaPiece &kind, uint32_t &size);
    // Gives the last `count` bytes of the piece just fetched back to `src`.
    void unfetchDmaPiece(DmaSource &src, DmaPiece kind, uint32_t size, uint32_t count);
    bool readDmaChainTag(DmaSource &src);
    void fetchAllDma(DmaSource &src, std::vector<uint8_t> &out);
    void writeDmaChannelState(const DmaSource &src);
    // VIF1 DMA sources not yet consumed, in order (behind a stall).
    std::vector<DmaSource> m_newVif1Sources; // kicked, not yet started
    std::deque<DmaSource> m_vif1Sources;
    // Fetches m_vif1Sources into `out` until an interrupt-bit VIFcode stops
    // VIF1 (true) or the sources end (false), scanning on m_vif1Shadow.
    bool fetchVif1Stream(std::vector<uint8_t> &out);
    void noteVif1SubstituteTag(DmaSource &src, uint32_t tagAddress, uint64_t tag, uint32_t id, uint32_t aspBefore);
    void stallVif1();
    void runVif1SourcesOnEe();
    void takeVif1StalledTail();
    std::mutex m_completedDmacMutex;
    std::vector<uint32_t> m_completedDmacCauses;

    struct CodeRegion
    {
        uint32_t start;
        uint32_t end;
        std::vector<bool> modified; // Bitmap of modified 4-byte blocks
    };
    std::vector<CodeRegion> m_codeRegions;

    bool isAddressInRegion(uint32_t address, const CodeRegion &region);
    void markModified(uint32_t address, uint32_t size);
    void markVU0CodeModified() { m_vu0CodeGeneration.fetch_add(1, std::memory_order_relaxed); }
    void markVU1CodeModified() { m_vu1CodeGeneration.fetch_add(1, std::memory_order_relaxed); }
    bool isScratchpad(uint32_t address) const;
    uint8_t *mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit);
    const uint8_t *mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const;
    struct EeTimer
    {
        uint32_t count = 0;
        uint32_t mode = 0;
        uint32_t compare = 0;
        uint32_t hold = 0;
        uint64_t clockRemainder = 0;
    };

    std::array<EeTimer, 4> m_eeTimers{};
    uint32_t m_eeTimersCounting = 0; // bit i: timer i's mode has CUE (it counts), kept with each mode write
    uint32_t advanceCountingEeTimers(uint64_t eeCycles) noexcept;
    bool tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr);
    void completeDmacChannel(uint32_t channelBase, uint32_t cause);
    void queueCompletedDmacCause(uint32_t cause);
};

#endif // PS2_MEMORY_H
