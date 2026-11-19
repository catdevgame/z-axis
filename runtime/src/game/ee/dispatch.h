#pragma once

// A jump the original dispatches at run time (ee/abi.h, safe points): a JR
// through a table the recompiler did not turn into a switch (no bounds check
// before it; readable_draft.py find_unchecked_tables), which generated code
// hands to dispatchGuestBranch as an IndirectJump: 8 EE cycles and a check,
// then always an unwind to the scheduler, which dispatches the target (8 and
// a check) through the function table; the rest of the call runs as the
// table maps the target (an entry block: slice by slice; the function's own
// case: the whole function). A draft's `switch` for such a table starts each
// case with
//
//     ee::jumpDispatch(points::target, true); // the target starts an entry block
//
// On a fiber with safe points on the readable function leaves there as the
// original does and goes on in the target's mode; under ps2xFnCheck (and with
// PS2X_READABLE_SAFEPOINTS=0) only the dispatch's 8 cycles are charged, as
// the traced original's IndirectJump is.
//
// (Kept out of ee/abi.h, which every traced original includes.)

#include <cstdint>

namespace ee
{
    void jumpDispatch(uint32_t target, bool entryBlock);

    // A jump table's word no case of the draft's `switch` lists, at the JR
    // `jr`: every word is a case, so only an index outside the table gets
    // here, which the original's own code rules out. It says so (1st, 2nd,
    // 4th ... time), and charges nothing.
    void unlistedTarget(uint32_t jr);

    // A JR through a jump table the recompiler did turn into a switch: the
    // whole function runs the case in place (nothing to charge), but an
    // entry block's code holds only the cases inside it, so from the entry
    // block `slice` (the one the JR is in) a target outside it is
    // dispatchGuestBranch's IndirectJump, as above (drafter23: animateProps'
    // kind table from entry_1C4DA8, 8 cycles early at career tick 1,543).
    // Written before the case's ee::block for each target outside `slice`;
    // a frame anywhere else (the whole function) charges nothing.
    //
    //     ee::tableJump(points::theCase, points::block, true); // the case, the JR's entry block
    void tableJump(uint32_t target, uint32_t slice, bool entryBlock);

    // Right after a call whose return address starts an entry block that
    // other paths reach too (ee::block(L, true) belongs to the call's own
    // fall-through only): if the call came back through the scheduler, the
    // frame goes on in entry-block mode at `leader`, as the original resumed
    // there; the ee::block(leader) that follows charges nothing more. A
    // draft writes it (readable_draft.py); otherwise ee::block(L, true).
    void afterCall(uint32_t leader);
}
