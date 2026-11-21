// Game objects and their messages (objects.h).

#include "aggressive/objects.h"
#include "aggressive/skater/skater.h"

#include "ee/abi.h"
#include "ee/callback.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include <bit>
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include "sym/aggressive/objects.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kHashFind = sym::searchTreeFind;  // (table*, key*) -> the entry, or 0 (engine)
        constexpr uint32_t kPoolGive = sym::poolGive;  // (pool, block) back

        inline ee::Global<uint32_t, sym::g_objectPool> g_objectPool;    // GameObject pairs
        constexpr uint32_t kClassRegistry = sym::classRegistry;          // the classes by kind
        inline ee::Global<uint32_t, sym::g_slotNodePool> g_slotNodePool;  // a slot's list nodes

        struct SlotNode
        {
            zge::ListNode link; // +0x00
            ee::Ptr<GameObject> object; // +0x08
        };
        static_assert(sizeof(SlotNode) == 0x0C);
        struct Slot
        {
            zge::List objects; // +0x00 SlotNodes
            uint32_t owner;    // +0x0C
        };
        static_assert(sizeof(Slot) == 0x10);
        // A keyed value: a search tree node holding {key, value}, from its pool.
        struct KeyedValue
        {
            zge::SearchTreeNode links; // +0x00
            uint32_t key;              // +0x08
            uint32_t value;            // +0x0C
        };
        // A class of the registry (classRegistry, by kind): what its objects
        // answer messages with.
        struct ClassEntry
        {
            zge::SearchTreeNode links; // +0x00 in the registry
            uint64_t kind;             // +0x08 the key (GameObject::classEntry points here)
            zge::SearchTree messages;  // +0x10 KeyedValues by message name hash
            uint32_t userData;         // +0x1C a word the class keeps (setClassUserData; cleared when registered)
        };
        static_assert(sizeof(ClassEntry) == 0x20 && offsetof(ClassEntry, messages) == 0x10);
        // An object's class entry, from the kind its classEntry points at.
        ee::Ptr<ClassEntry> classOf(ee::Ptr<GameObject> object)
        {
            return (object->classEntry.cast<uint8_t>() - offsetof(ClassEntry, kind)).cast<ClassEntry>();
        }

        // The node of a slot's list that holds this link.
        ee::Ptr<SlotNode> slotNode(ee::Ptr<zge::ListNode> link) { return link.cast<SlotNode>(); } // the node a link starts
        ee::Ptr<zge::ListNode> linkOf(ee::Ptr<SlotNode> node) { return node.cast<zge::ListNode>(); } // the link is its first field
        // A slot travels as a plain word (Player's objectSlots): the pointer's word.
        uint32_t handleOf(ee::Ptr<Slot> slot)
        {
            return std::bit_cast<uint32_t>(slot);
        }
        using MessageHandler = ee::Callback<uint32_t(ee::Ptr<ObjectMessageNote>)>;
        using SlotVisitor = ee::Callback<int32_t(ee::Ptr<GameObject>, uint32_t)>;
    }

    // newGameObject: a new object of class `kind` with its state block, or 0
    // for a kind no class is registered for.
    ee::Ptr<GameObject> newGameObject(uint64_t kind, ee::Address state)
    {
        ee::Local<uint64_t> key2;
        *key2 = kind; // the registry's key
        const ee::Ptr<ClassEntry> entry = zge::searchTreeEntry<ClassEntry>(zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kClassRegistry), key2));
        if (!entry)
            return {};
        const ee::Ptr<GameObject> object(poolTake(*g_objectPool));
        object->classEntry = &entry->kind;
        object->state = state;
        return object;
    }
    READABLE_FUNCTION(newGameObject, Verified);

    // releaseGameObject: the object back to its pool. Always 1.
    int32_t releaseGameObject(ee::Ptr<GameObject> object)
    {
        poolGive(*g_objectPool, object);
        return 1;
    }
    READABLE_FUNCTION(releaseGameObject, Verified);

    // gameObjectState: the object's state block.
    uint32_t gameObjectState(ee::Ptr<GameObject> object)
    {
        return object->state;
    }
    READABLE_FUNCTION(gameObjectState, Verified);

    // gameObjectKind: the object's kind (its class entry's 64-bit key).
    uint64_t gameObjectKind(ee::Ptr<GameObject> object)
    {
        return *object->classEntry;
    }
    READABLE_FUNCTION(gameObjectKind, Verified);

    // sendObjectMessage: tells the object `message` (a name hash) with its
    // argument; its handler's result into *result when `result` is given. 1
    // when the object's class handles the message, else 0.
    int32_t sendObjectMessage(ee::Ptr<GameObject> object, uint32_t message, ee::Address argument, ee::Address result)
    {
        ee::Local<ObjectMessageNote> note;
        note->argument = argument;
        note->object = object;
        return dispatchObjectMessage(&classOf(object)->messages, message, note, result);
    }
    READABLE_FUNCTION_WAITS(sendObjectMessage, Verified);

    // dispatchObjectMessage: the handler `messages` (a class's message table) has for
    // `message`, called with `note`; 0 when there is none.
    int32_t dispatchObjectMessage(ee::Ptr<zge::SearchTree> messages, uint32_t message, ee::Ptr<ObjectMessageNote> note, ee::Address result)
    {
        ee::Local<uint32_t> key2;
        *key2 = message; // the table's key
        const ee::Ptr<KeyedValue> entry = zge::searchTreeEntry<KeyedValue>(zge::searchTreeFind(messages, key2));
        if (!entry)
            return 0;
        const MessageHandler handler{entry->value};
        const uint32_t answer = handler(note);
        if (result)
            *ee::Ptr<uint32_t>(result) = answer;
        return 1;
    }
    READABLE_FUNCTION_WAITS(dispatchObjectMessage, Verified);

    // newSlotNode: a slot list node holding the object.
    uint32_t newSlotNode(ee::Ptr<GameObject> object)
    {
        const uint32_t node = poolTake(*g_slotNodePool);
        ee::Ptr<SlotNode>(node)->object = object;
        return node;
    }
    READABLE_FUNCTION(newSlotNode, Verified);

    // putObjectInSlot: the object put in the slot (at the front). Always 1.
    int32_t putObjectInSlot(uint32_t slot, ee::Ptr<GameObject> object)
    {
        const uint32_t node = newSlotNode(object);
        zge::listPush(slot, node);
        return 1;
    }
    READABLE_FUNCTION(putObjectInSlot, Verified);

    // objectInSlot: the slot's last object, 0 when it is empty.
    ee::Ptr<GameObject> objectInSlot(uint32_t slot)
    {
        const ee::Ptr<SlotNode> last = slotNode(ee::Ptr<zge::List>(slot)->tail);
        return last ? last->object : ee::Ptr<GameObject>();
    }
    READABLE_FUNCTION(objectInSlot, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_keyedValuePool> g_keyedValuePoolWord; // g_keyedValuePoolWord ((anonymous)::g_keyedValuePoolWord)
    }

    // freeKeyedValue: a keyed value's record given back to the keyed values' pool. Returns 1.
    uint32_t freeKeyedValue(uint32_t record)
    {
        poolGive(*g_keyedValuePoolWord, record);
        return 1;
    }
    READABLE_FUNCTION(freeKeyedValue, Verified);

}

// ---- object slots and keyed values (were the sweep's setKeyedValue ..
// slotNodeEmpty) -------------------------------------------------------------------
//
// A slot is a list of game objects (zge::List of SlotNodes from their pool,
// with its owner after it: a skater's tricks and current action, Player
// +0x898 .. +0x8A8). Emptying a slot tells each object "release" and gives
// it back to its pool.
namespace aggressive
{
    namespace
    {
        // This file's functions, called by address as the original calls them.
        constexpr uint32_t kNewSlot = sym::newSlot;
        constexpr uint32_t kFreeSlot = sym::freeSlot;
        constexpr uint32_t kFreeSlotNode = sym::freeSlotNode;
        constexpr uint32_t kEmptySlot = sym::emptySlot;
        constexpr uint32_t kSlotNodeObject = sym::slotNodeObject;
        constexpr uint32_t kFreeKeyedValue = sym::freeKeyedValue;     // (node, context): a search tree visit's: the node to the pool
        constexpr uint32_t kSearchTreeVisitAll = sym::searchTreeClear; // (tree, visit(node, context), context)

        constexpr uint32_t kSlots = 2, kSlotNodes = 0x40;
        inline ee::Global<uint32_t, sym::g_slotPool> g_slotPool; // Slots

        inline ee::Global<uint32_t, sym::g_keyedValuePool> g_keyedValuePool;

        // The object message that asks an object to let go (nameHash("release")).
    }

    namespace
    {
        int32_t searchTreeInsert(ee::Address tree, ee::Address node) { return ee::game<int32_t>(zge::kSearchTreeInsert, tree, node); }
    }

    // setKeyedValue: `key`'s value in the tree set to `value` (a new node
    // when it has none). Whether it is there now.
    int32_t setKeyedValue(ee::Ptr<zge::SearchTree> tree, uint32_t key, uint32_t value)
    {
        ee::Local<uint32_t> key2;
        *key2 = key;
        const ee::Ptr<KeyedValue> found = zge::searchTreeEntry<KeyedValue>(zge::searchTreeFind(tree, key2));
        if (found)
        {
            found->value = value;
            return 1;
        }
        const ee::Ptr<KeyedValue> node(poolTake(*g_keyedValuePool));
        if (!node)
            return 0;
        node->key = key;
        node->value = value;
        const int32_t inserted = searchTreeInsert(tree, node);
        return inserted;
    }
    READABLE_FUNCTION_WAITS(setKeyedValue, Verified);

    namespace
    {
        namespace tail
        {
        int32_t searchTreeClear(uint32_t tree, uint32_t node, uint32_t context) { return ee::tailCall<int32_t>(kSearchTreeVisitAll, tree, node, context); }
        }
    }

    // clearKeyedValues: every value of the tree given back to the pool (no code
    // calls it).
    int32_t clearKeyedValues(uint32_t tree)
    {
        return tail::searchTreeClear(tree, kFreeKeyedValue, 0);
    }
    READABLE_FUNCTION_WAITS(clearKeyedValues, Verified);

    // removeKeyedValue: `key` out of the tree (removeClassKeyedValue's). Whether it was
    // there.
    int32_t removeKeyedValue(ee::Ptr<zge::SearchTree> tree, uint32_t key)
    {
        ee::Local<uint32_t> keySlot;
        *keySlot = key;
        const ee::Ptr<zge::SearchTreeNode> node = zge::searchTreeRemove(tree, keySlot);
        const uint32_t pool = *g_keyedValuePool;
        if (!node)
            return 0;
        poolGive(pool, node);
        return 1;
    }
    READABLE_FUNCTION_WAITS(removeKeyedValue, Verified);

    // hasKeyedValue: whether `key` is in the tree (no code calls it).
    int32_t hasKeyedValue(uint32_t tree, uint32_t key)
    {
        ee::Local<uint32_t> key2;
        *key2 = key;
        return zge::searchTreeFind(ee::Ptr<zge::SearchTree>(tree), key2) ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(hasKeyedValue, Verified);

    // newSlot: a new empty slot for `owner`, or 0 when the pool is out.
    uint32_t newSlot(ee::Address owner)
    {
        const ee::Ptr<Slot> slot(poolTake(*g_slotPool));
        if (slot)
        {
            zge::initList(slot);
            slot->owner = owner;
        }
        return handleOf(slot);
    }
    READABLE_FUNCTION_WAITS(newSlot, Verified);

    // freeSlot: the slot emptied and given back. Always 1.
    uint32_t freeSlot(uint32_t slot)
    {
        emptySlot(slot);
        poolGive(*g_slotPool, slot);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeSlot, Verified);

    // freeSlotNode: a slot node back to its pool. Always 1.
    uint32_t freeSlotNode(ee::Ptr<SlotNode> node)
    {
        poolGive(*g_slotNodePool, node);
        return 1;
    }
    READABLE_FUNCTION(freeSlotNode, Verified);

    int32_t gameObjectSaveSize(ee::Ptr<GameObject> object);                       // (below)
    void saveGameObject(ee::Ptr<GameObject> object, uint32_t buffer, uint32_t bytes); // (below)
    ee::Ptr<GameObject> restoreGameObject(uint32_t buffer, uint32_t bytes);           // (below)

    // setupSlotPools / freeSlotPools: the slots' and their nodes' pools made
    // (two slots, 64 nodes) and freed.
    void setupSlotPools()
    {
        *g_slotPool = newPool(sizeof(Slot), kSlots, 0);
        *g_slotNodePool = newPool(sizeof(SlotNode), kSlotNodes, 0);
    }
    READABLE_FUNCTION_WAITS(setupSlotPools, Verified);

    uint32_t freeSlotPools()
    {
        freePool(*g_slotPool);
        freePool(*g_slotNodePool);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeSlotPools, Verified);

    // createSlot / destroySlot: newSlot and freeSlot as the rest of the
    // game calls them.
    int32_t createSlot(ee::Address owner)
    {
        return static_cast<int32_t>(newSlot(owner));
    }
    READABLE_FUNCTION_WAITS(createSlot, Verified);

    int32_t destroySlot(uint32_t slot)
    {
        return static_cast<int32_t>(freeSlot(slot));
    }
    READABLE_FUNCTION_WAITS(destroySlot, Verified);

    // emptySlot: every object taken out of the slot, told "release" and
    // given back (and its node); 1, or 0 when a node could not be freed.
    int32_t emptySlot(uint32_t slot)
    {
        ee::Local<uint32_t> result;
        const ee::Ptr<zge::List> objects(slot);
        int32_t freed = 1;
        *result = 0; // the release message's answer (unused)
        for (ee::Ptr<SlotNode> node = slotNode(objects->head); node; node = slotNode(objects->head))
        {
            zge::listRemove(objects, linkOf(node));
            sendObjectMessage(node->object, *g_msgTrickDroppedFromQueue, 0, result);
            releaseGameObject(node->object);
            node->object = {};
            freed = static_cast<int32_t>(freeSlotNode(node));
            if (!freed)
                break;
        }
        return freed;
    }
    READABLE_FUNCTION_WAITS(emptySlot, Verified);

    // takeFromSlot: the first object taken out of the slot (0: empty).
    ee::Ptr<GameObject> takeFromSlot(uint32_t slot)
    {
        const ee::Ptr<zge::List> objects(slot);
        const ee::Ptr<SlotNode> node = slotNode(objects->head);
        if (!node)
            return {};
        zge::listRemove(objects, linkOf(node));
        const ee::Ptr<GameObject> object = node->object;
        freeSlotNode(node);
        return object;
    }
    READABLE_FUNCTION(takeFromSlot, Verified);

    // forEachInSlot / forEachInSlotFromLast: visit(object, context) for each object
    // of the slot from the first (from the last), while it returns non-zero.
    void forEachInSlot(uint32_t slot, uint32_t visit, ee::Address context)
    {
        const SlotVisitor visitor{visit};
        ee::Ptr<SlotNode> node = slotNode(ee::Ptr<zge::List>(slot)->head);
        while (node)
        {
            const int32_t goOn = visitor(node->object, context);
            node = slotNode(node->link.next);
            if (!goOn)
                break;
        }
    }
    READABLE_FUNCTION(forEachInSlot, Verified);

    void forEachInSlotFromLast(uint32_t slot, uint32_t visit, ee::Address context)
    {
        const SlotVisitor visitor{visit};
        ee::Ptr<SlotNode> node = slotNode(ee::Ptr<zge::List>(slot)->tail);
        while (node)
        {
            const int32_t goOn = visitor(node->object, context);
            node = slotNode(node->link.prev);
            if (!goOn)
                break;
        }
    }
    READABLE_FUNCTION(forEachInSlotFromLast, Verified);

    // firstInSlot: the slot's first object (0: empty).
    ee::Ptr<GameObject> firstInSlot(uint32_t slot)
    {
        const ee::Ptr<SlotNode> node = slotNode(ee::Ptr<zge::List>(slot)->head);
        return node ? node->object : ee::Ptr<GameObject>();
    }
    READABLE_FUNCTION(firstInSlot, Verified);

    // slotNodeObject / slotNodeEmpty: a slot node's object; whether it has none.
    ee::Ptr<GameObject> slotNodeObject(uint32_t node)
    {
        return ee::Ptr<SlotNode>(node)->object;
    }
    READABLE_FUNCTION(slotNodeObject, Verified);

    int32_t slotNodeEmpty(uint32_t node)
    {
        return !slotNodeObject(node);
    }
    READABLE_FUNCTION(slotNodeEmpty, Verified);

    // pushSlotObject: the object into the slot: its node goes to the front,
    // then the slot's last node in front of it.
    uint32_t pushSlotObject(ee::Ptr<zge::List> slot, ee::Ptr<GameObject> object)
    {
        const uint32_t node = newSlotNode(object);
        // Pushed before the last object, which stays last.
        const ee::Ptr<zge::ListNode> last = slot->tail;
        if (last)
            zge::listRemove(slot, last);
        zge::listPush(slot, node);
        if (last)
            zge::listPush(slot, last);
        return 1;
    }
    READABLE_FUNCTION_WAITS(pushSlotObject, Verified);

    // slotSaveSize: the bytes saveSlotObjects writes for the slot.
    int32_t slotSaveSize(ee::Ptr<zge::List> slot)
    {
        int32_t bytes = 4;
        for (ee::Ptr<zge::ListNode> node = slot->head; node; node = node->next)
            bytes += gameObjectSaveSize(slotNode(node)->object) + 4;
        return bytes;
    }
    READABLE_FUNCTION_WAITS(slotSaveSize, Verified);

    // saveSlotObjects: the slot's objects into `buffer` (the replay's player
    // copy): the bytes in all (this word too), then each object's size and
    // saved state. `bytes` is the room left.
    uint32_t saveSlotObjects(ee::Ptr<zge::List> slot, uint32_t buffer, uint32_t bytes)
    {
        ee::Local<uint32_t> objectBytes;
        ee::Local<uint32_t> written;
        uint32_t out = buffer + 4;
        *written = 4;
        for (ee::Ptr<zge::ListNode> node = slot->head; node; node = node->next)
        {
            const ee::Ptr<GameObject> object = slotNode(node)->object;
            *objectBytes = static_cast<uint32_t>(gameObjectSaveSize(object));
            *written += 4;
            const uint32_t state = out + 4;
            zge::memoryCopy(out, objectBytes, 4);
            saveGameObject(object, state, bytes - *written);
            out = state + *objectBytes;
            *written += *objectBytes;
        }
        zge::memoryCopy(buffer, written, 4);
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveSlotObjects, Verified);

    // restoreSlotObjects: saveSlotObjects' reverse: the slot emptied and its
    // objects made again from `buffer`.
    uint32_t restoreSlotObjects(uint32_t slot, uint32_t buffer)
    {
        ee::Local<uint32_t> left;
        ee::Local<uint32_t> objectBytes;
        *left = 0;
        if (slotNodeEmpty(slot) == 0)
            emptySlot(slot);
        uint32_t in = buffer + 4;
        zge::memoryCopy(left, buffer, 4);
        *left -= 4;
        if (*left != 0)
        {
            for (;;)
            {
                *objectBytes = 0;
                zge::memoryCopy(objectBytes, in, 4);
                in += 4;
                *left -= 4;
                const ee::Ptr<GameObject> object = restoreGameObject(in, *left);
                putObjectInSlot(slot, object);
                in += *objectBytes;
                *left -= *objectBytes;
                if (*left == 0)
                    break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreSlotObjects, Verified);

    // slotNodeWord0C: a word past a slot node's end (no code calls it).
    uint32_t slotNodeWord0C(uint32_t node)
    {
        return ee::Ptr<KeyedValue>(node)->value;
    }
    READABLE_FUNCTION(slotNodeWord0C, Verified);
}

// ---- object classes (were the sweep's classKindLess .. initMessageTable) ---------------
//
// The class registry (a search tree at classRegistry keyed by the kind as a
// 64-bit word): each class an entry from its pool with its message table,
// a search tree of KeyedValues {message name hash, handler}.
namespace aggressive
{
    namespace
    {
        constexpr uint32_t kSearchTreeInit = sym::initSearchTree;   // (tree, less) empty (engine)
        constexpr uint32_t kWordLess = sym::lessWord;         // (a*, b*) the first words compared (engine)
        constexpr uint32_t kClassKindLess = sym::classKindLess;    // classKindLess below
        constexpr uint32_t kInitMessageTable = sym::initMessageTable; // initMessageTable below
        constexpr uint32_t kSetKeyedValue = sym::setKeyedValue;    // setKeyedValue above
        constexpr uint32_t kClassBlocks = 0x10, kObjectBlocks = 0x80, kKeyedValueBlocks = 0x80;
        constexpr uint32_t kObjectBlockBytes = 0x20;        // a GameObject's pool block

        inline ee::Global<uint32_t, sym::g_classPool> g_classPool;
    }

    // classKindLess: (the registry's order) whether class kind *a sorts
    // before *b (their low words, unsigned).
    int32_t classKindLess(uint32_t a, uint32_t b)
    {
        return static_cast<uint32_t>(*ee::Ptr<uint64_t>(a)) < static_cast<uint32_t>(*ee::Ptr<uint64_t>(b));
    }
    READABLE_FUNCTION(classKindLess, Verified);

    // setupObjectClasses / freeObjectClasses: the registry made empty with its pools
    // (16 classes, 128 objects); the pools freed.
    uint32_t setupObjectClasses()
    {
        zge::initSearchTree(ee::Ptr<zge::SearchTree>(kClassRegistry), kClassKindLess);
        *g_classPool = newPool(sizeof(ClassEntry), kClassBlocks, 0);
        *g_objectPool = newPool(kObjectBlockBytes, kObjectBlocks, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupObjectClasses, Verified);

    uint32_t freeObjectClasses()
    {
        if (*g_classPool)
        {
            freePool(*g_classPool);
            *g_classPool = 0;
        }
        if (*g_objectPool)
        {
            freePool(*g_objectPool);
            *g_objectPool = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeObjectClasses, Verified);

    int32_t initMessageTable(ee::Ptr<zge::SearchTree> table);

    // registerObjectClass: a class registered for `kind` (its message table
    // empty). 0 when there is one already or the pool is out.
    uint32_t registerObjectClass(uint32_t kind)
    {
        ee::Local<uint64_t> key2;
        *key2 = static_cast<uint64_t>(kind); // the key (the kind as an unsigned word)
        if (zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kClassRegistry), key2))
            return 0;
        const ee::Ptr<ClassEntry> entry(poolTake(*g_classPool));
        zge::memoryFill(entry, 0, sizeof(ClassEntry));
        if (!entry)
            return 0;
        entry->kind = static_cast<uint64_t>(kind);
        initMessageTable(&entry->messages);
        const uint32_t inserted = static_cast<uint32_t>(searchTreeInsert(kClassRegistry, entry));
        return inserted;
    }
    READABLE_FUNCTION_WAITS(registerObjectClass, Verified);

    // unregisterObjectClass: the class of `kind` taken out of the registry; whether
    // there was one.
    int32_t unregisterObjectClass(uint64_t kind)
    {
        ee::Local<uint64_t> kindSlot;
        *kindSlot = kind;
        const ee::Ptr<zge::SearchTreeNode> entry = zge::searchTreeRemove(ee::Ptr<zge::SearchTree>(kClassRegistry), kindSlot);
        const uint32_t pool = *g_classPool;
        if (entry)
        {
            poolGive(pool, entry);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(unregisterObjectClass, Verified);

    // setClassHandler: the handler of the class of `kind` for `message` set.
    // 0 when there is no such class.
    uint32_t setClassHandler(uint64_t kind, uint32_t message, uint32_t handler)
    {
        ee::Local<uint64_t> key;
        *key = kind;
        const ee::Ptr<ClassEntry> entry = zge::searchTreeEntry<ClassEntry>(zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kClassRegistry), key));
        if (!entry)
            return 0;
        const uint32_t set = static_cast<uint32_t>(setKeyedValue(&entry->messages, message, handler));
        return set;
    }
    READABLE_FUNCTION_WAITS(setClassHandler, Verified);

    // setGameObjectState: an object's state block replaced (the old one into *previous).
    void setGameObjectState(ee::Ptr<GameObject> object, ee::Address state, ee::Ptr<uint32_t> previous)
    {
        if (previous)
            *previous = object->state;
        object->state = state;
    }
    READABLE_FUNCTION(setGameObjectState, Verified);

    // setupKeyedValues / freeKeyedValues: the KeyedValues' pool (128) made and freed.
    uint32_t setupKeyedValues()
    {
        *g_keyedValuePool = newPool(sizeof(KeyedValue), kKeyedValueBlocks, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupKeyedValues, Verified);

    void freeKeyedValues()
    {
        freePool(*g_keyedValuePool);
    }
    READABLE_FUNCTION_WAITS(freeKeyedValues, Verified);

    namespace
    {
        namespace tail
        {
        int32_t initSearchTree(ee::Address tree, uint32_t compare) { return ee::tailCall<int32_t>(kSearchTreeInit, tree, compare); }
        }
    }

    // initMessageTable: a message table made empty (keyed by its first word).
    int32_t initMessageTable(ee::Ptr<zge::SearchTree> table)
    {
        return tail::initSearchTree(table, kWordLess);
    }
    READABLE_FUNCTION(initMessageTable, Verified);
}

// ---- a trick object queued by hand (was the sweep's beginEndAndQueueObject)
namespace aggressive
{

    // beginEndAndQueueObject: a trick object made outside the tricks (an
    // objective's bonus, the scripts' queueTrick) goes in the skater's line
    // as one already done: it hears "TrickStarted" and "TrickFinished" from
    // the skater, then joins the current object's slot. Returns the push's
    // result (1).
    int32_t beginEndAndQueueObject(ee::Ptr<aggressive::skater::Player> player, ee::Ptr<GameObject> object)
    {
        ee::Local<uint32_t> answer;
        *answer = 0;
        sendObjectMessage(object, *g_msgTrickStarted, player, answer);
        sendObjectMessage(object, *g_msgTrickFinished, player, answer);
        return static_cast<int32_t>(aggressive::pushSlotObject(ee::Ptr<zge::List>(player->currentObjectSlot), object));
    }
    READABLE_FUNCTION_WAITS(beginEndAndQueueObject, Verified);
}

// ---- the class registry's user data, a class's keyed value removed, game
// objects saved and restored (were the sweep's removeClassKeyedValue ..
// restoreGameObject)
namespace aggressive
{
    namespace
    {
        // The registry's entry of a class kind, 0 for none.
        ee::Ptr<ClassEntry> findClass(uint64_t kind)
        {
            ee::Local<uint64_t> key;
            *key = kind;
            return zge::searchTreeEntry<ClassEntry>(zge::searchTreeFind(ee::Ptr<zge::SearchTree>(kClassRegistry), key));
        }
    }

    // removeClassKeyedValue: `key` out of the class's message table; 0 when
    // the class is not registered (else removeKeyedValue's result).
    uint32_t removeClassKeyedValue(uint64_t kind, uint32_t key)
    {
        const ee::Ptr<ClassEntry> entry = findClass(kind);
        if (!entry)
            return 0;
        return static_cast<uint32_t>(removeKeyedValue(&entry->messages, key));
    }
    READABLE_FUNCTION_WAITS(removeClassKeyedValue, Verified);

    // setClassUserData / classUserData / gameObjectClassUserData: the word a
    // class keeps, set (the old one into *old when asked) or read, by kind
    // or for an object.
    uint32_t setClassUserData(uint64_t kind, uint32_t userData, ee::Ptr<uint32_t> old)
    {
        const ee::Ptr<ClassEntry> entry = findClass(kind);
        if (!entry)
            return 0;
        if (old)
            *old = entry->userData;
        entry->userData = userData;
        return 1;
    }
    READABLE_FUNCTION_WAITS(setClassUserData, Verified);

    uint32_t classUserData(uint64_t kind)
    {
        const ee::Ptr<ClassEntry> entry = findClass(kind);
        return entry ? entry->userData : 0;
    }
    READABLE_FUNCTION_WAITS(classUserData, Verified);

    uint32_t gameObjectClassUserData(ee::Ptr<aggressive::GameObject> object)
    {
        return classOf(object)->userData;
    }
    READABLE_FUNCTION(gameObjectClassUserData, Verified);

    namespace
    {
        // What an object saves takes its kind (8 bytes) before its own data.
        constexpr uint32_t kSavedKindBytes = 8;

        // The answer word of GetSpaceNeededForSaving: the bytes, then the
        // message's result word.
        struct SaveSpace
        {
            uint32_t bytes;
            uint32_t answer;
        };
    }

    // gameObjectSaveSize: the bytes the object's save takes: its kind and
    // what it answers "GetSpaceNeededForSaving".
    int32_t gameObjectSaveSize(ee::Ptr<aggressive::GameObject> object)
    {
        ee::Local<SaveSpace> space;
        space->bytes = 0;
        space->answer = 0;
        sendObjectMessage(object, *g_msgGetSpaceNeededForSaving, space, &space->answer);
        space->bytes += kSavedKindBytes;
        return static_cast<int32_t>(space->bytes);
    }
    READABLE_FUNCTION_WAITS(gameObjectSaveSize, Verified);

    // saveGameObject: the object's kind into `buffer`, then its data after
    // it ("SaveToBuffer", with the bytes left).
    void saveGameObject(ee::Ptr<aggressive::GameObject> object, uint32_t buffer, uint32_t bytes)
    {
        ee::Local<SaveRequest> request;
        ee::Local<uint64_t> kind;
        ee::Local<uint32_t> answer;
        *answer = 0;
        *kind = gameObjectKind(object);
        zge::memoryCopy(buffer, kind, kSavedKindBytes);
        zge::memoryFill(request, 0, sizeof(SaveRequest));
        request->bytes = bytes - kSavedKindBytes;
        request->buffer = buffer + kSavedKindBytes;
        sendObjectMessage(object, *g_msgSaveToBuffer, request, answer);
    }
    READABLE_FUNCTION_WAITS(saveGameObject, Verified);

    // restoreGameObject: a new object of the kind the buffer starts with,
    // its data loaded from the rest ("LoadFromBuffer").
    ee::Ptr<GameObject> restoreGameObject(uint32_t buffer, uint32_t bytes)
    {
        ee::Local<SaveRequest> request;
        ee::Local<uint64_t> kind;
        ee::Local<uint32_t> answer;
        *answer = 0;
        zge::memoryCopy(kind, buffer, kSavedKindBytes);
        const ee::Ptr<GameObject> object = newGameObject(*kind, 0);
        zge::memoryFill(request, 0, sizeof(SaveRequest));
        request->bytes = bytes - kSavedKindBytes;
        request->buffer = buffer + kSavedKindBytes;
        sendObjectMessage(object, *g_msgLoadFromBuffer, request, answer);
        return object;
    }
    READABLE_FUNCTION_WAITS(restoreGameObject, Verified);
}
