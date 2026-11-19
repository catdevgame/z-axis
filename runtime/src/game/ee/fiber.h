#pragma once

// Host fibers: a function running on its own stack that can leave in the
// middle and be entered again where it left. Readable functions whose
// callees can wait run on one (suspend.cpp), so a guest call they make can
// return to the EE scheduler and resume them later.
//
// No other includes on purpose: the POSIX version needs _XOPEN_SOURCE for
// <ucontext.h>, which must not leak into the runtime's headers.

#include <cstddef>

namespace ee::fiber
{
    struct Context;

    // A fiber that will run entry(arg) when first entered. When entry
    // returns, the fiber leaves for the last time (that enter() returns) and
    // must not be entered again.
    Context *create(void (*entry)(void *), void *arg, std::size_t stackBytes);
    // A fiber whose entry returned goes back to a pool for the next
    // create() (fibers are reused: a level load makes over a million);
    // one destroyed while its entry is still running is freed.
    void destroy(Context *fiber);
    // From the host: run the fiber until it leaves (or its entry returns).
    void enter(Context *fiber);
    // From inside the fiber: back to whoever entered it.
    void leave(Context *fiber);
}
