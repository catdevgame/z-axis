// Lane pokes and the lane watch (runtime/lane_watch.h): wlane.py's EE-side tools.

#include "runtime/lane_watch.h"

#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>
#if !defined(_WIN32)
#include <dlfcn.h> // (a caller's function by name in the report; Windows prints its address)
#endif

namespace ps2x::lanes
{
    bool g_watchOn = false;
    bool g_pokesOn = false;
    bool g_callersOn = false;
    bool g_diffOn = false;

    namespace
    {
        uint64_t g_tick = 0;

        // ---- the watch ----
        struct Hits
        {
            uint64_t count = 0, first = 0, last = 0;
        };
        std::mutex g_mutex;
        std::unordered_set<uint32_t> g_words;                   // watched word addresses (& ~3)
        std::unordered_set<uint32_t> g_pcs;                     // PS2X_LANE_WATCH_PCS: load and store sites whose addresses are wanted
        std::vector<bool> g_pages;                               // a page (4 KB) with a watched word
        std::map<std::tuple<char, uint64_t, uint32_t, uint32_t>, Hits> g_hits; // (kind, pc or caller, word, size)

        uint64_t g_reported = 0; // hits counted at the last report (the runner ends with _Exit: no atexit)

        void report()
        {
            const char *path = std::getenv("PS2X_LANE_WATCH_OUT");
            std::FILE *out = path && path[0] ? std::fopen(path, "w") : stderr;
            if (!out)
                out = stderr;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_reported = 0;
            for (const auto &[key, hits] : g_hits)
                g_reported += hits.count;
            for (const auto &[key, hits] : g_hits)
            {
                const auto &[kind, who, word, size] = key;
                if (kind == 'A')
                {
#if !defined(_WIN32)
                    Dl_info info{};
                    const void *caller = reinterpret_cast<const void *>(static_cast<uintptr_t>(who));
                    if (dladdr(caller, &info) && info.dli_sname)
                        std::fprintf(out, "A fn=%s+0x%llx", info.dli_sname,
                                     static_cast<unsigned long long>(static_cast<uintptr_t>(who) - reinterpret_cast<uintptr_t>(info.dli_saddr)));
                    else
#endif
                        std::fprintf(out, "A fn=0x%llx", static_cast<unsigned long long>(who));
                }
                else
                    std::fprintf(out, "%c pc=0x%llx", kind, static_cast<unsigned long long>(who));
                std::fprintf(out, " at=0x%x size=%u n=%llu first=%llu last=%llu\n", word, size,
                             static_cast<unsigned long long>(hits.count), static_cast<unsigned long long>(hits.first),
                             static_cast<unsigned long long>(hits.last));
            }
            if (out != stderr)
                std::fclose(out);
        }

        void installWatch()
        {
            const char *text = std::getenv("PS2X_LANE_WATCH");
            if (!text || !text[0])
                return;
            g_pages.assign(1u << 20, false); // 4 GB of pages: the mirrors too
            std::istringstream items(text);
            std::string item;
            uint32_t words = 0;
            while (std::getline(items, item, ','))
            {
                unsigned address = 0, bytes = 4;
                if (std::sscanf(item.c_str(), "%x+%x", &address, &bytes) < 1)
                    continue;
                for (uint32_t a = address & ~3u; a < address + bytes; a += 4u)
                {
                    g_words.insert(a & 0x1FFFFFFCu);
                    g_pages[(a & 0x1FFFFFFFu) >> 12] = true;
                    ++words;
                }
            }
            if (words == 0)
                return;
            g_watchOn = true;
            std::atexit(report);
            std::fprintf(stderr, "[lane-watch] %u words\n", words);
        }

        // PS2X_LANE_WATCH_PCS=PC,...: every access made by the instructions at those addresses, by the address it
        // reached (R/W lines with that pc: the backward step of a slice, from a store site to what it read).
        void installPcs()
        {
            const char *text = std::getenv("PS2X_LANE_WATCH_PCS");
            if (!text || !text[0])
                return;
            std::istringstream items(text);
            std::string item;
            while (std::getline(items, item, ','))
                if (!item.empty())
                    g_pcs.insert(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
            if (g_pcs.empty())
                return;
            if (g_pages.empty())
                g_pages.assign(1u << 20, false);
            if (!g_watchOn)
                std::atexit(report);
            g_watchOn = true;
            std::fprintf(stderr, "[lane-watch] %zu sites\n", g_pcs.size());
        }

        void hit(char kind, uint64_t who, uint32_t address, uint32_t size)
        {
            const uint32_t base = address & 0x1FFFFFFFu;
            if (kind != 'A' && !g_pcs.empty() && g_pcs.count(static_cast<uint32_t>(who)))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                Hits &hits = g_hits[std::make_tuple(kind, who, base & ~3u, size)];
                if (hits.count++ == 0)
                    hits.first = g_tick;
                hits.last = g_tick;
                return;
            }
            if (!g_pages[base >> 12] && !g_pages[(base + size - 1u) >> 12])
                return;
            for (uint32_t a = base & ~3u; a < base + size; a += 4u)
            {
                if (!g_words.count(a))
                    continue;
                std::lock_guard<std::mutex> lock(g_mutex);
                Hits &hits = g_hits[std::make_tuple(kind, who, a, size)];
                if (hits.count++ == 0)
                    hits.first = g_tick;
                hits.last = g_tick;
            }
        }

        // ---- the differential (PS2X_LANE_DIFF) ----
        // Per instruction, in the order the EE runs it: a count, a hash of its addresses (or directions, targets, call
        // sites) and a hash of the bytes it read or left. Instructions below kDiffSpan by address; dispatched callees
        // keyed by their address with kind C.
        struct Diff
        {
            uint64_t count = 0, a = 0, v = 0;
            char kind = 0;
        };
        constexpr uint32_t kDiffSpan = 0x02000000u;           // guest code addresses (32 MB of RAM)
        std::vector<Diff> g_diff;                              // by (pc >> 2)
        std::map<uint32_t, Diff> g_diffCalls;                  // callee -> its call sites in order
        std::vector<uint64_t> g_diffAt;                        // the ticks to report at
        std::string g_diffOut;

        inline uint64_t mix(uint64_t h, uint64_t x)
        {
            h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return h * 0xBF58476D1CE4E5B9ull;
        }

        Diff *diffSlot(uint32_t pc, char kind)
        {
            if (pc >= kDiffSpan)
                return nullptr;
            Diff &d = g_diff[pc >> 2];
            if (!d.kind)
                d.kind = kind;
            else if (d.kind != kind)
                d.kind = 'X'; // (two kinds at one pc: a stub's reads at its caller's pc; still compared)
            return &d;
        }

        void reportDiff(uint64_t tick)
        {
            const std::string path = g_diffOut + ".t" + std::to_string(tick);
            std::FILE *out = std::fopen(path.c_str(), "w");
            if (!out)
                return;
            for (uint32_t i = 0; i < g_diff.size(); ++i)
            {
                const Diff &d = g_diff[i];
                if (d.count)
                    std::fprintf(out, "D pc=0x%x k=%c n=%llu a=%016llx v=%016llx\n", i << 2, d.kind,
                                 static_cast<unsigned long long>(d.count), static_cast<unsigned long long>(d.a),
                                 static_cast<unsigned long long>(d.v));
            }
            for (const auto &[callee, d] : g_diffCalls)
                std::fprintf(out, "D pc=0x%x k=C n=%llu a=%016llx v=0000000000000000\n", callee,
                             static_cast<unsigned long long>(d.count), static_cast<unsigned long long>(d.a));
            std::fclose(out);
        }

        void installDiff()
        {
            const char *text = std::getenv("PS2X_LANE_DIFF");
            if (!text || !text[0])
                return;
            g_diffOut = text;
            if (const char *at = std::getenv("PS2X_LANE_DIFF_AT"))
            {
                std::istringstream items(at);
                std::string item;
                while (std::getline(items, item, ','))
                    if (!item.empty())
                        g_diffAt.push_back(std::strtoull(item.c_str(), nullptr, 10));
            }
            g_diff.assign(kDiffSpan >> 2, Diff{});
            g_diffOn = true;
            g_callersOn = true; // (the calls go through caller())
            std::fprintf(stderr, "[lane-diff] on, %zu report ticks\n", g_diffAt.size());
        }

        // ---- the callers ----
        std::unordered_map<uint64_t, uint64_t> g_callers; // (target << 32 | source) -> count
        bool g_callersGraph = false;                       // PS2X_LANE_CALLERS (g_callersOn is also the differential's)
        size_t g_callersReported = 0;

        void reportCallers()
        {
            const char *path = std::getenv("PS2X_LANE_CALLERS_OUT");
            std::FILE *out = path && path[0] ? std::fopen(path, "w") : nullptr;
            if (!out)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[key, n] : g_callers)
                std::fprintf(out, "C callee=0x%x from=0x%x n=%llu\n", static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key),
                             static_cast<unsigned long long>(n));
            g_callersReported = g_callers.size();
            std::fclose(out);
        }

        void installCallers()
        {
            const char *text = std::getenv("PS2X_LANE_CALLERS");
            if (text && text[0] == '1')
            {
                g_callersOn = g_callersGraph = true;
                std::fprintf(stderr, "[lane-callers] on\n");
            }
        }

        // ---- the pokes ----
        struct Poke
        {
            enum Kind
            {
                Vf,
                Gpr,
                Fpr,
                Slot
            } kind = Vf;
            int reg = 0;      // the register
            int lane = 0;     // Vf: 0..3; Gpr: 0..3; -1: words 2 and 3
            int32_t offset = 0; // Slot: from sp
            uint32_t value = 0;
        };
        struct Item
        {
            enum When
            {
                Call,
                Ret,
                Entry,
                Exit
            } when = Call;
            uint32_t pc = 0;
            std::vector<Poke> pokes;
            uint64_t applied = 0;
        };
        std::vector<Item> g_items;

        const char *const kAbi[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                      "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                      "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

        bool parsePoke(const std::string &word, Poke &out)
        {
            const size_t eq = word.find('=');
            if (eq == std::string::npos)
                return false;
            const std::string what = word.substr(0, eq);
            out.value = static_cast<uint32_t>(std::strtoul(word.c_str() + eq + 1, nullptr, 16));
            if (what.rfind("sp", 0) == 0 && what.size() > 3 && (what[2] == '+' || what[2] == '-'))
            {
                out.kind = Poke::Slot;
                const long off = std::strtol(what.c_str() + 3, nullptr, 16);
                out.offset = static_cast<int32_t>(what[2] == '-' ? -off : off);
                return true;
            }
            if (what.rfind("vf", 0) == 0)
            {
                const size_t dot = what.find('.');
                if (dot == std::string::npos || dot + 1 >= what.size())
                    return false;
                out.kind = Poke::Vf;
                out.reg = std::atoi(what.c_str() + 2);
                const char lane = what[dot + 1];
                out.lane = lane == 'x' ? 0 : lane == 'y' ? 1 : lane == 'z' ? 2 : lane == 'w' ? 3 : -2;
                return out.reg >= 0 && out.reg < 32 && out.lane >= 0;
            }
            if (what[0] == 'f' && what.size() > 1 && std::isdigit(static_cast<unsigned char>(what[1])))
            {
                out.kind = Poke::Fpr;
                out.reg = std::atoi(what.c_str() + 1);
                return out.reg >= 0 && out.reg < 32;
            }
            const size_t dot = what.find('.');
            if (dot == std::string::npos)
                return false;
            const std::string name = what.substr(0, dot), lane = what.substr(dot + 1);
            out.kind = Poke::Gpr;
            out.reg = -1;
            if (name[0] == 'r' && std::isdigit(static_cast<unsigned char>(name[1])))
                out.reg = std::atoi(name.c_str() + 1);
            else
                for (int i = 0; i < 32; ++i)
                    if (name == kAbi[i])
                        out.reg = i;
            out.lane = lane == "hi" ? -1 : std::atoi(lane.c_str());
            return out.reg >= 1 && out.reg < 32 && out.lane >= -1 && out.lane < 4 && (lane == "hi" || std::isdigit(static_cast<unsigned char>(lane[0])));
        }

        void installPokes()
        {
            const char *text = std::getenv("PS2X_LANE_POKE");
            if (!text || !text[0])
                return;
            std::istringstream items(text);
            std::string item;
            while (std::getline(items, item, ';'))
            {
                std::istringstream words(item);
                std::string word;
                Item out;
                bool ok = false;
                while (words >> word)
                {
                    const size_t eq = word.find('=');
                    const std::string key = eq == std::string::npos ? word : word.substr(0, eq);
                    if (key == "call" || key == "ret" || key == "entry" || key == "exit")
                    {
                        out.when = key == "call" ? Item::Call : key == "ret" ? Item::Ret : key == "entry" ? Item::Entry : Item::Exit;
                        out.pc = static_cast<uint32_t>(std::strtoul(word.c_str() + eq + 1, nullptr, 16));
                        ok = out.pc != 0;
                        continue;
                    }
                    Poke poke;
                    if (!parsePoke(word, poke))
                    {
                        std::fprintf(stderr, "PS2X_LANE_POKE: cannot read '%s'\n", word.c_str());
                        std::abort();
                    }
                    out.pokes.push_back(poke);
                }
                if (!ok || out.pokes.empty())
                {
                    if (!item.empty() && item.find_first_not_of(" \t") != std::string::npos)
                    {
                        std::fprintf(stderr, "PS2X_LANE_POKE: item '%s' needs call|ret|entry|exit=PC and a poke\n", item.c_str());
                        std::abort();
                    }
                    continue;
                }
                g_items.push_back(out);
            }
            if (g_items.empty())
                return;
            g_pokesOn = true;
            std::atexit([]()
                        {
                            for (const Item &item : g_items)
                                std::fprintf(stderr, "[lane-poke] %s=0x%x applied %llu times\n",
                                             item.when == Item::Call ? "call" : item.when == Item::Ret ? "ret" : item.when == Item::Entry ? "entry" : "exit",
                                             item.pc, static_cast<unsigned long long>(item.applied)); });
            std::fprintf(stderr, "[lane-poke] %zu items\n", g_items.size());
        }

        void apply(const Poke &poke, R5900Context *ctx, uint8_t *rdram)
        {
            switch (poke.kind)
            {
            case Poke::Vf:
            {
                uint32_t lanes[4];
                std::memcpy(lanes, &ctx->vu0_vf[poke.reg], 16);
                lanes[poke.lane] = poke.value;
                std::memcpy(&ctx->vu0_vf[poke.reg], lanes, 16);
                break;
            }
            case Poke::Gpr:
            {
                uint32_t words[4];
                std::memcpy(words, &ctx->r[poke.reg], 16);
                if (poke.lane < 0)
                    words[2] = words[3] = poke.value;
                else
                    words[poke.lane] = poke.value;
                std::memcpy(&ctx->r[poke.reg], words, 16);
                break;
            }
            case Poke::Fpr:
                std::memcpy(&ctx->f[poke.reg], &poke.value, 4);
                break;
            case Poke::Slot:
            {
                const uint32_t address = (getRegU32(ctx, 29) + static_cast<uint32_t>(poke.offset)) & 0x1FFFFFFCu;
                std::memcpy(rdram + address, &poke.value, 4);
                break;
            }
            }
        }

        const bool g_installed = []()
        {
            installWatch();
            installPcs();
            installPokes();
            installCallers();
            installDiff();
            return true;
        }();
    }

    void watchAccess(uint32_t address, uint32_t size, bool write, uint32_t pc)
    {
        hit(write ? 'W' : 'R', pc, address, size);
    }

    void watchReadable(uint32_t address, uint32_t size)
    {
        hit('A', static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))), address, size);
    }

    void poke(R5900Context *ctx, uint8_t *rdram, uint32_t sourcePc, uint32_t targetPc, Point point)
    {
        for (Item &item : g_items)
        {
            const bool now = point == Point::BeforeCall
                                 ? ((item.when == Item::Call && item.pc == sourcePc) || (item.when == Item::Entry && item.pc == targetPc))
                                 : ((item.when == Item::Ret && item.pc == sourcePc) || (item.when == Item::Exit && item.pc == targetPc));
            if (!now)
                continue;
            for (const Poke &p : item.pokes)
                apply(p, ctx, rdram);
            ++item.applied;
            bool power = item.applied < 100000u;
            for (uint64_t n = item.applied; power && n > 1u; n /= 10u)
                power = n % 10u == 0u;
            if (power)
                std::fprintf(stderr, "[lane-poke] %s=0x%x applied %llu\n",
                             item.when == Item::Call ? "call" : item.when == Item::Ret ? "ret" : item.when == Item::Entry ? "entry" : "exit",
                             item.pc, static_cast<unsigned long long>(item.applied));
        }
    }

    void caller(uint32_t targetPc, uint32_t sourcePc)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_diffOn)
        {
            Diff &d = g_diffCalls[targetPc];
            ++d.count;
            d.a = mix(d.a, sourcePc);
        }
        if (g_callersGraph)
            ++g_callers[(static_cast<uint64_t>(targetPc) << 32) | sourcePc];
    }

    void diffAccess(const uint8_t *rdram, uint32_t address, uint32_t size, bool write, uint32_t pc)
    {
        Diff *d = diffSlot(pc, write ? 'S' : 'L');
        if (!d)
            return;
        ++d->count;
        d->a = mix(d->a, address);
        if (PS2Runtime::isSpecialAddress(address) || !rdram)
            return; // (a hardware register: its address alone)
        uint8_t bytes[16];
        for (uint32_t i = 0; i < size && i < 16u; ++i)
            bytes[i] = rdram[(address + i) & PS2_RAM_MASK];
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, bytes, size < 8u ? size : 8u);
        if (size > 8u)
            std::memcpy(&hi, bytes + 8, size - 8u);
        d->v = mix(mix(d->v, lo), hi);
    }

    bool diffBranch(uint32_t pc, bool taken)
    {
        if (Diff *d = diffSlot(pc, 'B'))
        {
            ++d->count;
            d->a = mix(d->a, taken ? 1u : 2u);
        }
        return taken;
    }

    uint32_t diffJump(uint32_t pc, uint32_t target)
    {
        if (Diff *d = diffSlot(pc, 'J'))
        {
            ++d->count;
            d->a = mix(d->a, target);
        }
        return target;
    }

    void onVsync(uint64_t tick)
    {
        g_tick = tick;
        if (g_diffOn)
            for (const uint64_t at : g_diffAt)
                if (at == tick)
                    reportDiff(tick);
        if (g_callersOn && (tick & 31u) == 0u)
        {
            size_t size;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                size = g_callers.size();
            }
            if (size != g_callersReported)
                reportCallers();
        }
        // The report kept current every 32 vsyncs (the runner stops with _Exit, so atexit never runs).
        if (g_watchOn && (tick & 31u) == 0u)
        {
            uint64_t total = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const auto &[key, hits] : g_hits)
                    total += hits.count;
            }
            if (total != g_reported)
                report();
        }
    }
}
