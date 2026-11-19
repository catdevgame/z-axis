// The level objects a scene object belongs to (block 228, visitLevelObjectOfType ..; BMX
// XXX PS2 word for word): a pool of records at levelObjectsBegin .. levelObjectsEnd whose
// +0x0C is the object's node; the node's tag table's first record is the
// object's component (kObjectComponent: what the game side keeps of it, e.g.
// the animation natives' target) [inferred: the record's layout is the
// game's].

#include "node.h"
#include "scene.h"
#include "locators.h"
#include "level_objects.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "message.h"
#include "zge/math/vec.h"
#include "zge/assets/tag_table.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/math/matrix.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/render/part.h"
#include "zge/scene/scene_database.h"
#include "zge/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/objects.cpp.h"

namespace zge
{
    namespace
    {
        // The level objects' pool: a record per object, then a name's tree
        // node per record, in the same order.
        constexpr uint32_t kLevelObjectsBegin = sym::levelObjectsBegin;
        constexpr uint32_t kLevelObjects = game::readable::perGame(0x200u, 0x100u); // (DM2 has half the pool)
        constexpr uint32_t kFoundObjects = sym::foundObjects; // findObjects' answer

        constexpr uint32_t kLevelObjectsEnd = kLevelObjectsBegin + kLevelObjects * sizeof(LevelObject);

        // In g_levelObjectNames: a tree node with its name's hash as key.
        struct LevelObjectName
        {
            SearchTreeNode links; // +0x00 in the names' tree
            uint32_t nameHash;    // +0x08
            ee::Ptr<LevelObject> levelObject; // +0x0C
        };
        static_assert(sizeof(LevelObjectName) == 0x10);

        struct SpawnRequest;

        // What a level's nodes and locators can make (registerLevelObjectType).
        struct EntityType
        {
            uint32_t nameHash;   // +0x00
            char name[0x40];     // +0x04
            ee::FunctionPtr<uint32_t(ee::Ptr<SpawnRequest> request)> create; // +0x44 the scene object, or 0
            ee::FunctionPtr<int32_t(uint32_t object)> init;                  // +0x48 1 if it stays; 0: none
        };
        static_assert(sizeof(EntityType) == 0x4C && offsetof(EntityType, init) == 0x48);
        constexpr uint32_t kEntityTypeSlots = 0x80;
        const ee::Ptr<EntityType> kEntityTypes(sym::g_levelObjectTypesReady);

        // ZGE2's entity type: a name's hash and the init its entities get, in
        // the same table, counted (ZGE2's registerLevelObjectType). ZGE2 makes
        // every entity the same way (spawnLevelObject), so a type has no create
        // hook, nor a name.
        struct EntityInit
        {
            uint32_t nameHash;                              // +0x00
            ee::FunctionPtr<int32_t(uint32_t object)> init; // +0x04 run on each entity made (its result unread)
        };
        static_assert(sizeof(EntityInit) == 0x08);
        const ee::Ptr<EntityInit> kEntityInits(sym::g_levelObjectTypesReady);
        inline ee::Global<uint32_t, sym::dm2::g_levelObjectTypeCount> g_levelObjectTypeCount;

        enum SpawnFrom : uint32_t
        {
            kSpawnFromNode = 0,
            kSpawnFromRecord = 1, // a locator's
            kSpawnFromObject = 2, // (ZGE4) an object that exists already
        };
        struct SpawnRequest
        {
            uint32_t from;   // +0x00 SpawnFrom
            uint32_t source; // +0x04
        };

        constexpr uint32_t kModelSceneKind = 1;
        // ZGE4's particle emitter objects (object kind 4: forEachObjectOfKind's, the ZPE type's).
        constexpr uint64_t kObjectParticleEmitterKind = 4;

        constexpr uint32_t kCompareNameHashes = sym::lessWord;       // (the tree's less)
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage;       // by address: a game override hooks it
        constexpr uint32_t kSpawnLevelObject = sym::spawnLevelObject;
        constexpr uint32_t kSetNodeLevelObject = sym::setNodeLevelObject;
        constexpr uint32_t kFindNodeBone = sym::findNodeBone;
        constexpr uint32_t kFindEntityType = sym::findEntityType;
        constexpr uint32_t kObjectNameHash = sym::objectNameHash;          // (object) -> 0 if it has no node
        constexpr uint32_t kIdentityMatrix = sym::identityMatrix;

        inline ee::Global<int32_t, sym::g_levelObjectCount> g_levelObjectCount;
        inline ee::Global<SearchTree, sym::g_levelObjectNames> g_levelObjectNames;
    }

    uint32_t sceneObjectOwner(ee::Ptr<SceneObject> object);
    ee::Ptr<LevelObject> levelObjectOf(ee::Ptr<SceneObject> object);
    ee::Ptr<Node> levelObjectNode(ee::Ptr<LevelObject> levelObject);
    uint32_t nodeTags(ee::Ptr<Node> node);
    struct TagRecord;
    uint32_t forEachSceneBoneFromRoot(ee::Ptr<Scene> scene, uint32_t callback, ee::Address context);
    uint32_t forEachSceneBone(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, uint32_t callback, ee::Address context);
    uint32_t removeLevelObject(ee::Ptr<SceneObject> object);

    // sceneObjectOwner: what the scene object belongs to (+0x40).
    uint32_t sceneObjectOwner(ee::Ptr<SceneObject> object) { return object->owner; }
    READABLE_FUNCTION(sceneObjectOwner, Verified);

    // setSceneObjectShown: the object's shown flag set or cleared and its class told
    // (SceneObjectClass::shown), if it has that method. A game override wraps
    // it (the Zoo keeps its NPCs shown). Its result: the method's, or 0.
    namespace
    {
        constexpr uint64_t kObjectShown = kObjectMoved; // the same bit (BMX XXX's one higher, as its kind field is wider)

        // The object's shown flag set or cleared and its class told, where the class has a method for it.
        void showObject(ee::Ptr<SceneObject> object, uint32_t shown)
        {
            if (shown != 0)
                object->flags |= kObjectShown;
            else
                object->flags &= ~kObjectShown;
            if (!hasObjectClass(object))
                return;
            if (const auto method = object->objectClass->shown)
                method(object, shown);
        }
    }

    // ZGE4's visitor of forEachAttached in setSceneObjectShown and in itself: each attached object shown or hidden as
    // the word says, and its own attachments after it. Always 1.
    inline int32_t showAttachedObjects(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> shown)
    {
        showObject(object, *shown);
        forEachAttached(object, sym::bmx::showAttachedObjects, shown);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, showAttachedObjects);

    uint32_t setSceneObjectShown(ee::Ptr<SceneObject> object, uint32_t shown)
    {
        // ZGE4 tests the class first, drops the method's result and shows or hides the attached objects too
        // (showAttachedObjects); ZGE3 answers the class's method.
        if constexpr (game::readable::kIsBmx)
        {
            ee::Local<uint32_t> shownWord;
            *shownWord = shown;
            showObject(object, shown);
            return forEachAttached(object, sym::bmx::showAttachedObjects, shownWord);
        }
        if (shown != 0)
            object->flags |= kObjectShown;
        else
            object->flags &= ~kObjectShown;
        const auto method = object->objectClass->shown;
        return method ? method(object, shown) : 0;
    }
    READABLE_FUNCTION(setSceneObjectShown, Verified);

    // levelObjectOf: the level object the scene object belongs to: its owner
    // when that is one of the pool's records, else 0.
    ee::Ptr<LevelObject> levelObjectOf(ee::Ptr<SceneObject> object)
    {
        const uint32_t owner = sceneObjectOwner(object);
        uint32_t found = 0;
        if (owner)
        {
            if (owner < kLevelObjectsBegin)
                return {};
            found = owner < kLevelObjectsEnd ? owner : 0u;
        }
        return ee::Ptr<LevelObject>(found); // the owner word, one of the pool's records
    }
    READABLE_FUNCTION(levelObjectOf, Verified);

    namespace
    {
        // forEachObjectNamed's search: the callback for each object whose
        // level object's type has the name's hash.
        struct ObjectTypeSearch
        {
            uint32_t typeNameHash;                                                            // +0x00
            ee::FunctionPtr<int32_t(ee::Ptr<SceneObject> object, uint32_t userData)> callback; // +0x04
            uint32_t userData;                                                                // +0x08
        };
        static_assert(sizeof(ObjectTypeSearch) == 0x0C);
    }

    // visitLevelObjectOfType: forEachObjectNamed's visit: an object whose
    // level object's type is the one searched for goes to the search's
    // callback, whose verdict it returns; any other object gets 1 (go on).
    int32_t visitLevelObjectOfType(ee::Ptr<SceneObject> object, ee::Ptr<ObjectTypeSearch> search)
    {
        const ee::Ptr<LevelObject> record(levelObjectOf(object));
        const ee::Ptr<EntityType> type(record->type);
        if (type->nameHash != search->typeNameHash)
            return 1;
        return search->callback(object, search->userData);
    }
    READABLE_FUNCTION(visitLevelObjectOfType, Draft);

    // levelObjectNode: a level object's node (read through 0 too, as the
    // original's callers pass it).
    ee::Ptr<Node> levelObjectNode(ee::Ptr<LevelObject> levelObject)
    {
        return levelObject->node;
    }
    READABLE_FUNCTION(levelObjectNode, Verified);

    uint32_t sceneObjectSceneKind(ee::Ptr<SceneObject> object);

    namespace
    {
        // nodeSceneObject's two lookups: ZGE4 (BMX XXX) has them inline, the scene's kind with a test for no scene
        // (0 then); ZGE3 calls sceneObjectSceneKind and levelObjectNode. ZGE2 (DM2) reads the level object's node
        // inline too (objectLevelNode, objectComponent, levelObjectClassTag).
        uint32_t objectSceneKindOrNone(ee::Ptr<SceneObject> object)
        {
            if constexpr (game::readable::kIsBmx)
            {
                const ee::Ptr<Scene> scene = sceneOf(object);
                return scene ? static_cast<uint8_t>(scene->flags) : 0u;
            }
            else
            {
                return sceneObjectSceneKind(object);
            }
        }
        // A node's class's tag table: ZGE4 (BMX XXX) reads the node's model and the model's tags inline, ZGE2 and
        // ZGE3 call objectClass and classTags.
        ee::Ptr<TagTableHeader> nodeClassTags(ee::Ptr<Node> node)
        {
            if constexpr (game::readable::kIsBmx)
                return node->model->tags;
            else
                return classTags(objectClass(node));
        }
        ee::Ptr<Node> nodeOfLevelObject(ee::Ptr<LevelObject> record)
        {
            if constexpr (game::readable::kIsBmx || game::readable::kIsDm2)
                return record->node;
            else
                return levelObjectNode(record);
        }
    }

    // nodeTags: a node's tag table.
    uint32_t nodeTags(ee::Ptr<Node> node) { return node->tags; }
    READABLE_FUNCTION(nodeTags, Verified);

    // objectComponent (kObjectComponent): the first record of the scene
    // object's level object's node's tag table, or 0.
    ee::Ptr<TagRecord> objectComponent(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Node> node = nodeOfLevelObject(levelObjectOf(object));
        if (!game::readable::kIsDm2 && !node) // (ZGE2 asks a missing node's tags through 0)
            return {};
        const uint32_t tags = nodeTags(node);
        if (!tags)
            return {};
        return tagRecordAt(ee::Ptr<TagTableHeader>(tags), 0u);
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION(objectComponent, Verified)); // BMX XXX's: game/bmx/scene/object_components.cpp

    // ---- an application instance's node ---------------------------------------

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_geometryNodePool> g_geometryNodePool;
        inline ee::Global<uint32_t, sym::g_appInstanceCount> g_appInstanceCount; // the instances made: each one's name
        constexpr uint32_t kInstanceNameFormat = sym::instanceNameFormat; // "%s%x"
        constexpr uint32_t kInstanceNamePrefix = sym::instanceNamePrefix; // "AppInst:"
        constexpr uint32_t kNodeNameBytes = 0x1F; // stringCopyN's count: the name and its 0

        // The flags word as a doubleword: shown, collides, lights changed,
        // its own lights off, 0x100 set, and the 16-bit field at bits 45-60
        // (an instance count [inferred]) set to 1. ZGE4's field is the 13 bits from bit 32 (the second word's low
        // bits, which instanceGeometryNode copies too).
        constexpr uint64_t kInstanceFlagsSet = kNodeShown | kNodeCollides | kNodeLightsChanged;
        constexpr uint64_t kInstanceFlagsField = game::readable::perGame(0xE0001FFFFFFFFFFFull, 0xE0001FFFFFFFFFFFull, 0xFFFFE000FFFFFFFFull);
        constexpr uint64_t kInstanceFlagsOne = game::readable::perGame(1ull << 45, 1ull << 45, 1ull << 32);
        // drawFlags' doubleword: the byte at bits 13-20 set to 1 [unknown], then
        // kNodeCastsShadow.
        constexpr uint64_t kDrawFlagsField = 0xFFFFFFFFFFE01FFFull;
        constexpr uint64_t kDrawFlagsOne = 0x2000ull;
        constexpr uint16_t kNoPositionKey = 0x8001u; // both halves: a key no position gives
        // ZGE2's node level of detail (drawFlags bits 8-11, setNodeLevelOfDetail): a new instance starts at 15,
        // as ZGE3's views do (their lodAndFlags bits 1-4).
        constexpr uint32_t kNodeLevelOfDetailBits = 0xFu << 8;
    }

    // ---- ZGE4: an instance name no geometry node has yet -------------------------

    // forEachGeometryNode (ZGE4 only): visit(node, data) for each node of the
    // geometry nodes' pool until a visit answers 0 (poolForEach's result).
    inline uint32_t forEachGeometryNode(uint32_t visit, ee::Address data)
    {
        return poolForEach(*g_geometryNodePool, visit, data);
    }
    READABLE_FUNCTION_ONLY(bmx, forEachGeometryNode);

    // nodeNameDiffers (ZGE4 only): forEachGeometryNode's visitor: whether the
    // node's name hash is another (the one asked for stops the walk).
    inline uint32_t nodeNameDiffers(ee::Ptr<Node> node, uint32_t hash)
    {
        return node->nameHash != hash;
    }
    READABLE_FUNCTION_ONLY(bmx, nodeNameDiffers);

    // geometryNodeNamed (ZGE4 only): whether some geometry node has this name hash.
    inline uint32_t geometryNodeNamed(uint32_t hash)
    {
        return forEachGeometryNode(sym::bmx::nodeNameDiffers, hash) == 0;
    }
    READABLE_FUNCTION_ONLY(bmx, geometryNodeNamed);

    // newAppInstanceNode: a node for an instance of geometry, from the geometry
    // nodes' pool: named "AppInst:<count in hex>" (and its hash), cleared,
    // its matrices reset, the geometry set, shown and colliding, level and
    // material set 0, no position key yet, both views reset, grey (0x20) and
    // casting a shadow; with addToWorld, among the collision world's items.
    // 0 when the pool is empty.
    uint32_t newAppInstanceNode(ee::Address geometry, uint32_t addToWorld)
    {
        ee::Local<char[0x70]> name;
        const uint32_t count = *g_appInstanceCount;
        *g_appInstanceCount = count + 1;
        formatString(name, kInstanceNameFormat, kInstanceNamePrefix, count);
        uint32_t hash = nameHash(name);
        // ZGE4 takes the next count until the name is one no geometry node has.
        if constexpr (game::readable::kIsBmx)
            while (geometryNodeNamed(hash))
            {
                const uint32_t next = *g_appInstanceCount;
                *g_appInstanceCount = next + 1;
                formatString(name, kInstanceNameFormat, kInstanceNamePrefix, next);
                hash = nameHash(name);
            }
        const uint32_t nodeAddress = poolAlloc(*g_geometryNodePool);
        if (nodeAddress == 0)
            return 0;
        memoryFill(nodeAddress, 0, kNodeBytes);
        const ee::Ptr<Node> node(nodeAddress);
        uint64_t &drawFlags = node->drawFlagsDoubleword();
        // ZGE2 counts a node's references in the flags' low halfword, ZGE3 in drawFlags.
        if constexpr (game::readable::kIsDm2)
            node->flags = (node->flags & ~0xFFFFu) | 1u; // one reference (the halfword alone)
        else
            drawFlags = (drawFlags & kDrawFlagsField) | kDrawFlagsOne;
        resetMatrices(&node->local);
        setNodeGeometry(node, geometry);
        // ZGE2: no origin; shown and colliding in the state word, its own lights off (no light change and no object
        // groups).
        if constexpr (!game::readable::kIsDm2)
            node->origin = kZero;
        uint64_t &flags = node->flagsDoubleword();
        if constexpr (game::readable::kIsDm2)
            flags = (flags | static_cast<uint64_t>(kNodeShown | kNodeCollides) << 32) & ~kNodeOwnLightsBit;
        else
            flags = (((flags | kInstanceFlagsSet) & ~static_cast<uint64_t>(kNodeOwnLights) | 0x100ull) & kInstanceFlagsField) |
                    kInstanceFlagsOne;
        setLodTable(node, 0);
        setNodeMaterialSet(node, 0);
        // ZGE2 keeps no position key and one level of detail for the node (drawFlags bits 8-11), where ZGE3
        // keeps one per view.
        if constexpr (!game::readable::kIsDm2)
        {
            node->positionKey = kNoPositionKey << 16 | kNoPositionKey;
            for (uint32_t view = 0; view < 2; ++view)
            {
                NodeView &state = node->views[view];
                state.part = ee::Ptr<ZgePart>(0u);
                setViewFrameStamp(state, 0);
                state.lodAndFlags = (state.lodAndFlags | kViewLevelOfDetail) & ~kViewDrawn; // the level 15, not drawn
            }
        }
        const uint64_t shadow = drawFlags | kNodeCastsShadow | (game::readable::kIsDm2 ? kNodeLevelOfDetailBits : 0u);
        node->colour[3] = 0x80;
        node->nameHash = hash;
        drawFlags = shadow;
        node->colour[2] = 0x20;
        node->colour[0] = 0x20;
        node->colour[1] = 0x20;
        stringCopyN(node->name, name, kNodeNameBytes);
        if (addToWorld != 0)
            addWorldNode(node);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no node render state)
            newNodeRenderState();
        return nodeAddress;
    }
    READABLE_FUNCTION_WAITS(newAppInstanceNode, Verified);

    // nodeEntityType: the name hash of the entity type a node's class says it
    // makes (+8 of its first tag), 0 if none.
    uint32_t nodeEntityType(ee::Ptr<Node> node)
    {
        const ee::Ptr<TagTableHeader> tags = nodeClassTags(node);
        if (tags)
        {
            const ee::Ptr<TagRecord> tag = tagRecordAt(tags, 0);
            if (tag)
            {
                const uint32_t type = tagWord(tag, 8, 0);
                return type;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(nodeEntityType, Verified);

    // sceneObjectSceneKind: the kind of the object's scene: the low byte of its
    // flags (kModelSceneKind: a model).
    uint32_t sceneObjectSceneKind(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Scene> scene = sceneOf(object);
        return static_cast<uint8_t>(scene->flags);
    }

    READABLE_FUNCTION(sceneObjectSceneKind, Verified);

    // addLevelObject: a free record of the pool for the object: its nodes get
    // to know it, it goes on the object list and in the tree by name. No free
    // record: nothing.
    void addLevelObject(ee::Ptr<SceneObject> object, ee::Address type)
    {
        const ee::Ptr<LevelObject> records(kLevelObjectsBegin);
        const ee::Ptr<LevelObjectName> names(kLevelObjectsEnd);
        for (uint32_t i = 0; i < kLevelObjects; ++i)
        {
            const ee::Ptr<LevelObject> record = records + i;
            if (record->object)
                continue;
            record->type = type;
            record->object = object;
            ++*g_levelObjectCount;
            if (objectSceneKindOrNone(object) == kModelSceneKind)
            {
                const ee::Ptr<Scene> scene = sceneOf(object);
                record->node = sceneRootNode(scene);
                forEachSceneBoneFromRoot(scene, kSetNodeLevelObject, record);
            }
            else
                record->node = ee::Ptr<Node>();
            setObjectOwner(object, record);
            listPush(g_objectList.get(), record);
            const uint32_t name = objectNameHash(object);
            const ee::Ptr<LevelObjectName> entry = names + i;
            entry->nameHash = name;
            entry->levelObject = record;
            searchTreeInsert(g_levelObjectNames.get(), &entry->links);
            return;
        }
    }

    // registerLevelObject: ZGE2's addLevelObject: a free record of the pool for
    // the object, on the object list and in the tree under the name of its
    // scene's root bone's node. What changed in ZGE3: the record keeps the
    // entity type and the root node, a model's nodes get to know the record
    // (setNodeLevelObject) and the object its owner (setObjectOwner), and the
    // name is objectNameHash's. Returns the record (ZGE3's callers read none),
    // 0 when no record is free.
    ee::Ptr<LevelObject> registerLevelObject(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<LevelObject> records(kLevelObjectsBegin);
        const ee::Ptr<LevelObjectName> names(kLevelObjectsEnd);
        for (uint32_t i = 0; i < kLevelObjects; ++i)
        {
            const ee::Ptr<LevelObject> record = records + i;
            if (record->object)
                continue;
            const ee::Ptr<Scene> scene = sceneOf(object);
            const ee::Ptr<Node> node = boneNode(scene, rootBone(scene));
            record->object = object;
            ++*g_levelObjectCount;
            listPush(g_objectList.get(), record);
            const uint32_t name = zge::nodeNameHash(node);
            const ee::Ptr<LevelObjectName> entry = names + i;
            entry->nameHash = name;
            entry->levelObject = record;
            searchTreeInsert(g_levelObjectNames.get(), &entry->links);
            return record;
        }
        return ee::Ptr<LevelObject>();
    }
    READABLE_FUNCTION_WAITS_OWN(addLevelObject, Verified, dm2, registerLevelObject);

    ee::Ptr<EntityType> findEntityType(uint32_t nameHash);

    // spawnLevelObject: a node's entity made: the type's create hook gets the
    // request; then the object is added and its init hook run. An init that
    // doesn't return 1 removes it again. Returns 1 whatever became of it.
    uint32_t spawnLevelObject(ee::Ptr<Node> node)
    {
        ee::Local<SpawnRequest> spawnRequest;
        const ee::Ptr<EntityType> type = findEntityType(nodeEntityType(node));
        if (!type)
            return 1;
        *spawnRequest = {kSpawnFromNode, ee::Address(node)}; // the request's source word
        const uint32_t object = type->create(spawnRequest);
        if (object == 0)
            return 1;
        addLevelObject(ee::Ptr<SceneObject>(object), type);
        if (type->init && type->init(object) != 1)
            removeLevelObject(ee::Ptr<SceneObject>(object));
        return 1;
    }

    ee::Ptr<EntityInit> findEntityInit(uint32_t nameHash);

    // spawnPlainEntity: ZGE2's spawnLevelObject, with ZGE3's
    // createEntityObject written in: a node of a known entity type made a
    // level object of its model, placed where the node was (the node itself
    // at the identity), the record and the node pointed at each other, and
    // the type's init run (its result not looked at). No mirrored node is
    // passed over; a full pool still has the record's fields written (at
    // address 0). Returns 1 whatever became of it.
    uint32_t spawnPlainEntity(ee::Ptr<Node> node)
    {
        ee::Local<Matrix> placing;
        const ee::Ptr<EntityInit> type = findEntityInit(nodeEntityType(node));
        if (!type)
            return 1;
        copyRotationAndTranslation(placing, nodeMatrix(node, 1));
        setNodeLocalMatrix(node, ee::Ptr<const Matrix>(kIdentityMatrix));
        const ee::Ptr<SceneObject> object = newModelObject(setupNodeSceneModel(node), 0);
        const ee::Ptr<LevelObject> record = registerLevelObject(object);
        setObjectOwner(object, record);
        node->levelObject = ee::Address(record);
        record->node = node;
        record->type = ee::Address(type);
        setObjectRotationMatrix(object, placing);
        setObjectPosition(object, &placing->translation());
        type->init(ee::Address(object));
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(spawnLevelObject, Verified, dm2, spawnPlainEntity);

    // spawnLevelObjectByTag: the same from a locator's scene point, whose kind
    // is its entity type.
    void spawnLevelObjectByTag(uint32_t record)
    {
        ee::Local<SpawnRequest> spawnRequest;
        const ee::Ptr<EntityType> type = findEntityType(ee::Ptr<ScenePoint>(record)->kindHash);
        if (!type)
            return;
        *spawnRequest = {kSpawnFromRecord, record};
        const uint32_t object = type->create(spawnRequest);
        if (object == 0)
            return;
        addLevelObject(ee::Ptr<SceneObject>(object), type);
        if (type->init && type->init(object) != 1)
            removeLevelObject(ee::Ptr<SceneObject>(object));
    }
    READABLE_FUNCTION_WAITS(spawnLevelObjectByTag, Verified);

    // forEachSceneBoneFromRoot
    uint32_t forEachSceneBoneFromRoot(ee::Ptr<Scene> scene, uint32_t callback, ee::Address context)
    {
        const ee::Ptr<SceneBone> root = rootBone(scene);
        return game::tail(zge::forEachSceneBone, scene, root, callback, context);
    }
    READABLE_FUNCTION_WAITS(forEachSceneBoneFromRoot, Verified);

    // forEachSceneBone: callback(node, bone, context) on a bone with a node, then
    // its children, then the siblings after it. A callback's 0 stops the
    // whole walk, and the walk returns 0.
    uint32_t forEachSceneBone(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, uint32_t callbackFunction, ee::Address context)
    {
        const ee::FunctionPtr<int32_t(ee::Ptr<Node> node, ee::Ptr<SceneBone> bone, uint32_t context)> callback(callbackFunction);
        do
        {
            const ee::Ptr<Node> node = bone == rootBone(scene) ? sceneRootNode(scene) : boneNode(scene, bone);
            if (node && callback(node, bone, context) == 0)
                return 0;
            const ee::Ptr<SceneBone> firstChild = boneFirstChild(scene, bone);
            if (firstChild && forEachSceneBone(scene, firstChild, callbackFunction, context) == 0)
                return 0;
            bone = boneNextSibling(scene, bone);
        } while (bone);
        return 1;
    }
    READABLE_FUNCTION_WAITS(forEachSceneBone, Verified);

    // setNodeLevelObject: a forEachSceneBone callback: the node belongs to the level
    // object (0: to none).
    uint32_t setNodeLevelObject(ee::Ptr<Node> node, ee::Ptr<SceneBone> /*bone*/, uint32_t levelObject)
    {
        node->levelObject = levelObject;
        return 1;
    }
    READABLE_FUNCTION(setNodeLevelObject, Verified);

    namespace
    {
        struct NodeBoneSearch
        {
            ee::Ptr<Node> nodePointer;
            ee::Ptr<SceneBone> bone;
        };
    }

    // findNodeBone: a forEachSceneBone callback: the bone of the node asked
    // for, which ends the walk.
    uint32_t findNodeBone(ee::Ptr<Node> node, ee::Ptr<SceneBone> bone, ee::Ptr<NodeBoneSearch> search)
    {
        if (search->nodePointer == node)
        {
            search->bone = bone;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(findNodeBone, Verified);

    // removeLevelObject: the object told it is going (kRemovedMessage), out of the
    // tree by name, its nodes no longer pointing here, its model released and
    // its record free. Its record stays on the object list.
    uint32_t removeLevelObject(ee::Ptr<SceneObject> object)
    {
        ee::Local<EngineMessage> message;
        ee::Local<uint32_t> nameKey;
        const ee::Ptr<LevelObject> record = levelObjectOf(object);
        message->data = 0;
        message->type = kRemovedMessage;
        message->target = ee::Address(object); // the message's word
        message->sender = 0;
        message->flags = kMessageNow;
        sendEngineMessage(message);
        *nameKey = objectNameHash(object);
        searchTreeRemove(g_levelObjectNames.get(), nameKey);
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 asks for the object's scene first: one with none has no nodes to tell and no model to release;
            // it is deleted instead (the tags it keeps where a model keeps its animator released first), unless
            // it is a particle emitter.
            if (objectSceneKindOrNone(object) != 0)
            {
                const ee::Ptr<Node> node(objectLevelNode(object));
                if (node && objectSceneKindOrNone(object) == kModelSceneKind &&
                    node->levelObject >= kLevelObjectsBegin && node->levelObject < kLevelObjectsEnd)
                    forEachSceneBoneFromRoot(sceneOf(object), kSetNodeLevelObject, 0);
            }
            setObjectOwner(object, 0);
            if (objectSceneKindOrNone(object) != 0)
                releaseSceneModel(object);
            else if ((object->flags & kObjectKindMask) != kObjectParticleEmitterKind)
            {
                releaseTagTable(object->tags);
                deleteSceneObject(object);
            }
        }
        else
        {
            const ee::Ptr<Node> node(objectLevelNode(object));
            if (node && objectSceneKindOrNone(object) == kModelSceneKind &&
                node->levelObject >= kLevelObjectsBegin && node->levelObject < kLevelObjectsEnd)
                forEachSceneBoneFromRoot(sceneOf(object), kSetNodeLevelObject, 0);
            setObjectOwner(object, 0);
            releaseSceneModel(object);
        }
        record->object = ee::Ptr<SceneObject>();
        --*g_levelObjectCount;
        return 1;
    }

    // unregisterLevelObject: ZGE2's removeLevelObject: the object told it is
    // going (kRemovedMessage), out of the names' tree, its owner cleared, its
    // record's node told, the object deleted (deleteSceneObject), the record
    // freed. What changed in ZGE3: every node of the model's bone tree is told
    // (setNodeLevelObject), and the object's scene model is released
    // (releaseSceneModel) where ZGE2 deletes the object.
    uint32_t unregisterLevelObject(ee::Ptr<SceneObject> object)
    {
        ee::Local<EngineMessage> message;
        ee::Local<uint32_t> nameKey;
        const ee::Ptr<LevelObject> record = levelObjectOf(object);
        message->flags = kMessageNow;
        message->type = kRemovedMessage;
        message->target = ee::Address(object); // the message's word
        message->sender = 0;
        message->data = 0;
        sendEngineMessage(message);
        *nameKey = objectNameHash(object);
        searchTreeRemove(g_levelObjectNames.get(), nameKey);
        setObjectOwner(object, 0);
        record->node->levelObject = 0;
        deleteSceneObject(object);
        record->object = ee::Ptr<SceneObject>();
        --*g_levelObjectCount;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(removeLevelObject, Verified, dm2, unregisterLevelObject);

    // isLevelObject
    int32_t isLevelObject(ee::Ptr<SceneObject> object)
    {
        return levelObjectOf(object) ? 1 : 0;
    }
    READABLE_FUNCTION(isLevelObject, Verified);

    // nodeSceneObject: the object a node belongs to, 0 if none; with `boneOut`,
    // the model bone the node is on too (its scene bone's name looked up in
    // the object's model: the top one for its root node, the only one for a
    // model of one bone, 0 for no object).
    // ZGE2's search of a scene's bone tree for the bone whose node is `node`, from `bone` down (the bone itself
    // first, then each child's tree); 0 if none. ZGE3 walks the bones with a visitor instead (findNodeBone through
    // forEachSceneBoneFromRoot).
    inline ee::Ptr<SceneBone> searchBoneTreeForNode(ee::Ptr<Node> node, ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone)
    {
        if (same(boneNode(scene, bone), node))
            return bone;
        for (ee::Ptr<SceneBone> child = boneFirstChild(scene, bone); child; child = boneNextSibling(scene, child))
            if (const ee::Ptr<SceneBone> found = searchBoneTreeForNode(node, scene, child))
                return found;
        return ee::Ptr<SceneBone>();
    }
    READABLE_FUNCTION_ONLY(dm2, searchBoneTreeForNode);

    ee::Ptr<SceneObject> nodeSceneObject(ee::Ptr<Node> node, ee::Ptr<ee::Ptr<SceneBone>> boneOut)
    {
        ee::Local<NodeBoneSearch> search;
        const ee::Ptr<LevelObject> record(node->levelObject);
        const bool pooled = node->levelObject >= kLevelObjectsBegin && node->levelObject < kLevelObjectsEnd;
        ee::Ptr<SceneBone> bone;
        // ZGE2 looks the bone up for any pooled record, asked for or not (its scenes have no kind), with its own
        // search of the bone tree; ZGE3 only when asked and for a model's scene, with the findNodeBone visitor.
        const bool lookedUp = game::readable::kIsDm2 ? pooled : pooled && boneOut && objectSceneKindOrNone(record->object) == kModelSceneKind;
        if (lookedUp)
        {
            const ee::Ptr<Scene> scene = sceneOf(record->object);
            if (same(nodeOfLevelObject(record), node))
                bone = rootBone(scene);
            else if (modelBoneCount(record->object) >= 2)
            {
                if constexpr (game::readable::kIsDm2)
                    bone = searchBoneTreeForNode(node, scene, rootBone(scene));
                else
                {
                    search->nodePointer = node;
                    search->bone = {};
                    forEachSceneBoneFromRoot(scene, kFindNodeBone, search);
                    bone = search->bone;
                }
            }
        }
        if (boneOut)
            *boneOut = pooled && record->object ? findModelBone(record->object, zge::boneId(sceneOf(record->object), bone)) : ee::Ptr<SceneBone>();
        return pooled ? record->object : ee::Ptr<SceneObject>();
    }
    READABLE_FUNCTION_WAITS(nodeSceneObject, Verified);

    // levelObjectClassTag: the first tag of the class of the object's node.
    ee::Ptr<TagRecord> levelObjectClassTag(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<LevelObject> record = levelObjectOf(object);
        const ee::Ptr<Node> node = nodeOfLevelObject(record);
        if (game::readable::kIsDm2 || node) // (ZGE2 asks a missing node's class through 0)
        {
            const ee::Ptr<TagTableHeader> tags = nodeClassTags(node);
            if (tags)
            {
                return tagRecordAt(tags, 0);
            }
        }
        return {};
    }
    READABLE_FUNCTION(levelObjectClassTag, Verified);

    // objectLevelNode
    ee::Ptr<Node> objectLevelNode(ee::Ptr<SceneObject> object)
    {
        return nodeOfLevelObject(levelObjectOf(object));
    }
    READABLE_FUNCTION(objectLevelNode, Verified);

    namespace
    {
        // ZGE4's kind of zobjParticleEmitter's objects (BMX XXX's newParticleEmitterSceneObject and its other maker
        // there both set up kind 4; ZGE3's are kind 2, particle_emitter_objects.cpp).
        constexpr uint64_t kZge4ParticleEmitterKind = 4;
    }

    // objectNameHash: its node's name's hash. ZGE4 names an object whose scene has no type (no model's) otherwise: a
    // particle emitter object by its class's id (0 without one), any other by its own address.
    uint32_t objectNameHash(ee::Ptr<SceneObject> object)
    {
        // ZGE2 reads the record's node straight off levelObjectOf (no node test; a missing record would fault).
        if constexpr (game::readable::kIsDm2)
            return nodeNameHash(levelObjectOf(object)->node);
        if constexpr (game::readable::kIsBmx)
        {
            const ee::Ptr<Scene> scene = sceneOf(object);
            if (!scene || !scene->type)
            {
                if ((object->flags & kObjectKindMask) != kZge4ParticleEmitterKind)
                    return ee::Address(object);
                if (!hasObjectClass(object))
                    return 0;
                const auto id = object->objectClass->id;
                return id ? id(object) : 0;
            }
        }
        const ee::Ptr<Node> node = objectLevelNode(object);
        uint32_t hash = 0;
        if (node)
        {
            // ZGE4 reads the hash off the node itself (it has no nodeNameHash).
            if constexpr (game::readable::kIsBmx)
                hash = node->nameHash;
            else
                hash = nodeNameHash(node);
        }
        return hash;
    }
    READABLE_FUNCTION(objectNameHash, Verified);

    // levelObjectTypeHash
    uint32_t levelObjectTypeHash(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<LevelObject> record(levelObjectOf(object));
        return ee::Ptr<EntityType>(record->type)->nameHash;
    }
    READABLE_FUNCTION(levelObjectTypeHash, Verified);

    // findEntityType
    ee::Ptr<EntityType> findEntityType(uint32_t nameHash)
    {
        if (nameHash != 0)
            for (uint32_t i = 0; i < kEntityTypeSlots; ++i)
                if (kEntityTypes[i].nameHash == nameHash)
                    return kEntityTypes + i;
        return ee::Ptr<EntityType>();
    }

    // findEntityInit: ZGE2's findEntityType: the type of that name among
    // those registered (a hash of 0 is looked for too); null if none.
    ee::Ptr<EntityInit> findEntityInit(uint32_t nameHash)
    {
        for (uint32_t i = 0; i < *g_levelObjectTypeCount; ++i)
            if (kEntityInits[i].nameHash == nameHash)
                return kEntityInits + i;
        return ee::Ptr<EntityInit>();
    }
    READABLE_FUNCTION_OWN(findEntityType, Verified, dm2, findEntityInit);

    // findObjects: the objects named by `count` hashes at `names`, in a
    // shared table ending in 0; 0 if none was found.
    ee::Ptr<ee::Ptr<SceneObject>> findObjects(ee::Address namesAddress, uint32_t count)
    {
        ee::Ptr<uint32_t> names(namesAddress); // the callers' name hashes, a word each
        const ee::Ptr<ee::Ptr<SceneObject>> table(kFoundObjects);
        uint32_t found = 0;
        for (uint32_t i = 0; i < count; ++i, names += 1)
        {
            const ee::Ptr<LevelObjectName> entry = searchTreeEntry<LevelObjectName>(searchTreeFind(g_levelObjectNames.get(), names));
            if (entry)
                table[found++] = entry->levelObject->object;
        }
        if (found == 0)
            return {};
        table[found] = ee::Ptr<SceneObject>();
        return table;
    }
    READABLE_FUNCTION_WAITS(findObjects, Verified);

    // registerLevelObjectType: a type by name, with its hooks: 0 if the name is taken by
    // other hooks. Nothing stops the table's 128 slots overflowing.
    uint32_t registerLevelObjectType(uint32_t name, uint32_t create, uint32_t init)
    {
        const uint32_t hash = nameHash(ee::Ptr<const char>(name));
        ee::Ptr<EntityType> type = kEntityTypes;
        for (uint32_t i = 0; i < kEntityTypeSlots && type->nameHash != 0; ++i, type = type + 1)
            if (type->nameHash == hash)
                return type->create == create && type->init == init ? 1 : 0;
        type->nameHash = hash;
        type->create = create;
        type->init = init;
        stringCopy(type->name, name);
        return 1;
    }

    // registerEntityInit: ZGE2's registerLevelObjectType: a type by name
    // with the init its entities get, after the ones registered: 1 (also
    // when the name has that init already), 0 if the name has another one
    // or all 128 are taken.
    uint32_t registerEntityInit(ee::Ptr<const char> name, uint32_t init)
    {
        const uint32_t hash = nameHash(name);
        const uint32_t count = *g_levelObjectTypeCount;
        if (count == kEntityTypeSlots)
            return 0;
        for (uint32_t i = 0; i < count; ++i)
            if (kEntityInits[i].nameHash == hash)
                return kEntityInits[i].init == init ? 1 : 0;
        kEntityInits[count].nameHash = hash;
        kEntityInits[count].init = init;
        *g_levelObjectTypeCount = count + 1;
        return 1;
    }
    READABLE_FUNCTION_OWN(registerLevelObjectType, Verified, dm2, registerEntityInit);

    // spawnLevelObjectFromObject (ZGE4 only): the same for an object that exists
    // already, as a level object of the type named by the hash.
    inline void spawnLevelObjectFromObject(ee::Ptr<SceneObject> object, uint32_t typeHash)
    {
        ee::Local<SpawnRequest> spawnRequest;
        const ee::Ptr<EntityType> type = findEntityType(typeHash);
        if (!type)
            return;
        *spawnRequest = {kSpawnFromObject, ee::Address(object)};
        const uint32_t made = type->create(spawnRequest);
        if (made == 0)
            return;
        addLevelObject(ee::Ptr<SceneObject>(made), type);
        if (type->init && type->init(made) != 1)
            removeLevelObject(ee::Ptr<SceneObject>(made));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, spawnLevelObjectFromObject);

    namespace
    {
        // The "ZPE" type's name hash (the game's particle emitter level object, bmx/level/particle_emitters.cpp).
        constexpr uint32_t kZpeTypeHash = 0x5F45u; // not an address
        // ZGE4's objectComponent keeps the object it last looked up and what it found (a cache of one):
        // spawnLevelObjects and removeLevelObjects empty it.
        inline ee::Global<uint32_t, sym::bmx::g_componentCacheObject> g_componentCacheObject;
        inline ee::Global<uint32_t, sym::bmx::g_componentCacheRecord> g_componentCacheRecord;
    }

    // spawnZpeLevelObject (ZGE4 only; spawnLevelObjects' visitor of the particle
    // emitter objects): an emitter with tags becomes a "ZPE" level object. 1.
    inline uint32_t spawnZpeLevelObject(ee::Ptr<SceneObject> emitter)
    {
        if (emitterTagsOf(emitter)->tags)
            spawnLevelObjectFromObject(emitter, kZpeTypeHash);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, spawnZpeLevelObject);

    // spawnLevelObjects: a level's objects: every world mesh's entity, then each
    // locator's (ZGE4: then each particle emitter with tags, as a "ZPE").
    uint32_t spawnLevelObjects()
    {
        ee::Local<LocatorList> locators;
        initList(g_objectList.get());
        initSearchTree(g_levelObjectNames.get(), kCompareNameHashes);
        forEachWorldMesh(kSpawnLevelObject, 0);
        initList(locators);
        findLocators(locators, 0, 0);
        for (ee::Ptr<Locator> locator = locators->head; locator; locator = locator->next)
            spawnLevelObjectByTag(locator->point);
        freeLocators(locators);
        if constexpr (game::readable::kIsBmx)
        {
            forEachObjectOfKind(sym::bmx::spawnZpeLevelObject, 0, kZge4ParticleEmitterKind);
            *g_componentCacheObject = 0;
            *g_componentCacheRecord = 0;
        }
        return 1;
    }

    // spawnWorldMeshObjects: ZGE2's spawnLevelObjects: a level's objects, every world mesh's entity; ZGE2 has no
    // locators' pass. Always 1.
    uint32_t spawnWorldMeshObjects()
    {
        initList(g_objectList.get());
        initSearchTree(g_levelObjectNames.get(), kCompareNameHashes);
        forEachWorldMesh(kSpawnLevelObject, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(spawnLevelObjects, Verified, dm2, spawnWorldMeshObjects);

    // removeLevelObjects: every level object removed, the object list empty.
    uint32_t removeLevelObjects()
    {
        initList(g_objectList.get());
        const ee::Ptr<LevelObject> records(kLevelObjectsBegin);
        for (uint32_t i = 0; i < kLevelObjects; ++i)
            if (const ee::Ptr<SceneObject> object = records[i].object)
                removeLevelObject(object);
        if constexpr (game::readable::kIsBmx)
        {
            *g_componentCacheObject = 0;
            *g_componentCacheRecord = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(removeLevelObjects, Verified);

    // createEntityObject: the plain entity: the node's model as an object, where
    // the node was unless it is mirrored. The node
    // goes to the identity first. The entity type found is not used.
    ee::Ptr<SceneObject> createEntityObject(ee::Ptr<Node> node)
    {
        ee::Local<Matrix> rotation;
        ee::Local<Vec4> position;
        const uint32_t hash = nodeEntityType(node);
        findEntityType(hash);
        const ee::Ptr<Matrix> world = nodeMatrix(node, 1);
        *rotation = *world;
        setNodeLocalMatrix(node, ee::Ptr<const Matrix>(kIdentityMatrix));
        ee::Ptr<SceneObject> object;
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 reuses the node's model object where it has one (nodeModelObject), and leaves an object that
            // belongs to something where it is.
            object = nodeModelObject(node, 0);
            if (!object || sceneObjectOwner(object))
                return object;
        }
        else
            object = newModelObject(setupNodeSceneModel(node), 0);
        if ((node->flags & kNodeMirrored) == 0)
        {
            setObjectRotationMatrix(object, rotation);
            setObjectPosition(object, position);
        }
        return object;
    }
    READABLE_FUNCTION_WAITS(createEntityObject, Verified);

    // createLevelEntity: the plain entity type's create hook, from a node only.
    ee::Ptr<SceneObject> createLevelEntity(ee::Ptr<SpawnRequest> request)
    {
        ee::Ptr<SceneObject> object;
        if (request->from == kSpawnFromNode)
            object = createEntityObject(ee::Ptr<Node>(request->source)); // the request's source word: a node here
        return object;
    }
    READABLE_FUNCTION_WAITS(createLevelEntity, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_sceneObjectPool> g_sceneObjectPool;
        constexpr uint32_t kLevelObjectTypesReady = sym::g_levelObjectTypesReady;
        constexpr uint32_t kForEachObject = sym::forEachObject; // forEachObject ((visit(object, context), context); readable: forEachObject_, Verified)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(forEachObjectint32_t, ee::game<int32_t>, kForEachObject);
        }
    }

    // G_levelObjectCount.
    uint32_t levelObjectTypeCount()
    {
        return *g_levelObjectCount;
    }
    READABLE_FUNCTION(levelObjectTypeCount, Draft);

    // The level objects' save size (forEachObject with sumObjectSaveSize)
    int32_t levelObjectsSaveSize()
    {
        ee::Local<int32_t> dataSlot;
        *dataSlot = 4;
        zge::forEachObject(sym::sumObjectSaveSize, dataSlot.address());
        const int32_t data = *dataSlot;
        return (data ^ 0x4u) == 0 ? 0 : data;
    }

    int32_t clearLevelObjectTypes()
    {
        return memoryFill(kLevelObjectTypesReady, 0, 0x2600);
    }
    READABLE_FUNCTION(clearLevelObjectTypes, Draft);

    // (name, function, arg): forEachObject over those with the name's hash.
    int32_t forEachObjectNamed(uint32_t name, uint32_t callback, uint32_t userData)
    {
        ee::Local<int32_t> nameHash_Slot;
        ee::Local<int32_t> callbackSlot;
        ee::Local<int32_t> userDataSlot;
        const int32_t nameHashResult = nameHash(ee::Ptr<const char>(name));
        *callbackSlot = callback;
        *userDataSlot = userData;
        *nameHash_Slot = nameHashResult;
        return routed::forEachObjectint32_t(sym::visitLevelObjectOfType, nameHash_Slot.address());
    }
    READABLE_FUNCTION_WAITS(forEachObjectNamed, Draft);

    // ---- the level objects' save (nothing calls it in AI) --------------------
    // A save is a word, its own size, then a record an object: the bytes of
    // its state, its name's hash, its state.

    namespace
    {
        struct SavedObject
        {
            uint32_t stateBytes; // +0x00
            uint32_t nameHash;   // +0x04 objectNameHash: how restoreLevelObjects finds it
            uint8_t state[1];    // +0x08 [stateBytes]
        };
        constexpr uint32_t kSavedObjectHeader = offsetof(SavedObject, state);
        constexpr uint32_t kSaveSizeBytes = sizeof(uint32_t); // the save's first word

        // The records lie packed in the save's bytes: a place in it is a
        // word, the record there and the next after it.
        uint32_t afterSavedObject(uint32_t at) { return at + kSavedObjectHeader + ee::Ptr<SavedObject>(at)->stateBytes; }

        // A message delivered to the object at once, from no one; data is
        // what the message's type says it points to.
        int32_t sendNow(ee::Ptr<EngineMessage> message, ee::Address object, uint32_t type, ee::Address data)
        {
            message->type = type;
            message->data = data;
            message->target = object;
            message->flags = kMessageNow;
            message->sender = 0;
            return sendEngineMessage(message);
        }

    }

    // objectSaveSize: how many bytes of state the object saves (it answers
    // kSaveSizeMessage; 0 when it keeps none).
    uint32_t objectSaveSize(ee::Ptr<SceneObject> object)
    {
        ee::Local<EngineMessage> message;
        ee::Local<uint32_t> bytes;
        *bytes = 0;
        sendNow(message, object, kSaveSizeMessage, bytes.address());
        return *bytes;
    }
    READABLE_FUNCTION_WAITS(objectSaveSize, Draft);

    // sumObjectSaveSize (forEachObject's visit, levelObjectsSaveSize): the
    // object's record added to the total (its state and the record's
    // header), when it saves any. 1.
    uint32_t sumObjectSaveSize(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> total)
    {
        if (const uint32_t bytes = objectSaveSize(object))
            *total = *total + bytes + kSavedObjectHeader;
        return 1;
    }
    READABLE_FUNCTION_WAITS(sumObjectSaveSize, Draft);

    // saveObjectState (forEachObject's visit, saveLevelObjects): the object's
    // record written at the cursor (kSaveMessage writes its state), the
    // cursor moved past it; nothing for an object that saves nothing. 1.
    uint32_t saveObjectState(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> cursor)
    {
        ee::Local<EngineMessage> message;
        const uint32_t bytes = objectSaveSize(object);
        if (bytes == 0)
            return 1;
        const uint32_t at = *cursor;
        const ee::Ptr<SavedObject> record(at);
        record->stateBytes = bytes;
        record->nameHash = objectNameHash(object);
        sendNow(message, object, kSaveMessage, at + offsetof(SavedObject, state));
        *cursor = *cursor + kSavedObjectHeader + record->stateBytes; // the cursor read again, as the original
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveObjectState, Draft);

    // objectVelocity: the object's velocity into out ((0, 0, 0, 1) if it
    // does not answer kVelocityMessage). Returns the delivery's result.
    int32_t objectVelocity(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out)
    {
        ee::Local<EngineMessage> message;
        aligned(out) = kZero;
        return sendNow(message, object, kVelocityMessage, out);
    }
    READABLE_FUNCTION_WAITS(objectVelocity, Draft);

    // saveLevelObjects: every object's record written after the save's
    // first word, then the save's size in it. Returns the size.
    uint32_t saveLevelObjects(uint32_t save)
    {
        ee::Local<uint32_t> cursor;
        *cursor = save + kSaveSizeBytes;
        zge::forEachObject(sym::saveObjectState, cursor.address());
        const uint32_t size = *cursor - save;
        *ee::Ptr<uint32_t>(save) = size;
        return size;
    }

    // sumListedObjectsSaveSize: ZGE2's levelObjectsSaveSize walks the object
    // list itself (no forEachObject, no visit): each saving object's state
    // and record header added to the save's first word; 0 when none saves.
    int32_t sumListedObjectsSaveSize()
    {
        uint32_t total = kSaveSizeBytes;
        for (ee::Ptr<ObjectListNode> node = recordOfNode<ObjectListNode>(g_objectList->head); node;)
        {
            const uint32_t bytes = objectSaveSize(ee::Ptr<SceneObject>(node->object));
            node = recordOfNode<ObjectListNode>(node->link.next);
            if (bytes != 0)
                total = total + bytes + kSavedObjectHeader;
        }
        return total == kSaveSizeBytes ? 0 : static_cast<int32_t>(total);
    }
    READABLE_FUNCTION_WAITS_OWN(levelObjectsSaveSize, Draft, dm2, sumListedObjectsSaveSize);

    // saveListedObjects: ZGE2's saveLevelObjects walks the object list
    // itself: each saving object's record written at the cursor as
    // saveObjectState writes one. Returns the save's size, kept in its first
    // word.
    uint32_t saveListedObjects(uint32_t save)
    {
        ee::Local<EngineMessage> message;
        uint32_t cursor = save + kSaveSizeBytes;
        for (ee::Ptr<ObjectListNode> node = recordOfNode<ObjectListNode>(g_objectList->head); node;
             node = recordOfNode<ObjectListNode>(node->link.next))
        {
            const ee::Ptr<SceneObject> object(node->object);
            const uint32_t bytes = objectSaveSize(object);
            if (bytes == 0)
                continue;
            const ee::Ptr<SavedObject> record(cursor);
            record->stateBytes = bytes;
            record->nameHash = objectNameHash(object);
            sendNow(message, node->object, kSaveMessage, cursor + offsetof(SavedObject, state));
            cursor = cursor + record->stateBytes + kSavedObjectHeader;
        }
        const uint32_t size = cursor - save;
        *ee::Ptr<uint32_t>(save) = size;
        return size;
    }
    READABLE_FUNCTION_WAITS_OWN(saveLevelObjects, Draft, dm2, saveListedObjects);

    // restoreLevelObjects: each record's state handed back (kRestoreMessage)
    // to the object with its name. Kept bug: a name no object has makes it
    // read the found table's word at address 0.
    void restoreLevelObjects(uint32_t save)
    {
        ee::Local<EngineMessage> message;
        const uint32_t end = save + *ee::Ptr<uint32_t>(save);
        for (uint32_t at = save + kSaveSizeBytes; at < end; at = afterSavedObject(at))
        {
            const ee::Ptr<ee::Ptr<SceneObject>> found = findObjects(at + offsetof(SavedObject, nameHash), 1);
            sendNow(message, *found, kRestoreMessage, at + offsetof(SavedObject, state));
        }
    }
    READABLE_FUNCTION_WAITS(restoreLevelObjects, Draft);

    // A visit of forEachLevelObjectType's: (type's name hash, context).
    using LevelObjectTypeVisit = ee::FunctionPtr<int32_t(uint32_t nameHash, uint32_t context)>;

    // forEachLevelObjectType: visit(type's name hash, context) over the 128
    // type slots, empty ones too, until a visit returns anything but 1.
    // Returns that, or 0 after the last.
    int32_t forEachLevelObjectType(LevelObjectTypeVisit visit, uint32_t context)
    {
        for (uint32_t i = 0; i < kEntityTypeSlots; ++i)
        {
            const int32_t result = visit(kEntityTypes[i].nameHash, context);
            if (result != 1)
                return result;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(forEachLevelObjectType, Draft);

    // unregisterLevelObjectType (was findLevelObjectType): the slot of the
    // type with that name freed (its hash cleared). 1, or 0 if no type has
    // the name.
    uint32_t unregisterLevelObjectType(uint32_t name)
    {
        const uint32_t hash = nameHash(ee::Ptr<const char>(name));
        for (uint32_t i = 0; i < kEntityTypeSlots; ++i)
        {
            if (kEntityTypes[i].nameHash == hash)
            {
                kEntityTypes[i].nameHash = 0;
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(unregisterLevelObjectType, Draft);

    // findObjectByName: the level object with the name, 0 if none.
    ee::Ptr<SceneObject> findObjectByName(uint32_t name)
    {
        ee::Local<uint32_t> hash;
        *hash = nameHash(ee::Ptr<const char>(name));
        const ee::Ptr<ee::Ptr<SceneObject>> found = findObjects(hash, 1);
        return found ? *found : ee::Ptr<SceneObject>();
    }
    READABLE_FUNCTION_WAITS(findObjectByName, Draft);
}
