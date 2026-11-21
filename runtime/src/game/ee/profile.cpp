// ZX_FNPROF=<file.csv>: a profile of the game's functions, for planning
// what to port next (runtime/tools/readable_coverage.py reads it).
//
// Every function start in the dispatch table (each recompiled function,
// readable replacement or hook) is wrapped with a counter: calls, and the
// host time spent in it with and without its callees, on the EE thread.
// A function that yields to the scheduler halfway ends its measurement
// there (its resumption runs unwrapped), so times of functions that wait
// (thread switches, vsync waits) are low bounds; the ranking is what
// matters. The file is written at exit and every 10 seconds of vsyncs:
//
//     address,calls,inclusive_ns,self_ns
//
// ZX_FNPROF_FROM=<vsync>: start counting at this vsync (e.g. once the
// level runs), ZX_FNPROF_FOR=<vsyncs>: stop after that many.

#include "readable.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#include "sym/ee/profile.cpp.h"

namespace game::profile
{
    namespace
    {
        // Code region of the game's ELF (start .. the end of .text).
        constexpr uint32_t kCodeBegin = (1u << 20);
        constexpr uint32_t kCodeEnd = sym::codeEnd;

        struct Stats
        {
            uint32_t address = 0;
            PS2Runtime::RecompiledFunction function = nullptr;
            uint64_t calls = 0;
            uint64_t inclusive = 0;
            uint64_t self = 0;
        };

        struct Profiler
        {
            std::string path;
            std::vector<Stats> functions;
            std::vector<int32_t> indexOf; // per 4-byte slot of the code region, -1: not a function start
            // Children's time of the open calls (the shadow stack).
            std::vector<uint64_t> childTime;
            uint64_t from = 0, until = ~0ull;
            uint64_t lastWrite = 0;
            bool counting = false;
        };
        Profiler *g_profiler = nullptr;

        uint64_t now()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        void write()
        {
            std::ofstream out(g_profiler->path, std::ios::trunc);
            out << "address,calls,inclusive_ns,self_ns\n";
            char line[96];
            for (const Stats &s : g_profiler->functions)
            {
                if (!s.calls)
                    continue;
                std::snprintf(line, sizeof(line), "0x%08x,%llu,%llu,%llu\n", s.address,
                              static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.inclusive),
                              static_cast<unsigned long long>(s.self));
                out << line;
            }
        }

        void counted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Profiler &p = *g_profiler;
            const uint32_t slot = (ctx->pc - kCodeBegin) >> 2;
            const int32_t index = (ctx->pc >= kCodeBegin && ctx->pc < kCodeEnd) ? p.indexOf[slot] : -1;
            if (index < 0)
                return; // not reachable: only function starts point here
            Stats &s = p.functions[static_cast<size_t>(index)];
            if (!p.counting)
            {
                s.function(rdram, ctx, runtime);
                return;
            }
            p.childTime.push_back(0);
            const uint64_t start = now();
            s.function(rdram, ctx, runtime);
            const uint64_t spent = now() - start;
            const uint64_t children = p.childTime.back();
            p.childTime.pop_back();
            ++s.calls;
            s.inclusive += spent;
            s.self += spent > children ? spent - children : 0u;
            if (!p.childTime.empty())
                p.childTime.back() += spent;
        }

        // Wraps every function start in the table: once, at the first vsync,
        // after every override has installed its hooks.
        void wrapAll(PS2Runtime &runtime)
        {
            Profiler &p = *g_profiler;
            p.indexOf.assign((kCodeEnd - kCodeBegin) >> 2, -1);
            // A function's start is the lowest slot holding its pointer; the
            // later ones are its resume points.
            std::unordered_set<PS2Runtime::RecompiledFunction> seen;
            for (uint32_t address = kCodeBegin; address < kCodeEnd; address += 4u)
            {
                if (!runtime.hasFunction(address))
                    continue;
                PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(address);
                if (!seen.insert(fn).second)
                    continue;
                Stats s;
                s.address = address;
                s.function = fn;
                p.indexOf[(address - kCodeBegin) >> 2] = static_cast<int32_t>(p.functions.size());
                p.functions.push_back(s);
            }
            for (const Stats &s : p.functions)
                runtime.replaceFunction(s.address, &counted);
            std::cout << "[fnprof] " << p.functions.size() << " functions counted into " << p.path << std::endl;
        }

        void onVsync(PS2Runtime &runtime, uint64_t tick)
        {
            Profiler &p = *g_profiler;
            if (p.functions.empty())
                wrapAll(runtime);
            const bool counting = tick >= p.from && tick < p.until;
            if (counting != p.counting)
            {
                p.counting = counting;
                p.childTime.clear();
                if (!counting)
                    write();
            }
            if (counting && tick - p.lastWrite >= 600u)
            {
                p.lastWrite = tick;
                write();
            }
        }

        void atExit(PS2Runtime &)
        {
            if (g_profiler)
                write();
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *path = std::getenv("ZX_FNPROF");
        if (!path || !*path)
            return;
        static Profiler profiler;
        profiler.path = path;
        if (const char *from = std::getenv("ZX_FNPROF_FROM"))
            profiler.from = std::strtoull(from, nullptr, 0);
        if (const char *span = std::getenv("ZX_FNPROF_FOR"))
            profiler.until = profiler.from + std::strtoull(span, nullptr, 0);
        g_profiler = &profiler;
        PS2Runtime *rt = &runtime;
        runtime.addVSyncHook([rt](uint8_t *, uint64_t tick) { onVsync(*rt, tick); });
        ps2_game_overrides::addShutdownHook(&atExit);
    }
}
