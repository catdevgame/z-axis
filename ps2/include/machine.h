#ifndef PS2_MACHINE_H
#define PS2_MACHINE_H

#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <functional>
#include <initializer_list>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif
#include <atomic>
#include <array>
#include <mutex>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "runtime_log.h"
#include "runtime/ps2_address.h"
#include "gs/gif_arbiter.h"
#include "runtime/memory.h"
#include "runtime/guest_heap.h"
#include "gs/gs_frontend.h"
#include "vu1/vu1_interpreter.h"
#include "runtime/audio.h"
#include "runtime/pad.h"
#include "runtime/rom_device.h"
#include "runtime/vfs.h"
#include "ps2x/iop/iop_types.h"
#include "disc/iso9660.h"
#include "runtime/guest_cpu.h"

namespace ps2x::iop
{
    class IopSubsystem;
}

class PS2IopHostAdapter;
class PS2IopTransport;
class EeScheduler;
struct EeEvent;

inline constexpr uint32_t PS2_PATH_WATCH_ADDR = 0x01EFFFA0u;
inline constexpr uint32_t PS2_PATH_WATCH_BYTES = 0x200u;

inline uint32_t ps2PathWatchPhysAddr()
{
    return PS2_PATH_WATCH_ADDR & PS2_RAM_MASK;
}

inline uint8_t ps2PathWatchExtractByteFromWrite(uint32_t writeAddr, uint32_t watchAddr, uint64_t valueLo, uint64_t valueHi)
{
    const uint32_t byteIndex = watchAddr - writeAddr;
    if (byteIndex < 8u)
    {
        return static_cast<uint8_t>((valueLo >> (byteIndex * 8u)) & 0xFFu);
    }
    return static_cast<uint8_t>((valueHi >> ((byteIndex - 8u) * 8u)) & 0xFFu);
}

namespace zx
{
    class BootConfig; // runtime/boot_config.h
    namespace boot
    {
        class Scope;
    }
}

// EeScheduler::checkpointDue and accountCycles, for PS2Machine::eeCheckpointDue (EeScheduler.cpp).
bool eeSchedulerCheckpointDue(EeScheduler &scheduler, uint32_t cycles) noexcept;
void eeSchedulerAccountCycles(EeScheduler &scheduler, uint32_t cycles) noexcept;

class PS2Machine
{
public:
    struct IoPaths
    {
        std::filesystem::path elfPath;
        std::filesystem::path elfDirectory;
        std::filesystem::path hostRoot;
        std::filesystem::path cdRoot;
        std::filesystem::path mcRoot;
        std::filesystem::path cdImage;
        // cdrom0: names resolve inside cdImage (ISO9660) instead of the
        // cdRoot folder; cdRoot stays empty.
        bool cdFromImage = false;
    };

    // Host window and presentation. The defaults are the developer runner's.
    struct HostOptions
    {
        int windowWidth = 0;  // 0: HOST_WINDOW_WIDTH
        int windowHeight = 0; // 0: HOST_WINDOW_HEIGHT
        bool highDpi = false;
        // Whole multiples of 640x480 with nearest filtering instead of the
        // largest 4:3 fit with bilinear filtering.
        bool integerScale = false;
        // Bilinear filtering for the non-integer fit (default: nearest).
        bool smoothFilter = false;
        // 16:9 presentation for games whose override renders 16:9 (see
        // markWidescreenFrame). ZX_WIDESCREEN=1/0 overrides it.
        bool widescreen = false;
        // The host (window, GPU device, audio device, gamepads) belongs to
        // the caller, which keeps it up across runtimes and shuts it down
        // itself (ps2_host::shutdown): the destructor leaves it alone. By
        // default the destructor shuts it down, after the IOP's audio
        // stream. run() never does.
        bool hostOwned = false;
    };

    // A runtime is one game boot: config is what the boot is given (the
    // app's per-game settings and paths, a check's per-game switches;
    // runtime/boot_config.h). The default: the environment alone.
    PS2Machine();
    explicit PS2Machine(zx::BootConfig config);
    ~PS2Machine();

    bool initialize(const char *title = "PS2 Game");
    bool syncCoreSubsystems();
    bool loadELF(const std::string &elfPath);
    // As loadELF, from bytes already in memory (an ELF read from a disc
    // image); elfPath names it (its file name is the game's ELF name).
    // deriveIoRoots=false keeps the configured host/CD/card roots instead of
    // pointing them at elfPath's folder.
    bool loadELFImage(const std::string &elfPath, const std::vector<uint8_t> &bytes, bool deriveIoRoots = true);
    void run();

    void setHostOptions(const HostOptions &options)
    {
        m_hostOptions = options;
        setWidescreen(options.widescreen);
    }
    [[nodiscard]] const HostOptions &hostOptions() const { return m_hostOptions; }
    void setIntegerScale(bool enabled) { m_hostOptions.integerScale = enabled; }
    // Widescreen wanted by the user; read by game overrides on the EE thread.
    void setWidescreen(bool enabled) { m_widescreen.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool widescreen() const { return m_widescreen.load(std::memory_order_relaxed); }
    // The widescreen aspect (width / height, 16:9 by default; 0 follows the
    // window's shape, clamped to 4:3..32:9). ZX_ASPECT=16:9|16:10|21:9|
    // 32:9|window|<number> overrides it.
    void setWideAspect(float aspect) { m_wideAspect.store(aspect, std::memory_order_relaxed); }
    [[nodiscard]] float wideAspectSetting() const { return m_wideAspect.load(std::memory_order_relaxed); }
    // The aspect a game override renders widescreen frames for (the window's
    // when the setting follows it). Any thread.
    [[nodiscard]] float wideAspect() const;
    // View settings for game overrides (EE thread), render-side only; the
    // defaults are the game's own look. Motion blur: 0 off, 1 low, 2 the
    // game's (ZX_MOTION_BLUR). Draw distance: 0 the game's, 1 far, 2
    // farthest (ZX_DRAW_DISTANCE).
    void setMotionBlur(int level) { m_motionBlur.store(level, std::memory_order_relaxed); }
    [[nodiscard]] int motionBlur() const { return m_motionBlur.load(std::memory_order_relaxed); }
    void setDrawDistance(int level) { m_drawDistance.store(level, std::memory_order_relaxed); }
    [[nodiscard]] int drawDistance() const { return m_drawDistance.load(std::memory_order_relaxed); }
    // A game override calls this for each frame it renders for widescreen.
    // The picture is stretched to wideAspect() while such frames are shown;
    // other frames (movies, 2D screens) stay 4:3.
    void markWidescreenFrame() { m_widescreenMarks.fetch_add(1u, std::memory_order_relaxed); }
    // Makes the GS renderer again from ZX_GS_BACKEND / ZX_GS_SCALE
    // (renderer settings changed while running); done by run() between two
    // host frames, once no image transfer is under way. Window thread.
    void requestRasterBackendReload() { m_rasterReloadRequested = true; }
    // Display buffer flips so far (the game's frames, for a frame rate counter).
    static uint64_t displayFlips();
    // Saves the next presented picture as a PNG (as ZX_CONTROL "shot"
    // does: the displayed aspect at the renderer's internal resolution);
    // done(ok) runs on a worker thread once written. Any thread.
    void requestScreenshot(std::string path, std::function<void(bool ok)> done = {});
    // A filter over the presented picture (a game override's photo mode:
    // letterbox, vignette, black and white). While one is set, the picture
    // is passed through it on the window thread before it is shown and
    // screenshots get the same pixels (the GPU renderer's picture is read
    // back each frame for it, so it costs a few milliseconds). Top-down
    // RGBA, `stride` bytes per row, shown at `displayAspect` (width /
    // height on screen). Empty to remove. Any thread.
    using PictureFilter =
        std::function<void(uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride, float displayAspect)>;
    void setPictureFilter(PictureFilter filter);
    // Called once per host frame on the window thread, after input polling.
    using HostFrameCallback = std::function<void(PS2Machine &runtime)>;
    void setHostFrameCallback(HostFrameCallback callback) { m_hostFrameCallback = std::move(callback); }
    // Called at the end of run(), on the thread that ran it, once the game
    // thread and the graphics workers have stopped (guest memory still
    // there): what a run leaves that the next one must not find (the
    // readable functions still waiting on their fibers). In the order
    // added, each function once.
    using StoppedHook = void (*)(PS2Machine &runtime);
    void addStoppedHook(StoppedHook hook);
    // Freezes guest time (EE, and with it the IOP and sound) while set; the
    // window keeps presenting the last frame.
    void setHostPaused(bool paused);
    [[nodiscard]] bool isHostPaused() const;

    [[nodiscard]] ps2x::iop::ModuleLoadResult loadIopModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
    [[nodiscard]] ps2x::iop::ModuleLoadResult loadIopModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
    [[nodiscard]] bool stopIopModule(int32_t moduleId, int32_t *result = nullptr);
    [[nodiscard]] ps2x::iop::DebugSnapshot iopDebugSnapshot() const;
    uint32_t allocateIopMemory(uint32_t size, uint32_t alignment = 16u);
    bool freeIopMemory(uint32_t address);
    bool readIopMemory(uint32_t address, void *destination, size_t size) const;
    bool writeIopMemory(uint32_t address, const void *source, size_t size);
    bool zeroIopMemory(uint32_t address, size_t size);
    bool isIopMemoryRange(uint32_t address, size_t size) const;

    using DebugUiCallback = void (*)(PS2Machine &runtime, void *userData);
    void setDebugUiCallbacks(DebugUiCallback initCallback,
                             DebugUiCallback drawCallback,
                             DebugUiCallback shutdownCallback,
                             void *userData);

    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Machine *);

    enum class GuestBranchKind
    {
        DirectJump,
        DirectCall,
        IndirectJump,
        IndirectCall,
        Return,
    };

    enum class MissingFunctionPolicy : uint32_t
    {
        // Strict mode for tests/CI: log the bad target and request the runtime to stop.
        Stop = 0,

        // Debug mode: log once, leave ctx->pc on the bad target, and let the caller unwind.
        ContinueToTarget = 1,

        // Debug mode: same as ContinueToTarget, but triggers a debugger break once on MSVC.
        BreakOnce = 2,

        // Escape hatch only: skip missing calls by returning to fallthrough (it can hide guest bugs)
        SkipCallDebug = 3,
    };

    bool replaceFunction(uint32_t address, RecompiledFunction func);
    // TODO remove this later need to update all tests
    bool registerFunction(uint32_t address, RecompiledFunction func);
    RecompiledFunction lookupFunction(uint32_t address);
    // The table's function at address, null if none (nothing reported).
    RecompiledFunction tableEntry(uint32_t address) const;
    bool hasFunction(uint32_t address) const;
    // Calls a guest function from host code (a game override hook) and
    // returns its v0. It runs to completion: EE safe points are held off
    // meanwhile, so it must not block or switch threads (short helpers:
    // property access, string handling); tail jumps and branches into split
    // off blocks are followed to the return. Arguments go in a0-a3, then t0-t3
    // (EABI); the callee's stack starts at sp (16-byte aligned, below
    // anything the caller keeps there). Every register of ctx is restored
    // afterwards; false if the function is unknown or did not return.
    bool callGuestFunction(uint8_t *rdram, R5900Context *ctx, uint32_t address,
                           std::initializer_list<uint32_t> args, uint32_t sp, uint32_t *v0 = nullptr);
    // The same with the function itself (the original of a replaced one,
    // from lookupFunction before replaceFunction); entry is its address.
    bool callGuestFunction(uint8_t *rdram, R5900Context *ctx, RecompiledFunction fn, uint32_t entry,
                           std::initializer_list<uint32_t> args, uint32_t sp, uint32_t *v0 = nullptr);
    // A tail call (a `j` to a known function's start; ctx->pc is already
    // the target): generated code calls the function in the table, so a
    // replaced one (a readable version, a game override, the recorder's
    // shim) runs as it would through the dispatcher; `direct` is the
    // recompiled function, called when the table holds it (or the target
    // is outside the table). As in the game, no EE cycles are charged and
    // there is no safe point. sourcePc (the jump) is for ZX_TAILCALL_STATS.
    void tailCall(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                  RecompiledFunction direct)
    {
        const uint32_t slot = (targetPc - s_tailCallTable.base) >> 2;
        RecompiledFunction fn = slot < s_tailCallTable.slotCount ? s_tailCallTable.table[slot] : nullptr;
        if (fn == nullptr || fn == direct)
        {
            direct(rdram, ctx, this);
            return;
        }
        if (s_tailCallTable.stats)
            noteReplacedTailCall(sourcePc, targetPc);
        fn(rdram, ctx, this);
    }
    // The function table tailCall reads: the active one (machine.cpp
    // keeps it in step). Zero (static storage) until a table is chosen.
    struct TailCallTable
    {
        uint32_t base;
        uint32_t slotCount;
        RecompiledFunction *table;
        bool stats;
    };
    static inline TailCallTable s_tailCallTable{};
    // ZX_TAILCALL_STATS=1: counts tail calls that reached a replaced
    // function, by site; written to stderr at exit.
    static void noteReplacedTailCall(uint32_t sourcePc, uint32_t targetPc);
    struct TailCallCount
    {
        uint32_t sourcePc = 0;
        uint32_t targetPc = 0;
        uint64_t calls = 0;
    };
    // The counts so far, by target then site.
    static std::vector<TailCallCount> replacedTailCalls();
    // With ZX_TAILCALL_STATS: the addresses whose table entry is no longer
    // the generated function.
    static std::vector<uint32_t> replacedFunctions();
    // ZX_WATCH32 support (see machine.cpp).
    void checkWatch32(uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc, const char *where);
    // The start of every dispatch: pc at the target, its cycles charged. False
    // when a checkpoint is due there: the guest thread unwinds, pc at the target.
    bool dispatchCharge(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc);
    // Call tracing is on: every call goes through dispatchGuestBranch.
    bool guestCallsTraced() const;
    bool dispatchGuestBranch(uint8_t *rdram,
                             R5900Context *ctx,
                             uint32_t targetPc,
                             uint32_t sourcePc,
                             uint32_t fallthroughPc,
                             GuestBranchKind kind,
                             const char *debugName);
    void reportMissingFunction(uint8_t *rdram,
                               R5900Context *ctx,
                               uint32_t targetPc,
                               uint32_t sourcePc,
                               GuestBranchKind kind,
                               const char *debugName);
    void setMissingFunctionPolicy(MissingFunctionPolicy policy);
    MissingFunctionPolicy missingFunctionPolicy() const;
    void resetMissingFunctionReportOnce();

    static const IoPaths &getIoPaths();
    static void setIoPaths(const IoPaths &paths);
    // The disc image as a file system when IoPaths::cdFromImage is set.
    static std::shared_ptr<const zx::disc::Iso9660Image> cdImageFileSystem();
    static void configureIoPathsFromElf(const std::string &elfPath, bool deriveRoots = true);

    void SignalException(R5900Context *ctx, PS2Exception exception);

    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address);

public:
    void handleSyscall(uint8_t *rdram, R5900Context *ctx);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(uint8_t *rdram, R5900Context *ctx);

    void handleTrap(uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(R5900Context *ctx);
    void configureGuestHeap(uint32_t guestBase, uint32_t guestLimit = PS2_LEGACY_RAM_SIZE);
    uint32_t guestMalloc(uint32_t size, uint32_t alignment = 16u);
    uint32_t guestCalloc(uint32_t count, uint32_t size, uint32_t alignment = 16u);
    uint32_t guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment = 16u);
    void guestFree(uint32_t guestAddr);
    uint32_t guestHeapBase() const;
    uint32_t guestHeapEnd() const;
    uint32_t guestHeapLimit() const;
    // Extended memory: guest RAM in [base, end) (above PS2_LEGACY_RAM_SIZE)
    // becomes heap for guestMallocExtended, and for guestMalloc (the
    // game's malloc) only when the 32 MB heap has no room: every
    // allocation that fits there lands where it would on a PS2. It
    // survives heap resets; guestFree and guestRealloc take its blocks
    // like any other. False if it is not above
    // the 32 MB or overlaps a range given before.
    bool addGuestHeapRegion(uint32_t base, uint32_t end);
    // The main thread's stack grows down from `top` (SetupThread): the
    // heap stops `guard` bytes below it (at most the requested stack
    // size), so a full heap fails an allocation instead of overwriting the
    // stack. Applies to the current heap and every later reset.
    void guardMainStack(uint32_t top, uint32_t guard);
    // A block of extended memory, or 0 if there is none or no room.
    uint32_t guestMallocExtended(uint32_t size, uint32_t alignment = 16u);
    // The top `bytes` of extended memory (rounded up to 16), kept out of
    // every heap for good: no allocation lands there, extended or the
    // game's, and ranges added before or after (addGuestHeapRegion) and
    // heap resets stop below it. Everything else in extended memory sits
    // where it would without it (the heaps there fill from the bottom).
    // One range per runtime, ending at PS2_RAM_SIZE: asked for no more
    // than it has, the same range; for more, it grows down. Its base, or 0
    // when bytes is 0, more than extended memory, or a block in use is in
    // the way.
    uint32_t reserveExtendedTop(uint32_t bytes);
    // Bytes of extended memory in use (for logs).
    uint32_t extendedHeapInUse() const;
    // The heap's bytes in use, in the 32 MB and in extended memory, and the most each has held since the
    // last reset of the peaks (GuestHeap::Usage; ZX_TRACE_HEAP logs them per level: runtime/heap_trace.h).
    GuestHeap::Usage guestHeapUsage() const;
    void resetGuestHeapPeaks();
    uint32_t reserveAsyncCallbackStack(uint32_t size, uint32_t alignment = 16u);

    void drainCompletedDmacHandlers(uint8_t *rdram);
    // Dispatches INTC interrupts raised by devices (e.g. the VIF1 interrupt bit).
    void drainPendingIntcInterrupts();

    void requestStop();
    bool isStopRequested() const;

    EeScheduler &eeScheduler();
    const EeScheduler &eeScheduler() const;

    // COP0 Count (register 9) runs at the EE clock. Generated MFC0/MTC0 Count
    // go through these so the register follows emulated time.
    uint32_t readCop0Count() const noexcept;
    void writeCop0Count(uint32_t value) noexcept;
    void postEeEvent(EeEvent event);
    // Whether a checkpoint is due after `cycles` more: generated code returns (unwinds to the scheduler) when
    // it is. Not inside a host-made guest call (callGuestFunction): that returns to host code, which cannot be
    // resumed later. (Inline, one call into the scheduler: generated code asks at its loops' back edges and
    // readable code at its calls.)
    bool eeCheckpointDue(uint32_t cycles = 32u) noexcept
    {
        if (m_hostGuestCallDepth != 0u)
        {
            eeSchedulerAccountCycles(*m_eeScheduler.get(), cycles);
            return false;
        }
        const bool due = eeSchedulerCheckpointDue(*m_eeScheduler.get(), cycles);
        if (due)
            ++m_guestUnwinds;
        return due;
    }
    [[noreturn]] void eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc);

    struct EeExitHandlerRegistration
    {
        uint32_t function = 0;
        uint32_t argument = 0;
    };
    void addEeExitHandler(int threadId, uint32_t function, uint32_t argument);
    std::vector<EeExitHandlerRegistration> takeEeExitHandlers(int threadId);
    void removeEeExitHandlers(int threadId);
    bool findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const;
    void setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler);
    void initializeEeKernelState(uint8_t *rdram);

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);
    void kickGifDmaChainFromMMIO(uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t dPcrValue,
                                 uint32_t dStatValue,
                                 uint32_t tadr,
                                 uint32_t chcr);

    static inline bool isSpecialAddress(uint32_t addr)
    {
        return Ps2IsSpecialAddress(addr);
    }

public:
    inline R5900Context &cpu() { return m_cpuContext; }
    inline const R5900Context &cpu() const { return m_cpuContext; }

    inline PS2Memory &memory() { return m_memory; }
    inline const PS2Memory &memory() const { return m_memory; }

    inline GS &gs() { return m_gs; }
    inline const GS &gs() const { return m_gs; }
    inline GifArbiter &gifArbiter() { return m_gifArbiter; }
    inline const GifArbiter &gifArbiter() const { return m_gifArbiter; }
    inline VU1Interpreter &vu0() { return m_vu0; }
    inline const VU1Interpreter &vu0() const { return m_vu0; }
    inline VU1Interpreter &vu1() { return m_vu1; }
    inline const VU1Interpreter &vu1() const { return m_vu1; }

    inline PS2AudioBackend &audioBackend() { return m_audioBackend; }
    inline const PS2AudioBackend &audioBackend() const { return m_audioBackend; }
    inline PSPadBackend &padBackend() { return m_padBackend; }
    inline const PSPadBackend &padBackend() const { return m_padBackend; }
    inline PS2RomDevice &romDevice() { return m_romDevice; }
    inline const PS2RomDevice &romDevice() const { return m_romDevice; }
    inline PS2Vfs &vfs() { return m_vfs; }
    inline const PS2Vfs &vfs() const { return m_vfs; }

private:
    static uint32_t alignGuestHeapValue(uint32_t value, uint32_t alignment);
    static bool isGuestHeapAlignmentValid(uint32_t alignment);
    static uint32_t normalizeGuestHeapAlignment(uint32_t alignment);
    uint32_t clampGuestHeapBase(uint32_t guestBase) const;
    uint32_t clampGuestHeapLimit(uint32_t guestLimit) const;
    void resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit);
    void ensureGuestHeapInitializedLocked();
    uint32_t allocateGuestBlockLocked(uint32_t size, uint32_t alignment, bool extended = false);
    void HandleIntegerOverflow(R5900Context *ctx);

    [[nodiscard]] ps2x::iop::RpcAbi selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const;
    [[nodiscard]] bool canBindIopRpc(uint32_t sid) const noexcept;
    [[nodiscard]] bool hasIopModuleRpcServer(uint32_t sid) const noexcept;
    [[nodiscard]] ps2x::iop::RpcResult handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request);
    void notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer);
    // The IOP's share of an EE charge. (Inline: it runs at every charge. While the IOP is idle with nothing due,
    // a charge only moves its clock: those cycles wait here, up to what it takes so, and reach it in one go
    // before anything else of it is used, giveIopIdleCycles.)
    void advanceIopEeCycles(uint64_t eeCycles) noexcept
    {
        if (m_iopStopped)
            return;
        if (eeCycles < m_iopIdleRoom)
        {
            m_iopIdleRoom -= eeCycles;
            m_iopIdleCycles += eeCycles;
            return;
        }
        runIopEeCycles(eeCycles);
    }
    void runIopEeCycles(uint64_t eeCycles) noexcept;
    void giveIopIdleCycles() noexcept;
    [[nodiscard]] uint64_t iopEeCyclesUntilWork() noexcept;
public:
    // Game override: true while the guest shows a loading screen (EE RAM at
    // a VBlank). The scheduler runs such stretches unpaced while the sound
    // output is silent (EeScheduler, ZX_LOAD_FAST_FORWARD).
    using LoadingScreenProbe = bool (*)(const uint8_t *rdram);
    void setLoadingScreenProbe(LoadingScreenProbe probe) noexcept { m_loadingScreenProbe = probe; }
    [[nodiscard]] LoadingScreenProbe loadingScreenProbe() const noexcept { return m_loadingScreenProbe; }
    // SPU2 output frames (48 kHz) since the last non-zero sample.
    [[nodiscard]] uint64_t guestAudioSilentFrames() const noexcept;
    // Game override: a native sound driver that replaces the game's IOP
    // sound module plays its 48 kHz stereo frames (made in guest time) here;
    // while setNativeAudio(true) the SPU2's own output is not played. The
    // frames go the way the SPU2's did (movie sound mixed in, ZX_SPU2_WAV,
    // the host stream, the silence that lets a loading screen run unpaced).
    void setNativeAudio(bool active) noexcept;
    void submitNativeAudio(const int16_t *interleavedStereo, size_t frames);
    // Game override: called on the EE thread at every VBlank start, before
    // the guest sees it (guest time, so the same on every run).
    using VSyncHook = std::function<void(uint8_t *rdram, uint64_t tick)>;
    void addVSyncHook(VSyncHook hook) { m_vsyncHooks.push_back(std::move(hook)); }
    void runVSyncHooks(uint8_t *rdram, uint64_t tick)
    {
        for (const VSyncHook &hook : m_vsyncHooks)
            hook(rdram, tick);
    }
    // Game override: IRX file names (e.g. "PADMAN.IRX") whose
    // sceSifLoadModule succeeds without loading anything, for drivers the
    // game loads but never uses. ZX_LOAD_ALL_IOP_MODULES=1 loads them.
    void setSkippedIopModules(std::vector<std::string> names) { m_skippedIopModules = std::move(names); }
    // Game override: the game needs nothing the IOP runs (every module it
    // loads is skipped and replaced on the host). The IOP then does not run
    // at all: no R3000 interpreter, no IOP clock, no SPU2 mixing; its
    // memory and heap stay for the EE's bookkeeping calls.
    void setIopStopped(bool stopped) noexcept
    {
        giveIopIdleCycles();
        m_iopStopped = stopped;
    }
    [[nodiscard]] bool iopStopped() const noexcept { return m_iopStopped; }
    [[nodiscard]] bool isSkippedIopModule(std::string_view path) const;
    // Reply for an IOP -> EE RPC started through PS2IopHostAdapter::callEeRpc.
    void completeIopEeRpc(uint64_t token, const void *reply, size_t replySize);
private:
    void resetIop();

    friend class PS2IopTransport;
    friend class EeScheduler;

private:
    // First: every other member is made within the boot, and gone before it ends.
    std::unique_ptr<zx::boot::Scope> m_boot;
    PS2Memory m_memory;
    GifArbiter m_gifArbiter;
    GS m_gs;
    std::unique_ptr<PS2IopHostAdapter> m_iopHost;
    std::unique_ptr<ps2x::iop::IopSubsystem> m_iopSubsystem;
    uint32_t m_cop0CountBias = 0u; // Count = (EE cycle + bias) mod 2^32
    LoadingScreenProbe m_loadingScreenProbe = nullptr;
    std::vector<std::string> m_skippedIopModules;
    std::vector<VSyncHook> m_vsyncHooks;
    std::vector<StoppedHook> m_stoppedHooks;
    bool m_iopStopped = false;
    uint64_t m_iopIdleCycles = 0; // EE cycles charged while the IOP is idle, not given to it yet (advanceIopEeCycles)
    uint64_t m_iopIdleRoom = 0;   // how many more it takes as its clock alone (0: none, the next charge asks)
    PS2AudioBackend m_audioBackend;
    PSPadBackend m_padBackend;
    PS2RomDevice m_romDevice;
    PS2Vfs m_vfs;
    VU1Interpreter m_vu0{VU1Interpreter::Unit::VU0};
    VU1Interpreter m_vu1{VU1Interpreter::Unit::VU1};
    R5900Context m_cpuContext;
    std::unique_ptr<EeScheduler> m_eeScheduler;
    mutable std::mutex m_eeKernelStateMutex;
    std::unordered_map<int, std::vector<EeExitHandlerRegistration>> m_eeExitHandlers;
    std::unordered_map<uint32_t, uint32_t> m_eeSyscallOverrides;
    std::unordered_set<uint32_t> m_eeSyscallMirrorAddresses;
    mutable std::mutex m_guestHeapMutex;
    mutable std::mutex m_asyncCallbackStackMutex;
    GuestHeap m_guestHeap; // the blocks (guest_heap.h); the fields below are the policy's
    uint32_t m_guestHeapBase = 0x00100000u;
    uint32_t m_guestHeapEnd = 0x00100000u;
    uint32_t m_guestHeapLimit = PS2_LEGACY_RAM_SIZE;
    uint32_t m_guestHeapSuggestedBase = 0x00100000u;
    bool m_guestHeapConfigured = false;
    std::vector<std::pair<uint32_t, uint32_t>> m_guestHeapExtraRegions; // [base, end)
    uint32_t m_extendedReserved = 0;        // bytes at the top of extended memory no heap gets (reserveExtendedTop)
    uint32_t m_mainStackFloor = 0xFFFFFFFFu; // the heap's limit at most (guardMainStack)
    bool m_guestHeapOverflowed = false;      // logged the first malloc in extended memory
    // Times generated code unwound to the EE scheduler (checkpoint yields,
    // jumps, unresolved calls), see dispatchGuestBranch.
    uint64_t m_guestUnwinds = 0;
    uint32_t m_hostGuestCallDepth = 0; // callGuestFunction nesting: no safe points
    uint32_t m_asyncCallbackStackFloor = 0x01F00000u;
    uint32_t m_asyncCallbackStackTop = PS2_LEGACY_RAM_SIZE;

    std::atomic<uint32_t> m_missingFunctionPolicy{static_cast<uint32_t>(MissingFunctionPolicy::ContinueToTarget)};
    std::atomic<bool> m_missingFunctionReported{false};
    std::atomic<bool> m_stopRequested{false};
    DebugUiCallback m_debugUiInitCallback = nullptr;
    DebugUiCallback m_debugUiDrawCallback = nullptr;
    DebugUiCallback m_debugUiShutdownCallback = nullptr;
    void *m_debugUiUserData = nullptr;
    bool m_debugUiInitialized = false;
    HostOptions m_hostOptions;
    std::atomic<bool> m_widescreen{false};
    std::atomic<uint64_t> m_widescreenMarks{0};
    std::atomic<float> m_wideAspect{16.0f / 9.0f};
    std::atomic<float> m_windowAspect{16.0f / 9.0f};
    std::atomic<int> m_motionBlur{2};
    std::atomic<int> m_drawDistance{0};
    std::mutex m_screenshotMutex;
    std::shared_ptr<const PictureFilter> m_pictureFilter; // under m_screenshotMutex
    std::vector<std::pair<std::string, std::function<void(bool)>>> m_screenshotRequests;
    bool m_rasterReloadRequested = false;
    std::unique_ptr<GSRasterBackend> m_pendingRasterBackend;
    HostFrameCallback m_hostFrameCallback;

public:
    std::atomic<uint32_t> m_debugPc{0};
    std::atomic<uint32_t> m_debugRa{0};
    std::atomic<uint32_t> m_debugSp{0};
    std::atomic<uint32_t> m_debugGp{0};

private:
    struct LoadedModule
    {
        std::string name;
        uint32_t baseAddress;
        size_t size;
        bool active;
    };

    std::vector<LoadedModule> m_loadedModules;
    uint8_t *m_boundRdram = nullptr;
    uint8_t *m_boundGSVram = nullptr;
};

// Generated by ps2xRecomp in ps2/runtime/register_functions.cpp.
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Machine::RecompiledFunction g_ps2RecompiledFunctionTable[];

// Several games' generated code in one executable: code recompiled with
// [general] namespace = "<id>" registers its function table here (from its
// register_functions.cpp) instead of defining the global table above. At ELF
// load the runtime uses the set registered for that ELF's file name, else the
// global table (the single-game build: nothing changes).
namespace ps2_generated
{
    struct Code
    {
        const char *id = "";
        const char *elfName = "";
        uint32_t base = 0;
        uint32_t end = 0;
        uint32_t slotCount = 0;
        PS2Machine::RecompiledFunction *table = nullptr;
    };
    void add(const Code &code);
    // The code for an ELF no set names (AI's: its module registers as the default, runtime/game_module.h); without
    // one, the global table above.
    void setDefault(const Code &code);
    const std::vector<Code> &all();
    // The set for elfName (file name, case-insensitive) or the global table;
    // returns the chosen id ("" = the global table).
    std::string select(const std::string &elfName);
    struct Registrar
    {
        Registrar(const char *id, const char *elfName, uint32_t base, uint32_t end, uint32_t slotCount,
                  PS2Machine::RecompiledFunction *table)
        {
            const Code code{id, elfName, base, end, slotCount, table};
            if (id[0] == '\0')
                setDefault(code);
            else
                add(code);
        }
    };
}

#endif // PS2_MACHINE_H
