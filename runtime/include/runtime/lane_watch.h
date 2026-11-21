// Lane pokes and the lane watch: the EE-side tools of tooling/checks/wlane.py (the proof that a leftover vector
// lane, the w of a 128-bit value or a quadword's upper half, reaches nothing observable).
//
// ZX_LANE_POKE: register lanes and frame words forced at dispatch points, so a lane the original leaves stale
// can be run with two values and everything observable compared (the EE side of zx-vu1check's --poke).
// Items separated by ';', each `WHEN=PC WHAT=HEX [WHAT=HEX ...]`:
//   WHEN   call=PC   before the call made at site PC (the jal's or jalr's address)
//          ret=PC    after that call returns to its caller
//          entry=PC  when the function at PC is called, before it runs
//          exit=PC   when the function at PC returns (ret and exit: at a return through the call's dispatch; a callee the
//                    EE scheduler unwound returns through its resume instead and is not poked: entry= is the sure point)
//   WHAT   vfN.x|y|z|w  a VU0 register's lane;  rN.0..3 or an ABI name (a3.2, s0.3; .hi = words 2 and 3);
//          fN  an FPU register;  sp+OFF or sp-OFF  the word at that offset from sp as it is at the point.
// Every application counts; the total is printed at exit ([lane-poke] lines on stderr).
//
// ZX_LANE_WATCH=ADDR[+BYTES],...: in a build with ZX_LANE_WATCH_BUILD (a tool build), every guest load and store
// whose bytes cover a watched word is counted by its PC, and every readable access (ee::at) by its host caller;
// the report goes to ZX_LANE_WATCH_OUT (a file) or stderr at exit:
//   R pc=0x... at=0x... size=N n=COUNT first=TICK last=TICK      a load of the recompiled code
//   W pc=0x... at=0x... size=N n=COUNT first=TICK last=TICK      a store
//   A fn=SYMBOL+OFF at=0x... size=N n=COUNT first=TICK last=TICK  a readable body's ee::at<T> of it
// ZX_LANE_WATCH_PCS=PC,...: the same lines for every access the instructions at those addresses make, by the
// address reached (the backward step: what a store site's function read).
//
// ZX_LANE_DIFF=OUT (with ZX_LANE_DIFF_AT=TICK,...): the differential, in the same tool build. Per instruction of the
// recompiled code a count and a hash of what it did, in order: a load's and a store's address and the bytes it read or
// left (a hardware address: the address alone), a conditional branch's direction, a register jump's target; per
// dispatched callee the sequence of its call sites. At each tick listed (a vsync) they go to OUT.tTICK:
//   D pc=0x... k=L|S|B|J|C n=COUNT a=HASH v=HASH      (a: addresses, directions, targets or sites; v: the values)
// wlane.py poke --diff runs a lane at several values and compares: the same n and a everywhere is the same control
// flow, the same addresses and the same calls; the instructions whose v differ are where the lane is read and moved.
// The branches and jumps are seen only in generated code that names them (wlane_branches.py's copy of a game's
// generated folder: `ZX_LANE_BRANCH(pc, taken)`, `ZX_LANE_JUMP(pc, target)`).
//
// ZX_LANE_CALLERS=1: every distinct (callee, call site) pair dispatched, into ZX_LANE_CALLERS_OUT (a file, kept
// current every 32 vsyncs): `C callee=0x... from=0x... n=COUNT`, the run's call graph, which wlane.py --callers reads
// to ascend from a function into the callers no jal names (tasks, callbacks, pointers).
#pragma once

#include <stdint.h>

struct R5900Context;

namespace zx::lanes
{
    // The watch (ZX_LANE_WATCH_BUILD builds call these from the READ*/WRITE* macros and ee::at).
    extern bool g_watchOn;
    void watchAccess(uint32_t address, uint32_t size, bool write, uint32_t pc);
    // (Not inlined: its return address is the readable function's, as ee::checkGuestAccess.)
    void watchReadable(uint32_t address, uint32_t size) __attribute__((noinline));

    // The pokes (dispatchGuestBranch).
    extern bool g_pokesOn;
    enum class Point
    {
        BeforeCall, // the call at sourcePc is about to run targetPc
        AfterCall   // it returned
    };
    void poke(R5900Context *ctx, uint8_t *rdram, uint32_t sourcePc, uint32_t targetPc, Point point);

    // The callers (dispatchGuestBranch, every call).
    extern bool g_callersOn;
    void caller(uint32_t targetPc, uint32_t sourcePc);

    // The differential (ZX_LANE_DIFF): the READ*/WRITE* macros, the branches and jumps of a wlane_branches.py copy.
    extern bool g_diffOn;
    void diffAccess(const uint8_t *rdram, uint32_t address, uint32_t size, bool write, uint32_t pc);
    bool diffBranch(uint32_t pc, bool taken);
    uint32_t diffJump(uint32_t pc, uint32_t target);

    // The vsync tick, for the report's first/last.
    void onVsync(uint64_t tick);
}
