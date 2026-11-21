#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include "runtime/memory.h"
#include "runtime/thread_placement.h"
#include "runtime/frame_trace.h"
#include "runtime/state_probe.h"
#include "runtime/ps2_address.h"
#include "gs/gs_frontend.h"
#include "runtime_log.h"
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    stopAsyncGraphics();
    if (m_scratchpad)
        ps2SetScratchpadHostPtr(nullptr);
    m_guestMap.release();
    m_rdram = nullptr;
    m_scratchpad = nullptr;

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

void PS2Memory::initializeVu0Memory()
{
    if (!m_vu0Code)
        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
    if (!m_vu0Data)
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
    std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
    std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);
    std::memset(&vif0_regs, 0, sizeof(vif0_regs));
    markVU0CodeModified();
}

void PS2Memory::setVu0Memory(const uint8_t *code, const uint8_t *data)
{
    if (std::memcmp(m_vu0Code, code, PS2_VU0_CODE_SIZE) != 0)
    {
        std::memcpy(m_vu0Code, code, PS2_VU0_CODE_SIZE);
        markVU0CodeModified();
    }
    std::memcpy(m_vu0Data, data, PS2_VU0_DATA_SIZE);
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        m_guestMap.release();
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_vif1StallAfterCommand = false;
    m_vif1InterruptStalled = false;
    m_vif1StalledStream.clear();
    m_vif1Sources.clear();
    m_newVif1Sources.clear();
    m_pendingIntcCauses.clear();
    m_vif1Shadow = {};
    m_path3MaskedShadow = false;
    m_vif1ScanPendingImageQwc = 0u;
    m_vif1ScanStallAfter = false;
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Main RAM and the scratchpad, zeroed: the guest map (RAM at its base and its mirrors, the scratchpad at
        // 0x70000000 and 0xF0000000), which the readable game's ee::at<T> takes as base + address.
        if (ramSize != PS2_RAM_SIZE || !m_guestMap.create(static_cast<uint32_t>(ramSize), guest_map::Layout::Game))
        {
            std::cerr << "Error initializing PS2 memory: no guest map of " << ramSize << " bytes" << std::endl;
            cleanup();
            return false;
        }
        m_rdram = m_guestMap.base();
        m_scratchpad = m_guestMap.scratchpad();
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        m_gifFqcLive = false;

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
    m_eeTimersCounting = 0u;
}

uint32_t PS2Memory::advanceCountingEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    // GIF_STAT's FIFO count (FQC, set at a GIF kick) drains as soon as any
    // EE time passes. The flag spares the register map's lookup on every
    // charge: this runs for every block of guest code.
    if (m_gifFqcLive)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        m_gifFqcLive = false;
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        if (gifStatIt != m_ioRegisters.end())
            gifStatIt->second &= ~kGifFqcMask;
    }

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    // VU1 micro and data memory belong to the VIF1 worker.
    if (physAddr >= PS2_VU1_CODE_BASE && physAddr < PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
        syncVif1(SyncVuMem);
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        syncGraphics(SyncGsPriv);
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        syncGraphics(SyncGsPriv);
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        syncGraphics(SyncGsPriv);
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            if (regOff == 0x70u || regOff == 0x90u)
            {
                if (off == 0u)
                    zx::ftrace::mark(zx::ftrace::kDisplayWrite, static_cast<uint16_t>(newVal & 0x1FFu));
                setDisplayBuffer(*reg, newVal);
            }
            else
                *reg = newVal;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        syncGraphics(SyncGsPriv);
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            if (regOff == 0x70u || regOff == 0x90u)
            {
                zx::ftrace::mark(zx::ftrace::kDisplayWrite, static_cast<uint16_t>(value & 0x1FFu));
                setDisplayBuffer(*reg, value);
            }
            else
                *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        if (zx::packets::g_on)
            zx::packets::add(zx::packets::kVif1, fifoData, sizeof(fifoData));
        if (vif1AsyncNow())
        {
            std::vector<std::vector<uint8_t>> queued;
            queueVif1Stream(std::vector<uint8_t>(fifoData, fifoData + sizeof(fifoData)), queued);
            if (!queued.empty())
                postVif1Work([this, queued = std::move(queued)]()
                             { processVIF1Data(queued[0].data(), static_cast<uint32_t>(queued[0].size())); });
            return;
        }
        beginVif1OnEe();
        processVIF1Data(fifoData, sizeof(fifoData));
        resyncVif1Scan();
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if (address >= 0x10003000u && address < 0x10004000u)
    {
        // The interrupt handler's VIF1_MARK and FBRST.STC writes are queued
        // behind the worker's work instead of waiting for it.
        if (m_asyncGraphics && (address == 0x10003C30u || (address == 0x10003C10u && (value & 0x1u) == 0u)))
            return writeVif1RegisterQueued(address, value);
        // Writes elsewhere in this range only go to the EE's register map.
        if (address >= 0x10003C00u && address < 0x10003D00u)
            syncVif1(SyncVifReg);
    }
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (timer.mode & kEeTimerModeCue)
                m_eeTimersCounting |= 1u << timerIndex;
            else
                m_eeTimersCounting &= ~(1u << timerIndex);
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x1u) // RST also drops a stalled stream
            {
                m_vif1StallAfterCommand = false;
                if (m_vif1InterruptStalled)
                {
                    m_vif1InterruptStalled = false;
                    m_vif1StalledStream.clear();
                    m_vif1Sources.clear();
                    finishVif1Dma();
                }
            }
            if (value & 0x8u) // STC
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            // The worker is idle (synced above): the shadow restarts from
            // the registers before the resumed stream is queued.
            resyncVif1Shadow((value & 0x1u) != 0u);
            if (value & 0x8u)
                resumeVif1AfterInterrupt();
            return true;
        case 0x10003C20u:
            vif1_regs.err = value & 0x7u;
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }
        resyncVif1Shadow(false);

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            {
                // ZX_TRACE_DMA=<n>: log the first n DMA starts per channel.
                static const long traceLimit = []()
                {
                    const char *env = std::getenv("ZX_TRACE_DMA");
                    return env ? std::strtol(env, nullptr, 0) : 0L;
                }();
                if (traceLimit > 0)
                {
                    static std::unordered_map<uint32_t, long> traced;
                    long &count = traced[channelBase];
                    if (count < traceLimit)
                    {
                        ++count;
                        std::fprintf(stderr, "[dma-trace] ch=0x%08x chcr=0x%08x madr=0x%08x qwc=0x%x tadr=0x%08x mode=%u\n",
                                     channelBase, value, madr, qwc, m_ioRegisters[channelBase + 0x30], (value >> 2) & 3u);
                    }
                }
            }

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                    {
                        DmaSource src;
                        src.dataAddr = srcAddr;
                        src.dataBytes = qwCount * 16u;
                        if (s_vif1PrefetchChain)
                        {
                            std::vector<uint8_t> bytes;
                            fetchAllDma(src, bytes);
                            src = DmaSource{};
                            src.literal = std::move(bytes);
                        }
                        m_newVif1Sources.push_back(std::move(src));
                    }
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    DmaSource src;
                    src.chain = true;
                    src.channel = channelBase;
                    src.chcr = chcr;
                    src.tadr = m_ioRegisters[channelBase + 0x30];
                    src.asr0 = m_ioRegisters[channelBase + 0x40];
                    src.asr1 = m_ioRegisters[channelBase + 0x50];
                    src.asp = (chcr >> 4) & 0x3u;
                    src.tie = (chcr & (1u << 7)) != 0u;
                    // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                    // the tag payload. VIF chains use those bytes for two VIFcodes.
                    src.tte = (channelBase == 0x10009000u || channelBase == 0x10008000u) && (chcr & 0x40u) != 0u;
                    src.lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    if (channelBase == 0x10009000u && !s_vif1PrefetchChain)
                    {
                        // VIF1 reads the chain as it goes (fetchVif1Stream).
                        m_newVif1Sources.push_back(std::move(src));
                    }
                    else
                    {
                        std::vector<uint8_t> chainBuf;
                        fetchAllDma(src, chainBuf);
                        writeDmaChannelState(src);
                        if (channelBase == 0x10009000u)
                        {
                            DmaSource whole;
                            whole.literal = std::move(chainBuf);
                            m_newVif1Sources.push_back(std::move(whole));
                        }
                        else if (!chainBuf.empty())
                        {
                            PendingTransfer pt;
                            pt.chainData = std::move(chainBuf);
                            if (channelBase == 0x1000A000)
                                m_pendingGifTransfers.push_back(std::move(pt));
                            else
                                m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

namespace
{
    // A DMA address as the DMAC reads it: the EE's uncached mirrors
    // (0x20000000, 0x30000000) of main memory are the same RAM.
    uint32_t dmaRamAddress(uint32_t address)
    {
        return address & 0x0FFFFFFFu;
    }
}

bool PS2Memory::readDmaChainTag(DmaSource &src)
{
    if (!src.chain)
        return false;
    if (src.tagBudget <= 0)
    {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            std::fprintf(stderr, "[dma] chain on channel 0x%08x stopped after %d tags at 0x%08x (a loop?)\n",
                         src.channel, DmaSource{}.tagBudget, src.tadr);
        src.chain = false;
        return false;
    }
    const uint32_t tagAddr = src.tadr;
    const bool tagInSPR = isScratchpad(tagAddr);
    uint32_t physTag = 0;
    try
    {
        physTag = translateAddress(tagAddr);
    }
    catch (...)
    {
        src.chain = false;
        return false;
    }
    const uint8_t *tagBase = tagInSPR ? m_scratchpad : m_rdram;
    const uint32_t tagMax = tagInSPR ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
    if (physTag + 16 > tagMax)
    {
        src.chain = false;
        return false;
    }

    const uint8_t *tp = tagBase + physTag;
    const uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
    const uint32_t aspBefore = src.asp;
    const uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
    const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
    const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
    const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
    src.lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
    --src.tagBudget;

    uint32_t dataAddr = 0;
    bool hasPayload = (tagQwc > 0);
    bool endChain = false;
    switch (id)
    {
    case 0: // refe
        dataAddr = addr;
        src.tadr = tagAddr + 16;
        endChain = true;
        break;
    case 1: // cnt
        dataAddr = tagAddr + 16;
        src.tadr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
        break;
    case 2: // next
        dataAddr = tagAddr + 16;
        src.tadr = addr;
        break;
    case 3: // ref
    case 4: // refs
        dataAddr = addr;
        src.tadr = tagAddr + 16;
        break;
    case 5: // call
        dataAddr = tagAddr + 16;
        {
            const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
            if (src.asp == 0u)
            {
                src.asr0 = retAddr;
                src.asp = 1u;
            }
            else if (src.asp == 1u)
            {
                src.asr1 = retAddr;
                src.asp = 2u;
            }
        }
        src.tadr = addr;
        break;
    case 6: // ret
        dataAddr = tagAddr + 16;
        if (src.asp == 2u)
        {
            src.tadr = src.asr1;
            src.asp = 1u;
        }
        else if (src.asp == 1u)
        {
            src.tadr = src.asr0;
            src.asp = 0u;
        }
        else
        {
            endChain = true;
        }
        break;
    case 7: // end
        dataAddr = tagAddr + 16;
        endChain = true;
        break;
    default:
        hasPayload = false;
        endChain = true;
        break;
    }

    if (src.channel == 0x10009000u && (src.sub || !m_vif1Substitutes.empty()) && !tagInSPR)
        noteVif1SubstituteTag(src, dmaRamAddress(tagAddr), tag, id, aspBefore);
    if (src.tte)
    {
        std::memcpy(src.tagWords, tp + 8, 8);
        src.tagWordsLeft = 8u;
    }
    if (hasPayload)
    {
        src.dataAddr = dataAddr;
        src.dataBytes = static_cast<uint32_t>(tagQwc) * 16u;
    }
    // An interrupt-bit tag with CHCR.TIE ends the transfer after its data.
    if (irq && src.tie)
        endChain = true;
    if (endChain)
        src.chain = false;
    return true;
}

bool PS2Memory::fetchDmaPiece(DmaSource &src, std::vector<uint8_t> &out, DmaPiece &kind, uint32_t &size)
{
    for (;;)
    {
        if (src.literalPos < src.literal.size())
        {
            kind = DmaPiece::Literal;
            size = static_cast<uint32_t>(src.literal.size() - src.literalPos);
            out.insert(out.end(), src.literal.begin() + static_cast<std::ptrdiff_t>(src.literalPos), src.literal.end());
            src.literal.clear();
            src.literalPos = 0;
            return true;
        }
        if (src.tagWordsLeft != 0u)
        {
            kind = DmaPiece::TagWords;
            size = src.tagWordsLeft;
            out.insert(out.end(), src.tagWords + (8u - src.tagWordsLeft), src.tagWords + 8u);
            src.tagWordsLeft = 0u;
            return true;
        }
        if (src.dataBytes != 0u)
        {
            kind = DmaPiece::Data;
            size = src.dataBytes;
            const bool scratch = isScratchpad(src.dataAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            uint32_t phys = translateAddress(src.dataAddr);
            uint32_t bytes = src.dataBytes;
            while (bytes > 0)
            {
                if (phys >= limit)
                    phys = 0;
                const uint32_t chunk = std::min(bytes, limit - phys);
                out.insert(out.end(), base + phys, base + phys + chunk);
                bytes -= chunk;
                phys += chunk;
            }
            src.dataAddr += src.dataBytes;
            src.dataBytes = 0u;
            return true;
        }
        if (!readDmaChainTag(src))
            return false;
    }
}

void PS2Memory::unfetchDmaPiece(DmaSource &src, DmaPiece kind, uint32_t size, uint32_t count)
{
    if (count == 0u)
        return;
    switch (kind)
    {
    case DmaPiece::Literal:
        // fetchDmaPiece dropped the vector: keep the tail as the new literal.
        break;
    case DmaPiece::TagWords:
        src.tagWordsLeft = count;
        break;
    case DmaPiece::Data:
        src.dataAddr -= count;
        src.dataBytes = count;
        break;
    }
    (void)size;
}

void PS2Memory::fetchAllDma(DmaSource &src, std::vector<uint8_t> &out)
{
    DmaPiece kind;
    uint32_t size = 0;
    while (fetchDmaPiece(src, out, kind, size))
    {
    }
}

void PS2Memory::writeDmaChannelState(const DmaSource &src)
{
    if (src.channel == 0u)
        return;
    m_ioRegisters[src.channel + 0x30] = src.tadr;
    m_ioRegisters[src.channel + 0x40] = src.asr0;
    m_ioRegisters[src.channel + 0x50] = src.asr1;
    uint32_t chcr = m_ioRegisters[src.channel + 0x00];
    chcr = (chcr & ~(0x3u << 4)) | ((src.asp & 0x3u) << 4);
    chcr = (chcr & 0x0000FFFFu) | (src.lastTagUpper << 16);
    m_ioRegisters[src.channel + 0x00] = chcr;
}

bool PS2Memory::fetchVif1Stream(std::vector<uint8_t> &out)
{
    // The DMAC feeds VIF1 piece by piece (a tag's TTE words, its data); the
    // EE-side scan follows. When an interrupt-bit VIFcode stops VIF1, the
    // DMAC stops too: what was read past it goes back to the source and the
    // rest of the chain stays unread until VIF1 resumes.
    size_t scanned = out.size();
    const size_t firstByte = scanned;
    const auto logPacket = [&]
    {
        if (zx::packets::g_on && out.size() > firstByte)
            zx::packets::add(zx::packets::kVif1, out.data() + firstByte, out.size() - firstByte);
    };
    // Native draws: the substituted packet the bytes from each position on
    // came from (setVif1Substitute), and the microprogram runs scanned.
    std::vector<std::pair<size_t, std::shared_ptr<Vif1SubstituteState>>> owners;
    std::vector<uint32_t> mscals, found;
    bool anyOwned = false;
    auto ownerAt = [&](size_t at) -> std::shared_ptr<Vif1SubstituteState>
    {
        std::shared_ptr<Vif1SubstituteState> o;
        for (const auto &e : owners)
        {
            if (e.first > at)
                break;
            o = e.second;
        }
        return o;
    };
    // Native draws' anchors (addVif1NativeAnchor) are looked for while any
    // is waiting.
    const bool anchors = !m_vif1Anchors.empty();
    auto substitute = [&]()
    {
        if ((anyOwned || anchors) && !mscals.empty())
            substituteVif1Runs(out.data(), mscals, owners);
    };
    while (!m_vif1Sources.empty())
    {
        DmaSource &src = m_vif1Sources.front();
        if (m_vif1ScanStallAfter)
        {
            // The interrupt-bit VIFcode ended the last piece: VIF1 takes all
            // of it (the packet log hashes it as every other way out does).
            m_vif1ScanStallAfter = false;
            writeDmaChannelState(src);
            logPacket();
            substitute();
            return true;
        }
        const size_t pieceStart = out.size();
        DmaPiece kind = DmaPiece::Data;
        uint32_t size = 0;
        std::shared_ptr<Vif1SubstituteState> literalOwner = src.literalSub;
        if (!fetchDmaPiece(src, out, kind, size))
        {
            writeDmaChannelState(src);
            m_vif1Sources.pop_front();
            continue;
        }
        if (kind == DmaPiece::Literal)
            src.literalSub.reset();
        {
            const std::shared_ptr<Vif1SubstituteState> &owner = kind == DmaPiece::Literal ? literalOwner : src.sub;
            const bool same = owners.empty() ? !owner : owners.back().second == owner;
            if (!same)
            {
                owners.emplace_back(pieceStart, owner);
                anyOwned |= owner != nullptr;
            }
        }
        bool stalls = false, incomplete = false;
        found.clear();
        const size_t scanFrom = scanned;
        scanned += scanVif1Stream(out.data() + scanned, static_cast<uint32_t>(out.size() - scanned), stalls, &incomplete,
                                  anyOwned || anchors ? &found : nullptr);
        for (const uint32_t at : found)
            mscals.push_back(static_cast<uint32_t>(scanFrom + at));
        if (!stalls)
            continue;
        // Give back what VIF1 did not take: normally a tail of this piece.
        const uint32_t back = static_cast<uint32_t>(out.size() - scanned);
        const uint32_t pieceBack = std::min<uint32_t>(back, size);
        std::vector<uint8_t> extra(out.begin() + static_cast<std::ptrdiff_t>(scanned),
                                   out.begin() + static_cast<std::ptrdiff_t>(out.size() - pieceBack));
        if (kind == DmaPiece::Literal)
        {
            extra.insert(extra.end(), out.end() - static_cast<std::ptrdiff_t>(pieceBack), out.end());
            src.literal = std::move(extra);
            src.literalPos = 0;
        }
        else
        {
            unfetchDmaPiece(src, kind, size, pieceBack);
            if (!extra.empty())
            {
                src.literal = std::move(extra);
                src.literalPos = 0;
            }
        }
        if (!src.literal.empty())
            src.literalSub = ownerAt(scanned);
        out.resize(scanned);
        writeDmaChannelState(src);
        logPacket();
        substitute();
        return true;
    }
    logPacket();
    substitute();
    if (scanned < out.size())
    {
        // The transfer ended inside a VIFcode: as one that is cut short.
        bool stalls = false;
        scanVif1Stream(out.data() + scanned, static_cast<uint32_t>(out.size() - scanned), stalls);
    }
    return false;
}

// ---- Native draws: VU1 work substituted in the VIF1 stream ---------------

struct Vif1SubstituteState
{
    uint32_t tagAddress = 0;
    PS2Memory::Vif1Substitute substitute;
    bool begun = false;
    bool declined = false;
};

void PS2Memory::setVif1Substitute(uint32_t tagAddress, Vif1Substitute substitute)
{
    tagAddress = dmaRamAddress(tagAddress);
    auto state = std::make_shared<Vif1SubstituteState>();
    state->tagAddress = tagAddress;
    state->substitute = std::move(substitute);
    m_vif1Substitutes[tagAddress] = std::move(state);
}

void PS2Memory::clearVif1Substitute(uint32_t tagAddress)
{
    if (!m_vif1Substitutes.empty())
        m_vif1Substitutes.erase(dmaRamAddress(tagAddress));
}

PS2Memory::Vif1SubstituteStats PS2Memory::vif1SubstituteStats() const
{
    std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
    return m_vif1SubstituteStats;
}

void PS2Memory::noteVif1SubstituteTag(DmaSource &src, uint32_t tagAddress, uint64_t tag, uint32_t id, uint32_t aspBefore)
{
    // In a substituted packet: its RET (back to the call depth its CALL
    // started from) ends it after that tag's data.
    if (src.sub)
    {
        if (!src.subEnding && m_vif1Substitutes.find(tagAddress) == m_vif1Substitutes.end())
        {
            if (id == 6u && aspBefore == src.subAsp + 1u)
                src.subEnding = true;
            return;
        }
        src.sub.reset();
        src.subEnding = false;
    }
    auto it = m_vif1Substitutes.find(tagAddress);
    if (it == m_vif1Substitutes.end())
        return;
    std::shared_ptr<Vif1SubstituteState> state = std::move(it->second);
    m_vif1Substitutes.erase(it);
    // ID (bits 28-30) and ADDR (bits 32-62): the packet the game built
    // there when it was registered, not a later one at the same place.
    constexpr uint64_t kCompared = 0x7FFFFFFF70000000ull;
    if (id != 5u || aspBefore >= 2u || ((tag ^ state->substitute.tag) & kCompared) != 0u)
    {
        std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
        ++m_vif1SubstituteStats.mismatched;
        return;
    }
    src.sub = std::move(state);
    src.subAsp = aspBefore;
    src.subEnding = false;
}

void PS2Memory::substituteVif1Runs(uint8_t *stream, const std::vector<uint32_t> &mscals,
                                   const std::vector<std::pair<size_t, std::shared_ptr<Vif1SubstituteState>>> &owners)
{
    size_t o = 0;
    std::shared_ptr<Vif1SubstituteState> owner;
    uint64_t taken = 0, declined = 0, skipped = 0, unpacks = 0;
    uint64_t anchorsReached = 0, anchorsDeclined = 0;
    for (const uint32_t at : mscals)
    {
        while (o < owners.size() && owners[o].first <= at)
            owner = owners[o++].second;
        uint32_t cmd = 0;
        std::memcpy(&cmd, stream + at, 4);
        if ((cmd & 0x7F000000u) == 0u)
        {
            // A native draw's anchor (NOP): its GS work begun here and
            // posted in stream order, the copy VIF1 gets marked with its
            // sequence number (kVif1NativeBegin); a slot already taken
            // (a stalled tail fetched again) or never given stays a NOP.
            if (static_cast<uint8_t>(cmd >> 16) != kVif1NativeAnchor)
                continue;
            auto anchor = m_vif1Anchors.find(static_cast<uint16_t>(cmd & 0xFFFFu));
            if (anchor == m_vif1Anchors.end())
                continue;
            Vif1NativeBegin begin = std::move(anchor->second);
            m_vif1Anchors.erase(anchor);
            ++anchorsReached;
            std::function<void()> work = begin ? begin() : std::function<void()>{};
            if (!work)
            {
                ++anchorsDeclined;
                continue;
            }
            const uint16_t sequence = ++m_vif1NativeSequence;
            {
                std::lock_guard<std::mutex> lock(m_vif1NativeMutex);
                m_vif1NativeQueue.push_back({sequence, std::move(work)});
            }
            cmd = (cmd & 0xFF000000u) | (static_cast<uint32_t>(kVif1NativeBegin) << 16) | sequence;
            std::memcpy(stream + at, &cmd, 4);
            continue;
        }
        if (!owner || owner->declined)
            continue;
        if (((cmd >> 24) & 0x60u) == 0x60u)
        {
            // UNPACK: once the packet is taken, its data goes nowhere (the
            // ones before its first run stay: it may yet be declined).
            if (owner->begun && (cmd & kVif1NativeUnpackSkip) == 0u)
            {
                cmd |= kVif1NativeUnpackSkip;
                std::memcpy(stream + at, &cmd, 4);
                ++unpacks;
            }
            continue;
        }
        const uint8_t num = static_cast<uint8_t>(cmd >> 16);
        if (num == kVif1NativeSkip || num == kVif1NativeBegin)
            continue; // a stalled tail fetched again
        if (!owner->begun)
        {
            owner->begun = true;
            std::function<void()> work = owner->substitute.begin ? owner->substitute.begin() : std::function<void()>{};
            if (!work)
            {
                owner->declined = true;
                ++declined;
                continue;
            }
            const uint16_t sequence = ++m_vif1NativeSequence;
            {
                std::lock_guard<std::mutex> lock(m_vif1NativeMutex);
                m_vif1NativeQueue.push_back({sequence, std::move(work)});
            }
            cmd = (cmd & 0xFF000000u) | (static_cast<uint32_t>(kVif1NativeBegin) << 16) | sequence;
            ++taken;
        }
        else
            cmd = (cmd & 0xFF00FFFFu) | (static_cast<uint32_t>(kVif1NativeSkip) << 16);
        ++skipped;
        std::memcpy(stream + at, &cmd, 4);
    }
    if (taken || declined || skipped || unpacks || anchorsReached)
    {
        std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
        m_vif1SubstituteStats.packets += taken;
        m_vif1SubstituteStats.declined += declined;
        m_vif1SubstituteStats.runsSkipped += skipped;
        m_vif1SubstituteStats.unpacksSkipped += unpacks;
        m_vif1AnchorStats.reached += anchorsReached;
        m_vif1AnchorStats.declined += anchorsDeclined;
    }
}

uint16_t PS2Memory::addVif1NativeAnchor(Vif1NativeBegin begin)
{
    const uint16_t slot = m_vif1AnchorNext++;
    auto [it, added] = m_vif1Anchors.try_emplace(slot);
    it->second = std::move(begin);
    std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
    ++m_vif1AnchorStats.added;
    if (!added)
        ++m_vif1AnchorStats.replaced;
    return slot;
}

PS2Memory::Vif1AnchorStats PS2Memory::vif1AnchorStats() const
{
    std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
    return m_vif1AnchorStats;
}

void PS2Memory::runVif1NativeWork(uint16_t sequence)
{
    std::function<void()> work;
    uint64_t lost = 0;
    {
        std::lock_guard<std::mutex> lock(m_vif1NativeMutex);
        while (!m_vif1NativeQueue.empty())
        {
            Vif1NativeWork w = std::move(m_vif1NativeQueue.front());
            m_vif1NativeQueue.pop_front();
            if (w.sequence == sequence)
            {
                work = std::move(w.work);
                break;
            }
            ++lost; // its stream never reached VIF1 (a reset)
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_vif1SubstituteStatsMutex);
        m_vif1SubstituteStats.drawsLost += lost;
        if (work)
            ++m_vif1SubstituteStats.drawsPosted;
    }
    if (work)
        postGsSideband(std::move(work));
}

void PS2Memory::stallVif1()
{
    m_vif1InterruptStalled = true;
    m_pendingIntcCauses.push_back(5u); // INTC VIF1
}

void PS2Memory::runVif1SourcesOnEe()
{
    // VIF1 runs here, on the EE: the scan decides how far the DMAC reads
    // (fetchVif1Stream, on a copy of the scan state), then VIF1 processes
    // that and stops at the interrupt-bit VIFcode by itself.
    while (!m_vif1InterruptStalled)
    {
        const VIFRegisters savedShadow = m_vif1Shadow;
        const bool savedMasked = m_path3MaskedShadow;
        const uint32_t savedImage = m_vif1ScanPendingImageQwc;
        const bool savedStall = m_vif1ScanStallAfter;
        m_vif1Shadow = vif1_regs;
        m_path3MaskedShadow = m_path3Masked;
        m_vif1ScanPendingImageQwc = m_vif1PendingPath2ImageQwc;
        m_vif1ScanStallAfter = m_vif1StallAfterCommand;
        std::vector<uint8_t> bytes;
        const bool cut = fetchVif1Stream(bytes);
        m_vif1Shadow = savedShadow;
        m_path3MaskedShadow = savedMasked;
        m_vif1ScanPendingImageQwc = savedImage;
        m_vif1ScanStallAfter = savedStall;
        if (!bytes.empty())
            processVIF1Data(bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (!m_vif1InterruptStalled && m_vif1StallAfterCommand)
        {
            // The interrupt-bit VIFcode ended what was read.
            m_vif1StallAfterCommand = false;
            m_vif1InterruptStalled = true;
            m_pendingIntcCauses.push_back(5u);
        }
        if (!cut)
            break;
    }
    takeVif1StalledTail();
}

void PS2Memory::takeVif1StalledTail()
{
    // A stream VIF1 stopped inside keeps its tail ahead of the sources.
    if (m_vif1StalledStream.empty())
        return;
    DmaSource tail;
    tail.literal = std::move(m_vif1StalledStream);
    m_vif1StalledStream.clear();
    m_vif1Sources.push_front(std::move(tail));
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
        m_gifFqcLive = true;
    }

    // PATH3 packets in submission order, copied now: the GIF and VIF1 work
    // below may run later on the VIF1 worker, after the EE changed memory.
    std::vector<std::vector<uint8_t>> gifPackets;
    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            gifPackets.push_back(std::move(p.chainData));
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
            const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            uint32_t bytesLeft = sizeBytes;
            while (bytesLeft >= 16)
            {
                if (srcPhys >= limit)
                    srcPhys = 0;
                uint32_t chunk = bytesLeft;
                if (srcPhys + chunk > limit)
                    chunk = limit - srcPhys;
                if (chunk == 0)
                    break;
                m_seenGifCopy = true;
                m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                gifPackets.emplace_back(base + srcPhys, base + srcPhys + chunk);
                bytesLeft -= chunk;
                srcPhys += chunk;
            }
        }
    }
    m_pendingGifTransfers.clear();
    if (zx::packets::g_on)
        for (const std::vector<uint8_t> &packet : gifPackets)
            zx::packets::add(zx::packets::kGif, packet.data(), packet.size());

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    bool hadVif1 = !m_newVif1Sources.empty();
    for (auto &src : m_newVif1Sources)
        m_vif1Sources.push_back(std::move(src));
    m_newVif1Sources.clear();

    // PATH3, then VIF1 (PATH1/PATH2 drain the arbiter as they go).
    if (!gifPackets.empty() || hadVif1)
    {
        if (vif1AsyncNow())
        {
            // The EE sees the transfers complete (or VIF1 stalled on an
            // interrupt-bit VIFcode) now, as when they run synchronously.
            // VIF1 DMA is read up to the stall; the rest is read on STC.
            std::vector<uint8_t> vif1Bytes;
            if (hadVif1 && !m_vif1InterruptStalled && fetchVif1Stream(vif1Bytes))
                stallVif1();
            if (hadVif1)
                finishVif1Queue();
            if (m_vif1InterruptStalled)
                hadVif1 = false;
            // The stall state lives on the EE: the worker processes exactly
            // the fetched stream.
            postVif1Work([this, gif = std::move(gifPackets), vif1 = std::move(vif1Bytes)]()
                         {
                             for (const auto &packet : gif)
                                 submitGifPacket(GifPathId::Path3, packet.data(), static_cast<uint32_t>(packet.size()), false);
                             if (!vif1.empty())
                                 processVIF1Data(vif1.data(), static_cast<uint32_t>(vif1.size())); });
        }
        else
        {
            beginVif1OnEe();
            for (const auto &packet : gifPackets)
                submitGifPacket(GifPathId::Path3, packet.data(), static_cast<uint32_t>(packet.size()), false);
            if (hadVif1)
                runVif1SourcesOnEe();
            if (m_vif1InterruptStalled)
            {
                // VIF1 DMA stays busy (STR set) until the stream is resumed.
                hadVif1 = false;
            }
            if (m_gifArbiter)
                m_gifArbiter->drain();
            resyncVif1Scan();
        }
    }
    else if (m_gifArbiter && !m_asyncGraphics)
    {
        m_gifArbiter->drain();
    }

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::setAsyncGraphics(bool enabled)
{
    if (enabled == m_asyncGraphics)
        return;
    if (!enabled)
    {
        stopAsyncGraphics();
        return;
    }
    m_vifStage = std::make_unique<Ps2AsyncStage>("vif1-stage");
    m_gsStage = std::make_unique<Ps2AsyncStage>("gs-stage");
    // Their cores (ZX_THREAD_PLACEMENT=1, runtime/thread_placement.h), set on each worker.
    if (zx::placement::enabled())
    {
        m_vifStage->post([]()
                         { zx::placement::placeCurrentThread(zx::placement::Role::Graphics); });
        m_gsStage->post([]()
                        { zx::placement::placeCurrentThread(zx::placement::Role::Graphics); });
    }
    m_gsStage->setIdleHook([this]()
                           {
                               if (m_gsIdleSink)
                                   m_gsIdleSink(); });
    m_asyncGraphics = true;
}

void PS2Memory::stopAsyncGraphics()
{
    // The VIF1 stage feeds the GS stage: finish it first.
    if (m_vifStage)
        m_vifStage->stop();
    if (m_gsStage)
        m_gsStage->stop();
    m_asyncGraphics = false;
    m_vifStage.reset();
    m_gsStage.reset();
}

bool PS2Memory::vif1AsyncNow()
{
    return m_asyncGraphics && m_vifStage && (!m_vu1AsyncAllowed || m_vu1AsyncAllowed());
}

void PS2Memory::postVif1Work(std::function<void()> work)
{
    zx::ftrace::mark(zx::ftrace::kVifPost);
    m_vifStage->post([this, work = std::move(work)]()
                     {
                         work();
                         if (m_gifArbiter)
                             m_gifArbiter->drain();
                         finishVif1Async();
                         flushGsBatch(); });
}

void PS2Memory::finishVif1Async()
{
    // Interrupt stalls are decided on the EE (queueVif1Stream): the worker
    // never sets the stall state.
    if (!m_vif1StallAfterCommand)
        return;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        std::fprintf(stderr, "[vif1] stall state set on the VIF1 worker\n");
}

bool PS2Memory::writeVif1RegisterQueued(uint32_t address, uint32_t value)
{
    m_ioRegisters[address] = value;
    m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
    if (address == 0x10003C30u) // VIF1_MARK: also clears STAT.MRK
    {
        const uint32_t mark = value & 0xFFFFu;
        m_vif1Shadow.mark = mark;
        m_vif1Shadow.stat &= ~(1u << 6);
        postVif1Work([this, mark]()
                     {
                         vif1_regs.mark = mark;
                         vif1_regs.stat &= ~(1u << 6); });
        return true;
    }
    if (value & 0x8u) // VIF1_FBRST.STC (RST is handled synchronously)
    {
        constexpr uint32_t kCleared = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13);
        m_vif1Shadow.stat &= ~kCleared;
        postVif1Work([this]()
                     { vif1_regs.stat &= ~kCleared; });
        resumeVif1AfterInterrupt();
    }
    return true;
}

void PS2Memory::queueVif1Stream(std::vector<uint8_t> bytes, std::vector<std::vector<uint8_t>> &out)
{
    DmaSource src;
    src.literal = std::move(bytes);
    m_vif1Sources.push_back(std::move(src));
    if (m_vif1InterruptStalled)
        return; // Already stalled: queued behind the rest.
    std::vector<uint8_t> fetched;
    if (fetchVif1Stream(fetched))
        stallVif1();
    if (!fetched.empty())
        out.push_back(std::move(fetched));
}

void PS2Memory::finishVif1Queue()
{
    if (!m_vif1InterruptStalled && m_vif1ScanStallAfter)
    {
        // The interrupt-bit VIFcode was the last thing in the chain.
        m_vif1ScanStallAfter = false;
        m_vif1InterruptStalled = true;
        m_vif1StalledStream.clear();
        m_pendingIntcCauses.push_back(5u);
    }
}

void PS2Memory::beginVif1OnEe()
{
    syncVif1(SyncSynchronous);
    // VIF1 runs here once: it takes the stall due before the next command
    // from the scan state (the worker never holds it).
    if (m_asyncGraphics)
        m_vif1StallAfterCommand = m_vif1ScanStallAfter;
}

void PS2Memory::resyncVif1Scan()
{
    // After VIF1 ran on the EE (worker idle).
    resyncVif1Shadow(false);
    m_vif1ScanStallAfter = m_vif1StallAfterCommand;
    if (m_asyncGraphics)
        m_vif1StallAfterCommand = false;
}

void PS2Memory::resyncVif1Shadow(bool reset)
{
    // Only with the worker idle: the registers are final.
    m_vif1Shadow = vif1_regs;
    m_path3MaskedShadow = m_path3Masked;
    m_vif1ScanPendingImageQwc = m_vif1PendingPath2ImageQwc;
    if (reset)
        m_vif1ScanStallAfter = false;
}

void PS2Memory::deliverGifPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!m_gsPacketSink)
        return;
    if (!m_asyncGraphics || !m_gsStage)
    {
        m_gsPacketSink(data, sizeBytes);
        return;
    }
    if (onVu1Worker())
    {
        m_gsBatch.insert(m_gsBatch.end(), data, data + sizeBytes);
        m_gsBatchSizes.push_back(sizeBytes);
        if (m_gsBatch.size() >= 64u * 1024u)
            flushGsBatch();
        return;
    }
    // EE thread, with the VIF1 stage drained: straight to the GS stage.
    m_gsStage->post([this, packet = std::vector<uint8_t>(data, data + sizeBytes)]()
                    { m_gsPacketSink(packet.data(), static_cast<uint32_t>(packet.size())); });
}

void PS2Memory::flushGsBatch()
{
    if (m_gsBatchSizes.empty())
        return;
    m_gsStage->post([this, data = std::move(m_gsBatch), sizes = std::move(m_gsBatchSizes)]()
                    {
                        size_t offset = 0;
                        for (const uint32_t size : sizes)
                        {
                            m_gsPacketSink(data.data() + offset, size);
                            offset += size;
                        } });
    m_gsBatch.clear();
    m_gsBatchSizes.clear();
}

void PS2Memory::syncVif1(SyncReason reason) const
{
    // GS worker tasks (queueGsWork) run after the VIF1 output they follow.
    if (!m_vifStage || m_vifStage->idle() || m_vifStage->isWorkerThread() || onGsWorker())
        return;
    m_syncWaits[reason].fetch_add(1u, std::memory_order_relaxed);
    m_vifStage->drain();
}

void PS2Memory::queueGsWork(std::function<void()> fn)
{
    if (!m_asyncGraphics || !m_vifStage || !m_gsStage)
    {
        fn();
        return;
    }
    if (m_gsWorkDone.load(std::memory_order_acquire) < m_gsWorkPosted)
    {
        m_syncWaits[SyncGsHle].fetch_add(1u, std::memory_order_relaxed);
        zx::ftrace::Scope traceWait(zx::ftrace::kSwapWaitBegin, zx::ftrace::kSwapWaitEnd);
        std::unique_lock<std::mutex> lock(m_gsWorkMutex);
        m_gsWorkCv.wait(lock, [this]()
                        { return m_gsWorkDone.load(std::memory_order_acquire) >= m_gsWorkPosted; });
    }
    ++m_gsWorkPosted;
    zx::ftrace::mark(zx::ftrace::kSwapPosted);
    m_vifStage->post([this, fn = std::move(fn)]() mutable
                     {
                         flushGsBatch();
                         m_gsStage->post([this, fn = std::move(fn)]()
                                         {
                                             zx::ftrace::mark(zx::ftrace::kSwapRun);
                                             fn();
                                             {
                                                 std::lock_guard<std::mutex> lock(m_gsWorkMutex);
                                                 m_gsWorkDone.fetch_add(1u, std::memory_order_acq_rel);
                                             }
                                             m_gsWorkCv.notify_all();
                                             // (a buffer swap: its picture, the game frame just drawn; after
                                             // the work is done, so the EE's next swap does not wait for it)
                                             if (m_displayChangedInGsWork)
                                             {
                                                 m_displayChangedInGsWork = false;
                                                 if (m_displayChangeSink)
                                                     m_displayChangeSink();
                                             } }); });
}

void PS2Memory::postGsSideband(std::function<void()> fn)
{
    if (!m_asyncGraphics || !m_vifStage || !m_gsStage)
    {
        fn();
        return;
    }
    if (onGsWorker())
    {
        fn();
        return;
    }
    if (onVu1Worker())
    {
        flushGsBatch();
        m_gsStage->post(std::move(fn));
        return;
    }
    if (m_vifStage->idle())
    {
        m_gsStage->post(std::move(fn));
        return;
    }
    // Behind the VIF1 work queued so far, as queueGsWork, without its wait.
    m_vifStage->post([this, fn = std::move(fn)]() mutable
                     {
                         flushGsBatch();
                         m_gsStage->post(std::move(fn)); });
}

void PS2Memory::syncGraphics(SyncReason reason) const
{
    syncVif1(reason);
    if (!m_gsStage || m_gsStage->idle() || m_gsStage->isWorkerThread() || onVu1Worker())
        return;
    m_syncWaits[reason].fetch_add(1u, std::memory_order_relaxed);
    m_gsStage->drain();
}

void PS2Memory::finishVif1Dma()
{
    static constexpr uint32_t kVif1Channel = 0x10009000u;
    m_ioRegisters[kVif1Channel + 0x20] = 0;
    completeDmacChannel(kVif1Channel, 1u);
}

void PS2Memory::resumeVif1AfterInterrupt()
{
    if (!m_vif1InterruptStalled)
        return;
    m_vif1InterruptStalled = false;
    takeVif1StalledTail();
    if (vif1AsyncNow())
    {
        // The DMAC reads the rest of the transfer now, after the handler.
        std::vector<uint8_t> stream;
        if (fetchVif1Stream(stream))
            stallVif1();
        finishVif1Queue();
        if (!stream.empty())
            postVif1Work([this, stream = std::move(stream)]()
                         { processVIF1Data(stream.data(), static_cast<uint32_t>(stream.size())); });
        if (!m_vif1InterruptStalled)
            finishVif1Dma();
        return;
    }
    beginVif1OnEe();
    runVif1SourcesOnEe();
    if (m_gifArbiter)
        m_gifArbiter->drain();
    resyncVif1Scan();
    if (!m_vif1InterruptStalled)
        finishVif1Dma();
}

std::vector<uint32_t> PS2Memory::consumePendingIntcCauses()
{
    std::vector<uint32_t> causes;
    causes.swap(m_pendingIntcCauses);
    return causes;
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    syncVif1(SyncSynchronous);
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    syncVif1(SyncSynchronous);
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    syncGraphics(SyncGifChain);
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    syncGraphics(SyncGifChain);
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    // VIF1 registers and GIF_STAT come from the EE-side shadow (scanVif1Stream)
    // while the worker may still be busy; the other GIF/VIF registers here
    // only hold what the EE wrote. ZX_VIF1_SHADOW_CHECK=1 drains the worker
    // on every read and reports where the shadow differs.
    static const bool shadowCheck = []()
    {
        const char *value = std::getenv("ZX_VIF1_SHADOW_CHECK");
        return value && value[0] == '1';
    }();
    // Only the VIF1 registers 0x10003C00-0x10003CE0 and GIF_STAT's PATH3 mask
    // change on the worker; the rest of this range holds what the EE wrote.
    const bool shadowed = m_asyncGraphics && !shadowCheck &&
                          ((address >= 0x10003C00u && address <= 0x10003CE0u) || address == 0x10003020u);
    if (((address >= 0x10003C00u && address <= 0x10003CE0u) || address == 0x10003020u) && !shadowed)
        syncVif1(address == 0x10003020u ? SyncGifReg : SyncVifReg);
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address >= 0x10003C00u && address <= 0x10003CE0u && (address & 0xFu) == 0u)
    {
        if (m_asyncGraphics && shadowCheck)
        {
            const uint32_t *real = reinterpret_cast<const uint32_t *>(&vif1_regs);
            const uint32_t *shadow = reinterpret_cast<const uint32_t *>(&m_vif1Shadow);
            const uint32_t index = (address - 0x10003C00u) / 16u;
            static const uint8_t field[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
            static std::atomic<int> reported{0};
            if (index < 15u && index != 1u && real[field[index]] != shadow[field[index]] && reported.fetch_add(1) < 32)
                std::fprintf(stderr, "[vif1-shadow] 0x%08x real=0x%08x shadow=0x%08x\n", address, real[field[index]],
                             shadow[field[index]]);
        }
        const VIFRegisters &vif1 = shadowed ? m_vif1Shadow : vif1_regs;
        switch (address)
        {
        case 0x10003C00u:
            return vif1.stat | (m_vif1InterruptStalled ? (1u << 11) : 0u);
        case 0x10003C20u:
            return vif1.err;
        case 0x10003C30u:
            return vif1.mark;
        case 0x10003C40u:
            return vif1.cycle;
        case 0x10003C50u:
            return vif1.mode;
        case 0x10003C60u:
            return vif1.num;
        case 0x10003C70u:
            return vif1.mask;
        case 0x10003C80u:
            return vif1.code;
        case 0x10003C90u:
            return vif1.itops;
        case 0x10003CA0u:
            return vif1.base;
        case 0x10003CB0u:
            return vif1.ofst;
        case 0x10003CC0u:
            return vif1.tops;
        case 0x10003CD0u:
            return vif1.itop;
        case 0x10003CE0u:
            return vif1.top;
        default:
            break;
        }
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               ((shadowed ? m_path3MaskedShadow : m_path3Masked) ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
