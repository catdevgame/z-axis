// zgui's keyboard, where a player types a name: the characters of Items in
// rows of RowWidth, then the special keys that are on (SPC, DEL, BACK, END).
// The d-pad moves over them, skipping spaces and wrapping at the edges; accept
// types the character into String.

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/zgui/keyboard.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kMemset = sym::memset;      // libc
        constexpr uint32_t kSetVariable = sym::setVariable; // called by address: a game override hooks it

        // The special keys, past Items in the key list.
        constexpr uint8_t kSpaceKey = 252, kDeleteKey = 253, kCancelKey = 254, kEndKey = 255;

        constexpr uint32_t kCentred = 0x11;             // TextStyle::align: middle, middle
        constexpr uint32_t kSelectedCharName = sym::selectedCharName, kStringName = sym::g_textString, kItemsName = sym::g_textItems2;
        constexpr uint32_t kSpcLabel = sym::spcLabel, kDelLabel = sym::delLabel, kBackLabel = sym::backLabel, kEndLabel = sym::endLabel;

        constexpr ClassProperty kBeforeItems[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiKeyboard, scale)}, // "Scale" Z3DVector
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiKeyboard, charset)}, // "Charset" ZAssetID
            {sym::g_textItems2, sym::g_textString, offsetof(GuiKeyboard, items)}, // "Items" String
        };
        constexpr ClassProperty kAfterItems[] = {
            {sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiKeyboard, color)}, // "Color" ZRGBA_PAL
            {sym::g_textSelectedColor, sym::g_textZrgbaPal, offsetof(GuiKeyboard, selectedColor)}, // "SelectedColor" ZRGBA_PAL
            {sym::g_textHSpacing, sym::g_textZFloat, offsetof(GuiKeyboard, hSpacing)}, // "HSpacing" ZFloat
            {sym::g_textVSpacing, sym::g_textZFloat, offsetof(GuiKeyboard, vSpacing)}, // "VSpacing" ZFloat
            {sym::g_textRowWidth, sym::g_textSint32, offsetof(GuiKeyboard, rowWidth)}, // "RowWidth" Sint32
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(GuiKeyboard, selectedIndex)}, // "SelectedIndex" Sint32
            {sym::selectedCharName, sym::g_textString, offsetof(GuiKeyboard, selectedChar)}, // "SelectedChar" String
            {sym::g_textString, sym::g_textString, offsetof(GuiKeyboard, string)}, // "String" String
            {sym::g_textMaxItems, sym::g_textSint32, offsetof(GuiKeyboard, maxItems)}, // "MaxItems" Sint32
            {sym::g_textEnableBackspaceKey, sym::g_textBool, offsetof(GuiKeyboard, enableBackspaceKey)}, // "EnableBackspaceKey" Bool
            {sym::g_textShowEnd, sym::g_textBool, offsetof(GuiKeyboard, showEnd)}, // "ShowEnd" Bool
            {sym::g_textShowDel, sym::g_textBool, offsetof(GuiKeyboard, showDel)}, // "ShowDel" Bool
            {sym::g_textShowSpace, sym::g_textBool, offsetof(GuiKeyboard, showSpace)}, // "ShowSpace" Bool
            {sym::g_textShowCancel, sym::g_textBool, offsetof(GuiKeyboard, showCancel)}, // "ShowCancel" Bool
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiKeyboard, onGetFocus)}, // "OnGetFocus" Event
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiKeyboard, onLostFocus)}, // "OnLostFocus" Event
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(GuiKeyboard, onSelect)}, // "OnSelect" Event
            {sym::g_textOnOk, sym::g_textEvent, offsetof(GuiKeyboard, onOk)}, // "OnOk" Event
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiKeyboard, onCancel)}, // "OnCancel" Event
            {sym::g_textOnChange, sym::g_textEvent, offsetof(GuiKeyboard, onChange)}, // "OnChange" Event
            {sym::g_textOnItemFocus, sym::g_textEvent, offsetof(GuiKeyboard, onItemFocus)}, // "OnItemFocus" Event
            {sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(GuiKeyboard, onItemUnFocus)}, // "OnItemUnFocus" Event
        };

        // The default key list: "ABC..." in the ELF, copied to the frame
        // before it is set.
        struct KeyList
        {
            char keys[0x50];
        };
        inline ee::Global<KeyList, sym::defaultItems> g_defaultItems;

        // The special keys that are on added to the list of Items on the
        // frame; the list's length. (Blocks: the two callers' copies of this.)
        struct KeyBlocks
        {
            uint32_t afterSpace, afterDel, afterCancel, afterEnd;
        };
        int32_t appendSpecialKeys(const GuiKeyboard &keyboard, uint32_t keys, int32_t length, const KeyBlocks &blocks)
        {
            const auto append = [&](uint8_t key) {
                *ee::at<uint8_t>(keys + length) = key;
                ++length;
                *ee::at<uint8_t>(keys + length) = 0;
            };
            if (keyboard.showSpace)
                append(kSpaceKey);
            if (keyboard.showDel)
                append(kDeleteKey);
            if (keyboard.showCancel)
                append(kCancelKey);
            if (keyboard.showEnd)
                append(kEndKey);
            return length;
        }
    }


    namespace
    {
        // ZGE2's (DM2's) keyboard record, after WidgetRecord: its special
        // keys are texts of their own (On...Char) where ZGE3 has Show...
        // switches, and it has no Scale, Width or Height of its own.
        struct ListedKeyboard
        {
            WidgetRecord widget;
            ee::Ptr<UiAsset> font;   // +0x60 found by charset
            int32_t selectedIndex;   // +0x64
            int32_t rowWidth;        // +0x68 keys a row
            uint32_t _6c;
            zge::Vec4 selectedCoord; // +0x70 from the keyboard to the selected key
            Rgba color;              // +0x80
            Rgba selectedColor;      // +0x84
            ee::Float hSpacing;      // +0x88
            ee::Float vSpacing;      // +0x8C
            int32_t maxItems;        // +0x90 the string's most characters
            uint32_t selectedChar;   // +0x94 (String)
            uint32_t items;          // +0x98 the keys (String)
            uint32_t charset;        // +0x9C
            uint32_t string;         // +0xA0 what is typed (String)
            uint32_t onGetFocus;     // +0xA4 each a script (Event)
            uint32_t onLostFocus;    // +0xA8
            uint32_t onChange;       // +0xAC
            uint32_t onCancel;       // +0xB0
            uint32_t onSelect;       // +0xB4
            uint32_t onOk;           // +0xB8
            uint32_t onItemFocus;    // +0xBC
            uint32_t onItemUnFocus;  // +0xC0
            uint32_t onOkChar;       // +0xC4 each a String
            uint32_t onCancelChar;   // +0xC8
            uint32_t onDeleteChar;   // +0xCC
            uint32_t onSpaceChar;    // +0xD0
        };
        static_assert(offsetof(ListedKeyboard, selectedCoord) == 0x70 && offsetof(ListedKeyboard, color) == 0x80);
        static_assert(offsetof(ListedKeyboard, maxItems) == 0x90 && offsetof(ListedKeyboard, string) == 0xA0);
        static_assert(offsetof(ListedKeyboard, onItemUnFocus) == 0xC0 && offsetof(ListedKeyboard, onSpaceChar) == 0xD0);

        constexpr ListedProperty kListedKeyboardItems[] = {
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedKeyboard, charset)},
            {sym::g_textItems2, sym::g_textString, offsetof(ListedKeyboard, items)},
        };
        // After Items is set to the default keys.
        constexpr ListedProperty kListedKeyboardProperties[] = {
            {sym::g_textColor, sym::dm2::g_textZrgba, offsetof(ListedKeyboard, color)},
            {sym::g_textSelectedColor, sym::dm2::g_textZrgba, offsetof(ListedKeyboard, selectedColor)},
            {sym::g_textHSpacing, sym::g_textZFloat, offsetof(ListedKeyboard, hSpacing)},
            {sym::g_textVSpacing, sym::g_textZFloat, offsetof(ListedKeyboard, vSpacing)},
            {sym::g_textRowWidth, sym::g_textSint32, offsetof(ListedKeyboard, rowWidth)},
            {sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(ListedKeyboard, selectedIndex)},
            {sym::selectedCharName, sym::g_textString, offsetof(ListedKeyboard, selectedChar)},
            {sym::g_textString, sym::g_textString, offsetof(ListedKeyboard, string)},
            {sym::dm2::g_textOnOkChar, sym::g_textString, offsetof(ListedKeyboard, onOkChar)},
            {sym::dm2::g_textOnDeleteChar, sym::g_textString, offsetof(ListedKeyboard, onDeleteChar)},
            {sym::dm2::g_textOnCancelChar, sym::g_textString, offsetof(ListedKeyboard, onCancelChar)},
            {sym::dm2::g_textOnSpaceChar, sym::g_textString, offsetof(ListedKeyboard, onSpaceChar)},
            {sym::g_textMaxItems, sym::g_textSint32, offsetof(ListedKeyboard, maxItems)},
            {sym::g_textSelectedCoord, sym::g_textZ3DVector, offsetof(ListedKeyboard, selectedCoord)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedKeyboard, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedKeyboard, onLostFocus)},
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(ListedKeyboard, onSelect)},
            {sym::g_textOnOk, sym::g_textEvent, offsetof(ListedKeyboard, onOk)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedKeyboard, onCancel)},
            {sym::g_textOnChange, sym::g_textEvent, offsetof(ListedKeyboard, onChange)},
            {sym::g_textOnItemFocus, sym::g_textEvent, offsetof(ListedKeyboard, onItemFocus)},
            {sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(ListedKeyboard, onItemUnFocus)},
        };
    }

    // refreshGuiKeyboard: the keyboard's Refresh: its font, its two colours.
    uint32_t refreshGuiKeyboard(uint32_t address)
    {
        const ee::Ptr<GuiKeyboard> keyboard(address);
        keyboard->asset = findGuiAsset(keyboard->charset, kAssetFont);
        if (keyboard->color.paletteName)
            findPaletteColour(keyboard->color.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiKeyboard, color)));
        if (keyboard->selectedColor.paletteName)
            findPaletteColour(keyboard->selectedColor.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiKeyboard, selectedColor)));
        return 1;
    }

    // refreshKeyboardFont: ZGE2's refreshGuiKeyboard: its font found again
    // (ZGE2's colours have no palette names). 1.
    int32_t refreshKeyboardFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedKeyboard> keyboard = messageRecord<ListedKeyboard>(message);
        keyboard->font = findGuiAsset(keyboard->charset, kAssetFont);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiKeyboard, Verified, dm2, refreshKeyboardFont);

    namespace
    {
        void memset(ee::Ptr<uint8_t> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // drawGuiKeyboard: the keyboard's Draw: every key in its row and column, the
    // selected one in SelectedColor (OnItemFocus every frame; OnItemUnFocus
    // once at the first key drawn after it or at the end), the special keys
    // as small words. Its size from the rows drawn.
    uint32_t drawGuiKeyboard(uint32_t address)
    {
        namespace here = points::drawGuiKeyboard;
        ee::Local<uint8_t[0x80]> keys;
        ee::Local<zge::TextStyle> styleAddress;
        ee::Local<uint8_t[0x40]> buffer;
        ee::Local<uint8_t[0x10]> characterAddress;
        ee::Local<int32_t> column;
        ee::Local<int32_t> row;
        const ee::Ptr<GuiKeyboard> keyboard(address);
        constexpr uint32_t kKeys = 0x00, kStyle = 0x80, kCharacter = 0xD0, kColumn = 0xE0, kRow = 0xE4;

        int32_t index = 0;
        uint32_t focused = 0;
        const auto runScriptOf = [&](uint32_t script) {
            runScript(ee::Ptr<Node>(address), ee::Ptr<char>(script), 0);
        };

        if (!keyboard->charset || !keyboard->asset)
        {
            if (keyboard->charset)
                keyboard->asset = findGuiAsset(keyboard->charset, kAssetFont);
            if (!keyboard->asset)
                return 1;
        }
        if (keyboard->rowWidth > 0)
        {
            *row = 0;
            *column = 0;
            zge::copyQuadword(styleAddress.address(), address + offsetof(Node, screenPosition));
            const int32_t rowWidth = keyboard->rowWidth;
            const int32_t selected = keyboard->selectedIndex;
            const ee::Float hSpacing = keyboard->hSpacing;
            styleAddress->monospaced = 0;
            ee::Ptr<zge::TextStyle>(styleAddress.address())->angle = 0;
            styleAddress->position.x = styleAddress->position.x + hSpacing * ee::Float(0.5f);
            keyboard->selectedCoord.x = ee::toFloat(selected % rowWidth) * hSpacing;
            keyboard->selectedCoord.y = ee::toFloat(selected / rowWidth) * keyboard->vSpacing;
            keyboard->selectedCoord.z = keyboard->widget.node.screenPosition.z;
            zge::stringCopy(keys, keyboard->items);
            const int32_t length = zge::stringLength(keys);
            appendSpecialKeys(*keyboard, keys.address(), length, KeyBlocks{here::taken4, here::taken5, here::taken6, here::taken7});

            if (keys[0])
            {
                for (;;)
                {
                    memset(characterAddress, 0, 2);
                    characterAddress[0] = keys[index];
                    if (*column >= keyboard->rowWidth)
                    {
                        *column = 0;
                        ++*row;
                        const ee::Float nextY = styleAddress->position.y + keyboard->vSpacing;
                        styleAddress->position.x = keyboard->widget.node.screenPosition.x + keyboard->hSpacing * ee::Float(0.5f);
                        styleAddress->position.y = nextY;
                    }
                    if (index == keyboard->selectedIndex)
                    {
                        if (const uint32_t onItemFocus = keyboard->onItemFocus)
                        {
                            focused = 1;
                            runScriptOf(onItemFocus);
                        }
                        std::memcpy(styleAddress->colour, keyboard->selectedColor.rgba, 4);
                    }
                    else
                    {
                        const uint32_t onItemUnFocus = keyboard->onItemUnFocus;
                        if (onItemUnFocus && focused)
                        {
                            focused = 0;
                            runScriptOf(onItemUnFocus);
                        }
                        std::memcpy(styleAddress->colour, keyboard->color.rgba, 4);
                    }
                    zge::copyQuadword(styleAddress.address() + offsetof(zge::TextStyle, scaleX), address + offsetof(GuiKeyboard, scale));
                    styleAddress->letterSpacing = 1;
                    styleAddress->align = kCentred;
                    styleAddress->lineSpacing = ee::Float(1.0f);
                    zge::clearStyleReserved(*styleAddress);
                    for (int channel = 0; channel < 3; ++channel)
                        styleAddress->colour[channel] = faded(styleAddress->colour[channel]);

                    // A special key is a word, drawn smaller and tighter.
                    const auto drawWord = [&](uint32_t word, ee::Float size) {
                        styleAddress->letterSpacing = -3;
                        styleAddress->scaleX = styleAddress->scaleX * size;
                        drawText(ee::Ptr<zge::Font>(keyboard->asset->object), styleAddress, ee::Ptr<const int8_t>(word));
                    };
                    const auto drawCharacter = [&]() {
                        drawText(ee::Ptr<zge::Font>(keyboard->asset->object), styleAddress, ee::Ptr<const int8_t>(ee::Address(characterAddress)));
                    };
                    const uint8_t key = keys[index];
                    if (key == kDeleteKey)
                        drawWord(kDelLabel, ee::Float(0.7f));
                    else if (key < kCancelKey)
                    {
                        if (key == kSpaceKey)
                            drawWord(kSpcLabel, ee::Float(0.7f));
                        else
                            drawCharacter();
                    }
                    else
                    {
                        if (key == kCancelKey)
                            drawWord(kBackLabel, ee::Float(0.6f));
                        else if (key == kEndKey)
                            drawWord(kEndLabel, ee::Float(0.7f));
                        else
                            drawCharacter();
                    }
                    ++index;
                    styleAddress->position.x = styleAddress->position.x + keyboard->hSpacing;
                    ++*column;
                    if (!keys[index])
                        break;
                }
            }
            const ee::Float height = (keyboard->vSpacing + ee::Float(1.0f)) * ee::toFloat(*row);
            keyboard->width = keyboard->hSpacing * ee::toFloat(keyboard->rowWidth);
            keyboard->height = height;
            if (keyboard->onItemUnFocus && focused)
                runScriptOf(keyboard->onItemUnFocus);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiKeyboard, Verified, dm2, drawListedKeyboard);

    // drawListedKeyboard: ZGE2's drawGuiKeyboard, the keyboard's Draw: each
    // character of Items drawn as a key, rowWidth to a row, centred on its
    // place, with the widget's orientation and scale, the selected one in
    // its colour (OnItemFocus run at it, OnItemUnFocus at the next that is
    // not, or after the last). ZGE2's keys are all characters (its special
    // keys are among Items) and it keeps no selected coordinate or size
    // here. 1.
    int32_t drawListedKeyboard(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ListedStyleRoom> styleRoom;
        ee::Local<char[0x10]> character;
        const ee::Ptr<zge::TextStyle> style(styleRoom.address());
        const ee::Ptr<ListedKeyboard> keyboard = messageRecord<ListedKeyboard>(message);
        bool focused = false;
        uint32_t index = 0;
        const auto runScriptOf = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };
        if (!keyboard->charset || !keyboard->font)
        {
            if (keyboard->charset)
                keyboard->font = findGuiAsset(keyboard->charset, kAssetFont);
            if (!keyboard->font)
                return 1;
        }
        int32_t column = 0;
        style->position = keyboard->widget.position;
        zge::quaternionCopy(zge::styleOrientation(*style), &keyboard->widget.orientation);
        style->position.x = style->position.x + keyboard->hSpacing * ee::Float(0.5f);
        if (*ee::Ptr<const char>(keyboard->items) != 0)
        {
            for (;;)
            {
                fillBytes(character, 0, 2);
                character[0] = ee::Ptr<const char>(keyboard->items)[index];
                if (!(column < keyboard->rowWidth))
                {
                    column = 0;
                    const ee::Float nextY = style->position.y + keyboard->vSpacing;
                    style->position.x = keyboard->widget.position.x + keyboard->hSpacing * ee::Float(0.5f);
                    style->position.y = nextY;
                }
                if (static_cast<int32_t>(index) == keyboard->selectedIndex)
                {
                    if (const uint32_t onItemFocus = keyboard->onItemFocus)
                    {
                        runScriptOf(onItemFocus);
                        focused = true;
                    }
                    std::memcpy(style->colour, &keyboard->selectedColor, sizeof(Rgba));
                }
                else
                {
                    const uint32_t onItemUnFocus = keyboard->onItemUnFocus;
                    if (onItemUnFocus && focused)
                    {
                        runScriptOf(onItemUnFocus);
                        focused = false;
                    }
                    std::memcpy(style->colour, &keyboard->color, sizeof(Rgba));
                }
                std::memcpy(&style->scaleX, &keyboard->widget.scale, sizeof(zge::Vec4)); // the scale's four words
                style->letterSpacing = 1;
                style->align = kCentred;
                style->lineSpacing = ee::Float(1.0f);
                *reinterpret_cast<uint8_t *>(&style->reserved) = 0; // ZGE2 clears its one byte
                for (int channel = 0; channel < 3; ++channel)
                    style->colour[channel] = faded(style->colour[channel]);
                ++index;
                ++column;
                drawText(ee::Ptr<zge::Font>(keyboard->font->object), style, ee::Ptr<const int8_t>(ee::Address(character)));
                style->position.x = style->position.x + keyboard->hSpacing;
                if (ee::Ptr<const char>(keyboard->items)[index] == 0)
                    break;
            }
        }
        if (keyboard->onItemUnFocus && focused)
            runScriptOf(keyboard->onItemUnFocus);
        return 1;
    }

    // handleGuiKeyboardInput: the keyboard's Input. The d-pad moves over the key list,
    // on past spaces, wrapping within the rows; the option button deletes the
    // last character if EnableBackspaceKey says so; accept presses the key;
    // cancel runs OnCancel. String is a pointer the script gave: it has to be
    // long enough for maxItems (a character is typed through setVariable).
    uint32_t handleGuiKeyboardInput(uint32_t address, uint32_t inputAddress)
    {
        namespace here = points::handleGuiKeyboardInput;
        ee::Local<uint8_t[0x80]> keys;
        ee::Local<uint8_t[0x100]> typedAddress;
        ee::Local<uint8_t[0xA0]> characterAddress;
        const ee::Ptr<GuiKeyboard> keyboard(address);
        const ee::Ptr<Input> input(inputAddress);
        constexpr uint32_t kKeys = 0x00, kTyped = 0x80, kCharacter = 0x180;
        const uint32_t pressedMask = input->pressed, down = input->down;

        zge::stringCopy(keys, keyboard->items);
        const int32_t itemCount = zge::stringLength(keys);
        const int32_t count = appendSpecialKeys(*keyboard, keys.address(), itemCount, KeyBlocks{here::taken, here::taken2, here::taken3, here::taken4});

        const int32_t rowWidth = keyboard->rowWidth;
        const int32_t rows = (count + rowWidth - 1) / rowWidth;

        const uint32_t leftPressed = pressedMask & kButtonLeft, rightPressed = pressedMask & kButtonRight;
        const uint32_t downPressed = pressedMask & kButtonDown;
        const auto runScriptOf = [&](uint32_t script) {
            runScript(ee::Ptr<Node>(address), ee::Ptr<char>(script), 0);
        };
        const auto isSpace = [&](int32_t key) { return keys[key] == ' '; };

        // The selection's new place, and OnChange. The loops have read the
        // spacings before them and rowWidth from their own pass.
        const auto placeSelection = [&](int32_t selected, int32_t width, ee::Float hSpacing, ee::Float vSpacing, ee::Float z) {
            keyboard->selectedCoord.x = ee::toFloat(selected % width) * hSpacing;
            keyboard->selectedCoord.y = ee::toFloat(selected / width) * vSpacing;
            keyboard->selectedCoord.z = z;
        };

        const zge::Vec4 &position = keyboard->widget.node.screenPosition;
        if ((pressedMask & kButtonUp) && (down & kButtonUp))
        {
            int32_t selected = keyboard->selectedIndex;
            const ee::Float hSpacing = keyboard->hSpacing, vSpacing = keyboard->vSpacing, z = position.z;
            const uint32_t onChange = keyboard->onChange;
            for (;;)
            {
                if (selected - rowWidth >= 0)
                    keyboard->selectedIndex = selected - rowWidth;
                else
                {
                    // Off the top: the same column of the last row, or the
                    // last that far along.
                    const int32_t column = selected % rowWidth;
                    int32_t wrapped = static_cast<int32_t>(static_cast<uint32_t>(rows - 1) * static_cast<uint32_t>(rowWidth)) + column;
                    if (wrapped >= count)
                    {
                        for (;;)
                        {
                            wrapped -= rowWidth;
                            if (wrapped < count)
                                break;
                        }
                    }
                    keyboard->selectedIndex = wrapped;
                }
                selected = keyboard->selectedIndex;
                if (!isSpace(selected))
                    break;
            }
            placeSelection(selected, rowWidth, hSpacing, vSpacing, z);
            if (onChange)
                runScriptOf(onChange);
            if (const SoundHook hook = g_zgui->moveSound)
                hook();
        }

        if (downPressed && (down & kButtonDown))
        {
            const int32_t width = keyboard->rowWidth;
            int32_t selected = keyboard->selectedIndex;
            const ee::Float hSpacing = keyboard->hSpacing, vSpacing = keyboard->vSpacing, z = position.z;
            const uint32_t onChange = keyboard->onChange;
            for (;;)
            {
                if (selected + width < count)
                    keyboard->selectedIndex = selected + width;
                else
                {
                    const int32_t column = selected % width;
                    keyboard->selectedIndex = column;
                }
                selected = keyboard->selectedIndex;
                if (!isSpace(selected))
                    break;
            }
            placeSelection(selected, width, hSpacing, vSpacing, z);
            if (onChange)
                runScriptOf(onChange);
            if (const SoundHook hook = g_zgui->moveSound)
                hook();
        }

        if (leftPressed && (down & kButtonLeft))
        {
            const int32_t width = keyboard->rowWidth;
            const ee::Float hSpacing = keyboard->hSpacing, vSpacing = keyboard->vSpacing, z = position.z;
            const uint32_t onChange = keyboard->onChange;
            int32_t next = keyboard->selectedIndex - 1;
            int32_t selected;
            for (;;)
            {
                const int32_t column = next % width;
                keyboard->selectedIndex = next;
                if (column == width - 1)
                {
                    // Off the row's left: its right end, or the list's end.
                    int32_t wrapped = next + width;
                    keyboard->selectedIndex = wrapped;
                    if (wrapped >= count)
                    {
                        for (;;)
                        {
                            --wrapped;
                            if (wrapped < count)
                                break;
                        }
                        keyboard->selectedIndex = wrapped;
                    }
                }
                selected = keyboard->selectedIndex;
                if (selected < 0)
                {
                    selected = width - 1;
                    keyboard->selectedIndex = selected;
                }
                next = selected - 1;
                if (!isSpace(selected))
                    break;
            }
            placeSelection(selected, width, hSpacing, vSpacing, z);
            if (onChange)
                runScriptOf(onChange);
            if (const SoundHook hook = g_zgui->moveSound)
                hook();
        }

        if (rightPressed && (down & kButtonRight))
        {
            const int32_t width = keyboard->rowWidth;
            const ee::Float hSpacing = keyboard->hSpacing, vSpacing = keyboard->vSpacing, z = position.z;
            const uint32_t onChange = keyboard->onChange;
            int32_t next = keyboard->selectedIndex + 1;
            int32_t selected;
            for (;;)
            {
                const int32_t column = next % width;
                keyboard->selectedIndex = next;
                if (column == 0)
                    keyboard->selectedIndex = next - width;
                selected = keyboard->selectedIndex;
                if (selected >= count)
                {
                    const int32_t pastRowStart = selected % width;
                    selected -= pastRowStart;
                    keyboard->selectedIndex = selected;
                }
                next = selected + 1;
                if (!isSpace(selected))
                    break;
            }
            placeSelection(selected, width, hSpacing, vSpacing, z);
            if (onChange)
                runScriptOf(onChange);
            const SoundHook hook = g_zgui->moveSound;
            if (hook)
                hook();
        }

        const uint32_t option = g_zgui->optionButton;
        if (pressedMask & option)
        {
            if (down & option)
            {
                if (keyboard->enableBackspaceKey)
                {
                    const uint32_t string = keyboard->string;
                    if (string)
                    {
                        const uint32_t length = static_cast<uint32_t>(zge::stringLength(string));
                        SoundHook hook;
                        if (length)
                        {
                            ee::Ptr<uint8_t>(keyboard->string)[length - 1] = 0;
                            runScriptOf(keyboard->onSelect);
                            hook = g_zgui->cancelSound;
                        }
                        else
                            hook = g_zgui->blockedSound;
                        if (hook)
                            hook();
                    }
                    else
                    {
                        const SoundHook hook = g_zgui->blockedSound;
                        if (hook)
                            hook();
                    }
                }
            }
        }

        // The script that ends each way out, and its sound.
        const auto finishWith = [&](SoundHook hook) {
            if (hook)
                hook();
            return 1u;
        };

        const uint32_t accept = g_zgui->acceptButton;
        if (pressedMask & accept)
        {
            if (down & accept)
            {
                memset(characterAddress, 0, 2);
                characterAddress[0] = keys[keyboard->selectedIndex];
                setVariable(address, kSelectedCharName, characterAddress);
                const uint8_t key = characterAddress[0];

                // The character added to String, with the sound for it, then
                // OnSelect; the blocked sound if String is full.
                const auto type = [&](bool fits) {
                    SoundHook hook;
                    if (fits)
                    {
                        zge::stringCopy(typedAddress, keyboard->string);
                        zge::appendString(typedAddress, characterAddress);
                        setVariable(address, kStringName, typedAddress);
                        hook = g_zgui->selectSound;
                        if (hook)
                            hook();
                    }
                    else
                    {
                        hook = g_zgui->blockedSound;
                        if (hook)
                            hook();
                    }
                    runScriptOf(keyboard->onSelect);
                    return 1u;
                };
                // No String yet: the character is its first.
                const auto typeFirst = [&]() {
                    setVariable(address, kStringName, characterAddress);
                    runScriptOf(keyboard->onSelect);
                    return finishWith(g_zgui->selectSound);
                };

                if (key == kEndKey)
                {
                    runScriptOf(keyboard->onOk);
                    return finishWith(g_zgui->selectSound);
                }
                if (key == kCancelKey)
                {
                    if (const uint32_t onCancel = keyboard->onCancel)
                        runScriptOf(onCancel);
                    return finishWith(g_zgui->cancelSound);
                }
                if (key == kSpaceKey)
                {
                    const uint32_t string = keyboard->string;
                    characterAddress[0] = ' ';
                    if (!string)
                        return typeFirst();
                    const uint32_t length = static_cast<uint32_t>(zge::stringLength(string));
                    return type(length < keyboard->maxItems);
                }
                if (key == kDeleteKey)
                {
                    const uint32_t string = keyboard->string;
                    if (!string)
                        return finishWith(g_zgui->blockedSound);
                    const uint32_t length = static_cast<uint32_t>(zge::stringLength(string));
                    if (length)
                        ee::Ptr<uint8_t>(keyboard->string)[length - 1] = 0;
                    runScriptOf(keyboard->onSelect);
                    return finishWith(g_zgui->cancelSound);
                }
                const uint32_t onSelect = keyboard->onSelect;
                if (!onSelect)
                    return 1;
                const uint32_t string = keyboard->string;
                if (!string)
                    return typeFirst();
                const uint32_t length = static_cast<uint32_t>(zge::stringLength(string));
                return type(length < keyboard->maxItems);
            }
        }
        const uint32_t cancel = g_zgui->cancelButton;
        if (pressedMask & cancel)
        {
            if (!(down & cancel))
                return 1;
            if (const uint32_t onCancel = keyboard->onCancel)
            {
                runScriptOf(onCancel);
                return finishWith(g_zgui->cancelSound);
            }
            const SoundHook hook = g_zgui->blockedSound;
            if (!hook)
                return 1;
            hook();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiKeyboardInput, Verified, dm2, pressListedKeyboard);

    // pressListedKeyboard: ZGE2's handleGuiKeyboardInput, the keyboard's
    // Input. The d-pad moves over Items, on past spaces: up and down by a
    // row (off the top into the last row's column, off the bottom into the
    // first row's), left and right within the row, round to its other end;
    // each move places SelectedCoord, runs OnChange and plays the move
    // sound. The option button deletes String's last character (OnSelect,
    // the cancel sound; the blocked sound if there is none). Accept puts the
    // key into SelectedChar, then: the OnOkChar key runs OnOk, the
    // OnCancelChar key OnCancel, the OnDeleteChar key deletes as the option
    // button does, the OnSpaceChar key types a space, any other key types
    // itself if there is an OnSelect. Typing adds the key to String (made
    // the key alone when String has no text yet) unless String has MaxItems
    // characters (the blocked sound then), and runs OnSelect. Cancel runs
    // OnCancel (the blocked sound without one). 1.
    // Kept from the original: rowWidth is not checked (0 divides by zero),
    // and up into a column the last row has not got stops past the keys.
    int32_t pressListedKeyboard(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x100]> typed;
        ee::Local<char[0x10]> character;
        const ee::Ptr<ListedKeyboard> keyboard = messageRecord<ListedKeyboard>(message);
        const ee::Ptr<Input> input(message->data);
        const uint32_t pressed = input->pressed, down = input->down;
        const auto held = [&](uint32_t button) { return (pressed & button) && (down & button); };
        const auto play = [](SoundHook sound) {
            if (sound)
                sound();
        };
        const auto runScriptOf = [&](uint32_t script) {
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
        };
        const ee::Ptr<zge::List> properties(keyboard.address + offsetof(ListedKeyboard, widget.properties));

        const int32_t count = zge::stringLength(ee::Ptr<const char>(keyboard->items));
        const int32_t firstRowWidth = keyboard->rowWidth;
        const int32_t rows = (count + firstRowWidth - 1) / firstRowWidth;

        // The selection moved by `step` (the selected index in, the next
        // out) until it is not on a space; then placed and announced.
        const auto move = [&](int32_t rowWidth, auto step) {
            const ee::Ptr<const char> keys(keyboard->items);
            const ee::Float hSpacing = keyboard->hSpacing, vSpacing = keyboard->vSpacing;
            const ee::Float z = keyboard->widget.position.z;
            const uint32_t onChange = keyboard->onChange;
            int32_t selected = keyboard->selectedIndex;
            do
            {
                step(selected);
                selected = keyboard->selectedIndex;
            } while (keys[selected] == ' ');
            keyboard->selectedCoord.x = ee::toFloat(selected % rowWidth) * hSpacing;
            keyboard->selectedCoord.y = ee::toFloat(selected / rowWidth) * vSpacing;
            keyboard->selectedCoord.z = z;
            if (onChange)
                runScriptOf(onChange);
            play(g_zgui->moveSound);
        };
        if (held(kButtonUp))
        {
            const int32_t rowWidth = firstRowWidth;
            move(rowWidth, [&](int32_t selected) {
                if (selected - rowWidth >= 0)
                    keyboard->selectedIndex = selected - rowWidth;
                else
                    keyboard->selectedIndex = (rows - 1) * rowWidth + selected % rowWidth;
            });
        }
        if (held(kButtonDown))
        {
            const int32_t rowWidth = keyboard->rowWidth;
            move(rowWidth, [&](int32_t selected) {
                if (selected + rowWidth < count)
                    keyboard->selectedIndex = selected + rowWidth;
                else
                    keyboard->selectedIndex = selected % rowWidth;
            });
        }
        if (held(kButtonLeft))
        {
            const int32_t rowWidth = keyboard->rowWidth;
            move(rowWidth, [&](int32_t selected) {
                const int32_t left = selected - 1;
                keyboard->selectedIndex = left;
                if (left % rowWidth == rowWidth - 1)
                    keyboard->selectedIndex = left + rowWidth;
                if (keyboard->selectedIndex < 0)
                    keyboard->selectedIndex = rowWidth - 1;
            });
        }
        if (held(kButtonRight))
        {
            const int32_t rowWidth = keyboard->rowWidth;
            move(rowWidth, [&](int32_t selected) {
                const int32_t right = selected + 1;
                keyboard->selectedIndex = right;
                if (right % rowWidth == 0)
                    keyboard->selectedIndex = right - rowWidth;
            });
        }

        // String (a script may have set it again: read each time).
        const auto string = [&]() { return ee::Ptr<char>(keyboard->string); };
        // String's last character taken off (there is one): 1.
        const auto deleteLast = [&]() {
            const uint32_t length = zge::stringLength(string());
            if (length)
                string()[length - 1] = 0;
            return length != 0;
        };
        if (held(g_zgui->optionButton))
        {
            if (string() && deleteLast())
            {
                runScriptOf(keyboard->onSelect);
                play(g_zgui->cancelSound);
            }
            else
                play(g_zgui->blockedSound);
        }

        if (held(g_zgui->acceptButton))
        {
            fillBytes(character, 0, 2);
            character[0] = ee::Ptr<const char>(keyboard->items)[keyboard->selectedIndex];
            setListedVariable(properties, ee::Ptr<const char>(kSelectedCharName), character);
            const auto isKey = [&](uint32_t keyText) { return keyText && character[0] == *ee::Ptr<const char>(keyText); };
            const auto type = [&]() {
                if (!string())
                {
                    setListedVariable(properties, ee::Ptr<const char>(kStringName), character);
                    runScriptOf(keyboard->onSelect);
                    play(g_zgui->selectSound);
                    return;
                }
                if (zge::stringLength(string()) < static_cast<uint32_t>(keyboard->maxItems))
                {
                    zge::stringCopy(typed, string());
                    zge::appendString(typed, character);
                    setListedVariable(properties, ee::Ptr<const char>(kStringName), typed);
                    play(g_zgui->selectSound);
                }
                else
                    play(g_zgui->blockedSound);
                runScriptOf(keyboard->onSelect);
            };
            if (isKey(keyboard->onOkChar))
            {
                runScriptOf(keyboard->onOk);
                play(g_zgui->selectSound);
            }
            else if (isKey(keyboard->onCancelChar))
            {
                if (keyboard->onCancel)
                    runScriptOf(keyboard->onCancel);
                play(g_zgui->cancelSound);
            }
            else if (isKey(keyboard->onSpaceChar))
            {
                character[0] = ' ';
                type();
            }
            else if (isKey(keyboard->onDeleteChar))
            {
                if (string())
                {
                    deleteLast();
                    runScriptOf(keyboard->onSelect);
                    play(g_zgui->cancelSound);
                }
                else
                    play(g_zgui->blockedSound);
            }
            else if (keyboard->onSelect)
                type();
            return 1;
        }

        if (held(g_zgui->cancelButton))
        {
            if (keyboard->onCancel)
            {
                runScriptOf(keyboard->onCancel);
                play(g_zgui->cancelSound);
            }
            else
                play(g_zgui->blockedSound);
        }
        return 1;
    }


    // defineGuiKeyboardProperties: a keyboard's defaults (13 keys a row of A-Z, a-z, digits
    // and signs, white, yellow when selected, 16 characters, DEL and END on)
    // and properties. The default Items is the ELF's list, copied to the frame.
    uint32_t defineGuiKeyboardProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiKeyboard> keyboard(node);
        ee::Local<KeyList> items;
        *items = *g_defaultItems;

        const uint32_t scale = node + offsetof(GuiKeyboard, scale);
        defineWidgetProperties(node, text);
        keyboard->maxItems = 0x10;
        keyboard->rowWidth = 13;
        keyboard->vSpacing = ee::Float(24.0f);
        keyboard->hSpacing = ee::Float(24.0f);
        setWhite(keyboard->color);
        setWhite(keyboard->selectedColor);
        keyboard->selectedColor.rgba[2] = 0;
        keyboard->charset = 0;
        keyboard->asset = {};
        keyboard->onCancel = 0;
        keyboard->onGetFocus = 0;
        keyboard->onLostFocus = 0;
        keyboard->onSelect = 0;
        keyboard->onOk = 0;
        keyboard->selectedIndex = 0;
        keyboard->items = 0;
        keyboard->string = 0;
        keyboard->onItemFocus = 0;
        keyboard->onItemUnFocus = 0;
        keyboard->selectedCoord.x = 0.0f;
        keyboard->selectedCoord.y = 0.0f;
        keyboard->selectedCoord.z = 0.0f;
        keyboard->onChange = 0;
        keyboard->selectedChar = 0;
        keyboard->width = 0.0f;
        keyboard->height = 0.0f;
        keyboard->enableBackspaceKey = 1;
        keyboard->showSpace = 0;
        keyboard->showCancel = 0;
        keyboard->showDel = 1;
        keyboard->showEnd = 1;

        zge::vec3SetOne(ee::Ptr<zge::Vec3>(scale));
        addListener(node, Message::Draw, sym::drawGuiKeyboard);
        addListener(node, Message::Refresh, sym::refreshGuiKeyboard);
        addListener(node, Message::Input, sym::handleGuiKeyboardInput);
        addListener(node, Message::GetFocus, kRunOnGetFocus);
        addListener(node, Message::LostFocus, kRunOnLostFocus);
        // Items is set to the default list as soon as it is defined.
        defineClassProperties(keyboard, keyboard->properties, kBeforeItems);
        setVariable(ee::Ptr<Node>(node), ee::Ptr<const char>(kItemsName), items);
        defineClassProperties(keyboard, &keyboard->properties[3], kAfterItems);
        addVariableNode(ee::Ptr<Node>(node), ee::Ptr<const char>(sym::g_textSelectedCoord), ee::Ptr<const char>(sym::g_textZ3DVector), &keyboard->selectedCoord);
        addVariableNode(ee::Ptr<Node>(node), ee::Ptr<const char>(sym::g_textWidth), ee::Ptr<const char>(sym::g_textZFloat), &keyboard->width);
        addVariableNode(ee::Ptr<Node>(node), ee::Ptr<const char>(sym::g_textHeight), ee::Ptr<const char>(sym::g_textZFloat), &keyboard->height);
        return node;
    }

    // defineListedKeyboard: ZGE2's defineGuiKeyboardProperties: 13 keys a row,
    // white with a yellow selection, 24 apart, 16 characters, no special key
    // texts; its five handlers; Items set to the default keys (copied to the
    // frame) as soon as it is listed.
    uint32_t defineListedKeyboard(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedKeyboard> keyboard(address);
        ee::Local<KeyList> items;
        *items = *g_defaultItems;
        defineWidgetRecord(address, name);
        keyboard->charset = 0;
        keyboard->font = {};
        keyboard->onCancel = 0;
        keyboard->onGetFocus = 0;
        keyboard->onLostFocus = 0;
        keyboard->onSelect = 0;
        keyboard->onOk = 0;
        keyboard->selectedIndex = 0;
        keyboard->items = 0;
        keyboard->string = 0;
        keyboard->onItemFocus = 0;
        keyboard->onItemUnFocus = 0;
        keyboard->selectedCoord.x = ee::Float(0.0f);
        keyboard->selectedCoord.y = ee::Float(0.0f);
        keyboard->selectedCoord.z = ee::Float(0.0f);
        keyboard->onChange = 0;
        keyboard->selectedChar = 0;
        keyboard->onOkChar = 0;
        keyboard->onCancelChar = 0;
        keyboard->onDeleteChar = 0;
        keyboard->onSpaceChar = 0;
        keyboard->rowWidth = 13;
        keyboard->maxItems = 0x10;
        keyboard->vSpacing = ee::Float(24.0f);
        keyboard->hSpacing = ee::Float(24.0f);
        keyboard->color = {255, 255, 255, 255};
        keyboard->selectedColor = {255, 255, 0, 255};
        subscribeWidget(object, Message::Draw, sym::drawGuiKeyboard);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiKeyboard);
        subscribeWidget(object, Message::Input, sym::handleGuiKeyboardInput);
        subscribeWidget(object, Message::GetFocus, kRunOnGetFocus);
        subscribeWidget(object, Message::LostFocus, kRunOnLostFocus);
        addListedProperties(keyboard, kListedKeyboardItems);
        const ee::Ptr<zge::List> properties(valueAddress(keyboard->widget.properties));
        setListedVariable(properties, ee::Ptr<const char>(sym::g_textItems2), items);
        addListedProperties(keyboard, kListedKeyboardProperties);
        return address;
    }
    READABLE_FUNCTION_WAITS_OWN(defineGuiKeyboardProperties, Verified, dm2, defineListedKeyboard);
}
