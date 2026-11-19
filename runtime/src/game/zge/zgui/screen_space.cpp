// guiScreenSpace, the widget that holds a menu's screens, and what only its
// class has: PaletteEntries, a text of `name r g b a` lines that the colour
// properties look up (WidgetColour's paletteName). No shipped script sets it
// in AI. And guiNodeBounds, the rectangle a widget's Width, Height, Scale,
// Alignment and Angle make: dead.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/math/matrix.h"
#include "zge/memory/heaps.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/zgui/screen_space.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kQueuePaletteEntry = sym::queueGuiPaletteNode;
        constexpr uint32_t kRefreshScreenSpace = sym::refreshGuiScreenSpace;
        constexpr uint32_t kPaletteEntriesName = sym::g_textPaletteEntries, kZAssetIdType = sym::g_textZAssetID4;
        constexpr uint32_t kWidthName = sym::g_textWidth2, kHeightName = sym::g_textHeight3, kScaleName = sym::g_textScale2;
        constexpr uint32_t kAlignmentName = sym::g_textAlignment2, kAngleName = sym::g_textAngle2;
        constexpr uint32_t kNameBytes = 8;

        // The palette: a list of these, by the name's characters packed.
        struct PaletteEntry
        {
            ee::Ptr<PaletteEntry> next, previous;
            uint32_t colour;      // +0x08 r, g, b, a
            uint32_t padding;     // +0x0C never written: the 64-bit name's alignment
            uint64_t name;        // +0x10
        };
        static_assert(offsetof(PaletteEntry, colour) == 8 && offsetof(PaletteEntry, name) == 0x10 && sizeof(PaletteEntry) == 24);
        constexpr uint32_t kPalette = sym::g_palette;
        inline ee::Global<ee::Ptr<PaletteEntry>, sym::g_palette> g_palette;

        struct ScreenSpace
        {
            Widget widget;
            ee::Ptr<const char> paletteEntries; // +0xF0 "PaletteEntries" (a ZAssetID): its text
        };
        static_assert(offsetof(ScreenSpace, paletteEntries) == 0xF0);

        // The screen's size in pixels (ZGE4's screen space takes it).
        inline ee::Global<uint32_t, sym::g_screenWidth> g_screenWidth;
        inline ee::Global<uint32_t, sym::g_screenHeight> g_screenHeight;

        // Lanes of a scale: (1, 1, 1, 1) by .sdata's look.
        inline ee::Global<zge::Vec4, sym::g_unitScale> g_unitScale;

        // A widget's rectangle on the screen.
        struct ScreenBounds
        {
            ee::Float left, top, right, bottom;
        };

        // The blanks at `text` (space, CR, LF) skipped: the first other
        // character.
        int32_t skipBlanks(ee::Ptr<const char> &text)
        {
            while (*text == ' ' || *text == '\r' || *text == '\n')
                ++text;
            return static_cast<int8_t>(*text);
        }

        // The name at `text`, which moves past it (it ends at a space, a '{'
        // or the text's end): its length.
        int32_t scanName(ee::Ptr<const char> &text, int32_t head)
        {
            int32_t length = 0;
            if (head != ' ' && head != 0 && head != '{')
            {
                do
                {
                    ++text;
                    ++length;
                } while (*text != ' ' && *text != 0 && *text != '{');
            }
            return length;
        }

        // The rest of the line skipped, up to its CR or the text's end.
        void skipLine(ee::Ptr<const char> &text)
        {
            while (*text != '\r' && *text != 0)
                ++text;
        }
    }

    // queueGuiPaletteNode: a name's colour: its entry's changed, or one added.
    void queueGuiPaletteNode(uint64_t name, uint32_t colour)
    {
        ee::Local<uint32_t> colourSlot;
        *colourSlot = colour;
        ee::Ptr<PaletteEntry> entry(*g_palette);
        if (entry)
        {
            for (;;)
            {
                if (entry->name == name)
                {
                    std::memcpy(&entry->colour, &*colourSlot, 4);
                    return;
                }
                entry = entry->next;
                if (!entry)
                    break;
            }
        }
        const ee::Ptr<PaletteEntry> added(zge::heapTake(ee::Ptr<zge::Heap>(*g_heap), sizeof(PaletteEntry)));
        std::memcpy(&added->colour, &*colourSlot, 4);
        added->name = name;
        listPush(kPalette, added);
    }
    READABLE_FUNCTION(queueGuiPaletteNode, Verified);

    // refreshGuiScreenSpace: the Refresh message: each line of PaletteEntries (a name
    // of at most eight characters, four numbers) into the palette.
    int32_t refreshGuiScreenSpace(ee::Ptr<ScreenSpace> widget)
    {
        ee::Local<uint8_t[4]> colour; // r, g, b, a
        ee::Local<char[16]> name;
        ee::Local<int32_t[4]> numbers;
        ee::Ptr<const char> text = widget->paletteEntries;
        if (!text || *text == 0)
            return 1;
        for (;;)
        {
            const int32_t head = skipBlanks(text);
            const ee::Ptr<const char> start = text;
            const int32_t length = scanName(text, head);
            zge::stringCopyN(name, start, kNameBytes);
            name[length >= 9 ? 8 : length] = 0;
            for (uint32_t i = 0; i < 4; ++i)
                numbers[i] = 0;
            for (uint32_t i = 0; i < 4; ++i)
                text = parseInt(text, &numbers[i]);
            const char first = name[0];
            skipLine(text);

            // The name packed as findPaletteColour takes it: each character
            // sign-extended into the word.
            uint64_t packed = 0;
            if (first != 0)
            {
                for (uint32_t i = 0;;)
                {
                    packed |= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(name[i]))) << (i * 8);
                    ++i;
                    if (i >= kNameBytes || name[i] == 0)
                        break;
                }
            }
            for (uint32_t i = 0; i < 4; ++i)
                colour[i] = static_cast<uint8_t>(numbers[i]);
            uint32_t rgba;
            std::memcpy(&rgba, &colour[0], 4);
            queueGuiPaletteNode(packed, static_cast<uint64_t>(rgba));
            if (*text == 0)
                break;
        }
        return 1;
    }
    READABLE_FUNCTION(refreshGuiScreenSpace, Verified);

    // defineGuiScreenSpaceProperties: the class's set-up. ZGE4's screen
    // space is the screen's size (ScreenWidth by ScreenHeight, as unsigned),
    // with Width and Height as variables.
    uint32_t defineGuiScreenSpaceProperties(uint32_t address, uint32_t name)
    {
        defineWidgetProperties(address, name);
        const ee::Ptr<ScreenSpace> widget(address);
        widget->paletteEntries = 0;
        if constexpr (game::readable::kIsBmx)
            setWidgetSize(classCast<Widget>(widget), unsignedToFloat(*g_screenWidth), unsignedToFloat(*g_screenHeight));
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kPaletteEntriesName), ee::Ptr<const char>(kZAssetIdType), address + offsetof(ScreenSpace, paletteEntries));
        if constexpr (game::readable::kIsBmx)
        {
            const ee::Ptr<const char> zfloat(sym::bmx::g_textScreenSpaceZFloat);
            addVariableNode(address, ee::Ptr<const char>(sym::bmx::g_textScreenSpaceWidth), zfloat, widgetWidthAddress(classCast<Widget>(widget)));
            addVariableNode(address, ee::Ptr<const char>(sym::bmx::g_textScreenSpaceHeight), zfloat, widgetHeightAddress(classCast<Widget>(widget)));
        }
        addListener(address, Message::Refresh, kRefreshScreenSpace);
        return address;
    }
    READABLE_FUNCTION(defineGuiScreenSpaceProperties, Verified);

    // guiNodeMatrix: the rectangle (left, top, right, bottom) the widget's
    // four corners make, turned by its Angle. A corner the alignment does not
    // set is what the stack holds (the original's: values outside 0 .. 2).
    int32_t guiNodeMatrix(ee::Ptr<Node> node, ee::Ptr<ScreenBounds> bounds)
    {
        ee::Local<zge::Vec4> scale;
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Vec4[4]> corner; // top left, top right, bottom right, bottom left
        ee::Local<ee::Float> width;
        ee::Local<ee::Float> height;
        ee::Local<int32_t> alignment;
        ee::Local<ee::Float> angle;     // degrees
        if (!bounds || !node)
            return 0;
        if (node->kind() != NodeKind::Widget)
            return 0;
        *scale = *g_unitScale;
        *width = 0.0f;
        *height = 0.0f;
        *alignment = 0;
        *angle = 0.0f;
        copyVariable(node, kWidthName, width);
        copyVariable(node, kHeightName, height);
        copyVariable(node, kScaleName, scale);
        copyVariable(node, kAlignmentName, alignment);
        copyVariable(node, kAngleName, angle);
        zge::matrixIdentity(matrix);
        const int32_t turn = ee::toInt(*angle * ee::Float(182.04445f)); // degrees to angle units
        zge::matrixRotateZ(matrix, matrix, static_cast<zge::Angle>((turn << 16) >> 16));
        const uint32_t align = static_cast<uint32_t>(*alignment);
        // The matrix's translation: the screen position.
        zge::Vec4 &translation = zge::matrixRow(*matrix, 3);
        translation.x = node->screenPosition.x;
        translation.y = node->screenPosition.y;
        translation.z = node->screenPosition.z;
        // Vertical: 0 from the top, 0x10 about the middle, 0x20 up from the bottom.
        const uint32_t vertical = align & 0xF0u;
        if (vertical == 0x10u)
        {
            const ee::Float h = *height, s = scale->y;
            const ee::Float low = h * (s * ee::Float(0.5f)), high = -(h * ee::Float(0.5f)) * s;
            corner[3].y = low;
            corner[2].y = low;
            corner[1].y = high;
            corner[0].y = high;
        }
        else if (vertical == 0)
        {
            const ee::Float bottom = *height * scale->y;
            corner[1].y = 0.0f;
            corner[0].y = 0.0f;
            corner[3].y = bottom;
            corner[2].y = bottom;
        }
        else if (vertical == 0x20u)
        {
            const ee::Float s = scale->y, negative = -*height;
            corner[3].y = 0.0f;
            corner[2].y = 0.0f;
            const ee::Float top = negative * s;
            corner[1].y = top;
            corner[0].y = top;
        }
        // Horizontal: 0 from the left, 1 about the middle, 2 from the right.
        const uint32_t horizontal = align & 0xFu;
        const ee::Float w = *width, s = scale->x;
        if (horizontal == 1)
        {
            const ee::Float half = s * ee::Float(0.5f);
            const ee::Float right = w * half, left = -w * half;
            corner[2].x = right;
            corner[3].x = left;
            corner[0].x = left;
            corner[1].x = right;
        }
        else if (horizontal == 0)
        {
            const ee::Float right = w * s;
            corner[0].x = 0.0f;
            corner[3].x = 0.0f;
            corner[2].x = right;
            corner[1].x = right;
        }
        else if (horizontal == 2)
        {
            const ee::Float left = -w * s;
            corner[1].x = 0.0f;
            corner[2].x = 0.0f;
            corner[3].x = left;
            corner[0].x = left;
        }
        for (uint32_t i = 0; i < 4; ++i)
            corner[i] = zge::transformed(*matrix, corner[i]);
        // The rectangle around the four.
        ScreenBounds &rect = *bounds;
        rect.left = corner[0].x;
        rect.top = corner[0].y;
        rect.right = corner[2].x;
        rect.bottom = corner[2].y;
        for (uint32_t i = 0; i < 4; ++i)
        {
            const ee::Float x = corner[i].x;
            if (x < rect.left)
                rect.left = x;
            if (rect.right < x)
                rect.right = x;
            const ee::Float y = corner[i].y;
            if (y < rect.top)
                rect.top = y;
            if (rect.bottom < y)
                rect.bottom = y;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(guiNodeMatrix, Verified);
}
