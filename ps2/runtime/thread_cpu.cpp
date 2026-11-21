#include "runtime/thread_cpu.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#else
#include <ctime>
#if defined(__linux__)
#include <pthread.h>
#endif
#endif

namespace zx
{
#if defined(_WIN32)
    namespace
    {
        double filetimeMs(const FILETIME &kernel, const FILETIME &user)
        {
            auto ticks = [](const FILETIME &t)
            { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-4; // 100 ns units
        }

        // GetThreadTimes charges whole 15.6 ms scheduler ticks to the thread
        // that runs at the tick, so threads that run in short bursts (the
        // graphics workers) show far less than they use. Thread cycle times
        // are exact; they count the TSC, whose rate is measured once against
        // QueryPerformanceCounter (over at least 20 ms).
        double tscPerMs()
        {
            struct Start
            {
                LARGE_INTEGER qpc{};
                uint64_t tsc = 0;
                Start()
                {
                    QueryPerformanceCounter(&qpc);
                    tsc = __rdtsc();
                }
            };
            static const Start start;
            static std::atomic<double> rate{0.0};
            double value = rate.load(std::memory_order_relaxed);
            if (value > 0.0)
                return value;
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&now);
            double ms = static_cast<double>(now.QuadPart - start.qpc.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
            if (ms < 20.0)
            {
                Sleep(static_cast<DWORD>(21.0 - ms));
                QueryPerformanceCounter(&now);
                ms = static_cast<double>(now.QuadPart - start.qpc.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
            }
            const uint64_t tsc = __rdtsc();
            value = static_cast<double>(tsc - start.tsc) / ms;
            rate.store(value, std::memory_order_relaxed);
            return value;
        }
    }
#endif

    double currentThreadCpuSeconds()
    {
#if defined(_WIN32)
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
            return static_cast<double>(cycles) / tscPerMs() * 1e-3;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
            return 0.0;
        return filetimeMs(kernel, user) * 1e-3;
#else
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
    }

    uint64_t currentThreadCpuHandle()
    {
#if defined(_WIN32)
        HANDLE handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                             THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            return 0;
        return reinterpret_cast<uint64_t>(handle);
#elif defined(__linux__)
        // The thread's CPU clock id, with a marker bit so that a valid clock
        // is never 0 (clock ids are negative for threads).
        clockid_t clock{};
        if (pthread_getcpuclockid(pthread_self(), &clock) != 0)
            return 0;
        return static_cast<uint64_t>(static_cast<uint32_t>(clock)) | (1ull << 32);
#else
        return 0;
#endif
    }

    double threadCpuMilliseconds(uint64_t handle)
    {
#if defined(_WIN32)
        if (handle == 0)
            return 0.0;
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(reinterpret_cast<HANDLE>(handle), &cycles))
            return static_cast<double>(cycles) / tscPerMs();
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetThreadTimes(reinterpret_cast<HANDLE>(handle), &creation, &exit, &kernel, &user))
            return 0.0;
        return filetimeMs(kernel, user);
#elif defined(__linux__)
        if (handle == 0)
            return 0.0;
        timespec ts{};
        if (clock_gettime(static_cast<clockid_t>(static_cast<uint32_t>(handle)), &ts) != 0)
            return 0.0;
        return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
#else
        (void)handle;
        return 0.0;
#endif
    }

    double processCpuMilliseconds()
    {
#if defined(_WIN32)
        ULONG64 cycles = 0;
        if (QueryProcessCycleTime(GetCurrentProcess(), &cycles))
            return static_cast<double>(cycles) / tscPerMs();
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
            return 0.0;
        return filetimeMs(kernel, user);
#else
        return 0.0;
#endif
    }
}
