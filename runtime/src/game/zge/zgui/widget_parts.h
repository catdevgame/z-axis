#pragma once

// What zgui's widget classes share: the fields before a class's own, the
// colour property, the table their property definitions run from and the
// small helpers their Refresh and Draw handlers repeat. widgets.cpp has the
// plain classes; scroll_view.cpp, tree_view.cpp, keyboard.cpp, input_group.cpp,
// toggler.cpp, menu.cpp and key_sniffer.cpp the ones that take input.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/callback.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/widget_parts.h.h"

namespace zge
{
    struct Material;
}

namespace zge::zgui
{
    constexpr uint32_t kFindAsset = sym::findGuiAsset;   // (name, kind) -> the UI asset
    constexpr uint32_t kAssetTexture = 0, kAssetFont = 1;

    // A library's asset (library.cpp), and the head a library starts with.
    struct UiAsset
    {
        ee::Ptr<UiAsset> next;  // +0x00
        uint32_t _04;
        uint8_t header[0x80];   // +0x08 the chunk's body
        char name[0x20];        // +0x88
        uint32_t kind;          // +0xA8 kAssetTexture, kAssetFont ...
        uint32_t list[1];       // +0xAC a library: its assets (zge::List's head)
        uint32_t textureFlags;  // +0xB0
        uint32_t object;        // +0xB4 the material, font, motion or scene
    };
    static_assert(sizeof(UiAsset) == 0xB8);

    // findGuiAsset: "library.asset" by name; the kind must match, else 0.
    ee::Ptr<UiAsset> findGuiAsset(ee::Address path, uint32_t kind);
    constexpr uint32_t kRunOnGetFocus = sym::runOnGetFocus;  // the GetFocus handler: the widget's OnGetFocus script
    constexpr uint32_t kRunOnLostFocus = sym::runOnLostFocus; // the LostFocus handler: its OnLostFocus script
    // ZGE2's GetFocus and LostFocus handlers (runOnGetFocus, runOnLostFocus: build.cpp).
    int32_t runListedGetFocus(ee::Ptr<zge::EngineMessage> message);
    int32_t runListedLostFocus(ee::Ptr<zge::EngineMessage> message);
    // ZGE2's widget objects' class hooks other kinds' classes end with (build.cpp), and the guiModel a script's
    // MODEL block makes (gui_model.cpp).
    uint32_t destroyWidgetObject(ee::Ptr<zge::SceneObject> object);
    uint32_t widgetObjectShown(ee::Ptr<zge::SceneObject> object, uint32_t shown);
    ee::Ptr<zge::SceneObject> newGuiModel(ee::Ptr<const char> name);
    // setVariableAtPath, copyVariableAtPath (ZGE2 only, variables.cpp): the variable at a path from the root (a
    // widget's or an ACTION's) set from a value its type keeps, or copied out as its type keeps it: 1, 0 when the
    // path leads to no variable.
    int32_t setVariableAtPath(ee::Ptr<const char> path, ee::Address value);
    int32_t copyVariableAtPath(ee::Ptr<const char> path, ee::Address out);
    // defineWidgetProperty (ZGE2 only, variables.cpp): a variable of the type named added to the widget's own.
    int32_t defineWidgetProperty(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Ptr<const char> typeName,
                                 ee::Address value);
    constexpr uint32_t kDrawTextAndMeasure = sym::drawText; // (font, style, text): drawn; its size into style +0x3C (engine)

    // A widget's fields before its class's.
    // A property's node, in its widget's memory: a Node without positions.
    struct PropertyNode
    {
        uint8_t node[0x20];
    };

    struct Widget
    {
        Node node;
        ee::Ptr<char> onCreate;   // +0x40 scripts (Event properties)
        ee::Ptr<char> onDestroy;  // +0x44
#if PS2X_GAME_BMX
        // ZGE4 keeps every widget's size here (the kinds' Width and Height
        // properties; ZGE3's kinds keep their own).
        ee::Float width;          // +0x48
        ee::Float height;         // +0x4C
#else
        uint8_t _048[8];
#endif
        PropertyNode properties[5]; // +0x50 Name, OnCreate, OnDestroy, Visible, Position
    };
    static_assert(sizeof(Widget) == 0xF0);
    // A new widget's size set to nothing and its alignment to the top left:
    // ZGE4's alone (only its defineWidgetProperties step calls it).
#if PS2X_GAME_BMX
    inline void clearWidgetLayout(ee::Ptr<Widget> widget)
    {
        widget->width = ee::Float(0.0f);
        widget->height = ee::Float(0.0f);
        widget->node.alignment = 0;
    }
    // Where a ZGE4 widget keeps its size (the Width and Height properties of
    // ZGE4's own kinds point there).
    inline ee::Address widgetWidthAddress(ee::Ptr<Widget> widget) { return &widget->width; }
    inline ee::Address widgetHeightAddress(ee::Ptr<Widget> widget) { return &widget->height; }
    inline void setWidgetSize(ee::Ptr<Widget> widget, ee::Float width, ee::Float height)
    {
        widget->width = width;
        widget->height = height;
    }
    // ZGE4's drawWidget step: the widget's screen position its position from
    // its container's anchor point (the widget's Node::anchor): the
    // container's top, middle (half its Height) or bottom, and its left,
    // centre (half its Width) or right. Another anchor sets none. The
    // position's fourth word stays as it was.
    inline void placeAtAnchor(ee::Ptr<Node> widget, ee::Ptr<Node> container)
    {
        const ee::Ptr<Widget> parent = classCast<Widget>(container);
        const zge::Vec4 from = container->screenPosition;
        const ee::Float half(0.5f);
        ee::Float x, y;
        switch (static_cast<WidgetAnchor>(widget->anchor))
        {
        case WidgetAnchor::TopLeft: x = from.x; y = from.y; break;
        case WidgetAnchor::TopRight: x = from.x + parent->width; y = from.y; break;
        case WidgetAnchor::TopCenter: x = from.x + parent->width * half; y = from.y; break;
        case WidgetAnchor::BottomLeft: x = from.x; y = from.y + parent->height; break;
        case WidgetAnchor::BottomRight: x = from.x + parent->width; y = from.y + parent->height; break;
        case WidgetAnchor::BottomCenter: x = from.x + parent->width * half; y = from.y + parent->height; break;
        case WidgetAnchor::MiddleLeft: x = from.x; y = from.y + parent->height * half; break;
        case WidgetAnchor::MiddleRight: x = from.x + parent->width; y = from.y + parent->height * half; break;
        case WidgetAnchor::MiddleCenter: x = from.x + parent->width * half; y = from.y + parent->height * half; break;
        default: return;
        }
        widget->screenPosition.x = x + widget->position.x;
        widget->screenPosition.y = y + widget->position.y;
        widget->screenPosition.z = from.z + widget->position.z;
    }
#else
    void clearWidgetLayout(ee::Ptr<Widget> widget);
    void placeAtAnchor(ee::Ptr<Node> widget, ee::Ptr<Node> container);
    ee::Address widgetWidthAddress(ee::Ptr<Widget> widget);
    ee::Address widgetHeightAddress(ee::Ptr<Widget> widget);
    void setWidgetSize(ee::Ptr<Widget> widget, ee::Float width, ee::Float height);
#endif

    // WidgetLayout: ZGE3's kinds keep their own Alignment, Width and Height
    // (`alignment`, `width`, `height`); ZGE4 (BMX XXX) keeps them in every
    // widget (Node::alignment, Widget::width, Widget::height). Each ZGE4 record
    // names them over its widget (an anonymous union: the widget, and those
    // three at its offsets), so a kind's code reads `kind->width` in both.

    // A ZRGBA_PAL property: the colour, or a name in the palette the
    // Refresh message looks up (a doubleword: the name's packed characters).
    struct WidgetColour
    {
        uint8_t rgba[4];
        uint32_t _04;
        uint64_t paletteName;
    };
    static_assert(sizeof(WidgetColour) == 0x10);

    // The text classes' fields: a text box, a label and the plain text widget.
    struct GuiText
    {
#if PS2X_GAME_BMX
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
        ee::Ptr<UiAsset> asset;  // +0x100 found by charset
        uint32_t text;           // +0x104 with $KEYS
        uint32_t charset;        // +0x108
        ee::Float angle;         // +0x10C
        WidgetColour color;      // +0x110
        ee::Float spacing;       // +0x120
        int32_t tracking;        // +0x124
#if !PS2X_GAME_BMX
        uint32_t alignment;      // +0x128 (ZGE4: the widget's, above)
#endif
        uint32_t monospace;      // +0x12C (ZGE4 +0x128)
#if !PS2X_GAME_BMX
        ee::Float width;         // +0x130 a text box's; a label's measured (ZGE4: the widget's, above)
        ee::Float height;        // +0x134
#endif
        int32_t lineOffset;      // +0x138 (ZGE4 +0x12C) a text box's: the first line drawn
        int32_t lineCount;       // +0x13C (ZGE4 +0x130) the lines it has
        PropertyNode properties[12]; // +0x140 (ZGE4 +0x134) a text box's properties' nodes
    };
    static_assert(offsetof(GuiText, width) == game::readable::perGame(0x130u, 0x130u, 0x48u) &&
                  offsetof(GuiText, alignment) == game::readable::perGame(0x128u, 0x128u, 0x1Cu));
    static_assert(offsetof(GuiText, monospace) == game::readable::perGame(0x12Cu, 0x12Cu, 0x128u) &&
                  offsetof(GuiText, properties) == game::readable::perGame(0x140u, 0x140u, 0x134u));

    // A class's property: its name and type, and where in the class's
    // widget its value is.
    struct ClassProperty
    {
        uint32_t name, type, value;
    };

    // A class's properties defined on a widget, in the table's order: each
    // gets the next of the property nodes the widget's memory keeps for them
    // (the class's `properties`, after its fields).
    template <typename Class, size_t Count>
    void defineClassProperties(ee::Ptr<Class> widget, ee::Ptr<PropertyNode> nodes, const ClassProperty (&rows)[Count])
    {
        const ee::Ptr<uint8_t> fields = classCast<uint8_t>(widget);
        for (size_t i = 0; i < Count; ++i)
            defineProperty(nodes + i, widget, ee::Ptr<const char>(rows[i].name), ee::Ptr<const char>(rows[i].type),
                           fields + rows[i].value);
    }

    // A property: where its node is, its name and type, what it sets.
    struct PropertyRow
    {
        uint32_t property, name, type, value;
    };

    template <size_t Count>
    void defineProperties(ee::Address widget, const PropertyRow (&rows)[Count])
    {
        const uint32_t node = widget.address;
        for (const PropertyRow &row : rows)
            defineProperty(ee::Ptr<Node>(node + row.property), ee::Ptr<Node>(node), ee::Ptr<const char>(row.name),
                                        ee::Ptr<const char>(row.type), node + row.value);
    }

    // A step of a definition in which properties, variables and typed
    // properties mix: which, where the property's node is, its name and type,
    // what it sets.
    enum PropertyKind : uint32_t
    {
        Property,
        Variable,
        Typed,
    };
    struct PropertyStep
    {
        PropertyKind kind;
        uint32_t property, name, type, value;
    };

    template <size_t Count>
    void definePropertySteps(ee::Address widget, const PropertyStep (&steps)[Count])
    {
        const uint32_t node = widget.address;
        for (const PropertyStep &step : steps)
        {
            const ee::Ptr<const char> name(step.name), type(step.type);
            if (step.kind == Property)
                defineProperty(ee::Ptr<Node>(node + step.property), ee::Ptr<Node>(node), name, type, node + step.value);
            else if (step.kind == Variable)
                addVariableNode(ee::Ptr<Node>(node), name, type, node + step.value);
            else
                defineTypedProperty(ee::Ptr<Node>(node), name, type, node + step.value);
        }
    }

    // defineWidgetProperties: what a widget's definition starts with: every
    // widget's own properties (build.cpp).
    uint32_t defineWidgetProperties(ee::Address address, uint32_t name);
    // addListener: `handler` answers `message` on the widget (library.cpp).
    void addListener(ee::Address node, Message message, uint32_t handler);

    // ZGE2 (DM2): a widget kind's record follows WidgetRecord (widget.h), its
    // own fields from +0x60, and its define function takes (record, name, the
    // widget's scene object): defineWidgetRecord, the kind's handlers
    // subscribed on the object (subscribeWidget) and its properties listed on
    // the record (addListedProperties).
    uint32_t defineWidgetRecord(ee::Address address, uint32_t name);

    // subscribeWidget: `handler` answers `message` sent to the widget's scene
    // object (ZGE2's form of addListener): it takes the zge::EngineMessage,
    // the widget's record at its target's +0x3C.
    inline void subscribeWidget(ee::Address object, Message message, uint32_t handler)
    {
        zge::subscribe(object, static_cast<uint32_t>(message), handler);
    }

    // A ZGE2 widget's record (WidgetRecord, widget.h): its scene object's
    // +0x3C, where ZGE3's objects keep their animator.
    inline ee::Ptr<WidgetRecord> widgetRecord(ee::Ptr<zge::SceneObject> object)
    {
        return ee::Ptr<WidgetRecord>(ee::Address(object->animator));
    }
    // A ZGE2 widget's variables (its record's ListedVariables).
    inline ee::Ptr<zge::List> widgetProperties(ee::Ptr<zge::SceneObject> object)
    {
        return ee::Ptr<zge::List>(&widgetRecord(object)->properties);
    }
    // The kind of a ZGE2 widget's scene object (kWidgetObjectKind for a widget's).
    inline uint32_t sceneObjectKind(ee::Ptr<zge::SceneObject> object)
    {
        return static_cast<uint32_t>(object->flags & zge::kObjectKindMask);
    }
    // A ZGE2 widget's Visible: its scene object's shown flag (the bit
    // setSceneObjectShown sets, zge/scene/objects.cpp's kObjectShown).
    constexpr uint64_t kWidgetObjectShown = zge::kObjectMoved;
    inline bool widgetObjectShown(ee::Ptr<zge::SceneObject> object)
    {
        return (object->flags & kWidgetObjectShown) != 0;
    }

    // The record of the widget a ZGE2 handler's message went to.
    template <typename Record>
    ee::Ptr<Record> messageRecord(ee::Ptr<zge::EngineMessage> message)
    {
        return ee::Ptr<Record>(ee::Address(ee::Ptr<zge::SceneObject>(message->target)->animator));
    }

    // A ZGE2 kind's property: its name and type, and where in the kind's
    // record its value is.
    struct ListedProperty
    {
        uint32_t name, type, value;
    };

    // The kind's properties added to the record's list in the table's order
    // (addListedProperty).
    template <typename Record, size_t Count>
    void addListedProperties(ee::Ptr<Record> record, const ListedProperty (&rows)[Count])
    {
        const ee::Ptr<zge::List> properties(valueAddress(classCast<WidgetRecord>(record)->properties));
        for (const ListedProperty &row : rows)
            addListedProperty(properties, ee::Ptr<const char>(row.name), ee::Ptr<const char>(row.type), ee::Address(classCast<uint8_t>(record) + row.value));
    }

    // Where a ZGE2 GetBounds handler writes the widget's rectangle on the
    // screen: the message's argument.
    inline zge::TextBounds &messageBounds(ee::Ptr<zge::EngineMessage> message)
    {
        return *ee::Ptr<zge::TextBounds>(message->data);
    }

    // A rectangle of width by height as an alignment anchors it at the
    // origin: bits 0-3 0 the anchor is the left, 1 the middle, 2 the right;
    // bits 4-7 0x00 the top, 0x10 the middle, 0x20 the bottom. An axis with
    // another value keeps what the argument had (ZGE2's image and frame).
    inline void alignBounds(zge::TextBounds &bounds, uint32_t alignment, ee::Float width, ee::Float height)
    {
        const ee::Float half(0.5f);
        switch (alignment & 0xFu)
        {
        case 0:
            bounds.left = ee::Float(0.0f);
            bounds.right = width;
            break;
        case 1:
            bounds.left = -width * half;
            bounds.right = width * half;
            break;
        case 2:
            bounds.left = -width;
            bounds.right = ee::Float(0.0f);
            break;
        }
        switch (alignment & 0xF0u)
        {
        case 0x00:
            bounds.top = ee::Float(0.0f);
            bounds.bottom = height;
            break;
        case 0x10:
            bounds.top = -height * half;
            bounds.bottom = height * half;
            break;
        case 0x20:
            bounds.top = -height;
            bounds.bottom = ee::Float(0.0f);
            break;
        }
    }

    // The rectangle sized by the widget's matrix (left and right times its
    // x axis's x, top and bottom times its y axis's y: its orientation, and
    // its scale where the kind has one) and moved to the widget's position.
    inline void placeBounds(zge::TextBounds &bounds, const zge::Matrix &matrix, const zge::Vec4 &position)
    {
        bounds.left = bounds.left * matrix.m[0][0];
        bounds.right = bounds.right * matrix.m[0][0];
        bounds.top = bounds.top * matrix.m[1][1];
        bounds.bottom = bounds.bottom * matrix.m[1][1];
        bounds.left = bounds.left + position.x;
        bounds.right = bounds.right + position.x;
        bounds.top = bounds.top + position.y;
        bounds.bottom = bounds.bottom + position.y;
    }

    // ZGE2's tree items (the tree view walks them; build.cpp makes them, tree_view.cpp frees them).
    struct ListedTreeItem
    {
        ee::Ptr<ListedTreeItem> next;       // +0x00 the next item of the same parent
        uint32_t _04;
        ee::Ptr<ListedTreeItem> parent;     // +0x08 (0 at the top: cancel there runs OnCancel)
        uint32_t _0c;
        char text[0x40];                    // +0x10 (the room up to firstChild)
        ee::Ptr<ListedTreeItem> firstChild; // +0x50 the head of its children's List
        uint32_t childrenRest[2];           // +0x54 the rest of that List
    };
    static_assert(offsetof(ListedTreeItem, text) == 0x10 && offsetof(ListedTreeItem, firstChild) == 0x50);
    static_assert(sizeof(ListedTreeItem) == 0x5C);
    // An item's children: a zge::List of items (firstChild is its head).
    inline ee::Ptr<zge::List> childrenOf(ee::Ptr<ListedTreeItem> item)
    {
        return ee::Ptr<zge::List>(valueAddress(item->firstChild));
    }
    // newTreeItem (ZGE2's addTreeTextNode, build.cpp): an item with the text, the last of the parent's children (0:
    // a tree's top item). freeTreeItem (ZGE2 only, tree_view.cpp): the item freed, its children first.
    ee::Ptr<ListedTreeItem> newTreeItem(ee::Ptr<ListedTreeItem> parent, ee::Ptr<const char> text);
    void freeTreeItem(ee::Ptr<ListedTreeItem> item);

    // The room ZGE2's frames give a text style: its own 0x50 bytes (zge::TextStyle keeps ZGE3's later fields past
    // them for the shared text code; ZGE2's draws write none of those).
    using ListedStyleRoom = uint8_t[0x50];

    // ZGE2's frame record, after WidgetRecord (widgets.cpp defines it, box.cpp draws it): nine pictures in
    // reading order, top left to bottom right.
    struct ListedFrame
    {
        WidgetRecord widget;
        ee::Ptr<UiAsset> assets[9]; // +0x60 found by materials (refreshGuiFrame)
        Rgba color;                           // +0x84
        uint32_t alignment;                   // +0x88
        uint32_t materials[9];      // +0x8C
        ee::Float width;                      // +0xB0
        ee::Float height;                     // +0xB4
    };
    static_assert(offsetof(ListedFrame, color) == 0x84 && offsetof(ListedFrame, materials) == 0x8C);
    static_assert(offsetof(ListedFrame, height) == 0xB4);

    // ZGE2's label record, after WidgetRecord (widgets.cpp defines it,
    // text.cpp draws it).
    struct ListedLabel
    {
        WidgetRecord widget;
        ee::Ptr<UiAsset> font; // +0x60 found by charset (refreshLabelFont)
        uint32_t text;         // +0x64
        uint32_t charset;      // +0x68
        Rgba color;            // +0x6C
        ee::Float spacing;     // +0x70
        int32_t tracking;      // +0x74
        uint32_t alignment;    // +0x78
    };
    static_assert(offsetof(ListedLabel, font) == 0x60 && offsetof(ListedLabel, alignment) == 0x78);

    // ZGE2's image record, after WidgetRecord (widgets.cpp defines it,
    // quad.cpp draws it).
    struct ListedImage
    {
        WidgetRecord widget;
        ee::Ptr<UiAsset> asset; // +0x60 found by material (refreshGuiImage)
        Rgba topLeftColor;      // +0x64
        Rgba bottomLeftColor;   // +0x68
        Rgba topRightColor;     // +0x6C
        Rgba bottomRightColor;  // +0x70
        uint32_t material;      // +0x74
        uint32_t alignment;     // +0x78
        uint32_t unused;        // +0x7C made 0 here, as ZGE3's (GuiImage)
        ee::Float width;        // +0x80
        ee::Float height;       // +0x84
        ee::Float uScale;       // +0x88
        ee::Float vScale;       // +0x8C
    };
    static_assert(offsetof(ListedImage, material) == 0x74 && offsetof(ListedImage, vScale) == 0x8C);

    // A ZGE2 widget's matrix: its orientation (quaternionToMatrix normalizes
    // the record's in place), then its scale.
    inline void widgetMatrix(ee::Ptr<zge::Matrix> matrix, WidgetRecord &widget)
    {
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix.get(), &widget.orientation);
        zge::matrixScale(matrix.get(), matrix.get(), &widget.scale.xyz());
    }

    // A widget's default scale, x = y = z = 1.
    inline void setUnitScale(zge::Vec4 &scale)
    {
        zge::vec3SetOne(reinterpret_cast<zge::Vec3 *>(&scale));
    }

    // A colour named in the palette takes the palette's value.
    inline void lookUpPaletteColour(WidgetColour &colour)
    {
        if (colour.paletteName)
            findPaletteColour(colour.paletteName, ee::Ptr<uint8_t>(colour.rgba));
    }

    inline void setWhite(WidgetColour &colour)
    {
        colour.rgba[0] = colour.rgba[1] = colour.rgba[2] = colour.rgba[3] = 255;
        colour.paletteName = 0;
    }

    inline uint8_t faded(uint8_t channel)
    {
        return static_cast<uint8_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(channel)) * g_zgui->alpha));
    }

    // A UI font asset's line height, in texels (a font starts with it).
    inline int32_t fontHeight(ee::Ptr<UiAsset> asset)
    {
        return *ee::Ptr<uint8_t>(asset->object);
    }

    // The input widgets' direction buttons, in Input's masks.
    constexpr uint32_t kButtonUp = 0x1, kButtonDown = 0x2, kButtonLeft = 0x4, kButtonRight = 0x8;
}

namespace zge::zgui
{
    // (library.cpp)
    void registerZguiLibraryChunkHandlers();
    ee::Ptr<zge::Material> newUiDefaultMaterial();
    void setupUiMaterial(ee::Ptr<zge::Material> material, uint32_t flags);
    int32_t loadZguiLibrary(ee::Address name, ee::Address path, ee::Address onLoaded);
    int32_t loadUiAssetLibraryFile(ee::Address name, ee::Address path, ee::Address onLoaded);
    int32_t loadUiImageFile(ee::Address name, ee::Address path, ee::Address onLoaded);
    uint32_t findUiLibrary(uint32_t name);
    uint32_t unloadUiLibrary(uint32_t name);
    // A library loaded by name and counted (the name's address); its count back, the last one away unloading what
    // the load loaded.
    int32_t loadUiLibraryByName(uint32_t name, uint32_t file);
    uint32_t unloadUiLibraryByName(uint32_t name);
    int32_t reportUiLibraries();
    int32_t rebuildMaterialRegisters(ee::Ptr<zge::Material> material, uint32_t materialTable, uint32_t layer, uint32_t value);
}

namespace zge::zgui
{
    // (widgets_small.cpp)
    void registerWidgetKinds();
}
