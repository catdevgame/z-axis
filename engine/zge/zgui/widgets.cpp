// zgui's simple widgets (widget_parts.h): the line, the text box, the label,
// the image and the frame: what each draws is a class of its own (text.cpp,
// quad.cpp, box.cpp; the line here), its Refresh message finds the assets it
// names, and its definition sets its defaults and its properties, which the
// UI scripts set by name (Scale, Color, Text, Material ...).

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/text/font.h"
#include "zge/render/primitives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/zgui/widgets.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kTexturedLine = 0x02000004u; // its kind

        // What every class draws with: drawTextWidget and the others' listeners.
        constexpr uint32_t kDrawTextWidget = sym::drawTextWidget, kDrawLabel = sym::measureTextWidget;
        constexpr uint32_t kDrawImage = sym::drawQuadWidget, kDrawFrame = sym::drawBoxWidget;

        using GuiLabel = GuiText;
        using GuiTextBox = GuiText;

        // A label has no lines: its properties' nodes start where a text
        // box keeps its line offset and count.
        struct GuiLabelProperties
        {
            uint8_t text[offsetof(GuiText, lineOffset)];
            PropertyNode properties[9]; // +0x138 (ZGE4 +0x12C)
        };
        static_assert(offsetof(GuiLabelProperties, properties) == game::readable::perGame(0x138u, 0x138u, 0x12Cu));
        ee::Ptr<PropertyNode> labelPropertyNodes(ee::Ptr<GuiLabel> label)
        {
            return classCast<GuiLabelProperties>(label)->properties;
        }

        constexpr ClassProperty kLineProperties[] = {
            {sym::g_textVector, sym::g_textZ3DVector, offsetof(GuiLine, vector)}, // "Vector" Z3DVector
            {sym::g_textStartColor, sym::g_textZrgbaPal, offsetof(GuiLine, startColor)}, // "StartColor" ZRGBA_PAL
            {sym::g_textEndColor, sym::g_textZrgbaPal, offsetof(GuiLine, endColor)}, // "EndColor" ZRGBA_PAL
            {sym::g_textWidth, sym::g_textZFloat, offsetof(GuiLine, width)}, // "Width" ZFloat
            {sym::g_textMaterial, sym::g_textZAssetID2, offsetof(GuiLine, material)}, // "Material" ZAssetID
        };

        constexpr ClassProperty kTextBoxProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiText, scale)}, // "Scale" Z3DVector
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiText, alignment)}, // "Alignment" Alignment
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(GuiText, spacing)}, // "Spacing" ZFloat
            {sym::g_textTracking, sym::g_textSint32, offsetof(GuiText, tracking)}, // "Tracking" Sint32
            {sym::g_textMonospace, sym::g_textBool, offsetof(GuiText, monospace)}, // "Monospace" Bool
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiText, charset)}, // "Charset" ZAssetID
            {sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiText, color)}, // "Color" ZRGBA_PAL
            {sym::g_textText6, sym::g_textString, offsetof(GuiText, text)}, // "Text" String
            {sym::g_textAngle3, sym::g_textZFloat, offsetof(GuiText, angle)}, // "Angle" ZFloat
            {sym::g_textWidth, sym::g_textZFloat, offsetof(GuiText, width)}, // "Width" ZFloat
            {sym::g_textHeight, sym::g_textZFloat, offsetof(GuiText, height)}, // "Height" ZFloat
            {sym::g_textLineOffset, sym::g_textSint32, offsetof(GuiTextBox, lineOffset)}, // "LineOffset" Sint32
        };

        constexpr ClassProperty kLabelProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiLabel, scale)}, // "Scale" Z3DVector
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiLabel, alignment)}, // "Alignment" Alignment
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(GuiLabel, spacing)}, // "Spacing" ZFloat
            {sym::g_textTracking, sym::g_textSint32, offsetof(GuiLabel, tracking)}, // "Tracking" Sint32
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiLabel, charset)}, // "Charset" ZAssetID
            {sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiLabel, color)}, // "Color" ZRGBA_PAL
            {sym::g_textText6, sym::g_textString, offsetof(GuiLabel, text)}, // "Text" String
            {sym::g_textAngle3, sym::g_textZFloat, offsetof(GuiLabel, angle)}, // "Angle" ZFloat
            {sym::g_textMonospace, sym::g_textBool, offsetof(GuiLabel, monospace)}, // "Monospace" Bool
        };

        constexpr ClassProperty kImageProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiImage, scale)}, // "Scale" Z3DVector
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiImage, alignment)}, // "Alignment" Alignment
            {sym::g_textTopLeftColor, sym::g_textZrgbaPal, offsetof(GuiImage, topLeftColor)}, // "TopLeftColor" ZRGBA_PAL
            {sym::g_textBottomLeftColor, sym::g_textZrgbaPal, offsetof(GuiImage, bottomLeftColor)}, // "BottomLeftColor" ZRGBA_PAL
            {sym::g_textTopRightColor, sym::g_textZrgbaPal, offsetof(GuiImage, topRightColor)}, // "TopRightColor" ZRGBA_PAL
            {sym::g_textBottomRightColor, sym::g_textZrgbaPal, offsetof(GuiImage, bottomRightColor)}, // "BottomRightColor" ZRGBA_PAL
            {sym::g_textMaterial, sym::g_textZAssetID2, offsetof(GuiImage, material)}, // "Material" ZAssetID
            {sym::g_textWidth, sym::g_textZFloat, offsetof(GuiImage, width)}, // "Width" ZFloat
            {sym::g_textHeight, sym::g_textZFloat, offsetof(GuiImage, height)}, // "Height" ZFloat
            {sym::g_textUScale, sym::g_textZFloat, offsetof(GuiImage, uScale)}, // "UScale" ZFloat
            {sym::g_textVScale, sym::g_textZFloat, offsetof(GuiImage, vScale)}, // "VScale" ZFloat
        };

        constexpr ClassProperty kFrameProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiFrame, scale)}, // "Scale" Z3DVector
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiFrame, alignment)}, // "Alignment" Alignment
            {sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiFrame, color)}, // "Color" ZRGBA_PAL
            {sym::g_textMaterialTopLeft, sym::g_textZAssetID2, offsetof(GuiFrame, materials[0])}, // "Material_TopLeft" ZAssetID
            {sym::g_textMaterialTop, sym::g_textZAssetID2, offsetof(GuiFrame, materials[1])}, // "Material_Top" ZAssetID
            {sym::g_textMaterialTopRight, sym::g_textZAssetID2, offsetof(GuiFrame, materials[2])}, // "Material_TopRight" ZAssetID
            {sym::g_textMaterialLeft, sym::g_textZAssetID2, offsetof(GuiFrame, materials[3])}, // "Material_Left" ZAssetID
            {sym::g_textMaterialCenter, sym::g_textZAssetID2, offsetof(GuiFrame, materials[4])}, // "Material_Center" ZAssetID
            {sym::g_textMaterialRight, sym::g_textZAssetID2, offsetof(GuiFrame, materials[5])}, // "Material_Right" ZAssetID
            {sym::g_textMaterialBottomLeft, sym::g_textZAssetID2, offsetof(GuiFrame, materials[6])}, // "Material_BottomLeft" ZAssetID
            {sym::g_textMaterialBottom, sym::g_textZAssetID2, offsetof(GuiFrame, materials[7])}, // "Material_Bottom" ZAssetID
            {sym::g_textMaterialBottomRight, sym::g_textZAssetID2, offsetof(GuiFrame, materials[8])}, // "Material_BottomRight" ZAssetID
            {sym::g_textWidth, sym::g_textZFloat, offsetof(GuiFrame, width)}, // "Width" ZFloat
            {sym::g_textHeight, sym::g_textZFloat, offsetof(GuiFrame, height)}, // "Height" ZFloat
        };

        // ZGE2's (DM2's) records of these kinds, after WidgetRecord (widget_parts.h): colours of four bytes
        // (Rgba, no palette name) and no Scale of their own (the widget's is in WidgetRecord).
        struct ListedLine
        {
            WidgetRecord widget;
            ee::Ptr<UiAsset> asset; // +0x60 found by material (refreshGuiLine)
            zge::Vec4 vector;       // +0x70 the far end from the position
            Rgba startColor;        // +0x80
            Rgba endColor;          // +0x84
            ee::Float width;        // +0x88
            uint32_t material;      // +0x8C
        };
        static_assert(offsetof(ListedLine, vector) == 0x70 && offsetof(ListedLine, material) == 0x8C);

        constexpr ListedProperty kListedLabelProperties[] = {
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(ListedLabel, alignment)},
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(ListedLabel, spacing)},
            {sym::g_textTracking, sym::g_textSint32, offsetof(ListedLabel, tracking)},
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedLabel, charset)},
            {sym::g_textColor, sym::dm2::g_textZrgba, offsetof(ListedLabel, color)},
            {sym::g_textText6, sym::g_textString, offsetof(ListedLabel, text)},
        };

        constexpr ListedProperty kListedImageProperties[] = {
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(ListedImage, alignment)},
            {sym::g_textTopLeftColor, sym::dm2::g_textZrgba, offsetof(ListedImage, topLeftColor)},
            {sym::g_textBottomLeftColor, sym::dm2::g_textZrgba, offsetof(ListedImage, bottomLeftColor)},
            {sym::g_textTopRightColor, sym::dm2::g_textZrgba, offsetof(ListedImage, topRightColor)},
            {sym::g_textBottomRightColor, sym::dm2::g_textZrgba, offsetof(ListedImage, bottomRightColor)},
            {sym::g_textMaterial, sym::g_textZAssetID2, offsetof(ListedImage, material)},
            {sym::g_textWidth, sym::g_textZFloat, offsetof(ListedImage, width)},
            {sym::g_textHeight, sym::g_textZFloat, offsetof(ListedImage, height)},
            {sym::g_textUScale, sym::g_textZFloat, offsetof(ListedImage, uScale)},
            {sym::g_textVScale, sym::g_textZFloat, offsetof(ListedImage, vScale)},
        };

        constexpr ListedProperty kListedLineProperties[] = {
            {sym::g_textVector, sym::g_textZ3DVector, offsetof(ListedLine, vector)},
            {sym::g_textStartColor, sym::dm2::g_textZrgba, offsetof(ListedLine, startColor)},
            {sym::g_textEndColor, sym::dm2::g_textZrgba, offsetof(ListedLine, endColor)},
            {sym::g_textWidth, sym::g_textZFloat, offsetof(ListedLine, width)},
            {sym::g_textMaterial, sym::g_textZAssetID2, offsetof(ListedLine, material)},
        };

        constexpr ListedProperty kListedFrameProperties[] = {
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(ListedFrame, alignment)},
            {sym::g_textColor, sym::dm2::g_textZrgba, offsetof(ListedFrame, color)},
            {sym::g_textMaterialTopLeft, sym::g_textZAssetID2, offsetof(ListedFrame, materials[0])},
            {sym::g_textMaterialTop, sym::g_textZAssetID2, offsetof(ListedFrame, materials[1])},
            {sym::g_textMaterialTopRight, sym::g_textZAssetID2, offsetof(ListedFrame, materials[2])},
            {sym::g_textMaterialLeft, sym::g_textZAssetID2, offsetof(ListedFrame, materials[3])},
            {sym::g_textMaterialCenter, sym::g_textZAssetID2, offsetof(ListedFrame, materials[4])},
            {sym::g_textMaterialRight, sym::g_textZAssetID2, offsetof(ListedFrame, materials[5])},
            {sym::g_textMaterialBottomLeft, sym::g_textZAssetID2, offsetof(ListedFrame, materials[6])},
            {sym::g_textMaterialBottom, sym::g_textZAssetID2, offsetof(ListedFrame, materials[7])},
            {sym::g_textMaterialBottomRight, sym::g_textZAssetID2, offsetof(ListedFrame, materials[8])},
            {sym::g_textWidth, sym::g_textZFloat, offsetof(ListedFrame, width)},
            {sym::g_textHeight, sym::g_textZFloat, offsetof(ListedFrame, height)},
        };

        constexpr Rgba kWhite = {255, 255, 255, 255};

        // The style a text widget draws with.
        void fillTextStyle(ee::Ptr<zge::TextStyle> style, ee::Ptr<GuiText> widget)
        {
            style->position = widget->widget.node.screenPosition;
            std::memcpy(&style->scaleX, &widget->scale, sizeof(zge::Vec4));
            style->monospaced = widget->monospace;
            style->angle = widget->angle;
            style->align = widget->alignment;
            style->lineSpacing = widget->spacing;
            style->letterSpacing = widget->tracking;
            zge::clearStyleReserved(*style);
            style->colour[0] = faded(widget->color.rgba[0]);
            style->colour[1] = faded(widget->color.rgba[1]);
            style->colour[2] = faded(widget->color.rgba[2]);
            style->colour[3] = widget->color.rgba[3];
        }

        void setTextDefaults(GuiText &widget)
        {
            widget.text = 0;
            widget.alignment = 0;
            widget.asset = {};
            widget.charset = 0;
            widget.monospace = 0;
            widget.spacing = ee::Float(1.25f);
            widget.tracking = 1;
            setWhite(widget.color);
        }
    }

    // refreshGuiLine: the line's Refresh.
    uint32_t refreshGuiLine(ee::Ptr<GuiLine> line)
    {
        line->asset = findGuiAsset(line->material, kAssetTexture);
        lookUpPaletteColour(line->startColor);
        lookUpPaletteColour(line->endColor);
        return 1;
    }

    // refreshLineTexture: ZGE2's refreshGuiLine: its texture found again
    // (ZGE2's colours have no palette names). 1.
    int32_t refreshLineTexture(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedLine> line = messageRecord<ListedLine>(message);
        line->asset = findGuiAsset(line->material, kAssetTexture);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiLine, Verified, dm2, refreshLineTexture);

    namespace
    {
        // What drawLines takes: a material and two vertices.
        struct LineVertex
        {
            ee::Float x, y, z;
            uint32_t _0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(LineVertex) == 0x20);
        struct LineRecord
        {
            uint32_t material;
            ee::Ptr<LineVertex> first, second;
            uint32_t _0c;
            LineVertex vertices[2];  // +0x10
        };
        static_assert(sizeof(LineRecord) == 0x50);
    }

    // drawGuiLine: the line's Draw: one textured line, half a pixel off,
    // from the widget's position along its vector. A bug kept: the far end's
    // z is the vector's z added to the widget's y, not its z.
    uint32_t drawGuiLine(ee::Ptr<GuiLine> line)
    {
        ee::Local<LineRecord> record;

        ee::Ptr<UiAsset> asset = line->asset;
        if (line->material && !asset)
        {
            asset = findGuiAsset(line->material, kAssetTexture);
            line->asset = asset;
        }

        const zge::Vec4 &at = line->widget.node.screenPosition;
        const ee::Float halfPixel(-0.5f);
        LineVertex &start = record->vertices[0];
        LineVertex &end = record->vertices[1];
        start.x = at.x + halfPixel;
        start.y = at.y + halfPixel;
        start.z = at.z;
        start.u = ee::Float(0.0f);
        start.v = ee::Float(0.0f);
        end.x = at.x + line->vector.x + halfPixel;
        end.y = at.y + line->vector.y + halfPixel;
        end.z = at.y + line->vector.z;
        end.u = ee::Float(1.0f);
        end.v = ee::Float(1.0f);
        for (int channel = 0; channel < 3; ++channel)
        {
            start.colour[channel] = faded(line->startColor.rgba[channel]);
            end.colour[channel] = faded(line->endColor.rgba[channel]);
        }
        start.colour[3] = line->startColor.rgba[3];
        end.colour[3] = line->endColor.rgba[3];
        record->first = &start;
        record->second = &end;
        record->material = asset ? asset->object : g_zgui->defaultTexture;
        drawLines(kTexturedLine, ee::Address(record), 1, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiLine, Verified, dm2, drawListedLine);

    // drawListedLine: ZGE2's drawGuiLine, the line's Draw: the same line
    // from the ZGE2 record (the far end's depth from the position's y, as
    // ZGE3's).
    int32_t drawListedLine(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<LineRecord> record;
        const ee::Ptr<ListedLine> line = messageRecord<ListedLine>(message);
        if (line->material && !line->asset)
            line->asset = findGuiAsset(line->material, kAssetTexture);

        const zge::Vec4 &at = line->widget.position;
        const ee::Float halfPixel(-0.5f);
        LineVertex &start = record->vertices[0];
        LineVertex &end = record->vertices[1];
        start.x = at.x + halfPixel;
        start.y = at.y + halfPixel;
        start.z = at.z;
        start.u = ee::Float(0.0f);
        start.v = ee::Float(0.0f);
        end.x = at.x + line->vector.x + halfPixel;
        end.y = at.y + line->vector.y + halfPixel;
        end.z = at.y + line->vector.z;
        end.u = ee::Float(1.0f);
        end.v = ee::Float(1.0f);
        const uint8_t *const startColor = &line->startColor.r, *const endColor = &line->endColor.r;
        for (int channel = 0; channel < 3; ++channel)
        {
            start.colour[channel] = faded(startColor[channel]);
            end.colour[channel] = faded(endColor[channel]);
        }
        start.colour[3] = startColor[3];
        end.colour[3] = endColor[3];
        record->first = &start;
        record->second = &end;
        record->material = line->asset ? line->asset->object : g_zgui->defaultTexture;
        drawLines(kTexturedLine, ee::Address(record), 1, 0);
        return 1;
    }

    // defineGuiLineProperties: a line's defaults (white, 10 long) and properties.
    ee::Ptr<GuiLine> defineGuiLineProperties(ee::Ptr<GuiLine> line, uint32_t text)
    {
        defineWidgetProperties(line, text);
        line->asset = {};
        line->material = 0;
        setWhite(line->startColor);
        setWhite(line->endColor);
        line->vector.x = ee::Float(10.0f);
        line->vector.y = ee::Float(0.0f);
        line->vector.z = ee::Float(0.0f);
        line->width = ee::Float(1.0f);
        addListener(line, Message::Draw, sym::drawGuiLine);
        addListener(line, Message::Refresh, sym::refreshGuiLine);
        defineClassProperties(line, line->properties, kLineProperties);
        return line;
    }

    // defineListedLine: ZGE2's defineGuiLineProperties: white, 10 long; its
    // Draw, Refresh and GetBounds.
    uint32_t defineListedLine(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedLine> line(address);
        defineWidgetRecord(address, name);
        line->asset = {};
        line->material = 0;
        line->vector.y = ee::Float(0.0f);
        line->vector.z = ee::Float(0.0f);
        line->width = ee::Float(1.0f);
        line->vector.x = ee::Float(10.0f);
        line->startColor = kWhite;
        line->endColor = kWhite;
        subscribeWidget(object, Message::Draw, sym::drawGuiLine);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiLine);
        subscribeWidget(object, Message::GetBounds, sym::dm2::lineBounds);
        addListedProperties(line, kListedLineProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiLineProperties, Verified, dm2, defineListedLine);

    // lineBounds (ZGE2 only): the line's GetBounds: from its start (the
    // widget's position) to its far end (plus its vector), 2 wider on every
    // side. Left and top stay the start's side: a line drawn to the left or
    // up has them greater than right and bottom. 1.
    inline int32_t lineBounds(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedLine> line = messageRecord<ListedLine>(message);
        zge::TextBounds &bounds = messageBounds(message);
        const zge::Vec4 &start = line->widget.position;
        const ee::Float margin(2.0f), zero(0.0f);
        if (zero < line->vector.x)
        {
            bounds.left = start.x - margin;
            bounds.right = start.x + line->vector.x + margin;
        }
        else
        {
            bounds.left = start.x + margin;
            bounds.right = start.x + line->vector.x - margin;
        }
        if (zero < line->vector.y)
        {
            bounds.top = start.y - margin;
            bounds.bottom = start.y + line->vector.y + margin;
        }
        else
        {
            bounds.top = start.y + margin;
            bounds.bottom = start.y + line->vector.y - margin;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, lineBounds);

    namespace
    {
        void drawWrappedText(uint32_t font, ee::Ptr<zge::TextStyle> style, uint32_t text, int32_t firstLine, ee::Ptr<int32_t> lineCount, uint32_t draw, ee::Float maxWidth, ee::Float maxHeight) { ee::game(sym::drawWrappedText, font, style, text, firstLine, lineCount, draw, maxWidth, maxHeight); }
    }

    // refreshGuiTextBox: the text box's Refresh: its font found, its palette
    // colour looked up, and the text laid out for its line count: drawn with
    // no output, into lineCount.
    uint32_t refreshGuiTextBox(ee::Ptr<GuiText> box)
    {
        ee::Local<zge::TextStyle> style;
        box->asset = findGuiAsset(box->charset, kAssetFont);
        lookUpPaletteColour(box->color);
        if (box->asset)
        {
            const ee::Ptr<UiAsset> font = box->asset;
            fillTextStyle(style, box);
            const uint32_t text = expandText(ee::Ptr<const char>(box->text));
            drawWrappedText(font->object, style, text, box->lineOffset, &box->lineCount, 0, box->width, box->height);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(refreshGuiTextBox, Verified);

    // defineGuiTextBoxProperties: a text box's defaults (white, 128 square) and properties.
    ee::Ptr<GuiTextBox> defineGuiTextBoxProperties(ee::Ptr<GuiTextBox> box, uint32_t text)
    {
        defineWidgetProperties(box, text);
        setTextDefaults(*box);
        box->angle = 0;
        box->lineCount = 0;
        box->lineOffset = 0;
        box->width = ee::Float(128.0f);
        box->height = ee::Float(128.0f);
        setUnitScale(box->scale);
        addListener(box, Message::Draw, kDrawTextWidget);
        addListener(box, Message::Refresh, sym::refreshGuiTextBox);
        defineClassProperties(box, box->properties, kTextBoxProperties);
        addVariableNode(box, ee::Ptr<const char>(sym::g_textLineCount), ee::Ptr<const char>(sym::g_textSint32), &box->lineCount);
        return box;
    }
    READABLE_FUNCTION(defineGuiTextBoxProperties, Verified);

    // refreshGuiLabel: the label's Refresh: its font found, its palette colour
    // looked up, its size measured.
    uint32_t refreshGuiLabel(ee::Ptr<GuiLabel> label)
    {
        ee::Local<zge::TextBounds> bounds;
        ee::Local<zge::TextStyle> style;
        label->asset = findGuiAsset(label->charset, kAssetFont);
        lookUpPaletteColour(label->color);
        if (label->asset)
        {
            const ee::Ptr<UiAsset> font = label->asset;
            fillTextStyle(style, label);
            const uint32_t text = expandText(ee::Ptr<const char>(label->text));
            zge::measureText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(text), bounds, ee::Ptr<int32_t>());
            label->width = bounds->right - bounds->left;
            label->height = bounds->bottom - bounds->top;
        }
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiLabel, Verified, dm2, refreshLabelFont);

    // refreshLabelFont: ZGE2's refreshGuiLabel: the font found again (ZGE2's
    // colour has no palette name, and its size is measured when asked for:
    // labelBounds). 1.
    int32_t refreshLabelFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedLabel> label = messageRecord<ListedLabel>(message);
        label->font = findGuiAsset(label->charset, kAssetFont);
        return 1;
    }

    // defineGuiLabelProperties: a label's defaults and properties.
    ee::Ptr<GuiLabel> defineGuiLabelProperties(ee::Ptr<GuiLabel> label, uint32_t text)
    {
        defineWidgetProperties(label, text);
        setTextDefaults(*label);
        label->angle = 0;
        label->width = 0;
        label->height = 0;
        setUnitScale(label->scale);
        addListener(label, Message::Draw, kDrawLabel);
        addListener(label, Message::Refresh, sym::refreshGuiLabel);
        defineClassProperties(label, labelPropertyNodes(label), kLabelProperties);
        addVariableNode(label, ee::Ptr<const char>(sym::g_textWidth), ee::Ptr<const char>(sym::g_textZFloat), &label->width);
        addVariableNode(label, ee::Ptr<const char>(sym::g_textHeight), ee::Ptr<const char>(sym::g_textZFloat), &label->height);
        return label;
    }

    // defineListedLabel: ZGE2's defineGuiLabelProperties: its Draw, its
    // Refresh (the font) and its GetBounds (the text measured), its
    // properties; white, spacing 1.25, tracking 1.
    uint32_t defineListedLabel(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedLabel> label(address);
        defineWidgetRecord(address, name);
        label->text = 0;
        label->alignment = 0;
        label->font = {};
        label->charset = 0;
        label->spacing = ee::Float(1.25f);
        label->tracking = 1;
        label->color = kWhite;
        subscribeWidget(object, Message::Draw, kDrawLabel);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiLabel);
        subscribeWidget(object, Message::GetBounds, sym::dm2::labelBounds);
        addListedProperties(label, kListedLabelProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiLabelProperties, Verified, dm2, defineListedLabel);

    // refreshGuiImage: the image's Refresh: its texture and four corner colours.
    uint32_t refreshGuiImage(ee::Ptr<GuiImage> image)
    {
        image->asset = findGuiAsset(image->material, kAssetTexture);
        lookUpPaletteColour(image->topLeftColor);
        lookUpPaletteColour(image->bottomLeftColor);
        lookUpPaletteColour(image->topRightColor);
        lookUpPaletteColour(image->bottomRightColor);
        return 1;
    }

    // refreshImageTexture: ZGE2's refreshGuiImage: its texture found again. 1.
    int32_t refreshImageTexture(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedImage> image = messageRecord<ListedImage>(message);
        image->asset = findGuiAsset(image->material, kAssetTexture);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiImage, Verified, dm2, refreshImageTexture);

    // defineGuiImageProperties: an image's defaults (white, 128 square) and properties.
    ee::Ptr<GuiImage> defineGuiImageProperties(ee::Ptr<GuiImage> image, uint32_t text)
    {
        defineWidgetProperties(image, text);
        image->unused = 0;
        setWhite(image->topLeftColor);
        setWhite(image->bottomLeftColor);
        setWhite(image->topRightColor);
        setWhite(image->bottomRightColor);
        image->alignment = 0;
        image->asset = {};
        image->material = 0;
        image->width = ee::Float(128.0f);
        image->height = ee::Float(128.0f);
        image->uScale = ee::Float(1.0f);
        image->vScale = ee::Float(1.0f);
        setUnitScale(image->scale);
        addListener(image, Message::Draw, kDrawImage);
        addListener(image, Message::Refresh, sym::refreshGuiImage);
        defineClassProperties(image, image->properties, kImageProperties);
        return image;
    }

    // defineListedImage: ZGE2's defineGuiImageProperties: white, no size
    // (ZGE3's is 128 square), its Draw, Refresh and GetBounds.
    uint32_t defineListedImage(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedImage> image(address);
        defineWidgetRecord(address, name);
        image->unused = 0;
        image->alignment = 0;
        image->width = ee::Float(0.0f);
        image->height = ee::Float(0.0f);
        image->asset = {};
        image->material = 0;
        image->vScale = ee::Float(1.0f);
        image->uScale = ee::Float(1.0f);
        image->topLeftColor = kWhite;
        image->topRightColor = kWhite;
        image->bottomLeftColor = kWhite;
        image->bottomRightColor = kWhite;
        subscribeWidget(object, Message::Draw, kDrawImage);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiImage);
        subscribeWidget(object, Message::GetBounds, sym::dm2::imageBounds);
        addListedProperties(image, kListedImageProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiImageProperties, Verified, dm2, defineListedImage);

    // imageBounds (ZGE2 only): the image's GetBounds: Width by Height where
    // its Alignment anchors it, sized by its orientation and Scale, at its
    // position. 1.
    inline int32_t imageBounds(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Matrix> matrix;
        const ee::Ptr<ListedImage> image = messageRecord<ListedImage>(message);
        zge::TextBounds &bounds = messageBounds(message);
        alignBounds(bounds, image->alignment, image->width, image->height);
        widgetMatrix(matrix, image->widget);
        placeBounds(bounds, *matrix, image->widget.position);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, imageBounds);

    // refreshGuiFrame: the frame's Refresh: its nine parts' textures, its colour.
    uint32_t refreshGuiFrame(ee::Ptr<GuiFrame> frameWidget)
    {
        for (uint32_t part = 0; part < kFrameParts; ++part)
            frameWidget->assets[part] = findGuiAsset(frameWidget->materials[part], kAssetTexture);
        lookUpPaletteColour(frameWidget->color);
        return 1;
    }

    // refreshFrameTextures: ZGE2's refreshGuiFrame: its nine parts' textures
    // found again. 1.
    int32_t refreshFrameTextures(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedFrame> frameWidget = messageRecord<ListedFrame>(message);
        for (uint32_t part = 0; part < kFrameParts; ++part)
            frameWidget->assets[part] = findGuiAsset(frameWidget->materials[part], kAssetTexture);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiFrame, Verified, dm2, refreshFrameTextures);

    // defineGuiFrameProperties: a frame's defaults (white, 128 square) and properties.
    ee::Ptr<GuiFrame> defineGuiFrameProperties(ee::Ptr<GuiFrame> frameWidget, uint32_t text)
    {
        defineWidgetProperties(frameWidget, text);
        for (uint32_t part = 0; part < kFrameParts; ++part)
        {
            frameWidget->materials[part] = 0;
            frameWidget->assets[part] = {};
        }
        frameWidget->alignment = 0;
        setWhite(frameWidget->color);
        frameWidget->width = ee::Float(128.0f);
        frameWidget->height = ee::Float(128.0f);
        setUnitScale(frameWidget->scale);
        addListener(frameWidget, Message::Draw, kDrawFrame);
        addListener(frameWidget, Message::Refresh, sym::refreshGuiFrame);
        defineClassProperties(frameWidget, frameWidget->properties, kFrameProperties);
        return frameWidget;
    }

    // defineListedFrame: ZGE2's defineGuiFrameProperties: white, 128 square,
    // its Draw, Refresh and GetBounds.
    uint32_t defineListedFrame(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedFrame> frameWidget(address);
        defineWidgetRecord(address, name);
        for (uint32_t part = 0; part < kFrameParts; ++part)
        {
            frameWidget->materials[part] = 0;
            frameWidget->assets[part] = {};
        }
        frameWidget->alignment = 0;
        frameWidget->color = kWhite;
        frameWidget->width = ee::Float(128.0f);
        frameWidget->height = ee::Float(128.0f);
        subscribeWidget(object, Message::Draw, kDrawFrame);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiFrame);
        subscribeWidget(object, Message::GetBounds, sym::dm2::frameBounds);
        addListedProperties(frameWidget, kListedFrameProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiFrameProperties, Verified, dm2, defineListedFrame);

    // frameBounds (ZGE2 only): the frame's GetBounds, as the image's: Width
    // by Height where its Alignment anchors it, sized by its orientation and
    // Scale, at its position. 1.
    inline int32_t frameBounds(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Matrix> matrix;
        const ee::Ptr<ListedFrame> frameWidget = messageRecord<ListedFrame>(message);
        zge::TextBounds &bounds = messageBounds(message);
        alignBounds(bounds, frameWidget->alignment, frameWidget->width, frameWidget->height);
        widgetMatrix(matrix, frameWidget->widget);
        placeBounds(bounds, *matrix, frameWidget->widget.position);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, frameBounds);

    // The clipper: a widget whose children draw inside its rectangle (its
    // screen position, Width by Height). Its Draw swaps the camera's viewport
    // for that rectangle, keeping the old one, and its DrawAfter puts the old
    // one back. No class table of AI's registers it (dead here); Dave Mirra 2
    // and BMX XXX have the same three functions.
    namespace
    {
        // The type the clipper's Width and Height properties name: ZGE3's Sint32, ZGE4's ZFloat.
        constexpr uint32_t kClipperSizeType = game::readable::perGame(sym::g_textSint32, sym::g_textSint32, sym::g_textZFloat);

        ee::Ptr<zge::Camera> currentCamera() { return ee::Ptr<zge::Camera>(*zge::g_currentCamera); }
    }

    namespace
    {
        // ZGE2's (DM2's) clipper record, after WidgetRecord.
        struct ListedClipper
        {
            WidgetRecord widget;
            int32_t width;               // +0x60 "Width" (Sint32)
            int32_t height;              // +0x64 "Height" (Sint32)
            ScreenRect previousViewport; // +0x68 the camera's before the Draw (DM2's drawGuiClipper)
        };
        static_assert(offsetof(ListedClipper, width) == 0x60 && offsetof(ListedClipper, previousViewport) == 0x68);

        constexpr ListedProperty kListedClipperProperties[] = {
            {sym::g_textWidth, sym::g_textSint32, offsetof(ListedClipper, width)},
            {sym::g_textHeight, sym::g_textSint32, offsetof(ListedClipper, height)},
        };
    }

    // drawGuiClipper: the clipper's Draw: the camera's viewport is its
    // rectangle until the DrawAfter; the old one kept. 1.
    uint32_t drawGuiClipper(ee::Ptr<GuiClipper> clipper)
    {
        ee::Local<ScreenRect> clip;
        clip->x = clipper->widget.node.screenPosition.x;
        clip->y = clipper->widget.node.screenPosition.y;
        clip->width = clipWidth(clipper);
        clip->height = clipHeight(clipper);
        const ee::Ptr<ScreenRect> previous(zge::swapCameraViewport(currentCamera(), clip));
        clipper->previousViewport = *previous;
        return 1;
    }

    // clipListedChildren: ZGE2's drawGuiClipper: the same over its record,
    // the rectangle from the widget's own position. 1.
    int32_t clipListedChildren(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ScreenRect> clip;
        const ee::Ptr<ListedClipper> clipper = messageRecord<ListedClipper>(message);
        clip->x = clipper->widget.position.x;
        clip->y = clipper->widget.position.y;
        clip->width = ee::toFloat(clipper->width);
        clip->height = ee::toFloat(clipper->height);
        const ee::Ptr<ScreenRect> previous(zge::swapCameraViewport(currentCamera(), clip));
        clipper->previousViewport = *previous;
        return 1;
    }
    READABLE_FUNCTION_OWN(drawGuiClipper, Draft, dm2, clipListedChildren);

    // drawAfterGuiClipper: its DrawAfter, once its children are drawn: the
    // old viewport back. 1.
    uint32_t drawAfterGuiClipper(ee::Ptr<GuiClipper> clipper)
    {
        zge::swapCameraViewport(currentCamera(), &clipper->previousViewport);
        return 1;
    }

    // unclipListedChildren: ZGE2's drawAfterGuiClipper. 1.
    int32_t unclipListedChildren(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedClipper> clipper = messageRecord<ListedClipper>(message);
        zge::swapCameraViewport(currentCamera(), &clipper->previousViewport);
        return 1;
    }
    READABLE_FUNCTION_OWN(drawAfterGuiClipper, Draft, dm2, unclipListedChildren);

    // defineGuiClipperProperties: a clipper's defaults (64 square), its
    // properties and its two draw listeners. Width and Height are both
    // defined in the one property node: Height's definition writes over
    // Width's (the game's, kept as it is).
    ee::Ptr<GuiClipper> defineGuiClipperProperties(ee::Ptr<GuiClipper> clipper, uint32_t name)
    {
        defineWidgetProperties(clipper, name);
        clipper->height = 64;
        clipper->width = 64;
        const ee::Ptr<const char> sizeType(kClipperSizeType);
        defineProperty(&clipper->property, clipper, ee::Ptr<const char>(sym::g_textWidth), sizeType, &clipper->width);
        defineProperty(&clipper->property, clipper, ee::Ptr<const char>(sym::g_textHeight), sizeType, &clipper->height);
        addListener(clipper, Message::Draw, sym::drawGuiClipper);
        addListener(clipper, Message::DrawAfter, sym::drawAfterGuiClipper);
        return clipper;
    }

    // defineListedClipper: ZGE2's defineGuiClipperProperties: 64 square, each
    // property listed over its own field, its Draw and DrawAfter.
    uint32_t defineListedClipper(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedClipper> clipper(address);
        defineWidgetRecord(address, name);
        clipper->height = 64;
        clipper->width = 64;
        addListedProperties(clipper, kListedClipperProperties);
        subscribeWidget(object, Message::Draw, sym::drawGuiClipper);
        subscribeWidget(object, Message::DrawAfter, sym::drawAfterGuiClipper);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiClipperProperties, Draft, dm2, defineListedClipper);
}
