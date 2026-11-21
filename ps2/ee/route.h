#pragma once

// A named call of a readable function where its plain name cannot be used: another
// function of that name is in scope, or two registrations with identical code fold
// into one host function, so the call has to say which game function it means.
//
//     namespace routed { GAME_ROUTE(poolRecord, game::callAt<&zge::poolRecord_, sym::poolRecord>); }
//     routed::poolRecord(link);
//
// The routing is the one line's (hooks, the replay's answers, a function switched off).

#include "ee/readable.h"

#include <utility>

#define GAME_ROUTE(name, ...)                                 \
    template <typename... A>                                  \
    decltype(auto) name(A &&...args)                          \
    {                                                         \
        return __VA_ARGS__(std::forward<A>(args)...);         \
    }

// The same for a function the file knows only by its address: `routed::poolFree(pool, record)`.
#define GAME_TABLE_ROUTE(name, form, address)                                  \
    template <typename... A>                                                   \
    decltype(auto) name(A &&...args)                                           \
    {                                                                          \
        return form(address, std::forward<A>(args)...);                        \
    }
