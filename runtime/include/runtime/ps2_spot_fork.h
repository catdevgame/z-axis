#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

class PS2Runtime;

// PS2X_SPOT_FORK=<dir> (Mac and Linux; off by default): a run that reaches a
// spot becomes a server of copies of itself there. At the first VBlank start
// where the spot holds (PS2X_SPOT_AT="tick>=N" or "script>=N" on the runtime
// vsync tick or the pad script's clock, or the file <dir>/now made by the
// player), the game thread stops at that guest moment: the graphics workers
// drained and stopped, the host loop parked at its loop's top, the play link
// left. It writes <dir>/ready and then, for each request file <dir>/req/NAME
// (lines K=V: the child's environment on top of the parent's), forks: the
// child is the whole machine as it was (EE, VU, GS, IOP, the sound, the
// scheduler with its threads and closures, the host's state), with only the
// game thread running, so it builds the rest again from the quiet point: the
// graphics workers (if they ran), a new host loop thread, its own play link,
// state probe and recorder (by its environment), its log <dir>/NAME.log; then
// the game goes on exactly where the parent stopped. <dir>/NAME.pid holds the
// child's pid, <dir>/NAME.exit its exit status. At most PS2X_SPOT_FORK_MAX
// children run at once (2); the server ends at <dir>/stop or after
// PS2X_SPOT_FORK_IDLE seconds with no request (600).
//
// Exact only for what fork copies: the run must be headless with the null GS
// and no audio (ZAXIS_AUDIO=off), no save job or movie in progress at the spot,
// and no recorder in the parent (a child records from the spot on). Otherwise
// the server says why and the run goes on without it.
namespace ps2x::spot
{
    bool enabled();
    // The host loop (PS2Runtime::run): its body from the loop's top to the
    // run's end, so a forked child runs it again on a thread of its own.
    void setHostLoop(std::function<void()> loop);
    // At the top of each host loop iteration: parks the host thread while the
    // game thread serves (the parent's host loop never goes on).
    void hostLoopTop();
    // At a VBlank start on the game thread (EeScheduler, after the state probe).
    void onVsync(PS2Runtime &runtime, uint64_t tick, int64_t scriptTime);
    // Run in a child at the spot, after the runtime's own rebuild (the game
    // library's recorder, from its environment).
    void addChildHook(std::function<void(PS2Runtime &)> hook);
    // How many times this process has been forked at a spot (0 in the parent):
    // a subsystem that read its environment once reads it again when it changes.
    // (Inline, so a reader such as the state probe links without the server.)
    inline std::atomic<uint32_t> g_forkEpoch{0};
    inline uint32_t forkEpoch() { return g_forkEpoch.load(std::memory_order_acquire); }
}
