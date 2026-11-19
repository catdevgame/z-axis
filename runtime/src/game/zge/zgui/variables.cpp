// zgui's script variables (script.h): paths resolved from the running
// script's widget, variables read and set as text.

#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "sce/libgcc.h"

#include <cstring>
#include "sym/zge/zgui/variables.cpp.h"

namespace zge::zgui
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_rootHash> g_rootHash; // gp-0x7410: nameHash("Root"), on first use
        inline ee::Global<const char, sym::rootName> g_rootName; // "Root"
        constexpr uint32_t kRootName = sym::rootName;

        // ZGE2's three variables every widget's scene object keeps itself.
        constexpr uint32_t kPositionName = sym::g_textPosition2;            // "Position"
        constexpr uint32_t kOrientationName = sym::dm2::g_textOrientation; // "Orientation"
        constexpr uint32_t kVisibleName = sym::g_textVisible2;              // "Visible"
        constexpr uint32_t kPositionFormat = sym::dm2::g_positionTextFormat;       // "%f %f %f"
        constexpr uint32_t kOrientationFormat = sym::dm2::g_orientationTextFormat; // "%f %f %f %f"
        constexpr uint32_t kVisibleFormat = sym::dm2::g_visibleTextFormat;         // "%d"
        constexpr uint32_t kNameTabValueFormat = sym::dm2::g_nameTabValueFormat;   // "%s\t%s"

        uint16_t kindOf(ee::Ptr<Node> node) { return static_cast<uint16_t>(node->kind()); }

        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_uiStrings> g_uiStrings; // the UI's string table
        constexpr uint32_t kSharedText = sym::sharedText;     // zgui's text buffer (0x2000)
        constexpr uint32_t kNoText = sym::noText2;         // "" (gp-0x6060)
        constexpr uint32_t kDollar = sym::g_textDollar;    // "$"
    }

    int32_t resolvePath(ee::Ptr<const char> path, ee::Ptr<char> name, ee::Ptr<char> typeName, ee::Ptr<ee::Ptr<Node>> node)
    {
        ee::Local<char[0x80]> component;
        ee::Local<char[0x400]> target; // a dereferenced variable's text
        if (!*g_rootHash)
            *g_rootHash = zge::nameHash(g_rootName.get());
        if (*path == '{')
            return kPathLiteral;

        int32_t kind = static_cast<int32_t>(NodeKind::Widget);
        bool dereference = false;
        *node = (*g_script)->self;
        if (*path == '*')
        {
            ++path;
            dereference = true;
        }
        if (*path == '@' && (*g_script)->argument)
        {
            *node = (*g_script)->argument;
            ++path;
            kind = kPathArgument;
        }
        if (*path == '.')
        {
            // Up one level per dot.
            for (;;)
            {
                ++path;
                *node = (*node)->parent;
                if (*path == 0)
                    return kind;
                if (*path != '.')
                    break;
            }
        }
        if (*path == '^')
        {
            ++path;
            const ee::Ptr<Node> self = (*g_script)->self;
            *node = self;
            kind = kindOf(self);
            if (*path == 0)
                return kind;
            if (*path != '.')
                return kPathNotFound;
            ++path;
        }

        for (;;)
        {
            // The next component, to a dot or the end.
            uint32_t length = 0;
            while (*path != '.' && *path != 0)
            {
                component[length++] = *path;
                ++path;
            }
            component[length] = 0;
            if (*path == '.')
                ++path;
            const uint32_t hash = zge::nameHash(&component[0]);
            if (hash == *g_rootHash)
                *node = *g_root;
            else
            {
                ee::Ptr<Node> child = (*node)->firstChild;
                while (child && child->nameHash != hash)
                    child = child->nextSibling;
                if (!child)
                {
                    if (name)
                        zge::stringCopy(name, component);
                    return kPathNotFound;
                }
                *node = child;
                kind = kindOf(child);
                if (kind == kVariableKind)
                {
                    const ee::Ptr<VariableType> type = child->variableType;
                    if (name)
                        zge::stringCopy(name, child->name);
                    if (typeName)
                        zge::stringCopy(typeName, type->name);
                    // A path going on through a variable with no
                    // children yet: its type fills them in.
                    if (*path != 0 && !child->firstChild)
                    {
                        const auto expand = child->variableType->expand;
                        if (expand)
                            expand(child);
                    }
                }
            }
            if (*path == 0)
                break;
        }

        if (!dereference || kind != kVariableKind)
            return kind;
        // "*name": the variable's text is a path, followed in turn.
        const ee::Ptr<Node> variable = *node;
        variableText(variable->parent, variable->name, target);
        return resolvePath(target, name, typeName, node);
    }
    READABLE_FUNCTION_OWN(resolvePath, Verified, dm2, resolveObjectPath);

    uint64_t variableText(ee::Ptr<Node> parent, ee::Address name, ee::Ptr<char> out)
    {
        if (!parent || !name || !out)
            return 0;
        const ee::Ptr<Node> variable(findNode(parent, name, uint64_t{kVariableKind}));
        if (!variable)
            return 0;
        const auto toText = variable->variableType->toText;
        if (!toText)
            return 0;
        return toText(variable, out);
    }
    READABLE_FUNCTION_OWN(variableText, Verified, dm2, widgetVariableText);

    int32_t copyVariable(ee::Address parent, ee::Address name, ee::Address out)
    {
        const auto done = [](int32_t result)
        {
            return result;
        };
        if (!parent)
            return done(0);
        if (!name)
            return done(0);
        if (!out)
            return done(0);
        const ee::Ptr<Node> variable(findNode(ee::Ptr<Node>(parent), name, uint64_t{kVariableKind}));
        if (!variable)
            return done(0);
        variable->variableType->copyOut(variable, out);
        return done(1);
    }
    READABLE_FUNCTION_OWN(copyVariable, Verified, dm2, copyWidgetVariable);
}

namespace zge
{
    // stringTable (zge/text/strings.cpp): the string table in use.
    ee::Ptr<StringTable> stringTable();
}

namespace zge::zgui
{
    int32_t setVariable(ee::Address parent, ee::Address name, ee::Address value)
    {
        const auto done = [](int32_t result)
        {
            return result;
        };
        if (!parent)
            return done(0);
        if (!name)
            return done(0);
        if (!value)
            return done(0);
        const ee::Ptr<Node> variable(findNode(ee::Ptr<Node>(parent), name, uint64_t{kVariableKind}));
        if (!variable || (variable->message & kVariableLocked))
            return done(0);
        variable->variableType->set(variable, value);
        return done(1);
    }
    READABLE_FUNCTION_OWN(setVariable, Verified, dm2, setWidgetVariable);

    namespace
    {
        // libc's memset: what the compiler makes of a local key buffer's "" initializer (expandUiStrings).
        void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game(sym::memset, destination, byte, bytes); }
    }

    uint32_t expandText(ee::Ptr<const char> text)
    {
        if (!text)
            return kNoText;
        const ee::Ptr<zge::StringTable> saved = stringTable();
        setStringTable(*g_uiStrings);
        zge::expandStringKeys(ee::Ptr<char>(kSharedText).get(), text.get());
        setStringTable(saved);
        return kSharedText;
    }
    READABLE_FUNCTION_OWN(expandText, Verified, dm2, expandUiStrings);

    namespace
    {
        // Where a $KEY ends in ZGE2's expansion: a space, the end, a line end,
        // ':' or '"' (ZGE3's expandStringKeys ends one at '\\' too).
        bool endsUiKey(char c)
        {
            return c == ' ' || c == 0 || c == '\r' || c == '\n' || c == ':' || c == '"';
        }
    }

    // expandUiStrings: ZGE2's expandText: the text with its $KEYS from the UI's
    // string table (swapped in for the call) into zgui's shared text buffer,
    // which it returns; "" (noText2) for no text. ZGE2 writes the expansion
    // out itself where ZGE3 calls expandStringKeys: a key's text goes in, an
    // unknown key stays as written.
    uint32_t expandUiStrings(ee::Ptr<const char> text)
    {
        ee::Local<char[0x40]> name;
        if (!text)
            return kNoText;
        const ee::Ptr<zge::StringTable> saved = stringTable();
        setStringTable(*g_uiStrings);
        const ee::Ptr<char> buffer(kSharedText);
        ee::Ptr<char> out = buffer;
        *out = 0;
        uint32_t at = 0;
        while (text[at] != 0)
        {
            if (text[at] != '$')
            {
                *out = text[at];
                ++out;
                ++at;
                continue;
            }
            name[0] = 0;
            memset(ee::Ptr<char>(name) + 1, 0, 0x3F);
            ++at;
            ee::Ptr<char> nameEnd(name);
            while (!endsUiKey(text[at]))
            {
                *nameEnd = text[at];
                ++nameEnd;
                *nameEnd = 0;
                ++at;
            }
            const char *const found = zge::stringTableText(zge::nameHash(name));
            if (*found)
            {
                zge::stringCopy(out, found);
                out = out + zge::stringLength(found);
            }
            else
            {
                zge::stringCopy(out, kDollar);
                ++out;
                zge::stringCopy(out, name);
                out = out + zge::stringLength(name);
            }
        }
        *out = 0;
        setStringTable(saved);
        return kSharedText;
    }
}

// ZGE2's forms: a widget is a scene object with its WidgetRecord, its
// children the objects hanging from it, its variables its record's
// ListedVariables, and its place and shown flag the object's own.
namespace zge::zgui
{
    namespace
    {
        // The child widget of `object` called `name` (no case), or 0.
        ee::Ptr<zge::SceneObject> childWidgetNamed(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name)
        {
            for (ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object); child;
                 child = zge::nextAttachedSibling(child))
            {
                if (sceneObjectKind(child) == kWidgetObjectKind &&
                    zge::stringCompareNoCase(widgetRecord(child)->name, name) == 0)
                    return child;
            }
            return ee::Ptr<zge::SceneObject>(0u);
        }

        // The ACTION of `record` called `name` (no case), or 0.
        ee::Ptr<ListedAction> actionNamed(ee::Ptr<WidgetRecord> record, ee::Ptr<const char> name)
        {
            for (ee::Ptr<zge::ListNode> entry = record->actions.head; entry; entry = entry->next)
            {
                const ee::Ptr<ListedAction> action = zge::recordOfNode<ListedAction>(entry);
                if (zge::stringCompareNoCase(action->name, name) == 0)
                    return action;
            }
            return ee::Ptr<ListedAction>(0u);
        }
    }

    int32_t resolveObjectPath(ee::Ptr<const char> path, ee::Ptr<char> name, ee::Ptr<char> typeName,
                              ee::Ptr<ee::Address> found)
    {
        ee::Local<char[0x40]> component;
        ee::Local<char[0x400]> target; // a dereferenced variable's "name<TAB>value"
        ee::Local<uint32_t> dereference;
        *dereference = 0;
        if (*path == '{')
            return kObjectPathLiteral;

        int32_t kind = kObjectPathNotFound;
        ee::Ptr<zge::List> variables(0u); // the variables of the widget or ACTION reached
        if (*path == '*')
        {
            *dereference = 1;
            ++path;
        }
        *found = *g_scriptOwner;
        // Up one level per leading dot.
        uint32_t at = 0;
        while (path[at] == '.')
        {
            *found = zge::attachedParent(foundWidget(*found));
            ++at;
        }

        // Each component up to a dot: the root, a widget below the one
        // reached, else one of its ACTIONs.
        uint32_t startAt = at;
        for (; path[at] != 0; ++at)
        {
            if (path[at] != '.')
                continue;
            const uint32_t length = at - startAt;
            zge::stringCopyN(component, path + startAt, length);
            component[length] = 0;
            if (zge::compareStrings(kRootName, component) == 0)
                *found = *g_root;
            else if (const ee::Ptr<zge::SceneObject> child = childWidgetNamed(foundWidget(*found), component))
            {
                *found = child;
                kind = kObjectPathWidget;
                variables = widgetProperties(child);
            }
            else if (const ee::Ptr<ListedAction> action = actionNamed(widgetRecord(foundWidget(*found)), component))
            {
                *found = action;
                kind = kObjectPathAction;
                variables = actionProperties(action);
            }
            else
                return kObjectPathNotFound;
            startAt = at + 1;
        }

        // The last component: the variable's name (spaces before it skipped).
        if (!variables)
        {
            variables = widgetProperties(foundWidget(*found));
            kind = kObjectPathWidget;
        }
        ee::Ptr<const char> variableName = path + startAt;
        while (*variableName == ' ')
            ++variableName;
        zge::stringCopy(name, variableName);
        const uint32_t hash = zge::nameHash(variableName.get());
        for (ee::Ptr<zge::ListNode> entry = variables->head; entry; entry = entry->next)
        {
            const ee::Ptr<ListedVariable> variable = zge::recordOfNode<ListedVariable>(entry);
            if (variable->nameHash != hash)
                continue;
            if (!*dereference)
            {
                zge::stringCopy(typeName, variable->variableType->name);
                return kind | kObjectPathVariable;
            }
            // "*name": the variable's text (its value after the tab, perhaps
            // quoted) is a path, followed in turn.
            if (kind == kObjectPathWidget)
                widgetVariableText(foundWidget(*found), name, target);
            else
                listedVariableText(actionProperties(foundAction(*found)), name, target);
            ee::Ptr<char> value(zge::stringFind(target, '\t'));
            value = value + (value[1] == '"' ? 2 : 1);
            const int32_t length = zge::stringLength(value);
            if (value[length - 1] == '"')
                value[length - 1] = 0;
            return resolveObjectPath(value, name, typeName, found);
        }
        return kind;
    }

    uint32_t widgetVariableText(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Ptr<char> out)
    {
        ee::Local<char[0x400]> text;
        fillBytes(text, 0, 0x400);
        *out = 0;
        if (!object)
            return 0;
        uint32_t result = 1;
        if (zge::stringCompareNoCase(name, kPositionName) == 0)
        {
            const zge::Vec4 &position = object->position;
            zge::formatString(text, kPositionFormat, sce::libgcc::fptodp(position.x), sce::libgcc::fptodp(position.y),
                              sce::libgcc::fptodp(position.z));
            result = nearZero(position.x) && nearZero(position.y) && nearZero(position.z) ? 0 : 1;
        }
        else if (zge::stringCompareNoCase(name, kOrientationName) == 0)
        {
            const zge::Quaternion &orientation = object->orientation;
            zge::formatString(text, kOrientationFormat, sce::libgcc::fptodp(orientation.x),
                              sce::libgcc::fptodp(orientation.y), sce::libgcc::fptodp(orientation.z),
                              sce::libgcc::fptodp(orientation.w));
            const bool identity = nearZero(orientation.x) && nearZero(orientation.y) && nearZero(orientation.z) &&
                                  nearZero(orientation.w - ee::Float(1.0f));
            result = identity ? 0 : 1;
        }
        else if (zge::stringCompareNoCase(name, kVisibleName) == 0)
            zge::formatString(text, kVisibleFormat, static_cast<uint64_t>(widgetObjectShown(object) ? 1u : 0u));
        else
            result = static_cast<uint32_t>(listedVariableText(widgetProperties(object), name, text));
        zge::formatString(out, kNameTabValueFormat, name, text);
        return result;
    }

    int32_t copyWidgetVariable(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Address out)
    {
        if (!object)
            return 0;
        if (zge::stringCompareNoCase(name, kPositionName) == 0)
        {
            *ee::Ptr<zge::Vec4>(out) = object->position;
            return 1;
        }
        if (zge::stringCompareNoCase(name, kOrientationName) == 0)
        {
            zge::quaternionCopy(ee::Ptr<zge::Quaternion>(out), ee::Ptr<const zge::Quaternion>(&object->orientation));
            return 1;
        }
        if (zge::stringCompareNoCase(name, kVisibleName) == 0)
        {
            *ee::Ptr<uint32_t>(out) = widgetObjectShown(object) ? 1u : 0u;
            return 1;
        }
        return copyListedVariable(widgetProperties(object), name, out);
    }

    int32_t setWidgetVariable(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Address value)
    {
        if (!object)
            return 0;
        if (zge::stringCompareNoCase(name, kPositionName) == 0)
        {
            zge::setObjectPosition(object, ee::Ptr<zge::Vec4>(value));
            return 1;
        }
        if (zge::stringCompareNoCase(name, kOrientationName) == 0)
        {
            zge::setObjectOrientation(object, ee::Ptr<zge::Quaternion>(value));
            return 1;
        }
        if (zge::stringCompareNoCase(name, kVisibleName) == 0)
        {
            zge::setSceneObjectShown(object, *ee::Ptr<uint32_t>(value));
            return 1;
        }
        return setListedVariable(widgetProperties(object), name, value);
    }

    // setWidgetVariableText (ZGE2 only): Visible's bit is written on the
    // object itself here, with no word to its class (setWidgetVariable's
    // setSceneObjectShown tells it).
    inline int32_t setWidgetVariableText(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name,
                                         ee::Ptr<const char> text)
    {
        ee::Local<int32_t> shown;
        if (zge::stringCompareNoCase(name, kPositionName) == 0)
        {
            zge::Vec4 &position = object->position;
            ee::Ptr<const char> rest = parseFloat(text, ee::Ptr<ee::Float>(&position.x));
            rest = parseFloat(rest, ee::Ptr<ee::Float>(&position.y));
            parseFloat(rest, ee::Ptr<ee::Float>(&position.z));
            zge::setObjectPosition(object, ee::Ptr<zge::Vec4>(&position));
            return 1;
        }
        if (zge::stringCompareNoCase(name, kOrientationName) == 0)
        {
            zge::Quaternion &orientation = object->orientation;
            ee::Ptr<const char> rest = parseFloat(text, ee::Ptr<ee::Float>(&orientation.x));
            rest = parseFloat(rest, ee::Ptr<ee::Float>(&orientation.y));
            rest = parseFloat(rest, ee::Ptr<ee::Float>(&orientation.z));
            parseFloat(rest, ee::Ptr<ee::Float>(&orientation.w));
            zge::setObjectOrientation(object, ee::Ptr<zge::Quaternion>(&orientation));
            return 1;
        }
        if (zge::stringCompareNoCase(name, kVisibleName) == 0)
        {
            parseInt(text, shown);
            object->flags = (object->flags & ~kWidgetObjectShown) | ((*shown & 1) ? kWidgetObjectShown : 0u);
            return 1;
        }
        return setListedVariableText(widgetProperties(object), name, text);
    }
    READABLE_FUNCTION_ONLY(dm2, setWidgetVariableText);
}

// A widget's or an ACTION's variable by its path from the root (ZGE2 only:
// the game's menus set and read the UI's variables with these).
namespace zge::zgui
{
    // setVariableAtPath: the variable at `path` (followed from the root, which
    // becomes the script owner) set from a value its type keeps: 1, 0 when
    // the path leads to no variable.
    inline int32_t setVariableAtPath(ee::Ptr<const char> path, ee::Address value)
    {
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> name;
        ee::Local<ee::Address> found;
        setScriptOwner(*g_root);
        const int32_t kind = resolveObjectPath(path, name, typeName, found);
        if (kind == kWidgetVariablePath)
        {
            setWidgetVariable(foundWidget(*found), name, value);
            return 1;
        }
        if (kind == kActionVariablePath)
        {
            setListedVariable(actionProperties(foundAction(*found)), name, value);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, setVariableAtPath);

    // copyVariableAtPath: the variable at `path` copied out as its type keeps
    // it: 1, 0 when the path leads to no variable.
    inline int32_t copyVariableAtPath(ee::Ptr<const char> path, ee::Address out)
    {
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> name;
        ee::Local<ee::Address> found;
        setScriptOwner(*g_root);
        const int32_t kind = resolveObjectPath(path, name, typeName, found);
        if (kind == kWidgetVariablePath)
        {
            copyWidgetVariable(foundWidget(*found), name, out);
            return 1;
        }
        if (kind == kActionVariablePath)
        {
            copyListedVariable(actionProperties(foundAction(*found)), name, out);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_ONLY(dm2, copyVariableAtPath);

    // defineWidgetProperty: a variable of the type named added to the
    // widget's own (its record's list) over `value`: 1, 0 without an object.
    inline int32_t defineWidgetProperty(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name,
                                        ee::Ptr<const char> typeName, ee::Address value)
    {
        if (!object)
            return 0;
        addListedProperty(widgetProperties(object), name, typeName, value);
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, defineWidgetProperty);
}
