#include "abi.h"
#include "dispatch.h"
#include "markers.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ee
{
    namespace
    {
        // The entry block an ee::jumpDispatch(target, true) marked
        // (ThreadState::dispatchedMark): the ee::block(target) that follows
        // it (using up the dispatch paid for) is the same mark, not a second
        // one (rider10: DM2's airEnter, 86 "markers out of order").
        inline void marked(uint32_t address, MarkerKind kind)
        {
            if (MarkerHook hook = t_ee.markerHook)
            {
                const bool again = kind == MarkerKind::Block && address != 0 && address == t_ee.dispatchedMark;
                t_ee.dispatchedMark = 0;
                if (!again)
                    hook(address, kind);
            }
        }
    }

#if ZX_DEV_CHECKS
    void setMarkerHook(MarkerHook hook) { t_ee.markerHook = hook; }
#endif

    namespace
    {
        // The safe-point model runs: on a fiber in the game, or under
        // zx-fncheck's call handler while its --safepoints hook is set.
        inline bool modelOn() { return !t_ee.callHandler || t_ee.safePointHook; }
    }

    uint64_t directReturnDispatches() { return t_ee.directReturns; }
#if ZX_DEV_CHECKS
    void setSafePointHook(SafePointHook hook) { t_ee.safePointHook = hook; }
#endif
    bool safePointHookSet() { return t_ee.safePointHook != nullptr; }

    namespace
    {
        // Runs the callee (ctx->pc) to its return: PS2Runtime::callGuestFunction
        // calls this with ra = its sentinel. A tail jump or a branch into a
        // block the recompiler split off leaves pc elsewhere; those are
        // followed here so the results can be read at the return. A call with
        // a published return address gives the callee that (the original's
        // ra) and ends there, then hands callGuestFunction its sentinel.
        void trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sentinel = static_cast<uint32_t>(gpr64(ctx, reg::ra));
            const uint32_t back = t_ee.guestReturn ? t_ee.guestReturn : sentinel;
            t_ee.guestReturn = 0; // (a nested call sets its own)
            setGpr64(ctx, reg::ra, back);
            runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            for (uint32_t hops = 0; ctx->pc != back && hops < (1u << 24) && runtime->hasFunction(ctx->pc); ++hops)
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            if (ctx->pc == back)
                ctx->pc = sentinel;
            t_ee.v0 = gpr64(ctx, reg::v0);
            t_ee.v1 = gpr64(ctx, reg::v1);
            t_ee.f0 = ctx->f[reg::f0];
            t_ee.vu0 = Vu0State::of(*ctx);
            keepLeft(*ctx); // the callee's temporaries (ee::leftByCall)
        }
    }

    uint32_t publishedReturn(const Frame &frame)
    {
        const uint32_t bit = 1u << savedSlot(reg::ra);
        return returnsOn() && (frame.next.mask & bit) ? static_cast<uint32_t>(frame.next.value[savedSlot(reg::ra)]) : 0u;
    }

    uint32_t callReturn(const Frame &frame, uint32_t target)
    {
        if (const uint32_t r = publishedReturn(frame))
            return r;
        if (!returnsOn() || !frame.layout || !target)
            return 0u;
        return siteReturn(*frame.layout, target);
    }

    namespace
    {
    void publishSiteReturn(Frame *frame, uint32_t target)
    {
        const int slot = savedSlot(reg::ra);
        if (!frame || (frame->next.mask & (1u << slot)))
            return;
        if (const uint32_t r = callReturn(*frame, target))
        {
            frame->next.mask |= 1u << slot;
            frame->next.value[slot] = r;
        }
    }
    }

    void publishSiteReturn(uint32_t target) { publishSiteReturn(t_ee.frame, target); }

    uint32_t tailReturn(const Frame &frame)
    {
        if (!returnsOn())
            return 0u;
        if (frame.directDepth == 0)
            return frame.ra;
        const int slot = savedSlot(reg::ra);
        return (frame.view.mask & (1u << slot)) ? static_cast<uint32_t>(frame.view.value[slot]) : 0u;
    }

    void returnedTo(const Frame &frame, const SavedRegs &view)
    {
        const int slot = savedSlot(reg::ra);
        if (returnsOn() && frame.ctx && (view.mask & (1u << slot)))
            setGpr64(frame.ctx, reg::ra, static_cast<uint32_t>(view.value[slot]));
    }

    void CallRegs::into(R5900Context *ctx) const
    {
        for (int i = 0; i < intCount; ++i)
            if (qwordArgs & (1u << i))
                setGpr128(ctx, reg::a0 + i, Qword{a[i], aHigh[i]});
            else
                setGpr64(ctx, reg::a0 + i, a[i]);
        for (int i = 0; i < floatCount; ++i)
            ctx->f[reg::f12 + i] = f[i];
    }

    namespace detail
    {
        void storeStackWord(const CallRegs &regs, int index, uint64_t word, uint32_t bytes)
        {
            const Frame *frame = t_ee.frame;
            if (!frame)
            {
                std::fprintf(stderr, "[readable] call to 0x%x: a stack argument outside a readable function\n", regs.target);
                std::abort();
            }
            // (callGuest's choice of the callee's sp)
            const bool own = regs.tail && framesOn() && frame->fnSp;
            const uint32_t sp = own ? frame->fnSp : frame->calleeSp;
            const uint32_t at = sp + 8u * static_cast<uint32_t>(index);
            // A call's words lie in the caller's own frame (a tail call's where its caller's did).
            if (!own && at + 8u > frame->stackTop)
            {
                std::fprintf(stderr, "[readable] call to 0x%x: stack argument %d does not fit the caller's frame (sp 0x%x, frame top 0x%x)\n",
                             regs.target, index, sp, frame->stackTop);
                std::abort();
            }
            if (uint8_t *slot = ee::at<uint8_t>(at))
                std::memcpy(slot, &word, bytes == 8u ? 8u : 4u);
        }

        void noPlaceForArgument(const CallRegs &regs, const char *what)
        {
            std::fprintf(stderr, "[readable] call to 0x%x: %s is not modelled\n", regs.target, what);
            std::abort();
        }
    }

    void unlistedTarget(uint32_t jr)
    {
        static uint64_t count = 0;
        ++count;
        if ((count & (count - 1)) == 0)
            std::fprintf(stderr, "[readable] the jump table at 0x%x gave a target it does not list (%llu times)\n", jr,
                         static_cast<unsigned long long>(count));
    }

    void chargeCycles(uint64_t cycles)
    {
        if (CallHandler *handler = t_ee.callHandler)
        {
            handler->cycles(cycles);
            return;
        }
        if (Frame *frame = t_ee.frame; frame && frame->runtime)
            frame->runtime->eeScheduler().accountCycles(static_cast<uint32_t>(cycles));
    }

    // ---- the frame table (abi.h) ---------------------------------------------

    namespace
    {
        struct FrameTable
        {
            const FrameRow *rows = nullptr;
            uint32_t count = 0;
            const EntrySave *saves = nullptr;
            const uint16_t *locals = nullptr; // the offsets of ee::Local objects (tools/readable_locals.tsv)
        };
        FrameTable &frameTable()
        {
            static FrameTable table;
            return table;
        }
    }

    FrameTableRegistration::FrameTableRegistration(const FrameRow *rows, uint32_t count, const EntrySave *saves, const uint16_t *locals)
    {
        frameTable() = FrameTable{rows, count, saves, locals};
    }

    const FrameRow *frameLayout(uint32_t game, uint32_t address)
    {
        const FrameTable &table = frameTable();
        const FrameRow *end = table.rows + table.count;
        const FrameRow *it = std::lower_bound(table.rows, end, std::make_pair(game, address),
                                              [](const FrameRow &row, const std::pair<uint32_t, uint32_t> &key)
                                              { return row.game != key.first ? row.game < key.first : row.address < key.second; });
        return it != end && it->game == game && it->address == address ? it : nullptr;
    }

    const EntrySave *frameSaves(const FrameRow &row) { return frameTable().saves + row.first; }

    uint32_t frameRowNumber(const FrameRow *row)
    {
        const FrameTable &table = frameTable();
        const std::less<const FrameRow *> below;
        if (!row || below(row, table.rows) || !below(row, table.rows + table.count))
            return 0u;
        return static_cast<uint32_t>(row - table.rows) + 1u;
    }

    uint32_t frameRowCount() { return frameTable().count; }

    const FrameRow &frameRow(uint32_t number) { return frameTable().rows[number - 1u]; }

    namespace
    {
        struct CallSiteTable
        {
            const CallSite *rows = nullptr;
            uint32_t count = 0;
        };
        CallSiteTable &callSiteTable()
        {
            static CallSiteTable table;
            return table;
        }
        // A function's rows of the call-site table.
        struct SiteRange
        {
            const CallSite *first = nullptr, *last = nullptr;
        };
        SiteRange findSites(uint32_t game, uint32_t function)
        {
            const CallSiteTable &t = callSiteTable();
            const auto key = std::make_pair(game, function);
            const auto before = [](const CallSite &row, const std::pair<uint32_t, uint32_t> &k)
            { return std::make_pair(row.game, row.function) < k; };
            const auto after = [](const std::pair<uint32_t, uint32_t> &k, const CallSite &row)
            { return k < std::make_pair(row.game, row.function); };
            const CallSite *first = std::lower_bound(t.rows, t.rows + t.count, key, before);
            return SiteRange{first, std::upper_bound(first, t.rows + t.count, key, after)};
        }
        // One per frame-table row, made once: a level load asks some millions
        // of times, and a search of the whole table per call was a fifth of
        // its host time. (A row made by hand, the unit tests', is searched.)
        SiteRange sitesOf(const FrameRow &row)
        {
            static const std::vector<SiteRange> ranges = []
            {
                const FrameTable &frames = frameTable();
                std::vector<SiteRange> result(frames.count);
                for (uint32_t i = 0; i < frames.count; ++i)
                    result[i] = findSites(frames.rows[i].game, frames.rows[i].address);
                return result;
            }();
            const FrameTable &frames = frameTable();
            const std::less<const FrameRow *> below;
            if (below(&row, frames.rows) || !below(&row, frames.rows + frames.count))
                return findSites(row.game, row.address);
            return ranges[static_cast<std::size_t>(&row - frames.rows)];
        }
        const CallSite *findSite(const SiteRange &range, uint32_t target)
        {
            const CallSite *it = std::lower_bound(range.first, range.last, target, [](const CallSite &row, uint32_t t)
                                                  { return row.target < t; });
            return it != range.last && it->target == target ? it : nullptr;
        }
    }

    CallSiteRegistration::CallSiteRegistration(const CallSite *rows, uint32_t count)
    {
        callSiteTable() = CallSiteTable{rows, count};
    }

    namespace
    {
        uint32_t searchSiteReturn(const FrameRow &row, uint32_t target);
    }

    uint32_t siteReturn(const FrameRow &row, uint32_t target)
    {
        // (The thread's memo of the rows of the frame table: what the search below gives depends on the tables
        // alone, and calls ask it some millions of times in a load. A row made by hand is searched.)
        const uint32_t number = frameRowNumber(&row);
        if (!number)
            return searchSiteReturn(row, target);
        ThreadState::SiteReturn &memo =
            t_ee.siteReturns[(number * 0x9E3779B1u ^ target >> 2) % ThreadState::kSiteReturns];
        if (memo.row != number || memo.target != target)
            memo = ThreadState::SiteReturn{number, target, searchSiteReturn(row, target)};
        return memo.returnAddress;
    }

    namespace
    {
    uint32_t searchSiteReturn(const FrameRow &row, uint32_t target)
    {
        const SiteRange range = sitesOf(row);
        if (const CallSite *site = findSite(range, target & 0x1FFFFFFFu))
            return site->returnAddress;
        // Not a target it calls by address: its one indirect call, if the
        // function has no direct call of it (a function pointer).
        if (const CallSite *site = findSite(range, 0u))
            return site->returnAddress;
        return 0u;
    }
    }

    bool framesOn()
    {
        static const bool on = []
        {
            const char *v = std::getenv("ZX_READABLE_FRAMES");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    namespace
    {
        // ZX_READABLE_LEAN's parts. (The app's boot sets it, collection/settings.cpp gameBootConfig; the environment
        // wins.)
        uint32_t readLeanParts()
        {
            const char *v = zx::boot::text("ZX_READABLE_LEAN");
            if (!v || !*v || std::strcmp(v, "0") == 0)
                return 0u;
            if (std::strcmp(v, "1") == 0)
                return kLeanHeld; // (saves and registers cost little and change more: abi.h)
            uint32_t bits = 0; // a list, to tell the parts' effects apart: held,saves,registers
            const std::string list = v;
            for (const auto &[name, bit] : {std::pair<const char *, uint32_t>{"held", kLeanHeld}, {"saves", kLeanSaves},
                                            {"registers", kLeanRegisters}})
                if (list.find(name) != std::string::npos)
                    bits |= bit;
            return bits;
        }
    }

    uint32_t readBootSwitches()
    {
        // (said when the parts change from the last boot's, or are on)
        const uint32_t before = g_bootSwitches.load(std::memory_order_relaxed);
        const uint32_t bits = readLeanParts();
        if (bits != (before & (kLeanHeld | kLeanSaves | kLeanRegisters)) || bits)
            std::fprintf(stderr, "[readable] lean:%s%s%s%s\n", bits ? "" : " none: the proof scaffolding on",
                         (bits & kLeanHeld) ? " the held-register table off" : "",
                         (bits & kLeanSaves) ? " the frames' register saves off" : "",
                         (bits & kLeanRegisters) ? " guest callees' callee-saved registers off" : "");
        const uint32_t switches = bits | kBootSwitchesRead | (held::wanted() && !(bits & kLeanHeld) ? kBootHeldOn : 0u);
        g_bootSwitches.store(switches, std::memory_order_relaxed);
        return switches;
    }

    namespace
    {
        // ZX_READABLE_LEAN_RANGE=[!]LO-HI,...: lean's saves and registers only in the functions in those ranges
        // (less a `!` range), as ZX_READABLE_HELD_RANGE bisects the held table: which function a difference is.
        bool leanInRange(const FrameRow *layout)
        {
            static const std::vector<std::pair<bool, std::pair<uint32_t, uint32_t>>> ranges = []
            {
                std::vector<std::pair<bool, std::pair<uint32_t, uint32_t>>> list;
                const char *v = std::getenv("ZX_READABLE_LEAN_RANGE");
                for (const char *at = v; at && *at;)
                {
                    const bool except = *at == '!';
                    if (except)
                        ++at;
                    char *end = nullptr;
                    const uint32_t lo = static_cast<uint32_t>(std::strtoul(at, &end, 16));
                    const uint32_t hi = *end == '-' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 16)) : lo + 1u;
                    list.push_back({except, {lo, hi}});
                    at = *end == ',' ? end + 1 : end;
                    if (end == at && *at)
                        break;
                }
                return list;
            }();
            if (ranges.empty())
                return true;
            const uint32_t address = layout ? layout->address : 0u;
            bool in = false, anyIn = false;
            for (const auto &[except, r] : ranges)
            {
                const bool hit = address >= r.first && address < r.second;
                if (!except)
                {
                    anyIn = true;
                    in = in || hit;
                }
                else if (hit)
                    return false;
            }
            return anyIn ? in : true;
        }
    }

    namespace
    {
        // A callee-saved register's value at the running function's entry:
        // the entry registers' model where it has one, else the context.
        uint64_t entryValue(const Frame &frame, int reg)
        {
            const int slot = savedSlot(reg);
            const bool modelled = slot >= 0 && ((frame.view.mask >> slot) & 1u) != 0u;
            if (modelled && !frame.ctx)
                return frame.view.value[slot];
            uint64_t context = 0;
            if (reg >= 32)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &frame.ctx->f[reg - 32], 4);
                context = bits;
            }
            else
                context = gpr64(frame.ctx, reg);
            // (both read, one taken: which slots the model has differs from call to call, a branch on it is a guess)
            return modelled ? frame.view.value[slot] : context;
        }
    }

    uint64_t entryRegister(const Frame &frame, int reg) { return entryValue(frame, reg); }

    // ZX_STACK_POISON=1: every readable function finds the stack below its sp full of 0x5A, so a read of a value
    // left on the stack, by it or by a callee through a pointer into its frame, shows up against a run without it
    // (a stale read has no other witness: the replay answers calls, the A/B sees only dumps and streams).
    //
    // ZX_STACK_POISON_STORES=FILE and ZX_STACK_POISON_FRAMES=FILE (tools/readable_frame_stores.tsv: each
    // function's frame size in the original and the slots it stores there on any path): at the entry of EVERY
    // function of the table, readable or the recompiled original, so `off` gets the same fill as the defaults and
    // the A/B compares the two (career_ab.py stale). STORES fills the slots the function stores: a store the
    // readable keeps writes over the fill, one it drops leaves 0x5A where the original's value was, for a later
    // reader. FRAMES fills the whole frame: a read of a slot not written first. What the original itself reads
    // stale (a slot stored on another path, a byte a deeper frame left) reads the same fill on both sides.
    // (Filled where a call enters through the function table, installStackFill, and where a readable function is
    // entered, enterFrame: a readable call of a readable callee does not go through the table.)
    struct FrameFill
    {
        uint32_t size = 0;
        std::vector<std::pair<uint32_t, uint32_t>> slots; // (offset from the frame's bottom, bytes)
        PS2Runtime::RecompiledFunction original = nullptr;
    };
    enum class FillMode
    {
        None,
        Stores,
        Frames,
    };

    static std::unordered_map<uint32_t, FrameFill> &frameFills()
    {
        static std::unordered_map<uint32_t, FrameFill> fills;
        return fills;
    }

    static FillMode g_fillMode = FillMode::None;

    // A row: ADDRESS<TAB>FRAME SIZE[<TAB>OFFSET+BYTES,...].
    static void readFrameFills(const char *variable, const char *path)
    {
        std::ifstream file(path);
        if (!file)
        {
            std::fprintf(stderr, "%s: cannot read %s\n", variable, path);
            std::abort();
        }
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            char *at = nullptr;
            const uint32_t function = static_cast<uint32_t>(std::strtoul(line.c_str(), &at, 16));
            FrameFill &fill = frameFills()[function];
            fill.size = static_cast<uint32_t>(std::strtoul(at + 1, &at, 10));
            while ((*at == '\t' || *at == ',') && at[1] >= '0' && at[1] <= '9')
            {
                const uint32_t offset = static_cast<uint32_t>(std::strtoul(at + 1, &at, 10));
                const uint32_t bytes = static_cast<uint32_t>(std::strtoul(at + 1, &at, 10));
                fill.slots.emplace_back(offset, bytes);
            }
        }
    }

    static void fillFrame(const FrameFill &fill, uint32_t sp)
    {
        if (g_fillMode == FillMode::Frames)
        {
            std::memset(ee::at<uint8_t>(sp - fill.size), 0x5A, fill.size);
            return;
        }
        for (const auto &[offset, bytes] : fill.slots)
            std::memset(ee::at<uint8_t>(sp - fill.size + offset), 0x5A, bytes);
    }

    // A table entry: the function's fill at its sp as called, then the function.
    static void fillAtEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto found = frameFills().find(ctx->pc);
        if (found == frameFills().end() || !found->second.original)
        {
            std::fprintf(stderr, "ZX_STACK_POISON: entered at 0x%x, no function of the table\n", ctx->pc);
            std::abort();
        }
        fillFrame(found->second, static_cast<uint32_t>(gpr64(ctx, reg::sp)));
        found->second.original(rdram, ctx, runtime);
    }

    void installStackFill(PS2Runtime &runtime)
    {
        if constexpr (!kDevChecks)
            return; // (career_ab.py stale's tool: the dev flavour's)
        const char *stores = std::getenv("ZX_STACK_POISON_STORES");
        const char *frames = std::getenv("ZX_STACK_POISON_FRAMES");
        if (!stores && !frames)
            return;
        if (stores && frames)
        {
            std::fprintf(stderr, "ZX_STACK_POISON_STORES and ZX_STACK_POISON_FRAMES: one at a time\n");
            std::abort();
        }
        g_fillMode = stores ? FillMode::Stores : FillMode::Frames;
        readFrameFills(stores ? "ZX_STACK_POISON_STORES" : "ZX_STACK_POISON_FRAMES", stores ? stores : frames);
        uint32_t wrapped = 0;
        for (auto &[address, fill] : frameFills())
            if (runtime.hasFunction(address))
            {
                fill.original = runtime.lookupFunction(address);
                if (runtime.replaceFunction(address, &fillAtEntry))
                    ++wrapped;
            }
        std::fprintf(stderr, "[stack-fill] %s at the entry of %u functions\n", stores ? "stores" : "frames", wrapped);
    }

    static void poisonDeadStack(const Frame &frame, const FrameRow *layout, uint32_t sp)
    {
        if constexpr (!kDevChecks)
            return;
        if (g_fillMode != FillMode::None)
        {
            if (!layout)
                return;
            const auto found = frameFills().find(layout->address);
            if (found != frameFills().end())
                fillFrame(found->second, sp);
            return;
        }
        static const bool on = std::getenv("ZX_STACK_POISON") != nullptr;
        if (!on || !frame.rdram)
            return;
        const uint32_t bytes = layout && layout->size ? layout->size : 0x100u;
        std::memset(ee::at<uint8_t>(sp - bytes), 0x5A, bytes);
    }

    // One register save as the original's store writes it: sw, sd or sq. An sq
    // (the EE's 128-bit save, strcat's and the SDK's) writes the register's upper
    // half too, which only the context holds: a later function's unset vector
    // lanes read it in the dead stack (BMX XXX's level loading, after strcat).
    static void writeSave(uint8_t *at, const EntrySave &save, uint64_t value, uint64_t high)
    {
        // (Each size its own copy: a size the compiler knows is a store, not a library call.)
        if (save.bytes >= 8)
            std::memcpy(at, &value, 8u);
        else
            std::memcpy(at, &value, 4u);
        if (save.bytes == 16)
            std::memcpy(at + 8, &high, 8u);
    }

    // A saved register's value at the entry (writeSave's value and upper half): an argument register of a readable
    // callee called directly from the call (`direct`: its CallRegs; one the call does not pass is the context's, as
    // the callee's own read of it, ee::callerRegister), any other the entry registers' model or the context.
    // Set by enterSelfCall around its enterFrame: the inner call's arguments are not known (the self-call's own
    // body has them), so its argument registers' prologue stores are not written (none was before frame1).
    static bool argumentRegister(int r)
    {
        return (r >= reg::a0 && r < reg::a0 + kIntArgRegs) || (r >= 32 + reg::f12 && r < 32 + reg::f12 + kFloatArgRegs);
    }

    static void saveValue(const Frame &frame, const EntrySave &save, const CallRegs *direct, uint64_t &value,
                          uint64_t &high)
    {
        const int r = save.reg;
        if (direct && r >= reg::a0 && r < reg::a0 + kIntArgRegs && r - reg::a0 < direct->intCount)
        {
            const int i = r - reg::a0;
            value = direct->a[i];
            high = (direct->qwordArgs & (1u << i)) ? direct->aHigh[i] : 0u;
            return;
        }
        if (direct && r >= 32 + reg::f12 && r < 32 + reg::f12 + kFloatArgRegs && r - 32 - reg::f12 < direct->floatCount)
        {
            value = std::bit_cast<uint32_t>(direct->f[r - 32 - reg::f12]);
            high = 0u;
            return;
        }
        value = entryValue(frame, r);
        high = r < 32 ? gprHigh64(frame.ctx, r) : 0u;
    }

    void enterFrame(FunctionScope &scope, Frame &frame, const FrameRow *layout, uint32_t sp, ThreadState &thread)
    {
        poisonDeadStack(frame, layout, sp);
        // (Member by member, in the caller's scope: no held::State, some hundreds of bytes, zeroed or copied at
        // every call.)
        scope.entered = false;
        scope.heldOwn = false;
        scope.layout = frame.layout;
        scope.fnSp = frame.fnSp;
        scope.stackTop = frame.stackTop;
        scope.calleeSp = frame.calleeSp;
        scope.localCount = frame.localCount;
        scope.localNext = frame.localNext;
        if (!framesOn())
            return;
        scope.entered = true;
        frame.localCount = 0;
        frame.localNext = 0;
        frame.layout = layout;
        frame.fnSp = sp;
        frame.stackTop = sp;
        // A function without a frame of its own makes no calls in the original
        // (its tail calls run at its sp): the gap keeps a readable one's off
        // the caller's.
        frame.calleeSp = sp - (layout && layout->size ? layout->size : kCalleeStackGap);
        if (layout && layout->count && frame.ctx && !(lean(kLeanSaves) && leanInRange(layout)))
        {
            const uint32_t base = sp - layout->size;
            const EntrySave *saves = frameSaves(*layout);
            for (uint32_t i = 0; i < layout->count; ++i)
            {
                if (thread.selfCallFrame && argumentRegister(saves[i].reg))
                    continue;
                uint64_t value = 0, high = 0;
                saveValue(frame, saves[i], thread.directArguments, value, high);
                if (uint8_t *at = ee::at<uint8_t>(base + saves[i].offset))
                    writeSave(at, saves[i], value, high);
            }
        }
        // Its place in the original's calls: the entry (ee/held.h); a readable
        // callee called directly has its arguments from the call. A callee
        // with calls of its own in the table works in a place of its own,
        // here, so the caller's stays as it is; a leaf (no row) has none: the
        // caller's place without its function, which comes back after it. (A
        // place with no function, the frame's first or a leaf's, is the
        // callee's own: only that function comes back after it.)
        held::State &current = frame.held();
        scope.heldFunction = current.function;
        scope.heldFlags = current.flags;
        scope.outerHeld = frame.calleeHeld;
        if (const held::Function *row = held::rowOf(thread, layout))
        {
            if (current.function)
            {
                scope.heldOwn = true;
                frame.calleeHeld = &scope.held;
            }
            held::enter(frame, row, thread.directArguments, thread);
        }
        else
            current.function = nullptr;
        thread.directArguments = nullptr;
    }

    void leaveFrame(Frame &frame, const FunctionScope &scope)
    {
        if (!scope.entered)
            return;
        frame.layout = scope.layout;
        frame.fnSp = scope.fnSp;
        frame.stackTop = scope.stackTop;
        frame.calleeSp = scope.calleeSp;
        frame.localCount = scope.localCount;
        frame.localNext = scope.localNext;
        if (scope.heldOwn)
            frame.calleeHeld = scope.outerHeld;
        else
        {
            frame.held().function = scope.heldFunction;
            frame.held().flags = scope.heldFlags;
        }
    }

    uint32_t frameLocal(uint32_t size, uint32_t alignment)
    {
        const Frame *frame = t_ee.frame;
        const FrameRow *layout = frame ? frame->layout : nullptr;
        if (!layout || !layout->size)
        {
            std::fprintf(stderr, "[readable] an ee::Local in a function with no frame in the frame table\n");
            std::abort();
        }
        Frame &running = *t_ee.frame;
        const uint32_t ordinal = running.localCount++;
        const uint32_t offset = ordinal < layout->localCount ? frameTable().locals[layout->firstLocal + ordinal]
                                                             : (running.localNext + alignment - 1u) & ~(alignment - 1u);
        running.localNext = offset + size;
        if (running.localNext > layout->size)
        {
            std::fprintf(stderr, "[readable] ee::Local %u of 0x%08x does not fit its frame\n", ordinal, layout->address);
            std::abort();
        }
        held::noteLocal(running, offset, size); // (the held table's loads from the frame read inside it)
        return running.fnSp - layout->size + offset;
    }

    void enterDirectFrame(FunctionScope &scope, const FrameRow *layout, bool tail)
    {
        Frame *frame = t_ee.frame;
        if (!frame)
            return;
        // (A frame made by hand, DM2's host probes, has no fnSp: its callee sp.)
        enterFrame(scope, *frame, layout, tail && frame->fnSp ? frame->fnSp : frame->calleeSp, t_ee);
    }

    void leaveDirectFrame(const FunctionScope &scope)
    {
        if (Frame *frame = t_ee.frame)
            leaveFrame(*frame, scope);
    }

    bool safePointsOn()
    {
        static const bool on = []
        {
            const char *v = std::getenv("ZX_READABLE_SAFEPOINTS");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    // Whether a checkpoint is due after `cycles` more, as generated code
    // asks it (eeCheckpointDue: never inside a host-made guest call); off a
    // fiber (or with safe points off) the cycles are only counted.
    namespace
    {
        bool checkDue(const ThreadState &thread, Frame *frame, uint32_t cycles)
        {
            if (thread.suspendable && safePointsOn())
                return frame->runtime->eeCheckpointDue(cycles);
            frame->runtime->eeScheduler().accountCycles(cycles);
            return false;
        }
        // A safe point of the original: `cycles` charged and, if a
        // checkpoint is due, the fiber leaves until the scheduler resumes it.
        // True when it left. (`thread`: t_ee, which the caller has looked up.)
        bool check(ThreadState &thread, uint32_t cycles)
        {
            if (CallHandler *handler = thread.callHandler)
            {
                handler->cycles(cycles);
                // (--safepoints: due there, the frame goes on as resumed.)
                return thread.safePointHook && thread.frame && thread.safePointHook(static_cast<uint32_t>(cycles));
            }
            Frame *frame = thread.frame;
            if (!frame || !frame->runtime)
                return false;
            if (!checkDue(thread, frame, cycles))
                return false;
            yieldSafePoint();
            return true;
        }
    }

    void safePoint(uint32_t cycles) { (void)check(t_ee, cycles); }

    void chargeLoop(uint64_t iterations)
    {
        if (t_ee.callHandler ? !t_ee.safePointHook : (!t_ee.suspendable || !safePointsOn()))
        {
            chargeCycles(32u * iterations);
            return;
        }
        for (uint64_t i = 0; i < iterations; ++i)
            (void)check(t_ee, EeScheduler::kGeneratedCheckpointCycles);
    }

    namespace io
    {
        void hang()
        {
            for (;;)
                chargeLoop(1);
        }
    }

    void loopBack(uint32_t head)
    {
        marked(head, MarkerKind::LoopBack);
        Frame *frame = t_ee.frame;
        if (frame && frame->mode == kBlocks && modelOn())
        {
            // An entry block runs its own branch back to its start in place
            // (32 and a check); any other branch back leaves the slice, and
            // the head's dispatch is charged at its ee::block.
            if (frame->slice != head)
                return;
            frame->paid = true; // the goto: no dispatch
        }
        frame = t_ee.frame;
        if (check(t_ee, EeScheduler::kGeneratedCheckpointCycles) && (frame = t_ee.frame))
        {
            // Resumed at the head through the table: its entry block, already
            // dispatched (the resume slot's dispatch).
            frame->mode = kBlocks;
            frame->slice = head;
            frame->paid = true;
        }
    }

    void loopBackWhole(uint32_t target, uint32_t slice)
    {
        (void)target;
        Frame *frame = t_ee.frame;
        if (frame && frame->mode == kBlocks && modelOn() && frame->slice != slice)
        {
            // Out of the slice to an address the table gives the whole
            // function: dispatched to it, which goes on as a whole.
            frame->mode = kWhole;
            frame->paid = false;
            (void)check(t_ee, EeScheduler::kGuestDispatchCycles);
            return;
        }
        if (check(t_ee, EeScheduler::kGeneratedCheckpointCycles) && (frame = t_ee.frame))
            frame->mode = kWhole; // resumed through the table: the whole function
    }

    void block(uint32_t leader, bool afterCall)
    {
        marked(leader, MarkerKind::Block);
        Frame *frame = t_ee.frame;
        if (!frame || !modelOn())
            return;
        const bool unwound = frame->callUnwound;
        frame->callUnwound = false;
        if (afterCall && unwound)
        {
            // The call came back through the scheduler, which dispatched its
            // return address: this entry block.
            frame->mode = kBlocks;
            frame->slice = leader;
            frame->paid = false;
            return;
        }
        frame->slice = leader;
        if (frame->mode != kBlocks)
            return;
        if (frame->paid)
        {
            frame->paid = false;
            return;
        }
        // The scheduler's dispatch of the slice (8 and a check); if a
        // checkpoint is due it dispatches the slice again later.
        (void)check(t_ee, EeScheduler::kGuestDispatchCycles);
    }

    void afterCall(uint32_t leader)
    {
        Frame *frame = t_ee.frame;
        if (!frame || !modelOn())
            return;
        const bool unwound = frame->callUnwound;
        frame->callUnwound = false;
        if (!unwound)
            return;
        // The scheduler dispatched the return address: this entry block,
        // already paid (the ee::block(leader) that follows charges nothing).
        frame->mode = kBlocks;
        frame->slice = leader;
        frame->paid = true;
    }

    void blockWhole(uint32_t target, uint32_t slice)
    {
        (void)target;
        Frame *frame = t_ee.frame;
        if (!frame || !modelOn())
            return;
        frame->callUnwound = false;
        if (frame->mode != kBlocks || frame->slice == slice)
            return;
        frame->mode = kWhole;
        frame->paid = false;
        (void)check(t_ee, EeScheduler::kGuestDispatchCycles);
    }

    void tableJump(uint32_t target, uint32_t slice, bool entryBlock)
    {
        const Frame *frame = t_ee.frame;
        if (!frame || frame->mode != kBlocks || frame->slice != slice || !modelOn())
            return; // (the whole function: the case in place)
        jumpDispatch(target, entryBlock);
    }

    void jumpDispatch(uint32_t target, bool entryBlock)
    {
        if (entryBlock)
        {
            marked(target, MarkerKind::Block);
            if (t_ee.markerHook)
                t_ee.dispatchedMark = target;
        }
        if (CallHandler *handler = t_ee.callHandler)
        {
            handler->cycles(EeScheduler::kGuestDispatchCycles);
            return;
        }
        Frame *frame = t_ee.frame;
        if (!frame || !frame->runtime)
            return;
        if (!t_ee.suspendable || !safePointsOn())
        {
            chargeCycles(EeScheduler::kGuestDispatchCycles);
            return;
        }
        // dispatchGuestBranch's 8 and check; due or not, the JR unwinds (it
        // returns false with pc at the target), so the fiber leaves once and
        // the scheduler's dispatch enters it again.
        if (!check(t_ee, EeScheduler::kGuestDispatchCycles))
            yieldSafePoint();
        if ((frame = t_ee.frame))
        {
            frame->callUnwound = false;
            if (entryBlock)
            {
                // The target's entry block, dispatched by the scheduler (its
                // ee::block charges nothing more).
                frame->mode = kBlocks;
                frame->slice = target;
                frame->paid = true;
            }
            else
            {
                frame->mode = kWhole;
                frame->paid = false;
            }
        }
    }

    namespace
    {
        // The callee's entry-register model: the published values over the
        // caller's (Frame::view).
        void overlaySaves(Frame &frame)
        {
            const uint32_t mask = frame.next.mask;
            for (uint32_t left = mask; left; left &= left - 1u)
            {
                const int slot = std::countr_zero(left);
                frame.view.value[slot] = frame.next.value[slot];
            }
            frame.view.mask |= mask;
        }
    }

    namespace
    {
        void enterDirect(DirectCall &saved, ThreadState &thread, bool tail, bool checked);
        void leaveDirect(const DirectCall &saved, ThreadState &thread);
        void publishSiteReturn(Frame *frame, uint32_t target);
    }

    DirectCall enterDirectCall(bool tail)
    {
        DirectCall saved;
        enterDirect(saved, t_ee, tail, true);
        return saved;
    }

    namespace
    {
    void enterDirect(DirectCall &saved, ThreadState &thread, bool tail, bool checked)
    {
        Frame *frame = thread.frame;
        if (!frame)
        {
            saved.mode = 0;
            saved.paid = saved.callUnwound = false;
            saved.slice = 0;
            saved.resumes = 0;
            saved.view.mask = saved.view.replaced = 0;
            return;
        }
        // A jal's dispatch (a tail call is a direct C++ call: none). When a
        // checkpoint is due there, the caller unwinds and the scheduler
        // dispatches the callee later, so its return goes through the
        // scheduler too: the resumes are counted from before the check
        // (zgehand5: findRegion's call of its readable callee, -8 at career
        // tick 1094; sphereQueryVisit's, -8 at tick 1789).
        const uint64_t resumes = fiberResumes(thread);
        if (!tail && checked)
            (void)check(thread, EeScheduler::kGuestDispatchCycles);
        else if (!tail)
            thread.callHandler->cycles(EeScheduler::kGuestDispatchCycles);
        saved.mode = frame->mode;
        saved.paid = frame->paid;
        saved.callUnwound = frame->callUnwound;
        saved.slice = frame->slice;
        saved.resumes = resumes;
        frame = thread.frame;
        // The callee's entry registers: the caller's published values over
        // its own model (a tail call's callee gets the caller's entry
        // registers, put back by the original's epilogue). What they replace
        // is kept for leaveDirect: nothing else changes the model before it
        // (a call inside puts back what it replaced).
        saved.view.mask = frame->view.mask;
        saved.view.replaced = tail ? 0u : frame->next.mask;
        for (uint32_t left = saved.view.replaced; left; left &= left - 1u)
        {
            const int slot = std::countr_zero(left);
            saved.view.value[slot] = frame->view.value[slot];
            frame->view.value[slot] = frame->next.value[slot];
        }
        frame->view.mask |= saved.view.replaced;
        frame->next.mask = 0;
        frame->mode = kWhole;
        frame->paid = false;
        frame->callUnwound = false;
        frame->slice = 0;
        if (!tail)
            ++frame->directDepth;
    }
    }

    void leaveDirectCall(const DirectCall &saved) { leaveDirect(saved, t_ee); }

    namespace
    {
    void leaveDirect(const DirectCall &saved, ThreadState &thread)
    {
        Frame *frame = thread.frame;
        if (!frame)
            return;
        --frame->directDepth;
        returnedTo(*frame, frame->view); // (the callee's entry model: the caller's published ra)
        for (uint32_t left = saved.view.replaced; left; left &= left - 1u)
        {
            const int slot = std::countr_zero(left);
            frame->view.value[slot] = saved.view.value[slot];
        }
        frame->view.mask = saved.view.mask;
        frame->mode = saved.mode;
        frame->paid = saved.paid;
        frame->slice = saved.slice;
        frame->callUnwound = false;
        if (fiberResumes(thread) == saved.resumes || !safePointsOn())
            return;
        if (takeTailReturn(thread))
        {
            // The callee's last act, a tail call made as a guest call, came
            // back through the scheduler's dispatch of the resume slot: that
            // was the original's dispatch of the return address (the tail
            // callee returns there by itself), charged already.
            frame->mode = kWhole;
            frame->paid = false;
            frame->callUnwound = true;
            return;
        }
        // The callee's frame unwound: it returned through the scheduler,
        // which dispatches the return address (8 and a check) to the caller,
        // as the table has it (the whole function unless ee::block(...,
        // true) follows).
        frame->mode = kWhole;
        frame->paid = false;
        ++thread.directReturns;
        (void)check(thread, EeScheduler::kGuestDispatchCycles);
        if ((frame = thread.frame))
        {
            frame->mode = kWhole;
            frame->callUnwound = true;
        }
    }
    }

    void enterDirectCallee(DirectCallScope &scope, uint32_t address, const CallRegs &regs, const FrameRow *layout)
    {
        ThreadState &thread = t_ee;
        scope.thread = &thread;
        if (thread.frame && thread.frame->held().function)
            held::beforeCall(*thread.frame, regs, thread);
        publishSiteReturn(thread.frame, address);
        enterDirect(scope.call, thread, false, true);
        // (enterDirectFrame: the callee at the caller's callee sp)
        thread.directArguments = &regs;
        if (Frame *frame = thread.frame)
            enterFrame(scope.frame, *frame, layout, frame->calleeSp, thread);
        thread.directArguments = nullptr;
    }

    void leaveDirectCallee(const DirectCallScope &scope, uint64_t v0, uint32_t f0, uint8_t known)
    {
        ThreadState &thread = *scope.thread;
        if (Frame *frame = thread.frame)
            leaveFrame(*frame, scope.frame);
        leaveDirect(scope.call, thread);
        if (thread.frame && held::readsAfterCall(thread.frame->held()))
            held::afterCall(*thread.frame, v0, 0u, f0, known, thread);
    }

    void enterSelfCall(SelfCall &call)
    {
        // The original's jal of itself (ee/held.h): its arguments are not known here.
        if (t_ee.frame && t_ee.frame->layout && t_ee.frame->held().function)
        {
            CallRegs regs;
            regs.target = t_ee.frame->layout->address;
            held::beforeCall(*t_ee.frame, regs);
        }
        if (t_ee.frame && t_ee.frame->layout)
            publishSiteReturn(t_ee.frame->layout->address); // (the drafts publish it)
        call.interruptions = t_ee.callHandler ? t_ee.callHandler->interruptions() : 0u;
        if (t_ee.frame)
        {
            // The inner call's entry registers: the published values.
            call.view = t_ee.frame->view;
            overlaySaves(*t_ee.frame);
            t_ee.frame->next.mask = 0;
        }
        if (t_ee.frame && t_ee.frame->mode == kBlocks && modelOn())
        {
            // A slice's jal: dispatched like a readable callee called
            // directly (game::call), the whole function in it. Under
            // zx-fncheck (--safepoints: a frame goes slice by slice only
            // after an interruption) the dispatch's 8 are charged but it is
            // not a safe point: the replay of the original runs the call
            // inline, its SliceEmu charging the 8 and returning to the slice
            // in place (drafter23: resolvePath's 62 differing interruptions
            // were this path skipped, the inner call sharing the outer's
            // slice).
            enterDirect(call.saved, t_ee, false, t_ee.callHandler == nullptr);
            call.dispatched = true;
        }
        // The inner call's frame, below the caller's (its saves from the
        // published values).
        if (Frame *frame = t_ee.frame)
        {
            t_ee.selfCallFrame = true;
            enterFrame(call.scope, *frame, frame->layout, frame->calleeSp);
            t_ee.selfCallFrame = false;
            held::forgetArguments(*frame);
        }
    }

    namespace
    {
        // The scheduler dispatched a self-call's return address: an entry
        // block there goes on slice by slice (already dispatched), else the
        // whole function.
        void selfCallReturned(uint32_t returnBlock)
        {
            Frame *frame = t_ee.frame;
            if (!frame || !modelOn())
                return;
            frame->callUnwound = false;
            frame->mode = returnBlock ? kBlocks : kWhole;
            frame->slice = returnBlock;
            frame->paid = returnBlock != 0;
        }
    }

    void leaveSelfCall(const SelfCall &call, uint32_t returnBlock)
    {
        if (t_ee.frame)
        {
            leaveFrame(*t_ee.frame, call.scope);
            held::afterCall(*t_ee.frame, 0u, 0u, 0u, 0u); // (its results: not known here)
            // ra as the inner call's epilogue leaves it: the return address
            // published for it (its entry model).
            returnedTo(*t_ee.frame, t_ee.frame->view);
        }
        if (call.dispatched)
        {
            // In place, unless the inner call unwound: then the scheduler
            // dispatched the return address (leaveDirectCall charged it).
            leaveDirectCall(call.saved);
            // The caller's entry model back (leaveDirectCall restores the one
            // enterDirect saw, which already had the inner call's published
            // values: before drafter25 they stayed in the caller's model).
            if (t_ee.frame)
                t_ee.frame->view = call.view;
            if (returnBlock)
                afterCall(returnBlock);
            return;
        }
        if (t_ee.frame)
            t_ee.frame->view = call.view;
        if (t_ee.callHandler)
        {
            // zx-fncheck's replay of the original follows the return in
            // place, uncharged (as the recording's cycles leave out the
            // scheduler's dispatch of the return address). After its
            // --safepoints interruption the model goes on as that dispatch
            // has it, the entry block at the return address or the whole
            // function, as its SliceEmu does (drafter23).
            if (t_ee.frame && t_ee.callHandler->interruptions() != 0u)
                selfCallReturned(returnBlock);
            return;
        }
        if (t_ee.frame && t_ee.frame->mode == kBlocks)
        {
            // The inner call was interrupted and went on slice by slice: it
            // has unwound already, and its last slice's return is the
            // scheduler's dispatch of the return address.
            (void)check(t_ee, EeScheduler::kGuestDispatchCycles);
            selfCallReturned(returnBlock);
            return;
        }
        // The inner return leaves the C++ function: the guest thread unwinds
        // and the scheduler dispatches the return address.
        if (t_ee.suspendable && safePointsOn())
            yieldSafePoint();
        else
            chargeCycles(EeScheduler::kGuestDispatchCycles);
        selfCallReturned(returnBlock);
    }

    void calleeSaves(std::initializer_list<CalleeSave> saves)
    {
        Frame *frame = t_ee.frame;
        if (!frame)
            return;
        // (Under zx-fncheck too: it answers the call from the recording and
        // compares what was published with the registers the original held
        // there, its saves_differ note; drafter23.)
        for (const CalleeSave &save : saves)
        {
            const int slot = savedSlot(save.reg);
            if (slot < 0)
                continue;
            frame->next.mask |= 1u << slot;
            frame->next.value[slot] = save.value;
        }
    }

    void passRegisters(std::initializer_list<CalleeSave> regs)
    {
        Frame *frame = t_ee.frame;
        if (!frame || !frame->ctx)
            return;
        for (const CalleeSave &r : regs)
            if (r.reg > 0 && r.reg < 32)
                setGpr64(frame->ctx, r.reg, r.value);
    }

    KeptRegs applySaves(Frame &frame, R5900Context *ctx)
    {
        KeptRegs kept;
        if (lean(kLeanRegisters) && leanInRange(frame.layout))
        {
            // Lean's registers part: the guest callee finds the context's own registers (the entry values).
            frame.next.mask = 0;
            kept.mask = 0;
            return kept;
        }
        // Not ra: the caller sets it for the guest call (CallRegs::returnAddress,
        // publishedReturn: before this clears Frame::next), and it is not put
        // back after (the original's callee returns with it). The
        // registers the function has not changed are its entry values: the
        // entry registers' model where it has one (a readable function called
        // directly, a self-call), else the context's own (frame table on).
        const uint32_t viewMask = framesOn() ? frame.view.mask : 0u;
        kept.mask = (frame.next.mask | viewMask) & ~(1u << savedSlot(reg::ra));
        const uint32_t nextMask = frame.next.mask;
        frame.next.mask = 0;
        for (uint32_t left = kept.mask; left; left &= left - 1u)
        {
            const int slot = std::countr_zero(left);
            const int r = savedSlotReg(slot);
            const uint64_t value = (nextMask & (1u << slot)) ? frame.next.value[slot] : frame.view.value[slot];
            if (r >= 32)
            {
                kept.fpr[slot] = ctx->f[r - 32];
                const uint32_t bits = static_cast<uint32_t>(value);
                std::memcpy(&ctx->f[r - 32], &bits, 4);
            }
            else
            {
                kept.gpr[slot] = ctx->r[r];
                setGpr64(ctx, r, value);
            }
        }
        return kept;
    }

    void restoreSaves(const KeptRegs &kept, R5900Context *ctx)
    {
        for (uint32_t left = kept.mask; left; left &= left - 1u)
        {
            const int slot = std::countr_zero(left);
            const int r = savedSlotReg(slot);
            if (r >= 32)
                ctx->f[r - 32] = kept.fpr[slot];
            else
                ctx->r[r] = kept.gpr[slot];
        }
    }

    void StackFrame::saveEntry(std::initializer_list<EntrySave> saves) const
    {
        Frame *frame = m_frame;
        if (!frame || !frame->ctx || (lean(kLeanSaves) && leanInRange(frame->layout)))
            return;
        for (const EntrySave &save : saves)
        {
            uint64_t value = 0;
            const int slot = savedSlot(save.reg);
            if (slot >= 0 && (frame->view.mask & (1u << slot)))
                value = frame->view.value[slot];
            else if (save.reg >= 32)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &frame->ctx->f[save.reg - 32], 4);
                value = bits;
            }
            else
                value = gpr64(frame->ctx, save.reg);
            if (uint8_t *at = ee::at<uint8_t>(m_base + save.offset))
                writeSave(at, save, value, save.reg < 32 ? gprHigh64(frame->ctx, save.reg) : 0u);
        }
    }

    void callGuest(CallRegs &regs)
    {
        Frame *frame = t_ee.frame;
        if (!frame || !frame->runtime)
        {
            {
            static uint64_t count = 0;
            ++count;
            if ((count & (count - 1)) == 0) // (the 1st, 2nd, 4th ... time)
                std::fprintf(stderr, "[readable] call to 0x%x outside a readable function (%llu times)\n", regs.target,
                             static_cast<unsigned long long>(count));
        }
            return;
        }
        R5900Context *ctx = frame->ctx;
        // The dispatch a generated call charges (callGuestFunction does not);
        // a tail call, a direct C++ call in generated code, charges none.
        if (!regs.tail && !regs.kernel)
            frame->runtime->eeScheduler().accountCycles(8u);
        // callGuestFunction keeps (and afterwards restores) the context as
        // it is here, arguments included.
        regs.into(ctx);
        t_ee.v0 = t_ee.v1 = 0;
        t_ee.f0 = 0.0f;
        uint32_t v0 = 0;
        // Below the readable function's locals (StackFrame: the original's
        // frame, so the callee's sp is the original's), else kCalleeStackGap
        // below its sp (Frame::calleeSp starts there).
        // A tail call runs at the function's own sp: the original's epilogue
        // has released its frame (frame table on).
        const uint32_t calleeSp = regs.tail && framesOn() && frame->fnSp ? frame->fnSp : frame->calleeSp;
        t_ee.vu0 = Vu0State::of(*ctx);
        // The caller's working callee-saved registers, as the original has
        // them at its jal (the callee's saves write them).
        if (regs.tail)
            frame->next.mask = 0; // (a tail call: the entry values, put back by the epilogue)
        // The callee's ra: the original's return address where published (a
        // tail call's is the function's own, ra as its caller left it).
        regs.returnAddress = regs.tail ? tailReturn(*frame) : callReturn(*frame, regs.target);
        const KeptRegs kept = applySaves(*frame, ctx);
        t_ee.guestReturn = regs.returnAddress;
        if (!frame->runtime->callGuestFunction(frame->rdram, ctx, &trampoline, regs.target, {}, calleeSp, &v0))
            {
            static uint64_t count = 0;
            ++count;
            if ((count & (count - 1)) == 0) // (the 1st, 2nd, 4th ... time)
                std::fprintf(stderr, "[readable] call to 0x%x did not return (%llu times)\n", regs.target,
                             static_cast<unsigned long long>(count));
        }
        t_ee.guestReturn = 0;
        restoreSaves(kept, ctx);
        // ra as the callee returned with it (callGuestFunction gave the
        // context back as it was).
        if (regs.returnAddress)
            setGpr64(ctx, reg::ra, regs.returnAddress);
        // callGuestFunction gives the context back as it was; VU0 is as the
        // callee left it, as after a call in the original.
        t_ee.vu0.into(*ctx);
        regs.v0 = t_ee.v0;
        regs.v1 = t_ee.v1;
        regs.f0 = t_ee.f0;
    }
}
