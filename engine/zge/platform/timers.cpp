// The engine's timers (zTimer.c: "../ngengine/ZGE3/zge/mods/zTimer.c" names
// the update task): records due at a time of the game clock (collisionClock,
// milliseconds), kept on a priority queue keyed by that time, the spare ones
// on a free list. The update task hands each timer that is due to its
// callback. Nothing in this build starts a timer: initTimerQueue, the start
// functions and timerExpired have no caller, so the queue stays empty.

#include "zge/container/heap.h"
#include "zge/container/list.h"
#include "zge/memory/regions.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/collision/collision.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/timers.cpp.h"

namespace zge
{

    using TimerCallback = ee::FunctionPtr<void(uint32_t now, uint32_t object, uint32_t argument)>;

    // A timer (0x20 bytes from the heap).
    struct TimerRecord
    {
        ListNode link;     // +0x00 on the free list
        uint32_t due;      // +0x08 the clock it is due at: its key on the queue
        uint32_t delay;    // +0x0C what was added to the clock
        uint32_t mode;     // +0x10 TimerMode
        uint32_t object;   // +0x14 the callback's second argument
        uint32_t argument; // +0x18 its third
        TimerCallback callback; // +0x1C
    };
    static_assert(offsetof(TimerRecord, mode) == 0x10 && sizeof(TimerRecord) == 0x20);

    // What a due timer does (updateEngineTimers).
    enum TimerMode : uint32_t
    {
        kTimerCall = 1,    // its callback called, the record given back
        kTimerRepeat = 2,  // startRepeatingTimer's: given back uncalled (it does not repeat)
        kTimerHalt = 4,    // prepareHaltTimer's: the update stops there and answers 0, the record kept
    };

    // The timers' state.
    struct Timers
    {
        uint16_t capacity;       // +0x00 the records initTimerQueue makes (initEngineTimers' argument); the queue is word-aligned after it
        PriorityQueue queue;     // +0x04 the timers waiting, by due time (TimerRecord values)
        List freeRecords;        // +0x14 TimerRecord
        ee::Ptr<Task> task;      // +0x20 the task running updateEngineTimers
    };
    static_assert(offsetof(Timers, queue) == 0x04 && offsetof(Timers, freeRecords) == 0x14 && offsetof(Timers, task) == 0x20);

    namespace
    {
        inline ee::Global<Timers, sym::g_timers> g_timers;
        inline ee::Global<int32_t, sym::g_timersReady> g_timersReady;
        inline ee::Global<int32_t, sym::g_timerQueueReady> g_timerQueueReady;

        constexpr uint32_t kTaskPriority = kStageAfterFrame, kTaskLine = 0xD0;
        // The queue's third argument: the latest due time on top.
        constexpr uint32_t kLargestFirst = 1;

        // The free list's first record, as a timer (0: none).
        ee::Ptr<TimerRecord> firstFreeTimer()
        {
            return g_timers->freeRecords.head.cast<TimerRecord>();
        }
    }

    // initEngineTimers: the timers' capacity kept and their update task made.
    // 1, or 0 when no task could be made.
    int32_t initEngineTimers(uint32_t capacity)
    {
        g_timers->capacity = static_cast<uint16_t>(capacity);
        *g_timersReady = 1;
        const ee::Ptr<Task> task = newTask(kTaskPriority, sym::updateEngineTimers, 0, 0, sym::g_textNgengineZge3ZgeModsZTimerC, kTaskLine);
        g_timers->task = task;
        return task ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(initEngineTimers, Verified);

    // updateEngineTimers (the task): every timer due by now taken off the
    // queue and done as its mode says. 1, or 0 when a timer stopped it (a
    // halt timer, or one with no mode).
    uint32_t updateEngineTimers()
    {
        const uint32_t now = collisionClock();
        for (;;)
        {
            const ee::Ptr<HeapEntry> top = priorityQueueTop(&g_timers->queue);
            if (!top)
                return 1;
            const ee::Ptr<TimerRecord> timer(top->value);
            if (now < timer->due)
                return 1;
            priorityQueuePop(&g_timers->queue);
            const uint32_t mode = timer->mode;
            if (mode == kTimerCall)
                timer->callback(now, timer->object, timer->argument);
            else if (mode == 0 || mode >= kTimerHalt)
                return 0;
            listPush(&g_timers->freeRecords, timer);
        }
    }
    READABLE_FUNCTION(updateEngineTimers, Verified);

    // initTimerQueue (no caller in this build): once, the queue made with
    // room for every timer, and that many records put on the free list.
    void initTimerQueue()
    {
        if (*g_timerQueueReady != 0)
            return;
        Timers &timers = *g_timers;
        initPriorityQueue(&timers.queue, kLargestFirst, timers.capacity);
        initList(&timers.freeRecords);
        for (uint32_t i = 0; i < timers.capacity; ++i)
            listPush(&timers.freeRecords, heapAllocate(sizeof(TimerRecord)));
        *g_timerQueueReady = 1;
    }
    READABLE_FUNCTION_WAITS(initTimerQueue, Draft);

    // shutdownEngineTimers: the task deleted; once the queue was made, every
    // waiting timer moved to the free list, the queue freed and every record
    // given back to the heap. 0 when the timers were never set up, else 1.
    int32_t shutdownEngineTimers()
    {
        if (*g_timersReady == 0)
            return 0;
        deleteTask(g_timers->task);
        if (*g_timerQueueReady == 0)
            return 1;
        Timers &timers = *g_timers;
        for (ee::Ptr<HeapEntry> entry = priorityQueuePopAs<HeapEntry>(&timers.queue); entry; entry = priorityQueuePopAs<HeapEntry>(&timers.queue))
            listPush(&timers.freeRecords, entry->value);
        freePriorityQueue(&timers.queue);
        for (ee::Ptr<ListNode> record = timers.freeRecords.head; record; record = timers.freeRecords.head)
        {
            listRemove(&timers.freeRecords, record);
            heapFree(record);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownEngineTimers, Verified);

    namespace
    {
    // The free list's first record made a timer of the mode, due `delay`
    // from now, and queued; it stays on the free list too (the original
    // never takes it off). 1, or 0 with no record free.
    int32_t queueTimer(uint32_t delay, uint32_t callback, uint32_t object, uint32_t argument, TimerMode mode)
    {
        const uint32_t now = collisionClock();
        const ee::Ptr<TimerRecord> timer = firstFreeTimer();
        if (!timer)
            return 0;
        initTimerQueue();
        const uint32_t due = delay + now;
        timer->object = object;
        timer->argument = argument;
        timer->callback = TimerCallback(callback);
        timer->delay = delay;
        timer->mode = mode;
        timer->due = due;
        priorityQueueInsert(due, ee::Address(timer), &g_timers->queue);
        return 1;
    }
    }

    // startTimer (no caller in this build): a timer calling back(now,
    // object, argument) once, `delay` milliseconds from now (queueTimer).
    int32_t startTimer(uint32_t delay, uint32_t callback, uint32_t object, uint32_t argument)
    {
        return queueTimer(delay, callback, object, argument, kTimerCall);
    }
    READABLE_FUNCTION_WAITS(startTimer, Draft);

    // startRepeatingTimer (no caller in this build): the same with the
    // repeating mode, which the update gives back uncalled.
    int32_t startRepeatingTimer(uint32_t period, uint32_t callback, uint32_t object, uint32_t argument)
    {
        return queueTimer(period, callback, object, argument, kTimerRepeat);
    }
    READABLE_FUNCTION_WAITS(startRepeatingTimer, Draft);

    // prepareTimer (no caller in this build): the free list's first record
    // filled as a one-call timer due `delay` from now, not queued. The
    // record, or 0 with none free.
    ee::Ptr<TimerRecord> prepareTimer(uint32_t delay, uint32_t callback, uint32_t object, uint32_t argument)
    {
        const uint32_t now = collisionClock();
        const ee::Ptr<TimerRecord> timer = firstFreeTimer();
        if (!timer)
            return timer;
        initTimerQueue();
        timer->due = delay + now;
        timer->mode = kTimerCall;
        timer->object = object;
        timer->argument = argument;
        timer->callback = TimerCallback(callback);
        timer->delay = delay;
        return timer;
    }
    READABLE_FUNCTION_WAITS(prepareTimer, Draft);

    // prepareHaltTimer (no caller in this build): the free list's first
    // record filled as a halt timer due `delay` from now, with no callback,
    // not queued. The record, or 0 with none free.
    ee::Ptr<TimerRecord> prepareHaltTimer(uint32_t delay)
    {
        const uint32_t now = collisionClock();
        const ee::Ptr<TimerRecord> timer = firstFreeTimer();
        if (!timer)
            return timer;
        initTimerQueue();
        timer->due = delay + now;
        timer->mode = kTimerHalt;
        timer->delay = delay;
        timer->object = 0;
        timer->argument = 0;
        timer->callback = TimerCallback();
        return timer;
    }
    READABLE_FUNCTION_WAITS(prepareHaltTimer, Draft);

    // timerExpired (no caller in this build): whether the timer is due; a
    // one-call timer found due calls back (and stays as it is).
    uint32_t timerExpired(ee::Ptr<TimerRecord> timer)
    {
        const uint32_t now = collisionClock();
        if (now < timer->due)
            return 0;
        if (timer->mode == kTimerCall)
            timer->callback(now, timer->object, timer->argument);
        return 1;
    }
    READABLE_FUNCTION(timerExpired, Draft);
}
