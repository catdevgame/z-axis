#pragma once

// zgui, the game's UI: a tree of nodes built by the UI scripts (UIBuilder,
// assets\ui\*.zsc): widgets (menus, texts, the HUD's meters), and under
// them the scripts' message handlers. Drawing walks the tree each frame
// (drawWidget) and sends every visible widget the draw messages, which
// its handler nodes answer.

#include "ee/callback.h"
#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/container/list.h"
#include "zge/math/types.h"

#include <cstddef>
#include "sym/zge/zgui/widget.h.h"

namespace zge
{
    struct SceneObject; // zge/scene/scene.h
    struct StringTable; // zge/text/strings.h
}

namespace zge
{
    struct Heap;
}

namespace zge::zgui
{
    enum class NodeKind : uint16_t
    {
        Widget = 1,  // drawn, positioned in its parent widget
        Variable = 2, // a typed value (script.h)
        Handler = 3, // a message handler of its parent
        Animation = 4, // what PLAY plays on a widget (script.h Animation)
    };

    // Node::kindAndFlags' top bits: what destroyNode leaves to the owner.
    constexpr uint16_t kNodeStaticName = 0x4000u; // the name is not zgui's copy
    constexpr uint16_t kNodeEmbedded = 0x8000u;   // the node is part of its owner's memory

    // initNode's kindAndFlags: the kind in the low four bits, and these.
    constexpr uint32_t kInitStaticName = 0x10000u; // the name is the caller's (not copied)
    constexpr uint32_t kInitEmbedded = 0x20000u;   // the node is part of the caller's memory (not freed)

    // Messages zgui sends a widget (sendMessage): drawing it, and the
    // scripts' commands to the tree's root and to a widget. ZGE2 (DM2) sends
    // them through the scene messages (zge/scene subscribe) and numbers them
    // from 0x8000 with messages of its own between (read in DM2's root and
    // widget subscriptions, initZgui and the define* functions, and in its
    // commands' sends; PLAY sends 0x8000, STOP 0x8001). ZGE4 (BMX XXX) numbers
    // them from 1 in ZGE3's order: LockInput 1 .. GetFocus 6, then Draw 7
    // (ZGE3's 0x8009) .. Destroy 10 (read in its commands' sends, initZgui's
    // subscriptions and the define* functions').
    enum class Message : uint16_t
    {
        LockInput = game::readable::perGame(0x8001u, 0x8003u, 1u),   // LOCKINPUT a b (argument: a | b << 2)
        UnlockInput = game::readable::perGame(0x8002u, 0x8004u, 2u), // UNLOCKINPUT a (argument: a)
        Input = game::readable::perGame(0x8003u, 0x8005u, 3u),       // the focus widget's input (argument: Input *)
        SetFocus = game::readable::perGame(0x8004u, 0x8006u, 4u),    // SETFOCUS (argument: the widget, 0 for KILLFOCUS)
        LostFocus = game::readable::perGame(0x8005u, 0x8007u, 5u),   // the focus left the widget
        GetFocus = game::readable::perGame(0x8006u, 0x8008u, 6u),    // the focus came to the widget
        Draw = game::readable::perGame(0x8009u, 0x800Bu, 7u),        // before its children
        DrawAfter = game::readable::perGame(0x800Au, 0x800Cu, 8u),   // after its children
        Refresh = game::readable::perGame(0x800Bu, 0x800Du, 9u), // REFRESH, and a ZAssetID property changed: the widget reloads what it shows
        // ZGE2 only (ZGE3 sends no 0x800E): the widget writes its rectangle on the screen (zge::TextBounds:
        // left, top, right, bottom) to the argument (a label its text's bounds, an image and a scroll view their
        // size from where their alignment puts them, a line its ends with a margin of 2).
        GetBounds = 0x800E,
        // ZGE2 only, to the root (argument: a ListedAction): PLAY's and
        // QUEUE's message: the ACTION starts playing.
        PlayAction = 0x8000,
        // ZGE2 only, to the root (argument: a ListedAction): STOP's message:
        // the ACTION, if it is playing, is over.
        StopAction = 0x8001,
        // ZGE2 only, to the root (argument: a ListedAction): the ACTION is
        // going away; if it is playing it is ended (OnEnd, its kind's end).
        EndAction = 0x8010,
        Destroy = game::readable::perGame(0x800Cu, 0x800Fu, 0xAu), // DESTROY (argument: the widget): the root queues it
    };

    // A pointer to a node, or any record, as the class it is: a widget as its
    // Node, a Node as its widget.
    template <typename To, typename From>
    ee::Ptr<To> classCast(ee::Ptr<From> pointer)
    {
        return pointer.template cast<To>();
    }

    // A field's address, as a property or a variable keeps what it sets.
    template <typename T>
    ee::Address valueAddress(T &field)
    {
        return ee::Address(&field);
    }

    struct VariableType; // script.h

    // ZGE2 (DM2) keeps a variable as an entry of its owner's list, not as a tree node: 0x18 bytes from zgui's heap
    // (DM2's defineTypedProperty), found by its name's hash and freed with its value when undeclared. Its type's
    // methods take this record where ZGE3's take the Variable node (Node::valueAs reads it).
    struct ListedVariable
    {
        zge::ListNode link;                 // +0x00 the owner's list
        uint32_t nameHash;                  // +0x08 nameHash(name)
        uint32_t value;                     // +0x0C where its value is
        uint32_t ownsValue;                 // +0x10 non-zero: the value is freed with it (0 for a property)
        ee::Ptr<VariableType> variableType; // +0x14
    };
    static_assert(sizeof(ListedVariable) == 0x18 && offsetof(ListedVariable, value) == 0x0C &&
                  offsetof(ListedVariable, variableType) == 0x14);

    // ZGE2 (DM2) makes a widget a scene object (zge/scene SceneObject: kind 4, kWidgetObjectKind, and its class
    // the widget kind's) with this record at the object's +0x3C, of zgui's memory (DM2's newZguiWidget):
    // its place, its ACTIONs and its properties as lists. Each kind's own fields follow it (from +0x60).
    constexpr uint32_t kWidgetObjectKind = 4; // (DM2's newZguiWidget sets it, initTimedNode tests it)
    struct WidgetRecord
    {
        zge::Vec4 position;          // +0x00 (DM2's refreshGuiLabel and drawGuiClipper draw it there)
        zge::Quaternion orientation; // +0x10
        zge::Vec4 scale;             // +0x20 "Scale"
        zge::List actions;           // +0x30 its ACTIONs (ListedAction), found by name by PLAY
        ee::Ptr<const char> kind;    // +0x3C its kind's name (newZguiWidget)
        zge::List properties;        // +0x40 its ListedVariables
        ee::Ptr<const char> name;    // +0x4C "Name"
        ee::Ptr<char> onCreate;      // +0x50 "OnCreate"
        ee::Ptr<char> onDestroy;     // +0x54 "OnDestroy"
    };
    static_assert(offsetof(WidgetRecord, scale) == 0x20 && offsetof(WidgetRecord, actions) == 0x30 &&
                  offsetof(WidgetRecord, kind) == 0x3C && offsetof(WidgetRecord, properties) == 0x40);
    static_assert(offsetof(WidgetRecord, name) == 0x4C && offsetof(WidgetRecord, onDestroy) == 0x54);
    // A kind's own fields start at +0x60 (DM2's define* functions; none of them writes +0x58 or +0x5C).
    static_assert(sizeof(WidgetRecord) == 0x60);

    struct Node;
    // A message's handler: (widget, argument) -> a result its callers (sendMessage, fireGuiEventHandlers) drop.
    using MessageHandler = ee::Callback<uint64_t(ee::Ptr<Node>, uint32_t)>;

    struct Node
    {
        ee::Ptr<Node> nextSibling; // +0x00
        ee::Ptr<Node> firstChild;  // +0x04
        ee::Ptr<Node> parent;      // +0x08
#if PS2X_GAME_BMX
        // +0x0C ZGE4 (BMX XXX) keeps the kind, the two ownership flags, the
        // widget's anchor and the message in one word (its initNode, sendMessage,
        // addListener and destroyNode read and write these bits).
        uint32_t kindBits : 4;    // NodeKind
        uint32_t staticName : 1;  // the name is not zgui's copy
        uint32_t embedded : 1;    // the node is part of its owner's memory
        uint32_t anchor : 8;      // a widget's ANCHOR_TYPE (the script reader sets it; 0 TopLeft)
        uint32_t message : 16;    // a Handler's message (Message or the scripts' own)
        uint32_t unused : 2;      // (no function reads or writes them)
#else
        // +0x0C the node's kind in the low 14 bits (NodeKind); the top two
        // bits are flags.
        uint16_t kindAndFlags;
        uint16_t message;          // +0x0E a Handler's message (Message or the scripts' own)
#endif
        uint32_t nameHash;         // +0x10 nameHash(name): paths are looked up by it
        ee::Ptr<const char> name;  // +0x14
        union // +0x18, by the node's kind
        {
            uint32_t shown;         // a Widget: non-zero while shown
            MessageHandler handler; // a Handler: what handles its message
            uint32_t value;         // a Variable: where its value is (valueAs)
        };
        union // +0x1C, by the node's kind
        {
            ee::Ptr<VariableType> variableType; // a Variable: what its value is
            ee::Ptr<const char> type;           // a widget's class: its kind's name
            ee::Ptr<const char> itemText;       // a tree view's item (kind 7): its text
#if PS2X_GAME_BMX
            // A ZGE4 (BMX XXX) widget's Alignment (bits 0-3 horizontal, 4-7
            // vertical: widget_parts.h's alignBounds): ZGE4 keeps no kind
            // name, and every kind with an Alignment property points it here
            // (ZGE3's kinds keep their own).
            uint32_t alignment;
#endif
        };
        zge::Vec4 position;        // +0x20 in its parent widget
        zge::Vec4 screenPosition;  // +0x30 the parent widget's + position (set when drawn)

#if PS2X_GAME_BMX
        NodeKind kind() const { return static_cast<NodeKind>(kindBits); }
        bool nameIsStatic() const { return staticName; }
        bool isEmbedded() const { return embedded; }
#else
        NodeKind kind() const { return static_cast<NodeKind>(kindAndFlags & 0x3FFFu); }
        bool nameIsStatic() const { return kindAndFlags & kNodeStaticName; }
        bool isEmbedded() const { return kindAndFlags & kNodeEmbedded; }
#endif
        // A Variable's value, as its type keeps it (ZGE2: the ListedVariable's).
        template <typename T>
        ee::Ptr<T> valueAs() const
        {
            if constexpr (game::readable::kIsDm2)
                return ee::Ptr<T>(ee::Ptr<const Node>(this).cast<const ListedVariable>()->value);
            else
                return ee::Ptr<T>(value);
        }
    };
    // ZGE4's widget anchors (the script's ANCHOR_TYPE; Node::anchor), in the
    // order the script reader looks their names up.
    enum class WidgetAnchor : uint8_t
    {
        TopLeft, TopRight, TopCenter, BottomLeft, BottomRight, BottomCenter, MiddleLeft, MiddleRight, MiddleCenter,
    };
    // A widget given its anchor: ZGE4's alone (ZGE3 and ZGE2 keep none; only
    // ZGE4's script reader calls it).
#if PS2X_GAME_BMX
    inline void setNodeAnchor(ee::Ptr<Node> node, WidgetAnchor anchor) { node->anchor = static_cast<uint32_t>(anchor); }
#else
    void setNodeAnchor(ee::Ptr<Node> node, WidgetAnchor anchor);
#endif

    // initNode's kind and flags (kInitStaticName, kInitEmbedded) given to a
    // node (ZGE4: its anchor cleared). Always inlined: AI's initNode then
    // compiles as it did with the store written in it.
#if PS2X_GAME_BMX
    [[gnu::always_inline]] inline void setNodeKind(ee::Ptr<Node> node, uint32_t kindAndFlags)
    {
        node->anchor = 0;
        node->kindBits = kindAndFlags & 0xFu;
        node->staticName = (kindAndFlags & kInitStaticName) != 0;
        node->embedded = (kindAndFlags & kInitEmbedded) != 0;
    }
#else
    static_assert(offsetof(Node, kindAndFlags) == 0x0C && offsetof(Node, message) == 0x0E);
    [[gnu::always_inline]] inline void setNodeKind(ee::Ptr<Node> node, uint32_t kindAndFlags)
    {
        node->kindAndFlags = static_cast<uint16_t>((kindAndFlags & 0xFu) | ((kindAndFlags >> 2) & (kNodeStaticName | kNodeEmbedded)));
    }
#endif
    static_assert(offsetof(Node, nameHash) == 0x10 && offsetof(Node, name) == 0x14 && offsetof(Node, shown) == 0x18);
    static_assert(offsetof(Node, position) == 0x20 && offsetof(Node, screenPosition) == 0x30);

    // A pad as zgui reads it (the game's pad state).
    struct Pad
    {
        uint32_t _00;
        uint32_t flags;   // +0x04 bit 0: connected
        uint32_t down;    // +0x08 buttons held
        uint32_t pressed; // +0x0C buttons pressed this frame
        uint32_t _10;
        ee::Float stickX; // +0x14 -1 .. 1
        ee::Float stickY; // +0x18
        ee::Float rightStickX; // +0x1C
        ee::Float rightStickY; // +0x20
    };
    static_assert(offsetof(Pad, stickX) == 0x14 && offsetof(Pad, rightStickY) == 0x20);

    // Each pad's left stick as last pressed (x, y; 0 once let go): what
    // updateZgui and the key sniffer press the d-pad by.
    struct Stick
    {
        ee::Float x, y;
    };
#if PS2X_GAME_DM2
    // (DM2's own row: the bind table pairs AI's g_sticks with a word that is no data of DM2's.)
    inline ee::Global<Stick[4], sym::dm2::g_sticks> g_sticks;
#else
    inline ee::Global<Stick[4], sym::g_sticks> g_sticks;
#endif

    // zgui's input from one pad: held buttons repeat while held.
    struct PadInput
    {
        ee::Ptr<Pad> pad;          // +0x00 0: none
        uint32_t mask;             // +0x04 the buttons zgui takes from it
        ee::Float repeat[32];      // +0x08 per button: seconds to its next repeat
        uint32_t down;             // +0x88
        uint32_t pressed;          // +0x8C pressed this frame, or repeated
    };
    static_assert(sizeof(PadInput) == 0x90);

    // The Input message's argument (zgui globals +0x328): every pad's.
    struct Input
    {
        uint32_t down;
        uint32_t pressed;
        uint32_t idleFrames; // frames without a button down
    };

    using SoundHook = ee::Callback<void()>;

    // zgui's globals (g_zgui), as far as they are known.
    struct Zgui
    {
        uint32_t initialised;     // +0x000 non-zero between initZgui and shutdownZgui
        ee::Ptr<zge::Heap> heap;  // +0x004
        uint32_t assets;          // +0x008 the reader of the UI's archive (loadZguiScript)
        ee::Ptr<Node> root;       // +0x00C
        ee::Ptr<Node> focus;      // +0x010 the widget that gets the input
        uint32_t defaultTexture;  // +0x014
        ee::Float alpha;          // +0x018 colours are scaled by it (1/128 units, inferred)
        PadInput pads[4];         // +0x01C
#if PS2X_GAME_BMX
        // ZGE4 (BMX XXX): its game code keeps the UI's camera and string
        // table here (setUpUi clears them).
        ee::Ptr<zge::SceneObject> camera;  // +0x25C
        ee::Ptr<zge::StringTable> strings; // +0x260 the UI's string table
#else
        uint8_t _25c[8];
#endif
        uint32_t stickAsDpad;     // +0x264 the sticks press the d-pad's directions
#if PS2X_GAME_DM2
        uint32_t _268;            // DM2 +0x268 (not seen used; ZGE2 keeps no frame time here)
#elif PS2X_GAME_BMX
        ee::Float seconds;        // +0x268 the frame's time
        int32_t fileOperationCount; // +0x26C the root's FileOperationCount (initZgui; updateZgui counts it)
        uint32_t platformId;      // +0x270 the root's PlatformID (a String, "PS2")
#else
        ee::Float seconds;        // +0x268 the frame's time
        uint32_t _26c;
#endif
        // +0x270 (DM2 +0x26C, BMX XXX +0x274) the UI's sound hooks (0:
        // silent; registerSoundMenuFunctions sets them).
        SoundHook selectSound, cancelSound, moveSound, changeSound, blockedSound;
#if PS2X_GAME_DM2
        // DM2 +0x280: the widget kinds' list, the node kinds, the variable
        // types, the script functions, the UI libraries, the playing list and
        // the pending loads (each its own global); ZGE2 has no destroy queue.
        uint8_t _280[0x70];
#else
        zge::List destroyQueue;   // +0x284 (BMX XXX +0x288) DestroyRows: the nodes to destroy after the frame
        uint8_t _290[0x80];
#endif
        uint32_t startButton;     // +0x310 (DM2 +0x2F0, BMX XXX +0x314) the Input mask that starts
        uint32_t acceptButton;    // +0x314 (DM2 +0x2F4) the Input mask that accepts (initGame sets them)
        uint32_t optionButton;    // +0x318 (DM2 +0x2F8, BMX XXX +0x31C)
#if PS2X_GAME_BMX
        // ZGE4's second option mask (the key sniffer's OnOption2), before the
        // cancel mask.
        uint32_t option2Button;   // +0x320
#endif
        uint32_t cancelButton;    // +0x31C (DM2 +0x2FC, BMX XXX +0x324)
        uint8_t _320[8];
        Input input;              // +0x328 (DM2 +0x308, BMX XXX +0x330)
#if PS2X_GAME_DM2
        // ZGE3's, which ZGE2 keeps nowhere here: past DM2's globals, for the
        // shared code that names them (DM2 runs none of it).
        ee::Float seconds;
        zge::List destroyQueue;
#endif
    };
    static_assert(offsetof(Zgui, pads) == 0x1C && offsetof(Zgui, stickAsDpad) == 0x264);

    // A node queued to be destroyed after the frame (Zgui::destroyQueue).
    struct DestroyRow
    {
        zge::ListNode link; // +0x00
        ee::Ptr<Node> node; // +0x08
    };
    static_assert(sizeof(DestroyRow) == 12);
    static_assert(offsetof(Zgui, input) == game::readable::perGame(0x328u, 0x308u, 0x330u));
    static_assert(offsetof(Zgui, selectSound) == game::readable::perGame(0x270u, 0x26Cu, 0x274u) &&
                  offsetof(Zgui, blockedSound) == game::readable::perGame(0x280u, 0x27Cu, 0x284u));
    static_assert(offsetof(Zgui, startButton) == game::readable::perGame(0x310u, 0x2F0u, 0x314u) &&
                  offsetof(Zgui, acceptButton) == game::readable::perGame(0x314u, 0x2F4u, 0x318u) &&
                  offsetof(Zgui, cancelButton) == game::readable::perGame(0x31Cu, 0x2FCu, 0x324u));
#if !PS2X_GAME_DM2
    static_assert(offsetof(Zgui, destroyQueue) == game::readable::perGame(0x284u, 0x284u, 0x288u));
#endif
#if PS2X_GAME_BMX
    static_assert(offsetof(Zgui, camera) == 0x25C && offsetof(Zgui, platformId) == 0x270 &&
                  offsetof(Zgui, option2Button) == 0x320);
#endif
    inline ee::Global<Zgui, sym::g_zgui> g_zgui;
    // ZGE4's root FileOperationCount set (its updateZgui's step; ZGE3 keeps
    // none: only declared there, no ZGE3 step calls it).
#if PS2X_GAME_BMX
    inline void setFileOperationCount(Zgui &zgui, int32_t count) { zgui.fileOperationCount = count; }
#else
    void setFileOperationCount(Zgui &zgui, int32_t count);
#endif

    // Messages zgui sends besides drawing.
    constexpr uint32_t kInputMessage = static_cast<uint32_t>(Message::Input); // the focus widget's input (argument: Input *)

    // updateZgui: zgui's frame: the time since the last, the animations
    // advanced, the pads read into the focus widget's Input, the queued
    // nodes destroyed. 1.
    int32_t updateZgui();

    // sendMessage: runs the widget's handler for the message (its first
    // Handler child with that message) as handler(widget, argument). (The
    // original hands back the handler's result, or what its search last
    // loaded; none of its callers reads it.)
    void sendMessage(ee::Ptr<Node> widget, uint32_t message, ee::Address argument);
    constexpr uint32_t kSendMessage = sym::sendMessage;

    // initNode: a node set up unlinked: its kind (kindAndFlags' low
    // four bits; 0x10000 keeps the caller's name, else zgui's heap gets a
    // copy; 0x20000 marks it part of its owner's memory), its name and the
    // name's hash (returned).
    int32_t initNode(ee::Ptr<Node> node, ee::Ptr<const char> name, uint32_t kindAndFlags);
    // unlinkNode: the node taken out of its parent's children.
    void unlinkNode(ee::Ptr<Node> node);
    // appendChildNode: `child` made the parent's last child.
    void appendChildNode(ee::Ptr<Node> parent, ee::Ptr<Node> child);
    // nthChildOfKind: the parent's child of `kind` at `index` among those of
    // its kind, or 0.
    ee::Ptr<Node> nthChildOfKind(ee::Ptr<Node> node, uint32_t index, uint32_t kind);
    // countChildrenOfKind: how many children of `kind` the node has.
    int32_t countChildrenOfKind(ee::Ptr<Node> node, uint32_t kind);
    // destroyNode: the node and every node below it destroyed (what each
    // kind holds released, the node unlinked and freed unless its owner's).
    int32_t destroyNode(ee::Ptr<Node> node);
    // fireGuiEventHandlers: `message` to every handler for it among the node's
    // children, and on down through the widgets below.
    void fireGuiEventHandlers(ee::Ptr<Node> node, uint32_t message, uint32_t argument);
    // refreshWidgetTree: ZGE2's fireGuiEventHandlers (nodes.cpp): Refresh to a
    // widget's scene object and every object hanging from it.
    uint32_t refreshWidgetTree(ee::Ptr<zge::SceneObject> object, ee::Address context);

    // forEachChild2: visit(child) for each child of the node, in order.
    // `visit` is a game function's address.
    void forEachChild(ee::Ptr<Node> node, uint32_t visit);

    // drawWidget: draws a widget and the widgets below it.
    void drawWidget(ee::Ptr<Node> widget);
    constexpr uint32_t kDrawWidget = sym::drawWidget;
}

namespace zge::zgui
{
    // (nodes.cpp)
    int32_t variableToText(ee::Ptr<Node> variable, ee::Ptr<char> out);
    int32_t variableIntValueAgain(ee::Ptr<Node> variable, ee::Ptr<int32_t> out);
}

namespace zge::zgui
{
    // (build.cpp)
    ee::Ptr<Node> loadZguiScriptNode(ee::Address path, ee::Ptr<Node> parent);
    ee::Ptr<Node> loadZguiScriptText(ee::Address text, uint32_t bytes, ee::Ptr<Node> parent); // ZGE4 only (build.cpp)
    void setZguiPad(ee::Ptr<Pad> pad, uint32_t port);
    ee::Ptr<Node> addTreeTextNode(ee::Ptr<Node> parent, ee::Address text);
    int32_t destroyGuiNodeMethod(ee::Ptr<Node> node);
    int32_t registerZguiNodeKind(uint32_t name, uint32_t bytes, uint32_t create);
    uint32_t initTimedNode(ee::Address address, ee::Address name, ee::Ptr<Node> parent);
    int32_t runNodeDestroyScript(ee::Address widget);
    int32_t setNodeValueByPath(ee::Address path, ee::Address value);
    int32_t runOnGetFocus(ee::Address widget);
    int32_t runOnLostFocus(ee::Address widget);
}

namespace zge::zgui
{
    // (system.cpp)
    uint32_t initZgui(uint32_t bytes);
    uint32_t shutdownZgui();
    uint32_t registerWidgetKind(uint32_t name, uint32_t bytes, uint32_t flags, uint32_t define READABLE_PARAM_ONLY(dm2, uint32_t objectClass));
    ee::Ptr<Node> newZguiWidget(ee::Address kindName, ee::Address name);
    ee::Ptr<Node> loadZguiScript(ee::Address fileName, ee::Ptr<Node> parent);
    uint32_t parseZguiTokens(ee::Ptr<char> out, int32_t mode);
}

namespace zge::zgui
{
    // (nodes.cpp)
    int32_t variableFloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out);
}

namespace zge::zgui
{
    // (build.cpp)
    ee::Ptr<Pad> zguiPad(uint32_t port);
}
