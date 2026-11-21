#pragma once

#include "machine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;
    // Most host-time lag the pacer catches up after a stall (three vsyncs).
    static constexpr std::chrono::microseconds kMaxPacingDebt{50000};

    explicit EeScheduler(PS2Machine &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    // Host pause (window lost focus): the executor blocks at its next
    // checkpoint, and host-time deadlines move by the paused time.
    void setHostPaused(bool paused);
    [[nodiscard]] bool isHostPaused() const;
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    void accountCycles(uint32_t cycles) noexcept;
    // Emulated EE cycles since reset (advanced by guest work and idle waits).
    [[nodiscard]] uint64_t eeCycle() const noexcept { return m_eeCycle; }
    // Passes of the run loop (each dispatch, event, thread switch or idle wait is at least one) and checkpoints come
    // due, counted together: one more between two moments of a thread means only the dispatch of that thread's pc
    // ran between them (fncheck's recorder resumes a call it parked with none of its calls open only so).
    [[nodiscard]] uint64_t scheduleSteps() const noexcept { return m_scheduleSteps; }
    // Times other guest code than one thread's ran between that thread's (a dispatch of another thread or of an
    // invocation, an interrupt handler's) and checkpoints come due (their events): unchanged between two moments
    // of a thread means only that thread's own code ran between them (fncheck's recorder keeps a call whose callee
    // went through the scheduler only so).
    [[nodiscard]] uint64_t interleaves() const noexcept { return m_interleaves; }
    [[nodiscard]] bool isExecutingGuest() const noexcept;
    // Host time the pacer did not catch up after stalls (kMaxPacingDebt).
    [[nodiscard]] uint64_t droppedPacingNs() const noexcept { return m_droppedPacingNs.load(std::memory_order_relaxed); }
    // Vsyncs run unpaced on a silent loading screen (PS2Machine::setLoadingScreenProbe).
    [[nodiscard]] uint64_t fastForwardVsyncs() const noexcept { return m_fastForwardVsyncs.load(std::memory_order_relaxed); }

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    [[nodiscard]] uint32_t invocationStackTop();
    // Returns a deleted thread's invocation stacks to the free list.
    void releaseInvocationStacks(int threadId);
    [[nodiscard]] size_t invocationStackCount() const { return m_invocationStackTops.size(); }

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});
    // A synchronous SIF RPC blocks its caller on hardware until the IOP's
    // reply arrives, and other ready threads run meanwhile. The IOP answers
    // at once here, so canAwaitRpcReply() tells whether waiting would let
    // anything else run (a plain guest thread, not an interrupt or callback,
    // with another thread ready); awaitRpcReply() then blocks the caller for
    // the reply latency and runs `completion` (which delivers the reply)
    // when it wakes.
    [[nodiscard]] bool canAwaitRpcReply() const;
    [[noreturn]] void awaitRpcReply(std::function<void(R5900Context &)> completion);

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    // A line per guest thread (id, status, stack, its size, sp), for the
    // RAM dumps' .sp files: what career_ab.py calls dead stack.
    [[nodiscard]] std::string threadStacksText() const;
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    void publishSnapshot();
    void flushSnapshot();

private:
    // EE time passes: `cycles` charged (on ZX_EE_SCALE's clock, but an idle jump to the next event, which
    // stays whole), with the timers and the IOP brought along; `caller` names the charge in the EE trace.
    void advanceCycles(uint32_t cycles, void *caller, bool idleJump) noexcept;
    // The scheduler's idle wait jumps to the next event (advanceCycles' idle jump).
    void accountIdleCycles(uint32_t cycles) noexcept;
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void capPacingDebt(std::chrono::steady_clock::time_point oldestDue, std::chrono::steady_clock::time_point now);
    void updateFastForward();
    [[nodiscard]] bool pacingOff() const noexcept;
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Machine &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    std::thread::id m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    bool m_hostPaused = false; // guarded by m_eventMutex
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_scheduleSteps = 0;
    uint64_t m_interleaves = 0;
    int m_lastDispatchedThread = 0; // (0: an invocation's)
    uint64_t m_rpcReplyToken = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    // Stacks of deleted threads, reused before reserving new guest memory.
    std::vector<uint32_t> m_freeInvocationStacks;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};
    std::atomic<uint64_t> m_droppedPacingNs{0};
    bool m_fastForward = false; // executor only
    uint64_t m_fastForwardStart = 0;
    std::atomic<uint64_t> m_fastForwardVsyncs{0};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;
    bool m_snapshotDirty = true; // executor only
};
