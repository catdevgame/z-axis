#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// Frame timeline trace (developer tool): timestamped marks from the EE, the
// graphics workers and the window thread, to see where a frame's wall time
// goes and which threads wait for which. Off unless started (ZX_CONTROL
// "ftrace start" / "ftrace stop FILE"); a mark costs one relaxed load when
// off. tools/ftrace.py in the game repo reads the file.
namespace zx::ftrace
{
    enum Kind : uint16_t
    {
        kTaskBegin = 1, // arg: stage (1 VIF1/VU1, 2 GS)
        kTaskEnd,
        kDrainBegin, // EE waits for a stage; arg: stage
        kDrainEnd,
        kSwapWaitBegin, // EE waits at the buffer swap for the previous one
        kSwapWaitEnd,
        kSwapPosted, // EE queued a buffer swap
        kSwapRun,    // GS worker ran it
        kVsync,      // guest vsync tick
        kHostFrameBegin,
        kHostFrameEnd,
        kGpuClaimBegin, // a thread waits for another's GPU command buffer
        kGpuClaimEnd,
        kGpuSubmitBegin,
        kGpuSubmitEnd,
        kGpuPassBegin, // render pass recorded (closePass)
        kGpuPassEnd,
        kVifPost, // EE posted VIF1 work
        kLatchBegin, // window thread takes the GS picture
        kLatchEnd,
        kUser1,
        kUser2,
        kUser3,
        kUser4,
        kUser5,
        kUser6,
        kUser7,
        kUser8,
        // GPU renderer work that may stall a frame (begin; end = begin + 100).
        kPipelineCreate, // arg: 1 native, 2 skinned native
        kTextureDecode,  // a GS texture decoded on the CPU; arg: width / 8
        kTargetLoad,     // a render target loaded from GS memory
        kTargetDownload, // a render target written back to GS memory
        kReplacementUpload, // a replacement picture's upload prepared; arg: KB / 64
        kCpuDraw,        // a primitive drawn by the CPU renderer
        kMipCopy,        // a mip copy built
        kBufferCreate,   // a GPU buffer or transfer buffer created
        // Frames (marks): the EE wrote DISPFB1/2 (arg: FBP), the window
        // thread's latch saw a new display buffer (arg: FBP).
        kDisplayWrite,
        kFlipSeen,
        // EE-side work that may stall a frame (begin; end = begin + 100).
        kDiscOpen,  // a game file opened (the native disc layer)
        kDiscRead,  // a game file read; arg: KB / 16
        kFileWrite, // a host file written (saves, records)
        kEeIdle,    // the EE scheduler waits for an event or timer (guest idle)
    };

    struct Event
    {
        int64_t ns;
        uint32_t thread;
        uint16_t kind;
        uint16_t arg;
    };

    extern std::atomic<bool> g_on;
    void markSlow(uint16_t kind, uint16_t arg);

    inline void mark(uint16_t kind, uint16_t arg = 0)
    {
        if (g_on.load(std::memory_order_relaxed))
            markSlow(kind, arg);
    }

    // Starts recording (up to `capacity` events), or stops and writes
    // the events to `path` (binary: Event records). Return false on error.
    bool start(uint32_t capacity = 4u << 20);
    bool stop(const std::string &path);

    struct Scope
    {
        uint16_t endKind, arg;
        Scope(uint16_t begin, uint16_t end, uint16_t a = 0) : endKind(end), arg(a) { mark(begin, a); }
        ~Scope() { mark(endKind, arg); }
    };
}
