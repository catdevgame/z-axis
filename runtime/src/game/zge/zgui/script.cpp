// zgui's script interpreter (script.h): running a script, splitting it
// into statements and labels, reading tokens, the command table, GOTO and
// EXIT, and finding a node by path.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "sym/zge/zgui/script.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kStrncpy = sym::stringCopyN; // (dst, src, n) (engine wrapper)
        constexpr uint32_t kValueText = sym::valueText; // (token, out, report) the token's value as text
        // (format, ...) a script error (report.cpp)
        constexpr uint32_t kInvalidIdentifierText = sym::invalidIdentifier2; // "zgui: (Invalid Identifier: '%s')"

        // A token buffer the commands share (tokenBuffer, 0x200 bytes).
        constexpr uint32_t kTokenBuffer = sym::tokenBuffer;


        bool isBlank(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
    }

    void runScript(ee::Ptr<Node> widget, ee::Ptr<char> text, ee::Address argument)
    {
        // The script's state is the original's stack frame; nested scripts
        // (PLAY, CALL) get their own below it and put this one back.
        const ee::Ptr<ScriptContext> outer = *g_script;
        if (!widget)
            widget = *g_root;
        if (!text)
            return;
        if (*text == 0)
            return;
        ee::Local<ScriptContext> context;
        context->text = text;
        context->self = widget;
        context->argument = ee::Ptr<Node>(argument);
        *g_script = context;
        splitScript();

        ee::Ptr<ScriptContext> running = *g_script;
        running->next = 0;
        running->stop = 0;
        bool restored = false; // the last statement run: restored on the way out (no block of its own)
        if (running->statementCount)
        {
            for (bool first = true;; first = false)
            {
                const uint32_t i = running->next;
                running->next = i + 1;
                runStatement(running->statement[i]);
                running = *g_script;
                if (!(running->next < running->statementCount))
                {
                    *g_script = outer;
                    restored = true;
                    break;
                }
                if (running->stop)
                    break;
            }
        }
        if (!restored)
            *g_script = outer;
    }

    // scanScriptLabels (ZGE2 only): the running script's labels (":name"
    // lines) and the line after each, its cursor back at the text's start.
    // A line ends at a carriage return, and its "\r\n" is stepped over; the
    // scan of each line after the first starts at its second character.
    inline void scanScriptLabels()
    {
        ee::Ptr<LineScriptContext> context = *g_lineScript;
        context->labelCount = 0;
        context->cursor = context->text;
        uint32_t length = 0;
        for (;;)
        {
            ee::Ptr<char> line = context->cursor;
            while (line[length] != 0 && line[length] != '\r')
                ++length;
            if (line[0] == ':')
            {
                zge::stringCopyN(context->labelName[context->labelCount], line + 1, length - 1);
                context = *g_lineScript;
                context->labelName[context->labelCount][length - 1] = 0;
                context = *g_lineScript;
                const ee::Ptr<char> after = context->cursor + length;
                context->labelLine[context->labelCount] = *after == 0 ? after : after + 2;
                context = *g_lineScript;
                context->labelCount = context->labelCount + 1;
            }
            line = context->cursor;
            if (line[length] == 0)
                break;
            context->cursor = line + (length + 2);
            context = *g_lineScript;
            length = 1;
        }
        context->cursor = context->text;
    }
    READABLE_FUNCTION_ONLY(dm2, scanScriptLabels);

    // runScriptLines: ZGE2's runScript: `text` run for `widget` (the root if
    // 0) a line at a time, each line's command looked up by name and handed
    // the rest of the line, until the cursor reaches the text's end (EXIT
    // and GOTO move it). ZGE3 splits the text into statements first and
    // gives the script an argument ("@").
    void runScriptLines(ee::Ptr<Node> widget, ee::Ptr<char> text)
    {
        const ee::Ptr<LineScriptContext> outer = *g_lineScript;
        if (!widget)
            widget = *g_root;
        if (!text)
            return;
        if (*text == 0)
            return;
        ee::Local<LineScriptContext> context;
        ee::Local<char[0x100]> line;
        ee::Local<char[0x10]> word;
        context->text = text;
        *g_lineScript = context;
        scanScriptLabels();
        for (uint32_t length = 0;; length = 0)
        {
            ee::Ptr<LineScriptContext> running = *g_lineScript;
            while (running->cursor[length] != 0 && running->cursor[length] != '\r' && running->cursor[length] != '\n')
                ++length;
            zge::stringCopyN(line, running->cursor, length);
            line[length] = 0;
            const ee::Ptr<char> arguments(readToken(line, 0, word));
            running = *g_lineScript;
            const ee::Ptr<char> end = running->cursor + length;
            if (*end == 0)
                running->cursor = end;
            else
            {
                running->cursor = end + 1;
                if (*running->cursor == '\n' || *running->cursor == '\r')
                    running->cursor = running->cursor + 1;
            }
            *g_scriptOwner = widget;
            const ee::Ptr<LineCommand> commands = classCast<LineCommand>(kCommands);
            for (uint32_t i = 0; commands[i].handler; ++i)
            {
                if (zge::stringCompareNoCase(ee::Address(&commands[i].name[0]), word) == 0)
                {
                    commands[i].handler(arguments);
                    break;
                }
            }
            if (*(*g_lineScript)->cursor == 0)
                break;
        }
        *g_lineScript = outer;
    }
    // Commands load files (LOADZSC, LOADLIB) and run other scripts.
    READABLE_FUNCTION_WAITS_OWN(runScript, Verified, dm2, runScriptLines);

    void splitScript()
    {
        ee::Ptr<ScriptContext> context = *g_script;
        context->statementCount = 0;
        ee::Ptr<char> line = context->text;
        context->labelCount = 0;
        for (;;)
        {
            // Blank characters before the line (a space steps through its
            // own block first).
            const char first = *line;
            if (first == ' ')
            {
                ++line;
                continue;
            }
            if (isBlank(first))
            {
                ++line;
                continue;
            }
            if (first == 0)
                return;
            // The line's end: the first carriage return after it anywhere in
            // the text, else the first line feed.
            ee::Ptr<char> end(stringFind(line, 0x0D));
            if (!end)
                end = ee::Ptr<char>(stringFind(line, 0x0A));
            if (line[0] == '/')
            {
                if (line[1] == '/')
                {
                    // A comment: the next line, straight to its first
                    // character (the loop's start, past the blank check's
                    // first test).
                    if (!end)
                        return;
                    line = end + 1;
                    continue;
                }
            }
            context = *g_script;
            if (line[0] == ':')
            {
                // A label: its name (to the line's end, at most what the
                // copy is given) and the statement it stands before.
                const ee::Ptr<char> name = (*g_script)->labelName[context->labelCount];
                if (!end)
                    zge::stringCopy(name, line + 1);
                else
                {
                    const uint32_t length = static_cast<uint32_t>(end - line);
                    zge::stringCopyN(name, line + 1, length - 1);
                    context = *g_script;
                    context->labelName[context->labelCount][length - 1] = 0;
                }
                context = *g_script;
                context->labelStatement[context->labelCount] = context->statementCount;
                context = *g_script;
                context->labelCount = context->labelCount + 1;
            }
            else
            {
                context->statement[context->statementCount] = line;
                context->statementCount = context->statementCount + 1;
            }
            if (!end)
                return;
            line = end + 1;
        }
    }
    READABLE_FUNCTION(splitScript, Verified);

    // runStatement: a statement's command run with the rest of the line (an
    // unknown command does nothing). The original hands back the command's
    // result; its callers (runScript, commandIf) drop it.
    void runStatement(ee::Ptr<char> line)
    {
        ee::Local<char> name;
        const ee::Ptr<char> arguments(readToken(line, 0, ee::Ptr<char>(name.address())));
        const uint32_t hash = zge::nameHash(ee::Ptr<char>(name.address()).get());
        for (uint32_t i = 0;; ++i)
        {
            const ee::Ptr<Command> command = kCommands + i;
            if (!command->handler)
                return;
            if (command->hash == hash)
            {
                command->handler(arguments);
                return;
            }
        }
    }
    READABLE_FUNCTION_WAITS(runStatement, Verified);

    // readToken: token `index` of the text (blanks outside quotes and braces
    // separate them) copied to `out`; returns where it ends, or 0 past the
    // text's end.
    ee::Ptr<char> readToken(ee::Ptr<char> text, int32_t index, ee::Ptr<char> out)
    {
        // `index` counts down the separators still to pass; the token is
        // copied while it is 0 and ends at the next separator (-1).
        ee::Ptr<char> p = text;
        if (*p == ' ' || *p == '\t')
        {
            for (bool first = true;; first = false)
            {
                ++p;
                if (*p != ' ' && *p != '\t')
                    break;
            }
        }
        bool inQuotes = false, inBraces = false;
        for (bool first = true;; first = false)
        {
            const char c = *p;
            // The original's tests in order, each failing test's next block
            // marked; a separator (a blank outside quotes and braces, or the
            // end) counts down.
            bool separator = false;
            if (c == '{' && !inQuotes)
                inBraces = true;
            else
            {
                if (c == '}' && !inQuotes)
                    inBraces = false;
                else
                {
                    if (c == '"')
                        inQuotes = !inQuotes;
                    else
                    {
                        if (c == ' ' || c == '\r' || c == '\n')
                        {
                            if (!inQuotes && !inBraces)
                                separator = true;
                        }
                        else
                            separator = c == 0;
                    }
                }
            }
            if (separator)
            {
                --index;
                if (index == -1)
                {
                    *out = 0;
                    return p;
                }
                if (c == 0)
                    return ee::Ptr<char>();
            }
            ++p;
            if (index == 0)
            {
                *out = c;
                ++out;
            }
        }
    }

    // readSpacedToken: ZGE2's readToken: token `index` of the text copied to
    // `out`, the tokens separated by runs of spaces outside quotes and
    // braces (ZGE3 takes any blank). Leading spaces are skipped. It returns
    // the last space after the token, or the text's end for the last token;
    // 0 when the text ends before token `index`. A '}' ends the braces
    // whatever their depth.
    ee::Ptr<char> readSpacedToken(ee::Ptr<char> text, int32_t index, ee::Ptr<char> out)
    {
        int32_t separatorsLeft = index - 1; // -1: the token being read is the one wanted
        uint32_t i = 0;
        while (text[i] == ' ')
            ++i;
        uint32_t start = i;
        bool inQuotes = false, inBraces = false;
        for (; text[i] != 0; ++i)
        {
            const char c = text[i];
            if (!inQuotes && c == '{')
                inBraces = true;
            if (!inQuotes && c == '}')
                inBraces = false;
            if (c == '"')
                inQuotes = !inQuotes;
            if (c != ' ' || inQuotes || inBraces)
                continue;
            const uint32_t end = i;
            while (text[i + 1] == ' ')
                ++i;
            if (separatorsLeft == -1)
            {
                zge::stringCopyN(out, text + start, end - start);
                out[end - start] = 0;
                return text + i;
            }
            --separatorsLeft;
            start = i + 1;
        }
        if (separatorsLeft != -1)
            return ee::Ptr<char>(0u);
        zge::stringCopyN(out, text + start, i - start);
        out[i - start] = 0;
        return text + i;
    }
    READABLE_FUNCTION_OWN(readToken, Verified, dm2, readSpacedToken);

    int32_t commandExit(ee::Ptr<char>)
    {
        (*g_script)->stop = 1;
        return 1;
    }
    READABLE_FUNCTION(commandExit, Verified);

    void commandGoto(ee::Ptr<char> arguments)
    {
        ee::Local<char> label;
        readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer));
        valueText(ee::Ptr<const char>(kTokenBuffer), ee::Ptr<char>(label.address()), 0);
        if (!(*g_script)->labelCount)
            return;
        for (uint32_t i = 0;; ++i)
        {
            const ee::Ptr<const char> name = (*g_script)->labelName[i];
            if (stringCompareNoCase(name, label) == 0)
            {
                const ee::Ptr<ScriptContext> context = *g_script;
                context->next = context->labelStatement[i];
                return;
            }
            if (!(i + 1 < (*g_script)->labelCount))
                return;
        }
    }
    READABLE_FUNCTION_WAITS(commandGoto, Verified);

    ee::Ptr<Node> findNode(ee::Ptr<Node> root, ee::Address path, uint64_t kind)
    {
        ee::Local<char> copy;
        ee::Local<char> component;
        zge::stringCopy(ee::Ptr<char>(copy.address()), path);
        if (!root)
            root = *g_root;
        ee::Ptr<char> p = ee::Ptr<char>(copy.address());
        if (*p == '.')
        {
            // Up one level per leading dot.
            for (bool first = true;; first = false)
            {
                if (root)
                    root = root->parent;
                ++p;
                if (*p != '.')
                    break;
            }
        }
        ee::Ptr<Node> node = root;
        for (;;)
        {
            // The next component (to a dot or the end).
            ee::Ptr<char> c = ee::Ptr<char>(component.address());
            for (;;)
            {
                *c = *p;
                if (*p == 0 || *p == '.')
                {
                    *c = 0;
                    break;
                }
                ++c;
                ++p;
            }
            const uint32_t hash = zge::nameHash(ee::Ptr<char>(component.address()).get());
            const bool last = *p == 0;
            ee::Ptr<Node> child = node->firstChild;
            for (;;)
            {
                if (!child || child->nameHash == hash)
                    break;
                child = child->nextSibling;
            }
            if (child)
            {
                node = child;
                if (last)
                    return static_cast<uint64_t>(child->kind()) == kind ? child : ee::Ptr<Node>();
            }
            if (last)
                return {};
            ++p;
        }
    }
    READABLE_FUNCTION_OWN(findNode, Verified, dm2, findWidgetByPath);

    // findWidgetByPath: ZGE2's findNode (object, path): the widget a dotted
    // path of names leads to from a scene object: at each step the widget
    // object hanging from it whose id (its class's hook: the hash of its
    // name) is the component's hash; one whose class has no id hook stands
    // for an empty component. 0 if a step finds none.
    ee::Ptr<zge::SceneObject> findWidgetByPath(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> path)
    {
        ee::Local<char[0x40]> component;
        for (;;)
        {
            const ee::Ptr<const char> dot(zge::stringFind(path, '.'));
            if (dot)
            {
                const uint32_t length = static_cast<uint32_t>(ee::Address(dot) - ee::Address(path));
                zge::stringCopyN(component, path, length);
                path = dot + 1;
                component[length] = 0;
            }
            else
            {
                zge::stringCopy(component, path);
                path = {};
            }
            const uint32_t hash = zge::nameHash(ee::Ptr<const char>(component.address()));
            ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object);
            for (;; child = zge::nextAttachedSibling(child))
            {
                if (!child)
                    return {};
                if (sceneObjectKind(child) != kWidgetObjectKind)
                    continue;
                if (const auto id = child->objectClass->id)
                {
                    if (id(child) == hash)
                        break;
                }
                else if (hash == 0)
                    break;
            }
            object = child;
            if (!path)
                return child;
        }
    }

    namespace
    {
        constexpr uint32_t kHeapHigh = ee::high(sym::commands);  // the command table's high half
        constexpr uint32_t kNamesHigh = ee::high(sym::boolName); // the type names' high half
        constexpr uint32_t kBoolName = sym::boolName, kFloatName = sym::floatName, kUint8Name = sym::uint8Name,
                           kSint32Name = sym::sint32Name, kColorName = sym::colorName, kVectorName = sym::vectorName,
                           kQuaternionName = sym::quaternionName, kStringName = sym::stringName, kAssetName = sym::assetName2,
                           kEventName = sym::eventTypeName;
    }

    void initScriptHashes()
    {
        // Each command's name hash, for runStatement's search.
        for (ee::Ptr<Command> command = kCommands; command->handler; ++command)
            command->hash = zge::nameHash(command->name);
        // The variable types' names (script.h).
        const struct
        {
            uint32_t name;
            uint32_t *hash;
        } types[] = {
            {kBoolName, g_boolType.get()},       {kFloatName, g_floatType.get()},
            {kUint8Name, g_uint8Type.get()},     {kSint32Name, g_sint32Type.get()},
            {kColorName, g_colorType.get()},     {kVectorName, g_vectorType.get()},
            {kQuaternionName, g_quaternionType.get()}, {kStringName, g_stringType.get()},
            {kAssetName, g_assetType.get()},     {kEventName, g_eventType.get()},
        };
        for (const auto &type : types)
            *type.hash = zge::nameHash(ee::Ptr<const char>(type.name));
    }
    READABLE_FUNCTION(initScriptHashes, Verified);

    int32_t setVariableTextByPath(ee::Ptr<const char> path, ee::Ptr<const char> text)
    {
        // The variable at `path` (from the running script's widget) set
        // from text: setVariableText's result, else reported.
        ee::Local<char> name;
        ee::Local<char> typeName;
        ee::Local<ee::Ptr<Node>> node;
        const int32_t kind = resolvePath(path, ee::Ptr<char>(name.address()), ee::Ptr<char>(typeName.address()), node);
        int32_t result;
        if (kind == kVariableKind)
            result = setVariableText(ee::Ptr<Node>(*node)->parent, ee::Ptr<const char>(name.address()), text);
        else
            result = report(kInvalidIdentifierText, path);
        return result;
    }
    // Draft: no caller (dead) in ZGE3; ZGE2's commands set variables from text through it.
    READABLE_FUNCTION_WAITS_OWN(setVariableTextByPath, Draft, dm2, setVariableTextAtPath);

    // setVariableTextAtPath: ZGE2's setVariableTextByPath: the variable at
    // `path` (resolveObjectPath) set from text; a widget's ZassetID variable
    // set tells the widget to refresh (Message::Refresh from the running
    // script's widget, at once). No report, no result.
    namespace
    {
        constexpr uint32_t kAssetTypeName = sym::assetName; // "ZassetID" (ZGE2's spelling of the type)
    }
    void setVariableTextAtPath(ee::Ptr<const char> path, ee::Ptr<const char> text)
    {
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> typeName;
        ee::Local<ee::Address> found;
        const int32_t kind = resolveObjectPath(path, name, typeName, found);
        if (kind == kWidgetVariablePath)
        {
            setWidgetVariableText(foundWidget(*found), name, text);
            if (zge::stringCompareNoCase(typeName, kAssetTypeName) == 0)
                zge::sendNewMessage(*g_scriptOwner, *found, static_cast<uint32_t>(Message::Refresh), 0u, zge::kMessageNow);
        }
        else if (kind == kActionVariablePath)
            setListedVariableText(actionProperties(foundAction(*found)), name, text);
    }

    // setScriptOwner (ZGE2 only): runScriptLines sets the owner itself before
    // each command; a library load's finish sets it before its PLAY.
    inline void setScriptOwner(ee::Ptr<Node> widget)
    {
        *g_scriptOwner = widget;
    }
    READABLE_FUNCTION_ONLY(dm2, setScriptOwner);

    ee::Ptr<Node> currentScriptOwner()
    {
        // The widget the running script runs for (0 outside a script). ZGE2
        // keeps it in a global of its own, which its runScript sets before
        // each command.
        if constexpr (game::readable::kIsDm2)
            return *g_scriptOwner;
        if (const ee::Ptr<ScriptContext> script = *g_script)
            return script->self;
        return ee::Ptr<Node>(0u);
    }
    READABLE_FUNCTION(currentScriptOwner, Verified);

    namespace
    {
        constexpr uint32_t kTextStrcatSaveLoadChooseParkMenu = sym::g_textStrcatSaveLoadChooseParkMenu;
        constexpr uint32_t kTextStrcatSaveLoadChooseParkMenu2 = sym::g_textStrcatSaveLoadChooseParkMenu2;
        constexpr uint32_t kTextStrcatSaveLoadChooseParkMenu3 = sym::g_textStrcatSaveLoadChooseParkMenu3;
        constexpr uint32_t kTextStrcatSaveLoadChooseParkMenu4 = sym::g_textStrcatSaveLoadChooseParkMenu4;
        constexpr uint32_t kTextSaveLoadChooseParkMenuFileListMenu = sym::g_textSaveLoadChooseParkMenuFileListMenu;
        constexpr uint32_t kTextSaveLoadChooseParkMenuAuthorMenu = sym::g_textSaveLoadChooseParkMenuAuthorMenu;
        constexpr uint32_t kTextSaveLoadChooseParkMenuThemeMenu = sym::g_textSaveLoadChooseParkMenuThemeMenu;
        constexpr uint32_t kTextPlaySaveLoadChooseParkMenu2 = sym::g_textPlaySaveLoadChooseParkMenu2;
        constexpr uint32_t kTextPlaySaveLoadChooseParkMenu = sym::g_textPlaySaveLoadChooseParkMenu;
        constexpr uint32_t kTextPlaySaveLoadSaveInfoOnShutdown = sym::g_textPlaySaveLoadSaveInfoOnShutdown;
        constexpr uint32_t kMenuOnInitScript = sym::g_textPlaySaveLoadSaveLoadMenuOnInit;
        constexpr uint32_t kTextNew = sym::g_textNew;
        inline ee::Global<int32_t, sym::g_parkSaving> g_parkFileMenuShown; // g_parkFileMenuShown; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkSaveFileCount> g_parkSaveFileCount; // g_parkSaveFileCount; its name is a low-confidence guess (readable_names.tsv)
    }

}

namespace zge::zgui
{
    // findActionByPath (ZGE2 only): the ACTION a dotted path leads to from a
    // scene object: every component but the last an object hanging from the
    // one before by its id (its class's hook; one without the hook stands
    // for an empty component), the last an ACTION of the widget reached by
    // its name (no case). 0 if a step finds none, or without an object.
    inline ee::Ptr<ListedAction> findActionByPath(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> path)
    {
        ee::Local<char[0x40]> component;
        if (!object)
            return ee::Ptr<ListedAction>(0u);
        for (;;)
        {
            const ee::Ptr<const char> dot(zge::stringFind(path, '.'));
            if (dot)
            {
                const uint32_t length = static_cast<uint32_t>(ee::Address(dot) - ee::Address(path));
                zge::stringCopyN(component, path, length);
                path = dot + 1;
                component[length] = 0;
            }
            else
            {
                zge::stringCopy(component, path);
                path = {};
            }
            if (!path)
            {
                for (ee::Ptr<zge::ListNode> entry = widgetRecord(object)->actions.head; entry; entry = entry->next)
                {
                    const ee::Ptr<ListedAction> action = zge::recordOfNode<ListedAction>(entry);
                    if (zge::stringCompareNoCase(action->name, component) == 0)
                        return action;
                }
                return ee::Ptr<ListedAction>(0u);
            }
            const uint32_t hash = zge::nameHash(ee::Ptr<const char>(component.address()));
            ee::Ptr<zge::SceneObject> child = zge::firstAttachedChild(object);
            for (;; child = zge::nextAttachedSibling(child))
            {
                if (!child)
                    return ee::Ptr<ListedAction>(0u);
                if (const auto id = child->objectClass->id)
                {
                    if (id(child) == hash)
                        break;
                }
                else if (hash == 0)
                    break;
            }
            object = child;
        }
    }
    READABLE_FUNCTION_ONLY(dm2, findActionByPath);
}

