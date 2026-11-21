#ifndef PS2_VU1_AOT_PROGRAM_H
#define PS2_VU1_AOT_PROGRAM_H

// Support for ahead-of-time compiled VU1 microprograms. zx-vu1aot writes one
// source per micro memory image: the image's decoded pairs as constants and a
// function that runs the interpreter's own step (stepPair) on them, with a
// goto per pair for sequential flow and a switch on pc for everything else.
// With the pairs constant the compiler folds the decoding, dispatch and
// dead checks away; the semantics stay the interpreter's. The function is
// registered under the image's hash and used whenever micro memory matches
// (vu1_aot_register.h: in the runner, or in a game module's list).
// Written against the core alone (vu1/vu1_core.h), so the same
// sources build into the runner and into a game module.
// Only the step, its bookkeeping and the FMAC fast path are forced inline:
// forcing execLower/execUpper (big switches) into every pair costs far more
// to compile than it saves.
#define VU1_FMAC_INLINE inline __attribute__((always_inline))
#include "vu1_exec_lower.h"
#include "vu1_exec_upper.h"
#include "vu1_step.h"

template <uint64_t Hash>
struct VU1AotProgram;

// The largest of the issue-cycle terms of a block pair (inline everywhere:
// MSVC's std::max of an initializer list is an out-of-line vector routine).
template <typename... T>
VU1_FORCE_INLINE uint64_t vu1AotMax(uint64_t value, T... rest)
{
    ((value = value < static_cast<uint64_t>(rest) ? static_cast<uint64_t>(rest) : value), ...);
    return value;
}

// The code is split into chunks of consecutive pairs, one function (and one
// source) each, to keep compile time down. A chunk runs until execution
// leaves it (returns false: the caller dispatches on pc again) or the run
// loop ends (returns true).

// One pair: the run loop's condition, the step, then straight on to the next
// pair if execution continues in sequence. The interpreter loop's commit at
// the top of each pass is left out: every step ends with advanceOneCycle,
// which commits for the new cycle, and VU1Interpreter::run commits before
// entering the program.
#define VU1_AOT_PAIR(index, nextPc, nextLabel)                                  \
    if (!(vu.m_cycle < rc.budgetEnd && !vu.m_stopRequested))                    \
        return true;                                                            \
    if (vu.stepPair(kPairs[index], true, rc) == VU1Core::StepResult::Stop) \
        return true;                                                            \
    if (vu.m_state.pc == (nextPc))                                              \
        goto nextLabel;                                                         \
    goto dispatch;
// The last pair of a chunk: anything but the end of the run goes back to
// the dispatcher.
#define VU1_AOT_LAST_PAIR(index)                                                \
    if (!(vu.m_cycle < rc.budgetEnd && !vu.m_stopRequested))                    \
        return true;                                                            \
    if (vu.stepPair(kPairs[index], true, rc) == VU1Core::StepResult::Stop) \
        return true;                                                            \
    goto dispatch;

#endif
