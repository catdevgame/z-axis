#pragma once

// The readable game: hand-written C++ that replaces recompiled functions
// one at a time.
//
// Each replacement is registered next to its definition:
//
//     const char *stringTableLookup(uint32_t id) { ... }
//     READABLE_FUNCTION(stringTableText, stringTableLookup, Tested);
//
// and installed over the recompiled function when the game starts
// (PS2Machine::replaceFunction, before any other override wraps it), if it
// is switched on. An override that wraps it wraps the readable version, and
// readable callers reach the wrapper: game::call goes through the table for
// a hooked function (Function::hooked). The status says how far it is proven:
//
//   Planned   no readable version yet; registered so zx-fncheck can
//             record its calls (READABLE_PLANNED)
//   Draft     written, not yet clean against its recorded calls
//   Tested    identical to the recompiled function on every recorded call
//             (zx-fncheck, ps2/ee/fncheck.h)
//   Verified  Tested, and games/aggressive/tools/logiccheck.py stays 21/21 MATCH with it on
//
// Verified functions are on by default. ZX_READABLE changes that, a
// comma-separated list read left to right:
//
//   on / verified   the default (Verified functions)
//   off             none: the game runs the recompiled code only
//   tested          Tested and Verified functions
//   all             every function with a readable version
//   stringTableText or stringTableLookup        this one on
//   -stringTableText or -stringTableLookup      this one off
//
// Functions registered with READABLE_FUNCTION_WAITS (their callees can
// wait, e.g. the chunk loaders' file reads) run on a fiber, so their calls
// can suspend them (ee/suspend.cpp).
//
// Build files: CMake reads the READABLE_FUNCTION / READABLE_PLANNED
// addresses from the sources (game/game.cmake) to build the recompiled
// originals a second time with call recording (fncheck).
//
// Games (ee/game_id.h): a registration belongs to one game, Aggressive
// Inline's unless made with an _IN macro (READABLE_PLANNED_IN(dm2,
// ADDRESS, "riderFrame")). install(runtime, game) installs only the
// running game's registrations, so AI's readable functions never go over
// another game's code at the same addresses; lookups by address or name
// take the game (AI's by default), and `dm2:ADDRESS` or `dm2:riderFrame`
// names another game's.

#include "abi.h"
#include "game_id.h"

#include "machine.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace game::readable
{
    enum class Status : uint8_t
    {
        Planned,
        Draft,
        Tested,
        Verified,
    };
    const char *statusName(Status status);

    using ::game::Game;

    // One object per C++ function type: a registration's type, compared by address.
    template <typename F>
    inline constexpr char kSignatureTag = 0;
    template <typename F>
    constexpr const void *signatureOf()
    {
        return &kSignatureTag<F>;
    }

    struct Function
    {
        Game game = Game::Ai;
        uint32_t address = 0;
        const char *name = "";
        const char *file = "";
        Status status = Status::Planned;
        // ee::entry<&fn>; null while Planned.
        PS2Machine::RecompiledFunction readable = nullptr;
        ee::ReturnKind returnKind = ee::ReturnKind::Void;
        // The C++ function itself (for game::call<&fn>).
        const void *host = nullptr;
        // Its C++ type (signatureOf): a call of the function at this address from its own body runs `host` directly
        // under zx-fncheck only when the caller's entry has the same type (an own body's caller is its shared
        // body's name: game::detail::Route).
        const void *signature = nullptr;
        // Its callees can wait (a file read, a semaphore): it runs on a fiber
        // and its calls suspend it (ee::entrySuspendable).
        bool waits = false;
        // Its callers read what it leaves in VU0: it writes the registers,
        // Q and flags as the original leaves them (ee::vu0::Leave), and
        // zx-fncheck compares them (READABLE_FUNCTION_VU0).
        bool leavesVu0 = false;

        // Set by install(): the recompiled function, the same built with
        // fncheck's tracing (null if this build has none), and whether the
        // readable version runs in the game.
        PS2Machine::RecompiledFunction original = nullptr;
        PS2Machine::RecompiledFunction traced = nullptr;
        bool enabled = false;
        // What its table slot held when install() finished, and whether a
        // game override wrapped it since (replaceFunction, seen once all
        // overrides are in: noteOverrides). game::call then goes through
        // the table, as the original's jal does: a direct call would skip
        // the hook (zgehand6's beginCameraFrame skipped the widescreen hooks
        // on updateProjection and updateFrustum).
        PS2Machine::RecompiledFunction installedSlot = nullptr;
        bool hooked = false;
    };

    // Every registered function of every game, in (game, address) order.
    const std::vector<Function *> &functions();
    // The function at `address` in `game`.
    Function *find(uint32_t address, Game game = Game::Ai);
    // By name or 0xaddress in `game`; a `<game>:` prefix (dm2:ADDRESS,
    // dm2:riderFrame) names another game's.
    Function *find(const std::string &nameOrAddress, Game game = Game::Ai);
    Function *findHost(const void *host);
    // Whether game::call or game::tailCall names this host function somewhere
    // in the build: only its signature's arguments reach it there, so a
    // register it leaves for a callee must be one a call passes (zx-fncheck
    // holds it to that: entry_left).
    bool noteCalledDirectly(const void *host);
    bool calledDirectly(const void *host);
    // Whether the readable version of the function at `address` runs in the
    // game (set by install()); for overrides that depend on one.
    // (One-argument forms, AI's: the AI override declares them itself.)
    bool runsInGame(uint32_t address, Game game);
    bool runsInGame(uint32_t address);
    // Whether install() will switch the readable version of the function at
    // `address` in `game` on: its status's default, then ZX_READABLE (as
    // install() reads it). Asked before the game starts (a front end's row
    // that needs the function); changes nothing.
    bool switchedOnAtBoot(uint32_t address, Game game);
    // The game install() was called for (AI's until then).
    Game runningGame();

    struct Registration
    {
        Registration(uint32_t address, const char *name, const char *file, Status status,
                     PS2Machine::RecompiledFunction readable, ee::ReturnKind returnKind, const void *host,
                     bool waits = false, bool leavesVu0 = false, Game game = Game::Ai);
        Function function;
    };

    // The traced original at `address` (null if this build has none): the
    // fncheck build of a recompiled function registers itself with
    // TracedRegistration (ee/fncheck_trace.h).
    PS2Machine::RecompiledFunction tracedOriginal(uint32_t address, Game game = Game::Ai);

    // Which functions ZX_READABLE switches on (see above), among `game`'s.
    // A library function the game links that the runtime replaces with host code (ps2_stubs) which neither
    // waits nor calls the game: memcpy, memset, the malloc family (sce/libc.cpp). A readable function running
    // on a fiber calls one there, as it calls a readable callee, instead of leaving its fiber for the host stack
    // (ee/suspend.cpp). Registered with READABLE_LIBRARY_LEAF(sym::name) where the library is bound.
    struct LibraryLeaf
    {
        LibraryLeaf(uint32_t address, Game game);
    };

    // Marks the running game's enabled functions whose table slot an override
    // has wrapped since install(), so game::call stops going around it.
    // ps2_game_overrides::applyMatching calls it after every module.
    void noteOverrides(PS2Machine &runtime);

    // Applies to the registry's enabled flags; returns false on an unknown
    // name.
    bool applySwitch(const std::string &spec, std::string *error = nullptr, Game game = Game::Ai);

    // Installs the running game's switched-on readable functions over the
    // recompiled ones (every other game's are off) and, with ZX_FNREC and
    // ZX_FNPROF, the call recorder and the profiler. Call before any other
    // override looks functions up, so wrappers wrap the readable version.
    void install(PS2Machine &runtime, Game game);
    void install(PS2Machine &runtime); // AI's (the AI override declares it itself)
}

namespace game::profile
{
    // ZX_FNPROF=<file.csv>: calls and time per game function (profile.cpp).
    void install(PS2Machine &runtime);
    // ZX_TAILCALL_STATS=<file.csv>: the tail calls generated code made to
    // a replaced function, by site (tailcall_stats.cpp).
    void installTailCallStats(PS2Machine &runtime);
    // ZX_FNCYCLES=<addr>[,<addr>...]:<file>: EE cycles of a few functions,
    // call by call (cycletrace.cpp).
    void installCycleTrace(PS2Machine &runtime);
    // Its view of readable functions called directly (game::call and
    // game::tailCall: no dispatch table to wrap): begin returns a token for
    // end (0: not traced), called after the dispatch the caller charges.
    // Null unless ZX_FNCYCLES is set.
    struct DirectCallTrace
    {
        uint64_t (*begin)(uint32_t address);
        void (*end)(uint64_t token);
    };
#if ZX_DEV_CHECKS
    inline const DirectCallTrace *g_directCallTrace = nullptr;
#else
    inline constexpr const DirectCallTrace *g_directCallTrace = nullptr; // (the app's flavour: no ZX_FNCYCLES)
#endif
}

namespace game
{
    namespace detail
    {
        // A readable callee called directly, as the original's jal: the
        // dispatch (8 EE cycles, a safe point) and, if its frame unwound,
        // the return through the scheduler (ee::enterDirectCall). ZX_FNCYCLES
        // sees it when it traces the address, from after the dispatch (and
        // its resume, if a checkpoint was due there) to after the return, as
        // a table call's wrapper does.
        template <auto Fn, typename... A>
        auto directAt(uint32_t address, A... args)
        {
            using R = typename ee::Signature<decltype(Fn)>::Return;
            // The call's registers: what the original holds in its callee-saved
            // ones here, and the callee's arguments at its entry (ee/held.h).
            ee::CallRegs regs;
            regs.target = address;
            (ee::detail::registerArg(regs, args), ...);
            // The dispatch, and its frame below the caller's, its saves written (ee::enterFrame); a hand-written
            // caller publishes no return address, so the site's is (ee::enterDirectCallee).
            ee::DirectCallScope scope;
            ee::enterDirectCallee(scope, address, regs, ee::layoutOf<Fn>());
            const profile::DirectCallTrace *trace = profile::g_directCallTrace;
            const uint64_t token = trace ? trace->begin(address) : 0u;
            if constexpr (std::is_void_v<R>)
            {
                Fn(args...);
                ee::leaveDirectCallee(scope, 0u, 0u, 0u);
                if (token)
                    trace->end(token);
            }
            else
            {
                R result = Fn(args...);
                using Result = ee::Abi<std::remove_cvref_t<R>>;
                if constexpr (Result::isFloat)
                    ee::leaveDirectCallee(scope, 0u, std::bit_cast<uint32_t>(static_cast<float>(Result::toReg(result))), 4u);
                else
                    ee::leaveDirectCallee(scope, static_cast<uint64_t>(Result::toReg(result)), 0u, 1u);
                if (token)
                    trace->end(token);
                return result;
            }
        }

        // A function that calls itself, under zx-fncheck: the body runs here, a call the recording does not hold
        // (CallHandler::callsItself).
        template <typename F, typename... A>
        auto itselfThrough(F body, A... args)
        {
            using R = typename ee::Signature<F>::Return;
            if constexpr (std::is_void_v<R>)
                ee::selfCall([&] { body(args...); });
            else
            {
                std::remove_cvref_t<R> result{};
                ee::selfCall([&] { result = body(args...); });
                return result;
            }
        }
        template <auto Fn, typename... A>
        auto itself(A... args)
        {
            return itselfThrough(Fn, args...);
        }

        // directAt for a callee known by its host function (its registered
        // address, for the trace).
        template <auto Fn, typename... A>
        auto direct(A... args)
        {
            static const readable::Function *const target = readable::findHost(reinterpret_cast<const void *>(Fn));
            return directAt<Fn>(target ? target->address : 0u, args...);
        }

        // A readable callee of a tail call, directly: the callee's own
        // safe-point mode, no dispatch; seen by ZX_FNCYCLES as generated
        // code's tail call through the table is (PS2Machine::tailCall).
        template <auto Fn, typename... A>
        auto directTail(uint32_t address, A... args)
        {
            using R = typename ee::Signature<decltype(Fn)>::Return;
            (void)ee::enterDirectCall(true);
            // At the caller's sp: the original's epilogue released its frame.
            // (Its arguments at entry for the held-register table: ee/held.h.)
            ee::CallRegs regs;
            regs.target = address;
            (ee::detail::registerArg(regs, args), ...);
            ee::t_ee.directArguments = &regs;
            ee::FunctionScope scope;
            ee::enterDirectFrame(scope, ee::layoutOf<Fn>(), true);
            ee::t_ee.directArguments = nullptr;
            const profile::DirectCallTrace *trace = profile::g_directCallTrace;
            const uint64_t token = trace ? trace->begin(address) : 0u;
            if constexpr (std::is_void_v<R>)
            {
                Fn(args...);
                ee::leaveDirectFrame(scope);
                if (token)
                    trace->end(token);
            }
            else
            {
                R result = Fn(args...);
                ee::leaveDirectFrame(scope);
                if (token)
                    trace->end(token);
                return result;
            }
        }

        // Under zx-fncheck, a tail call that is a jump inside the function replayed (CallHandler::jumpsInside):
        // generated code runs it as a goto, so the callee's body runs here, at the caller's sp with its own frame
        // row (directTail), and its calls are answered from the caller's recording.
        inline bool jumpsInside(uint32_t address)
        {
            return ee::t_ee.callHandler && ee::t_ee.callHandler->jumpsInside(address);
        }
    }

    // Calls another readable function: directly when it is on and no override
    // hooks it, else the game function at its address (the recompiled one, or
    // the hook around either). Under zx-fncheck
    // the call is answered from the recording like any other, so a
    // function is checked on its own; as a direct call (CallRegs::direct):
    // only the arguments the callee's signature names reach it, so one its
    // original reads that the call leaves out is noted (direct_left).
    namespace detail
    {
        template <auto Fn>
        inline const bool g_calledDirectly = readable::noteCalledDirectly(reinterpret_cast<const void *>(Fn));
    }

    template <auto Fn, typename... A>
    auto tailCall(A... args);
    template <auto Fn, uint32_t Address, typename... A>
    auto tailCallAt(A... args);

    template <auto Fn, typename... A>
    auto call(A... args)
    {
        using R = typename ee::Signature<decltype(Fn)>::Return;
        (void)detail::g_calledDirectly<Fn>;
        if (ee::takeTailNext()) // (made by game::tail: the original's tail jump)
            return tailCall<Fn>(args...);
        static const readable::Function *const target = readable::findHost(reinterpret_cast<const void *>(Fn));
        if (target && ee::t_ee.callHandler && ee::t_ee.callHandler->callsItself(target->address))
            return detail::itself<Fn>(args...);
        if (target && target->enabled && !target->hooked && !ee::t_ee.callHandler)
            return detail::directAt<Fn>(target->address, args...);
        return ee::detail::callWith<R>(target ? target->address : 0u, false, true, args...);
    }

    // A tail call of another readable function (ee::tailCall): directly
    // when it is on, without the dispatch's charge.
    template <auto Fn, typename... A>
    auto tailCall(A... args)
    {
        using R = typename ee::Signature<decltype(Fn)>::Return;
        (void)detail::g_calledDirectly<Fn>;
        (void)ee::takeTailNext();
        static const readable::Function *const target = readable::findHost(reinterpret_cast<const void *>(Fn));
        if (target && detail::jumpsInside(target->address))
            return detail::directTail<Fn>(target->address, args...);
        if (target && target->enabled && !target->hooked && !ee::t_ee.callHandler)
            return detail::directTail<Fn>(target->address, args...);
        return ee::detail::callWith<R>(target ? target->address : 0u, true, true, args...);
    }
    template <auto Fn, uint32_t Address, typename... A>
    auto tailCallAt(A... args)
    {
        using R = typename ee::Signature<decltype(Fn)>::Return;
        (void)detail::g_calledDirectly<Fn>;
        (void)ee::takeTailNext();
        static const readable::Function *const target = readable::find(Address);
        if (target && detail::jumpsInside(Address))
            return detail::directTail<Fn>(Address, args...);
        if (target && target->enabled && !target->hooked && !ee::t_ee.callHandler)
            return detail::directTail<Fn>(Address, args...);
        return ee::detail::callWith<R>(Address, true, true, args...);
    }

    // The same for a function registered at more than one address with
    // identical code: a linker that folds identical functions (lld-link's
    // ICF on Windows) gives them one host address, which game::call cannot
    // tell apart (it takes the first registration). The address says which.
    template <auto Fn, uint32_t Address, typename... A>
    auto callAt(A... args)
    {
        using R = typename ee::Signature<decltype(Fn)>::Return;
        (void)detail::g_calledDirectly<Fn>;
        if (ee::takeTailNext())
            return tailCallAt<Fn, Address>(args...);
        static const readable::Function *const target = readable::find(Address);
        if (ee::t_ee.callHandler && ee::t_ee.callHandler->callsItself(Address))
            return detail::itself<Fn>(args...);
        if (target && target->enabled && !target->hooked && !ee::t_ee.callHandler)
            return detail::directAt<Fn>(Address, args...);
        return ee::detail::callWith<R>(Address, false, true, args...);
    }
}

// A game function's body and the name it is called by. Game code calls a
// function by its plain name; that name has to do what game::call does, so
// hooks, the replay's answers and a function switched off are all kept, and
// C++ has one name for one signature. So the definition carries a trailing
// underscore and GAME_CALL gives the plain name:
//
//     void orthonormalize_(ee::Ptr<Vec4> forward, ee::Ptr<Vec4> right, ee::Ptr<const Vec4> up) { ... }
//     READABLE_FUNCTION(sym::orthonormalize, orthonormalize_, Verified);
//     GAME_CALL(orthonormalize);
//
//     orthonormalize(right, up, forward);
//
// The function's name is `orthonormalize` everywhere else (ZX_READABLE,
// the tables): a registration drops the underscore. Arguments go as they are
// given, an ee::Local, an object in game memory and an ee::Ptr as the address
// they are. A body whose callees can wait gets its fiber as a call through
// the table does, when the caller has none.
namespace game::detail
{
    template <typename T>
    struct IsGuestPointer : std::false_type {};
    template <typename T>
    struct IsGuestPointer<ee::Ptr<T>> : std::true_type {};

    template <typename T>
    inline constexpr bool isFloating = std::is_floating_point_v<T> || std::is_same_v<T, ee::Float>;

    // An argument as the parameter it goes to takes it: a guest address is
    // the same thing as an ee::Ptr, a host pointer or an integer here, as it
    // is in the register a call through the table sets.
    template <typename P, typename A>
    decltype(auto) handOn(const A &value)
    {
        using V = std::remove_cvref_t<A>;
        using Q = std::remove_cvref_t<P>;
        constexpr bool toGuest = IsGuestPointer<Q>::value || std::is_integral_v<Q>;
        constexpr bool toHost = std::is_pointer_v<Q>;
        static_assert(isFloating<Q> == isFloating<V> || !std::is_arithmetic_v<V>, "a float goes in another register than an integer");
        if constexpr (requires { value.address(); }) // an ee::Local
        {
            if constexpr (toGuest)
                return Q(value.address());
            else if constexpr (toHost)
                return Q(ee::pointerAt<std::remove_pointer_t<Q>>(value.address()));
            else
                return *value;
        }
        else if constexpr (requires { value.address; }) // an ee::Ptr
        {
            if constexpr (toGuest)
                return Q(value.address);
            else if constexpr (toHost)
                return Q(ee::pointerAt<std::remove_pointer_t<Q>>(value.address));
            else
                return value;
        }
        else if constexpr (std::is_pointer_v<V>)
        {
            if constexpr (toGuest)
                return Q(ee::addressOf(value));
            else
                return Q(value);
        }
        else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>)
        {
            if constexpr (IsGuestPointer<Q>::value)
                return Q(static_cast<uint32_t>(value));
            else if constexpr (toHost)
                return Q(ee::pointerAt<std::remove_pointer_t<Q>>(static_cast<uint32_t>(value)));
            else if constexpr (std::is_enum_v<Q>)
                return static_cast<Q>(value);
            else
                return value;
        }
        else if constexpr (toGuest)
            return Q(ee::addressOf(&value)); // an object in game memory
        else if constexpr (toHost)
            return Q(&value);
        else
            return value;
    }

    // An argument as a call through the table sets its register: an address
    // stays the address it is (a host pointer would lose the uncached mirror
    // it was reached through).
    template <typename P, typename A>
    decltype(auto) inRegister(const A &value)
    {
        using Q = std::remove_cvref_t<P>;
        if constexpr (std::is_pointer_v<Q> && requires { value.address(); })
            return value.address();
        else if constexpr (std::is_pointer_v<Q> && requires { value.address; })
            return value.address;
        else if constexpr (std::is_pointer_v<Q> && std::is_integral_v<A>)
            return static_cast<uint32_t>(value);
        else
            return handOn<P>(value);
    }

    template <auto Fn>
    struct Route;
    template <typename R, typename... P, R (*Fn)(P...)>
    struct Route<Fn>
    {
        template <typename... A>
        static std::remove_cvref_t<R> call(const A &...args)
        {
            static const readable::Function *const target = readable::findHost(reinterpret_cast<const void *>(Fn));
            return through(target, args...);
        }

        // The entry of a function a plain source defines (readable_entries.py): its registration found by its
        // address, which a linker folding identical bodies (lld-link's ICF) cannot confuse.
        template <uint32_t Address, ::game::Game G, typename... A>
        static std::remove_cvref_t<R> at(const A &...args)
        {
            static const readable::Function *const target = readable::find(Address, G);
            return through(target, args...);
        }

        // The registration at an entry's address may be another function's: in a game's instance two shared
        // functions can bind to one address there (in DM2 sceneHook, level 2, and endCameraSetup, level
        // 1: DM2's code is sceneHook's). Fn runs directly only when the registration found is its own; else the
        // call goes through the table to what is installed at the address, as the original's jal does.
        template <typename... A>
        static std::remove_cvref_t<R> through(const readable::Function *target, const A &...args)
        {
            using Result = std::remove_cvref_t<R>;
            (void)g_calledDirectly<Fn>;
            const uint32_t address = target ? target->address : 0u;
            const bool own = target && target->host == reinterpret_cast<const void *>(Fn);
            if (ee::takeTailNext()) // game::tail: as game::tailCall<Fn>
            {
                if (own && jumpsInside(address))
                    return directTail<Fn>(address, handOn<P>(args)...);
                if (own && target->enabled && !target->hooked && !ee::t_ee.callHandler)
                    return directTail<Fn>(address, handOn<P>(args)...);
                return ee::detail::callWith<Result>(address, true, true, inRegister<P>(args)...);
            }
            if (target && ee::t_ee.callHandler && ee::t_ee.callHandler->callsItself(address))
            {
                if (own)
                    return itself<Fn>(handOn<P>(args)...);
                // The registration there is another body of the same type: a version's own body (READABLE_FUNCTION_OWN)
                // calling its shared name, as DM2's instanceNodeOnItsObject calls instanceGeometryNode for a child.
                // The original's call is to itself, which generated code runs inline: so does the replay.
                if (target->host && target->signature == readable::signatureOf<decltype(Fn)>())
                    return itselfThrough(reinterpret_cast<decltype(Fn)>(const_cast<void *>(target->host)), handOn<P>(args)...);
            }
            if (target && target->waits && !ee::t_ee.suspendable && !ee::t_ee.callHandler)
                return ee::detail::callWith<Result>(address, false, false, inRegister<P>(args)...);
            if (own && target->enabled && !target->hooked && !ee::t_ee.callHandler)
                return directAt<Fn>(address, handOn<P>(args)...);
            return ee::detail::callWith<Result>(address, false, true, inRegister<P>(args)...);
        }
    };
}

// A call the original makes as its tail call (a `j` to the function, not a `jal`), of a
// function a plain source defines (readable_entries.py makes its entry): `return game::tail(vec3Lerp, out, a, b,
// t);` is game::tailCall<&vec3Lerp_>(out, a, b, t).
//
// A variadic callee (formatString: its words declared `ee::VarArg first = {}`) is called with the words its format
// reads, as a plain call names them: the words left out are passed left out, as their defaults are (a reference to
// the function does not carry its default arguments). A name with overloads gives the template no one function to
// take: the callee's type is then named, `game::tail<zge::FormatString>(zge::formatString, out, format, inches)`.
namespace ee
{
    class VarArg;
}

namespace game
{
    namespace detail
    {
        template <typename F>
        struct TailParameters
        {
            using Types = std::tuple<>;
        };
        template <typename R, typename... P>
        struct TailParameters<R(P...)>
        {
            using Types = std::tuple<P...>;
        };
        template <typename R, typename... P>
        struct TailParameters<R(P...) noexcept> : TailParameters<R(P...)>
        {
        };

        // The callee called with the arguments given and, after them, its variadic words the call leaves out.
        template <typename F, std::size_t... Left, typename... A>
        decltype(auto) tailLeavingWordsOut(F &function, std::index_sequence<Left...>, A &&...args)
        {
            using Types = typename TailParameters<F>::Types;
            static_assert((std::is_same_v<std::tuple_element_t<sizeof...(A) + Left, Types>, ee::VarArg> && ...),
                          "game::tail: a parameter the call leaves out is a variadic word (ee::VarArg) only");
            return function(std::forward<A>(args)..., std::tuple_element_t<sizeof...(A) + Left, Types>{}...);
        }
    }

    namespace detail
    {
        // game::tail's mark, cleared when its callee returns if no call took it (ee::t_ee.tailNext).
        struct TailMark
        {
            TailMark() { ee::t_ee.tailNext = true; }
            ~TailMark();
            TailMark(const TailMark &) = delete;
            TailMark &operator=(const TailMark &) = delete;
        };
    }

    template <typename F, typename... A>
    decltype(auto) tail(F &function, A &&...args)
    {
        constexpr std::size_t kParameters = std::tuple_size_v<typename detail::TailParameters<F>::Types>;
        const detail::TailMark mark;
        if constexpr (sizeof...(A) < kParameters)
            return detail::tailLeavingWordsOut(function, std::make_index_sequence<kParameters - sizeof...(A)>{},
                                               std::forward<A>(args)...);
        else
            return function(std::forward<A>(args)...);
    }
}

#define GAME_CALL(NAME)                                                                                            \
    template <typename... A>                                                                                       \
    inline auto NAME(const A &...args)                                                                             \
    {                                                                                                              \
        return ::game::detail::Route<&NAME##_>::call(args...);                                                     \
    }

#define READABLE_JOIN2(a, b) a##b
#define READABLE_JOIN(a, b) READABLE_JOIN2(a, b)

namespace game::readable
{
    // A registration's file as the repository names it (engine/zge/render/draw.cpp, games/dm2/rider/rider.cpp), the
    // same on every host and in every checkout: the source's own path from its readable folder's top-level folder on
    // (engine/zge, engine/host, ps2/ee, ps2/sce, games/<game>: tools/checks/zlayout.py READABLE), with '\' written '/',
    // made at compile time, so the binary holds only that part (never the folder the checkout is in). A path with
    // none of those folders in it keeps its file name alone.
    template <std::size_t N>
    struct SourcePath
    {
        char text[N] {};

        consteval SourcePath(const char (&path)[N])
        {
            constexpr const char *kFolders[] = {"engine/zge/", "engine/host/", "ps2/ee/", "ps2/sce/",
                                                "games/aggressive/", "games/dm2/", "games/bmx/"};
            const auto separator = [](char c) { return c == '/' || c == '\\'; };
            std::size_t length = 0;
            while (length < N && path[length] != '\0')
                ++length;
            std::size_t from = 0;
            bool found = false;
            for (std::size_t i = 0; i < length; ++i)
            {
                if (i != 0 && !separator(path[i - 1]))
                    continue;
                for (const char *folder : kFolders)
                {
                    bool same = true;
                    std::size_t k = 0;
                    for (; folder[k] != '\0' && same; ++k)
                        same = i + k < length && (folder[k] == '/' ? separator(path[i + k]) : path[i + k] == folder[k]);
                    if (same)
                    {
                        from = i;   // the last one: a checkout inside another's games/ is named by its own
                        found = true;
                    }
                }
            }
            if (!found)
                for (std::size_t i = 0; i < length; ++i)
                    if (separator(path[i]))
                        from = i + 1;
            for (std::size_t i = from; i < length; ++i)
                text[i - from] = separator(path[i]) ? '/' : path[i];
        }
    };

    template <SourcePath Path>
    inline constexpr const char *sourcePath = Path.text;
}

// The file a registration names (Function::file): sourcePath of the file it is written in.
#define READABLE_SOURCE_FILE (::game::readable::sourcePath<__FILE__>)

// Registers fn (a C++ function with the game function's signature) as the
// readable version of the function at ADDRESS. STATUS: Draft, Tested or
// Verified.
// (READABLE_FUNCTION and its _WAITS and _VU0 forms: at the end of this file.)

// A function to be ported: registered for recording only. In a game's
// instance it is that game's, and one the game has not got (its table gives
// the symbol no row: bindPlanned, below) registers nothing.
#if defined(ZX_GAME_INSTANCE)
#define READABLE_PLANNED(ADDRESS, NAME)                                                                             \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bindPlanned<(ADDRESS)>(NAME, READABLE_SOURCE_FILE)
#else
#define READABLE_PLANNED(ADDRESS, NAME)                                                                             \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), NAME, READABLE_SOURCE_FILE, ::game::readable::Status::Planned, nullptr, ::ee::ReturnKind::Void, nullptr)
#endif

// The same for another game's function (GAME: dm2; ee/game_id.h):
// READABLE_PLANNED_IN(dm2, ADDRESS, "riderFrame"). game.cmake builds its
// traced original from that game's generated code.
#define READABLE_PLANNED_IN(GAME, ADDRESS, NAME)                                                                    \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), NAME, READABLE_SOURCE_FILE, ::game::readable::Status::Planned, nullptr, ::ee::ReturnKind::Void, nullptr,    \
        false, false, ::game::games::GAME)

// A readable function of another game (READABLE_FUNCTION's arguments after
// the game).
#define READABLE_FUNCTION_IN(GAME, ADDRESS, FN, STATUS)                                                            \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entry<&FN>,                            \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN), false, false,             \
        ::game::games::GAME)

// Registrations by name (tools/readable/readable_symbols.py, docs/READABLE.md "Symbols"):
// READABLE_FUNCTION(fn, Verified), the address is the symbol's;
// READABLE_FUNCTION(sym::name, fn, Verified) when the symbol is not the C++ name.
// A source built for another game (a per-game instance, game.cmake:
// ZX_GAME_INSTANCE) has that game's level and status from its generated
// header: 2 its body runs there, 1 only the address is known (registered
// Planned, so game::call<&fn> reaches the recompiled function), 0 no such
// function there.
namespace game::readable
{
    // The game a source is built for: AI's, unless it is a per-game instance
    // (game.cmake compiles a shared source again with ZX_GAME_INSTANCE set,
    // its symbols at that game's addresses and zge and sce renamed, so every
    // function and constant of it is that game's own).
#if defined(ZX_GAME_INSTANCE)
    inline constexpr Game kInstanceGame = ::game::games::ZX_GAME_INSTANCE;
#else
    inline constexpr Game kInstanceGame = Game::Ai;
#endif

    // A small per-game difference in a shared body: this game's value of a constant.
    template <typename T>
    constexpr T perGame(T ai, T dm2)
    {
        return kInstanceGame == Game::Dm2 ? dm2 : ai;
    }
    inline constexpr bool kIsDm2 = kInstanceGame == Game::Dm2;
    // The same where BMX XXX has a value of its own.
    template <typename T>
    constexpr T perGame(T ai, T dm2, T bmx)
    {
        return kInstanceGame == Game::Bmx ? bmx : perGame(ai, dm2);
    }
    inline constexpr bool kIsBmx = kInstanceGame == Game::Bmx;

    // A per-game instance's registration.
    struct Bound
    {
    };
    template <auto Fn, uint32_t Address, int Level, Status S, bool Waits, bool Vu0>
    Bound bind(const char *name, const char *file)
    {
        if constexpr (Level == 2)
        {
            static Registration registration(Address, name, file, S, Waits ? &ee::entrySuspendable<Fn> : &ee::entry<Fn>,
                                             ee::Signature<decltype(Fn)>::returnKind, reinterpret_cast<const void *>(Fn),
                                             Waits, Vu0, kInstanceGame);
            registration.function.signature = signatureOf<decltype(Fn)>();
        }
        else if constexpr (Level == 1)
        {
            static Registration registration(Address, name, file, Status::Planned, nullptr, ee::ReturnKind::Void,
                                             reinterpret_cast<const void *>(Fn), false, false, kInstanceGame);
        }
        return {};
    }

    // The value a game's symbol table gives a symbol the game has no row for
    // (tools/readable/readable_symbols.py UNBOUND): a planned function at it is one
    // this engine version has not got, and its instance registers nothing.
    constexpr uint32_t kUnboundSymbol = 0xDEAD0000u; // not an address
    constexpr bool symbolBound(uint32_t address) { return (address & 0xFFFF0000u) != kUnboundSymbol; }
    template <uint32_t Address>
    Bound bindPlanned(const char *name, const char *file)
    {
        if constexpr (symbolBound(Address))
        {
            static Registration registration(Address, name, file, Status::Planned, nullptr, ee::ReturnKind::Void, nullptr,
                                             false, false, kInstanceGame);
        }
        return {};
    }
}

#if defined(ZX_GAME_INSTANCE)
// (sym::name, fn, status): the address, level and status the name has in this
// game; in the lambda `sym` stands for the table of levels or of statuses.
#define READABLE_LEVEL_OF(SYMBOL) ([] { namespace sym = ::sym::level; return SYMBOL; }())
#define READABLE_STATUS_OF(SYMBOL) ([] { namespace sym = ::sym::status; return SYMBOL; }())
#define READABLE_BIND_AT(SYMBOL, FN, WAITS, VU0)                                                                   \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<&FN, SYMBOL, READABLE_LEVEL_OF(SYMBOL), READABLE_STATUS_OF(SYMBOL), WAITS, VU0>(#FN, READABLE_SOURCE_FILE)
#define READABLE_FUNCTION_AT(SYMBOL, FN, STATUS) READABLE_BIND_AT(SYMBOL, FN, false, false)
#define READABLE_FUNCTION_WAITS_AT(SYMBOL, FN, STATUS) READABLE_BIND_AT(SYMBOL, FN, true, false)
#define READABLE_FUNCTION_VU0_AT(SYMBOL, FN, STATUS) READABLE_BIND_AT(SYMBOL, FN, false, true)
#else
#define READABLE_FUNCTION_AT(ADDRESS, FN, STATUS)                                                                  \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entry<&FN>,                            \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN))
#define READABLE_FUNCTION_WAITS_AT(ADDRESS, FN, STATUS)                                                            \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entrySuspendable<&FN>,                  \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN), true)
#define READABLE_FUNCTION_VU0_AT(ADDRESS, FN, STATUS)                                                              \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entry<&FN>,                            \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN), false, true)
#endif

#define READABLE_LIBRARY_LEAF(SYMBOL)                                                                              \
    static const ::game::readable::LibraryLeaf READABLE_JOIN(g_readableLibraryLeaf_, __LINE__)(                    \
        (SYMBOL), ::game::readable::kInstanceGame)

#define READABLE_PICK(_1, _2, _3, NAME, ...) NAME
#if defined(ZX_GAME_INSTANCE)
#define READABLE_NAMED(FN, WAITS, VU0)                                                                             \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<&FN, sym::FN, sym::level::FN, sym::status::FN, WAITS, VU0>(#FN, READABLE_SOURCE_FILE)
#define READABLE_FUNCTION_NAMED(FN, STATUS) READABLE_NAMED(FN, false, false)
#define READABLE_FUNCTION_WAITS_NAMED(FN, STATUS) READABLE_NAMED(FN, true, false)
#define READABLE_FUNCTION_VU0_NAMED(FN, STATUS) READABLE_NAMED(FN, false, true)
#else
#define READABLE_FUNCTION_NAMED(FN, STATUS) READABLE_FUNCTION_AT(sym::FN, FN, STATUS)
#define READABLE_FUNCTION_WAITS_NAMED(FN, STATUS) READABLE_FUNCTION_WAITS_AT(sym::FN, FN, STATUS)
#define READABLE_FUNCTION_VU0_NAMED(FN, STATUS) READABLE_FUNCTION_VU0_AT(sym::FN, FN, STATUS)
#endif
#define READABLE_FUNCTION(...) READABLE_PICK(__VA_ARGS__, READABLE_FUNCTION_AT, READABLE_FUNCTION_NAMED, )(__VA_ARGS__)
#define READABLE_FUNCTION_WAITS(...)                                                                               \
    READABLE_PICK(__VA_ARGS__, READABLE_FUNCTION_WAITS_AT, READABLE_FUNCTION_WAITS_NAMED, )(__VA_ARGS__)
#define READABLE_FUNCTION_VU0(...)                                                                                 \
    READABLE_PICK(__VA_ARGS__, READABLE_FUNCTION_VU0_AT, READABLE_FUNCTION_VU0_NAMED, )(__VA_ARGS__)
