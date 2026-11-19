// The game's timers (readable round gamehand13; were the sweep's drafts of
// newTimerRecord .. timerHook): a timer counts its time down by the frame's (up
// with a positive step), calls its tick hook when it passes its tick time
// and its expire hook when it runs out; scripts make them (createTimer's
// natives, script/library.cpp), the challenges and the HUD's queued
// messages too. One timer may be the visible one: the HUD's
// "OverlayToggle.Timer" widget shows its time (its "time" variable, "PLAY
// Refresh"). A released timer is freed at the end of the next frame's
// update.

#include "aggressive/globals.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/abi.h"
#include "ee/callback.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/text/cstring.h"
#include "aggressive/sound/events.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/timers.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kSetVariable = sym::setVariable; // zgui setVariable (widget, name, value *), by address (hooked)
        constexpr uint32_t kRunScriptAt = sym::runScript; // zgui runScript, by address where its result is ours
        constexpr uint32_t kNewTimerRecord = sym::newTimerRecord;
        constexpr uint32_t kFreeTimerRecord = sym::freeTimerRecord;
        constexpr uint32_t kRunTimerHooks = sym::runTimerHooks;
        constexpr uint32_t kStepTimer = sym::stepTimer;
        constexpr uint32_t kTimerTime = sym::timerTime;
        constexpr uint32_t kTimerTickTime = sym::timerTickTime;
        constexpr uint32_t kTimerHook = sym::timerHook;

        using TimerHook = ee::Callback<int32_t(uint32_t, uint32_t, uint32_t, ee::Float)>;

        // A timer (0x30 bytes from the timer pool).
        struct Timer
        {
            uint32_t next, previous; // +0x00 the timers' list
            ee::Float time;          // +0x08 seconds left
            TimerHook onExpire;      // +0x0C (timer, 0, expireData, 0) when it runs out, or 0
            uint32_t expireData;     // +0x10
            ee::Float tickTime;      // +0x14 the time onTick is called at (-1: none)
            TimerHook onTick;        // +0x18 (timer, rising, tickData, time), or 0
            uint32_t tickData;       // +0x1C
            uint32_t running;        // +0x20 1 while it counts (0 once it has run out)
            // +0x24 its node in the released list (its value the timer).
            uint32_t releasedNext, releasedPrevious, self;
        };
        static_assert(sizeof(Timer) == 0x30 && offsetof(Timer, tickTime) == 0x14 && offsetof(Timer, releasedNext) == 0x24);

        inline ee::Global<uint32_t, sym::g_timerPool> g_timerPool;
        constexpr ee::Ptr<zge::List> kTimers{sym::timers};   // zge::List of the timers
        constexpr ee::Ptr<zge::List> kReleased{sym::released}; // zge::List of the released ones (their +0x24 nodes)
        // A released list node: the list's links, then its value.
        struct ReleasedNode
        {
            uint32_t next, previous;
            uint32_t timer;
        };
        inline ee::Global<uint32_t, sym::timers> g_firstTimer;
        inline ee::Global<ee::Ptr<ReleasedNode>, sym::released> g_firstReleased;
        ee::Ptr<Timer> timerAt(uint32_t timer) { return ee::Ptr<Timer>(timer); }
        ee::Ptr<zge::ListNode> releasedNode(uint32_t timer) { return ee::Ptr<zge::ListNode>(timer + static_cast<uint32_t>(offsetof(Timer, releasedNext))); }
        inline ee::Global<int32_t, sym::g_releasedCount> g_releasedCount;
        inline ee::Global<uint32_t, sym::g_visibleTimer> g_visibleTimer;
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_timerWidget> g_timerWidget; // found when first needed
        constexpr uint32_t kSplitScreenTimer = sym::splitScreenTimer;    // "MultiplayerUI.OverlayToggle.Timer"
        constexpr uint32_t kTimerWidget = sym::timerWidget;         // "InGameUI.OverlayToggle.Timer"
        constexpr ee::Ptr<const char> kTimeVariable{sym::timeVariable};        // "time"
        constexpr uint32_t kVisibleVariable = sym::visibleVariable;     // "Visible"
        constexpr uint32_t kRefresh = sym::refresh;             // "PLAY Refresh"
        constexpr uint32_t kShow = sym::show;                // "PLAY Show"
        constexpr uint32_t kAbortHide = sym::abortHide;           // "ABORT Hide"
        constexpr uint32_t kStopFreakOut = sym::stopFreakOut2;        // "STOP FreakOut"
        constexpr uint32_t kHide = sym::hide;                // "PLAY Hide"
        constexpr uint32_t kAbortShow = sym::abortShow;           // "ABORT Show"
        constexpr uint32_t kTimeUpSound = 46;                  // soundEvent's: the main bank's 90
        constexpr uint32_t kTimerPoolBlocks = 2;
        constexpr float kNoTick = -1.0f;

        // The HUD's timer widget (the split screen's in split screen), found
        // the first time it is wanted.
        ee::Ptr<zgui::Node> timerWidget()
        {
            if (!*g_timerWidget)
            {
                const uint32_t path = *g_gameType == GameType::SplitScreen ? kSplitScreenTimer : kTimerWidget;
                *g_timerWidget = zgui::findNode(*zgui::g_root, ee::Ptr<const char>(path), uint64_t{1});
            }
            return *g_timerWidget;
        }

        int32_t runScriptResult(ee::Address widget, uint32_t script) { return ee::game<int32_t>(kRunScriptAt, widget, script, 0); }
        void runTimerScript(uint32_t script) { zgui::runScript(timerWidget(), ee::Ptr<char>(script), 0); }
    }

    namespace
    {
        // timerHook is void: setTimer and setTimerTick return what it leaves in the result register.
        int32_t timerHookKeepingResult() { return ee::game<int32_t>(kTimerHook); }
    }

    void timerHook(); // (below)

    // newTimerRecord: a timer record, zeroed (0 when the pool has none).
    uint32_t newTimerRecord()
    {
        const uint32_t timer = poolTake(*g_timerPool);
        if (timer == 0)
            return 0;
        zge::memoryFill(timer, 0, static_cast<uint32_t>(sizeof(Timer)));
        return timer;
    }
    READABLE_FUNCTION_WAITS(newTimerRecord, Verified);

    // freeTimerRecord (timer): its record back to the pool.
    int32_t freeTimerRecord(uint32_t timer)
    {
        return game::tail(poolGive, *g_timerPool, timer);
    }
    READABLE_FUNCTION(freeTimerRecord, Verified);

    ee::Float timerTickTime(uint32_t timer);

    // runTimerHooks (a step's times): its tick hook when the step passed its
    // tick time going down, its expire hook when it ran out going down. Bug
    // kept: the test going up can never be true (the tick would have to be at
    // or below `before` with `after` under it), so a rising timer never ticks.
    void runTimerHooks(uint32_t timerAddress, ee::Float before, ee::Float after)
    {
        const Timer &timer = *ee::Ptr<Timer>(timerAddress);
        if (before == after)
            return;
        const uint32_t rising = after < before ? 0 : 1;
        const ee::Float tick = timerTickTime(timerAddress);
        if (ee::Float(0.0f) <= tick && !(tick == ee::Float(kNoTick)))
        {
            const bool passedGoingDown = !rising && tick < before && after <= tick;
            const bool passedGoingUp = rising == 1 && tick <= before && after < tick;
            if (passedGoingDown || passedGoingUp)
                timer.onTick(timerAddress, rising, timer.tickData, after);
        }
        if (ee::Float(0.0f) < before && after <= ee::Float(0.0f) && !rising && timer.onExpire)
            timer.onExpire(timerAddress, 0, timer.expireData, ee::Float(0.0f));
    }
    READABLE_FUNCTION(runTimerHooks, Verified);

    // initTimers: the timers started (the pool, the two lists, no
    // widget nor visible timer). Returns 1.
    uint32_t initTimers()
    {
        *g_timerPool = newPool(static_cast<uint32_t>(sizeof(Timer)), kTimerPoolBlocks, 0);
        zge::initList(kTimers);
        zge::initList(kReleased);
        *g_timerWidget = {};
        *g_visibleTimer = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initTimers, Verified);

    // shutdownTimers: their end (the pool freed).
    void shutdownTimers()
    {
        freePool(*g_timerPool);
        *g_timerPool = 0;
        *g_timerWidget = {};
        *g_visibleTimer = 0;
    }
    READABLE_FUNCTION_WAITS(shutdownTimers, Verified);

    void stepTimer(uint32_t timerAddress, ee::Float step);

    // updateTimers (seconds, the game frame's): every timer stepped by
    // -seconds, then the released ones freed. Returns the last freeing's
    // (the released count with none).
    int32_t updateTimers(ee::Float seconds)
    {
        for (uint32_t timer = *g_firstTimer; timer != 0; timer = timerAt(timer)->next)
            stepTimer(timer, -seconds);

        int32_t result = *g_releasedCount;
        if (result <= 0)
            return result;
        while (*g_firstReleased)
        {
            const uint32_t timer = (*g_firstReleased)->timer;
            zge::listRemove(ee::Ptr<zge::List>(kTimers), ee::Ptr<zge::ListNode>(timer));
            zge::listRemove(ee::Ptr<zge::List>(kReleased), releasedNode(timer));
            result = freeTimerRecord(timer);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(updateTimers, Verified);

    // createTimer: a running timer. Returns it.
    uint32_t createTimer(uint32_t onExpire, ee::Address expireData, uint32_t onTick, ee::Address tickData, ee::Float seconds, ee::Float tickTime)
    {
        const uint32_t timerAt = newTimerRecord();
        Timer &timer = *ee::Ptr<Timer>(timerAt);
        timer.time = seconds;
        timer.onExpire = TimerHook{onExpire};
        timer.expireData = expireData;
        timer.tickTime = tickTime;
        timer.onTick = TimerHook{onTick};
        timer.tickData = tickData;
        timer.self = timerAt;
        timer.running = 1;
        zge::listPush(kTimers, timerAt);
        timerHook();
        return timerAt;
    }
    READABLE_FUNCTION_WAITS(createTimer, Verified);

    // releaseTimer (timer or 0): released (freed after the next update).
    // Returns the append's (for none, what the caller had).
    void releaseTimer(uint32_t timer)
    {
        if (timer != 0)
        {
            timerHook();
            game::tail(zge::listPush, kReleased, timer + offsetof(Timer, releasedNext));
        }
    }
    READABLE_FUNCTION(releaseTimer, Verified);

    // setTimer (timer, seconds): its time set.
    int32_t setTimer(uint32_t timer, ee::Float seconds)
    {
        const int32_t result = timerHookKeepingResult();
        timerAt(timer)->time = seconds;
        return result;
    }
    READABLE_FUNCTION(setTimer, Verified);

    // stepTimer (timer, step; each frame, -seconds): its time
    // moved by the step (a step up from 0 runs it again); at 0 it stops
    // (the time-up sound for the visible timer), never below; the visible
    // timer's widget shown the new whole seconds; then its hooks
    // (runTimerHooks). Returns theirs (the hook's for a 0 step).
    void stepTimer(uint32_t timerAddress, ee::Float step)
    {
        ee::Local<ee::Float> value;
        Timer &timer = *timerAt(timerAddress);
        timerHook();
        const ee::Float before = timer.time;
        if (step == ee::Float(0.0f))
        if (before <= ee::Float(0.0f))
        {
            if (ee::Float(0.0f) < step)
                timer.running = 1;
        }
        timer.time = timer.time + step;
        if (timer.time <= ee::Float(0.0f))
        {
            if (*g_visibleTimer == timerAddress && timer.running == 1)
                soundEvent(kTimeUpSound, 0, 0, 0, 0);
            timer.running = 0;
        }
        const uint32_t visible = *g_visibleTimer;
        timer.time = ee::max(timer.time, ee::Float(0.0f));
        if (visible == timerAddress && !(ee::toInt(before) == ee::toInt(timer.time)))
        {
            *value = timer.time;
            if (!(timer.time < ee::Float(0.0f)))
            {
                zgui::setVariable(*g_timerWidget, kTimeVariable, value);
                zgui::runScript(*g_timerWidget, ee::Ptr<char>(kRefresh), 0);
            }
        }
        runTimerHooks(timerAddress, before, timer.time);
    }
    READABLE_FUNCTION_WAITS(stepTimer, Verified);

    // timerTime (timer): its time.
    ee::Float timerTime(uint32_t timer)
    {
        timerHook();
        return timerAt(timer)->time;
    }
    READABLE_FUNCTION(timerTime, Verified);

    // setTimerTick (timer, tick time): its tick time set; -1 none (the
    // tick hook dropped too).
    int32_t setTimerTick(uint32_t timerAddress, ee::Float tickTime)
    {
        Timer &timer = *timerAt(timerAddress);
        const int32_t result = timerHookKeepingResult();
        if (tickTime == ee::Float(kNoTick))
        {
            timer.tickTime = kNoTick;
            timer.tickData = 0;
            timer.onTick = TimerHook{0};
        }
        else
            timer.tickTime = tickTime;
        return result;
    }
    READABLE_FUNCTION(setTimerTick, Verified);

    // timerTickTime (timer): its tick time.
    ee::Float timerTickTime(uint32_t timer)
    {
        timerHook();
        return timerAt(timer)->tickTime;
    }
    READABLE_FUNCTION(timerTickTime, Verified);

    // setTimerRunning (timer, running): it counting or not.
    void setTimerRunning(uint32_t timer, uint32_t running)
    {
        timerHook();
        timerAt(timer)->running = running;
    }
    READABLE_FUNCTION(setTimerRunning, Verified);

    // timerRunning (timer): whether it counts.
    uint32_t timerRunning(uint32_t timer)
    {
        timerHook();
        return timerAt(timer)->running;
    }
    READABLE_FUNCTION(timerRunning, Verified);

    // setVisibleTimer (timer): it the visible one: the HUD's timer widget
    // shown (or its hiding aborted), its freak-out stopped, its time put in
    // (unless below 0). Returns the last script's (timerTime's otherwise).
    int32_t setVisibleTimer(uint32_t timer)
    {
        ee::Local<uint32_t> visible;
        ee::Local<ee::Float> time;
        *visible = 0;
        zgui::copyVariable(timerWidget(), ee::Ptr<const char>(kVisibleVariable), ee::Ptr<char>(visible.address()));
        runTimerScript(*visible == 0 ? kShow : kAbortHide);
        runTimerScript(kStopFreakOut);
        *time = timerTime(timer);
        int32_t result = 0;
        if (!(*time < ee::Float(0.0f)))
        {
            zgui::setVariable(*g_timerWidget, kTimeVariable, time);
            result = runScriptResult(*g_timerWidget, kRefresh);
        }
        *g_visibleTimer = timer;
        return result;
    }
    READABLE_FUNCTION_WAITS(setVisibleTimer, Verified);

    // visibleTimer: the visible timer (0: none).
    uint32_t visibleTimer()
    {
        return *g_visibleTimer;
    }
    READABLE_FUNCTION(visibleTimer, Verified);

    // hideVisibleTimer: no visible timer: the widget hidden (or its showing
    // aborted). Returns the script's.
    int32_t hideVisibleTimer()
    {
        ee::Local<uint32_t> visible;
        *visible = 1;
        zgui::copyVariable(timerWidget(), ee::Ptr<const char>(kVisibleVariable), ee::Ptr<char>(visible.address()));
        const int32_t result = runScriptResult(timerWidget(), *visible != 0 ? kHide : kAbortShow);
        *g_visibleTimer = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(hideVisibleTimer, Verified);

    // timerHook: an empty hook every timer function calls first.
    void timerHook()
    {
    }
    READABLE_FUNCTION(timerHook, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_animalCallTicks> g_animalCallTicks; // g_animalCallTicks; its name is a low-confidence guess (readable_names.tsv)
    }

    int32_t stepParticleEmitters()
    {
        zge::searchTreeForEach(ee::Ptr<zge::SearchTree>(sym::g_animalCarrierTree), sym::updateCarriedAnimals, 0u);
        const int32_t animalCallTicks = *g_animalCallTicks + 1;
        *g_animalCallTicks = animalCallTicks;
        return animalCallTicks;
    }
    READABLE_FUNCTION_WAITS(stepParticleEmitters, Verified);
}
