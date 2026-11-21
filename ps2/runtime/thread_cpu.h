#pragma once

#include <cstdint>

// CPU time for the perf logs and tools: CLOCK_THREAD_CPUTIME_ID on POSIX,
// thread / process cycle times on Windows (macOS reads other threads
// through Mach ports where it is used).
namespace zx
{
    // The calling thread, in seconds.
    double currentThreadCpuSeconds();
    // Windows and Linux: a handle to the calling thread (a thread handle, the
    // thread's CPU clock) that other threads can pass to threadCpuMilliseconds
    // (kept for the process's life); 0 elsewhere.
    uint64_t currentThreadCpuHandle();
    double threadCpuMilliseconds(uint64_t handle);
    // Windows: the whole process, in milliseconds (0 elsewhere).
    double processCpuMilliseconds();
}
