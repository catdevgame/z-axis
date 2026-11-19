// zgui's trees as the UI scripts build them (.zsc: OBJECT and ACTION blocks,
// each `name value` a property), the node kinds the ACTIONs name, what every
// widget has, and the root's handlers for focus and the pads.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/heaps.h"
#include "zge/text/cstring.h"
#include "zge/memory/regions.h"
#include "zge/container/list.h"
#include "zge/text/token_reader.h"
#include "zge/assets/file.h"
#include "zge/math/quaternion.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include "sym/zge/zgui/build.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kNewNode = sym::newZguiNode;          // newZguiNode (kind, name, parent)
        constexpr uint32_t kSetVariable = sym::setVariable;      // by address: a game override hooks it

        constexpr uint32_t kObjectWord = sym::g_textObject2;       // "OBJECT"
        constexpr uint32_t kActionWord = sym::g_textAction2;       // "ACTION"
        constexpr uint32_t kOnCreateName = sym::g_textOnCreate;     // "OnCreate"
        constexpr uint32_t kStringType = sym::g_textString3;       // "String"
        constexpr uint32_t kTreeNodeName = sym::g_textTreeNode;     // "TreeNode"
        constexpr uint32_t kTextName = sym::g_textText2;         // "Text"
        constexpr uint32_t kOnEndName = sym::g_textOnEnd, kOnStartName = sym::g_textOnStart2, kOnUpdateName = sym::g_textOnUpdate;
        constexpr uint32_t kNameName = sym::g_textName3, kNameType = sym::g_textString5;
        constexpr uint32_t kStartDelayName = sym::g_textStartDelay, kZFloatType = sym::g_textZFloat3, kEventType = sym::g_textEvent2;
        constexpr uint32_t kWidgetNameName = sym::g_textName4, kWidgetNameType = sym::g_textString6;
        constexpr uint32_t kOnCreateProperty = sym::g_textOnCreate2, kWidgetEventType = sym::g_textEvent3, kOnDestroyName = sym::g_textOnDestroy;
        constexpr uint32_t kVisibleName = sym::g_textVisible2, kBoolType = sym::g_textBool3;
        constexpr uint32_t kPositionName = sym::g_textPosition2, kZ3DVectorType = sym::g_textZ3DVector3;
        constexpr uint32_t kOnGetFocusName = sym::g_textOnGetFocus2, kOnLostFocusName = sym::g_textOnLostFocus2;
        // ZGE2's widgets have these three properties besides (defineWidgetRecord).
        constexpr uint32_t kOrientationName = sym::dm2::g_textOrientation, kZQuaternionType = sym::dm2::g_textZQuaternion;
        constexpr uint32_t kScaleName = sym::dm2::g_textWidgetScale;

        constexpr uint32_t kScriptBytes = 0x4000;           // a property's text can be this long
        constexpr uint32_t kTreeNodeKind = 0x00010007u;     // initNode's: a tree node that owns its name
        constexpr uint32_t kTreeNodeBytes = 0x20;           // only a Node's first fields

        // The file being read. A nested OBJECT goes on from where its parent
        // was: the parser's state is these globals.
        inline ee::Global<uint32_t, sym::g_wholeFile> g_wholeFile;
        inline ee::Global<uint32_t, sym::g_parseCursor> g_parseCursor;
        inline ee::Global<uint32_t, sym::g_parseEnd> g_parseEnd;
        inline ee::Global<ee::Ptr<char>, sym::g_scriptText> g_scriptText;
        inline ee::Global<uint8_t[2], sym::g_openBrace> g_openBrace;   // "{"
        inline ee::Global<uint8_t[2], sym::g_closeBrace> g_closeBrace;  // "}"

        constexpr uint32_t kNodeKinds = sym::g_nodeKinds;
        inline ee::Global<uint32_t, sym::g_nodeKinds> g_nodeKinds;

        // A widget's Event properties, the nodes of its properties.
        constexpr uint32_t kOnCreate = 0x40, kOnDestroy = 0x44;
        constexpr uint32_t kNameProperty = 0x50, kOnCreateVariable = 0x70, kOnDestroyVariable = 0x90;
        constexpr uint32_t kVisibleVariable = 0xB0, kPositionVariable = 0xD0;
    }

    ee::Ptr<Animation> newZguiNode(ee::Address kind, ee::Address name, ee::Ptr<Node> parent);

    namespace
    {
        // ZGE4's ANCHOR_TYPE word and its values' names, in WidgetAnchor's order.
        constexpr uint32_t kAnchorTypeWord = sym::bmx::g_textAnchorType; // "ANCHOR_TYPE"
        constexpr uint32_t kAnchorNames[] = {
            sym::bmx::g_textTopLeft, sym::bmx::g_textTopRight, sym::bmx::g_textTopCenter,
            sym::bmx::g_textBottomLeft, sym::bmx::g_textBottomRight, sym::bmx::g_textBottomCenter,
            sym::bmx::g_textMiddleLeft, sym::bmx::g_textMiddleRight, sym::bmx::g_textMiddleCenter,
        };
    }

    // loadZguiScriptNode: a .zsc file's tree built under `parent` (0: a free
    // widget), or, with no path, the rest of the block being read: the
    // OBJECTs in it, each built and its OnCreate run; the ACTIONs in it with
    // their properties; every other `name value` a property of `parent`.
    // The last OBJECT is the answer (it gets the Refresh message when its
    // file is done). 0 if the file is missing or a block's brace is.
    ee::Ptr<Node> loadZguiScriptNode(ee::Address path, ee::Ptr<Node> parent)
    {
        ee::Local<char[0x40]> second;   // OBJECT's and ACTION's kind
        ee::Local<char[0x410]> token;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x10]> openBrace;
        ee::Local<char[0x10]> closeBrace;
        ee::Local<uint32_t> fileBytes;
        openBrace[0] = static_cast<char>((*g_openBrace)[0]);
        openBrace[1] = static_cast<char>((*g_openBrace)[1]);
        closeBrace[0] = static_cast<char>((*g_closeBrace)[0]);
        closeBrace[1] = static_cast<char>((*g_closeBrace)[1]);
        ee::Ptr<Node> widget;
        if (path != 0)
        {
            const uint32_t file = zge::loadFile(path, fileBytes);
            *g_wholeFile = file;
            *g_parseCursor = file;
            if (file == 0)
                return {};
            const uint32_t heap = *g_heap;
            *g_parseEnd = file + *fileBytes;
            *g_scriptText = ee::Ptr<char>(zge::heapTake(ee::Ptr<zge::Heap>(heap), kScriptBytes));
        }
        // The way back round: a property's call ends in a block of its own.
        bool afterProperty = true;
        for (;;)
        {
            if (parseZguiTokens(token, 0) == 0)
                break;
            if (zge::stringCompareNoCase(token, kObjectWord) == 0)
            {
                parseZguiTokens(second, 1);
                parseZguiTokens(name, 1);
                widget = newZguiWidget(second, name);
                if (parseZguiTokens(openBrace, 2) == 0)
                    return {};
                if (parent)
                    appendChildNode(parent, widget);
                loadZguiScriptNode(0, widget); // the OBJECT's own block
                copyVariable(widget, ee::Ptr<const char>(kOnCreateName), token);
                afterProperty = token[0] == 0;
                if (!afterProperty)
                    runScript(widget, token, 0);
                continue;
            }
            if (zge::stringCompareNoCase(token, kActionWord) == 0)
            {
                parseZguiTokens(second, 1);
                parseZguiTokens(name, 1);
                const ee::Ptr<Animation> action = newZguiNode(second, name, parent);
                const int32_t opened = parseZguiTokens(openBrace, 2);
                if (opened == 0)
                    return {};
                ee::Ptr<char> text;
                for (bool first = true;; first = false)
                {
                    if (!first)
                    {
                        parseZguiTokens(text, 1);
                        setVariableText(action, token, *g_scriptText);
                    }
                    parseZguiTokens(token, 0);
                    const int32_t differs = zge::stringCompareNoCase(token, closeBrace);
                    text = *g_scriptText;
                    if (differs == 0)
                        break;
                }
                afterProperty = false;
                continue;
            }
            const int32_t notEnd = zge::stringCompareNoCase(token, closeBrace);
            if (notEnd == 0)
                break;
            const ee::Ptr<char> text = *g_scriptText;
            parseZguiTokens(text, 1);
            setVariableText(parent, token, *g_scriptText);
            afterProperty = true;
        }
        if (path != 0)
        {
            zge::freeLoadedFile(*g_wholeFile);
            zge::heapGive(ee::Ptr<zge::Heap>(*g_heap), *g_scriptText);
            if (widget != 0)
                fireGuiEventHandlers(ee::Ptr<Node>(widget), static_cast<uint32_t>(Message::Refresh), 0);
        }
        return widget;
    }
    READABLE_FUNCTION_WAITS_OWN(loadZguiScriptNode, Verified, dm2, loadObjectScript, bmx, loadZguiScriptFile);

    // loadZguiScriptText: ZGE4's .zsc reader (its loadZguiScriptNode and
    // loadZguiScript read the script whole and hand its text here): `text`,
    // `bytes` long, begins the parse; 0 goes on in the text being read (a
    // nested block). OBJECT kind name { } makes a widget under `parent`, its
    // block read with it as the parent, then its OnCreate run; ACTION kind
    // name { } an animation of the parent, each line of its block one of its
    // properties; ANCHOR_TYPE name gives the parent that anchor; any other
    // word is a property of the parent, set from the rest of its line; "}"
    // ends the block. The last widget made is the answer (it gets the Refresh
    // message when a parse begun here is done); 0 if a block's brace is
    // missing.
    inline ee::Ptr<Node> loadZguiScriptText(ee::Address text, uint32_t bytes, ee::Ptr<Node> parent)
    {
        ee::Local<char[0x40]> second;   // OBJECT's and ACTION's kind
        ee::Local<char[0x400]> token;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x10]> openBrace;
        ee::Local<char[0x10]> closeBrace;
        openBrace[0] = static_cast<char>((*g_openBrace)[0]);
        openBrace[1] = static_cast<char>((*g_openBrace)[1]);
        closeBrace[0] = static_cast<char>((*g_closeBrace)[0]);
        closeBrace[1] = static_cast<char>((*g_closeBrace)[1]);
        ee::Ptr<Node> widget;
        if (text != 0)
        {
            *g_wholeFile = text;
            *g_parseEnd = text + bytes;
            *g_parseCursor = text;
            *g_scriptText = ee::Ptr<char>(zge::allocateMemory(kScriptBytes));
        }
        while (parseZguiTokens(token, 0) != 0)
        {
            if (zge::stringCompareNoCase(token, kObjectWord) == 0)
            {
                parseZguiTokens(second, 1);
                parseZguiTokens(name, 1);
                widget = newZguiWidget(second, name);
                if (parseZguiTokens(openBrace, 2) == 0)
                    return {};
                if (parent)
                    appendChildNode(parent, widget);
                loadZguiScriptText(0, 0, widget); // the OBJECT's own block
                const ee::Ptr<ee::Ptr<char>> onCreate(variableValue(widget, ee::Ptr<const char>(kOnCreateName)));
                if (onCreate && *onCreate)
                    runScript(widget, *onCreate, 0);
                continue;
            }
            if (zge::stringCompareNoCase(token, kActionWord) == 0)
            {
                parseZguiTokens(second, 1);
                parseZguiTokens(name, 1);
                const ee::Ptr<Animation> action = newZguiNode(second, name, parent);
                if (parseZguiTokens(openBrace, 2) == 0)
                    return {};
                for (;;)
                {
                    parseZguiTokens(token, 0);
                    if (zge::stringCompareNoCase(token, closeBrace) == 0)
                        break;
                    parseZguiTokens(*g_scriptText, 1);
                    setVariableText(action, token, *g_scriptText);
                }
                continue;
            }
            if (zge::stringCompareNoCase(token, kAnchorTypeWord) == 0)
            {
                parseZguiTokens(*g_scriptText, 1);
                for (uint32_t anchor = 0; anchor < std::size(kAnchorNames); ++anchor)
                {
                    if (zge::stringCompareNoCase(*g_scriptText, kAnchorNames[anchor]) == 0)
                    {
                        setNodeAnchor(parent, static_cast<WidgetAnchor>(anchor));
                        break;
                    }
                }
                continue;
            }
            if (zge::stringCompareNoCase(token, closeBrace) == 0)
                break;
            parseZguiTokens(*g_scriptText, 1);
            setVariableText(parent, token, *g_scriptText);
        }
        if (text != 0)
        {
            zge::freeMemory(*g_scriptText);
            if (widget)
                fireGuiEventHandlers(widget, static_cast<uint32_t>(Message::Refresh), 0);
        }
        return widget;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadZguiScriptText);

    // loadZguiScriptFile: ZGE4's loadZguiScriptNode: the .zsc file read
    // whole, its text's tree built under `parent`, the file freed.
    ee::Ptr<Node> loadZguiScriptFile(ee::Address path, ee::Ptr<Node> parent)
    {
        ee::Local<uint32_t> bytes;
        const uint32_t file = zge::loadFile(path, bytes);
        const ee::Ptr<Node> widget = loadZguiScriptText(file, *bytes, parent);
        zge::freeLoadedFile(file);
        return widget;
    }

    namespace
    {
        constexpr uint32_t kModelWord = sym::dm2::g_textModelWord;   // "MODEL"
        constexpr uint32_t kLightWord = sym::dm2::g_textLightWord;   // "LIGHT"
        constexpr uint32_t kCameraWord = sym::dm2::g_textCameraWord; // "CAMERA"
        constexpr uint32_t kLineBytes = 0x400;                       // a token is read into this much
        // Nothing hangs an object on a bone here.
        constexpr uint32_t kNoBone = 0xFFFFFFFFu;
    }

    // loadObjectScript: ZGE2's loadZguiScriptNode: a .zsc file's objects made
    // under `parent` (0: free), read with the engine's token reader (a file
    // is begun for `path`; a nested block goes on in the same one). OBJECT
    // kind name { } makes a widget, MODEL, LIGHT and CAMERA kind name { }
    // a guiModel, a light and a camera object; each is attached to the parent
    // and its block read with it as the parent; a widget or a model then runs
    // its OnCreate. ACTION kind name { } makes an ACTION of the parent, each
    // line of its block a property set from the rest of the line. Any other
    // word is a property of the parent, set from the rest of its line; "}"
    // ends the block. The last object made, or 0 (0 too when a block has no
    // "{").
    ee::Ptr<zge::SceneObject> loadObjectScript(ee::Address path, ee::Ptr<zge::SceneObject> parent)
    {
        ee::Local<char[0x40]> kind;
        ee::Local<char[0x410]> token;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x10]> openBrace;
        ee::Local<char[0x10]> closeBrace;
        openBrace[0] = static_cast<char>((*g_openBrace)[0]);
        openBrace[1] = static_cast<char>((*g_openBrace)[1]);
        closeBrace[0] = static_cast<char>((*g_closeBrace)[0]);
        closeBrace[1] = static_cast<char>((*g_closeBrace)[1]);
        ee::Ptr<zge::SceneObject> made;
        if (path != 0)
            zge::beginScript(path);
        zge::setTokenCapitalize(0);
        const auto isWord = [&](uint32_t word) { return zge::stringCompareNoCase(token, word) == 0; };
        // The kind and the name after a block's word.
        const auto readKindAndName = [&] {
            zge::readStringToken(kind, sizeof(char[0x40]));
            zge::readStringToken(name, sizeof(char[0x40]));
        };
        // The object's block: attached to the parent and read with the object as the parent.
        const auto readBlock = [&]() {
            if (zge::expectToken(openBrace) == 0)
                return false;
            if (parent)
                zge::attachObjectAt(made, parent, {}, kNoBone);
            loadObjectScript(0u, made); // (in itself: the block's tokens follow in the same reader)
            return true;
        };
        // What is left of the line as a property's text, the line then ended.
        const auto restOfLine = [] {
            return ee::Ptr<const char>(zge::restOfTokenLine() + 1);
        };
        while (zge::nextToken(token, kLineBytes))
        {
            const bool object = isWord(kObjectWord);
            if (object || isWord(kModelWord))
            {
                readKindAndName();
                if (object)
                    made = classCast<zge::SceneObject>(newZguiWidget(kind, name));
                else
                    made = newGuiModel(name);
                if (!readBlock())
                    return {};
                copyVariable(made, ee::Ptr<const char>(kOnCreateName), token);
                if (token[0])
                    runScriptLines(classCast<Node>(made), token);
                continue;
            }
            const bool light = isWord(kLightWord);
            if (light || isWord(kCameraWord))
            {
                readKindAndName();
                made = ee::game<ee::Ptr<zge::SceneObject>>(light ? sym::dm2::newGuiLight : sym::dm2::newGuiCamera, kind, name);
                if (!readBlock())
                    return {};
                continue;
            }
            if (isWord(kActionWord))
            {
                readKindAndName();
                const ee::Ptr<ListedAction> action = classCast<ListedAction>(newZguiNode(kind, name, classCast<Node>(parent)));
                if (zge::expectToken(openBrace) == 0)
                    return {};
                for (;;)
                {
                    zge::nextToken(token, kLineBytes);
                    if (zge::stringCompareNoCase(token, closeBrace) == 0)
                        break;
                    const ee::Ptr<const char> text = restOfLine();
                    setListedVariableText(actionProperties(action), token, text);
                    ee::game(sym::dm2::endTokenLine);
                }
                continue;
            }
            if (zge::stringCompareNoCase(token, closeBrace) == 0)
                break;
            const ee::Ptr<const char> text = restOfLine();
            setWidgetVariableText(parent, token, text);
            ee::game(sym::dm2::endTokenLine);
        }
        if (path != 0)
            zge::endScript();
        return made;
    }

    // setTopPanel: the root's SetFocus handler: the focus widget told, the
    // new one told.
    int32_t setTopPanel(uint32_t /*widget*/, uint32_t focus)
    {
        const ee::Ptr<Node> before = g_zgui->focus;
        if constexpr (game::readable::kIsBmx)
        {
            if (before == ee::Ptr<Node>(focus))
                return 1; // ZGE4: the focus is there already, nothing is sent
        }
        if (before)
            sendMessage(before, static_cast<uint32_t>(Message::LostFocus), 0);
        g_zgui->focus = ee::Ptr<Node>(focus);
        if (focus != 0)
            sendMessage(ee::Ptr<Node>(focus), static_cast<uint32_t>(Message::GetFocus), 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(setTopPanel, Verified, dm2, setTopPanelByMessage);

    // setTopPanelByMessage: ZGE2's setTopPanel, the root's SetFocus handler
    // with the EngineMessage (the widget in its word): LostFocus and GetFocus
    // go from the root at once.
    int32_t setTopPanelByMessage(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Address focus = message->data;
        const ee::Ptr<Node> before = g_zgui->focus;
        if (before)
            zge::sendNewMessage(*g_root, before, static_cast<uint32_t>(Message::LostFocus), 0u, zge::kMessageNow);
        g_zgui->focus = ee::Ptr<Node>(static_cast<uint32_t>(focus));
        if (focus)
            zge::sendNewMessage(*g_root, focus, static_cast<uint32_t>(Message::GetFocus), 0u, zge::kMessageNow);
        return 1;
    }

    // setPadMapping: the root's LockInput handler: the pad's mask (the low two
    // bits name the pad).
    int32_t setPadMapping(uint32_t /*widget*/, uint32_t padAndMask)
    {
        g_zgui->pads[padAndMask & 3u].mask = padAndMask >> 2;
        return 1;
    }
    READABLE_FUNCTION_OWN(setPadMapping, Verified, dm2, setPadMappingByMessage);

    // setPadMappingByMessage: ZGE2's setPadMapping, the root's LockInput
    // handler with the EngineMessage (the pad and its mask in its word).
    int32_t setPadMappingByMessage(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t padAndMask = message->data;
        g_zgui->pads[padAndMask & 3u].mask = padAndMask >> 2;
        return 1;
    }

    // resetPadMapping: the root's UnlockInput handler: every button back, and
    // each one's repeat at half a second.
    namespace
    {
        int32_t resetPad(uint32_t padNumber)
        {
            PadInput &pad = g_zgui->pads[padNumber & 3u];
            pad.mask = 0xFFFFFFFFu;
            for (uint32_t button = 0;;)
            {
                pad.repeat[button] = ee::Float(0.5f);
                if (++button >= 32)
                    return 1;
            }
        }
    }
    int32_t resetPadMapping(uint32_t /*widget*/, uint32_t padNumber)
    {
        return resetPad(padNumber);
    }
    READABLE_FUNCTION_OWN(resetPadMapping, Verified, dm2, resetPadMappingByMessage);

    // resetPadMappingByMessage: ZGE2's resetPadMapping, the root's UnlockInput
    // handler with the EngineMessage (the pad's number in its word).
    int32_t resetPadMappingByMessage(ee::Ptr<zge::EngineMessage> message)
    {
        return resetPad(message->data);
    }

    // setZguiPad: which pad a port reads. (The original hands back an
    // address its one caller, initPlayerPads, drops.)
    void setZguiPad(ee::Ptr<Pad> pad, uint32_t port)
    {
        g_zgui->pads[port].pad = pad;
    }
    READABLE_FUNCTION(setZguiPad, Verified);

    // zguiPad
    ee::Ptr<Pad> zguiPad(uint32_t port)
    {
        return g_zgui->pads[port].pad;
    }
    READABLE_FUNCTION(zguiPad, Verified);

    // addTreeTextNode: a line of a tree view: a bare node with its Text.
    ee::Ptr<Node> addTreeTextNode(ee::Ptr<Node> parent, ee::Address text)
    {
        const ee::Ptr<Node> node(zge::heapTake(ee::Ptr<zge::Heap>(*g_heap), kTreeNodeBytes));
        initNode(node, ee::Ptr<const char>(kTreeNodeName), kTreeNodeKind);
        node->shown = 0;
        node->type = 0;
        defineTypedProperty(node, ee::Ptr<const char>(kTextName), ee::Ptr<const char>(kStringType), &node->type);
        setVariable(node, kTextName, text);
        if (parent)
            appendChildNode(parent, node);
        return node;
    }
    READABLE_FUNCTION_WAITS_OWN(addTreeTextNode, Verified, dm2, newTreeItem);

    // newTreeItem: ZGE2's addTreeTextNode: a tree view's line is an item of
    // the engine's heap with that text, the last of `parent`'s children (0:
    // at the top).
    ee::Ptr<ListedTreeItem> newTreeItem(ee::Ptr<ListedTreeItem> parent, ee::Ptr<const char> text)
    {
        const ee::Ptr<ListedTreeItem> item(zge::heapAllocate(sizeof(ListedTreeItem)));
        item->_0c = 0;
        item->parent = parent;
        zge::stringCopy(item->text, text);
        zge::initList(childrenOf(item));
        if (parent)
            zge::listPush(childrenOf(parent), item);
        return item;
    }

    // destroyGuiNodeMethod: nothing calls it.
    int32_t destroyGuiNodeMethod(ee::Ptr<Node> node)
    {
        return game::tail(zge::zgui::destroyNode, node);
    }
    READABLE_FUNCTION_WAITS(destroyGuiNodeMethod, Verified);

    // registerZguiNodeKind: a kind an ACTION can name.
    int32_t registerZguiNodeKind(uint32_t name, uint32_t bytes, uint32_t create)
    {
        const ee::Ptr<NodeKindRow> row(zguiAllocate(*g_heap, sizeof(NodeKindRow)));
        zge::stringCopy(row->name, name);
        row->bytes = bytes;
        row->create = create;
        listPush(kNodeKinds, row);
        return 1;
    }
    READABLE_FUNCTION(registerZguiNodeKind, Verified);

    // newZguiNode: the node of an ACTION, its kind found by name; 0 if there
    // is none.
    ee::Ptr<Animation> newZguiNode(ee::Address kind, ee::Address name, ee::Ptr<Node> parent)
    {
        for (ee::Ptr<NodeKindRow> row(*g_nodeKinds); row; row = row->next)
        {
            if (zge::stringCompareNoCase(kind, row->name) == 0)
            {
                const ee::Ptr<Animation> node(zguiAllocate(*g_heap, row->bytes));
                if constexpr (game::readable::kIsDm2)
                    classCast<ListedAction>(node)->kind = ee::Ptr<const char>(row->name); // ZGE2's ACTION node
                else
                    node->kind = ee::Ptr<const char>(row->name);
                row->create(node, name, parent);
                return node;
            }
        }
        return {};
    }
    READABLE_FUNCTION(newZguiNode, Verified);

    // initTimedNode: what every ACTION has, under `parent`.
    uint32_t initTimedNode(ee::Address address, ee::Address name, ee::Ptr<Node> parent)
    {
        const ee::Ptr<Animation> node(address);
        initNode(ee::Ptr<Node>(address), ee::Ptr<const char>(name), static_cast<uint32_t>(NodeKind::Animation));
        ee::Ptr<Animation>(address)->delay = 0;
        ee::Ptr<Animation>(address)->onStart = 0;
        ee::Ptr<Animation>(address)->onUpdate = 0;
        ee::Ptr<Animation>(address)->onEnd = 0;
        node->playingSize = sizeof(Playing);
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kNameName), ee::Ptr<const char>(kNameType), address + offsetof(Animation, name));
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kStartDelayName), ee::Ptr<const char>(kZFloatType), address + offsetof(Animation, delay));
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kOnStartName), ee::Ptr<const char>(kEventType), address + offsetof(Animation, onStart));
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kOnUpdateName), ee::Ptr<const char>(kEventType), address + offsetof(Animation, onUpdate));
        defineTypedProperty(ee::Ptr<Node>(address), ee::Ptr<const char>(kOnEndName), ee::Ptr<const char>(kEventType), address + offsetof(Animation, onEnd));
        appendChildNode(ee::Ptr<Node>(parent), ee::Ptr<Node>(address));
        return address;
    }
    
    // initListedAction: ZGE2's initTimedNode: what every ACTION has
    // (ListedAction), its properties a list of its own, and the action
    // listed in its widget's actions when `parent` is a widget's scene object.
    uint32_t initListedAction(ee::Address address, ee::Address name, ee::Ptr<zge::SceneObject> parent)
    {
        const ee::Ptr<ListedAction> action(address);
        const ee::Ptr<zge::List> properties(valueAddress(action->properties));
        zge::initList(properties);
        action->name = ee::Ptr<const char>(0u);
        action->parent = parent;
        action->delay = 0;
        action->onStart = 0;
        action->onUpdate = 0;
        action->onEnd = 0;
        action->over = 0;
        action->started = 0;
        addListedProperty(properties, ee::Ptr<const char>(kNameName), ee::Ptr<const char>(kNameType), valueAddress(action->name));
        addListedProperty(properties, ee::Ptr<const char>(kStartDelayName), ee::Ptr<const char>(kZFloatType), valueAddress(action->delay));
        addListedProperty(properties, ee::Ptr<const char>(kOnStartName), ee::Ptr<const char>(kEventType), valueAddress(action->onStart));
        addListedProperty(properties, ee::Ptr<const char>(kOnUpdateName), ee::Ptr<const char>(kEventType), valueAddress(action->onUpdate));
        addListedProperty(properties, ee::Ptr<const char>(kOnEndName), ee::Ptr<const char>(kEventType), valueAddress(action->onEnd));
        setListedVariable(properties, ee::Ptr<const char>(kNameName), name);
        if (sceneObjectKind(parent) == kWidgetObjectKind)
            zge::listPush(valueAddress(widgetRecord(parent)->actions), address);
        return address;
    }
    READABLE_FUNCTION_OWN(initTimedNode, Verified, dm2, initListedAction);

    // defineWidgetProperties: what every widget has: Name, OnCreate, OnDestroy, Visible,
    // Position, each a property node of the widget's memory.
    uint32_t defineWidgetProperties(ee::Address address, uint32_t name)
    {
        const ee::Ptr<Node> node(address);
        const auto define = [&](uint32_t property, uint32_t propertyName, uint32_t type, uint32_t value)
        {
            defineProperty(ee::Ptr<Node>(address + property), node, ee::Ptr<const char>(propertyName), ee::Ptr<const char>(type), value);
        };
        initNode(node, ee::Ptr<const char>(name), static_cast<uint32_t>(NodeKind::Widget));
        define(kNameProperty, kWidgetNameName, kWidgetNameType, address + offsetof(Node, name));
        define(kOnCreateVariable, kOnCreateProperty, kWidgetEventType, address + kOnCreate);
        define(kOnDestroyVariable, kOnDestroyName, kWidgetEventType, address + kOnDestroy);
        define(kVisibleVariable, kVisibleName, kBoolType, address + offsetof(Node, shown));
        define(kPositionVariable, kPositionName, kZ3DVectorType, address + offsetof(Node, position));
        node->shown = 1;
        node->position.x = ee::Float(0.0f);
        node->position.y = ee::Float(0.0f);
        node->position.z = ee::Float(0.0f);
        const ee::Ptr<Widget> widget(address);
        widget->onCreate = {};
        widget->onDestroy = {};
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4: no size, the top left, no kind name (its +0x1C is the alignment).
            clearWidgetLayout(widget);
        }
        return address;
    }

    // defineWidgetRecord: ZGE2's defineWidgetProperties: what every widget's
    // record has (WidgetRecord): its two lists, its place at the origin with
    // no turn and a scale of 1, and the properties Name, OnCreate, OnDestroy,
    // Visible, Position, Orientation and Scale. Visible, Position and
    // Orientation are listed with no value: setting one sets the scene
    // object's own (DM2's setVariable).
    uint32_t defineWidgetRecord(ee::Address address, uint32_t name)
    {
        const ee::Ptr<WidgetRecord> widget(address);
        const ee::Ptr<zge::List> properties(valueAddress(widget->properties));
        const auto define = [&](uint32_t propertyName, uint32_t type, ee::Address value)
        {
            addListedProperty(properties, ee::Ptr<const char>(propertyName), ee::Ptr<const char>(type), value);
        };
        zge::initList(valueAddress(widget->actions));
        zge::initList(properties);
        widget->name = ee::Ptr<const char>(0u);
        define(kWidgetNameName, kWidgetNameType, valueAddress(widget->name));
        setListedVariable(properties, ee::Ptr<const char>(kWidgetNameName), name);
        define(kOnCreateProperty, kWidgetEventType, valueAddress(widget->onCreate));
        define(kOnDestroyName, kWidgetEventType, valueAddress(widget->onDestroy));
        define(kVisibleName, kBoolType, 0);
        define(kPositionName, kZ3DVectorType, 0);
        define(kOrientationName, kZQuaternionType, 0);
        define(kScaleName, kZ3DVectorType, valueAddress(widget->scale));
        widget->position.x = ee::Float(0.0f);
        widget->position.y = ee::Float(0.0f);
        widget->position.z = ee::Float(0.0f);
        widget->scale.x = ee::Float(1.0f);
        widget->scale.y = ee::Float(1.0f);
        widget->scale.z = ee::Float(1.0f);
        zge::quaternionIdentity(&widget->orientation);
        widget->onCreate = 0;
        widget->onDestroy = 0;
        return address;
    }
    READABLE_FUNCTION_OWN(defineWidgetProperties, Verified, dm2, defineWidgetRecord);

    // runNodeDestroyScript: a widget's Destroy handler: the focus let go, its
    // OnDestroy run.
    int32_t runNodeDestroyScript(ee::Address widgetAddress)
    {
        const ee::Ptr<Widget> widget(widgetAddress);
        if (g_zgui->focus == &widget->node)
            g_zgui->focus = {};
        if (widget->onDestroy)
            runScript(&widget->node, widget->onDestroy, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(runNodeDestroyScript, Verified);

    namespace
    {
        enum class ByPath { Set, CopyOut };

        // The variable at `path` under the root given `value` by its type's
        // set, or its value copied out to `value` by copyOut: 0 if it is not
        // found.
        int32_t setByPath(ee::Address path, ee::Address value, ByPath method)
        {
            const ee::Ptr<Node> variable(findNode(*g_root, path, static_cast<uint64_t>(NodeKind::Variable)));
            if (!variable)
                return 0;
            const ee::Ptr<VariableType> type = variable->variableType;
            if (method == ByPath::Set)
                type->set(variable, value);
            else
                type->copyOut(variable, value);
            return 1;
        }
    }

    // setNodeValueByPath
    int32_t setNodeValueByPath(ee::Address path, ee::Address value)
    {
        return setByPath(path, value, ByPath::Set);
    }
    READABLE_FUNCTION(setNodeValueByPath, Verified);

    // callGuiNodeMethodByName: nothing calls it.
    int32_t callGuiNodeMethodByName(ee::Address path, ee::Address value)
    {
        return setByPath(path, value, ByPath::CopyOut);
    }
    READABLE_FUNCTION(callGuiNodeMethodByName, Verified);

    namespace
    {
        // A focus handler: the widget's script of that name, if it has one.
        int32_t runFocusScript(uint32_t widget, uint32_t name, uint32_t end)
        {
            const uint32_t variable = variableValue(ee::Ptr<Node>(widget), ee::Ptr<const char>(name));
            if (variable != 0)
            {
                const uint32_t script = *ee::at<uint32_t>(variable);
                if (script != 0)
                    runScript(ee::Ptr<Node>(widget), ee::Ptr<char>(script), 0);
            }
            return 1;
        }
    }

    // runOnGetFocus
    int32_t runOnGetFocus(ee::Address widget)
    {
        namespace here = points::runOnGetFocus;
        return runFocusScript(widget, kOnGetFocusName, here::join);
    }

    // runOnLostFocus
    int32_t runOnLostFocus(ee::Address widget)
    {
        namespace here = points::runOnLostFocus;
        return runFocusScript(widget, kOnLostFocusName, here::join);
    }

    namespace
    {
        // ZGE2's focus handler: the widget's script of that name copied out
        // (its Event's text, through copyVariable, into `script`) and run for
        // it if not empty. ZGE3 runs the variable's text where it is.
        int32_t runListedFocusScript(ee::Ptr<zge::EngineMessage> message, uint32_t name, ee::Ptr<char> script)
        {
            if (copyVariable(message->target, ee::Ptr<const char>(name), script) && script[0] != 0)
                runScriptLines(ee::Ptr<Node>(message->target), script);
            return 1;
        }
    }

    // runListedGetFocus: ZGE2's runOnGetFocus.
    int32_t runListedGetFocus(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x400]> script;
        return runListedFocusScript(message, kOnGetFocusName, script);
    }
    READABLE_FUNCTION_WAITS_OWN(runOnGetFocus, Verified, dm2, runListedGetFocus);

    // runListedLostFocus: ZGE2's runOnLostFocus.
    int32_t runListedLostFocus(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x400]> script;
        return runListedFocusScript(message, kOnLostFocusName, script);
    }
    READABLE_FUNCTION_WAITS_OWN(runOnLostFocus, Verified, dm2, runListedLostFocus);

    namespace
    {
        constexpr uint32_t kTextSaveLoadSaveLoadMenuMessage = sym::g_textSaveLoadSaveLoadMenuMessage;
        constexpr uint32_t kTextSaveLoadSaveLoadMenuMessage2 = sym::g_textSaveLoadSaveLoadMenuMessage2;
        constexpr uint32_t kNamesHigh = ee::high(sym::g_textSaveLoadSaveLoadMenuMemCardMenu);
        constexpr uint32_t kTextSaveLoadSaveLoadMenuMemCardMenu2 = sym::g_textSaveLoadSaveLoadMenuMemCardMenu2;
        constexpr uint32_t kTextKillfocusSetSaveLoadSaveLoadMenu = sym::g_textKillfocusSetSaveLoadSaveLoadMenu;
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(runScriptint32_t, ee::game<int32_t>, kRunScript);
        }
    }

}

// The hooks of ZGE2's widget objects (their scene object class,
// g_widgetObjectClass): the widget's record follows its object.
namespace zge::zgui
{
    // destroyWidgetObject: the class's destroy: the widget goes with its
    // object: the focus dropped if it has it, OnDestroy run, the objects
    // hanging from it deleted, each ACTION ended at the root (EndAction) and
    // freed with its variables, then the widget's variables and its record. 1.
    inline uint32_t destroyWidgetObject(ee::Ptr<zge::SceneObject> object)
    {
        if (g_zgui->focus == classCast<Node>(object))
            g_zgui->focus = {};
        const ee::Ptr<WidgetRecord> record = widgetRecord(object);
        if (record->onDestroy)
            runScriptLines(classCast<Node>(object), record->onDestroy);
        while (const ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object))
            zge::deleteSceneObject(child);
        const ee::Ptr<zge::List> actions(&record->actions);
        while (const ee::Ptr<zge::ListNode> entry = actions->head)
        {
            const ee::Ptr<ListedAction> action = zge::recordOfNode<ListedAction>(entry);
            zge::sendNewMessage(0u, *g_root, static_cast<uint32_t>(Message::EndAction), action, zge::kMessageNow);
            zge::listRemove(actions, action);
            freeListedVariables(actionProperties(action));
            zguiRelease(*g_heap, action);
        }
        freeListedVariables(widgetProperties(object));
        zguiRelease(*g_heap, record);
        object->animator = {};
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, destroyWidgetObject);

    // widgetObjectUpdate, widgetObjectShown: nothing to do (1).
    inline uint32_t widgetObjectUpdate(ee::Ptr<zge::SceneObject> /*object*/)
    {
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectUpdate);

    inline uint32_t widgetObjectShown(ee::Ptr<zge::SceneObject> /*object*/, uint32_t /*shown*/)
    {
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectShown);

    // widgetObjectMoved: the object's new position kept in the record (if it
    // still has one). 1.
    inline uint32_t widgetObjectMoved(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Vec4> position)
    {
        if (const ee::Ptr<WidgetRecord> record = widgetRecord(object))
            record->position = *position;
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectMoved);

    // widgetObjectRotated: the same for its orientation.
    inline uint32_t widgetObjectRotated(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Quaternion> orientation)
    {
        if (const ee::Ptr<WidgetRecord> record = widgetRecord(object))
            zge::quaternionCopy(&record->orientation, orientation);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectRotated);

    // widgetObjectScaled: the scale kept (the record is not tested here). 1.
    inline uint32_t widgetObjectScaled(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Vec4> scale)
    {
        widgetRecord(object)->scale = *scale;
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectScaled);

    // widgetObjectId: the class's id hook: the hash of the widget's name
    // (findWidgetByPath finds a widget by it).
    inline uint32_t widgetObjectId(ee::Ptr<zge::SceneObject> object)
    {
        return zge::nameHash(widgetRecord(object)->name);
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectId);

    // widgetObjectName: the debug report's hook: the widget's name.
    inline ee::Ptr<const char> widgetObjectName(ee::Ptr<zge::SceneObject> object)
    {
        return widgetRecord(object)->name;
    }
    READABLE_FUNCTION_ONLY(dm2, widgetObjectName);
}
