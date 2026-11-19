#include "runtime/lane_watch.h"
#include "ps2_runtime.h"
#include "runtime/ps2_function_table.h"
#include "runtime/game_module.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_frame_trace.h"
#include "runtime/ps2_host_pacing.h"
#include "runtime/gs/gs_interpolate.h"
#include "runtime/ps2_frame_interpolation.h"
#include "runtime/ps2_vu1_record.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_null_backend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_debug_control.h"
#include "runtime/ps2_state_probe.h"
#include "ps2_iop_host.h"
#include "runtime/ps2_thread_cpu.h"
#include "runtime/ps2_thread_placement.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/play_recording.h"
#include "runtime/ps2_spot_fork.h"
#include "runtime/ps2_games.h"
#include "runtime/ps2_front_end.h"
#include "runtime/heap_trace.h"

#include <iostream>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <cmath>
#include <iterator>
#include <functional>
#include <memory>

#if defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

namespace ps2_stubs
{
    void resetSifState();
}


namespace
{
    // All recompiled guest code (every EE thread, multiplexed by EeScheduler)
    // runs on a single host "game thread", so its stack depth follows the guest
    // call depth. macOS gives secondary threads only 512 KiB of stack (Linux
    // uses 8 MiB, Windows 1 MiB), so on Apple hosts the game thread is created
    // through pthreads with an explicit, Linux-sized stack; Android's threads
    // get about 1 MiB, so there too. On Windows the
    // runner is linked with an 8 MiB default stack reserve (/STACK), which
    // std::thread uses.
    class GameHostThread
    {
    public:
        explicit GameHostThread(std::function<void()> body)
        {
#if defined(__APPLE__) || defined(__ANDROID__)
            constexpr size_t kStackSize = 8u * 1024u * 1024u;
            auto *heapBody = new std::function<void()>(std::move(body));
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, kStackSize);
            const int rc = pthread_create(
                &m_thread, &attr,
                [](void *arg) -> void *
                {
                    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(arg));
                    (*fn)();
                    return nullptr;
                },
                heapBody);
            pthread_attr_destroy(&attr);
            if (rc != 0)
            {
                delete heapBody;
                throw std::runtime_error("Failed to create game thread");
            }
            m_joinable = true;
#else
            m_thread = std::thread(std::move(body));
#endif
        }

        GameHostThread(const GameHostThread &) = delete;
        GameHostThread &operator=(const GameHostThread &) = delete;

        ~GameHostThread()
        {
            if (joinable())
            {
                join();
            }
        }

        bool joinable() const
        {
#if defined(__APPLE__) || defined(__ANDROID__)
            return m_joinable;
#else
            return m_thread.joinable();
#endif
        }

        void join()
        {
#if defined(__APPLE__) || defined(__ANDROID__)
            pthread_join(m_thread, nullptr);
            m_joinable = false;
#else
            m_thread.join();
#endif
        }

    private:
#if defined(__APPLE__) || defined(__ANDROID__)
        pthread_t m_thread{};
        bool m_joinable = false;
#else
        std::thread m_thread;
#endif
    };
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_LEGACY_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    uint32_t computeBufferCrc32(const std::vector<uint8_t> &bytes)
    {
        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        for (const uint8_t byte : bytes)
        {
            crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
        }
        return ~crc;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }


        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// Display buffer changes seen by UploadFrame: the game's own frame rate for
// double-buffered games (PS2X_PERF_LOG).
static std::atomic<uint64_t> s_displayFlips{0};

uint64_t PS2Runtime::displayFlips()
{
    return s_displayFlips.load(std::memory_order_relaxed);
}
// The GS renderer was switched: the next host frame latches a new picture.
static bool s_forceLatch = false;
// Roughly process start (static initialization), for PS2X_PERF_LOG's startup time.
static const std::chrono::steady_clock::time_point s_processStart = std::chrono::steady_clock::now();

// The presented picture: the latched display buffer copied into a
// FB_WIDTH x FB_HEIGHT RGBA8 image (top left outWidth x outHeight). changed
// is false when it is the previous frame's image again. With the frame
// queue (GS::setFrameQueue) the picture is the oldest game frame waiting
// (taken.frame its number, 0 for a picture latched here).
// dueTick >= 0 (frame interpolation): a waiting frame is taken only once the refresh's display time (in vblank
// ticks) has reached it; until then the window shows pictures between the latched frame and it.
static const uint8_t *UploadFrame(PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight, bool &changed,
                                  GS::TakenFrame &taken, double dueTick = -1.0)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    // When to take the picture. Latching drains the GS raster work and is
    // taken again if the game's buffer swap lands during the copy, which
    // costs the GS worker (and the EE behind it) a lot. A vsynced host loop
    // (120 Hz on ProMotion) can lock onto the moment just before the swap,
    // so a double-buffered game's frame is taken when its display buffer
    // changes, right after the swap. Single-buffered pictures (no display
    // change for a while) are taken every vsync, and every few vsyncs at
    // the latest in any case. A change is a new value of the display
    // registers or a change of the display buffer since the last picture
    // (PS2Memory::displayChanges): a host frame that took two refreshes can
    // see the registers back where they were (A, B, A) after two swaps.
    static uint64_t s_lastDisplayKey = 0u;
    static uint64_t s_lastDisplayChanges = 0u;
    static uint64_t s_lastDisplayChangeTick = 0u;
    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    uint64_t displayKey = 0u;
    {
        // Read without the GS lock: only a hint for when to latch.
        GSRegisters &regs = rt->memory().gs();
#if defined(__ANDROID__) // (the NDK's libc++ has no atomic_ref)
        auto load = [](uint64_t &value)
        { return __atomic_load_n(&value, __ATOMIC_RELAXED); };
#else
        auto load = [](uint64_t &value)
        { return std::atomic_ref<uint64_t>(value).load(std::memory_order_relaxed); };
#endif
        displayKey = load(regs.dispfb1) * 0x9E3779B97F4A7C15ull ^ load(regs.dispfb2) * 0xC2B2AE3D27D4EB4Full ^
                     load(regs.display1) * 0x165667B19E3779F9ull ^ load(regs.display2) ^
                     (load(regs.pmode) << 17) ^ (load(regs.smode2) << 43);
    }
    const uint64_t displayChanges = rt->memory().displayChanges();
    const bool displayChanged = displayKey != s_lastDisplayKey || displayChanges != s_lastDisplayChanges;
    if (displayChanged)
        s_lastDisplayChangeTick = currentTick;
    const bool singleBuffered = currentTick - s_lastDisplayChangeTick > 8u;
    taken = GS::TakenFrame{};
    // The frame queue: each buffer swap's picture is made on the GS worker;
    // this takes the oldest waiting, and latches here only what has no swap
    // (the first picture, a single-buffered one, a renderer switch).
    const bool queued = rt->gs().frameQueue() && s_hasLatchedInitialFrame && !s_forceLatch &&
                        (dueTick < 0.0 || [&]()
                         {
                             GS::TakenFrame next;
                             if (!rt->gs().peekPresentationFrame(next))
                                 return false;
                             const GS::TakenFrame shown = rt->gs().shownPresentationFrame();
                             return shown.frame == 0u || next.vsync <= shown.vsync ||
                                    ps2x::gs_interpolate::alphaBetween(shown.vsync, next.vsync, dueTick) >=
                                        1.0f - ps2x::gs_interpolate::kSnap;
                         }()) &&
                        rt->gs().takePresentationFrame(taken);
    const bool needsLatch =
        queued || !s_hasLatchedInitialFrame || s_forceLatch ||
        (currentTick != s_lastPresentationTick &&
         (rt->gs().frameQueue() ? singleBuffered
                                : (displayChanged || singleBuffered || currentTick - s_lastPresentationTick >= 4u)));
    // (the display buffer changed since the last picture, A, B, A included)
    const bool bufferChanged = queued || (needsLatch && displayChanges != s_lastDisplayChanges);
    if (needsLatch)
    {
        if (!queued)
            rt->gs().latchHostPresentationFrame();
        s_forceLatch = false;
        s_lastPresentationTick = currentTick;
        s_lastDisplayKey = displayKey;
        s_lastDisplayChanges = displayChanges;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        changed = false;
        return s_uploadBuffer.data();
    }
    changed = true;

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    bool onGpu = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource,
                                                   &onGpu))
    {
        // No frame yet: magenta.
        for (size_t i = 0; i + 3u < s_uploadBuffer.size(); i += 4u)
        {
            s_uploadBuffer[i] = 0xFFu;
            s_uploadBuffer[i + 1u] = 0x00u;
            s_uploadBuffer[i + 2u] = 0xFFu;
            s_uploadBuffer[i + 3u] = 0xFFu;
        }
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return s_uploadBuffer.data();
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    if (displayFbp != s_lastDisplayFbp || sourceFbp != s_lastSourceFbp || bufferChanged)
    {
        s_displayFlips.fetch_add(1u, std::memory_order_relaxed);
        ps2x::ftrace::mark(ps2x::ftrace::kFlipSeen, static_cast<uint16_t>(displayFbp & 0xFFFFu));
    }
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
    if (onGpu)
        return s_uploadBuffer.data(); // the GPU renderer's texture is shown instead

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }
    return s_uploadBuffer.data();
}

PS2Runtime::PS2Runtime() : PS2Runtime(ps2x::BootConfig{})
{
}

PS2Runtime::PS2Runtime(ps2x::BootConfig config) : m_boot(std::make_unique<ps2x::boot::Scope>(std::move(config)))
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeap.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_LEGACY_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        // Everything that holds a host object first (the IOP's SPU2 audio
        // stream, the sounds), then the host, which SDL_Quit ends. The GS
        // renderer (a member, destroyed after this) gives its GPU resources
        // back through the host's device release hook, or itself when the
        // host stays up.
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (!m_hostOptions.hostOwned)
            ps2_host::shutdown();

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

bool PS2Runtime::hasIopModuleRpcServer(uint32_t sid) const noexcept
{
    return m_iopSubsystem->hasModuleRpcServer(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::runIopEeCycles(uint64_t eeCycles) noexcept
{
    giveIopIdleCycles();
    m_iopSubsystem->runEeCycles(eeCycles);
    m_iopIdleRoom = m_iopSubsystem->eeCyclesIdle();
}

// The idle cycles charged so far, given to the IOP (its clock alone moves: as each charge moved it), before anything
// else of it is used.
void PS2Runtime::giveIopIdleCycles() noexcept
{
    m_iopIdleRoom = 0;
    if (!m_iopIdleCycles)
        return;
    const uint64_t cycles = m_iopIdleCycles;
    m_iopIdleCycles = 0;
    m_iopSubsystem->runEeCycles(cycles);
}

bool PS2Runtime::isSkippedIopModule(std::string_view path) const
{
    static const bool loadAll = []()
    {
        const char *env = std::getenv("PS2X_LOAD_ALL_IOP_MODULES");
        return env && env[0] == '1';
    }();
    if (loadAll || m_skippedIopModules.empty())
        return false;
    // "cdrom0:\PADMAN.IRX;1" -> "PADMAN.IRX"
    const size_t slash = path.find_last_of("\\/:");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1u);
    name = name.substr(0, name.find(';'));
    for (const std::string &skipped : m_skippedIopModules)
    {
        if (skipped.size() == name.size() &&
            std::equal(skipped.begin(), skipped.end(), name.begin(), [](char a, char b)
                       { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
            return true;
    }
    return false;
}

uint64_t PS2Runtime::guestAudioSilentFrames() const noexcept
{
    return m_iopHost ? m_iopHost->silentAudioFrames() : std::numeric_limits<uint64_t>::max();
}

void PS2Runtime::setNativeAudio(bool active) noexcept
{
    if (m_iopHost)
        m_iopHost->setNativeAudio(active);
}

void PS2Runtime::submitNativeAudio(const int16_t *interleavedStereo, size_t frames)
{
    // PS2X_SOUND_PCM=<file>: every frame the native sound driver makes, as
    // it makes them (16-bit stereo, 48 kHz, raw): in guest time, so an
    // unpaced run on a fixed input writes the same bytes each time.
    static std::FILE *pcm = []() -> std::FILE *
    {
        const char *path = std::getenv("PS2X_SOUND_PCM");
        return path && path[0] ? std::fopen(path, "wb") : nullptr;
    }();
    if (pcm && interleavedStereo && frames)
    {
        std::fwrite(interleavedStereo, sizeof(int16_t) * 2u, frames, pcm);
        std::fflush(pcm);
    }
    if (m_iopHost)
        m_iopHost->submitNativeAudio(interleavedStereo, frames);
}

uint64_t PS2Runtime::iopEeCyclesUntilWork() noexcept
{
    if (m_iopSubsystem)
        giveIopIdleCycles();
    return m_iopSubsystem && !m_iopStopped ? m_iopSubsystem->eeCyclesUntilWork() : std::numeric_limits<uint64_t>::max();
}

uint32_t PS2Runtime::readCop0Count() const noexcept
{
    const uint64_t cycle = m_eeScheduler ? m_eeScheduler->eeCycle() : 0u;
    return static_cast<uint32_t>(cycle) + m_cop0CountBias;
}

void PS2Runtime::writeCop0Count(uint32_t value) noexcept
{
    const uint64_t cycle = m_eeScheduler ? m_eeScheduler->eeCycle() : 0u;
    m_cop0CountBias = value - static_cast<uint32_t>(cycle);
}

void PS2Runtime::completeIopEeRpc(uint64_t token, const void *reply, size_t replySize)
{
    giveIopIdleCycles();
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    m_iopSubsystem->completeEeRpc(token, reply, replySize);
}

void PS2Runtime::resetIop()
{
    giveIopIdleCycles();
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    if (m_iopSubsystem)
        giveIopIdleCycles();
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    if (m_iopSubsystem)
        giveIopIdleCycles();
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    if (m_iopSubsystem)
        giveIopIdleCycles();
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    if (m_iopSubsystem)
        giveIopIdleCycles();
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    // Callers (GS HLE) touch the GS and its registers directly.
    m_memory.syncGraphics(PS2Memory::SyncGsHle);
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_padBackend.setVsyncSource(&m_memory.gs().vsyncTick);
    m_padBackend.setGuestMemory(rdram);
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_memory.deliverGifPacket(data, size); });
    m_memory.setGsPacketSink([this](const uint8_t *data, uint32_t size)
                             { m_gs.processGIFPacket(data, size); });
    m_memory.setGsIdleSink([this]()
                           { m_gs.workerIdle(); });
    m_memory.setDisplayChangeSink([this]()
                                  { m_gs.capturePresentationFrame(); });
    m_gs.setDisplayChangeCount([this]()
                               { return m_memory.displayChanges(); });
    // VIF1 work may run on the VIF1 worker unless a VU1 D/T stop could have
    // to be reported in the current EE context's VPU-STAT.
    m_memory.setVu1AsyncAllowed([this]()
                                {
                                    const R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                    if (!cpuContext)
                                        cpuContext = &m_cpuContext;
                                    return (cpuContext->vu0_fbrst & 0x0C00u) == 0u &&
                                           (cpuContext->vu0_vpu_stat & 0x0600u) == 0u; });
    m_memory.setGifArbiter(&m_gifArbiter);
    // PS2X_TRACE_VU1=<n> logs the first n VU1 microprogram runs (start pc,
    // cycles, end pc/E-bit and XGKICK packets submitted).
    auto traceVu1Run = [this](const char *kind, uint32_t startPC, uint32_t top, uint32_t itop,
                              uint64_t cyclesBefore, uint64_t kicksBefore)
    {
        static const long limit = []()
        {
            const char *value = std::getenv("PS2X_TRACE_VU1");
            return value ? std::strtol(value, nullptr, 0) : 0L;
        }();
        static long logged = 0;
        if (logged >= limit)
            return;
        ++logged;
        const VU1State &st = m_vu1.state();
        std::fprintf(stderr, "[vu1-trace] %s start=0x%x top=0x%x itop=0x%x cycles=%llu end_pc=0x%x ebit=%d xgkicks=%llu\n",
                     kind, startPC, top, itop, static_cast<unsigned long long>(st.cycles - cyclesBefore), st.pc,
                     st.ebit ? 1 : 0, static_cast<unsigned long long>(st.xgkickPackets - kicksBefore));
    };
    // PS2X_VU1_SKIP=1 (for PS2X_GS_NULL runs that only look at game state):
    // MSCAL/MSCNT return at once without running the microprogram, as if
    // it ended immediately. Its only output, the XGKICK packets for the GS,
    // is dropped by the null backend anyway. Not valid if the game reads
    // VU1 memory back.
    static const bool skipVu1 = []()
    {
        const char *value = std::getenv("PS2X_VU1_SKIP");
        const bool enabled = value && value[0] != '\0' && std::strcmp(value, "0") != 0;
        if (enabled)
            std::fprintf(stderr, "[vu1] PS2X_VU1_SKIP: microprograms are not run\n");
        return enabled;
    }();
    // PS2X_VU1_RECORD=<file>: write every VU1 run (inputs, outputs, PATH1
    // packets) to a corpus for ps2xVu1Check. PS2X_VU1_RECORD_FROM=<vsync>
    // starts recording at that guest vsync, PS2X_VU1_RECORD_VSYNCS=<n> stops
    // after n vsyncs (default 120), PS2X_VU1_RECORD_RUNS=<n> after n runs.
    // A run that used its whole budget without reaching an E bit (or a D/T
    // stop) was cut short; say so once (VU1Interpreter::kProgramCycleLimit).
    auto warnVu1CycleLimit = [this](const char *what, uint32_t pc, uint64_t cyclesBefore)
    {
        static std::atomic<bool> warned{false};
        const VU1State &state = m_vu1.state();
        if (state.cycles - cyclesBefore < VU1Interpreter::kProgramCycleLimit || state.stoppedByD || state.stoppedByT ||
            warned.exchange(true))
            return;
        std::fprintf(stderr, "[vu1] %s at pc 0x%x ran %u cycles without ending (stopped at pc 0x%x)\n", what, pc,
                     VU1Interpreter::kProgramCycleLimit, state.pc);
    };
    auto recordVu1Run = [this](uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop,
                               uint32_t maxCycles, const std::function<void()> &untimedRun)
    {
        // PS2X_VU1_STATS=1: every 2 s of wall time, log the VU1 runs, pairs,
        // VU cycles and the wall and thread CPU time spent in them.
        static const bool stats = []()
        {
            const char *value = std::getenv("PS2X_VU1_STATS");
            return value && value[0] == '1';
        }();
        auto run = [&]()
        {
            if (!stats)
            {
                untimedRun();
                return;
            }
            static uint64_t runs = 0, pairs = 0, cycles = 0;
            static double wallSeconds = 0.0, cpuSeconds = 0.0;
            static auto lastReport = std::chrono::steady_clock::now();
            auto threadCpu = []()
            { return ps2x::currentThreadCpuSeconds(); };
            const uint64_t pairs0 = m_vu1.pairsExecuted();
            const uint64_t cycles0 = m_vu1.state().cycles;
            const auto wall0 = std::chrono::steady_clock::now();
            const double cpu0 = threadCpu();
            untimedRun();
            const auto wall1 = std::chrono::steady_clock::now();
            cpuSeconds += threadCpu() - cpu0;
            wallSeconds += std::chrono::duration<double>(wall1 - wall0).count();
            ++runs;
            pairs += m_vu1.pairsExecuted() - pairs0;
            cycles += m_vu1.state().cycles - cycles0;
            if (wall1 - lastReport >= std::chrono::seconds(2))
            {
                std::fprintf(stderr, "[vu1-stats] vsync=%llu runs=%llu pairs=%llu cycles=%llu wall_ms=%.1f cpu_ms=%.1f\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(runs), static_cast<unsigned long long>(pairs),
                             static_cast<unsigned long long>(cycles), wallSeconds * 1e3, cpuSeconds * 1e3);
                runs = pairs = cycles = 0;
                wallSeconds = cpuSeconds = 0.0;
                lastReport = wall1;
            }
        };
        static ps2vu1rec::Recorder recorder;
        static const char *path = std::getenv("PS2X_VU1_RECORD");
        static const uint64_t from = []()
        {
            const char *value = std::getenv("PS2X_VU1_RECORD_FROM");
            return value ? std::strtoull(value, nullptr, 0) : 0ull;
        }();
        static const uint64_t vsyncs = []()
        {
            const char *value = std::getenv("PS2X_VU1_RECORD_VSYNCS");
            return value ? std::strtoull(value, nullptr, 0) : 120ull;
        }();
        static const uint64_t maxRuns = []()
        {
            const char *value = std::getenv("PS2X_VU1_RECORD_RUNS");
            return value ? std::strtoull(value, nullptr, 0) : ~0ull;
        }();
        static bool finished = false;
        const uint64_t vsync = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
        if (!path || !path[0] || finished || vsync < from)
        {
            run();
            return;
        }
        if (!recorder.isOpen())
        {
            if (!recorder.open(path))
            {
                std::fprintf(stderr, "[vu1-record] cannot open %s\n", path);
                finished = true;
                run();
                return;
            }
            std::fprintf(stderr, "[vu1-record] recording to %s from vsync %llu\n", path,
                         static_cast<unsigned long long>(vsync));
        }
        if (vsync >= from + vsyncs || recorder.runCount() >= maxRuns)
        {
            std::fprintf(stderr, "[vu1-record] done: %llu runs\n",
                         static_cast<unsigned long long>(recorder.runCount()));
            recorder.close();
            finished = true;
            run();
            return;
        }
        recorder.beginRun(kind, startPC, top, itop, maxCycles, m_memory.getVU1Code(), m_memory.getVU1Data(),
                          m_vu1.state(), m_vu1.pipelinesIdle(), vsync);
        m_vu1.setPacketObserver([](const uint8_t *data, uint32_t size)
                                { recorder.addPacket(data, size); });
        run();
        m_vu1.setPacketObserver(nullptr);
        recorder.endRun(m_memory.getVU1Data(), m_vu1.state());
    };
    m_memory.setVu1MscalCallback([this, traceVu1Run, recordVu1Run, warnVu1CycleLimit](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     // On the VIF1 worker D/T stops are disabled (setVu1AsyncAllowed) and
                                     // VPU-STAT stays clear: a scratch context keeps the EE's untouched.
                                     static thread_local R5900Context workerContext;
                                     workerContext.vu0_fbrst = 0u;
                                     workerContext.vu0_vpu_stat = 0u;
                                     R5900Context *cpuContext = m_memory.onVu1Worker()
                                                                    ? &workerContext
                                                                    : (m_eeScheduler ? m_eeScheduler->currentContext() : nullptr);
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     if (skipVu1)
                                     {
                                         cpuContext->vu0_vpu_stat &= ~0x0600u;
                                         return;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     const uint64_t cyclesBefore = m_vu1.state().cycles;
                                     const uint64_t kicksBefore = m_vu1.state().xgkickPackets;
                                     // PS2X_VU1_DUMP=<prefix> PS2X_VU1_DUMP_AT=<n>: for the n-th MSCAL (0-based),
                                     // write <prefix>.code, .data-in, .data-out and .regs (vf[32][4], vi[16], start/top/itop).
                                     static const char *vu1DumpPrefix = std::getenv("PS2X_VU1_DUMP");
                                     static const long vu1DumpAt = []()
                                     {
                                         const char *value = std::getenv("PS2X_VU1_DUMP_AT");
                                         return value ? std::strtol(value, nullptr, 0) : -1L;
                                     }();
                                     static long mscalIndex = -1;
                                     ++mscalIndex;
                                     // PS2X_VU1_DUMP_MATCH=<x>,<y> (GS 12.4 integers): instead dump the first MSCAL at or
                                     // after index PS2X_VU1_DUMP_AT whose run writes a qword with those x/y into data memory.
                                     static const bool vu1DumpMatch = std::getenv("PS2X_VU1_DUMP_MATCH") != nullptr;
                                     static const uint32_t vu1MatchX = []()
                                     {
                                         const char *value = std::getenv("PS2X_VU1_DUMP_MATCH");
                                         return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
                                     }();
                                     static const uint32_t vu1MatchY = []()
                                     {
                                         const char *value = std::getenv("PS2X_VU1_DUMP_MATCH");
                                         const char *comma = value ? std::strchr(value, ',') : nullptr;
                                         return comma ? static_cast<uint32_t>(std::strtoul(comma + 1, nullptr, 0)) : 0u;
                                     }();
                                     static bool vu1MatchDone = false;
                                     const bool matchCandidate = vu1DumpPrefix && vu1DumpMatch && !vu1MatchDone && mscalIndex >= vu1DumpAt;
                                     const bool dumpThis = vu1DumpPrefix && ((!vu1DumpMatch && mscalIndex == vu1DumpAt) || matchCandidate);
                                     std::vector<uint8_t> vu1DataBefore;
                                     if (matchCandidate)
                                         vu1DataBefore.assign(m_memory.getVU1Data(), m_memory.getVU1Data() + PS2_VU1_DATA_SIZE);
                                     auto writeFile = [](const std::string &path, const void *data, size_t size)
                                     {
                                         if (FILE *f = std::fopen(path.c_str(), "wb"))
                                         {
                                             std::fwrite(data, 1, size, f);
                                             std::fclose(f);
                                         }
                                     };
                                     std::vector<uint8_t> vu1RegsBefore;
                                     if (dumpThis)
                                     {
                                         if (!matchCandidate)
                                             vu1DataBefore.assign(m_memory.getVU1Data(), m_memory.getVU1Data() + PS2_VU1_DATA_SIZE);
                                         const VU1State &st = m_vu1.state();
                                         vu1RegsBefore.resize(sizeof(st.vf) + sizeof(st.vi) + 12);
                                         std::memcpy(vu1RegsBefore.data(), st.vf, sizeof(st.vf));
                                         std::memcpy(vu1RegsBefore.data() + sizeof(st.vf), st.vi, sizeof(st.vi));
                                         const uint32_t extra[3] = {startPC, top, itop};
                                         std::memcpy(vu1RegsBefore.data() + sizeof(st.vf) + sizeof(st.vi), extra, sizeof(extra));
                                     }
                                     recordVu1Run(0u, startPC, top, itop, VU1Interpreter::kProgramCycleLimit, [&]()
                                                  { m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                                  m_gs, &m_memory, startPC, top, itop,
                                                                  VU1Interpreter::kProgramCycleLimit); });
                                     warnVu1CycleLimit("MSCAL", startPC, cyclesBefore);
                                     bool matched = !matchCandidate;
                                     if (matchCandidate)
                                     {
                                         const uint8_t *after = m_memory.getVU1Data();
                                         for (uint32_t off = 0; off + 16u <= PS2_VU1_DATA_SIZE && !matched; off += 16u)
                                         {
                                             if (std::memcmp(after + off, vu1DataBefore.data() + off, 16u) == 0)
                                                 continue;
                                             uint32_t xy[2];
                                             std::memcpy(xy, after + off, sizeof(xy));
                                             matched = (xy[0] & 0xFFFFu) == vu1MatchX && (xy[1] & 0xFFFFu) == vu1MatchY;
                                         }
                                         if (matched)
                                             vu1MatchDone = true;
                                     }
                                     if (dumpThis && matched)
                                     {
                                         writeFile(std::string(vu1DumpPrefix) + ".code", m_memory.getVU1Code(), PS2_VU1_CODE_SIZE);
                                         writeFile(std::string(vu1DumpPrefix) + ".data-in", vu1DataBefore.data(), vu1DataBefore.size());
                                         writeFile(std::string(vu1DumpPrefix) + ".regs-in", vu1RegsBefore.data(), vu1RegsBefore.size());
                                         writeFile(std::string(vu1DumpPrefix) + ".data-out", m_memory.getVU1Data(), PS2_VU1_DATA_SIZE);
                                         const VU1State &st = m_vu1.state();
                                         std::fprintf(stderr, "[vu1-dump] mscal #%ld start=0x%x top=0x%x itop=0x%x end_pc=0x%x ebit=%d kicks=%llu cycles=%llu\n",
                                                      mscalIndex, startPC, top, itop, st.pc, st.ebit ? 1 : 0,
                                                      static_cast<unsigned long long>(st.xgkickPackets - kicksBefore),
                                                      static_cast<unsigned long long>(st.cycles - cyclesBefore));
                                     }
                                     traceVu1Run("MSCAL", startPC, top, itop, cyclesBefore, kicksBefore);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this, traceVu1Run, recordVu1Run, warnVu1CycleLimit](uint32_t top, uint32_t itop)
                                 {
                                     static thread_local R5900Context workerContext;
                                     workerContext.vu0_fbrst = 0u;
                                     workerContext.vu0_vpu_stat = 0u;
                                     R5900Context *cpuContext = m_memory.onVu1Worker()
                                                                    ? &workerContext
                                                                    : (m_eeScheduler ? m_eeScheduler->currentContext() : nullptr);
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     if (skipVu1)
                                     {
                                         cpuContext->vu0_vpu_stat &= ~0x0600u;
                                         return;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     const uint64_t cyclesBefore = m_vu1.state().cycles;
                                     const uint64_t kicksBefore = m_vu1.state().xgkickPackets;
                                     const uint32_t resumePc = m_vu1.state().pc;
                                     recordVu1Run(1u, resumePc, top, itop, VU1Interpreter::kProgramCycleLimit, [&]()
                                                  { m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                                 m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                                 m_gs, &m_memory, top, itop,
                                                                 VU1Interpreter::kProgramCycleLimit); });
                                     warnVu1CycleLimit("MSCNT", resumePc, cyclesBefore);
                                     traceVu1Run("MSCNT", resumePc, top, itop, cyclesBefore, kicksBefore);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

namespace
{
    // "16:9", "21:9", "1.6", "window" (0); 0 for anything else.
    float parseAspect(const char *text, bool &ok)
    {
        ok = true;
        const std::string value(text);
        if (value == "window")
            return 0.0f;
        const size_t colon = value.find(':');
        const float a = std::strtof(value.c_str(), nullptr);
        const float b = colon == std::string::npos ? 1.0f : std::strtof(value.c_str() + colon + 1, nullptr);
        if (a > 0.0f && b > 0.0f)
            return a / b;
        ok = false;
        return 0.0f;
    }
}

float PS2Runtime::wideAspect() const
{
    const float setting = wideAspectSetting();
    const float aspect = setting > 0.0f ? setting : m_windowAspect.load(std::memory_order_relaxed);
    // The window's shape reaches the game's logic (GameSettingsProfile::viewChangesRandom): a play recording keeps it.
    return play_recording::read(play_recording::Channel::ViewAspect, std::clamp(aspect, 4.0f / 3.0f, 32.0f / 9.0f));
}

bool PS2Runtime::initialize(const char *title)
{
    if (const char *wide = std::getenv("PS2X_WIDESCREEN"); wide && wide[0])
        setWidescreen(wide[0] == '1');
    if (const char *aspect = std::getenv("PS2X_ASPECT"); aspect && aspect[0])
    {
        bool ok = false;
        const float value = parseAspect(aspect, ok);
        if (ok)
            setWideAspect(value);
    }
    if (const char *blur = std::getenv("PS2X_MOTION_BLUR"); blur && blur[0])
        setMotionBlur(std::clamp(std::atoi(blur), 0, 2));
    // The draw distance from the boot on (the app's BootConfig, or the environment).
    if (const char *distance = ps2x::boot::text("PS2X_DRAW_DISTANCE"); distance && distance[0])
        setDrawDistance(std::clamp(std::atoi(distance), 0, 2));
    // The guest heap's peaks per level, logged (PS2X_TRACE_HEAP; off: nothing installed).
    ps2x::heap_trace::install(*this);
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        ps2_host::WindowOptions window;
        window.width = m_hostOptions.windowWidth > 0 ? m_hostOptions.windowWidth : HOST_WINDOW_WIDTH;
        window.height = m_hostOptions.windowHeight > 0 ? m_hostOptions.windowHeight : HOST_WINDOW_HEIGHT;
        window.highDpi = m_hostOptions.highDpi;
        // PS2X_HEADLESS=1 or 0 says so; otherwise a null-GS run is headless.
        const char *headless = std::getenv("PS2X_HEADLESS");
        window.headless = headless && headless[0] ? std::strcmp(headless, "0") != 0 : gsNullRequested();
        if (!ps2_host::initialize(title, window))
        {
            std::cerr << "Failed to create the host window" << std::endl;
            return false;
        }
        // A front end's own (runtime/ps2_front_end.h: Android's background pause).
        if (const ps2x::front_end::WindowMade windowMade = ps2x::front_end::windowMade())
            windowMade(*this);
        // After the window: the GPU GS renderer draws on its GPU device.
        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
        m_audioBackend.setAudioReady(ps2_host::audioReady());
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    std::ifstream input(elfPath, std::ios::binary);
    if (!input)
    {
        configureIoPathsFromElf(elfPath);
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
    {
        configureIoPathsFromElf(elfPath);
        std::cerr << "Failed to read ELF file: " << elfPath << std::endl;
        return false;
    }
    return loadELFImage(elfPath, bytes);
}

bool PS2Runtime::loadELFImage(const std::string &elfPath, const std::vector<uint8_t> &bytes, bool deriveIoRoots)
{
    configureIoPathsFromElf(elfPath, deriveIoRoots);
    play_recording::noteElfImage(bytes.data(), bytes.size());

    std::istringstream file(std::string(bytes.begin(), bytes.end()), std::ios::binary);

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    // Several games' code in this executable: the one recompiled from this ELF.
    if (!ps2_generated::all().empty())
    {
        const std::string codeId = ps2_generated::select(elfPath);
        std::cout << "[function-table] " << (codeId.empty() ? std::string("global") : "\"" + codeId + "\"")
                  << " generated code for " << std::filesystem::path(elfPath).filename().string() << std::endl;
    }

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_LEGACY_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_LEGACY_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_LEGACY_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_LEGACY_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_LEGACY_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_LEGACY_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    const uint32_t elfCrc32 = computeBufferCrc32(bytes);
    const bool elfCrc32Valid = true;
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    giveIopIdleCycles();
    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty() && !normalized.cdFromImage)
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.cdFromImage)
    {
        normalized.cdRoot.clear();
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath, bool deriveRoots)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (deriveRoots && !paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.cdFromImage ? std::filesystem::path() : paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

std::shared_ptr<const ps2x::iop::Iso9660Image> PS2Runtime::cdImageFileSystem()
{
    const IoPaths &paths = runtimeIoPaths();
    if (!paths.cdFromImage || paths.cdImage.empty())
    {
        return nullptr;
    }
    return ps2x::iop::Iso9660Image::shared(paths.cdImage);
}

namespace
{
    // The function table in use: the global one, or a registered namespaced
    // set chosen at ELF load (ps2_generated::select).
    struct ActiveFunctionTable
    {
        uint32_t base = 0;
        uint32_t end = 0;
        uint32_t slotCount = 0;
        PS2Runtime::RecompiledFunction *table = nullptr;
        bool chosen = false;
    };
    ActiveFunctionTable g_activeTable;

    // Each table's slots as generated, for the slots replaceFunction has
    // changed since (the first entry each held), by table: select() puts them
    // back, so a game chosen again starts from its table as generated
    // whatever an earlier run installed (readable functions, override hooks,
    // resume and return slots). Any thread.
    std::mutex g_generatedSlotsMutex;
    std::unordered_map<PS2Runtime::RecompiledFunction *, std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction>>
        g_generatedSlots;

    void noteGeneratedSlot(PS2Runtime::RecompiledFunction *table, uint32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_generatedSlotsMutex);
        g_generatedSlots[table].try_emplace(slot, table[slot]);
    }

    // Puts every changed slot of `table` back; how many there were.
    size_t restoreGeneratedSlots(PS2Runtime::RecompiledFunction *table)
    {
        std::lock_guard<std::mutex> lock(g_generatedSlotsMutex);
        const auto it = g_generatedSlots.find(table);
        if (it == g_generatedSlots.end())
            return 0;
        for (const auto &[slot, generated] : it->second)
            table[slot] = generated;
        const size_t count = it->second.size();
        g_generatedSlots.erase(it);
        return count;
    }

    // The table as generated, kept for PS2Runtime::replacedFunctions when
    // PS2X_TAILCALL_STATS is set (copied when the table is chosen, before
    // any override).
    std::vector<PS2Runtime::RecompiledFunction> g_generatedTable;
    uint32_t g_generatedTableBase = 0;

    // PS2Runtime::tailCall reads the active table through its own copy.
    void syncTailCallTable()
    {
        PS2Runtime::s_tailCallTable.base = g_activeTable.base;
        PS2Runtime::s_tailCallTable.slotCount = g_activeTable.slotCount;
        PS2Runtime::s_tailCallTable.table = g_activeTable.table;
        if (const char *stats = std::getenv("PS2X_TAILCALL_STATS"); stats && *stats && g_activeTable.table)
        {
            g_generatedTable.assign(g_activeTable.table, g_activeTable.table + g_activeTable.slotCount);
            g_generatedTableBase = g_activeTable.base;
        }
    }

    // The default code: a game module registered as the default (AI's, ps2_generated::setDefault), else the
    // global table (the one linked into this program: empty when the game code is in modules).
    ps2_generated::Code g_defaultCode;
    bool g_hasDefaultCode = false;

    ActiveFunctionTable defaultTable()
    {
        if (g_hasDefaultCode)
            return {g_defaultCode.base, g_defaultCode.end, g_defaultCode.slotCount, g_defaultCode.table, true};
        return {g_ps2RecompiledFunctionTableBase, g_ps2RecompiledFunctionTableEnd,
                g_ps2RecompiledFunctionTableSlotCount, g_ps2RecompiledFunctionTable, true};
    }

    ActiveFunctionTable &activeTable()
    {
        if (!g_activeTable.chosen)
        {
            ps2x::game_module::ensureLoaded();
            g_activeTable = defaultTable();
            syncTailCallTable();
        }
        return g_activeTable;
    }

    std::mutex g_tailCallStatsMutex;
    std::unordered_map<uint64_t, uint64_t> g_tailCallStats;

    std::vector<ps2_generated::Code> &generatedCodeSets()
    {
        static std::vector<ps2_generated::Code> sets;
        return sets;
    }

    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        const ActiveFunctionTable &t = activeTable();
        if ((address & 3u) != 0u || t.slotCount == 0u)
        {
            return false;
        }

        if (address < t.base || address >= t.end)
        {
            return false;
        }

        const uint32_t offset = address - t.base;
        slot = offset >> 2;
        return slot < t.slotCount;
    }
}

namespace ps2_generated
{
    void add(const Code &code)
    {
        generatedCodeSets().push_back(code);
    }

    void setDefault(const Code &code)
    {
        g_defaultCode = code;
        g_hasDefaultCode = true;
        // (a module loaded after a table was first chosen, the app's code made at boot: the next use chooses again)
        if (g_activeTable.chosen && g_activeTable.table == g_ps2RecompiledFunctionTable)
            g_activeTable.chosen = false;
    }

    const std::vector<Code> &all()
    {
        ps2x::game_module::ensureLoaded();
        return generatedCodeSets();
    }

    namespace
    {
        // The table for the ELF being loaded, as generated (an earlier run of
        // this game, in this process, changed it).
        void choose(const ActiveFunctionTable &table)
        {
            if (const size_t restored = restoreGeneratedSlots(table.table))
                std::cout << "[function-table] " << restored << " slots put back as generated" << std::endl;
            g_activeTable = table;
            syncTailCallTable();
        }
    }

    std::string select(const std::string &elfName)
    {
        auto lower = [](std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        };
        ps2x::game_module::ensureLoaded();
        const std::string want = lower(std::filesystem::path(elfName).filename().string());
        for (const Code &code : generatedCodeSets())
        {
            if (lower(code.elfName) == want)
            {
                choose({code.base, code.end, code.slotCount, code.table, true});
                return code.id;
            }
        }
        choose(defaultTable());
        return {};
    }
}

void PS2Runtime::requestScreenshot(std::string path, std::function<void(bool ok)> done)
{
    std::lock_guard<std::mutex> lock(m_screenshotMutex);
    m_screenshotRequests.emplace_back(std::move(path), std::move(done));
}

void PS2Runtime::setPictureFilter(PictureFilter filter)
{
    std::lock_guard<std::mutex> lock(m_screenshotMutex);
    m_pictureFilter = filter ? std::make_shared<const PictureFilter>(std::move(filter)) : nullptr;
}

void PS2Runtime::noteReplacedTailCall(uint32_t sourcePc, uint32_t targetPc)
{
    std::lock_guard<std::mutex> lock(g_tailCallStatsMutex);
    ++g_tailCallStats[(static_cast<uint64_t>(sourcePc) << 32) | targetPc];
}

std::vector<PS2Runtime::TailCallCount> PS2Runtime::replacedTailCalls()
{
    std::vector<TailCallCount> counts;
    {
        std::lock_guard<std::mutex> lock(g_tailCallStatsMutex);
        counts.reserve(g_tailCallStats.size());
        for (const auto &[key, calls] : g_tailCallStats)
            counts.push_back({static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key), calls});
    }
    std::sort(counts.begin(), counts.end(), [](const TailCallCount &a, const TailCallCount &b)
              { return a.targetPc != b.targetPc ? a.targetPc < b.targetPc : a.sourcePc < b.sourcePc; });
    return counts;
}

std::vector<uint32_t> PS2Runtime::replacedFunctions()
{
    std::vector<uint32_t> replaced;
    const ActiveFunctionTable &t = activeTable();
    if (t.base != g_generatedTableBase || g_generatedTable.size() != t.slotCount)
        return replaced;
    for (uint32_t slot = 0; slot < t.slotCount; ++slot)
        if (t.table[slot] != g_generatedTable[slot])
            replaced.push_back(t.base + slot * 4u);
    return replaced;
}

namespace ps2x::function_table
{
    View active()
    {
        const ActiveFunctionTable &t = activeTable();
        return View{t.base, t.end, t.slotCount, t.table};
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << activeTable().base
                  << ", 0x" << activeTable().end << ")"
                  << std::dec << std::endl;
        return false;
    }

    ActiveFunctionTable &table = activeTable();
    noteGeneratedSlot(table.table, slot);
    table.table[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::callGuestFunction(uint8_t *rdram, R5900Context *ctx, uint32_t address,
                                   std::initializer_list<uint32_t> args, uint32_t sp, uint32_t *v0)
{
    return callGuestFunction(rdram, ctx, hasFunction(address) ? lookupFunction(address) : nullptr, address, args, sp,
                             v0);
}

bool PS2Runtime::callGuestFunction(uint8_t *rdram, R5900Context *ctx, RecompiledFunction fn, uint32_t address,
                                   std::initializer_list<uint32_t> args, uint32_t sp, uint32_t *v0)
{
    if (!fn || !ctx || args.size() > 8u)
        return false;
    // Returns to a sentinel address that is never code.
    constexpr uint32_t kReturnSentinel = 0xFFFFFFF0u;
    const R5900Context saved = *ctx;
    static constexpr int kArgRegs[8] = {4, 5, 6, 7, 8, 9, 10, 11};
    int i = 0;
    for (uint32_t arg : args)
    {
        const int reg = kArgRegs[i++]; // SET_GPR_U32 is a macro: no side effects in its arguments
        SET_GPR_U32(ctx, reg, arg);
    }
    SET_GPR_U32(ctx, 29, sp & ~0xFu);
    SET_GPR_U32(ctx, 31, kReturnSentinel);
    ctx->pc = address;
    ctx->in_delay_slot = false;
    {
        // The depth comes back down however the call ends: a wait in it
        // (sceMcSync, a semaphore) throws to the scheduler through here
        // (ps2xFnCheck's atomic recorder, drafter23); the context is then
        // the waiting thread's, kept for its resume, not put back.
        struct Depth
        {
            uint32_t &depth;
            explicit Depth(uint32_t &d) : depth(d) { ++depth; }
            ~Depth() { --depth; }
        } depth(m_hostGuestCallDepth);
        fn(rdram, ctx, this);
        // A tail jump (j to another function), or a branch out of a block the
        // recompiler split off (a jump table's case), leaves generated code
        // with pc at its target, where the scheduler's loop would go on: follow
        // it to the sentinel the same way (a loop over such blocks takes a hop
        // per pass; the bound only stops a runaway).
        for (uint32_t hops = 0; ctx->pc != kReturnSentinel && hops < (1u << 24) && hasFunction(ctx->pc); ++hops)
            lookupFunction(ctx->pc)(rdram, ctx, this);
    }
    const uint32_t stoppedAt = ctx->pc;
    const bool returned = stoppedAt == kReturnSentinel;
    if (v0)
        *v0 = getRegU32(ctx, 2);
    *ctx = saved;
    if (!returned)
        std::cerr << "[runtime] guest call 0x" << std::hex << address << " did not return (pc 0x" << stoppedAt
                  << ")" << std::dec << std::endl;
    return returned;
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && activeTable().table[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::tableEntry(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) ? activeTable().table[slot] : nullptr;
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = activeTable().table[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << activeTable().base
              << " tableEnd=0x" << activeTable().end
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)); // ra: diagnostics only
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

namespace
{
    // PS2X_TRACE_CALLS=0xADDR[,0xADDR...] logs every dispatched call to those
    // guest addresses with a0-a3/ra, and the v0 it returns with. An entry of
    // the form @0xADDR matches the call site instead (useful for jalr).
    // Calls that block (thread switch) unwind the host stack, so their return
    // line can be missing.
    struct CallTraceConfig
    {
        bool enabled = false;
        std::unordered_set<uint32_t> targets;
        std::unordered_set<uint32_t> sources;
    };

    const CallTraceConfig &callTraceConfig()
    {
        static const CallTraceConfig config = []()
        {
            CallTraceConfig parsed;
            const char *value = std::getenv("PS2X_TRACE_CALLS");
            if (!value || value[0] == '\0')
            {
                return parsed;
            }
            std::string list(value);
            size_t start = 0;
            while (start <= list.size())
            {
                const size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty() && item[0] == '@')
                {
                    parsed.sources.insert(static_cast<uint32_t>(std::strtoul(item.c_str() + 1, nullptr, 0)));
                }
                else if (!item.empty())
                {
                    parsed.targets.insert(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0)));
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            parsed.enabled = !parsed.targets.empty() || !parsed.sources.empty();
            return parsed;
        }();
        return config;
    }
}

// PS2X_WATCH32=<addr>: log when the 32-bit word at addr changes, with the
// guest branch that first observed it (the write happened in the code run
// since the previous check). Checked at every inter-function branch and, with
// `where` set, at EE scheduler boundaries (a thread or invocation resumed),
// so writes made by scheduler-run guest code are told apart.
void PS2Runtime::checkWatch32(uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc, const char *where)
{
    static const uint32_t watchAddr = []()
    {
        const char *value = std::getenv("PS2X_WATCH32");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) & 0x1FFFFFFCu : 0u;
    }();
    if (watchAddr == 0u || !rdram)
        return;
    static uint32_t lastValue = 0u;
    static uint32_t lastSource = 0u;
    static std::string lastWhere;
    uint32_t value = 0u;
    std::memcpy(&value, rdram + watchAddr, sizeof(value));
    if (value != lastValue)
    {
        std::fprintf(stderr, "[watch] 0x%08x: 0x%08x -> 0x%08x between %s0x%x and %s0x%x->0x%x ra=0x%x sp=0x%x\n",
                     watchAddr, lastValue, value, lastWhere.c_str(), lastSource, where ? where : "",
                     sourcePc, targetPc, ctx ? GPR_U32(ctx, 31) : 0u, ctx ? GPR_U32(ctx, 29) : 0u);
        lastValue = value;
    }
    lastSource = sourcePc;
    lastWhere = where ? where : "";
}

bool PS2Runtime::dispatchCharge(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc)
{
    checkWatch32(rdram, ctx, sourcePc, targetPc, nullptr);

    ctx->pc = targetPc;

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_hostGuestCallDepth != 0u)
    {
        if (m_eeScheduler)
            m_eeScheduler->accountCycles(EeScheduler::kGuestDispatchCycles);
    }
    else if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ++m_guestUnwinds;
        return false;
    }
    return true;
}

bool PS2Runtime::guestCallsTraced() const
{
    return callTraceConfig().enabled;
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    if (!dispatchCharge(rdram, ctx, targetPc, sourcePc))
        return false;

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        ++m_guestUnwinds;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            ++m_guestUnwinds;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        ++m_guestUnwinds;
        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t unwindsBefore = m_guestUnwinds;
    const CallTraceConfig &trace = callTraceConfig();
    const bool traced = trace.enabled && (trace.targets.count(targetPc) != 0u || trace.sources.count(sourcePc) != 0u);
    if (traced)
    {
        // Arguments that point at printable C strings are shown too.
        auto guestString = [rdram](uint32_t address) -> std::string
        {
            std::string text;
            if (address < 0x1000u)
                return text;
            for (uint32_t i = 0; i < 48u; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, address + i);
                if (!c || *c == 0u)
                    break;
                if (*c < 0x20u || *c >= 0x7Fu)
                    return std::string();
                text.push_back(static_cast<char>(*c));
            }
            return text.size() >= 3u ? text : std::string();
        };
        std::string strings;
        for (int reg = 4; reg <= 7; ++reg)
        {
            const std::string text = guestString(getRegU32(ctx, reg));
            if (!text.empty())
                strings += " a" + std::to_string(reg - 4) + "=\"" + text + "\"";
        }
        std::fprintf(stderr, "[call-trace] -> 0x%x from 0x%x a0=0x%x a1=0x%x a2=0x%x a3=0x%x sp=0x%x%s\n",
                     targetPc, sourcePc, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                     getRegU32(ctx, 29), strings.c_str());
    }
    if (ps2x::lanes::g_pokesOn)
        ps2x::lanes::poke(ctx, rdram, sourcePc, targetPc, ps2x::lanes::Point::BeforeCall);
    if (ps2x::lanes::g_callersOn)
        ps2x::lanes::caller(targetPc, sourcePc);
    targetFn(rdram, ctx, this);
    // (ret= and exit= pokes at a real return only: a callee the scheduler unwound comes back here with its own pc and
    // sp, and returns to its caller through the scheduler's resume, not this call)
    if (ps2x::lanes::g_pokesOn && (ctx->pc == fallthroughPc || (ctx->pc == entryPc && m_guestUnwinds == unwindsBefore)))
        ps2x::lanes::poke(ctx, rdram, sourcePc, targetPc, ps2x::lanes::Point::AfterCall);
    if (traced)
    {
        std::fprintf(stderr, "[call-trace] <- 0x%x v0=0x%x pc=0x%x sp=0x%x\n", targetPc, getRegU32(ctx, 2), ctx->pc, getRegU32(ctx, 29));
    }

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // A callee that leaves ctx->pc at its own entry returned without setting
    // it (HLE stubs do this). But if guest code unwound to the scheduler
    // meanwhile, pc == entryPc means something else: a nested call to the
    // same function (recursion) yielded before entering it, or the callee
    // jumped back to its entry. Treating that as a return resumed the caller
    // with the nested frames' sp and lost them.
    if (ctx->pc == entryPc && m_guestUnwinds == unwindsBefore)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
    drainPendingIntcInterrupts();
}

void PS2Runtime::drainPendingIntcInterrupts()
{
    if (!m_eeScheduler)
    {
        return;
    }
    for (uint32_t cause : m_memory.consumePendingIntcCauses())
    {
        m_eeScheduler->dispatchIrq(false, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min({kGuestHeapHardLimit, PS2_LEGACY_RAM_SIZE, m_mainStackFloor});
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeap.clear();
    if (limit > base)
    {
        m_guestHeap.appendFree(base, limit - base);
    }
    for (const auto &region : m_guestHeapExtraRegions)
    {
        m_guestHeap.appendFree(region.first, region.second - region.first);
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment, bool extended)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    const uint32_t address = m_guestHeap.allocate(allocSize, normalizedAlignment, extended);
    if (address != 0u && !extended)
    {
        m_guestHeapEnd = std::max(m_guestHeapEnd, address + allocSize);
    }
    return address;
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    if (const uint32_t address = allocateGuestBlockLocked(size, alignment))
    {
        return address;
    }
    // The 32 MB heap is full: extended memory, if a game override added
    // some (a PS2 would have failed here).
    const uint32_t address = allocateGuestBlockLocked(size, alignment, true);
    if (address && !m_guestHeapOverflowed)
    {
        m_guestHeapOverflowed = true;
        std::cerr << "[heap] the 32 MB heap is full: " << size << " bytes at 0x" << std::hex << address << std::dec
                  << " (extended memory)" << std::endl;
    }
    return address;
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const uint32_t oldAddr = guestAddr & PS2_RAM_MASK;
    const uint32_t oldSize = m_guestHeap.usedSize(oldAddr);
    if (oldSize == 0u)
    {
        return 0u;
    }

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            m_guestHeap.shrink(oldAddr, requestedSize);
        }
        return oldAddr;
    }

    if (m_guestHeap.growInPlace(oldAddr, requestedSize))
    {
        m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
        return oldAddr;
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    m_guestHeap.release(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    m_guestHeap.release(guestAddr & PS2_RAM_MASK);
}

bool PS2Runtime::addGuestHeapRegion(uint32_t base, uint32_t end)
{
    base = alignGuestHeapValue(base, kGuestHeapDefaultAlignment);
    end &= ~(kGuestHeapDefaultAlignment - 1u);
    if (base < PS2_LEGACY_RAM_SIZE || end > PS2_RAM_SIZE || end <= base)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    // (below the range reserveExtendedTop keeps)
    end = std::min(end, PS2_RAM_SIZE - m_extendedReserved);
    if (end <= base)
    {
        return false;
    }
    for (const auto &region : m_guestHeapExtraRegions)
    {
        if (base < region.second && region.first < end)
        {
            return false;
        }
    }
    m_guestHeapExtraRegions.emplace_back(base, end);
    std::sort(m_guestHeapExtraRegions.begin(), m_guestHeapExtraRegions.end());
    if (m_guestHeapConfigured)
    {
        // Among the blocks, by its address (all of the 32 MB heap is lower).
        m_guestHeap.insertFree(base, end - base);
    }
    return true;
}

uint32_t PS2Runtime::reserveExtendedTop(uint32_t bytes)
{
    constexpr uint32_t kExtendedBytes = PS2_RAM_SIZE - PS2_LEGACY_RAM_SIZE;
    if (bytes == 0u || bytes > kExtendedBytes)
    {
        return 0u;
    }
    bytes = alignGuestHeapValue(bytes, kGuestHeapDefaultAlignment);
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    if (bytes <= m_extendedReserved)
    {
        return PS2_RAM_SIZE - m_extendedReserved;
    }
    const uint32_t base = PS2_RAM_SIZE - bytes;
    if (m_guestHeap.usedAbove(base))
    {
        return 0u;
    }
    // The ranges given to the heap, and their free blocks, end below it.
    for (auto &region : m_guestHeapExtraRegions)
    {
        region.second = std::max(region.first, std::min(region.second, base));
    }
    m_guestHeapExtraRegions.erase(std::remove_if(m_guestHeapExtraRegions.begin(), m_guestHeapExtraRegions.end(),
                                                 [](const auto &region) { return region.second <= region.first; }),
                                  m_guestHeapExtraRegions.end());
    m_guestHeap.cutFreeFrom(base);
    m_extendedReserved = bytes;
    return base;
}

void PS2Runtime::guardMainStack(uint32_t top, uint32_t guard)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    if (top <= guard || top > PS2_LEGACY_RAM_SIZE)
    {
        return;
    }
    m_mainStackFloor = (top - guard) & ~(kGuestHeapDefaultAlignment - 1u);
    if (!m_guestHeapConfigured || m_guestHeapLimit <= m_mainStackFloor)
    {
        return;
    }
    // Cut the heap's free space above the floor (nothing is allocated
    // there unless the stack was already overwritten).
    m_guestHeapLimit = std::max(m_guestHeapBase, m_mainStackFloor);
    m_guestHeap.cutFreeAbove(m_mainStackFloor);
}

uint32_t PS2Runtime::guestMallocExtended(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment, true);
}

uint32_t PS2Runtime::extendedHeapInUse() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeap.extendedInUse();
}

GuestHeap::Usage PS2Runtime::guestHeapUsage() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeap.usage();
}

void PS2Runtime::resetGuestHeapPeaks()
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    m_guestHeap.resetPeaks();
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_LEGACY_RAM_SIZE)
    {
        top = PS2_LEGACY_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

void PS2Runtime::setHostPaused(bool paused)
{
    if (m_eeScheduler)
    {
        m_eeScheduler->setHostPaused(paused);
    }
}

bool PS2Runtime::isHostPaused() const
{
    return m_eeScheduler && m_eeScheduler->isHostPaused();
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

namespace
{
    // A thread's CPU time in milliseconds, by the handle the perf log keeps
    // (a Mach port on macOS, ps2x::currentThreadCpuHandle elsewhere).
    double threadCpuMs(uint64_t port)
    {
#if defined(_WIN32) || defined(__linux__)
        return ps2x::threadCpuMilliseconds(port);
#elif defined(__APPLE__)
        if (port == 0u)
            return 0.0;
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(static_cast<mach_port_t>(port), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
            return 0.0;
        return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
               (info.user_time.microseconds + info.system_time.microseconds) * 1e-3;
#else
        (void)port;
        return 0.0;
#endif
    }

    // The calling thread's handle for threadCpuMs.
    uint64_t hostThreadCpuPort()
    {
#if defined(__APPLE__)
        return pthread_mach_thread_np(pthread_self());
#else
        return ps2x::currentThreadCpuHandle();
#endif
    }

    // A play recording's telemetry (runtime/play_recording.h): what each
    // host frame took, sampled after its present, on the guest's vsync clock.
    struct PlayTelemetry
    {
        bool started = false;
        std::chrono::steady_clock::time_point lastPresent;
        double cpu[4] = {}; // game, VIF1, GS, host
        uint64_t flips = 0;
        uint64_t droppedNs = 0;
        uint64_t hostPort = 0;

        void sample(PS2Runtime &, PS2Memory &memory, uint64_t displayFlips, uint64_t gamePort, uint64_t dropped)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!started)
                hostPort = hostThreadCpuPort();
            const double cpuNow[4] = {threadCpuMs(gamePort), memory.vif1WorkerCpuMs(), memory.gsWorkerCpuMs(),
                                      threadCpuMs(hostPort)};
            if (started)
            {
                play_recording::HostFrame frame;
                frame.vsync = memory.gs().vsyncTick.load(std::memory_order_acquire);
                frame.frameMs = std::chrono::duration<double, std::milli>(now - lastPresent).count();
                frame.gameCpuMs = cpuNow[0] - cpu[0];
                frame.vif1CpuMs = cpuNow[1] - cpu[1];
                frame.gsCpuMs = cpuNow[2] - cpu[2];
                frame.hostCpuMs = cpuNow[3] - cpu[3];
                frame.flips = static_cast<uint32_t>(displayFlips - flips);
                frame.pacerDroppedMs = static_cast<double>(dropped - droppedNs) * 1e-6;
                play_recording::onHostFrame(frame);
            }
            started = true;
            lastPresent = now;
            std::copy(cpuNow, cpuNow + 4, cpu);
            flips = displayFlips;
            droppedNs = dropped;
        }
    };
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    {
        // While the game runs, GS local memory is only read through m_gs
        // (the VRAM dump syncs first), so draws may be queued and drawn in
        // parallel. PS2X_GS_DEFER=0 draws every primitive immediately.
        const char *defer = std::getenv("PS2X_GS_DEFER");
        m_gs.setDeferredRasterization(!(defer && defer[0] == '0'));
    }
    ps2_stubs::resetSifState();
    ps2_syscalls::resetKernelState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_LEGACY_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A play recording of this boot, or its replay (runtime/play_recording.h),
    // from power-on.
    {
        play_recording::BootInfo boot;
        boot.game = ps2x::boot::game();
        boot.fromApp = !boot.game.empty(); // the app's BootConfig names the game (ps2x::app::gameBootConfig)
        const IoPaths &paths = getIoPaths();
        boot.elfPath = paths.elfPath.string();
        if (boot.game.empty())
        {
            // A runner given an ELF: the game by its ELF's name.
            std::string elf = paths.elfPath.filename().string();
            std::transform(elf.begin(), elf.end(), elf.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            const ps2x::games::Game &game = ps2x::games::game(elf);
            if (elf == game.elf)
                boot.game = game.id;
        }
        boot.discRoot = paths.cdFromImage ? paths.cdImage : paths.cdRoot;
        boot.memcardSlot1 = paths.mcRoot.empty() ? std::filesystem::path() : paths.mcRoot.lexically_normal();
        if (!boot.memcardSlot1.empty())
        {
            // Slot 2's folder, named as the card stubs name it (MemoryCard.cpp getMcRootPath).
            const std::filesystem::path parent = boot.memcardSlot1.parent_path();
            std::string leaf = boot.memcardSlot1.filename().string();
            std::string lower = leaf;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            boot.memcardSlot2 = lower == "mc0"     ? parent / "mc1"
                                : lower == "slot1" ? parent / (leaf.substr(0, 4) + "2")
                                                   : parent / (leaf + "_slot1");
        }
        boot.vsyncTick = &m_memory.gs().vsyncTick;
        boot.rdram = m_memory.getRDRAM();
        play_recording::beginBoot(boot);
    }

    std::atomic<bool> gameThreadFinished{false};

    // PS2X_VU1_ASYNC=0: VIF1/VU1 and GS packet processing on the game thread
    // (default: on their own worker threads).
    {
        const char *value = std::getenv("PS2X_VU1_ASYNC");
        const bool async = !(value && value[0] == '0');
        m_memory.setAsyncGraphics(async);
        if (!async)
            std::fprintf(stderr, "[vu1] PS2X_VU1_ASYNC=0: VIF1/VU1 run on the game thread\n");
    }
    // PS2X_VIF1_PREFETCH=1 (debug): VIF1 DMA read whole at the kick instead
    // of as VIF1 consumes it (the part behind an interrupt stall then misses
    // what the handler wrote).
    {
        const char *value = std::getenv("PS2X_VIF1_PREFETCH");
        PS2Memory::s_vif1PrefetchChain = value && value[0] == '1';
        if (PS2Memory::s_vif1PrefetchChain)
            std::fprintf(stderr, "[vif1] PS2X_VIF1_PREFETCH=1: VIF1 DMA read whole at the kick\n");
    }

    std::atomic<uint64_t> gameThreadPort{0}; // Mach port (macOS), thread handle (Windows, Linux)
    // This loop runs the settings' changes once per host frame (runPending
    // below): the game's Settings page waits on it (ps2_settings.h).
    ps2x::settings::setFrameLoop(true);
    GameHostThread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        ps2x::placement::placeCurrentThread(ps2x::placement::Role::Game);
        play_recording::setGameThread();
#if defined(__APPLE__)
        gameThreadPort.store(pthread_mach_thread_np(pthread_self()), std::memory_order_release);
        // The EE thread sets the frame rate with the graphics workers: keep
        // it on performance cores when the machine is busy.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
        gameThreadPort.store(ps2x::currentThreadCpuHandle(), std::memory_order_release);
#endif
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
            play_recording::noteError(e.what());
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
            play_recording::noteError("unknown exception");
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    // The host loop, from here to the run's end: a lambda so a child forked at
    // a spot (PS2X_SPOT_FORK, runtime/ps2_spot_fork.h) runs it again on a
    // thread of its own; this thread runs it once, as before.
    auto hostLoop = [&]()
    {
    uint64_t tick = 0;
    // PS2X_DUMP_AT_FRAME=<n>[,<n>...] writes EE RAM and GS VRAM snapshots to
    // <PS2X_DUMP_PREFIX or ps2x-dump>-<n>.ram/.vram when the host loop reaches
    // frame n. Taken from the render thread while the guest runs, so they are
    // only approximately consistent; good enough for post-mortems. These
    // are read at every run: each boot its own.
    const std::vector<uint64_t> dumpFrames = []()
    {
        std::vector<uint64_t> frames;
        if (const char *value = ps2x::boot::text("PS2X_DUMP_AT_FRAME"))
        {
            const char *cursor = value;
            while (*cursor)
            {
                char *end = nullptr;
                const unsigned long long frame = std::strtoull(cursor, &end, 0);
                if (end == cursor)
                    break;
                frames.push_back(frame);
                cursor = (*end == ',') ? end + 1 : end;
            }
        }
        return frames;
    }();
    // PS2X_GS_DUMP=<file> PS2X_GS_DUMP_FRAME=<n> [PS2X_GS_DUMP_COUNT=<frames>]
    // records the GS input from host frame n for that many presented frames
    // (default 2); replay with ps2xGsReplay.
    const char *gsDumpPath = std::getenv("PS2X_GS_DUMP");
    const uint64_t gsDumpFrame = []()
    {
        const char *value = std::getenv("PS2X_GS_DUMP_FRAME");
        return value ? std::strtoull(value, nullptr, 0) : 0ull;
    }();
    const uint32_t gsDumpCount = []()
    {
        const char *value = std::getenv("PS2X_GS_DUMP_COUNT");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 2u;
    }();
    // PS2X_SHOT_AT="<t>:<file.png>,..." saves the presented game picture
    // once the pad script clock (StateProbe::scriptTime, vsyncs; also run by
    // PS2X_HOST_INPUT_SCRIPT alone) reaches t (seconds, or vsyncs with a
    // "v" suffix), for before/after comparisons of scripted input.
    struct ScriptShots
    {
        std::vector<std::pair<double, std::string>> pending;
        ScriptShots()
        {
            const char *spec = ps2x::boot::text("PS2X_SHOT_AT");
            std::string list = spec ? spec : "";
            size_t pos = 0;
            while (pos < list.size())
            {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos)
                    comma = list.size();
                const std::string item = list.substr(pos, comma - pos);
                pos = comma + 1;
                const size_t colon = item.find(':');
                if (colon == std::string::npos || colon + 1 >= item.size())
                    continue;
                std::string when = item.substr(0, colon);
                double t = 0.0;
                if (!when.empty() && (when.back() == 'v' || when.back() == 'V'))
                    t = std::atof(when.substr(0, when.size() - 1).c_str());
                else
                    t = std::atof(when.c_str()) * 59.94;
                pending.emplace_back(t, item.substr(colon + 1));
            }
            std::sort(pending.begin(), pending.end());
        }
        bool due() const
        {
            const int64_t now = StateProbe::scriptTime();
            return !pending.empty() && now >= 0 && static_cast<double>(now) >= pending.front().first;
        }
        void capture(const std::function<void(const std::string &, std::function<void(bool)>)> &write)
        {
            const int64_t now = StateProbe::scriptTime();
            while (due())
            {
                const std::string path = pending.front().second;
                write(path, [now, path](bool ok)
                      { std::cerr << "[shot] script " << now << "v -> " << path << (ok ? "" : " (failed)") << std::endl; });
                pending.erase(pending.begin());
            }
        }
    } shots;

    uint64_t hostFrame = 0;
    PlayTelemetry playTelemetry; // a play recording's (this boot's)
    // Widescreen: the shown frame counts as rendered for 16:9 if the game
    // override marked one (markWidescreenFrame) within the last few display
    // flips. Without flips (a loading pause) the aspect stays as it was.
    uint64_t wideLastFlips = 0;
    uint64_t wideLastMarks = 0;
    uint64_t flipsSinceWideMark = std::numeric_limits<uint64_t>::max() / 2u;
    // Every game frame shown (GS's frame queue); ZAXIS_FRAME_QUEUE=0: the
    // newest picture at each refresh, as before. Headless runs too: their
    // pictures (PS2X_CONTROL shot, PS2X_SHOTS) come from the same pictures
    // as the window's and the app's screenshots, made on the GS worker at
    // the buffer swap. Latched here instead, a picture could be a copy of a
    // buffer the GS worker cleared and was drawing again (two swaps during
    // the copy leave the display registers as they were: A, B, A).
    {
        const char *queue = std::getenv("ZAXIS_FRAME_QUEUE");
        m_gs.setFrameQueue(!(queue && queue[0] == '0'));
    }
    ps2x::host_pacing::ShownFrames shownFrames;
    std::vector<double> shownLatencies; // ms from a game frame's picture to its present
    // PS2X_PRESENT_LOG=<file> (developers): a line for every new picture the
    // window shows (the game frame it is, 0 for one the window latched, a
    // hash of its pixels; the CPU renderer's) and for every shot taken from
    // it, to compare two runs' presentation and to find a shot of a frame
    // copied while the game swapped (disturbed).
    struct PresentLog
    {
        FILE *file = nullptr;
        uint64_t frame = 0, hash = 0;
        bool latched = false, disturbed = false;
        PresentLog()
        {
            if (const char *path = std::getenv("PS2X_PRESENT_LOG"); path && path[0])
                file = std::fopen(path, "w");
        }
        ~PresentLog()
        {
            if (file)
                std::fclose(file);
        }
        static uint64_t hashOf(const uint8_t *rgba, uint32_t width, uint32_t height, size_t stride)
        {
            uint64_t h = 0xCBF29CE484222325ull; // FNV-1a
            for (uint32_t y = 0; y < height; ++y)
                for (const uint8_t *p = rgba + y * stride, *end = p + static_cast<size_t>(width) * 4u; p != end; ++p)
                    h = (h ^ *p) * 0x100000001B3ull;
            return h;
        }
    } presentLog;
    ps2x::FrameInterpolation interpolation; // (runtime/ps2_frame_interpolation.h)
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        ps2x::spot::hostLoopTop();
        ++hostFrame;
        // Window, keyboard, mouse and gamepad events of this frame.
        ps2_host::pumpEvents();
        if (gsDumpPath && gsDumpPath[0] && hostFrame == gsDumpFrame)
            m_gs.startRecording(gsDumpPath, gsDumpCount);
        {
            uint32_t frames = 2;
            if (const std::string path = ps2_debug_control::takePendingGsDump(frames); !path.empty())
                m_gs.startRecording(path, frames);
        }
        if (!dumpFrames.empty() && std::find(dumpFrames.begin(), dumpFrames.end(), hostFrame) != dumpFrames.end())
        {
            const char *prefixEnv = ps2x::boot::text("PS2X_DUMP_PREFIX");
            const std::string prefix = (prefixEnv && prefixEnv[0]) ? prefixEnv : "ps2x-dump";
            const std::string base = prefix + "-" + std::to_string(hostFrame);
            if (FILE *ram = std::fopen((base + ".ram").c_str(), "wb"))
            {
                std::fwrite(m_memory.getRDRAM(), 1, PS2_LEGACY_RAM_SIZE, ram);
                std::fclose(ram);
            }
            if (GSRasterBackend *backend = m_gs.rasterBackend())
                backend->Sync(GSSyncReason::DebugReadback);
            if (FILE *vram = std::fopen((base + ".vram").c_str(), "wb"))
            {
                std::fwrite(m_memory.getGSVRAM(), 1, PS2_GS_VRAM_SIZE, vram);
                std::fclose(vram);
            }
            std::cerr << "[dump] frame " << hostFrame << " -> " << base << ".ram/.vram pc=0x" << std::hex
                      << m_debugPc.load(std::memory_order_relaxed) << std::dec << std::endl;
        }
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << " vsync=" << gs.vsyncTick.load(std::memory_order_relaxed)
                                               << std::endl);
                // PS2X_TRACE_THREADS=1 adds every EE thread's state to each tick line.
                static const bool traceThreads = []()
                {
                    const char *value = std::getenv("PS2X_TRACE_THREADS");
                    return value && value[0] == '1';
                }();
                if (traceThreads)
                {
                    for (const EeThreadSnapshot &t : eeSnapshot.threads)
                        RUNTIME_LOG("[run:thread] id=" << t.id << " status=" << static_cast<int>(t.status)
                                                       << " wait=" << static_cast<int>(t.waitReason) << "/" << t.waitId
                                                       << " prio=" << t.currentPriority << std::hex
                                                       << " pc=0x" << t.pc << " ra=0x" << t.ra << " sp=0x" << t.sp
                                                       << " entry=0x" << t.entry << std::dec << std::endl);
                }

            }
        });
        // PS2X_CONTROL "renderer" (runtime/ps2_debug_control.h).
        if (const std::string renderer = ps2_debug_control::takePendingRenderer(); !renderer.empty())
        {
            const size_t space = renderer.find(' ');
            ps2x::boot::change("PS2X_GS_BACKEND", renderer.substr(0, space));
            ps2x::boot::change("PS2X_GS_SCALE", renderer.substr(space + 1));
            requestRasterBackendReload();
        }
        // Renderer settings changed (app menu): the new GS backend takes
        // over local memory from the old one.
        if (m_rasterReloadRequested)
        {
            static uint32_t attempts = 0;
            if (!m_pendingRasterBackend)
            {
                m_pendingRasterBackend = createConfiguredRasterBackend();
                attempts = 0;
            }
            // A transfer the game never finishes does not hold it up long.
            if (m_gs.switchRasterBackend(m_pendingRasterBackend, ++attempts > 60u))
            {
                m_rasterReloadRequested = false;
                s_forceLatch = true;
                std::cerr << "[gs] renderer switched" << std::endl;
            }
        }
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        bool frameChanged = true;
        // The picture is chosen when the display is ready for the next one:
        // at a refresh, or with variable refresh when a game frame is.
        // Frame interpolation (runtime/gs/gs_interpolate.h): on a display
        // faster than the game, every refresh at its top rate shows the game
        // at its own time less one game frame (displayTick), between the
        // latched frame and the next.
        const bool interpolating = interpolation.active(*this);
        const bool variableRefresh = ps2_host::variableRefresh() && m_gs.frameQueue() && !interpolating;
        if (variableRefresh)
            m_gs.waitForPresentationFrame(std::chrono::microseconds(16667));
        ps2_host::waitForRefresh();
        const auto latchTime = std::chrono::steady_clock::now();
        if (!variableRefresh)
            ps2x::host_pacing::noteLatch(latchTime); // (the guest's vblanks keep in phase with it)
        const double displayTick = interpolating ? interpolation.displayTick(latchTime) : -1.0;
        GS::TakenFrame taken;
        const uint8_t *framePixels = UploadFrame(this, presentWidth, presentHeight, frameChanged, taken, displayTick);
        // (interpolating, the next frame waits by design: a backlog is more than that one)
        if (!variableRefresh && m_gs.frameQueue())
            ps2x::host_pacing::noteTaken(taken.frame != 0 ? (interpolating && taken.waiting ? taken.waiting - 1u : taken.waiting) : 0u);
        if (presentLog.file && frameChanged)
        {
            uint32_t gpuWidth = 0, gpuHeight = 0;
            GSGpuPicture onGpu;
            const bool gpu = m_gs.latchedHostPresentationGpuPicture(onGpu, gpuWidth, gpuHeight);
            presentLog.frame = taken.frame;
            presentLog.latched = taken.frame == 0;
            presentLog.disturbed = m_gs.shownPictureDisturbed();
            presentLog.hash = gpu ? 0u
                                  : PresentLog::hashOf(framePixels, std::min<uint32_t>(presentWidth, FB_WIDTH),
                                                       std::min<uint32_t>(presentHeight, FB_HEIGHT), FB_WIDTH * 4u);
            std::fprintf(presentLog.file, "present host=%llu vsync=%llu frame=%llu latched=%d disturbed=%d %ux%u hash=%016llx\n",
                         static_cast<unsigned long long>(hostFrame),
                         static_cast<unsigned long long>(m_memory.gs().vsyncTick.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(presentLog.frame), presentLog.latched ? 1 : 0,
                         presentLog.disturbed ? 1 : 0, presentWidth, presentHeight,
                         static_cast<unsigned long long>(presentLog.hash));
        }
        if (taken.frame != 0)
            shownFrames.shown(taken.frame);
        else if (!frameChanged)
            shownFrames.repeated();

        // A PS2 picture fills a 4:3 TV whatever the buffer size: a 640x224
        // field-mode buffer is shown line-doubled, a 512x448 one stretched.
        // Fit a 4:3 rectangle instead of the buffer's own aspect; a frame
        // the game rendered for 16:9 is stretched to 16:9 instead, as a
        // widescreen TV (or PCSX2's aspect setting) does.
        {
            const uint64_t flips = s_displayFlips.load(std::memory_order_relaxed);
            if (flips != wideLastFlips)
            {
                const uint64_t marks = m_widescreenMarks.load(std::memory_order_relaxed);
                flipsSinceWideMark = marks != wideLastMarks ? 0u : flipsSinceWideMark + (flips - wideLastFlips);
                wideLastMarks = marks;
                wideLastFlips = flips;
            }
        }
        // The GPU renderer's picture stays in its texture on the window's
        // GPU device, at the internal resolution.
        GSGpuPicture gpuPicture;
        uint32_t gpuFrameWidth = 0, gpuFrameHeight = 0;
        const bool pictureOnGpu = m_gs.latchedHostPresentationGpuPicture(gpuPicture, gpuFrameWidth, gpuFrameHeight);
        // The picture between the latched frame and the next, in its place.
        if (interpolating && pictureOnGpu)
            interpolation.between(*this, displayTick, latchTime, taken, gpuPicture);
        const bool widePicture = widescreen() && flipsSinceWideMark <= 3u;
        {
            static const bool traceWide = []()
            {
                const char *value = std::getenv("PS2X_TRACE_WIDESCREEN");
                return value && value[0] == '1';
            }();
            static bool lastWide = false;
            if (traceWide && widePicture != lastWide)
                std::fprintf(stderr, "[widescreen] picture %s at vsync %llu (flips %llu, marks %llu)\n",
                             widePicture ? "16:9" : "4:3",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(wideLastFlips), static_cast<unsigned long long>(wideLastMarks));
            lastWide = widePicture;
        }
        {
            int windowWidth = 0, windowHeight = 0;
            ps2_host::windowSize(windowWidth, windowHeight);
            if (windowWidth > 0 && windowHeight > 0)
                m_windowAspect.store(static_cast<float>(windowWidth) / static_cast<float>(windowHeight),
                                     std::memory_order_relaxed);
        }
        const float kDisplayAspect = widePicture ? wideAspect() : 4.0f / 3.0f;
        // The debug UI builds its frame first; the host layer draws it over
        // the picture.
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        // Host keyboard/mouse/gamepad for the next pad read.
        m_padBackend.pollHost();
        // setPictureFilter: the picture as shown, filtered (read back from
        // the GPU renderer), for the window and for screenshots.
        std::shared_ptr<const PictureFilter> pictureFilter;
        {
            std::lock_guard<std::mutex> lock(m_screenshotMutex);
            pictureFilter = m_pictureFilter;
        }
        std::vector<uint8_t> filtered;
        uint32_t filteredWidth = 0, filteredHeight = 0;
        if (pictureFilter)
        {
            if (pictureOnGpu)
            {
                if (!m_gs.readLatchedHostPresentationGpuPicture(filtered, filteredWidth, filteredHeight))
                    filtered.clear();
            }
            else
            {
                filteredWidth = std::max<uint32_t>(1u, std::min<uint32_t>(presentWidth, FB_WIDTH));
                filteredHeight = std::max<uint32_t>(1u, std::min<uint32_t>(presentHeight, FB_HEIGHT));
                filtered.resize(static_cast<size_t>(filteredWidth) * filteredHeight * 4u);
                for (uint32_t y = 0; y < filteredHeight; ++y)
                    std::memcpy(filtered.data() + static_cast<size_t>(y) * filteredWidth * 4u,
                                framePixels + static_cast<size_t>(y) * FB_WIDTH * 4u,
                                static_cast<size_t>(filteredWidth) * 4u);
            }
            if (!filtered.empty() && filtered.size() >= static_cast<size_t>(filteredWidth) * filteredHeight * 4u)
                (*pictureFilter)(filtered.data(), filteredWidth, filteredHeight, filteredWidth * 4u, kDisplayAspect);
            else
                filtered.clear();
        }
        {
            ps2_host::Frame frame;
            frame.rgba = framePixels;
            frame.stride = FB_WIDTH * 4u;
            frame.texWidth = FB_WIDTH;
            frame.texHeight = FB_HEIGHT;
            frame.changed = frameChanged;
            frame.width = std::max<uint32_t>(1u, std::min<uint32_t>(presentWidth, FB_WIDTH));
            frame.height = std::max<uint32_t>(1u, std::min<uint32_t>(presentHeight, FB_HEIGHT));
            if (pictureOnGpu)
            {
                frame.gpuTexture = gpuPicture.texture;
                frame.gpuLevels = gpuPicture.levels;
                frame.width = gpuPicture.width;
                frame.height = gpuPicture.height;
            }
            frame.aspect = kDisplayAspect;
            // Integer scaling: whole multiples of 640x480 in framebuffer
            // pixels (a Retina window has two per point), nearest filtering.
            frame.integerScale = m_hostOptions.integerScale;
            frame.linearFilter = m_hostOptions.smoothFilter;
            if (!filtered.empty())
            {
                frame.rgba = filtered.data();
                frame.stride = filteredWidth * 4u;
                frame.texWidth = filteredWidth;
                frame.texHeight = filteredHeight;
                frame.width = filteredWidth;
                frame.height = filteredHeight;
                frame.changed = true;
                frame.gpuTexture = nullptr;
            }
            ps2x::ftrace::Scope traceHost(ps2x::ftrace::kHostFrameBegin, ps2x::ftrace::kHostFrameEnd);
            ps2_host::present(frame);
        }
        interpolation.presented(std::chrono::steady_clock::now());
        if (taken.frame != 0 && shownLatencies.size() < 100000u)
            shownLatencies.push_back(
                (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() -
                 taken.madeNs) * 1e-6);
        // A play recording's telemetry: this frame on the guest's vsync clock.
        if (play_recording::active())
        {
            playTelemetry.sample(*this, m_memory, s_displayFlips.load(std::memory_order_relaxed),
                             gameThreadPort.load(std::memory_order_acquire),
                             m_eeScheduler ? m_eeScheduler->droppedPacingNs() : 0u);
        }
        // The picture as presented: the displayed part of the frame image,
        // at 4:3 (640x480) or the widescreen aspect (854x480 for 16:9),
        // times the GPU renderer's internal resolution.
        // Scaling and PNG encoding run on a thread of their own (a 3x
        // picture takes a few hundred ms); done(ok) is called there.
        auto writeShot = [&](const std::string &path, std::function<void(bool)> done)
        {
            if (presentLog.file)
                std::fprintf(presentLog.file, "shot host=%llu frame=%llu latched=%d disturbed=%d hash=%016llx %s\n",
                             static_cast<unsigned long long>(hostFrame), static_cast<unsigned long long>(presentLog.frame),
                             presentLog.latched ? 1 : 0, presentLog.disturbed ? 1 : 0,
                             static_cast<unsigned long long>(presentLog.hash), path.c_str());
            const uint32_t outWidth = widePicture ? static_cast<uint32_t>(480.0f * kDisplayAspect + 0.5f) : 640u;
            std::vector<uint8_t> pixels;
            uint32_t width = 0, height = 0, scale = 1;
            if (!filtered.empty())
            {
                pixels = filtered;
                width = filteredWidth;
                height = filteredHeight;
                scale = pictureOnGpu ? gpuPicture.scale : 1u;
            }
            else if (pictureOnGpu)
            {
                if (!m_gs.readLatchedHostPresentationGpuPicture(pixels, width, height))
                {
                    done(false);
                    return;
                }
                scale = gpuPicture.scale;
            }
            else
            {
                width = std::max<uint32_t>(1u, std::min<uint32_t>(presentWidth, FB_WIDTH));
                height = std::max<uint32_t>(1u, std::min<uint32_t>(presentHeight, FB_HEIGHT));
                pixels.resize(static_cast<size_t>(width) * height * 4u);
                for (uint32_t y = 0; y < height; ++y)
                    std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4u,
                                framePixels + static_cast<size_t>(y) * FB_WIDTH * 4u, static_cast<size_t>(width) * 4u);
            }
            std::thread([path, pixels = std::move(pixels), width, height, outWidth, scale, done = std::move(done)]()
                        { done(ps2_host::writePng(path, pixels.data(), width, height, width * 4u, outWidth * scale,
                                                  480u * scale)); })
                .detach();
        };
        shots.capture(writeShot);
        // PS2X_CONTROL "shot" (runtime/ps2_debug_control.h).
        if (const std::string controlShot = ps2_debug_control::takePendingShot(); !controlShot.empty())
            writeShot(controlShot, [controlShot](bool ok)
                      { ps2_debug_control::shotDone(controlShot, ok); });
        // requestScreenshot (a screenshot key of a game override).
        {
            std::vector<std::pair<std::string, std::function<void(bool)>>> requests;
            {
                std::lock_guard<std::mutex> lock(m_screenshotMutex);
                requests.swap(m_screenshotRequests);
            }
            for (auto &[path, done] : requests)
                writeShot(path, [done = std::move(done)](bool ok)
                          {
                              if (done)
                                  done(ok);
                          });
        }
        // Settings changed from the game's menus or the app's (runtime/ps2_settings.h).
        ps2x::settings::runPending();
        if (m_hostFrameCallback)
        {
            m_hostFrameCallback(*this);
        }
        // PS2X_PERF_LOG=<seconds> prints host frames, guest vsyncs and
        // display buffer flips (the game's frame rate) per second.
        static const double perfInterval = []()
        {
            const char *value = std::getenv("PS2X_PERF_LOG");
            return value ? std::atof(value) : 0.0;
        }();
        if (perfInterval > 0.0)
        {
            if (hostFrame == 1)
            {
                std::fprintf(stderr, "[perf] first frame %.0f ms after start\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_processStart).count());
            }
            auto secondsNow = []()
            {
                return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_processStart).count();
            };
            static double perfStart = secondsNow();
            static uint64_t perfFrames = 0;
            static uint64_t perfVsync = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
            static uint64_t perfFlips = s_displayFlips.load(std::memory_order_relaxed);
            ++perfFrames;
            const double now = secondsNow();
            // Game frames as shown: the time from one host frame that showed
            // a new game picture to the next (a multiple of the display's
            // refresh), for the frame pacing (PS2X_PERF_FRAMES=1 prints each).
            static uint64_t seenFlips = perfFlips;
            static double newFrameAt = now;
            static std::vector<double> frameGaps;
            if (const uint64_t f = s_displayFlips.load(std::memory_order_relaxed); f != seenFlips)
            {
                frameGaps.push_back((now - newFrameAt) * 1e3);
                seenFlips = f;
                newFrameAt = now;
            }
            if (now - perfStart >= perfInterval)
            {
                const uint64_t vsync = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
                const uint64_t flips = s_displayFlips.load(std::memory_order_relaxed);
                const double span = now - perfStart;
                std::fprintf(stderr, "[perf] t=%.1f host_fps=%.1f vsync_hz=%.1f game_fps=%.1f vsync=%llu\n", now,
                             static_cast<double>(perfFrames) / span, static_cast<double>(vsync - perfVsync) / span,
                             static_cast<double>(flips - perfFlips) / span, static_cast<unsigned long long>(vsync));
                if (ps2x::gs_interpolate::enabled())
                    interpolation.report(*this, span);
                if (!frameGaps.empty())
                {
                    std::vector<double> sorted = frameGaps;
                    std::sort(sorted.begin(), sorted.end());
                    size_t over20 = 0, over34 = 0;
                    for (const double g : sorted)
                    {
                        over20 += g > 20.0 ? 1u : 0u;
                        over34 += g > 34.0 ? 1u : 0u;
                    }
                    std::fprintf(stderr, "[perf] game frames: n=%zu p50=%.1f p99=%.1f max=%.1f ms over20=%zu over34=%zu\n",
                                 sorted.size(), sorted[sorted.size() / 2], sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)],
                                 sorted.back(), over20, over34);
                    static const bool eachFrame = []()
                    {
                        const char *value = std::getenv("PS2X_PERF_FRAMES");
                        return value && value[0] == '1';
                    }();
                    if (eachFrame)
                    {
                        std::string line = "[perf] frame ms:";
                        char item[16];
                        for (const double g : frameGaps)
                        {
                            std::snprintf(item, sizeof(item), " %.1f", g);
                            line += item;
                        }
                        std::fprintf(stderr, "%s\n", line.c_str());
                    }
                    frameGaps.clear();
                }
                if (m_gs.frameQueue())
                {
                    // Game frames by the refreshes they stayed on screen, those
                    // never shown (pushed out of the queue, or skipped), and
                    // the time from a frame's picture to its present.
                    static uint64_t lastQueued = 0, lastDropped = 0, lastNever = 0;
                    static uint64_t lastBy[5] = {};
                    const uint64_t queued = m_gs.framesMade(), droppedFrames = m_gs.framesDropped();
                    std::sort(shownLatencies.begin(), shownLatencies.end());
                    auto at = [&](double q)
                    { return shownLatencies.empty() ? 0.0 : shownLatencies[std::min(shownLatencies.size() - 1, static_cast<size_t>(q * shownLatencies.size()))]; };
                    std::fprintf(stderr, "[perf] shown: frames=%llu refreshes1=%llu 2=%llu 3=%llu 4=%llu 5+=%llu never=%llu "
                                         "pushed_out=%llu latency_p50=%.1f p99=%.1f max=%.1f ms\n",
                                 static_cast<unsigned long long>(queued - lastQueued),
                                 static_cast<unsigned long long>(shownFrames.byRefreshes[0] - lastBy[0]),
                                 static_cast<unsigned long long>(shownFrames.byRefreshes[1] - lastBy[1]),
                                 static_cast<unsigned long long>(shownFrames.byRefreshes[2] - lastBy[2]),
                                 static_cast<unsigned long long>(shownFrames.byRefreshes[3] - lastBy[3]),
                                 static_cast<unsigned long long>(shownFrames.byRefreshes[4] - lastBy[4]),
                                 static_cast<unsigned long long>(shownFrames.never - lastNever),
                                 static_cast<unsigned long long>(droppedFrames - lastDropped), at(0.5), at(0.99),
                                 shownLatencies.empty() ? 0.0 : shownLatencies.back());
                    lastQueued = queued;
                    lastDropped = droppedFrames;
                    lastNever = shownFrames.never;
                    std::copy(shownFrames.byRefreshes, shownFrames.byRefreshes + 5, lastBy);
                    shownLatencies.clear();
                }
                else
                {
                    // Without the queue: display buffer changes the window never showed.
                    static uint64_t lastChanges = m_gs.framesMade(), lastFlips = flips;
                    const uint64_t changes = m_gs.framesMade();
                    const uint64_t made = changes - lastChanges, seen = flips - lastFlips;
                    std::fprintf(stderr, "[perf] shown: frames=%llu new_pictures=%llu never=%llu\n",
                                 static_cast<unsigned long long>(made), static_cast<unsigned long long>(seen),
                                 static_cast<unsigned long long>(made > seen ? made - seen : 0u));
                    lastChanges = changes;
                    lastFlips = flips;
                }
                if (m_eeScheduler)
                {
                    // Catch-up the pacer dropped after stalls in this interval.
                    static uint64_t lastDropped = 0;
                    const uint64_t dropped = m_eeScheduler->droppedPacingNs();
                    if (dropped != lastDropped)
                        std::fprintf(stderr, "[perf] pacer dropped %.1f ms of catch-up\n", (dropped - lastDropped) * 1e-6);
                    lastDropped = dropped;
                }
                {
                    static uint64_t lastRetries = 0;
                    const uint64_t retries = m_gs.presentationRetries();
                    if (retries != lastRetries)
                        std::fprintf(stderr, "[perf] presentation latched again after a display switch: %llu\n",
                                     static_cast<unsigned long long>(retries - lastRetries));
                    lastRetries = retries;
                }
                {
                    // CPU time per guest vsync in this interval: game (EE) thread,
                    // VIF1/VU1 worker, GS worker, host (this) thread, whole process.
                    auto machThreadMs = [](uint64_t port) -> double { return threadCpuMs(port); };
                    auto processMs = []() -> double
                    {
#if defined(_WIN32)
                        return ps2x::processCpuMilliseconds();
#else
                        rusage usage{};
                        getrusage(RUSAGE_SELF, &usage);
                        return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e3 +
                               (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-3;
#endif
                    };
#if defined(__APPLE__)
                    static const uint64_t hostPort = hostThreadCpuPort();
#else
                    static const uint64_t hostPort = hostThreadCpuPort();
#endif
                    const double now[5] = {machThreadMs(gameThreadPort.load(std::memory_order_acquire)),
                                           m_memory.vif1WorkerCpuMs(), m_memory.gsWorkerCpuMs(),
                                           machThreadMs(hostPort), processMs()};
                    static double last[5] = {};
                    static uint64_t lastVsync = 0;
                    const double vsyncs = static_cast<double>(std::max<uint64_t>(1u, vsync - lastVsync));
                    if (lastVsync != 0)
                        std::fprintf(stderr, "[perf] cpu ms/vsync: game=%.2f vif1=%.2f gs=%.2f host=%.2f process=%.2f\n",
                                     (now[0] - last[0]) / vsyncs, (now[1] - last[1]) / vsyncs, (now[2] - last[2]) / vsyncs,
                                     (now[3] - last[3]) / vsyncs, (now[4] - last[4]) / vsyncs);
                    std::copy(now, now + 5, last);
                    lastVsync = vsync;
                }
                if (m_memory.asyncGraphics())
                {
                    // EE waits on the graphics workers, per reason, in this interval.
                    static std::array<uint64_t, PS2Memory::SyncReasonCount> lastWaits{};
                    static const char *const names[] = {"vifreg", "gifreg", "vumem", "gspriv", "gshle", "gifchain", "sync", "other"};
                    std::string line;
                    for (uint32_t r = 0; r < PS2Memory::SyncReasonCount; ++r)
                    {
                        const uint64_t waits = m_memory.syncWaits(static_cast<PS2Memory::SyncReason>(r));
                        line += " " + std::string(names[r]) + "=" + std::to_string(waits - lastWaits[r]);
                        lastWaits[r] = waits;
                    }
                    std::fprintf(stderr, "[perf] ee waits:%s\n", line.c_str());
                }
                perfStart = now;
                perfFrames = 0;
                perfVsync = vsync;
                perfFlips = flips;
            }
        }

        if (ps2_host::quitRequested())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
        // A replay reached its stop (ZAXIS_REPLAY_STOP, or the recording's end), or a recording run was told to end.
        if (play_recording::stopRequested())
        {
            RUNTIME_LOG((play_recording::mode() == play_recording::Mode::Replay
                             ? "[run] the replay reached its stop, breaking out of loop"
                             : "[run] told to end (SIGTERM or SIGINT), breaking out of loop"));
            requestStop();
            break;
        }
        // The game left for the app's menu: stopped as for a window close;
        // the app shows its menu.
        if (ps2_host::exitToMenuRequested())
        {
            RUNTIME_LOG("[run] exit to the menu requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    // (a game thread waiting on a change of its Settings page goes on)
    ps2x::settings::setFrameLoop(false);
    requestStop();
    const auto stopReason = [&]() -> const char *
    {
        if (ps2_host::quitRequested())
            return "window closed";
        if (ps2_host::exitToMenuRequested())
            return "left for the menu";
        if (play_recording::stopRequested())
            return play_recording::mode() == play_recording::Mode::Replay ? "replay stop" : "terminated";
        if (gameThreadFinished.load())
            return "game thread ended";
        return "stopped";
    };
    if (gameThread.joinable())
    {
        // The game thread sees the stop where the guest waits (a vsync, a
        // thread switch). Guest code that never gets there (a loop that does
        // not end) would hold the process here for good, deaf to a window
        // close or SIGTERM: past kStopWaitSeconds the process ends without
        // it, the recording given its end record first.
        constexpr int kStopWaitSeconds = 10;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kStopWaitSeconds);
        while (!gameThreadFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!gameThreadFinished.load(std::memory_order_acquire))
        {
            const uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[run] the game thread did not stop within %d s (%s; last EE pc 0x%x): the process ends without it\n",
                         kStopWaitSeconds, stopReason(), pc);
            const std::string why = std::string(stopReason()) + " (the game thread did not stop)";
            play_recording::noteError("the game thread did not stop");
            play_recording::endBoot(why.c_str());
            std::cout.flush();
            std::cerr.flush();
            std::fflush(nullptr);
            std::_Exit(std::strcmp(stopReason(), "terminated") == 0 ? 143 : 1); // (143: SIGTERM's)
        }
        gameThread.join();
    }
    m_memory.stopAsyncGraphics();
    play_recording::endBoot(stopReason());

    for (const StoppedHook hook : m_stoppedHooks)
        hook(*this);

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    // The host stays up: the destructor shuts it down (after the IOP's audio
    // stream), or its owner does (HostOptions::hostOwned).

    RUNTIME_LOG("[run] exiting loop");
    };
    ps2x::spot::setHostLoop(hostLoop);
    hostLoop();
}

void PS2Runtime::addStoppedHook(StoppedHook hook)
{
    if (hook && std::find(m_stoppedHooks.begin(), m_stoppedHooks.end(), hook) == m_stoppedHooks.end())
        m_stoppedHooks.push_back(hook);
}
