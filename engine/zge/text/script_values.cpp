// ZGE4's script values (script_values.h; BMX XXX only): a set of values made, added to, set as text, an integer
// or a float, copied and freed, each value found by its key and read as any of the three or a switch, and the
// PROPERTY_BEGIN block that adds a value to a set from a script. The blocks that hold the values and their script
// reader are script_blocks.cpp's.

#include "script_values.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "zge/assets/file.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/script_blocks.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include "sym/zge/text/script_values.cpp.h"

namespace zge
{
    uint32_t beginScriptFromFile(uint32_t file); // (token_reader.cpp)
    uint32_t saveTokenReader();
    uint32_t restoreTokenReader(uint32_t saved);
    uint32_t poolTakeCreating(ee::Ptr<ee::Ptr<Pool>> slot, uint32_t recordBytes); // (pool.cpp)
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record);

    namespace
    {
        constexpr uint32_t kFloatFormat = sym::bmx::g_textValueFloatFormat;     // "%f"
        constexpr uint32_t kIntegerFormat = sym::bmx::g_textValueIntegerFormat; // "%d"
        // scriptValueSwitch's words: three for off, then three for on.
        constexpr uint32_t kSwitchWords = sym::bmx::g_switchWords;
        constexpr uint32_t kSwitchSpellings = 3;
        constexpr uint32_t kSwitchTextBytes = 0x40;
        struct SwitchWords
        {
            ee::Ptr<const char> spelling[2][kSwitchSpellings];
        };

        // The values' and the key names' pools (each made with its first record), and the key names by their hash
        // (one tree for every set: a key's name is kept once, however many values have it).
        inline ee::Ptr<ee::Ptr<Pool>> valuePool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_scriptValuePool); }
        inline ee::Ptr<ee::Ptr<Pool>> keyNamePool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_scriptKeyNamePool); }
        inline ee::Ptr<ee::Ptr<Pool>> valueSetPool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_scriptValueSetPool); }
        inline ee::Global<uint32_t, sym::bmx::g_scriptKeyNamesReady> g_scriptKeyNamesReady; // the tree set up
        inline ee::Ptr<SearchTree> scriptKeyNames() { return ee::Ptr<SearchTree>(sym::bmx::g_scriptKeyNames); }

        // What a text value's copy is read into to be set as an integer or a float, and back.
        constexpr uint32_t kNumberTextBytes = 0x400;
        // copyScriptValue's text of a value of another kind.
        constexpr uint32_t kCopyTextBytes = 0x20;
    }

    // tellScriptValueWatcher: the set's watcher told of the event (ScriptValueEvent) to one of its values, with
    // its owner; nothing when the set has none.
    inline void tellScriptValueWatcher(ee::Ptr<ScriptValue> value, uint32_t event)
    {
        const ee::Ptr<ScriptValueSet> values = value->set;
        if (values->watcher)
            values->watcher(values, value, event, values->watcherOwner);
    }
    READABLE_FUNCTION_ONLY(bmx, tellScriptValueWatcher);

    // takeScriptKeyName: the kept name of that key, with one more user (made, with one, the first time).
    inline ee::Ptr<ScriptKeyName> takeScriptKeyName(ee::Ptr<const char> key)
    {
        if (*g_scriptKeyNamesReady == 0)
        {
            initSearchTree(scriptKeyNames(), sym::lessWord);
            *g_scriptKeyNamesReady = 1;
        }
        ee::Local<uint32_t> hash;
        *hash = nameHash(key);
        ee::Ptr<ScriptKeyName> name = treeRecordOf<ScriptKeyName>(searchTreeFind(scriptKeyNames(), hash));
        if (name)
        {
            ++name->users;
            return name;
        }
        name = ee::Ptr<ScriptKeyName>(poolTakeCreating(keyNamePool(), sizeof(ScriptKeyName)));
        name->nameHash = *hash;
        name->users = 1;
        stringCopy(name->text, key);
        searchTreeInsert(scriptKeyNames(), &name->node);
        return name;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, takeScriptKeyName);

    // releaseScriptKeyName: one user less; the last one's leaving takes the name out of the tree and back to the
    // pool.
    inline void releaseScriptKeyName(ee::Ptr<ScriptKeyName> name)
    {
        if (--name->users != 0)
            return;
        searchTreeRemove(scriptKeyNames(), &name->nameHash);
        game::tail(poolGiveDestroying, keyNamePool(), name);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, releaseScriptKeyName);

    // textToFloat, textToInteger: the text read as one token by the script reader (its state saved around it).
    inline ee::Float textToFloat(ee::Ptr<const char> text)
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<ZFile> file = openMemoryFile(text, static_cast<uint32_t>(stringLength(text)));
        const uint32_t saved = saveTokenReader();
        beginScriptFromFile(ee::Address(file));
        readFloatToken(value);
        endScript();
        restoreTokenReader(saved);
        return *value;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, textToFloat);

    inline int32_t textToInteger(ee::Ptr<const char> text)
    {
        ee::Local<int32_t> value;
        const ee::Ptr<ZFile> file = openMemoryFile(text, static_cast<uint32_t>(stringLength(text)));
        const uint32_t saved = saveTokenReader();
        beginScriptFromFile(ee::Address(file));
        readNumberToken(value);
        endScript();
        restoreTokenReader(saved);
        return *value;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, textToInteger);

    // floatToText, integerToText: "%f" and "%d" into out (formatString's result).
    inline int32_t floatToText(ee::Ptr<char> out, ee::Float number)
    {
        return game::tail(formatString, out, kFloatFormat, sce::libgcc::fptodp(number));
    }
    READABLE_FUNCTION_ONLY(bmx, floatToText);

    inline int32_t integerToText(int32_t integer, ee::Ptr<char> out)
    {
        return game::tail(formatString, out, kIntegerFormat, integer);
    }
    READABLE_FUNCTION_ONLY(bmx, integerToText);

    // setTextCopy: the text's own copy (from the heap) in place of the one the slot holds, if any.
    inline uint32_t setTextCopy(ee::Ptr<ee::Ptr<char>> slot, ee::Ptr<const char> text)
    {
        if (*slot)
            heapFree(*slot);
        *slot = ee::Ptr<char>(heapAllocate(static_cast<uint32_t>(stringLength(text)) + 1));
        return game::tail(stringCopy, *slot, text);
    }
    READABLE_FUNCTION_ONLY(bmx, setTextCopy);

    // newScriptValue: a value of that key and kind (0 or empty) for the set, not yet in it.
    inline ee::Ptr<ScriptValue> newScriptValue(ee::Ptr<const char> key, uint32_t kind, ee::Ptr<ScriptValueSet> values)
    {
        const ee::Ptr<ScriptValue> value(poolTakeCreating(valuePool(), sizeof(ScriptValue)));
        memoryFill(value, 0, sizeof(ScriptValue));
        value->kind = kind;
        value->keyHash = nameHash(key);
        value->key = takeScriptKeyName(key);
        value->set = values;
        return value;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newScriptValue);

    // freeScriptValue: the watcher told it is going, its key's name released, its text freed, back to the pool.
    // (The set's tree still holds it: freeScriptValues takes it out first.)
    inline void freeScriptValue(ee::Ptr<ScriptValue> value)
    {
        tellScriptValueWatcher(value, kScriptValueRemoved);
        releaseScriptKeyName(value->key);
        if (value->kind == kScriptValueText && value->text)
            heapFree(value->text);
        game::tail(poolGiveDestroying, valuePool(), value);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeScriptValue);

    // setScriptValueText, setScriptValueInteger, setScriptValueFloat: the value set from a text, an integer or a
    // float, converted to the value's own kind (a float as an integer rounded half away from zero), and the
    // watcher told it changed.
    inline void setScriptValueText(ee::Ptr<ScriptValue> value, ee::Ptr<const char> text)
    {
        switch (value->kind)
        {
        case kScriptValueInteger:
            value->integer = textToInteger(text);
            break;
        case kScriptValueText:
            setTextCopy(&value->text, text);
            break;
        case kScriptValueFloat:
            value->number = textToFloat(text);
            break;
        default:
            break;
        }
        game::tail(tellScriptValueWatcher, value, kScriptValueChanged);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setScriptValueText);

    inline void setScriptValueInteger(ee::Ptr<ScriptValue> value, int32_t integer)
    {
        ee::Local<char[kNumberTextBytes]> text;
        switch (value->kind)
        {
        case kScriptValueInteger:
            value->integer = integer;
            break;
        case kScriptValueText:
            integerToText(integer, text);
            setTextCopy(&value->text, text);
            break;
        case kScriptValueFloat:
            value->number = ee::toFloat(integer);
            break;
        default:
            break;
        }
        tellScriptValueWatcher(value, kScriptValueChanged);
    }
    READABLE_FUNCTION_ONLY(bmx, setScriptValueInteger);

    inline void setScriptValueFloat(ee::Ptr<ScriptValue> value, ee::Float number)
    {
        ee::Local<char[kNumberTextBytes]> text;
        switch (value->kind)
        {
        case kScriptValueInteger:
            value->integer = ee::toInt(number < ee::Float(0.0f) ? number - ee::Float(0.5f) : number + ee::Float(0.5f));
            break;
        case kScriptValueText:
            floatToText(text, number);
            setTextCopy(&value->text, text);
            break;
        case kScriptValueFloat:
            value->number = number;
            break;
        default:
            break;
        }
        tellScriptValueWatcher(value, kScriptValueChanged);
    }
    READABLE_FUNCTION_ONLY(bmx, setScriptValueFloat);

    inline void scriptValueText(ee::Ptr<ScriptValue> value, ee::Ptr<char> out, uint32_t /*outBytes*/)
    {
        switch (value->kind)
        {
        case kScriptValueInteger:
            game::tail(integerToText, value->integer, out);
            return;
        case kScriptValueText:
            if (!value->text)
            {
                *out = 0;
                return;
            }
            game::tail(stringCopy, out, value->text);
            return;
        case kScriptValueFloat:
            game::tail(floatToText, out, value->number);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_ONLY(bmx, scriptValueText);

    inline int32_t scriptValueInteger(ee::Ptr<ScriptValue> value)
    {
        switch (value->kind)
        {
        case kScriptValueInteger:
            return value->integer;
        case kScriptValueText:
            return textToInteger(value->text);
        case kScriptValueFloat:
        {
            const ee::Float number = value->number;
            return ee::toInt(number < ee::Float(0.0f) ? number - ee::Float(0.5f) : number + ee::Float(0.5f));
        }
        default:
            return 0;
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, scriptValueInteger);

    inline ee::Float scriptValueFloat(ee::Ptr<ScriptValue> value)
    {
        switch (value->kind)
        {
        case kScriptValueInteger:
            return ee::toFloat(value->integer);
        case kScriptValueText:
            return textToFloat(value->text);
        case kScriptValueFloat:
            return value->number;
        default:
            return ee::Float(0.0f);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, scriptValueFloat);

    inline uint32_t scriptValueSwitch(ee::Ptr<ScriptValue> value, uint32_t otherwise)
    {
        ee::Local<char[kSwitchTextBytes]> text;
        ee::Local<SwitchWords> words;
        *words = *ee::Ptr<SwitchWords>(kSwitchWords); // (copied in place: no call)
        scriptValueText(value, text, kSwitchTextBytes);
        for (uint32_t on = 0; on < 2; ++on)
        {
            for (uint32_t spelling = 0; spelling < kSwitchSpellings; ++spelling)
            {
                if (compareStrings(text, words->spelling[on][spelling]) == 0)
                    return on;
            }
        }
        return otherwise;
    }
    READABLE_FUNCTION_ONLY(bmx, scriptValueSwitch);

    // copyScriptValue: the value of `from` into `to`: as it is when the two are of one kind, else through its
    // text (setScriptValueText, which tells the watcher too). The watcher is told `to` changed.
    inline void copyScriptValue(ee::Ptr<ScriptValue> to, ee::Ptr<ScriptValue> from)
    {
        ee::Local<char[kCopyTextBytes]> text;
        if (to->kind != from->kind)
        {
            scriptValueText(from, text, kCopyTextBytes);
            setScriptValueText(to, text);
        }
        else
        {
            switch (to->kind)
            {
            case kScriptValueInteger:
                to->integer = from->integer;
                break;
            case kScriptValueText:
                setTextCopy(&to->text, from->text);
                break;
            case kScriptValueFloat:
                to->number = from->number;
                break;
            default:
                break;
            }
        }
        tellScriptValueWatcher(to, kScriptValueChanged);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyScriptValue);

    inline ee::Ptr<ScriptValue> findScriptValue(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> key)
    {
        ee::Local<uint32_t> hash;
        *hash = nameHash(key);
        return treeRecordOf<ScriptValue>(searchTreeFind(valuesTree(values), hash));
    }
    READABLE_FUNCTION_ONLY(bmx, findScriptValue);

    inline ee::Ptr<char> scriptValueKey(ee::Ptr<ScriptValue> value) { return ee::Ptr<char>(value->key->text); }
    READABLE_FUNCTION_ONLY(bmx, scriptValueKey);
    inline uint32_t scriptValueKind(ee::Ptr<ScriptValue> value) { return value->kind; }
    READABLE_FUNCTION_ONLY(bmx, scriptValueKind);

    inline void watchScriptValues(ee::Ptr<ScriptValueSet> values, ScriptValueWatcher watcher, ee::Address owner)
    {
        values->watcherOwner = owner;
        values->watcher = watcher;
    }
    READABLE_FUNCTION_ONLY(bmx, watchScriptValues);
    inline void unwatchScriptValues(ee::Ptr<ScriptValueSet> values)
    {
        values->watcherOwner = 0;
        values->watcher = 0;
    }
    READABLE_FUNCTION_ONLY(bmx, unwatchScriptValues);

    // freeScriptValues: every value taken out and freed, then the set back to its pool.
    inline void freeScriptValues(ee::Ptr<ScriptValueSet> values)
    {
        while (values->values.count != 0)
        {
            const ee::Ptr<ScriptValue> value = treeRecordOf<ScriptValue>(values->values.root);
            searchTreeRemove(valuesTree(values), &value->keyHash);
            freeScriptValue(value);
        }
        game::tail(poolGiveDestroying, valueSetPool(), values);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeScriptValues);

    // addScriptValue: the set's value of that key; a new one of that kind (the watcher told it was added) when
    // the set has none. A value the set has keeps its own kind.
    inline ee::Ptr<ScriptValue> addScriptValue(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> key, uint32_t kind)
    {
        ee::Local<uint32_t> hash;
        *hash = nameHash(key);
        ee::Ptr<ScriptValue> value = treeRecordOf<ScriptValue>(searchTreeFind(valuesTree(values), hash));
        if (value)
            return value;
        value = newScriptValue(key, kind, values);
        searchTreeInsert(valuesTree(values), &value->node);
        tellScriptValueWatcher(value, kScriptValueAdded);
        return value;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addScriptValue);

    // forEachScriptValue: the set's tree walked (the original's jump to searchTreeForEach).
    inline int32_t forEachScriptValue(ee::Ptr<ScriptValueSet> values, ScriptValueVisit visit, ee::Address context)
    {
        return game::tail(searchTreeForEach, valuesTree(values), SearchTreeVisit(visit), context);
    }
    READABLE_FUNCTION_ONLY(bmx, forEachScriptValue);

    // ---- PROPERTY_BEGIN .. PROPERTY_END: a value added to a set from a script ----
    namespace
    {
        constexpr uint32_t kPropertyKeys = 3; // NAME, TYPE, VALUE
        constexpr uint32_t kPropertyTextBytes = 0x100;
        using PropertyKeyNames = std::array<ee::Ptr<const char>, kPropertyKeys>;
        inline ee::Ptr<const char> text(uint32_t symbol) { return ee::Ptr<const char>(symbol); }
        // libc's memset (the SDK's, run as the recompiled original).
        inline void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game<void>(sym::memset, destination, byte, bytes); }
    }

    // addPropertyFromBlock (the property block's end handler): the block's NAME, TYPE (STRING, INTEGER or FLOAT,
    // any case; anything else is a text) and VALUE, added to the set the class's owner word holds and set from
    // the VALUE text. 1: the reading goes on.
    inline int32_t addPropertyFromBlock(uint32_t beginKeyword)
    {
        const ee::Ptr<const char> begin(beginKeyword);
        const ee::Ptr<ScriptValueSet> blockValues = blockClassValues(begin);
        const ee::Ptr<ScriptValueSet> values(blockClassOwner(begin));
        ee::Local<char[kPropertyTextBytes]> name;
        ee::Local<char[kPropertyTextBytes]> type;
        ee::Local<char[kPropertyTextBytes]> valueText;
        scriptValueText(findScriptValue(blockValues, text(sym::bmx::g_textNameKey)), name, kPropertyTextBytes);
        scriptValueText(findScriptValue(blockValues, text(sym::bmx::g_textTypeKey)), type, kPropertyTextBytes);
        scriptValueText(findScriptValue(blockValues, text(sym::bmx::g_textValueKey)), valueText, kPropertyTextBytes);
        uint32_t kind;
        if (stringCompareNoCase(type, text(sym::bmx::g_textStringKind)) == 0)
            kind = kScriptValueText;
        else if (stringCompareNoCase(type, text(sym::bmx::g_textIntegerKind)) == 0)
            kind = kScriptValueInteger;
        else
            kind = stringCompareNoCase(type, text(sym::bmx::g_textFloatKind)) == 0 ? kScriptValueFloat : kScriptValueText;
        setScriptValueText(addScriptValue(values, name, kind), valueText);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addPropertyFromBlock);

    // registerPropertyBlock: the PROPERTY_BEGIN .. PROPERTY_END class (its three keys all read as text) inside the
    // parent's blocks, its owner word the set its properties go to.
    inline void registerPropertyBlock(ee::Ptr<ScriptValueSet> values, ee::Ptr<const char> parentBegin)
    {
        ee::Local<PropertyKeyNames> keyNames;
        ee::Local<uint32_t[kPropertyKeys]> keyKinds;
        ee::Local<BlockClassSetup> setup;
        *keyNames = *ee::Ptr<PropertyKeyNames>(sym::bmx::g_propertyKeyNames); // (copied in place: no call)
        memset(keyKinds, 0, sizeof(uint32_t) * kPropertyKeys); // every key a text (kScriptValueText)
        setup->begin = text(sym::bmx::g_textPropertyBegin);
        setup->end = text(sym::bmx::g_textPropertyEnd);
        setup->parentBegin = parentBegin;
        setup->keyNames = ee::Ptr<ee::Ptr<const char>>(keyNames->data());
        setup->keyKinds = keyKinds;
        setup->keyCount = kPropertyKeys;
        setup->onBegin = 0;
        setup->onEnd = sym::bmx::addPropertyFromBlock;
        registerBlockClass(setup);
        setBlockClassOwner(setup->begin, values);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, registerPropertyBlock);

    inline void unregisterPropertyBlock()
    {
        game::tail(unregisterBlockClass, text(sym::bmx::g_textPropertyBegin));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, unregisterPropertyBlock);
}
