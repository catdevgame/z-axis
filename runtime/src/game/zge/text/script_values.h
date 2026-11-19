#pragma once

// ZGE4's script values (script_values.cpp; BMX XXX only): what a script block's keys hold once read. A value is a
// text, an integer or a float (the script's STRING, INTEGER and FLOAT), kept by its key's hash in a search tree; a
// reader asks for it as whichever of the three it wants, and it is converted.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    enum ScriptValueKind : uint32_t
    {
        kScriptValueText = 0,
        kScriptValueInteger = 1,
        kScriptValueFloat = 2,
    };

    struct ScriptValueSet;

    // A key's name, kept once for all the values of that key (0x30 bytes, from a pool of its own, in a tree by the
    // name's hash): a value made for the key takes a use of it, and the last value of the key gives it back.
    struct ScriptKeyName
    {
        SearchTreeNode node;        // +0x00
        uint32_t nameHash;          // +0x08 nameHash(text) (what the tree sorts by)
        uint32_t users;             // +0x0C the values of this key
        char text[0x20];            // +0x10
    };
    static_assert(offsetof(ScriptKeyName, users) == 0x0C && offsetof(ScriptKeyName, text) == 0x10 && sizeof(ScriptKeyName) == 0x30);

    // A value, as a search tree's node (0x1C bytes).
    struct ScriptValue
    {
        SearchTreeNode node;        // +0x00
        uint32_t keyHash;           // +0x08 nameHash of its key (what the tree sorts by)
        ee::Ptr<ScriptKeyName> key; // +0x0C its key's name
        uint32_t kind;              // +0x10 ScriptValueKind
        union
        {
            ee::Ptr<char> text;     // +0x14 kScriptValueText: its own copy, 0 for none
            int32_t integer;        //       kScriptValueInteger
            ee::Float number;       //       kScriptValueFloat
        };
        ee::Ptr<ScriptValueSet> set; // +0x18 the set it is in
    };
    static_assert(offsetof(ScriptValue, key) == 0x0C && offsetof(ScriptValue, kind) == 0x10 && offsetof(ScriptValue, text) == 0x14 &&
                  offsetof(ScriptValue, set) == 0x18 && sizeof(ScriptValue) == 0x1C);

    // What a set's watcher is told about one of its values.
    enum ScriptValueEvent : uint32_t
    {
        kScriptValueChanged = 0, // set again (as a text, an integer or a float)
        kScriptValueAdded = 1,   // put in the set
        kScriptValueRemoved = 2, // about to be freed
    };
    // (the set, the value, a ScriptValueEvent, the owner the watcher was given)
    using ScriptValueWatcher = ee::FunctionPtr<void(ee::Ptr<ScriptValueSet> values, ee::Ptr<ScriptValue> value, uint32_t event, uint32_t owner)>;

    // A set of values (0x14 bytes, from a pool of its own): a search tree of ScriptValue by key hash.
    struct ScriptValueSet
    {
        SearchTree values;          // +0x00
        ScriptValueWatcher watcher; // +0x0C told of every change to one of its values, 0: none
        uint32_t watcherOwner;      // +0x10 handed to the watcher
    };
    static_assert(offsetof(ScriptValueSet, watcher) == 0x0C && sizeof(ScriptValueSet) == 0x14);

    // (a value of the set, the context) -> 1 to go on, 0 to stop (searchTreeForEach's visit).
    using ScriptValueVisit = ee::FunctionPtr<int32_t(ee::Ptr<ScriptValue> value, uint32_t context)>;

    // findScriptValue: the value of that key in the set, or 0.
    ee::Ptr<ScriptValue> findScriptValue(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> key);
    // scriptValueText: the value as text into out: a text copied (empty for none), an integer as "%d", a float as "%f".
    // The third word is the callers' buffer size, which nothing reads.
    void scriptValueText(ee::Ptr<ScriptValue> value, ee::Ptr<char> out, uint32_t outBytes);
    // scriptValueInteger: an integer as it is, a text read as a number token, a float rounded half away from zero.
    int32_t scriptValueInteger(ee::Ptr<ScriptValue> value);
    // scriptValueFloat: a float as it is, a text read as a float token, an integer converted.
    ee::Float scriptValueFloat(ee::Ptr<ScriptValue> value);
    // scriptValueSwitch: the value's text as a switch: FALSE, NO or 0 is 0; TRUE, YES or 1 is 1; anything else is
    // otherwise.
    uint32_t scriptValueSwitch(ee::Ptr<ScriptValue> value, uint32_t otherwise);

    // newScriptValues: an empty set of values, with no watcher (script_blocks.cpp).
    ee::Ptr<ScriptValueSet> newScriptValues();
    // scriptValueKey: the name of the value's key; scriptValueKind: its ScriptValueKind.
    ee::Ptr<char> scriptValueKey(ee::Ptr<ScriptValue> value);
    uint32_t scriptValueKind(ee::Ptr<ScriptValue> value);
    // watchScriptValues: the watcher told of each change to one of the set's values, with the owner;
    // unwatchScriptValues: none any more.
    void watchScriptValues(ee::Ptr<ScriptValueSet> values, ScriptValueWatcher watcher, ee::Address owner);
    void unwatchScriptValues(ee::Ptr<ScriptValueSet> values);
    // forEachScriptValue: visit(value, context) for each value of the set, by key hash (searchTreeForEach's
    // result).
    int32_t forEachScriptValue(ee::Ptr<ScriptValueSet> values, ScriptValueVisit visit, ee::Address context);
    // addScriptValue: the set's value of that key, a new one of that kind when it has none (the watcher told).
    ee::Ptr<ScriptValue> addScriptValue(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> key, uint32_t kind);
    // setScriptValueText, setScriptValueInteger, setScriptValueFloat: the value set, converted to its own kind
    // (a float as an integer rounded half away from zero), the watcher told.
    void setScriptValueText(ee::Ptr<ScriptValue> value, ee::Ptr<const char> text);
    void setScriptValueInteger(ee::Ptr<ScriptValue> value, int32_t integer);
    void setScriptValueFloat(ee::Ptr<ScriptValue> value, ee::Float number);
    // copyScriptValue: from's value into to, through its text when the two kinds differ.
    void copyScriptValue(ee::Ptr<ScriptValue> to, ee::Ptr<ScriptValue> from);
    // freeScriptValues: every value of the set freed (the watcher told of each), and the set.
    void freeScriptValues(ee::Ptr<ScriptValueSet> values);
    // registerPropertyBlock: PROPERTY_BEGIN .. PROPERTY_END blocks (NAME, TYPE, VALUE) inside the parent block
    // read into the values; unregisterPropertyBlock: no more.
    void registerPropertyBlock(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> parentBegin);
    void unregisterPropertyBlock();
    // A set as the search tree it is (the values by their key's hash).
    inline ee::Ptr<SearchTree> valuesTree(ee::Ptr<ScriptValueSet> values) { return values.cast<SearchTree>(); }
}
