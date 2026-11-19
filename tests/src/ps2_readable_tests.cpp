// The readable game's framework (runtime/src/game/ee): EE floats, the
// calling convention adapter, the PS2X_READABLE switch and the fncheck
// recording format.
#include "MiniTest.h"

#include "ee/abi.h"
#include "ee/diffcall.h"
#include "ee/fiber.h"
#include "ee/fncheck.h"
#include "ee/fncheck_build.h"
#include "ee/fnrec_file.h"
#include "ee/held_program.h"
#include "ee/readable.h"
#include "ee/io.h"
#include "ee/mmi.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_write.h"
#include "zge/render/ps2/packet_size.h"
#include "aggressive/level/collision_setup_host.h"
#include "zge/native/collision_data.h"
#include "zge/native/geo_file.h"
#include "zge/native/strips.h"

#include <zstd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Two ps2x_tests at once (agents, a check beside a build) must not share
    // a folder: each run's is its own.
    std::filesystem::path uniqueTempFolder(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    struct Pair
    {
        ee::Float a, b;
    };

    uint32_t mixArguments(uint32_t first, ee::Float scale, int32_t second, ee::Float offset)
    {
        return first * 1000u + static_cast<uint32_t>(second) * 10u + static_cast<uint32_t>(ee::toInt(scale * offset));
    }

    Pair *swapPair(Pair *dst, const Pair *src)
    {
        const Pair copy = *src;
        dst->a = copy.b;
        dst->b = copy.a;
        return dst;
    }

    ee::Float sumPair(const Pair *p) { return p->a + p->b; }

    void touchNothing(uint32_t) {}

    // Hands a local in its stack frame to a callee, as an original would.
    int32_t localToCallee(uint32_t value)
    {
        ee::StackFrame frame(0x30);
        *frame.at<uint32_t>(0x10) = value;
        return ee::call<int32_t>(0x00123450u, frame.address(0x10)) + 1;
    }

    // A suspendable function: two calls, the first to a callee that goes
    // elsewhere (as a wait would) before it returns.
    int32_t addTwoCallResults(uint32_t base)
    {
        ee::StackFrame frame(0x20);
        const int32_t first = ee::call<int32_t>(0x00002000u, base);
        const int32_t second = ee::call<int32_t>(0x00002100u, first);
        return first + second;
    }

    void calleeThatWaits(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        // Leaves to another block first, as the scheduler would after a wait.
        ee::setGpr64(ctx, ee::reg::v0, ee::gpr64(ctx, ee::reg::a0) + 1u);
        ctx->pc = 0x00002200u;
    }
    void afterTheWait(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void doubles(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ee::setGpr64(ctx, ee::reg::v0, ee::gpr64(ctx, ee::reg::a0) * 2u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }

    // Readable callees of a readable caller: they run on the caller's fiber
    // (ee::noteReadableEntry). One returns at once, one's callee goes
    // elsewhere first, as a wait would.
    int32_t readableLeaf(uint32_t base) { return static_cast<int32_t>(base) + 1; }
    int32_t readableThatWaits(uint32_t base) { return ee::call<int32_t>(0x00002000u, base) * 2; }
    int32_t readableCaller(uint32_t base)
    {
        const int32_t first = ee::call<int32_t>(0x00003200u, base);
        const int32_t second = ee::call<int32_t>(0x00003300u, first);
        return first + second;
    }

    // A readable callee's tail call whose callee waits: the wait never comes
    // back to the fiber, and the tail callee returns past the function to its
    // caller (the same fiber: ee::endTailCaller).
    void tailCalleeThatThrows(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ee::setGpr64(ctx, ee::reg::v0, ee::gpr64(ctx, ee::reg::a0) + 1u);
        ctx->pc = 0x00002200u;
        throw EeDispatcherTransfer{};
    }
    int32_t readableTailThatWaits(uint32_t base) { return ee::tailCall<int32_t>(0x00002300u, base); }
    int32_t readableCallerOfTail(uint32_t base) { return ee::call<int32_t>(0x00003500u, base) + 100; }

    // The same calls with the return addresses published, as the drafter
    // writes them (ee::calleeSaves's ra: the jal's + 8). The callees note
    // the ra they get; the readable notes ra after its first call.
    uint32_t g_calleeRa[2] = {}, g_raAfterFirst = 0;
    bool g_originalRestRan = false;
    int32_t addTwoCallResultsPublished(uint32_t base)
    {
        ee::StackFrame frame(0x20);
        ee::calleeSaves({{31, 0x00007010u}});
        const int32_t first = ee::call<int32_t>(0x00002000u, base);
        g_raAfterFirst = static_cast<uint32_t>(ee::callerRegister(ee::reg::ra));
        ee::calleeSaves({{31, 0x00007020u}});
        const int32_t second = ee::call<int32_t>(0x00002400u, first);
        return first + second;
    }
    void waitsNotingRa(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_calleeRa[0] = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        calleeThatWaits(rdram, ctx, runtime);
    }
    void doublesNotingRa(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_calleeRa[1] = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        doubles(rdram, ctx, runtime);
    }
    // What the table has at the return addresses: the original's rest,
    // which must not run while the readable version waits there.
    void originalRest(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_originalRestRan = true;
        ctx->pc = 0u;
    }

    void fiberCounter(void *arg)
    {
        auto *state = static_cast<std::pair<ee::fiber::Context *, int> *>(arg);
        for (;;)
        {
            ++state->second;
            ee::fiber::leave(state->first);
        }
    }

    struct FiberJob
    {
        ee::fiber::Context *fiber = nullptr;
        int calls = 0;
        int made = 0;
        int id = 0;
        int ranAs = -1;
    };

    // A suspendable body: a few calls out, then return.
    void fiberJob(void *arg)
    {
        auto *job = static_cast<FiberJob *>(arg);
        job->ranAs = job->id;
        for (int i = 0; i < job->calls; ++i)
        {
            ++job->made;
            ee::fiber::leave(job->fiber);
        }
    }

    struct RecordingHandler : ee::CallHandler
    {
        ee::CallRegs last;
        void call(ee::CallRegs &regs) override
        {
            last = regs;
            regs.v0 = static_cast<uint64_t>(*ee::at<uint32_t>(static_cast<uint32_t>(regs.a[0])));
        }
    };

    uint32_t bits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    // Registered under made-up addresses for the switch tests.
    READABLE_FUNCTION(0x00000100, touchNothing, Verified);
    READABLE_FUNCTION(0x00000200, sumPair, Tested);
    READABLE_FUNCTION(0x00000300, mixArguments, Draft);
    READABLE_PLANNED(0x00000400, "plannedOnly");
    void readsAFile(uint32_t) {}
    // One host function registered at two addresses, as a linker that folds
    // identical code leaves two readable functions (game::callAt).
    // (A body of its own: an empty one would be folded with the other empty
    // test functions too.)
    volatile uint32_t g_sharedCodeArgument = 0; // (volatile: the store is the body)
    void sharedCode(uint32_t value) { g_sharedCodeArgument = value * 0x9E3779B1u + 0x600u; }
    READABLE_FUNCTION(0x00000600, sharedCode, Draft);
    READABLE_FUNCTION(0x00000700, sharedCode, Draft);
    struct TargetHandler : ee::CallHandler
    {
        uint32_t target = 0;
        void call(ee::CallRegs &regs) override { target = regs.target; }
    };
    READABLE_FUNCTION_WAITS(0x00000500, readsAFile, Tested);

    // A readable function an override wraps, and a readable caller: the hook
    // must run however the caller reaches it (game::call).
    constexpr uint32_t kHookTarget = 0x00002800u;
    volatile uint32_t g_hookTargetRan = 0, g_hookRan = 0;
    void hookTarget(uint32_t value) { g_hookTargetRan = value + 1u; }
    READABLE_FUNCTION(0x00002800, hookTarget, Verified);
    void hookTargetCaller(uint32_t value) { game::call<&hookTarget>(value); }
    // A kernel call's wrapper as the drafter writes it, and the COP0 and MMI
    // forms drafts use (ee/io.h, ee/mmi.h).
    int32_t flushCacheWrapper() { return ee::kernelCall(0x64u); }
    uint32_t cop0Round(uint32_t status)
    {
        ee::io::setCop0Register(12, status);
        return ee::io::cop0Status();
    }
    PS2Runtime::RecompiledFunction g_hookNext = nullptr;
    void hookAround(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_hookRan;
        g_hookNext(rdram, ctx, runtime);
    }

    // A body with GAME_CALL's marker, the name it is called by, and callers
    // that give it a Local, an object in game memory and a pointer.
    constexpr uint32_t kRouted = 0x00002900u, kRoutedWaits = 0x00002A00u;
    constexpr uint32_t kRoutedObject = 0x00200100u;
    struct RoutedThing
    {
        uint32_t value;
    };
    volatile uint32_t g_routedRan = 0, g_routedStubRan = 0;
    uint32_t routed_(uint32_t value, ee::Ptr<uint32_t> out, ee::Ptr<RoutedThing> thing, uint32_t address)
    {
        g_routedRan = value + 1u;
        *out = value + thing->value;
        return value * 2u + address;
    }
    READABLE_FUNCTION(0x00002900, routed_, Verified);
    GAME_CALL(routed);
    uint32_t routedCaller(uint32_t value)
    {
        // (a function of no frame table: its row by hand)
        static const ee::FrameRow row{0u, 0x00345000u, 0x40u, 0u, 0u, 0u, 0u};
        ee::t_ee.frame->layout = &row;
        ee::t_ee.frame->fnSp = 0x00100040u;
        ee::Local<uint32_t> out;
        RoutedThing &thing = *ee::at<RoutedThing>(kRoutedObject);
        const uint32_t result = routed(value, out, thing, ee::Ptr<uint32_t>(0x40u));
        return result + *out;
    }
    void routedWaits_() { ++g_routedRan; }
    READABLE_FUNCTION_WAITS(0x00002A00, routedWaits_, Verified);
    GAME_CALL(routedWaits);
    void routedWaitsCaller() { routedWaits(); }
    void routedStub(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_routedStubRan;
        ee::setGpr64(ctx, ee::reg::v0, 1000u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, 31));
    }

    // A version's own body registered at the shared function's address (READABLE_FUNCTION_OWN) that calls the shared
    // name for part of its work, as DM2's instanceNodeOnItsObject calls instanceGeometryNode for a child: the
    // original's call there is a call of itself, which generated code runs inline (the recording holds none for it).
    constexpr uint32_t kOwnSelf = 0x00002B00u;
    volatile uint32_t g_sharedSelfRan = 0;
    uint32_t sharedSelf(uint32_t depth)
    {
        ++g_sharedSelfRan;
        return depth;
    }
    uint32_t ownSelf(uint32_t depth)
    {
        // (a function of no frame table: its row by hand)
        static const ee::FrameRow row{0u, 0x00345000u, 0x40u, 0u, 0u, 0u, 0u};
        ee::t_ee.frame->layout = &row;
        if (depth == 0u)
            return 0u;
        return 1u + ::game::detail::Route<&sharedSelf>::template at<kOwnSelf, ::game::Game::Ai>(depth - 1u);
    }
    READABLE_FUNCTION(0x00002B00, ownSelf, Draft);
    struct SelfCallHandler : ee::CallHandler
    {
        uint32_t self = 0;
        int calls = 0;
        void call(ee::CallRegs &regs) override
        {
            ++calls;
            regs.v0 = 1000u;
        }
        bool callsItself(uint32_t target) const override { return target == self; }
    };

    // A function that calls itself, as generated code runs it (the shape of
    // setSkaterScore's rollover, FUN_001B4D40): the self-call's jal is a
    // jump to the start with ra at the return point (0x4010, a resume entry
    // of the function), and each call ends by a tail call of the counter
    // 0x6000 with its argument, or, for 100, by jr ra. Above
    // 100 it first calls itself with a0 - 100, then counts a self-call at
    // 0x5004. Traced as fncheck_traced.h traces generated code.
    constexpr uint32_t kSelfCalling = 0x00004000u, kSelfCallReturn = 0x00004010u, kCounter = 0x00006000u;
    void selfCallingTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto word = [&](uint32_t address) -> uint32_t &
        { return *reinterpret_cast<uint32_t *>(rdram + address); };
        auto sp = [&] { return static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)); };
        if (ctx->pc == kSelfCallReturn)
            goto resume;
    start:
        ee::setGpr64(ctx, ee::reg::sp, sp() - 0x20u);
        fncheck::traceWrite(sp() + 0x10u, 4u);
        word(sp() + 0x10u) = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        fncheck::traceWrite(sp(), 4u);
        word(sp()) = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
        if (ee::gpr64(ctx, ee::reg::a0) > 100u)
        {
            ee::setGpr64(ctx, ee::reg::ra, kSelfCallReturn); // jal 0x4000 at 0x4008
            ee::setGpr64(ctx, ee::reg::a0, ee::gpr64(ctx, ee::reg::a0) - 100u);
            goto start;
        }
        goto leave;
    resume:
        fncheck::traceRead(0x5004u, 4u);
        fncheck::traceWrite(0x5004u, 4u);
        ++word(0x5004u);
    leave:
        fncheck::traceRead(sp() + 0x10u, 4u);
        ee::setGpr64(ctx, ee::reg::ra, word(sp() + 0x10u));
        fncheck::traceRead(sp(), 4u);
        ee::setGpr64(ctx, ee::reg::a0, word(sp()));
        ee::setGpr64(ctx, ee::reg::sp, sp() + 0x20u);
        if (ee::gpr64(ctx, ee::reg::a0) == 100u)
        {
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra)); // jr ra
            return;
        }
        // j 0x6000: the tracer's tail call (fncheck::tailCallTo)
        if (fncheck::Tracer *tracer = fncheck::t_tracer)
        {
            tracer->beforeCall(rdram, ctx, kCounter);
            tracer->tailCall(true);
            ctx->pc = kCounter;
            return;
        }
        ctx->pc = kCounter;
        runtime->lookupFunction(kCounter)(rdram, ctx, runtime);
    }
    // Appends a0 to the list at 0x5100 (its count at 0x50FC).
    void counterCallee(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint32_t &count = *reinterpret_cast<uint32_t *>(rdram + 0x50FCu);
        *reinterpret_cast<uint32_t *>(rdram + 0x5100u + 4u * count++) =
            static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    READABLE_PLANNED(0x00004000, "selfCalling");

    // A nested call of the same function under a callee, whose own callee
    // leaves by a jump (the shape of startPlaying's OnStart script starting
    // another animation, round 12). nestedPlaying (0x4400) keeps a0 in s0,
    // calls 0x4800 (return point 0x440C) and returns the word at s0. 0x4800
    // calls nestedPlaying again the first time (with a0 0x5304, through
    // 0x4300, the same code at a second address, so the nested call is not
    // recorded: as a call the recorder does not sample), and the second
    // time leaves by a jump to 0x4900, which returns to ra. The nested call
    // unwinds, and its continuation is at 0x440C too, with a deeper sp.
    constexpr uint32_t kNestedPlaying = 0x00004400u, kNestedReturn = 0x0000440Cu, kNestedAlias = 0x00004300u;
    constexpr uint32_t kNestedCallee = 0x00004800u, kNestedCalleeReturn = 0x0000480Cu, kNestedJumpTarget = 0x00004900u;
    constexpr int kS0 = 16;
    template <bool Traced>
    void nestedPlayingBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto word = [&](uint32_t address) -> uint32_t &
        { return *reinterpret_cast<uint32_t *>(rdram + address); };
        auto sp = [&] { return static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)); };
        auto read = [&](uint32_t address) { if (Traced) fncheck::traceRead(address, 4u); return word(address); };
        auto write = [&](uint32_t address, uint32_t value) { if (Traced) fncheck::traceWrite(address, 4u); word(address) = value; };
        if (ctx->pc == kNestedReturn)
            goto resume;
        ee::setGpr64(ctx, ee::reg::sp, sp() - 0x20u);
        write(sp(), static_cast<uint32_t>(ee::gpr64(ctx, kS0)));
        write(sp() + 0x10u, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra)));
        ee::setGpr64(ctx, kS0, ee::gpr64(ctx, ee::reg::a0));
        ee::setGpr64(ctx, ee::reg::ra, kNestedReturn);
        {
            const bool back = Traced ? fncheck::tracedDispatch(runtime, rdram, ctx, kNestedCallee, 0x4408u, kNestedReturn,
                                                               static_cast<int>(PS2Runtime::GuestBranchKind::DirectCall), "")
                                     : runtime->dispatchGuestBranch(rdram, ctx, kNestedCallee, 0x4408u, kNestedReturn,
                                                                    PS2Runtime::GuestBranchKind::DirectCall, "");
            if (!back)
                return;
        }
    resume:
        ee::setGpr64(ctx, ee::reg::v0, read(static_cast<uint32_t>(ee::gpr64(ctx, kS0))));
        ee::setGpr64(ctx, kS0, read(sp()));
        ee::setGpr64(ctx, ee::reg::ra, read(sp() + 0x10u));
        ee::setGpr64(ctx, ee::reg::sp, sp() + 0x20u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void nestedCallee(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto word = [&](uint32_t address) -> uint32_t &
        { return *reinterpret_cast<uint32_t *>(rdram + address); };
        auto sp = [&] { return static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)); };
        if (ctx->pc == kNestedCalleeReturn)
            goto resume;
        if (word(0x5200u)++ != 0u)
        {
            ctx->pc = kNestedJumpTarget; // j 0x4900: not a call, the function is left
            return;
        }
        ee::setGpr64(ctx, ee::reg::sp, sp() - 0x10u);
        word(sp()) = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        ee::setGpr64(ctx, ee::reg::a0, 0x5304u);
        ee::setGpr64(ctx, ee::reg::ra, kNestedCalleeReturn);
        if (!runtime->dispatchGuestBranch(rdram, ctx, kNestedAlias, 0x4808u, kNestedCalleeReturn,
                                          PS2Runtime::GuestBranchKind::DirectCall, ""))
            return;
    resume:
        ee::setGpr64(ctx, ee::reg::ra, word(sp()));
        ee::setGpr64(ctx, ee::reg::sp, sp() + 0x10u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void nestedJumpTarget(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    READABLE_PLANNED(0x00004400, "nestedPlaying");

    // A function that reads and writes I/O registers, as traced code does
    // (TracedRuntime's accessors): a load of VIF1 MARK, then a store of
    // that value + 0x140 to D2 CHCR, and a quadword store.
    constexpr uint32_t kIoTouching = 0x00004A00u;
    uint32_t g_ioMark = 0, g_ioStored = 0;
    void ioTouchingTraced(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t mark = fncheck::ioLoad<uint32_t>(0x10003C30u, [] { return g_ioMark; });
        fncheck::ioStore<uint32_t>(0x1000A000u, mark + 0x140u, [&] { g_ioStored = mark + 0x140u; });
        const __m128i q = _mm_set_epi32(4, 3, 2, 1);
        fncheck::ioStore<__m128i>(0x10006000u, q, [] {});
        ee::setGpr64(ctx, ee::reg::v0, mark);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    READABLE_PLANNED(0x00004A00, "ioTouching");

    // A function whose code jumps into a second registered function inside its own extent (the shape of BMX's
    // ghostRideMove, which releases its frame and jumps to the riderless bike's move):
    // generated code runs the jump as a goto, so a recording of the outer function holds the inner one's calls as
    // its own and none of the inner function. The readable outer tail-calls the inner (game::tail, game::tailCall).
    constexpr uint32_t kJumpingOuter = 0x00002C00u, kJumpedInner = 0x00002C40u, kJumpingEnd = 0x00002C80u;
    constexpr uint32_t kJumpedOutside = 0x00002D00u, kJumpCallee = 0x00002E00u;
    volatile uint32_t g_jumpedInnerSp = 0;
    uint32_t jumpedInner_(uint32_t value)
    {
        g_jumpedInnerSp = ee::t_ee.frame ? ee::t_ee.frame->fnSp : 0u;
        const uint32_t first = ee::call<uint32_t>(kJumpCallee + 4u, value);
        return ee::call<uint32_t>(kJumpCallee + 8u, first);
    }
    READABLE_FUNCTION(0x00002C40, jumpedInner_, Draft);
    uint32_t jumpedOutside_(uint32_t value) { return value; }
    READABLE_FUNCTION(0x00002D00, jumpedOutside_, Draft);
    // Their entries, as readable_entries.py makes a plain source's (game::tail takes an entry).
    uint32_t jumpedInner(uint32_t value)
    {
        return ::game::detail::Route<&jumpedInner_>::template at<kJumpedInner, ::game::Game::Ai>(value);
    }
    uint32_t jumpedOutside(uint32_t value)
    {
        return ::game::detail::Route<&jumpedOutside_>::template at<kJumpedOutside, ::game::Game::Ai>(value);
    }
    enum class JumpBy : uint32_t
    {
        Tail,          // game::tail(jumpedInner, ...)
        TailCall,      // game::tailCall<&jumpedInner_>
        TailCallAt,    // game::tailCallAt<&jumpedInner_, kJumpedInner>
        TailOutside,   // game::tail(jumpedOutside, ...): outside the extent
    };
    uint32_t jumpingOuter(uint32_t value, uint32_t how)
    {
        const JumpBy by = static_cast<JumpBy>(how);
        // (a function of no frame table: its row by hand)
        static const ee::FrameRow row{0u, 0x00345000u, 0x20u, 0u, 0u, 0u, 0u};
        ee::t_ee.frame->layout = &row;
        const uint32_t first = ee::call<uint32_t>(kJumpCallee, value);
        switch (by)
        {
        case JumpBy::Tail:
            return game::tail(jumpedInner, first);
        case JumpBy::TailCall:
            return game::tailCall<&jumpedInner_>(first);
        case JumpBy::TailCallAt:
            return game::tailCallAt<&jumpedInner_, kJumpedInner>(first);
        case JumpBy::TailOutside:
            break;
        }
        return game::tail(jumpedOutside, first);
    }
    READABLE_FUNCTION(0x00002C00, jumpingOuter, Draft);
    // Answers calls as a recording does, in order (each returns its argument + 1), with ps2xFnCheck's rule for a
    // jump inside the function replayed (fncheck_main.cpp Script::jumpsInside).
    struct JumpInsideHandler : ee::CallHandler
    {
        uint32_t function = 0, end = 0;
        std::vector<uint32_t> recorded, made;
        void call(ee::CallRegs &regs) override
        {
            made.push_back(regs.target);
            regs.v0 = regs.a[0] + 1u;
        }
        bool jumpsInside(uint32_t target) const override
        {
            if (target < function || target >= end)
                return false;
            return made.size() >= recorded.size() || recorded[made.size()] != target;
        }
    };

    // game::tail's mark (ee::t_ee.tailNext) through callees that are not a readable function's entry: a plain helper
    // over ee::call (BMX's pathman::activatePlayerObjectiveByName over ee::game) and one that makes no call. The
    // mark is the helper's call's, and never a later call's (frame1: givePathManObjective's mark made
    // riderActivatesPathMan's next call a tail call).
    constexpr uint32_t kTailMarkOuter = 0x00002F00u, kTailMarkCallee = 0x00003700u;
    uint32_t plainTailHelper(uint32_t value) { return ee::call<uint32_t>(kTailMarkCallee, value); }
    uint32_t noCallHelper(uint32_t value) { return value + 1u; }
    uint32_t tailMarkOuter(uint32_t value, uint32_t how)
    {
        static const ee::FrameRow row{0u, 0x00345100u, 0x20u, 0u, 0u, 0u, 0u};
        ee::t_ee.frame->layout = &row;
        if (how == 0)
            return game::tail(plainTailHelper, value);
        const uint32_t next = game::tail(noCallHelper, value); // (a tail that is not the last act: the test's shape)
        return ee::call<uint32_t>(kTailMarkCallee + 4u, next);
    }
    READABLE_FUNCTION(0x00002F00, tailMarkOuter, Draft);
    struct TailMarkHandler : ee::CallHandler
    {
        std::vector<std::pair<uint32_t, bool>> made; // (target, a tail call)
        void call(ee::CallRegs &regs) override
        {
            made.emplace_back(regs.target, regs.tail);
            regs.v0 = regs.a[0] + 1u;
        }
    };

    // Two traced functions whose extents overlap (an entry_ function registered inside its neighbour's), with a
    // jump table's jump in each part (fncheck.cpp sameTracedFunction).
    constexpr uint32_t kOverlapOuter = 0x00008000u, kOverlapInner = 0x00008200u;
    int g_overlapOuterRan = 0, g_overlapInnerRan = 0;
    void overlapOuterTraced(uint8_t *, R5900Context *, PS2Runtime *) { ++g_overlapOuterRan; }
    void overlapInnerTraced(uint8_t *, R5900Context *, PS2Runtime *) { ++g_overlapInnerRan; }
    struct QuietTracer : fncheck::Tracer
    {
        void read(uint32_t, uint32_t) override {}
        void write(uint32_t, uint32_t) override {}
        void special(uint32_t) override {}
        void beforeCall(uint8_t *, R5900Context *, uint32_t) override {}
        void afterCall(uint8_t *, R5900Context *, bool) override {}
    };

    // A function of two entry_ chunks (ps2_fncheck_chunks_traced.cpp: twoChunks
    // at 0x4E00, its second chunk at 0x4E18), and the callee it calls, 0x6100:
    // v0 = a0 * 2, and a0 written to 0x5700.
    constexpr uint32_t kTwoChunks = 0x00004E00u, kSecondChunk = 0x00004E18u, kChunkCallee = 0x00006100u;
    void chunkCallee(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t a0 = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
        std::memcpy(rdram + 0x5700u, &a0, 4);
        ee::setGpr64(ctx, ee::reg::v0, a0 * 2u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    READABLE_PLANNED(0x00004E00, "twoChunks");

    // Callee writes as the recorder keeps them (round 14: page protection,
    // PS2X_FNREC_EFFECTS=pages, against the full RAM diff): pageWriter
    // (0x4C00, and the same at 0x4C80) calls 0x4D00, which writes a word in
    // one page and one in another, writes a word back unchanged, writes its
    // own frame (dead), and fills 64 bytes as a file read does
    // (ps2BeforeHostWrite, then the copy).
    constexpr uint32_t kPageWriter = 0x00004C00u, kPageWriterB = 0x00004C80u, kPageCallee = 0x00004D00u;
    void pageCallee(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        auto put = [&](uint32_t address, uint32_t value) { std::memcpy(rdram + address, &value, 4); };
        put(0x5400u, 0x01020304u);
        put(0x00300010u, 0xAABBCCDDu);
        uint32_t same = 0;
        std::memcpy(&same, rdram + 0x5408u, 4);
        put(0x5408u, same);
        put(static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)) - 0x40u, 0x77u);
        uint8_t block[64];
        std::memset(block, 0x5A, sizeof(block));
        ps2BeforeHostWrite(rdram + 0x00400000u, sizeof(block));
        std::memcpy(rdram + 0x00400000u, block, sizeof(block));
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    template <uint32_t Start>
    void pageWriterTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t ra = ee::gpr64(ctx, ee::reg::ra);
        ee::setGpr64(ctx, ee::reg::ra, Start + 0x0Cu);
        if (!fncheck::tracedDispatch(runtime, rdram, ctx, kPageCallee, Start + 8u, Start + 0x0Cu,
                                     static_cast<int>(PS2Runtime::GuestBranchKind::DirectCall), ""))
            return;
        ee::setGpr64(ctx, ee::reg::ra, ra);
        ctx->pc = static_cast<uint32_t>(ra);
    }
    READABLE_PLANNED(0x00004C00, "pageWriter");
    READABLE_PLANNED(0x00004C80, "pageWriterB");

    // A function that never returns (fncheck_stops.h, PS2X_FNREC_STOP): it
    // writes a0 + 1 at 0x5800, calls 0x6C00 with a0 (which writes 0x5810 and
    // returns a0 + 13), writes the result at 0x5804 and calls 0x6D00 with it,
    // which overwrites 0x5800 and then ends the thread (throws to the
    // scheduler, its thread gone). Traced as fncheck_traced.h traces
    // generated code.
    constexpr uint32_t kStopping = 0x00004B00u, kStoppingFirst = 0x00006C00u, kStoppingEnd = 0x00006D00u;
    void stoppingTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto word = [&](uint32_t address) -> uint32_t & { return *reinterpret_cast<uint32_t *>(rdram + address); };
        constexpr int kCall = static_cast<int>(PS2Runtime::GuestBranchKind::DirectCall);
        const uint32_t p = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
        fncheck::traceWrite(0x5800u, 4u);
        word(0x5800u) = p + 1u;
        ee::setGpr64(ctx, ee::reg::ra, kStopping + 0x10u);
        if (!fncheck::tracedDispatch(runtime, rdram, ctx, kStoppingFirst, kStopping + 8u, kStopping + 0x10u, kCall, ""))
            return;
        const uint32_t result = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::v0));
        fncheck::traceWrite(0x5804u, 4u);
        word(0x5804u) = result;
        ee::setGpr64(ctx, ee::reg::a0, result);
        ee::setGpr64(ctx, ee::reg::ra, kStopping + 0x20u);
        if (!fncheck::tracedDispatch(runtime, rdram, ctx, kStoppingEnd, kStopping + 0x18u, kStopping + 0x20u, kCall, ""))
            return;
        ctx->pc = 0u;
    }
    void stoppingFirst(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        *reinterpret_cast<uint32_t *>(rdram + 0x5810u) = 0x77u;
        ee::setGpr64(ctx, ee::reg::v0, ee::gpr64(ctx, ee::reg::a0) + 13u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void stoppingEnd(uint8_t *rdram, R5900Context *, PS2Runtime *)
    {
        *reinterpret_cast<uint32_t *>(rdram + 0x5800u) = 0xDEADu;
        throw EeDispatcherTransfer{};
    }
    READABLE_PLANNED(0x00004B00, "stoppingTest");
    // A record on the caller's stack read by its callees (tools44: BMX XXX's
    // beginObjectiveBlock hands registerBlockClass the set-up of a block
    // class in its frame). frameRecordCaller (0x7000) builds three words in
    // its frame, at sp + 0x10, 0x14 and 0x18 (the last read by no one), then
    // calls frameRecordReader (0x7100: the sum of the two words at a0) with
    // a0 0x5600 (not its frame), with a0 sp + 0x10, and calls
    // frameRecordRelay (0x7200: a frame of its own, the reader called with
    // the a0 it was given) with a0 sp + 0x10.
    constexpr uint32_t kFrameCaller = 0x00007000u, kFrameReader = 0x00007100u, kFrameRelay = 0x00007200u;
    template <bool Traced>
    struct FrameRecordCode
    {
        static uint32_t &word(uint8_t *rdram, uint32_t address) { return *reinterpret_cast<uint32_t *>(rdram + address); }
        static uint32_t sp(R5900Context *ctx) { return static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::sp)); }
        static uint32_t read(uint8_t *rdram, uint32_t address)
        {
            if (Traced)
                fncheck::traceRead(address, 4u);
            return word(rdram, address);
        }
        static void write(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (Traced)
                fncheck::traceWrite(address, 4u);
            word(rdram, address) = value;
        }
        static bool call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t target, uint32_t a0, uint32_t back)
        {
            ee::setGpr64(ctx, ee::reg::a0, a0);
            ee::setGpr64(ctx, ee::reg::ra, back);
            const int kind = static_cast<int>(PS2Runtime::GuestBranchKind::DirectCall);
            return Traced ? fncheck::tracedDispatch(runtime, rdram, ctx, target, back - 8u, back, kind, "")
                          : runtime->dispatchGuestBranch(rdram, ctx, target, back - 8u, back,
                                                         PS2Runtime::GuestBranchKind::DirectCall, "");
        }
        static void caller(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ee::setGpr64(ctx, ee::reg::sp, sp(ctx) - 0x40u);
            write(rdram, sp(ctx) + 0x30u, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra)));
            write(rdram, sp(ctx) + 0x10u, 0xAAAA0001u);
            write(rdram, sp(ctx) + 0x14u, 0xBBBB0002u);
            write(rdram, sp(ctx) + 0x18u, 0xCCCC0003u);
            if (!call(rdram, ctx, runtime, kFrameReader, 0x5600u, kFrameCaller + 0x10u) ||
                !call(rdram, ctx, runtime, kFrameReader, sp(ctx) + 0x10u, kFrameCaller + 0x20u) ||
                !call(rdram, ctx, runtime, kFrameRelay, sp(ctx) + 0x10u, kFrameCaller + 0x30u))
                return;
            ee::setGpr64(ctx, ee::reg::ra, read(rdram, sp(ctx) + 0x30u));
            ee::setGpr64(ctx, ee::reg::sp, sp(ctx) + 0x40u);
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        }
        static void reader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t a0 = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
            ee::setGpr64(ctx, ee::reg::v0, read(rdram, a0) + read(rdram, a0 + 4u));
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        }
        static void relay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ee::setGpr64(ctx, ee::reg::sp, sp(ctx) - 0x20u);
            write(rdram, sp(ctx) + 0x10u, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra)));
            write(rdram, sp(ctx), 0x12345678u);
            if (!call(rdram, ctx, runtime, kFrameReader, static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0)), kFrameRelay + 0x10u))
                return;
            ee::setGpr64(ctx, ee::reg::ra, read(rdram, sp(ctx) + 0x10u));
            ee::setGpr64(ctx, ee::reg::sp, sp(ctx) + 0x20u);
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        }
    };
    READABLE_PLANNED(0x00007000, "frameRecordCaller");
    READABLE_PLANNED(0x00007100, "frameRecordReader");
    READABLE_PLANNED(0x00007200, "frameRecordRelay");

    // A function of two blocks (PS2X_FNREC_NEW_BLOCKS=3, the recorder's refusal): its first block writes a0 to
    // 0x5900 and returns a0 + 1; its second runs only for a0 = 7. The traced original reports its blocks as
    // fncheck_traced.h's markers do. Four copies at four addresses: the recorder keeps a function's state for the
    // process, so each install of a test records a function of its own.
    template <uint32_t Address>
    struct TwoBlocks
    {
        static constexpr uint32_t kStarts[2] = {Address, Address + 0x10u};
        static inline const fncheck::BlockTable kTable{Address, game::Game::Ai, kStarts, 2u, "two-blocks-test"};
        template <bool Traced>
        static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t a0 = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::a0));
            if constexpr (Traced)
            {
                fncheck::blockHit(kTable, 0u);
                fncheck::traceWrite(0x5900u, 4u);
            }
            std::memcpy(rdram + 0x5900u, &a0, 4);
            if (a0 == 7u)
            {
                if constexpr (Traced)
                    fncheck::blockHit(kTable, 1u);
            }
            ee::setGpr64(ctx, ee::reg::v0, a0 + 1u);
            ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
        }
    };
    constexpr uint32_t kTwoBlocksCapped = 0x00007400u, kTwoBlocksMode3 = 0x00007480u;
    constexpr uint32_t kTwoBlocksAllowed = 0x00007500u, kTwoBlocksOff = 0x00007580u;
    READABLE_PLANNED(0x00007400, "twoBlocksCapped");
    READABLE_PLANNED(0x00007480, "twoBlocksMode3");
    READABLE_PLANNED(0x00007500, "twoBlocksAllowed");
    READABLE_PLANNED(0x00007580, "twoBlocksOff");
    constexpr uint32_t kTwoBlocksUntilPast = 0x00007600u, kTwoBlocksUntilAhead = 0x00007680u;
    READABLE_PLANNED(0x00007600, "twoBlocksUntilPast");
    READABLE_PLANNED(0x00007680, "twoBlocksUntilAhead");

    // Two games' functions at one address (ee/game_id.h): AI's Verified one
    // must never install over DM2's code there.
    volatile uint32_t g_gameProbe = 0; // (volatile: the store is the body)
    void aiAt800(uint32_t value) { g_gameProbe = value + 0xA1u; }
    void dm2At800(uint32_t value) { g_gameProbe = value * 3u + 0xD2u; }
    READABLE_FUNCTION(0x00000800, aiAt800, Verified);
    READABLE_FUNCTION_IN(dm2, 0x00000800, dm2At800, Tested);
    // DM2's planned function at 0x900: adds 1 to the word at 0x5200 into
    // 0x5204, then jr ra (traced as fncheck_traced.h traces generated code).
    constexpr uint32_t kDm2Planned = 0x00000900u;
    READABLE_PLANNED_IN(dm2, 0x00000900, "dm2PlannedProbe");
    // DM2's waiting form (ee/readable_waits.h): a function whose callees can wait.
    volatile uint32_t g_dm2WaitsProbe = 0; // (volatile: the store is the body)
    int32_t dm2WaitsAtA00(uint32_t value) { g_dm2WaitsProbe = value ^ 0xA00u; return static_cast<int32_t>(value); }
    READABLE_FUNCTION_WAITS_IN(dm2, 0x00000A00, dm2WaitsAtA00, Draft);
    void dm2PlannedTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint32_t value = 0;
        fncheck::traceRead(0x5200u, 4u);
        std::memcpy(&value, rdram + 0x5200u, 4);
        ++value;
        fncheck::traceWrite(0x5204u, 4u);
        std::memcpy(rdram + 0x5204u, &value, 4);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void recompiledAt800(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    static game::readable::TracedRegistration g_dm2TracedAt800(0x00000800u, &recompiledAt800, game::Game::Dm2);

    // A tail call as generated code makes it (PS2Runtime::tailCall): the
    // recompiled target sets v0 1, a replacement 2.
    void tailTarget(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ee::setGpr64(ctx, ee::reg::v0, 1u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void tailReplacement(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ee::setGpr64(ctx, ee::reg::v0, 2u);
        ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
    }
    void tailCaller(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0x00007000u;
        runtime->tailCall(rdram, ctx, 0x00007000u, 0x00006F00u, &tailTarget);
    }

    void setEnvironment(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        ::setenv(name, value, 1);
#endif
    }

    // The recorder as a recording run installs it: with readable code off (it refuses otherwise, fncheck.h).
    bool installTestRecorder(PS2Runtime &runtime, game::Game game = game::Game::Ai)
    {
        const char *was = std::getenv("PS2X_READABLE");
        const std::string previous = was ? was : "";
        setEnvironment("PS2X_READABLE", "off");
        const bool installed = fncheck::installRecorder(runtime, game);
        setEnvironment("PS2X_READABLE", previous.c_str());
        return installed;
    }

    struct GuestRamFixture
    {
        // RAM as the game has it: the guest map (ee::at is its base + address).
        guest_map::Map map;
        uint8_t *previous = ee::g_ram;
        GuestRamFixture() { ee::g_ram = map.create(PS2_RAM_SIZE, guest_map::Layout::Game) ? map.base() : nullptr; }
        ~GuestRamFixture() { ee::g_ram = previous; }
    };

    R5900Context callContext(uint32_t ra)
    {
        R5900Context ctx;
        for (int r = 1; r < 32; ++r)
            ee::setGpr64(&ctx, r, 0x1111111100000000ull + static_cast<uint64_t>(r));
        ee::setGpr64(&ctx, ee::reg::sp, 0x01F00000u);
        ee::setGpr64(&ctx, ee::reg::ra, ra);
        return ctx;
    }
}

namespace
{
    // The held-register table (ee/held.h) for one made-up function: a call
    // that keeps the entry's a1 in s0 and a frame address in s1 (one the site
    // does not prove, kChecked, and the case has no check: not given), one
    // after it with s1 the first call's result + 4, two sites of one callee
    // told apart by their first argument (s2 = 7 or 9, kChecked: their check
    // proves it; a third site of it, which would give s2 = 11, is ruled out by a
    // condition its path met that fails, kind 18), and a callee the table does not know.
    constexpr uint32_t kHeldFunction = 0x00346000u;
    const ee::FrameRow kHeldRow{0u, kHeldFunction, 0x40u, 0u, 0u, 0u, 0u};
    using namespace ee::held;
    const Node kHeldNodes[] = {
        {kEntry, 5, 0},                                   // 0: a1 at entry
        {kEntry, 29, 0}, {kAlu, kDaddiu, -0x30},          // 1: sp - 0x30
        {kResult, 0, 0}, {kAlu, kAddiu, 4},               // 3: v0 + 4
        {kConst, 0, 7}, {kConst, 0, 1}, {kConst, 0, 9}, {kConst, 0, 2}, // 5..8
        {kConst, 0, 11}, {kConst, 0, 0},                                  // 9, 10
    };
    // (Value: slot, node count, first node; Case: prev, first value, first check, values, checks)
    const Value kHeldValues[] = {{0u, 1u, 0u}, {1u | kChecked, 2u, 1u}, {1u, 2u, 3u}, {2u | kChecked, 1u, 5u}, {2u | kChecked, 1u, 7u},
                                 {2u, 1u, 9u}};
    const Value kHeldChecks[] = {{0u, 1u, 6u}, {0u, 1u, 8u}, {18u, 1u, 10u}};
    const Case kHeldCases[] = {
        {kFromEntry, 0u, 0u, 2u, 0u},
        {0x00346010u, 2u, 0u, 1u, 0u, 1u, 0u}, // (its values as code too: heldCaseValues)
        {kLost, 5u, 0u, 0u, 0u}, // (after a call not placed: nothing known)
        {0x00346020u, 3u, 0u, 1u, 1u},
        {0x00346020u, 4u, 1u, 1u, 1u},
        {0x00346020u, 5u, 2u, 1u, 1u},
    };
    const Site kHeldSites[] = {
        {0x00346010u, 0x00123450u, 0u, 1u},
        {0x00346020u, 0x00123460u, 1u, 2u},
        {0x00346030u, 0x00123470u, 3u, 1u},
        {0x00346040u, 0x00123470u, 4u, 1u},
        {0x00346050u, 0x00123470u, 5u, 1u},
    };
    const Function kHeldFunctions[] = {{0u, kHeldFunction, 0u, 5u, kReadsEntryArguments | kReadsResults}};
    // Case 1's values as readable_held_code.py writes them (s1 = v0 + 4): the call after the first is placed from its
    // successor alone (perf7's common call), the others through the search.
    void heldCaseValues(const program::Context &c, bool, program::Worked &w)
    {
        uint64_t v0 = 0, x = 0;
        if (program::result(c, 0, v0) && program::alu(kAddiu, 4, v0, 0u, 0u, x))
        {
            w.value[1] = x;
            w.known |= 1u << 1;
        }
        else
            w.cleared |= 1u << 1;
    }
    const program::CaseValuesCode kHeldCaseValues[] = {nullptr, &heldCaseValues};
    const Registration g_testHeld{{kHeldFunctions, 1u, kHeldSites, kHeldCases, kHeldValues, kHeldChecks, kHeldNodes,
                                   nullptr, nullptr, nullptr, kHeldCaseValues, nullptr}};

    // What each call was given in s0, s1, s2 and ra (~0: nothing published,
    // the entry value), as ps2xFnCheck's compareSaves sees it.
    struct HeldHandler : ee::CallHandler
    {
        std::vector<std::vector<uint64_t>> got;
        void call(ee::CallRegs &regs) override
        {
            ee::Frame *frame = ee::t_ee.frame;
            std::vector<uint64_t> row;
            for (int r : {16, 17, 18, 31})
            {
                const int slot = ee::savedSlot(r);
                row.push_back((frame->next.mask & (1u << slot)) ? frame->next.value[slot] : ~0ull);
            }
            got.push_back(row);
            frame->next.mask = 0; // (used up by the call, as applySaves does)
            regs.v0 = 0x500u;
        }
    };

    uint32_t heldCaller(uint32_t a, uint32_t b)
    {
        // (a function of no frame table: its row by hand, entered as ee::enterFrame enters it)
        ee::t_ee.frame->layout = &kHeldRow;
        ee::t_ee.frame->fnSp = 0x01F00000u;
        ee::held::enter(*ee::t_ee.frame, ee::held::rowOf(ee::t_ee, &kHeldRow), nullptr, ee::t_ee);
        const uint32_t r = ee::call<uint32_t>(0x00123450u, a);
        ee::call<void>(0x00123460u, r);
        ee::call<void>(0x00123470u, b == 2u ? 2u : 1u);
        ee::call<void>(0x00123480u);
        ee::call<void>(0x00123460u, r);
        return r;
    }
    // A function the table has no row for.
    void notHeldCaller(uint32_t a) { ee::call<void>(0x00123450u, a); }
}

namespace fncheck_test
{
    PS2Runtime::RecompiledFunction twoChunksWhole();
    PS2Runtime::RecompiledFunction twoChunksFirstChunk();
}

void register_ps2_readable_tests()
{
    MiniTest::Case("Readable", [](TestCase &tc)
    {
        tc.Run("ee::Local: declaration order in the frame, its place back at the end of its scope", [](TestCase &t)
        {
            const ee::FrameRow row{0u, 0x00345000u, 0x40u, 0u, 0u, 0u, 0u};
            ee::Frame frame;
            frame.fnSp = 0x00100040u;
            frame.layout = &row;
            ee::Frame *const before = ee::t_ee.frame;
            ee::t_ee.frame = &frame;
            {
                ee::Local<uint32_t> number;
                t.Equals(number.address(), 0x00100000u, "first local at the frame's bottom");
                for (int i = 0; i < 2; ++i)
                {
                    ee::Local<uint64_t> wide;
                    t.Equals(wide.address(), 0x00100008u, "aligned, and the same place each time round");
                }
                ee::Local<uint32_t> next;
                t.Equals(next.address(), 0x00100004u, "the scope's end gave the place back");
            }
            t.Equals(frame.localCount, 0u, "nothing left claimed");
            ee::t_ee.frame = before;
        });

        tc.Run("EE float operators round like the EE FPU helpers", [](TestCase &t)
        {
            const float values[] = {1.0f, 3.0f, 1e-3f, -7.25f, 16777215.0f, 0.1f, 1.0e30f, -2.5e-30f};
            for (float a : values)
                for (float b : values)
                {
                    t.Equals(bits((ee::Float(a) + ee::Float(b)).raw()), bits(ps2_fadd(a, b)), "add");
                    t.Equals(bits((ee::Float(a) - ee::Float(b)).raw()), bits(ps2_fsub(a, b)), "sub");
                    t.Equals(bits((ee::Float(a) * ee::Float(b)).raw()), bits(ps2_fmul(a, b)), "mul");
                    t.Equals(bits((ee::Float(a) / ee::Float(b)).raw()), bits(ps2_fdiv_nearest(a, b)), "div");
                }
            // 1 + 2^-24 * 1.5 rounds to nearest on a host (up) but towards zero on the EE.
            t.Equals(bits((ee::Float(1.0f) + ee::Float(1.5f * 0x1p-24f)).raw()), 0x3F800000u, "chopped sum");
            // Exponent 255 is a number on the EE (no NaN): 0x7FFFFFFF is the largest float.
            t.IsTrue(ee::Float::fromBits(0x7FFFFFFFu) > ee::Float(1.0f), "max float compares as a number");
            t.Equals(bits(ee::abs(ee::Float(-2.0f)).raw()), 0x40000000u, "abs clears the sign bit");
            t.Equals(ee::toInt(ee::Float(-2.75f)), -2, "CVT.W.S truncates");
        });

        tc.Run("entry adapter reads EABI arguments and sets the result", [](TestCase &t)
        {
            GuestRamFixture ram;
            R5900Context ctx = callContext(0x00123458u);
            ee::setGpr64(&ctx, 4, 7u);                       // a0: first
            ee::setGpr64(&ctx, 5, static_cast<uint64_t>(-3)); // a1: second (int)
            ctx.f[12] = 2.0f;                                 // f12: scale
            ctx.f[13] = 4.5f;                                 // f13: offset
            ee::entry<&mixArguments>(ram.map.base(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 7000u - 30u + 9u, "v0");
            t.Equals(ctx.pc, 0x00123458u, "returns to ra");
            t.Equals(ee::gpr64(&ctx, 16), 0x1111111100000010ull, "s0 untouched");
            t.Equals(ee::gpr64(&ctx, 8), 0x1111111100000008ull, "t0 untouched");
        });

        tc.Run("pointers in and out, float results", [](TestCase &t)
        {
            GuestRamFixture ram;
            Pair *src = ee::at<Pair>(0x00200000u);
            src->a = 1.5f;
            src->b = -4.0f;
            R5900Context ctx = callContext(0x00100010u);
            ee::setGpr64(&ctx, 4, 0x00200100u);
            ee::setGpr64(&ctx, 5, 0x00200000u);
            ee::entry<&swapPair>(ram.map.base(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 0x00200100u, "returns dst as an EE address");
            t.Equals(bits(ee::at<Pair>(0x00200100u)->a.raw()), bits(-4.0f), "swapped a");
            t.Equals(ee::addressOf(src), 0x00200000u, "addressOf");
            t.IsTrue(ee::pointerAt<Pair>(0) == nullptr, "address 0 is null");

            R5900Context ctx2 = callContext(0x00100020u);
            ee::setGpr64(&ctx2, 4, 0x00200000u);
            ee::entry<&sumPair>(ram.map.base(), &ctx2, nullptr);
            t.Equals(bits(ctx2.f[0]), bits(ps2_fadd(1.5f, -4.0f)), "f0");
            ee::Ptr<Pair> p(0x00200000u);
            t.Equals(bits(p->b.raw()), bits(-4.0f), "Ptr dereferences");
        });

        tc.Run("held registers: a call the table has a row for gets the original's values, one without the entry's", [](TestCase &t)
        {
            GuestRamFixture ram;
            HeldHandler handler;
            ee::t_ee.callHandler = &handler;
            R5900Context ctx = callContext(0x00100030u);
            ee::setGpr64(&ctx, 4, 0x11u);
            ee::setGpr64(&ctx, 5, 0x00200040u);
            ee::entry<&heldCaller>(ram.map.base(), &ctx, nullptr);
            t.Equals(handler.got.size(), size_t(5), "five calls");
            t.Equals(handler.got[0][0], uint64_t(0x00200040u), "first call: s0 is the entry's a1");
            t.Equals(handler.got[0][1], ~0ull, "first call: s1 needs a check the case has not: the entry value");
            t.Equals(handler.got[0][2], ~0ull, "first call: s2 unchanged, its entry value (not published)");
            t.Equals(handler.got[0][3], uint64_t(0x00346018u), "first call: ra the jal's + 8");
            t.Equals(handler.got[1][0], uint64_t(0x00200040u), "second call: s0 still what the first call got");
            t.Equals(handler.got[1][1], uint64_t(0x504u), "second call: s1 the first call's result + 4");
            t.Equals(handler.got[1][3], uint64_t(0x00346028u), "second call: its own ra");
            t.Equals(handler.got[2][2], uint64_t(7u), "a callee with two sites: the argument picks the first (s2 = 7; the third ruled out)");
            t.Equals(handler.got[2][3], uint64_t(0x00346038u), "and its ra");
            t.Equals(handler.got[3][0], ~0ull, "a callee the table does not know: nothing published (the entry values)");
            t.Equals(handler.got[3][3], ~0ull, "nor ra");
            t.Equals(handler.got[4][0], ~0ull, "after it the place is lost: still the entry values");
            t.Equals(handler.got[4][3], uint64_t(0x00346028u), "but the site is known again by its callee (ra)");

            handler.got.clear();
            R5900Context ctx2 = callContext(0x00100030u);
            ee::setGpr64(&ctx2, 4, 0x11u);
            ee::setGpr64(&ctx2, 5, 2u);
            ee::entry<&heldCaller>(ram.map.base(), &ctx2, nullptr);
            t.Equals(handler.got[2][2], uint64_t(9u), "the other site of that callee by its argument (s2 = 9)");
            t.Equals(handler.got[2][3], uint64_t(0x00346048u), "and its ra");

            handler.got.clear();
            R5900Context ctx3 = callContext(0x00100030u);
            ee::setGpr64(&ctx3, 4, 0x11u);
            ee::entry<&notHeldCaller>(ram.map.base(), &ctx3, nullptr);
            ee::t_ee.callHandler = nullptr;
            t.Equals(handler.got.size(), size_t(1), "one call");
            t.Equals(handler.got[0][0], ~0ull, "no row: s0 not published, the entry value");
            t.Equals(handler.got[0][3], ~0ull, "no row: no ra");
        });

        tc.Run("held registers: a readable callee called directly works in a place of its own, its caller's kept", [](TestCase &t)
        {
            GuestRamFixture ram;
            HeldHandler handler;
            ee::t_ee.callHandler = &handler;
            R5900Context ctx = callContext(0x00100030u);
            ee::setGpr64(&ctx, 4, 0x11u);
            ee::setGpr64(&ctx, 5, 0x00200040u);
            ee::Frame frame{ram.map.base(), &ctx, nullptr, 0x01F00000u, 0x00100030u, 0x01F00000u - 0x40u, 0x01F00000u};
            ee::Frame *const before = ee::t_ee.frame;
            ee::t_ee.frame = &frame;
            ee::FunctionScope outer;
            ee::enterFrame(outer, frame, &kHeldRow, 0x01F00000u, ee::t_ee);
            t.IsTrue(frame.calleeHeld == nullptr && frame.held().function != nullptr, "the frame's function: the frame's own place");
            const uint32_t r = ee::call<uint32_t>(0x00123450u, 0x11u);
            const ee::held::State caller = frame.held();
            {
                // (the same function's row, called directly: its own two calls take its place past the caller's)
                ee::FunctionScope callee;
                ee::enterFrame(callee, frame, &kHeldRow, frame.calleeSp, ee::t_ee);
                t.IsTrue(frame.calleeHeld == &callee.held, "a callee with a row: a place of its own, in its scope");
                const uint32_t inner = ee::call<uint32_t>(0x00123450u, 0x22u);
                ee::call<void>(0x00123460u, inner);
                t.Equals(frame.held().prev[0], 0x00346020u, "the callee's place after its second call");
                ee::leaveFrame(frame, callee);
            }
            t.IsTrue(frame.calleeHeld == nullptr, "the caller's place back");
            t.Equals(frame.held().prev[0], caller.prev[0], "the caller's place: its last call's site as it was");
            t.Equals(frame.held().known, caller.known, "and what it knows");
            ee::call<void>(0x00123460u, r);
            t.Equals(handler.got.back()[1], uint64_t(0x504u), "the caller's next call: s1 its first call's result + 4");
            t.Equals(handler.got.back()[3], uint64_t(0x00346028u), "and that site's ra");

            // A leaf (no row): the caller's place without its function, which comes back; a callee with a row under
            // it works in that same place (it has no function to go back to), and only its null function comes back.
            const ee::FrameRow leafRow{0u, 0x00347000u, 0x20u, 0u, 0u, 0u, 0u};
            {
                ee::FunctionScope leaf;
                ee::enterFrame(leaf, frame, &leafRow, frame.calleeSp, ee::t_ee);
                t.IsTrue(frame.calleeHeld == nullptr && frame.held().function == nullptr, "a leaf: the caller's place, no function");
                {
                    ee::FunctionScope inner;
                    ee::enterFrame(inner, frame, &kHeldRow, frame.calleeSp, ee::t_ee);
                    t.IsTrue(frame.calleeHeld == nullptr && frame.held().function == caller.function,
                             "a callee with a row under a leaf: the shared place, its function in it");
                    ee::leaveFrame(frame, inner);
                    t.IsTrue(frame.held().function == nullptr, "its null function back");
                }
                ee::leaveFrame(frame, leaf);
            }
            t.IsTrue(frame.held().function == caller.function, "the caller's function back after the leaf");
            t.Equals(frame.held().flags, caller.flags, "with its flags");
            ee::t_ee.frame = before;
            ee::t_ee.callHandler = nullptr;
        });

        tc.Run("keepLeft: each register's halves as gpr64 and gprHigh64 read them, r0 0, the FPRs", [](TestCase &t)
        {
            R5900Context ctx{};
            for (int r = 0; r < 32; ++r)
                ctx.r[r] = _mm_set_epi64x(static_cast<int64_t>(0xA000000000000000ull | (uint64_t(r) << 8)),
                                          static_cast<int64_t>(0x5000000000000000ull | uint64_t(r)));
            for (int f = 0; f < 32; ++f)
                ctx.f[f] = static_cast<float>(f) * 0.5f;
            ee::keepLeft(ctx);
            bool same = true;
            for (int r = 0; r < 32; ++r)
                same = same && ee::t_ee.left.low[r] == ee::gpr64(&ctx, r) && ee::t_ee.left.high[r] == ee::gprHigh64(&ctx, r);
            t.IsTrue(same, "every register's low and upper half");
            t.Equals(ee::t_ee.left.low[0], uint64_t(0), "r0's low half 0");
            t.Equals(ee::t_ee.left.high[0], uint64_t(0), "r0's upper half 0");
            t.Equals(bits(ee::t_ee.left.f[7]), bits(3.5f), "an FPR");
        });

        tc.Run("held registers: the original's operations as generated code runs them", [](TestCase &t)
        {
            ee::Frame frame;
            ee::held::State state;
            ee::CallRegs regs;
            regs.intCount = 1;
            regs.a[0] = 0x7FFFFFFFu;
            auto run = [&](std::initializer_list<ee::held::Node> nodes, uint64_t &out)
            { return ee::held::evaluate(frame, state, regs, nodes.begin(), static_cast<uint32_t>(nodes.size()), out); };
            uint64_t v = 0;
            t.IsTrue(run({{kArgument, 0, 0}, {kAlu, kAddiu, 1}}, v), "addiu");
            t.Equals(v, 0xFFFFFFFF80000000ull, "addiu: 32 bits, sign-extended");
            t.IsTrue(run({{kArgument, 0, 0}, {kAlu, kDaddiu, 1}}, v), "daddiu");
            t.Equals(v, 0x80000000ull, "daddiu: 64 bits");
            t.IsTrue(run({{kConst, 0, 3}, {kAlu, kSll, 31}}, v), "sll");
            t.Equals(v, 0xFFFFFFFF80000000ull, "sll: sign-extended");
            t.IsTrue(run({{kConst, 0, 5}, {kConst, 0, 0}, {kConst, 0, 9}, {kAlu, kMovz, 0}}, v), "movz");
            t.Equals(v, 5ull, "movz: the source when the condition is 0");
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.5f * 0x1p-24f))},
                          {kAlu, kAddS, 0}}, v), "add.s");
            t.Equals(v, uint64_t(bits(ps2_fadd(1.0f, 1.5f * 0x1p-24f))), "add.s: the EE's rounding");
            t.IsTrue(!run({{kArgument, 1, 0}}, v), "an argument the call does not pass: not known");
            t.IsTrue(!run({{kPrevious, 0, 0}}, v), "what a call got, with no call placed: not known");
            t.IsTrue(!run({{kConst, 0, 0x10000000}, {kLoad, kLw, 0}}, v), "no load from I/O space");
            t.IsTrue(run({{kConst, 0, 5}, {kConst, 0, 0x1D}, {kConst, 0, 0x1D}, {kAlu, kXor, 0}, {kConst, 0, 9}, {kAlu, kMovn, 0}}, v),
                     "a select");
            t.Equals(v, 9ull, "a select: the not-taken value when the branch's operands are equal");
            state.entryLoads = 0;
            t.IsTrue(!run({{kEntryLoad, 0, 0}}, v), "an entry load not read: not known");
            state.entryLoads = 1u;
            state.entryLoad[0] = 0x2Au;
            t.IsTrue(run({{kEntryLoad, 0, 0}, {kAlu, kSll, 2}}, v), "an entry load");
            t.Equals(v, 0xA8ull, "an entry load: as read at the entry");
            t.IsTrue(run({{kArgument, 0, 0}, {kConst, 0, 0xC0}, {kAlu, kMultLo, 0}}, v), "mult's LO");
            t.Equals(v, 0xFFFFFFFFFFFFFF40ull, "mult's LO: the low word, sign-extended");
            t.IsTrue(run({{kArgument, 0, 0}, {kConst, 0, 0xC0}, {kAlu, kMultHi, 0}}, v), "mult's HI");
            t.Equals(v, 0x5Full, "mult's HI: the high word");
            t.IsTrue(run({{kConst, 0, -7}, {kConst, 0, 2}, {kAlu, kDivLo, 0}}, v), "div's LO");
            t.Equals(v, ~2ull, "div's LO: the quotient toward 0 (-3)");
            t.IsTrue(run({{kConst, 0, -7}, {kConst, 0, 2}, {kAlu, kDivHi, 0}}, v), "div's HI");
            t.Equals(v, ~0ull, "div's HI: the remainder (-1)");
            t.IsTrue(run({{kConst, 0, 5}, {kConst, 0, 0}, {kAlu, kDivuLo, 0}}, v), "divu by 0");
            t.Equals(v, ~0ull, "divu by 0: LO all ones, as generated code leaves it");
            t.IsTrue(run({{kConst, 0, -1}, {kConst, 0, 0x10}, {kAlu, kDivuHi, 0}}, v), "divu's HI");
            t.Equals(v, 0xFull, "divu's HI: 0xFFFFFFFF % 0x10");
            // The FPU's condition (what bc1t/bc1f branch on), 1 or 0, with the PS2's compare.
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kAlu, kCEqS, 0}}, v), "c.eq.s");
            t.Equals(v, 1ull, "c.eq.s: equal floats");
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(-2.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kAlu, kCLtS, 0}}, v), "c.lt.s");
            t.Equals(v, 1ull, "c.lt.s: -2 < 1");
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kAlu, kCLtS, 0}}, v), "c.lt.s equal");
            t.Equals(v, 0ull, "c.lt.s: not 1 < 1");
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kAlu, kCLeS, 0}}, v), "c.le.s");
            t.Equals(v, 1ull, "c.le.s: 1 <= 1");
            t.IsTrue(run({{kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kConst, 0, static_cast<int32_t>(bits(1.0f))}, {kAlu, kCEqS, 0},
                          {kSkipIf, 0, 2}, {kConst, 0, 7}, {kSkip, 0, 1}, {kConst, 0, 9}}, v), "a select a bc1t makes");
            t.Equals(v, 7ull, "a select a bc1t makes: the taken arm when the condition holds");
            // A loop program: a count carried from 0, stepped by 1 while it is below 3; the call's path when not.
            t.IsTrue(run({{kConst, 0, 0}, {kLoop, 1, 14},
                          {kLeaf, 1, 4}, {kCarried, 0, 0}, {kConst, 0, 3}, {kAlu, kSltu, 0}, {kAlu, kSltiu, 1}, {kLeafEnd, 0, 1}, {kCarried, 0, 0},
                          {kLeaf, 0, 3}, {kCarried, 0, 0}, {kConst, 0, 3}, {kAlu, kSltu, 0}, {kLeafEnd, 0, 2}, {kCarried, 0, 0}, {kAlu, kAddiu, 1}}, v),
                     "a loop program");
            t.Equals(v, 3ull, "a loop program: the count when it reaches the bound");
            t.IsTrue(!run({{kConst, 0, 0}, {kLoop, 1, 5}, {kLeaf, 0, 1}, {kConst, 0, 1}, {kLeafEnd, 0, 2}, {kCarried, 0, 0}, {kAlu, kAddiu, 1}}, v),
                     "a loop program whose call's path is never taken: not known");
            state.returnLoads = 0;
            t.IsTrue(!run({{kReturnLoad, 0, 0}}, v), "a return load not read: not known");
            state.returnLoads = 2u;
            state.returnLoad[1] = 0x2D9274u;
            t.IsTrue(run({{kReturnLoad, 1, 0}, {kAlu, kAddiu, 8}}, v), "a return load");
            t.Equals(v, 0x2D927Cull, "a return load: as read when the last call returned");
            // A select worked out lazily: the arm not taken is not worked out (its load of I/O space would fail).
            t.IsTrue(run({{kConst, 0, 0}, {kSkipIf, 0, 3}, {kConst, 0, 0x10000000}, {kLoad, kLw, 0}, {kSkip, 0, 1}, {kConst, 0, 7}}, v),
                     "a lazy select, the other arm");
            t.Equals(v, 7ull, "a lazy select: the other arm's value");
            t.IsTrue(!run({{kConst, 0, 1}, {kSkipIf, 0, 3}, {kConst, 0, 0x10000000}, {kLoad, kLw, 0}, {kSkip, 0, 1}, {kConst, 0, 7}}, v),
                     "a lazy select, the taken arm worked out (its load: not known)");
            t.IsTrue(run({{kConst, 0, 0}, {kSkipIf, 1, 2}, {kConst, 0, 5}, {kSkip, 0, 1}, {kConst, 0, 9}}, v), "a lazy movz");
            t.Equals(v, 5ull, "a lazy movz: the taken arm when the condition is 0");
            {
                // A word of the function's own frame: known inside an ee::Local only (ee/held.h State).
                GuestRamFixture ram;
                frame.fnSp = 0x01F00000u;
                frame.layout = &kHeldRow; // (a 0x40 frame)
                const uint32_t word = 0x01F00000u - 0x40u + 0x10u;
                *ee::at<uint32_t>(word) = 0x1234u;
                state.localRanges = 0;
                const ee::held::Node frameWord[] = {{kConst, 0, static_cast<int32_t>(word)}, {kLoad, kLw, 0}};
                t.IsTrue(!ee::held::evaluate(frame, state, regs, frameWord, 2u, v), "a frame word outside any Local: not known");
                state.localRanges = 1;
                state.localFrom[0] = 0x10u;
                state.localTo[0] = 0x20u;
                t.IsTrue(ee::held::evaluate(frame, state, regs, frameWord, 2u, v), "a frame word inside a Local");
                t.Equals(v, 0x1234ull, "a frame word inside a Local: as the Local holds it");
                frame.fnSp = 0u;
                frame.layout = nullptr;
            }
        });

        tc.Run("stack locals and calls through the handler", [](TestCase &t)
        {
            GuestRamFixture ram;
            RecordingHandler handler;
            ee::t_ee.callHandler = &handler;
            R5900Context ctx = callContext(0x00100030u);
            ee::setGpr64(&ctx, 4, 41u);
            ee::entry<&localToCallee>(ram.map.base(), &ctx, nullptr);
            ee::t_ee.callHandler = nullptr;
            t.Equals(handler.last.target, 0x00123450u, "target");
            t.Equals(handler.last.intCount, 1, "one argument");
            t.Equals(static_cast<uint32_t>(handler.last.a[0]), 0x01F00000u - 0x30u + 0x10u, "the local's address in the original's frame");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 42u, "result");
        });

        tc.Run("a call's integer arguments after the eighth go to the caller's stack, as wide as they are", [](TestCase &t)
        {
            GuestRamFixture ram;
            const ee::FrameRow row{0u, 0x00345000u, 0x60u, 0u, 0u, 0u, 0u};
            ee::Frame frame;
            frame.fnSp = 0x00100060u;
            frame.stackTop = 0x00100060u;
            frame.calleeSp = 0x00100000u;
            frame.layout = &row;
            ee::Frame *const before = ee::t_ee.frame;
            ee::t_ee.frame = &frame;
            std::memset(ee::at<uint8_t>(0x00100000u), 0xEE, 0x60u);
            RecordingHandler handler;
            ee::t_ee.callHandler = &handler;
            // Eleven integer words and two floats: a0-t3, then three on the stack.
            ee::call<void>(0x00123450u, 0x00200000u, 2, 3, 4, 1.5f, 5, 6, 7, 8, 2.5f, int32_t(-9), uint64_t(0x1122334455667788ull),
                           ee::Ptr<uint32_t>(0x00200010u));
            ee::t_ee.callHandler = nullptr;
            t.Equals(handler.last.intCount, 8, "eight in registers, no more");
            t.Equals(handler.last.stackCount, 3, "three on the stack");
            t.Equals(static_cast<uint32_t>(handler.last.a[7]), 8u, "t3");
            t.Equals(handler.last.floatCount, 2, "the floats in their own registers");
            t.Equals(bits(handler.last.f[0]), bits(1.5f), "f12 is not written over by the ninth word");
            t.Equals(bits(handler.last.f[1]), bits(2.5f), "f13");
            t.Equals(*ee::at<uint32_t>(0x00100000u), static_cast<uint32_t>(-9), "the ninth at the callee's sp (sw)");
            t.Equals(*ee::at<uint32_t>(0x00100004u), 0xEEEEEEEEu, "a 32-bit word leaves its slot's upper half");
            t.Equals(*ee::at<uint64_t>(0x00100008u), 0x1122334455667788ull, "a 64-bit word whole (sd)");
            t.Equals(*ee::at<uint32_t>(0x00100010u), 0x00200010u, "an address");
            t.Equals(*ee::at<uint32_t>(0x00100014u), 0xEEEEEEEEu, "and no further");
            t.Equals(*ee::at<uint32_t>(0x00100018u), 0xEEEEEEEEu, "nothing after the last");

            // A variadic function's long call: the words past its registers, a word left out not stored.
            std::memset(ee::at<uint8_t>(0x00100000u), 0xEE, 0x60u);
            ee::storeVariadicStackWords(ee::VarArg(7), ee::Ptr<uint32_t>(0x00200020u), uint64_t(0x4004000000000000ull), ee::VarArg());
            t.Equals(*ee::at<uint32_t>(0x00100000u), 7u, "the first stack word");
            t.Equals(*ee::at<uint32_t>(0x00100004u), 0xEEEEEEEEu, "sw");
            t.Equals(*ee::at<uint32_t>(0x00100008u), 0x00200020u, "an address");
            t.Equals(*ee::at<uint64_t>(0x00100010u), 0x4004000000000000ull, "a double's bits (sd)");
            t.Equals(*ee::at<uint64_t>(0x00100018u), 0xEEEEEEEEEEEEEEEEull, "a word the call does not name");
            t.Equals(ee::VarArg(int32_t(-1)).stackBytes(), 4u, "an int's width");
            t.Equals(ee::VarArg(int64_t(-1)).stackBytes(), 8u, "a 64-bit word's width");
            ee::t_ee.frame = before;
        });

        tc.Run("fibers leave and are entered again", [](TestCase &t)
        {
            std::pair<ee::fiber::Context *, int> state{nullptr, 0};
            state.first = ee::fiber::create(&fiberCounter, &state, 64 * 1024);
            ee::fiber::enter(state.first);
            ee::fiber::enter(state.first);
            ee::fiber::enter(state.first);
            t.Equals(state.second, 3, "three entries");
            ee::fiber::destroy(state.first);
        });

        tc.Run("fibers are reused once their entry returned, with the new entry", [](TestCase &t)
        {
            // A fiber parked in an endless entry (the test above) is never handed out again.
            std::pair<ee::fiber::Context *, int> parked{nullptr, 0};
            parked.first = ee::fiber::create(&fiberCounter, &parked, 128 * 1024);
            ee::fiber::enter(parked.first);
            ee::fiber::destroy(parked.first);

            ee::fiber::Context *first = nullptr;
            bool reused = true;
            for (int n = 0; n < 1000; ++n)
            {
                FiberJob job;
                job.id = n;
                job.calls = n % 4;
                job.fiber = ee::fiber::create(&fiberJob, &job, 128 * 1024);
                if (n == 0)
                    first = job.fiber;
                else if (job.fiber != first)
                    reused = false;
                int entries = 0;
                while (entries <= job.calls) // each call leaves once, then the return
                {
                    ee::fiber::enter(job.fiber);
                    ++entries;
                }
                if (job.ranAs != n || job.made != job.calls || entries != job.calls + 1)
                {
                    t.IsTrue(false, "job " + std::to_string(n) + " ran its own entry, its calls, and returned");
                    break;
                }
                ee::fiber::destroy(job.fiber);
            }
            t.IsTrue(reused, "one fiber for all 1000 (none created per call)");
            t.Equals(parked.second, 1, "the parked fiber was not entered again");
        });

        tc.Run("suspendable calls: straight back, or through the scheduler after a wait", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(ee::installSuspendableCalls(runtime), "resume slot");
            runtime.replaceFunction(0x00002000u, &calleeThatWaits);
            runtime.replaceFunction(0x00002200u, &afterTheWait);
            runtime.replaceFunction(0x00002100u, &doubles);
            runtime.replaceFunction(0x00003000u, &ee::entrySuspendable<&addTwoCallResults>);
            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, 4, 10u);
            ctx.pc = 0x00003000u;
            // The scheduler's loop: run whatever pc names until the caller's return address.
            int hops = 0;
            while (ctx.pc != 0x00100040u && runtime.hasFunction(ctx.pc) && hops < 100)
            {
                runtime.lookupFunction(ctx.pc)(ram.map.base(), &ctx, &runtime);
                ++hops;
            }
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<int32_t>(ee::gpr64(&ctx, ee::reg::v0)), 11 + 22, "results of both calls");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::sp)), 0x01F00000u, "sp restored");
            // The callee that leaves to another block unwinds to the loop
            // (entry, that block, the resume slot); the second call comes
            // straight back inside the fiber's dispatch.
            t.Equals(hops, 3, "entry, the waiting callee's other block, resume");
        });

        tc.Run("a readable callee on its caller's fiber: straight back in place, a wait through the scheduler", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(ee::installSuspendableCalls(runtime), "resume slot");
            runtime.replaceFunction(0x00002000u, &calleeThatWaits);
            runtime.replaceFunction(0x00002200u, &afterTheWait);
            runtime.replaceFunction(0x00003200u, &ee::entry<&readableLeaf>);
            runtime.replaceFunction(0x00003300u, &ee::entry<&readableThatWaits>);
            runtime.replaceFunction(0x00003400u, &ee::entrySuspendable<&readableCaller>);
            ee::noteReadableEntry(&ee::entry<&readableLeaf>);
            ee::noteReadableEntry(&ee::entry<&readableThatWaits>);
            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, 4, 10u);
            ctx.pc = 0x00003400u;
            int hops = 0;
            while (ctx.pc != 0x00100040u && runtime.hasFunction(ctx.pc) && hops < 100)
            {
                runtime.lookupFunction(ctx.pc)(ram.map.base(), &ctx, &runtime);
                ++hops;
            }
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<int32_t>(ee::gpr64(&ctx, ee::reg::v0)), 11 + 24, "results of both calls");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::sp)), 0x01F00000u, "sp restored");
            // The caller's entry (the leaf comes straight back in it), the
            // waiting callee's other block, the resume of the readable callee
            // and, its return through the scheduler, of its caller.
            t.Equals(hops, 4, "entry, the other block, two resumes");
        });

        tc.Run("a readable callee's tail call that waits returns past it, on the fiber they share", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(ee::installSuspendableCalls(runtime), "resume slot");
            runtime.replaceFunction(0x00002300u, &tailCalleeThatThrows);
            runtime.replaceFunction(0x00002200u, &afterTheWait);
            runtime.replaceFunction(0x00003500u, &ee::entry<&readableTailThatWaits>);
            runtime.replaceFunction(0x00003600u, &ee::entrySuspendable<&readableCallerOfTail>);
            ee::noteReadableEntry(&ee::entry<&readableTailThatWaits>);
            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, 4, 10u);
            ctx.pc = 0x00003600u;
            int hops = 0;
            while (ctx.pc != 0x00100040u && runtime.hasFunction(ctx.pc) && hops < 100)
            {
                try
                {
                    runtime.lookupFunction(ctx.pc)(ram.map.base(), &ctx, &runtime);
                }
                catch (const EeDispatcherTransfer &)
                {
                }
                ++hops;
            }
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<int32_t>(ee::gpr64(&ctx, ee::reg::v0)), 11 + 100, "the tail callee's result");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::sp)), 0x01F00000u, "sp restored");
            t.Equals(hops, 3, "entry, the other block, the resume");
        });

        tc.Run("published return addresses: the callee's ra is the original's, a wait returns through it", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(ee::installSuspendableCalls(runtime), "resume slot");
            runtime.replaceFunction(0x00002000u, &waitsNotingRa);
            runtime.replaceFunction(0x00002200u, &afterTheWait);
            runtime.replaceFunction(0x00002400u, &doublesNotingRa);
            runtime.replaceFunction(0x00007010u, &originalRest);
            runtime.replaceFunction(0x00007020u, &originalRest);
            runtime.replaceFunction(0x00007100u, &ee::entrySuspendable<&addTwoCallResultsPublished>);
            g_calleeRa[0] = g_calleeRa[1] = g_raAfterFirst = 0;
            g_originalRestRan = false;
            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, 4, 10u);
            ctx.pc = 0x00007100u;
            int hops = 0;
            while (ctx.pc != 0x00100040u && runtime.hasFunction(ctx.pc) && hops < 100)
            {
                runtime.lookupFunction(ctx.pc)(ram.map.base(), &ctx, &runtime);
                ++hops;
            }
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<int32_t>(ee::gpr64(&ctx, ee::reg::v0)), 11 + 22, "results of both calls");
            t.Equals(g_calleeRa[0], 0x00007010u, "the waiting callee's ra: the first jal's return address");
            t.Equals(g_calleeRa[1], 0x00007020u, "the second callee's ra: its jal's");
            t.Equals(g_raAfterFirst, 0x00007010u, "ra after the first call, as the callee left it");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::ra)), 0x00100040u, "ra at the return: the entry's");
            t.IsTrue(!g_originalRestRan, "the original's rest at the return address did not run");
            // entry, the waiting callee's other block, its return at 0x7010
            t.Equals(hops, 3, "the wait came back through the return address's slot");

            // Without a fiber (callGuest: the context comes back as it was), the same.
            g_calleeRa[0] = g_calleeRa[1] = g_raAfterFirst = 0;
            R5900Context direct = callContext(0x00100050u);
            ee::setGpr64(&direct, 4, 10u);
            direct.pc = 0x00007100u;
            ee::entryDirect<&addTwoCallResultsPublished>(ram.map.base(), &direct, &runtime);
            t.Equals(direct.pc, 0x00100050u, "callGuest: returned");
            t.Equals(static_cast<int32_t>(ee::gpr64(&direct, ee::reg::v0)), 11 + 22, "callGuest: results");
            t.Equals(g_calleeRa[0], 0x00007010u, "callGuest: the first callee's ra");
            t.Equals(g_calleeRa[1], 0x00007020u, "callGuest: the second callee's ra");
            t.Equals(g_raAfterFirst, 0x00007010u, "callGuest: ra after the first call");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&direct, ee::reg::ra)), 0x00100050u, "callGuest: ra at the return");
            t.IsTrue(!g_originalRestRan, "callGuest: the original's rest did not run");
        });

        tc.Run("packet size: chunks of 60, material tags, quadword padding", [](TestCase &t)
        {
            // Skinned: 68 + 28 n per chunk, padded; a material change pads
            // and adds an 8-byte tag.
            zge::PacketSize skinned(zge::packetBytesPerEntry(0x08), 0);
            skinned.group(0, 0, 61); // chunks of 60 and 1
            // tag 8, 8 + 68 + 1680 = 1756 -> 1760; 1760 + 68 + 28 = 1856 -> 1856
            t.Equals(skinned.bytes(), 1856u, "two chunks of one material");
            skinned.group(0, 0, 3); // same material: no tag
            t.Equals(skinned.bytes(), 1856u + 160u, "68 + 84 = 152 -> 160");
            skinned.group(0, 1, 3); // a new material: tag
            t.Equals(skinned.bytes(), 2176u, "a tag at 2016, then 2024 + 152 = 2176");
            t.Equals(skinned.entries(), 67u, "entries");

            // A triangle list takes whole triangles; a start 4 past a
            // quadword pads 12 first.
            zge::PacketSize list(zge::packetBytesPerEntry(0x04), zge::kPacketWorstStartAlignment);
            list.group(1, 2, 62); // 60, then 2 left: no triangle, stops
            t.Equals(list.entries(), 60u, "a list's leftover entries are not sent");
            // 4 -> 16, + 8 = 24, + 68 + 1440 = 1532 -> 1536; minus the start 4
            t.Equals(list.bytes(), 1532u, "rigid list from a start 4 past a quadword");
            t.Equals(zge::packetBytesPerEntry(0x20), 24u, "terrain");
            t.Equals(zge::packetBytesPerEntry(0x10), 0u, "no builder");
            zge::PacketSize none(24u, 8u);
            t.Equals(none.bytes(), 8u, "no groups: padded to a quadword");
        });

        tc.Run("strips made again draw the same triangles in their winding", [](TestCase &t)
        {
            namespace st = zge::native::strips;
            // A grid with holes, some triangles twice (one each way, and
            // the same way twice): more neighbours than three.
            std::vector<st::Triangle> triangles;
            uint32_t seed = 12345u;
            auto next = [&] { return seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7FFFu; };
            const uint16_t w = 12;
            for (uint16_t y = 0; y < w; ++y)
                for (uint16_t x = 0; x < w; ++x)
                {
                    const uint16_t a = static_cast<uint16_t>(y * (w + 1) + x), b = static_cast<uint16_t>(a + 1),
                                   c = static_cast<uint16_t>(a + w + 1), d = static_cast<uint16_t>(c + 1);
                    if (next() % 5 == 0)
                        continue;
                    triangles.push_back({a, b, c});
                    triangles.push_back({b, d, c});
                    if (next() % 7 == 0)
                        triangles.push_back({a, c, b});
                    if (next() % 9 == 0)
                        triangles.push_back({a, b, c});
                }
            const std::vector<uint16_t> entries = st::makeStrips(triangles);
            std::vector<st::Triangle> drawn;
            size_t groups = 0, longest = 0;
            for (const auto &group : st::cutGroups(entries, 60))
            {
                st::trianglesOf(group, drawn);
                ++groups;
                longest = std::max(longest, group.size());
            }
            auto canonicalSorted = [](std::vector<st::Triangle> v)
            {
                for (auto &x : v)
                    x = st::canonical(x);
                std::sort(v.begin(), v.end());
                return v;
            };
            t.IsTrue(canonicalSorted(drawn) == canonicalSorted(triangles), "the same triangles, the same winding");
            t.IsTrue(longest <= 60u, "groups of at most 60");
            t.IsTrue(entries.size() < triangles.size() * 2u, "fewer than two entries a triangle on a grid");
            t.IsTrue(groups > 1u, "cut into groups");
        });

        tc.Run("ee::Ptr's index is guest address arithmetic: it wraps in 32 bits", [](TestCase &t)
        {
            GuestRamFixture ram;
            struct Key
            {
                uint32_t time, value[3];
            };
            *ee::at<uint32_t>(0x00002000u) = 0x11111111u;
            *ee::at<uint32_t>(0x00002010u) = 0x22222222u;
            *ee::at<uint32_t>(0x00001FF0u) = 0x33333333u;
            const ee::Ptr<Key> keys(0x00002000u);
            t.Equals(keys[0].time, 0x11111111u, "index 0");
            t.Equals(keys[1].time, 0x22222222u, "index 1");
            const uint32_t minusOne = 0xFFFFFFFFu;
            t.Equals(keys[minusOne].time, 0x33333333u, "a uint32 index of -1: the element before");
            t.Equals(keys[static_cast<size_t>(-1)].time, 0x33333333u, "a size_t -1");
            int negative = -1;
            t.Equals(keys[negative].time, 0x33333333u, "an int -1");
            // Below address 0 the guest address wraps and RAM's mask takes it back in.
            *ee::at<uint32_t>(0xFFFFFFF0u) = 0x44444444u;
            const ee::Ptr<Key> low(0u);
            t.Equals(low[minusOne].time, 0x44444444u, "wraps below address 0, inside RAM");
            t.Equals(&low[minusOne].time, ee::at<uint32_t>(0xFFFFFFF0u), "the same place as at()'s");
            // Landing on address 0 or on an I/O address: RAM, not a null pointer.
            *reinterpret_cast<uint32_t *>(ram.map.base()) = 0x55555555u;
            t.Equals(ee::Ptr<Key>(0x00000010u)[minusOne].time, 0x55555555u, "address 0 itself");
            t.Equals(ee::Ptr<uint32_t>(0x10000000u)[0u], *reinterpret_cast<uint32_t *>(ram.map.base()), "an I/O address: masked into RAM");
            // A wrap that a host pointer would have taken 64 GB away.
            const uint32_t far = 0xF0000000u;
            t.IsTrue(reinterpret_cast<uint8_t *>(&keys[far]) >= ram.map.base() &&
                         reinterpret_cast<uint8_t *>(&keys[far]) < ram.map.base() + PS2_RAM_SIZE,
                     "any index stays in guest RAM");
        });

        tc.Run("a null Ptr reads low RAM, and still says it is null", [](TestCase &t)
        {
            GuestRamFixture ram;
            struct Record
            {
                uint32_t first;
                uint32_t second;
            };
            *reinterpret_cast<uint32_t *>(ram.map.base() + 4) = 0x66666666u;
            const ee::Ptr<Record> null(0u);
            // As the EE reads through a null pointer (cov-ragdoll's solveDynamicsIsland).
            t.Equals(null->second, 0x66666666u, "-> reads low RAM");
            t.Equals((*null).second, 0x66666666u, "* reads low RAM");
            null->first = 0x77777777u;
            t.Equals(*reinterpret_cast<uint32_t *>(ram.map.base()), 0x77777777u, "-> writes low RAM");
            t.IsTrue(ee::at<uint32_t>(0u) == reinterpret_cast<uint32_t *>(ram.map.base()), "at(0) is RAM's start");
            // C++ that tests a pointer still sees null.
            t.IsTrue(!null, "!ptr");
            t.IsTrue(null.get() == nullptr, "get()");
            t.IsTrue(static_cast<Record *>(null) == nullptr, "the conversion to T *");
            t.IsTrue(ee::pointerAt<Record>(0u) == nullptr, "pointerAt(0)");
            t.IsTrue(ee::Abi<Record *>::fromReg(0u) == nullptr, "a host pointer parameter");
            t.Equals(ee::addressOf(ee::at<Record>(0u)), 0u, "RAM's start is address 0");
            const ee::Ptr<Record> real(0x00000010u);
            t.IsTrue(!!real && real.get() == ee::at<Record>(0x00000010u), "other addresses as before");
        });

        tc.Run("kernel calls, COP0 and MMI forms of the drafts", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(ee::installSuspendableCalls(runtime), "slots");
            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, ee::reg::v0, 0x1234u);
            ctx.pc = 0x00003200u;
            ee::entryDirect<&flushCacheWrapper>(ram.map.base(), &ctx, &runtime);
            t.Equals(ctx.pc, 0x00100040u, "kernelCall: returned");
            t.Equals(static_cast<int32_t>(ee::gpr64(&ctx, ee::reg::v0)), 0, "kernelCall: FlushCache's result");

            R5900Context fiber = callContext(0x00100050u);
            ee::setGpr64(&fiber, ee::reg::v0, 0x1234u);
            fiber.pc = 0x00003200u;
            ee::entrySuspendable<&flushCacheWrapper>(ram.map.base(), &fiber, &runtime);
            t.Equals(fiber.pc, 0x00100050u, "kernelCall on a fiber: returned");
            t.Equals(static_cast<int32_t>(ee::gpr64(&fiber, ee::reg::v0)), 0, "kernelCall on a fiber: the result");

            R5900Context cop0 = callContext(0x00100060u);
            ee::setGpr64(&cop0, 4, 0xFFFFFFFFu);
            cop0.pc = 0x00003200u;
            ee::entryDirect<&cop0Round>(ram.map.base(), &cop0, &runtime);
            t.Equals(static_cast<uint32_t>(ee::gpr64(&cop0, ee::reg::v0)), 0xFF57FFFFu, "MTC0 Status: the recompiler's mask");

            const ee::Qword a{0x0001000200030004ull, 0x7FFF000000000000ull}, b{0x0001000100010001ull, 0x0001000000000000ull};
            const ee::Qword sum = ee::mmi::paddh(a, b);
            t.Equals(sum.low, 0x0002000300040005ull, "paddh, low");
            t.Equals(sum.high, 0x8000000000000000ull, "paddh, high (wraps)");
            const ee::Qword low = ee::mmi::pcpyld(a, b);
            t.Equals(low.low, b.low, "pcpyld: rt's low first");
            t.Equals(low.high, a.low, "pcpyld: rs's low second");
            const ee::Qword copies = ee::mmi::pcpyh(a);
            t.Equals(copies.low, 0x0004000400040004ull, "pcpyh, low");
            t.Equals(copies.high, 0ull, "pcpyh, high");
            t.Equals(ee::mmi::hiLo(1, -1), 0x00000001FFFFFFFFll, "hi:lo");
        });

        tc.Run("game::call goes through the table once an override hooks the callee", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture ram;
            PS2Runtime runtime;
            r::Function *f = r::find(kHookTarget);
            t.IsTrue(f && f->enabled, "registered, on");
            if (!f)
                return;
            runtime.replaceFunction(kHookTarget, f->readable);
            f->installedSlot = f->readable;
            r::noteOverrides(runtime);
            t.IsFalse(f->hooked, "the slot as install left it: not hooked");
            auto callIt = [&](uint32_t value) {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, 4, value);
                ctx.pc = 0x00003100u;
                ee::entryDirect<&hookTargetCaller>(ram.map.base(), &ctx, &runtime);
            };
            g_hookTargetRan = g_hookRan = 0;
            callIt(5u);
            t.Equals(static_cast<uint32_t>(g_hookTargetRan), 6u, "direct: the callee ran");
            t.Equals(static_cast<uint32_t>(g_hookRan), 0u, "direct: no hook there");

            g_hookNext = runtime.lookupFunction(kHookTarget);
            runtime.replaceFunction(kHookTarget, &hookAround);
            callIt(7u);
            t.Equals(static_cast<uint32_t>(g_hookRan), 0u, "not noted yet: still direct");
            r::noteOverrides(runtime);
            t.IsTrue(f->hooked, "the override's wrapper noted");
            callIt(9u);
            t.Equals(static_cast<uint32_t>(g_hookRan), 1u, "the hook ran");
            t.Equals(static_cast<uint32_t>(g_hookTargetRan), 10u, "and the callee through it");
            f->hooked = false;
            f->installedSlot = nullptr;
        });

        tc.Run("an own body's call of its shared name is, under the replay, a call of itself: run inline", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture ram;
            PS2Runtime runtime;
            r::Function *f = r::find(kOwnSelf);
            t.IsTrue(f != nullptr, "registered");
            if (!f)
                return;
            auto run = [&](uint32_t depth) {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, 4, depth);
                ctx.pc = kOwnSelf;
                ee::entry<&ownSelf>(ram.map.base(), &ctx, &runtime);
                return static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0));
            };
            SelfCallHandler handler;
            handler.self = kOwnSelf;
            ee::t_ee.callHandler = &handler;
            // (an instance's registration has its body's type: readable.h bind)
            f->signature = r::signatureOf<decltype(&ownSelf)>();
            g_sharedSelfRan = 0;
            t.Equals(run(3u), 3u, "the own body ran at every depth");
            t.Equals(handler.calls, 0, "no call made: the recording has none for it");
            t.Equals(static_cast<uint32_t>(g_sharedSelfRan), 0u, "never the shared body");
            // A registration of another type is not run as this one: the call goes to the handler, as before.
            f->signature = r::signatureOf<void (*)()>();
            t.Equals(run(2u), 1001u, "another type: answered by the handler");
            t.Equals(handler.calls, 1, "one call made");
            ee::t_ee.callHandler = nullptr;
            f->signature = nullptr;
        });

        tc.Run("GAME_CALL: a plain name keeps hooks, a function switched off and the replay's answers", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture ram;
            PS2Runtime runtime;
            r::Function *f = r::find(kRouted);
            t.IsTrue(f && f->enabled, "registered, on");
            if (!f)
                return;
            t.IsTrue(r::find("routed") == f, "it goes by the name it is called by");
            t.IsTrue(r::find("routed_") == nullptr, "the marker is not a name");
            t.IsTrue(r::calledDirectly(f->host), "it counts as called directly");
            ee::at<RoutedThing>(kRoutedObject)->value = 100u;
            auto callIt = [&](uint32_t value) {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, 4, value);
                ctx.pc = 0x00003100u;
                ee::entry<&routedCaller>(ram.map.base(), &ctx, &runtime);
                return static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0));
            };

            runtime.replaceFunction(kRouted, f->readable);
            f->installedSlot = f->readable;
            r::noteOverrides(runtime);
            g_routedRan = 0;
            t.Equals(callIt(5u), 5u * 2u + 0x40u + 105u, "direct: the arguments as given, the result");
            t.Equals(static_cast<uint32_t>(g_routedRan), 6u, "direct: the body ran");

            g_hookNext = runtime.lookupFunction(kRouted);
            runtime.replaceFunction(kRouted, &hookAround);
            r::noteOverrides(runtime);
            g_hookRan = 0;
            t.Equals(callIt(7u), 7u * 2u + 0x40u + 107u, "hooked: the same");
            t.Equals(static_cast<uint32_t>(g_hookRan), 1u, "hooked: the hook ran");
            f->hooked = false;

            runtime.replaceFunction(kRouted, &routedStub);
            f->installedSlot = &routedStub;
            f->enabled = false;
            g_routedRan = g_routedStubRan = 0;
            callIt(9u);
            t.Equals(static_cast<uint32_t>(g_routedStubRan), 1u, "off: the recompiled function ran");
            t.Equals(static_cast<uint32_t>(g_routedRan), 0u, "off: the body did not");
            f->enabled = true;
            f->installedSlot = nullptr;

            RecordingHandler handler;
            ee::t_ee.callHandler = &handler;
            g_routedRan = 0;
            callIt(11u);
            ee::t_ee.callHandler = nullptr;
            t.Equals(static_cast<uint32_t>(g_routedRan), 0u, "replay: the handler answers, the body does not run");
            t.Equals(handler.last.target, kRouted, "replay: the callee's address");
            t.IsTrue(handler.last.direct, "replay: as a direct call");
            t.Equals(handler.last.intCount, 4, "replay: four arguments");
            t.Equals(static_cast<uint32_t>(handler.last.a[0]), 11u, "replay: the value");
            t.Equals(static_cast<uint32_t>(handler.last.a[2]), kRoutedObject, "replay: an object goes as its address");
            t.Equals(static_cast<uint32_t>(handler.last.a[3]), 0x40u, "replay: a Ptr to an integer as its address");

            r::Function *w = r::find(kRoutedWaits);
            t.IsTrue(w && w->waits, "a body that waits");
            if (w)
            {
                runtime.replaceFunction(kRoutedWaits, &routedStub);
                g_routedRan = g_routedStubRan = 0;
                R5900Context ctx = callContext(0x00100040u);
                ctx.pc = 0x00003100u;
                ee::entryDirect<&routedWaitsCaller>(ram.map.base(), &ctx, &runtime);
                t.Equals(static_cast<uint32_t>(g_routedStubRan), 1u, "no fiber: through the table, which gives it one");
                t.Equals(static_cast<uint32_t>(g_routedRan), 0u, "no fiber: not straight into the body");
            }
        });

        tc.Run("PS2X_READABLE switch", [](TestCase &t)
        {
            namespace r = game::readable;
            r::Function *verified = r::find(0x00000100u);
            r::Function *tested = r::find("sumPair");
            r::Function *draft = r::find(0x00000300u);
            r::Function *planned = r::find("plannedOnly");
            t.IsTrue(verified && tested && draft && planned, "registered");
            if (!verified || !tested || !draft || !planned)
                return;
            t.IsTrue(verified->enabled && !tested->enabled && !draft->enabled, "default: verified only");
            t.Equals(static_cast<int>(planned->status), static_cast<int>(r::Status::Planned), "planned");
            t.IsTrue(r::applySwitch("off,+sumPair") && !verified->enabled && tested->enabled, "off, then one on");
            t.IsTrue(r::applySwitch("tested") && verified->enabled && tested->enabled && !draft->enabled, "tested");
            t.IsTrue(r::applySwitch("all,-0x00000100") && !verified->enabled && draft->enabled, "all but one");
            t.IsFalse(planned->enabled, "planned never runs");
            std::string error;
            t.IsFalse(r::applySwitch("plannedOnly,noSuchFunction", &error), "unknown or not ported");
            t.IsTrue(error.find("noSuchFunction") != std::string::npos, "names the unknown one");
            r::Function *waits = r::find("readsAFile");
            t.IsTrue(waits && waits->waits, "registered as waiting");
            if (waits)
            {
                r::applySwitch("off,readsAFile");
                t.IsTrue(waits->enabled, "switchable by name");
            }
            r::applySwitch("on");
            t.IsTrue(verified->enabled && !tested->enabled, "back to the default");
            t.Equals(static_cast<int>(tested->returnKind), static_cast<int>(ee::ReturnKind::Float), "float result");
            t.Equals(static_cast<int>(verified->returnKind), static_cast<int>(ee::ReturnKind::Void), "void");
        });

        tc.Run("fncheck recording round trip", [](TestCase &t)
        {
            fncheck::Record record;
            record.sequence = 41;
            record.vsync = 1234;
            record.deadLow = 0x01EF0000u;
            ee::setGpr64(&record.entry, 4, 0xABCDu);
            ee::setGpr64(&record.exit, 2, 0x55u);
            fncheck::Line line;
            line.address = 0x00200010u;
            line.mask = 0x00F0u;
            line.bytes[4] = 0x7Au;
            record.inputs.push_back(line);
            record.outputs.push_back(line);
            fncheck::Call call;
            call.target = 0x00117C50u;
            call.a[0] = 0x40u;
            call.f[1] = 2.0f;
            ee::setGpr64(&call.after, 2, 0x00300000u);
            call.effects.push_back(line);
            call.aHigh[1] = 0x1122334455667788ull;
            fncheck::Line frameRead;
            frameRead.address = 0x01EFFF20u;
            frameRead.mask = 0x000Fu;
            frameRead.bytes[0] = 0x98u;
            call.reads.push_back(frameRead);
            record.calls.push_back(call);

            std::stringstream file;
            fncheck::writeHeader(file, 0x00148408u);
            fncheck::writeRecord(file, record);
            fncheck::writeRecord(file, record);
            fncheck::FileHeader header;
            std::string error;
            t.IsTrue(fncheck::readHeader(file, header, &error), "header");
            t.Equals(header.address, 0x00148408u, "address");
            fncheck::Record back;
            int n = 0;
            while (fncheck::readRecord(file, back))
                ++n;
            t.Equals(n, 2, "two records");
            t.Equals(back.sequence, uint64_t(41), "sequence");
            t.Equals(back.deadLow, 0x01EF0000u, "dead stack");
            t.Equals(ee::gpr64(&back.entry, 4), uint64_t(0xABCDu), "entry a0");
            t.Equals(back.inputs.size(), size_t(1), "inputs");
            t.Equals(int(back.inputs[0].bytes[4]), 0x7A, "input byte");
            t.Equals(back.calls.size(), size_t(1), "calls");
            t.Equals(back.calls[0].target, 0x00117C50u, "call target");
            t.Equals(ee::gpr64(&back.calls[0].after, 2), uint64_t(0x00300000u), "call result");
            t.Equals(back.calls[0].effects.size(), size_t(1), "call effects");
            t.Equals(back.calls[0].aHigh[1], 0x1122334455667788ull, "a quadword argument's upper half (version 5)");
            t.IsTrue(back.calls[0].highKnown, "upper halves recorded");
            t.IsTrue(back.calls[0].reads.size() == 1u && back.calls[0].reads[0].address == 0x01EFFF20u &&
                         back.calls[0].reads[0].mask == 0x000Fu && back.calls[0].reads[0].bytes[0] == 0x98u,
                     "what the callee read of the caller's frame (version 7)");
            {
                // The head reader skips them as the full reader reads them.
                file.clear();
                file.seekg(sizeof(fncheck::FileHeader));
                fncheck::RecordHead head;
                int skimmed = 0;
                while (fncheck::skimRecord(file, head))
                    ++skimmed;
                t.Equals(skimmed, 2, "two records skimmed");
            }
            {
                ee::CallRegs regs;
                ee::detail::pushArg(regs, 7u);
                ee::detail::pushArg(regs, ee::Qword{0x0102030405060708ull, 0x1112131415161718ull});
                R5900Context ctx = callContext(0u);
                regs.into(&ctx);
                t.Equals(ee::gpr64(&ctx, 5), 0x0102030405060708ull, "a1 low");
                t.Equals(ee::gprHigh64(&ctx, 5), 0x1112131415161718ull, "a1 high: the whole register");
                t.Equals(ee::gprHigh64(&ctx, 4), 0x0ull, "a0 keeps its upper half (an integer argument)");
            }
            std::stringstream junk("not a recording");
            t.IsFalse(fncheck::readHeader(junk, header, &error), "rejects other files");
        });

        tc.Run("fncheck recordings plain or compressed: the same records either way, a seek, a cut or damaged file said", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_packed_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fs::create_directories(folder);
            // Records of several sizes, one of them many times the reader's buffer.
            std::vector<fncheck::Record> written;
            for (uint32_t i = 0; i < 40u; ++i)
            {
                fncheck::Record r;
                r.sequence = 100u + i;
                r.vsync = 10u * i;
                ee::setGpr64(&r.entry, 4, i);
                for (uint32_t k = 0; k < (i == 7u ? 30000u : 3u + i); ++k)
                {
                    fncheck::Line line;
                    line.address = 0x00200000u + 16u * k;
                    line.mask = 0xFFFFu;
                    for (int b = 0; b < 16; ++b)
                        line.bytes[b] = static_cast<uint8_t>(k * 7u + static_cast<uint32_t>(b) + i);
                    r.inputs.push_back(line);
                }
                written.push_back(std::move(r));
            }
            fncheck::FileHeader header;
            header.address = 0x00148408u;
            std::stringstream expected;
            fncheck::writeHeader(expected, header.address);
            for (const fncheck::Record &r : written)
                fncheck::writeRecord(expected, r);
            const std::string plainBytes = expected.str();
            auto bytesOf = [](const fs::path &path) {
                std::ifstream in(path, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            auto put = [](const fs::path &path, const std::string &bytes) {
                std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            };
            // Each form written as the recorder writes it: half the records, the file closed and appended to.
            const fs::path plain = folder / "plain.fnrec", packed = folder / "packed.fnrec";
            for (const bool compressed : {false, true})
            {
                const fs::path &path = compressed ? packed : plain;
                fncheck::RecordingWriter writer;
                t.IsTrue(writer.create(path, header, compressed), "made");
                for (size_t i = 0; i < 20u; ++i)
                    writer.write(written[i]);
                writer.close();
                t.IsTrue(writer.append(path) && writer.compressed() == compressed, "appended to in its own form");
                for (size_t i = 20u; i < written.size(); ++i)
                    writer.write(written[i]);
            }
            t.IsTrue(bytesOf(plain) == plainBytes, "the plain form is the format as before");
            const std::string packedBytes = bytesOf(packed);
            t.IsTrue(fncheck::isPacked(packedBytes.data(), packedBytes.size()) && !fncheck::isPacked(plainBytes.data(), plainBytes.size()),
                     "the compressed form is told by its first bytes");
            t.IsTrue(packedBytes.size() * 4u < plainBytes.size(), "compressed");
            auto readAll = [](const fs::path &path, std::vector<uint64_t> *sequences, std::vector<std::streamoff> *offsets) {
                fncheck::RecordingReader reader;
                reader.open(path);
                std::istream &in = reader.stream();
                fncheck::FileHeader h;
                if (!fncheck::readHeader(in, h, nullptr))
                    return std::string("no header");
                fncheck::Record r;
                for (;;)
                {
                    const std::streamoff at = in.tellg();
                    if (!fncheck::readRecord(in, r, h.version))
                        break;
                    sequences->push_back(r.sequence);
                    if (offsets)
                        offsets->push_back(at);
                }
                return reader.problem();
            };
            std::vector<uint64_t> fromPlain, fromPacked;
            std::vector<std::streamoff> plainOffsets, packedOffsets;
            t.IsTrue(readAll(plain, &fromPlain, &plainOffsets).empty() && readAll(packed, &fromPacked, &packedOffsets).empty(),
                     "both read to their end with nothing wrong");
            t.Equals(fromPacked.size(), size_t(40), "every record of the compressed form");
            t.IsTrue(fromPlain == fromPacked && plainOffsets == packedOffsets, "the same records at the same (plain) offsets");
            {
                fncheck::RecordingReader reader;
                reader.open(packed);
                std::istream &in = reader.stream();
                const std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                t.IsTrue(back == plainBytes, "the compressed form's bytes are the plain file's");
                // Seeks: back to an early record, forward past several frames, with the frames this reader passed.
                fncheck::Record r;
                in.clear();
                t.IsTrue(in.seekg(packedOffsets[5]) && fncheck::readRecord(in, r) && r.sequence == 105u, "a seek back");
                t.IsTrue(in.seekg(packedOffsets[30]) && fncheck::readRecord(in, r) && r.sequence == 130u, "a seek on");
                fncheck::RecordingReader again;
                again.open(packed, reader.frames());
                t.IsTrue(reader.frames().size() > 40u, "a frame a record (and the head's)");
                t.IsTrue(again.stream().seekg(packedOffsets[8]) && fncheck::readRecord(again.stream(), r) && r.sequence == 108u,
                         "a seek by the frames another reader found");
                fncheck::RecordingReader fresh;
                fresh.open(packed);
                t.IsTrue(fresh.stream().seekg(packedOffsets[39]) && fncheck::readRecord(fresh.stream(), r) && r.sequence == 139u,
                         "a seek ahead of what was read");
            }
            {
                // A file made by the zstd tool (one frame, no head) reads as well.
                std::string frame(ZSTD_compressBound(plainBytes.size()), '\0');
                frame.resize(ZSTD_compress(frame.data(), frame.size(), plainBytes.data(), plainBytes.size(), 1));
                put(folder / "tool.fnrec", frame);
                std::vector<uint64_t> sequences;
                t.IsTrue(readAll(folder / "tool.fnrec", &sequences, nullptr).empty() && sequences == fromPlain,
                         "a plain zstd stream");
            }
            {
                // Cut short: the whole records before the cut, and a problem that says so.
                put(folder / "cut.fnrec", packedBytes.substr(0, packedBytes.size() - 10u));
                std::vector<uint64_t> sequences;
                const std::string problem = readAll(folder / "cut.fnrec", &sequences, nullptr);
                t.IsTrue(problem.rfind("cut short", 0) == 0, "a cut file says so: " + problem);
                t.IsTrue(sequences.size() == 39u && sequences.back() == 138u, "the records before the cut");
            }
            {
                // A byte changed inside a frame: its checksum (or its structure) says so.
                std::string damaged = packedBytes;
                damaged[damaged.size() / 2u] = static_cast<char>(damaged[damaged.size() / 2u] ^ 0x5A);
                put(folder / "damaged.fnrec", damaged);
                std::vector<uint64_t> sequences;
                const std::string problem = readAll(folder / "damaged.fnrec", &sequences, nullptr);
                t.IsTrue(!problem.empty(), "a damaged file says so: " + problem);
                t.IsTrue(sequences.size() < 40u, "not read past the damage");
            }
            {
                // A head that names more plain bytes than the stream holds (a converted file cut at a frame's end).
                std::string longer = packedBytes;
                fncheck::PackedHead head;
                std::memcpy(&head, longer.data() + 8, sizeof(head));
                head.plainBytes = plainBytes.size() + 24u;
                std::memcpy(longer.data() + 8, &head, sizeof(head));
                put(folder / "short.fnrec", longer);
                std::vector<uint64_t> sequences;
                const std::string problem = readAll(folder / "short.fnrec", &sequences, nullptr);
                t.IsTrue(problem.find("its head names") != std::string::npos && sequences.size() == 40u,
                         "fewer bytes than its head names: " + problem);
            }
            fncheck::RecordingReader none;
            t.IsFalse(none.open(folder / "missing.fnrec") || static_cast<bool>(none.stream()), "a missing file fails to open");
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck records a self-call's both calls (a tail call each)", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kSelfCalling);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            fn->traced = &selfCallingTraced;
            const uint32_t jal = 0x0C000000u | (kSelfCalling >> 2);
            std::memcpy(ram + kSelfCallReturn - 8u, &jal, 4);
            runtime.replaceFunction(kSelfCalling, &selfCallingTraced);
            runtime.replaceFunction(kCounter, &counterCallee);
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_selfcall_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            setEnvironment("PS2X_FNREC", "selfCalling");
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            installTestRecorder(runtime);
            setEnvironment("PS2X_FNREC", "");

            // 250: two self-calls, each call ending by a tail call (50, 150,
            // 250); 300: the innermost (100) returns by jr ra, then two tail
            // calls (200, 300).
            for (uint32_t value : {250u, 300u})
            {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, ee::reg::a0, value);
                ctx.pc = kSelfCalling;
                runtime.lookupFunction(kSelfCalling)(ram, &ctx, &runtime);
                t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            }
            const uint32_t *list = reinterpret_cast<const uint32_t *>(ram + 0x5100u);
            t.Equals(*reinterpret_cast<const uint32_t *>(ram + 0x50FCu), 5u, "the counter ran five times");
            t.IsTrue(list[0] == 50u && list[1] == 150u && list[2] == 250u && list[3] == 200u && list[4] == 300u,
                     "in the game's order");

            fncheck::RecordingReader recording;
            recording.open(folder / "00004000.fnrec");
            std::istream &file = recording.stream();
            fncheck::FileHeader header;
            std::string error;
            t.IsTrue(fncheck::readHeader(file, header, &error), "recorded");
            std::vector<fncheck::Record> records;
            fncheck::Record record;
            while (fncheck::readRecord(file, record, header.version))
                records.push_back(record);
            t.Equals(records.size(), size_t(2), "both calls kept");
            const std::vector<std::vector<uint32_t>> want = {{50u, 150u, 250u}, {200u, 300u}};
            for (size_t i = 0; i < records.size() && i < want.size(); ++i)
            {
                const fncheck::Record &r = records[i];
                t.Equals(r.calls.size(), want[i].size(), "one call per tail call, the inner calls' too");
                for (size_t c = 0; c < r.calls.size() && c < want[i].size(); ++c)
                {
                    t.Equals(r.calls[c].target, kCounter, "the counter");
                    t.Equals(static_cast<uint32_t>(r.calls[c].a[0]), want[i][c], "its argument");
                    t.IsTrue((r.calls[c].flags & fncheck::kTailCall) && (r.calls[c].flags & fncheck::kDirectTailCall),
                             "a direct tail call");
                    // Each call's effects are its own append alone (the count
                    // and a list entry): the outer call's work after the
                    // return point is the function's own, traced.
                    t.Equals(r.calls[c].effects.size(), size_t(2), "its own effects only");
                }
                t.Equals(r.cycles, uint64_t(0u), "direct tail calls charge no dispatch");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&r.exit, ee::reg::sp)), 0x01F00000u, "sp back");
                uint32_t selfCalls = 0;
                bool written = false;
                for (const fncheck::Line &l : r.outputs)
                    if (l.address == 0x5000u && (l.mask & 0x00F0u) == 0x00F0u)
                    {
                        std::memcpy(&selfCalls, l.bytes + 4, 4);
                        written = true;
                    }
                t.IsTrue(written, "the self-call count is the function's own output");
                t.Equals(selfCalls, i == 0 ? 2u : 4u, "counted at both return points");
            }
            recording.close();
            // (The recorder keeps its files open to the end of the process:
            // Windows refuses to remove them yet.)
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck: a nested call's continuation at the same return point is not the callee's return", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kNestedPlaying);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            fn->traced = &nestedPlayingBody<true>;
            for (uint32_t address : {kNestedPlaying, kNestedReturn, kNestedAlias})
                runtime.replaceFunction(address, &nestedPlayingBody<false>);
            runtime.replaceFunction(kNestedCallee, &nestedCallee);
            runtime.replaceFunction(kNestedCalleeReturn, &nestedCallee);
            runtime.replaceFunction(kNestedJumpTarget, &nestedJumpTarget);
            *reinterpret_cast<uint32_t *>(ram + 0x5300u) = 0x11111111u;
            *reinterpret_cast<uint32_t *>(ram + 0x5304u) = 0x22222222u;
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_nested_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            setEnvironment("PS2X_FNREC", "nestedPlaying");
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            installTestRecorder(runtime);
            setEnvironment("PS2X_FNREC", "");

            R5900Context ctx = callContext(0x00100040u);
            ee::setGpr64(&ctx, ee::reg::a0, 0x5300u);
            ctx.pc = kNestedPlaying;
            runtime.lookupFunction(kNestedPlaying)(ram, &ctx, &runtime);
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::sp)), 0x01F00000u, "with its sp");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 0x11111111u, "its own a0's word");
            t.Equals(*reinterpret_cast<const uint32_t *>(ram + 0x5200u), 2u, "the callee ran twice");

            fncheck::RecordingReader recording;
            recording.open(folder / "00004400.fnrec");
            std::istream &file = recording.stream();
            fncheck::FileHeader header;
            std::string error;
            t.IsTrue(fncheck::readHeader(file, header, &error), "recorded");
            fncheck::Record record;
            t.IsTrue(fncheck::readRecord(file, record, header.version), "the call kept");
            t.Equals(record.calls.size(), size_t(1), "one call");
            if (!record.calls.empty())
            {
                const fncheck::Call &c = record.calls[0];
                t.Equals(c.target, kNestedCallee, "the callee");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&c.after, ee::reg::sp)), c.sp, "it returned with the caller's sp");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&c.after, kS0)), 0x5300u, "and its s0");
                t.Equals(c.after.pc, kNestedReturn, "at the return point");
            }
            t.Equals(static_cast<uint32_t>(ee::gpr64(&record.exit, ee::reg::v0)), 0x11111111u, "the result");
            recording.close();
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck keeps a call that never returns to its stop, only for a function recorded to a stop", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kStopping);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            const PS2Runtime::RecompiledFunction registered = fn->traced;
            fn->traced = &stoppingTraced;
            runtime.replaceFunction(kStoppingFirst, &stoppingFirst);
            runtime.replaceFunction(kStoppingEnd, &stoppingEnd);
            std::error_code ignored;
            // stop: PS2X_FNREC_STOP names it (the frame rule's form); else recorded as before.
            auto record = [&](bool stop, const char *name)
            {
                const fs::path folder = uniqueTempFolder(name);
                fs::remove_all(folder, ignored);
                setEnvironment("PS2X_FNREC", "stoppingTest");
                setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
                setEnvironment("PS2X_FNREC_ATOMIC", "1");
                setEnvironment("PS2X_FNREC_STOP", stop ? "stoppingTest" : "");
                installTestRecorder(runtime);
                setEnvironment("PS2X_FNREC", "");
                setEnvironment("PS2X_FNREC_STOP", "");
                std::memset(ram + 0x5800u, 0, 0x20u);
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, ee::reg::a0, 7u);
                ctx.pc = kStopping;
                bool ended = false;
                try
                {
                    runtime.lookupFunction(kStopping)(ram, &ctx, &runtime);
                }
                catch (const EeDispatcherTransfer &)
                {
                    ended = true;
                }
                t.IsTrue(ended, "the thread ended in the second call");
                std::vector<fncheck::Record> records;
                fncheck::RecordingReader recording;
                recording.open(folder / "00004b00.fnrec");
                std::istream &file = recording.stream();
                fncheck::FileHeader header;
                std::string error;
                if (file && fncheck::readHeader(file, header, &error))
                {
                    fncheck::Record r;
                    while (fncheck::readRecord(file, r, header.version))
                        records.push_back(r);
                }
                recording.close();
                fs::remove_all(folder, ignored);
                return records;
            };
            t.Equals(record(false, "ps2x_fnrec_nostop_test").size(), size_t(0), "not recorded to a stop: no call kept, as before");
            const std::vector<fncheck::Record> records = record(true, "ps2x_fnrec_stop_test");
            t.Equals(records.size(), size_t(1), "recorded to a stop: the call kept");
            if (!records.empty())
            {
                const fncheck::Record &r = records[0];
                t.IsTrue((r.flags & fncheck::kStopsAtCall) != 0u, "a record that stops at a call");
                t.Equals(r.calls.size(), size_t(2), "both calls");
                if (r.calls.size() == 2u)
                {
                    t.Equals(r.calls[0].target, kStoppingFirst, "the first callee");
                    t.IsFalse((r.calls[0].flags & fncheck::kStopCall) != 0u, "which came back");
                    t.Equals(static_cast<uint32_t>(ee::gpr64(&r.calls[0].after, ee::reg::v0)), 20u, "with its result");
                    t.Equals(r.calls[0].effects.size(), size_t(1), "and its write");
                    t.Equals(r.calls[1].target, kStoppingEnd, "the stop's callee");
                    t.IsTrue((r.calls[1].flags & fncheck::kStopCall) != 0u, "marked as the stop");
                    t.Equals(static_cast<uint32_t>(r.calls[1].a[0]), 20u, "its argument");
                    t.Equals(r.calls[1].effects.size(), size_t(0), "no effects: it never came back");
                }
                uint32_t at0 = 0, at4 = 0;
                for (const fncheck::Line &l : r.outputs)
                    if (l.address == 0x5800u)
                    {
                        std::memcpy(&at0, l.bytes, 4);
                        std::memcpy(&at4, l.bytes + 4, 4);
                    }
                t.Equals(at0, 8u, "outputs as at the stop call (not the stop's callee's overwrite)");
                t.Equals(at4, 20u, "the function's second write");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&r.exit, ee::reg::a0)), 20u, "exit: the registers at the stop call");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&r.exit, ee::reg::ra)), kStopping + 0x20u, "its return point in ra");
                t.Equals(static_cast<uint32_t>(r.exit.pc), kStoppingEnd, "pc the stop's callee");
            }
            fn->traced = registered;
        });

#if defined(PS2X_FNCHECK_EXE)
        tc.Run("ps2xFnCheck judges a stop record at its stop: the right version passes, the broken ones fail", [](TestCase &t)
        {
            const std::string command = std::string("\"") + PS2X_FNCHECK_EXE + "\" --self-test-stops";
            t.Equals(std::system(command.c_str()), 0, "ps2xFnCheck --self-test-stops");
        });
#endif

        tc.Run("fncheck records what a callee reads of its caller's frame, kept or not", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *caller = game::readable::find(kFrameCaller);
            game::readable::Function *reader = game::readable::find(kFrameReader);
            game::readable::Function *relay = game::readable::find(kFrameRelay);
            t.IsTrue(caller && reader && relay, "registered");
            if (!caller || !reader || !relay)
                return;
            caller->traced = &FrameRecordCode<true>::caller;
            reader->traced = &FrameRecordCode<true>::reader;
            relay->traced = &FrameRecordCode<true>::relay;
            runtime.replaceFunction(kFrameCaller, &FrameRecordCode<false>::caller);
            runtime.replaceFunction(kFrameReader, &FrameRecordCode<false>::reader);
            runtime.replaceFunction(kFrameRelay, &FrameRecordCode<false>::relay);
            *reinterpret_cast<uint32_t *>(ram + 0x5600u) = 5u;
            *reinterpret_cast<uint32_t *>(ram + 0x5604u) = 6u;
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_frame_reads_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fs::create_directories(folder);
            // The reader and the relay keep one call a vsync: the reader's
            // first (a0 0x5600) and the relay's; the reader's later calls are
            // not kept, and are run traced only for the frame they are handed.
            {
                std::ofstream list(folder / "list.txt");
                list << "frameRecordCaller\nframeRecordReader 100 1\nframeRecordRelay 100 1\n";
            }
            const std::string spec = "@" + (folder / "list.txt").string();
            setEnvironment("PS2X_FNREC", spec.c_str());
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            installTestRecorder(runtime);
            setEnvironment("PS2X_FNREC", "");

            R5900Context ctx = callContext(0x00100040u);
            ctx.pc = kFrameCaller;
            runtime.lookupFunction(kFrameCaller)(ram, &ctx, &runtime);
            t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 0xAAAA0001u + 0xBBBB0002u, "the relay's sum");

            auto records = [&](const char *name)
            {
                std::vector<fncheck::Record> out;
                fncheck::RecordingReader recording;
                recording.open(folder / name);
                std::istream &file = recording.stream();
                fncheck::FileHeader header;
                std::string error;
                if (!fncheck::readHeader(file, header, &error))
                    return out;
                t.Equals(header.version, fncheck::kRecordVersion, "this recorder's version");
                fncheck::Record r;
                while (fncheck::readRecord(file, r, header.version))
                    out.push_back(r);
                return out;
            };
            // The frame's bytes sp + 0x10 .. 0x17 (sp 0x01F00000 - 0x40), as the caller left them.
            auto readsAreTheRecord = [&](const fncheck::Call &c, const char *what)
            {
                std::map<uint32_t, uint8_t> bytes;
                for (const fncheck::Line &l : c.reads)
                    for (uint32_t i = 0; i < 16u; ++i)
                        if (l.mask & (1u << i))
                            bytes[l.address + i] = l.bytes[i];
                const uint32_t record = 0x01F00000u - 0x40u + 0x10u;
                t.Equals(bytes.size(), size_t(8), what);
                const uint32_t words[2] = {0xAAAA0001u, 0xBBBB0002u};
                bool same = bytes.size() == 8u;
                for (uint32_t i = 0; i < 8u && same; ++i)
                    same = bytes.count(record + i) && bytes[record + i] == reinterpret_cast<const uint8_t *>(words)[i];
                t.IsTrue(same, "the two words the callee read, as at the call (not the third, which nothing read)");
            };
            const std::vector<fncheck::Record> callerRecords = records("00007000.fnrec");
            t.Equals(callerRecords.size(), size_t(1), "the caller's call kept");
            if (callerRecords.size() == 1u && callerRecords[0].calls.size() == 3u)
            {
                const std::vector<fncheck::Call> &calls = callerRecords[0].calls;
                t.IsTrue(calls[0].reads.empty(), "a0 not in the frame: no frame reads");
                readsAreTheRecord(calls[1], "the reader's call not kept: its reads of the frame are");
                readsAreTheRecord(calls[2], "through the relay: the reader's reads of the frame handed on");
            }
            else
                t.Equals(callerRecords.empty() ? size_t(0) : callerRecords[0].calls.size(), size_t(3), "three calls");
            const std::vector<fncheck::Record> readerRecords = records("00007100.fnrec");
            t.Equals(readerRecords.size(), size_t(1), "the reader's one kept call, its first");
            if (!readerRecords.empty())
                t.Equals(static_cast<uint32_t>(ee::gpr64(&readerRecords[0].entry, ee::reg::a0)), 0x5600u, "a0 0x5600");
            const std::vector<fncheck::Record> relayRecords = records("00007200.fnrec");
            t.Equals(relayRecords.size(), size_t(1), "the relay's call kept");
            if (relayRecords.size() == 1u && relayRecords[0].calls.size() == 1u)
                t.IsTrue(relayRecords[0].calls[0].reads.empty(), "the bytes are the caller's frame's, not the relay's");
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck refuses to record with readable code on, unless a test allows it", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_refuse_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fs::create_directories(folder);
            const char *was = std::getenv("PS2X_READABLE");
            const std::string previous = was ? was : "";
            // Each case: PS2X_READABLE, PS2X_FNREC_ALLOW_READABLE, the function asked for, installed or refused.
            struct Case
            {
                const char *readable, *allow;
                uint32_t address;
                const char *name;
                PS2Runtime::RecompiledFunction plain, traced;
                bool installs;
                const char *what;
            };
            const Case cases[] = {
                {"", "", kTwoBlocksAllowed, "twoBlocksAllowed", &TwoBlocks<kTwoBlocksAllowed>::run<false>,
                 &TwoBlocks<kTwoBlocksAllowed>::run<true>, false, "unset (the defaults): refused"},
                {"tested", "", kTwoBlocksAllowed, "twoBlocksAllowed", &TwoBlocks<kTwoBlocksAllowed>::run<false>,
                 &TwoBlocks<kTwoBlocksAllowed>::run<true>, false, "tested: refused"},
                {"on", "0", kTwoBlocksAllowed, "twoBlocksAllowed", &TwoBlocks<kTwoBlocksAllowed>::run<false>,
                 &TwoBlocks<kTwoBlocksAllowed>::run<true>, false, "on, the allowance not 1: refused"},
                {"", "1", kTwoBlocksAllowed, "twoBlocksAllowed", &TwoBlocks<kTwoBlocksAllowed>::run<false>,
                 &TwoBlocks<kTwoBlocksAllowed>::run<true>, true, "unset, a test's allowance: recorded"},
                {"off", "", kTwoBlocksOff, "twoBlocksOff", &TwoBlocks<kTwoBlocksOff>::run<false>,
                 &TwoBlocks<kTwoBlocksOff>::run<true>, true, "off: recorded"}};
            for (const Case &c : cases)
            {
                game::readable::Function *fn = game::readable::find(c.address);
                t.IsTrue(fn != nullptr, "registered");
                if (!fn)
                    continue;
                const PS2Runtime::RecompiledFunction registered = fn->traced;
                fn->traced = c.traced;
                runtime.replaceFunction(c.address, c.plain);
                char file[32];
                std::snprintf(file, sizeof(file), "%08x.fnrec", c.address);
                fs::remove(folder / file, ignored);
                setEnvironment("PS2X_FNREC", c.name);
                setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
                setEnvironment("PS2X_READABLE", c.readable);
                setEnvironment("PS2X_FNREC_ALLOW_READABLE", c.allow);
                const bool installed = fncheck::installRecorder(runtime);
                setEnvironment("PS2X_FNREC", "");
                setEnvironment("PS2X_FNREC_ALLOW_READABLE", "");
                setEnvironment("PS2X_READABLE", previous.c_str());
                t.Equals(installed, c.installs, c.what);
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, ee::reg::a0, 1u);
                ctx.pc = c.address;
                runtime.lookupFunction(c.address)(ram, &ctx, &runtime);
                t.Equals(fs::exists(folder / file), c.installs, c.installs ? "its call recorded" : "nothing recorded");
                fn->traced = registered;
            }
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck PS2X_FNREC_UNTIL_SCRIPT: no call begun once the pad script's clock is at its value", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            // No pad script runs here: its clock reads -1. A limit of -1 is reached (nothing kept), one of 0 is not.
            struct Case
            {
                const char *until;
                uint32_t address;
                const char *name;
                PS2Runtime::RecompiledFunction plain, traced;
                bool records;
            };
            const Case cases[] = {{"-1", kTwoBlocksUntilPast, "twoBlocksUntilPast", &TwoBlocks<kTwoBlocksUntilPast>::run<false>,
                                   &TwoBlocks<kTwoBlocksUntilPast>::run<true>, false},
                                  {"0", kTwoBlocksUntilAhead, "twoBlocksUntilAhead", &TwoBlocks<kTwoBlocksUntilAhead>::run<false>,
                                   &TwoBlocks<kTwoBlocksUntilAhead>::run<true>, true}};
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_until_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fs::create_directories(folder);
            for (const Case &c : cases)
            {
                game::readable::Function *fn = game::readable::find(c.address);
                t.IsTrue(fn != nullptr, "registered");
                if (!fn)
                    continue;
                const PS2Runtime::RecompiledFunction registered = fn->traced;
                fn->traced = c.traced;
                runtime.replaceFunction(c.address, c.plain);
                setEnvironment("PS2X_FNREC", c.name);
                setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
                setEnvironment("PS2X_FNREC_UNTIL_SCRIPT", c.until);
                installTestRecorder(runtime);
                setEnvironment("PS2X_FNREC", "");
                setEnvironment("PS2X_FNREC_UNTIL_SCRIPT", "");
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, ee::reg::a0, 1u);
                ctx.pc = c.address;
                runtime.lookupFunction(c.address)(ram, &ctx, &runtime);
                char file[32];
                std::snprintf(file, sizeof(file), "%08x.fnrec", c.address);
                t.Equals(fs::exists(folder / file), c.records, c.records ? "a limit ahead: recorded" : "the limit reached: nothing kept");
                fn->traced = registered;
            }
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck NEW_BLOCKS=3 keeps the capped sample and, past it, only the calls that run a new block", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            // Six calls in one vsync, a0 1, 1, 1, 7, 1, 7; the sample's cap is 2. Unset records one copy of the
            // function, mode 3 the other (the recorder keeps a function's state for the process).
            auto record = [&](const char *mode, const char *name, uint32_t address, const char *function,
                              PS2Runtime::RecompiledFunction plain, PS2Runtime::RecompiledFunction traced)
            {
                std::vector<uint32_t> a0s;
                game::readable::Function *fn = game::readable::find(address);
                t.IsTrue(fn != nullptr, "registered");
                if (!fn)
                    return a0s;
                const PS2Runtime::RecompiledFunction registered = fn->traced;
                fn->traced = traced;
                runtime.replaceFunction(address, plain);
                const fs::path folder = uniqueTempFolder(name);
                std::error_code ignored;
                fs::remove_all(folder, ignored);
                fs::create_directories(folder);
                {
                    std::ofstream list(folder / "list.txt");
                    list << function << " 2 8\n";
                }
                const std::string spec = "@" + (folder / "list.txt").string();
                setEnvironment("PS2X_FNREC", spec.c_str());
                setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
                setEnvironment("PS2X_FNREC_ATOMIC", "1");
                setEnvironment("PS2X_FNREC_NEW_BLOCKS", mode);
                installTestRecorder(runtime);
                setEnvironment("PS2X_FNREC", "");
                setEnvironment("PS2X_FNREC_NEW_BLOCKS", "");
                for (uint32_t a0 : {1u, 1u, 1u, 7u, 1u, 7u})
                {
                    R5900Context ctx = callContext(0x00100040u);
                    ee::setGpr64(&ctx, ee::reg::a0, a0);
                    ctx.pc = address;
                    runtime.lookupFunction(address)(ram, &ctx, &runtime);
                    t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), a0 + 1u, "the call's result");
                }
                char file_name[32];
                std::snprintf(file_name, sizeof(file_name), "%08x.fnrec", address);
                fncheck::RecordingReader recording;
                recording.open(folder / file_name);
                std::istream &file = recording.stream();
                fncheck::FileHeader header;
                std::string error;
                if (fncheck::readHeader(file, header, &error))
                {
                    fncheck::Record r;
                    while (fncheck::readRecord(file, r, header.version))
                        a0s.push_back(static_cast<uint32_t>(ee::gpr64(&r.entry, ee::reg::a0)));
                }
                recording.close();
                fs::remove_all(folder, ignored);
                fn->traced = registered;
                return a0s;
            };
            const std::vector<uint32_t> capped =
                record("", "ps2x_fnrec_mode3_capped", kTwoBlocksCapped, "twoBlocksCapped",
                       &TwoBlocks<kTwoBlocksCapped>::run<false>, &TwoBlocks<kTwoBlocksCapped>::run<true>);
            t.Equals(capped.size(), size_t(2), "unset: the cap's two calls");
            const std::vector<uint32_t> mode3 =
                record("3", "ps2x_fnrec_mode3", kTwoBlocksMode3, "twoBlocksMode3", &TwoBlocks<kTwoBlocksMode3>::run<false>,
                       &TwoBlocks<kTwoBlocksMode3>::run<true>);
            t.Equals(mode3.size(), size_t(3), "mode 3: the sample and one call past it");
            if (mode3.size() == 3u && capped.size() == 2u)
            {
                t.IsTrue(mode3[0] == capped[0] && mode3[1] == capped[1], "the sample is the capped run's calls");
                t.Equals(mode3[2], 7u, "past the cap only the first call that runs the second block (a0 7); the dull ones and the second a0 7 dropped");
            }
        });

        tc.Run("fncheck records and replays a function of two entry_ chunks as one call", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kTwoChunks);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            const PS2Runtime::RecompiledFunction registered = fn->traced;
            // (the caller's return point: a function there, as in the game)
            auto caller = [](uint8_t *, R5900Context *, PS2Runtime *) {};
            // The game's table: the function at its chunks' addresses (the
            // traced build runs as the original with no tracer), the callee.
            runtime.replaceFunction(kTwoChunks, fncheck_test::twoChunksWhole());
            runtime.replaceFunction(kSecondChunk, fncheck_test::twoChunksWhole());
            runtime.replaceFunction(kChunkCallee, &chunkCallee);
            runtime.replaceFunction(0x00100040u, caller);
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_chunks_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fn->traced = fncheck_test::twoChunksWhole();
            setEnvironment("PS2X_FNREC", "twoChunks");
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            installTestRecorder(runtime);
            setEnvironment("PS2X_FNREC", "");
            // Two calls with `traced` as the traced original (the recorder
            // reads it at each call): a0 0, the fall through into the second
            // chunk; 5, the branch.
            auto run = [&](PS2Runtime::RecompiledFunction traced)
            {
                fn->traced = traced;
                for (uint32_t value : {0u, 5u})
                {
                    R5900Context ctx = callContext(0x00100040u);
                    ee::setGpr64(&ctx, ee::reg::a0, value);
                    ctx.pc = kTwoChunks;
                    runtime.lookupFunction(kTwoChunks)(ram, &ctx, &runtime);
                    // (the first chunk alone, as before: pc at the second, which the game's scheduler runs)
                    if (ctx.pc == kSecondChunk)
                        runtime.lookupFunction(kSecondChunk)(ram, &ctx, &runtime);
                    t.Equals(ctx.pc, 0x00100040u, "returned to the caller");
                }
            };
            // The first chunk alone (the traced original before): it returns
            // with pc at the second chunk, not at ra, and no call is kept.
            run(fncheck_test::twoChunksFirstChunk());
            run(fncheck_test::twoChunksWhole());
            std::vector<fncheck::Record> records;
            {
                fncheck::RecordingReader recording;
                recording.open(folder / "00004e00.fnrec");
                std::istream &file = recording.stream();
                fncheck::FileHeader header;
                std::string error;
                t.IsTrue(fncheck::readHeader(file, header, &error), "recorded");
                fncheck::Record r;
                while (fncheck::readRecord(file, r, header.version))
                    records.push_back(r);
            }
            // (The recorder keeps its files open to the end of the process:
            // Windows refuses to remove them yet.)
            fs::remove_all(folder, ignored);
            t.Equals(records.size(), size_t(2), "the whole function's two calls kept, none of the first chunk's alone");
            for (size_t i = 0; i < records.size() && i < 2u; ++i)
            {
                const fncheck::Record &r = records[i];
                const uint32_t a0 = i == 0 ? 0u : 5u;
                t.Equals(static_cast<uint32_t>(r.exit.pc), 0x00100040u, "ends at its return address");
                t.Equals(static_cast<uint32_t>(ee::gpr64(&r.exit, ee::reg::sp)), 0x01F00000u, "sp back");
                t.Equals(r.calls.size(), size_t(1), "one call: the callee's (the hop to the second chunk is not one)");
                if (!r.calls.empty())
                {
                    t.Equals(r.calls[0].target, kChunkCallee, "the callee");
                    t.Equals(static_cast<uint32_t>(r.calls[0].a[0]), a0, "its argument");
                    t.Equals(r.calls[0].effects.size(), size_t(1), "its own write only");
                }
                // The second chunk's write is the function's own output.
                bool second = false, fellThrough = false;
                for (const fncheck::Line &l : r.outputs)
                {
                    if (l.address != 0x5600u)
                        continue;
                    uint32_t at0 = 0, at4 = 0;
                    std::memcpy(&at0, l.bytes, 4);
                    std::memcpy(&at4, l.bytes + 4, 4);
                    second = (l.mask & 0x00F0u) == 0x00F0u && at4 == a0 * 2u + 1u;
                    fellThrough = (l.mask & 0x000Fu) == 0x000Fu && at0 == 1u;
                }
                t.IsTrue(second, "the second chunk's write (v0 + 1 at 0x5604) is the function's output");
                t.Equals(fellThrough, i == 0, "the first chunk's write on the fall through only");
                // Charged: the call's dispatch, the scheduler's dispatch of the
                // second chunk (EeScheduler's slice dispatch) and the return's.
                t.Equals(r.cycles, uint64_t(3u * EeScheduler::kGuestDispatchCycles), "three dispatches");

                // Replayed as ps2xFnCheck does: the inputs on a filled RAM,
                // the callee answered from the recording, the traced original
                // run once from the entry, which must return to ra with the
                // recorded outputs.
                GuestRamFixture again;
                uint8_t *replayRam = again.map.base();
                std::memset(replayRam, 0xA5, 0x10000u);
                for (const fncheck::Line &l : r.inputs)
                    for (uint32_t b = 0; b < 16u; ++b)
                        if (l.mask & (1u << b))
                            replayRam[l.address + b] = l.bytes[b];
                static const fncheck::Call *s_call = nullptr;
                s_call = r.calls.empty() ? nullptr : &r.calls[0];
                PS2Runtime replayRuntime;
                replayRuntime.replaceFunction(0x00100040u, caller);
                replayRuntime.replaceFunction(kChunkCallee, [](uint8_t *mem, R5900Context *ctx, PS2Runtime *)
                {
                    const uint32_t ra = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra));
                    for (const fncheck::Line &l : s_call->effects)
                        for (uint32_t b = 0; b < 16u; ++b)
                            if (l.mask & (1u << b))
                                mem[l.address + b] = l.bytes[b];
                    ee::setGpr64(ctx, ee::reg::v0, ee::gpr64(&s_call->after, ee::reg::v0));
                    ctx->pc = ra;
                });
                R5900Context ctx = r.entry;
                ctx.pc = kTwoChunks;
                fn->traced(replayRam, &ctx, &replayRuntime);
                t.Equals(static_cast<uint32_t>(ctx.pc), 0x00100040u, "the replay returns in one run of the traced original");
                bool same = true;
                for (const fncheck::Line &l : r.outputs)
                    for (uint32_t b = 0; b < 16u; ++b)
                        if ((l.mask & (1u << b)) && replayRam[l.address + b] != l.bytes[b])
                            same = false;
                t.IsTrue(same, "with the recorded outputs");

                // A resume at the first chunk's return point (a parked wait,
                // a --safepoints interruption): that chunk from there, then
                // the second.
                ctx = r.entry;
                ee::setGpr64(&ctx, ee::reg::sp, 0x01F00000u - 0x10u);
                ee::setGpr64(&ctx, ee::reg::v0, 7u);
                const uint32_t back = 0x00100040u;
                std::memcpy(replayRam + 0x01F00000u - 0x10u, &back, 4);
                ctx.pc = 0x00004E14u;
                fn->traced(replayRam, &ctx, &replayRuntime);
                uint32_t written = 0;
                std::memcpy(&written, replayRam + 0x5604u, 4);
                t.IsTrue(ctx.pc == 0x00100040u && written == 8u, "a resume goes on in its chunk, then the next");

                // The hop is the scheduler's dispatch of the second chunk, a safe point: interrupted there
                // (ps2xFnCheck --safepoints), the traced original unwinds with pc at the second chunk, and a
                // resume there finishes the call. Not interrupted, the same two safe points (the call's dispatch,
                // the hop) and the same cycles.
                struct HopTracer : fncheck::Tracer
                {
                    uint32_t points = 0, interruptAt = 0;
                    uint64_t charged = 0;
                    void read(uint32_t, uint32_t) override {}
                    void write(uint32_t, uint32_t) override {}
                    void special(uint32_t) override {}
                    void beforeCall(uint8_t *, R5900Context *, uint32_t) override {}
                    void afterCall(uint8_t *, R5900Context *, bool) override {}
                    void cycles(uint32_t count) override { charged += count; }
                    bool safePointDue(uint32_t) override { return ++points == interruptAt; }
                };
                for (uint32_t at : {0u, 2u})
                {
                    HopTracer tracer;
                    tracer.interruptAt = at;
                    for (const fncheck::Line &l : r.inputs)
                        for (uint32_t b = 0; b < 16u; ++b)
                            if (l.mask & (1u << b))
                                replayRam[l.address + b] = l.bytes[b];
                    std::memset(replayRam + 0x5604u, 0, 4);
                    ctx = r.entry;
                    ctx.pc = kTwoChunks;
                    fncheck::t_tracer = &tracer;
                    fn->traced(replayRam, &ctx, &replayRuntime);
                    if (at)
                    {
                        std::memcpy(&written, replayRam + 0x5604u, 4);
                        t.IsTrue(ctx.pc == kSecondChunk && written == 0u, "interrupted at the hop: pc at the second chunk, not run");
                        fn->traced(replayRam, &ctx, &replayRuntime); // (the resume)
                    }
                    fncheck::t_tracer = nullptr;
                    std::memcpy(&written, replayRam + 0x5604u, 4);
                    t.IsTrue(ctx.pc == 0x00100040u && written == a0 * 2u + 1u, "the call finished");
                    t.Equals(tracer.points, 2u, "two safe points: the call's dispatch and the hop");
                    t.Equals(tracer.charged, uint64_t(3u * EeScheduler::kGuestDispatchCycles), "three dispatches charged");
                }
            }
            fn->traced = registered;
        });

        tc.Run("a tail call into the replayed function's own extent runs in place, its calls answered", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture ram;
            PS2Runtime runtime;
            t.IsTrue(r::find(kJumpingOuter) != nullptr && r::find(kJumpedInner) != nullptr, "registered");
            auto run = [&](JumpInsideHandler &handler, JumpBy by) {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, 4, 10u);
                ee::setGpr64(&ctx, 5, static_cast<uint32_t>(by));
                ctx.pc = kJumpingOuter;
                g_jumpedInnerSp = 0;
                ee::t_ee.callHandler = &handler;
                ee::entry<&jumpingOuter>(ram.map.base(), &ctx, &runtime);
                ee::t_ee.callHandler = nullptr;
                return static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0));
            };
            const std::vector<uint32_t> inner{kJumpCallee, kJumpCallee + 4u, kJumpCallee + 8u};
            for (JumpBy by : {JumpBy::Tail, JumpBy::TailCall, JumpBy::TailCallAt})
            {
                JumpInsideHandler handler;
                handler.function = kJumpingOuter;
                handler.end = kJumpingEnd;
                handler.recorded = inner;
                t.Equals(run(handler, by), 13u, "the inner function ran: its two calls answered");
                t.IsTrue(handler.made == inner, "one recorded call: the outer's call, then the inner's, no call of the inner");
                t.Equals(static_cast<uint32_t>(g_jumpedInnerSp), 0x01F00000u, "at the caller's sp (its frame released)");
            }
            // Any other tail call is answered by the handler, as before: one outside the extent, and one the
            // recording holds.
            {
                JumpInsideHandler handler;
                handler.function = kJumpingOuter;
                handler.end = kJumpingEnd;
                handler.recorded = {kJumpCallee, kJumpedOutside};
                t.Equals(run(handler, JumpBy::TailOutside), 12u, "outside the extent: answered");
                t.IsTrue(handler.made == handler.recorded, "a call of the function outside");
                t.Equals(static_cast<uint32_t>(g_jumpedInnerSp), 0u, "the inner function did not run");
            }
            {
                JumpInsideHandler handler;
                handler.function = kJumpingOuter;
                handler.end = kJumpingEnd;
                handler.recorded = {kJumpCallee, kJumpedInner};
                t.Equals(run(handler, JumpBy::Tail), 12u, "a recorded call of the inner function: answered");
                t.IsTrue(handler.made == handler.recorded, "the recording's call made");
                t.Equals(static_cast<uint32_t>(g_jumpedInnerSp), 0u, "the inner function did not run");
            }
            {
                JumpInsideHandler handler; // (no extent: a handler that has none, the default)
                handler.recorded = {kJumpCallee, kJumpedInner};
                t.Equals(run(handler, JumpBy::TailCall), 12u, "no extent: answered");
                t.IsTrue(handler.made == handler.recorded, "the call made");
            }
        });

        tc.Run("game::tail's mark is its callee's call's, and never outlives the callee", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            auto run = [&](TailMarkHandler &handler, uint32_t how) {
                R5900Context ctx = callContext(0x00100040u);
                ee::setGpr64(&ctx, 4, 10u);
                ee::setGpr64(&ctx, 5, how);
                ctx.pc = kTailMarkOuter;
                ee::t_ee.callHandler = &handler;
                ee::entry<&tailMarkOuter>(ram.map.base(), &ctx, &runtime);
                ee::t_ee.callHandler = nullptr;
                return static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0));
            };
            {
                TailMarkHandler handler;
                t.Equals(run(handler, 0u), 11u, "the helper's call answered");
                t.Equals(handler.made.size(), size_t(1), "one call");
                t.IsTrue(!handler.made.empty() && handler.made[0] == std::make_pair(kTailMarkCallee, true),
                         "a plain helper's ee::call is the tail call");
                t.IsFalse(ee::t_ee.tailNext, "the mark taken");
                ee::t_ee.callHandler = &handler; // (the thread's next call)
                (void)ee::call<uint32_t>(kTailMarkCallee + 8u, 1u);
                ee::t_ee.callHandler = nullptr;
                t.IsTrue(handler.made.size() == 2 && !handler.made[1].second, "the next call is a call");
            }
            {
                TailMarkHandler handler;
                t.Equals(run(handler, 1u), 12u, "the helper made no call; the outer's call answered");
                t.IsTrue(handler.made.size() == 1 && handler.made[0] == std::make_pair(kTailMarkCallee + 4u, false),
                         "the call after a tail whose callee made none is a call");
                t.IsFalse(ee::t_ee.tailNext, "no mark left");
            }
        });

        tc.Run("fncheck: a jump table's jump where traced extents overlap goes on in the one extent that holds it", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            // (BMX XXX's: no traced function of the test binary is there)
            static const fncheck::TracedExtent outer(kOverlapOuter, kOverlapOuter + 0x400u, &overlapOuterTraced, game::Game::Bmx);
            static const fncheck::TracedExtent inner(kOverlapInner, kOverlapInner + 0x100u, &overlapInnerTraced, game::Game::Bmx);
            QuietTracer tracer;
            fncheck::setTracedGame(game::Game::Bmx);
            fncheck::t_tracer = &tracer;
            auto jump = [&](uint32_t from, uint32_t to) {
                R5900Context ctx = callContext(0x00100040u);
                const bool returned = fncheck::tracedDispatch(&runtime, ram.map.base(), &ctx, to, from, from + 8u,
                                                              static_cast<int>(PS2Runtime::GuestBranchKind::IndirectJump), "JR");
                t.IsFalse(returned, "the traced function went on");
                t.Equals(ctx.pc, to, "at the target");
            };
            g_overlapOuterRan = g_overlapInnerRan = 0;
            jump(kOverlapOuter + 0x10u, kOverlapOuter + 0x40u);
            t.Equals(g_overlapOuterRan, 1, "before the inner extent: the outer function");
            jump(kOverlapOuter + 0x350u, kOverlapOuter + 0x380u);
            t.Equals(g_overlapOuterRan, 2, "after the inner extent: the outer function (before, no function: a tail call)");
            jump(kOverlapOuter + 0x350u, kOverlapInner + 0x10u);
            t.Equals(g_overlapOuterRan, 3, "into the inner extent from the outer's own part: the outer function");
            t.Equals(g_overlapInnerRan, 0, "never the inner one");
            // (A jump with both ends in both extents stops the run, naming both functions: fncheck.cpp
            // sameTracedFunction. Not run here: it aborts.)
            fncheck::t_tracer = nullptr;
            fncheck::setTracedGame(game::Game::Ai);
        });

        tc.Run("fncheck keeps a call's I/O register accesses with their values, in order", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kIoTouching);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            fn->traced = &ioTouchingTraced;
            runtime.replaceFunction(kIoTouching, &ioTouchingTraced);
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_io_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            setEnvironment("PS2X_FNREC", "ioTouching");
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            installTestRecorder(runtime);
            setEnvironment("PS2X_FNREC", "");
            for (uint32_t mark : {1u, 2u})
            {
                g_ioMark = mark;
                R5900Context ctx = callContext(0x00100040u);
                ctx.pc = kIoTouching;
                runtime.lookupFunction(kIoTouching)(ram, &ctx, &runtime);
                t.Equals(g_ioStored, mark + 0x140u, "the store reached the runtime");
            }
            fncheck::RecordingReader recording;
            recording.open(folder / "00004a00.fnrec");
            std::istream &file = recording.stream();
            fncheck::FileHeader header;
            std::string error;
            t.IsTrue(fncheck::readHeader(file, header, &error), "recorded");
            std::vector<fncheck::Record> records;
            fncheck::Record record;
            while (fncheck::readRecord(file, record, header.version))
                records.push_back(record);
            t.Equals(records.size(), size_t(2), "both calls kept (and read back past the I/O log)");
            for (size_t i = 0; i < records.size(); ++i)
            {
                const fncheck::Record &r = records[i];
                t.IsTrue((r.flags & fncheck::kIoLog) && !(r.flags & fncheck::kSpecialAccess), "an I/O log, replayable");
                t.Equals(r.io.size(), size_t(3), "three accesses");
                if (r.io.size() != 3)
                    continue;
                t.IsTrue(r.io[0].address == 0x10003C30u && r.io[0].size == 4 && !r.io[0].store && r.io[0].lo == i + 1,
                         "the load and the value read");
                t.IsTrue(r.io[1].address == 0x1000A000u && r.io[1].store && r.io[1].lo == i + 1 + 0x140u,
                         "the store and the value written");
                t.IsTrue(r.io[2].size == 16 && r.io[2].lo == 0x0000000200000001ull && r.io[2].hi == 0x0000000400000003ull,
                         "a quadword store, both halves");
            }

            // ps2xFnCheck's side: an IoPort answers the loads instead of the runtime.
            struct Port final : fncheck::IoPort
            {
                std::vector<fncheck::IoAccess> seen;
                void load(uint32_t address, uint32_t size, uint64_t &lo, uint64_t &hi) override
                {
                    lo = 0x77;
                    hi = 0;
                    seen.push_back({address, static_cast<uint8_t>(size), 0, 0, lo, hi});
                }
                void store(uint32_t address, uint32_t size, uint64_t lo, uint64_t hi) override
                {
                    seen.push_back({address, static_cast<uint8_t>(size), 1, 0, lo, hi});
                }
            } port;
            g_ioStored = 0;
            fncheck::t_ioPort = &port;
            R5900Context ctx = callContext(0x00100040u);
            ioTouchingTraced(ram, &ctx, &runtime);
            fncheck::t_ioPort = nullptr;
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 0x77u, "the load answered by the port");
            t.Equals(g_ioStored, 0u, "the runtime not touched");
            t.IsTrue(port.seen.size() == 3 && port.seen[1].lo == 0x77u + 0x140u, "the stores given to the port");
            recording.close();
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck: callee writes by page protection are the full RAM diff's", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *a = game::readable::find(kPageWriter);
            game::readable::Function *b = game::readable::find(kPageWriterB);
            t.IsTrue(a && b, "registered");
            if (!a || !b)
                return;
            a->traced = &pageWriterTraced<kPageWriter>;
            b->traced = &pageWriterTraced<kPageWriterB>;
            runtime.replaceFunction(kPageWriter, &pageWriterTraced<kPageWriter>);
            runtime.replaceFunction(kPageWriterB, &pageWriterTraced<kPageWriterB>);
            runtime.replaceFunction(kPageCallee, &pageCallee);
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_pages_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "1");
            std::vector<fncheck::Line> effects[2];
            const uint32_t functions[2] = {kPageWriter, kPageWriterB};
            const char *modes[2] = {"pages", "diff"};
            for (int i = 0; i < 2; ++i)
            {
                std::memset(ram + 0x5400u, 0, 16);
                std::memset(ram + 0x00300000u, 0, 32);
                std::memset(ram + 0x00400000u, 0, 64);
                setEnvironment("PS2X_FNREC_EFFECTS", modes[i]);
                setEnvironment("PS2X_FNREC", i == 0 ? "pageWriter" : "pageWriterB");
                installTestRecorder(runtime);
                R5900Context ctx = callContext(0x00100040u);
                ctx.pc = functions[i];
                runtime.lookupFunction(functions[i])(ram, &ctx, &runtime);
                t.Equals(ctx.pc, 0x00100040u, "returned");
                char name[32];
                std::snprintf(name, sizeof(name), "%08x.fnrec", functions[i]);
                fncheck::RecordingReader recording;
                recording.open(folder / name);
                std::istream &file = recording.stream();
                fncheck::FileHeader header;
                std::string error;
                fncheck::Record record;
                t.IsTrue(fncheck::readHeader(file, header, &error) && fncheck::readRecord(file, record, header.version),
                         "recorded");
                t.Equals(record.calls.size(), size_t(1), "one call");
                if (!record.calls.empty())
                    effects[i] = record.calls[0].effects;
            }
            setEnvironment("PS2X_FNREC", "");
            setEnvironment("PS2X_FNREC_EFFECTS", "");
            t.Equals(effects[0].size(), size_t(6), "two words and four lines of the file read; not the unchanged word, "
                                                   "not the dead frame");
            t.Equals(effects[0].size(), effects[1].size(), "as many lines as the full diff");
            bool same = effects[0].size() == effects[1].size();
            for (size_t i = 0; same && i < effects[0].size(); ++i)
                same = std::memcmp(&effects[0][i], &effects[1][i], sizeof(fncheck::Line)) == 0;
            t.IsTrue(same, "the same lines, byte for byte");
            fs::remove_all(folder, ignored);
        });

        tc.Run("fncheck reads recordings made before memory keys", [](TestCase &t)
        {
            // Lines at the uncached mirror and the scratchpad's EE address
            // (as recordings before round 7 kept them) fold onto their keys;
            // two lines of the same RAM line merge.
            fncheck::Record record;
            fncheck::Line cached, uncached, scratch;
            cached.address = 0x00441290u;
            cached.mask = 0x0003u;
            cached.bytes[0] = 1;
            cached.bytes[1] = 2;
            uncached = cached;
            uncached.address = 0x20441290u;
            uncached.mask = 0x0004u;
            uncached.bytes[2] = 3;
            scratch.address = 0x70000400u;
            scratch.mask = 0x0001u;
            scratch.bytes[0] = 9;
            record.inputs = {cached, uncached, scratch};
            std::stringstream file;
            fncheck::writeHeader(file, 0x00107BD0u);
            fncheck::writeRecord(file, record);
            fncheck::FileHeader header;
            t.IsTrue(fncheck::readHeader(file, header, nullptr), "header");
            fncheck::Record back;
            t.IsTrue(fncheck::readRecord(file, back, header.version), "record");
            t.Equals(back.inputs.size(), size_t(2), "the two RAM lines merged, the scratchpad's kept");
            if (back.inputs.size() == 2)
            {
                t.Equals(back.inputs[0].address, 0x00441290u, "the mirror folded onto RAM");
                t.Equals(int(back.inputs[0].mask), 0x0007, "both lines' bytes");
                t.Equals(int(back.inputs[0].bytes[2]), 3, "the mirror's byte");
                t.Equals(back.inputs[1].address, fncheck::kScratchpadKey + 0x400u, "the scratchpad keyed after RAM");
            }
        });
        tc.Run("a tail call runs the function in the table", [](TestCase &t)
        {
            GuestRamFixture ram;
            PS2Runtime runtime;
            R5900Context ctx = callContext(0x00100040u);
            runtime.replaceFunction(0x00007000u, nullptr);
            tailCaller(ram.map.base(), &ctx, &runtime);
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 1u, "no entry: the recompiled target");
            runtime.replaceFunction(0x00007000u, &tailTarget);
            tailCaller(ram.map.base(), &ctx, &runtime);
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 1u, "the recompiled target in the table");
            const bool stats = PS2Runtime::s_tailCallTable.stats;
            PS2Runtime::s_tailCallTable.stats = true;
            runtime.replaceFunction(0x00007000u, &tailReplacement);
            tailCaller(ram.map.base(), &ctx, &runtime);
            tailCaller(ram.map.base(), &ctx, &runtime);
            PS2Runtime::s_tailCallTable.stats = stats;
            t.Equals(static_cast<uint32_t>(ee::gpr64(&ctx, ee::reg::v0)), 2u, "a replacement in the table runs");
            t.Equals(ctx.pc, 0x00100040u, "back at the caller's return address");
            bool counted = false;
            for (const PS2Runtime::TailCallCount &c : PS2Runtime::replacedTailCalls())
                counted |= c.sourcePc == 0x00006F00u && c.targetPc == 0x00007000u && c.calls == 2u;
            t.IsTrue(counted, "both calls to the replacement counted at their site");
            runtime.replaceFunction(0x00007000u, nullptr);
        });
        tc.Run("game::callAt tells functions with one host code apart", [](TestCase &t)
        {
            TargetHandler handler;
            ee::CallHandler *previous = ee::t_ee.callHandler;
            ee::t_ee.callHandler = &handler;
            game::call<&sharedCode>(1u);
            t.Equals(handler.target, 0x00000600u, "game::call: the first registration");
            game::callAt<&sharedCode, 0x00000700u>(1u);
            t.Equals(handler.target, 0x00000700u, "game::callAt: the address given");
            ee::t_ee.callHandler = previous;
        });
        tc.Run("a game::call marks its callee, and reads of what a call left reach the handler", [](TestCase &t)
        {
            t.IsTrue(game::readable::calledDirectly(reinterpret_cast<const void *>(&hookTarget)), "hookTarget: named by a game::call");
            t.IsFalse(game::readable::calledDirectly(reinterpret_cast<const void *>(&sumPair)), "sumPair: no game::call names it");
            struct ReadHandler : ee::CallHandler
            {
                int reg = -1;
                bool left = false;
                void call(ee::CallRegs &) override {}
                void registerRead(int r, bool leftByCall) override
                {
                    reg = r;
                    left = leftByCall;
                }
            } handler;
            ee::CallHandler *previous = ee::t_ee.callHandler;
            ee::t_ee.callHandler = &handler;
            (void)ee::leftByCall(ee::reg::a0);
            ee::t_ee.callHandler = previous;
            t.Equals(handler.reg, static_cast<int>(ee::reg::a0), "ee::leftByCall's register");
            t.IsTrue(handler.left, "as a callee's");
        });
        tc.Run("recordings key the scratchpad after RAM", [](TestCase &t)
        {
            t.Equals(fncheck::memoryKey(0x00123450u), 0x00123450u, "RAM");
            t.Equals(fncheck::memoryKey(0x20123450u), 0x00123450u, "uncached mirror");
            t.Equals(fncheck::memoryKey(0x70000400u), fncheck::kScratchpadKey + 0x400u, "scratchpad");
            t.Equals(fncheck::kMemoryKeys, PS2_RAM_SIZE + PS2_SCRATCHPAD_SIZE, "keys");
            std::vector<uint8_t> ram(16), scratch(PS2_SCRATCHPAD_SIZE);
            uint8_t *previous = ps2GetScratchpadHostPtr();
            ps2SetScratchpadHostPtr(scratch.data());
            t.IsTrue(fncheck::keyPointer(ram.data(), 4u) == ram.data() + 4, "RAM byte");
            t.IsTrue(fncheck::keyPointer(ram.data(), fncheck::kScratchpadKey + 0x800u) == scratch.data() + 0x800,
                     "scratchpad byte");
            ps2SetScratchpadHostPtr(previous);
        });

        tc.Run("registrations are per game: (game, address), names with a game prefix", [](TestCase &t)
        {
            namespace r = game::readable;
            r::Function *ai = r::find(0x00000800u);
            r::Function *dm2 = r::find(0x00000800u, game::Game::Dm2);
            t.IsTrue(ai && dm2 && ai != dm2, "both registered at one address");
            if (!ai || !dm2)
                return;
            t.IsTrue(ai->game == game::Game::Ai && dm2->game == game::Game::Dm2, "each names its game");
            t.IsTrue(std::string(ai->name) == "aiAt800" && std::string(dm2->name) == "dm2At800", "AI's by default");
            t.IsTrue(r::find("dm2:0x00000800") == dm2 && r::find("dm2:dm2At800") == dm2, "dm2: prefix");
            t.IsTrue(r::find("0x00000800", game::Game::Dm2) == dm2 && r::find("dm2At800", game::Game::Dm2) == dm2,
                     "in a game");
            t.IsTrue(r::find("dm2At800") == nullptr && r::find("aiAt800", game::Game::Dm2) == nullptr,
                     "names are a game's own");
            t.IsTrue(r::find("ai:aiAt800") == ai, "ai: prefix");
            r::Function *waitsIn = r::find(0x00000A00u, game::Game::Dm2);
            t.IsTrue(waitsIn && waitsIn->game == game::Game::Dm2 && waitsIn->waits && !waitsIn->leavesVu0,
                     "READABLE_FUNCTION_WAITS_IN: DM2's, waiting");
            t.IsTrue(waitsIn && waitsIn->readable == &ee::entrySuspendable<&dm2WaitsAtA00> &&
                         waitsIn->returnKind == ee::ReturnKind::Int32 && waitsIn->status == r::Status::Draft &&
                         std::string(waitsIn->name) == "dm2WaitsAtA00",
                     "READABLE_FUNCTION_WAITS_IN: on a fiber, its signature and status");
            t.IsTrue(r::find(0x00000A00u) == nullptr && r::find("dm2:dm2WaitsAtA00") == waitsIn, "not AI's");
            t.IsTrue(r::find("xbox:0x00000800") == nullptr, "unknown game");
            r::Function *planned = r::find("dm2:dm2PlannedProbe");
            t.IsTrue(planned && planned->status == r::Status::Planned && !planned->readable, "DM2's planned");
            t.IsTrue(ai->enabled && !dm2->enabled, "statuses as registered (AI's Verified on, DM2's Tested off)");
            t.IsTrue(r::tracedOriginal(0x00000800u) == nullptr &&
                         r::tracedOriginal(0x00000800u, game::Game::Dm2) == &recompiledAt800,
                     "traced originals by game");
            // A switch applies to one game's registrations.
            t.IsTrue(r::applySwitch("all", nullptr, game::Game::Dm2) && dm2->enabled && ai->enabled, "DM2's all");
            t.IsTrue(r::applySwitch("off", nullptr, game::Game::Dm2) && !dm2->enabled && ai->enabled, "DM2's off");
            t.IsFalse(r::applySwitch("aiAt800", nullptr, game::Game::Dm2), "AI's name is not DM2's");
            t.IsFalse(r::applySwitch("dm2:dm2At800"), "nor DM2's in AI's switch");
            t.IsTrue(ai->enabled && !dm2->enabled, "neither changed");
        });

        tc.Run("install in DM2 never puts AI's readable functions over DM2's code", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture guest;
            r::Function *ai = r::find(0x00000800u);
            r::Function *dm2 = r::find(0x00000800u, game::Game::Dm2);
            t.IsTrue(ai && dm2, "registered");
            if (!ai || !dm2)
                return;
            std::vector<std::pair<r::Function *, bool>> saved;
            for (r::Function *f : r::functions())
                saved.emplace_back(f, f->enabled);
            PS2Runtime runtime;
            runtime.replaceFunction(0x00000800u, &recompiledAt800);
            runtime.replaceFunction(kDm2Planned, &dm2PlannedTraced);
            setEnvironment("PS2X_READABLE", "");
            r::install(runtime, game::Game::Dm2);
            t.IsTrue(runtime.lookupFunction(0x00000800u) == &recompiledAt800, "DM2's code stays (AI's Verified one not installed)");
            t.IsTrue(!ai->enabled && ai->original == nullptr, "AI's off, no original in DM2");
            t.IsTrue(dm2->original == &recompiledAt800 && dm2->traced == &recompiledAt800, "DM2's original and traced");
            t.IsTrue(r::runningGame() == game::Game::Dm2 && !r::runsInGame(0x00000800u), "running DM2; AI's not running");
            setEnvironment("PS2X_READABLE", "tested");
            r::install(runtime, game::Game::Dm2);
            setEnvironment("PS2X_READABLE", "");
            t.IsTrue(dm2->enabled && runtime.lookupFunction(0x00000800u) == dm2->readable, "DM2's Tested one with tested");
            t.IsTrue(!ai->enabled && r::runsInGame(0x00000800u, game::Game::Dm2), "AI's still off");
            runtime.replaceFunction(0x00000800u, &recompiledAt800);
            for (auto &[f, enabled] : saved)
                f->enabled = enabled;
        });

        tc.Run("install puts its game's readable flags back as registered: another run or game changed them", [](TestCase &t)
        {
            namespace r = game::readable;
            GuestRamFixture guest;
            r::Function *ai = r::find(0x00000800u);
            r::Function *dm2 = r::find(0x00000800u, game::Game::Dm2);
            t.IsTrue(ai && dm2, "registered");
            if (!ai || !dm2)
                return;
            std::vector<std::pair<r::Function *, bool>> saved;
            for (r::Function *f : r::functions())
                saved.emplace_back(f, f->enabled);
            PS2Runtime runtime;
            runtime.replaceFunction(0x00000800u, &recompiledAt800);
            runtime.replaceFunction(kDm2Planned, &dm2PlannedTraced);
            setEnvironment("PS2X_READABLE", "tested");
            r::install(runtime, game::Game::Dm2);
            t.IsTrue(dm2->enabled && !ai->enabled, "DM2 with tested: DM2's Tested one on, AI's off");
            setEnvironment("PS2X_READABLE", "");
            runtime.replaceFunction(0x00000800u, &recompiledAt800);
            r::install(runtime, game::Game::Dm2);
            t.IsTrue(!dm2->enabled && runtime.lookupFunction(0x00000800u) == &recompiledAt800,
                     "DM2 again with the defaults: its Tested one off, the recompiled function stays");
            r::install(runtime, game::Game::Ai);
            t.IsTrue(ai->enabled && runtime.lookupFunction(0x00000800u) == ai->readable,
                     "AI after DM2: its Verified one on again");
            t.IsTrue(!dm2->enabled, "DM2's off in AI");
            runtime.replaceFunction(0x00000800u, &recompiledAt800);
            for (auto &[f, enabled] : saved)
                f->enabled = enabled;
        });

        tc.Run("a DM2 recording names its game; ps2xFnCheck's reader takes it", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            t.Equals(fncheck::recordingName(0x001A9C90u, game::Game::Ai), std::string("001a9c90.fnrec"), "AI's file");
            t.Equals(fncheck::recordingName(0x001A9C90u, game::Game::Dm2), std::string("dm2-001a9c90.fnrec"), "DM2's file");
            {
                std::stringstream file;
                fncheck::writeHeader(file, 0x001A9C90u, game::Game::Dm2);
                fncheck::FileHeader header;
                std::string error;
                t.IsTrue(fncheck::readHeader(file, header, &error), "header");
                t.Equals(header.version, fncheck::kRecordVersion, "the recorder's version (6 on name the game)");
                t.Equals(header.game, 1u, "DM2");
            }
            {
                // Version 5: the word was reserved (AI's, whatever it holds).
                fncheck::FileHeader old;
                old.version = 5;
                old.game = 1;
                std::stringstream file;
                file.write(reinterpret_cast<const char *>(&old), sizeof(old));
                fncheck::FileHeader header;
                t.IsTrue(fncheck::readHeader(file, header, nullptr) && header.game == 0u, "a version 5 file is AI's");
            }
            {
                fncheck::FileHeader unknown;
                unknown.game = 7;
                std::stringstream file;
                file.write(reinterpret_cast<const char *>(&unknown), sizeof(unknown));
                fncheck::FileHeader header;
                std::string error;
                t.IsFalse(fncheck::readHeader(file, header, &error), "an unknown game is refused");
            }

            // The recorder in DM2: DM2's names; AI's are not DM2's.
            GuestRamFixture guest;
            uint8_t *ram = guest.map.base();
            PS2Runtime runtime;
            game::readable::Function *fn = game::readable::find(kDm2Planned, game::Game::Dm2);
            t.IsTrue(fn != nullptr, "registered");
            if (!fn)
                return;
            fn->traced = &dm2PlannedTraced;
            runtime.replaceFunction(kDm2Planned, &dm2PlannedTraced);
            const fs::path folder = uniqueTempFolder("ps2x_fnrec_dm2_test");
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            setEnvironment("PS2X_FNREC", "dm2PlannedProbe,selfCalling");
            setEnvironment("PS2X_FNREC_DIR", folder.string().c_str());
            setEnvironment("PS2X_FNREC_ATOMIC", "0");
            installTestRecorder(runtime, game::Game::Dm2);
            setEnvironment("PS2X_FNREC", "");
            fncheck::setTracedGame(game::Game::Ai);
            *reinterpret_cast<uint32_t *>(ram + 0x5200u) = 41u;
            R5900Context ctx = callContext(0x00100040u);
            ctx.pc = kDm2Planned;
            runtime.lookupFunction(kDm2Planned)(ram, &ctx, &runtime);
            t.Equals(*reinterpret_cast<uint32_t *>(ram + 0x5204u), 42u, "ran");
            t.IsFalse(fs::exists(folder / "00004000.fnrec"), "AI's function not recorded in DM2");
            fncheck::RecordingReader recording;
            recording.open(folder / "dm2-00000900.fnrec");
            std::istream &file = recording.stream();
            fncheck::FileHeader header;
            std::string error;
            t.IsTrue(fncheck::readHeader(file, header, &error), "recorded under DM2's name");
            t.Equals(header.game, 1u, "the header names DM2");
            t.Equals(header.address, kDm2Planned, "its address");
            fncheck::Record record;
            t.IsTrue(fncheck::readRecord(file, record, header.version), "a record");
            t.Equals(record.outputs.size(), size_t(1), "its write");
            recording.close();
            fs::remove_all(folder, ignored);
        });

        tc.Run("GEO collision chunks read on the host", [](TestCase &t)
        {
            // A TGEO body as the game parses it: TGHD, TGFR, TGCI, then a
            // part with positions, faces, indexed face normals and the two
            // buffer streams.
            std::vector<uint8_t> f;
            auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(uint8_t(v >> (8 * i))); };
            auto u16 = [&](uint16_t v) { f.push_back(uint8_t(v)); f.push_back(uint8_t(v >> 8)); };
            auto f32 = [&](float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); };
            auto chunk = [&](const char *tag) { f.insert(f.end(), tag, tag + 4); u32(0); return f.size(); };
            auto close = [&](size_t start) { const uint32_t n = uint32_t(f.size() - start); std::memcpy(&f[start - 4], &n, 4); };
            size_t c = chunk("TGHD");
            f.resize(f.size() + 0x40, 0);
            close(c);
            c = chunk("TGFR");
            u32(0x00030201u);
            close(c);
            c = chunk("TGCI");
            u32(7);   // part id
            u32(12);  // tree bytes: one leaf with both faces
            f.push_back(0); f.push_back(0); u16(2); u32(4); u16(0); u16(1);
            close(c);
            const size_t part = chunk("TGPT");
            c = chunk("TGPH");
            u16(1); u16(0); u16(4); u16(2); u16(0); f.push_back(0); f.push_back(0); f.push_back(0); f.push_back(0);
            f.push_back(1); f.push_back(0); u32(7);
            close(c);
            c = chunk("TGVP");
            for (int v = 0; v < 4; ++v) { f32(float(v)); f32(0.0f); f32(float(v * 2)); }
            close(c);
            c = chunk("TGFP");
            u16(0x2300); u16(0); u16(1); u16(2);
            u16(0x6300); u16(1); u16(3); u16(2);
            close(c);
            const size_t normals = chunk("TIFN");
            c = chunk("TIDA");
            f32(0.0f); f32(-1.0f); f32(0.0f);
            f32(0.0f); f32(1.0f); f32(0.0f);
            close(c);
            c = chunk("TII8");
            f.push_back(1); f.push_back(0); f.push_back(0); f.push_back(0);
            close(c);
            close(normals);
            c = chunk("TGFT");
            u32(0x40C1u); u32(2); u16(0); u16(0xFFFF); u32(0x11223344u); u32(0);
            close(c);
            c = chunk("TGFE");
            u32(0x8181u); u32(6); for (int i = 0; i < 6; ++i) u16(0xFFFF);
            close(c);
            close(part);

            std::string error;
            const auto objects = zge::native::readGeoFile(f, &error);
            t.Equals(error, std::string(), "no error");
            t.Equals(objects.size(), size_t(1), "one object");
            const auto &collision = *objects[0].collision;
            t.IsTrue(collision.hasFrame && collision.frame[1] == 2, "TGFR");
            t.IsTrue(collision.hasTree, "TGCI");
            t.Equals(collision.treePartId, 7u, "TGCI's part id");
            t.Equals(collision.tree.size(), size_t(12), "tree bytes");
            t.Equals(objects[0].parts.size(), size_t(1), "one part");
            const auto &p = objects[0].parts[0];
            t.Equals(p.partId, 7u, "TGPH's id");
            t.Equals(p.faces.size(), size_t(16), "faces");
            t.Equals(int(p.faces[1]), 0x23, "face flags");
            t.IsTrue(p.hasFaceNormals && p.faceNormals.size() == 6, "face normals");
            t.Equals(p.faceNormals[1], 1.0f, "face 0 through index 1");
            t.Equals(p.faceNormals[4], -1.0f, "face 1 through index 0");
            t.IsTrue(p.hasFaceTags && p.faceTags.size() == 12, "face tags after their header");
            t.IsTrue(p.hasFaceEdges && p.faceEdges.size() == 12, "face edges after their header");
            t.IsTrue(p.objectCollision.get() == objects[0].collision.get(), "the object's chunks shared");
        });

        tc.Run("the level setup's load-time words made on the host", [](TestCase &t)
        {
            // A square of two faces on y = 0: face 0 (v0, v1, v2) tagged kind 1
            // (the nearest rail edge), face 1 (v1, v3, v2) kind 0xB (its own
            // index). Edges: face 0's v0-v1 a kind 2 record, face 0's v2-v0 and
            // face 1's v1-v3 the same kind 1 record.
            zge::native::GeoFilePart file;
            file.partId = 9;
            file.vertexCount = 4;
            file.faceCount = 2;
            file.positions = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1};
            auto face = [&](uint16_t a, uint16_t b, uint16_t c)
            {
                const uint16_t words[4] = {0x2300, a, b, c};
                const uint8_t *p = reinterpret_cast<const uint8_t *>(words);
                file.faces.insert(file.faces.end(), p, p + 8);
            };
            face(0, 1, 2);
            face(1, 3, 2);
            file.hasFaceTags = true;
            file.faceTagsDescriptor = 0u | (28u << 4) | (2u << 14);
            file.faceTagsCount = 2;
            file.faceTags.assign(16 + 2 * 28, 0);
            file.faceTags[0] = 0;
            file.faceTags[1] = 1;
            file.faceTags[16 + 0x0C] = 1;         // record 0: kind 1
            file.faceTags[16 + 28 + 0x0C] = 0x0B; // record 1: kind 0xB
            file.hasFaceEdges = true;
            file.faceEdgesDescriptor = 0u | (24u << 4) | (2u << 14);
            file.faceEdgesCount = 6;
            file.faceEdges.assign(16 + 2 * 24, 0);
            const uint8_t edgeIndex[6] = {1, 0xFF, 0, 0, 0xFF, 0xFF};
            std::memcpy(file.faceEdges.data(), edgeIndex, 6);
            file.faceEdges[16 + 0x08] = 1;      // record 0: kind 1
            file.faceEdges[16 + 24 + 0x08] = 2; // record 1: kind 2
            const uint32_t widths[16] = {1, 2};
            const auto part = zge::native::makeHostCollisionPart(file, widths, &aggressive::applyLevelSetupOnHost);
            t.IsTrue(part != nullptr, "made");
            // A game with no level setup step (DM2, BMX XXX) keeps the file's words.
            const auto raw = zge::native::makeHostCollisionPart(file, widths, nullptr);
            t.IsTrue(raw != nullptr && raw->faceEdges.body == file.faceEdges && raw->faceTags.body == file.faceTags,
                     "no model: the file's tag tables as they are");
            t.Equals(raw->setup.edgeElements, 0u, "no model: no edge element written");
            auto word = [](const std::vector<uint8_t> &body, size_t at)
            {
                uint32_t v;
                std::memcpy(&v, body.data() + at, 4);
                return v;
            };
            // Face 0's centroid (1/3, 0, 1/3): v2-v0 (element 2) at 1/3, v1-v3 (element 3) at 2/3.
            t.Equals(word(part->faceTags.body, 16 + 4), 2u, "face 0's nearest kind-1 edge");
            t.Equals(word(part->faceTags.body, 16 + 28 + 8), 1u, "the kind-0xB face's index");
            t.Equals(word(part->faceEdges.body, 16 + 4), 3u, "edge record 0: the last element that picks it");
            t.Equals(word(part->faceEdges.body, 16 + 24 + 4), 0u, "edge record 1: element 0");
            t.Equals(part->setup.nearestEdges, 1u, "one nearest edge");
            t.Equals(part->setup.edgeElements, 3u, "three edge elements");
            t.IsTrue(part->positions[3].w == 1.0f && part->positions[3].x == 1.0f, "float4 positions, w 1");
            t.Equals(part->faceTags.recordOffset(5), int64_t(-1), "an element past the indices");
            t.Equals(part->faceEdges.recordOffset(1), int64_t(-1), "index 0xFF: none");
            t.Equals(part->faceEdges.recordOffset(2), int64_t(16), "index 0: the first record");
        });

        tc.Run("VU0 upload: the game's VIF0 stream through the runtime's VIF0, put back at each replay, run by VCALLMS", [](TestCase &t)
        {
            // A game's upload as vu0_upload.py writes one: NOP, MPG of three pairs at VU0's address 0 (vf2 = vf1 +
            // vf1, then an E-bit pair and its delay slot), each pair its lower word, then its upper.
            constexpr uint32_t kVifMpgThreePairsAtZero = 0x4A030000u;
            constexpr uint32_t kUpperAddVf2Vf1Vf1 = (0xFu << 21) | (1u << 16) | (1u << 11) | (2u << 6) | 0x28u;
            constexpr uint32_t kUpperNop = 0x0000003Fu, kUpperEndNop = 0x4000003Fu, kLowerNone = 0u;
            const uint32_t words[] = {0u, kVifMpgThreePairsAtZero, kLowerNone, kUpperAddVf2Vf1Vf1,
                                      kLowerNone, kUpperEndNop, kLowerNone, kUpperNop};
            uint8_t stream[sizeof(words)];
            std::memcpy(stream, words, sizeof(words));

            PS2Runtime runtime; // (no initialize(): ps2xFnCheck's has no memory of its own but VU0's)
            PS2Memory &memory = runtime.memory();
            t.IsTrue(memory.getVU0Code() == nullptr, "no VU0 memory before the upload");
            fncheck::Vu0Memory vu0;
            t.IsFalse(vu0.uploaded(), "nothing uploaded yet");
            vu0.upload(memory, stream, sizeof(stream));
            t.IsTrue(vu0.uploaded(), "uploaded");
            t.IsTrue(std::memcmp(memory.getVU0Code(), words + 2, 24u) == 0, "the MPG's pairs at VU0's address 0");
            t.IsTrue(memory.getVU0Code()[24] == 0u, "the rest of the code memory zero");

            const auto run = [&]()
            {
                R5900Context ctx{};
                ctx.vu0_vf[1] = _mm_setr_ps(1.0f, 2.0f, 3.0f, 4.0f);
                runtime.executeVU0Microprogram(nullptr, &ctx, 0u);
                alignas(16) float vf2[4]{};
                _mm_storeu_ps(vf2, ctx.vu0_vf[2]);
                return vf2[0] == 2.0f && vf2[1] == 4.0f && vf2[2] == 6.0f && vf2[3] == 8.0f;
            };
            t.IsTrue(run(), "VCALLMS 0 runs the uploaded microprogram: vf2 = vf1 + vf1");

            std::memset(memory.getVU0Code(), 0, 24u); // (as if something had changed it)
            std::memset(memory.getVU0Data(), 0xA5, PS2_VU0_DATA_SIZE);
            vu0.restore(memory);
            t.IsTrue(std::memcmp(memory.getVU0Code(), words + 2, 24u) == 0, "the code put back");
            t.IsTrue(memory.getVU0Data()[0] == 0u && memory.getVU0Data()[PS2_VU0_DATA_SIZE - 1] == 0u,
                     "the data memory as the upload left it");
            t.IsTrue(run(), "and run again from it (the decoded program made again)");

            fncheck::Vu0Memory none;
            none.upload(memory, nullptr, 0u);
            t.IsTrue(none.uploaded() && memory.getVU0Code()[4] == 0u, "a game with no upload: VU0's memory zeroed");
        });

        tc.Run("generated calls: the accesses the hardware would fault on", [](TestCase &t)
        {
            namespace dc = game::diffcall;
            const auto wild = [](uint32_t address, uint32_t size)
            {
                dc::g_wildAccesses = 0;
                dc::noteAccess(address, size);
                const bool found = dc::g_wildAccesses != 0;
                dc::g_wildAccesses = 0;
                return found;
            };
            t.IsFalse(wild(0x00100000u, 4u), "RAM");
            t.IsFalse(wild(0x20100000u, 4u), "the uncached mirror");
            t.IsFalse(wild(0x70000010u, 16u), "the scratchpad");
            t.IsFalse(wild(0x00100001u, 1u), "a byte at any address");
            t.IsTrue(wild(0x00000028u, 4u), "a null pointer's field");
            t.IsTrue(wild(0xFFFFFFFCu, 4u), "before address 0");
            t.IsTrue(wild(0x40000000u, 4u), "no memory there");
            t.IsTrue(wild(0x00100002u, 4u), "a misaligned word");
            t.IsTrue(wild(0x00100008u, 16u), "a misaligned quadword: the hardware clears its low bits, generated code does not");
        });
    });
}
