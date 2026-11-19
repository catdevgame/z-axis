#pragma once

// The kernel as a host form (ee/host_form.h) uses it: the waits and ends a
// function leaves for after its return, and the kernel's objects, made as the
// PS2 form makes them. The disc device's (zge/host/disc_device.cpp), the
// save jobs' (zge/host/save_jobs.cpp) and the movie stream's
// (zge/host/movie_stream.cpp).

#include <cstdint>
#include <initializer_list>

namespace game::host
{
    // ---- after the function returns (host_form.h) --------------------------

    // Its caller waits as for an RPC's reply when another thread is ready to
    // run meanwhile (EeScheduler::awaitRpcReply: the reply latency, ~25 us of
    // EE time), the result kept.
    void waitAsForReply();
    // A thread made ready runs now if it comes first (EeScheduler::
    // transferIfRequested).
    void letReadyThreadRun();
    // The thread sleeps; a wakeup runs `function` (the host form's function,
    // again).
    void sleepInto(uint32_t function);
    // The thread ends and is deleted (ExitDeleteThread).
    void endThread();

    // ---- the kernel's objects, made as the PS2 form makes them -------------

    int createSemaphore(int count, int maxCount);
    int createThread(uint32_t entry, uint32_t stack, uint32_t stackBytes, uint32_t globalPointer, int priority);
    // Started with argument 0 (it runs when the caller lets it:
    // letReadyThreadRun).
    void startThread(int thread);
    // The calling thread's priority.
    void changeOwnPriority(int priority);

    // ---- a game function called from a host form ----------------------------

    // `function` called now on the caller's thread with up to eight word
    // arguments, its frames `kGameCallBelowStack` below the caller's stack
    // pointer (where the caller's own frames would go next); false when the
    // game has no function there.
    constexpr uint32_t kGameCallBelowStack = 0x400u;
    bool callGameFunction(uint32_t function, std::initializer_list<uint32_t> arguments);
    // After the form returns: `function` called on the caller's thread with
    // one word argument, as a call of its own on the scheduler's invocation
    // stack (no frame of the caller's touched; it may wait and let other
    // threads run), then the caller goes on at its return address with
    // `result` for the form's result. It does not come back.
    void callGameFunctionAfterReturn(uint32_t function, uint32_t argument, int32_t result);

    // ---- what the running thread sees --------------------------------------

    // The vsync count now.
    uint64_t vsyncNow();
    // The running thread's id.
    int currentThread();
    // Guest memory free while the form runs: `bytes` below the caller's stack
    // pointer, 16-byte aligned (where its own frames would go next).
    uint32_t belowStack(uint32_t bytes);
}
