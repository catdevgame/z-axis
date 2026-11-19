#pragma once

// zgui's scripts: the text the UI's widgets carry (assets\ui\*.zsc, and
// strings in the game's code such as "PLAY SetScore"), run by an
// interpreter that reads the text itself each time (no compiled form).
//
// A script is lines. Each line is a statement, "COMMAND arguments", or a
// label ":NAME" (GOTO's target); "//" starts a comment line; blank lines
// and leading spaces, tabs and line ends are skipped. Arguments are tokens
// separated by spaces or line ends; "..." and {...} keep their spaces.
// Commands (the table at commands): SET, IF, PLAY, ADD, SUB, MULT, DIV,
// GOTO, LINEAR, RAND, DECLARE, UNDECLARE, STRCAT, STRCMP, STREXTRACT,
// STRTOKEN, REFRESH, EXIT, STOP, ABORT, LOADZSC, CALL, SETFOCUS, KILLFOCUS,
// DESTROY, LOADLIB, UNLOADLIB, USE_STR, LOCKINPUT, UNLOCKINPUT, ROTQUATX/Y/Z,
// DEBUG_PRINT, CANCELLOADLIB, REPORTLIB, FORMAT, LOADMODEL, LOADIMAGE.
//
// Names are paths through the zgui tree (widget.h), resolved from the
// widget the script runs for: "a.b.c" walks down by name, a leading "."
// goes up one level per dot, "^" is the widget itself, "@" the script's
// argument node, "Root" the tree's root; "*" in front dereferences a
// variable that holds a path. Variables are nodes of kind Variable whose
// type (int, float, string and the UI's own) parses, prints and combines
// their values.

#include "zge/zgui/widget.h"
#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/container/tree.h"
#include "zge/memory/dmm.h"
#include "zge/memory/heaps.h"
#include "zge/text/strings.h"

#include <cstddef>
#include "sym/zge/zgui/script.h.h"

namespace ee
{
    template <typename T>
    class Local; // ee/plain.h
}

namespace zge
{
    struct SceneObject; // zge/scene/scene.h
    uint32_t zmemDMMLock(ee::Ptr<DmmRegion> region, uint32_t index); // (dmm.cpp)
}

namespace zge::zgui
{
    constexpr uint16_t kVariableKind = 2; // a node holding a typed value (NodeKind 2)
    // A Variable's flags (Node::message).
    constexpr uint16_t kVariableOwnsValue = 1u << 0; // the value is freed with the variable (DECLARE's)
    constexpr uint16_t kVariableLocked = 1u << 1;    // set only by its owner (the game's variables)

    // A running script's state: the original's stack frame (runScript,
    // runScript, 0x8C0 bytes).
    struct ScriptContext
    {
        static constexpr uint32_t kMaxLabels = 32, kLabelLength = 0x20, kMaxStatements = 256;

        uint32_t labelCount;                         // +0x000
        uint32_t labelStatement[kMaxLabels];         // +0x004 the statement after each label
        char labelName[kMaxLabels][kLabelLength];    // +0x084
        uint32_t statementCount;                     // +0x484
        uint32_t next;                               // +0x488 the statement to run next (GOTO sets it)
        ee::Ptr<char> statement[kMaxStatements];     // +0x48C each statement's start in the text
        ee::Ptr<char> text;                          // +0x88C
        ee::Ptr<Node> argument;                      // +0x890 "@" (0: none)
        ee::Ptr<Node> self;                          // +0x894 the widget the script runs for
        uint32_t stop;                               // +0x898 EXIT
    };
    static_assert(offsetof(ScriptContext, labelName) == 0x084 && offsetof(ScriptContext, statementCount) == 0x484);
    static_assert(offsetof(ScriptContext, statement) == 0x48C && offsetof(ScriptContext, text) == 0x88C);
    static_assert(offsetof(ScriptContext, argument) == 0x890 && offsetof(ScriptContext, self) == 0x894);
    static_assert(offsetof(ScriptContext, stop) == 0x898);
    constexpr uint32_t kScriptFrameSize = 0x8C0;

    inline ee::Global<ee::Ptr<ScriptContext>, sym::g_script> g_script; // the script running now

    // ZGE2's running script: the original's stack frame (DM2's runScript, 0x48C
    // bytes at its start). ZGE2 reads the text a line at a time and keeps
    // no statement table: a label is found by name and the line after it
    // run next.
    struct LineScriptContext
    {
        static constexpr uint32_t kMaxLabels = 32, kLabelLength = 0x20;

        uint32_t labelCount;                         // +0x000
        ee::Ptr<char> labelLine[kMaxLabels];         // +0x004 the line after each label
        char labelName[kMaxLabels][kLabelLength];    // +0x084
        ee::Ptr<char> text;                          // +0x484
        ee::Ptr<char> cursor;                        // +0x488 the next line to run (GOTO sets it)
    };
    static_assert(offsetof(LineScriptContext, labelName) == 0x084 && offsetof(LineScriptContext, text) == 0x484);
    static_assert(offsetof(LineScriptContext, cursor) == 0x488 && sizeof(LineScriptContext) == 0x48C);
    inline ee::Global<ee::Ptr<LineScriptContext>, sym::g_script> g_lineScript; // ZGE2: the script running now
    // ZGE2: the running script's widget (its runScript sets it before each
    // command; the commands send their messages from it).
    inline ee::Global<ee::Ptr<Node>, sym::dm2::g_scriptOwner> g_scriptOwner;
    // setScriptOwner (ZGE2 only, script.cpp): the widget the commands run for
    // (a load's finish sets it before its PLAY).
    void setScriptOwner(ee::Ptr<Node> widget);

    // ZGE2's command (the same table, 0x14 bytes an entry): no hash, the
    // name compared as text.
    struct LineCommand
    {
        char name[0x10];
        ee::Callback<uint64_t(ee::Ptr<char>)> handler; // (arguments text)
    };
    static_assert(sizeof(LineCommand) == 0x14);
    // runScriptLines: ZGE2's runScript (script.cpp); scanScriptLabels (ZGE2
    // only) finds the running script's labels.
    void runScriptLines(ee::Ptr<Node> widget, ee::Ptr<char> text);
    void scanScriptLabels();

    // A command (the table at commands, ended by an entry without handler).
    struct Command
    {
        char name[0x10];
        ee::Callback<uint64_t(ee::Ptr<char>)> handler; // (arguments text)
        uint32_t hash;    // nameHash(name), set at boot
    };
    static_assert(sizeof(Command) == 0x18);
    constexpr ee::Ptr<Command> kCommands{sym::commands};

    // zgui's globals (g_zgui): the tree's root at +0x0C.
    inline ee::Global<ee::Ptr<Node>, sym::g_uiRoot> g_root;

    // runScript: runs `text` for `widget` (the root if 0) with
    // `argument` as "@", to its end, EXIT or STOP.
    void runScript(ee::Ptr<Node> widget, ee::Ptr<char> text, ee::Address argument);
    constexpr uint32_t kRunScript = sym::runScript;
    // splitScript: the running script's text split into statements and labels.
    void splitScript();
    // runStatement: runs one statement; what its command's handler returns
    // (the table's end when the command is unknown).
    void runStatement(ee::Ptr<char> line);
    // readToken: copies token `index` of `text` to `out` (index 0 the
    // first; a later one keeps the one separator before it); returns where
    // the copy stopped (the separator after it), or 0 when the text ends
    // first.
    ee::Ptr<char> readToken(ee::Ptr<char> text, int32_t index, ee::Ptr<char> out);
    // commandExit, EXIT: stops the running script after this statement (1).
    int32_t commandExit(ee::Ptr<char> arguments);
    // commandGoto, GOTO label.
    void commandGoto(ee::Ptr<char> arguments);
    struct Color; // a ZRGBA_PAL value

    // A Variable node's type (Node::variableType; the types are a list at
    // g_variableTypes): its name and what it does with values. A method left 0
    // is one the type has not got.
    struct VariableType
    {
        using Variable = ee::Ptr<Node>;
        template <typename T>
        using Method = ee::Callback<void(Variable, ee::Ptr<T>)>;

        ee::Ptr<VariableType> next; // +0x00 the next type
        ee::Ptr<VariableType> previous; // +0x04 (the list's links)
        char name[0x20];            // +0x08 "Sint32", "ZFloat", "String" ...
#if PS2X_GAME_DM2
        // ZGE2's type (0x40 bytes; DM2's 11 static tables, String's first):
        // no name hash, and five methods, those of ZGE3's with the same names
        // (DM2's tables hold the counterparts of AI's methods in these slots).
        ee::Callback<void(Variable, uint32_t)> set;            // +0x28 the value set (setVariable)
        ee::Callback<uint64_t(Variable, ee::Address)> copyOut; // +0x2C the value copied out (copyVariable)
        ee::Callback<uint64_t(Variable, ee::Address)> setText; // +0x30 the value set from text
        ee::Callback<uint64_t(Variable, ee::Ptr<char>)> toText; // +0x34 the value as text into out
        ee::Callback<void(Variable)> destroy;                  // +0x38 a temporary's value freed
        uint32_t _3C;                                          // +0x3C 0 in all 11 tables
        // ZGE3's other fields, past ZGE2's record: shared code names them in
        // ZGE3's steps; no ZGE2 table has them.
        uint32_t hash;
        Method<ee::Float> floatValue;
        Method<int32_t> intValue;
        Method<Color> colorValue;
        Method<zge::Vec4> vectorValue;
        Method<ee::Float> setFloat;
        Method<int32_t> setInt;
        Method<Color> setColor;
        Method<zge::Vec4> setVector;
        ee::Callback<void(Variable)> expand;
#else
        uint32_t hash;              // +0x28 nameHash(name)
        ee::Callback<void(Variable, uint32_t)> set;     // +0x2C the value set (setVariable)
        ee::Callback<uint64_t(Variable, ee::Address)> copyOut; // +0x30 the value copied out as the type keeps it (copyVariable)
        ee::Callback<void(Variable)> destroy;           // +0x34 a temporary's value freed
        // The value read out, for operateVariables.
        Method<ee::Float> floatValue;                   // +0x38
        Method<int32_t> intValue;                       // +0x3C
        // +0x40 the value as text into out: 1, or 0 for an empty value (a
        // zero vector, the identity quaternion, an empty string).
        ee::Callback<uint64_t(Variable, ee::Ptr<char>)> toText;
        Method<Color> colorValue;                // +0x44
        Method<zge::Vec4> vectorValue;                  // +0x48
        // The value set.
        Method<ee::Float> setFloat;                     // +0x4C
        Method<int32_t> setInt;                         // +0x50
        ee::Callback<uint64_t(Variable, ee::Address)> setText; // +0x54 also makes a literal's temporary
        Method<Color> setColor;                  // +0x58
        Method<zge::Vec4> setVector;                    // +0x5C
        ee::Callback<void(Variable)> expand;            // +0x60 fills in a value that has children (a path through it)
#endif
    };
#if PS2X_GAME_DM2
    static_assert(offsetof(VariableType, set) == 0x28 && offsetof(VariableType, copyOut) == 0x2C);
    static_assert(offsetof(VariableType, setText) == 0x30 && offsetof(VariableType, toText) == 0x34);
    static_assert(offsetof(VariableType, destroy) == 0x38 && offsetof(VariableType, hash) == 0x40);
#else
    static_assert(offsetof(VariableType, hash) == 0x28 && offsetof(VariableType, copyOut) == 0x30);
    static_assert(offsetof(VariableType, intValue) == 0x3C && offsetof(VariableType, setText) == 0x54);
    static_assert(offsetof(VariableType, expand) == 0x60);
#endif
    inline ee::Global<ee::Ptr<VariableType>, sym::g_variableTypes> g_variableTypes;

    // ZGE3 keeps each type's name hash (registerVariableType sets it); ZGE2's
    // types have none, and a search hashes each name as it reaches it.
    constexpr bool kTypesKeepNameHash = !game::readable::kIsDm2;

    // The registered type with that name hash, or 0.
    inline ee::Ptr<VariableType> findVariableType(uint32_t hash)
    {
        ee::Ptr<VariableType> type = *g_variableTypes;
        if constexpr (kTypesKeepNameHash)
        {
            while (type && type->hash != hash)
                type = type->next;
        }
        else
        {
            while (type && zge::nameHash(type->name) != hash)
                type = type->next;
        }
        return type;
    }

    // What resolvePath found: the kind of the node at the path's end (a
    // NodeKind), or these.
    enum PathResult : int32_t
    {
        kPathNotFound = 0,
        kPathArgument = 4, // "@" alone: the script's argument
        kPathLiteral = 9,  // a {...} literal, not a path
    };

    // resolvePath: follows `path` from the running script's widget; the
    // node reached into *node, and for a Variable its name into `name` and
    // its type's name into `typeName` (each when given; `name` gets the
    // component not found when there is none). Returns the node's kind,
    // or a PathResult.
    int32_t resolvePath(ee::Ptr<const char> path, ee::Ptr<char> name, ee::Ptr<char> typeName, ee::Ptr<ee::Ptr<Node>> node);

    // ZGE2 has no node tree: a path walks the widgets' scene objects (a
    // widget's children hang from its object) and a widget's ACTIONs, and its
    // last component names a variable of the one reached. What
    // resolveObjectPath (ZGE2's resolvePath, variables.cpp) found: a widget's
    // scene object or an ACTION, with kObjectPathVariable added when the last
    // component named one of its variables; a {literal}; nothing.
    enum ObjectPathResult : int32_t
    {
        kObjectPathNotFound = 0,
        kObjectPathLiteral = 1,  // a {...} literal, not a path
        kObjectPathWidget = 2,   // *found: a widget's scene object
        kObjectPathAction = 4,   // *found: one of a widget's ACTIONs (a ListedAction)
        kObjectPathVariable = 8, // added: the last component named a variable of it
    };
    constexpr int32_t kWidgetVariablePath = kObjectPathWidget | kObjectPathVariable;
    constexpr int32_t kActionVariablePath = kObjectPathAction | kObjectPathVariable;
    // resolveObjectPath: follows `path` from the running script's widget
    // (g_scriptOwner): a leading "." per level up, "Root" the root, each
    // other component a child widget by name, else an ACTION of the widget
    // reached; the last component is the variable's name, copied to `name`
    // (its type's name to `typeName` when it is found). "*" in front: the
    // variable's text is a path, followed in turn. Both buffers must be given.
    int32_t resolveObjectPath(ee::Ptr<const char> path, ee::Ptr<char> name, ee::Ptr<char> typeName,
                              ee::Ptr<ee::Address> found);
    // ZGE2's variables of a widget (variables.cpp): Position, Orientation and
    // Visible are its scene object's own place and shown flag, every other
    // one a ListedVariable of its record's properties.
    // widgetVariableText (ZGE2's variableText): "name<TAB>value" into out: 1,
    // 0 for an empty value (a position at the origin, the identity
    // orientation, what the ListedVariable's toText says) or no object.
    uint32_t widgetVariableText(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Ptr<char> out);
    // copyWidgetVariable (ZGE2's copyVariable): the value copied out as its
    // type keeps it: 1, 0 without the variable or the object.
    int32_t copyWidgetVariable(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Address out);
    // setWidgetVariable (ZGE2's setVariable): the value set from one its type
    // keeps (a position or orientation through the object's setter, Visible
    // through setSceneObjectShown): 1, 0 without the variable or the object.
    int32_t setWidgetVariable(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name, ee::Address value);
    // setWidgetVariableText (ZGE2 only): the value set from text (a
    // position's or orientation's numbers parsed into the object, Visible's
    // bit written): 1, 0 without the variable.
    int32_t setWidgetVariableText(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> name,
                                  ee::Ptr<const char> text);
    // listedVariableText, setListedVariableText (ZGE2 only, nodes.cpp): the
    // ListedVariable called `name` as text (its type's toText: what it
    // returns, 0 without the variable) and set from text (1, 0 without it).
    uint64_t listedVariableText(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Ptr<char> out);
    int32_t setListedVariableText(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Ptr<const char> text);
    // The ZGE2 forms of the token helpers below (commands.cpp, script.cpp),
    // over resolveObjectPath: no report when a token is no variable.
    void valueTextAtPath(ee::Ptr<const char> token, ee::Ptr<char> out);
    void tokenValueAtPath(ee::Ptr<const char> token, ee::Address out, ee::Ptr<char> typeName);
    void setTokenValueAtPath(ee::Ptr<const char> token, ee::Address value);
    void setVariableTextAtPath(ee::Ptr<const char> path, ee::Ptr<const char> text);

    // The C library's memset, as the game calls it.
    inline void fillBytes(ee::Address bytes, uint32_t value, uint32_t count)
    {
        ee::game(zge::kMemset, bytes, value, count);
    }

    // commandPlayAtPath, commandQueue (ZGE2, commands.cpp): PLAY name and
    // QUEUE name [sender]: the ACTION called by the path's last component, of
    // the widget the path leads to, is played by a PlayAction message to the
    // root: PLAY's at once from the running script's widget, QUEUE's queued,
    // from the widget at the second path when one is given.
    void commandPlayAtPath(ee::Ptr<char> arguments);
    void commandQueue(ee::Ptr<char> arguments);

    // Within this of 0 a vector or quaternion component counts as 0 (the
    // types' toText; ZGE2 keeps the same number for a widget's place).
    constexpr ee::Float kNearZero = ee::Float(1e-05f);
    inline bool nearZero(ee::Float value) { return ee::abs(value) < kNearZero; }
    // What evaluate does with two values of a type (the
    // type's own code, operateVariables): an assignment, arithmetic, or IF's
    // comparisons.
    enum class Operation : int32_t
    {
        Set = 0,
        Add = 1,
        Subtract = 2,
        Multiply = 3,
        Divide = 4,
        // ZGE4's (BMX XXX) ROUND: the source rounded to a whole number (half
        // away from zero) into the destination; ZGE3 and ZGE2 have none (-1:
        // no operation is that). ZGE4 numbers the comparisons after it.
        Round = game::readable::perGame(-1, -1, 5),
        Equal = game::readable::perGame(5, 5, 6),          // "=", "=="
        LessOrEqual = game::readable::perGame(6, 6, 7),    // "<="
        Less = game::readable::perGame(7, 7, 8),           // "<"
        GreaterOrEqual = game::readable::perGame(8, 8, 9), // ">="
        Greater = game::readable::perGame(9, 9, 10),       // ">"
        NotEqual = game::readable::perGame(10, 10, 11),    // "<>", "!="
    };

    // The variable types' name hashes (set at boot).
    inline ee::Global<uint32_t, sym::g_boolType> g_boolType;       // "Bool"
    inline ee::Global<uint32_t, sym::g_floatType> g_floatType;      // "ZFloat"
    inline ee::Global<uint32_t, sym::g_uint8Type> g_uint8Type;      // "Uint8"
    inline ee::Global<uint32_t, sym::g_sint32Type> g_sint32Type;     // "Sint32"
    inline ee::Global<uint32_t, sym::g_colorType> g_colorType;      // "ZRGBA_PAL"
    inline ee::Global<uint32_t, sym::g_vectorType> g_vectorType;     // "Z3DVector"
    inline ee::Global<uint32_t, sym::g_quaternionType> g_quaternionType; // "ZQuaternion"
    inline ee::Global<uint32_t, sym::g_stringType> g_stringType;     // "String"
    inline ee::Global<uint32_t, sym::g_eventType> g_eventType;      // "Event" (a script)
    inline ee::Global<uint32_t, sym::g_assetType> g_assetType;      // "ZAssetID"

    // A ZRGBA_PAL value.
    struct Color
    {
        uint8_t r, g, b, a;
        uint32_t _04;
        // +0x08 a palette colour's name, up to eight characters packed as
        // findPaletteColour takes it; 0: the colour as it is.
        uint64_t paletteName;
    };
    static_assert(sizeof(Color) == 0x10);

    // ZGE2's colour (its type ZRGBA): the four channels alone, no palette name.
    struct Rgba
    {
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(Rgba) == 4);

    // operateVariables: the operation on two variables of the type (its name
    // hash): their values read out, combined, written back to the
    // destination (0), or compared (1 or 0; a String's "!=" gives strcmp's
    // result). Types other than these (ZQuaternion) do nothing (0).
    int32_t operateVariables(ee::Ptr<Node> destination, ee::Ptr<Node> source, uint32_t type, Operation operation);

    // valueText: the token's value as text into out: a variable's text,
    // a {literal}'s inside, else the token itself (reported when `report`).
    void valueText(ee::Ptr<const char> token, ee::Ptr<char> out, int32_t report);
    // tokenValue: the token's value into out as its type keeps it (LINEAR
    // works on these): a variable's, its type's name into typeName when
    // given; a {literal} read as a value of the type typeName names.
    void tokenValue(ee::Ptr<const char> token, ee::Address out, ee::Ptr<char> typeName);

    // A value as tokenValue writes it and setTokenValue reads it, by its
    // type (a Vector's four words at most).
    union ScriptValue
    {
        uint32_t boolean;      // Bool
        int32_t integer;       // Sint32
        uint8_t byte;          // Uint8
        ee::Float real;        // ZFloat
        ee::Float vector[4];   // Vector
        struct
        {
            uint8_t rgba[4];
            uint32_t _04;
            uint64_t paletteName; // 0: the colour as it is
        } colour;              // Color
    };
    static_assert(sizeof(ScriptValue) == 0x10);
    // setTokenValue: the variable at `token` set from a value its type keeps
    // (setVariable); reported when the token is no variable.
    void setTokenValue(ee::Ptr<const char> token, ee::Address value);
    // commandLinear, LINEAR destination from to t: the destination set to
    // from + (to - from) * t, by its type (Bool, ZFloat, Sint32, Uint8,
    // ZRGBA_PAL per channel, Z3DVector); t is a ZFloat, or an integer.
    void commandLinear(ee::Ptr<char> arguments);
    // evaluate: "destination source" with the operation: SET and the
    // arithmetic change the destination; the comparisons give 1 or 0.
    uint64_t evaluate(ee::Ptr<char> text, Operation operation);

    // A doubly linked list of the engine (listPush appends, listRemove
    // removes): nodes start with {next, previous}.
    struct List
    {
        uint32_t first; // +0x00
        uint32_t last;  // +0x04
        uint32_t count; // +0x08
    };

    // An animation node (a zgui node of its own class): what PLAY plays on a
    // widget. It starts after its delay (its OnStart script, then its kind's
    // start), its kind's update runs each frame while it says so (its
    // OnUpdate script before), then it ends (OnEnd, its kind's end). The
    // scripts are Event variables of the node.
    struct Playing;
    struct Animation
    {
        ee::Ptr<Node> nextSibling;  // +0x00
        ee::Ptr<Node> firstChild;   // +0x04
        ee::Ptr<Node> parent;       // +0x08
        uint16_t kindAndFlags;      // +0x0C
        uint16_t message;           // +0x0E
        uint32_t nameHash;          // +0x10
        ee::Ptr<const char> name;   // +0x14
        ee::Ptr<const char> kind;   // +0x18 "Script", or another animation kind
        ee::Float delay;            // +0x1C seconds before it starts; a Script with none runs at once
        uint32_t playingSize;       // +0x20 the size of its Playing record
        ee::Callback<void(ee::Ptr<Playing>)> start; // +0x24 its kind's start (0: none)
        ee::Callback<void(ee::Ptr<Playing>)> end;   // +0x28 its kind's end (0: none)
        // +0x2C its kind's update, (playing, seconds) -> 0 once over (0: none, over at once)
        ee::Callback<int32_t(ee::Ptr<Playing>, ee::Float)> update;
        ee::Ptr<char> onStart;      // +0x30 its Event scripts
        ee::Ptr<char> onUpdate;     // +0x34
        ee::Ptr<char> onEnd;        // +0x38
        ee::Ptr<char> script;       // +0x3C a Script's text
    };
    static_assert(offsetof(Animation, kind) == 0x18 && offsetof(Animation, playingSize) == 0x20);
    static_assert(offsetof(Animation, update) == 0x2C && offsetof(Animation, script) == 0x3C);

    // ZGE2's ACTION node (DM2's initTimedNode sets it up): not a tree node but
    // an entry of its widget's actions (WidgetRecord, widget.h), its
    // properties a list of ListedVariables, and its playing state in itself
    // (ZGE3 keeps that in a Playing record).
    struct ListedAction
    {
        zge::ListNode link;          // +0x00 its widget's actions
        ee::Ptr<const char> kind;    // +0x08 its kind's name (newZguiNode)
        ee::Ptr<zge::SceneObject> parent; // +0x0C its widget's scene object
        zge::List properties;        // +0x10 Name, StartDelay, OnStart, OnUpdate, OnEnd
        ee::Ptr<const char> name;    // +0x1C "Name" (PLAY finds it by it)
        ee::Float time;              // +0x20 seconds since it was played (it starts once delay <= time)
        ee::Float delay;             // +0x24 "StartDelay"
        uint32_t over;               // +0x28 ZGE3's kPlayingOver: set when its update returns 0
        uint32_t started;            // +0x2C ZGE3's kPlayingStarted: past its delay
        ee::Callback<void(ee::Ptr<ListedAction>)> start; // +0x30 its kind's (0: none)
        ee::Callback<void(ee::Ptr<ListedAction>)> end;   // +0x34
        // +0x38 its kind's update, (action, seconds) -> 0 once over (0: none)
        ee::Callback<int32_t(ee::Ptr<ListedAction>, ee::Float)> update;
        ee::Ptr<char> onStart;       // +0x3C its Event scripts
        ee::Ptr<char> onUpdate;      // +0x40
        ee::Ptr<char> onEnd;         // +0x44
        ee::Ptr<char> script;        // +0x48 a Script's text (the Script kind's record goes on to here)
    };
    static_assert(offsetof(ListedAction, kind) == 0x08 && offsetof(ListedAction, parent) == 0x0C &&
                  offsetof(ListedAction, properties) == 0x10 && offsetof(ListedAction, name) == 0x1C);
    static_assert(offsetof(ListedAction, delay) == 0x24 && offsetof(ListedAction, started) == 0x2C &&
                  offsetof(ListedAction, update) == 0x38 && offsetof(ListedAction, script) == 0x48);
    // findActionByPath (ZGE2 only, script.cpp): the ACTION a dotted path leads to from a scene object, or 0.
    ee::Ptr<ListedAction> findActionByPath(ee::Ptr<zge::SceneObject> object, ee::Ptr<const char> path);

    // ZGE2's playing ACTIONs (zgui's globals; the root's PlayAction handler
    // adds an entry, the action player steps them): an entry names its ACTION,
    // whose playing state is in itself.
    struct PlayingAction
    {
        zge::ListNode link;            // +0x00
        ee::Ptr<ListedAction> action;  // +0x08
    };
    static_assert(offsetof(PlayingAction, action) == 0x08);
    inline ee::Global<zge::List, sym::dm2::g_playingActions> g_playingActions;

    // What resolveObjectPath found (*found) as the widget's scene object or
    // the ACTION it is, and an ACTION's variables.
    inline ee::Ptr<zge::SceneObject> foundWidget(ee::Address found)
    {
        return ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(found));
    }
    inline ee::Ptr<ListedAction> foundAction(ee::Address found)
    {
        return ee::Ptr<ListedAction>(static_cast<uint32_t>(found));
    }
    inline ee::Ptr<zge::List> actionProperties(ee::Ptr<ListedAction> action)
    {
        return ee::Ptr<zge::List>(&action->properties);
    }

    // The node kinds (g_nodeKinds's list): an ACTION's name, its bytes and
    // what sets one up (node, name, parent).
    struct NodeKindRow
    {
        ee::Ptr<NodeKindRow> next, previous;
        uint32_t unused;       // +0x08 no function of either game writes or reads it
        char name[0x20];       // +0x0C
        uint32_t bytes;        // +0x2C
#if !PS2X_GAME_DM2
        uint32_t unused2;      // +0x30 no function writes or reads it (ZGE2's row has no word here)
#endif
        ee::Callback<void(ee::Ptr<Animation>, ee::Address, ee::Ptr<Node>)> create; // +0x34, ZGE2 +0x30 (node, name, parent)
    };
    static_assert(offsetof(NodeKindRow, name) == 0xC && offsetof(NodeKindRow, bytes) == 0x2C);
    static_assert(offsetof(NodeKindRow, create) == game::readable::perGame(0x34u, 0x30u) &&
                  sizeof(NodeKindRow) == game::readable::perGame(0x38u, 0x34u));

    // An animation playing on a widget (in g_playing).
    struct Playing
    {
        ee::Ptr<Playing> next;     // +0x00
        ee::Ptr<Playing> previous; // +0x04
        ee::Ptr<Animation> animation; // +0x08
        ee::Float time;            // +0x0C since it was played (to its delay), then its kind's
        uint32_t flags;            // +0x10 PlayingFlags
        ee::Ptr<Node> target;      // +0x14 the widget
    };
    static_assert(offsetof(Playing, flags) == 0x10 && offsetof(Playing, target) == 0x14);
    enum PlayingFlags : uint32_t
    {
        kPlayingOver = 1u << 0,    // ended or stopped: freed at the next update (PLAY starts it again)
        kPlayingStarted = 1u << 1, // past its delay, updated each frame
        kPlayingPooled = 1u << 2,  // from the small-block pool, else the heap
    };
    inline ee::Global<List, sym::g_playing> g_playing;
    inline ee::Global<uint32_t, sym::g_playingPool> g_playingPool;
    inline ee::Global<uint32_t, sym::g_heap> g_heap;        // zgui's heap (zgui globals +0x04)

    // A block of zgui's memory (`memory`: g_heap's word). ZGE3 takes it from
    // zgui's heap; ZGE2's zgui has a DMM region there instead, and each block
    // is allocated in it and locked where it is.
    inline uint32_t zguiAllocate(uint32_t memory, uint32_t bytes)
    {
        if constexpr (game::readable::kIsDm2)
        {
            const ee::Ptr<zge::DmmRegion> region(memory);
            return zge::zmemDMMLock(region, zge::dmmAllocate(region, bytes));
        }
        else
            return zge::heapTake(ee::Ptr<zge::Heap>(memory), bytes);
    }

    // A block of zgui's memory given back (`memory`: g_heap's word, as
    // zguiAllocate takes it): ZGE2 releases its handle in the DMM region.
    inline void zguiRelease(uint32_t memory, ee::Address block)
    {
        if constexpr (game::readable::kIsDm2)
        {
            const ee::Ptr<zge::DmmRegion> region(memory);
            zge::dmmRelease(region, zge::dmmHandleOf(region, block));
        }
        else
            zge::heapGive(ee::Ptr<zge::Heap>(memory), block);
    }

    // zguiRelease as its caller's last step (the original's tail call of the release).
    inline void zguiReleaseLast(uint32_t memory, ee::Address block)
    {
        if constexpr (game::readable::kIsDm2)
        {
            const ee::Ptr<zge::DmmRegion> region(memory);
            game::tail(zge::dmmRelease, region, zge::dmmHandleOf(region, block));
        }
        else
            game::tail(zge::heapGive, ee::Ptr<zge::Heap>(memory), block);
    }

    // variableValue: the value of variable `name` of `parent` (its +0x18),
    // 0 without it.
    uint32_t variableValue(ee::Ptr<Node> parent, ee::Ptr<const char> name);
    // startPlaying: a Playing record past its delay started (OnStart, the
    // kind's start, a first update): 1 while it plays on, 0 if it is over
    // already (or 1 while its delay runs).
    int32_t startPlaying(ee::Ptr<Playing> playing);
    // endPlaying: a Playing record ended (OnEnd, the kind's end).
    void endPlaying(ee::Ptr<Playing> playing);
    // updatePlaying: every animation playing advanced by `seconds`: the
    // over ones ended and freed, the waiting ones started after their delay,
    // the started ones updated.
    void updatePlaying(ee::Float seconds);
    // playWidget: plays the animation on the target (its parent if 0):
    // a script without duration at once, else a Playing record stepped
    // each frame. Always 1.
    int32_t playWidget(ee::Ptr<Animation> animation, ee::Ptr<Node> target);
    // stopPlaying: the animation playing on the target (its parent if 0)
    // stopped (over, freed at the next update). Always 1.
    int32_t stopPlaying(ee::Ptr<Animation> animation, ee::Ptr<Node> target);
    // commandStop, STOP @ [target].
    void commandStop(ee::Ptr<char> arguments);
    // commandIf, IF a op b statement.
    void commandIf(ee::Ptr<char> arguments);
    // commandPlay, PLAY name [target].
    void commandPlay(ee::Ptr<char> arguments);

    // parseInt, parseFloat (parse.cpp): the number at `text` into *value (0
    // first); the text after it, null without a value pointer or text.
    ee::Ptr<const char> parseInt(ee::Ptr<const char> text, ee::Ptr<int32_t> value);
    ee::Ptr<const char> parseFloat(ee::Ptr<const char> text, ee::Ptr<ee::Float> value);

    // addVariableNode: a locked variable `name` (the caller's text) of the
    // type named, over the caller's value, added to `parent`.
    void addVariableNode(ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value);
    // defineProperty: a widget kind's property: the variable node `property`
    // in the widget's own memory, over its value, added to `parent`.
    void defineProperty(ee::Address property, ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value);
    // defineTypedProperty: a property on zgui's heap: 1, 0 for an unknown type.
    int32_t defineTypedProperty(ee::Address parent, ee::Address name, ee::Address typeName, ee::Address value);
    // declareVariable: DECLARE's variable (owns its name and value): 1, 0 for
    // an unknown type.
    int32_t declareVariable(ee::Ptr<Node> parent, ee::Address name, ee::Address typeName, ee::Address value);
    // ZGE2's variables are ListedVariables on a list (widget.h; nodes.cpp):
    // addListedProperty is its defineTypedProperty; setListedVariable and
    // copyListedVariable (ZGE2 only) set and copy out the one with that name
    // through its type: 1, 0 without it.
    int32_t addListedProperty(ee::Ptr<zge::List> properties, ee::Ptr<const char> name, ee::Ptr<const char> typeName,
                              ee::Address value);
    int32_t setListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Address value);
    int32_t copyListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name, ee::Address out);
    // removeListedVariable (ZGE2 only): the one with that name taken off the
    // list and freed, its value too when it owns it: 1, 0 without it.
    int32_t removeListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name);
    // freeListedVariables (ZGE2 only): every variable of the list freed (its
    // type's destroy, its value when it owns it, itself).
    void freeListedVariables(ee::Ptr<zge::List> variables);
    // declareListedVariable: ZGE2's declareVariable: DECLARE's variable added
    // to the list as a property is, then marked as owning its value: 1, 0
    // for an unknown type.
    int32_t declareListedVariable(ee::Ptr<zge::List> variables, ee::Ptr<const char> name,
                                  ee::Ptr<const char> typeName, ee::Address value);
    // initListedAction: ZGE2's initTimedNode (build.cpp).
    uint32_t initListedAction(ee::Address address, ee::Address name, ee::Ptr<zge::SceneObject> parent);
    // setVariableText: the variable at `path` below `parent` set from text: 1, else 0.
    int32_t setVariableText(ee::Address parent, ee::Address path, ee::Ptr<const char> text);
    // variableIntValue: the variable's value as an integer into out (its type's
    // intValue): 1, 0 for no variable or a type without one.
    int32_t variableIntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out);

    // variableText: variable `name` of `parent` as text into out; what its
    // type's toText returns, 0 without one (or without the variable).
    uint64_t variableText(ee::Ptr<Node> parent, ee::Address name, ee::Ptr<char> out);
    // copyVariable: variable `name` of `parent` copied out (its type's
    // copyOut) into out: 1, 0 without it.
    int32_t copyVariable(ee::Address parent, ee::Address name, ee::Address out);

    // setVariable: variable `name` of `parent` set from a value of its type
    // (the type's set, +0x2C) unless it is locked: 1, else 0.
    int32_t setVariable(ee::Address parent, ee::Address name, ee::Address value);
    // setVariableValue: the variable node set from a value of its type at
    // the address `value` (the type's set) unless it is locked: 1, else 0 (0
    // for no variable or no value).
    int32_t setVariableValue(ee::Ptr<Node> variable, uint32_t value);
    // (a frame local's value: its address goes, not the value it holds)
    template <typename T>
    inline int32_t setVariableValue(ee::Ptr<Node> variable, const ee::Local<T> &value)
    {
        return setVariableValue(variable, value.address());
    }
    // expandText: the text with its $KEYS from the UI's string table
    // (zgui's shared text buffer sharedText); "" (noText2) for no text.
    uint32_t expandText(ee::Ptr<const char> text);
    int32_t findPaletteColour(uint64_t name, ee::Ptr<uint8_t> out);              // (variable_types.cpp)

    // findNode: the node at `path` below `root` (the tree's root if 0)
    // whose kind is `kind`, or 0. Leading dots go up from root; a path
    // component no child has is skipped.
    ee::Ptr<Node> findNode(ee::Ptr<Node> root, ee::Address path, uint64_t kind);
    constexpr uint32_t kFindNode = sym::findNode;
}

namespace zge::zgui
{
    // (animation.cpp)
    void initZguiPlayingPool();
    void destroyZguiPlayingPool();
    int32_t stopComponentScript(ee::Address animation, ee::Ptr<Node> target);
    void unlinkPlayingNode(ee::Address animation);
}

namespace zge::zgui
{
    // (script.cpp)
    void initScriptHashes();
    ee::Ptr<Node> currentScriptOwner();
}

namespace zge::zgui
{
    // (commands_ui.cpp)
    uint32_t registerScriptFunction(ee::Address name, uint32_t function);

    // A game function the scripts' CALL reaches: an entry of the host
    // functions' search tree, by its name's hash, called with the argument
    // nodes (a list ended by 0).
    struct HostFunction
    {
        zge::SearchTreeNode links; // +0x00 in the host functions' tree
        uint32_t hash;             // +0x08 its key
#if PS2X_GAME_BMX
        ee::Callback<void(ee::Ptr<ee::Ptr<Node>>)> function; // +0x0C the argument nodes
        // ZGE4's entry ends here (kHostFunctionBytes): it keeps nothing of
        // the name but its hash. ZGE3's copy of the name, for ZGE3's body:
        uint32_t name;
#else
        uint32_t name;             // +0x0C a copy of the name
        ee::Callback<void(ee::Ptr<ee::Ptr<Node>>)> function; // +0x10 the argument nodes
#endif
    };
    constexpr uint32_t kHostFunctionBytes = game::readable::perGame(0x14u, 0x14u, 0x10u);
    static_assert(offsetof(HostFunction, function) == game::readable::perGame(0x10u, 0x10u, 0x0Cu));
}

namespace zge::zgui
{
    // (report.cpp) report: a script error, the statement's line and the message formatted (printf-like); vsprintf's
    // count. Variadic in the original: a call names the words its format reads (ee::VarArg).
    int32_t report(ee::Address format, ee::VarArg first = {}, ee::VarArg second = {}, ee::VarArg third = {},
                   ee::VarArg fourth = {}, ee::VarArg fifth = {}, ee::VarArg sixth = {}, ee::VarArg seventh = {});
}
