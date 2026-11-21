// zgui's tree (widget.h): a node's set-up and its place among its parent's
// children, a node destroyed with the nodes below it, a message broadcast
// down the tree, and the Variable nodes the game and the scripts declare
// (script.h): a variable's value read out or set through its type.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "sym/zge/zgui/nodes.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // A Variable node: the Node up to its type (no positions).
        constexpr uint32_t kVariableNodeSize = 0x20u;
    }

    // `message` to every handler for it among the node's children, and on down
    // through the widgets below.
    void fireGuiEventHandlers(ee::Ptr<Node> node, uint32_t message, uint32_t argument)
    {
        for (ee::Ptr<Node> child = node->firstChild; child; child = child->nextSibling)
        {
            if (child->kind() == NodeKind::Handler && child->message == message)
                child->handler(node, argument);
            if (child->kind() == NodeKind::Widget)
                fireGuiEventHandlers(child, message, argument);
        }
    }
    // A handler can load or run scripts (loadZguiScriptNode's "loaded" message).
    READABLE_FUNCTION_WAITS_OWN(fireGuiEventHandlers, Verified, dm2, refreshWidgetTree);

    // refreshWidgetTree: ZGE2's fireGuiEventHandlers, a forEachAttached visitor:
    // Refresh sent at once (no sender) to the object, then through itself to
    // every object hanging from it. Always 1. ZGE2 only ever refreshes: the
    // callers' message word goes unread.
    uint32_t refreshWidgetTree(ee::Ptr<zge::SceneObject> object, ee::Address /*context*/)
    {
        zge::sendNewMessage(0u, object, static_cast<uint32_t>(Message::Refresh), 0u, zge::kMessageNow);
        zge::forEachAttached(object, sym::fireGuiEventHandlers, 0u);
        return 1;
    }

    namespace
    {
        uint32_t heapTake(uint32_t heap, uint32_t bytes) { return ee::game<uint32_t>(sym::heapTake, heap, bytes); }
    }

    int32_t initNode(ee::Ptr<Node> node, ee::Ptr<const char> name, uint32_t kindAndFlags)
    {
        node->message = 0;
        node->firstChild = {};
        node->parent = {};
        node->nextSibling = {};
        // The kind (four bits) and the two ownership flags.
        setNodeKind(node, kindAndFlags);
        if (kindAndFlags & kInitStaticName)
            node->name = name;
        else
        {
            // A copy on zgui's heap, with a 1 after its terminator.
            const int32_t length = zge::stringLength(name);
            const ee::Ptr<char> copy(heapTake(*g_heap, length + 2));
            node->name = copy.get();
            zge::stringCopy(copy, name);
            copy[length + 1] = 1;
        }
        const uint32_t hash = zge::nameHash(name.get());
        node->nameHash = hash;
        return static_cast<int32_t>(hash);
    }
    READABLE_FUNCTION(initNode, Verified);

    void unlinkNode(ee::Ptr<Node> node)
    {
        if (!node || !node->parent)
            return;
        const ee::Ptr<Node> parent = node->parent;
        ee::Ptr<Node> before = parent->firstChild;
        if (before == node)
            parent->firstChild = node->nextSibling;
        else
        {
            // The child before it (none: not among its parent's children).
            for (;;)
            {
                const ee::Ptr<Node> next = before->nextSibling;
                if (!next)
                    return;
                if (next == node)
                    break;
                before = next;
            }
            before->nextSibling = node->nextSibling;
        }
        node->nextSibling = {};
        node->parent = {};
    }
    READABLE_FUNCTION(unlinkNode, Verified);

    // "a.b.c" into out: the names from the tree's root (left out) down to the
    // node.
    void nodePath(ee::Ptr<Node> node, ee::Ptr<char> out)
    {
        ee::Local<char> path;
        *out = 0;
        for (; node && node != g_zgui->root; node = node->parent)
        {
            zge::stringCopy(path, node->name);
            if (*out != 0)
            {
                zge::appendString(path, sym::pathSeparator2);
                zge::appendString(path, out);
            }
            zge::stringCopy(out, path);
        }
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(nodePath, Draft);

    ee::Ptr<Node> nthChildOfKind(ee::Ptr<Node> node, uint32_t index, uint32_t kind)
    {
        ee::Ptr<Node> child = node->firstChild;
        if (child)
        {
            for (;;)
            {
                const bool ofKind = static_cast<uint32_t>(child->kind()) == kind;
                if (index == 0)
                {
                    if (ofKind)
                        return child;
                }
                child = child->nextSibling;
                if (ofKind)
                    --index;
                if (!child)
                    break;
            }
        }
        return ee::Ptr<Node>(0u);
    }
    READABLE_FUNCTION(nthChildOfKind, Verified);

    int32_t countChildrenOfKind(ee::Ptr<Node> node, uint32_t kind)
    {
        int32_t count = 0;
        ee::Ptr<Node> child = node->firstChild;
        if (child)
        {
            for (;;)
            {
                if (static_cast<uint32_t>(child->kind()) == kind)
                    ++count;
                child = child->nextSibling;
                if (!child)
                    break;
            }
        }
        return count;
    }
    READABLE_FUNCTION(countChildrenOfKind, Verified);

    void appendChildNode(ee::Ptr<Node> parent, ee::Ptr<Node> child)
    {
        ee::Ptr<Node> last = parent->firstChild;
        if (!last)
        {
            // The first child.
            child->nextSibling = ee::Ptr<Node>(0u);
            child->parent = parent;
            parent->firstChild = child;
            return;
        }
        ee::Ptr<Node> next = last->nextSibling;
        if (next)
        {
            for (;;)
            {
                last = next;
                next = last->nextSibling;
                if (!next)
                    break;
            }
        }
        child->parent = parent;
        child->nextSibling = ee::Ptr<Node>(0u);
        last->nextSibling = child;
    }
    READABLE_FUNCTION(appendChildNode, Verified);

    // `next` goes after `node` among its parent's children.
    void insertNodeAfter(ee::Ptr<Node> node, ee::Ptr<Node> next)
    {
        next->parent = node->parent;
        next->nextSibling = node->nextSibling;
        node->nextSibling = next;
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(insertNodeAfter, Draft);

    // `previous` goes in front of `node` among its parent's children.
    void insertNodeBefore(ee::Ptr<Node> node, ee::Ptr<Node> previous)
    {
        ee::Ptr<Node> before = node->parent->firstChild;
        if (before == node)
        {
            previous->parent = node->parent;
            node->parent->firstChild = previous;
            previous->nextSibling = before;
            return;
        }
        for (;;)
        {
            const ee::Ptr<Node> next = before->nextSibling;
            if (!next)
                return;
            if (next == node)
                break;
            before = next;
        }
        previous->nextSibling = node;
        previous->parent = node->parent;
        before->nextSibling = previous;
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(insertNodeBefore, Draft);

    int32_t freeNodeData(ee::Ptr<Node> variable);

    namespace
    {
        void heapGive(uint32_t heap, ee::Ptr<const char> memory) { ee::game(sym::heapGive, heap, memory); }
        namespace tail
        {
        int32_t heapGive(uint32_t heap, ee::Address memory) { return ee::tailCall<int32_t>(sym::heapGive, heap, memory); }
        }
    }

    int32_t destroyNode(ee::Ptr<Node> node)
    {
        // What the node's kind has to undo first: a Variable's value freed,
        // a Widget's OnDestroy script, an Animation stopped.
        const NodeKind kind = node->kind();
        if (kind == NodeKind::Variable)
            freeNodeData(node);
        else if (static_cast<uint16_t>(kind) < 3)
        {
            if (kind == NodeKind::Widget)
                runNodeDestroyScript(node);
        }
        else
        {
            if (kind == NodeKind::Animation)
                unlinkPlayingNode(node);
        }
        // The nodes below it, then the node itself out of its parent.
        forEachChild(node, sym::destroyNode);
        if (node->parent)
            unlinkNode(node);
        if (!node->nameIsStatic())
        {
            if (node->name)
            {
                heapGive(*g_heap, node->name);
                node->name = ee::Ptr<const char>(0u);
            }
        }
        if (!node->isEmbedded())
            return tail::heapGive(*g_heap, node); // the original's tail call
        return 1;
    }
    // A widget's OnDestroy script can load files.
    READABLE_FUNCTION_WAITS(destroyNode, Verified);

    namespace
    {
        // The value accessors below: the variable's value read out through
        // one of its type's methods: 1, or 0 for no variable or a type
        // without that method.
        template <typename Method, typename Out>
        int32_t readValue(ee::Ptr<Node> variable, Method VariableType::*method, Out out)
        {
            if (!variable || variable->kind() != NodeKind::Variable)
                return 0;
            const Method read = variable->variableType.get()->*method;
            if (!read)
                return 0;
            read(variable, out);
            return 1;
        }
    }

    int32_t variableIntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        return readValue(variable, &VariableType::intValue, out);
    }
    READABLE_FUNCTION(variableIntValue, Verified);

    int32_t variableFloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        return readValue(variable, &VariableType::floatValue, out);
    }
    READABLE_FUNCTION(variableFloatValue, Verified);

    int32_t variableToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        return readValue(variable, &VariableType::toText, out);
    }
    READABLE_FUNCTION(variableToText, Verified);

    // A second copy of variableIntValue (the menus' volume sliders call it).
    int32_t variableIntValueAgain(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        return readValue(variable, &VariableType::intValue, out);
    }
    READABLE_FUNCTION(variableIntValueAgain, Verified);

    int32_t variableColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        return readValue(variable, &VariableType::colorValue, out);
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(variableColorValue, Draft);

    int32_t variableVectorValue(ee::Ptr<Node> variable, ee::Ptr<zge::Vec4> out)
    {
        return readValue(variable, &VariableType::vectorValue, out);
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(variableVectorValue, Draft);

    void addVariableNode(ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value)
    {
        // A locked Variable (the scripts read it, only its owner sets it) of
        // the type named, over the caller's value; its name is the caller's.
        const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(ee::Ptr<const char>(typeName).get()));
        if (!type)
            return;
        const ee::Ptr<Node> variable(heapTake(*g_heap, kVariableNodeSize));
        initNode(variable, ee::Ptr<const char>(name), kInitStaticName | static_cast<uint32_t>(NodeKind::Variable));
        variable->message = kVariableLocked;
        variable->variableType = type;
        variable->value = value;
        appendChildNode(ee::Ptr<Node>(parent), variable);
    }
    READABLE_FUNCTION(addVariableNode, Verified);

    void defineProperty(ee::Address propertyNode, ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value)
    {
        // A widget kind's property: a Variable node that is part of the
        // widget's own memory (`property`), over its value, its name the
        // caller's.
        const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(ee::Ptr<const char>(typeName).get()));
        if (type)
        {
            const ee::Ptr<Node> property(propertyNode);
            initNode(property, ee::Ptr<const char>(name), kInitEmbedded | kInitStaticName | static_cast<uint32_t>(NodeKind::Variable));
            property->variableType = type;
            property->value = value;
            property->message = 0;
            appendChildNode(ee::Ptr<Node>(parent), property);
        }
    }
    READABLE_FUNCTION(defineProperty, Verified);

    int32_t defineTypedProperty(ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value)
    {
        // A property on zgui's heap (its name the caller's): 1, 0 for an
        // unknown type.
        const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(ee::Ptr<const char>(typeName).get()));
        if (!type)
            return 0;
        const ee::Ptr<Node> property(heapTake(*g_heap, kVariableNodeSize));
        initNode(property, ee::Ptr<const char>(name), kInitStaticName | static_cast<uint32_t>(NodeKind::Variable));
        property->variableType = type;
        property->value = value;
        property->message = 0;
        appendChildNode(ee::Ptr<Node>(parent), property);
        return 1;
    }

    // addListedProperty: ZGE2's defineTypedProperty: the property a
    // ListedVariable (widget.h) of zgui's memory pushed on `properties`, over
    // the caller's value: 1, 0 for an unknown type.
    int32_t addListedProperty(ee::Ptr<zge::List> properties, ee::Ptr<const char> name, ee::Ptr<const char> typeName,
                              ee::Address value)
    {
        const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(typeName.get()));
        if (!type)
            return 0;
        const ee::Ptr<ListedVariable> property(zguiAllocate(*g_heap, sizeof(ListedVariable)));
        property->variableType = type;
        property->value = value;
        property->ownsValue = 0;
        property->nameHash = zge::nameHash(name.get());
        listPush(properties, property);
        return 1;
    }
    READABLE_FUNCTION_OWN(defineTypedProperty, Verified, dm2, addListedProperty);

    // setListedVariable (ZGE2 only): the ListedVariable called `name` in
    // `variables` set from a value of its type (the type's set): 1, 0 without
    // it. ZGE3 keeps variables as nodes and sets one with setVariable.
    inline int32_t setListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Address value)
    {
        const uint32_t hash = zge::nameHash(name.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash == hash)
            {
                variable->variableType->set(classCast<Node>(variable), value);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, setListedVariable);

    // copyListedVariable (ZGE2 only): the ListedVariable called `name` in
    // `variables` copied out (its type's copyOut) into out: 1, 0 without it.
    // ZGE3's copyVariable finds a Variable node instead.
    inline int32_t copyListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Address out)
    {
        const uint32_t hash = zge::nameHash(name.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash == hash)
            {
                variable->variableType->copyOut(classCast<Node>(variable), out);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, copyListedVariable);

    // listedVariableText (ZGE2 only): the ListedVariable called `name` as text
    // (its type's toText into out): what toText returns, 0 without it.
    inline uint64_t listedVariableText(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Ptr<char> out)
    {
        const uint32_t hash = zge::nameHash(name.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash == hash)
                return variable->variableType->toText(classCast<Node>(variable), out);
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, listedVariableText);

    // setListedVariableText (ZGE2 only): the ListedVariable called `name` set
    // from text (its type's setText): 1, 0 without it.
    inline int32_t setListedVariableText(ee::Ptr<zge::List> variables, ee::Ptr<const char> name,
                                         ee::Ptr<const char> text)
    {
        const uint32_t hash = zge::nameHash(name.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash == hash)
            {
                variable->variableType->setText(classCast<Node>(variable), text);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, setListedVariableText);

    // removeListedVariable (ZGE2 only): ZGE3 finds the Variable node and
    // destroys it instead.
    inline int32_t removeListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name)
    {
        const uint32_t hash = zge::nameHash(name.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash != hash)
                continue;
            if (const auto destroy = variable->variableType->destroy)
                destroy(classCast<Node>(variable));
            if (variable->ownsValue)
                zguiRelease(*g_heap, variable->value);
            zge::listRemove(variables, variable);
            zguiRelease(*g_heap, variable);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, removeListedVariable);

    // freeListedVariables (ZGE2 only): a widget's or an ACTION's variables
    // freed with it, first to last.
    inline void freeListedVariables(ee::Ptr<zge::List> variables)
    {
        while (const ee::Ptr<zge::ListNode> entry = variables->head)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (const auto destroy = variable->variableType->destroy)
                destroy(classCast<Node>(variable));
            if (variable->ownsValue)
                zguiRelease(*g_heap, variable->value);
            zge::listRemove(variables, variable);
            zguiRelease(*g_heap, variable);
        }
    }
    READABLE_FUNCTION_ONLY(dm2, freeListedVariables);

    int32_t declareVariable(ee::Ptr<Node> parent, ee::Address name, ee::Address typeName, ee::Address value)
    {
        // A script's DECLARE: a Variable that owns its name and its value
        // (freed with it): 1, 0 for an unknown type.
        const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(ee::Ptr<const char>(typeName).get()));
        if (!type)
            return 0;
        const ee::Ptr<Node> variable(heapTake(*g_heap, kVariableNodeSize));
        initNode(variable, ee::Ptr<const char>(name), static_cast<uint32_t>(NodeKind::Variable));
        variable->message = kVariableOwnsValue;
        variable->variableType = type;
        variable->value = value;
        appendChildNode(parent, variable);
        return 1;
    }
    READABLE_FUNCTION_OWN(declareVariable, Verified, dm2, declareListedVariable);

    int32_t declareListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Ptr<const char> typeName,
                                  ee::Address value)
    {
        const int32_t added = addListedProperty(variables, name, typeName, value);
        if (added)
            zge::recordOfNode<ListedVariable>(variables->tail)->ownsValue = 1;
        return added;
    }

    int32_t setVariableValue(ee::Ptr<Node> variable, uint32_t value)
    {
        // The type's set, unless the variable is locked: 1, else 0.
        if (variable && value)
        {
            if (variable->kind() != NodeKind::Variable || (variable->message & kVariableLocked))
                return 0;
            variable->variableType->set(variable, value);
            return 1;
        }
        return 0;
    }
    // Draft: no caller (dead).
    READABLE_FUNCTION(setVariableValue, Draft);

    int32_t setVariableText(ee::Address parent, ee::Address path, ee::Ptr<const char> text)
    {
        // The variable at `path` below `parent` set from text (its type's
        // setText): 1, else 0.
        if (!parent || !path || !text)
            return 0;
        const ee::Ptr<Node> variable(findNode(ee::Ptr<Node>(parent), path, static_cast<uint64_t>(NodeKind::Variable)));
        if (!variable)
            return 0;
        const auto setText = variable->variableType->setText;
        if (!setText)
            return 0;
        setText(variable, text);
        return 1;
    }
    READABLE_FUNCTION(setVariableText, Verified);

    int32_t freeNodeData(ee::Ptr<Node> variable)
    {
        // A Variable's value: its type's destroy, then freed if the variable
        // owns it (the result: the flag, or the free's).
        const auto destroy = variable->variableType->destroy;
        if (destroy)
            destroy(variable);
        if (variable->message & kVariableOwnsValue)
            return tail::heapGive(*g_heap, variable->value); // the original's tail call
        return 0;
    }
    READABLE_FUNCTION(freeNodeData, Verified);
}
