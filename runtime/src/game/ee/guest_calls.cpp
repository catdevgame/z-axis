// PS2X_GUEST_CALL: guest functions called at a chosen vsync, for recording
// paths no input reaches (docs/DEVTOOLS.md). The game's input
// recorder (replayFrame) records and plays back only after
// setReplayMode is called with 1 or 2, which only the debug code
// cycleReplayMode does, and nothing calls that; a route sets the modes this way:
//
//   PS2X_GUEST_CALL=HOOK@VSYNC:TARGET[:A0[:A1[:A2[:A3]]]][,...]
//
// At the first call of HOOK at or after vsync VSYNC, TARGET is called with
// a0 and a1 (and a2, a3 when given: paths6, e.g. scriptSendEvent's three)
// before HOOK runs, on the EE thread, from HOOK's entry (its caller's
// registers, the stack below its caller's sp), all registers kept.
// Off unless set; it changes what the game does (a test route only).
// Example (the career input, the Movie Lot): record from tick 1500, play
// back from 2100, off at 2700:
//   PS2X_GUEST_CALL=replayFrame@1500:setReplayMode:1,replayFrame@2100:setReplayMode:2,replayFrame@2700:setReplayMode:0
//
// Strings and buffers (paths7): an argument `$N` passes the address of a
// copy of the N-th string of PS2X_GUEST_STRINGS (strings separated by `|`,
// so they may hold `:` and `,`), and `#N` the address of N zero bytes (an
// out parameter). They are written on the guest stack below HOOK's caller's
// sp, the callee's stack below them, so they last for the call only. Up to
// eight arguments (a0-a3, then t0-t3) with these. Example: zgui's library
// load with a completion name (an asynchronous load, which only DM2's menus
// and AI's leftover MENU/MAIN.ZSC ask for):
//   PS2X_GUEST_STRINGS='P7Test|uilib:PauseMenu.Zif|LoadComplete'
//   PS2X_GUEST_CALL=renderFrame@900:loadZguiLibrary:$0:$1:$2
// An argument `^N` passes what the N-th call of PS2X_GUEST_CALL (from 0, in
// the order given) returned in v0 (0 before it ran): a handle one call opens
// and the next ones use. Calls due at the same hook entry run in that order.
// VSYNC `+N`: N vsyncs after the item before it ran (and not before), so a
// sequence can start at whatever vsync a hook first runs (a state's own
// function as HOOK: the call made while the skater is in that state).
// TARGET `~ADDR` (paths8): a call that may wait. The plain call runs inside the
// hook, where a wait (the memory card device's, a semaphore, a vsync) throws to
// the scheduler and the call never comes back (FAILED); `~` makes it an
// invocation on the hook's thread instead (the scheduler's own kind of guest
// call: its own stack, a thread-like context that may wait and be resumed), and
// the thread goes on at HOOK once it has returned (such a call may also take `F1.5` single-precision and `L0x..`
// 64-bit arguments, see placeArguments). Its v0 is logged when it
// does ("[guest-call] ... returned"), and ^N passes it on as for any call. At
// most one `~` item runs per hook entry; the others follow at the next entry.

#include "ee/guest_calls.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace game::guest_calls
{
    namespace
    {
        struct Pending
        {
            uint64_t vsync;
            uint32_t target;
            uint32_t a0, a1;
            bool done;
            uint32_t a2 = 0, a3 = 0;
            int extra = 0; // a2 (1) and a3 (2) given
            // paths7: the arguments as given when one is a string ($N) or a
            // buffer (#N), or there are more than four; empty otherwise
            std::vector<std::string> raw;
            size_t index = 0; // its place in PS2X_GUEST_CALL
            bool relative = false; // VSYNC was +N: due N vsyncs after the item before ran
            // paths8: TARGET was `~ADDR`: the call is made as an invocation on the
            // thread (EeScheduler::invokeCurrent), so it may wait (a semaphore,
            // a vsync, an I/O completion) and the thread goes on at HOOK after it
            bool invoke = false;
            // paths8: TARGET `W`, `H` or `B` (HOOK@VSYNC:W:ADDRESS:VALUE): not a call but a word, halfword or byte stored
            // (both arguments as for a call: numbers, ^N, %EXPR), for building structures a call needs (a FILE)
            char store = 0;
        };

        std::vector<std::string> g_strings; // PS2X_GUEST_STRINGS
        std::vector<uint32_t> g_returned;   // each call's v0 (^N), by index
        std::vector<uint64_t> g_ranAt;      // the vsync each item ran at (+N), by index; ~0 not yet

        R5900Context *g_hookCtx = nullptr; // the hook's registers while its items' arguments are made (`@N`)

        // `%EXPR`: sum := term ('+' term)*; term := '*' term | '(' sum ')' | '@' N | number. `@N` (paths8): general
        // register N as the hook was entered (`@4`: a0, the handler's note when HOOK is a message handler).
        uint32_t evalSum(uint8_t *rdram, const char *&p);
        uint32_t evalTerm(uint8_t *rdram, const char *&p)
        {
            if (*p == '*')
            {
                ++p;
                const uint32_t at = evalTerm(rdram, p);
                uint32_t word = 0;
                if (at != 0u && at < 0x02000000u)
                    std::memcpy(&word, getMemPtr(rdram, at), sizeof(word));
                return word;
            }
            if (*p == '^')
            {
                ++p;
                char *end = nullptr;
                const unsigned long n = std::strtoul(p, &end, 10);
                p = end && end != p ? end : p + std::strlen(p);
                return n < g_returned.size() ? g_returned[n] : 0u;
            }
            if (*p == '@')
            {
                ++p;
                char *end = nullptr;
                const unsigned long n = std::strtoul(p, &end, 10);
                p = end && end != p ? end : p + std::strlen(p);
                return g_hookCtx && n < 32 ? GPR_U32(g_hookCtx, static_cast<int>(n)) : 0u;
            }
            if (*p == '(')
            {
                ++p;
                const uint32_t v = evalSum(rdram, p);
                if (*p == ')')
                    ++p;
                return v;
            }
            char *end = nullptr;
            const uint32_t v = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            p = end && end != p ? end : p + std::strlen(p);
            return v;
        }
        uint32_t evalSum(uint8_t *rdram, const char *&p)
        {
            uint32_t v = evalTerm(rdram, p);
            while (*p == '+')
            {
                ++p;
                v += evalTerm(rdram, p);
            }
            return v;
        }

        // The arguments of a call with strings or buffers: each `$N` / `#N`
        // copied below sp (16-byte aligned); *callSp the callee's stack below them.
        // `F1.5` (paths8, `~` calls only): a single-precision argument in the next of $f12, $f13, ... (this ABI counts
        // the integer and the floating-point arguments apart: scalbnf(x, n) has x in $f12 and n in $a0); `L0x...`: a
        // 64-bit integer argument (a soft-float double's bits: libgcc's), its low word kept in the list and the whole
        // value in *wide by position.
        std::vector<uint32_t> placeArguments(uint8_t *rdram, const std::vector<std::string> &raw, uint32_t sp,
                                             uint32_t *callSp, std::vector<float> *floats = nullptr,
                                             std::vector<std::pair<size_t, uint64_t>> *wide = nullptr)
        {
            std::vector<uint32_t> out;
            uint32_t cursor = sp;
            for (const std::string &a : raw)
            {
                if (a.size() > 1 && a[0] == 'F' && floats)
                {
                    floats->push_back(std::strtof(a.c_str() + 1, nullptr));
                }
                else if (a.size() > 1 && a[0] == 'L' && wide)
                {
                    const uint64_t value = std::strtoull(a.c_str() + 1, nullptr, 0);
                    wide->push_back({out.size(), value});
                    out.push_back(static_cast<uint32_t>(value));
                }
                else if (a.size() > 1 && a[0] == '^')
                {
                    const unsigned long n = std::strtoul(a.c_str() + 1, nullptr, 10);
                    out.push_back(n < g_returned.size() ? g_returned[n] : 0u);
                }
                else if (a.size() > 2 && a[0] == '*' && a[1] == '^')
                {
                    const unsigned long n = std::strtoul(a.c_str() + 2, nullptr, 10);
                    const uint32_t at = n < g_returned.size() ? g_returned[n] : 0u;
                    uint32_t word = 0;
                    if (at != 0u)
                        std::memcpy(&word, getMemPtr(rdram, at), sizeof(word));
                    out.push_back(word);
                }
                else if (a.size() > 1 && a[0] == '%')
                {
                    const char *p = a.c_str() + 1;
                    out.push_back(evalSum(rdram, p));
                }
                else if (a.size() > 1 && a[0] == '=')
                {
                    // `=V` (a block of 16 bytes with the word V first), or `=V0/V1/V2/...` (paths8: up to 64
                    // words, the block rounded up to 16 bytes, the rest zero)
                    std::vector<uint32_t> words;
                    const char *w = a.c_str() + 1;
                    while (*w && words.size() < 64u)
                    {
                        char *end = nullptr;
                        words.push_back(static_cast<uint32_t>(std::strtoul(w, &end, 0)));
                        w = end && end != w ? end : w + std::strlen(w);
                        if (*w == '/')
                            ++w;
                    }
                    if (words.empty())
                        words.push_back(0u);
                    const uint32_t size = (static_cast<uint32_t>(words.size()) * 4u + 15u) & ~15u;
                    cursor -= size;
                    uint8_t *dst = getMemPtr(rdram, cursor);
                    std::memset(dst, 0, size);
                    std::memcpy(dst, words.data(), words.size() * sizeof(uint32_t));
                    out.push_back(cursor);
                }
                else if (a.size() > 1 && (a[0] == '$' || a[0] == '#'))
                {
                    const unsigned long n = std::strtoul(a.c_str() + 1, nullptr, 10);
                    std::string bytes;
                    if (a[0] == '$')
                        bytes = n < g_strings.size() ? g_strings[n] : std::string();
                    else
                        bytes.assign(n, '\0');
                    const uint32_t size = (static_cast<uint32_t>(bytes.size()) + 1u + 15u) & ~15u;
                    cursor -= size;
                    uint8_t *dst = getMemPtr(rdram, cursor);
                    std::memset(dst, 0, size);
                    std::memcpy(dst, bytes.data(), bytes.size());
                    out.push_back(cursor);
                }
                else
                    out.push_back(static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 0)));
            }
            *callSp = (cursor - 16u) & ~15u;
            return out;
        }

        bool callWith(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t target,
                      const std::vector<uint32_t> &a, uint32_t sp, uint32_t *v0)
        {
            auto at = [&](size_t i) { return i < a.size() ? a[i] : 0u; };
            switch (a.size())
            {
            case 0: return runtime->callGuestFunction(rdram, ctx, target, {}, sp, v0);
            case 1: return runtime->callGuestFunction(rdram, ctx, target, {at(0)}, sp, v0);
            case 2: return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1)}, sp, v0);
            case 3: return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1), at(2)}, sp, v0);
            case 4: return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1), at(2), at(3)}, sp, v0);
            case 5: return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1), at(2), at(3), at(4)}, sp, v0);
            case 6:
                return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1), at(2), at(3), at(4), at(5)}, sp, v0);
            case 7:
                return runtime->callGuestFunction(rdram, ctx, target, {at(0), at(1), at(2), at(3), at(4), at(5), at(6)},
                                                  sp, v0);
            default:
                return runtime->callGuestFunction(rdram, ctx, target,
                                                  {at(0), at(1), at(2), at(3), at(4), at(5), at(6), at(7)}, sp, v0);
            }
        }

        struct Hook
        {
            PS2Runtime::RecompiledFunction previous = nullptr;
            std::vector<Pending> calls;
        };

        std::map<uint32_t, Hook> g_hooks;

        void hookEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t address = ctx->pc;
            const auto it = g_hooks.find(address);
            if (it == g_hooks.end())
                return;
            Hook &hook = it->second;
            const uint64_t vsync = runtime->eeScheduler().currentVSyncTick();
            g_hookCtx = ctx;
            for (Pending &p : hook.calls)
            {
                if (p.done)
                    continue;
                if (p.relative)
                {
                    const uint64_t before = p.index > 0 ? g_ranAt[p.index - 1] : 0u;
                    if (before == ~0ull || vsync < before + p.vsync)
                        continue;
                }
                else if (vsync < p.vsync)
                    continue;
                g_ranAt[p.index] = vsync;
                p.done = true;
                uint32_t v0 = 0;
                const uint32_t sp = GPR_U32(ctx, 29) & ~15u;
                if (p.store)
                {
                    uint32_t callSp = sp;
                    const std::vector<uint32_t> args = placeArguments(rdram, p.raw, sp, &callSp);
                    const uint32_t address = args.size() > 0 ? args[0] : 0u;
                    const uint32_t value = args.size() > 1 ? args[1] : 0u;
                    if (address != 0u && address < 0x02000000u)
                    {
                        uint8_t *dst = getMemPtr(rdram, address);
                        if (p.store == 'W')
                            std::memcpy(dst, &value, 4);
                        else if (p.store == 'H')
                            std::memcpy(dst, &value, 2);
                        else
                            std::memcpy(dst, &value, 1);
                    }
                    if (p.index < g_returned.size())
                        g_returned[p.index] = value;
                    std::fprintf(stderr, "[guest-call] vsync %llu: store %c 0x%x = 0x%x before 0x%x\n",
                                 static_cast<unsigned long long>(vsync), p.store, address, value, address);
                    continue;
                }
                if (p.invoke)
                {
                    std::vector<uint32_t> args;
                    std::vector<float> floats;
                    std::vector<std::pair<size_t, uint64_t>> wide;
                    uint32_t callSp = sp;
                    if (!p.raw.empty())
                        args = placeArguments(rdram, p.raw, sp, &callSp, &floats, &wide);
                    else
                        args = {p.a0, p.a1, p.a2, p.a3};
                    std::string shown;
                    for (size_t i = 0; i < args.size(); ++i)
                    {
                        char b[16];
                        std::snprintf(b, sizeof(b), "%s0x%x", i ? ", " : "", args[i]);
                        shown += b;
                    }
                    std::fprintf(stderr, "[guest-call] vsync %llu: 0x%x(%s) before 0x%x: invoked\n",
                                 static_cast<unsigned long long>(vsync), p.target, shown.c_str(), address);
                    EeScheduler &ee = runtime->eeScheduler();
                    GuestInvocation invocation{};
                    invocation.kind = GuestInvocationKind::HleCall;
                    invocation.context = *ctx;
                    for (size_t i = 0; i < args.size() && i < 8u; ++i)
                        SET_GPR_U32(&invocation.context, static_cast<int>(4 + i), args[i]);
                    for (const auto &w : wide)
                        if (w.first < 8u)
                            SET_GPR_U64(&invocation.context, static_cast<int>(4 + w.first), w.second);
                    for (size_t i = 0; i < floats.size() && i < 8u; ++i)
                        invocation.context.f[12 + i] = floats[i];
                    SET_GPR_U32(&invocation.context, 29, ee.invocationStackTop() - 0x40u);
                    SET_GPR_U32(&invocation.context, 31, 0u);
                    invocation.context.pc = p.target;
                    invocation.context.in_delay_slot = false;
                    const size_t index = p.index;
                    const uint32_t target = p.target;
                    invocation.onComplete = [index, target, address](const R5900Context &done, R5900Context &)
                    {
                        const uint32_t result = getRegU32(&done, 2);
                        if (index < g_returned.size())
                            g_returned[index] = result;
                        std::fprintf(stderr, "[guest-call] 0x%x before 0x%x: returned, v0 0x%x\n", target, address,
                                     result);
                    };
                    // the thread goes on at HOOK (its previous function, once every item is done)
                    ctx->pc = address;
                    ee.invokeCurrent(std::move(invocation));
                }
                if (!p.raw.empty())
                {
                    uint32_t callSp = sp;
                    const std::vector<uint32_t> args = placeArguments(rdram, p.raw, sp, &callSp);
                    const bool ok = callWith(runtime, rdram, ctx, p.target, args, callSp, &v0);
                    if (p.index < g_returned.size())
                        g_returned[p.index] = v0;
                    std::string shown;
                    for (size_t i = 0; i < args.size(); ++i)
                    {
                        char b[16];
                        std::snprintf(b, sizeof(b), "%s0x%x", i ? ", " : "", args[i]);
                        shown += b;
                    }
                    std::fprintf(stderr, "[guest-call] vsync %llu: 0x%x(%s) before 0x%x: %s, v0 0x%x\n",
                                 static_cast<unsigned long long>(vsync), p.target, shown.c_str(), address,
                                 ok ? "returned" : "FAILED", v0);
                    continue;
                }
                const bool ok = p.extra == 0 ? runtime->callGuestFunction(rdram, ctx, p.target, {p.a0, p.a1}, sp, &v0)
                                : p.extra == 1 ? runtime->callGuestFunction(rdram, ctx, p.target, {p.a0, p.a1, p.a2}, sp, &v0)
                                               : runtime->callGuestFunction(rdram, ctx, p.target, {p.a0, p.a1, p.a2, p.a3}, sp, &v0);
                if (p.index < g_returned.size())
                    g_returned[p.index] = v0;
                std::fprintf(stderr, "[guest-call] vsync %llu: 0x%x(0x%x, 0x%x) before 0x%x: %s, v0 0x%x\n",
                             static_cast<unsigned long long>(vsync), p.target, p.a0, p.a1, address,
                             ok ? "returned" : "FAILED", v0);
            }
            ctx->pc = address;
            hook.previous(rdram, ctx, runtime);
        }

        uint32_t number(const std::string &s)
        {
            return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 0));
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *spec = std::getenv("PS2X_GUEST_CALL");
        if (!spec || !*spec)
            return;
        std::string all(spec);
        if (const char *strings = std::getenv("PS2X_GUEST_STRINGS"))
        {
            std::string s(strings);
            size_t from = 0;
            while (true)
            {
                const size_t bar = s.find('|', from);
                g_strings.push_back(s.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos)
                    break;
                from = bar + 1;
            }
        }
        size_t start = 0;
        while (start < all.size())
        {
            size_t end = all.find(',', start);
            if (end == std::string::npos)
                end = all.size();
            const std::string item = all.substr(start, end - start);
            start = end + 1;
            const size_t index = g_returned.size(); // ^N counts every item given
            g_returned.push_back(0u);
            g_ranAt.push_back(~0ull);
            // HOOK@VSYNC:TARGET[:A0[:A1[:A2[:A3]]]]
            const size_t at = item.find('@');
            if (at == std::string::npos)
            {
                std::fprintf(stderr, "[guest-call] not HOOK@VSYNC:TARGET[:A0[:A1]]: %s\n", item.c_str());
                continue;
            }
            std::vector<std::string> parts;
            size_t p = at + 1;
            while (p <= item.size())
            {
                size_t q = item.find(':', p);
                if (q == std::string::npos)
                    q = item.size();
                parts.push_back(item.substr(p, q - p));
                p = q + 1;
            }
            if (parts.size() < 2)
            {
                std::fprintf(stderr, "[guest-call] no target: %s\n", item.c_str());
                continue;
            }
            const uint32_t hookAddress = number(item.substr(0, at));
            const char store = (parts[1] == "W" || parts[1] == "H" || parts[1] == "B") ? parts[1][0] : 0;
            const bool invoke = !parts[1].empty() && parts[1][0] == '~';
            if (invoke)
                parts[1].erase(0, 1);
            Pending call{std::strtoull(parts[0].c_str(), nullptr, 0), store ? 0u : number(parts[1]),
                         parts.size() > 2 ? number(parts[2]) : 0u, parts.size() > 3 ? number(parts[3]) : 0u, false};
            if (parts.size() > 4)
            {
                call.a2 = number(parts[4]);
                call.extra = 1;
            }
            if (parts.size() > 5)
            {
                call.a3 = number(parts[5]);
                call.extra = 2;
            }
            bool data = parts.size() > 6;
            for (size_t i = 2; i < parts.size(); ++i)
                data = data || (parts[i].size() > 1 && (parts[i][0] == '$' || parts[i][0] == '#' || parts[i][0] == '^' ||
                                                        parts[i][0] == '*' || parts[i][0] == '=' || parts[i][0] == '%' ||
                                                        (parts[i].size() > 1 && invoke && (parts[i][0] == 'F' || parts[i][0] == 'L'))));
            if (data)
                call.raw.assign(parts.begin() + 2, parts.begin() + std::min<size_t>(parts.size(), 10));
            call.index = index;
            call.invoke = invoke;
            call.store = store;
            if (store)
                call.raw.assign(parts.begin() + 2, parts.begin() + std::min<size_t>(parts.size(), 4));
            if (!parts[0].empty() && parts[0][0] == '+')
            {
                call.relative = true;
                call.vsync = std::strtoull(parts[0].c_str() + 1, nullptr, 0);
            }
            if (!runtime.hasFunction(hookAddress) || (!store && !runtime.hasFunction(call.target)))
            {
                std::fprintf(stderr, "[guest-call] no function at 0x%x or 0x%x\n", hookAddress, call.target);
                continue;
            }
            Hook &hook = g_hooks[hookAddress];
            if (!hook.previous)
            {
                hook.previous = runtime.lookupFunction(hookAddress);
                if (!runtime.replaceFunction(hookAddress, &hookEntry))
                {
                    g_hooks.erase(hookAddress);
                    continue;
                }
            }
            hook.calls.push_back(call);
            std::fprintf(stderr, "[guest-call] 0x%x(0x%x, 0x%x) before 0x%x from vsync %llu\n", call.target, call.a0,
                         call.a1, hookAddress, static_cast<unsigned long long>(call.vsync));
        }
    }
}
