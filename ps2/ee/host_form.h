#pragma once

// Host forms: an engine function as the host platform runs it, in place of
// the PS2 one. The engine's own source tree keeps a platform's code in a
// folder of its own (ZGE's ps2/ beside XBox/, the Xbox build's file device
// XBoxFile among it); the host's is zge/host, beside the PS2 forms the
// readable game proves against the original (zge/assets/dvd_device.cpp: the
// CD/DVD client over the IOP).
//
// A host form is ordinary readable C++ with the game function's signature,
// registered next to it with the function it stands for and its system:
//
//     uint32_t openOnHost(ee::Ptr<ZFile> file, ee::Ptr<const char> path, ...);
//     HOST_FORM(disc, sym::dvdOpen, openOnHost);
//
// A form of a function the game's engine version has not got (its symbol
// unbound in that game: BMX XXX's setAudioDecVolume) registers nothing there.
// install(runtime, game, "disc") puts every form of the running game's
// system over the game's function (PS2Machine::replaceFunction, after the
// readable install, so it stands in place of the PS2 form there; a form can
// still hand a call on to it: continueInPs2Form). The
// system's own switch calls it (the disc's: [host] disc = native, the
// default), never ZX_READABLE, so a run with the readable game off runs
// the same host forms as one with it on. A form is not recorded or replayed:
// its proof is the run's (the files it served and the guest state after).
//
// What a host form leaves for after its return (a completion block's
// callback, its caller's wait as for an RPC's reply, a switch to a thread it
// made ready) is the host service's (engine/host/disc_files.h): the form
// returns its result as any function does, and the work runs once the
// caller's context holds it.

#include "ee/abi.h"
#include "ee/game_id.h"
#include "ee/readable.h"

#include "machine.h"

#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>

namespace game::host
{
    // A function's host form, by its game.
    struct Form
    {
        Game game = Game::Ai;
        const char *system = "";
        uint32_t address = 0;
        const char *name = "";
        PS2Machine::RecompiledFunction entry = nullptr;
    };

    struct Registration
    {
        explicit Registration(const Form &form);
    };

    // Every host form of `system` the game has over its functions; false
    // (and none of them installed) if the game has none, or one of its
    // functions is not in the runtime.
    bool install(PS2Machine &runtime, Game game, const char *system);

    // The call of a host form in progress (its entry's): the guest memory,
    // the caller's context and the runtime, for the host service.
    struct Call
    {
        uint8_t *rdram = nullptr;
        R5900Context *ctx = nullptr;
        PS2Machine *runtime = nullptr;
    };
    const Call &currentCall();

    // Work for after the running form's return, with the result in the
    // caller's context and pc at its return address (it may switch threads:
    // it does not come back). One per call.
    using AfterReturn = std::function<void(const Call &call)>;
    void afterReturn(AfterReturn work);

    // The call goes on in the PS2 form of `function` (the one the running
    // form stands over, as install found it): after the form's return, from
    // the PS2 form's start with the caller's arguments and return address
    // as they were. For a form that changes only some of its calls.
    void continueInPs2Form(uint32_t function);

    namespace detail
    {
        struct CallScope
        {
            explicit CallScope(const Call &call);
            ~CallScope();
            Call outer;
        };
        // The work afterReturn left (an empty function if none), taken.
        AfterReturn takeAfterReturn();

        template <auto Fn, typename R, typename... P>
        void invoke(R (*)(P...), R5900Context *ctx)
        {
            int nextInt = 0, nextFloat = 0;
            std::tuple<std::decay_t<P>...> args{ee::detail::readArg<std::decay_t<P>>(ctx, nextInt, nextFloat)...};
            if constexpr (std::is_void_v<R>)
                std::apply(Fn, args);
            else
            {
                static_assert(sizeof(R) == 4 && std::is_integral_v<R>, "a host form returns a 32-bit word");
                // The word sign-extended into v0, its upper half clear (as a
                // runtime function's result: setReturnS32).
                setReturnS32(ctx, static_cast<int32_t>(std::apply(Fn, args)));
            }
        }
    }

    // The RecompiledFunction of a host form: its arguments from the
    // registers, its result to v0, back to the caller, then what it left
    // for after its return.
    template <auto Fn>
    void entry(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const uint32_t returnAddress = getRegU32(ctx, 31);
        {
            const detail::CallScope scope(Call{rdram, ctx, runtime});
            detail::invoke<Fn>(Fn, ctx);
        }
        ctx->pc = returnAddress;
        if (AfterReturn work = detail::takeAfterReturn())
            work(Call{rdram, ctx, runtime});
    }
}

#define HOST_FORM_JOIN2(a, b) a##b
#define HOST_FORM_JOIN(a, b) HOST_FORM_JOIN2(a, b)
// Registers FN as the host form of the function at SYMBOL (this game's: the
// source's instance), in SYSTEM's switch.
#define HOST_FORM(SYSTEM, SYMBOL, FN)                                                                              \
    static const ::game::host::Registration HOST_FORM_JOIN(g_hostForm_, __LINE__)(                                \
        ::game::host::Form{::game::readable::kInstanceGame, #SYSTEM, (SYMBOL), #FN, &::game::host::entry<&FN>})
