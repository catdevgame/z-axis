#pragma once

// READABLE_FUNCTION_WAITS_IN: another game's readable function whose callees
// can wait (READABLE_FUNCTION_WAITS for READABLE_FUNCTION_IN; DM2's bailState
// calls a sound function that waits). Like _WAITS, it runs on a fiber that
// leaves while a callee waits (ee::entrySuspendable, Function::waits).
//
//     READABLE_FUNCTION_WAITS_IN(dm2, ADDRESS, bailState, Draft);
//
// READABLE_FUNCTION_VU0_IN: another game's readable function whose callers
// read the VU0 state it leaves (READABLE_FUNCTION_VU0 for _IN; DM2's
// closestOnRail): zx-fncheck compares its VU0 registers, Q and flags.
//
//     READABLE_FUNCTION_VU0_IN(dm2, ADDRESS, closestOnRail, Draft);
//
// READABLE_FUNCTION_ONLY(GAME, FN): a function only one engine
// version has, in the shared file of the code around it (ZGE2's lengthSquared:
// a call where ZGE3 and ZGE4 square the vector inline). There is no AI
// function to bind it to: its symbol is the game's own row (sym::dm2::FN or
// sym::bmx::FN; readable_symbols.py add function FN ADDRESS --game dm2), and
// its body is defined `inline` in the same file (headers stops otherwise), so
// AI's build, which registers nothing for it, emits nothing of it: AI's
// objects are what they were. In GAME's instance it is bound as a shared
// function is, at the level headers gives it (2, or 1 when something it names
// is not bound there) and the status of game/<game>/symbols_status.tsv (Draft
// without a row; the table is its status's one place, as for a shared
// function: the form names none); another game's instance registers nothing. Callers call it by
// name, as any plain source's function (readable_entries.py gives it its entry
// at sym::GAME::FN, READABLE_ONLY_DEFINE below), from GAME's code only: a step
// under `if constexpr (game::readable::kIsDm2)`, or GAME's folder; another file
// declares it as any engine function is (in the header, not inline).
// It is not another game's registration (readable_layering's reg_in): the
// tools read it as GAME's READABLE_FUNCTION_IN, as they read a game folder's.
//
//     inline ee::Float lengthSquared(const Vec3 *v) { ... }
//     READABLE_FUNCTION_ONLY(dm2, lengthSquared);
//
// Kept out of ee/readable.h, which every traced original includes (a change
// there rebuilds all ~6,000 of them): fold them in at the next change there.
// game.cmake (the traced originals of another game) and readable_digest.py
// read them as READABLE_FUNCTION_IN.

#include "readable.h"

#if defined(ZX_GAME_INSTANCE)
#define READABLE_ONLY(GAME, FN, WAITS, VU0)                                                                        \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<&FN, sym::GAME::FN, sym::level::GAME::FN, sym::status::GAME::FN, WAITS, VU0>(#FN,   \
                                                                                                         READABLE_SOURCE_FILE)
#else
// (AI's build: nothing registered, and an unevaluated operand does not use the inline body)
#define READABLE_ONLY(GAME, FN, WAITS, VU0) static_assert(sizeof(&FN) != 0, #FN ": " #GAME " only")
#endif
// (a status word after FN, the form before tools19, is read by nothing: the table's is the status)
#define READABLE_FUNCTION_ONLY(GAME, FN, ...) READABLE_ONLY(GAME, FN, false, false)
#define READABLE_FUNCTION_WAITS_ONLY(GAME, FN, ...) READABLE_ONLY(GAME, FN, true, false)
#define READABLE_FUNCTION_VU0_ONLY(GAME, FN, ...) READABLE_ONLY(GAME, FN, false, true)
// READABLE_ONLY_AT(GAME, NAME, FN, ...): the symbol's name apart from the body's, for the build copy
// readable_entries.py makes (the body renamed NAME_, NAME defined inline as its entry at sym::GAME::NAME, so a
// plain call of it goes where the original's jal goes). Sources write the _ONLY forms, never _ONLY_AT.
#if defined(ZX_GAME_INSTANCE)
#define READABLE_ONLY_AT(GAME, NAME, FN, WAITS, VU0)                                                               \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<&FN, sym::GAME::NAME, sym::level::GAME::NAME, sym::status::GAME::NAME, WAITS, VU0>( \
            #NAME, READABLE_SOURCE_FILE)
#else
#define READABLE_ONLY_AT(GAME, NAME, FN, WAITS, VU0) static_assert(sizeof(&FN) != 0, #NAME ": " #GAME " only")
#endif
#define READABLE_FUNCTION_ONLY_AT(GAME, NAME, FN, ...) READABLE_ONLY_AT(GAME, NAME, FN, false, false)
#define READABLE_FUNCTION_WAITS_ONLY_AT(GAME, NAME, FN, ...) READABLE_ONLY_AT(GAME, NAME, FN, true, false)
#define READABLE_FUNCTION_VU0_ONLY_AT(GAME, NAME, FN, ...) READABLE_ONLY_AT(GAME, NAME, FN, false, true)
// READABLE_ONLY_DEFINE(GAME, DEFINITION...): the _ONLY entry's definition (readable_entries.py's copy), an
// external function of GAME's instance (game::detail::Route at the game's own symbol), so a caller in any file
// of that instance calls it by name; another build has none, so AI's objects are what they were (an entry
// there would emit the body), and its callers (steps the build discards) need none.
#if ZX_GAME_DM2
#define READABLE_ONLY_DEFINE_dm2(...) __VA_ARGS__
#else
#define READABLE_ONLY_DEFINE_dm2(...)
#endif
#if ZX_GAME_BMX
#define READABLE_ONLY_DEFINE_bmx(...) __VA_ARGS__
#else
#define READABLE_ONLY_DEFINE_bmx(...)
#endif
#define READABLE_ONLY_DEFINE(GAME, ...) READABLE_ONLY_DEFINE_##GAME(__VA_ARGS__)

// Per-version signatures: one body, and each engine version's own words (ZGE4's colour writers take the colour's
// shift, which ZGE2 and ZGE3 have no parameter for and halve by; ZGE2's cloneScene takes the scene alone).
//
// READABLE_PARAM_ONLY(GAME, TYPE NAME = VALUE): a parameter only GAME's version takes, written straight after the
// parameter before it, with no comma between (the form holds its own). The other versions have no such parameter,
// and NAME is VALUE in their bodies: what that version does in its place. READABLE_PARAM_EXCEPT(GAME, ...): every
// version's but GAME's. A declaration (a header's) names the parameter without the value; a call passes the word
// with the same form, so the call is written once and each version passes its own words.
//
//     void skinnedColourWord(Packet packet, ee::Ptr<ZgePart> part, uint32_t vertex
//                            READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))
//     skinnedColourWord(packet, part, entry & kStripVertexMask READABLE_PARAM_ONLY(bmx, colourShift));
//     ee::Ptr<Scene> cloneScene(ee::Ptr<Scene> scene READABLE_PARAM_EXCEPT(dm2, uint32_t flags));     // scene.h
//
// READABLE_RESULT_ONLY(GAME, TYPE), READABLE_RESULT_EXCEPT(GAME, TYPE): a return type some versions have, void in
// the others; each return is `return READABLE_RETURN_ONLY(GAME, VALUE);` (_EXCEPT), the value where the version
// returns it and evaluated and dropped where it returns nothing.
//
//     READABLE_RESULT_ONLY(dm2, uint32_t) addRigidColours(..., uint32_t streams READABLE_PARAM_ONLY(bmx, ...))
//     return READABLE_RETURN_ONLY(dm2, vuAddress);
//
// GAME is ai, dm2 or bmx. Each instance compiles its own signature (the registration's ee::Signature, the entry's
// words, the checker's); in AI's build another game's form is no text at all, so AI's objects are what they were.
// readable_entries.py's copy of a definition writes the value as a constant at the top of the body in the versions
// without the parameter (READABLE_PARAM_ONLY_VALUE below), and the entry passes the parameter on. A difference the
// forms do not say (a first parameter one version has not, another order, other logic) is an own body (_OWN below):
// its parameter list is its own, its declaration a plain one, and its version's callers call it by its name (DM2's
// shapeBoundsOverlapBox, ZGE2's queryOverlapsBox with the shape and no matrix).
#if defined(ZX_GAME_INSTANCE)
#define READABLE_IN_ai(...)
#define READABLE_NOT_IN_ai(...) __VA_ARGS__
#else
#define READABLE_IN_ai(...) __VA_ARGS__
#define READABLE_NOT_IN_ai(...)
#endif
#if ZX_GAME_DM2
#define READABLE_IN_dm2(...) __VA_ARGS__
#define READABLE_NOT_IN_dm2(...)
#else
#define READABLE_IN_dm2(...)
#define READABLE_NOT_IN_dm2(...) __VA_ARGS__
#endif
#if ZX_GAME_BMX
#define READABLE_IN_bmx(...) __VA_ARGS__
#define READABLE_NOT_IN_bmx(...)
#else
#define READABLE_IN_bmx(...)
#define READABLE_NOT_IN_bmx(...) __VA_ARGS__
#endif
#define READABLE_PARAM_ONLY(GAME, ...) READABLE_IN_##GAME(, __VA_ARGS__)
#define READABLE_PARAM_EXCEPT(GAME, ...) READABLE_NOT_IN_##GAME(, __VA_ARGS__)
#define READABLE_RESULT_ONLY(GAME, ...) READABLE_IN_##GAME(__VA_ARGS__) READABLE_NOT_IN_##GAME(void)
#define READABLE_RESULT_EXCEPT(GAME, ...) READABLE_NOT_IN_##GAME(__VA_ARGS__) READABLE_IN_##GAME(void)
#define READABLE_RETURN_ONLY(GAME, ...) READABLE_IN_##GAME(__VA_ARGS__) READABLE_NOT_IN_##GAME(static_cast<void>(__VA_ARGS__))
#define READABLE_RETURN_EXCEPT(GAME, ...) READABLE_NOT_IN_##GAME(__VA_ARGS__) READABLE_IN_##GAME(static_cast<void>(__VA_ARGS__))
// READABLE_EXCEPT(GAME, REGISTRATION): a shared function's registration (READABLE_FUNCTION and its _WAITS and _VU0
// forms, as written) made in every build but GAME's instance, which registers nothing at the function's address:
// GAME's version is far from the shared one and its own body registers there (in game/GAME at GAME's own row, or an
// _ONLY function the bind's pair put the shared name on), so the instance has one registration at the address. The shared function keeps its entry in GAME's instance (a plain
// call of it goes to what GAME has at the address, as the original's jal does).
//
//     READABLE_EXCEPT(bmx, READABLE_FUNCTION(debugPrint, Verified)); // BMX XXX's: game/bmx/debug/debug_print.cpp
//
#define READABLE_EXCEPT(GAME, ...) READABLE_NOT_IN_##GAME(__VA_ARGS__)
// (readable_entries.py's copy, at the top of a definition's body: the parameter's value where the version has none.
// Sources write the forms above, never these.)
#define READABLE_PARAM_ONLY_VALUE(GAME, ...) READABLE_NOT_IN_##GAME([[maybe_unused]] constexpr __VA_ARGS__;)
#define READABLE_PARAM_EXCEPT_VALUE(GAME, ...) READABLE_IN_##GAME([[maybe_unused]] constexpr __VA_ARGS__;)

#define READABLE_FUNCTION_WAITS_IN(GAME, ADDRESS, FN, STATUS)                                                      \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entrySuspendable<&FN>,                 \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN), true, false,              \
        ::game::games::GAME)

#define READABLE_FUNCTION_VU0_IN(GAME, ADDRESS, FN, STATUS)                                                        \
    static ::game::readable::Registration READABLE_JOIN(g_readableRegistration_, __LINE__)(                      \
        (ADDRESS), #FN, READABLE_SOURCE_FILE, ::game::readable::Status::STATUS, &::ee::entry<&FN>,                            \
        ::ee::Signature<decltype(&FN)>::returnKind, reinterpret_cast<const void *>(&FN), false, true,              \
        ::game::games::GAME)

// READABLE_FUNCTION_OWN(FN, STATUS, GAME, OWN[, GAME2, OWN2]): the shared function
// FN whose body in GAME's instance is OWN, that version's own implementation
// (different logic: the same file, named for what it does), and in GAME2's OWN2
// when the other version has one too. AI's build registers FN as
// READABLE_FUNCTION(FN, STATUS) does, and so does an instance with no own body;
// GAME's binds OWN at FN's symbol. One registration per symbol: the level is the
// symbol's (readable_symbols.py headers gives GAME's instance the own body's,
// "own body OWN"; readable_layering counts each own body as own_body). _WAITS and
// _VU0 as for READABLE_FUNCTION. OWN is GAME's alone, as an _ONLY function is:
// readable_entries.py's copy defines its body READABLE_OWN_INLINE(GAME) (inline
// in every other build, AI's too, so none emits anything of it; in GAME's as
// written) and its entry with READABLE_ONLY_DEFINE(GAME, ...), so GAME's code
// calls it by name and another game's has no such function (a call of it there
// is a step that build discards). Its parameter list is its own.
//
//     READABLE_FUNCTION_WAITS_OWN(newModelPose, Verified, bmx, newModelPoseWithCapacity);
//     READABLE_FUNCTION_WAITS_OWN(drawScene, Verified, dm2, drawSceneBySector, bmx, drawSceneByNodeTrees);
//
#define READABLE_OWN_INLINE(GAME) READABLE_NOT_IN_##GAME(inline)
namespace game::readable
{
    template <auto Fn, Game G, auto Own, Game G2 = G, auto Own2 = Own>
    consteval auto ownBody()
    {
        if constexpr (kInstanceGame == G)
            return Own;
        else if constexpr (kInstanceGame == G2)
            return Own2;
        else
            return Fn;
    }
}
// READABLE_OWN_AT(NAME, FN, ...): the symbol's name apart from the body's, for the build copy readable_entries.py
// makes (the bodies renamed NAME_ and OWN_, their names defined as entries), as READABLE_FUNCTION_AT is for the
// plain form. Sources write the _OWN forms, never _OWN_AT.
#if defined(ZX_GAME_INSTANCE)
#define READABLE_OWN2_AT(NAME, FN, STATUS, WAITS, VU0, GAME, OWN, GAME2, OWN2)                                     \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<::game::readable::ownBody<&FN, ::game::games::GAME, &OWN, ::game::games::GAME2,    \
                                                         &OWN2>(),                                                   \
                               sym::NAME, sym::level::NAME, sym::status::NAME, WAITS, VU0>(#NAME, READABLE_SOURCE_FILE)
#define READABLE_OWN_AT(NAME, FN, STATUS, WAITS, VU0, GAME, OWN)                                                   \
    static const ::game::readable::Bound READABLE_JOIN(g_readableRegistration_, __LINE__) =                        \
        ::game::readable::bind<::game::readable::ownBody<&FN, ::game::games::GAME, &OWN>(), sym::NAME,            \
                               sym::level::NAME, sym::status::NAME, WAITS, VU0>(#NAME, READABLE_SOURCE_FILE)
#else
#define READABLE_OWN2_AT(NAME, FN, STATUS, WAITS, VU0, GAME, OWN, GAME2, OWN2) READABLE_OWN_PLAIN_##WAITS##_##VU0(NAME, FN, STATUS)
#define READABLE_OWN_AT(NAME, FN, STATUS, WAITS, VU0, GAME, OWN) READABLE_OWN_PLAIN_##WAITS##_##VU0(NAME, FN, STATUS)
#define READABLE_OWN_PLAIN_false_false(NAME, FN, STATUS) READABLE_FUNCTION_AT(sym::NAME, FN, STATUS)
#define READABLE_OWN_PLAIN_true_false(NAME, FN, STATUS) READABLE_FUNCTION_WAITS_AT(sym::NAME, FN, STATUS)
#define READABLE_OWN_PLAIN_false_true(NAME, FN, STATUS) READABLE_FUNCTION_VU0_AT(sym::NAME, FN, STATUS)
#endif
#define READABLE_OWN(FN, STATUS, WAITS, VU0, GAME, OWN) READABLE_OWN_AT(FN, FN, STATUS, WAITS, VU0, GAME, OWN)
// (one own body or two: picked by the count of arguments, as READABLE_FUNCTION picks its form)
#define READABLE_OWN_PICK(_1, _2, _3, _4, _5, _6, _7, NAME, ...) NAME
#define READABLE_OWN_1(FN, STATUS, GAME, OWN) READABLE_OWN_AT(FN, FN, STATUS, false, false, GAME, OWN)
#define READABLE_OWN_2(FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(FN, FN, STATUS, false, false, GAME, OWN, GAME2, OWN2)
#define READABLE_OWN_WAITS_1(FN, STATUS, GAME, OWN) READABLE_OWN_AT(FN, FN, STATUS, true, false, GAME, OWN)
#define READABLE_OWN_WAITS_2(FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(FN, FN, STATUS, true, false, GAME, OWN, GAME2, OWN2)
#define READABLE_OWN_VU0_1(FN, STATUS, GAME, OWN) READABLE_OWN_AT(FN, FN, STATUS, false, true, GAME, OWN)
#define READABLE_OWN_VU0_2(FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(FN, FN, STATUS, false, true, GAME, OWN, GAME2, OWN2)
#define READABLE_FUNCTION_OWN(...) READABLE_OWN_PICK(__VA_ARGS__, , READABLE_OWN_2, , READABLE_OWN_1, , , , )(__VA_ARGS__)
#define READABLE_FUNCTION_WAITS_OWN(...)                                                                           \
    READABLE_OWN_PICK(__VA_ARGS__, , READABLE_OWN_WAITS_2, , READABLE_OWN_WAITS_1, , , , )(__VA_ARGS__)
#define READABLE_FUNCTION_VU0_OWN(...)                                                                             \
    READABLE_OWN_PICK(__VA_ARGS__, , READABLE_OWN_VU0_2, , READABLE_OWN_VU0_1, , , , )(__VA_ARGS__)
#define READABLE_OWN_AT_1(NAME, FN, STATUS, GAME, OWN) READABLE_OWN_AT(NAME, FN, STATUS, false, false, GAME, OWN)
#define READABLE_OWN_AT_2(NAME, FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(NAME, FN, STATUS, false, false, GAME, OWN, GAME2, OWN2)
#define READABLE_OWN_WAITS_AT_1(NAME, FN, STATUS, GAME, OWN) READABLE_OWN_AT(NAME, FN, STATUS, true, false, GAME, OWN)
#define READABLE_OWN_WAITS_AT_2(NAME, FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(NAME, FN, STATUS, true, false, GAME, OWN, GAME2, OWN2)
#define READABLE_OWN_VU0_AT_1(NAME, FN, STATUS, GAME, OWN) READABLE_OWN_AT(NAME, FN, STATUS, false, true, GAME, OWN)
#define READABLE_OWN_VU0_AT_2(NAME, FN, STATUS, GAME, OWN, GAME2, OWN2) READABLE_OWN2_AT(NAME, FN, STATUS, false, true, GAME, OWN, GAME2, OWN2)
#define READABLE_FUNCTION_OWN_AT(...) READABLE_OWN_PICK(__VA_ARGS__, READABLE_OWN_AT_2, , READABLE_OWN_AT_1, , , , , )(__VA_ARGS__)
#define READABLE_FUNCTION_WAITS_OWN_AT(...)                                                                        \
    READABLE_OWN_PICK(__VA_ARGS__, READABLE_OWN_WAITS_AT_2, , READABLE_OWN_WAITS_AT_1, , , , , )(__VA_ARGS__)
#define READABLE_FUNCTION_VU0_OWN_AT(...)                                                                          \
    READABLE_OWN_PICK(__VA_ARGS__, READABLE_OWN_VU0_AT_2, , READABLE_OWN_VU0_AT_1, , , , , )(__VA_ARGS__)
