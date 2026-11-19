#pragma once

// The level scripts: Aggressive Inline's career goals, Tutorial lessons,
// doors, NPC givers and camera fly-throughs are small state machines
// written in Z-Axis's script language and compiled to a bytecode the game
// runs in a stack machine (from nextZfileLibraryAsset on).
//
// ---- the language ---------------------------------------------------------
//
// A level's script is a list of objects ("scriptObj_playerOne", one per
// goal giver, door, NPC ...). Each object has variables and states; each
// state has event handlers ("enter", "exit", "action", "gotolessonA", the
// engine's and the scripts' own messages). An object is in one state at a
// time; a handler runs when its object, in that state, gets the event.
// The compiler's statements (its log on the disc, levels\<L>\<L>.LOG,
// names them): CALL a native function, math, IF / ELSE, SWITCH / CASE /
// DEFAULT, SET (state), SEND (event).
//
// Names are never stored: objects, states, events and natives are
// zge::nameHash of their names (case-insensitive ELF hash).
//
// ---- the file (levels\<L>\<L>.BIN, <L>T.BIN free skate; little endian) ----
//
//     ScriptFileHeader                     0x14 bytes
//     strings      [stringBytes]           NUL-terminated, referred to by offset
//     data         [dataBytes]             a scratch block the scripts' natives use
//     variables    [variableWords] u32     every object's state and variables
//     code         [codeWords] u32         the object table, objects, states, handlers
//
// The loader (loadScriptObjects) reads each part into its own heap block, in
// that order, and keeps them in ScriptVm. Offsets in the code are counted
// in 32-bit words:
//
//     code:    ScriptTableEntry objects[] { name hash, offset of the object from code }, END
//     object:  u32 variableIndex, u32 (unused), then
//              ScriptTableEntry states[] { state hash, offset of the state from the object }, END
//     state:   ScriptTableEntry events[] { event hash, offset of the handler from the state }, END
//     handler: ScriptInstruction[] up to (and with) the End opcode
//
//     variables[variableIndex]       the object's current state (a state hash)
//     variables[variableIndex + 1..] its variables, ScriptVariable { type, value }
//
// The variables part holds their initial values; the save data keeps it
// (scriptVariables), and scriptRestoreVariables puts a saved copy back.
//
// ---- the machine ----------------------------------------------------------
//
// One stack of 32-bit words (ints, floats, hashes, EE addresses; 64 KB),
// the running handler's object's variables (the frame) and an instruction
// pointer, all globals (ScriptVm). An instruction is { opcode, operand }.
// Arithmetic and the conditional jumps work on ints, or on floats when the
// last variable loaded (or a SetType) said float.
//
// Natives (123, registered by name at boot, registerScriptNatives) take their
// arguments off the stack (the scriptPop* functions below) and push their
// results (scriptPush*); a native returns non-zero when it pushed one, which
// the machine drops unless a Store takes it. kNativeNames lists them.
//
// Events reach a script object as engine messages: entering a state
// subscribes the object's handler (scriptMessageHandler) to each event that state
// handles, and sends it "enter"; leaving sends "exit" and unsubscribes.
// A message runs the handler synchronously (scriptRunEvent), which may send
// more messages (nested handlers); SEND and SET keep the machine's position
// on the stack meanwhile.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/scene/message.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/script/script.h.h"

namespace zge { struct SceneObject; }
namespace aggressive::level { struct AnimObjectState; } // (level/animated_objects.h)

namespace aggressive::script
{
    struct ScriptFileHeader
    {
        ee::Float version;      // +0x00 2.7 on the PS2 disc
        uint32_t codeWords;     // +0x04
        uint32_t variableWords; // +0x08
        uint32_t dataBytes;     // +0x0C
        uint32_t stringBytes;   // +0x10
    };
    static_assert(sizeof(ScriptFileHeader) == 0x14);

    // A name and where it leads (objects, states, events). Lists end with
    // an entry whose hash is kEndOfList.
    struct ScriptTableEntry
    {
        uint32_t hash;
        uint32_t offset; // in words, see above
    };
    static_assert(sizeof(ScriptTableEntry) == 8);
    constexpr uint32_t kEndOfList = 0xFFFFFFFFu;

    struct ScriptObject
    {
        uint32_t variableIndex;     // +0x00 its state and variables in ScriptVm::variables
        uint32_t _04;               // +0x04 (not read by the game)
        ScriptTableEntry states[1]; // +0x08 [..], kEndOfList
    };
    static_assert(offsetof(ScriptObject, states) == 8);

    enum class ValueType : uint32_t
    {
        Int = 1,
        Float = 2,
    };

    struct ScriptVariable
    {
        ValueType type; // +0x00
        uint32_t value; // +0x04 an int, a float's bits, a hash, an address
    };
    static_assert(sizeof(ScriptVariable) == 8);

    // The opcodes (scriptExecute's switch). Operands: imm a constant (a
    // float's bits for float arithmetic), var a variable's index in the
    // frame, rel a jump counted in instructions from the next one.
    enum class Opcode : uint32_t
    {
        Push = 0x0A,              // imm: push it
        Store = 0x14,             // var: pop into it
        CallNative = 0x1E,        // native hash: run it
        Send = 0x28,              // object hash: pop sender (unused), pop event; send it the event
        SetState = 0x32,          // object hash: pop a state hash; switch the object to it
        Load = 0x3C,              // var: push its value; its type becomes the value type
        PushAddress = 0x3D,       // var: push the EE address of its value
        AddImmediate = 0x46,      // imm: top += operand
        AddVariable = 0x47,       // var
        SubtractImmediate = 0x50, // imm: top -= operand
        SubtractVariable = 0x51,  // var
        MultiplyImmediate = 0x5A, // imm (not in the PS2 disc's scripts)
        MultiplyVariable = 0x5B,  // var (not in the disc's scripts)
        DivideImmediate = 0x64,   // imm (not in the disc's scripts)
        DivideVariable = 0x65,    // var (not in the disc's scripts)
        JumpIfNotZero = 0x6E,     // rel: pop; jump if != 0
        JumpIfZero = 0x78,        // rel: pop; jump if == 0
        JumpIfNegative = 0x82,    // rel: pop; jump if < 0
        JumpIfPositive = 0x8C,    // rel: pop; jump if > 0 (not in the disc's scripts)
        JumpIfNotPositive = 0x96, // rel: pop; jump if <= 0
        JumpIfNotNegative = 0xA0, // rel: pop; jump if >= 0
        Jump = 0xAA,              // rel
        Pick = 0xB4,              // imm n: push the stack's n-th word from the top (1: the top)
        SetType = 0xBE,           // imm: the value type (ValueType) for what follows
        OrImmediate = 0xC8,       // imm: top |= operand
        OrVariable = 0xC9,        // var
        AndImmediate = 0xD2,      // imm (not in the disc's scripts)
        AndVariable = 0xD3,       // var (not in the disc's scripts)
        ShiftLeftImmediate = 0xDC,  // imm (not in the disc's scripts)
        ShiftLeftVariable = 0xDD,   // var (not in the disc's scripts)
        ShiftRightImmediate = 0xE6, // imm, logical (not in the disc's scripts)
        ShiftRightVariable = 0xE7,  // var (not in the disc's scripts)
        End = 0x1F40,             // the handler's end
    };

    struct ScriptInstruction
    {
        Opcode opcode;
        uint32_t operand;
    };
    static_assert(sizeof(ScriptInstruction) == 8);

    struct ScriptNative
    {
        uint32_t nameHash; // zge::nameHash("playAnim")
        uint32_t function; // the game function (no arguments; returns non-zero if it pushed a result)
    };
    static_assert(sizeof(ScriptNative) == 8);

    // The machine and the loaded script: small globals, in g_vm.
    struct ScriptVm
    {
        ee::Ptr<char> strings;                // +0x00 g_vm
        ee::Ptr<uint32_t> variables;          // +0x04
        ee::Ptr<ScriptTableEntry> code;       // +0x08 (starts with the object table)
        ee::Ptr<uint8_t> data;                // +0x0C
        ee::Ptr<uint32_t> stackBase;          // +0x10 (64 KB)
        union
        {
            ee::Ptr<uint32_t> stackTop;                 // +0x14 the next free word
            ee::Ptr<ee::Ptr<uint32_t>> addressStackTop; // the same, where an address is pushed
        };
        uint32_t nativeCount;                 // +0x18
        uint32_t variablesBytes;              // +0x1C
        uint32_t dataBytes;                   // +0x20
        ee::Ptr<ScriptNative> natives;        // +0x24 (0x400 bytes: 128)
        ee::Ptr<ScriptVariable> frame;        // +0x28 the running handler's object's variables
        ee::Ptr<ScriptInstruction> ip;        // +0x2C the instruction running
        ValueType valueType;                  // +0x30
        ee::FunctionPtr<void()> loadCallback; // +0x34 called by the loader before the variables are read (0: none)
    };
    static_assert(offsetof(ScriptVm, stackTop) == 0x14 && offsetof(ScriptVm, natives) == 0x24);
    static_assert(offsetof(ScriptVm, ip) == 0x2C && sizeof(ScriptVm) == 0x38);

    inline ee::Global<ScriptVm, sym::g_vm> g_vm;

    // The engine's message to an object (zge, sendNewMessage builds one on the
    // stack and hands it to the dispatcher sendEngineMessage).
    using Message = zge::EngineMessage; // (zge/scene/message.h)

    // An engine message no script ever reports handled (scriptRunEvent).
    constexpr uint32_t kUnreportedMessage = 0x7000u;

    // ---- engine functions the scripts use (not ported) ---------------------
    constexpr uint32_t kObjectNameHash = sym::objectNameHash;   // (object) -> its name hash, 0 if none
    constexpr uint32_t kFindObjects = sym::findObjects;      // (hashes*, count) -> 0-terminated object list (foundObjects), or 0
    constexpr uint32_t kSubscribe = sym::subscribe;        // (object, message type, handler)
    constexpr uint32_t kUnsubscribe = sym::unsubscribe;      // (object, message type, handler)
    constexpr uint32_t kSendMessage = sym::sendNewMessage;      // (sender, target, type, data, synchronous)
    constexpr uint32_t kMemcpy = sym::memoryCopy;           // (dst, src, bytes)
    constexpr uint32_t kAllocate = sym::heapAllocate;         // (bytes) -> heap block
    constexpr uint32_t kFree = sym::heapFree;             // (block)
    constexpr uint32_t kEnterName = sym::enterName;        // "enter"
    constexpr uint32_t kExitName = sym::exitName;         // "exit"
    // The script object handler, as the engine's messages call it.
    constexpr uint32_t kScriptMessageHandler = sym::scriptMessageHandler;
    constexpr uint32_t kSetState = sym::scriptSetState;         // hooked by game_overrides/aggressive_inline_restart.cpp

    // ---- the stack, as natives see it (vm.cpp) ------------------------------
    // Several identical copies exist in the game; each keeps its address.
    int32_t scriptPopInt();
    int32_t scriptPopCondition();       // (the conditional jumps' int pop)
    ee::Float scriptPopFloat();
    uint32_t scriptPopMessageData();
    void scriptPushInt(ee::Address value);
    void scriptPushResult(uint32_t value);
    void scriptPushFloat(ee::Float value);
    void scriptPushConstant(uint32_t value); // (opcode Push)
    void scriptPushMessageData(uint32_t value);
    // scriptPopString: pops a string (its offset in the strings) and, under it,
    // a word for *second (if given). Returns the string.
    ee::Ptr<char> scriptPopString(ee::Ptr<uint32_t> second);

    // ---- the opcodes (vm.cpp) ------------------------------------------------
    void scriptLoadVariable(uint32_t index);
    void scriptPushVariableAddress(uint32_t index);
    void scriptPick(uint32_t depth);
    void scriptStoreVariable(uint32_t index);
    void scriptAddImmediate(uint32_t operand);
    void scriptAddVariable(uint32_t index);
    void scriptSubtractImmediate(uint32_t operand);
    void scriptSubtractVariable(uint32_t index);
    void scriptMultiplyImmediate(uint32_t operand);
    void scriptMultiplyVariable(uint32_t index);
    void scriptDivideImmediate(uint32_t operand);
    void scriptDivideVariable(uint32_t index);
    void scriptOrImmediate(uint32_t operand);
    void scriptOrVariable(uint32_t index);
    void scriptAndImmediate(uint32_t operand);
    void scriptAndVariable(uint32_t index);
    void scriptShiftImmediate(uint32_t amount, int32_t left);
    void scriptShiftVariable(uint32_t index, int32_t left);
    void scriptCallNative(uint32_t nameHash);
    // scriptExecute: runs a handler's instructions up to End.
    void scriptExecute(ee::Ptr<ScriptInstruction> code);

    // ---- objects, states and events (objects.cpp) ---------------------------
    // scriptSendEvent: sends an object (by name hash) the event, synchronously.
    // The sender the script pushed is not passed on. Always 1.
    int32_t scriptSendEvent(uint32_t objectHash, uint32_t sender, uint32_t eventHash);
    // scriptRunEvent: runs the handler of the object's current state for the
    // event, with data pushed for it. 0 if the object or its state is not in
    // the script; else whether the event is not kUnreportedMessage.
    int32_t scriptRunEvent(uint32_t objectHash, uint32_t senderHash, uint32_t eventHash, uint32_t data);
    // scriptMessageHandler: the engine message handler of every script object.
    int32_t scriptMessageHandler(ee::Ptr<Message> message);
    // scriptSetState: switches an object (by name hash) to a state: "exit" to
    // the old state, its events unsubscribed, the new state's subscribed,
    // "enter". 1 when switched or in that state already (then nothing is
    // sent); 0 if the object or the new state is unknown.
    int32_t scriptSetState(uint32_t objectHash, uint32_t stateHash);
    // scriptStartObjects: subscribes every object to its current state's events
    // and, if `sendEnter`, sends each "enter". 0 if an object's state is
    // not in the script.
    int32_t scriptStartObjects(int32_t sendEnter);
    // scriptStopObjects: sends every object "exit" and unsubscribes its events.
    int32_t scriptStopObjects();
    // scriptRestoreVariables: the objects stopped, the variables (and states) copied
    // from a saved image, the objects started again with "enter".
    int32_t scriptRestoreVariables(ee::Ptr<uint32_t> saved);

    // ---- natives and the loaded script (objects.cpp) ------------------------
    void scriptInitNatives();                                  // the stack and the native table
    void scriptFreeNatives();
    void scriptRegisterNative(ee::Ptr<const char> name, uint32_t function);
    void scriptSetLoadCallback(uint32_t function);
    void scriptUnload();
    ee::Ptr<uint8_t> scriptData(ee::Ptr<uint32_t> bytes);
    ee::Ptr<uint32_t> scriptVariables(ee::Ptr<uint32_t> bytes);
    ee::Ptr<char> scriptString(uint32_t offset);
    ee::Ptr<ee::Ptr<ScriptVariable>> scriptFrameRegister();   // &vm.frame
    ee::Ptr<ee::Ptr<ScriptInstruction>> scriptIpRegister();   // &vm.ip

    // The natives the game registers (registerScriptNatives, the park editor's
    // registerParkEditorNatives and initInlineFx), for tools and debugging.
    struct NativeName
    {
        uint32_t function;
        const char *name;
    };
    extern const NativeName kNativeNames[];
    extern const size_t kNativeNameCount;
    // The native's name for a hash, or nullptr.
    const char *nativeName(uint32_t nameHash);
}

namespace aggressive::script
{
    // (anim_objects.cpp)
    uint32_t restoreAnimObjectState(ee::Ptr<zge::SceneObject> object, ee::Ptr<level::AnimObjectState> saved);
}

namespace aggressive::script
{
    // (broadcast.cpp)
    void broadcastEvent(uint32_t event, uint32_t kind);
}

namespace aggressive::script
{
    // (objects.cpp)
    void loadScriptObjects(ee::Address path);
}

namespace aggressive::script
{
    // (register_natives.cpp)
    void registerScriptNatives();
}
