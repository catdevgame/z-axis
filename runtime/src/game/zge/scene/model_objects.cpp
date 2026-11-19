// Model objects (zobjModel.c, setUpAnimatedModel ..): the scene objects of kind 0,
// each showing a scene (a loaded model's bone tree, zge/scene/scene.h).
// Two classes (SceneObjectClass): the animated one (animatedModelClass: a model with
// more than one bone, or asked for) gets an animation of its own, the rigid
// one (rigidModelClass) not; their other hooks hand the object's changes on to its
// scene. Most of the module is that: a word of the object forwarded to the
// scene function that does the work.
//

#include "scene.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/tasks.h"
#include "zge/collision/collision.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene_database.h"

#include <cstdint>
#include "sym/zge/scene/model_objects.cpp.h"

namespace zge
{

    namespace
    {
        // The two classes.
        constexpr uint32_t kAnimatedModelClass = sym::animatedModelClass;
        constexpr uint32_t kRigidModelClass = sym::rigidModelClass;
        constexpr uint32_t kModelKind = kObjectModelKind; // SceneObject::flags' kind (ZGE4's 1)

        // Set once initModelObjects registered the animation chunks.
        inline ee::Global<int32_t, sym::g_modelObjectsReady> g_modelObjectsReady;
        // Set while the models are hidden (a debug switch, setModelsHiddenTask,
        // hides every model while it is on) [inferred: what turns it on].
        inline ee::Global<int32_t, sym::g_modelsHidden> g_modelsHidden;

        // The scene objects' module (scene_objects.cpp), by address as the
        // originals call them.

        // Scenes (the zglTerrainRender module's scene functions, countModelNodes ..).

        // Animation (zge/scene/animation.cpp's module, sampleRotationKey ..).

        // (file name, context, tag): the file's chunks read with the tag's
        // handlers, which leave their result in the context.
        constexpr uint32_t kModelFileTag = 0x4C444D46u; // "FMDL"

        // The animated class's set-up and the rigid class's (below), and the
        // hide the debug switch applies (hideModelObject: setModelShown(object, 0)).
        constexpr uint32_t kSetUpRigidModel = sym::setUpRigidModel;
    }

    // setUpAnimatedModel: the animated class's set-up: the object's animator made.
    // 1.
    uint32_t setUpAnimatedModel(ee::Ptr<SceneObject> object)
    {
        createObjectAnimator(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setUpAnimatedModel, Verified);

    // destroyAnimatedModel (the animated class's destroy): its animator freed, then
    // its scene released. 1.
    uint32_t destroyAnimatedModel(ee::Ptr<SceneObject> object)
    {
        freeObjectAnimator(object);
        if (const ee::Ptr<Scene> scene = object->scene)
            releaseScene(scene);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyAnimatedModel, Verified);

    // modelUpdatePose (the animated class's update): the pose brought up to
    // date. 1.
    uint32_t modelUpdatePose(ee::Ptr<SceneObject> object)
    {
        updatePose(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(modelUpdatePose, Verified);

    // setModelPosition (both classes' moved): a model object's position handed
    // on to its scene; 0 if it has none.
    int32_t setModelPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        const ee::Ptr<Scene> scene = object->scene;
        return scene ? setScenePosition(scene, position) : 0;
    }
    READABLE_FUNCTION(setModelPosition, Verified);

    // setModelOrientation (both classes' rotated): the orientation handed on to the
    // scene; 0 if it has none.
    int32_t setModelOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        if (const ee::Ptr<Scene> scene = object->scene)
        {
            const int32_t set = setSceneOrientation(scene, orientation);
            return set;
        }
        return 0;
    }
    READABLE_FUNCTION(setModelOrientation, Verified);

    // setModelScale (both classes' scaled): the scale handed on to the scene,
    // which returns 1. Unlike the position and the orientation, the scene is
    // not checked first.
    uint32_t setModelScale(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> scale)
    {
        return setSceneScale(object->scene, scale);
    }
    READABLE_FUNCTION(setModelScale, Verified);

    // setModelShown (both classes' shown): the scene's nodes shown or hidden.
    // 1.
    uint32_t setModelShown(ee::Ptr<SceneObject> object, uint32_t shown)
    {
        if (shown)
        {
            changeSceneState(object->scene, 0, kSceneShown);
            return 1;
        }
        changeSceneState(object->scene, kSceneShown, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setModelShown, Verified);

    // modelId (both classes' id): the scene's id.
    uint32_t modelId(ee::Ptr<SceneObject> object)
    {
        return boneId(object->scene, 0);
    }
    READABLE_FUNCTION(modelId, Verified);

    // setUpRigidModel: the rigid class's set-up: nothing to make. 1.
    uint32_t setUpRigidModel()
    {
        return 1;
    }
    READABLE_FUNCTION(setUpRigidModel, Verified);

    // rigidModelUpdatePose (the rigid class's update): as modelUpdatePose. 1.
    uint32_t rigidModelUpdatePose(ee::Ptr<SceneObject> object)
    {
        updatePose(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(rigidModelUpdatePose, Verified);

    // destroyRigidModel (the rigid class's destroy): its scene released (ZGE4 frees its animator first). 1.
    uint32_t destroyRigidModel(ee::Ptr<SceneObject> object)
    {
        if constexpr (game::readable::kIsBmx)
            freeObjectAnimator(object);
        if (const ee::Ptr<Scene> scene = object->scene)
            releaseScene(scene);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyRigidModel, Verified);

    // initModelObjects: the module's set-up (initSceneObjects' first object
    // kind): the animation chunks' handlers registered. 1 when done.
    int32_t initModelObjects()
    {
        if (!registerAnimationChunks())
            return 0;
        *g_modelObjectsReady = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initModelObjects, Verified);

    uint32_t hideModelObject(ee::Ptr<SceneObject> sceneObject); // (below)

    // newModelObject: a model object made to show `scene`: animated when it
    // has more than one bone or bit 0 of `flags` asks for it, else rigid; the
    // scene told its object; hidden at once while the debug switch is on.
    // The object, or 0 (no scene, or the pool gave none).
    ee::Ptr<SceneObject> newModelObject(ee::Ptr<Scene> scene, uint32_t flags)
    {
        if (!scene)
            return {};
        ee::Ptr<SceneObject> object;
        if constexpr (game::readable::kIsBmx) // ZGE4's initSceneObject takes the kind alone and makes the object
        {
            object = newObjectOfKind(kModelKind);
            if (!object)
                return {};
        }
        else
        {
            object = newSceneObject();
            if (!object)
                return {};
            initSceneObject(object, kModelKind);
        }
        object->scene = scene;
        const uint32_t bones = sceneBoneCount(scene);
        const uint32_t animated = flags & 1u;
        if (animated || bones > 1u)
        {
            object->objectClass = ee::Ptr<SceneObjectClass>(kAnimatedModelClass);
            setUpAnimatedModel(object);
        }
        else
        {
            object->objectClass = ee::Ptr<SceneObjectClass>(kRigidModelClass);
            setUpRigidModel();
        }
        setSceneObject(object->scene, object);
        if (*g_modelsHidden)
            hideModelObject(object);
        return object;
    }
    READABLE_FUNCTION_WAITS(newModelObject, Verified);

    // loadModelFile: a model file read (its FMDL chunks); what the chunks'
    // handler made (the scene), or 0.
    uint32_t loadModelFile(ee::Address fileName)
    {
        ee::Local<uint32_t> loaded;
        *loaded = 0;
        readChunkFile(fileName, loaded, kModelFileTag);
        return *loaded;
    }
    READABLE_FUNCTION_WAITS(loadModelFile, Verified);

    // parseLibraryModelChunks: one model file's chunks; the model their handlers made.
    uint32_t parseLibraryModelChunks(ee::Ptr<ChunkReader> chunkReader)
    {
        ee::Local<uint32_t> context;
        *context = 0;
        parseChunks(chunkReader, context);
        return *context;
    }
    READABLE_FUNCTION_WAITS(parseLibraryModelChunks, Draft);

    // loadLibraryModelEntry: a model out of an asset library: its bytes read into
    // memory and parsed as a memory file ("FMDL" chunks).
    uint32_t loadLibraryModelEntry(uint32_t library, uint32_t id)
    {
        constexpr uint32_t kModelFileTag = 0x4C444D46u; // "FMDL"
        int64_t model = 0;
        if (findAsset(library, id) == 0)
            return 0;
        const uint32_t bytes = currentAssetLength(library);
        // ZGE4 allocates the read rounded up to the library's asset
        // alignment; the memory file still gets the asset's length.
        uint32_t allocated = bytes;
        if constexpr (game::readable::kIsBmx)
        {
            const uint32_t alignment = static_cast<uint32_t>(currentAssetAlignment(library));
            allocated = (bytes + alignment - 1) / alignment * alignment;
        }
        const uint32_t memory = allocateMemory(allocated);
        if (readAsset(library, memory, 0) != 0)
        {
            const ee::Ptr<ZFile> file = openMemoryFile(memory, bytes);
            const ee::Ptr<ChunkReader> chunks = openChunkFile(file, kModelFileTag, 0);
            if (chunks)
            {
                model = static_cast<int32_t>(parseLibraryModelChunks(chunks));
                closeChunkFile(chunks);
            }
            closeFile(file, 0);
        }
        freeMemory(memory);
        return static_cast<uint32_t>(model);
    }
    READABLE_FUNCTION_WAITS(loadLibraryModelEntry, Verified);

    // releaseSceneAssetsScene
    int32_t releaseSceneAssetsScene(ee::Ptr<Scene> scene)
    {
        return releaseScene(scene);
    }
    READABLE_FUNCTION_WAITS(releaseSceneAssetsScene, Draft);

    // cloneModelScene: a copy of a model's scene whose nodes keep their region
    // when they move (kNodeKeepsRegion; ZGE2's cloneScene takes the scene
    // alone and always clones so).
    ee::Ptr<Scene> cloneModelScene(ee::Ptr<Scene> scene)
    {
        return cloneScene(scene READABLE_PARAM_EXCEPT(dm2, kNodeKeepsRegion));
    }
    READABLE_FUNCTION_WAITS(cloneModelScene, Verified);

    // modelBoneCount: how many bones the model's scene has.
    uint32_t modelBoneCount(ee::Ptr<SceneObject> object)
    {
        return sceneBoneCount(object->scene);
    }
    READABLE_FUNCTION(modelBoneCount, Verified);

    // modelBoneIds: each of the model's bones' ids into ids[] (a tail call).
    void modelBoneIds(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> ids)
    {
        game::tail(sceneBoneIds, object->scene, ids);
    }
    READABLE_FUNCTION(modelBoneIds, Verified);

    // findModelBone: the model's bone with a name hash, 0 for none.
    ee::Ptr<SceneBone> findModelBone(ee::Ptr<SceneObject> object, uint32_t nameHash)
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4 asks through its thunk)
            return findSceneModelBoneThunk(object->scene, nameHash);
        else
            return findSceneModelBone(object->scene, nameHash);
    }
    READABLE_FUNCTION(findModelBone, Verified);

    // modelBoneBindPosition: a bone's place in its parent at rest into `out` (a tail
    // call).
    void modelBoneBindPosition(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<Vec4> out)
    {
        game::tail(boneBindPosition, object->scene, bone, out);
    }
    READABLE_FUNCTION(modelBoneBindPosition, Verified);

    // findAnimatorBone: the object's animation channel for a bone's name hash,
    // 0 for none.
    ee::Ptr<AnimationChannel> findAnimatorBone(ee::Ptr<SceneObject> object, uint32_t nameHash)
    {
        return findBoneChannel(object->animator, nameHash);
    }
    READABLE_FUNCTION(findAnimatorBone, Verified);

    // modelBoneParent: the parent of one of the model's bones (nothing calls
    // it in AI).
    ee::Ptr<SceneBone> modelBoneParent(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone)
    {
        // ZGE4's answers the scene's root bone whatever the bone asked about (its code calls rootBone where ZGE3's
        // calls boneParent, the function next to it).
        if constexpr (game::readable::kIsBmx)
            return rootBone(object->scene);
        return boneParent(object->scene, bone);
    }
    READABLE_FUNCTION(modelBoneParent, Draft);

    // modelHasAnimator: 1 when the object has an animator of its own (an
    // animated model's, setModelAnimator), else 0.
    uint32_t modelHasAnimator(ee::Ptr<SceneObject> object)
    {
        return object->animator ? 1 : 0;
    }
    READABLE_FUNCTION(modelHasAnimator, Verified);

    // setModelAnimator: the object given an animator, its scene's bones bound to
    // its channels (a tail call).
    void setModelAnimator(ee::Ptr<SceneObject> object, ee::Ptr<Animator> animator)
    {
        const ee::Ptr<Scene> scene = object->scene;
        object->animator = animator;
        game::tail(bindSceneAnimator, scene, animator);
    }
    READABLE_FUNCTION(setModelAnimator, Verified);

    namespace
    {
        constexpr uint32_t kTextNgengineZge3ZgeModsZobjModel = sym::g_textNgengineZge3ZgeModsZobjModel;
        inline ee::Global<ee::Ptr<Task>, sym::g_modelsHiddenTask> g_modelsHiddenTask;
    }

    uint32_t modelUnusedHook()
    {
        return 0;
    }
    READABLE_FUNCTION(modelUnusedHook, Verified);

    // modelDebugReport (both classes' debugReport, reportSceneObjects): the
    // scene's own name (no bone). It is returned, not printed: the shipped
    // report prints nothing for a model.
    ee::Ptr<char> modelDebugReport(ee::Ptr<SceneObject> object)
    {
        return boneName(object->scene, ee::Ptr<SceneBone>());
    }
    READABLE_FUNCTION(modelDebugReport, Verified);

    // ZobjModel: returns 1 (forEachObjectOfKind's callback)
    uint32_t modelObjectVisitTrue()
    {
        return 1;
    }
    READABLE_FUNCTION(modelObjectVisitTrue, Draft);

    // ZobjModel: forEachObjectOfKind with modelObjectVisitTrue.
    uint32_t visitModelObjects()
    {
        zge::forEachObjectOfKind(sym::modelObjectVisitTrue, 0, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(visitModelObjects, Draft);

    uint32_t hideModelObject(ee::Ptr<SceneObject> sceneObject)
    {
        zge::setModelShown(sceneObject, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(hideModelObject, Verified);

    uint32_t showModelObject(ee::Ptr<SceneObject> sceneObject)
    {
        zge::setModelShown(sceneObject, 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(showModelObject, Draft);

    uint32_t shutdownModelObjects()
    {
        if (*g_modelObjectsReady != 0)
        {
            const int32_t unregisterAnimationChunksResult = zge::unregisterAnimationChunks();
            if (unregisterAnimationChunksResult == 0)
            {
                return 0;
            }
            *g_modelObjectsReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownModelObjects, Verified);

    // (nameHash, ..): a model object (newModelObject) for the pose (findModelPose) and its world node
    // (findWorldNodeByHash, setupNodeScen.
    ee::Ptr<SceneObject> newModelObjectFromPose(uint32_t nameHash, int32_t poseName)
    {
        const ee::Ptr<Scene> modelPose = findModelPose(nameHash, poseName);
        if (modelPose)
        {
            const ee::Ptr<SceneObject> shown = zge::sceneObject(modelPose);
            if (shown)
                return shown;
            return newModelObject(modelPose, 0);
        }
        const ee::Ptr<Node> worldNode = zge::findWorldNodeByHash(nameHash, poseName, 0);
        if (!worldNode)
            return {};
        const ee::Ptr<Scene> scene = setupNodeSceneModel(worldNode);
        if (!scene)
            return {};
        return newModelObject(scene, 0);
    }
    READABLE_FUNCTION_WAITS(newModelObjectFromPose, Draft);

    // nodeModelObject (ZGE4 only; ZGE2 and ZGE3's createEntityObject makes a new
    // model object every time): the node's model object. An object whose scene
    // is rooted at the node already is answered as it is; else the library pose
    // of the node's name rooted at it gives its object (made when it has none);
    // else the node's own model becomes a new model object, placed where the
    // node was in the world unless it is mirrored (the node goes to the
    // identity first). With registerAsset, a pose's or a new object is
    // registered as a loaded model, released when the scene is left. 0 when the
    // node has no model or no object could be made (its scene released).
    inline ee::Ptr<SceneObject> nodeModelObject(ee::Ptr<Node> node, uint32_t registerAsset)
    {
        ee::Local<Matrix> placement;
        const uint32_t id = node->nameHash;
        for (uint32_t object = findSceneObject(id, 0, 0); object; object = findSceneObject(id, 0, object))
            if (sceneRootNode(sceneOf(ee::Ptr<SceneObject>(object))) == node)
                return ee::Ptr<SceneObject>(object);
        int32_t skip = 0;
        for (ee::Ptr<Scene> pose = findModelPose(id, skip); pose; pose = findModelPose(id, skip))
        {
            if (sceneRootNode(pose) != node)
            {
                ++skip; // another node's pose of the same name
                continue;
            }
            ee::Ptr<SceneObject> object = sceneObject(pose);
            if (!object)
                object = newModelObject(pose, 0);
            if (registerAsset)
                registerLoadedAsset(0, id, kAssetModel, 0, object READABLE_PARAM_EXCEPT(dm2, 1));
            return object;
        }
        *placement = node->world;
        setNodeLocalMatrix(node, ee::Ptr<const Matrix>(sym::identityMatrix));
        const ee::Ptr<Scene> scene = setupNodeSceneModel(node);
        if (!scene)
            return {};
        const ee::Ptr<SceneObject> object = newModelObject(scene, 0);
        if (!object)
        {
            releaseScene(scene);
            return {};
        }
        if ((node->flags & kNodeMirrored) == 0)
        {
            setObjectRotationMatrix(object, placement);
            setObjectPosition(object, &placement->translation());
        }
        if (registerAsset)
            registerLoadedAsset(0, node->nameHash, kAssetModel, 0, object READABLE_PARAM_EXCEPT(dm2, 1));
        return object;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, nodeModelObject);

    // namedModelObject (ZGE4 only): nodeModelObject by a name's hash: the first object of that name as it is; else
    // the first library pose of that name gives its object (made when it has none); else the world's node of that
    // name becomes a new model object as nodeModelObject makes one. 0 when nothing has the name or no object could
    // be made.
    inline ee::Ptr<SceneObject> namedModelObject(uint32_t nameHash, uint32_t registerAsset)
    {
        ee::Local<Matrix> placement;
        ee::Ptr<SceneObject> object(findSceneObject(nameHash, 0, 0));
        if (object)
            return object;
        if (const ee::Ptr<Scene> pose = findModelPose(nameHash, 0))
        {
            object = sceneObject(pose);
            if (!object)
                object = newModelObject(pose, 0);
        }
        else
        {
            const ee::Ptr<Node> node = findWorldNodeByHash(nameHash, 0, 0);
            if (!node)
                return {};
            *placement = node->world;
            setNodeLocalMatrix(node, ee::Ptr<const Matrix>(sym::identityMatrix));
            const ee::Ptr<Scene> scene = setupNodeSceneModel(node);
            if (!scene)
                return {};
            object = newModelObject(scene, 0);
            if (!object)
            {
                releaseScene(scene);
                return {};
            }
            if ((node->flags & kNodeMirrored) == 0)
            {
                setObjectRotationMatrix(object, placement);
                setObjectPosition(object, &placement->translation());
            }
        }
        if (registerAsset)
            registerLoadedAsset(0, nameHash, kAssetModel, 0, object READABLE_PARAM_EXCEPT(dm2, 1));
        return object;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, namedModelObject);

    // ZobjModel: returns 0.
    uint32_t modelStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(modelStubZero, Draft);

    // ZobjModel ("../ngengine/ZGE3/ZgeMods/zobjModel.c"): (hidden): the task that hides every model object
    // made or deleted (g_modelsHidden)
    int32_t setModelsHiddenTask(uint32_t enabled)
    {
        int32_t result = 0;
        if (enabled != 0)
        {
            result = *g_modelsHidden;
            if (result != 0)
                return result;
            const ee::Ptr<Task> task = newTask(257, sym::visitModelObjects, 0, 1, kTextNgengineZge3ZgeModsZobjModel, game::readable::perGame(875u, 833u)); // zobjModel.c's line
            *g_modelsHiddenTask = task;
            result = zge::forEachObjectOfKind(sym::hideModelObject, 0, 0);
            *g_modelsHidden = 1;
            return result;
        }
        result = *g_modelsHidden;
        if (result != 0)
        {
            deleteTask(*g_modelsHiddenTask);
            result = zge::forEachObjectOfKind(sym::hideModelObject, 0, 0);
            *g_modelsHidden = 0;
            return result;
        }
        return result;
        return result;
    }
    READABLE_FUNCTION_WAITS(setModelsHiddenTask, Draft);
}
