#pragma once

// zgui's widget kinds whose records ZGE4 (BMX XXX) lays out otherwise than
// ZGE3: each record with its fields side by side, and the steps that read a
// field one version has and the other has not. The records stay their files'
// own (an unnamed namespace: each file that includes this has its copy), as
// they were in those files (widgets.cpp, text.cpp, quad.cpp, widgets_small.cpp,
// scroll_view.cpp).

#include "zge/zgui/widget_parts.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/widget_kinds.h.h"

namespace zge::zgui
{
    namespace
    {
        // A line widget (defineGuiLineProperties): from its position to
        // position + vector, its colour running from startColor to endColor.
        struct GuiLine
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Width is the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x48];
                    ee::Float width; // +0x048 Widget::width
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 vector;        // +0x0F0 the far end from the position
            ee::Ptr<UiAsset> asset;  // +0x100 found by material
            uint8_t _104[4];
            WidgetColour startColor; // +0x108
            WidgetColour endColor;   // +0x118
#if !ZX_GAME_BMX
            ee::Float width;         // +0x128 (a property; no draw reads it; ZGE4: the widget's, above)
#endif
            uint32_t material;       // +0x12C (ZGE4 +0x128)
            PropertyNode properties[5]; // +0x130 (ZGE4 +0x12C) its properties' nodes (kLineProperties)
        };
        static_assert(offsetof(GuiLine, width) == game::readable::perGame(0x128u, 0x128u, 0x48u));
        static_assert(offsetof(GuiLine, material) == game::readable::perGame(0x12Cu, 0x12Cu, 0x128u) &&
                      offsetof(GuiLine, properties) == game::readable::perGame(0x130u, 0x130u, 0x12Cu));

        // An image (defineGuiImageProperties; quad.cpp draws it).
        struct GuiImage
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;         // +0x0F0
            ee::Ptr<UiAsset> asset;  // +0x100
            uint8_t _104[4];
            WidgetColour topLeftColor;     // +0x108
            WidgetColour bottomLeftColor;  // +0x118
            WidgetColour topRightColor;    // +0x128
            WidgetColour bottomRightColor; // +0x138
            uint32_t material;       // +0x148
#if !ZX_GAME_BMX
            uint32_t alignment;      // +0x14C (ZGE4: the widget's, above)
#endif
            uint32_t unused;         // +0x150 (ZGE4 +0x14C) made 0 here; read nowhere (PS2, Xbox, GameCube; DM2's image the same at +0x7C)
#if !ZX_GAME_BMX
            ee::Float width;         // +0x154 (ZGE4: the widget's, above)
            ee::Float height;        // +0x158
#endif
            ee::Float uScale;        // +0x15C (ZGE4 +0x150)
            ee::Float vScale;        // +0x160 (ZGE4 +0x154)
            PropertyNode properties[11]; // +0x164 (ZGE4 +0x158) its properties' nodes (kImageProperties)
        };
        static_assert(offsetof(GuiImage, width) == game::readable::perGame(0x154u, 0x154u, 0x48u) &&
                      offsetof(GuiImage, alignment) == game::readable::perGame(0x14Cu, 0x14Cu, 0x1Cu));
        static_assert(offsetof(GuiImage, material) == 0x148 &&
                      offsetof(GuiImage, vScale) == game::readable::perGame(0x160u, 0x160u, 0x154u) &&
                      offsetof(GuiImage, properties) == game::readable::perGame(0x164u, 0x164u, 0x158u));

        // A frame (defineGuiFrameProperties; box.cpp draws it).
        constexpr uint32_t kFrameParts = 9; // top left, top, top right, left, centre, right, bottom left, bottom, bottom right
        struct GuiFrame
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;         // +0x0F0
            ee::Ptr<UiAsset> assets[kFrameParts]; // +0x100
            uint8_t _124[4];
            WidgetColour color;      // +0x128
#if !ZX_GAME_BMX
            uint32_t alignment;      // +0x138 (ZGE4: the widget's, above)
#endif
            uint32_t materials[kFrameParts]; // +0x13C (ZGE4 +0x138)
#if !ZX_GAME_BMX
            ee::Float width;         // +0x160 (ZGE4: the widget's, above)
            ee::Float height;        // +0x164
#endif
            PropertyNode properties[14]; // +0x168 (ZGE4 +0x15C) its properties' nodes (kFrameProperties)
        };
        static_assert(offsetof(GuiFrame, width) == game::readable::perGame(0x160u, 0x160u, 0x48u) &&
                      offsetof(GuiFrame, alignment) == game::readable::perGame(0x138u, 0x138u, 0x1Cu));
        static_assert(offsetof(GuiFrame, color) == 0x128 &&
                      offsetof(GuiFrame, materials) == game::readable::perGame(0x13Cu, 0x13Cu, 0x138u) &&
                      offsetof(GuiFrame, properties) == game::readable::perGame(0x168u, 0x168u, 0x15Cu));

        // A screen rectangle, as a camera's viewport takes it.
        struct ScreenRect
        {
            ee::Float x, y, width, height;
        };

        // A clipper (defineGuiClipperProperties): its Width and Height are
        // Sint32s of its own in ZGE3, the widget's ZFloats in ZGE4.
        struct GuiClipper
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x48];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
#if !ZX_GAME_BMX
            int32_t width;               // +0x0F0 "Width" (Sint32)
            int32_t height;              // +0x0F4 "Height" (Sint32)
#endif
            ScreenRect previousViewport; // +0x0F8 (ZGE4 +0x0F0) the camera's before the Draw
            PropertyNode property;       // +0x108 (ZGE4 +0x100) both properties' node (defineGuiClipperProperties)
        };
        static_assert(offsetof(GuiClipper, width) == game::readable::perGame(0xF0u, 0xF0u, 0x48u));
        static_assert(offsetof(GuiClipper, previousViewport) == game::readable::perGame(0xF8u, 0xF8u, 0xF0u) &&
                      offsetof(GuiClipper, property) == game::readable::perGame(0x108u, 0x108u, 0x100u));
        // The clipper's rectangle's size.
        [[gnu::always_inline]] inline ee::Float clipWidth(ee::Ptr<GuiClipper> clipper)
        {
#if ZX_GAME_BMX
            return clipper->widget.width;
#else
            return ee::toFloat(clipper->width);
#endif
        }
        [[gnu::always_inline]] inline ee::Float clipHeight(ee::Ptr<GuiClipper> clipper)
        {
#if ZX_GAME_BMX
            return clipper->widget.height;
#else
            return ee::toFloat(clipper->height);
#endif
        }

        // The style the font's drawer gets (zge::TextStyle). This class's Draw
        // and Refresh set one byte of the word after the spacing (ZGE4: its
        // low four bits; bit 4 is the monospaced flag ZGE3 keeps in the next
        // word, and ZGE4's style is a word shorter).
        struct TextStyle
        {
            zge::Vec4 position; // +0x00 the widget's screen position
            zge::Vec4 size;     // +0x10
            uint8_t colour[4];  // +0x20
#if ZX_GAME_BMX
            ee::Float lineSpacing; // +0x24
            int32_t letterSpacing; // +0x28
            uint32_t align;     // +0x2C
            uint32_t cleared : 4; // +0x30 0
            uint32_t monospaced : 1;
            uint32_t : 27;
#else
            uint32_t align;     // +0x24
            ee::Float lineSpacing; // +0x28
            int32_t letterSpacing; // +0x2C
            uint8_t cleared;    // +0x30 0 (the other three bytes are not set)
            uint8_t _31[3];
            uint32_t monospaced; // +0x34
#endif
            ee::Float angle;    // +0x38 (ZGE4 +0x34)
            ee::Float width;    // +0x3C (ZGE4 +0x38) measured
            ee::Float height;   // +0x40 (ZGE4 +0x3C)
        };
        static_assert(offsetof(TextStyle, colour) == 0x20 &&
                      offsetof(TextStyle, angle) == game::readable::perGame(0x38u, 0x38u, 0x34u));
        static_assert(offsetof(TextStyle, height) == game::readable::perGame(0x40u, 0x40u, 0x3Cu));

        // An image as its Draw reads it (quad.cpp: drawQuadWidget).
        struct QuadWidget
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;          // +0x0F0
            ee::Ptr<UiAsset> texture;         // +0x100 found by name on first use
            uint32_t _104;
            uint8_t topLeft[4];       // +0x108 RGBA, RGB scaled by zgui's alpha
            uint32_t _10c[3];
            uint8_t bottomLeft[4];    // +0x118
            uint32_t _11c[3];
            uint8_t topRight[4];      // +0x128
            uint32_t _12c[3];
            uint8_t bottomRight[4];   // +0x138
            uint32_t _13c[3];
            uint32_t textureName;     // +0x148 0: none
#if !ZX_GAME_BMX
            uint32_t alignment;       // +0x14C low nibble 0 left, 1 centre, 2 right; high 0 top, 1 middle, 2 bottom (ZGE4: the widget's, above)
#endif
            uint32_t _150;            // (ZGE4 +0x14C)
#if !ZX_GAME_BMX
            ee::Float width;          // +0x154 (ZGE4: the widget's, above)
            ee::Float height;         // +0x158
#endif
            ee::Float u, v;           // +0x15C (ZGE4 +0x150) the texture's far corner
        };
        static_assert(offsetof(QuadWidget, width) == game::readable::perGame(0x154u, 0x154u, 0x48u) &&
                      offsetof(QuadWidget, alignment) == game::readable::perGame(0x14Cu, 0x14Cu, 0x1Cu));
        static_assert(offsetof(QuadWidget, texture) == 0x100 && offsetof(QuadWidget, bottomRight) == 0x138);
        static_assert(offsetof(QuadWidget, textureName) == 0x148 &&
                      offsetof(QuadWidget, u) == game::readable::perGame(0x15Cu, 0x15Cu, 0x150u));

        // A tape (widgets_small.cpp): a ruler that scrolls by a value.
        struct GuiTape
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x48];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;           // +0x0F0
            uint64_t textScale;        // +0x100 a Z3DVector: x, y
            uint64_t textScaleZW;      // +0x108
            ee::Ptr<UiAsset> uiAsset;  // +0x110 found by charset
            ee::Float distance;        // +0x114 the value at the middle
            ee::Float lineHeight;      // +0x118
            ee::Float lineWidth;       // +0x11C
            ee::Float lineWeight;      // +0x120
            uint32_t _124;
            WidgetColour lineColor;    // +0x128
            uint32_t textRatio;        // +0x138 a label every so many lines
            ee::Float textDistanceValue; // +0x13C the value between labels
            WidgetColour textColor;    // +0x140
            WidgetColour sepColor;     // +0x150
            ee::Float textOffsetX;     // +0x160
            ee::Float textOffsetY;     // +0x164
            ee::Float middleRatio;     // +0x168
#if !ZX_GAME_BMX
            uint32_t width;            // +0x16C a Sint32 (ZGE4: the widget's ZFloat, above)
            uint32_t height;           // +0x170
#endif
            uint32_t textFormat;       // +0x174 (ZGE4 +0x16C)
            uint32_t charset;          // +0x178 (ZGE4 +0x170)
        };
        static_assert(offsetof(GuiTape, width) == game::readable::perGame(0x16Cu, 0x16Cu, 0x48u));
        static_assert(offsetof(GuiTape, lineColor) == 0x128 && offsetof(GuiTape, textRatio) == 0x138 &&
                      offsetof(GuiTape, charset) == game::readable::perGame(0x178u, 0x178u, 0x170u));

        // A scroll view (scroll_view.cpp): text that scrolls up through a window.
        struct GuiScrollView
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;          // +0x0F0
            ee::Ptr<UiAsset> asset;         // +0x100 found by charset
            uint32_t parseText;       // +0x104 expand the text's $KEYS first
            uint32_t text;            // +0x108
            uint32_t charset;         // +0x10C
            WidgetColour topColor;    // +0x110
            WidgetColour bottomColor; // +0x120
            WidgetColour midColor;    // +0x130
            int32_t tracking;         // +0x140
            ee::Float spacing;        // +0x144
#if !ZX_GAME_BMX
            ee::Float width;          // +0x148 (ZGE4: the widget's, above)
            ee::Float height;         // +0x14C
#endif
            ee::Float offset;         // +0x150 (ZGE4 +0x148) how far the text has scrolled up
#if ZX_GAME_BMX
            // ZGE4 +0x14C "EndReached" (a Bool variable): the last line drawn
            // has scrolled above the window's top (drawGuiScrollView).
            uint32_t endReached;
#endif
            PropertyNode properties[game::readable::perGame(12, 12, 13)]; // +0x154 (ZGE4 +0x150) its properties' nodes (kScrollViewProperties)
        };
        static_assert(offsetof(GuiScrollView, width) == game::readable::perGame(0x148u, 0x148u, 0x48u));
        static_assert(offsetof(GuiScrollView, properties) == game::readable::perGame(0x154u, 0x154u, 0x150u));
        static_assert(offsetof(GuiScrollView, topColor) == 0x110 &&
                      offsetof(GuiScrollView, offset) == game::readable::perGame(0x150u, 0x150u, 0x148u));

        constexpr uint32_t kCentredText = 0x11; // TextStyle::align: middle, middle

        constexpr ClassProperty kScrollViewProperties[] = {
            {sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiScrollView, scale)}, // "Scale" Z3DVector
#if ZX_GAME_BMX
            {sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiScrollView, alignment)}, // "Alignment" Alignment (ZGE4)
#endif
            {sym::g_textSpacing, sym::g_textZFloat, offsetof(GuiScrollView, spacing)}, // "Spacing" ZFloat
            {sym::g_textTracking, sym::g_textSint32, offsetof(GuiScrollView, tracking)}, // "Tracking" Sint32
            {sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiScrollView, charset)}, // "Charset" ZAssetID
            {sym::g_textText6, sym::g_textString, offsetof(GuiScrollView, text)}, // "Text" String
            {sym::g_textParseText, sym::g_textBool, offsetof(GuiScrollView, parseText)}, // "ParseText" Bool
            {sym::g_textTopColor, sym::g_textZrgbaPal, offsetof(GuiScrollView, topColor)}, // "TopColor" ZRGBA_PAL
            {sym::g_textMidColor, sym::g_textZrgbaPal, offsetof(GuiScrollView, midColor)}, // "MidColor" ZRGBA_PAL
            {sym::g_textBottomColor, sym::g_textZrgbaPal, offsetof(GuiScrollView, bottomColor)}, // "BottomColor" ZRGBA_PAL
            {sym::g_textWidth, sym::g_textZFloat, offsetof(GuiScrollView, width)}, // "Width" ZFloat
            {sym::g_textHeight, sym::g_textZFloat, offsetof(GuiScrollView, height)}, // "Height" ZFloat
            {sym::g_textOffset, sym::g_textZFloat, offsetof(GuiScrollView, offset)}, // "Offset" ZFloat
        };

        // How the lines are aligned: ZGE3 centres them; ZGE4 has an Alignment
        // property (the widget's, above).
        [[gnu::always_inline]] inline uint32_t lineAlignment([[maybe_unused]] ee::Ptr<GuiScrollView> view)
        {
#if ZX_GAME_BMX
            return view->alignment;
#else
            return kCentredText;
#endif
        }
        // ZGE4's EndReached (a scroll view of ZGE3 has none: no ZGE3 step
        // asks for it).
        [[gnu::always_inline]] inline ee::Ptr<uint32_t> endReachedOf([[maybe_unused]] ee::Ptr<GuiScrollView> view)
        {
#if ZX_GAME_BMX
            return &view->endReached;
#else
            return {};
#endif
        }

        // A menu (menu.cpp): Items' lines, one selected.
        struct GuiMenu
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;             // +0x0F0
            zge::Vec4 selectedCoord;     // +0x100 from the menu to the selected item
#if ZX_GAME_BMX
            // ZGE4 +0x110, +0x114: set to 1 when the selection steps towards
            // the start (or the end), less by a rate each draw down to 0; the
            // StartArrow's (EndArrow's) colours' alpha is 255 * (1 - it).
            ee::Float startArrowFlash;
            ee::Float endArrowFlash;
#endif
            ee::Ptr<UiAsset> asset;      // +0x110 (ZGE4 +0x118; each field to onChange 8 further) found by charset
            int32_t lastSelectedIndex;   // +0x114 what OnChange last ran for
            uint32_t horizontal;         // +0x118
            ee::Float horizontalSpacing; // +0x11C
            uint32_t wrapAround;         // +0x120
            int32_t visibleCount;        // +0x124
            int32_t selectedIndex;       // +0x128 a line of Items
            uint8_t _12c[4];             // +0x12C not used
            int32_t scrollOffset;        // +0x130 the first row shown
            uint32_t hideSelectionOnLostFocus; // +0x134
            uint32_t blockedPlayed;      // +0x138 the blocked sound played since the selection last moved
#if !ZX_GAME_BMX
            uint32_t alignment;          // +0x13C (ZGE4: the widget's, above)
#endif
            WidgetColour color;          // +0x140 (ZGE4 +0x148: the colours' 8-byte alignment)
            WidgetColour selectedColor;  // +0x150
            WidgetColour disabledColor;  // +0x160
            ee::Float spacing;           // +0x170
#if !ZX_GAME_BMX
            ee::Float width;             // +0x174 measured (ZGE4: the widget's, above)
            ee::Float height;            // +0x178
#endif
            uint32_t selectedText;       // +0x17C (ZGE4 the same)
            uint32_t charset;            // +0x180
            uint32_t items;              // +0x184
            uint32_t onGetFocus;         // +0x188
            uint32_t onLostFocus;        // +0x18C
            uint32_t onCancel;           // +0x190
            uint32_t onItemFocus;        // +0x194
            uint32_t onItemUnFocus;      // +0x198
            uint32_t onItemSelect;       // +0x19C
            uint32_t onChange;           // +0x1A0
#if ZX_GAME_BMX
            // ZGE4 +0x1A4, +0x1A8 (Event properties): the paths of the widgets
            // shown as arrows while there is more to scroll to at the start
            // (the end) of a menu with a VisibleCount.
            uint32_t startArrow;
            uint32_t endArrow;
#endif
            PropertyNode properties[game::readable::perGame(21, 21, 23)]; // +0x1A4 (ZGE4 +0x1AC) its properties' nodes (kMenuProperties)
        };
        static_assert(offsetof(GuiMenu, width) == game::readable::perGame(0x174u, 0x174u, 0x48u) &&
                      offsetof(GuiMenu, alignment) == game::readable::perGame(0x13Cu, 0x13Cu, 0x1Cu));
        static_assert(offsetof(GuiMenu, color) == game::readable::perGame(0x140u, 0x140u, 0x148u) &&
                      offsetof(GuiMenu, selectedText) == 0x17C && offsetof(GuiMenu, onChange) == 0x1A0 &&
                      offsetof(GuiMenu, properties) == game::readable::perGame(0x1A4u, 0x1A4u, 0x1ACu));
        // A menu property's node.
        constexpr uint32_t menuNode(uint32_t index) { return offsetof(GuiMenu, properties) + index * sizeof(PropertyNode); }

        constexpr PropertyStep kMenuProperties[] = {
            {Property, menuNode(0), sym::g_textScale, sym::g_textZ3DVector, offsetof(GuiMenu, scale)}, // "Scale" Z3DVector
            {Property, menuNode(1), sym::g_textHorizontal, sym::g_textBool, offsetof(GuiMenu, horizontal)}, // "Horizontal" Bool
            {Property, menuNode(2), sym::g_textHorizontalSpacing, sym::g_textZFloat, offsetof(GuiMenu, horizontalSpacing)}, // "HorizontalSpacing" ZFloat
            {Property, menuNode(3), sym::g_textWrapAround, sym::g_textBool, offsetof(GuiMenu, wrapAround)}, // "WrapAround" Bool
            {Property, menuNode(4), sym::g_textVisibleCount, sym::g_textSint32, offsetof(GuiMenu, visibleCount)}, // "VisibleCount" Sint32
            {Property, menuNode(5), sym::g_textSelectedIndex2, sym::g_textSint32, offsetof(GuiMenu, selectedIndex)}, // "SelectedIndex" Sint32
            {Variable, 0x0, sym::g_textSelectedCoord, sym::g_textZ3DVector, offsetof(GuiMenu, selectedCoord)}, // "SelectedCoord" Z3DVector
            {Typed, 0x0, sym::g_textSelectedText, sym::g_textString, offsetof(GuiMenu, selectedText)}, // "SelectedText" String
            {Variable, 0x0, sym::g_textScrollOffset, sym::g_textSint32, offsetof(GuiMenu, scrollOffset)}, // "ScrollOffset" Sint32
            {Property, menuNode(6), sym::g_textHideSelectionOnLostFocus, sym::g_textBool, offsetof(GuiMenu, hideSelectionOnLostFocus)}, // "HideSelectionOnLostFocus" Bool
            {Property, menuNode(7), sym::g_textCharset, sym::g_textZAssetID2, offsetof(GuiMenu, charset)}, // "Charset" ZAssetID
            {Property, menuNode(8), sym::g_textSpacing, sym::g_textZFloat, offsetof(GuiMenu, spacing)}, // "Spacing" ZFloat
            {Property, menuNode(9), sym::g_textAlignment, sym::g_textAlignment, offsetof(GuiMenu, alignment)}, // "Alignment" Alignment
            {Property, menuNode(10), sym::g_textColor, sym::g_textZrgbaPal, offsetof(GuiMenu, color)}, // "Color" ZRGBA_PAL
            {Property, menuNode(11), sym::g_textSelectedColor, sym::g_textZrgbaPal, offsetof(GuiMenu, selectedColor)}, // "SelectedColor" ZRGBA_PAL
            {Property, menuNode(12), sym::g_textDisabledColor, sym::g_textZrgbaPal, offsetof(GuiMenu, disabledColor)}, // "DisabledColor" ZRGBA_PAL
            {Property, menuNode(13), sym::g_textItems2, sym::g_textString, offsetof(GuiMenu, items)}, // "Items" String
            {Property, menuNode(14), sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiMenu, onGetFocus)}, // "OnGetFocus" Event
            {Property, menuNode(15), sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiMenu, onLostFocus)}, // "OnLostFocus" Event
            {Property, menuNode(16), sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiMenu, onCancel)}, // "OnCancel" Event
            {Property, menuNode(17), sym::g_textOnItemFocus, sym::g_textEvent, offsetof(GuiMenu, onItemFocus)}, // "OnItemFocus" Event
            {Property, menuNode(18), sym::g_textOnItemUnFocus, sym::g_textEvent, offsetof(GuiMenu, onItemUnFocus)}, // "OnItemUnFocus" Event
            {Property, menuNode(19), sym::g_textOnItemSelect, sym::g_textEvent, offsetof(GuiMenu, onItemSelect)}, // "OnItemSelect" Event
            {Property, menuNode(20), sym::g_textOnChange, sym::g_textEvent, offsetof(GuiMenu, onChange)}, // "OnChange" Event
#if ZX_GAME_BMX
            {Property, menuNode(21), sym::bmx::g_textStartArrow, sym::g_textEvent, offsetof(GuiMenu, startArrow)}, // "StartArrow" Event (ZGE4)
            {Property, menuNode(22), sym::bmx::g_textEndArrow, sym::g_textEvent, offsetof(GuiMenu, endArrow)}, // "EndArrow" Event (ZGE4)
#endif
            {Variable, 0x0, sym::g_textWidth, sym::g_textZFloat, offsetof(GuiMenu, width)}, // "Width" ZFloat
            {Variable, 0x0, sym::g_textHeight, sym::g_textZFloat, offsetof(GuiMenu, height)}, // "Height" ZFloat
        };
        static_assert(menuNode(20) == game::readable::perGame(0x424u, 0x424u, 0x42Cu));
        // ZGE4's menu arrows (its own draw and input bodies, menu.cpp, read
        // and write them; a ZGE3 build only declares these: no step of it
        // calls them).
#if ZX_GAME_BMX
        inline ee::Float &startArrowFlash(ee::Ptr<GuiMenu> menu) { return menu->startArrowFlash; }
        inline ee::Float &endArrowFlash(ee::Ptr<GuiMenu> menu) { return menu->endArrowFlash; }
        inline uint32_t &startArrow(ee::Ptr<GuiMenu> menu) { return menu->startArrow; }
        inline uint32_t &endArrow(ee::Ptr<GuiMenu> menu) { return menu->endArrow; }
#else
        ee::Float &startArrowFlash(ee::Ptr<GuiMenu> menu);
        ee::Float &endArrowFlash(ee::Ptr<GuiMenu> menu);
        uint32_t &startArrow(ee::Ptr<GuiMenu> menu);
        uint32_t &endArrow(ee::Ptr<GuiMenu> menu);
#endif

        // A keyboard (keyboard.cpp): a grid of keys that types a String.
        struct GuiKeyboard
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                Widget widget;
                struct
                {
                    uint8_t _000[0x48];
                    ee::Float width; // +0x048 Widget::width
                    ee::Float height; // +0x04C Widget::height
                };
            };
#else
            Widget widget;
#endif
            zge::Vec4 scale;            // +0x0F0
            zge::Vec4 selectedCoord;    // +0x100 the selected key from the keyboard's position (z: its z)
            ee::Ptr<UiAsset> asset;     // +0x110 found by charset
            int32_t selectedIndex;      // +0x114 in the key list
            int32_t rowWidth;           // +0x118
            uint8_t _11c[4];
            WidgetColour color;         // +0x120
            WidgetColour selectedColor; // +0x130
            ee::Float hSpacing;         // +0x140
            ee::Float vSpacing;         // +0x144
            uint32_t maxItems;          // +0x148 String's longest
            uint32_t selectedChar;      // +0x14C
            uint32_t items;             // +0x150
            uint32_t charset;           // +0x154
            uint32_t string;            // +0x158 what was typed
            uint32_t onGetFocus;        // +0x15C
            uint32_t onLostFocus;       // +0x160
            uint32_t onChange;          // +0x164
            uint32_t onCancel;          // +0x168
            uint32_t onSelect;          // +0x16C
            uint32_t onOk;              // +0x170
            uint32_t onItemFocus;       // +0x174
            uint32_t onItemUnFocus;     // +0x178
            uint32_t enableBackspaceKey; // +0x17C the option button deletes
            uint32_t showEnd;           // +0x180
            uint32_t showDel;           // +0x184
            uint32_t showSpace;         // +0x188
            uint32_t showCancel;        // +0x18C
#if !ZX_GAME_BMX
            ee::Float width;            // +0x190 measured (ZGE4: the widget's, above)
            ee::Float height;           // +0x194
#endif
            PropertyNode properties[25]; // +0x198 (ZGE4 +0x190) its properties' nodes
        };
        static_assert(offsetof(GuiKeyboard, color) == 0x120 &&
                      offsetof(GuiKeyboard, height) == game::readable::perGame(0x194u, 0x194u, 0x4Cu));
        static_assert(offsetof(GuiKeyboard, properties) == game::readable::perGame(0x198u, 0x198u, 0x190u) &&
                      sizeof(GuiKeyboard) == game::readable::perGame(0x4C0u, 0x4C0u, 0x4B0u)); // the kind's bytes

        // A key sniffer (key_sniffer.cpp): runs its Events for the pads' buttons and sticks.
        struct GuiKeySniffer
        {
            Widget widget;
            uint32_t active;          // +0x0F0
            uint32_t flags;           // +0x0F4
            int32_t curCtrl;          // +0x0F8 the pad it reads
#if ZX_GAME_BMX
            uint32_t _0fc[4];         // ZGE4 +0x0FC: 1 at definition; no property names them, its draw reads none
#endif
            uint32_t enableAnalogDir; // +0x0FC (ZGE4 +0x10C; the rest 0x10 further)
            uint32_t autoRepeat;      // +0x100
            ee::Float leftAxisX;      // +0x104
            ee::Float leftAxisY;      // +0x108
            ee::Float rightAxisX;     // +0x10C
            ee::Float rightAxisY;     // +0x110
            uint32_t onGetFocus;      // +0x114
            uint32_t onLostFocus;     // +0x118
            uint32_t onAccept;        // +0x11C
            uint32_t onCancel;        // +0x120
            uint32_t onOption;        // +0x124
#if ZX_GAME_BMX
            uint32_t onOption2;       // ZGE4 +0x138 zgui's second option button
#endif
            uint32_t onStart;         // +0x128
            uint32_t onSelect;        // +0x12C
            uint32_t onLeftStickMove; // +0x130
            uint32_t onRightStickMove; // +0x134
            uint32_t onLTrig1;        // +0x138
            uint32_t onLTrig2;        // +0x13C
#if ZX_GAME_BMX
            uint32_t onLTrig3;        // ZGE4 +0x154 (button 0x1000)
#endif
            uint32_t onRTrig1;        // +0x140
            uint32_t onRTrig2;        // +0x144
#if ZX_GAME_BMX
            uint32_t onRTrig3;        // ZGE4 +0x160 a property its draw never runs (button 0x2000 runs OnRTrig2)
            uint32_t onSpecial;       // ZGE4 +0x164 a property its draw never runs
#endif
            uint32_t onUpKey;         // +0x148
            uint32_t onDownKey;       // +0x14C
            uint32_t onRightKey;      // +0x150
            uint32_t onLeftKey;       // +0x154
            uint32_t onFire1;         // +0x158
            uint32_t onFire2;         // +0x15C
            uint32_t onFire3;         // +0x160
            uint32_t onFire4;         // +0x164
            PropertyNode properties[game::readable::perGame(25, 25, 29)]; // +0x168 (ZGE4 +0x188) its properties' nodes (kKeySnifferProperties)
        };
        static_assert(offsetof(GuiKeySniffer, active) == 0xF0 &&
                      offsetof(GuiKeySniffer, enableAnalogDir) == game::readable::perGame(0xFCu, 0xFCu, 0x10Cu) &&
                      offsetof(GuiKeySniffer, onFire4) == game::readable::perGame(0x164u, 0x164u, 0x184u) &&
                      offsetof(GuiKeySniffer, properties) == game::readable::perGame(0x168u, 0x168u, 0x188u));
#if ZX_GAME_BMX
        static_assert(offsetof(GuiKeySniffer, onOption2) == 0x138 && offsetof(GuiKeySniffer, onSpecial) == 0x164);
        // A new ZGE4 sniffer's own fields: its four words 1, its own Events none.
        inline void clearZge4SnifferFields(ee::Ptr<GuiKeySniffer> sniffer)
        {
            for (uint32_t &word : sniffer->_0fc)
                word = 1;
            sniffer->onOption2 = 0;
            sniffer->onLTrig3 = 0;
            sniffer->onRTrig3 = 0;
            sniffer->onSpecial = 0;
        }
#else
        void clearZge4SnifferFields(ee::Ptr<GuiKeySniffer> sniffer); // (ZGE4's alone)
#endif

        constexpr ClassProperty kKeySnifferProperties[] = {
            {sym::g_textActive2, sym::g_textBool, offsetof(GuiKeySniffer, active)}, // "Active" Bool
            {sym::g_textOnGetFocus, sym::g_textEvent, offsetof(GuiKeySniffer, onGetFocus)}, // "OnGetFocus" Event
            {sym::g_textOnLostFocus, sym::g_textEvent, offsetof(GuiKeySniffer, onLostFocus)}, // "OnLostFocus" Event
            {sym::g_textFlags, sym::g_textSint32, offsetof(GuiKeySniffer, flags)}, // "Flags" Sint32
            {sym::g_textEnableAnalogDir, sym::g_textBool, offsetof(GuiKeySniffer, enableAnalogDir)}, // "EnableAnalogDir" Bool
            {sym::g_textAutoRepeat, sym::g_textBool, offsetof(GuiKeySniffer, autoRepeat)}, // "AutoRepeat" Bool
            {sym::g_textOnAccept, sym::g_textEvent, offsetof(GuiKeySniffer, onAccept)}, // "OnAccept" Event
            {sym::g_textOnCancel, sym::g_textEvent, offsetof(GuiKeySniffer, onCancel)}, // "OnCancel" Event
            {sym::g_textOnOption, sym::g_textEvent, offsetof(GuiKeySniffer, onOption)}, // "OnOption" Event
#if ZX_GAME_BMX
            {sym::bmx::g_textOnOption2, sym::g_textEvent, offsetof(GuiKeySniffer, onOption2)}, // "OnOption2" Event (ZGE4)
#endif
            {sym::g_textOnStart, sym::g_textEvent, offsetof(GuiKeySniffer, onStart)}, // "OnStart" Event
            {sym::g_textOnSelect, sym::g_textEvent, offsetof(GuiKeySniffer, onSelect)}, // "OnSelect" Event
#if !ZX_GAME_BMX
            {sym::g_textOnFire1, sym::g_textEvent, offsetof(GuiKeySniffer, onFire1)}, // "OnFire1" Event
            {sym::g_textOnFire2, sym::g_textEvent, offsetof(GuiKeySniffer, onFire2)}, // "OnFire2" Event
            {sym::g_textOnFire3, sym::g_textEvent, offsetof(GuiKeySniffer, onFire3)}, // "OnFire3" Event
            {sym::g_textOnFire4, sym::g_textEvent, offsetof(GuiKeySniffer, onFire4)}, // "OnFire4" Event
#endif
            {sym::g_textOnLeftKey, sym::g_textEvent, offsetof(GuiKeySniffer, onLeftKey)}, // "OnLeftKey" Event
            {sym::g_textOnUpKey, sym::g_textEvent, offsetof(GuiKeySniffer, onUpKey)}, // "OnUpKey" Event
            {sym::g_textOnDownKey, sym::g_textEvent, offsetof(GuiKeySniffer, onDownKey)}, // "OnDownKey" Event
            {sym::g_textOnRightKey, sym::g_textEvent, offsetof(GuiKeySniffer, onRightKey)}, // "OnRightKey" Event
            {sym::g_textOnLeftStickMove, sym::g_textEvent, offsetof(GuiKeySniffer, onLeftStickMove)}, // "OnLeftStickMove" Event
            {sym::g_textOnRightStickMove, sym::g_textEvent, offsetof(GuiKeySniffer, onRightStickMove)}, // "OnRightStickMove" Event
            {sym::g_textOnLTrig1, sym::g_textEvent, offsetof(GuiKeySniffer, onLTrig1)}, // "OnLTrig1" Event
            {sym::g_textOnLTrig2, sym::g_textEvent, offsetof(GuiKeySniffer, onLTrig2)}, // "OnLTrig2" Event
#if ZX_GAME_BMX
            {sym::bmx::g_textOnLTrig3, sym::g_textEvent, offsetof(GuiKeySniffer, onLTrig3)}, // "OnLTrig3" Event (ZGE4)
#endif
            {sym::g_textOnRTrig1, sym::g_textEvent, offsetof(GuiKeySniffer, onRTrig1)}, // "OnRTrig1" Event
            {sym::g_textOnRTrig2, sym::g_textEvent, offsetof(GuiKeySniffer, onRTrig2)}, // "OnRTrig2" Event
#if ZX_GAME_BMX
            {sym::bmx::g_textOnRTrig3, sym::g_textEvent, offsetof(GuiKeySniffer, onRTrig3)}, // "OnRTrig3" Event (ZGE4)
            {sym::bmx::g_textOnSpecial, sym::g_textEvent, offsetof(GuiKeySniffer, onSpecial)}, // "OnSpecial" Event (ZGE4)
            {sym::g_textOnFire1, sym::g_textEvent, offsetof(GuiKeySniffer, onFire1)}, // "OnFire1" Event (ZGE4: last)
            {sym::g_textOnFire2, sym::g_textEvent, offsetof(GuiKeySniffer, onFire2)}, // "OnFire2" Event
            {sym::g_textOnFire3, sym::g_textEvent, offsetof(GuiKeySniffer, onFire3)}, // "OnFire3" Event
            {sym::g_textOnFire4, sym::g_textEvent, offsetof(GuiKeySniffer, onFire4)}, // "OnFire4" Event
#endif
        };

        // The four Fire buttons, in a pad's words.
        constexpr uint32_t kButtonFire1 = 0x10, kButtonFire2 = 0x20, kButtonFire3 = 0x40, kButtonFire4 = 0x80;

        // Each pressed and held button's Event, in the original's order.
        // Accept, cancel and option (ZGE4: and option2) are zgui's words.
        struct KeyEvent
        {
            uint32_t mask;
            uint32_t Zgui::*menuMask; // or zgui's word, when set
            uint32_t GuiKeySniffer::*script;
        };
        constexpr KeyEvent kKeyEvents[] = {
            {0, &Zgui::acceptButton, &GuiKeySniffer::onAccept},
            {0, &Zgui::cancelButton, &GuiKeySniffer::onCancel},
            {0, &Zgui::optionButton, &GuiKeySniffer::onOption},
#if ZX_GAME_BMX
            {0, &Zgui::option2Button, &GuiKeySniffer::onOption2},
#endif
            {kButtonFire1, nullptr, &GuiKeySniffer::onFire1},
            {kButtonFire2, nullptr, &GuiKeySniffer::onFire2},
            {kButtonFire3, nullptr, &GuiKeySniffer::onFire3},
            {kButtonFire4, nullptr, &GuiKeySniffer::onFire4},
            {0x100, nullptr, &GuiKeySniffer::onLTrig1},
            {0x400, nullptr, &GuiKeySniffer::onLTrig2},
#if ZX_GAME_BMX
            {0x1000, nullptr, &GuiKeySniffer::onLTrig3},
#endif
            {0x200, nullptr, &GuiKeySniffer::onRTrig1},
            {0x800, nullptr, &GuiKeySniffer::onRTrig2},
#if ZX_GAME_BMX
            {0x2000, nullptr, &GuiKeySniffer::onRTrig2}, // a bug kept: ZGE4's third right trigger runs OnRTrig2
#endif
            {0x4000, nullptr, &GuiKeySniffer::onStart},
            {0x8000, nullptr, &GuiKeySniffer::onSelect},
            {kButtonUp, nullptr, &GuiKeySniffer::onUpKey},
            {kButtonDown, nullptr, &GuiKeySniffer::onDownKey},
            {kButtonRight, nullptr, &GuiKeySniffer::onRightKey},
        };

        // A frame as its Draw reads it (box.cpp: drawBoxWidget).
        struct BoxWidget
        {
#if ZX_GAME_BMX
            // ZGE4: the kind's Alignment, Width and Height are the widget's, named here over it (WidgetLayout).
            union
            {
                struct
                {
                    Node node;
                };
                struct
                {
                    uint8_t _000[0x1C];
                    uint32_t alignment; // +0x01C Node::alignment
                    uint8_t _020[0x28];
                    ee::Float width;    // +0x048 Widget::width
                    ee::Float height;   // +0x04C Widget::height
                };
            };
            uint8_t _050[0xA0];
#else
            Node node;
            uint8_t _040[0xB0];
#endif
            zge::Vec4 scale;          // +0x0F0
            ee::Ptr<UiAsset> texture[9]; // +0x100 found by name on first use
            uint32_t _124;
            uint8_t colour[4];        // +0x128 RGBA, RGB scaled by zgui's alpha
            uint32_t _12c[3];
#if !ZX_GAME_BMX
            uint32_t alignment;       // +0x138 low nibble 0 left, 1 centre, 2 right; high 0 top, 1 middle, 2 bottom (ZGE4: above)
#endif
            uint32_t textureName[9];  // +0x13C (ZGE4 +0x138) 0: none
#if !ZX_GAME_BMX
            ee::Float width;          // +0x160 (ZGE4: above)
            ee::Float height;         // +0x164
#endif
        };
        static_assert(offsetof(BoxWidget, texture) == 0x100 && offsetof(BoxWidget, colour) == 0x128);
        static_assert(offsetof(BoxWidget, alignment) == game::readable::perGame(0x138u, 0x138u, 0x1Cu) &&
                      offsetof(BoxWidget, textureName) == game::readable::perGame(0x13Cu, 0x13Cu, 0x138u));
        static_assert(offsetof(BoxWidget, width) == game::readable::perGame(0x160u, 0x160u, 0x48u) &&
                      offsetof(BoxWidget, height) == game::readable::perGame(0x164u, 0x164u, 0x4Cu));
    }
}
