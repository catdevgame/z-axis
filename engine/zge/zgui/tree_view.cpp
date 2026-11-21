// zgui's tree view: a list of a node's children of kind 7, each its text; the
// d-pad moves through them, accept goes down into an item's children or runs
// OnItemSelect at a leaf, cancel comes back up and at the top runs OnCancel.

#include "zge/zgui/widget_parts.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/container/list.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/tree_view.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kTreeItem = 7;        // the node kind of a tree's items

        struct GuiTreeView
        {
            Widget widget;
            zge::Vec4 scale;            // +0x0F0
            zge::Vec4 selectedCoord;    // +0x100 from the view to the selected item
            ee::Ptr<Node> rootNode;     // +0x110
            ee::Ptr<Node> parentNode;   // +0x114 the items shown are its children
            ee::Ptr<Node> selectedNode; // +0x118
            ee::Ptr<UiAsset> asset;           // +0x11C found by charset
            int32_t visibleCount;       // +0x120
            int32_t scrollOffset;       // +0x124 the first item shown
            int32_t selectedIndex;      // +0x128
            int32_t lastSelectedIndex;  // +0x12C what OnChange last ran for
            uint8_t _130[8];
            WidgetColour color;         // +0x138
            WidgetColour selectedColor; // +0x148
            ee::Float spacing;          // +0x158
            uint32_t charset;           // +0x15C
            uint32_t onGetFocus;        // +0x160
            uint32_t onLostFocus;       // +0x164
            uint32_t onCancel;          // +0x168
            uint32_t onItemFocus;       // +0x16C
            uint32_t onItemUnFocus;     // +0x170
            uint32_t onItemSelect;      // +0x174
            uint32_t onChange;          // +0x178
            uint32_t onLeft;            // +0x17C
            uint32_t onRight;           // +0x180
            PropertyNode properties[21]; // +0x184 its properties' nodes (kTreeViewProperties)
        };
        static_assert(offsetof(GuiTreeView, properties) == 0x184);
        static_assert(offsetof(GuiTreeView, color) == 0x138 && offsetof(GuiTreeView, onRight) == 0x180);

        // ZGE2's (DM2's) tree view record, after WidgetRecord (widget_parts.h):
        // no Scale of its own.
        // An item of the tree a ZGE2 tree view shows (the game builds the
        // tree and hands the view its RootNode and ParentNode; DM2's tree view
        // draw and input walk it): its text in itself.
        struct ListedTreeView
        {
            WidgetRecord widget;
            ee::Ptr<ListedTreeItem> rootNode;     // +0x60
            ee::Ptr<ListedTreeItem> parentNode;   // +0x64 the items shown are its children
            ee::Ptr<ListedTreeItem> selectedNode; // +0x68
            ee::Ptr<UiAsset> font;      // +0x6C found by charset (refreshGuiTreeView)
            int32_t visibleCount;       // +0x70
            int32_t scrollOffset;       // +0x74 the first item shown
            int32_t selectedIndex;      // +0x78
            int32_t lastSelectedIndex;  // +0x7C what OnChange last ran for (its define leaves it)
            uint32_t unused80;          // +0x80 no DM2 tree view function reads or writes it
            Rgba color;                 // +0x84
            Rgba selectedColor;         // +0x88
            ee::Float spacing;          // +0x8C
            zge::Vec4 selectedCoord;    // +0x90 from the view to the selected item
            uint32_t charset;           // +0xA0
            uint32_t onGetFocus;        // +0xA4 each a script (Event)
            uint32_t onLostFocus;       // +0xA8
            uint32_t onCancel;          // +0xAC
            uint32_t onItemFocus;       // +0xB0
            uint32_t onItemUnFocus;     // +0xB4
            uint32_t onItemSelect;      // +0xB8
            uint32_t onChange;          // +0xBC
            uint32_t onLeft;            // +0xC0
            uint32_t onRight;           // +0xC4
        };
        static_assert(offsetof(ListedTreeView, font) == 0x6C && offsetof(ListedTreeView, color) == 0x84);
        static_assert(offsetof(ListedTreeView, selectedCoord) == 0x90 && offsetof(ListedTreeView, onRight) == 0xC4);

        constexpr ListedProperty kListedTreeViewProperties[] = {
            {sym::g_textRootNode, sym::g_textVoid, offsetof(ListedTreeView, rootNode)},
            {sym::g_textParentNode, sym::g_textVoid, offsetof(ListedTreeView, parentNode)},
            {sym::g_textSelectedNode, sym::g_textVoid, offsetof(ListedTreeView, selectedNode)},
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(ListedTreeView, selectedIndex)},
            {sym::g_textVisibleCount, sym::g_textSint32, offsetof(ListedTreeView, visibleCount)},
            {sym::g_textSelectedCoord, sym::g_textZ3DVector, offsetof(ListedTreeView, selectedCoord)},
            {sym::g_textScrollOffset, sym::g_textSint32, offsetof(ListedTreeView, scrollOffset)},
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedTreeView, charset)},
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(ListedTreeView, spacing)},
            {sym::g_textColor, sym::dm2::g_textZrgba, offsetof(ListedTreeView, color)},
            {sym::g_textSelectedColor, sym::dm2::g_textZrgba, offsetof(ListedTreeView, selectedColor)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedTreeView, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedTreeView, onLostFocus)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedTreeView, onCancel)},
            {sym::g_textOnItemFocus, sym::g_textEvent, offsetof(ListedTreeView, onItemFocus)},
            {sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(ListedTreeView, onItemUnFocus)},
            {sym::g_textOnItemSelect, sym::g_textEvent, offsetof(ListedTreeView, onItemSelect)},
            {sym::g_textOnChange, sym::g_textEvent, offsetof(ListedTreeView, onChange)},
            {sym::g_textOnLeft, sym::g_textEvent, offsetof(ListedTreeView, onLeft)},
            {sym::g_textOnRight, sym::g_textEvent, offsetof(ListedTreeView, onRight)},
        };

        constexpr ClassProperty kTreeViewProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiTreeView, scale)}, // "Scale" Z3DVector
            {sym::g_textRootNode, sym::g_textVoid, offsetof(GuiTreeView, rootNode)}, // "RootNode" Void
            {sym::g_textParentNode, sym::g_textVoid, offsetof(GuiTreeView, parentNode)}, // "ParentNode" Void
            {sym::g_textSelectedNode, sym::g_textVoid, offsetof(GuiTreeView, selectedNode)}, // "SelectedNode" Void
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(GuiTreeView, selectedIndex)}, // "SelectedIndex" Sint32
            {sym::g_textVisibleCount, sym::g_textSint32, offsetof(GuiTreeView, visibleCount)}, // "VisibleCount" Sint32
            {sym::g_textSelectedCoord, sym::g_textZ3DVector, offsetof(GuiTreeView, selectedCoord)}, // "SelectedCoord" Z3DVector
            {sym::g_textScrollOffset, sym::g_textSint32, offsetof(GuiTreeView, scrollOffset)}, // "ScrollOffset" Sint32
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiTreeView, charset)}, // "Charset" ZAssetID
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(GuiTreeView, spacing)}, // "Spacing" ZFloat
            {sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiTreeView, color)}, // "Color" ZRGBA_PAL
            {sym::g_textSelectedColor, sym::g_textZrgbaPal, offsetof(GuiTreeView, selectedColor)}, // "SelectedColor" ZRGBA_PAL
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiTreeView, onGetFocus)}, // "OnGetFocus" Event
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiTreeView, onLostFocus)}, // "OnLostFocus" Event
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiTreeView, onCancel)}, // "OnCancel" Event
            {sym::g_textOnItemFocus, sym::g_textEvent, offsetof(GuiTreeView, onItemFocus)}, // "OnItemFocus" Event
            {sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(GuiTreeView, onItemUnFocus)}, // "OnItemUnFocus" Event
            {sym::g_textOnItemSelect, sym::g_textEvent, offsetof(GuiTreeView, onItemSelect)}, // "OnItemSelect" Event
            {sym::g_textOnChange, sym::g_textEvent, offsetof(GuiTreeView, onChange)}, // "OnChange" Event
            {sym::g_textOnLeft, sym::g_textEvent, offsetof(GuiTreeView, onLeft)}, // "OnLeft" Event
            {sym::g_textOnRight, sym::g_textEvent, offsetof(GuiTreeView, onRight)}, // "OnRight" Event
        };
    }

    // refreshGuiTreeView: the tree view's Refresh: its font, its two colours.
    uint32_t refreshGuiTreeView(uint32_t address)
    {
        const ee::Ptr<GuiTreeView> view(address);
        view->asset = findGuiAsset(view->charset, kAssetFont);
        if (view->color.paletteName)
            findPaletteColour(view->color.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiTreeView, color)));
        if (view->selectedColor.paletteName)
            findPaletteColour(view->selectedColor.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiTreeView, selectedColor)));
        return 1;
    }

    // refreshTreeViewFont: ZGE2's refreshGuiTreeView: its font found again. 1.
    int32_t refreshTreeViewFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedTreeView> view = messageRecord<ListedTreeView>(message);
        view->font = findGuiAsset(view->charset, kAssetFont);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiTreeView, Verified, dm2, refreshTreeViewFont);

    // drawGuiTreeView: the tree view's Draw: the scroll offset brought to the
    // selection, then the shown items drawn down from the view's position.
    // The selected one runs OnItemFocus every frame; the first other item
    // drawn after it, or the end of the list, runs OnItemUnFocus once. Then
    // OnChange if the selection moved.
    uint32_t drawGuiTreeView(uint32_t address)
    {
        // The values the item loop uses, which the original keeps on its stack.
        struct LoopValues
        {
            int32_t end;                     // the index past the last item shown
            ee::Ptr<zge::Vec4> position;     // the view's screen position
            ee::Ptr<ee::Float> styleScale;   // the text style's scale
            ee::Ptr<zge::Vec4> viewScale;    // the view's
        };
        ee::Local<zge::Vec4> pen;
        ee::Local<zge::TextStyle> style;
        ee::Local<LoopValues> kept;
        const ee::Ptr<GuiTreeView> view(address);

        bool focused = false;
        const auto runScriptOf = [&](uint32_t script) {
            runScript(&view->widget.node, ee::Ptr<char>(script), 0);
        };

        // OnChange when the selection moved since the last frame.
        const auto reportChange = [&]() {
            const int32_t selected = view->selectedIndex;
            if (view->lastSelectedIndex != selected)
            {
                const uint32_t onChange = view->onChange;
                view->lastSelectedIndex = selected;
                if (onChange)
                    runScriptOf(onChange);
            }
            return 1u;
        };

        if (!view->parentNode)
            return 1;
        ee::Ptr<UiAsset> asset = view->asset;
        if (!view->charset || !asset)
        {
            if (view->charset)
            {
                view->asset = findGuiAsset(view->charset, kAssetFont);
                asset = view->asset;
            }
            if (!asset)
                return reportChange();
        }
        if (view->visibleCount <= 0)
            return reportChange();

        const zge::Vec4 &position = view->widget.node.screenPosition;
        kept->position = &view->widget.node.screenPosition;
        *pen = position;

        // The first item shown keeps the selection in view.
        const int32_t visibleCount = view->visibleCount;
        const int32_t selected = view->selectedIndex;
        int32_t first = view->scrollOffset;
        if (selected >= first + visibleCount)
        {
            first = selected - visibleCount + 1;
            view->scrollOffset = first;
        }
        if (selected >= 0 && selected < first)
        {
            view->scrollOffset = selected;
            first = selected;
        }
        const int32_t end = first + visibleCount;
        kept->end = end;

        uint32_t index = 0;
        ee::Ptr<Node> item = nthChildOfKind(view->parentNode, 0, kTreeItem);
        if (item)
        {
            kept->styleScale = &style->scaleX;
            kept->viewScale = &view->scale;
            for (; item; item = nthChildOfKind(view->parentNode, ++index, kTreeItem))
            {
                const int32_t at = static_cast<int32_t>(index);
                if (at < first || at >= end)
                    continue;
                if (view->selectedIndex == at)
                {
                    const uint32_t onItemFocus = view->onItemFocus;
                    if (onItemFocus)
                    {
                        focused = true;
                        runScriptOf(onItemFocus);
                    }
                    view->selectedNode = item;
                    view->selectedCoord = *pen;
                    view->selectedCoord = view->selectedCoord - position;
                }
                else
                {
                    const uint32_t onItemUnFocus = view->onItemUnFocus;
                    if (onItemUnFocus && focused)
                    {
                        focused = false;
                        runScriptOf(onItemUnFocus);
                    }
                }

                style->position = *pen;
                std::memcpy(&style->scaleX, &view->scale, sizeof(zge::Vec4)); // the scale's four words
                style->monospaced = 0;
                style->angle = 0;
                style->align = 0;
                const bool isSelected = view->selectedIndex == at;
                const WidgetColour &colour = isSelected ? view->selectedColor : view->color;
                for (int channel = 0; channel < 4; ++channel)
                    style->colour[channel] = colour.rgba[channel];
                const ee::Float spacing = view->spacing;
                style->letterSpacing = 1;
                style->lineSpacing = spacing;
                zge::clearStyleReserved(*style);
                const ee::Ptr<UiAsset> font = view->asset;
                const uint32_t text = expandText(item->itemText);
                drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(text));
                pen->y = pen->y + ee::toFloat(fontHeight(view->asset)) * view->spacing;
            }
        }
        const uint32_t onItemUnFocus = view->onItemUnFocus;
        if (onItemUnFocus && focused)
            runScriptOf(onItemUnFocus);
        return reportChange();
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiTreeView, Verified, dm2, drawListedTreeView);

    // drawListedTreeView: ZGE2's drawGuiTreeView, the tree view's Draw with the
    // EngineMessage: the same scrolling, scripts and OnChange over the
    // ListedTreeView; the items are ParentNode's own list, drawn down from
    // the widget's position with its orientation and scale in the style.
    int32_t drawListedTreeView(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> pen;
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedTreeView> view = messageRecord<ListedTreeView>(message);
        const auto runScriptOf = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(static_cast<uint32_t>(message->target)), ee::Ptr<char>(script));
        };
        // OnChange when the selection moved since the last frame.
        const auto reportChange = [&]() {
            const int32_t selected = view->selectedIndex;
            if (view->lastSelectedIndex != selected)
            {
                const uint32_t onChange = view->onChange;
                view->lastSelectedIndex = selected;
                if (onChange)
                    runScriptOf(onChange);
            }
            return 1;
        };

        if (!view->parentNode)
            return 1;
        bool focused = false;
        if (view->charset && !view->font)
            view->font = findGuiAsset(view->charset, kAssetFont);
        if (!view->font)
            return reportChange();
        const int32_t visibleCount = view->visibleCount;
        if (visibleCount <= 0)
            return reportChange();

        const zge::Vec4 &position = view->widget.position;
        *pen = position;

        // The first item shown keeps the selection in view.
        const int32_t selected = view->selectedIndex;
        int32_t first = view->scrollOffset;
        if (selected >= first + visibleCount)
        {
            first = selected - visibleCount + 1;
            view->scrollOffset = first;
        }
        if (selected >= 0 && selected < first)
        {
            view->scrollOffset = selected;
            first = selected;
        }
        const int32_t end = first + visibleCount;

        int32_t at = 0;
        for (ee::Ptr<ListedTreeItem> item = view->parentNode->firstChild; item; item = item->next, ++at)
        {
            if (at < first || at >= end)
                continue;
            if (view->selectedIndex == at)
            {
                const uint32_t onItemFocus = view->onItemFocus;
                if (onItemFocus)
                {
                    focused = true;
                    runScriptOf(onItemFocus);
                }
                view->selectedNode = item;
                view->selectedCoord = *pen;
                view->selectedCoord = view->selectedCoord - position;
            }
            else
            {
                const uint32_t onItemUnFocus = view->onItemUnFocus;
                if (onItemUnFocus && focused)
                {
                    focused = false;
                    runScriptOf(onItemUnFocus);
                }
            }

            style->position = *pen;
            zge::quaternionCopy(zge::styleOrientation(*style), &view->widget.orientation);
            std::memcpy(&style->scaleX, &view->widget.scale, sizeof(zge::Vec4)); // the scale's four words
            style->align = 0;
            const Rgba &colour = view->selectedIndex == at ? view->selectedColor : view->color;
            std::memcpy(style->colour, &colour, sizeof(Rgba));
            style->letterSpacing = 1;
            style->lineSpacing = view->spacing;
            *reinterpret_cast<uint8_t *>(&style->reserved) = 0; // ZGE2 clears its one byte
            const ee::Ptr<UiAsset> font = view->font;
            const uint32_t text = expandText(ee::Ptr<const char>(item->text));
            drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(text));
            pen->y = pen->y + ee::toFloat(fontHeight(view->font)) * view->spacing;
        }
        const uint32_t onItemUnFocus = view->onItemUnFocus;
        if (onItemUnFocus && focused)
            runScriptOf(onItemUnFocus);
        return reportChange();
    }

    // handleGuiTreeViewInput: the tree view's Input: up and down move the selection
    // (with the blocked sound at either end), left and right run their
    // scripts, accept and cancel go down and up the tree.
    uint32_t handleGuiTreeViewInput(uint32_t address, uint32_t inputAddress)
    {
        const ee::Ptr<GuiTreeView> view(address);
        const ee::Ptr<Input> input(inputAddress);
        const uint32_t pressed = input->pressed, down = input->down;
        const auto held = [&](uint32_t mask) { return (pressed & mask) && (down & mask); };

        if (!view->parentNode)
            return 1u;

        const int32_t count = countChildrenOfKind(ee::Ptr<Node>(view->parentNode), kTreeItem);

        const auto runScriptOf = [&](uint32_t script) {
            runScript(ee::Ptr<Node>(address), ee::Ptr<char>(script), 0);
        };

        if (held(kButtonUp))
        {
            const int32_t selected = view->selectedIndex;
            if (selected > 0)
            {
                view->selectedIndex = selected - 1;
                if (const SoundHook hook = g_zgui->moveSound)
                    hook();
            }
            else
            {
                if (const SoundHook hook = g_zgui->blockedSound)
                    hook();
            }
        }

        if (held(kButtonDown))
        {
            const int32_t selected = view->selectedIndex;
            if (selected < count - 1)
            {
                if (const SoundHook hook = g_zgui->moveSound)
                {
                    hook();
                    view->selectedIndex = view->selectedIndex + 1;
                }
                else
                    view->selectedIndex = selected + 1;
            }
            else
            {
                const SoundHook hook = g_zgui->blockedSound;
                if (hook)
                    hook();
            }
        }

        if (held(kButtonLeft))
        {
            if (view->onLeft)
            {
                if (const SoundHook hook = g_zgui->changeSound)
                    hook();
                runScriptOf(view->onLeft);
            }
        }

        if (held(kButtonRight))
        {
            if (view->onRight)
            {
                if (const SoundHook hook = g_zgui->changeSound)
                    hook();
                runScriptOf(view->onRight);
            }
        }

        const ee::Ptr<Node> selectedNode = nthChildOfKind(view->parentNode, static_cast<uint32_t>(view->selectedIndex), kTreeItem);
        view->selectedNode = selectedNode;

        const uint32_t accept = g_zgui->acceptButton;
        if (held(accept))
        {
            if (countChildrenOfKind(selectedNode, kTreeItem))
            {
                view->selectedIndex = 0;
                view->parentNode = view->selectedNode;
            }
            else
            {
                const uint32_t onItemSelect = view->onItemSelect;
                if (!onItemSelect)
                    return 1u;
                runScriptOf(onItemSelect);
            }
            if (const SoundHook hook = g_zgui->selectSound)
                hook();
            return 1;
        }

        const uint32_t cancel = g_zgui->cancelButton;
        if (!(pressed & cancel))
            return 1u;
        if (!(down & cancel))
            return 1;
        // Back up: the item we were under is selected by node, not by index.
        const ee::Ptr<Node> shown = view->parentNode;
        view->selectedNode = shown;
        const ee::Ptr<Node> above = shown->parent;
        view->parentNode = above;
        if (above && static_cast<uint32_t>(above->kind()) != kTreeItem)
            view->parentNode = 0;
        view->selectedIndex = 0;
        if (!view->parentNode)
        {
            if (const uint32_t onCancel = view->onCancel)
            {
                runScriptOf(onCancel);
                const SoundHook hook = g_zgui->cancelSound;
                if (!hook)
                    return 1;
                hook();
            }
        }
        return 1u;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiTreeViewInput, Verified, dm2, pressListedTreeView);

    // pressListedTreeView: ZGE2's handleGuiTreeViewInput, the tree view's Input
    // with the EngineMessage: up and down move the selection among
    // ParentNode's items (the blocked sound at either end), left and right
    // run their scripts, accept goes into the selected item (or runs
    // OnItemSelect for one without items), cancel goes up to the parent
    // (silently; at the top ParentNode becomes none and OnCancel runs). 1.
    int32_t pressListedTreeView(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedTreeView> view = messageRecord<ListedTreeView>(message);
        const ee::Ptr<Input> input(message->data);
        const uint32_t pressed = input->pressed, down = input->down;
        const auto press = [&](uint32_t mask) { return (pressed & mask) && (down & mask); };
        const auto play = [&](SoundHook Zgui::*sound) {
            const SoundHook hook = (*g_zgui).*sound;
            if (hook)
                hook();
        };
        const auto run = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(static_cast<uint32_t>(message->target)), ee::Ptr<char>(script));
        };

        if (!view->parentNode)
            return 1;
        int32_t count = 0;
        for (ee::Ptr<ListedTreeItem> item = view->parentNode->firstChild; item; item = item->next)
            ++count;

        if (press(kButtonUp))
        {
            if (view->selectedIndex > 0)
            {
                view->selectedIndex = view->selectedIndex - 1;
                play(&Zgui::moveSound);
            }
            else
                play(&Zgui::blockedSound);
        }
        if (press(kButtonDown))
        {
            if (view->selectedIndex < count - 1)
            {
                play(&Zgui::moveSound);
                view->selectedIndex = view->selectedIndex + 1;
            }
            else
                play(&Zgui::blockedSound);
        }
        if (press(kButtonLeft) && view->onLeft)
        {
            play(&Zgui::changeSound);
            run(view->onLeft);
        }
        if (press(kButtonRight) && view->onRight)
        {
            play(&Zgui::changeSound);
            run(view->onRight);
        }

        // The selected item, by its place among the parent's.
        const ee::Ptr<ListedTreeItem> parent = view->parentNode;
        int32_t at = 0;
        for (ee::Ptr<ListedTreeItem> item = parent->firstChild; item; item = item->next, ++at)
        {
            if (at == view->selectedIndex)
                view->selectedNode = item;
        }

        if (press(g_zgui->acceptButton))
        {
            const ee::Ptr<ListedTreeItem> selected = view->selectedNode;
            if (selected->firstChild)
            {
                view->parentNode = selected;
                view->selectedIndex = 0;
            }
            else
            {
                const uint32_t onItemSelect = view->onItemSelect;
                if (!onItemSelect)
                    return 1;
                run(onItemSelect);
            }
            play(&Zgui::selectSound);
        }
        else if (press(g_zgui->cancelButton))
        {
            view->selectedNode = parent;
            const ee::Ptr<ListedTreeItem> above = parent->parent;
            view->selectedIndex = 0;
            view->parentNode = above;
            if (above)
                return 1;
            const uint32_t onCancel = view->onCancel;
            if (!onCancel)
                return 1;
            run(onCancel);
            play(&Zgui::cancelSound);
        }
        return 1;
    }

    // defineGuiTreeViewProperties: a tree view's listeners, defaults (white, yellow when
    // selected) and properties.
    uint32_t defineGuiTreeViewProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiTreeView> view(node);
        const uint32_t scale = node + offsetof(GuiTreeView, scale);
        defineWidgetProperties(node, text);
        addListener(node, Message::Input, sym::handleGuiTreeViewInput);
        addListener(node, Message::Draw, sym::drawGuiTreeView);
        addListener(node, Message::Refresh, sym::refreshGuiTreeView);
        addListener(node, Message::GetFocus, kRunOnGetFocus);
        addListener(node, Message::LostFocus, kRunOnLostFocus);
        defineClassProperties(view, view->properties, kTreeViewProperties);
        setWhite(view->color);
        setWhite(view->selectedColor);
        view->selectedColor.rgba[2] = 0;
        zge::vec3SetOne(ee::Ptr<zge::Vec3>(scale));
        view->selectedCoord = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        view->selectedNode = 0;
        view->spacing = ee::Float(1.25f);
        view->rootNode = 0;
        view->parentNode = 0;
        view->selectedIndex = 0;
        view->scrollOffset = 0;
        view->asset = {};
        view->charset = 0;
        view->visibleCount = 0;
        view->onChange = 0;
        view->onCancel = 0;
        view->onLostFocus = 0;
        view->onGetFocus = 0;
        view->onItemFocus = 0;
        view->onItemUnFocus = 0;
        view->onItemSelect = 0;
        view->onLeft = 0;
        view->onRight = 0;
        return node;
    }

    // defineListedTreeView: ZGE2's defineGuiTreeViewProperties: its handlers,
    // its properties, white with a yellow selection, spacing 1.25, the first
    // item selected (at (0, 0, 0, 1)).
    uint32_t defineListedTreeView(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedTreeView> view(address);
        defineWidgetRecord(address, name);
        subscribeWidget(object, Message::Input, sym::handleGuiTreeViewInput);
        subscribeWidget(object, Message::Draw, sym::drawGuiTreeView);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiTreeView);
        subscribeWidget(object, Message::GetFocus, kRunOnGetFocus);
        subscribeWidget(object, Message::LostFocus, kRunOnLostFocus);
        addListedProperties(view, kListedTreeViewProperties);
        view->color = {255, 255, 255, 255};
        view->selectedColor = {255, 255, 0, 255};
        view->selectedNode = {};
        view->spacing = ee::Float(1.25f);
        view->selectedCoord = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        view->rootNode = {};
        view->parentNode = {};
        view->selectedIndex = 0;
        view->scrollOffset = 0;
        view->font = {};
        view->charset = 0;
        view->visibleCount = 0;
        view->onChange = 0;
        view->onCancel = 0;
        view->onLostFocus = 0;
        view->onGetFocus = 0;
        view->onItemFocus = 0;
        view->onItemUnFocus = 0;
        view->onItemSelect = 0;
        view->onLeft = 0;
        view->onRight = 0;
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiTreeViewProperties, Verified, dm2, defineListedTreeView);
}

// ZGE2's tree items: made and freed by the game's menus (the tree view walks
// them).
namespace zge::zgui
{
    // freeTreeItem (ZGE2 only): the item freed, its children first (each
    // takes itself off its parent's list).
    inline void freeTreeItem(ee::Ptr<ListedTreeItem> item)
    {
        while (const ee::Ptr<ListedTreeItem> child = item->firstChild)
            freeTreeItem(child);
        if (const ee::Ptr<ListedTreeItem> parent = item->parent)
            zge::listRemove(childrenOf(parent), item);
        game::tail(zge::heapFree, item);
    }
    READABLE_FUNCTION_ONLY(dm2, freeTreeItem);
}

