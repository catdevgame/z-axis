// zgui's text widgets (widget.h): drawn and measured with their font.

#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstring>
#include "sym/zge/zgui/text.cpp.h"

namespace zge::zgui
{
    namespace
    {
        uint8_t scaled(uint8_t c, ee::Float alpha) { return static_cast<uint8_t>(ee::toInt(ee::toFloat(c) * alpha)); }


        // The font (looked up once), and the style for it; false without one.
        bool prepare(ee::Ptr<GuiText> widget, TextStyle &style)
        {
            if (widget->charset && !widget->asset)
                widget->asset = findGuiAsset(widget->charset, kAssetFont);
            if (!widget->asset)
                return false;
            style.position = widget->widget.node.screenPosition;
            style.size = widget->scale;
            style.monospaced = widget->monospace;
            style.angle = widget->angle;
            style.align = widget->alignment;
            std::memcpy(style.colour, widget->color.rgba, 4);
            const ee::Float alpha = g_zgui->alpha;
            const uint8_t r = scaled(style.colour[0], alpha), g = scaled(style.colour[1], alpha),
                          b = scaled(style.colour[2], alpha);
            style.letterSpacing = widget->tracking;
            style.lineSpacing = widget->spacing;
            style.cleared = 0;
            style.colour[0] = r;
            style.colour[1] = g;
            style.colour[2] = b;
            return true;
        }
    }

    namespace
    {
        void drawWrappedText(uint32_t font, ee::Ptr<TextStyle> style, uint32_t text, int32_t firstLine, ee::Ptr<int32_t> lineCount, uint32_t draw, ee::Float maxWidth, ee::Float maxHeight) { ee::game(sym::drawWrappedText, font, style, text, firstLine, lineCount, draw, maxWidth, maxHeight); }
    }

    int32_t drawTextWidget(ee::Ptr<GuiText> widget)
    {
        ee::Local<TextStyle> style;
        if (!prepare(widget, *style))
            return 1;
        const ee::Ptr<UiAsset> font = widget->asset;
        const uint32_t text = expandText(ee::Ptr<const char>(widget->text));
        drawWrappedText(font->object, style, text, widget->lineOffset, &widget->lineCount, 1, widget->width, widget->height);
        return 1;
    }
    READABLE_FUNCTION(drawTextWidget, Verified);

    namespace
    {
        void drawText(uint32_t font, ee::Ptr<TextStyle> style, uint32_t text) { ee::game(sym::drawText, font, style, text); }
    }

    int32_t measureTextWidget(ee::Ptr<GuiText> widget)
    {
        ee::Local<TextStyle> style;
        if (!prepare(widget, *style))
            return 1;
        const ee::Ptr<UiAsset> font = widget->asset;
        const uint32_t text = expandText(ee::Ptr<const char>(widget->text));
        drawText(font->object, style, text);
        widget->width = style->width;
        widget->height = style->height;
        return 1;
    }
    READABLE_FUNCTION_OWN(measureTextWidget, Verified, dm2, drawListedLabel);

    namespace
    {
        // ZGE2's label: its font (looked up once) and the style for it: the
        // widget's place, orientation and scale, its colour dimmed by zgui's
        // alpha; false without a font.
        bool prepareListedLabel(ee::Ptr<ListedLabel> label, zge::TextStyle &style)
        {
            if (label->charset && !label->font)
                label->font = findGuiAsset(label->charset, kAssetFont);
            if (!label->font)
                return false;
            style.position = label->widget.position;
            zge::quaternionCopy(zge::styleOrientation(style), &label->widget.orientation);
            std::memcpy(&style.scaleX, &label->widget.scale, sizeof(zge::Vec4)); // the scale's four words
            style.align = label->alignment;
            const ee::Float alpha = g_zgui->alpha;
            std::memcpy(style.colour, &label->color, sizeof(Rgba));
            const uint8_t r = scaled(style.colour[0], alpha), g = scaled(style.colour[1], alpha),
                          b = scaled(style.colour[2], alpha);
            style.colour[0] = r;
            style.colour[1] = g;
            style.colour[2] = b;
            style.lineSpacing = label->spacing;
            style.letterSpacing = label->tracking;
            *reinterpret_cast<uint8_t *>(&style.reserved) = 0; // ZGE2 clears its one byte
            return true;
        }
    }

    // drawListedLabel: ZGE2's measureTextWidget, the label's Draw: its text
    // drawn in its style (ZGE2 keeps no measured size). 1.
    int32_t drawListedLabel(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedLabel> label = messageRecord<ListedLabel>(message);
        if (!prepareListedLabel(label, *style))
            return 1;
        const ee::Ptr<UiAsset> font = label->font;
        const uint32_t text = expandText(ee::Ptr<const char>(label->text));
        ee::game(sym::drawText, font->object, style, text);
        return 1;
    }

    // labelBounds (ZGE2 only): the label's GetBounds: its text measured in
    // its style into the message's rectangle. 1.
    inline int32_t labelBounds(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedLabel> label = messageRecord<ListedLabel>(message);
        if (!prepareListedLabel(label, *style))
            return 1;
        const ee::Ptr<UiAsset> font = label->font;
        const uint32_t text = expandText(ee::Ptr<const char>(label->text));
        zge::measureText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(text),
                         ee::Ptr<zge::TextBounds>(message->data), ee::Ptr<int32_t>());
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, labelBounds);

    namespace
    {
        // textQuadList (font.cpp): the font's quads for the text as it
        // would be drawn, in a list; 0 for none.
        uint32_t textQuadList(uint32_t font, ee::Ptr<TextStyle> style, uint32_t text) { return ee::game<uint32_t>(sym::textQuadList, font, style, text); }
    }

    // measureTextAsset: the text widget's text as its font's quads (the list
    // textQuadList makes, styled as drawTextWidget styles it); 0 without
    // a font. No caller in AI.
    uint32_t measureTextAsset(ee::Ptr<GuiText> widget)
    {
        ee::Local<TextStyle> style;
        if (!prepare(widget, *style))
            return 0;
        const ee::Ptr<UiAsset> font = widget->asset;
        const uint32_t text = expandText(ee::Ptr<const char>(widget->text));
        return textQuadList(font->object, style, text);
    }
    READABLE_FUNCTION_WAITS_OWN(measureTextAsset, Draft, dm2, listedLabelQuads);

    // listedLabelQuads: ZGE2's measureTextAsset: a label object's text as its
    // font's quads; 0 without a font. No caller found.
    uint32_t listedLabelQuads(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::TextStyle> style;
        const ee::Ptr<ListedLabel> label(widgetRecord(object).address);
        if (!prepareListedLabel(label, *style))
            return 0;
        const ee::Ptr<UiAsset> font = label->font;
        const uint32_t text = expandText(ee::Ptr<const char>(label->text));
        return ee::game<uint32_t>(sym::textQuadList, font->object, style, text);
    }
}
