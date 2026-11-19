// zgui's frame (widget.h): its time, the animations, the pads' input to the
// focus widget, the nodes queued for destruction.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/collision/collision.h"
#include "zge/scene/message.h"
#include "sym/zge/zgui/frame.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kDestroyNode = sym::destroyNode;  // (node) the node and its children
        constexpr uint32_t kHeapGive = sym::heapGive;     // (heap, block) (engine)

        inline ee::Global<uint32_t, sym::g_lastMilliseconds> g_lastMilliseconds;  // gp-0x6040
        inline ee::Global<ee::Float, sym::g_secondsPerTick> g_secondsPerTick;    // 0.001
        inline ee::Global<ee::Float, sym::g_repeatInterval> g_repeatInterval;    // 0.05 s between repeats
        constexpr float kRepeatDelay = 0.5f;                           // before the first repeat
        constexpr float kStickPress = 0.75f;                           // a stick past this presses a direction

        // The d-pad bits a stick presses (the mask's).
        constexpr uint32_t kStickRight = 0x8, kStickLeft = 0x4, kStickUp = 0x2, kStickDown = 0x1;
        // Directions whose repeats do not count as presses (L2, R2, L1, R1 by
        // the button order, inferred), and one that never repeats.
        constexpr uint32_t kNoRepeat = 0x10;

        struct AxisBlocks
        {
            uint32_t notHigh, store, inside, insideLow, pressAgain, join, after;
        };
        constexpr AxisBlocks kAxisX{points::updateZgui::taken9, points::updateZgui::taken10, points::updateZgui::taken11, points::updateZgui::taken12, points::updateZgui::taken13, points::updateZgui::join, points::updateZgui::taken14};
        constexpr AxisBlocks kAxisY{points::updateZgui::taken15, points::updateZgui::taken16, points::updateZgui::taken17, points::updateZgui::taken18, points::updateZgui::taken19, points::updateZgui::join2, 0};

        // A stick axis as d-pad presses: past the threshold it holds its
        // direction (`downHigh` / `downLow` into down, pressed when it gets
        // there); back inside, the direction it left is pressed again unless
        // `heldHigh` / `heldLow` (a bit of down) says the button itself is held.
        void stickAxis(PadInput &input, ee::Float value, ee::Float &last, uint32_t downHigh, uint32_t downLow,
                       uint32_t pressHigh, uint32_t pressLow, uint32_t heldHighBit, uint32_t heldLowBit,
                       const AxisBlocks &blocks)
        {
            const ee::Float high = kStickPress, low = -kStickPress;
            // The paths' ends: the x axis' held-high path skips its join (the
            // threshold still in its register), the y axis' has only the join.
            auto end = [&](bool skipJoin) {
            };
            auto press = [&](uint32_t bits) {
                last = value;
                input.pressed |= input.mask & bits;
                end(false);
            };
            if (high <= value)
            {
                input.down |= downHigh;
                if (last < high)
                    return press(pressHigh);
                return end(true);
            }
            if (value <= low)
            {
                input.down |= downLow;
                if (low < last)
                    return press(pressLow);
                return end(false);
            }
            uint32_t heldBit, pressBits;
            if (high <= last)
            {
                heldBit = heldHighBit;
                pressBits = pressHigh;
            }
            else
            {
                if (!(last <= low))
                    return end(false);
                heldBit = heldLowBit;
                pressBits = pressLow;
            }
            if (!((input.down >> heldBit) & 1))
                input.pressed |= input.mask & pressBits;
            last = 0.0f;
            end(false);
        }
    }

    namespace
    {
        void heapGive(ee::Address heap, ee::Address memory) { ee::game(kHeapGive, heap, memory); }
    }

    // destroyQueuedNodes (ZGE4 only): the nodes queued for destruction
    // destroyed, each row taken off the queue and given back (ZGE3 does this
    // at the end of updateZgui).
    inline void destroyQueuedNodes()
    {
        Zgui &zgui = *g_zgui;
        while (zgui.destroyQueue.head)
        {
            const ee::Ptr<DestroyRow> row = classCast<DestroyRow>(zgui.destroyQueue.head);
            destroyNode(row->node);
            listRemove(&zgui.destroyQueue, row);
            heapGive(zgui.heap, row);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, destroyQueuedNodes);

    int32_t updateZgui()
    {
        Zgui &zgui = *g_zgui;
        // The frame's time.
        const uint32_t now = collisionClock();
        if (now < *g_lastMilliseconds)
            zgui.seconds = 0.0f;
        else
        {
            // CVT.S.W of an unsigned count: halved (keeping the low bit) and doubled past 2^31.
            const uint32_t ticks = now - *g_lastMilliseconds;
            ee::Float t;
            if (static_cast<int32_t>(ticks) >= 0)
                t = ee::toFloat(static_cast<int32_t>(ticks));
            else
            {
                t = ee::toFloat(static_cast<int32_t>((ticks >> 1) | (ticks & 1)));
                t = t + t;
            }
            zgui.seconds = t * *g_secondsPerTick;
        }
        *g_lastMilliseconds = now;
        if constexpr (game::readable::kIsBmx) // ZGE4's root FileOperationCount: the file requests in flight
            setFileOperationCount(zgui, zge::fileRequestsInFlight());
        updatePlaying(zgui.seconds);

        // The pads.
        zgui.input.idleFrames += 1;
        zgui.input.down = 0;
        zgui.input.pressed = 0;
        for (uint32_t i = 0;;)
        {
            PadInput &input = zgui.pads[i];
            const ee::Ptr<Pad> pad = input.pad;
            if (!pad || !(pad->flags & 1))
            {
                // None: nothing held, nothing repeats (the original clears
                // the two words once per repeat timer).
                for (uint32_t b = 0;;)
                {
                    input.pressed = 0;
                    input.down = 0;
                    input.repeat[b] = 0.0f;
                    if (++b == 32)
                        break;
                }
            }
            else
            {
                input.down = pad->down & input.mask;
                input.pressed = pad->pressed & input.mask;
                ee::Float x = 0.0f;
                ee::Float y = 0.0f;
                if (input.mask & kStickLeft)
                    x = pad->stickX;
                if (input.mask & kStickDown)
                    y = pad->stickY;
                if (zgui.stickAsDpad)
                {
                    Stick &stick = (*g_sticks)[i];
                    stickAxis(input, x, stick.x, kStickRight, kStickLeft, kStickRight, kStickLeft, 8, 4, kAxisX);
                    stickAxis(input, y, stick.y, input.mask & kStickUp, input.mask & kStickDown, kStickUp,
                              kStickDown, 2, 1, kAxisY);
                }
                zgui.input.down |= input.down;
                zgui.input.pressed |= input.pressed;
                // Held buttons repeat: after half a second, then every
                // repeat interval (as presses, but for a few buttons).
                for (uint32_t b = 0;;)
                {
                    bool idle = true; // the idle count's reset
                    if ((input.pressed >> b) & 1)
                        input.repeat[b] = kRepeatDelay;
                    else
                    {
                        if (!((input.down >> b) & 1))
                            idle = false;
                        else
                        {
                            const ee::Float left = input.repeat[b] - zgui.seconds;
                            input.repeat[b] = left;
                            if (left <= ee::Float(0.0f))
                            {
                                const uint32_t bit = 1u << b;
                                input.repeat[b] = *g_repeatInterval;
                                if constexpr (game::readable::kIsBmx)
                                {
                                    // ZGE4 repeats the d-pad alone.
                                    if (bit == kStickDown || bit == kStickUp || bit == kStickLeft || bit == kStickRight)
                                        zgui.input.pressed |= bit;
                                }
                                // Three buttons get no repeated press.
                                else if (bit == 0x20 || bit == 0x40 || bit == 0x80)
                                {
                                    zgui.input.idleFrames = 0;
                                    idle = false;
                                }
                                else if (bit != kNoRepeat)
                                    zgui.input.pressed |= bit;
                            }
                        }
                    }
                    if (idle)
                        zgui.input.idleFrames = 0;
                    if (++b == 32)
                        break;
                }
            }
            if (++i == 4)
                break;
        }

        // The focus widget hears of it.
        const bool hears = zgui.focus && zgui.input.down && zgui.input.pressed;
        if (hears)
            sendMessage(zgui.focus, kInputMessage, &zgui.input);

        // The nodes queued for destruction (ZGE4: its own function).
        if constexpr (game::readable::kIsBmx)
            destroyQueuedNodes();
        else
        {
            while (zgui.destroyQueue.head)
            {
                const ee::Ptr<DestroyRow> row = classCast<DestroyRow>(zgui.destroyQueue.head);
                destroyNode(row->node);
                listRemove(&zgui.destroyQueue, row);
                heapGive(zgui.heap, row);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(updateZgui, Verified, dm2, updateZguiObjects);

    namespace
    {
        constexpr uint32_t kActionOnEnd = sym::dm2::g_textActionOnEnd;       // "OnEnd"
        constexpr uint32_t kActionOnStart = sym::dm2::g_textActionOnStart;   // "OnStart"
        constexpr uint32_t kActionOnUpdate = sym::dm2::g_textActionOnUpdate; // "OnUpdate"

        // ZGE2's playing ACTIONs stepped by the frame's time: one that is over
        // is ended (OnEnd, its kind's end) and its entry freed; one in its
        // delay waits, then starts (OnStart, its kind's start, a first update
        // of no time); a started one is updated. An update runs OnUpdate
        // first and answers 0 once the ACTION is over; an ACTION without an
        // update is over at once.
        void stepPlayingActions(ee::Local<char[0x400]> &script, ee::Float seconds)
        {
            // An Event of the ACTION run for its widget, if it has a text.
            const auto runEvent = [&](ee::Ptr<ListedAction> action, uint32_t name) {
                script[0] = 0;
                copyListedVariable(actionProperties(action), ee::Ptr<const char>(name), script);
                if (script[0])
                    runScriptLines(ee::Ptr<Node>(action->parent.address), script);
            };
            const auto over = [](ee::Ptr<ListedAction> action) {
                action->over = 1;
                action->started = 0;
            };
            ee::Ptr<zge::ListNode> next;
            for (ee::Ptr<zge::ListNode> entry = g_playingActions->head; entry; entry = next)
            {
                const ee::Ptr<PlayingAction> playing = zge::recordOfNode<PlayingAction>(entry);
                next = entry->next;
                if (playing->action->over)
                {
                    zge::listRemove(g_playingActions.address, playing);
                    runEvent(playing->action, kActionOnEnd);
                    if (const auto end = playing->action->end)
                        end(playing->action);
                    playing->action->started = 0;
                    zguiRelease(*g_heap, playing);
                    continue;
                }
                if (!playing->action->started)
                {
                    playing->action->time = playing->action->time + seconds;
                    if (!(playing->action->delay <= playing->action->time))
                        continue;
                    playing->action->over = 0;
                    playing->action->started = 1;
                    runEvent(playing->action, kActionOnStart);
                    if (const auto start = playing->action->start)
                        start(playing->action);
                    if (!playing->action->update)
                        continue;
                    runEvent(playing->action, kActionOnUpdate);
                    if (playing->action->update(playing->action, ee::Float(0.0f)) == 0)
                        over(playing->action);
                    continue;
                }
                if (!playing->action->update)
                {
                    over(playing->action);
                    continue;
                }
                runEvent(playing->action, kActionOnUpdate);
                if (playing->action->update(playing->action, seconds) == 0)
                    over(playing->action);
            }
        }
    }

    // updateZguiObjects: ZGE2's updateZgui: the frame's time kept only for
    // this frame, the playing ACTIONs stepped here (stepPlayingActions), the
    // pads read as ZGE3 reads them, the Input sent at once from the root to
    // the focus widget's object, then the root's waiting messages delivered
    // (ZGE2 has no destroy queue: a Destroy message deletes its object). 1.
    int32_t updateZguiObjects()
    {
        ee::Local<char[0x400]> script;
        Zgui &zgui = *g_zgui;
        // The frame's time.
        const uint32_t now = collisionClock();
        ee::Float seconds;
        if (now < *g_lastMilliseconds)
            seconds = 0.0f;
        else
        {
            // CVT.S.W of an unsigned count: halved (keeping the low bit) and doubled past 2^31.
            const uint32_t ticks = now - *g_lastMilliseconds;
            ee::Float t;
            if (static_cast<int32_t>(ticks) >= 0)
                t = ee::toFloat(static_cast<int32_t>(ticks));
            else
            {
                t = ee::toFloat(static_cast<int32_t>((ticks >> 1) | (ticks & 1)));
                t = t + t;
            }
            seconds = t * *g_secondsPerTick;
        }
        *g_lastMilliseconds = now;
        stepPlayingActions(script, seconds);

        // The pads (as updateZgui's).
        zgui.input.idleFrames += 1;
        zgui.input.down = 0;
        zgui.input.pressed = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
            PadInput &input = zgui.pads[i];
            const ee::Ptr<Pad> pad = input.pad;
            if (!pad || !(pad->flags & 1))
            {
                for (uint32_t b = 0; b < 32; ++b)
                {
                    input.pressed = 0;
                    input.down = 0;
                    input.repeat[b] = 0.0f;
                }
                continue;
            }
            input.down = pad->down & input.mask;
            input.pressed = pad->pressed & input.mask;
            ee::Float x = 0.0f;
            ee::Float y = 0.0f;
            if (input.mask & kStickLeft)
                x = pad->stickX;
            if (input.mask & kStickDown)
                y = pad->stickY;
            if (zgui.stickAsDpad)
            {
                Stick &stick = (*g_sticks)[i];
                stickAxis(input, x, stick.x, kStickRight, kStickLeft, kStickRight, kStickLeft, 8, 4, kAxisX);
                stickAxis(input, y, stick.y, input.mask & kStickUp, input.mask & kStickDown, kStickUp, kStickDown, 2, 1,
                          kAxisY);
            }
            zgui.input.down |= input.down;
            zgui.input.pressed |= input.pressed;
            // Held buttons repeat: after half a second, then every repeat
            // interval (as presses, but for a few buttons).
            for (uint32_t b = 0; b < 32; ++b)
            {
                if ((input.pressed >> b) & 1)
                    input.repeat[b] = kRepeatDelay;
                else
                {
                    if (!((input.down >> b) & 1))
                        continue;
                    const ee::Float left = input.repeat[b] - seconds;
                    input.repeat[b] = left;
                    if (left <= ee::Float(0.0f))
                    {
                        const uint32_t bit = 1u << b;
                        input.repeat[b] = *g_repeatInterval;
                        if (bit != kNoRepeat && bit != 0x20 && bit != 0x40 && bit != 0x80)
                            zgui.input.pressed |= bit;
                    }
                }
                zgui.input.idleFrames = 0;
            }
        }

        // The focus widget hears of it.
        if (zgui.focus && zgui.input.down && zgui.input.pressed)
            zge::sendNewMessage(zgui.root, zgui.focus, kInputMessage, &zgui.input, zge::kMessageNow);
        zge::deliverObjectMessages(zgui.root.address);
        return 1;
    }
}
