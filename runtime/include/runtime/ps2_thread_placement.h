#pragma once

// Which cores the emulator's busy threads run on (Linux and Android), behind
// ZX_THREAD_PLACEMENT=1 (off by default: the scheduler places them). Phone
// chips have tiers of cores (on the test handheld's Snapdragon 8 Gen 2 one prime,
// four mid and three little); the scheduler moved the game (EE) thread and
// the GS worker on and off the prime core, and the one off it ran a third
// slower. With placement the game thread keeps the fastest tier and the
// graphics workers (VIF1/VU1, GS) the next one. The tiers come from the
// kernel's cpu_capacity; a machine with one tier is left alone.

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <vector>
#endif
#include <cstdlib>

namespace zx::placement
{
    enum class Role
    {
        Game,     // the EE thread
        Graphics, // the VIF1/VU1 and GS workers
    };

    // ZX_THREAD_PLACEMENT=1 on Linux and Android.
    inline bool enabled()
    {
#if defined(__linux__)
        static const bool on = []()
        {
            const char *v = std::getenv("ZX_THREAD_PLACEMENT");
            return v && v[0] == '1';
        }();
        return on;
#else
        return false;
#endif
    }

    inline void placeCurrentThread(Role role)
    {
#if defined(__linux__)
        if (!enabled())
            return;
        const long count = sysconf(_SC_NPROCESSORS_CONF);
        if (count <= 1 || count > 256)
            return;
        std::vector<long> capacity(static_cast<size_t>(count), 0);
        long top = 0;
        for (long cpu = 0; cpu < count; ++cpu)
        {
            char path[96];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", cpu);
            FILE *f = std::fopen(path, "r");
            if (!f)
                return; // no tiers to go by
            long value = 0;
            if (std::fscanf(f, "%ld", &value) != 1)
                value = 0;
            std::fclose(f);
            capacity[static_cast<size_t>(cpu)] = value;
            top = value > top ? value : top;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        int chosen = 0;
        for (long cpu = 0; cpu < count; ++cpu)
        {
            const long c = capacity[static_cast<size_t>(cpu)];
            const bool fits = role == Role::Game ? c == top : (c < top && c * 2 >= top);
            if (fits)
            {
                CPU_SET(static_cast<int>(cpu), &set);
                ++chosen;
            }
        }
        if (chosen == 0 || chosen == count)
            return;
        const int rc = sched_setaffinity(0, sizeof(set), &set);
        std::fprintf(stderr, "[placement] %s thread on %d of %ld cores%s\n", role == Role::Game ? "game" : "graphics", chosen,
                     count, rc == 0 ? "" : " (refused)");
#else
        static_cast<void>(role);
#endif
    }
} // namespace zx::placement
