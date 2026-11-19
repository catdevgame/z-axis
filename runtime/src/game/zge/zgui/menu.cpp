// zgui's menu: Items is one string of lines ended by a carriage return (and
// perhaps a line feed), one item a line. A line starting "!!" is a comment
// that takes no place on screen but still counts in SelectedIndex; one
// starting "!" is a disabled item: drawn in DisabledColor without its mark,
// selectable, but accept only plays the blocked sound.

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "zge/math/quaternion.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "zge/math/vec.h"
#include "sym/zge/zgui/menu.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kSelectedTextName = sym::g_textSelectedText; // "SelectedText"
        constexpr uint32_t kBlockAlignment = 0xF0;        // Alignment's top bits: the items as a block, vertically
        constexpr uint32_t kBlockTop = 0x00, kBlockMiddle = 0x10, kBlockBottom = 0x20;

        // ZGE2's (DM2's) menu record, after WidgetRecord (widget_parts.h): no
        // Scale, Horizontal or HorizontalSpacing of its own.
        struct ListedMenu
        {
            WidgetRecord widget;
            ee::Ptr<UiAsset> font;             // +0x60 found by charset (refreshGuiMenu)
            int32_t lastSelectedIndex;         // +0x64 what OnChange last ran for
            uint32_t wrapAround;               // +0x68 (its define leaves it as it was)
            int32_t visibleCount;              // +0x6C
            int32_t selectedIndex;             // +0x70 a line of Items
            uint32_t unused74;                 // +0x74 no DM2 menu function reads or writes it
            int32_t scrollOffset;              // +0x78 the first row shown
            uint32_t hideSelectionOnLostFocus; // +0x7C
            uint32_t alignment;                // +0x80
            Rgba color;                        // +0x84
            Rgba selectedColor;                // +0x88
            Rgba disabledColor;                // +0x8C
            ee::Float spacing;                 // +0x90
            uint32_t unused94[3];              // +0x94 no DM2 menu function reads or writes them
            zge::Vec4 selectedCoord;           // +0xA0 from the menu to the selected item
            uint32_t charset;                  // +0xB0
            uint32_t items;                    // +0xB4
            uint32_t onGetFocus;               // +0xB8 each a script (Event)
            uint32_t onLostFocus;              // +0xBC
            uint32_t onCancel;                 // +0xC0
            uint32_t onItemFocus;              // +0xC4
            uint32_t onItemUnFocus;            // +0xC8
            uint32_t onItemSelect;             // +0xCC
            uint32_t onChange;                 // +0xD0
        };
        static_assert(offsetof(ListedMenu, scrollOffset) == 0x78 && offsetof(ListedMenu, spacing) == 0x90);
        static_assert(offsetof(ListedMenu, selectedCoord) == 0xA0 && offsetof(ListedMenu, onChange) == 0xD0);

        constexpr ListedProperty kListedMenuProperties[] = {
            {sym::g_textWrapAround, sym::g_textBool, offsetof(ListedMenu, wrapAround)},
            {sym::g_textVisibleCount, sym::g_textSint32, offsetof(ListedMenu, visibleCount)},
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(ListedMenu, selectedIndex)},
            {sym::g_textSelectedCoord, sym::g_textZ3DVector, offsetof(ListedMenu, selectedCoord)},
            {sym::g_textHideSelectionOnLostFocus, sym::g_textBool, offsetof(ListedMenu, hideSelectionOnLostFocus)},
            {sym::g_textScrollOffset, sym::g_textSint32, offsetof(ListedMenu, scrollOffset)},
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedMenu, charset)},
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(ListedMenu, spacing)},
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(ListedMenu, alignment)},
            {sym::g_textColor, sym::dm2::g_textZrgba, offsetof(ListedMenu, color)},
            {sym::g_textSelectedColor, sym::dm2::g_textZrgba, offsetof(ListedMenu, selectedColor)},
            {sym::g_textDisabledColor, sym::dm2::g_textZrgba, offsetof(ListedMenu, disabledColor)},
            {sym::g_textItems2, sym::g_textString, offsetof(ListedMenu, items)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedMenu, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedMenu, onLostFocus)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedMenu, onCancel)},
            {sym::g_textOnItemFocus, sym::g_textEvent, offsetof(ListedMenu, onItemFocus)},
            {sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(ListedMenu, onItemUnFocus)},
            {sym::g_textOnItemSelect, sym::g_textEvent, offsetof(ListedMenu, onItemSelect)},
            {sym::g_textOnChange, sym::g_textEvent, offsetof(ListedMenu, onChange)},
        };

        constexpr char kCarriageReturn = '\r', kLineFeed = '\n', kItemMark = '!';


        using Text = ee::Ptr<const char>;

        // Items is one string of lines, each ended by a carriage return (and
        // perhaps a line feed). The end of the line at `line`: its carriage
        // return, or the string's end.
        Text lineEnd(Text line)
        {
            while (*line != kCarriageReturn && *line)
                ++line;
            return line;
        }

        // The start of the next line after a line's terminator at `end`.
        Text nextLine(Text end)
        {
            return (end[1] == kLineFeed ? end + 1 : end) + 1;
        }

        // "!!" starts a comment line: it takes no row, but counts in
        // SelectedIndex. "!" starts a disabled line.
        bool isComment(Text line) { return line[0] == kItemMark && line[1] == kItemMark; }

        // The text from `start` up to `end`, ended, into out.
        void copyLine(ee::Ptr<char> out, Text start, Text end)
        {
            const uint32_t length = static_cast<uint32_t>(end - start);
            zge::stringCopyN(out, start, length);
            out[length] = 0;
        }

        // Line `index` of the items goes into SelectedText. The line's start, or
        // 0 when the items have fewer lines.
        Text setSelectedText(ee::Ptr<GuiMenu> menu, Text items, int32_t index, ee::Ptr<char> buffer)
        {
            Text line = items;
            for (int32_t at = 0;; ++at)
            {
                const Text end = lineEnd(line);
                if (at == index)
                {
                    copyLine(buffer, line, end);
                    setVariable(menu, kSelectedTextName, buffer);
                    return line;
                }
                if (!*end)
                    return {};
                line = nextLine(end);
            }
        }

        void runItemScript(ee::Ptr<GuiMenu> menu, uint32_t script)
        {
            runScript(&menu->widget.node, ee::Ptr<char>(script), 0);
        }

        // A row's script, and the font found again in case it changed it.
        void runAndReload(ee::Ptr<GuiMenu> menu, uint32_t script)
        {
            runItemScript(menu, script);
            menu->asset = findGuiAsset(menu->charset, kAssetFont);
        }

        // After the rows: SelectedText and OnChange if the selection moved. A
        // selection of -1 sends SelectedCoord far off screen.
        void reportSelection(ee::Ptr<GuiMenu> menu, ee::Ptr<char> line)
        {
            const int32_t selected = menu->selectedIndex;
            if (menu->lastSelectedIndex == selected)
                return;
            if (selected == -1)
            {
                const ee::Float farOff(1e+04f);
                menu->selectedCoord.z = farOff;
                menu->selectedCoord.x = farOff;
                menu->selectedCoord.y = farOff;
            }
            const Text items(menu->items);
            menu->lastSelectedIndex = selected;
            setSelectedText(menu, items, selected, line);
            if (menu->onChange)
                runItemScript(menu, menu->onChange);
        }

        // OnItemUnFocus once the selected row has been left.
        void endItemFocus(ee::Ptr<GuiMenu> menu, bool &focused)
        {
            if (menu->onItemUnFocus && focused)
            {
                focused = false;
                runAndReload(menu, menu->onItemUnFocus);
            }
        }

        // The row the selected line is on: comment lines take none.
        int32_t rowOfLine(Text items, int32_t selected)
        {
            int32_t row = 0;
            Text line = items;
            for (int32_t index = 0;; ++index)
            {
                const Text end = lineEnd(line);
                if (index == selected)
                    break;
                if (!isComment(line))
                    ++row;
                if (!*end)
                    break;
                line = nextLine(end);
            }
            return row;
        }

        // The pen's start: the menu's position, moved up for the block's
        // vertical alignment. A bug kept: any alignment but top, middle or
        // bottom leaves the pen as the stack had it (no UI script has one).
        void placePen(ee::Ptr<GuiMenu> menu, zge::Vec4 &pen)
        {
            const uint32_t alignment = menu->alignment & kBlockAlignment;
            if (alignment != kBlockTop && alignment != kBlockMiddle && alignment != kBlockBottom)
                return;
            pen = menu->widget.node.screenPosition;
            if (alignment == kBlockTop)
                return;
            const ee::Float blockHeight = ee::toFloat(menu->visibleCount) * menu->spacing * (menu->scale.y * ee::toFloat(fontHeight(menu->asset)));
            pen.y = pen.y - (alignment == kBlockMiddle ? blockHeight * ee::Float(0.5f) : blockHeight);
        }

        // The shown row's text, in the colour already in the style, and the pen
        // moved on. False without a font.
        bool drawRow(ee::Ptr<GuiMenu> menu, zge::Vec4 &pen, ee::Ptr<zge::TextStyle> style, ee::Ptr<char> text)
        {
            style->position = pen;
            std::memcpy(&style->scaleX, &menu->scale, sizeof(zge::Vec4));
            const ee::Float spacing = menu->spacing;
            const ee::Ptr<UiAsset> font = menu->asset;
            style->letterSpacing = 1;
            style->align = menu->alignment & 0xFu;
            style->lineSpacing = spacing;
            style->monospaced = 0;
            style->angle = ee::Float(0.0f);
            style->reserved = 0;
            if (!font)
                return false;
            const uint32_t expanded = expandText(text);
            drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(expanded));
            const ee::Float height = style->height;
            if (!menu->horizontal)
            {
                const ee::Float width = style->width;
                if (menu->width < width)
                    menu->width = width;
                pen.y = pen.y + ee::toFloat(fontHeight(menu->asset)) * menu->spacing;
            }
            else
            {
                if (menu->height < height)
                    menu->height = height;
                pen.x = pen.x + menu->horizontalSpacing * menu->scale.x;
            }
            return true;
        }
    }

    // guiMenuGetFocus: the menu's GetFocus: a menu with no selection selects its
    // first line, and the blocked sound may play again.
    uint32_t guiMenuGetFocus(ee::Ptr<GuiMenu> menu)
    {
        menu->blockedPlayed = 0;
        if (menu->selectedIndex == -1)
        {
            menu->scrollOffset = 0;
            menu->selectedIndex = 0;
        }
        runOnGetFocus(menu);
        return 1;
    }

    // focusListedMenu: ZGE2's guiMenuGetFocus: a menu with no selection
    // selects its first line (ZGE3 also scrolls to the top and lets the
    // blocked sound play again), then OnGetFocus. 1.
    int32_t focusListedMenu(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        if (menu->selectedIndex == -1)
            menu->selectedIndex = 0;
        runListedGetFocus(message);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(guiMenuGetFocus, Verified, dm2, focusListedMenu, bmx, focusArrowedMenu);

    // guiMenuLostFocus: the menu's LostFocus.
    uint32_t guiMenuLostFocus(ee::Ptr<GuiMenu> menu)
    {
        runOnLostFocus(menu);
        return 1;
    }

    // unfocusListedMenu: ZGE2's guiMenuLostFocus: with
    // HideSelectionOnLostFocus the selection's mark is moved off the screen
    // (ZGE3's draw leaves the selection out instead), then OnLostFocus. 1.
    int32_t unfocusListedMenu(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        if (menu->hideSelectionOnLostFocus)
        {
            const ee::Float hidden(10000.0f); // far off the screen
            menu->selectedCoord.z = hidden;
            menu->selectedCoord.x = hidden;
            menu->selectedCoord.y = hidden;
        }
        runListedLostFocus(message);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(guiMenuLostFocus, Verified, dm2, unfocusListedMenu, bmx, unfocusArrowedMenu);

    // refreshGuiMenu: the menu's Refresh: its font, its three colours.
    uint32_t refreshGuiMenu(ee::Ptr<GuiMenu> menu)
    {
        menu->asset = findGuiAsset(menu->charset, kAssetFont);
        lookUpPaletteColour(menu->color);
        lookUpPaletteColour(menu->selectedColor);
        lookUpPaletteColour(menu->disabledColor);
        return 1;
    }

    // refreshMenuFont: ZGE2's refreshGuiMenu: its font found again (ZGE2's
    // colours have no palette names). 1.
    int32_t refreshMenuFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        menu->font = findGuiAsset(menu->charset, kAssetFont);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiMenu, Verified, dm2, refreshMenuFont);

    // drawGuiMenu: the menu's Draw: the row of the selected line found, the
    // scroll offset brought to it, the rows shown drawn from the menu's
    // position, their width and height measured; OnItemFocus each frame for
    // the selected row and OnItemUnFocus once after it. Then, if the selection
    // moved, SelectedText and OnChange.
    uint32_t drawGuiMenu(ee::Ptr<GuiMenu> menu)
    {
        ee::Local<zge::Vec4> pen;
        ee::Local<char[0x100]> lineText;
        ee::Local<zge::TextStyle> style;
        bool focused = false;
        const auto finish = [&]() {
            endItemFocus(menu, focused);
            reportSelection(menu, lineText);
            return 1u;
        };

        if (!menu->items)
            return 1;

        ee::Ptr<UiAsset> asset = menu->asset;
        if (!menu->charset || !asset)
        {
            if (menu->charset)
            {
                menu->asset = findGuiAsset(menu->charset, kAssetFont);
                asset = menu->asset;
            }
            if (!asset)
                return finish();
        }
        if (menu->visibleCount <= 0)
            return finish();

        placePen(menu, *pen);
        const Text items(menu->items);

        // The scroll offset brought to the selected row.
        const int32_t row = rowOfLine(items, menu->selectedIndex);
        int32_t first = menu->scrollOffset;
        if (row >= first + menu->visibleCount)
        {
            first = row - menu->visibleCount + 1;
            menu->scrollOffset = first;
        }
        if (row >= 0 && row < first)
        {
            menu->scrollOffset = row;
            first = row;
        }
        menu->width = ee::Float(0.0f);
        menu->height = ee::Float(0.0f);
        const int32_t endShown = first + menu->visibleCount;

        int32_t item = 0;
        Text line = items;
        for (Text cursor = items;;)
        {
            cursor = lineEnd(cursor);
            copyLine(lineText, line, cursor);
            Text end = cursor;
            if (*end && end[1] == kLineFeed)
                ++end;
            line = end + 1;

            if (!(lineText[0] == kItemMark && lineText[1] == kItemMark))
            {
                bool measured = true;
                ee::Ptr<UiAsset> font;
                if (item < first || item >= endShown)
                    font = menu->asset;
                else
                {
                    if (row == item && (g_zgui->focus == &menu->widget.node || !menu->hideSelectionOnLostFocus))
                    {
                        menu->selectedCoord = *pen - menu->widget.node.screenPosition;
                        if (menu->onItemFocus)
                        {
                            focused = true;
                            runAndReload(menu, menu->onItemFocus);
                        }
                        std::memcpy(style->colour, menu->selectedColor.rgba, 4);
                    }
                    else
                    {
                        endItemFocus(menu, focused);
                        std::memcpy(style->colour, menu->color.rgba, 4);
                    }
                    const bool disabled = lineText[0] == kItemMark;
                    if (disabled)
                        std::memcpy(style->colour, menu->disabledColor.rgba, 4);
                    // A disabled line is shown without its mark.
                    measured = drawRow(menu, *pen, style, ee::Ptr<char>(lineText) + (disabled ? 1 : 0));
                    font = menu->asset;
                }
                if (measured && font)
                {
                    if (!menu->horizontal)
                        menu->height = menu->height + ee::toFloat(fontHeight(font)) * menu->spacing;
                    else if (*end)
                        menu->width = menu->width + menu->horizontalSpacing * menu->scale.x;
                }
                ++item;
            }
            if (!*end)
                break;
            cursor = end + 1;
        }
        return finish();
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiMenu, Verified, dm2, drawListedMenu, bmx, drawArrowedMenu);

    // setWidgetAlpha (ZGE4 only): the widget's colour properties that it has
    // (TopLeftColor, TopRightColor, BottomLeftColor, BottomRightColor and
    // Color) get the alpha 255 * amount. Nothing without a widget.
    inline void setWidgetAlpha(ee::Ptr<Node> widget, ee::Float amount)
    {
        ee::Local<WidgetColour> colour;
        if (!widget)
            return;
        for (const uint32_t name : {sym::g_textTopLeftColor, sym::g_textTopRightColor, sym::g_textBottomLeftColor,
                                    sym::g_textBottomRightColor, sym::g_textColor})
        {
            if (copyVariable(widget, name, colour))
            {
                colour->rgba[3] = static_cast<uint8_t>(ee::toInt(amount * ee::Float(255.0f)));
                setVariable(widget, name, colour);
            }
        }
    }
    READABLE_FUNCTION_ONLY(bmx, setWidgetAlpha);

    namespace
    {
        // A ZGE4 menu's script that shows or hides one of its arrows (the
        // widget at the arrow's path): "SET <path>.Visible {1}" or "{0}".
        void runArrowScript(ee::Ptr<GuiMenu> menu, ee::Ptr<char> script, bool shown, uint32_t arrow)
        {
            if (shown)
                zge::formatString(script, sym::bmx::g_formatShowWidget, arrow);
            else
                zge::formatString(script, sym::bmx::g_formatHideWidget, arrow);
            runItemScript(menu, ee::Address(script));
        }

        // An arrow's fade a draw on (down by 0.2 to 0), and the arrow's
        // widget as bright as the fade is far gone.
        void fadeArrow(ee::Ptr<GuiMenu> menu, ee::Float &flash, uint32_t arrow)
        {
            const ee::Float zero(0.0f);
            if (!(zero < flash))
                return;
            const ee::Float faded = flash - ee::Float(0.2f);
            flash = faded < zero ? zero : faded;
            if (arrow)
                setWidgetAlpha(findNode(&menu->widget.node, arrow, 1), ee::Float(1.0f) - flash);
        }
    }

    // drawArrowedMenu: ZGE4's drawGuiMenu (BMX XXX), the menu's Draw: as
    // ZGE3's, and the StartArrow and EndArrow widgets lit back up as their
    // fades go (setWidgetAlpha). Other differences: no selection (-1) is the
    // first row; the block's middle is VisibleCount - 1 rows up by half; the
    // rows are drawn with the whole Alignment and a line spacing of 1, their
    // steps and the measured size scaled by Scale; a disabled row that is the
    // shown selection is in half its colour; a vertical menu's Width is its
    // widest row over Scale's x and its Height the visible rows'. After the
    // rows, a changed selection shows the StartArrow (hides it at the first
    // line, without WrapAround), and -1 empties SelectedText and runs no
    // OnChange. 1.
    int32_t drawArrowedMenu(ee::Ptr<GuiMenu> menu)
    {
        ee::Local<zge::Vec4> pen;
        ee::Local<char[0x100]> lineText;
        ee::Local<zge::TextStyle> style;
        ee::Local<char[0x100]> arrowScript;
        ee::Local<char[0x100]> selectedLine;
        bool focused = false;
        ee::Float widest(0.0f);

        if (!menu->items)
            return 1;
        if (menu->charset && !menu->asset)
            menu->asset = findGuiAsset(menu->charset, kAssetFont);
        menu->width = ee::Float(0.0f);
        menu->height = ee::Float(0.0f);
        const zge::Vec4 scale = menu->scale; // (as the draw started: a row's script may change it)
        fadeArrow(menu, startArrowFlash(menu), startArrow(menu));
        fadeArrow(menu, endArrowFlash(menu), endArrow(menu));

        if (menu->asset && menu->visibleCount > 0)
        {
            const auto lineHeight = [&]() { return ee::toFloat(fontHeight(menu->asset)) * menu->spacing; };
            // The pen's start (any alignment but top, middle or bottom leaves
            // it as the stack had it: kept, as ZGE3's).
            const uint32_t block = menu->alignment & kBlockAlignment;
            if (block == kBlockTop)
                *pen = menu->widget.node.screenPosition;
            else if (block == kBlockMiddle)
            {
                *pen = menu->widget.node.screenPosition;
                pen->y = pen->y - ee::toFloat(menu->visibleCount - 1) * scale.y * lineHeight() * ee::Float(0.5f);
            }
            else if (block == kBlockBottom)
            {
                *pen = menu->widget.node.screenPosition;
                pen->y = pen->y - ee::toFloat(menu->visibleCount) * menu->spacing * (scale.y * ee::toFloat(fontHeight(menu->asset)));
            }

            const Text items(menu->items);
            const int32_t row = rowOfLine(items, menu->selectedIndex < 1 ? 0 : menu->selectedIndex);
            int32_t first = menu->scrollOffset;
            if (row >= first + menu->visibleCount)
            {
                first = row - menu->visibleCount + 1;
                menu->scrollOffset = first;
            }
            if (row >= 0 && row < first)
            {
                menu->scrollOffset = row;
                first = row;
            }
            const int32_t endShown = first + menu->visibleCount;

            int32_t item = 0;
            Text line = items;
            for (Text cursor = items;;)
            {
                cursor = lineEnd(cursor);
                copyLine(lineText, line, cursor);
                Text end = cursor;
                if (*end && end[1] == kLineFeed)
                    ++end;
                line = end + 1;

                if (!(lineText[0] == kItemMark && lineText[1] == kItemMark))
                {
                    bool measured = true;
                    if (item >= first && item < endShown)
                    {
                        const bool shownSelected = row == item && (g_zgui->focus == &menu->widget.node || !menu->hideSelectionOnLostFocus);
                        if (shownSelected)
                        {
                            menu->selectedCoord = *pen - menu->widget.node.screenPosition;
                            if (menu->onItemFocus)
                            {
                                focused = true;
                                runAndReload(menu, menu->onItemFocus);
                            }
                            std::memcpy(style->colour, menu->selectedColor.rgba, 4);
                        }
                        else
                        {
                            endItemFocus(menu, focused);
                            std::memcpy(style->colour, menu->color.rgba, 4);
                        }
                        // A disabled line is shown without its mark, in half
                        // its colour when it is the selection.
                        uint32_t markLength = 0;
                        if (lineText[0] == kItemMark)
                        {
                            std::memcpy(style->colour, menu->disabledColor.rgba, 4);
                            if (row == item && (g_zgui->focus == &menu->widget.node || !menu->hideSelectionOnLostFocus))
                                for (int channel = 0; channel < 3; ++channel)
                                    style->colour[channel] = static_cast<uint8_t>(style->colour[channel] >> 1);
                            markLength = 1;
                        }
                        style->position = *pen;
                        std::memcpy(&style->scaleX, &menu->scale, sizeof(zge::Vec4));
                        style->monospaced = 0;
                        style->angle = ee::Float(0.0f);
                        const ee::Ptr<UiAsset> font = menu->asset;
                        style->align = menu->alignment;
                        zge::clearStyleReserved(*style);
                        style->lineSpacing = ee::Float(1.0f);
                        style->letterSpacing = 1;
                        if (!font)
                            measured = false;
                        else
                        {
                            const uint32_t text = expandText(ee::Ptr<const char>(lineText) + markLength);
                            drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(text));
                            if (!menu->horizontal)
                            {
                                if (widest < style->width)
                                    widest = style->width;
                                pen->y = pen->y + lineHeight() * scale.y;
                            }
                            else
                            {
                                if (menu->height < style->height)
                                    menu->height = style->height;
                                pen->x = pen->x + menu->horizontalSpacing * scale.x;
                            }
                        }
                    }
                    if (measured && menu->asset)
                    {
                        if (!menu->horizontal)
                            menu->height = menu->height + lineHeight() * scale.y;
                        else if (*end)
                            menu->width = menu->width + menu->horizontalSpacing * scale.x;
                    }
                    ++item;
                }
                if (!*end)
                    break;
                cursor = end + 1;
            }
            if (!menu->horizontal)
            {
                menu->width = widest / scale.x;
                menu->height = lineHeight() * ee::toFloat(menu->visibleCount);
            }
        }
        endItemFocus(menu, focused);

        // The selection's report.
        if (menu->lastSelectedIndex == menu->selectedIndex)
            return 1;
        if (menu->selectedIndex == 0 && !menu->wrapAround && startArrow(menu))
            runArrowScript(menu, lineText, false, startArrow(menu));
        else if (g_zgui->focus == &menu->widget.node && startArrow(menu))
            runArrowScript(menu, arrowScript, true, startArrow(menu));
        if (menu->selectedIndex == -1)
        {
            const ee::Float farOff(1e+04f);
            menu->selectedCoord.z = farOff;
            menu->selectedCoord.x = farOff;
            menu->selectedCoord.y = farOff;
            setVariable(menu, kSelectedTextName, sym::bmx::g_textEmpty);
            return 1;
        }
        const int32_t selected = menu->selectedIndex;
        menu->lastSelectedIndex = selected;
        setSelectedText(menu, Text(menu->items), selected, selectedLine);
        if (menu->onChange)
            runItemScript(menu, menu->onChange);
        return 1;
    }

    // drawListedMenu: ZGE2's drawGuiMenu, the menu's Draw: Items' lines (CR
    // LF between them) drawn one below the other from the scroll offset,
    // visibleCount of them, the block placed by the alignment's vertical
    // part; the scroll offset is first brought to the selected line. The
    // selected line (while the menu has the focus, or always unless
    // HideSelectionOnLostFocus) is in its colour, sets SelectedCoord and
    // runs OnItemFocus; the next other line, or the end, runs OnItemUnFocus.
    // A line starting with "!" is disabled: its colour, drawn without the
    // mark. Then a changed SelectedIndex is noted (-1 puts SelectedCoord far
    // away) and OnChange run. ZGE2 has no comment lines, no horizontal
    // menus and keeps no size. 1.
    int32_t drawListedMenu(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> pen;
        ee::Local<char[0x400]> text;
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        bool focused = false;
        const auto runScriptOf = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };
        // A changed selection noted, OnChange run.
        const auto noteSelection = [&]() {
            const int32_t selected = menu->selectedIndex;
            if (menu->lastSelectedIndex == selected)
                return 1;
            if (selected == -1)
            {
                const ee::Float hidden(10000.0f); // far off the screen
                menu->selectedCoord.z = hidden;
                menu->selectedCoord.x = hidden;
                menu->selectedCoord.y = hidden;
            }
            const uint32_t onChange = menu->onChange;
            menu->lastSelectedIndex = selected;
            if (onChange)
                runScriptOf(onChange);
            return 1;
        };
        if (!menu->items)
            return 1;
        if (!menu->charset || !menu->font)
        {
            if (menu->charset)
                menu->font = findGuiAsset(menu->charset, kAssetFont);
            if (!menu->font)
                return noteSelection();
        }
        if (menu->visibleCount <= 0)
            return noteSelection();

        // The pen at the first line: the block of visibleCount lines hangs
        // from the position, is centred on it or stands on it.
        const uint32_t vertical = menu->alignment & 0xF0;
        if (vertical == 0x00)
            *pen = menu->widget.position;
        else if (vertical == 0x10)
        {
            *pen = menu->widget.position;
            const ee::Float lines = ee::toFloat(menu->visibleCount) * menu->spacing;
            pen->y = pen->y - lines * (menu->widget.scale.y * ee::toFloat(fontHeight(menu->font))) * ee::Float(0.5f);
        }
        else if (vertical == 0x20)
        {
            *pen = menu->widget.position;
            const ee::Float lines = ee::toFloat(menu->visibleCount) * menu->spacing;
            pen->y = pen->y - lines * (menu->widget.scale.y * ee::toFloat(fontHeight(menu->font)));
        }
        zge::stringCopy(text, menu->items);

        // The scroll offset brought to the selected line.
        const int32_t selected = menu->selectedIndex;
        int32_t first = menu->scrollOffset;
        if (!(selected < first + menu->visibleCount))
        {
            first = selected - menu->visibleCount + 1;
            menu->scrollOffset = first;
        }
        if (selected >= 0 && selected < first)
        {
            menu->scrollOffset = selected;
            first = selected;
        }
        const int32_t endShown = first + menu->visibleCount;

        ee::Ptr<char> line = text;
        uint32_t at = 0;
        for (int32_t row = 0;; ++row)
        {
            // The line's end: a CR (made the line's end) or the text's.
            while (text[at] != kCarriageReturn && text[at] != 0)
                ++at;
            if (text[at] != 0)
            {
                text[at] = 0;
                ++at;
            }
            if (!(row < first) && row < endShown)
            {
                if (menu->selectedIndex == row &&
                    (ee::Address(g_zgui->focus) == message->target || !menu->hideSelectionOnLostFocus))
                {
                    if (const uint32_t onItemFocus = menu->onItemFocus)
                    {
                        runScriptOf(onItemFocus);
                        focused = true;
                    }
                    menu->selectedCoord = *pen;
                    menu->selectedCoord = menu->selectedCoord - menu->widget.position;
                    std::memcpy(style->colour, &menu->selectedColor, sizeof(Rgba));
                }
                else
                {
                    const uint32_t onItemUnFocus = menu->onItemUnFocus;
                    if (onItemUnFocus && focused)
                    {
                        runScriptOf(onItemUnFocus);
                        focused = false;
                    }
                    std::memcpy(style->colour, &menu->color, sizeof(Rgba));
                }
                if (line[0] == kItemMark)
                {
                    std::memcpy(style->colour, &menu->disabledColor, sizeof(Rgba));
                    ++line;
                }
                style->position = *pen;
                zge::quaternionCopy(zge::styleOrientation(*style), &menu->widget.orientation);
                std::memcpy(&style->scaleX, &menu->widget.scale, sizeof(zge::Vec4)); // the scale's four words
                style->align = menu->alignment & 0xF;
                style->letterSpacing = 1;
                style->lineSpacing = menu->spacing;
                std::memset(&style->reserved, 0, 1); // ZGE2 clears its one byte
                const ee::Ptr<UiAsset> font = menu->font;
                const uint32_t expanded = expandText(line);
                drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(expanded));
                pen->y = pen->y + ee::toFloat(fontHeight(menu->font)) * menu->spacing;
            }
            // The byte after the CR (an LF) is skipped; the text's end ends it.
            if (text[at] == 0)
                break;
            ++at;
            line = ee::Ptr<char>(text) + at;
        }
        if (menu->onItemUnFocus && focused)
            runScriptOf(menu->onItemUnFocus);
        return noteSelection();
    }

    // handleGuiMenuInput: the menu's Input: the lines before and after the selected
    // one found (comments skipped); up and down, or left and right on a
    // horizontal menu, move to them, wrapping if WrapAround is set and
    // otherwise playing the blocked sound once; the new line goes into
    // SelectedText; accept runs OnItemSelect unless the line is disabled;
    // cancel runs OnCancel.
    uint32_t handleGuiMenuInput(ee::Ptr<GuiMenu> menu, ee::Ptr<Input> input)
    {
        ee::Local<char[0x170]> lineText;
        const uint32_t pressed = input->pressed, down = input->down;
        const auto held = [&](uint32_t mask) { return (pressed & mask) && (down & mask); };

        if (!menu->items)
            return 1u;

        // The first and last items, and the nearest either side of the
        // selection.
        int32_t firstItem = 10000, lastItem = -10000, previousItem = -1, nextItem = 10000;
        int32_t selected = menu->selectedIndex;
        {
            Text line(menu->items);
            for (int32_t index = 0;; ++index)
            {
                const Text end = lineEnd(line);
                if (!isComment(line))
                {
                    firstItem = index < firstItem ? index : firstItem;
                    lastItem = lastItem < index ? index : lastItem;
                    if (index < selected)
                        previousItem = previousItem < index ? index : previousItem;
                    if (selected < index)
                        nextItem = index < nextItem ? index : nextItem;
                }
                if (!*end)
                    break;
                line = nextLine(end);
            }
        }

        const auto moveTo = [&](int32_t line) {
            menu->selectedIndex = line;
            menu->blockedPlayed = 0;
            selected = line;
            if (const SoundHook sound = g_zgui->moveSound)
            {
                sound();
                selected = menu->selectedIndex;
            }
        };
        // At the end with no wrapping: the blocked sound, once.
        const auto refuse = [&]() {
            const SoundHook sound = g_zgui->blockedSound;
            if (!sound || menu->blockedPlayed)
                return;
            sound();
            selected = menu->selectedIndex;
            menu->blockedPlayed = 1;
        };
        const auto moveBack = [&]() {
            if (firstItem < selected)
                moveTo(previousItem);
            else if (menu->wrapAround)
                moveTo(lastItem);
            else
                refuse();
        };
        const auto moveForward = [&]() {
            if (selected < lastItem)
                moveTo(nextItem);
            else if (menu->wrapAround)
                moveTo(firstItem);
            else
                refuse();
        };

        if (held(menu->horizontal ? kButtonLeft : kButtonUp))
            moveBack();
        else if (held(menu->horizontal ? kButtonRight : kButtonDown))
            moveForward();

        // SelectedText: the selected line, and whether it may be selected.
        const Text line = setSelectedText(menu, Text(menu->items), selected, lineText);
        const bool selectable = line && *line != kItemMark;

        if (held(g_zgui->acceptButton))
        {
            if (menu->onItemSelect && selectable)
            {
                if (const SoundHook sound = g_zgui->selectSound)
                    sound();
                runItemScript(menu, menu->onItemSelect);
                return 1u;
            }
            if (const SoundHook sound = g_zgui->blockedSound)
                sound();
            return 1u;
        }

        if (!held(g_zgui->cancelButton))
            return 1u;
        if (menu->onCancel)
        {
            if (const SoundHook sound = g_zgui->cancelSound)
                sound();
            runItemScript(menu, menu->onCancel);
            return 1u;
        }
        if (const SoundHook sound = g_zgui->blockedSound)
            sound();
        return 1u;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiMenuInput, Verified, dm2, pressListedMenu, bmx, pressArrowedMenu);

    // focusArrowedMenu: ZGE4's guiMenuGetFocus (BMX XXX), the menu's
    // GetFocus: as ZGE3's (no selection selects the first line, scrolled to
    // the top, and the blocked sound may play again), and the arrows' fades
    // over; the StartArrow hidden at a non-wrapping menu's first line (with
    // Items), else shown, the EndArrow shown while Items has more than one
    // line, else hidden, each at full alpha; with Items, SelectedText and
    // OnChange for the selection; then OnGetFocus. 1.
    uint32_t focusArrowedMenu(ee::Ptr<GuiMenu> menu)
    {
        ee::Local<char[0x100]> script;
        ee::Local<char[0x100]> selectedLine;
        menu->blockedPlayed = 0;
        startArrowFlash(menu) = ee::Float(0.0f);
        endArrowFlash(menu) = ee::Float(0.0f);
        if (menu->selectedIndex == -1)
        {
            menu->lastSelectedIndex = -1;
            menu->scrollOffset = 0;
            menu->selectedIndex = 0;
        }
        const auto hasItems = [&] { return menu->items && *Text(menu->items) != 0; };
        if (startArrow(menu))
        {
            const bool atFirstLine = !menu->wrapAround && menu->selectedIndex == 0 && hasItems();
            runArrowScript(menu, script, !atFirstLine, startArrow(menu));
            setWidgetAlpha(findNode(&menu->widget.node, startArrow(menu), 1), ee::Float(1.0f));
        }
        if (endArrow(menu))
        {
            const bool moreLines = hasItems() && zge::stringFind(Text(menu->items), kCarriageReturn);
            runArrowScript(menu, script, moreLines, endArrow(menu));
            setWidgetAlpha(findNode(&menu->widget.node, endArrow(menu), 1), ee::Float(1.0f));
        }
        if (hasItems())
        {
            const int32_t selected = menu->selectedIndex;
            menu->lastSelectedIndex = selected;
            setSelectedText(menu, Text(menu->items), selected, selectedLine);
            if (menu->onChange)
                runItemScript(menu, menu->onChange);
        }
        runOnGetFocus(menu);
        return 1;
    }

    // unfocusArrowedMenu: ZGE4's guiMenuLostFocus: with
    // HideSelectionOnLostFocus its StartArrow and EndArrow hidden, then
    // OnLostFocus. 1.
    uint32_t unfocusArrowedMenu(ee::Ptr<GuiMenu> menu)
    {
        ee::Local<char[0x100]> script;
        if (startArrow(menu) && menu->hideSelectionOnLostFocus)
            runArrowScript(menu, script, false, startArrow(menu));
        if (endArrow(menu) && menu->hideSelectionOnLostFocus)
            runArrowScript(menu, script, false, endArrow(menu));
        runOnLostFocus(menu);
        return 1;
    }

    // pressArrowedMenu: ZGE4's handleGuiMenuInput (BMX XXX), the menu's
    // Input: ZGE3's, with its arrows. A step back from the last line (without
    // WrapAround) shows the EndArrow; each step back lights the start arrow's
    // fade and shows the StartArrow, or hides it on reaching the first line
    // (without WrapAround); a step on lights the end arrow's fade and shows
    // the EndArrow, or hides it on reaching the last line. 1.
    uint32_t pressArrowedMenu(ee::Ptr<GuiMenu> menu, ee::Ptr<Input> input)
    {
        ee::Local<char[0x100]> lineText;
        ee::Local<char[0x100]> script;
        ee::Local<char[0x100]> secondScript;
        const uint32_t pressed = input->pressed, down = input->down;
        const auto held = [&](uint32_t mask) { return (pressed & mask) && (down & mask); };

        if (!menu->items)
            return 1u;

        // The first and last items, and the nearest either side of the
        // selection.
        int32_t firstItem = 10000, lastItem = -10000, previousItem = -1, nextItem = 10000;
        int32_t selected = menu->selectedIndex;
        {
            Text line(menu->items);
            for (int32_t index = 0;; ++index)
            {
                const Text end = lineEnd(line);
                if (!isComment(line))
                {
                    firstItem = index < firstItem ? index : firstItem;
                    lastItem = lastItem < index ? index : lastItem;
                    if (index < selected)
                        previousItem = previousItem < index ? index : previousItem;
                    if (selected < index)
                        nextItem = index < nextItem ? index : nextItem;
                }
                if (!*end)
                    break;
                line = nextLine(end);
            }
        }

        const auto moveTo = [&](int32_t line, ee::Float &flash) {
            menu->selectedIndex = line;
            flash = ee::Float(1.0f);
            menu->blockedPlayed = 0;
            selected = line;
            if (const SoundHook sound = g_zgui->moveSound)
            {
                sound();
                selected = menu->selectedIndex;
            }
        };
        // At the end with no wrapping: the blocked sound, once.
        const auto refuse = [&]() {
            const SoundHook sound = g_zgui->blockedSound;
            if (!sound || menu->blockedPlayed)
                return;
            sound();
            selected = menu->selectedIndex;
            menu->blockedPlayed = 1;
        };
        const auto moveBack = [&]() {
            if (firstItem < selected)
            {
                if (selected == lastItem && !menu->wrapAround && endArrow(menu))
                    runArrowScript(menu, lineText, true, endArrow(menu));
                moveTo(previousItem, startArrowFlash(menu));
                if (startArrow(menu))
                {
                    if (previousItem == firstItem && !menu->wrapAround)
                        runArrowScript(menu, script, false, startArrow(menu));
                    else
                        runArrowScript(menu, secondScript, true, startArrow(menu));
                }
            }
            else if (menu->wrapAround)
                moveTo(lastItem, startArrowFlash(menu));
            else
                refuse();
        };
        const auto moveForward = [&]() {
            if (selected < lastItem)
            {
                moveTo(nextItem, endArrowFlash(menu));
                if (endArrow(menu))
                {
                    if (nextItem == lastItem && !menu->wrapAround)
                        runArrowScript(menu, lineText, false, endArrow(menu));
                    else
                        runArrowScript(menu, script, true, endArrow(menu));
                }
            }
            else if (menu->wrapAround)
                moveTo(firstItem, endArrowFlash(menu));
            else
                refuse();
        };

        if (held(menu->horizontal ? kButtonLeft : kButtonUp))
            moveBack();
        else if (held(menu->horizontal ? kButtonRight : kButtonDown))
            moveForward();

        // SelectedText: the selected line, and whether it may be selected.
        const Text line = setSelectedText(menu, Text(menu->items), selected, lineText);
        const bool selectable = line && *line != kItemMark;

        if (held(g_zgui->acceptButton))
        {
            if (menu->onItemSelect && selectable)
            {
                if (const SoundHook sound = g_zgui->selectSound)
                    sound();
                runItemScript(menu, menu->onItemSelect);
                return 1u;
            }
            if (const SoundHook sound = g_zgui->blockedSound)
                sound();
            return 1u;
        }

        if (!held(g_zgui->cancelButton))
            return 1u;
        if (menu->onCancel)
        {
            if (const SoundHook sound = g_zgui->cancelSound)
                sound();
            runItemScript(menu, menu->onCancel);
            return 1u;
        }
        if (const SoundHook sound = g_zgui->blockedSound)
            sound();
        return 1u;
    }

    // pressListedMenu: ZGE2's handleGuiMenuInput, the menu's Input: up and
    // down move the selection among Items' lines (at an end, round to the
    // other with WrapAround; else the blocked sound), accept runs
    // OnItemSelect unless the selected line is disabled ("!") and cancel
    // runs OnCancel, each with its sound, the blocked sound without the
    // script. ZGE2 steps over no disabled or comment lines. 1.
    int32_t pressListedMenu(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        const ee::Ptr<Input> input(message->data);
        const uint32_t down = input->down;
        const uint32_t pressed = input->pressed;
        const ee::Ptr<const char> items(menu->items);
        if (!items)
            return 1;
        const auto play = [](SoundHook sound) {
            if (sound)
                sound();
        };
        const auto runScriptOf = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };

        // The lines counted, and whether the selected one can be chosen.
        int32_t lineCount = 1;
        bool selectable = true;
        ee::Ptr<const char> line = items;
        const int32_t selected = menu->selectedIndex;
        for (uint32_t at = 0; items[at] != 0; ++at)
        {
            if (items[at] != kCarriageReturn)
                continue;
            if (lineCount == selected + 1)
                selectable = line[0] != kItemMark;
            ++lineCount;
            line = items + (at + 1);
            if (line[0] == kLineFeed)
                ++line;
        }
        if (lineCount == selected + 1)
            selectable = line[0] != kItemMark;

        constexpr uint32_t kUp = 1, kDown = 2;
        if ((pressed & kUp) && (down & kUp))
        {
            if (selected > 0)
            {
                menu->selectedIndex = selected - 1;
                play(g_zgui->moveSound);
            }
            else if (menu->wrapAround)
            {
                menu->selectedIndex = lineCount - 1;
                play(g_zgui->moveSound);
            }
            else
                play(g_zgui->blockedSound);
        }
        if ((pressed & kDown) && (down & kDown))
        {
            const int32_t now = menu->selectedIndex;
            if (now < lineCount - 1)
            {
                menu->selectedIndex = now + 1;
                play(g_zgui->moveSound);
            }
            else if (menu->wrapAround)
            {
                menu->selectedIndex = 0;
                play(g_zgui->moveSound);
            }
            else
                play(g_zgui->blockedSound);
        }
        const uint32_t accept = g_zgui->acceptButton;
        if ((pressed & accept) && (down & accept))
        {
            if (menu->onItemSelect && selectable)
            {
                play(g_zgui->selectSound);
                runScriptOf(menu->onItemSelect);
            }
            else
                play(g_zgui->blockedSound);
            return 1;
        }
        const uint32_t cancel = g_zgui->cancelButton;
        if ((pressed & cancel) && (down & cancel))
        {
            if (menu->onCancel)
            {
                play(g_zgui->cancelSound);
                runScriptOf(menu->onCancel);
            }
            else
                play(g_zgui->blockedSound);
        }
        return 1;
    }

    // defineGuiMenuProperties: a menu's handlers, properties and defaults.
    ee::Ptr<GuiMenu> defineGuiMenuProperties(ee::Ptr<GuiMenu> menu, uint32_t text)
    {
        defineWidgetProperties(menu, text);
        addListener(menu, Message::Input, sym::handleGuiMenuInput);
        addListener(menu, Message::Draw, sym::drawGuiMenu);
        addListener(menu, Message::Refresh, sym::refreshGuiMenu);
        addListener(menu, Message::GetFocus, sym::guiMenuGetFocus);
        addListener(menu, Message::LostFocus, sym::guiMenuLostFocus);
        definePropertySteps(menu, kMenuProperties);
        setUnitScale(menu->scale);
        setWhite(menu->color);
        setWhite(menu->selectedColor);
        menu->selectedColor.rgba[2] = 0;
        menu->disabledColor.rgba[0] = menu->disabledColor.rgba[1] = menu->disabledColor.rgba[2] = 0x80;
        menu->disabledColor.rgba[3] = 255;
        menu->disabledColor.paletteName = 0;
        menu->selectedText = 0;
        menu->selectedCoord = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        menu->horizontalSpacing = ee::Float(10.0f);
        menu->spacing = ee::Float(1.25f);
        menu->lastSelectedIndex = -1;
        menu->hideSelectionOnLostFocus = 1;
        menu->blockedPlayed = 0;
        menu->horizontal = 0;
        menu->scrollOffset = 0;
        menu->asset = {};
        menu->items = 0;
        menu->charset = 0;
        menu->visibleCount = game::readable::perGame(0, 0, 5); // (ZGE4 shows five rows at first)
        menu->selectedIndex = -1;
        menu->onChange = 0;
        menu->onCancel = 0;
        menu->onLostFocus = 0;
        menu->onGetFocus = 0;
        menu->onItemFocus = 0;
        menu->onItemUnFocus = 0;
        menu->onItemSelect = 0;
        if constexpr (game::readable::kIsBmx)
        {
            startArrow(menu) = 0;
            endArrow(menu) = 0;
        }
        menu->alignment = 0;
        if constexpr (game::readable::kIsBmx) // (ZGE4 clears the measured size here too)
        {
            menu->width = ee::Float(0.0f);
            menu->height = ee::Float(0.0f);
        }
        return menu;
    }

    // defineListedMenu: ZGE2's defineGuiMenuProperties: its six handlers
    // (with GetBounds), its properties, white with a yellow selection and grey
    // disabled items, spacing 1.25, nothing selected (at (0, 0, 0, 1)).
    uint32_t defineListedMenu(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedMenu> menu(address);
        defineWidgetRecord(address, name);
        subscribeWidget(object, Message::Input, sym::handleGuiMenuInput);
        subscribeWidget(object, Message::Draw, sym::drawGuiMenu);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiMenu);
        subscribeWidget(object, Message::GetBounds, sym::dm2::menuBounds);
        subscribeWidget(object, Message::GetFocus, sym::guiMenuGetFocus);
        subscribeWidget(object, Message::LostFocus, sym::guiMenuLostFocus);
        addListedProperties(menu, kListedMenuProperties);
        menu->color = {255, 255, 255, 255};
        menu->selectedColor = {255, 255, 0, 255};
        menu->disabledColor = {0x80, 0x80, 0x80, 255};
        menu->lastSelectedIndex = -1;
        menu->spacing = ee::Float(1.25f);
        menu->selectedCoord = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        menu->hideSelectionOnLostFocus = 1;
        menu->scrollOffset = 0;
        menu->font = {};
        menu->items = 0;
        menu->charset = 0;
        menu->visibleCount = 0;
        menu->selectedIndex = -1;
        menu->onChange = 0;
        menu->onCancel = 0;
        menu->onLostFocus = 0;
        menu->onGetFocus = 0;
        menu->onItemFocus = 0;
        menu->onItemUnFocus = 0;
        menu->onItemSelect = 0;
        menu->alignment = 0;
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiMenuProperties, Verified, dm2, defineListedMenu);
}

namespace zge::zgui
{
    // menuBounds (ZGE2 only): the menu's GetBounds: Items measured as one
    // text in the menu's style into the message's rectangle, then its top
    // and bottom set to the block of visibleCount lines as the alignment's
    // vertical part places it (hanging from the position, centred on it or
    // standing on it). 1.
    inline int32_t menuBounds(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedMenu> menu = messageRecord<ListedMenu>(message);
        if (!menu->charset || !menu->font)
        {
            if (menu->charset)
                menu->font = findGuiAsset(menu->charset, kAssetFont);
            if (!menu->font)
                return 1;
        }
        zge::TextBounds &bounds = messageBounds(message);
        style->position = menu->widget.position;
        zge::quaternionCopy(zge::styleOrientation(*style), &menu->widget.orientation);
        std::memcpy(&style->scaleX, &menu->widget.scale, sizeof(zge::Vec4)); // the scale's four words
        style->align = menu->alignment & 0xF;
        std::memcpy(style->colour, &menu->color, sizeof(Rgba));
        style->letterSpacing = 1;
        style->lineSpacing = menu->spacing;
        std::memset(&style->reserved, 0, 1); // ZGE2 clears its one byte
        zge::measureText(ee::Ptr<zge::Font>(menu->font->object), style, ee::Ptr<const int8_t>(menu->items),
                         ee::Ptr<zge::TextBounds>(message->data), ee::Ptr<int32_t>());
        const ee::Float blockHeight =
            ee::toFloat(menu->visibleCount) * menu->spacing * (menu->widget.scale.y * ee::toFloat(fontHeight(menu->font)));
        const ee::Float y = menu->widget.position.y;
        const uint32_t vertical = menu->alignment & 0xF0;
        if (vertical == 0x00)
        {
            bounds.top = y;
            bounds.bottom = y + blockHeight;
        }
        else if (vertical == 0x10)
        {
            const ee::Float half = blockHeight * ee::Float(0.5f);
            bounds.top = y - half;
            bounds.bottom = y + half;
        }
        else if (vertical == 0x20)
        {
            bounds.top = y - blockHeight;
            bounds.bottom = y;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, menuBounds);
}

