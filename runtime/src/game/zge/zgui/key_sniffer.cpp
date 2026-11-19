// zgui's key sniffer: a widget that shows nothing and turns the pads into
// script calls: one Event per button, and the sticks' axes as variables. Its
// Draw handler, which reads the pads, is drawGuiKeySniffer.

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "zge/zgui/script.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/key_sniffer.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kDrawGuiKeySniffer = sym::drawGuiKeySniffer;

        // ZGE2's (DM2's) key sniffer record, after WidgetRecord (widget_parts.h):
        // no EnableAnalogDir, AutoRepeat, OnAccept, OnCancel or OnOption.
        struct ListedKeySniffer
        {
            WidgetRecord widget;
            uint32_t active;           // +0x60
            uint32_t flags;            // +0x64
            int32_t curCtrl;           // +0x68 the pad it reads
            ee::Float leftAxisX;       // +0x6C
            ee::Float leftAxisY;       // +0x70
            ee::Float rightAxisX;      // +0x74
            ee::Float rightAxisY;      // +0x78
            uint32_t onGetFocus;       // +0x7C each a script (Event)
            uint32_t onLostFocus;      // +0x80
            uint32_t onStart;          // +0x84
            uint32_t onSelect;         // +0x88
            uint32_t onFire1;          // +0x8C
            uint32_t onFire2;          // +0x90
            uint32_t onFire3;          // +0x94
            uint32_t onFire4;          // +0x98
            uint32_t onLeftStickMove;  // +0x9C
            uint32_t onRightStickMove; // +0xA0
            uint32_t onLTrig1;         // +0xA4
            uint32_t onLTrig2;         // +0xA8
            uint32_t onRTrig1;         // +0xAC
            uint32_t onRTrig2;         // +0xB0
            uint32_t onUpKey;          // +0xB4
            uint32_t onDownKey;        // +0xB8
            uint32_t onRightKey;       // +0xBC
            uint32_t onLeftKey;        // +0xC0
        };
        static_assert(offsetof(ListedKeySniffer, active) == 0x60 && offsetof(ListedKeySniffer, onGetFocus) == 0x7C);
        static_assert(offsetof(ListedKeySniffer, onFire1) == 0x8C && offsetof(ListedKeySniffer, onLeftKey) == 0xC0);

        constexpr ListedProperty kListedKeySnifferProperties[] = {
            {sym::g_textActive2, sym::g_textBool, offsetof(ListedKeySniffer, active)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedKeySniffer, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedKeySniffer, onLostFocus)},
            {sym::g_textFlags, sym::g_textSint32, offsetof(ListedKeySniffer, flags)},
            {sym::g_textCurCtrl, sym::g_textSint32, offsetof(ListedKeySniffer, curCtrl)},
            {sym::g_textLeftAxisX, sym::g_textZFloat, offsetof(ListedKeySniffer, leftAxisX)},
            {sym::g_textLeftAxisY, sym::g_textZFloat, offsetof(ListedKeySniffer, leftAxisY)},
            {sym::g_textRightAxisX, sym::g_textZFloat, offsetof(ListedKeySniffer, rightAxisX)},
            {sym::g_textRightAxisY, sym::g_textZFloat, offsetof(ListedKeySniffer, rightAxisY)},
            {sym::g_textOnStart, sym::g_textEvent, offsetof(ListedKeySniffer, onStart)},
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(ListedKeySniffer, onSelect)},
            {sym::g_textOnFire1, sym::g_textEvent, offsetof(ListedKeySniffer, onFire1)},
            {sym::g_textOnFire2, sym::g_textEvent, offsetof(ListedKeySniffer, onFire2)},
            {sym::g_textOnFire3, sym::g_textEvent, offsetof(ListedKeySniffer, onFire3)},
            {sym::g_textOnFire4, sym::g_textEvent, offsetof(ListedKeySniffer, onFire4)},
            {sym::g_textOnLeftKey, sym::g_textEvent, offsetof(ListedKeySniffer, onLeftKey)},
            {sym::g_textOnUpKey, sym::g_textEvent, offsetof(ListedKeySniffer, onUpKey)},
            {sym::g_textOnDownKey, sym::g_textEvent, offsetof(ListedKeySniffer, onDownKey)},
            {sym::g_textOnRightKey, sym::g_textEvent, offsetof(ListedKeySniffer, onRightKey)},
            {sym::g_textOnLeftStickMove, sym::g_textEvent, offsetof(ListedKeySniffer, onLeftStickMove)},
            {sym::g_textOnRightStickMove, sym::g_textEvent, offsetof(ListedKeySniffer, onRightStickMove)},
            {sym::g_textOnLTrig1, sym::g_textEvent, offsetof(ListedKeySniffer, onLTrig1)},
            {sym::g_textOnLTrig2, sym::g_textEvent, offsetof(ListedKeySniffer, onLTrig2)},
            {sym::g_textOnRTrig1, sym::g_textEvent, offsetof(ListedKeySniffer, onRTrig1)},
            {sym::g_textOnRTrig2, sym::g_textEvent, offsetof(ListedKeySniffer, onRTrig2)},
        };

    }

    // defineGuiKeySnifferProperties: a key sniffer's defaults (all pads' flags 3, auto repeat
    // on), its axes as variables, its properties.
    uint32_t defineGuiKeySnifferProperties(uint32_t node, uint32_t text)
    {
        namespace here = points::defineGuiKeySnifferProperties;
        const ee::Ptr<GuiKeySniffer> sniffer(node);
        defineWidgetProperties(node, text);
        sniffer->onGetFocus = 0;
        sniffer->onLostFocus = 0;
        sniffer->curCtrl = 0;
        sniffer->active = 0;
        ee::Ptr<GuiKeySniffer>(node)->leftAxisX = 0;
        ee::Ptr<GuiKeySniffer>(node)->leftAxisY = 0;
        ee::Ptr<GuiKeySniffer>(node)->rightAxisX = 0;
        ee::Ptr<GuiKeySniffer>(node)->rightAxisY = 0;
        sniffer->onStart = 0;
        sniffer->onSelect = 0;
        sniffer->onFire1 = 0;
        sniffer->onFire2 = 0;
        sniffer->onFire3 = 0;
        sniffer->onFire4 = 0;
        sniffer->onLeftStickMove = 0;
        sniffer->onRightStickMove = 0;
        sniffer->onLTrig1 = 0;
        sniffer->onLTrig2 = 0;
        sniffer->onRTrig1 = 0;
        sniffer->onRTrig2 = 0;
        sniffer->onUpKey = 0;
        sniffer->onDownKey = 0;
        sniffer->onRightKey = 0;
        sniffer->onLeftKey = 0;
        sniffer->onAccept = 0;
        sniffer->onCancel = 0;
        sniffer->onOption = 0;
        if constexpr (game::readable::kIsBmx)
            clearZge4SnifferFields(sniffer);
        sniffer->enableAnalogDir = 0;
        sniffer->flags = 3;
        sniffer->autoRepeat = 1;
        addListener(node, Message::Draw, kDrawGuiKeySniffer);
        addListener(node, Message::GetFocus, kRunOnGetFocus);
        addListener(node, Message::LostFocus, kRunOnLostFocus);
        constexpr uint32_t kSint32 = sym::g_textSint32, kZFloat = sym::g_textZFloat;
        addVariableNode(ee::Ptr<Node>(node), ee::Ptr<const char>(sym::g_textCurCtrl), ee::Ptr<const char>(kSint32), node + offsetof(GuiKeySniffer, curCtrl));
        const auto addAxis = [&](uint32_t name, uint32_t offset, uint32_t returnsTo) {
            addVariableNode(ee::Ptr<Node>(node), ee::Ptr<const char>(name), ee::Ptr<const char>(kZFloat), node + offset);
        };
        addAxis(sym::g_textLeftAxisX, offsetof(GuiKeySniffer, leftAxisX), here::afterAddVariableNode2);  // "LeftAxisX"
        addAxis(sym::g_textLeftAxisY, offsetof(GuiKeySniffer, leftAxisY), here::afterAddVariableNode3);  // "LeftAxisY"
        addAxis(sym::g_textRightAxisX, offsetof(GuiKeySniffer, rightAxisX), here::afterAddVariableNode4); // "RightAxisX"
        addAxis(sym::g_textRightAxisY, offsetof(GuiKeySniffer, rightAxisY), here::afterAddVariableNode5); // "RightAxisY"
        defineClassProperties(sniffer, sniffer->properties, kKeySnifferProperties);
        return node;
    }

    // defineListedKeySniffer: ZGE2's defineGuiKeySnifferProperties: every
    // script none, Flags 3; its Draw, GetFocus and LostFocus.
    uint32_t defineListedKeySniffer(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedKeySniffer> sniffer(address);
        defineWidgetRecord(address, name);
        sniffer->onGetFocus = 0;
        sniffer->onLostFocus = 0;
        sniffer->curCtrl = 0;
        sniffer->active = 0;
        sniffer->leftAxisX = ee::Float(0.0f);
        sniffer->leftAxisY = ee::Float(0.0f);
        sniffer->rightAxisX = ee::Float(0.0f);
        sniffer->rightAxisY = ee::Float(0.0f);
        sniffer->onStart = 0;
        sniffer->onSelect = 0;
        sniffer->onFire1 = 0;
        sniffer->onFire2 = 0;
        sniffer->onFire3 = 0;
        sniffer->onFire4 = 0;
        sniffer->onLeftStickMove = 0;
        sniffer->onRightStickMove = 0;
        sniffer->onLTrig1 = 0;
        sniffer->onLTrig2 = 0;
        sniffer->onRTrig1 = 0;
        sniffer->onRTrig2 = 0;
        sniffer->onUpKey = 0;
        sniffer->onDownKey = 0;
        sniffer->onRightKey = 0;
        sniffer->onLeftKey = 0;
        sniffer->flags = 3;
        subscribeWidget(object, Message::Draw, kDrawGuiKeySniffer);
        addListedProperties(sniffer, kListedKeySnifferProperties);
        subscribeWidget(object, Message::GetFocus, kRunOnGetFocus);
        subscribeWidget(object, Message::LostFocus, kRunOnLostFocus);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiKeySnifferProperties, Verified, dm2, defineListedKeySniffer);

    namespace
    {
        constexpr uint32_t kButtons = 32, kPads = 4;
        // The sniffers' button repeats: seconds to each button's next press, a pad's 32.
        inline ee::Global<ee::Float[kPads][kButtons], sym::repeats> g_repeats;

        // zgui's d-pad words, and the four buttons that never repeat.
        constexpr uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8;

        // A word as a 64-bit register holds it (the masks are compared with
        // all of the pressed word).
        uint64_t extended(uint32_t word) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word))); }

        // A stick at rest stays quiet: the Event runs while it moves, and
        // the frame after it stops.
        bool stickMoved(ee::Float x, ee::Float y, ee::Float lastX, ee::Float lastY)
        {
            const ee::Float tiny(1e-05f);
            bool still = ee::abs(x) < tiny;
            if (!still)
                return true;
            still = ee::abs(y) < tiny;
            return !still || !(lastX == ee::Float(0.0f)) || !(lastY == ee::Float(0.0f));
        }

        // EnableAnalogDir: the left stick on one axis presses its two
        // directions past 0.75 either way (once, until it is let go); let go,
        // the direction counts as pressed again if the held word, shifted by
        // the direction's own word (not its bit number: the original's),
        // says it is not held.
        void stickPresses(ee::Float value, ee::Float &last, uint32_t plus, uint32_t minus, uint32_t &held, uint64_t &pressed)
        {
            const ee::Float press(0.75f), pressBack(-0.75f);
            if (press <= value)
            {
                held |= plus;
                if (last < press)
                {
                    last = value;
                    pressed |= plus;
                }
            }
            else if (value <= pressBack)
            {
                held |= minus;
                if (pressBack < last)
                {
                    last = value;
                    pressed |= minus;
                }
            }
            else if (press <= last)
            {
                last = ee::Float::fromBits(0);
                if (((held >> plus) & 1u) == 0)
                    pressed |= plus;
            }
            else if (last <= pressBack)
            {
                last = ee::Float::fromBits(0);
                if (((held >> minus) & 1u) == 0)
                    pressed |= minus;
            }
        }
    }

    // drawGuiKeySniffer: the sniffer's Draw handler (only while it is active or has
    // the focus): each pad its flags name read: the sticks' Events and axes;
    // with EnableAnalogDir the left stick presses the d-pad (past 0.75);
    // with AutoRepeat a held button presses again (not the four Fire
    // buttons); each pressed and held button's Event.
    int32_t drawGuiKeySniffer(ee::Ptr<GuiKeySniffer> sniffer)
    {
        if (sniffer->active == 0 && g_zgui->focus != classCast<Node>(sniffer))
            return 1;
        const auto run = [&](uint32_t script) { runScript(&sniffer->widget.node, ee::Ptr<char>(script), 0); };
        for (uint32_t pad = 0; pad < kPads; ++pad)
        {
            if (((static_cast<int32_t>(sniffer->flags) >> pad) & 1) == 0)
                continue;
            if (!g_zgui->pads[pad].pad || g_zgui->pads[pad].mask == 0)
                continue;
            const ee::Ptr<Pad> input = g_zgui->pads[pad].pad;
            uint32_t held = input->down;
            uint64_t pressed = extended(input->pressed);
            const ee::Float leftX = input->stickX, leftY = input->stickY;
            const ee::Float rightX = input->rightStickX, rightY = input->rightStickY;
            sniffer->curCtrl = static_cast<int32_t>(pad);
            if (stickMoved(leftX, leftY, sniffer->leftAxisX, sniffer->leftAxisY))
            {
                sniffer->leftAxisX = leftX;
                sniffer->leftAxisY = leftY;
                if (sniffer->onLeftStickMove != 0)
                    run(sniffer->onLeftStickMove);
            }
            if (stickMoved(rightX, rightY, sniffer->rightAxisX, sniffer->rightAxisY))
            {
                sniffer->rightAxisX = rightX;
                sniffer->rightAxisY = rightY;
                if (sniffer->onRightStickMove != 0)
                    run(sniffer->onRightStickMove);
            }
            if (sniffer->enableAnalogDir != 0)
            {
                Stick &stick = (*g_sticks)[pad];
                stickPresses(leftX, stick.x, kRight, kLeft, held, pressed);
                stickPresses(leftY, stick.y, kDown, kUp, held, pressed);
            }
            if (sniffer->autoRepeat != 0)
            {
                ee::Float (&repeat)[kButtons] = (*g_repeats)[pad];
                for (uint32_t button = 0; button < kButtons; ++button)
                {
                    if (((static_cast<uint32_t>(pressed) >> button) & 1u) != 0)
                        repeat[button] = ee::Float(0.5f);
                    else if (((held >> button) & 1u) != 0)
                    {
                        const ee::Float left = repeat[button] - g_zgui->seconds;
                        repeat[button] = left;
                        if (left <= ee::Float(0.0f))
                        {
                            const uint64_t bit = extended(1u << button);
                            repeat[button] = ee::Float(game::readable::perGame(0.05f, 0.05f, 0.064f));
                            // ZGE3 repeats every button but the four Fire buttons; ZGE4 the d-pad alone.
                            const bool repeats = game::readable::kIsBmx ? (bit == kUp || bit == kDown || bit == kLeft || bit == kRight)
                                                                        : (bit != kButtonFire1 && bit != kButtonFire2 && bit != kButtonFire3 && bit != kButtonFire4);
                            if (repeats)
                                pressed |= bit;
                        }
                    }
                }
            }
            for (const KeyEvent &key : kKeyEvents)
            {
                const uint32_t mask = key.menuMask ? (*g_zgui).*key.menuMask : key.mask;
                if ((pressed & extended(mask)) != 0 && (held & mask) != 0)
                {
                    const uint32_t script = (*sniffer).*key.script;
                    if (script != 0)
                        run(script);
                }
            }
            if ((pressed & kLeft) != 0)
            {
                if ((held & kLeft) != 0 && sniffer->onLeftKey != 0)
                    run(sniffer->onLeftKey);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiKeySniffer, Verified, dm2, sniffListedKeys);

    namespace
    {
        // ZGE2's pressed and held buttons' Events, in the original's order
        // (no accept, cancel or option words; the left key is one of them).
        struct ListedKeyEvent
        {
            uint32_t mask;
            uint32_t ListedKeySniffer::*script;
        };
        constexpr ListedKeyEvent kListedKeyEvents[] = {
            {kButtonFire1, &ListedKeySniffer::onFire1},
            {kButtonFire2, &ListedKeySniffer::onFire2},
            {kButtonFire3, &ListedKeySniffer::onFire3},
            {kButtonFire4, &ListedKeySniffer::onFire4},
            {0x100, &ListedKeySniffer::onLTrig1},
            {0x400, &ListedKeySniffer::onLTrig2},
            {0x200, &ListedKeySniffer::onRTrig1},
            {0x800, &ListedKeySniffer::onRTrig2},
            {0x4000, &ListedKeySniffer::onStart},
            {0x8000, &ListedKeySniffer::onSelect},
            {kUp, &ListedKeySniffer::onUpKey},
            {kDown, &ListedKeySniffer::onDownKey},
            {kRight, &ListedKeySniffer::onRightKey},
            {kLeft, &ListedKeySniffer::onLeftKey},
        };
    }

    // sniffListedKeys: ZGE2's drawGuiKeySniffer, the key sniffer's Draw: while
    // it is active or has the focus, each pad its Flags name is read as it
    // is (ZGE2 has no analog directions and no auto repeat here): the sticks'
    // moves kept and their Events run, then the Event of each button
    // pressed this frame. 1.
    int32_t sniffListedKeys(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedKeySniffer> sniffer = messageRecord<ListedKeySniffer>(message);
        if (sniffer->active == 0 && g_zgui->focus.address != message->target)
            return 1;
        const auto run = [&](uint32_t script) { runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script)); };
        for (uint32_t pad = 0; pad < kPads; ++pad)
        {
            if (((static_cast<int32_t>(sniffer->flags) >> pad) & 1) == 0)
                continue;
            const ee::Ptr<Pad> input = g_zgui->pads[pad].pad;
            if (!input)
                continue;
            const uint32_t held = input->down;
            const uint32_t pressed = input->pressed;
            const ee::Float leftX = input->stickX, leftY = input->stickY;
            const ee::Float rightX = input->rightStickX, rightY = input->rightStickY;
            sniffer->curCtrl = static_cast<int32_t>(pad);
            if (stickMoved(leftX, leftY, sniffer->leftAxisX, sniffer->leftAxisY))
            {
                sniffer->leftAxisX = leftX;
                sniffer->leftAxisY = leftY;
                if (sniffer->onLeftStickMove != 0)
                    run(sniffer->onLeftStickMove);
            }
            if (stickMoved(rightX, rightY, sniffer->rightAxisX, sniffer->rightAxisY))
            {
                sniffer->rightAxisX = rightX;
                sniffer->rightAxisY = rightY;
                if (sniffer->onRightStickMove != 0)
                    run(sniffer->onRightStickMove);
            }
            for (const ListedKeyEvent &key : kListedKeyEvents)
            {
                if ((pressed & key.mask) != 0 && (held & key.mask) != 0)
                {
                    const uint32_t script = (*sniffer).*key.script;
                    if (script != 0)
                        run(script);
                }
            }
        }
        return 1;
    }
}
