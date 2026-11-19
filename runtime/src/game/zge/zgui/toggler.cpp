// zgui's toggler: a value left and right change between MinValue and
// MaxValue, in steps of IncValue, either wrapping or stopping with the blocked
// sound once. Its OnChange script runs when the value moved; accept, cancel
// and option run theirs.

#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/toggler.cpp.h"

namespace zge::zgui
{
    namespace
    {
        struct GuiToggler
        {
            Widget widget;
            uint32_t wrapAround;   // +0x0F0
            uint32_t isDisabled;   // +0x0F4
            ee::Float minValue;    // +0x0F8
            ee::Float maxValue;    // +0x0FC
            ee::Float incValue;    // +0x100
            ee::Float curValue;    // +0x104
            ee::Float lastDir;     // +0x108 -1, 1; 0 if the last press hit the end
            uint32_t enableSound;  // +0x10C
            uint32_t blockedPlayed; // +0x110 the blocked sound played since the value last moved
            uint32_t onGetFocus;   // +0x114
            uint32_t onLostFocus;  // +0x118
            uint32_t onSelect;     // +0x11C
            uint32_t onOption;     // +0x120
            uint32_t onCancel;     // +0x124
            uint32_t onChange;     // +0x128
            PropertyNode properties[14]; // +0x12C its properties' nodes (kTogglerProperties)
        };
        static_assert(offsetof(GuiToggler, properties) == 0x12C);
        static_assert(offsetof(GuiToggler, wrapAround) == 0xF0 && offsetof(GuiToggler, onChange) == 0x128);

        // ZGE2's (DM2's) toggler record, after WidgetRecord (widget_parts.h):
        // ZGE3's fields without blockedPlayed.
        struct ListedToggler
        {
            WidgetRecord widget;
            uint32_t wrapAround;  // +0x60
            uint32_t isDisabled;  // +0x64
            ee::Float minValue;   // +0x68
            ee::Float maxValue;   // +0x6C
            ee::Float incValue;   // +0x70
            ee::Float curValue;   // +0x74
            ee::Float lastDir;    // +0x78
            uint32_t enableSound; // +0x7C
            uint32_t onGetFocus;  // +0x80 each a script (Event)
            uint32_t onLostFocus; // +0x84
            uint32_t onSelect;    // +0x88
            uint32_t onOption;    // +0x8C
            uint32_t onCancel;    // +0x90
            uint32_t onChange;    // +0x94
        };
        static_assert(offsetof(ListedToggler, wrapAround) == 0x60 && offsetof(ListedToggler, onGetFocus) == 0x80);
        static_assert(offsetof(ListedToggler, onChange) == 0x94);

        constexpr ListedProperty kListedTogglerProperties[] = {
            {sym::g_textWrapAround, sym::g_textBool, offsetof(ListedToggler, wrapAround)},
            {sym::g_textIsDisabled, sym::g_textBool, offsetof(ListedToggler, isDisabled)},
            {sym::g_textMinValue, sym::g_textZFloat, offsetof(ListedToggler, minValue)},
            {sym::g_textMaxValue, sym::g_textZFloat, offsetof(ListedToggler, maxValue)},
            {sym::g_textIncValue, sym::g_textZFloat, offsetof(ListedToggler, incValue)},
            {sym::g_textCurValue2, sym::g_textZFloat, offsetof(ListedToggler, curValue)},
            {sym::g_textLastDir, sym::g_textZFloat, offsetof(ListedToggler, lastDir)},
            {sym::g_textEnableSound, sym::g_textBool, offsetof(ListedToggler, enableSound)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedToggler, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedToggler, onLostFocus)},
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(ListedToggler, onSelect)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedToggler, onCancel)},
            {sym::g_textOnOption, sym::g_textEvent, offsetof(ListedToggler, onOption)},
            {sym::g_textOnChange, sym::g_textEvent, offsetof(ListedToggler, onChange)},
        };

        constexpr ClassProperty kTogglerProperties[] = {
            {sym::g_textWrapAround, sym::g_textBool, offsetof(GuiToggler, wrapAround)}, // "WrapAround" Bool
            {sym::g_textIsDisabled, sym::g_textBool, offsetof(GuiToggler, isDisabled)}, // "IsDisabled" Bool
            {sym::g_textMinValue, sym::g_textZFloat, offsetof(GuiToggler, minValue)}, // "MinValue" ZFloat
            {sym::g_textMaxValue, sym::g_textZFloat, offsetof(GuiToggler, maxValue)}, // "MaxValue" ZFloat
            {sym::g_textIncValue, sym::g_textZFloat, offsetof(GuiToggler, incValue)}, // "IncValue" ZFloat
            {sym::g_textCurValue2, sym::g_textZFloat, offsetof(GuiToggler, curValue)}, // "CurValue" ZFloat
            {sym::g_textLastDir, sym::g_textZFloat, offsetof(GuiToggler, lastDir)}, // "LastDir" ZFloat
            {sym::g_textEnableSound, sym::g_textBool, offsetof(GuiToggler, enableSound)}, // "EnableSound" Bool
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiToggler, onGetFocus)}, // "OnGetFocus" Event
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiToggler, onLostFocus)}, // "OnLostFocus" Event
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(GuiToggler, onSelect)}, // "OnSelect" Event
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiToggler, onCancel)}, // "OnCancel" Event
            {sym::g_textOnOption, sym::g_textEvent, offsetof(GuiToggler, onOption)}, // "OnOption" Event
            {sym::g_textOnChange, sym::g_textEvent, offsetof(GuiToggler, onChange)}, // "OnChange" Event
        };
    }

    // handleGuiTogglerInput: the toggler's Input. Left wins over right when both are
    // held. On a disabled toggler only accept is refused: left, right, cancel
    // and option still work.
    uint32_t handleGuiTogglerInput(uint32_t address, uint32_t inputAddress)
    {
        const ee::Ptr<GuiToggler> toggler(address);
        const ee::Ptr<Input> input(inputAddress);
        const uint32_t pressed = input->pressed, down = input->down;

        const auto canPlay = [&](SoundHook hook) { return hook && toggler->enableSound; };

        // The value moved: the change sound. True if it played.
        const auto moved = [&]() {
            toggler->blockedPlayed = 0;
            const SoundHook hook = g_zgui->changeSound;
            if (!canPlay(hook))
                return false;
            hook();
            return true;
        };
        // OnChange, unless the press hit the end. Whether the last block ran.
        const auto reportChange = [&]() {
            const uint32_t onChange = toggler->onChange;
            if (onChange && toggler->lastDir == ee::Float(0.0f))
                return;
            if (onChange)
                runScript(ee::Ptr<Node>(address), ee::Ptr<char>(onChange), 0);
        };

        if ((pressed & kButtonLeft) && (down & kButtonLeft))
        {
            toggler->lastDir = ee::Float(-1.0f);
            if (toggler->wrapAround)
            {
                const ee::Float minValue = toggler->minValue;
                const ee::Float value = toggler->curValue - toggler->incValue;
                toggler->curValue = value;
                if (value < minValue)
                    toggler->curValue = value + (toggler->maxValue - minValue + ee::Float(1.0f));
                moved();
            }
            else
            {
                const ee::Float minValue = toggler->minValue;
                const ee::Float value = toggler->curValue - toggler->incValue;
                toggler->curValue = value;
                if (value < minValue)
                {
                    const SoundHook hook = g_zgui->blockedSound;
                    if (canPlay(hook) && !toggler->blockedPlayed)
                    {
                        toggler->blockedPlayed = 1;
                        hook();
                        toggler->curValue = toggler->minValue;
                    }
                    else
                        toggler->curValue = minValue;
                    ee::Ptr<GuiToggler>(address)->lastDir = 0;
                }
                else
                    moved();
            }
            reportChange();
        }
        else
        {
            if ((pressed & kButtonRight) && (down & kButtonRight))
            {
                toggler->lastDir = ee::Float(1.0f);
                if (toggler->wrapAround)
                {
                    const ee::Float maxValue = toggler->maxValue;
                    const ee::Float value = toggler->curValue + toggler->incValue;
                    toggler->curValue = value;
                    if (maxValue < value)
                        toggler->curValue = value - (maxValue - toggler->minValue + ee::Float(1.0f));
                    moved();
                }
                else
                {
                    const ee::Float maxValue = toggler->maxValue;
                    const ee::Float value = toggler->curValue + toggler->incValue;
                    toggler->curValue = value;
                    if (maxValue < value)
                    {
                        const SoundHook hook = g_zgui->blockedSound;
                        if (canPlay(hook) && !toggler->blockedPlayed)
                        {
                            hook();
                            toggler->blockedPlayed = 1;
                            toggler->curValue = toggler->maxValue;
                        }
                        else
                            toggler->curValue = maxValue;
                        ee::Ptr<GuiToggler>(address)->lastDir = 0;
                    }
                    else
                        moved();
                }
                reportChange();
            }
        }

        // The sound if it may play, then the script (read again after it).
        const auto soundAndScript = [&](SoundHook hook, const uint32_t &script) {
            if (!hook || toggler->enableSound)
            {
                if (hook)
                    hook();
            }
            runScript(ee::Ptr<Node>(address), ee::Ptr<char>(script), 0);
        };

        const uint32_t accept = g_zgui->acceptButton;
        if ((pressed & accept) && (down & accept))
        {
            if (!toggler->onSelect)
                return 1u;
            if (toggler->isDisabled)
                return 1;
            soundAndScript(g_zgui->selectSound, toggler->onSelect);
            return 1u;
        }

        const uint32_t cancel = g_zgui->cancelButton;
        if ((pressed & cancel) && (down & cancel))
        {
            if (!toggler->onCancel)
                return 1u;
            soundAndScript(g_zgui->cancelSound, toggler->onCancel);
            return 1u;
        }

        const uint32_t option = g_zgui->optionButton;
        if (pressed & option)
        {
            if (!(down & option))
                return 1u;
            if (!toggler->onOption)
                return 1;
            soundAndScript(g_zgui->selectSound, toggler->onOption);
        }
        return 1u;
    }

    // pressListedToggler: ZGE2's handleGuiTogglerInput, which differs from
    // ZGE3's twice: the blocked sound plays on every press at an end (ZGE3's
    // once until the value moves or the focus comes back), and left and right
    // pressed together move the value both ways (ZGE3's left wins). 1.
    int32_t pressListedToggler(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedToggler> toggler = messageRecord<ListedToggler>(message);
        const ee::Ptr<Input> input(message->data);
        const uint32_t pressed = input->pressed, down = input->down;
        const auto press = [&](uint32_t mask) { return (pressed & mask) && (down & mask); };
        const auto play = [&](SoundHook Zgui::*sound) {
            const SoundHook hook = (*g_zgui).*sound;
            if (hook && toggler->enableSound)
                hook();
        };
        const auto run = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };
        // OnChange, unless the press stopped at an end.
        const auto reportChange = [&]() {
            const uint32_t onChange = toggler->onChange;
            if (onChange && !(toggler->lastDir == ee::Float(0.0f)))
                run(onChange);
        };

        if (press(kButtonLeft))
        {
            toggler->lastDir = ee::Float(-1.0f);
            const ee::Float minValue = toggler->minValue;
            const ee::Float value = toggler->curValue - toggler->incValue;
            toggler->curValue = value;
            if (!(value < minValue))
                play(&Zgui::changeSound);
            else if (toggler->wrapAround)
            {
                toggler->curValue = value + (toggler->maxValue - minValue + ee::Float(1.0f));
                play(&Zgui::changeSound);
            }
            else
            {
                play(&Zgui::blockedSound);
                toggler->curValue = toggler->minValue;
                toggler->lastDir = ee::Float(0.0f);
            }
            reportChange();
        }
        if (press(kButtonRight))
        {
            toggler->lastDir = ee::Float(1.0f);
            const ee::Float maxValue = toggler->maxValue;
            const ee::Float value = toggler->curValue + toggler->incValue;
            toggler->curValue = value;
            if (!(maxValue < value))
                play(&Zgui::changeSound);
            else if (toggler->wrapAround)
            {
                toggler->curValue = value - (maxValue - toggler->minValue + ee::Float(1.0f));
                play(&Zgui::changeSound);
            }
            else
            {
                play(&Zgui::blockedSound);
                toggler->curValue = toggler->maxValue;
                toggler->lastDir = ee::Float(0.0f);
            }
            reportChange();
        }

        if (press(g_zgui->acceptButton))
        {
            if (toggler->onSelect && !toggler->isDisabled)
            {
                play(&Zgui::selectSound);
                run(toggler->onSelect);
            }
        }
        else if (press(g_zgui->cancelButton))
        {
            if (toggler->onCancel)
            {
                play(&Zgui::cancelSound);
                run(toggler->onCancel);
            }
        }
        else if (press(g_zgui->optionButton))
        {
            if (toggler->onOption)
            {
                play(&Zgui::selectSound);
                run(toggler->onOption);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiTogglerInput, Verified, dm2, pressListedToggler);

    // guiTogglerGetFocus: the toggler's GetFocus: the blocked sound may play again.
    uint32_t guiTogglerGetFocus(uint32_t address)
    {
        ee::Ptr<GuiToggler>(address)->blockedPlayed = 0;
        runOnGetFocus(address);
        return 1;
    }
    READABLE_FUNCTION_WAITS(guiTogglerGetFocus, Verified);

    // defineGuiTogglerProperties: a toggler's defaults (0 to 10 in ones, wrapping, with
    // sound) and properties.
    uint32_t defineGuiTogglerProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiToggler> toggler(node);
        defineWidgetProperties(node, text);
        toggler->isDisabled = 0;
        ee::Ptr<GuiToggler>(node)->curValue = 0;
        ee::Ptr<GuiToggler>(node)->minValue = 0;
        toggler->onChange = 0;
        toggler->onGetFocus = 0;
        toggler->onLostFocus = 0;
        toggler->onSelect = 0;
        toggler->onCancel = 0;
        toggler->onOption = 0;
        ee::Ptr<GuiToggler>(node)->lastDir = 0;
        toggler->blockedPlayed = 0;
        toggler->maxValue = ee::Float(10.0f);
        toggler->incValue = ee::Float(1.0f);
        toggler->enableSound = 1;
        toggler->wrapAround = 1;
        defineClassProperties(toggler, toggler->properties, kTogglerProperties);
        addListener(node, Message::Input, sym::handleGuiTogglerInput);
        addListener(node, Message::GetFocus, sym::guiTogglerGetFocus);
        addListener(node, Message::LostFocus, kRunOnLostFocus);
        return node;
    }

    // defineListedToggler: ZGE2's defineGuiTogglerProperties: the same
    // defaults; its GetFocus is the plain OnGetFocus script (runOnGetFocus).
    uint32_t defineListedToggler(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedToggler> toggler(address);
        defineWidgetRecord(address, name);
        toggler->isDisabled = 0;
        toggler->curValue = ee::Float(0.0f);
        toggler->minValue = ee::Float(0.0f);
        toggler->onChange = 0;
        toggler->onGetFocus = 0;
        toggler->onLostFocus = 0;
        toggler->onSelect = 0;
        toggler->onCancel = 0;
        toggler->onOption = 0;
        toggler->lastDir = ee::Float(0.0f);
        toggler->maxValue = ee::Float(10.0f);
        toggler->incValue = ee::Float(1.0f);
        toggler->enableSound = 1;
        toggler->wrapAround = 1;
        addListedProperties(toggler, kListedTogglerProperties);
        subscribeWidget(object, Message::Input, sym::handleGuiTogglerInput);
        subscribeWidget(object, Message::GetFocus, kRunOnGetFocus);
        subscribeWidget(object, Message::LostFocus, kRunOnLostFocus);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiTogglerProperties, Verified, dm2, defineListedToggler);
}
