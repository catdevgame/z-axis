#include "runtime/thread_priority.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#endif

namespace zx::thread_priority
{
    void raiseFrameRateThread(const char *name)
    {
#if defined(_WIN32)
        const HANDLE thread = GetCurrentThread();
        const bool priority = SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL) != 0;
        // Execution speed throttling controlled by the thread (ControlMask)
        // and off (StateMask 0): high quality of service.
        THREAD_POWER_THROTTLING_STATE throttling{};
        throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        const bool unthrottled = SetThreadInformation(thread, ThreadPowerThrottling, &throttling, sizeof(throttling)) != 0;
        if (!priority)
            std::fprintf(stderr, "[thread] %s: above-normal priority refused\n", name);
        if (!unthrottled)
            std::fprintf(stderr, "[thread] %s: power throttling off refused (error %lu)\n", name, GetLastError());
#else
        static_cast<void>(name);
#endif
    }
}
