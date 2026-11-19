// zgui's scroll view: a text shown line by line through a window of the
// camera's viewport, drawn from a vertical Offset, each line's colour blended
// from TopColor through MidColor to BottomColor by how far down the window it
// is (the default colours fade the top and the bottom out: the credits).

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "zge/math/quaternion.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/zgui/scroll_view.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // ZGE2's (DM2's) scroll view record, after WidgetRecord (widget_parts.h).
        struct ListedScrollView
        {
            WidgetRecord widget;
            ee::Ptr<UiAsset> font; // +0x60 found by charset (refreshGuiScrollView)
            uint32_t parseText;    // +0x64 expand the text's $KEYS first
            uint32_t text;         // +0x68
            uint32_t charset;      // +0x6C
            Rgba topColor;         // +0x70
            Rgba bottomColor;      // +0x74
            Rgba midColor;         // +0x78
            int32_t tracking;      // +0x7C
            ee::Float spacing;     // +0x80
            ee::Float width;       // +0x84
            ee::Float height;      // +0x88
            ee::Float offset;      // +0x8C how far the text has scrolled up
        };
        static_assert(offsetof(ListedScrollView, topColor) == 0x70 && offsetof(ListedScrollView, offset) == 0x8C);

        constexpr ListedProperty kListedScrollViewProperties[] = {
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(ListedScrollView, spacing)},
            {sym::g_textTracking, sym::g_textSint32, offsetof(ListedScrollView, tracking)},
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedScrollView, charset)},
            {sym::g_textText6, sym::g_textString, offsetof(ListedScrollView, text)},
            {sym::g_textParseText, sym::g_textBool, offsetof(ListedScrollView, parseText)},
            {sym::g_textTopColor, sym::dm2::g_textZrgba, offsetof(ListedScrollView, topColor)},
            {sym::g_textMidColor, sym::dm2::g_textZrgba, offsetof(ListedScrollView, midColor)},
            {sym::g_textBottomColor, sym::dm2::g_textZrgba, offsetof(ListedScrollView, bottomColor)},
            {sym::g_textWidth, sym::g_textZFloat, offsetof(ListedScrollView, width)},
            {sym::g_textHeight, sym::g_textZFloat, offsetof(ListedScrollView, height)},
            {sym::g_textOffset, sym::g_textZFloat, offsetof(ListedScrollView, offset)},
        };

        inline constexpr ee::Ptr<char> g_textBuffer{sym::sharedText};

        uint8_t blended(uint8_t from, uint8_t to, ee::Float amount)
        {
            const int32_t start = from;
            return static_cast<uint8_t>(ee::toInt(ee::toFloat(start) + ee::toFloat(static_cast<int32_t>(to) - start) * amount));
        }
    }

    // refreshGuiScrollView: the scroll view's Refresh: its font, its three colours.
    uint32_t refreshGuiScrollView(uint32_t address)
    {
        const ee::Ptr<GuiScrollView> view(address);
        view->asset = findGuiAsset(view->charset, kAssetFont);
        if (view->topColor.paletteName)
            findPaletteColour(view->topColor.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiScrollView, topColor)));
        if (view->bottomColor.paletteName)
            findPaletteColour(view->bottomColor.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiScrollView, bottomColor)));
        if (view->midColor.paletteName)
            findPaletteColour(view->midColor.paletteName, ee::Ptr<uint8_t>(address + offsetof(GuiScrollView, midColor)));
        return 1;
    }

    // refreshScrollViewFont: ZGE2's refreshGuiScrollView: its font found
    // again. 1.
    int32_t refreshScrollViewFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedScrollView> view = messageRecord<ListedScrollView>(message);
        view->font = findGuiAsset(view->charset, kAssetFont);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiScrollView, Verified, dm2, refreshScrollViewFont);

    // drawGuiScrollView: the scroll view's Draw: the viewport swapped for the
    // window, each line that is inside it (half a line's slack at either
    // end) drawn centred, the viewport put back. Lines end at a carriage
    // return. The original also counts the text's characters first and never
    // uses the count.
    uint32_t drawGuiScrollView(uint32_t address)
    {
        ee::Local<ScreenRect> previousViewport;
        ee::Local<ScreenRect> window;
        ee::Local<zge::TextStyle> style;
        ee::Local<char[256]> line;
        const ee::Ptr<GuiScrollView> view(address);

        ee::Ptr<UiAsset> asset = view->asset;
        if (!view->charset || !asset)
        {
            if (view->charset)
            {
                view->asset = findGuiAsset(view->charset, kAssetFont);
                asset = view->asset;
            }
            if (!asset)
                return 1;
        }
        if (!view->text)
            return 1;

        const zge::Vec4 &position = view->widget.node.screenPosition;
        const ee::Float startOffset = view->height - view->offset;
        const ee::Float lineHeight = ee::toFloat(fontHeight(asset)) * view->spacing;

        // The camera's viewport is the window while the lines draw.
        window->x = position.x;
        window->y = position.y;
        window->width = view->width;
        window->height = view->height;
        const ee::Ptr<ScreenRect> previous(zge::swapCameraViewport(ee::Ptr<zge::Camera>(*zge::g_currentCamera), window));
        *previousViewport = *previous;

        style->position = position;
        style->position.x = style->position.x + view->width * ee::Float(0.5f);
        style->position.y = style->position.y + startOffset;
        std::memcpy(&style->scaleX, &view->scale, sizeof(zge::Vec4)); // the scale's four words
        style->align = lineAlignment(view);
        style->lineSpacing = view->spacing;
        style->letterSpacing = view->tracking;
        style->monospaced = 0;
        style->angle = 0;
        zge::clearStyleReserved(*style);

        ee::Ptr<const char> character(view->text);
        if (view->parseText)
        {
            const uint32_t expanded = expandText(ee::Ptr<const char>(view->text));
            zge::stringCopy(g_textBuffer, expanded);
            character = g_textBuffer.get();
        }

        for (;;)
        {
            // The next line, up to its carriage return.
            ee::Ptr<char> out = line;
            for (;;)
            {
                const char copied = *character;
                *out = copied;
                ++out;
                if (copied == '\r' || !copied)
                    break;
                ++character;
            }
            *(out - 1) = 0;

            const ee::Float top = position.y;
            const ee::Float halfLine = lineHeight * ee::Float(0.5f);
            ee::Float lineY = style->position.y;
            const bool belowTop = top - halfLine < lineY;
            const bool aboveBottom = belowTop && lineY < top + view->height + halfLine;
            if (aboveBottom)
            {
                // Its colour between the top's and the middle's, or the
                // middle's and the bottom's, by where it is.
                const ee::Float middle = top + view->height * ee::Float(0.5f);
                const ee::Float span = view->height + lineHeight;
                if (lineY < middle)
                {
                    const ee::Float half = span * ee::Float(0.5f);
                    const ee::Float amount = (half - (middle - lineY)) / half;
                    for (int channel = 0; channel < 4; ++channel)
                        style->colour[channel] = blended(view->topColor.rgba[channel], view->midColor.rgba[channel], amount);
                }
                else
                {
                    const ee::Float amount = (lineY - middle) / (span * ee::Float(0.5f));
                    for (int channel = 0; channel < 4; ++channel)
                        style->colour[channel] = blended(view->midColor.rgba[channel], view->bottomColor.rgba[channel], amount);
                }
                for (int channel = 0; channel < 3; ++channel)
                    style->colour[channel] = faded(style->colour[channel]);
                const uint32_t expanded = expandText(line);
                drawText(ee::Ptr<zge::Font>(view->asset->object), style, ee::Ptr<const int8_t>(expanded));
                lineY = style->position.y;
            }
            style->position.y = lineY + lineHeight;
            const char ended = *character;
            ++character;
            if (!ended)
                break;
        }
        if constexpr (game::readable::kIsBmx)
            *endReachedOf(view) = style->position.y < position.y ? 1 : 0;
        zge::swapCameraViewport(ee::Ptr<zge::Camera>(*zge::g_currentCamera), previousViewport);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiScrollView, Verified, dm2, drawListedScrollView);

    // drawListedScrollView: ZGE2's drawGuiScrollView, the scroll view's Draw:
    // the same lines in the same window and colours over the ZGE2 record,
    // with the widget's orientation in the style and the expanded text
    // (ParseText) in the frame.
    int32_t drawListedScrollView(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x400]> expandedText;
        ee::Local<ScreenRect> previousViewport;
        ee::Local<ScreenRect> window;
        ee::Local<ListedStyleRoom> styleRoom;
        ee::Local<char[0xF0]> line;
        const ee::Ptr<zge::TextStyle> style(styleRoom.address());
        const ee::Ptr<ListedScrollView> view = messageRecord<ListedScrollView>(message);
        if (!view->charset || !view->font)
        {
            if (view->charset)
                view->font = findGuiAsset(view->charset, kAssetFont);
            if (!view->font)
                return 1;
        }
        if (!view->text)
            return 1;

        const zge::Vec4 &position = view->widget.position;
        const ee::Float startOffset = view->height - view->offset;
        const ee::Float lineHeight = ee::toFloat(fontHeight(view->font)) * view->spacing;

        // The camera's viewport is the window while the lines draw.
        window->x = position.x;
        window->y = position.y;
        window->width = view->width;
        window->height = view->height;
        const ee::Ptr<ScreenRect> previous(zge::swapCameraViewport(ee::Ptr<zge::Camera>(*zge::g_currentCamera), window));
        *previousViewport = *previous;

        style->position = position;
        style->position.y = style->position.y + startOffset;
        style->position.x = style->position.x + view->width * ee::Float(0.5f);
        zge::quaternionCopy(zge::styleOrientation(*style), &view->widget.orientation);
        std::memcpy(&style->scaleX, &view->widget.scale, sizeof(zge::Vec4)); // the scale's four words
        *reinterpret_cast<uint8_t *>(&style->reserved) = 0; // ZGE2 clears its one byte
        style->align = kCentredText;
        style->lineSpacing = view->spacing;
        style->letterSpacing = view->tracking;

        ee::Ptr<const char> character(view->text);
        if (view->parseText)
        {
            const uint32_t expanded = expandText(ee::Ptr<const char>(view->text));
            zge::stringCopy(expandedText, expanded);
            character = ee::Ptr<const char>(expandedText.address());
        }

        const uint8_t *const topColor = &view->topColor.r, *const midColor = &view->midColor.r,
                             *const bottomColor = &view->bottomColor.r;
        for (;;)
        {
            // The next line, up to its carriage return.
            ee::Ptr<char> out = line;
            for (;;)
            {
                const char copied = *character;
                *out = copied;
                ++out;
                if (copied == '\r' || !copied)
                    break;
                ++character;
            }
            *(out - 1) = 0;

            const ee::Float top = position.y;
            const ee::Float halfLine = lineHeight * ee::Float(0.5f);
            ee::Float lineY = style->position.y;
            const bool belowTop = top - halfLine < lineY;
            const bool aboveBottom = belowTop && lineY < top + view->height + halfLine;
            if (aboveBottom)
            {
                // Its colour between the top's and the middle's, or the
                // middle's and the bottom's, by where it is.
                const ee::Float middle = top + view->height * ee::Float(0.5f);
                const ee::Float span = view->height + lineHeight;
                if (lineY < middle)
                {
                    const ee::Float half = span * ee::Float(0.5f);
                    const ee::Float amount = (half - (middle - lineY)) / half;
                    for (int channel = 0; channel < 4; ++channel)
                        style->colour[channel] = blended(topColor[channel], midColor[channel], amount);
                }
                else
                {
                    const ee::Float amount = (lineY - middle) / (span * ee::Float(0.5f));
                    for (int channel = 0; channel < 4; ++channel)
                        style->colour[channel] = blended(midColor[channel], bottomColor[channel], amount);
                }
                for (int channel = 0; channel < 3; ++channel)
                    style->colour[channel] = faded(style->colour[channel]);
                const uint32_t expanded = expandText(line);
                drawText(ee::Ptr<zge::Font>(view->font->object), style, ee::Ptr<const int8_t>(expanded));
                lineY = style->position.y;
            }
            style->position.y = lineY + lineHeight;
            const char ended = *character;
            ++character;
            if (!ended)
                break;
        }
        zge::swapCameraViewport(ee::Ptr<zge::Camera>(*zge::g_currentCamera), previousViewport);
        return 1;
    }

    // defineGuiScrollViewProperties: a scroll view's defaults (white, faded out at the top and
    // the bottom, 128 square) and properties.
    uint32_t defineGuiScrollViewProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiScrollView> view(node);
        const uint32_t scale = node + offsetof(GuiScrollView, scale);
        defineWidgetProperties(node, text);
        view->text = 0;
        view->asset = {};
        view->charset = 0;
        ee::Ptr<GuiScrollView>(node)->offset = 0;
        if constexpr (game::readable::kIsBmx)
            *endReachedOf(view) = 0;
        setWhite(view->topColor);
        setWhite(view->midColor);
        setWhite(view->bottomColor);
        view->topColor.rgba[3] = 0;
        view->bottomColor.rgba[3] = 0;
        view->spacing = ee::Float(1.25f);
        view->height = ee::Float(128.0f);
        view->width = ee::Float(128.0f);
        view->parseText = 1;
        view->tracking = 1;
        zge::vec3SetOne(ee::Ptr<zge::Vec3>(scale));
        addListener(node, Message::Draw, sym::drawGuiScrollView);
        addListener(node, Message::Refresh, sym::refreshGuiScrollView);
        defineClassProperties(view, view->properties, kScrollViewProperties);
        if constexpr (game::readable::kIsBmx)
            addVariableNode(view, ee::Ptr<const char>(sym::bmx::g_textEndReached), ee::Ptr<const char>(sym::g_textBool), endReachedOf(view));
        return node;
    }

    // defineListedScrollView: ZGE2's defineGuiScrollViewProperties: the same
    // defaults, its Draw, Refresh and GetBounds.
    uint32_t defineListedScrollView(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedScrollView> view(address);
        defineWidgetRecord(address, name);
        view->text = 0;
        view->font = {};
        view->charset = 0;
        view->offset = ee::Float(0.0f);
        view->topColor = {255, 255, 255, 0};
        view->bottomColor = {255, 255, 255, 0};
        view->midColor = {255, 255, 255, 255};
        view->spacing = ee::Float(1.25f);
        view->height = ee::Float(128.0f);
        view->width = ee::Float(128.0f);
        view->parseText = 1;
        view->tracking = 1;
        subscribeWidget(object, Message::Draw, sym::drawGuiScrollView);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiScrollView);
        subscribeWidget(object, Message::GetBounds, sym::dm2::scrollViewBounds);
        addListedProperties(view, kListedScrollViewProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiScrollViewProperties, Verified, dm2, defineListedScrollView);

    // scrollViewBounds (ZGE2 only): the scroll view's GetBounds: Width by
    // Height from its position down and to the right, sized by its
    // orientation (not its scale). 1.
    inline int32_t scrollViewBounds(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Matrix> matrix;
        const ee::Ptr<ListedScrollView> view = messageRecord<ListedScrollView>(message);
        zge::TextBounds &bounds = messageBounds(message);
        bounds.left = ee::Float(0.0f);
        bounds.right = view->width;
        bounds.top = ee::Float(0.0f);
        bounds.bottom = view->height;
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix, &view->widget.orientation);
        placeBounds(bounds, *matrix, view->widget.position);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, scrollViewBounds);
}
