// The level scripts' objects: events delivered as engine messages, state
// changes, starting and stopping a level's objects, the natives' table and
// the loaded script's parts (script.h describes them).

#include "script.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"

#include <cstring>
#include "sym/aggressive/script/objects.cpp.h"

namespace aggressive::script
{
    namespace
    {
        // The entry with this hash in a kEndOfList-terminated list, or null.
        ee::Ptr<ScriptTableEntry> findEntry(ee::Ptr<ScriptTableEntry> list, uint32_t hash)
        {
            for (ee::Ptr<ScriptTableEntry> entry = list; entry->hash != kEndOfList; ++entry)
                if (entry->hash == hash)
                    return entry;
            return ee::Ptr<ScriptTableEntry>();
        }

        // Where an entry's offset leads, counted in words from `base`.
        template <typename T, typename B>
        ee::Ptr<T> follow(ee::Ptr<B> base, const ScriptTableEntry &entry)
        {
            return (base.template cast<uint32_t>() + entry.offset).template cast<T>();
        }

        ee::Ptr<ScriptTableEntry> statesOf(ee::Ptr<ScriptObject> object)
        {
            return ee::Ptr<ScriptTableEntry>(object->states);
        }

        // The object's current state (a state hash) in the variables.
        uint32_t &currentState(ee::Ptr<ScriptObject> object)
        {
            return (*g_vm).variables[object->variableIndex];
        }

        namespace
        {
            uint32_t findObjects(ee::Address names, uint32_t count) { return ee::game<uint32_t>(kFindObjects, names, count); }
        }

        // The scene object with this name (the first the engine finds), or 0.
        // `hashes` is an EE address holding the name hash (findObjects takes
        // a list of them).
        uint32_t findSceneObject(ee::Ptr<ScriptTableEntry> hashes)
        {
            const ee::Ptr<uint32_t> found(findObjects(hashes, 1));
            return found ? *found : 0u;
        }

        namespace
        {
            void sendNewMessage(uint32_t value, uint32_t object, uint32_t type, uint32_t data, uint32_t value2) { ee::game(kSendMessage, value, object, type, data, value2); }
        }

        void sendSynchronously(uint32_t target, uint32_t type)
        {
            sendNewMessage(0, target, type, 0, 1);
        }

        uint32_t nameHashOf(uint32_t nameAddress)
        {
            return zge::nameHash(ee::at<const char>(nameAddress));
        }
    }

    // ---- events ------------------------------------------------------------------

    int32_t scriptSendEvent(uint32_t objectHash, uint32_t /*sender*/, uint32_t eventHash)
    {
        // The hash goes to the object search by address: it lives in the
        // original's stack frame.
        ee::Local<uint32_t> names;
        *names = objectHash;
        if (const ee::Ptr<uint32_t> found{findObjects(names, 1)})
            sendNewMessage(0, *found, eventHash, 0, 1);
        return 1;
    }
    READABLE_FUNCTION(scriptSendEvent, Verified);

    namespace
    {
        // An object's words are its current state, then its variables.
        ee::Ptr<ScriptVariable> variablesAfter(ee::Ptr<uint32_t> objectWords)
        {
            return (objectWords + 1).cast<ScriptVariable>();
        }
        // By address: identical bodies fold into one host function on some linkers.
        void pushMessageData(uint32_t data) { scriptPushMessageData(data); }
    }

    int32_t scriptRunEvent(uint32_t objectHash, uint32_t /*senderHash*/, uint32_t eventHash, uint32_t data)
    {
        ScriptVm &vm = *g_vm;
        const ee::Ptr<ScriptTableEntry> objectEntry =
            findEntry(vm.code, objectHash);
        if (!objectEntry)
            return 0;
        const ee::Ptr<ScriptObject> object = follow<ScriptObject>(vm.code, *objectEntry);
        // The handler runs with the object's variables as its frame.
        const ee::Ptr<uint32_t> objectWords = vm.variables + object->variableIndex;
        const uint32_t state = *objectWords;
        vm.frame = variablesAfter(objectWords);

        const ee::Ptr<ScriptTableEntry> stateEntry =
            findEntry(statesOf(object), state);
        if (!stateEntry)
            return 0;
        const ee::Ptr<ScriptTableEntry> events = follow<ScriptTableEntry>(object, *stateEntry);
        const bool reported = eventHash != kUnreportedMessage;
        const ee::Ptr<ScriptTableEntry> eventEntry =
            findEntry(events, eventHash);
        if (eventEntry)
        {
            pushMessageData(data);
            scriptExecute(follow<ScriptInstruction>(events, *eventEntry));
            scriptPopMessageData();
        }
        return reported;
    }
    READABLE_FUNCTION(scriptRunEvent, Verified);

    namespace
    {
        uint32_t objectNameHash(uint32_t object) { return ee::game<uint32_t>(kObjectNameHash, object); }
    }

    int32_t scriptMessageHandler(ee::Ptr<Message> message)
    {
        const uint32_t objectHash = objectNameHash(message->target);
        uint32_t senderHash = kEndOfList;
        if (message->sender)
            senderHash = objectNameHash(message->sender);
        return scriptRunEvent(objectHash, senderHash, message->type, message->data);
    }
    READABLE_FUNCTION(scriptMessageHandler, Verified);

    // ---- states ------------------------------------------------------------------

    namespace
    {
        void unsubscribe(uint32_t object, uint32_t eventHash, uint32_t handler) { ee::game(kUnsubscribe, object, eventHash, handler); }
        void subscribe(uint32_t object, uint32_t messageType, uint32_t handler) { ee::game(kSubscribe, object, messageType, handler); }
    }

    int32_t scriptSetState(uint32_t objectHash, uint32_t stateHash)
    {
        ee::Local<uint32_t> names;
        *names = objectHash;
        const ee::Ptr<uint32_t> found(findObjects(names, 1));
        if (!found)
            return 0;
        const uint32_t sceneObject = *found;

        ScriptVm &vm = *g_vm;
        const ee::Ptr<ScriptTableEntry> objectEntry =
            findEntry(vm.code, objectHash);
        if (!objectEntry)
            return 0;
        const ee::Ptr<ScriptObject> object = follow<ScriptObject>(vm.code, *objectEntry);
        uint32_t &state = currentState(object);
        if (state == stateHash)
        {
            // Nothing to do: no exit, no enter.
            return 1;
        }
        const ee::Ptr<ScriptTableEntry> newState =
            findEntry(statesOf(object), stateHash);
        if (!newState)
            return 0;
        const ee::Ptr<ScriptTableEntry> oldState =
            findEntry(statesOf(object), state);
        // (An unknown old state: the game reads its event list from the
        // list's end marker on, as the code below does.)
        const ee::Ptr<ScriptTableEntry> oldEntry = oldState ? oldState : [&] {
            ee::Ptr<ScriptTableEntry> end = statesOf(object);
            while (end->hash != kEndOfList)
                ++end;
            return end;
        }();

        // Leave the old state: "exit", then its events unsubscribed.
        const ee::Ptr<ScriptTableEntry> oldEvents = follow<ScriptTableEntry>(object, *oldEntry);
        sendSynchronously(sceneObject, nameHashOf(kExitName));
        for (ee::Ptr<ScriptTableEntry> event = oldEvents; event->hash != kEndOfList; ++event)
        {
            unsubscribe(sceneObject, event->hash, kScriptMessageHandler);
        }

        // Enter the new one: its events subscribed, then "enter".
        state = stateHash;
        const ee::Ptr<ScriptTableEntry> newEvents = follow<ScriptTableEntry>(object, *newState);
        for (ee::Ptr<ScriptTableEntry> event = newEvents; event->hash != kEndOfList; ++event)
        {
            subscribe(sceneObject, event->hash, kScriptMessageHandler);
        }
        sendSynchronously(sceneObject, nameHashOf(kEnterName));
        return 1;
    }
    READABLE_FUNCTION(scriptSetState, Verified);

    int32_t scriptStartObjects(int32_t sendEnter)
    {
        ScriptVm &vm = *g_vm;
        for (ee::Ptr<ScriptTableEntry> entry = vm.code; entry->hash != kEndOfList;)
        {
            const ee::Ptr<ScriptObject> object = follow<ScriptObject>(vm.code, *entry);
            const ee::Ptr<ScriptTableEntry> stateEntry = findEntry(
                statesOf(object), currentState(object));
            if (!stateEntry)
                return 0;
            const ee::Ptr<ScriptTableEntry> events = follow<ScriptTableEntry>(object, *stateEntry);
            for (ee::Ptr<ScriptTableEntry> event = events; event->hash != kEndOfList; ++event)
            {
                // The object table entry's hash is the name list the search takes.
                if (const uint32_t sceneObject = findSceneObject(entry))
                    subscribe(sceneObject, event->hash, kScriptMessageHandler);
            }
            ++entry;
        }
        if (sendEnter)
        {
            for (ee::Ptr<ScriptTableEntry> entry = (*g_vm).code; entry->hash != kEndOfList; ++entry)
            {
                if (const uint32_t sceneObject = findSceneObject(entry))
                    sendSynchronously(sceneObject, nameHashOf(kEnterName));
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(scriptStartObjects, Verified);

    int32_t scriptStopObjects()
    {
        ScriptVm &vm = *g_vm;
        for (ee::Ptr<ScriptTableEntry> entry = vm.code; entry->hash != kEndOfList;)
        {
            const uint32_t sceneObject = findSceneObject(entry);
            if (sceneObject)
            {
                const ee::Ptr<ScriptObject> object = follow<ScriptObject>((*g_vm).code, *entry);
                const ee::Ptr<ScriptTableEntry> stateEntry = findEntry(
                    statesOf(object), currentState(object));
                if (!stateEntry)
                    return 0;
                const ee::Ptr<ScriptTableEntry> events = follow<ScriptTableEntry>(object, *stateEntry);
                sendSynchronously(sceneObject, nameHashOf(kExitName));
                for (ee::Ptr<ScriptTableEntry> event = events; event->hash != kEndOfList; ++event)
                {
                    if (const uint32_t subscriber = findSceneObject(entry))
                        unsubscribe(subscriber, event->hash, kScriptMessageHandler);
                }
            }
            ++entry;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(scriptStopObjects, Verified);

    int32_t scriptRestoreVariables(ee::Ptr<uint32_t> saved)
    {
        scriptStopObjects();
        zge::memoryCopy((*g_vm).variables, saved, (*g_vm).variablesBytes);
        return game::tail(aggressive::script::scriptStartObjects, 1);
    }
    READABLE_FUNCTION_WAITS(scriptRestoreVariables, Verified);

    // ---- natives and the loaded script ---------------------------------------------

    void scriptInitNatives()
    {
        ScriptVm &vm = *g_vm;
        const uint32_t stack = zge::heapAllocate(0x10000);
        vm.nativeCount = 0;
        vm.stackTop = ee::Ptr<uint32_t>(stack);
        vm.stackBase = ee::Ptr<uint32_t>(stack);
        vm.natives = ee::Ptr<ScriptNative>(zge::heapAllocate(0x400));
        vm.loadCallback = 0;
    }
    READABLE_FUNCTION(scriptInitNatives, Verified);

    void scriptFreeNatives()
    {
        ScriptVm &vm = *g_vm;
        zge::heapFree(vm.natives);
        vm.nativeCount = 0;
        vm.natives = ee::Ptr<ScriptNative>();
        zge::heapFree(vm.stackBase);
        vm.stackBase = ee::Ptr<uint32_t>();
    }
    READABLE_FUNCTION(scriptFreeNatives, Verified);

    void scriptRegisterNative(ee::Ptr<const char> name, uint32_t function)
    {
        const uint32_t hash = zge::nameHash(name.get());
        ScriptVm &vm = *g_vm;
        vm.natives[vm.nativeCount] = ScriptNative{hash, function};
        ++vm.nativeCount;
    }
    READABLE_FUNCTION(scriptRegisterNative, Verified);

    void scriptSetLoadCallback(uint32_t function)
    {
        (*g_vm).loadCallback = function;
    }
    READABLE_FUNCTION(scriptSetLoadCallback, Verified);

    void scriptUnload()
    {
        scriptStopObjects();
        ScriptVm &vm = *g_vm;
        if (vm.strings)
        {
            zge::heapFree(vm.strings);
            vm.strings = ee::Ptr<char>();
        }
        if (vm.data)
        {
            zge::heapFree(vm.data);
            vm.data = ee::Ptr<uint8_t>();
        }
        if (vm.variables)
        {
            zge::heapFree(vm.variables);
            vm.variables = ee::Ptr<uint32_t>();
        }
        if (vm.code)
        {
            zge::heapFree(vm.code);
            vm.code = ee::Ptr<ScriptTableEntry>();
        }
    }
    READABLE_FUNCTION_WAITS(scriptUnload, Verified);

    ee::Ptr<uint8_t> scriptData(ee::Ptr<uint32_t> bytes)
    {
        *bytes = (*g_vm).dataBytes;
        return (*g_vm).data;
    }
    READABLE_FUNCTION(scriptData, Verified);

    ee::Ptr<uint32_t> scriptVariables(ee::Ptr<uint32_t> bytes)
    {
        *bytes = (*g_vm).variablesBytes;
        return (*g_vm).variables;
    }
    READABLE_FUNCTION(scriptVariables, Verified);

    ee::Ptr<char> scriptString(uint32_t offset)
    {
        return (*g_vm).strings + offset;
    }
    READABLE_FUNCTION(scriptString, Verified);

    ee::Ptr<ee::Ptr<ScriptVariable>> scriptFrameRegister()
    {
        return &g_vm->frame;
    }
    READABLE_FUNCTION(scriptFrameRegister, Verified);

    ee::Ptr<ee::Ptr<ScriptInstruction>> scriptIpRegister()
    {
        return &g_vm->ip;
    }
    READABLE_FUNCTION(scriptIpRegister, Verified);

    namespace
    {
        // scriptStartObjects is hooked by a game override: called by address.
        // The file system by address: the unit tests link this file without it.
        int32_t readScriptFile(ee::Ptr<zge::ZFile> file, ee::Address dst, uint32_t bytes, uint32_t callback) { return ee::game<int32_t>(sym::fileRead, file, dst, bytes, callback); }
        ee::Ptr<zge::ZFile> openScriptFile(uint32_t path, uint32_t mode) { return ee::game<zge::OpenFileResult>(sym::openFile, path, mode, 0).file(); }
        void closeScriptFile(ee::Ptr<zge::ZFile> file) { ee::game(sym::closeFile, file, 0); }
    }

    // loadScriptObjects: a level's compiled script (levels\<L>\<L>.BIN, see
    // script.h) read part by part into heap blocks the machine keeps, the
    // load callback called before the variables are read, and with code
    // there, every object started in its state (sent "enter").
    // Kept from the original: the file is closed only when the script has
    // code, and nothing is checked (the file opened, the header's version,
    // the reads, the blocks).
    void loadScriptObjects(ee::Address path)
    {
        ee::Local<ScriptFileHeader> header;
        ScriptVm &vm = *g_vm;
        const ee::Ptr<zge::ZFile> file = openScriptFile(path, zge::kFileModeRead);
        readScriptFile(file, header, sizeof(ScriptFileHeader), 0);
        if (header->stringBytes != 0)
        {
            vm.strings = ee::Ptr<char>(zge::heapAllocate(header->stringBytes));
            readScriptFile(file, vm.strings, header->stringBytes, 0);
        }
        vm.dataBytes = header->dataBytes;
        if (vm.dataBytes != 0)
        {
            vm.data = ee::Ptr<uint8_t>(zge::heapAllocate(vm.dataBytes));
            readScriptFile(file, vm.data, vm.dataBytes, 0);
        }
        if (vm.loadCallback)
            vm.loadCallback();
        vm.variablesBytes = header->variableWords * 4;
        vm.variables = ee::Ptr<uint32_t>(zge::heapAllocate(vm.variablesBytes));
        readScriptFile(file, vm.variables, vm.variablesBytes, 0);
        if (header->codeWords != 0)
        {
            const uint32_t codeBytes = header->codeWords * 4;
            vm.code = ee::Ptr<ScriptTableEntry>(zge::heapAllocate(codeBytes));
            readScriptFile(file, vm.code, codeBytes, 0);
            scriptStartObjects(1);
            closeScriptFile(file);
        }
    }
    READABLE_FUNCTION_WAITS(loadScriptObjects, Verified);
}
