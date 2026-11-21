// Scene objects (zobj.c, from placeAttached on): an object's scene, its
// position, the update of the objects whose pose went out of date, the
// module's shutdown, and the attachments (ObjectAttachment: an object hung
// from another, or from one of its bones).
//

#include "scene.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/fx_manager.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/debug/debug.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/scene/scene_objects.cpp.h"

namespace zge
{
    int32_t updateStaleObjects();
    uint32_t debugStubZero(); // zge/debug/debug_draw.cpp

    using ObjectVisitor = ee::FunctionPtr<void(ee::Ptr<SceneObject> object, uint32_t data)>;

    namespace
    {
        // The scene objects' pool.
        inline ee::Global<ee::Ptr<Pool>, sym::g_sceneObjectPool> g_sceneObjectPool;
        // (object, visit, data): visit(attached, data) for the objects hanging
        // from it; the visit placeAttached places one by the matrix in data.
        constexpr uint32_t kForEachAttached = sym::forEachAttached;
        constexpr uint32_t kPlaceAttached = sym::placeAttached;
    }

    namespace
    {
        // Set while the scene objects are set up (initSceneObjects).
        inline ee::Global<int32_t, sym::g_sceneObjectsReady> g_sceneObjectsReady;
        // The attachment records' pool, made with the objects'.
        inline ee::Global<ee::Ptr<Pool>, sym::g_attachmentPool> g_attachmentPool;
        // The task that runs updateStaleObjects.
        inline ee::Global<ee::Ptr<Task>, sym::g_staleObjectsTask> g_staleObjectsTask;

        constexpr uint32_t kCreatePool = sym::createPool;     // (record bytes, records a block | memory flags << 16, free callback) -> pool
        constexpr uint32_t kUpdateStaleObjects = sym::updateStaleObjects; // updateStaleObjects below, the task's function
        constexpr uint32_t kDeleteSceneObject = sym::deleteSceneObject;  // deleteSceneObject below, the pool's free callback
        constexpr uint32_t kZobjSource = sym::zobjSource;     // "../ngengine/ZGE3/zobj.c"
        constexpr uint32_t kZobjTaskLine = game::readable::perGame(201u, 201u, 234u); // zobj.c's line of the task's making
        constexpr uint32_t kStaleObjectsTaskPriority = game::readable::perGame(263u, 262u, 263u);
        // ZGE2's registration of the object pool's debug report (debugStubZero's answer: none in this build).
        inline ee::Global<uint32_t, sym::dm2::g_sceneObjectReport> g_sceneObjectReport;
        constexpr uint32_t kSceneObjectsABlock = game::readable::perGame(0x100u, 0x100u, 0x40u);
        // The object kinds' set-ups, in the order initSceneObjects runs them:
        // zobjModel's (initModelObjects, model_objects.cpp) and the three
        // kinds after it (updatePose .., their module names not in the ELF).
        constexpr ee::FunctionPtr<int32_t()> kObjectKindSetups[] = {sym::initModelObjects, sym::initLightObjects, sym::initCameraObjects, sym::initEmitterObjects};
        constexpr uint32_t kInitEmitterObjects = sym::registerParticleEmitterChunkHandlers; // zobjParticleEmitter's set-up
        // (object): its message box released (zprofile block, releaseMessages).
        // (object): taken out of the attachment it hangs in.
        constexpr uint32_t kDetachObject = sym::detachObject;
        // (record) -> its pool link (the record less the link's 16 bytes).
        constexpr uint32_t kPoolLinkOf = sym::poolLinkOf;
        // setObjectOrientation below, called by address as the originals do
        // (it ends in a tail call).
        constexpr uint32_t kSetObjectOrientation = sym::setObjectOrientation;
    }

    namespace
    {
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t registerParticleEmitterChunkHandlers() { return ee::game<int32_t>(kInitEmitterObjects); }
    }

    // initSceneObjects: the scene objects' module set up: their pool (256 a
    // block, deleteSceneObject when one is freed), the attachment pool, the
    // task that updates the stale objects, then each object kind's set-up and
    // the effects'. 1 when all of them worked.
    int32_t initSceneObjects()
    {
        *g_sceneObjectsReady = 1;
        *g_sceneObjectPool = ee::Ptr<Pool>(createPool(sizeof(SceneObject), kSceneObjectsABlock, kDeleteSceneObject));
        *g_attachmentPool = ee::Ptr<Pool>(createPool(sizeof(ObjectAttachment), 4, 0));
        *g_staleObjectsTask = newTask(kStaleObjectsTaskPriority, kUpdateStaleObjects, 0, 0, kZobjSource, kZobjTaskLine);
        if (!*g_staleObjectsTask)
            return 0;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 registers the pool's debug report (debugStubZero, handed reportSceneObjectPool: a stub in this
            // build), and the emitters' set-up is its last step: it has no effect arrays and no emitter chunks.
            *g_sceneObjectReport = debugStubZero();
            for (uint32_t kind = 0; kind < 3; ++kind)
                if (!kObjectKindSetups[kind]())
                    return 0;
            return kObjectKindSetups[3]() ? 1 : 0;
        }
        for (const auto setup : kObjectKindSetups)
            if (!setup())
                return 0;
        if (!initEffectArrays())
            return 0;
        return registerParticleEmitterChunkHandlers() ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(initSceneObjects, Verified);

    // newObjectOfKind: ZGE4's initSceneObject, which takes the kind alone: a new object's record from the pool,
    // cleared and set up as `kind`: no turn, at the origin, moved and its pose out of date (so the next update
    // places it). 0 when the pool gave none.
    ee::Ptr<SceneObject> newObjectOfKind(uint32_t kind)
    {
        const ee::Ptr<SceneObject> object(poolAlloc(*g_sceneObjectPool));
        if (!object)
            return object;
        memoryFill(object, 0, sizeof(SceneObject));
        object->flags = (object->flags & ~kObjectKindMask) | (kind & kObjectKindMask) | kObjectMoved | kObjectPoseStale;
        quaternionIdentity(&object->orientation);
        object->position = kZero;
        return object;
    }

    // newSceneObject: a new object's record from the pool (not set up; ZGE4's is set up as kind 0).
    ee::Ptr<SceneObject> newSceneObject()
    {
        if constexpr (game::readable::kIsBmx)
            return newObjectOfKind(0);
        return ee::Ptr<SceneObject>(poolAlloc(*g_sceneObjectPool));
    }
    READABLE_FUNCTION_WAITS(newSceneObject, Verified);

    // initSceneObject: an object's record cleared and set up as `kind`: no
    // turn, moved and its pose out of date (so the next update places it).
    ee::Ptr<SceneObject> initSceneObject(ee::Ptr<SceneObject> object, uint32_t kind)
    {
        memoryFill(object, 0, sizeof(SceneObject));
        quaternionIdentity(&object->orientation);
        // ZGE2 has no pose-stale flag (no markPoseStale step either).
        constexpr uint64_t kNewObjectFlags = game::readable::kIsDm2 ? kObjectMoved : kObjectMoved | kObjectPoseStale;
        object->flags = (object->flags & ~kObjectKindMask) | (kind & kObjectKindMask) | kNewObjectFlags;
        return object;
    }
    READABLE_FUNCTION_OWN(initSceneObject, Verified, bmx, newObjectOfKind);

    int32_t detachObject(ee::Ptr<SceneObject> object);
    int32_t releaseAttachment(ee::Ptr<SceneObject> object);

    // deleteSceneObject: the pool's free callback: the object's messages
    // released, its class told (destroy), taken out of its attachment, and
    // its record handed back. 1.
    uint32_t deleteSceneObject(ee::Ptr<SceneObject> object)
    {
        releaseMessages(object);
        if (hasObjectClass(object))
            if (const auto destroy = object->objectClass->destroy)
                destroy(object);
        if (object->attachment)
        {
            detachObject(object);
            // ZGE4's detachObject leaves the attachment record to its caller.
            if constexpr (game::readable::kIsBmx)
                if (object->attachment)
                    releaseAttachment(object);
        }
        poolFree(*g_sceneObjectPool, object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(deleteSceneObject, Verified);

    // sceneOf (kSceneOf): the object's scene.
    ee::Ptr<Scene> sceneOf(ee::Ptr<SceneObject> object) { return object->scene; }
    READABLE_FUNCTION(sceneOf, Verified);

    // setObjectPosition: the object's position set, its class's hook told
    // (unless kObjectNoHooks, or in ZGE4 an object with no class), its pose marked out of date.
    ee::Ptr<SceneObject> setObjectPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        object->position = quadwordOf(position);
        // ZGE3 marks the pose stale after a move or a turn (markPoseStale) and answers the top object; ZGE2 has no such
        // step and answers what its hook step left: 1 with hooks off, 0 without a hook, else the hook's own word.
        uint32_t hookWord = 1;
        if (!(object->flags & kObjectNoHooks) && hasObjectClass(object))
        {
            hookWord = 0;
            if (const auto moved = object->objectClass->moved)
                hookWord = moved(object, position);
        }
        if constexpr (game::readable::kIsDm2)
            return ee::Ptr<SceneObject>(hookWord);
        return game::tail(zge::markPoseStale, object);
    }
    READABLE_FUNCTION(setObjectPosition, Verified);

    // objectPosition: where the object's position is kept.
    ee::Ptr<Vec4> objectPosition(ee::Ptr<SceneObject> object)
    {
        return &object->position;
    }
    READABLE_FUNCTION(objectPosition, Verified);

    // setObjectOrientation: the object's orientation set, its class told
    // (rotated; not with kObjectNoHooks, or in ZGE4 an object with no class), its pose marked out of date.
    ee::Ptr<SceneObject> setObjectOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        // The orientation may sit off a 16-byte boundary (callers pass one at a word boundary, which the original
        // copies by unaligned doublewords), so it is read as bytes from its plain address: a Quaternion read
        // through its typed pointer is an aligned quadword load on the host, which faults there on x86.
        object->orientation = loadUnaligned<Quaternion>(ee::Address(orientation));
        uint32_t hookWord = 1; // (as setObjectPosition's)
        if (!(object->flags & kObjectNoHooks) && hasObjectClass(object))
        {
            hookWord = 0;
            if (const auto rotated = object->objectClass->rotated)
                hookWord = rotated(object, orientation);
        }
        if constexpr (game::readable::kIsDm2)
            return ee::Ptr<SceneObject>(hookWord);
        return game::tail(zge::markPoseStale, object);
    }
    READABLE_FUNCTION(setObjectOrientation, Verified);

    // setObjectAlwaysUpdated (ZGE4 only): kObjectAlwaysUpdated set when `on`, else cleared (the particle emitter
    // object's update: on while its particles live).
    inline void setObjectAlwaysUpdated(ee::Ptr<SceneObject> object, uint32_t on)
    {
        if (on)
            object->flags |= kObjectAlwaysUpdated;
        else
            object->flags &= ~kObjectAlwaysUpdated;
    }
    READABLE_FUNCTION_ONLY(bmx, setObjectAlwaysUpdated);

    // objectOrientation: where the object's orientation is kept.
    ee::Ptr<Quaternion> objectOrientation(ee::Ptr<SceneObject> object)
    {
        return &object->orientation;
    }
    READABLE_FUNCTION(objectOrientation, Verified);

    // setObjectAngles (kSetNodeAngles): the object turned to Euler angles
    // (int16 x, y, z), its pose marked out of date (again: its result).
    ee::Ptr<SceneObject> setObjectAngles(ee::Ptr<SceneObject> object, ee::Ptr<EulerAngles> angles)
    {
        ee::Local<Quaternion> orientation;
        quaternionFromEuler(orientation, angles);
        if constexpr (game::readable::kIsDm2) // (ZGE2 has no markPoseStale step: the turn's own answer)
            return setObjectOrientation(object, orientation);
        setObjectOrientation(object, orientation);
        return markPoseStale(object);
    }
    READABLE_FUNCTION_WAITS(setObjectAngles, Verified);

    // setObjectRotationMatrix: the object turned as a matrix's rotation, its pose
    // marked out of date (again: its result).
    ee::Ptr<SceneObject> setObjectRotationMatrix(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> rotation)
    {
        ee::Local<Quaternion> orientation;
        matrixToQuaternion(orientation, rotation);
        if constexpr (game::readable::kIsDm2) // (ZGE2 has no markPoseStale step: the turn's own answer)
            return setObjectOrientation(object, orientation);
        setObjectOrientation(object, orientation);
        return markPoseStale(object);
    }
    READABLE_FUNCTION_WAITS(setObjectRotationMatrix, Verified);

    // setObjectOwner: the level object the object belongs to.
    void setObjectOwner(ee::Ptr<SceneObject> object, ee::Address owner)
    {
        object->owner = owner;
    }
    READABLE_FUNCTION(setObjectOwner, Verified);

    // setObjectCastsShadow: whether the object casts a shadow (the guiModel widget's
    // ShowShadow, its only caller, drawGuiModel): kObjectCastsShadow set or
    // cleared and, for a model (kObjectModelKind), its scene's nodes changed with
    // it (a tail call; 0). Another kind: its flags.
    uint64_t setObjectCastsShadow(ee::Ptr<SceneObject> object, uint32_t casts)
    {
        if (casts)
        {
            const uint64_t flags = object->flags | kObjectCastsShadow;
            object->flags = flags;
            if ((object->flags & kObjectKindMask) != kObjectModelKind)
                return flags;
            return game::tail(changeSceneState, sceneOf(object), 0, kSceneCastsShadow);
        }
        const uint64_t flags = object->flags & ~kObjectCastsShadow;
        object->flags = flags;
        if ((object->flags & kObjectKindMask) == kObjectModelKind)
            return game::tail(changeSceneState, sceneOf(object), kSceneCastsShadow, 0);
        return flags;
    }
    READABLE_FUNCTION_WAITS(setObjectCastsShadow, Verified);

    // findSceneObject: the next pooled object (after `after`, or from the
    // newest when 0) whose class gives `id`; an object whose class has no id
    // matches 0. 0 when none does.
    uint32_t findSceneObject(uint32_t id, uint32_t /*unused*/, uint32_t after)
    {
        ee::Ptr<ListNode> link = after ? nextPoolLink(poolLinkOfRecord(after)) : firstPoolLink(*g_sceneObjectPool);
        for (; link; link = nextPoolLink(link))
        {
            const ee::Ptr<SceneObject> object = poolRecordAs<SceneObject>(link);
            // (ZGE4: an object with no class matches 0 as well.)
            const auto idOf = hasObjectClass(object) ? object->objectClass->id : decltype(SceneObjectClass::id)();
            if (idOf ? idOf(object) == id : id == 0)
                return ee::Address(object);
        }
        return 0;
    }
    READABLE_FUNCTION(findSceneObject, Verified);

    uint32_t forEachAttached(ee::Ptr<SceneObject> object, uint32_t visit, ee::Address data);

    // updateStaleObjects: every pooled object marked kObjectMoved whose pose is out
    // of date (and whose hooks are on): the mark cleared, its class's update
    // hook called, and, when it is in an attachment, the objects hanging
    // from it placed by its orientation and position. 1.
    namespace
    {
        // The objects hanging from one placed by its matrix (its orientation and position).
        void placeAttachedObjects(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> matrix)
        {
            quaternionToMatrix(matrix, &object->orientation);
            std::memcpy(matrix->m[3], &object->position, sizeof(Vec4));
            forEachAttached(object, kPlaceAttached, matrix);
        }
    }

    // updateMovedObjects: ZGE2's updateStaleObjects. ZGE2 has no pose-stale flag: every moved object whose hooks are
    // on is updated on each pass (no flag cleared), and the objects hanging from it placed. 1.
    int32_t updateMovedObjects()
    {
        ee::Local<Matrix> matrix;
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_sceneObjectPool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<SceneObject> object = poolRecordAs<SceneObject>(link);
            if ((object->flags & (kObjectMoved | kObjectNoHooks)) != kObjectMoved)
                continue;
            if (const auto update = object->objectClass->update)
                update(object);
            if (object->attachment)
                placeAttachedObjects(object, matrix);
        }
        return 1;
    }

    // updateStaleAndAlwaysUpdated: ZGE4's updateStaleObjects. A stale object is updated whether or not it moved
    // (its hooks on), and a kObjectAlwaysUpdated one on every pass; the pose-stale flag cleared, the class's hook
    // called when the object has a class, and the objects hanging from it placed. 1.
    int32_t updateStaleAndAlwaysUpdated()
    {
        ee::Local<Matrix> matrix;
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_sceneObjectPool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<SceneObject> object = poolRecordAs<SceneObject>(link);
            const uint64_t flags = object->flags;
            if ((flags & (kObjectNoHooks | kObjectPoseStale)) != kObjectPoseStale && !(flags & kObjectAlwaysUpdated))
                continue;
            object->flags = flags & ~kObjectPoseStale;
            if (object->objectClass)
                if (const auto update = object->objectClass->update)
                    update(object);
            if (object->attachment)
                placeAttachedObjects(object, matrix);
        }
        return 1;
    }

    // updateStaleObjects (the stale-objects task's function): each moved object whose pose is out of date (its hooks
    // on) updated by its class, and the objects hanging from it placed by its matrix. 1.
    int32_t updateStaleObjects()
    {
        ee::Local<Matrix> matrix;
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_sceneObjectPool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<SceneObject> object = poolRecordAs<SceneObject>(link);
            const uint64_t flags = object->flags;
            if ((flags & (kObjectMoved | kObjectNoHooks | kObjectPoseStale)) != (kObjectMoved | kObjectPoseStale))
                continue;
            const auto update = object->objectClass->update;
            object->flags = flags & ~kObjectPoseStale;
            if (update)
                update(object);
            if (object->attachment)
            {
                quaternionToMatrix(matrix, &object->orientation);
                std::memcpy(matrix->m[3], &object->position, sizeof(Vec4));
                forEachAttached(object, kPlaceAttached, matrix);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(updateStaleObjects, Verified, dm2, updateMovedObjects, bmx, updateStaleAndAlwaysUpdated);

    void placeOnParent(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> matrix);

    // placeAttached (forEachAttached's visit): an object hanging from one
    // that moved, placed by its parent's matrix when it is marked
    // kObjectMoved (placeOnParent), its class's update hook called, and then
    // the objects hanging from it placed in turn by its own matrix (its
    // orientation and position times the parent's). 1.
    uint32_t placeAttached(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> parentMatrix)
    {
        ee::Local<Matrix> matrix;
        // (ZGE4 places every attached object, moved or not)
        if (!game::readable::kIsBmx && (object->flags & kObjectMoved) == 0)
            return 1;
        zge::placeOnParent(object, parentMatrix);
        if (hasObjectClass(object))
            if (const auto update = object->objectClass->update)
                update(object);
        quaternionToMatrix(matrix, &object->orientation);
        std::memcpy(matrix->m[3], &object->position, sizeof(Vec4));
        matrixMultiply(matrix, matrix, ee::Ptr<Matrix>(parentMatrix));
        forEachAttached(object, kPlaceAttached, matrix);
        return 1;
    }
    READABLE_FUNCTION_WAITS(placeAttached, Verified);

    // ---- the attachments and the module's end (all Draft: no recorded
    // play reaches them) ------------------------------------------------

    namespace
    {
        constexpr uint32_t kDestroyPool = sym::destroyPool;   // (pool): its records freed (the callback first), then it
        constexpr uint32_t kPoolFreeRecord = sym::poolFree; // poolFree, by address
        constexpr uint32_t kPoolForEach = sym::poolForEach;   // (pool, visit, data): visit(record, data) until it returns 0
        constexpr uint32_t kPoolCount = sym::poolCount;     // (pool) -> records in use
        constexpr uint32_t kPoolMemory = sym::poolRecordBytes;    // (pool) -> bytes allocated (Pool +0x28)
        // Each set-up's shutdown, in the order the module calls them (the
        // set-ups' reverse): the emitters', the effects', the kinds' after
        // the models (initEmitterObjects, initCameraObjects, initLightObjects's) and the models'.
        constexpr ee::FunctionPtr<int32_t()> kObjectKindShutdowns[] = {sym::shutdownParticleEmitterObjects, sym::shutdownParticleTypes, sym::shutdownEmitterObjects, sym::shutdownSceneRecords, sym::shutdownLightObjects,
                                                     sym::shutdownModelObjects};
        // ZGE2's: no particle emitter objects or particle types (ZGE3's additions) to shut down.
        constexpr ee::FunctionPtr<int32_t()> kZge2ObjectKindShutdowns[] = {sym::shutdownEmitterObjects, sym::shutdownSceneRecords, sym::shutdownLightObjects, sym::shutdownModelObjects};
        // This part's functions as the originals call them, by address.
        constexpr uint32_t kNewAttachment = sym::newAttachment;
        constexpr uint32_t kNextAttachedSibling = sym::nextAttachedSibling;
        constexpr uint32_t kFirstAttachedChild = sym::firstAttachedChild;
        constexpr uint32_t kAttachedParent = sym::attachedParent;
        constexpr uint32_t kObjectDebugReport = sym::objectDebugReport;
        // releaseAttachment's result without a record: the high half of the
        // pool global's address.
        constexpr int32_t kAttachmentPoolHigh = ee::high(sym::g_sceneObjectsReady);

        // An object let go of its parent: its link cleared, its hooks on
        // again and sent its position and orientation.
        void clearParentLink(ee::Ptr<SceneObject> object)
        {
            object->flags &= ~kObjectNoHooks;
            ObjectAttachment &link = *object->attachment;
            link.parent = ee::Ptr<SceneObject>();
            link.bone = 0;
            link.nextSibling = ee::Ptr<SceneObject>();
            if (const auto moved = object->objectClass->moved)
                moved(object, &object->position);
            if (const auto rotated = object->objectClass->rotated)
                rotated(object, &object->orientation);
        }
    }

    // shutDownSceneObjects: the scene objects' module shut down: the update task
    // deleted, both pools destroyed, then each set-up's shutdown (the first
    // that fails: 0, the module left up). 1 when it is down.
    int32_t shutDownSceneObjects()
    {
        if (*g_sceneObjectsReady == 0)
            return 1;
        sceneObjectsHook();
        deleteTask(*g_staleObjectsTask);
        destroyPool(*g_sceneObjectPool);
        destroyPool(*g_attachmentPool);
        if constexpr (game::readable::kIsDm2)
        {
            for (const auto shutDown : kZge2ObjectKindShutdowns)
                if (shutDown() == 0)
                    return 0;
        }
        else
            for (const auto shutDown : kObjectKindShutdowns)
                if (shutDown() == 0)
                    return 0;
        *g_sceneObjectsReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownSceneObjects, Verified);

    // callUnusedHook: the class's unused hook called (nothing calls this); its
    // result, 0 without one.
    int32_t callUnusedHook(ee::Ptr<SceneObject> object)
    {
        const auto hook = object->objectClass->unusedHook;
        return hook ? hook(object) : 0;
    }
    READABLE_FUNCTION(callUnusedHook, Verified);

    // setObjectScale: the object's scale set (its class's scaled hook), its
    // pose marked stale.
    ee::Ptr<SceneObject> setObjectScale(ee::Ptr<SceneObject> object, uint32_t scale)
    {
        if (const auto scaled = object->objectClass->scaled)
            scaled(object, scale);
        return game::tail(zge::markPoseStale, object);
    }
    READABLE_FUNCTION(setObjectScale, Verified);

    // forEachObjectOfKind: visit(object, data) for every scene object of a kind. 1.
    uint32_t forEachObjectOfKind(uint32_t visitFunction, uint32_t data, uint32_t kind)
    {
        const ObjectVisitor visit(visitFunction);
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_sceneObjectPool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<SceneObject> object = poolRecordAs<SceneObject>(link);
            if ((object->flags & kObjectKindMask) == kind)
                visit(object, data);
        }
        return 1;
    }
    READABLE_FUNCTION(forEachObjectOfKind, Verified);

    // newAttachment: the object's attachment record made (cleared) if it has
    // none.
    void newAttachment(ee::Ptr<SceneObject> object)
    {
        if (!object->attachment)
        {
            const uint32_t made = poolAlloc(*g_attachmentPool);
            object->attachment = ee::Ptr<ObjectAttachment>(made);
            memoryFill(made, 0, sizeof(ObjectAttachment));
        }
    }
    READABLE_FUNCTION_WAITS(newAttachment, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t freeRecord(ee::Ptr<Pool> pool, ee::Ptr<ObjectAttachment> record) { return ee::game<int32_t>(kPoolFreeRecord, pool, record); }
    }

    // releaseAttachment: the object's attachment record handed back. Its
    // result is only what the original leaves: what the free left, or without
    // a record the high half of the module's globals.
    int32_t releaseAttachment(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ObjectAttachment> record = object->attachment;
        if (!record)
            return kAttachmentPoolHigh;
        const int32_t result = freeRecord(*g_attachmentPool, record);
        object->attachment = ee::Ptr<ObjectAttachment>();
        return result;
    }
    READABLE_FUNCTION(releaseAttachment, Verified);

    // attachObject: the object hung from parent (from its bone, 0: itself),
    // first of the parent's attached objects; the parent's pose marked stale
    // (a tail call). Nothing if it hangs from something already.
    void attachObject(ee::Ptr<SceneObject> object, ee::Ptr<SceneObject> parent, ee::Ptr<SceneBone> bone)
    {
        if (const ee::Ptr<ObjectAttachment> record = object->attachment; record)
        {
            if (record->parent)
                return;
        }
        else
            newAttachment(object);
        if (!parent->attachment)
            newAttachment(parent);
        object->flags |= kObjectNoHooks;
        object->attachment->parent = parent;
        object->attachment->bone = bone;
        object->attachment->nextSibling = parent->attachment->firstChild;
        parent->attachment->firstChild = object;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no markPoseStale step)
            game::tail(zge::markPoseStale, parent);
    }
    READABLE_FUNCTION_WAITS(attachObject, Verified);

    namespace
    {
        constexpr uint32_t kAttachLast = 0xFFFFFFFFu; // attachObjectAt's `after`

        // The end of a list of siblings (null for an empty one).
        ee::Ptr<SceneObject> lastSibling(ee::Ptr<SceneObject> sibling)
        {
            while (sibling && sibling->attachment->nextSibling)
                sibling = sibling->attachment->nextSibling;
            return sibling;
        }
        // The list's entry for `wanted`, null when it is not on it.
        ee::Ptr<SceneObject> findSibling(ee::Ptr<SceneObject> sibling, ee::Ptr<SceneObject> wanted)
        {
            while (sibling && sibling != wanted)
                sibling = sibling->attachment->nextSibling;
            return sibling;
        }
    }

    // attachObjectAt: attachObject with a place among the parent's attached
    // objects: after 0 first, -1 last, else right after that object (not
    // linked at all if the parent has no such one). The parent's pose
    // marked stale; hanging already: what it hangs from.
    ee::Ptr<SceneObject> attachObjectAt(ee::Ptr<SceneObject> object, ee::Ptr<SceneObject> parent, ee::Ptr<SceneBone> bone, uint32_t after)
    {
        if (const ee::Ptr<ObjectAttachment> record = object->attachment; record)
        {
            if (record->parent)
                return record->parent;
        }
        else
            newAttachment(object);
        if (!parent->attachment)
            newAttachment(parent);
        object->flags |= kObjectNoHooks;
        object->attachment->parent = parent;
        object->attachment->bone = bone;

        // The sibling the object goes after (null: first).
        const ee::Ptr<SceneObject> first = parent->attachment->firstChild;
        ee::Ptr<SceneObject> sibling;
        if (after == kAttachLast)
            sibling = lastSibling(first);
        else if (after != 0)
        {
            sibling = findSibling(first, ee::Ptr<SceneObject>(after));
            if (!sibling)
            {
                // ZGE2 marks no pose stale and answers the last word its search read: the attachment record of the
                // last sibling (of the parent when it has none). No caller reads it.
                if constexpr (game::readable::kIsDm2)
                    return ee::Ptr<SceneObject>(ee::Address(first ? lastSibling(first)->attachment : parent->attachment));
                return game::tail(zge::markPoseStale, parent);
            }
        }
        if (sibling)
        {
            object->attachment->nextSibling = sibling->attachment->nextSibling;
            sibling->attachment->nextSibling = object;
        }
        else
        {
            object->attachment->nextSibling = first;
            parent->attachment->firstChild = object;
        }
        // ZGE2 marks no pose stale: it answers the next sibling the object now has (what it linked), and an
        // object put last in an empty list its own attachment record (the last word it wrote through).
        if constexpr (game::readable::kIsDm2)
        {
            if (after == kAttachLast && !first)
                return ee::Ptr<SceneObject>(ee::Address(object->attachment));
            return object->attachment->nextSibling;
        }
        return game::tail(zge::markPoseStale, parent);
    }
    READABLE_FUNCTION_WAITS(attachObjectAt, Verified);

    ee::Ptr<SceneObject> attachedParent(ee::Ptr<SceneObject> object);

    ee::Ptr<SceneObject> firstAttachedChild(ee::Ptr<SceneObject> object);

    ee::Ptr<SceneObject> nextAttachedSibling(ee::Ptr<SceneObject> object);

    // detachObject: the object taken off what it hangs from (its hooks sent
    // its position and orientation again) and every object hanging from it
    // let go; its attachment record handed back.
    int32_t detachObject(ee::Ptr<SceneObject> object)
    {
        if (!object->attachment)
            return 0;
        if (const ee::Ptr<SceneObject> parent(attachedParent(object)); parent)
        {
            const ee::Ptr<ObjectAttachment> siblings = parent->attachment;
            ee::Ptr<SceneObject> sibling = siblings->firstChild;
            if (sibling == object)
            {
                siblings->firstChild = object->attachment->nextSibling;
                clearParentLink(object);
                return game::tail(releaseAttachment, object);
            }
            if (sibling)
            {
                ee::Ptr<ObjectAttachment> siblingLink = sibling->attachment;
                for (;;)
                {
                    sibling = siblingLink->nextSibling;
                    if (sibling == object)
                    {
                        siblingLink->nextSibling = object->attachment->nextSibling;
                        clearParentLink(object);
                        const int32_t result = releaseAttachment(object);
                        return result;
                    }
                    if (!sibling)
                        break;
                    siblingLink = sibling->attachment;
                }
            }
        }
        // The objects hanging from it let go.
        ee::Ptr<SceneObject> child = firstAttachedChild(object);
        if (!child)
            return 0;
        while (child)
        {
            const ee::Ptr<SceneObject> next = nextAttachedSibling(child);
            ObjectAttachment &link = *child->attachment;
            link.parent = ee::Ptr<SceneObject>();
            link.bone = 0;
            link.nextSibling = ee::Ptr<SceneObject>();
            child = next;
        }
        object->attachment->firstChild = ee::Ptr<SceneObject>();
        const int32_t result = releaseAttachment(object);
        return result;
    }
    READABLE_FUNCTION(detachObject, Verified);

    // nextAttachedSibling: the next object hanging from the same parent, or 0.
    ee::Ptr<SceneObject> nextAttachedSibling(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ObjectAttachment> record = object->attachment;
        return record ? record->nextSibling : ee::Ptr<SceneObject>();
    }
    READABLE_FUNCTION(nextAttachedSibling, Verified);

    // firstAttachedChild: the first object hanging from it, or 0.
    ee::Ptr<SceneObject> firstAttachedChild(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ObjectAttachment> record = object->attachment;
        return record ? record->firstChild : ee::Ptr<SceneObject>();
    }
    READABLE_FUNCTION(firstAttachedChild, Verified);

    // attachedParent: what it hangs from, or 0.
    ee::Ptr<SceneObject> attachedParent(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ObjectAttachment> record = object->attachment;
        return record ? record->parent : ee::Ptr<SceneObject>();
    }
    READABLE_FUNCTION(attachedParent, Verified);

    // forEachAttached: visit(attached, data) for each object hanging from it. 1.
    uint32_t forEachAttached(ee::Ptr<SceneObject> object, uint32_t visitFunction, ee::Address data)
    {
        const ObjectVisitor visit(visitFunction);
        for (ee::Ptr<SceneObject> child = firstAttachedChild(object); child; child = nextAttachedSibling(child))
            visit(child, data);
        return 1;
    }
    READABLE_FUNCTION(forEachAttached, Verified);

    // placeOnParent (placeAttached's work): an attached object placed by its
    // parent's matrix (made its bone's world matrix first when it follows a
    // bone): its position transformed as a point, its orientation the
    // matrix's rotation times its own, both sent to its class's moved and
    // rotated hooks (the object's own position and orientation stay what they
    // were: they are relative to the parent).
    void placeOnParent(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> matrix)
    {
        ee::Local<Vec4> position;
        ee::Local<Matrix> rotation;
        ee::Local<Matrix> turned;
        ee::Local<Quaternion> orientation;
        const ObjectAttachment &link = *object->attachment;
        if (link.bone)
            objectBoneMatrix(link.parent, link.bone, matrix, kBoneInWorld);
        // ZGE2 transforms the position through its own transformPoint; ZGE3 inlines the product.
        if constexpr (game::readable::kIsDm2)
            transformPoint(position, matrix, &object->position);
        else
            *position = transformed(*matrix, object->position);
        matrixClearTranslation(&*rotation);
        quaternionToMatrix(&*rotation, &object->orientation);
        matrixMultiply(turned, matrix, rotation);
        matrixToQuaternion(&*orientation, &*turned);
        if (hasObjectClass(object))
            if (const auto moved = object->objectClass->moved)
                moved(object, position);
        if (hasObjectClass(object))
            if (const auto rotated = object->objectClass->rotated)
                rotated(object, orientation);
    }
    READABLE_FUNCTION(placeOnParent, Verified);

    // objectDebugReport (the debug report's visit): the class's debugReport
    // hook. 1.
    uint32_t objectDebugReport(ee::Ptr<SceneObject> object)
    {
        if (const auto hook = object->objectClass->debugReport)
            hook(object);
        return 1;
    }
    READABLE_FUNCTION(objectDebugReport, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t poolForEach(ee::Ptr<Pool> pool, uint32_t visit, uint32_t data) { return ee::tailCall<int32_t>(kPoolForEach, pool, visit, data); }
        }
    }

    // reportSceneObjects: the pools' debug report ("ZOBJ: MemAllocated:%d,
    // Objects in use:%d" in the GC and Xbox builds; the counts unused
    // here), then every object's report (a tail call).
    int32_t reportSceneObjects()
    {
        poolCount(*g_sceneObjectPool);
        poolRecordBytes(*g_sceneObjectPool);
        poolCount(*g_attachmentPool);
        poolRecordBytes(*g_attachmentPool);
        return tail::poolForEach(*g_sceneObjectPool, kObjectDebugReport, 0);
    }
    READABLE_FUNCTION_WAITS(reportSceneObjects, Verified);

    // reportSceneObjectPool (nothing calls it in AI): "ZObj: Used:%d" with
    // the scene objects in use, at the debug text's (0, -1). 1.
    uint32_t reportSceneObjectPool()
    {
        debugPrint(0, -1, sym::g_textZObjUsed, poolCount(*g_sceneObjectPool));
        return 1;
    }
    READABLE_FUNCTION(reportSceneObjectPool, Draft);
}
