#include "held.h"

#include "abi.h"
#include "ee.h"
#include "held_program.h"
#include "ee_instructions.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

namespace ee::held
{
    static_assert(kSlots == kSavedSlots, "held::State keeps one value per saved slot");

    namespace
    {
        Table &table()
        {
            static Table t;
            return t;
        }

        // The slots the table speaks of: s0-s7, fp, f20-f31 (not gp, not ra); f20's the first of the FPRs'.
        constexpr uint32_t kHeldSlots = 0xFFu | (1u << 9) | (0xFFFu << 11);
        constexpr int kFirstFloatSlot = 11;
        static_assert(savedSlotReg(kFirstFloatSlot) == 32 + 20 && savedSlotReg(kFirstFloatSlot - 1) < 32);

        using program::bitsOf;

        uint32_t returnAddressSlot() { return 1u << savedSlot(reg::ra); }

        // ZX_READABLE_HELD_TRACE=1: each call's place on stderr (the function, the callee, the
        // site it was taken for, the slots given and those not known, the callee's sp, the values
        // given, then the entry values handed on as e<slot>); =LO-HI[,LO-HI...]: the same for the functions in
        // those ranges only; =count: how many
        // calls were placed and not, at exit.
        const char *traceMode()
        {
            static const char *const value = std::getenv("ZX_READABLE_HELD_TRACE");
            return value;
        }
        bool tracing()
        {
            static const bool value = traceMode() && std::strcmp(traceMode(), "count") != 0;
            return value;
        }
        // The functions ZX_READABLE_HELD_TRACE names: =LO-HI[,LO-HI...], any of the ranges (all without one).
        bool tracedHere(uint32_t address)
        {
            if (!tracing())
                return false; // (every call asks: the ranges are read only for a trace)
            static const std::vector<std::pair<uint32_t, uint32_t>> ranges = []
            {
                std::vector<std::pair<uint32_t, uint32_t>> list;
                const char *v = traceMode();
                while (v && *v)
                {
                    char *end = nullptr;
                    const uint32_t lo = static_cast<uint32_t>(std::strtoul(v, &end, 0));
                    if (!end || *end != '-')
                        break;
                    list.emplace_back(lo, static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)));
                    v = *end == ',' ? end + 1 : nullptr;
                }
                if (list.empty())
                    list.emplace_back(0u, ~0u);
                return list;
            }();
            return std::any_of(ranges.begin(), ranges.end(), [address](const auto &r)
                                            { return address >= r.first && address < r.second; });
        }
        // ZX_READABLE_HELD_TRACE=count (read once).
        bool counting()
        {
            static const bool value = traceMode() && std::strcmp(traceMode(), "count") == 0;
            return value;
        }
        struct Counts
        {
            uint64_t placed = 0, lost = 0, entries = 0, entryLoads = 0;
            uint64_t indexed = 0, searched = 0; // (calls after one place or none, from its successors; the others)
            void print() const
            {
                std::fprintf(stderr, "[held] calls placed %llu, not placed %llu; entries with entry loads %llu, entry loads "
                             "read %llu; calls after one place %llu, after several (searched) %llu\n",
                             static_cast<unsigned long long>(placed), static_cast<unsigned long long>(lost),
                             static_cast<unsigned long long>(entries), static_cast<unsigned long long>(entryLoads),
                             static_cast<unsigned long long>(indexed), static_cast<unsigned long long>(searched));
            }
            ~Counts()
            {
                if (traceMode() && std::strcmp(traceMode(), "count") == 0)
                    print();
            }
        };
        Counts g_counts;
    }

    namespace
    {
        // Per function, how many of its sites are jalrs (target 0: they come
        // first): beforeCall skips them as a block, instead of scanning them
        // at every call.
        std::vector<uint32_t> g_indirectSites;

        // The calls that can come after one place of a function (one site of its last call, its entry, or none
        // placed: kLost), worked out once from the table: per such place, each site of the function with a case
        // after it, in the function's order of sites (by target, jalrs first), with that case. What beforeCall's
        // search of the sites finds for a call of a target after one place is the place's jalrs, then its entries
        // of that target, in this order. (A function's places are its entry, kLost and its sites' addresses.)
        struct Next
        {
            uint32_t target; // the site's (0: a jalr)
            uint32_t site, caseIndex; // the table's
            bool needsProof; // the case has checks and a kChecked value (alone, its checks prove it)
            // What a call placed here alone needs of the site and the case (beforeCall's common call reads no more):
            bool fromLost;        // the case is kLost's (only its own values hold after it)
            bool noValues;        // the case gives no values (what the last call had holds)
            uint32_t siteAddress; // the site's jal (the place a call there leaves)
            uint32_t after;       // the successors of that place (After index)
            program::CaseValuesCode values; // the case's values as code (null: none, its rows are worked through)
        };
        struct After
        {
            uint32_t first = 0, count = 0; // Successors::next [first, first + count)
            uint32_t jalrs = 0;            // the first ones, the jalrs'
        };
        struct Successors
        {
            bool on = false;                  // (off: the table's sites are not in the order beforeCall searches)
            std::vector<Next> next;
            std::vector<After> after;         // [0]: none
            std::vector<uint32_t> ofEntry, ofLost; // per function, its entry's and kLost's (After index)
            std::vector<uint32_t> ofSite;      // per site, the place of its address (After index)
        };
        Successors g_successors;

        // What a call can be (beforeCall): a site and its case after the last call's place.
        struct Pick
        {
            const Site *site;
            const Case *c;
        };
        constexpr int kPicks = 64;
        struct Picks
        {
            Pick pick[kPicks];
            int count = 0;
            bool overflow = false;   // more than kPicks
            bool needsProof = false; // one, whose case has checks and a kChecked value
        };
        void checkSuccessors(const Table &t);
        const Case *caseOf(const Site &site, uint32_t prev);

        void workOutSuccessors(const Table &t)
        {
            Successors &out = g_successors;
            out = Successors{};
            for (uint32_t i = 0; i < t.count; ++i)
            {
                const Function &fn = t.functions[i];
                for (uint32_t j = fn.firstSite; j + 1 < fn.firstSite + fn.siteCount; ++j)
                    if (t.sites[j].target > t.sites[j + 1].target)
                        return; // (not sorted: beforeCall searches as it is)
                for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                    for (uint32_t k = t.sites[j].firstCase; k + 1 < t.sites[j].firstCase + t.sites[j].caseCount; ++k)
                        if (t.cases[k].prev > t.cases[k + 1].prev)
                            return;
            }
            out.after.emplace_back();
            out.ofEntry.assign(t.count, 0u);
            out.ofLost.assign(t.count, 0u);
            uint32_t sites = 0;
            for (uint32_t i = 0; i < t.count; ++i)
                sites = std::max(sites, t.functions[i].firstSite + t.functions[i].siteCount);
            out.ofSite.assign(sites, 0u);
            struct Item
            {
                uint32_t prev;
                Next next;
            };
            std::vector<Item> items;
            for (uint32_t i = 0; i < t.count; ++i)
            {
                const Function &fn = t.functions[i];
                items.clear();
                for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                {
                    const Site &site = t.sites[j];
                    for (uint32_t k = site.firstCase; k < site.firstCase + site.caseCount; ++k)
                    {
                        const Case &c = t.cases[k];
                        if (k != site.firstCase && t.cases[k - 1].prev == c.prev)
                            continue; // (caseOf takes the first)
                        bool needsProof = false;
                        if (c.checkCount)
                            for (uint32_t v = 0; v < c.valueCount && !needsProof; ++v)
                                needsProof = (t.values[c.firstValue + v].slot & kChecked) != 0;
                        items.push_back(Item{c.prev, Next{site.target, j, k, needsProof, c.prev == kLost, c.valueCount == 0,
                                                          site.site, 0u,
                                                          c.valuesCode && t.caseValues ? t.caseValues[c.valuesCode] : nullptr}});
                    }
                }
                // (by place, each place's in the order of sites; a site has one case after a place, caseOf)
                std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.prev < b.prev; });
                for (size_t from = 0; from < items.size();)
                {
                    size_t to = from;
                    After a;
                    a.first = static_cast<uint32_t>(out.next.size());
                    for (; to < items.size() && items[to].prev == items[from].prev; ++to)
                    {
                        if (items[to].next.target == 0u)
                            ++a.jalrs;
                        out.next.push_back(items[to].next);
                    }
                    a.count = static_cast<uint32_t>(to - from);
                    const uint32_t index = static_cast<uint32_t>(out.after.size());
                    out.after.push_back(a);
                    const uint32_t prev = items[from].prev;
                    if (prev == kFromEntry)
                        out.ofEntry[i] = index;
                    else if (prev == kLost)
                        out.ofLost[i] = index;
                    else
                        for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                            if (t.sites[j].site == prev)
                                out.ofSite[j] = index;
                    from = to;
                }
            }
            for (Next &next : out.next)
                next.after = out.ofSite[next.site];
            out.on = true;
        }
    }

    Registration::Registration(const Table &t)
    {
        table() = t;
        g_indirectSites.assign(t.count, 0u);
        for (uint32_t i = 0; i < t.count; ++i)
        {
            const Function &fn = t.functions[i];
            uint32_t n = 0;
            while (n < fn.siteCount && t.sites[fn.firstSite + n].target == 0u)
                ++n;
            g_indirectSites[i] = n;
        }
        workOutSuccessors(t);
        if (const char *v = std::getenv("ZX_READABLE_HELD_SUCCESSORS"); v && std::strcmp(v, "check") == 0)
            checkSuccessors(t);
    }

    const Function *find(uint32_t game, uint32_t address)
    {
        const Table &t = table();
        const Function *end = t.functions + t.count;
        const Function *it = std::lower_bound(t.functions, end, std::make_pair(game, address),
                                              [](const Function &f, const std::pair<uint32_t, uint32_t> &key)
                                              { return f.game != key.first ? f.game < key.first : f.address < key.second; });
        return it != end && it->game == game && it->address == address ? it : nullptr;
    }

    bool wanted()
    {
        static const bool value = []
        {
            const char *v = std::getenv("ZX_READABLE_HELD");
            return !(v && v[0] == '0') && framesOn();
        }();
        return value;
    }

    bool on() { return (bootSwitches() & kBootHeldOn) != 0u; }

    namespace
    {
        // placeAs's pairs (a handful: set at boot, read at every call once one is set).
        struct StandIn
        {
            uint32_t game, target, standsFor;
        };
        std::vector<StandIn> g_standIns;
        std::atomic<bool> g_anyStandIn{false};

        bool hasSiteOf(const Table &t, const Function &fn, uint32_t target)
        {
            const Site *first = t.sites + fn.firstSite, *last = first + fn.siteCount;
            const Site *it = std::lower_bound(first, last, target, [](const Site &x, uint32_t v) { return x.target < v; });
            return it != last && it->target == target;
        }

        // The target a call is placed as: its own, or the one it stands for where the original has no site of it.
        HELD_INLINE uint32_t placedTarget(const Table &t, const Function &fn, uint32_t target)
        {
            if (!g_anyStandIn.load(std::memory_order_relaxed) || target == 0u)
                return target;
            for (const StandIn &s : g_standIns)
                if (s.target == target && s.game == fn.game && !hasSiteOf(t, fn, target) && hasSiteOf(t, fn, s.standsFor))
                    return s.standsFor;
            return target;
        }
    }

    void placeAs(uint32_t game, uint32_t target, uint32_t standsFor)
    {
        target &= 0x1FFFFFFFu;
        standsFor &= 0x1FFFFFFFu;
        for (const StandIn &s : g_standIns)
            if (s.game == game && s.target == target && s.standsFor == standsFor)
                return;
        g_standIns.push_back(StandIn{game, target, standsFor});
        g_anyStandIn.store(true);
    }

    HELD_NOINLINE bool program::load(const Context &c, uint32_t kind, uint32_t address, uint64_t &v)
    {
        if (!c.loads)
            return false;
        // (a word of the function's own frame: known only inside one of its ee::Local objects, see State)
        const Frame &frame = c.frame;
        if (frame.fnSp && frame.layout && frame.layout->size && address < frame.fnSp &&
            address >= frame.fnSp - frame.layout->size)
        {
            const uint32_t offset = address - (frame.fnSp - frame.layout->size);
            const uint32_t width = kind == kLb || kind == kLbu ? 1u : kind == kLh || kind == kLhu ? 2u
                                 : kind == kLd ? 8u : kind == kLq ? 16u : 4u;
            bool inLocal = false;
            for (uint32_t i = 0; i < c.state.localRanges && !inLocal; ++i)
                inLocal = offset >= c.state.localFrom[i] && offset + width <= c.state.localTo[i];
            if (!inLocal)
                return false;
        }
        // (a check's load in a replay: not read, and the check not known, see held.h)
        if (c.unreadable && c.replaying)
            return false;
        bool *const unreadable = c.unreadable;
        const auto readMemory = [unreadable](uint32_t at, uint32_t bytes, uint64_t &value)
        {
            const bool ok = program::readMemory(at, bytes, value);
            if (!ok && unreadable)
                *unreadable = true;
            return ok;
        };
        uint64_t raw = 0;
        switch (kind)
        {
        case kLb: if (!readMemory(address, 1, raw)) return false; v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw))); return true;
        case kLbu: if (!readMemory(address, 1, raw)) return false; v = raw; return true;
        case kLh: if (!readMemory(address, 2, raw)) return false; v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw))); return true;
        case kLhu: if (!readMemory(address, 2, raw)) return false; v = raw; return true;
        case kLw: if (!readMemory(address, 4, raw)) return false; v = program::sx32(raw); return true;
        case kLwu: case kLwc1: if (!readMemory(address, 4, raw)) return false; v = raw; return true;
        case kLd: if (!readMemory(address, 8, raw)) return false; v = raw; return true;
        case kLq: if (!readMemory(address & ~15u, 8, raw)) return false; v = raw; return true;
        default: return false;
        }
    }

    namespace
    {
        // Where a program runs: inside a loop program (kLoop) the values it carries, which kCarried reads (null
        // outside one), and how many loops deep; whether a recording answers the calls (zx-fncheck).
        struct Scope
        {
            const uint64_t *carried = nullptr;
            uint32_t loopDepth = 0;
            bool replaying = false;
        };

        constexpr uint32_t kDepth = 32; // (a program's stack)

        // A node that takes nothing from the stack (a leaf of the expression: what the runtime has at the call):
        // its value, false when it is not known (held_program.h). Most values are one of these alone (beforeCall
        // works those out without running a program).
        inline bool isLeaf(uint8_t op)
        {
            return op == kConst || op == kEntry || op == kPrevious || op == kResult || op == kArgument ||
                   op == kFloatArgument || op == kTarget || op == kEntryLoad || op == kReturnLoad || op == kCarried;
        }
        HELD_INLINE bool leafValue(const Frame &frame, const State &state, const CallRegs &regs, const Node &n,
                              const Scope &scope, uint64_t &v)
        {
            const program::Context c{frame, state, regs, true, nullptr, scope.replaying};
            switch (n.op)
            {
            case kConst: v = program::constant(n.imm); return true;
            case kCarried:
                if (!scope.carried || n.sub >= kCarriedValues)
                    return false;
                v = scope.carried[n.sub];
                return true;
            case kEntry: return program::entry(c, n.sub, v);
            case kPrevious: return program::previous(c, n.sub, v);
            case kResult: return program::result(c, n.sub, v);
            case kArgument: return program::argument(c, n.sub, v);
            case kFloatArgument: return program::floatArgument(c, n.sub, v);
            case kTarget: return program::target(c, v);
            case kEntryLoad: return program::entryLoad(c, n.sub, v);
            case kReturnLoad: return program::returnLoad(c, n.sub, v);
            default: return false;
            }
        }

        bool run(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes, uint32_t count,
                 uint64_t *stack, uint32_t &depth, bool loads, bool *unreadable, const Scope &scope);
        // The kLoop at nodes[i] (see held.h): its result into v, i to the program's last node. Out of run, whose
        // other nodes then need no loop's stacks in their frame.
        HELD_NOINLINE bool runLoop(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes,
                                   uint32_t count, uint32_t &i, uint64_t *stack, uint32_t &depth, bool loads,
                                   bool *unreadable, const Scope &scope, uint64_t &v);
    }

    bool evaluate(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes, uint32_t count,
                  uint64_t &out, bool loads, bool *unreadable, bool replaying)
    {
        uint64_t stack[kDepth];
        uint32_t depth = 0;
        if (!run(frame, state, regs, nodes, count, stack, depth, loads, unreadable, Scope{nullptr, 0u, replaying}) ||
            depth != 1)
            return false;
        out = stack[0];
        return true;
    }

    namespace
    {
        // ZX_READABLE_HELD_CODE (default 1): the programs and cases generated as code run (Value::program,
        // Case::valuesCode, checksCode); 0: the table's rows and nodes are worked through instead (what the code
        // buys); programs: the programs' code alone; values: and the cases' values' (their checks' rows worked
        // through); check: both at every call, and a value, a known, an unreadable, a fit or a proof the two work out
        // differently said on stderr (the first 20, and how many at exit), and at the first call every program and
        // case of the table run both ways on made-up states and calls (sweepCode).
        enum CodeMode : uint8_t { kInterpret, kCodePrograms, kCodeValues, kCode, kCodeChecked };
        CodeMode codeMode()
        {
            const char *v = std::getenv("ZX_READABLE_HELD_CODE");
            return !v ? kCode : v[0] == '0' ? kInterpret : std::strcmp(v, "check") == 0 ? kCodeChecked
                 : std::strcmp(v, "programs") == 0 ? kCodePrograms : std::strcmp(v, "values") == 0 ? kCodeValues : kCode;
        }
        const CodeMode g_codeMode = codeMode();
        struct CodeChecks
        {
            uint64_t run = 0, differ = 0;
            ~CodeChecks()
            {
                if (g_codeMode == kCodeChecked)
                    std::fprintf(stderr, "[held] code checked against the rows and nodes %llu, differing %llu\n",
                                 static_cast<unsigned long long>(run), static_cast<unsigned long long>(differ));
            }
        };
        CodeChecks g_codeChecks;

        HELD_NOINLINE bool checkedProgram(const Frame &frame, const State &state, const CallRegs &regs, const Value &value,
                                          uint64_t &out, bool loads, bool *unreadable, bool replaying)
        {
            const Table &t = table();
            uint64_t nodesOut = 0, codeOut = 0;
            bool nodesUnreadable = false, codeUnreadable = false;
            const bool nodesKnown = evaluate(frame, state, regs, t.nodes + value.first, value.count, nodesOut, loads,
                                             unreadable ? &nodesUnreadable : nullptr, replaying);
            const bool codeKnown = t.programs[value.program](
                program::Context{frame, state, regs, loads, unreadable ? &codeUnreadable : nullptr, replaying}, codeOut);
            ++g_codeChecks.run;
            if (nodesKnown != codeKnown || (nodesKnown && nodesOut != codeOut) || nodesUnreadable != codeUnreadable)
            {
                if (++g_codeChecks.differ <= 20u)
                    std::fprintf(stderr, "[held] program %u (nodes %u+%u) differs from its nodes: known %d/%d, value "
                                 "0x%llx/0x%llx, unreadable %d/%d\n", value.program, value.first, value.count,
                                 nodesKnown, codeKnown, static_cast<unsigned long long>(nodesOut),
                                 static_cast<unsigned long long>(codeOut), nodesUnreadable, codeUnreadable);
            }
            if (unreadable && nodesUnreadable)
                *unreadable = true;
            out = nodesOut;
            return nodesKnown;
        }

        // evaluate() of a table's value or check: a leaf alone (most of them) worked out in place, a program
        // generated as code run as that.
        HELD_INLINE bool evaluateValue(const Frame &frame, const State &state, const CallRegs &regs, const Value &value,
                                  uint64_t &out, bool loads, bool *unreadable = nullptr, bool replaying = false)
        {
            const Table &t = table();
            const Node *nodes = t.nodes + value.first;
            if (value.count == 1 && isLeaf(nodes[0].op))
                return leafValue(frame, state, regs, nodes[0], Scope{nullptr, 0u, replaying}, out);
            if (value.program && g_codeMode != kInterpret)
            {
                if (g_codeMode == kCodeChecked)
                    return checkedProgram(frame, state, regs, value, out, loads, unreadable, replaying);
                return t.programs[value.program](program::Context{frame, state, regs, loads, unreadable, replaying}, out);
            }
            return evaluate(frame, state, regs, nodes, value.count, out, loads, unreadable, replaying);
        }

    bool runLoop(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes, uint32_t count,
                 uint32_t &i, uint64_t *stack, uint32_t &depth, bool loads, bool *unreadable, const Scope &scope,
                 uint64_t &v)
    {
        const Node &n = nodes[i];
        const uint32_t carriedCount = n.sub;
        const uint32_t length = static_cast<uint32_t>(n.imm);
        if (carriedCount == 0 || carriedCount > static_cast<uint32_t>(kCarriedValues) || depth < carriedCount ||
            n.imm < 1 || i + length >= count || scope.loopDepth >= 2)
            return false;
        uint64_t carried[kCarriedValues];
        for (uint32_t k = carriedCount; k-- > 0;)
            carried[k] = stack[--depth];
        const Node *program = nodes + i + 1;
        const Scope inside{carried, scope.loopDepth + 1u, scope.replaying};
        bool done = false, ok = true;
        for (uint32_t iteration = 0; iteration < kLoopIterations && !done && ok; ++iteration)
        {
            bool taken = false;
            for (uint32_t j = 0; j < length && !taken && ok;)
            {
                const Node &leaf = program[j];
                const uint32_t conditionLength = static_cast<uint32_t>(leaf.imm);
                if (leaf.op != kLeaf || leaf.imm < 1 || j + 1 + conditionLength >= length ||
                    program[j + 1 + conditionLength].op != kLeafEnd)
                {
                    ok = false;
                    break;
                }
                const uint32_t valuesLength = static_cast<uint32_t>(program[j + 1 + conditionLength].imm);
                const uint32_t next = j + 2 + conditionLength + valuesLength;
                if (next > length)
                {
                    ok = false;
                    break;
                }
                uint64_t conditionStack[kDepth];
                uint32_t conditionDepth = 0;
                if (!run(frame, state, regs, program + j + 1, conditionLength, conditionStack, conditionDepth, loads, unreadable, inside) ||
                    conditionDepth != 1)
                {
                    ok = false;
                    break;
                }
                if (conditionStack[0] != 0u)
                {
                    uint64_t valueStack[kDepth];
                    uint32_t valueDepth = 0;
                    if (!run(frame, state, regs, program + j + 2 + conditionLength, valuesLength, valueStack, valueDepth, loads, unreadable, inside))
                    {
                        ok = false;
                        break;
                    }
                    if (leaf.sub == 1)
                    {
                        if (valueDepth != 1)
                        {
                            ok = false;
                            break;
                        }
                        v = valueStack[0];
                        done = true;
                    }
                    else
                    {
                        if (valueDepth != carriedCount)
                        {
                            ok = false;
                            break;
                        }
                        std::memcpy(carried, valueStack, sizeof(uint64_t) * carriedCount);
                    }
                    taken = true;
                }
                j = next;
            }
            if (!taken)
                ok = false; // (no leaf's path: not this loop's iteration as the original ran it)
        }
        if (!ok || !done)
            return false;
        i += length; // (the program was run here)
        return true;
    }

    bool run(const Frame &frame, const State &state, const CallRegs &regs, const Node *nodes, uint32_t count,
             uint64_t *stack, uint32_t &depth, bool loads, bool *unreadable, const Scope &scope)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const Node &n = nodes[i];
            uint64_t v = 0;
            switch (n.op)
            {
            case kLoop:
                if (!runLoop(frame, state, regs, nodes, count, i, stack, depth, loads, unreadable, scope, v))
                    return false;
                break;
            case kLeaf:
            case kLeafEnd:
                return false; // (only inside a kLoop program)
            case kLoad:
                if (depth < 1)
                    return false;
                if (!program::load(program::Context{frame, state, regs, loads, unreadable, scope.replaying}, n.sub,
                                   static_cast<uint32_t>(stack[--depth]) + static_cast<uint32_t>(n.imm), v))
                    return false;
                break;
            case kSkipIf:
            {
                if (depth < 1 || n.imm < 1 || i + static_cast<uint32_t>(n.imm) >= count)
                    return false;
                const uint64_t condition = stack[--depth];
                if ((n.sub == 0) != (condition != 0u))
                    i += static_cast<uint32_t>(n.imm); // (the taken arm and its kSkip: the other arm follows)
                continue;
            }
            case kSkip:
                if (n.imm < 0 || i + static_cast<uint32_t>(n.imm) >= count + 0u)
                    return false;
                i += static_cast<uint32_t>(n.imm);
                continue;
            case kAlu:
            {
                const int k = program::aluOperands(n.sub);
                if (depth < static_cast<uint32_t>(k) || n.sub >= kAluCount)
                    return false;
                uint64_t in[3] = {0, 0, 0};
                for (int j = k - 1; j >= 0; --j)
                    in[j] = stack[--depth];
                if (!program::alu(n.sub, static_cast<int64_t>(n.imm), in[0], in[1], in[2], v))
                    return false;
                break;
            }
            case kConst:
            case kEntry:
            case kPrevious:
            case kResult:
            case kArgument:
            case kFloatArgument:
            case kTarget:
            case kEntryLoad:
            case kReturnLoad:
            case kCarried:
                if (!leafValue(frame, state, regs, n, scope, v))
                    return false;
                break;
            default:
                return false;
            }
            if (depth >= kDepth)
                return false;
            stack[depth++] = v;
        }
        return true;
    }
    }

    namespace
    {
        // ZX_READABLE_HELD_RANGE=[!]LO-HI[,...] (a bisect's): only the functions at LO <= address < HI of
        // the ranges given use the table (every function when none is), less those of a !range; the others
        // enter with no row, as with the table off.
        bool inRange(uint32_t address)
        {
            struct Range
            {
                uint32_t lo, hi;
                bool out;
            };
            static const std::vector<Range> ranges = []
            {
                std::vector<Range> list;
                const char *v = std::getenv("ZX_READABLE_HELD_RANGE");
                while (v && *v)
                {
                    const bool out = *v == '!';
                    char *end = nullptr;
                    const uint32_t lo = static_cast<uint32_t>(std::strtoul(v + (out ? 1 : 0), &end, 0));
                    const uint32_t hi = *end == '-' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)) : ~0u;
                    list.push_back(Range{lo, hi, out});
                    v = *end == ',' ? end + 1 : nullptr;
                }
                return list;
            }();
            bool any = false, in = false;
            for (const Range &r : ranges)
            {
                const bool here = address >= r.lo && address < r.hi;
                if (r.out && here)
                    return false;
                any = any || !r.out;
                in = in || (!r.out && here);
            }
            return !any || in;
        }

        // The table's row of each frame-table row (ZX_READABLE_HELD_RANGE's functions), worked out once: what
        // find() gives depends on the tables alone, and every readable entry asks. By the frame row's place in the
        // frame table (ee::frameRowNumber's, here: no call for it).
        struct RowsOfFrameRows
        {
            const FrameRow *first = nullptr; // the frame table's rows
            uint32_t count = 0;
            std::vector<const Function *> functions; // per row
        };
        const RowsOfFrameRows &rowsOfFrameRows()
        {
            static const RowsOfFrameRows rows = []
            {
                RowsOfFrameRows made;
                made.count = frameRowCount();
                made.first = made.count ? &frameRow(1) : nullptr;
                made.functions.assign(made.count, nullptr);
                for (uint32_t i = 0; i < made.count; ++i)
                {
                    const FrameRow &row = made.first[i];
                    made.functions[i] = inRange(row.address) ? find(row.game, row.address) : nullptr;
                }
                return made;
            }();
            return rows;
        }

        // A function's row by its frame row; a row made by hand through the thread's cache of the last ones.
        const Function *functionOf(ThreadState &thread, const FrameRow *layout)
        {
            if (!layout)
                return nullptr;
            const RowsOfFrameRows &rows = rowsOfFrameRows();
            const std::less<const FrameRow *> below;
            if (rows.count && !below(layout, rows.first) && below(layout, rows.first + rows.count))
                return rows.functions[static_cast<size_t>(layout - rows.first)];
            ThreadState::HeldRow &e =
                thread.heldRows[(reinterpret_cast<uintptr_t>(layout) / sizeof(FrameRow)) % ThreadState::kHeldRows];
            if (e.layout != layout)
                e = ThreadState::HeldRow{layout, inRange(layout->address) ? find(layout->game, layout->address) : nullptr};
            return e.function;
        }

        // Whether the call fits case c's checks; `confirmed`: a check was worked out and held (what
        // a kChecked value needs). several: the call has other cases it can be (only then is a condition its
        // path met, kind 18, worked out: it chooses among them).
        bool interpretedMatches(const Frame &frame, const State &state, const CallRegs &regs, const Case &c,
                                bool &confirmed, bool several, bool replaying)
        {
            const Table &t = table();
            confirmed = false;
            for (uint32_t i = 0; i < c.checkCount; ++i)
            {
                const Value &check = t.checks[c.firstCheck + i];
                if (check.slot == 18 && !several)
                    continue;
                uint64_t want = 0;
                bool unreadable = false;
                if (!evaluateValue(frame, state, regs, check, want, true, &unreadable, replaying))
                {
                    if (unreadable)
                        return false; // (a load it cannot read: not this case)
                    continue;
                }
                if (check.slot < 8)
                {
                    if (check.slot < static_cast<uint32_t>(regs.intCount) &&
                        static_cast<uint32_t>(regs.a[check.slot]) != static_cast<uint32_t>(want))
                        return false;
                }
                else if (check.slot < 16)
                {
                    const uint32_t i2 = check.slot - 8;
                    if (i2 < static_cast<uint32_t>(regs.floatCount) && bitsOf(regs.f[i2]) != static_cast<uint32_t>(want))
                        return false;
                }
                else if (check.slot == 17 || check.slot == 18)
                {
                    // (the case's path condition: the branch facts since the previous call, 1 when they hold;
                    // 18: a condition the path met, which rules the case out when it fails and proves nothing)
                    if (want == 0u)
                        return false;
                    if (check.slot == 18)
                        continue;
                }
                else if ((regs.target & 0x1FFFFFFFu) != (static_cast<uint32_t>(want) & 0x1FFFFFFFu))
                    return false;
                // (an argument the call does not pass is no proof)
                confirmed = confirmed || check.slot >= 16 ||
                            (check.slot < 8 ? check.slot < static_cast<uint32_t>(regs.intCount)
                                            : check.slot - 8u < static_cast<uint32_t>(regs.floatCount));
            }
            return true;
        }

        HELD_NOINLINE bool checkedMatches(const Frame &frame, const State &state, const CallRegs &regs, const Case &c,
                                          bool &confirmed, bool several, bool replaying)
        {
            const Table &t = table();
            bool nodesConfirmed = false, codeConfirmed = false;
            const bool nodesFit = interpretedMatches(frame, state, regs, c, nodesConfirmed, several, replaying);
            const bool codeFit = t.caseChecks[c.checksCode](program::Context{frame, state, regs, true, nullptr, replaying},
                                                            several, codeConfirmed);
            ++g_codeChecks.run;
            if (nodesFit != codeFit || (nodesFit && nodesConfirmed != codeConfirmed))
            {
                if (++g_codeChecks.differ <= 20u)
                    std::fprintf(stderr, "[held] the checks of case %u (code %u) differ from their rows: fit %d/%d, "
                                 "confirmed %d/%d\n", static_cast<unsigned>(&c - t.cases), c.checksCode, nodesFit,
                                 codeFit, nodesConfirmed, codeConfirmed);
            }
            confirmed = nodesConfirmed;
            return nodesFit;
        }
        // Whether the call fits case c's checks (interpretedMatches), through their code when they have it.
        HELD_INLINE bool matches(const Frame &frame, const State &state, const CallRegs &regs, const Case &c,
                                 bool &confirmed, bool several, bool replaying)
        {
            if (c.checksCode && g_codeMode >= kCode)
            {
                if (g_codeMode == kCodeChecked)
                    return checkedMatches(frame, state, regs, c, confirmed, several, replaying);
                return table().caseChecks[c.checksCode](program::Context{frame, state, regs, true, nullptr, replaying},
                                                        several, confirmed);
            }
            return interpretedMatches(frame, state, regs, c, confirmed, several, replaying);
        }

        // Case c's values through their code, when they have it (false: none, the rows are worked through): each
        // into w, a kChecked one only when `confirmed`; loads: memory may be read (one case alone).
        HELD_NOINLINE void checkedValues(const Frame &frame, const State &state, const CallRegs &regs, const Case &c,
                                         bool confirmed, bool loads, program::Worked &w)
        {
            const Table &t = table();
            program::Worked nodes;
            for (uint32_t k = 0; k < c.valueCount; ++k)
            {
                const Value &v = t.values[c.firstValue + k];
                const uint32_t slot = v.slot & ~kChecked;
                uint64_t x = 0;
                if (v.count && (!(v.slot & kChecked) || confirmed) && evaluateValue(frame, state, regs, v, x, loads))
                {
                    nodes.value[slot] = x;
                    nodes.known |= 1u << slot;
                }
                else
                    nodes.cleared |= 1u << slot;
            }
            t.caseValues[c.valuesCode](program::Context{frame, state, regs, loads, nullptr, false}, confirmed, w);
            ++g_codeChecks.run;
            bool same = nodes.known == w.known && nodes.cleared == w.cleared;
            for (uint32_t left = nodes.known; left && same; left &= left - 1u)
                same = nodes.value[std::countr_zero(left)] == w.value[std::countr_zero(left)];
            if (!same && ++g_codeChecks.differ <= 20u)
                std::fprintf(stderr, "[held] the values of case %u (code %u) differ from their rows: known 0x%x/0x%x, "
                             "cleared 0x%x/0x%x\n", static_cast<unsigned>(&c - t.cases), c.valuesCode, nodes.known,
                             w.known, nodes.cleared, w.cleared);
            w = nodes;
        }
        HELD_INLINE bool valuesByCode(const Frame &frame, const State &state, const CallRegs &regs, const Case &c,
                                      bool confirmed, bool loads, program::Worked &w)
        {
            if (!c.valuesCode || g_codeMode < kCodeValues)
                return false;
            if (g_codeMode == kCodeChecked)
                checkedValues(frame, state, regs, c, confirmed, loads, w);
            else
                table().caseValues[c.valuesCode](program::Context{frame, state, regs, loads, nullptr, false}, confirmed, w);
            return true;
        }

        // The case of `site` the running function is in: the one after its
        // previous call (kLost: the site's own lost case).
        // (a site's cases are sorted by prev: the entry's 0 first, kLost last)
        const Case *caseOf(const Site &site, uint32_t prev)
        {
            const Table &t = table();
            const Case *first = t.cases + site.firstCase, *last = first + site.caseCount;
            const Case *it = std::lower_bound(first, last, prev, [](const Case &c, uint32_t v) { return c.prev < v; });
            return it != last && it->prev == prev ? it : nullptr;
        }
    }

    const Function *rowOf(ThreadState &thread, const FrameRow *layout) { return on() ? functionOf(thread, layout) : nullptr; }

    namespace
    {
        // ZX_READABLE_HELD_ENTRY_LOADS=0: no entry or return load is read (what they cost).
        bool loadsOn()
        {
            static const bool value = []
            {
                const char *v = std::getenv("ZX_READABLE_HELD_ENTRY_LOADS");
                return !(v && v[0] == '0');
            }();
            return value;
        }
    }

    namespace
    {
        void enterArguments(Frame &frame, State &s, const CallRegs *arguments);
    }

    void enter(Frame &frame, const Function *row, const CallRegs *arguments, const ThreadState &thread)
    {
        State &s = frame.held();
        s.function = row;
        if (!s.function)
            return;
        s.flags = row->flags;
        s.prev[0] = kFromEntry;
        s.places = 1;
        s.after = g_successors.on ? g_successors.ofEntry[static_cast<size_t>(row - table().functions)] : 0u;
        s.known = kHeldSlots;
        s.entry = kHeldSlots;
        s.results = 0;
        s.arguments = 0;
        s.entryLoads = 0;
        s.lastSite = nullptr;
        s.returnLoads = 0;
        s.localRanges = 0;
        if (s.function->flags & kReadsEntryArguments)
            enterArguments(frame, s, arguments);
        // (in a replay none is read, as a check's load: a recording does not say which bytes it holds, so
        // they are not known and their values are the entry's; ZX_READABLE_HELD_ENTRY_LOADS=0: none, to
        // measure what they cost)
        if (!s.function->entryLoadCount || thread.callHandler || !loadsOn())
            return;
        ++g_counts.entries;
        // (in order: one may read through an earlier one; through the bounds-checked read, never recorded)
        const Table &t = table();
        static const CallRegs none{}; // (no call: one made once)
        for (uint32_t i = 0; i < s.function->entryLoadCount && i < static_cast<uint32_t>(kEntryLoads); ++i)
        {
            const Value &load = t.entryLoads[s.function->firstEntryLoad + i];
            uint64_t x = 0;
            ++g_counts.entryLoads;
            if (evaluateValue(frame, s, none, load, x, true))
            {
                s.entryLoad[i] = x;
                s.entryLoads |= static_cast<uint8_t>(1u << i);
            }
        }
    }

    namespace
    {
    void enterArguments(Frame &frame, State &s, const CallRegs *arguments)
    {
        if (arguments)
        {
            for (int i = 0; i < arguments->intCount && i < 8; ++i)
                s.argument[i] = arguments->a[i];
            for (int i = 0; i < arguments->floatCount && i < 8; ++i)
                s.floatArgument[i] = static_cast<uint32_t>(bitsOf(arguments->f[i]));
            s.arguments = static_cast<uint16_t>(((1u << std::min(arguments->intCount, 8)) - 1u) |
                                                (((1u << std::min(arguments->floatCount, 8)) - 1u) << 8));
        }
        else if (frame.ctx)
        {
            for (int i = 0; i < 8; ++i)
                s.argument[i] = gpr64(frame.ctx, reg::a0 + i);
            for (int i = 0; i < 8; ++i)
                s.floatArgument[i] = static_cast<uint32_t>(bitsOf(frame.ctx->f[reg::f12 + i]));
            s.arguments = 0xFFFFu;
        }
    }
    }

    void noteLocal(Frame &frame, uint32_t offset, uint32_t bytes)
    {
        State &s = frame.held();
        if (!s.function || s.localRanges >= kLocalRanges || offset + bytes > 0xFFFFu)
            return;
        s.localFrom[s.localRanges] = static_cast<uint16_t>(offset);
        s.localTo[s.localRanges] = static_cast<uint16_t>(offset + bytes);
        ++s.localRanges;
    }

    void forgetArguments(Frame &frame)
    {
        // (and the entry loads, read with arguments that were not the inner call's)
        frame.held().arguments = 0;
        frame.held().entryLoads = 0;
    }

#if ZX_DEV_CHECKS
    namespace
    {
        // ZX_READABLE_HELD_CODE=check's sweep: every program and case the table has as code, run both ways
        // (code, rows and nodes) ZX_READABLE_HELD_CODE_ROUNDS times (default 16) on states and calls made up
        // from `frame`'s and `regs` (masks, registers, arguments, pointers into RAM, the call's counts), so that
        // what a game's run never reaches is compared too. Through the checked paths above (g_codeChecks).
        void sweepCode(const Frame &frame, const CallRegs &regs)
        {
            const Table &t = table();
            if (!t.programs)
                return;
            const char *roundsText = std::getenv("ZX_READABLE_HELD_CODE_ROUNDS");
            const uint32_t rounds = roundsText ? static_cast<uint32_t>(std::strtoul(roundsText, nullptr, 0)) : 16u;
            std::mt19937_64 rng(0x48454C44u);
            const auto word = [&rng]() -> uint64_t
            {
                switch (rng() % 4u)
                {
                case 0: return (rng() % PS2_RAM_SIZE & ~15u) + (rng() % 4u) * 4u; // (a pointer into RAM)
                case 1: return rng() % 64u;
                case 2: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(rng())));
                default: return rng();
                }
            };
            const auto madeUp = [&](State &st, CallRegs &r)
            {
                st = frame.held();
                st.known = static_cast<uint32_t>(rng()) & kHeldSlots;
                st.entry = st.known & static_cast<uint32_t>(rng());
                for (uint64_t &v : st.value)
                    v = word();
                st.arguments = static_cast<uint16_t>(rng());
                for (uint64_t &v : st.argument)
                    v = word();
                for (uint32_t &v : st.floatArgument)
                    v = static_cast<uint32_t>(word());
                st.results = static_cast<uint8_t>(rng() % 8u);
                st.v0 = word();
                st.v1 = word();
                st.f0 = static_cast<uint32_t>(word());
                st.entryLoads = static_cast<uint8_t>(rng());
                for (uint64_t &v : st.entryLoad)
                    v = word();
                st.returnLoads = static_cast<uint8_t>(rng());
                for (uint64_t &v : st.returnLoad)
                    v = word();
                st.localRanges = static_cast<uint8_t>(rng() % (kLocalRanges + 1u));
                for (uint32_t i = 0; i < st.localRanges; ++i)
                {
                    st.localFrom[i] = static_cast<uint16_t>(rng() % 0x100u * 4u);
                    st.localTo[i] = static_cast<uint16_t>(st.localFrom[i] + rng() % 0x40u * 4u);
                }
                r = regs;
                r.intCount = static_cast<int>(rng() % 9u);
                r.floatCount = static_cast<int>(rng() % 9u);
                for (uint64_t &v : r.a)
                    v = word();
                for (float &v : r.f)
                    v = std::bit_cast<float>(static_cast<uint32_t>(word()));
                r.target = rng() % 2u ? regs.target : static_cast<uint32_t>(word());
            };
            const uint64_t before = g_codeChecks.run;
            State st;
            CallRegs r;
            const Value *const lists[] = {t.values, t.checks, t.entryLoads, t.returnLoads};
            uint32_t sizes[4] = {0, 0, 0, 0}; // (the lists' lengths, from the cases, sites and functions that index them)
            for (uint32_t i = 0; i < t.count; ++i)
            {
                const Function &fn = t.functions[i];
                sizes[2] = std::max(sizes[2], fn.firstEntryLoad + fn.entryLoadCount);
                for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                {
                    const Site &site = t.sites[j];
                    sizes[3] = std::max(sizes[3], site.firstReturnLoad + site.returnLoadCount);
                    for (uint32_t k = site.firstCase; k < site.firstCase + site.caseCount; ++k)
                    {
                        const Case &c = t.cases[k];
                        sizes[0] = std::max(sizes[0], c.firstValue + c.valueCount);
                        sizes[1] = std::max(sizes[1], c.firstCheck + c.checkCount);
                        for (uint32_t round = 0; round < rounds; ++round)
                        {
                            madeUp(st, r);
                            program::Worked w;
                            if (c.valuesCode)
                                checkedValues(frame, st, r, c, rng() % 2u != 0u, rng() % 2u != 0u, w);
                            bool confirmed = false;
                            if (c.checksCode)
                                checkedMatches(frame, st, r, c, confirmed, rng() % 2u != 0u, rng() % 4u == 0u);
                        }
                    }
                }
            }
            for (int list = 0; list < 4; ++list)
                for (uint32_t i = 0; i < sizes[list]; ++i)
                {
                    const Value &v = lists[list][i];
                    if (!v.program)
                        continue;
                    for (uint32_t round = 0; round < rounds; ++round)
                    {
                        madeUp(st, r);
                        uint64_t out = 0;
                        bool unreadable = false;
                        checkedProgram(frame, st, r, v, out, rng() % 2u != 0u, rng() % 2u ? &unreadable : nullptr,
                                       rng() % 4u == 0u);
                    }
                }
            std::fprintf(stderr, "[held] the table's code swept against its rows and nodes: %llu runs, %llu differing "
                         "so far\n", static_cast<unsigned long long>(g_codeChecks.run - before),
                         static_cast<unsigned long long>(g_codeChecks.differ));
        }
    }
#endif

    namespace
    {
        // The sites a call can be: its target's (or a jalr's) with a case after one of the sites the last call can
        // have been (`places` of `prev`; none: kLost's cases). In the function's order of sites, each place's case
        // in turn; at most kPicks (overflow: more). needsProof: one case with checks and a kChecked value.
        void searchPicks(const Table &t, const Function &fn, const uint32_t *prev, int places, uint32_t target,
                         Picks &out)
        {
            // (a function's sites are sorted by target: its jalrs, target 0, first, then the
            // target's own run)
            const Site *first = t.sites + fn.firstSite, *last = first + fn.siteCount;
            const Site *indirectEnd = first;
            const size_t fnIndex = static_cast<size_t>(&fn - t.functions);
            if (fnIndex < g_indirectSites.size())
                indirectEnd += g_indirectSites[fnIndex];
            else
                while (indirectEnd != last && indirectEnd->target == 0u)
                    ++indirectEnd;
            const Site *own = std::lower_bound(indirectEnd, last, target, [](const Site &x, uint32_t v) { return x.target < v; });
            const Site *ownEnd = own;
            while (ownEnd != last && ownEnd->target == target)
                ++ownEnd;
            for (int range = 0; range < 2 && target != 0u; ++range)
                for (const Site *it = range == 0 ? first : own; it != (range == 0 ? indirectEnd : ownEnd); ++it)
                {
                    for (int p = 0; p < (places ? places : 1); ++p)
                    {
                        const Case *c = caseOf(*it, places ? prev[p] : kLost);
                        if (!c)
                            continue;
                        if (out.count == kPicks)
                            out.overflow = true;
                        else
                            out.pick[out.count++] = Pick{it, c};
                    }
                }
            if (out.count == 1 && out.pick[0].c->checkCount)
                for (uint32_t k = 0; k < out.pick[0].c->valueCount && !out.needsProof; ++k)
                    out.needsProof = (t.values[out.pick[0].c->firstValue + k].slot & kChecked) != 0;
        }

        // The same after one place (or none), from its successors (After `after`): its jalrs, then its sites
        // of the target.
        void nextPicks(const Table &t, uint32_t after, uint32_t target, Picks &out)
        {
            if (target == 0u)
                return;
            const After &place = g_successors.after[after];
            const Next *const first = g_successors.next.data() + place.first, *const last = first + place.count;
            const Next *const own = std::lower_bound(first + place.jalrs, last, target,
                                                     [](const Next &x, uint32_t v) { return x.target < v; });
            const Next *firstPick = nullptr;
            for (int range = 0; range < 2; ++range)
                for (const Next *it = range == 0 ? first : own;
                     it != (range == 0 ? first + place.jalrs : last) && (range == 0 || it->target == target); ++it)
                {
                    if (!firstPick)
                        firstPick = it;
                    if (out.count == kPicks)
                        out.overflow = true;
                    else
                        out.pick[out.count++] = Pick{t.sites + it->site, t.cases + it->caseIndex};
                }
            out.needsProof = out.count == 1 && firstPick->needsProof;
        }

        // The one pick of a call of `target` after one place (After `after`), when it is one: its jalr alone, or its
        // one site of the target and no jalr. Null: none, or several (nextPicks then).
        HELD_INLINE const Next *soleNext(uint32_t after, uint32_t target)
        {
            const After &place = g_successors.after[after];
            const Next *const first = g_successors.next.data() + place.first, *const last = first + place.count;
            if (place.jalrs > 1)
                return nullptr;
            const Next *const own = std::lower_bound(first + place.jalrs, last, target,
                                                     [](const Next &x, uint32_t v) { return x.target < v; });
            const bool owns = own != last && own->target == target;
            if (place.jalrs == 1)
                return owns ? nullptr : first;
            return owns && (own + 1 == last || own[1].target != target) ? own : nullptr;
        }

        // ZX_READABLE_HELD_SUCCESSORS=check: every place of every function, with each target its sites have
        // and two they do not (0, and one no function is at), its picks from the successors against the search.
        void checkSuccessors(const Table &t)
        {
            uint64_t tried = 0, differ = 0;
            std::vector<uint32_t> targets;
            for (uint32_t i = 0; i < t.count; ++i)
            {
                const Function &fn = t.functions[i];
                targets.assign({0u, 0x1FFFFFFCu});
                for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                    targets.push_back(t.sites[j].target);
                struct Place
                {
                    uint32_t prev;
                    int places;
                    uint32_t after;
                };
                std::vector<Place> places{{kFromEntry, 1, g_successors.ofEntry[i]}, {kLost, 0, g_successors.ofLost[i]}};
                for (uint32_t j = fn.firstSite; j < fn.firstSite + fn.siteCount; ++j)
                    places.push_back({t.sites[j].site, 1, g_successors.ofSite[j]});
                for (const Place &place : places)
                    for (const uint32_t target : targets)
                    {
                        Picks a, b;
                        searchPicks(t, fn, &place.prev, place.places, target, a);
                        nextPicks(t, place.after, target, b);
                        bool same = a.count == b.count && a.overflow == b.overflow && a.needsProof == b.needsProof;
                        for (int k = 0; k < a.count && same; ++k)
                            same = a.pick[k].site == b.pick[k].site && a.pick[k].c == b.pick[k].c;
                        ++tried;
                        if (!same && ++differ <= 20u)
                            std::fprintf(stderr, "[held] successors differ from the search: 0x%x after 0x%x (%d places), "
                                         "target 0x%x: %d/%d picks\n", fn.address, place.prev, place.places, target,
                                         a.count, b.count);
                    }
            }
            std::fprintf(stderr, "[held] successors checked against the search: %llu calls, %llu differing\n",
                         static_cast<unsigned long long>(tried), static_cast<unsigned long long>(differ));
        }
    }

    namespace
    {
        // The call placed, at `placeCount` places (its site's jal when one): `known` of the place's values
        // (s.value, `entry` of them still the entry's) published into frame.next for the callee, as the original's
        // registers hold them (what the function published itself, a draft's ee::calleeSaves, stands; an FPR's bits
        // are its low word, in both), with the jal's return address when it is one site; the place it leaves.
        HELD_INLINE void settle(Frame &frame, State &s, const Function *fn, uint32_t target, uint32_t known,
                                uint32_t entry, const uint32_t (&places)[kPlaces], int placeCount, const Site *site,
                                uint32_t after)
        {
            uint64_t *const held = s.value;
            SavedRegs &next = frame.next;
            const uint32_t own = next.mask & kHeldSlots, given = known & ~entry & ~next.mask;
            next.mask |= given;
            for (uint32_t left = given; left; left &= left - 1u)
            {
                const int slot = std::countr_zero(left);
                if (slot >= kFirstFloatSlot)
                    held[slot] = static_cast<uint32_t>(held[slot]);
                next.value[slot] = held[slot];
            }
            for (uint32_t left = own; left; left &= left - 1u)
                held[std::countr_zero(left)] = next.value[std::countr_zero(left)];
            known |= own;
            entry &= ~own;
            // The jal's return address, when it is one site.
            if (placeCount == 1 && returnsOn() && !(next.mask & returnAddressSlot()))
            {
                next.mask |= returnAddressSlot();
                next.value[savedSlot(reg::ra)] = places[0] + 8u;
            }
            const bool traceThis = tracing() && tracedHere(fn->address); // (the row read only for a trace)
            if (traceThis)
            {
                std::fprintf(stderr, "[held] 0x%x: call of 0x%x after 0x%x (%d places): site 0x%x (%d places), given 0x%x, not known 0x%x, sp 0x%x fnSp 0x%x:",
                             fn->address, target, s.places ? s.prev[0] : kLost, s.places, places[0], placeCount, known & ~entry,
                             kHeldSlots & ~known, frame.calleeSp, frame.fnSp);
                for (uint32_t left = next.mask; left; left &= left - 1u)
                    std::fprintf(stderr, " %d=%llx", std::countr_zero(left), static_cast<unsigned long long>(next.value[std::countr_zero(left)]));
                for (uint32_t left = kHeldSlots & ~next.mask & (frame.ctx ? ~0u : 0u); left; left &= left - 1u)
                    std::fprintf(stderr, " e%d=%llx", std::countr_zero(left),
                                 static_cast<unsigned long long>(entryRegister(frame, savedSlotReg(std::countr_zero(left)))));
                std::fprintf(stderr, "\n");
            }
            ++g_counts.placed;
            if (counting() && g_counts.placed % 1000000u == 0u)
            {
                g_counts.print();
                if (g_codeMode == kCodeChecked)
                    std::fprintf(stderr, "[held] code checked against the rows and nodes %llu, differing %llu\n",
                                 static_cast<unsigned long long>(g_codeChecks.run),
                                 static_cast<unsigned long long>(g_codeChecks.differ));
            }
            s.known = known;
            s.entry = entry;
            std::memcpy(s.prev, places, sizeof s.prev); // (all kPlaces: a fixed copy; past placeCount none is read)
            s.places = static_cast<uint8_t>(placeCount);
            s.results = 0;
            s.lastSite = placeCount == 1 ? site : nullptr;
            s.after = after;
            s.returnLoads = 0;
        }

        // A case's values worked out (w) into the place's: the slots it gives, the rest as the last call had them
        // (after a call not placed, only the case's).
        HELD_INLINE void takeValues(State &s, const program::Worked &w, bool fromLost, uint32_t &known, uint32_t &entry)
        {
            known = fromLost ? 0u : s.known;
            entry = fromLost ? 0u : s.entry;
            for (uint32_t left = w.known; left; left &= left - 1u)
                s.value[std::countr_zero(left)] = w.value[std::countr_zero(left)];
            known = (known | w.known) & ~w.cleared;
            entry &= ~(w.known | w.cleared) & known;
        }

        // Several cases fit the call: what they all agree on (a slot at the entry value in one case and worked out
        // in another: known only if the numbers agree) into the place's values; none of their loads made (they
        // may be anywhere: evaluate).
        HELD_NOINLINE void agreeOnCases(const Frame &frame, State &s, const CallRegs &regs, const Pick *picks,
                                        const bool *confirmed, int count, uint32_t &known, uint32_t &entry)
        {
            const Table &t = table();
            uint64_t value[kSlots];
            known = kHeldSlots;
            entry = s.entry;
            for (int i = 0; i < count; ++i)
            {
                const Case &c = *picks[i].c;
                uint32_t caseKnown = c.prev == kLost ? 0u : s.known;
                uint32_t caseEntry = c.prev == kLost ? 0u : s.entry;
                uint64_t caseValue[kSlots];
                std::memcpy(caseValue, s.value, sizeof caseValue);
                program::Worked w;
                if (valuesByCode(frame, s, regs, c, confirmed[i], false, w))
                {
                    for (uint32_t left = w.known; left; left &= left - 1u)
                        caseValue[std::countr_zero(left)] = w.value[std::countr_zero(left)];
                    caseKnown = (caseKnown | w.known) & ~w.cleared;
                    caseEntry &= ~(w.known | w.cleared);
                }
                else
                    for (uint32_t k = 0; k < c.valueCount; ++k)
                    {
                        const Value &v = t.values[c.firstValue + k];
                        const uint32_t slot = v.slot & ~kChecked;
                        const uint32_t bit = 1u << slot;
                        uint64_t x = 0;
                        if (v.count && (!(v.slot & kChecked) || confirmed[i]) && evaluateValue(frame, s, regs, v, x, false))
                        {
                            caseValue[slot] = x;
                            caseKnown |= bit;
                        }
                        else
                            caseKnown &= ~bit;
                        caseEntry &= ~bit;
                    }
                if (i == 0)
                {
                    std::memcpy(value, caseValue, sizeof value);
                    known = caseKnown;
                    entry = caseEntry;
                    continue;
                }
                for (uint32_t left = known; left; left &= left - 1u)
                {
                    const int slot = std::countr_zero(left);
                    const uint32_t bit = 1u << slot;
                    if (!(caseKnown & bit))
                    {
                        known &= ~bit;
                        continue;
                    }
                    const bool a = entry & bit, b = caseEntry & bit;
                    if (a && b)
                        continue;
                    const uint64_t va = a ? (frame.ctx ? entryRegister(frame, savedSlotReg(slot)) : 0u) : value[slot];
                    const uint64_t vb = b ? (frame.ctx ? entryRegister(frame, savedSlotReg(slot)) : 0u) : caseValue[slot];
                    if (va != vb || ((a || b) && !frame.ctx))
                        known &= ~bit;
                    else
                    {
                        value[slot] = va;
                        entry &= ~bit;
                    }
                }
                entry &= known;
            }
            entry &= known;
            std::memcpy(s.value, value, sizeof value);
        }
    }

    void beforeCall(Frame &frame, const CallRegs &regs) { beforeCall(frame, regs, t_ee); }

    void beforeCall(Frame &frame, const CallRegs &regs, ThreadState &thread)
    {
        State &s = frame.held();
        const Function *fn = s.function;
        if (!fn)
            return;
        const Table &t = table();
#if ZX_DEV_CHECKS
        if (g_codeMode == kCodeChecked)
        {
            static bool swept = false;
            if (!swept)
            {
                swept = true;
                sweepCode(frame, regs);
            }
        }
#endif
        // (a host form's call in place of one of the original's: placed as that one, placeAs)
        const uint32_t target = placedTarget(t, *fn, regs.target & 0x1FFFFFFFu);
        // The common call: after one place, one site of its target (or one jalr) with a case after it, which
        // needs no proof and has its values as code or none (Next): placed there.
        if (s.places <= 1 && g_successors.on && target != 0u && g_codeMode >= kCodeValues && g_codeMode <= kCode)
        {
            ++g_counts.indexed;
            if (const Next *sole = soleNext(s.after, target); sole && !sole->needsProof && (sole->values || sole->noValues))
            {
                program::Worked w;
                if (sole->values)
                    sole->values(program::Context{frame, s, regs, true, nullptr, false}, false, w);
                uint32_t known = 0, entry = 0;
                takeValues(s, w, sole->fromLost, known, entry);
                const uint32_t places[kPlaces] = {sole->siteAddress};
                settle(frame, s, fn, target, known, entry, places, 1, t.sites + sole->site, sole->after);
                return;
            }
        }
        else if (s.places <= 1 && g_successors.on)
            ++g_counts.indexed;
        else
            ++g_counts.searched;
        // The sites it can be: its target's (or a jalr's) with a case after one
        // of the sites the last call can have been; of several, the ones the
        // call's arguments agree with.
        Picks found;
        if (s.places <= 1 && g_successors.on)
            nextPicks(t, s.after, target, found);
        else
            searchPicks(t, *fn, s.prev, s.places, target, found);
        Pick *const picks = found.pick;
        int count = found.count;
        bool overflow = found.overflow;
        const bool needsProof = found.needsProof;
        // (several cases, or one with a kChecked value: a single case's checks prove those, and one that
        // does not fit is not this call's)
        bool confirmed[kPicks];
        if (count > 1 || needsProof)
        {
            int kept = 0;
            for (int i = 0; i < count; ++i)
            {
                bool proven = false;
                if (matches(frame, s, regs, *picks[i].c, proven, count > 1, thread.callHandler != nullptr))
                {
                    confirmed[kept] = proven;
                    picks[kept++] = picks[i];
                }
            }
            count = kept;
        }
        else if (count == 1)
            confirmed[0] = false;
        // The sites it is now.
        uint32_t places[kPlaces];
        int placeCount = 0;
        for (int i = 0; i < count && !overflow; ++i)
        {
            bool seen = false;
            for (int j = 0; j < placeCount; ++j)
                seen = seen || places[j] == picks[i].site->site;
            if (seen)
                continue;
            if (placeCount == kPlaces)
                overflow = true;
            else
                places[placeCount++] = picks[i].site->site;
        }
        if (count == 0 || overflow)
        {
            if (tracing() && tracedHere(fn->address))
                std::fprintf(stderr, "[held] 0x%x: call of 0x%x after 0x%x (%d places): %d cases fit%s, not placed\n",
                             fn->address, target, s.places ? s.prev[0] : kLost, s.places, count, overflow ? " (too many)" : "");
            // Not placed: from here on what the calls get is not known, until a
            // call the runtime can place again.
            ++g_counts.lost;
            s.places = 0;
            s.after = g_successors.on ? g_successors.ofLost[static_cast<size_t>(fn - t.functions)] : 0u;
            s.known = 0;
            s.entry = 0;
            s.results = 0;
            s.lastSite = nullptr;
            s.returnLoads = 0;
            return;
        }
        // What it gets: each case's values, the rest as the last call had them
        // (after a call not placed, only the case's); of several cases, what
        // they all agree on. One case (nearly every call): in place.
        uint32_t known = kHeldSlots, entry = s.entry;
        if (count == 1)
        {
            const Case &c = *picks[0].c;
            program::Worked w;
            if (!valuesByCode(frame, s, regs, c, confirmed[0], true, w))
                for (uint32_t k = 0; k < c.valueCount; ++k)
                {
                    const Value &v = t.values[c.firstValue + k];
                    const uint32_t slot = v.slot & ~kChecked;
                    uint64_t x = 0;
                    if (v.count && (!(v.slot & kChecked) || confirmed[0]) &&
                        evaluateValue(frame, s, regs, v, x, true))
                    {
                        w.value[slot] = x;
                        w.known |= 1u << slot;
                    }
                    else
                        w.cleared |= 1u << slot;
                }
            takeValues(s, w, c.prev == kLost, known, entry);
        }
        else
            agreeOnCases(frame, s, regs, picks, confirmed, count, known, entry);
        const Site *const site = placeCount == 1 ? picks[0].site : nullptr;
        settle(frame, s, fn, target, known, entry, places, placeCount, site,
               site && g_successors.on ? g_successors.ofSite[static_cast<size_t>(site - t.sites)] : 0u);
    }

    void afterCall(Frame &frame, uint64_t v0, uint64_t v1, uint32_t f0, uint8_t known)
    {
        afterCall(frame, v0, v1, f0, known, t_ee);
    }

    void afterCall(Frame &frame, uint64_t v0, uint64_t v1, uint32_t f0, uint8_t known, const ThreadState &thread)
    {
        State &s = frame.held();
        if (!readsAfterCall(s))
            return;
        s.v0 = v0;
        s.v1 = v1;
        s.f0 = f0;
        s.results = s.places == 0 ? 0u : known;
        // The last call's return loads, read now (in order: one may read through an earlier one; through the
        // bounds-checked read, never recorded; in a replay none, see held.h).
        const Site *site = s.lastSite;
        if (!site || !site->returnLoadCount || thread.callHandler || !loadsOn())
            return;
        const Table &t = table();
        static const CallRegs none{}; // (no call: one made once)
        for (uint32_t i = 0; i < site->returnLoadCount && i < static_cast<uint32_t>(kReturnLoads); ++i)
        {
            const Value &load = t.returnLoads[site->firstReturnLoad + i];
            uint64_t x = 0;
            ++g_counts.entryLoads;
            if (evaluateValue(frame, s, none, load, x, true))
            {
                s.returnLoad[i] = x;
                s.returnLoads |= static_cast<uint8_t>(1u << i);
            }
        }
    }
}
