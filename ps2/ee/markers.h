#pragma once

// The safe-point markers a readable function passes (ee::block,
// ee::loopBack, an entry block's ee::jumpDispatch), told to a hook while one
// is set: zx-fncheck --markers sets it around its replays and lists the
// original's entry blocks and loop heads that the replayed calls reach but
// the readable code never marks (fncheck_tables.h SafePointMark). Kept out of
// ee/abi.h, which every traced original includes.

#include <cstdint>

namespace ee
{
    enum class MarkerKind : uint8_t
    {
        Block,    // ee::block(leader) / ee::jumpDispatch(target, true)
        LoopBack, // ee::loopBack(head)
    };
    using MarkerHook = void (*)(uint32_t address, MarkerKind kind);
    // The thread's hook (nullptr: none, the default). The dev flavour's alone (ZX_DEV_CHECKS): the app's has
    // no checker to set one, and its hooks are constant nulls.
    void setMarkerHook(MarkerHook hook);

    // zx-fncheck --safepoints: under its call handler, every safe point a
    // readable replay passes (its cycles, after they are charged) asks the
    // hook whether a checkpoint is due there; true: the frame goes on as if
    // it had left and been resumed (the safe-point model of ee/abi.h runs
    // under the handler while a hook is set). nullptr: none, the default.
    using SafePointHook = bool (*)(uint32_t cycles);
    void setSafePointHook(SafePointHook hook);
    // Whether one is set (the model runs under the call handler).
    bool safePointHookSet();

    // The returns through the scheduler ee::leaveDirectCall has charged on
    // this thread (8 cycles each): ZX_FNCYCLES (cycletrace.cpp) leaves the
    // one after a direct callee that did not unwind itself out of its line,
    // as the table wrapper's line ends before the scheduler's dispatch of
    // the return address.
    uint64_t directReturnDispatches();
}
