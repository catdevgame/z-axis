// The small widgets: a button, the idle function (a script after so many
// frames with no input) and the tape (a ruler that scrolls by a value, as the
// bike tapes' speed and height), and the table of widget kinds.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/text/font.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/widgets_small.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kFrameStateHigh = ee::high(sym::g_zgui);
        inline ee::Global<uint32_t, sym::g_idleFrames> g_idleFrames;   // frames since the last input
        inline ee::Global<uint32_t, sym::g_uiDefaultMaterial> g_uiDefaultMaterial;

        struct GuiButton
        {
            Widget widget;
            uint32_t isDisabled;   // +0x0F0
            uint32_t onGetFocus;   // +0x0F4
            uint32_t onLostFocus;  // +0x0F8
            uint32_t onSelect;     // +0x0FC
            uint32_t onCancel;     // +0x100
            uint32_t onStart;      // +0x104
        };
        static_assert(offsetof(GuiButton, onStart) == 0x104);

        struct GuiIdleFunc
        {
            Widget widget;
            uint32_t activationFrame;    // +0x0F0
            uint32_t activateOnce;       // +0x0F4
            uint32_t activated;          // +0x0F8
            uint32_t onActivationScript; // +0x0FC
        };
        static_assert(sizeof(GuiIdleFunc) == 0x100);

        // The tape's Width and Height as floats: ZGE3's Sint32s read as
        // unsigned, ZGE4's the widget's ZFloats; and the type they are.
        [[gnu::always_inline]] inline ee::Float sizeAsFloat(uint32_t size) { return unsignedToFloat(size); }
        [[gnu::always_inline]] inline ee::Float sizeAsFloat(ee::Float size) { return size; }
        constexpr uint32_t kTapeSizeType = game::readable::perGame(sym::g_textSint32, sym::g_textSint32, sym::g_textZFloat);

        constexpr uint32_t kUiAssetFontObject = 0xB4;

        // ZGE2's (DM2's) records of these kinds, after WidgetRecord (widget_parts.h), and their properties.
        struct ListedButton
        {
            WidgetRecord widget;
            uint32_t isDisabled;   // +0x60
            uint32_t onGetFocus;   // +0x64 each a script (Event)
            uint32_t onLostFocus;  // +0x68
            uint32_t onSelect;     // +0x6C
            uint32_t onCancel;     // +0x70
            uint32_t onStart;      // +0x74
        };
        static_assert(offsetof(ListedButton, isDisabled) == 0x60 && offsetof(ListedButton, onStart) == 0x74);

        struct ListedIdleFunc
        {
            WidgetRecord widget;
            uint32_t activationFrame;    // +0x60
            uint32_t activateOnce;       // +0x64
            uint32_t activated;          // +0x68
            uint32_t onActivationScript; // +0x6C
        };
        static_assert(offsetof(ListedIdleFunc, activationFrame) == 0x60 && offsetof(ListedIdleFunc, onActivationScript) == 0x6C);

        struct ListedTape
        {
            WidgetRecord widget;
            ee::Ptr<UiAsset> font;       // +0x60 found by charset (refreshTapeFont)
            ee::Float distance;          // +0x64 the value at the middle
            ee::Float lineHeight;        // +0x68
            ee::Float lineWidth;         // +0x6C
            ee::Float lineWeight;        // +0x70
            Rgba lineColor;              // +0x74
            uint32_t textRatio;          // +0x78 a label every so many lines
            ee::Float textDistanceValue; // +0x7C the value between labels
            Rgba textColor;              // +0x80
            Rgba sepColor;               // +0x84
            ee::Float textOffsetX;       // +0x88
            ee::Float textOffsetY;       // +0x8C
            zge::Vec4 textScale;         // +0x90
            ee::Float middleRatio;       // +0xA0
            uint32_t width;              // +0xA4
            uint32_t height;             // +0xA8
            uint32_t textFormat;         // +0xAC
            uint32_t charset;            // +0xB0
        };
        static_assert(offsetof(ListedTape, lineColor) == 0x74 && offsetof(ListedTape, textScale) == 0x90);
        static_assert(offsetof(ListedTape, middleRatio) == 0xA0 && offsetof(ListedTape, charset) == 0xB0);

        constexpr ListedProperty kListedButtonProperties[] = {
            {sym::g_textIsDisabled, sym::g_textBool, offsetof(ListedButton, isDisabled)},
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(ListedButton, onGetFocus)},
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(ListedButton, onLostFocus)},
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(ListedButton, onSelect)},
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(ListedButton, onCancel)},
            {sym::g_textOnStart, sym::g_textEvent, offsetof(ListedButton, onStart)},
        };

        constexpr ListedProperty kListedIdleFuncProperties[] = {
            {sym::g_textActivationFrame, sym::g_textSint32, offsetof(ListedIdleFunc, activationFrame)},
            {sym::g_textActivateOnce, sym::g_textBool, offsetof(ListedIdleFunc, activateOnce)},
            {sym::g_textOnActivationScript, sym::g_textEvent, offsetof(ListedIdleFunc, onActivationScript)},
        };

        constexpr ListedProperty kListedTapeProperties[] = {
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(ListedTape, charset)},
            {sym::g_textDistance, sym::g_textZFloat, offsetof(ListedTape, distance)},
            {sym::g_textLineWidth, sym::g_textZFloat, offsetof(ListedTape, lineWidth)},
            {sym::g_textLineHeight, sym::g_textZFloat, offsetof(ListedTape, lineHeight)},
            {sym::g_textLineWeight, sym::g_textZFloat, offsetof(ListedTape, lineWeight)},
            {sym::g_textLineColor, sym::dm2::g_textZrgba, offsetof(ListedTape, lineColor)},
            {sym::g_textTextRatio, sym::g_textSint32, offsetof(ListedTape, textRatio)},
            {sym::g_textTextDistanceValue, sym::g_textZFloat, offsetof(ListedTape, textDistanceValue)},
            {sym::g_textSepColor, sym::dm2::g_textZrgba, offsetof(ListedTape, sepColor)},
            {sym::g_textTextColor, sym::dm2::g_textZrgba, offsetof(ListedTape, textColor)},
            {sym::g_textTextScale, sym::g_textZ3DVector, offsetof(ListedTape, textScale)},
            {sym::g_textTextOffsetX, sym::g_textZFloat, offsetof(ListedTape, textOffsetX)},
            {sym::g_textTextOffsetY, sym::g_textZFloat, offsetof(ListedTape, textOffsetY)},
            {sym::g_textTextFormat, sym::g_textString, offsetof(ListedTape, textFormat)},
            {sym::g_textMiddleRatio, sym::g_textZFloat, offsetof(ListedTape, middleRatio)},
            {sym::g_textWidth, sym::g_textSint32, offsetof(ListedTape, width)},
            {sym::g_textHeight, sym::g_textSint32, offsetof(ListedTape, height)},
        };


        constexpr PropertyStep kButtonProperties[] = {
            {Property, 0x108, sym::g_textIsDisabled, sym::g_textBool, offsetof(GuiButton, isDisabled)}, // "IsDisabled" Bool
            {Property, 0x128, sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiButton, onGetFocus)}, // "OnGetFocus" Event
            {Property, 0x148, sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiButton, onLostFocus)}, // "OnLostFocus" Event
            {Property, 0x168, sym::g_textOnSelect, sym::g_textEvent, offsetof(GuiButton, onSelect)}, // "OnSelect" Event
            {Property, 0x188, sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiButton, onCancel)}, // "OnCancel" Event
            {Property, 0x1A8, sym::g_textOnStart, sym::g_textEvent, offsetof(GuiButton, onStart)}, // "OnStart" Event
        };

        constexpr PropertyStep kIdleFuncProperties[] = {
            {Typed, 0x0, sym::g_textActivationFrame, sym::g_textSint32, offsetof(GuiIdleFunc, activationFrame)}, // "ActivationFrame" Sint32
            {Typed, 0x0, sym::g_textActivateOnce, sym::g_textBool, offsetof(GuiIdleFunc, activateOnce)}, // "ActivateOnce" Bool
            {Typed, 0x0, sym::g_textOnActivationScript, sym::g_textEvent, offsetof(GuiIdleFunc, onActivationScript)}, // "OnActivationScript" Event
        };

        constexpr PropertyStep kTapeProperties[] = {
            {Typed, 0x0, sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiTape, scale)}, // "Scale" Z3DVector
            {Typed, 0x0, sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiTape, charset)}, // "Charset" ZAssetID
            {Typed, 0x0, sym::g_textDistance, sym::g_textZFloat, offsetof(GuiTape, distance)}, // "Distance" ZFloat
            {Typed, 0x0, sym::g_textLineWidth, sym::g_textZFloat, offsetof(GuiTape, lineWidth)}, // "LineWidth" ZFloat
            {Typed, 0x0, sym::g_textLineHeight, sym::g_textZFloat, offsetof(GuiTape, lineHeight)}, // "LineHeight" ZFloat
            {Typed, 0x0, sym::g_textLineWeight, sym::g_textZFloat, offsetof(GuiTape, lineWeight)}, // "LineWeight" ZFloat
            {Typed, 0x0, sym::g_textLineColor, sym::g_textZrgbaPal, offsetof(GuiTape, lineColor)}, // "LineColor" ZRGBA_PAL
            {Typed, 0x0, sym::g_textTextRatio, sym::g_textSint32, offsetof(GuiTape, textRatio)}, // "TextRatio" Sint32
            {Typed, 0x0, sym::g_textTextDistanceValue, sym::g_textZFloat, offsetof(GuiTape, textDistanceValue)}, // "TextDistanceValue" ZFloat
            {Typed, 0x0, sym::g_textSepColor, sym::g_textZrgbaPal, offsetof(GuiTape, sepColor)}, // "SepColor" ZRGBA_PAL
            {Typed, 0x0, sym::g_textTextColor, sym::g_textZrgbaPal, offsetof(GuiTape, textColor)}, // "TextColor" ZRGBA_PAL
            {Typed, 0x0, sym::g_textTextScale, sym::g_textZ3DVector, offsetof(GuiTape, textScale)}, // "TextScale" Z3DVector
            {Typed, 0x0, sym::g_textTextOffsetX, sym::g_textZFloat, offsetof(GuiTape, textOffsetX)}, // "TextOffsetX" ZFloat
            {Typed, 0x0, sym::g_textTextOffsetY, sym::g_textZFloat, offsetof(GuiTape, textOffsetY)}, // "TextOffsetY" ZFloat
            {Typed, 0x0, sym::g_textTextFormat, sym::g_textString, offsetof(GuiTape, textFormat)}, // "TextFormat" String
            {Typed, 0x0, sym::g_textMiddleRatio, sym::g_textZFloat, offsetof(GuiTape, middleRatio)}, // "MiddleRatio" ZFloat
            {Typed, 0x0, sym::g_textWidth, kTapeSizeType, offsetof(GuiTape, width)}, // "Width" Sint32 (ZGE4 ZFloat)
            {Typed, 0x0, sym::g_textHeight, kTapeSizeType, offsetof(GuiTape, height)}, // "Height" Sint32 (ZGE4 ZFloat)
        };
    }

    // handleGuiButtonInput: the accept, cancel and start buttons run the button's
    // scripts on their press, with the menu's sounds.
    uint32_t handleGuiButtonInput(uint32_t node, ee::Ptr<Input> input)
    {
        const ee::Ptr<GuiButton> button(node);
        const uint32_t changed = input->pressed, held = input->down;
        const auto script = [&](uint32_t text) {
            runScript(ee::Ptr<Node>(node), ee::Ptr<char>(text), 0);
        };
        const uint32_t accept = g_zgui->acceptButton;
        bool reachesMask = false; // the paths that join
        if ((changed & accept) && (held & accept))
        {
            if (const uint32_t onSelect = button->onSelect)
            {
                script(onSelect);
                if (const SoundHook sound = g_zgui->selectSound)
                    sound();
                else
                    reachesMask = true;
            }
        }
        else
        {
            const uint32_t cancel = g_zgui->cancelButton;
            bool next = true;
            if (changed & cancel)
            {
                if (!(held & cancel))
                    next = false;
                else if (const uint32_t onCancel = button->onCancel)
                {
                    script(onCancel);
                    if (const SoundHook sound = g_zgui->cancelSound)
                        sound();
                    else
                        next = false;
                }
            }
            reachesMask = true;
        }
        const uint32_t start = g_zgui->startButton;
        if (changed & start)
        {
            const uint32_t onStart = button->onStart;
            if ((held & start) == 0 || onStart == 0)
                return 1;
            script(onStart);
            const SoundHook sound = g_zgui->selectSound;
            if (!sound)
                return 1;
            sound();
        }
        return 1;
    }

    // A ZGE2 widget's script run for it (its scene object), then the sound
    // hook as it is after the script, if set.
    inline void runWidgetScript(ee::Address widget, uint32_t script, SoundHook Zgui::*sound)
    {
        runScriptLines(ee::Ptr<Node>(widget), ee::Ptr<char>(script));
        if (const SoundHook hook = (*g_zgui).*sound)
            hook();
    }

    // pressListedButton: ZGE2's handleGuiButtonInput: an accept press runs
    // OnSelect (the select sound), else a cancel press OnCancel (the cancel
    // sound); then a start press runs OnStart (the select sound). A press
    // counts while its button is held. 1.
    int32_t pressListedButton(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedButton> button = messageRecord<ListedButton>(message);
        const ee::Ptr<Input> input(message->data);
        const uint32_t pressed = input->pressed, held = input->down;
        const auto press = [&](uint32_t mask) { return (pressed & mask) && (held & mask); };
        if (press(g_zgui->acceptButton))
        {
            if (const uint32_t onSelect = button->onSelect)
                runWidgetScript(message->target, onSelect, &Zgui::selectSound);
        }
        else if (press(g_zgui->cancelButton))
        {
            if (const uint32_t onCancel = button->onCancel)
                runWidgetScript(message->target, onCancel, &Zgui::cancelSound);
        }
        if (press(g_zgui->startButton))
        {
            if (const uint32_t onStart = button->onStart)
                runWidgetScript(message->target, onStart, &Zgui::selectSound);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleGuiButtonInput, Verified, dm2, pressListedButton);

    // defineGuiButtonProperties: a button's handlers, properties and defaults.
    uint32_t defineGuiButtonProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiButton> button(node);
        defineWidgetProperties(node, text);
        addListener(node, Message::Input, sym::handleGuiButtonInput);
        addListener(node, Message::GetFocus, kRunOnGetFocus);
        addListener(node, Message::LostFocus, kRunOnLostFocus);
        definePropertySteps(node, kButtonProperties);
        button->isDisabled = 0;
        button->onCancel = 0;
        button->onLostFocus = 0;
        button->onGetFocus = 0;
        button->onSelect = 0;
        button->onStart = 0;
        return node;
    }

    // defineListedButton: ZGE2's defineGuiButtonProperties: its handlers on
    // the scene object, its properties, no scripts.
    uint32_t defineListedButton(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedButton> button(address);
        defineWidgetRecord(address, name);
        subscribeWidget(object, Message::Input, sym::handleGuiButtonInput);
        subscribeWidget(object, Message::GetFocus, kRunOnGetFocus);
        subscribeWidget(object, Message::LostFocus, kRunOnLostFocus);
        addListedProperties(button, kListedButtonProperties);
        button->isDisabled = 0;
        button->onCancel = 0;
        button->onLostFocus = 0;
        button->onGetFocus = 0;
        button->onSelect = 0;
        button->onStart = 0;
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiButtonProperties, Verified, dm2, defineListedButton);

    // drawGuiIdleFunc: after ActivationFrame frames with no input the script
    // runs, each frame or once.
    uint32_t drawGuiIdleFunc(uint32_t node)
    {
        const ee::Ptr<GuiIdleFunc> idle(node);
        if (idle->activationFrame < *g_idleFrames)
        {
            const uint32_t script = idle->onActivationScript;
            if (idle->activated || script == 0)
                return 1;
            runScript(ee::Ptr<Node>(node), ee::Ptr<char>(script), 0);
            if (idle->activateOnce)
                idle->activated = 1;
        }
        return 1;
    }

    // runIdleScript: ZGE2's drawGuiIdleFunc, the same steps over its record
    // (the script runs for the widget's scene object). 1.
    int32_t runIdleScript(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedIdleFunc> idle = messageRecord<ListedIdleFunc>(message);
        if (idle->activationFrame < *g_idleFrames)
        {
            const uint32_t script = idle->onActivationScript;
            if (idle->activated || script == 0)
                return 1;
            runScriptLines(ee::Ptr<Node>(message->target), ee::Ptr<char>(script));
            if (idle->activateOnce)
                idle->activated = 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiIdleFunc, Verified, dm2, runIdleScript);

    // defineGuiIdleFuncProperties.
    uint32_t defineGuiIdleFuncProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiIdleFunc> idle(node);
        defineWidgetProperties(node, text);
        addListener(node, Message::Draw, sym::drawGuiIdleFunc);
        idle->onActivationScript = 0;
        idle->activated = 0;
        idle->activationFrame = 1800;
        idle->activateOnce = 1;
        definePropertySteps(node, kIdleFuncProperties);
        return node;
    }

    // defineListedIdleFunc: ZGE2's defineGuiIdleFuncProperties.
    uint32_t defineListedIdleFunc(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedIdleFunc> idle(address);
        defineWidgetRecord(address, name);
        subscribeWidget(object, Message::Draw, sym::drawGuiIdleFunc);
        idle->onActivationScript = 0;
        idle->activated = 0;
        idle->activationFrame = 1800;
        idle->activateOnce = 1;
        addListedProperties(idle, kListedIdleFuncProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiIdleFuncProperties, Verified, dm2, defineListedIdleFunc);

    // refreshGuiTape: the font again, and palette colours looked up.
    uint32_t refreshGuiTape(uint32_t node)
    {
        const ee::Ptr<GuiTape> tape(node);
        tape->uiAsset = findGuiAsset(tape->charset, kAssetFont);
        if (const uint64_t name = tape->lineColor.paletteName)
            findPaletteColour(name, ee::Ptr<uint8_t>(node + offsetof(GuiTape, lineColor)));
        const uint64_t textName = tape->textColor.paletteName;
        uint64_t sepName;
        if (textName)
        {
            findPaletteColour(textName, ee::Ptr<uint8_t>(node + offsetof(GuiTape, textColor)));
            sepName = tape->sepColor.paletteName;
        }
        else
            sepName = tape->sepColor.paletteName;
        if (sepName)
            findPaletteColour(sepName, ee::Ptr<uint8_t>(node + offsetof(GuiTape, sepColor)));
        return 1;
    }

    // refreshTapeFont: ZGE2's refreshGuiTape: the font found again (ZGE2's
    // colours have no palette names). 1.
    int32_t refreshTapeFont(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ListedTape> tape = messageRecord<ListedTape>(message);
        tape->font = findGuiAsset(tape->charset, kAssetFont);
        return 1;
    }
    READABLE_FUNCTION_OWN(refreshGuiTape, Verified, dm2, refreshTapeFont);

    namespace
    {
        // What drawPrimitiveQuads takes: a material and four vertices.
        struct TapeVertex
        {
            ee::Float x, y, z, _0c;
            ee::Float u, v;
            uint32_t colour, _1c;
        };
        static_assert(sizeof(TapeVertex) == 0x20);

        // What drawPrimitiveQuads takes: a material and its four corners
        // (in the order 0, 2, 3, 1 of the vertices).
        struct TapeQuad
        {
            uint32_t material;
            ee::Ptr<TapeVertex> corner[4];
            uint32_t _14[3];
        };
        static_assert(sizeof(TapeQuad) == 0x20);
        constexpr uint32_t kQuads = 0x02000004u;

        // One quad's corners: (0, 0), (0, 1), (1, 0) and (1, 1) in texture
        // space, all the colour's, the right side at `right`.
        void setQuad(TapeQuad &quad, TapeVertex *vertex, uint32_t material, const WidgetColour &colour, ee::Float right)
        {
            quad.material = material;
            quad.corner[0] = &vertex[0];
            quad.corner[1] = &vertex[2];
            quad.corner[2] = &vertex[3];
            quad.corner[3] = &vertex[1];
            for (uint32_t corner = 0; corner < 4; ++corner)
            {
                vertex[corner].x = corner >= 2 ? right : ee::Float(0.0f);
                vertex[corner].z = ee::Float(0.0f);
                vertex[corner].u = (corner & 2) ? ee::Float(1.0f) : ee::Float(0.0f);
                vertex[corner].v = (corner & 1) ? ee::Float(1.0f) : ee::Float(0.0f);
                std::memcpy(&vertex[corner].colour, colour.rgba, 4);
            }
        }

        // A quad's top at y and its bottom at `bottom`.
        void setRow(TapeVertex *vertex, ee::Float y, ee::Float bottom)
        {
            vertex[2].y = y;
            vertex[0].y = y;
            vertex[3].y = bottom;
            vertex[1].y = bottom;
        }
    }

    // drawGuiTape: a line for each step of LineHeight, every TextRatio-th
    // one longer and labelled, scrolled so that Distance sits at MiddleRatio.
    uint32_t drawGuiTape(uint32_t node)
    {
        ee::Local<TapeQuad> lineQuad;
        ee::Local<TapeQuad> labelQuad;
        ee::Local<TapeVertex[4]> line;
        ee::Local<TapeVertex[4]> label;
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::TextStyle> style;
        ee::Local<char[48]> text;
        const ee::Ptr<GuiTape> tape(node);
        const uint32_t name = tape->charset;
        if (name && !tape->uiAsset)
            tape->uiAsset = findGuiAsset(name, kAssetFont);
        const ee::Float heightFloat = sizeAsFloat(tape->height);
        const int32_t lines = sce::libgcc::fptoui(heightFloat / tape->lineHeight) - 1;
        const uint32_t material = *g_uiDefaultMaterial;
        setQuad(*lineQuad, &line[0], material, tape->lineColor, tape->lineWidth);
        setQuad(*labelQuad, &label[0], material, tape->sepColor, ee::Float(0.0f));
        const ee::Float widthFloat = sizeAsFloat(tape->width);
        label[2].x = widthFloat;
        label[3].x = widthFloat;
        zge::matrixIdentity(matrix);
        zge::matrixScale(matrix, matrix, &tape->scale.xyz());
        zge::matrixRow(*matrix, 3) = tape->widget.node.screenPosition;

        const uint32_t ratio = tape->textRatio;
        ee::Float step = tape->textDistanceValue / unsignedToFloat(ratio);
        ee::Float value = tape->distance + tape->middleRatio * ee::toFloat(lines + 1) * step;
        ee::Float steps;
        if (!(step == ee::Float(0.0f))) // a distance of nothing per line: one
            steps = value / step;
        else
        {
            step = ee::Float(1.0f);
            steps = step;
        }
        const int32_t wholeSteps = ee::toInt(steps);
        const ee::Float lineHeight = tape->lineHeight;
        style->letterSpacing = 1;
        style->align = 1;
        zge::clearStyleReservedByte(*style);
        style->lineSpacing = ee::Float::fromBits(0);
        style->position.z = tape->widget.node.screenPosition.z;
        std::memcpy(style->colour, tape->textColor.rgba, 4);
        std::memcpy(&style->scaleX, &tape->textScale, 8);        // TextScale's x and y
        std::memcpy(&style->scaleExtra, &tape->textScaleZW, 8);  // and its z and w
        style->monospaced = 0;
        style->angle = ee::Float::fromBits(0);
        const ee::Float offset = (value - ee::toFloat(wholeSteps) * step) / step * lineHeight;
        const uint32_t phase = ratio - static_cast<uint32_t>(wholeSteps) % ratio;
        if (lines <= 0)
            return 1;
        // The plain lines first, then the labelled ones over them.
        for (int32_t index = 0; index < lines; ++index)
        {
            if ((index + phase) % tape->textRatio == 0)
                continue;
            const ee::Float y = ee::toFloat(index) * tape->lineHeight + offset;
            setRow(&line[0], y, y + tape->lineWeight);
            zge::drawPrimitiveQuads(kQuads, lineQuad, 1, matrix);
        }
        for (int32_t index = 0; index < lines; ++index, value = value - step)
        {
            if ((index + phase) % tape->textRatio != 0)
                continue;
            const ee::Float y = ee::toFloat(index) * tape->lineHeight + offset;
            setRow(&label[0], y, y + tape->lineWeight);
            zge::drawPrimitiveQuads(kQuads, labelQuad, 1, matrix);
            const uint32_t format = tape->textFormat;
            const ee::Float textY = tape->textOffsetY + tape->widget.node.screenPosition.y + label[0].y;
            style->position.x = tape->textOffsetX + tape->widget.node.screenPosition.x;
            style->position.y = textY;
            if (format)
            {
                // The label: the value through TextFormat.
                const uint64_t asDouble = sce::libgcc::fptodp(value);
                zge::formatString(text, tape->textFormat, asDouble);
                if (const ee::Ptr<UiAsset> asset = tape->uiAsset)
                    drawText(ee::Ptr<zge::Font>(asset->object), style, ee::Ptr<const int8_t>(ee::Address(text)));
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawGuiTape, Verified, dm2, drawListedTape);

    namespace
    {
        // A ZGE2 colour as setQuad takes one.
        WidgetColour quadColour(Rgba colour)
        {
            WidgetColour out{};
            std::memcpy(out.rgba, &colour, sizeof(Rgba));
            return out;
        }
    }

    // drawListedTape: ZGE2's drawGuiTape, the tape's Draw: the same lines and
    // labels from the ZGE2 record, turned by the widget's orientation as well
    // as scaled, the labels' style with the orientation and all of TextScale.
    int32_t drawListedTape(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<TapeQuad> lineQuad;
        ee::Local<TapeQuad> labelQuad;
        ee::Local<TapeVertex[4]> line;
        ee::Local<TapeVertex[4]> label;
        ee::Local<zge::Matrix> matrix;
        ee::Local<ListedStyleRoom> styleRoom;
        ee::Local<char[48]> text;
        const ee::Ptr<zge::TextStyle> style(styleRoom.address());
        const ee::Ptr<ListedTape> tape = messageRecord<ListedTape>(message);
        if (tape->charset && !tape->font)
            tape->font = findGuiAsset(tape->charset, kAssetFont);
        const ee::Float heightFloat = unsignedToFloat(tape->height);
        const int32_t lines = sce::libgcc::fptoui(heightFloat / tape->lineHeight) - 1;
        const uint32_t material = *g_uiDefaultMaterial;
        setQuad(*lineQuad, &line[0], material, quadColour(tape->lineColor), tape->lineWidth);
        setQuad(*labelQuad, &label[0], material, quadColour(tape->sepColor), ee::Float(0.0f));
        const ee::Float widthFloat = unsignedToFloat(tape->width);
        label[2].x = widthFloat;
        label[3].x = widthFloat;
        widgetMatrix(matrix, tape->widget);
        zge::matrixRow(*matrix, 3) = tape->widget.position;

        const uint32_t ratio = tape->textRatio;
        ee::Float step = tape->textDistanceValue / unsignedToFloat(ratio);
        ee::Float value = tape->distance + tape->middleRatio * ee::toFloat(lines + 1) * step;
        ee::Float steps;
        if (!(step == ee::Float(0.0f))) // a distance of nothing per line: one
            steps = value / step;
        else
        {
            step = ee::Float(1.0f);
            steps = step;
        }
        const int32_t wholeSteps = ee::toInt(steps);
        const ee::Float lineHeight = tape->lineHeight;
        style->letterSpacing = 1;
        style->align = 1;
        *reinterpret_cast<uint8_t *>(&style->reserved) = 0; // ZGE2 clears its one byte
        style->lineSpacing = ee::Float::fromBits(0);
        std::memcpy(style->colour, &tape->textColor, sizeof(Rgba));
        std::memcpy(zge::styleOrientation(*style), &tape->widget.orientation, sizeof(zge::Quaternion)); // (copied as it is)
        std::memcpy(&style->scaleX, &tape->textScale, sizeof(zge::Vec4)); // TextScale's four words
        style->position.z = tape->widget.position.z;
        const ee::Float offset = (value - ee::toFloat(wholeSteps) * step) / step * lineHeight;
        const uint32_t phase = ratio - static_cast<uint32_t>(wholeSteps) % ratio;
        if (lines <= 0)
            return 1;
        // The plain lines first, then the labelled ones over them.
        for (int32_t index = 0; index < lines; ++index)
        {
            if ((index + phase) % tape->textRatio == 0)
                continue;
            const ee::Float y = ee::toFloat(index) * tape->lineHeight + offset;
            setRow(&line[0], y, y + tape->lineWeight);
            zge::drawPrimitiveQuads(kQuads, lineQuad, 1, matrix);
        }
        for (int32_t index = 0; index < lines; ++index, value = value - step)
        {
            if ((index + phase) % tape->textRatio != 0)
                continue;
            const ee::Float y = ee::toFloat(index) * tape->lineHeight + offset;
            setRow(&label[0], y, y + tape->lineWeight);
            zge::drawPrimitiveQuads(kQuads, labelQuad, 1, matrix);
            const ee::Float textY = tape->textOffsetY + tape->widget.position.y + label[0].y;
            style->position.x = tape->textOffsetX + tape->widget.position.x;
            style->position.y = textY;
            if (tape->textFormat)
            {
                // The label: the value through TextFormat.
                const uint64_t asDouble = sce::libgcc::fptodp(value);
                zge::formatString(text, tape->textFormat, asDouble);
                if (const ee::Ptr<UiAsset> font = tape->font)
                    drawText(ee::Ptr<zge::Font>(font->object), style, ee::Ptr<const int8_t>(ee::Address(text)));
            }
        }
        return 1;
    }

    // defineGuiTapeProperties: a tape's handlers, defaults and properties.
    uint32_t defineGuiTapeProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<GuiTape> tape(node);
        const uint32_t scale = node + offsetof(GuiTape, scale);
        defineWidgetProperties(node, text);
        addListener(node, Message::Draw, sym::drawGuiTape);
        addListener(node, Message::Refresh, sym::refreshGuiTape);
        tape->charset = 0;
        tape->distance = ee::Float(0.0f);
        tape->lineColor.paletteName = 0;
        tape->sepColor.paletteName = 0;
        tape->textFormat = 0;
        tape->uiAsset = 0;
        tape->lineHeight = ee::Float(15.0f);
        tape->lineWidth = ee::Float(10.0f);
        tape->textRatio = 5;
        tape->height = 0x80;
        tape->width = 0x20;
        tape->textOffsetY = ee::Float(2.0f);
        tape->lineWeight = ee::Float(1.0f);
        tape->middleRatio = ee::Float(0.5f);
        tape->textDistanceValue = ee::Float(1.0f);
        tape->textOffsetX = ee::Float(2.0f);
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            tape->lineColor.rgba[channel] = 255;
            tape->sepColor.rgba[channel] = 255;
            tape->textColor.rgba[channel] = 255;
        }
        zge::vec3SetOne(ee::Ptr<zge::Vec3>(scale));
        definePropertySteps(node, kTapeProperties);
        return node;
    }

    // defineListedTape: ZGE2's defineGuiTapeProperties: no Scale of its own
    // (the widget's is in WidgetRecord) and no default TextScale.
    uint32_t defineListedTape(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedTape> tape(address);
        defineWidgetRecord(address, name);
        subscribeWidget(object, Message::Draw, sym::drawGuiTape);
        subscribeWidget(object, Message::Refresh, sym::refreshGuiTape);
        tape->charset = 0;
        tape->distance = ee::Float(0.0f);
        tape->textFormat = 0;
        tape->font = 0;
        tape->lineHeight = ee::Float(15.0f);
        tape->lineWidth = ee::Float(10.0f);
        tape->textRatio = 5;
        tape->height = 0x80;
        tape->width = 0x20;
        tape->textOffsetY = ee::Float(2.0f);
        tape->lineWeight = ee::Float(1.0f);
        tape->middleRatio = ee::Float(0.5f);
        tape->textDistanceValue = ee::Float(1.0f);
        tape->textOffsetX = ee::Float(2.0f);
        tape->lineColor = {255, 255, 255, 255};
        tape->textColor = {255, 255, 255, 255};
        tape->sepColor = {255, 255, 255, 255};
        addListedProperties(tape, kListedTapeProperties);
        return address;
    }
    READABLE_FUNCTION_OWN(defineGuiTapeProperties, Verified, dm2, defineListedTape);

    namespace
    {
        // ZGE4's guiGroup: a widget with its Width and Height properties
        // (ZGE3's is a plain widget, no size).
        struct GuiGroup
        {
            Widget widget;
            PropertyNode properties[2]; // +0x0F0 Width's and Height's nodes
        };
        static_assert(sizeof(GuiGroup) == 0x130);
    }

    // defineGuiGroupProperties (ZGE4 only): the guiGroup kind's definition:
    // every widget's properties, and Width and Height over the widget's size.
    inline uint32_t defineGuiGroupProperties(ee::Address address, uint32_t name)
    {
        defineWidgetProperties(address, name);
        const ee::Ptr<GuiGroup> group(address);
        const ee::Ptr<const char> zfloat(sym::g_textZFloat);
        defineProperty(&group->properties[0], group, ee::Ptr<const char>(sym::g_textWidth), zfloat, widgetWidthAddress(classCast<Widget>(group)));
        defineProperty(&group->properties[1], group, ee::Ptr<const char>(sym::g_textHeight), zfloat, widgetHeightAddress(classCast<Widget>(group)));
        return address;
    }
    READABLE_FUNCTION_ONLY(bmx, defineGuiGroupProperties);

    // registerWidgetKinds: the kinds a UI script's objects can be, each with
    // the bytes its record takes (ZGE4's kinds that keep their alignment and
    // size in the widget are smaller; its guiGroup has a size of its own).
    void registerWidgetKinds()
    {
        struct Kind
        {
            uint32_t name, bytes, define;
        };
        using game::readable::perGame;
        static constexpr Kind kKinds[] = {
            {sym::g_textGuiGroup, perGame(0xF0u, 0xF0u, 0x130u),
             perGame(sym::defineWidgetProperties, sym::defineWidgetProperties, sym::bmx::defineGuiGroupProperties)}, // guiGroup
            {sym::g_textGuiButton, 0x1D0u, sym::defineGuiButtonProperties}, // guiButton
            {sym::g_textGuiClipper, perGame(0x150u, 0x150u, 0x140u), sym::defineGuiClipperProperties}, // guiClipper
            {sym::g_textGuiFrame, perGame(0x330u, 0x330u, 0x320u), sym::defineGuiFrameProperties}, // guiFrame
            {sym::g_textGuiIdleFunc, 0x100u, sym::defineGuiIdleFuncProperties}, // guiIdleFunc
            {sym::g_textGuiImage, perGame(0x2D0u, 0x2D0u, 0x2C0u), sym::defineGuiImageProperties}, // guiImage
            {sym::g_textGuiInputGroup, 0x1D0u, sym::defineGuiInputGroupProperties}, // guiInputGroup
            {sym::g_textGuiKeyboard, perGame(0x4C0u, 0x4C0u, 0x4B0u), sym::defineGuiKeyboardProperties}, // guiKeyboard
            {sym::g_textGuiKeySniffer, perGame(0x490u, 0x490u, 0x530u), sym::defineGuiKeySnifferProperties}, // guiKeySniffer
            {sym::g_textGuiLabel, perGame(0x260u, 0x260u, 0x250u), sym::defineGuiLabelProperties}, // guiLabel
            {sym::g_textGuiTextBox, 0x2C0u, sym::defineGuiTextBoxProperties}, // guiTextBox
            {sym::g_textGuiLine, 0x1D0u, sym::defineGuiLineProperties}, // guiLine
            {sym::g_textGuiMenu, perGame(0x450u, 0x450u, 0x490u), sym::defineGuiMenuProperties}, // guiMenu
            {sym::g_textGuiModel, 0x160u, sym::defineGuiModelProperties}, // guiModel
            {sym::g_textGuiTape, 0x180u, sym::defineGuiTapeProperties}, // guiTape
            {sym::g_textGuiToggler, 0x2F0u, sym::defineGuiTogglerProperties}, // guiToggler
            {sym::g_textGuiTreeView, 0x430u, sym::defineGuiTreeViewProperties}, // guiTreeView
            {sym::g_textGuiScrollView, perGame(0x2E0u, 0x2E0u, 0x2F0u), sym::defineGuiScrollViewProperties}, // guiScrollView
        };
        constexpr uint32_t kCount = sizeof(kKinds) / sizeof(kKinds[0]);
        for (uint32_t index = 0; index + 1 < kCount; ++index)
            registerWidgetKind(kKinds[index].name, kKinds[index].bytes, 1, kKinds[index].define READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
        game::tail(registerWidgetKind, kKinds[kCount - 1].name, kKinds[kCount - 1].bytes, 1, kKinds[kCount - 1].define READABLE_PARAM_ONLY(dm2, sym::dm2::g_widgetObjectClass));
    }

    // registerListedKinds: ZGE2's registerWidgetKinds: its kinds with their
    // records' sizes and flags (it has a guiContainer, no group, text box or
    // model), each widget a scene object of g_widgetObjectClass.
    void registerListedKinds()
    {
        struct Kind
        {
            uint32_t name, bytes, flags, define;
        };
        static constexpr Kind kKinds[] = {
            {sym::g_textGuiMenu, 0xE0u, 3, sym::defineGuiMenuProperties},
            {sym::g_textGuiLabel, 0x80u, 1, sym::defineGuiLabelProperties},
            {sym::g_textGuiImage, 0x90u, 1, sym::defineGuiImageProperties},
            {sym::g_textGuiFrame, 0xC0u, 1, sym::defineGuiFrameProperties},
            {sym::dm2::g_textGuiContainer, 0x70u, 3, sym::dm2::defineGuiContainerProperties},
            {sym::g_textGuiClipper, 0x80u, 3, sym::defineGuiClipperProperties},
            {sym::g_textGuiInputGroup, 0x80u, 3, sym::defineGuiInputGroupProperties},
            {sym::g_textGuiToggler, 0xA0u, 3, sym::defineGuiTogglerProperties},
            {sym::g_textGuiButton, 0x80u, 3, sym::defineGuiButtonProperties},
            {sym::g_textGuiLine, 0x90u, 3, sym::defineGuiLineProperties},
            {sym::g_textGuiKeyboard, 0xE0u, 1, sym::defineGuiKeyboardProperties},
            {sym::g_textGuiTreeView, 0xD0u, 1, sym::defineGuiTreeViewProperties},
            {sym::g_textGuiScrollView, 0x90u, 1, sym::defineGuiScrollViewProperties},
            {sym::g_textGuiIdleFunc, 0x70u, 1, sym::defineGuiIdleFuncProperties},
            {sym::g_textGuiTape, 0xC0u, 1, sym::defineGuiTapeProperties},
            {sym::g_textGuiKeySniffer, 0xD0u, 1, sym::defineGuiKeySnifferProperties},
        };
        constexpr uint32_t kObjectClass = sym::dm2::g_widgetObjectClass;
        constexpr uint32_t kCount = sizeof(kKinds) / sizeof(kKinds[0]);
        for (uint32_t index = 0; index + 1 < kCount; ++index)
            registerWidgetKind(kKinds[index].name, kKinds[index].bytes, kKinds[index].flags,
                               kKinds[index].define READABLE_PARAM_ONLY(dm2, kObjectClass));
        const Kind &last = kKinds[kCount - 1]; // (the original's last one is its tail jump)
        game::tail(registerWidgetKind, last.name, last.bytes, last.flags, last.define READABLE_PARAM_ONLY(dm2, kObjectClass));
    }
    READABLE_FUNCTION_OWN(registerWidgetKinds, Verified, dm2, registerListedKinds);
}

// ZGE2's guiContainer (no ZGE3 kind): of the widgets hanging from it, only
// the one its SelectedIndex counts to is shown.
namespace zge::zgui
{
    namespace
    {
        struct ListedContainer
        {
            WidgetRecord widget;
            int32_t selectedIndex; // +0x60 "SelectedIndex" (-1: none)
        };
        static_assert(offsetof(ListedContainer, selectedIndex) == 0x60);
    }

    // showSelectedChild (ZGE2 only): the guiContainer's Draw: each object
    // hanging from the widget shown if it is the SelectedIndex-th, else
    // hidden. 1.
    inline int32_t showSelectedChild(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> container(message->target);
        const ee::Ptr<ListedContainer> record = messageRecord<ListedContainer>(message);
        int32_t index = 0;
        for (ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(container); child; child = zge::nextAttachedSibling(child))
        {
            zge::setSceneObjectShown(child, index == record->selectedIndex ? 1 : 0);
            ++index;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, showSelectedChild);

    // defineGuiContainerProperties (ZGE2 only): a guiContainer's record: no
    // selection, its SelectedIndex property and its Draw. The record.
    inline uint32_t defineGuiContainerProperties(uint32_t address, uint32_t name, ee::Address object)
    {
        const ee::Ptr<ListedContainer> container(address);
        defineWidgetRecord(address, name);
        container->selectedIndex = -1;
        addListedProperty(ee::Ptr<zge::List>(&container->widget.properties), ee::Ptr<const char>(sym::g_textSelectedIndex2),
                          ee::Ptr<const char>(sym::g_textSint32), valueAddress(container->selectedIndex));
        subscribeWidget(object, Message::Draw, sym::dm2::showSelectedChild);
        return address;
    }
    READABLE_FUNCTION_ONLY(dm2, defineGuiContainerProperties);
}

