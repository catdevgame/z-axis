// Suspendable readable calls (see Suspendable in abi.h).
//
// The generated code survives a wait because every function can be
// resumed at each of its call sites: a waiting syscall unwinds the host
// stack to the EE scheduler, which later re-enters the guest thread at
// the saved pc. A readable function cannot be re-entered halfway, so one
// whose callees can wait runs on a fiber instead, and its calls are real
// guest calls:
//
//   1. entrySuspendable starts the function on a fiber.
//   2. At ee::call the fiber leaves. The host side sets up the call in the
//      guest context (arguments, sp below the function's frame, ra = the
//      resume slot) and dispatches it as generated code does. A callee
//      that returns straight away gives its results back to the fiber.
//   3. A callee that waits unwinds the guest thread to the scheduler (as
//      generated code does: pc stays where it is to be resumed); the fiber
//      stays waiting under the callee's sp. When the callee returns to the
//      resume slot, that fiber is entered again with the results.
//   4. When the function returns, its result goes to the context and pc to
//      its caller's return address.
//
// A callee that is itself a readable function runs on its caller's fiber
// instead of one of its own (callOnFiber): the same guest call, with the same
// steps but no switches, and the same state machine if it does leave. A
// level load makes 3.5 million such calls, and about 1 in 100 unwinds.

#include "abi.h"
#include "fiber.h"
#include "game_id.h"

#include "machine.h"
#include "runtime/ee_scheduler.h"
#include "runtime/function_table.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ee
{
    struct Suspendable
    {
        enum class State
        {
            Running,
            Calling,
            Yielded, // at a safe point with a checkpoint due (ee::safePoint)
            Done,
        };
        fiber::Context *fiber = nullptr;
        Frame frame;
        void (*body)(Frame &frame) = nullptr;
        State state = State::Running;
        // The function whose guest call this one is, when it runs on that
        // one's fiber (callOnFiber), and the same the other way while it
        // runs: functions on one fiber return last in, first out.
        Suspendable *caller = nullptr;
        Suspendable *callee = nullptr;
        // Where the fiber's stack starts (it grows down).
        uintptr_t stackStart = 0;
        CallRegs *call = nullptr; // the call it is making (on the fiber's stack)
        uint32_t calleeSp = 0;    // the callee's sp: the key it waits under
        uint32_t returnPc = 0;    // where its callee returns (the call's ra)
        // The fiber left with a call that did not run (a checkpoint due at its
        // dispatch) or the run stopping: the guest thread just unwinds.
        bool unwinding = false;
        bool listed = false;      // in g_waiting
        const uint8_t *waitRam = nullptr; // the guest memory it waits in (g_waiting's key)
        uint64_t resumes = 0;     // times resume() entered it again (an unwind)
        // The resume count at which a tail call made as a guest call (from a
        // readable function called directly) came back (ee::takeTailReturn);
        // 0: none pending.
        uint64_t tailReturn = 0;
        uint32_t address = 0;     // the function (pc at entry), for diagnostics
        // Its tail call went on without it (the callee unwound and returns to
        // the function's caller by itself): the rest runs on a scratch context.
        bool detached = false;
        std::unique_ptr<R5900Context> scratch;
    };

    namespace
    {
        // A fiber's stack. Readable calls made directly (game::call, a
        // FunctionPtr's visit) run on it with no check of their depth, and a
        // recursion goes as deep as the guest's: DM2's in-order walk of a
        // level's waypoint tree overran 512 KB on Park Quest's Woodward
        // (ZX_READABLE=tested). Reserved, not committed (fiber_posix.cpp,
        // fiber_windows.cpp): a fiber uses what it touches.
        constexpr std::size_t kFiberStackBytes = 8u * 1024u * 1024u;
        // A callee runs on its caller's fiber while this much of the stack is
        // free (what a fiber of its own had before calls shared them), else
        // on a new one.
        constexpr std::size_t kCalleeStackBytes = 256u * 1024u;
        constexpr std::size_t kSpareSuspendables = 64;
        uint32_t g_resumePc = 0;
        uint32_t g_kernelPc = 0;           // the kernel slot (kernelSlot, ee::kernelCall)
        // (the call it makes: ThreadState::kernelNumber)
        // Where a fiber that left at a safe point is resumed: a slot of its
        // own, so a caller's dispatch (whose return address is g_resumePc)
        // never takes an inner function's unwind for its callee's return.
        uint32_t g_yieldPc = 0;
        // Fibers waiting for their callee, by the guest memory and the
        // callee's sp (unique: each waits below its own frame on its own
        // guest thread's stack). The memory too: guest code run in RAM of
        // its own with stacks at the same addresses would by sp alone take
        // the waiting fibers' keys, and their callee would then return to
        // the resume slot with nothing waiting (gamehand14, drafter25). The
        // EE thread is the only user.
        struct WaitKey
        {
            const uint8_t *ram;
            uint32_t sp;
            bool operator==(const WaitKey &o) const { return ram == o.ram && sp == o.sp; }
        };
        struct WaitKeyHash
        {
            size_t operator()(const WaitKey &k) const
            {
                return std::hash<const void *>()(k.ram) ^ (static_cast<size_t>(k.sp) * 0x9E3779B97F4A7C15ull);
            }
        };
        std::unordered_map<WaitKey, Suspendable *, WaitKeyHash> g_waiting;
        // g_waiting's nodes taken out, for the next listing: a readable function's guest call lists it and takes it
        // away again (drive()), so the map would allocate and free a node at every such call. The same map
        // either way: a node put back holds the new key and function, as a new one would.
        std::vector<decltype(g_waiting)::node_type> g_spareWaiting;
        constexpr size_t kSpareWaiting = 64;
        void takeOut(decltype(g_waiting)::node_type node)
        {
            if (node && g_spareWaiting.size() < kSpareWaiting)
                g_spareWaiting.push_back(std::move(node));
        }
        WaitKey keyAt(uint32_t sp) { return WaitKey{g_ram, sp}; }

        // A message that may repeat without end (a broken state the guest
        // loops over): the 1st, 2nd, 4th, 8th ... time only, with the count.
        bool reportNow(uint64_t &count)
        {
            ++count;
            return (count & (count - 1)) == 0;
        }

        void resume(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);

        // The original's return addresses that calls from readable code give
        // their callees (CallRegs::returnAddress): each one's table slot, in
        // the table it was taken in, goes to
        // returned(), which takes a callee that comes back there through the
        // scheduler to the fiber waiting for it, and runs what the slot had
        // for anything else. An address is inside the readable function's
        // original, which does not run while the readable one does.
        struct ReturnSlot
        {
            PS2Machine::RecompiledFunction previous = nullptr;
            bool usable = false; // false: the call goes through g_resumePc
        };
        std::unordered_map<const void *, std::unordered_map<uint32_t, ReturnSlot>> g_returnSlots;

        void returned(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t pc = ctx->pc;
            const auto it = g_waiting.find(keyAt(static_cast<uint32_t>(gpr64(ctx, reg::sp))));
            if (it != g_waiting.end() && it->second->state == Suspendable::State::Calling && it->second->returnPc == pc)
            {
                resume(rdram, ctx, runtime);
                return;
            }
            const auto table = g_returnSlots.find(zx::function_table::active().table);
            if (table != g_returnSlots.end())
                if (const auto slot = table->second.find(pc); slot != table->second.end() && slot->second.previous)
                {
                    slot->second.previous(rdram, ctx, runtime);
                    return;
                }
            static uint64_t count = 0;
            if (reportNow(count))
                std::fprintf(stderr, "[readable] return slot 0x%x with nothing to run (%llu times)\n", pc,
                             static_cast<unsigned long long>(count));
        }

        // Whether a call can return through `address` (its slot set up the
        // first time). Not one that starts a function (a jal as a function's
        // last act, its callee never returning: a dispatch of the next
        // function there with the same sp would look like the return).
        bool returnSlot(PS2Machine *runtime, uint32_t address)
        {
            if (!address || !g_resumePc)
                return false;
            auto &slots = g_returnSlots[zx::function_table::active().table];
            const auto it = slots.find(address);
            if (it != slots.end())
                return it->second.usable;
            ReturnSlot &slot = slots[address];
            for (uint32_t g = 0; g < game::kGameCount; ++g)
                if (frameLayout(g, address))
                    return false;
            if (!runtime->hasFunction(address))
                return false;
            slot.previous = runtime->lookupFunction(address);
            slot.usable = slot.previous == &returned || runtime->replaceFunction(address, &returned);
            if (slot.previous == &returned)
                slot.previous = nullptr;
            return slot.usable;
        }

        // Registers s as waiting under its callee's sp. A fiber already
        // waiting there can never be resumed: the guest stack it waits on is
        // in use again (its thread went on elsewhere, e.g. ended or restarted
        // while it waited), so the newer one takes the key.
        // ZX_SAFEPOINT_TRACE=1: every safe-point yield and resume on stderr.
        bool traceOn()
        {
            static const bool on = [] { const char *v = std::getenv("ZX_SAFEPOINT_TRACE"); return v && v[0] == '1'; }();
            return on;
        }

        void listWaiting(Suspendable *s, const R5900Context *ctx)
        {
            s->listed = true;
            const WaitKey key{s->waitRam, s->calleeSp};
            decltype(g_waiting)::iterator it;
            bool fresh = false;
            if (g_spareWaiting.empty())
                std::tie(it, fresh) = g_waiting.emplace(key, s);
            else
            {
                decltype(g_waiting)::node_type node = std::move(g_spareWaiting.back());
                g_spareWaiting.pop_back();
                node.key() = key;
                node.mapped() = s;
                auto placed = g_waiting.insert(std::move(node));
                it = placed.position;
                fresh = placed.inserted;
                takeOut(std::move(placed.node)); // (the key was there: the node comes back unused)
            }
            if (fresh)
                return;
            static int reported = 0;
            if (reported < 20)
            {
                ++reported;
                std::fprintf(stderr,
                             "[readable] a fiber still waiting at sp 0x%x (state %d, ra 0x%x, resumes %llu) replaced "
                             "(fn 0x%x; new: fn 0x%x, ra 0x%x, pc 0x%x)\n",
                             s->calleeSp, static_cast<int>(it->second->state), it->second->frame.ra,
                             static_cast<unsigned long long>(it->second->resumes), it->second->address, s->address,
                             s->frame.ra, ctx->pc);
            }
            it->second = s;
        }

        // s may wait, with the guest thread unwinding to the scheduler, and
        // then so do the callers on its fiber, which were not listed when
        // they called. (A function listed has its callers listed.)
        void waitFor(Suspendable *s, const R5900Context *ctx)
        {
            for (; s && !s->listed; s = s->caller)
                listWaiting(s, ctx);
        }

        void unlist(Suspendable *s)
        {
            takeOut(g_waiting.extract(WaitKey{s->waitRam, s->calleeSp}));
            s->listed = false;
        }

        // The function that left a fiber last (ThreadState::leaving): the one
        // entered or one running on top of it (callOnFiber).

        void leaveFiber(Suspendable *s)
        {
            t_ee.leaving = s;
            fiber::leave(s->fiber);
        }

        // Suspendables are reused like their fibers: a level load starts
        // millions (they hold a Frame, so a new one costs an allocation and
        // some hundreds of bytes set). Freed with their thread (the game
        // thread ends with each run).
        struct Spares : std::vector<Suspendable *>
        {
            ~Spares()
            {
                for (Suspendable *s : *this)
                    delete s;
            }
        };
        thread_local Spares t_spare;

        // The function running on a fiber, with its count of resumes where fiberResumes reads it.
        void setSuspendable(Suspendable *s)
        {
            t_ee.suspendable = s;
            t_ee.suspendableResumes = s ? &s->resumes : nullptr;
        }

        Suspendable *newSuspendable()
        {
            if (t_spare.empty())
                return new Suspendable;
            Suspendable *s = t_spare.back();
            t_spare.pop_back();
            return s;
        }

        void finished(Suspendable *s)
        {
            if (!s->caller)
                fiber::destroy(s->fiber);
            s->fiber = nullptr;
            s->caller = nullptr;
            if (t_spare.size() >= kSpareSuspendables)
            {
                delete s;
                return;
            }
            s->state = Suspendable::State::Running;
            s->call = nullptr;
            s->resumes = 0;
            s->tailReturn = 0;
            s->detached = false;
            s->scratch.reset();
            t_spare.push_back(s);
        }

        void fiberMain(void *arg)
        {
            Suspendable *s = static_cast<Suspendable *>(arg);
            char stackHere = 0;
            s->stackStart = reinterpret_cast<uintptr_t>(&stackHere);
            s->body(s->frame);
            s->state = Suspendable::State::Done; // returning leaves the fiber for good
            t_ee.leaving = s;
        }

        // What a guest call does to the context before its dispatch (ee::call's
        // arguments, sp, ra). Whether it returns through the original's
        // return address.
        bool setUpCall(Suspendable *s, R5900Context *ctx)
        {
            const CallRegs &call = *s->call;
            call.into(ctx);
            setGpr64(ctx, reg::sp, s->calleeSp);
            // The callee's ra: the original's return address (its slot takes
            // the callee back here when it returns through the scheduler),
            // else the resume slot.
            const bool original = returnSlot(s->frame.runtime, call.returnAddress);
            s->returnPc = original ? call.returnAddress : g_resumePc;
            setGpr64(ctx, reg::ra, s->returnPc);
            s->waitRam = g_ram;
            return original;
        }

        // A callee that came straight back: its results.
        void cameBack(Suspendable *s, R5900Context *ctx)
        {
            CallRegs &call = *s->call;
            call.v0 = gpr64(ctx, reg::v0);
            call.v1 = gpr64(ctx, reg::v1);
            call.f0 = ctx->f[reg::f0];
            keepLeft(*ctx);
            s->frame.ctx = ctx; // VU0 as the callee left it (a real guest call)
            s->state = Suspendable::State::Running;
        }

        // Runs the fiber until it ends or a callee does not come straight
        // back. A call goes through dispatchGuestBranch like a generated
        // one: the callee runs here, and only when it waits (or a safe
        // point is due) does the guest thread unwind to the scheduler,
        // with the fiber left waiting for the callee's return to the resume
        // slot. So the scheduling is the recompiled function's.
        void drive(Suspendable *s, R5900Context *ctx)
        {
            for (;;)
            {
                Frame *outerFrame = t_ee.frame;
                Suspendable *outer = t_ee.suspendable;
                t_ee.frame = &s->frame;
                setSuspendable(s);
                fiber::enter(s->fiber);
                t_ee.frame = outerFrame;
                setSuspendable(outer);
                s = t_ee.leaving;
                ctx = s->frame.ctx;

                if (s->unwinding)
                {
                    s->unwinding = false;
                    waitFor(s, ctx);
                    return;
                }
                if (s->state == Suspendable::State::Done && s->detached)
                {
                    // (Its callers on this fiber wait for the tail callee.)
                    waitFor(s->caller, ctx);
                    finished(s);
                    return;
                }
                if (s->state == Suspendable::State::Done)
                {
                    // Returned: back to the caller as a jr ra would.
                    setGpr64(ctx, reg::sp, s->frame.sp);
                    setGpr64(ctx, reg::ra, s->frame.ra);
                    ctx->pc = s->frame.ra;
                    finished(s);
                    return;
                }
                if (s->state == Suspendable::State::Yielded)
                {
                    // A safe point with a checkpoint due: the guest thread
                    // unwinds to the scheduler (the callers see pc leave their
                    // return address, as after the original's eeCheckpointDue)
                    // and the scheduler resumes it at the resume slot.
                    setGpr64(ctx, reg::sp, s->calleeSp);
                    ctx->pc = g_yieldPc;
                    if (traceOn())
                    {
                        EeScheduler &sched = s->frame.runtime->eeScheduler();
                        std::fprintf(stderr, "[safepoint] yield fn 0x%x sp 0x%x thread %d cycle %llu ctx %p\n",
                                     s->address, s->calleeSp, sched.currentThreadId(),
                                     static_cast<unsigned long long>(sched.eeCycle()), static_cast<void *>(ctx));
                    }
                    s->waitRam = g_ram;
                    waitFor(s, ctx);
                    return;
                }
                CallRegs &call = *s->call;
                PS2Machine *runtime = s->frame.runtime;
                if (call.tail && safePointsOn() && s->frame.directDepth == 0)
                {
                    // (A callee that waits returns to the caller on this fiber.)
                    waitFor(s->caller, ctx);
                    // The original's tail call: its epilogue has put sp and ra
                    // back, and the j goes to the callee as a direct C++ call
                    // (no dispatch, no charge, no safe point); the callee
                    // returns to the function's caller. (From a readable
                    // function called directly, game::call, whose own sp and
                    // return address are not known: as a call.)
                    call.into(ctx);
                    setGpr64(ctx, reg::sp, s->frame.sp);
                    setGpr64(ctx, reg::ra, s->frame.ra);
                    ctx->pc = call.target;
                    runtime->lookupFunction(call.target)(s->frame.rdram, ctx, runtime);
                    if (ctx->pc != s->frame.ra)
                    {
                        // It unwound and goes on to the caller by itself: the
                        // function's own rest (nothing after a tail call)
                        // runs off the guest context.
                        s->scratch = std::make_unique<R5900Context>(*ctx);
                        s->frame.ctx = s->scratch.get();
                        s->detached = true;
                        ctx = s->scratch.get();
                    }
                    call.v0 = gpr64(ctx, reg::v0);
                    call.v1 = gpr64(ctx, reg::v1);
                    call.f0 = ctx->f[reg::f0];
                    keepLeft(*ctx);
                    if (!s->detached)
                        s->frame.ctx = ctx;
                    s->state = Suspendable::State::Running;
                    continue;
                }
                const bool original = setUpCall(s, ctx);
                // Before the call: a callee that waits never comes back here
                // (EeDispatcherTransfer goes through this frame).
                waitFor(s, ctx);
                if (call.tail || call.kernel)
                {
                    // A tail call: generated code calls the function directly
                    // (no dispatch, no charge, no safe point); a callee that
                    // waits or leaves elsewhere unwinds to the scheduler. A
                    // kernel call the same: the kernel slot's handler in place
                    // of the SYSCALL.
                    ctx->pc = call.target;
                    runtime->lookupFunction(call.target)(s->frame.rdram, ctx, runtime);
                    if (ctx->pc != s->returnPc)
                        return; // resume() takes it from the callee's return
                }
                else if (!runtime->dispatchGuestBranch(s->frame.rdram, ctx, call.target,
                                                       original ? call.returnAddress - 8u : s->frame.ra, s->returnPc,
                                                       PS2Machine::GuestBranchKind::DirectCall, "readable call"))
                    return; // resume() takes it from the callee's return
                unlist(s);
                cameBack(s, ctx);
            }
        }

        // The table entries of readable functions (ee::noteReadableEntry), an
        // open-addressed set of their addresses: asked at every guest call
        // from a fiber.
        constexpr std::size_t kEntrySlots = std::size_t{1} << 15;
        uintptr_t g_readableEntries[kEntrySlots];
        std::size_t g_readableEntryCount = 0;

        std::size_t entrySlot(uintptr_t entry)
        {
            return static_cast<std::size_t>((entry * 0x9E3779B97F4A7C15ull) >> 49);
        }

        bool isReadableEntry(PS2Machine::RecompiledFunction function)
        {
            const uintptr_t entry = reinterpret_cast<uintptr_t>(function);
            for (std::size_t slot = entrySlot(entry);; slot = (slot + 1u) & (kEntrySlots - 1u))
            {
                if (g_readableEntries[slot] == entry)
                    return true;
                if (!g_readableEntries[slot])
                    return false;
            }
        }

        // ZX_READABLE_FIBER_CALLS=0: every readable callee on a fiber of
        // its own, as before callOnFiber.
        bool fiberCallsOn()
        {
            static const bool on = []
            {
                const char *v = std::getenv("ZX_READABLE_FIBER_CALLS");
                return safePointsOn() && !(v && v[0] == '0');
            }();
            return on;
        }

        // The runtime's library leaves' table entries (ee::noteLibraryLeaf): a handful.
        constexpr std::size_t kLibraryLeaves = 16;
        uintptr_t g_libraryLeaves[kLibraryLeaves];
        std::size_t g_libraryLeafCount = 0;

        bool isLibraryLeaf(PS2Machine::RecompiledFunction function)
        {
            const uintptr_t entry = reinterpret_cast<uintptr_t>(function);
            for (std::size_t i = 0; i < g_libraryLeafCount; ++i)
                if (g_libraryLeaves[i] == entry)
                    return true;
            return false;
        }

        // The callee taken by the runSuspendable of the entry callOnFiber runs:
        // ThreadState::fiberCaller.

        // A guest call of a readable function from a fiber, on that fiber:
        // what drive() and dispatchGuestBranch do for it, without leaving.
        // The callee's entry (runSuspendable, runOnCallersFiber) goes on as
        // it would on a fiber of its own, and if it leaves, it leaves this
        // fiber: to the scheduler the two functions are as before, each
        // waiting under its callee's sp, and it returns to this one through
        // the scheduler. False: not such a call, nothing done. (Generated
        // code stays on the host stack, whose depth it may need.)
        bool callOnFiber(Suspendable *s)
        {
            CallRegs &call = *s->call;
            PS2Machine *runtime = s->frame.runtime;
            if (call.tail || call.kernel || !fiberCallsOn() || runtime->guestCallsTraced())
                return false;
            const PS2Machine::RecompiledFunction entry = runtime->tableEntry(call.target);
            if (!entry || !isReadableEntry(entry))
                return false;
            char stackHere = 0;
            if (s->stackStart - reinterpret_cast<uintptr_t>(&stackHere) > kFiberStackBytes - kCalleeStackBytes)
                return false;
            R5900Context *ctx = s->frame.ctx;
            const bool original = setUpCall(s, ctx);
            if (runtime->dispatchCharge(s->frame.rdram, ctx, call.target,
                                        original ? call.returnAddress - 8u : s->frame.ra))
            {
                const uint64_t resumes = s->resumes;
                t_ee.fiberCaller = s;
                entry(s->frame.rdram, ctx, runtime);
                if (s->resumes != resumes)
                    return true; // resume() took it from the callee's return
                if (!runtime->isStopRequested())
                {
                    if (s->listed)
                        unlist(s); // (a call of the callee's listed it)
                    cameBack(s, ctx);
                    return true;
                }
            }
            s->unwinding = true;
            leaveFiber(s); // resume() takes it from the callee's return
            return true;
        }

        // A guest call of one of the runtime's library leaves from a fiber, on that fiber: drive()'s call
        // (setUpCall, dispatchGuestBranch, cameBack) without leaving. What the fibers waiting are listed as
        // afterwards is drive()'s too: it lists the caller and its callers on the fiber before the dispatch
        // and takes the caller's own listing away when the call comes back. A dispatch that unwinds (a
        // checkpoint due) leaves as callOnFiber's does, and drive() lists them then. False: not such a call.
        bool callLeafOnFiber(Suspendable *s)
        {
            CallRegs &call = *s->call;
            PS2Machine *runtime = s->frame.runtime;
            if (g_libraryLeafCount == 0 || call.tail || call.kernel || !fiberCallsOn() || runtime->guestCallsTraced())
                return false;
            const PS2Machine::RecompiledFunction entry = runtime->tableEntry(call.target);
            if (!entry || !isLibraryLeaf(entry))
                return false;
            R5900Context *ctx = s->frame.ctx;
            const bool original = setUpCall(s, ctx);
            if (!runtime->dispatchGuestBranch(s->frame.rdram, ctx, call.target,
                                              original ? call.returnAddress - 8u : s->frame.ra, s->returnPc,
                                              PS2Machine::GuestBranchKind::DirectCall, "readable call"))
            {
                s->unwinding = true;
                leaveFiber(s); // resume() takes it from the callee's return
                return true;
            }
            if (s->listed)
                unlist(s);
            else
            {
                waitFor(s->caller, ctx);
                // (listed and taken away again: what is left at its key is nothing)
                if (const auto it = g_waiting.find(WaitKey{s->waitRam, s->calleeSp}); it != g_waiting.end())
                {
                    listWaiting(s, ctx); // (as drive()'s: the other fiber's entry reported and replaced)
                    unlist(s);
                }
            }
            cameBack(s, ctx);
            return true;
        }

        // The callee on top of a function being returned to, on their fiber:
        // its tail call waited (so never came back to drive()) and has
        // returned past it to that function. Its own rest, nothing after a
        // tail call, runs off the guest context, as in drive().
        void endTailCaller(Suspendable *s, const R5900Context *ctx)
        {
            if (s->state != Suspendable::State::Calling || !s->call->tail)
            {
                // Only a guest longjmp could do this, and these games have none.
                std::fprintf(stderr, "[readable] a function returned to while its callee 0x%x still runs\n", s->address);
                std::abort();
            }
            s->scratch = std::make_unique<R5900Context>(*ctx);
            s->frame.ctx = s->scratch.get();
            s->detached = true;
            s->state = Suspendable::State::Running;
            Frame *outerFrame = t_ee.frame;
            Suspendable *outer = t_ee.suspendable;
            t_ee.frame = &s->frame;
            setSuspendable(s);
            fiber::enter(s->fiber); // (leaves at the end of runOnCallersFiber)
            t_ee.frame = outerFrame;
            setSuspendable(outer);
            finished(s);
        }

        // A readable callee's body on its caller's fiber (callOnFiber).
        void runOnCallersFiber(Suspendable *s, Suspendable *caller, R5900Context *ctx)
        {
            s->caller = caller;
            s->fiber = caller->fiber;
            s->stackStart = caller->stackStart;
            caller->callee = s;
            t_ee.frame = &s->frame;
            setSuspendable(s);
            s->body(s->frame);
            t_ee.frame = &caller->frame;
            setSuspendable(caller);
            caller->callee = nullptr;
            if (s->resumes != 0 || s->detached)
            {
                // It left, so its caller's dispatch is gone: its return goes
                // through the scheduler as from a fiber of its own (drive()),
                // and resume() enters the fiber again for the caller.
                s->state = Suspendable::State::Done;
                leaveFiber(s);
                return;
            }
            // As drive() ends a function: back to the caller as a jr ra would.
            setGpr64(ctx, reg::sp, s->frame.sp);
            setGpr64(ctx, reg::ra, s->frame.ra);
            ctx->pc = s->frame.ra;
            finished(s);
        }

        // The kernel slot: ee::kernelCall's SYSCALL as a callee, so a call that
        // waits unwinds as any callee does and comes back at ra. (pc = ra
        // before it: what a wait leaves for the scheduler; the recompiled
        // SYSCALL goes on at its next instruction whatever the handler left.)
        void kernelSlot(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t ra = static_cast<uint32_t>(gpr64(ctx, reg::ra));
            setGpr64(ctx, reg::v1, t_ee.kernelNumber);
            ctx->pc = ra;
            runtime->handleSyscall(rdram, ctx, 0u);
            ctx->pc = ra;
        }

        // The resume slot: a callee returned to a waiting fiber.
        void resume(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t sp = static_cast<uint32_t>(gpr64(ctx, reg::sp));
            const auto it = g_waiting.find(keyAt(sp));
            if (it == g_waiting.end())
            {
                static uint64_t count = 0;
                if (reportNow(count))
                    std::fprintf(stderr, "[readable] return to the resume slot with no call waiting at sp 0x%x (%llu times)\n",
                                 sp, static_cast<unsigned long long>(count));
                return;
            }
            Suspendable *s = it->second;
            takeOut(g_waiting.extract(it));
            s->listed = false;
            if (s->callee)
                endTailCaller(s->callee, ctx);
            if (s->state != Suspendable::State::Yielded)
            {
                CallRegs &call = *s->call;
                call.v0 = gpr64(ctx, reg::v0);
                call.v1 = gpr64(ctx, reg::v1);
                call.f0 = ctx->f[reg::f0];
                keepLeft(*ctx);
            }
            ++s->resumes;
            if (traceOn())
                std::fprintf(stderr, "[safepoint] resume fn 0x%x sp 0x%x state %d thread %d ctx %p\n", s->address, sp,
                             static_cast<int>(s->state), runtime->eeScheduler().currentThreadId(),
                             static_cast<void *>(ctx));
            // The guest thread's context may be another object by now.
            s->frame.rdram = rdram;
            s->frame.ctx = ctx;
            s->frame.runtime = runtime;
            s->state = Suspendable::State::Running;
            drive(s, ctx);
        }
    }

    void callSuspending(CallRegs &regs)
    {
        Suspendable *s = t_ee.suspendable;
        if (s->detached)
        {
            s->frame.next.mask = 0;
            return; // after its tail call went on without it: nothing more runs
        }
        s->call = &regs;
        s->state = Suspendable::State::Calling;
        s->calleeSp = s->frame.calleeSp; // (StackFrame's, else the original's frame or kCalleeStackGap below sp)
        // A tail call from a readable function called directly runs at that
        // function's sp, its frame released (frame table on; the fiber's own
        // function's tail call goes to its caller in drive()).
        if (regs.tail && framesOn() && s->frame.directDepth != 0 && s->frame.fnSp)
            s->calleeSp = s->frame.fnSp;
        const uint64_t resumes = s->resumes;
        Frame &frame = s->frame;
        frame.callUnwound = false;
        // The callee's ra: the original's return address where the caller
        // published it; a tail call's, the function's own (drive() takes the
        // fiber's own function's tail call to its caller itself).
        regs.returnAddress = regs.tail ? (frame.directDepth != 0 ? tailReturn(frame) : 0u) : callReturn(frame, regs.target);
        // The caller's working callee-saved registers for the callee (not
        // for a tail call: the original's epilogue has put the entry values
        // back), and the context's own back after it (the callee restores
        // what it was given; ctx may be another object by then).
        if (regs.tail)
            frame.next.mask = 0;
        const KeptRegs kept = applySaves(frame, frame.ctx);
        if (!callOnFiber(s) && !callLeafOnFiber(s))
            leaveFiber(s);
        restoreSaves(kept, s->frame.ctx);
        // Entered again by resume() (the results are in regs), or straight
        // back. A call that unwound came back through the scheduler, which
        // dispatched its return address: the original goes on as the table
        // has it there (the whole function unless ee::block(..., true)).
        if (s->resumes != resumes && !regs.tail)
        {
            frame.mode = kWhole;
            frame.paid = false;
            frame.callUnwound = true;
        }
        else if (s->resumes != resumes && frame.directDepth != 0)
            s->tailReturn = s->resumes; // came back through the resume slot's dispatch
    }

    bool takeTailReturn(ThreadState &thread)
    {
        Suspendable *s = thread.suspendable;
        if (!s || s->tailReturn == 0)
            return false;
        const bool last = s->tailReturn == s->resumes;
        s->tailReturn = 0;
        return last;
    }

    void yieldSafePoint()
    {
        Suspendable *s = t_ee.suspendable;
        if (!s || s->detached)
            return;
        s->state = Suspendable::State::Yielded;
        s->calleeSp = s->frame.calleeSp; // (StackFrame's, else sp - kCalleeStackGap)
        leaveFiber(s);
        // Entered again by resume(), from the scheduler's dispatch.
    }


    bool fiberEntry(const PS2Machine *runtime)
    {
        return runtime && !t_ee.callHandler && g_resumePc && safePointsOn();
    }

    void runSuspendable(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, void (*body)(Frame &frame),
                        const FrameRow *layout)
    {
        const uint32_t sp = static_cast<uint32_t>(gpr64(ctx, reg::sp));
        Frame frame{rdram, ctx, runtime, sp, static_cast<uint32_t>(gpr64(ctx, reg::ra)), sp - kCalleeStackGap, sp};
        FunctionScope scope; // (a frame of its own: its place is the frame's, copied with it below)
        enterFrame(scope, frame, layout, sp);
        if (!g_resumePc)
        {
            // Not installed (no runtime table): plain calls.
            const uint64_t entryRa = gpr64(ctx, reg::ra);
            Frame local = frame;
            Frame *outer = t_ee.frame;
            t_ee.frame = &local;
            body(local);
            t_ee.frame = outer;
            setGpr64(ctx, reg::ra, entryRa);
            ctx->pc = local.ra;
            return;
        }
        Suspendable *s = newSuspendable();
        s->frame = frame;
        s->body = body;
        s->address = ctx->pc;
        if (Suspendable *caller = t_ee.fiberCaller)
        {
            t_ee.fiberCaller = nullptr;
            runOnCallersFiber(s, caller, ctx);
            return;
        }
        if (traceOn() && t_ee.suspendable)
            std::fprintf(stderr, "[safepoint] fiber fn 0x%x started inside fiber fn 0x%x (ra 0x%x)\n", s->address,
                         t_ee.suspendable->address, frame.ra);
        s->fiber = fiber::create(&fiberMain, s, kFiberStackBytes);
        drive(s, ctx);
    }

    void noteReadableEntry(PS2Machine::RecompiledFunction entry)
    {
        const uintptr_t value = reinterpret_cast<uintptr_t>(entry);
        if (!value || isReadableEntry(entry) || g_readableEntryCount >= kEntrySlots / 2u)
            return; // (one not noted just takes the slower way)
        std::size_t slot = entrySlot(value);
        while (g_readableEntries[slot])
            slot = (slot + 1u) & (kEntrySlots - 1u);
        g_readableEntries[slot] = value;
        ++g_readableEntryCount;
    }

    void noteLibraryLeaf(PS2Machine::RecompiledFunction entry)
    {
        if (entry && !isLibraryLeaf(entry) && g_libraryLeafCount < kLibraryLeaves)
            g_libraryLeaves[g_libraryLeafCount++] = reinterpret_cast<uintptr_t>(entry);
    }

    void clearLibraryLeaves() { g_libraryLeafCount = 0; }

    bool installSuspendableCalls(PS2Machine &runtime)
    {
        // Three slots of the table in use no code maps to (guest code never
        // branches there by itself): calls' returns, safe points' resumes,
        // kernel calls. Chosen in each table at each install, the first ones
        // free or already these (an earlier install in this table; each game
        // has its own table, put back as generated when it is chosen again),
        // so the same three for a game however many runs and games came first.
        const zx::function_table::View table = zx::function_table::active();
        uint32_t found[3] = {};
        int n = 0;
        for (uint32_t address = table.base + 4u; address < table.end && n < 3; address += 4u)
        {
            const PS2Machine::RecompiledFunction entry = runtime.tableEntry(address);
            if (entry && entry != &resume && entry != &kernelSlot)
                continue;
            if (!runtime.replaceFunction(address, n == 2 ? &kernelSlot : &resume))
                return false;
            found[n++] = address;
        }
        if (n < 3)
            return false;
        g_resumePc = found[0];
        g_yieldPc = found[1];
        g_kernelPc = found[2];
        runtime.addStoppedHook(&dropWaiting);
        return true;
    }

    size_t waitingFunctions() { return g_waiting.size(); }

    void dropWaiting(PS2Machine &runtime)
    {
        // The functions waiting (a listed one's callers on its fiber are
        // listed too).
        std::vector<Suspendable *> all;
        for (const auto &[key, s] : g_waiting)
            all.push_back(s);
        g_waiting.clear();
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        // Never entered again: the guest memory and threads they waited in
        // are gone. The fiber belongs to the first function on it. The host
        // frames on it are not unwound: what their locals hold on the host
        // heap stays allocated.
        for (Suspendable *s : all)
            if (!s->caller && s->fiber)
                fiber::destroy(s->fiber);
        for (Suspendable *s : all)
            delete s;
        // The return slots set up while it ran, in the table it ran in: back
        // to what they had (a later run sets them up again as it needs them).
        size_t slots = 0;
        if (const auto table = g_returnSlots.find(zx::function_table::active().table); table != g_returnSlots.end())
            for (const auto &[address, slot] : table->second)
                if (slot.usable && slot.previous && runtime.tableEntry(address) == &returned)
                {
                    runtime.replaceFunction(address, slot.previous);
                    ++slots;
                }
        g_returnSlots.clear();
        if (!all.empty() || slots != 0)
            std::fprintf(stderr, "[readable] the run stopped: %zu functions still waiting dropped, %zu return slots put back\n",
                         all.size(), slots);
    }

    int32_t kernelCall(uint32_t number)
    {
        CallRegs regs;
        regs.kernel = true;
        if (t_ee.callHandler)
        {
            t_ee.callHandler->call(regs); // (target 0: not in any recording)
            return static_cast<int32_t>(regs.v0);
        }
        const Frame *frame = t_ee.frame;
        if (!frame || !frame->runtime)
            return 0;
        t_ee.kernelNumber = number;
        if (!g_kernelPc)
        {
            // No slots: no fibers either, so straight on this stack.
            setGpr64(frame->ctx, reg::v1, number);
            frame->runtime->handleSyscall(frame->rdram, frame->ctx, 0u);
            return static_cast<int32_t>(gpr64(frame->ctx, reg::v0));
        }
        regs.target = g_kernelPc;
        if (t_ee.suspendable)
            callSuspending(regs);
        else
            callGuest(regs);
        return static_cast<int32_t>(regs.v0);
    }
}
