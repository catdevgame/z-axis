// zgui's input group: a widget whose child widgets are the rows of a screen.
// The group takes the d-pad, moves SelectedIndex over its children (past the
// ones whose IsDisabled variable is set) and passes the rest of the input on
// to the selected one; its Draw moves the focus messages along.

#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/input_group.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kIsDisabledName = sym::g_textIsDisabled; // "IsDisabled"
        constexpr uint32_t kChildKind = static_cast<uint32_t>(NodeKind::Widget);

        struct GuiInputGroup
        {
            Widget widget;
            int32_t lastSelectedIndex;  // +0x0F0 the child that has the focus messages
            int32_t selectedIndex;      // +0x0F4
            uint32_t wrapAround;        // +0x0F8
            uint32_t onGetFocus;        // +0x0FC
            uint32_t onLostFocus;       // +0x100
            uint32_t onSelect;          // +0x104
            uint32_t onCancel;          // +0x108
            PropertyNode properties[6]; // +0x10C its properties' nodes (kInputGroupProperties)
        };
        static_assert(offsetof(GuiInputGroup, properties) == 0x10C);
        static_assert(offsetof(GuiInputGroup, lastSelectedIndex) == 0xF0 && offsetof(GuiInputGroup, onCancel) == 0x108);

        // ZGE2's (DM2's) input group record, after WidgetRecord (widget_parts.h).
        struct ListedInputGroup
        {
            WidgetRecord widget;
            int32_t lastSelectedIndex; // +0x60 the child that has the focus messages
            int32_t selectedIndex;     // +0x64
            uint32_t wrapAround;       // +0x68
            uint32_t onGetFocus;       // +0x6C each a script (Event)
            uint32_t onLostFocus;      // +0x70
            uint32_t onSelect;         // +0x74
            uint32_t onCancel;         // +0x78
        };
        static_assert(offsetof(ListedInputGroup, lastSelectedIndex) == 0x60 && offsetof(ListedInputGroup, onCancel) == 0x78);

        constexpr ListedProperty kListedInputGroupProperties[] = {
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(ListedInputGroup, selectedIndex)},
            {sym::g_textWrapAround, sym::g_textBool, offsetof(ListedInputGroup, wrapAround)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedInputGroup, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedInputGroup, onLostFocus)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedInputGroup, onCancel)},
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(ListedInputGroup, onSelect)},
        };

        constexpr ClassProperty kInputGroupProperties[] = {
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(GuiInputGroup, selectedIndex)}, // "SelectedIndex" Sint32
            {sym::g_textWrapAround, sym::g_textBool, offsetof(GuiInputGroup, wrapAround)}, // "WrapAround" Bool
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiInputGroup, onGetFocus)}, // "OnGetFocus" Event
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiInputGroup, onLostFocus)}, // "OnLostFocus" Event
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiInputGroup, onCancel)}, // "OnCancel" Event
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(GuiInputGroup, onSelect)}, // "OnSelect" Event
        };
    }

    // guiInputGroupGetFocus: the group's GetFocus: OnGetFocus, then the first child
    // from the selected one on that is not disabled (once around, wrapping
    // only if WrapAround is set).
    uint32_t guiInputGroupGetFocus(ee::Ptr<GuiInputGroup> group)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<Node> node = classCast<Node>(group);

        runOnGetFocus(group);
        group->lastSelectedIndex = -1;
        const int32_t count = countChildrenOfKind(node, kChildKind);
        if (group->selectedIndex < 0 || group->selectedIndex >= count)
            group->selectedIndex = 0;
        const int32_t start = group->selectedIndex;
        ee::Ptr<Node> child = nthChildOfKind(node, static_cast<uint32_t>(start), kChildKind);
        for (;;)
        {
            if (!child)
                break;
            *disabled = 0;
            copyVariable(child, kIsDisabledName, disabled);
            if (!*disabled)
                break;
            const int32_t next = ++group->selectedIndex;
            if (next == start)
                break;
            if (next >= count)
            {
                if (!group->wrapAround)
                    return 1;
                group->selectedIndex = 0;
            }
            child = nthChildOfKind(node, static_cast<uint32_t>(group->selectedIndex), kChildKind);
        }
        return 1;
    }

    namespace
    {
        // ZGE2's group children: the objects hanging from the widget's scene
        // object. How many there are.
        int32_t attachedChildCount(ee::Ptr<zge::SceneObject> object)
        {
            int32_t count = 0;
            for (ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object); child; child = zge::nextAttachedSibling(child))
                ++count;
            return count;
        }

        // The child at `index` (from 0), or 0.
        ee::Ptr<zge::SceneObject> attachedChildAt(ee::Ptr<zge::SceneObject> object, int32_t index)
        {
            ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object);
            for (int32_t at = 0; child && at != index; ++at)
                child = zge::nextAttachedSibling(child);
            return child;
        }
    }

    // focusListedInputGroup: ZGE2's guiInputGroupGetFocus: OnGetFocus, then
    // from the selected child (the first if the selection is out of range)
    // on, the first that is not disabled becomes the selection. ZGE2's
    // children are the objects hanging from the widget's scene object; it
    // stops after the last (ZGE3 wraps round with WrapAround), asking the
    // object after it (none) too.
    int32_t focusListedInputGroup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<ListedInputGroup> group = messageRecord<ListedInputGroup>(message);
        const ee::Ptr<zge::SceneObject> object(message->target);

        runListedGetFocus(message);
        group->lastSelectedIndex = -1;
        const int32_t count = attachedChildCount(object);
        zge::firstAttachedChild(object); // (the original's result unused)
        if (group->selectedIndex < 0 || !(group->selectedIndex < count))
            group->selectedIndex = 0;

        ee::Ptr<zge::SceneObject> child = attachedChildAt(object, group->selectedIndex);
        if (!child)
            return 1;
        // The child's IsDisabled (not disabled without one).
        const auto isDisabled = [&]() {
            if (!copyVariable(child, ee::Ptr<const char>(kIsDisabledName), disabled))
                *disabled = 0;
            return *disabled != 0;
        };
        if (!isDisabled())
            return 1;
        do
        {
            group->selectedIndex = group->selectedIndex + 1;
            child = zge::nextAttachedSibling(child);
        } while (isDisabled() && child);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(guiInputGroupGetFocus, Verified, dm2, focusListedInputGroup);

    // guiInputGroupLostFocus: the group's LostFocus.
    uint32_t guiInputGroupLostFocus(uint32_t address)
    {
        runOnLostFocus(address);
        return 1;
    }
    READABLE_FUNCTION_WAITS(guiInputGroupLostFocus, Verified);

    // handleGuiInputGroupInput: the group's Input: up and down move the selection, accept
    // and cancel run its scripts; if the child it lands on is disabled it
    // keeps moving the way the buttons say until it finds one or comes back
    // to where it was (and then nothing gets the input). The selected child
    // gets the input.
    uint32_t handleGuiInputGroupInput(ee::Ptr<GuiInputGroup> group, ee::Ptr<Input> input)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<Node> node = classCast<Node>(group);

        const uint32_t pressed = input->pressed, down = input->down;
        const uint32_t upPressed = pressed & kButtonUp;
        const int32_t count = countChildrenOfKind(node, kChildKind);
        const uint32_t downPressed = pressed & kButtonDown;

        if (upPressed && (down & kButtonUp))
        {
            const int32_t selected = group->selectedIndex;
            if (selected > 0 || group->wrapAround)
            {
                group->selectedIndex = selected > 0 ? selected - 1 : count - 1;
                if (const SoundHook hook = g_zgui->moveSound)
                    hook();
            }
            else
            {
                if (const SoundHook hook = g_zgui->blockedSound)
                    hook();
            }
        }

        if (downPressed && (down & kButtonDown))
        {
            const int32_t selected = group->selectedIndex;
            const bool last = selected >= count - 1;
            if (!last || group->wrapAround)
            {
                group->selectedIndex = last ? 0 : selected + 1;
                if (const SoundHook hook = g_zgui->moveSound)
                    hook();
            }
            else
            {
                const SoundHook hook = g_zgui->blockedSound;
                if (hook)
                    hook();
            }
        }

        const uint32_t accept = g_zgui->acceptButton;
        bool ran = false;
        if (pressed & accept)
        {
            ran = true;
            if (down & accept)
            {
                if (group->onSelect)
                {
                    if (const SoundHook hook = g_zgui->selectSound)
                        hook();
                    runScript(node, ee::Ptr<char>(group->onSelect), 0);
                }
            }
            else
                ran = false;
        }
        if (!ran)
        {
            const uint32_t cancel = g_zgui->cancelButton;
            if (pressed & cancel)
            {
                ran = true;
                if ((down & cancel) && group->onCancel)
                {
                    if (const SoundHook hook = g_zgui->cancelSound)
                        hook();
                    runScript(node, ee::Ptr<char>(group->onCancel), 0);
                }
            }
        }

        ee::Ptr<Node> child = nthChildOfKind(node, static_cast<uint32_t>(group->selectedIndex), kChildKind);
        if (!child)
            return 1;

        const int32_t start = group->selectedIndex;
        const uint32_t downHeld = down & kButtonDown;
        // The child's IsDisabled (not disabled without one).
        const auto readDisabled = [&]() {
            if (!copyVariable(child, kIsDisabledName, disabled))
                *disabled = 0;
        };
        readDisabled();
        if (*disabled)
        {
            const int32_t last = count - 1;
            const uint32_t upHeld = down & kButtonUp;
            for (;;)
            {
                if (upPressed && upHeld)
                {
                    const int32_t selected = group->selectedIndex;
                    if (selected > 0)
                        group->selectedIndex = selected - 1;
                    else
                    {
                        if (group->wrapAround)
                            group->selectedIndex = last;
                    }
                    child = nthChildOfKind(node, static_cast<uint32_t>(group->selectedIndex), kChildKind);
                }
                if (downPressed && downHeld)
                {
                    const int32_t selected = group->selectedIndex;
                    const bool atEnd = selected >= last;
                    if (!atEnd || group->wrapAround)
                    {
                        const int32_t next = atEnd ? 0 : selected + 1;
                        group->selectedIndex = next;
                        child = nthChildOfKind(node, static_cast<uint32_t>(next), kChildKind);
                    }
                }
                if (!child || start == group->selectedIndex)
                {
                    child = {};
                    break;
                }
                readDisabled();
                if (!*disabled)
                    break;
            }
        }
        if (child)
            sendMessage(child, kInputMessage, input);
        return 1;
    }

    // pressListedInputGroup: ZGE2's handleGuiInputGroupInput, the same steps
    // over the objects hanging from the widget's scene object: up and down
    // move the selection (the move sound, or the blocked sound at an end
    // without WrapAround), accept and cancel run its scripts with their
    // sounds; past disabled children it keeps moving the way the buttons are
    // held until one is not or it comes back to where it was. The selected
    // child gets the input, as a message sent now. 1.
    int32_t pressListedInputGroup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<ListedInputGroup> group = messageRecord<ListedInputGroup>(message);
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<Input> input(message->data);
        const uint32_t pressed = input->pressed, down = input->down;
        const auto play = [](SoundHook Zgui::*sound) {
            if (const SoundHook hook = (*g_zgui).*sound)
                hook();
        };
        const auto run = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };

        const int32_t count = attachedChildCount(object);
        const uint32_t upPressed = pressed & kButtonUp, downPressed = pressed & kButtonDown;

        if (upPressed && (down & kButtonUp))
        {
            const int32_t selected = group->selectedIndex;
            if (selected > 0 || group->wrapAround)
            {
                group->selectedIndex = selected > 0 ? selected - 1 : count - 1;
                play(&Zgui::moveSound);
            }
            else
                play(&Zgui::blockedSound);
        }
        if (downPressed && (down & kButtonDown))
        {
            const int32_t selected = group->selectedIndex;
            if (selected < count - 1 || group->wrapAround)
            {
                group->selectedIndex = selected < count - 1 ? selected + 1 : 0;
                play(&Zgui::moveSound);
            }
            else
                play(&Zgui::blockedSound);
        }

        const uint32_t accept = g_zgui->acceptButton;
        if ((pressed & accept) && (down & accept))
        {
            if (group->onSelect)
            {
                play(&Zgui::selectSound);
                run(group->onSelect);
            }
        }
        else
        {
            const uint32_t cancel = g_zgui->cancelButton;
            if ((pressed & cancel) && (down & cancel) && group->onCancel)
            {
                play(&Zgui::cancelSound);
                run(group->onCancel);
            }
        }

        // The selected child.
        ee::Ptr<zge::SceneObject> child = attachedChildAt(object, group->selectedIndex);
        if (!child)
            return 1;
        const int32_t start = group->selectedIndex;
        // The child's IsDisabled (not disabled without one).
        const auto isDisabled = [&]() {
            if (!copyVariable(child, ee::Ptr<const char>(kIsDisabledName), disabled))
                *disabled = 0;
            return *disabled != 0;
        };
        if (isDisabled())
        {
            const int32_t last = count - 1;
            const uint32_t upHeld = down & kButtonUp, downHeld = down & kButtonDown;
            do
            {
                if (upPressed && upHeld)
                {
                    const int32_t selected = group->selectedIndex;
                    if (selected > 0)
                        group->selectedIndex = selected - 1;
                    else if (group->wrapAround)
                        group->selectedIndex = last;
                    child = attachedChildAt(object, group->selectedIndex);
                }
                if (downPressed && downHeld)
                {
                    if (group->selectedIndex < last)
                    {
                        child = zge::nextAttachedSibling(child);
                        group->selectedIndex = group->selectedIndex + 1;
                    }
                    else if (group->wrapAround)
                    {
                        child = zge::firstAttachedChild(object);
                        group->selectedIndex = 0;
                    }
                }
                if (!child || start == group->selectedIndex)
                    return 1;
            } while (isDisabled());
        }
        zge::sendNewMessage(ee::Address(), ee::Address(child), kInputMessage, message->data, zge::kMessageNow);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiInputGroupInput, Verified, dm2, pressListedInputGroup);

    // drawGuiInputGroup: the group's Draw: when the selection moved, the child it
    // left gets LostFocus and the new one GetFocus (neither if disabled).
    uint32_t drawGuiInputGroup(ee::Ptr<GuiInputGroup> group)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<Node> node = classCast<Node>(group);

        const int32_t last = group->lastSelectedIndex;
        if (last == group->selectedIndex)
            return 1;
        const ee::Ptr<Node> left = nthChildOfKind(node, static_cast<uint32_t>(last), kChildKind);
        group->lastSelectedIndex = group->selectedIndex;
        const auto notifyUnlessDisabled = [&](ee::Ptr<Node> child, Message message) {
            if (!copyVariable(child, kIsDisabledName, disabled))
                *disabled = 0;
            if (!*disabled)
                sendMessage(child, static_cast<uint32_t>(message), 0);
        };
        if (left)
            notifyUnlessDisabled(left, Message::LostFocus);
        const ee::Ptr<Node> entered = nthChildOfKind(node, static_cast<uint32_t>(group->selectedIndex), kChildKind);
        if (entered)
            notifyUnlessDisabled(entered, Message::GetFocus);
        group->lastSelectedIndex = group->selectedIndex;
        return 1;
    }

    // drawListedInputGroup: ZGE2's drawGuiInputGroup: when the selection
    // moved, the child it left gets LostFocus and the new one GetFocus
    // (neither if disabled), each a message sent now from the group's object.
    // While LostFocus is sent the selection reads as the child it left; it is
    // put back after unless that handler moved either. 1.
    int32_t drawListedInputGroup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<int32_t> disabled; // the child's IsDisabled
        const ee::Ptr<ListedInputGroup> group = messageRecord<ListedInputGroup>(message);
        const ee::Ptr<zge::SceneObject> object(message->target);
        const auto tellUnlessDisabled = [&](ee::Ptr<zge::SceneObject> child, Message focus) {
            if (!copyVariable(child, ee::Ptr<const char>(kIsDisabledName), disabled))
                *disabled = 0;
            if (!*disabled)
                zge::sendNewMessage(message->target, ee::Address(child), static_cast<uint32_t>(focus), ee::Address(), zge::kMessageNow);
        };

        const int32_t selected = group->selectedIndex;
        if (group->lastSelectedIndex == selected)
            return 1;
        const ee::Ptr<zge::SceneObject> left = attachedChildAt(object, group->lastSelectedIndex);
        group->selectedIndex = group->lastSelectedIndex;
        if (left)
            tellUnlessDisabled(left, Message::LostFocus);
        if (group->lastSelectedIndex == group->selectedIndex)
            group->selectedIndex = selected;
        const ee::Ptr<zge::SceneObject> entered = attachedChildAt(object, group->selectedIndex);
        if (entered)
            tellUnlessDisabled(entered, Message::GetFocus);
        group->lastSelectedIndex = group->selectedIndex;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiInputGroup, Verified, dm2, drawListedInputGroup);

    // defineGuiInputGroupProperties: a group's defaults and properties.
    uint32_t defineGuiInputGroupProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiInputGroup> group(node);
        defineWidgetProperties(node, text);
        group->onGetFocus = 0;
        group->onLostFocus = 0;
        group->onCancel = 0;
        group->onSelect = 0;
        group->wrapAround = 1;
        group->lastSelectedIndex = -1;
        group->selectedIndex = -1;
        defineClassProperties(group, group->properties, kInputGroupProperties);
        addListener(node, Message::Input, sym::handleGuiInputGroupInput);
        addListener(node, Message::Draw, sym::drawGuiInputGroup);
        addListener(node, Message::GetFocus, sym::guiInputGroupGetFocus);
        addListener(node, Message::LostFocus, sym::guiInputGroupLostFocus);
        return node;
    }

    // defineListedInputGroup: ZGE2's defineGuiInputGroupProperties.
    uint32_t defineListedInputGroup(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedInputGroup> group(address);
        defineWidgetRecord(address, name);
        group->onGetFocus = 0;
        group->onLostFocus = 0;
        group->onCancel = 0;
        group->onSelect = 0;
        group->wrapAround = 1;
        group->lastSelectedIndex = -1;
        group->selectedIndex = -1;
        addListedProperties(group, kListedInputGroupProperties);
        subscribeWidget(object, Message::Input, sym::handleGuiInputGroupInput);
        subscribeWidget(object, Message::Draw, sym::drawGuiInputGroup);
        subscribeWidget(object, Message::GetFocus, sym::guiInputGroupGetFocus);
        subscribeWidget(object, Message::LostFocus, sym::guiInputGroupLostFocus);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiInputGroupProperties, Verified, dm2, defineListedInputGroup);
}
