// ZX_TAILCALL_STATS=<file.csv>: the tail calls (a `j` to a function's
// start) generated code made to a function the table holds a replacement
// for (PS2Runtime::tailCall): a readable version, a game override, the
// recorder's shim. Written at exit and every 10 seconds of vsyncs:
//
//     source,target,calls,kind,name
//
// source is the jump's address; kind is what the table held for the target
// when the file was written: `readable` (the readable version) or
// `override` (anything else: a hook, which may wrap a readable version);
// name is the readable registration's, if any. <file>.replaced lists every
// function whose table entry is no longer the generated one (address,kind,
// name), for the sites the run did not reach.

#include "readable.h"

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace game::profile
{
    namespace
    {
        std::string g_path;
        PS2Runtime *g_runtime = nullptr;

        void write()
        {
            std::FILE *file = std::fopen(g_path.c_str(), "w");
            if (!file)
                return;
            std::fprintf(file, "source,target,calls,kind,name\n");
            for (const PS2Runtime::TailCallCount &c : PS2Runtime::replacedTailCalls())
            {
                const readable::Function *f = readable::find(c.targetPc, readable::runningGame());
                const PS2Runtime::RecompiledFunction now =
                    g_runtime && g_runtime->hasFunction(c.targetPc) ? g_runtime->lookupFunction(c.targetPc) : nullptr;
                const bool isReadable = f && f->readable && now == f->readable;
                std::fprintf(file, "0x%06X,0x%06X,%llu,%s,%s\n", c.sourcePc, c.targetPc,
                             static_cast<unsigned long long>(c.calls), isReadable ? "readable" : "override",
                             f ? f->name : "");
            }
            std::fclose(file);

            // Every replaced function (for the sites not reached): address,kind,name.
            if (std::FILE *all = std::fopen((g_path + ".replaced").c_str(), "w"))
            {
                std::fprintf(all, "address,kind,name\n");
                for (uint32_t address : PS2Runtime::replacedFunctions())
                {
                    const readable::Function *f = readable::find(address, readable::runningGame());
                    const bool isReadable = f && f->readable && g_runtime->lookupFunction(address) == f->readable;
                    std::fprintf(all, "0x%06X,%s,%s\n", address, isReadable ? "readable" : "override", f ? f->name : "");
                }
                std::fclose(all);
            }
        }

        void atExit(PS2Runtime &) { write(); }
    }

    void installTailCallStats(PS2Runtime &runtime)
    {
        const char *path = std::getenv("ZX_TAILCALL_STATS");
        if (!path || !*path)
            return;
        g_path = path;
        g_runtime = &runtime;
        PS2Runtime::s_tailCallTable.stats = true;
        runtime.addVSyncHook([](uint8_t *, uint64_t tick)
                             {
                                 if (tick % 600u == 0u)
                                     write();
                             });
        ps2_game_overrides::addShutdownHook(&atExit);
    }
}
