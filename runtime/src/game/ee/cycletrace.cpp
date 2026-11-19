// PS2X_FNCYCLES=<addr>[,<addr>...]:<file> (or @<list file>:<file>): the EE cycles of a few game
// functions in the running game, call by call, for comparing a readable
// function with its original at the same call (readable sweep follow-up:
// the career's timing with every Tested function on).
//
// Each listed function start in the dispatch table (whatever is installed
// there: the recompiled function or its readable replacement) is wrapped at
// the first vsync. Every call writes one line:
//
//     addr call tick entry_cycle cycles end ra pc
//
// cycles: EE cycles from the entry to the return to its caller; end: `ret`
// when the function returned to its caller in the wrapper (pc == ra at
// entry), `late` when it gave the EE back to the scheduler halfway (a safe
// point, or a suspendable readable function's guest call) and came back to
// ra later through the scheduler (the table slot at ra is wrapped then, so
// cycles still runs to the return), `lost` when it never came back. A late
// return is paired with its call by ra and sp (the callee's sp at its entry,
// which its epilogue restores): calls made from a readable caller all return
// to its fiber's resume slot, so ra alone cannot tell them apart.
//
// PS2X_FNCYCLES_WATCH=<addr>[+<bytes>][,...]:<file> (drafter21): memory
// write-watch. The watched bytes (main RAM, or the scratchpad at
// the scratchpad; 16 bytes unless +bytes says) are compared with what they held
// at every traced function's entry and return (table and direct calls) and
// at every vsync; when they changed since, a line says where it was seen:
//
//     W in|out|din|dout|vsync addr call tick cycle sp ra @watched: old -> new
//
// (in: at a traced call's entry, so the write came before it; out: at its
// return, so the call or something it called wrote it). With every
// default-on function traced this brackets a write to one call. At each
// vsync a line `H tick @addr fnv1a` hashes every range (two runs' watches
// compared tick by tick: where state outside main RAM, the scratchpad,
// first differs).
//
// PS2X_FNCYCLES_VU0=<reg>[,<reg>...]:<file> (abdiff1): the same for VU0's
// vector registers (0 to 31): compared at every traced function's entry and
// return, a change writes
//
//     V in|out|din|dout addr call tick cycle sp ra vf<reg>: old -> new
//
// (the four lanes' bits, x y z w). For a stale lane a stored vector carries
// (a VMULx.xyz keeps the destination's w, an SQC2 stores all four): two runs'
// files show which call first leaves a register different. Direct calls of
// readable functions see the context the caller runs on.

#include "markers.h"
#include "readable.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace game::profile
{
    namespace
    {
        struct Traced
        {
            uint32_t address = 0;
            PS2Runtime::RecompiledFunction function = nullptr;
            uint64_t calls = 0;
        };
        // A call that left its wrapper before returning: its return is
        // caught at ra, when the scheduler dispatches it.
        struct Pending
        {
            uint32_t address = 0, ra = 0, sp = 0;
            uint64_t call = 0, tick = 0, start = 0;
        };
        struct Watch
        {
            uint32_t ra = 0;
            PS2Runtime::RecompiledFunction function = nullptr;
        };
        struct Tracer
        {
            std::unordered_map<uint32_t, Traced> functions;
            std::vector<Pending> pending;
            std::unordered_map<uint32_t, Watch> watches;
            std::FILE *out = nullptr;
            std::FILE *args = nullptr; // PS2X_FNCYCLES_ARGS
            PS2Runtime *runtime = nullptr;
            bool wrapped = false;
        };
        Tracer *g_tracer = nullptr;

        // PS2X_FNCYCLES_FROM=<tick>: no lines for calls before that vsync
        // tick (a trace of many functions over a short window; drafter20).
        uint64_t g_fromTick = 0;

        // PS2X_FNCYCLES_WATCH (see the top).
        struct WatchRange
        {
            uint32_t address = 0, bytes = 16;
            std::vector<uint8_t> last;
        };
        std::vector<WatchRange> g_watchRanges;
        std::FILE *g_watchOut = nullptr;

        // PS2X_FNCYCLES_VU0 (see the top).
        struct WatchVu0
        {
            int reg = 0;
            uint32_t last[4] = {};
        };
        std::vector<WatchVu0> g_watchVu0;
        std::FILE *g_vu0Out = nullptr;

        void vu0Check(const char *where, const R5900Context *ctx, uint32_t address, uint64_t call, uint32_t sp, uint32_t ra)
        {
            if (g_watchVu0.empty() || !ctx)
                return;
            EeScheduler &s = g_tracer->runtime->eeScheduler();
            for (WatchVu0 &w : g_watchVu0)
            {
                uint32_t now[4];
                std::memcpy(now, &ctx->vu0_vf[w.reg], sizeof(now));
                if (std::memcmp(now, w.last, sizeof(now)) == 0)
                    continue;
                const uint64_t tick = s.currentVSyncTick();
                if (tick >= g_fromTick)
                    std::fprintf(g_vu0Out, "V %s %06x %llu %llu %llu %08x %08x vf%d: %08x %08x %08x %08x -> %08x %08x %08x %08x\n",
                                 where, address, static_cast<unsigned long long>(call), static_cast<unsigned long long>(tick),
                                 static_cast<unsigned long long>(s.eeCycle()), sp, ra, w.reg, w.last[0], w.last[1], w.last[2],
                                 w.last[3], now[0], now[1], now[2], now[3]);
                std::memcpy(w.last, now, sizeof(now));
            }
        }

        const uint8_t *watchedBytes(PS2Runtime *runtime, const WatchRange &w)
        {
            const uint32_t a = w.address;
            if (a >= PS2_SCRATCHPAD_BASE && a + w.bytes <= PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
                return runtime->memory().getScratchpad() + (a - PS2_SCRATCHPAD_BASE);
            const uint32_t p = a & 0x1FFFFFFFu;
            if (p + w.bytes > PS2_RAM_SIZE)
                return nullptr;
            return runtime->memory().getRDRAM() + p;
        }

        void hexBytes(std::FILE *out, const uint8_t *bytes, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                std::fprintf(out, "%s%02x", (i && i % 4 == 0) ? " " : "", bytes[i]);
        }

        // Compares the watched bytes with their last copy; a line for each
        // range that changed (only from PS2X_FNCYCLES_FROM on; the copy
        // follows the memory all along).
        void watchCheck(const char *where, uint32_t address, uint64_t call, uint32_t sp, uint32_t ra)
        {
            if (g_watchRanges.empty() || !g_tracer)
                return;
            PS2Runtime *runtime = g_tracer->runtime;
            EeScheduler &s = runtime->eeScheduler();
            for (WatchRange &w : g_watchRanges)
            {
                const uint8_t *now = watchedBytes(runtime, w);
                if (!now || std::memcmp(now, w.last.data(), w.bytes) == 0)
                    continue;
                const uint64_t tick = s.currentVSyncTick();
                if (tick >= g_fromTick)
                {
                    std::fprintf(g_watchOut, "W %s %06x %llu %llu %llu %08x %08x @%08x: ", where, address,
                                 static_cast<unsigned long long>(call), static_cast<unsigned long long>(tick),
                                 static_cast<unsigned long long>(s.eeCycle()), sp, ra, w.address);
                    if (w.bytes <= 32)
                    {
                        hexBytes(g_watchOut, w.last.data(), w.bytes);
                        std::fputs(" -> ", g_watchOut);
                        hexBytes(g_watchOut, now, w.bytes);
                    }
                    else
                    {
                        // A large range: how many bytes changed, and the 16
                        // (aligned) around the first.
                        uint32_t first = w.bytes, count = 0;
                        for (uint32_t i = 0; i < w.bytes; ++i)
                            if (now[i] != w.last[i])
                            {
                                first = std::min(first, i);
                                ++count;
                            }
                        const uint32_t at = std::min(first & ~15u, w.bytes - 16u);
                        std::fprintf(g_watchOut, "%u bytes, first +0x%x; +0x%x: ", count, first, at);
                        hexBytes(g_watchOut, w.last.data() + at, 16);
                        std::fputs(" -> ", g_watchOut);
                        hexBytes(g_watchOut, now + at, 16);
                    }
                    std::fputc('\n', g_watchOut);
                }
                std::memcpy(w.last.data(), now, w.bytes);
            }
        }

        void line(const Pending &p, uint64_t spent, const char *end, uint32_t pc)
        {
            if (p.tick < g_fromTick)
                return;
            std::fprintf(g_tracer->out, "%06x %llu %llu %llu %llu %s %06x %06x\n", p.address,
                         static_cast<unsigned long long>(p.call), static_cast<unsigned long long>(p.tick),
                         static_cast<unsigned long long>(p.start), static_cast<unsigned long long>(spent), end, p.ra, pc);
        }

        // PS2X_FNCYCLES_ARGS=<file>: a line a table call at its entry (address,
        // call number, tick, entry cycle, a0 to a3, for a0 and a1 that
        // point into RAM the first 40 bytes there as text (`.` for a byte
        // outside printable ASCII, the text ends at a 0) and the first 16 in
        // hex (what a word-wise string function reads past the 0), then sp
        // and ra), for finding the call
        // whose data differs between two runs (drafter20).
        void argsLine(const uint8_t *rdram, const R5900Context *ctx, uint32_t address, uint64_t call,
                      uint64_t tick, uint64_t start)
        {
            std::FILE *out = g_tracer->args;
            const uint32_t a[4] = {getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)};
            std::fprintf(out, "%06x %llu %llu %llu %08x %08x %08x %08x", address, static_cast<unsigned long long>(call),
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(start), a[0], a[1],
                         a[2], a[3]);
            for (int i = 0; i < 2; ++i)
            {
                const uint32_t p = a[i] & 0x1FFFFFFFu;
                if (p < (1u << 20) || p >= 0x2000000u - 40u)
                {
                    std::fputs(" -", out);
                    continue;
                }
                char text[41];
                int n = 0;
                for (; n < 40 && rdram[p + n]; ++n)
                    text[n] = (rdram[p + n] >= 0x20 && rdram[p + n] < 0x7F && rdram[p + n] != ' ') ? static_cast<char>(rdram[p + n]) : '.';
                text[n] = 0;
                std::fprintf(out, " '%s' ", text);
                for (int k = 0; k < 16; ++k)
                    std::fprintf(out, "%02x", rdram[p + k]);
            }
            std::fprintf(out, " sp=%08x ra=%08x\n", getRegU32(ctx, 29), getRegU32(ctx, 31));
        }

        void returned(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Tracer &t = *g_tracer;
            const uint32_t pc = ctx->pc;
            const uint32_t sp = getRegU32(ctx, 29);
            for (size_t i = t.pending.size(); i-- > 0;)
                if (t.pending[i].ra == pc && t.pending[i].sp == sp)
                {
                    line(t.pending[i], runtime->eeScheduler().eeCycle() - t.pending[i].start, "late", pc);
                    t.pending.erase(t.pending.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            if (const auto w = t.watches.find(pc); w != t.watches.end())
                w->second.function(rdram, ctx, runtime);
        }

        void watchReturn(PS2Runtime *runtime, uint32_t ra)
        {
            Tracer &t = *g_tracer;
            if (t.watches.count(ra) || !runtime->hasFunction(ra))
                return;
            t.watches[ra] = Watch{ra, runtime->lookupFunction(ra)};
            runtime->replaceFunction(ra, &returned);
        }

        void traced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Tracer &t = *g_tracer;
            const uint32_t pc = ctx->pc;
            const auto it = t.functions.find(pc);
            if (it == t.functions.end())
                return; // only the listed starts point here
            Traced *f = &it->second;
            EeScheduler &s = runtime->eeScheduler();
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t sp = getRegU32(ctx, 29);
            const uint64_t tick = s.currentVSyncTick();
            const uint64_t start = s.eeCycle();
            const uint64_t call = f->calls++;
            if (t.args && tick >= g_fromTick)
                argsLine(rdram, ctx, pc, call, tick, start);
            watchCheck("in", pc, call, sp, ra);
            vu0Check("in", ctx, pc, call, sp, ra);
            f->function(rdram, ctx, runtime);
            watchCheck("out", pc, call, sp, ra);
            vu0Check("out", ctx, pc, call, sp, ra);
            const Pending p{pc, ra, sp, call, tick, start};
            if (t.args && ctx->pc == ra && tick >= g_fromTick)
            {
                // (PS2X_FNCYCLES_ARGS: the registers it returned with, GPRs'
                // low words and FPRs' bits, for a caller reading a temporary.)
                std::fprintf(t.args, "%06x %llu ret", pc, static_cast<unsigned long long>(call));
                for (int r = 1; r < 32; ++r)
                    std::fprintf(t.args, " %x", getRegU32(ctx, r));
                for (int f = 0; f < 32; ++f)
                {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &ctx->f[f], 4);
                    std::fprintf(t.args, " %x", bits);
                }
                std::fputc('\n', t.args);
            }
            if (ctx->pc == ra)
            {
                line(p, s.eeCycle() - start, "ret", ctx->pc);
                return;
            }
            t.pending.push_back(p);
            watchReturn(runtime, ra);
        }

        // game::call's and game::tailCall's direct calls of a listed readable
        // function: the same call count and line (ra and pc 0: no return
        // address in the table; `late` when its fiber left and was resumed
        // meanwhile).
        struct DirectPending
        {
            Pending p;
            uint64_t resumes, returns;
        };
        std::vector<DirectPending> g_direct;
        uint64_t directBegin(uint32_t address)
        {
            Tracer &t = *g_tracer;
            const auto it = t.functions.find(address);
            if (it == t.functions.end() || !t.wrapped)
                return 0;
            EeScheduler &s = t.runtime->eeScheduler();
            const uint32_t top = ee::t_ee.frame ? ee::t_ee.frame->stackTop : 0u;
            watchCheck("din", address, it->second.calls, top, 0u);
            vu0Check("din", ee::t_ee.frame ? ee::t_ee.frame->ctx : nullptr, address, it->second.calls, top, 0u);
            if (t.args && s.currentVSyncTick() >= g_fromTick)
            {
                // (PS2X_FNCYCLES_ARGS: a direct call's line has the callee's
                // stack top, where its frame goes: the original's sp at entry
                // when every readable caller's StackFrame is the original's.)
                const R5900Context *ctx = ee::t_ee.frame ? ee::t_ee.frame->ctx : nullptr;
                std::fprintf(t.args, "%06x %llu %llu %llu direct sp=%08x ctxsp=%08x\n", address,
                             static_cast<unsigned long long>(it->second.calls),
                             static_cast<unsigned long long>(s.currentVSyncTick()),
                             static_cast<unsigned long long>(s.eeCycle()), top,
                             ctx ? getRegU32(ctx, 29) : 0u);
            }
            // (From after the caller's dispatch, as a table call's wrapper.)
            g_direct.push_back(DirectPending{Pending{address, 0, 0, it->second.calls++, s.currentVSyncTick(),
                                                     s.eeCycle()},
                                             ee::fiberResumes(), ee::directReturnDispatches()});
            return g_direct.size();
        }
        void directEnd(uint64_t token)
        {
            if (token == 0 || token != g_direct.size())
                return;
            const DirectPending d = g_direct.back();
            g_direct.pop_back();
            watchCheck("dout", d.p.address, d.p.call, ee::t_ee.frame ? ee::t_ee.frame->stackTop : 0u, 0u);
            vu0Check("dout", ee::t_ee.frame ? ee::t_ee.frame->ctx : nullptr, d.p.address, d.p.call, ee::t_ee.frame ? ee::t_ee.frame->stackTop : 0u, 0u);
            const bool late = ee::fiberResumes() != d.resumes;
            uint64_t spent = g_tracer->runtime->eeScheduler().eeCycle() - d.p.start;
            // A callee that did not unwind but returns through the scheduler
            // (the caller's jal check yielded, so the scheduler dispatched
            // it): its line ends at its return, before that dispatch, as the
            // table wrapper's does (nothing in it unwound, so the return
            // charged meanwhile is its own call's).
            if (!late && ee::directReturnDispatches() != d.returns && spent >= EeScheduler::kGuestDispatchCycles)
                spent -= EeScheduler::kGuestDispatchCycles;
            line(d.p, spent, late ? "late" : "ret", 0);
        }
        const DirectCallTrace g_directTrace{&directBegin, &directEnd};

        void watchHashes()
        {
            if (g_watchRanges.empty())
                return;
            const uint64_t tick = g_tracer->runtime->eeScheduler().currentVSyncTick();
            if (tick < g_fromTick)
                return;
            for (const WatchRange &w : g_watchRanges)
            {
                const uint8_t *now = watchedBytes(g_tracer->runtime, w);
                uint64_t h = 0xCBF29CE484222325ull;
                for (uint32_t i = 0; now && i < w.bytes; ++i)
                    h = (h ^ now[i]) * 0x100000001B3ull;
                std::fprintf(g_watchOut, "H %llu @%08x %016llx\n", static_cast<unsigned long long>(tick), w.address,
                             static_cast<unsigned long long>(h));
            }
        }

        void onVsync(uint64_t)
        {
            Tracer &t = *g_tracer;
            watchCheck("vsync", 0, 0, 0, 0);
            watchHashes();
            if (t.wrapped)
                return;
            t.wrapped = true;
            for (auto &[address, f] : t.functions)
            {
                if (!t.runtime->hasFunction(f.address))
                    continue;
                f.function = t.runtime->lookupFunction(f.address);
                t.runtime->replaceFunction(f.address, &traced);
            }
        }

        void atExit(PS2Runtime &)
        {
            if (!g_tracer || !g_tracer->out)
                return;
            for (const Pending &p : g_tracer->pending)
                line(p, 0, "lost", 0);
            std::fflush(g_tracer->out);
            if (g_tracer->args)
                std::fflush(g_tracer->args);
            if (g_watchOut)
                std::fflush(g_watchOut);
            if (g_vu0Out)
                std::fflush(g_vu0Out);
        }
    }

    void installCycleTrace(PS2Runtime &runtime)
    {
        const char *spec = std::getenv("PS2X_FNCYCLES");
        if (!spec || !*spec)
            return;
        const char *colon = std::strrchr(spec, ':');
        if (!colon)
            return;
        static Tracer tracer;
        // The list: hex addresses, comma-separated, or @file (one a line).
        std::string list(spec, colon);
        if (!list.empty() && list[0] == '@')
        {
            std::ifstream in(list.substr(1));
            std::string text, row;
            while (std::getline(in, row))
                if (!row.empty() && row[0] != '#')
                    text += row.substr(0, row.find_first_of(" \t")) + ",";
            list = text;
        }
        for (size_t at = 0; at < list.size();)
        {
            size_t comma = list.find(',', at);
            if (comma == std::string::npos)
                comma = list.size();
            Traced f;
            f.address = static_cast<uint32_t>(std::strtoul(list.substr(at, comma - at).c_str(), nullptr, 16));
            if (f.address)
                tracer.functions[f.address] = f;
            at = comma + 1;
        }
        tracer.out = std::fopen(colon + 1, "w");
        if (!tracer.out)
            return;
        // Runs end by a kill (STOP_TICK): a few functions line by line, many in
        // blocks (the last block of a killed run is lost).
        std::setvbuf(tracer.out, nullptr, tracer.functions.size() > 16 ? _IOFBF : _IOLBF, 1u << 20);
        if (const char *from = std::getenv("PS2X_FNCYCLES_FROM"); from && *from)
            g_fromTick = std::strtoull(from, nullptr, 0);
        if (const char *args = std::getenv("PS2X_FNCYCLES_ARGS"); args && *args)
            if ((tracer.args = std::fopen(args, "w")))
                std::setvbuf(tracer.args, nullptr, tracer.functions.size() > 16 ? _IOFBF : _IOLBF, 1u << 20);
        tracer.runtime = &runtime;
        g_tracer = &tracer;
        if (const char *watch = std::getenv("PS2X_FNCYCLES_WATCH"); watch && *watch)
        {
            // <addr>[+<bytes>][,...]:<file>
            const char *wcolon = std::strrchr(watch, ':');
            if (wcolon && (g_watchOut = std::fopen(wcolon + 1, "w")))
            {
                std::setvbuf(g_watchOut, nullptr, _IOLBF, 1u << 16);
                const std::string wl(watch, wcolon);
                for (size_t at = 0; at < wl.size();)
                {
                    size_t comma = wl.find(',', at);
                    if (comma == std::string::npos)
                        comma = wl.size();
                    const std::string item = wl.substr(at, comma - at);
                    WatchRange w;
                    char *end = nullptr;
                    w.address = static_cast<uint32_t>(std::strtoul(item.c_str(), &end, 16));
                    if (end && *end == '+')
                        w.bytes = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0));
                    w.bytes = std::max(16u, std::min(w.bytes, PS2_SCRATCHPAD_SIZE));
                    w.last.resize(w.bytes);
                    if (const uint8_t *now = watchedBytes(&runtime, w))
                    {
                        std::memcpy(w.last.data(), now, w.bytes);
                        g_watchRanges.push_back(std::move(w));
                    }
                    at = comma + 1;
                }
            }
        }
        if (const char *vu0 = std::getenv("PS2X_FNCYCLES_VU0"); vu0 && *vu0)
        {
            // <reg>[,<reg>...]:<file>
            const char *vcolon = std::strrchr(vu0, ':');
            if (vcolon && (g_vu0Out = std::fopen(vcolon + 1, "w")))
            {
                std::setvbuf(g_vu0Out, nullptr, _IOFBF, 1u << 20);
                for (const char *at = vu0; at < vcolon;)
                {
                    char *end = nullptr;
                    const long reg = std::strtol(at, &end, 10);
                    if (end == at)
                        break;
                    if (reg >= 0 && reg < 32)
                    {
                        WatchVu0 w;
                        w.reg = static_cast<int>(reg);
                        g_watchVu0.push_back(w);
                    }
                    at = *end == ',' ? end + 1 : vcolon;
                }
            }
        }
        g_directCallTrace = &g_directTrace;
        runtime.addVSyncHook([](uint8_t *, uint64_t tick) { onVsync(tick); });
        ps2_game_overrides::addShutdownHook(&atExit);
    }
}
