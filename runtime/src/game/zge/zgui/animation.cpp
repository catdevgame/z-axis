// zgui's animations (script.h): PLAY and STOP's players, started after a
// delay, updated each frame, ended with their scripts.

#include "zge/zgui/script.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include "sym/zge/zgui/animation.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kPoolGive = sym::poolFree;   // (pool, block) (engine)
        constexpr uint32_t kHeapTake = sym::heapTake;   // (heap, size) -> a block (engine)
        constexpr uint32_t kHeapGive = sym::heapGive;   // (heap, block) (engine)
        constexpr uint32_t kListRemove = sym::listRemove; // (list, node) (engine)

        constexpr uint32_t kInterpolateStart = sym::clearNodeTimes, kInterpolateUpdate = sym::updateInterpolate; // clearNodeTimes, updateInterpolate
        constexpr uint32_t kLoopModeName = sym::g_textLoopMode, kInterpolateModeName = sym::g_textInterpolateMode, kSint32Type = sym::g_textSint323;
        constexpr uint32_t kIntervalModeName = sym::g_textIntervalMode, kIntervalValueName = sym::g_textIntervalValue;
        constexpr uint32_t kRatioName = sym::g_textRatio, kRatioType = sym::g_textZFloat3;
        constexpr uint32_t kScriptKind = sym::scriptKind; // "Script"
        constexpr uint32_t kOnStart = sym::g_textOnStart2;    // "OnStart"
        constexpr uint32_t kOnUpdate = sym::g_textOnUpdate;   // "OnUpdate"
        constexpr uint32_t kOnEnd = sym::g_textOnEnd;      // "OnEnd"
        // .sdata: a delay shorter than this is none (1e-6).
        inline ee::Global<ee::Float, sym::g_noDelay> g_noDelay;

        // The Interpolate kind (initAnimationNode sets it up; its Playing records
        // are 0x24 bytes): a ratio from 0 to 1 over an interval, eased or not,
        // once, looping or there and back.
        struct Interpolate
        {
            uint8_t _00[0x3C];          // Animation
            int32_t interpolateMode;    // +0x3C "InterpolateMode" (InterpolateMode)
            int32_t loopMode;           // +0x40 "LoopMode" (LoopMode)
            int32_t intervalMode;       // +0x44 "IntervalMode" (IntervalMode)
            int32_t intervalValue;      // +0x48 "IntervalValue": the interval, in its mode's units
            ee::Float ratio;            // +0x4C "Ratio": the value now
        };
        static_assert(offsetof(Interpolate, intervalValue) == 0x48 && offsetof(Interpolate, ratio) == 0x4C);
        struct PlayingInterpolate
        {
            uint8_t _00[0x18];          // Playing
            ee::Float elapsed;          // +0x18 into the interval (milliseconds or frames)
            int32_t backwards;          // +0x1C going back (LoopMode 2's way back)
            ee::Float ratio;            // +0x20
        };
        static_assert(sizeof(PlayingInterpolate) == 0x24);
        // ZGE2's Interpolate ACTION (DM2's initAnimationNode): the Interpolate's
        // fields after the ListedAction's, and its playing state in itself
        // (where ZGE3 has the PlayingInterpolate).
        struct ListedInterpolate
        {
            uint8_t action[offsetof(ListedAction, script)];
            ee::Float elapsed;          // +0x48 into the interval (milliseconds or frames)
            int32_t backwards;          // +0x4C going back (LoopMode 2's way back)
            int32_t interpolateMode;    // +0x50 "InterpolateMode"
            int32_t loopMode;           // +0x54 "LoopMode"
            int32_t intervalMode;       // +0x58 "IntervalMode"
            int32_t intervalValue;      // +0x5C "IntervalValue"
            ee::Float ratio;            // +0x60 "Ratio"
        };
        static_assert(offsetof(ListedInterpolate, elapsed) == 0x48 && offsetof(ListedInterpolate, interpolateMode) == 0x50 &&
                      offsetof(ListedInterpolate, ratio) == 0x60);
        enum IntervalMode : int32_t { kMilliseconds = 0, kFrames = 1, kAllAtOnce = 2 };
        enum InterpolateMode : int32_t { kLinear = 0, kEaseOut = 1, kRoot = 2 };
        enum LoopMode : int32_t { kOnce = 0, kLoop = 1, kPingPong = 2 };

        inline ee::Global<uint64_t, sym::g_easeNearlyThere> g_easeNearlyThere;   // 0.999 (a double)
        inline ee::Global<uint64_t, sym::g_easeNearlyBack> g_easeNearlyBack;    // 0.0001 (a double)
        inline ee::Global<ee::Float, sym::g_easeThere> g_easeThere;        // 1.0001: past the end
        inline ee::Global<ee::Float, sym::g_easeBack> g_easeBack;         // 0.0001

        // dt in whole milliseconds (fptoui) as a float, as the original
        // converts an unsigned: halved and doubled when the top bit is set.
        ee::Float milliseconds(ee::Float dt, uint32_t highLeader)
        {
            const uint32_t ms = sce::libgcc::fptoui(dt * ee::Float(1000.0f));
            if (static_cast<int32_t>(ms) >= 0)
                return ee::toFloat(static_cast<int32_t>(ms));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((ms & 1u) | (ms >> 1)));
            return half + half;
        }

        // The animation's script `event` (an Event variable of the node),
        // run for the playing record's target with the animation as "@".
        // True when it ran one.
        bool runEvent(ee::Ptr<Playing> playing, uint32_t event)
        {
            const ee::Ptr<uint32_t> value(variableValue(classCast<Node>(playing->animation), ee::Ptr<const char>(event)));
            if (!value || !*value)
                return false;
            runScript(playing->target, ee::Ptr<char>(*value), playing->animation);
            return true;
        }
    }

    uint32_t variableValue(ee::Ptr<Node> parent, ee::Ptr<const char> name)
    {
        if (!parent)
            return 0;
        if (!name)
            return 0;
        const ee::Ptr<Node> variable(findNode(parent, name, uint64_t{kVariableKind}));
        return variable ? variable->value : 0u;
    }
    READABLE_FUNCTION(variableValue, Verified);

    int32_t startPlaying(ee::Ptr<Playing> playing)
    {
        auto leave = [](int32_t result) {
            return result;
        };
        uint32_t flags = playing->flags & ~kPlayingOver & ~kPlayingStarted;
        playing->flags = flags;
        const ee::Ptr<Animation> animation(playing->animation);
        if (!(animation->delay <= playing->time))
            return leave(1);
        playing->flags = flags | kPlayingStarted;
        runEvent(playing, kOnStart);
        if (!playing->animation)
            return leave(1);
        if (const auto start = playing->animation->start)
            start(playing);
        if (!playing->animation)
            return leave(1);
        const auto update = playing->animation->update;
        if (!update)
            return leave(1);
        if (update(playing, ee::Float(0.0f)))
        {
            if (playing->animation)
            {
                if (!runEvent(playing, kOnUpdate))
                    return leave(1);
            }
            return leave(1);
        }
        // Over at its first update.
        if (playing->animation)
            runEvent(playing, kOnUpdate);
        if (playing->flags & kPlayingStarted)
            endPlaying(playing);
        return leave(0);
    }
    READABLE_FUNCTION_WAITS(startPlaying, Verified);

    void endPlaying(ee::Ptr<Playing> playing)
    {
        if (playing->animation)
        {
            runEvent(playing, kOnEnd);
            if (playing->animation)
            {
                if (const auto end = playing->animation->end)
                    end(playing);
            }
        }
        playing->time = 0.0f;
        playing->flags = (playing->flags | kPlayingOver) & ~kPlayingStarted;
    }
    READABLE_FUNCTION_WAITS(endPlaying, Verified);

    namespace
    {
        void heapGive(uint32_t heap, ee::Ptr<Playing> memory) { ee::game(kHeapGive, heap, memory); }
    }

    void updatePlaying(ee::Float seconds)
    {
        ee::Ptr<Playing> playing(g_playing->first);
        ee::Ptr<Playing> next;
        for (; playing;)
        {
            next = playing->next;
            const uint32_t flags = playing->flags;
            if (flags & kPlayingOver)
            {
                // freePlaying, with the original's blocks.
                endPlaying(playing);
                zge::listRemove(g_playing.get(), playing);
                if (playing->flags & kPlayingPooled)
                    zge::poolFree(ee::Ptr<Pool>(*g_playingPool), playing);
                else
                    heapGive(*g_heap, playing);
            }
            else
            {
                if (!(flags & kPlayingStarted))
                {
                    // Waiting out its delay.
                    playing->time = playing->time + seconds;
                    if (playing->animation->delay <= playing->time)
                    {
                        startPlaying(playing);
                    }
                }
                else
                {
                    bool over = true;
                    if (playing->animation->update)
                    {
                        runEvent(playing, kOnUpdate);
                        const auto update = playing->animation->update;
                        over = !update(playing, seconds);
                    }
                    if (over)
                        playing->flags = (playing->flags | kPlayingOver) & ~kPlayingStarted;
                }
            }
            playing = next;
            playing;
        }
    }
    READABLE_FUNCTION_WAITS(updatePlaying, Verified);

    namespace
    {
        uint32_t heapTake(uint32_t heap, uint32_t bytes) { return ee::game<uint32_t>(kHeapTake, heap, bytes); }
    }

    int32_t playWidget(ee::Ptr<Animation> animation, ee::Ptr<Node> target)
    {
        if (!target)
            target = animation->parent;
        // A script without delay runs at once.
        if (stringCompareNoCase(animation->kind, kScriptKind) == 0)
        {
            const bool now = ee::abs(animation->delay) < *g_noDelay;
            if (now)
            {
                runScript(target, animation->script, animation);
                return 1;
            }
        }
        // Already playing on the target: started again if it is over.
        ee::Ptr<Playing> playing(g_playing->first);
        if (playing)
            for (;;)
            {
                if (playing->animation == animation && playing->target == target)
                {
                    if (playing->flags & kPlayingOver)
                    {
                        endPlaying(playing);
                        if (playing->animation)
                            startPlaying(playing);
                    }
                    return 1;
                }
                playing = playing->next;
                if (!playing)
                    break;
            }
        // A new record: small ones from the pool, others from the heap.
        const uint32_t size = animation->playingSize;
        if (size < 0x31)
        {
            playing = poolAllocAs<Playing>(*g_playingPool);
            playing->flags = playing->flags | kPlayingPooled;
        }
        else
        {
            playing = ee::Ptr<Playing>(heapTake(*g_heap, size));
            playing->flags = playing->flags & ~kPlayingPooled;
        }
        playing->animation = animation;
        playing->target = target ? target : animation->parent;
        playing->time = 0.0f;
        listPush(g_playing.get(), playing);
        if (!startPlaying(playing))
        {
            // Over at once: freePlaying, with the original's blocks.
            zge::listRemove(g_playing.get(), playing);
            if (playing->flags & kPlayingPooled)
                zge::poolFree(ee::Ptr<Pool>(*g_playingPool), playing);
            else
                heapGive(*g_heap, playing);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(playWidget, Verified);

    int32_t stopPlaying(ee::Ptr<Animation> animation, ee::Ptr<Node> target)
    {
        if (!target)
            target = animation->parent;
        for (ee::Ptr<Playing> playing(g_playing->first);;)
        {
            if (!playing)
                break;
            if (playing->animation == animation && playing->target == target)
            {
                playing->flags = (playing->flags | kPlayingOver) & ~kPlayingStarted;
                break;
            }
            playing = playing->next;
        }
        return 1;
    }
    READABLE_FUNCTION(stopPlaying, Verified);

    // updateInterpolate: the Interpolate kind's update (playing, seconds) -> 0 once
    // over. The elapsed time moves on by the interval's units (milliseconds
    // of the frame, or one frame; back when going backwards), the ratio
    // follows (elapsed over the interval; eased: a step of (1 - ratio) *
    // interval * seconds, ending past 0.999 at 1.0001 or below 0.0001 at
    // 0.0001; or the root of 1 - elapsed over the interval), then the loop
    // mode: once ends past 1 (the ratio 1), a loop starts again (the ratio
    // less 1, the elapsed time 0), there and back turns at 1 and at 0. The
    // animation's Ratio is the result. IntervalMode 2 steps a frame at a
    // time, running OnUpdate after each, until the animation is over (or
    // its mode changes).
    //
    // OnUpdate is looked for among the Playing record's children (its +0x04,
    // the list's previous record), not the animation's, as the original
    // does; updatePlaying runs the animation's own each frame.
    int32_t updateInterpolate(ee::Ptr<Playing> playing, ee::Float seconds)
    {
        namespace here = points::updateInterpolate;
        const ee::Ptr<PlayingInterpolate> state = classCast<PlayingInterpolate>(playing);
        const ee::Ptr<Interpolate> animation = classCast<Interpolate>(playing->animation);
        int32_t on = 1;
        int32_t mode = animation->intervalMode;
        for (bool first = true;; first = false)
        {
            if (!first)
            {
                if (!on)
                    break;
            }

            // The elapsed time.
            bool stepped = true;
            ee::Float elapsed, step;
            bool forwards = true;
            if (mode == kFrames)
            {
                elapsed = state->elapsed;
                forwards = state->backwards == 0;
                step = 1.0f;
            }
            else if (mode == kMilliseconds)
            {
                forwards = state->backwards == 0;
                step = milliseconds(seconds, forwards ? here::taken7 : here::taken4);
                elapsed = state->elapsed;
            }
            else if (mode == kAllAtOnce)
            {
                elapsed = state->elapsed;
                forwards = state->backwards == 0;
                step = 1.0f;
            }
            else
                stepped = false;
            if (stepped)
            {
                if (forwards)
                    elapsed = elapsed + step;
                else
                    elapsed = elapsed - step;
                state->elapsed = elapsed;
            }

            // The ratio.
            const int32_t interpolate = animation->interpolateMode;
            if (interpolate == kEaseOut)
            {
                const ee::Float ratio = state->ratio;
                const ee::Float ease = (ee::Float(1.0f) - ratio) * ee::toFloat(animation->intervalValue) * seconds;
                if (state->backwards == 0)
                {
                    const ee::Float now = ratio + ease;
                    state->ratio = now;
                    const uint64_t asDouble = sce::libgcc::fptodp(now);
                    if (sce::libgcc::dpcmp(asDouble, *g_easeNearlyThere) > 0)
                        state->ratio = *g_easeThere;
                }
                else
                {
                    const ee::Float now = ratio - ease;
                    state->ratio = now;
                    const uint64_t asDouble = sce::libgcc::fptodp(now);
                    if (sce::libgcc::dpcmp(asDouble, *g_easeNearlyBack) < 0)
                        state->ratio = *g_easeBack;
                }
            }
            else if (interpolate == kLinear)
            {
                if constexpr (game::readable::kIsBmx) // (ZGE4: no interval, no division: a ratio of 0)
                    state->ratio = animation->intervalValue == 0 ? ee::Float(0.0f)
                                                                 : state->elapsed / ee::toFloat(animation->intervalValue);
                else
                {
                    const ee::Float ratio = state->elapsed / ee::toFloat(animation->intervalValue);
                    state->ratio = ratio;
                }
            }
            else if (interpolate >= kRoot)
            {
                if (interpolate == kRoot)
                {
                    const ee::Float elapsedNow = state->elapsed;
                    const ee::Float ratio = ee::sqrt(ee::Float(1.0f) - elapsedNow / ee::toFloat(animation->intervalValue));
                    state->ratio = ratio;
                }
            }

            // The loop mode.
            const int32_t loop = animation->loopMode;
            if (loop == kLoop)
            {
                const ee::Float ratio = state->ratio;
                if (ee::Float(1.0f) < ratio)
                {
                    state->elapsed = 0.0f; // the original's word store of 0
                    state->ratio = ratio - ee::Float(1.0f);
                }
            }
            else if (loop == kOnce)
            {
                if (ee::Float(1.0f) < state->ratio)
                {
                    state->ratio = 1.0f;
                    on = 0;
                }
            }
            else if (loop >= kPingPong)
            {
                if (loop == kPingPong)
                {
                    ee::Float ratio = state->ratio;
                    if (ee::Float(1.0f) < ratio)
                    {
                        state->backwards = 1;
                        ratio = ee::Float(1.0f) - (ratio - ee::Float(1.0f));
                        state->ratio = ratio;
                    }
                    if (ratio < ee::Float(0.0f))
                    {
                        state->backwards = 0;
                        state->ratio = -ratio;
                    }
                }
            }
            mode = animation->intervalMode;

            // All at once: a frame at a time, OnUpdate after each.
            if (mode != kAllAtOnce)
            {
                animation->ratio = state->ratio;
                return on;
            }
            if (on)
            {
                const ee::Ptr<uint32_t> onUpdate(variableValue(classCast<Node>(playing), ee::Ptr<const char>(kOnUpdate)));
                if (onUpdate && *onUpdate)
                    runScript(playing->target, ee::Ptr<char>(*onUpdate), playing->animation);
                mode = animation->intervalMode;
            }
            if (mode != kAllAtOnce)
                break;
        }
        animation->ratio = state->ratio;
        return on;
    }
    READABLE_FUNCTION_WAITS_OWN(updateInterpolate, Verified, dm2, updateListedInterpolate);

    // updateListedInterpolate: ZGE2's updateInterpolate, the Interpolate
    // ACTION's update (the ACTION, seconds) -> 0 once over: the same steps on
    // the playing state the ACTION holds itself. All at once, it runs the
    // ACTION's OnUpdate text after each frame's step whether it has one or
    // not (the text buffer is not cleared first), and goes on while the
    // interval mode stays what the step began with.
    int32_t updateListedInterpolate(ee::Ptr<ListedInterpolate> node, ee::Float seconds)
    {
        ee::Local<char[0x400]> script;
        const ee::Ptr<ListedAction> action = classCast<ListedAction>(node);
        int32_t on = 1;
        int32_t mode = node->intervalMode;
        for (;;)
        {
            const int32_t steppedMode = mode;

            // The elapsed time.
            if (steppedMode == kMilliseconds || steppedMode == kFrames || steppedMode == kAllAtOnce)
            {
                const bool forwards = node->backwards == 0;
                const ee::Float step = steppedMode == kMilliseconds ? milliseconds(seconds, 0) : ee::Float(1.0f);
                const ee::Float elapsed = node->elapsed;
                node->elapsed = forwards ? elapsed + step : elapsed - step;
            }

            // The ratio.
            const int32_t interpolate = node->interpolateMode;
            if (interpolate == kEaseOut)
            {
                const ee::Float ratio = node->ratio;
                const ee::Float ease = (ee::Float(1.0f) - ratio) * ee::toFloat(node->intervalValue) * seconds;
                if (node->backwards == 0)
                {
                    const ee::Float now = ratio + ease;
                    node->ratio = now;
                    if (sce::libgcc::dpcmp(sce::libgcc::fptodp(now), *g_easeNearlyThere) > 0)
                        node->ratio = *g_easeThere;
                }
                else
                {
                    const ee::Float now = ratio - ease;
                    node->ratio = now;
                    if (sce::libgcc::dpcmp(sce::libgcc::fptodp(now), *g_easeNearlyBack) < 0)
                        node->ratio = *g_easeBack;
                }
            }
            else if (interpolate == kLinear)
                node->ratio = node->elapsed / ee::toFloat(node->intervalValue);
            else if (interpolate == kRoot)
                node->ratio = ee::sqrt(ee::Float(1.0f) - node->elapsed / ee::toFloat(node->intervalValue));

            // The loop mode.
            const int32_t loop = node->loopMode;
            if (loop == kLoop)
            {
                const ee::Float ratio = node->ratio;
                if (ee::Float(1.0f) < ratio)
                {
                    node->elapsed = 0.0f;
                    node->ratio = ratio - ee::Float(1.0f);
                }
            }
            else if (loop == kOnce)
            {
                if (ee::Float(1.0f) < node->ratio)
                {
                    node->ratio = 1.0f;
                    on = 0;
                }
            }
            else if (loop == kPingPong)
            {
                ee::Float ratio = node->ratio;
                if (ee::Float(1.0f) < ratio)
                {
                    node->backwards = 1;
                    ratio = ee::Float(1.0f) - (ratio - ee::Float(1.0f));
                    node->ratio = ratio;
                }
                if (ratio < ee::Float(0.0f))
                {
                    node->backwards = 0;
                    node->ratio = -ratio;
                }
            }

            // All at once: a frame at a time, OnUpdate after each.
            if (steppedMode != kAllAtOnce)
                return on;
            if (on)
            {
                copyListedVariable(actionProperties(action), ee::Ptr<const char>(kOnUpdate), script);
                runScriptLines(classCast<Node>(action->parent), script);
                mode = node->intervalMode;
            }
            if (mode != steppedMode || !on)
                return on;
        }
    }

    // The Script animation kind (a UI script's "Script" child of a widget):
    // played, its text runs at once on the target with the animation as "@".
    namespace
    {
        constexpr uint32_t kScriptProperty = sym::scriptProperty;    // "Script"
        constexpr uint32_t kEventTypeName = sym::eventTypeName;     // "Event"
        constexpr uint32_t kOnStartName = sym::onStartName;       // "OnStart"
        constexpr uint32_t kOnUpdateName = sym::onUpdateName;      // "OnUpdate"
        constexpr uint32_t kOnEndName = sym::onEndName;         // "OnEnd"
    }

    // updateScriptAnimation: the Script kind's update: the script run; over at once (0).
    int32_t updateScriptAnimation(ee::Ptr<Playing> playing)
    {
        const ee::Ptr<Animation> animation(playing->animation);
        runScript(playing->target, animation->script, animation);
        return 0;
    }
    READABLE_FUNCTION_WAITS_OWN(updateScriptAnimation, Verified, dm2, runListedScript);

    // runListedScript: ZGE2's updateScriptAnimation, the Script ACTION's update:
    // its text run for its widget; over at once (0).
    int32_t runListedScript(ee::Ptr<ListedAction> action)
    {
        runScriptLines(classCast<Node>(action->parent), action->script);
        return 0;
    }

    // initScriptAnimation: a Script animation node set up: an animation with only
    // an update (no start or end), its OnStart, OnUpdate and OnEnd
    // properties taken away and its "Script" (Event) defined. The node.
    ee::Ptr<Animation> initScriptAnimation(ee::Ptr<Animation> node, ee::Ptr<const char> name, ee::Ptr<Node> widget)
    {
        namespace here = points::initScriptAnimation;
        initTimedNode(node, name, widget);
        node->start = {};
        node->end = {};
        node->script = ee::Ptr<char>(0u);
        node->update = sym::updateScriptAnimation;
        const ee::Ptr<Node> asNode = classCast<Node>(node);
        const uint32_t unused[] = {kOnStartName, kOnUpdateName, kOnEndName};
        for (const uint32_t property : unused)
        {
            const ee::Ptr<Node> found(findNode(asNode, ee::Ptr<const char>(property), static_cast<uint64_t>(NodeKind::Variable)));
            destroyNode(found);
        }
        defineTypedProperty(asNode, ee::Ptr<const char>(kScriptProperty), ee::Ptr<const char>(kEventTypeName), &node->script);
        return node;
    }

    // initListedScript: ZGE2's initScriptAnimation: a Script ACTION
    // (ListedAction) with only an update, its OnStart, OnUpdate and OnEnd
    // properties taken off its list and its "Script" (Event) listed.
    ee::Ptr<ListedAction> initListedScript(ee::Ptr<ListedAction> action, ee::Address name, ee::Ptr<zge::SceneObject> widget)
    {
        initListedAction(action, name, widget);
        action->start = 0;
        action->end = 0;
        action->script = ee::Ptr<char>(0u);
        action->update = sym::updateScriptAnimation;
        const ee::Ptr<zge::List> properties(valueAddress(action->properties));
        const uint32_t unused[] = {kOnStartName, kOnUpdateName, kOnEndName};
        for (const uint32_t property : unused)
            removeListedVariable(properties, ee::Ptr<const char>(property));
        addListedProperty(properties, ee::Ptr<const char>(kScriptProperty), ee::Ptr<const char>(kEventTypeName),
                          valueAddress(action->script));
        return action;
    }
    READABLE_FUNCTION_WAITS_OWN(initScriptAnimation, Verified, dm2, initListedScript);

    // initZguiPlayingPool: the small records PLAY takes (playingSize 0x30 at most).
    void initZguiPlayingPool()
    {
        *g_playingPool = zge::createPool(0x30, 10, 0);
    }
    READABLE_FUNCTION_WAITS(initZguiPlayingPool, Verified);

    // destroyZguiPlayingPool
    void destroyZguiPlayingPool()
    {
        game::tail(zge::destroyPool, ee::Ptr<zge::Pool>(*g_playingPool));
    }
    READABLE_FUNCTION_WAITS(destroyZguiPlayingPool, Verified);

    // stopAllPlaying: every playing of an animation over, on any target (nothing
    // calls it).
    int32_t stopAllPlaying(ee::Ptr<Animation> animation)
    {
        ee::Ptr<Playing> playing(g_playing->first);
        if (playing)
        {
            for (;;)
            {
                if (playing->animation == animation)
                    playing->flags = (playing->flags | kPlayingOver) & ~kPlayingStarted;
                playing = playing->next;
                if (!playing)
                    break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(stopAllPlaying, Verified);

    // stopComponentScript: an animation's playing on a target (its parent if none)
    // over; its animation word is cleared too, so the list's update frees it
    // without its OnEnd.
    int32_t stopComponentScript(ee::Address animationNode, ee::Ptr<Node> target)
    {
        const ee::Ptr<Animation> animation(animationNode);
        if (!target)
            target = animation->parent;
        for (ee::Ptr<Playing> playing(g_playing->first); playing; playing = playing->next)
        {
            if (playing->animation == animation && playing->target == target)
            {
                playing->animation = {};
                playing->flags = (playing->flags & ~kPlayingStarted) | kPlayingOver;
                break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(stopComponentScript, Verified);

    // unlinkPlayingNode: the first playing of an animation over, its word
    // cleared.
    void unlinkPlayingNode(ee::Address animationNode)
    {
        const ee::Ptr<Animation> animation(animationNode);
        for (ee::Ptr<Playing> playing(g_playing->first); playing; playing = playing->next)
        {
            if (playing->animation == animation)
            {
                playing->animation = {};
                playing->flags = (playing->flags & ~kPlayingStarted) | kPlayingOver;
                break;
            }
        }
    }
    READABLE_FUNCTION(unlinkPlayingNode, Verified);

    // clearNodeTimes: the Interpolate kind's start.
    void clearNodeTimes(ee::Ptr<PlayingInterpolate> playing)
    {
        playing->backwards = 0;
        playing->ratio = 0;
        playing->elapsed = 0;
    }
    READABLE_FUNCTION_OWN(clearNodeTimes, Verified, dm2, resetListedInterpolate);

    // resetListedInterpolate: ZGE2's clearNodeTimes, the Interpolate ACTION's
    // start: its playing state in the ACTION itself.
    void resetListedInterpolate(ee::Ptr<ListedInterpolate> node)
    {
        node->backwards = 0;
        node->ratio = 0;
        node->elapsed = 0;
    }

    // initAnimationNode: the Interpolate kind's set-up: what an ACTION has
    // (initTimedNode), its modes as properties, its Ratio.
    uint32_t initAnimationNode(uint32_t address, uint32_t name, ee::Ptr<Node> parent)
    {
        const ee::Ptr<Interpolate> node(address);
        const ee::Ptr<Animation> animation(address);
        initTimedNode(address, name, parent);
        animation->playingSize = sizeof(PlayingInterpolate);
        node->loopMode = 0;
        node->interpolateMode = 0;
        node->intervalMode = 0;
        node->intervalValue = 0;
        animation->end = {};
        animation->start = kInterpolateStart;
        animation->update = kInterpolateUpdate;
        const auto define = [&](uint32_t propertyName, uint32_t value)
        {
            defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(propertyName), ee::Ptr<const char>(kSint32Type), address + value);
        };
        define(kInterpolateModeName, offsetof(Interpolate, interpolateMode));
        define(kLoopModeName, offsetof(Interpolate, loopMode));
        define(kIntervalModeName, offsetof(Interpolate, intervalMode));
        define(kIntervalValueName, offsetof(Interpolate, intervalValue));
        addVariableNode(ee::Ptr<Node>(address), ee::Ptr<const char>(kRatioName), ee::Ptr<const char>(kRatioType), address + offsetof(Interpolate, ratio));
        return address;
    }
    READABLE_FUNCTION_OWN(initAnimationNode, Verified, dm2, initListedInterpolate);

    // initListedInterpolate: ZGE2's initAnimationNode: the Interpolate ACTION
    // set up over a ListedAction, its properties ListedVariables on it (Ratio
    // one of them, where ZGE3 adds a locked variable).
    uint32_t initListedInterpolate(ee::Address address, ee::Address name, ee::Ptr<zge::SceneObject> parent)
    {
        const ee::Ptr<ListedInterpolate> node(address);
        const ee::Ptr<ListedAction> action(address);
        initListedAction(address, name, parent);
        node->loopMode = 0;
        node->interpolateMode = 0;
        node->intervalMode = 0;
        node->intervalValue = 0;
        node->backwards = 0;
        node->elapsed = 0;
        node->ratio = 0;
        action->end = {};
        action->start = kInterpolateStart;
        action->update = kInterpolateUpdate;
        const auto define = [&](uint32_t propertyName, uint32_t typeName, uint32_t value)
        {
            addListedProperty(actionProperties(action), ee::Ptr<const char>(propertyName), ee::Ptr<const char>(typeName),
                              address + value);
        };
        define(kInterpolateModeName, kSint32Type, offsetof(ListedInterpolate, interpolateMode));
        define(kLoopModeName, kSint32Type, offsetof(ListedInterpolate, loopMode));
        define(kIntervalModeName, kSint32Type, offsetof(ListedInterpolate, intervalMode));
        define(kIntervalValueName, kSint32Type, offsetof(ListedInterpolate, intervalValue));
        define(kRatioName, kRatioType, offsetof(ListedInterpolate, ratio));
        return address;
    }
}

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kActionOnEndName = sym::dm2::g_textActionOnEnd; // "OnEnd"
        constexpr uint32_t kActionOnStartName = sym::dm2::g_textActionOnStart;   // "OnStart"
        constexpr uint32_t kActionOnUpdateName = sym::dm2::g_textActionOnUpdate; // "OnUpdate"
    }

    // stopListedAction (ZGE2 only): the root's StopAction handler: the ACTION
    // in the message's word, if it is playing, is marked over and no longer
    // started (the action player ends it). 1.
    inline int32_t stopListedAction(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Address stopping = message->data;
        for (ee::Ptr<zge::ListNode> entry = g_playingActions->head; entry; entry = entry->next)
        {
            const ee::Ptr<PlayingAction> playing = zge::recordOfNode<PlayingAction>(entry);
            if (ee::Address(playing->action) != stopping)
                continue;
            playing->action->over = 1;
            playing->action->started = 0;
            break;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, stopListedAction);

    // endListedAction (ZGE2 only): the root's EndAction handler: the ACTION in
    // the message's word, if it is playing, is taken off the playing list
    // and ended (its OnEnd script run for its widget, its kind's end), and
    // its entry freed. 1.
    inline int32_t endListedAction(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x400]> script;
        const ee::Address ending = message->data;
        for (ee::Ptr<zge::ListNode> entry = g_playingActions->head; entry; entry = entry->next)
        {
            const ee::Ptr<PlayingAction> playing = zge::recordOfNode<PlayingAction>(entry);
            if (ee::Address(playing->action) != ending)
                continue;
            zge::listRemove(&*g_playingActions, playing);
            script[0] = 0;
            copyListedVariable(actionProperties(playing->action), ee::Ptr<const char>(kActionOnEndName), script);
            if (script[0])
                runScriptLines(classCast<Node>(playing->action->parent), script);
            if (const auto end = playing->action->end)
                end(playing->action);
            zguiRelease(*g_heap, playing);
            break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, endListedAction);
}

namespace zge::zgui
{
    // playListedAction (ZGE2 only): the root's PlayAction handler: the ACTION
    // in the message's word starts playing. One playing already goes on
    // unless it is over; then it is ended (OnEnd, its kind's end) and started
    // again in its entry. A new one gets an entry on the playing list.
    // Starting: its time is 0; if its delay is over by then, OnStart and its
    // kind's start run, and with an update OnUpdate and a first update, which
    // answers 0 once the ACTION is over. 1.
    inline int32_t playListedAction(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x400]> script;
        const ee::Ptr<ListedAction> action(static_cast<uint32_t>(message->data));
        // An Event of the ACTION run for its widget, if it has a text.
        const auto runActionEvent = [&](uint32_t name) {
            script[0] = 0;
            copyListedVariable(actionProperties(action), ee::Ptr<const char>(name), script);
            if (script[0])
                runScriptLines(classCast<Node>(action->parent), script);
        };
        // The ACTION past its delay: started, and updated once if it has an update.
        const auto start = [&]() {
            action->started = 1;
            runActionEvent(kActionOnStartName);
            if (const auto kindStart = action->start)
                kindStart(action);
            if (const auto update = action->update)
            {
                runActionEvent(kActionOnUpdateName);
                if (update(action, action->time) == 0)
                {
                    action->over = 1;
                    action->started = 0;
                }
            }
        };

        for (ee::Ptr<zge::ListNode> entry = g_playingActions->head; entry; entry = entry->next)
        {
            if (zge::recordOfNode<PlayingAction>(entry)->action != action)
                continue;
            if (!action->over)
                return 1;
            runActionEvent(kActionOnEndName);
            if (const auto end = action->end)
                end(action);
            action->time = 0;
            action->over = 0;
            action->started = 0;
            if (action->delay <= action->time)
                start();
            return 1;
        }

        action->time = 0;
        action->over = 0;
        action->started = 0;
        if (action->delay <= action->time)
            start();
        const ee::Ptr<PlayingAction> playing(zguiAllocate(*g_heap, sizeof(PlayingAction)));
        playing->action = action;
        zge::listPush(&*g_playingActions, playing);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, playListedAction);
}
