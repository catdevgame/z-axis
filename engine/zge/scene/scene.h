#pragma once

// An object's scene (the object's +0x38, kSceneOf sceneOf): its model's
// bone tree and where the object puts it. Two kinds (flag kSceneSkinned):
// a rigid model has a scene node per bone, whose local and world matrices
// are the bones' (SceneBone::matrices points at the node's +0x10); a
// skinned model has one node for the whole model (Scene::node) and the
// bones' matrices on their own, in the model's space, read by the skinned
// renderer (zge/scene/skeleton.h's poses). The pose update is
// zge/scene/pose.cpp. Offsets are the ones the pose code uses; BMX XXX has
// the same layout four bytes further on from +0x44.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // A bone's key for this frame (the animation's output): its place
    // relative to its parent bone.
    struct BoneKey
    {
        Vec4 translation;    // +0x00 added to the bone's bind position
        Vec4 scale;          // +0x10 (x, y, z)
        Quaternion rotation; // +0x20
    };
    static_assert(offsetof(BoneKey, scale) == 0x10 && offsetof(BoneKey, rotation) == 0x20);

    // A bone's two matrices, laid out as a node's (Node::local, Node::world).
    struct BoneMatrices
    {
        Matrix local; // +0x00 relative to the parent bone
        Matrix model; // +0x40 the parent's model matrix times local (a rigid model's: the node's world)
    };
    static_assert(sizeof(BoneMatrices) == 0x80);

    struct SceneBone
    {
        ee::Ptr<Node> node;                 // +0x00 a rigid model's node of the bone (the first bone's: the model's top)
        uint32_t id;                        // +0x04 its name's hash: the animation's boneId
        ee::Ptr<char> name;                 // +0x08 a rigid model's: its node's name; a skinned one's: Scene::boneNames
        ee::Ptr<BoneKey> key;               // +0x0C 0: the bone's local matrix is left as it is
        ee::Ptr<BoneMatrices> matrices;     // +0x10
        uint8_t _14[0x0C];
        Vec4 bindPosition;                  // +0x20 the bone's place in its parent at rest
        ee::Ptr<SceneBone> firstChild;      // +0x30
        ee::Ptr<SceneBone> nextSibling;     // +0x34
        ee::Ptr<SceneBone> parent;          // +0x38 0 for the first bone
        uint32_t _3c;
    };
    static_assert(sizeof(SceneBone) == 0x40);
    static_assert(offsetof(SceneBone, id) == 0x04 && offsetof(SceneBone, name) == 0x08);
    static_assert(offsetof(SceneBone, key) == 0x0C && offsetof(SceneBone, matrices) == 0x10);
    static_assert(offsetof(SceneBone, bindPosition) == 0x20 && offsetof(SceneBone, parent) == 0x38);

    // A MOBO chunk (a skinned model's skeleton, readMoboChunk): this header,
    // then a record a bone.
    struct MoboHeader
    {
        uint16_t boneCount; // +0x00
        uint16_t _02;
        uint32_t id;        // +0x04 the scene's id
    };
    static_assert(sizeof(MoboHeader) == 0x08);

    // A bone's record in a MOBO chunk. DM2's names the parent by its place
    // and has no parent id.
    struct MoboBone
    {
        uint16_t _00;
#if ZX_GAME_DM2
        uint16_t parentNumber; // +0x02 its parent's place among the bones, from 1; 0: a top bone
#else
        uint16_t hasParent;  // +0x02 0: a top bone
#endif
        ee::Float x, y, z;   // +0x04 its bind position
        uint32_t id;         // +0x10 its name's hash
        char name[0x20];     // +0x14
#if !ZX_GAME_DM2
        uint32_t parentId;   // +0x34
        // Its parent among the scene's bones, the record being bone `place`
        // (0: a top bone): the first earlier bone with its parent's id (none
        // found: the bone itself, readMoboChunk's kept bug).
        ee::Ptr<SceneBone> parentAmong(ee::Ptr<SceneBone> bones, int32_t place) const
        {
            if (!hasParent)
                return ee::Ptr<SceneBone>();
            int32_t j = 0;
            while (j < place && bones[j].id != parentId)
                ++j;
            return bones + j;
        }
#else
        // Its parent among the scene's bones (0: a top bone): the bone at its
        // parent's place.
        ee::Ptr<SceneBone> parentAmong(ee::Ptr<SceneBone> bones, int32_t) const
        {
            if (!parentNumber)
                return ee::Ptr<SceneBone>();
            return bones + (parentNumber - 1);
        }
#endif
    };
    static_assert(offsetof(MoboBone, id) == 0x10 && sizeof(MoboBone) == game::readable::perGame(0x38u, 0x34u));

    // What the animation changed since the pose was last built: the
    // object's Animator (zge/scene/animation.h), its +0x04 word.
    struct PoseChanges
    {
        uint32_t _00;
        // +0x04 bits 0-2 and 16-31: changes the bones' local matrices must
        // be rebuilt for (cleared when they are); bits 3-15 kept.
        uint32_t bits;
    };
    constexpr uint32_t kPoseChangesKept = 0xFFF8u;
    constexpr uint32_t kPoseChangesPending = 0xFFFF0007u;

    // Scene::flags (a 64-bit word).
    constexpr uint64_t kSceneScaled = 1ull << 32;       // Scene::scale applies
    constexpr uint64_t kSceneKeysChanged = 1ull << 33;  // the bones' keys changed: their local matrices are rebuilt
    constexpr uint64_t kSceneSkinned = 1ull << 34;      // one node (Scene::node) for the whole model
    constexpr uint64_t kSceneCloned = 1ull << 35;       // made by cloneScene: its node tree is its own, released with it
    // ZGE4 only: set by copyScenePose on the scene the pose went into; placeScene then keeps a rigid model's
    // unkeyed top bone's local matrix (the copied one) and clears it.
    constexpr uint64_t kScenePoseCopied = 1ull << 36;

    // A scene's states (changeSceneState switches them over its node tree).
    enum SceneStates : uint32_t
    {
        kSceneShown = 1u << 0,       // its nodes shown (setNodeShown)
        kSceneCastsShadow = 1u << 2, // its nodes cast a shadow (setNodeCastsShadow)
        kSceneOwnLights = 1u << 3,   // its top node lit by its own lights (kNodeOwnLights)
    };

    // A skinned scene's bone names: this many bytes each (Scene::boneNames).
    constexpr uint32_t kBoneNameBytes = 0x20;

    struct BonePose; // zge/scene/skeleton.h

    struct SceneObject;

    // A scene is an engine object: its first byte is its type (1; ZGE2's
    // scenes have none: newModelPose writes 0), set to 0xFF when it is freed
    // (freeModelPose).
    constexpr uint8_t kSceneType = game::readable::perGame(1u, 0u, 1u);
    constexpr uint8_t kFreedSceneType = 0xFF;

    struct Scene
    {
        union
        {
            uint64_t flags;             // +0x00
            uint8_t type;               // +0x00 kSceneType
        };
        ee::Ptr<SceneObject> object;    // +0x08 the model object made for it (newModelObject)
        uint32_t _0c;
        Vec4 position;                  // +0x10 where the object puts the model
        Vec4 scale;                     // +0x20 (x, y, z), with kSceneScaled
        Quaternion orientation;         // +0x30
#if ZX_GAME_BMX
        // BMX +0x40: ZGE4 keeps how many bones the scene has room for apart from how many it
        // uses (newModelPose's second argument, 0x20 for a MOBO's; copyBoneList copies it and
        // initPoseBones sets up that many), so its bone list is three words. Every later field
        // is four bytes further on than ZGE3's.
        int32_t boneCapacity;
#endif
        int32_t boneCount;              // +0x40 (BMX +0x44)
        ee::Ptr<SceneBone> bones;       // +0x44 (BMX +0x48) the first is the tree's top
        uint32_t id;                    // +0x48 (BMX +0x4C) its name's hash: the node's (setupSceneModel) or the MOBO's
        ee::Ptr<char> name;             // +0x4C (BMX +0x50) its node's name (0 for a MOBO's)
        // +0x50 (BMX +0x54) how many bone lists start at +0x40 (a count, then the bones;
        // the next would sit where id and name are): 1 for a scene made from
        // a node tree, 0 for a MOBO's. bindSceneAnimator binds their bones.
        int32_t boneListCount;
#if !ZX_GAME_DM2
        // +0x54 (BMX +0x58) the animator's changes it follows, 0 if none (DM2's scenes
        // keep no such word: its bindSceneAnimator stores none, and the rest
        // are one word lower there).
        ee::Ptr<PoseChanges> changes;
#endif
        // +0x58 a skinned model's bone matrices, 0xC0 bytes each (its node's
        // Node::bonePoses), and +0x5C its bones' names, 0x20 bytes each
        // (DM2 +0x54 and +0x58; BMX +0x5C and +0x60).
        ee::Ptr<BonePose> bonePoses;
        ee::Ptr<char> boneNames;
        ee::Ptr<Node> node;             // +0x60 (DM2 +0x5C, BMX +0x64) a skinned model's node
    };
    static_assert(offsetof(Scene, object) == 0x08 && offsetof(Scene, id) == game::readable::perGame(0x48u, 0x48u, 0x4Cu) &&
                  offsetof(Scene, name) == game::readable::perGame(0x4Cu, 0x4Cu, 0x50u));

    // A scene's bone list (boneList in pose.cpp): Scene::boneCount and Scene::bones as one record, boneListCount of
    // them from there; ZGE4's has the capacity first (Scene::boneCapacity).
    struct SceneBoneList
    {
#if ZX_GAME_BMX
        int32_t capacity;
#endif
        int32_t count;
        ee::Ptr<SceneBone> bones;
        // How many bones initPoseBones sets up: every bone allocated (ZGE4 keeps the capacity; ZGE2 and
        // ZGE3 allocate the count).
        int32_t allocated() const
        {
#if ZX_GAME_BMX
            return capacity;
#else
            return count;
#endif
        }
        // The counts another list has (copyBoneList): ZGE4 copies the capacity too.
        void takeCountsOf(const SceneBoneList &from)
        {
#if ZX_GAME_BMX
            capacity = from.capacity;
#endif
            count = from.count;
        }
    };
    static_assert(sizeof(SceneBoneList) == game::readable::perGame(8u, 8u, 12u));
    // A bone's matrices (SceneBone::matrices): a rigid bone's are its
    // node's local and world matrices, a skinned bone's the first two of its
    // pose.
    inline ee::Ptr<BoneMatrices> rigidBoneMatrices(ee::Ptr<Node> node) { return ee::Ptr<Matrix>(&node->local).cast<BoneMatrices>(); }
    inline ee::Ptr<BoneMatrices> skinnedBoneMatrices(ee::Ptr<BonePose> pose) { return pose.cast<BoneMatrices>(); }

    // An animator's change bits (Scene::changes), and its channels: a
    // channel starts with the key it gives its bone.
    struct Animator;
    struct AnimationChannel;
    inline ee::Ptr<PoseChanges> animatorChanges(ee::Ptr<Animator> animator) { return animator.cast<PoseChanges>(); }
    inline ee::Ptr<BoneKey> channelKey(ee::Ptr<AnimationChannel> channel) { return channel.cast<BoneKey>(); }

    static_assert(offsetof(Scene, boneListCount) == game::readable::perGame(0x50u, 0x50u, 0x54u));
    static_assert(offsetof(Scene, bonePoses) == game::readable::perGame(0x58u, 0x54u, 0x5Cu) &&
                  offsetof(Scene, boneNames) == game::readable::perGame(0x5Cu, 0x58u, 0x60u));
    // (newModelPose's record: 0x70 bytes in AI and BMX XXX, 0x60 in DM2)
    static_assert(sizeof(Scene) == game::readable::perGame(0x70u, 0x60u, 0x70u));
    static_assert(offsetof(Scene, position) == 0x10 && offsetof(Scene, scale) == 0x20);
    static_assert(offsetof(Scene, orientation) == 0x30 && offsetof(Scene, boneCount) == game::readable::perGame(0x40u, 0x40u, 0x44u));
    static_assert(offsetof(Scene, bones) == game::readable::perGame(0x44u, 0x44u, 0x48u) && offsetof(Scene, node) == game::readable::perGame(0x60u, 0x5Cu, 0x64u));
#if ZX_GAME_BMX
    static_assert(offsetof(Scene, boneCapacity) == 0x40);
    // The first word of the scene's bone lists (SceneBoneList): ZGE4's capacity, else the count.
    inline int32_t &boneListsStart(Scene &scene) { return scene.boneCapacity; }
    // How many bones the scene has room for (newModelPoseWithCapacity): ZGE4 keeps it apart from the count.
    inline int32_t &sceneBoneCapacity(Scene &scene) { return scene.boneCapacity; }
#else
    inline int32_t &boneListsStart(Scene &scene) { return scene.boneCount; }
    // (ZGE2 and ZGE3 allocate the count: the room is the count)
    inline int32_t &sceneBoneCapacity(Scene &scene) { return scene.boneCount; }
#endif

    // The animator's changes a scene follows, 0 if none, and setting them
    // (bindSceneAnimator). DM2's scenes keep none.
#if !ZX_GAME_DM2
    static_assert(offsetof(Scene, changes) == game::readable::perGame(0x54u, 0x54u, 0x58u));
    inline ee::Ptr<PoseChanges> sceneChanges(ee::Ptr<Scene> scene) { return scene->changes; }
    inline void setSceneChanges(ee::Ptr<Scene> scene, ee::Ptr<PoseChanges> changes) { scene->changes = changes; }
#else
    inline ee::Ptr<PoseChanges> sceneChanges(ee::Ptr<Scene>) { return ee::Ptr<PoseChanges>(); }
    inline void setSceneChanges(ee::Ptr<Scene>, ee::Ptr<PoseChanges>) {}
#endif

    // A skinned model's node: where the model's pivot (the first bone's
    // place less its bind position, turned by the object's matrix) is kept
    // [inferred: who reads it].
    constexpr uint32_t kNodePivot = 0x110u;

    // The game object that owns a scene (zobjModel's objects): only the
    // words the pose code touches.
    struct Animator; // zge/scene/animation.h
    // A scene object's class: its hooks (game functions, 0: none), called
    // with the object first. The model objects' two classes (animatedModelClass
    // animated, rigidModelClass rigid; zge/scene/model_objects.cpp) fill every one.
    struct SceneObject;
    struct SceneObjectClass
    {
        using ObjectHook = ee::FunctionPtr<void(ee::Ptr<SceneObject> object)>;
        ObjectHook destroy; // +0x00 the object is being freed (deleteSceneObject)
        ObjectHook update;  // +0x04 its pose is out of date (updateStaleObjects)
        // +0x08 its position was set, +0x0C its orientation: each answers a word, which ZGE2's setters pass on
        // (setObjectPosition, setObjectOrientation); ZGE3's drop it.
        ee::FunctionPtr<uint32_t(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)> moved;
        ee::FunctionPtr<uint32_t(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)> rotated;
        ee::FunctionPtr<void(ee::Ptr<SceneObject> object, uint32_t scale)> scaled;               // +0x10 its scale was set
        ee::FunctionPtr<uint32_t(ee::Ptr<SceneObject> object, uint32_t visible)> shown;          // +0x14 shown or hidden
        ee::FunctionPtr<uint32_t(ee::Ptr<SceneObject> object)> id; // +0x18 its id (findSceneObject; a model's: its scene's)
        // +0x1C (object): read only by callUnusedHook, which nothing calls;
        // the models' returns 0.
        ee::FunctionPtr<int32_t(ee::Ptr<SceneObject> object)> unusedHook;
        // +0x20 (object): called on every object by the pools' debug report
        // (reportSceneObjects); the models' returns their scene's +0x4C.
        ObjectHook debugReport;
    };
    static_assert(sizeof(SceneObjectClass) == 0x24);
    struct ObjectAttachment; // below

    struct SceneObject
    {
        // +0x00 the low four bits: the object's kind (0: a model, 3: an
        // animated thing without a scene [inferred]); kObjectPoseStale,
        // kObjectMoved, kObjectNoHooks above them.
        uint64_t flags;
        uint8_t _08[0x08];
        Vec4 position;                  // +0x10 (setObjectPosition)
        Quaternion orientation;         // +0x20
        // +0x30 its attachment record (ObjectAttachment, made the first time
        // it is attached or something is attached to it; 0: none).
        ee::Ptr<ObjectAttachment> attachment;
        ee::Ptr<SceneObjectClass> objectClass; // +0x34
        ee::Ptr<Scene> scene;           // +0x38
        union
        {
            ee::Ptr<Animator> animator;     // +0x3C
            ee::Ptr<struct TagTableHeader> tags; // ZGE4: an object with no scene keeps its tag table here (removeLevelObject releases it)
        };
        // +0x40 the level object it belongs to (zge/scene/objects.cpp: one of
        // the pool levelObjectsBegin .. levelObjectsEnd, or something else) [inferred]
        uint32_t owner;
        uint32_t _44;
        uint32_t messages;              // +0x48 its message box (zge/scene/message.h MessageTarget), 0: none
        uint32_t _4c;
    };
    static_assert(sizeof(SceneObject) == 0x50);
    static_assert(offsetof(SceneObject, position) == 0x10 && offsetof(SceneObject, orientation) == 0x20);
    static_assert(offsetof(SceneObject, attachment) == 0x30 && offsetof(SceneObject, scene) == 0x38);
    static_assert(offsetof(SceneObject, animator) == 0x3C && offsetof(SceneObject, owner) == 0x40);
    static_assert(offsetof(SceneObject, messages) == 0x48);
    // A scene object's place in the attachments (16 bytes, a record of the
    // attachments' pool, zobj.c): what it hangs from and the objects hanging
    // from it, a list through their nextSibling. The GC and Xbox builds
    // print "ZOBJ: Object is already attached to another" for an object
    // attached twice.
    struct ObjectAttachment
    {
        ee::Ptr<SceneObject> parent;      // +0x00 the object it hangs from (0: none)
        ee::Ptr<SceneBone> bone;          // +0x04 the parent's bone it follows (0: the parent itself)
        ee::Ptr<SceneObject> nextSibling; // +0x08 the next object hanging from the same parent
        ee::Ptr<SceneObject> firstChild;  // +0x0C the first object hanging from it
    };
    static_assert(sizeof(ObjectAttachment) == 0x10);

    // ZGE2's camera objects keep their place and four matrices in a record of their own (SceneObject's +0x3C,
    // 0x130 bytes from a pool initCameraObjects makes; newCameraObject takes one, destroyCameraObject frees it):
    // objectMatrix makes matrix 1 from the place and the rest from it when first asked for. ZGE3 keeps the
    // matrices in the camera record (MatrixCache, zge/utilities.cpp) and has no such record.
    struct CameraObjectMatrices
    {
        Vec4 position;        // +0x00 matrix 1's translation
        Quaternion orientation; // +0x10 matrix 1's turn
        // +0x20: 0 matrix 1's inverse (rigid), 1 the place, 2 matrix 0 without its translation (its 3x3
        // transposed), 3 a turn about y facing matrix 0's z axis
        Matrix matrices[4];
        uint32_t valid;       // +0x120 a bit for each matrix made
    };
    static_assert(offsetof(CameraObjectMatrices, matrices) == 0x20 && offsetof(CameraObjectMatrices, valid) == 0x120);
    inline constexpr uint32_t kCameraObjectMatricesBytes = 0x130; // the pool's record

    // ZGE4 has five bits of kind (BMX XXX: the mask 0x1F), so each flag above them is one bit higher there.
    inline constexpr uint64_t kObjectKindMask = game::readable::perGame(0xFu, 0xFu, 0x1Fu);
    // The kind of an object with a model scene (setObjectCastsShadow reaches its
    // scene for these): ZGE2 and ZGE3 number it 0, ZGE4 1 (BMX XXX tests the
    // five bits against 1 where AI's tests the four against 0).
    inline constexpr uint64_t kObjectModelKind = game::readable::perGame(0u, 0u, 1u);
    // Set while the object's pose is out of date; updatePose clears it
    // [inferred: who sets it].
    inline constexpr uint64_t kObjectPoseStale = game::readable::perGame(1ull << 7, 1ull << 7, 1ull << 8);
    // Bit 4: updateStaleObjects updates the object [inferred: what else it
    // means]; bit 6: its class's hooks are not called (set while it is
    // attached to another object: detachObject sends them its position and
    // orientation again).
    inline constexpr uint64_t kObjectMoved = game::readable::perGame(1ull << 4, 1ull << 4, 1ull << 5);
    inline constexpr uint64_t kObjectNoHooks = game::readable::perGame(1ull << 6, 1ull << 6, 1ull << 7);
    // Bit 5: the object casts a shadow (setObjectCastsShadow, the guiModel
    // widget's ShowShadow; a model's nodes get kSceneCastsShadow's bit).
    inline constexpr uint64_t kObjectCastsShadow = game::readable::perGame(1ull << 5, 1ull << 5, 1ull << 6);
    // ZGE4's bit 9: updateStaleObjects updates the object on every pass while it is set, stale or not
    // (setObjectAlwaysUpdated; a particle emitter object sets it while particles of it live). ZGE2 and ZGE3 have none.
    inline constexpr uint64_t kObjectAlwaysUpdated = game::readable::perGame(0ull, 0ull, 1ull << 9);

    // Which space boneMatrix and bonePoint give a bone's matrix in.
    enum BoneSpace : uint32_t
    {
        kBoneInWorld = 0,
        kBoneInScene = 1, // the model's space (the scene's node's)
        kBoneInParent = 2, // the bone's local matrix
    };

    // Whether an object's class is read for a hook: ZGE4 (BMX XXX) lets an object have none, and its
    // deleteSceneObject, placeOnParent and placeAttached test for that; ZGE2's and ZGE3's objects all have one.
    inline bool hasObjectClass(ee::Ptr<SceneObject> object) { return !game::readable::kIsBmx || object->objectClass; }

    ee::Ptr<Node> sceneRootNode(ee::Ptr<Scene> scene);
    uint32_t poseScene(ee::Ptr<Scene> scene);
    ee::Ptr<Matrix> boneMatrix(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, ee::Ptr<Matrix> out, uint32_t space);
    ee::Ptr<Vec4> bonePoint(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> point, ee::Ptr<Vec4> out,
                            uint32_t space);                                                           // bonePoint

    // The four matrices a scene keeps from +0x70 (zge/utilities.cpp), made
    // when first asked for.
    struct MatrixCache;
    ee::Ptr<Matrix> cachedMatrix(ee::Address object, uint32_t which); // a scene's or a camera's
    ee::Ptr<Matrix> objectMatrix(ee::Ptr<SceneObject> object, uint32_t which);
}

namespace zge
{
    // (model_objects.cpp)
    ee::Ptr<SceneObject> newModelObject(ee::Ptr<Scene> scene, uint32_t flags);
    // ZGE4 only: the node's model object, found or made (model_objects.cpp).
    ee::Ptr<SceneObject> nodeModelObject(ee::Ptr<Node> node, uint32_t registerAsset);
    ee::Ptr<SceneObject> namedModelObject(uint32_t nameHash, uint32_t registerAsset); // ZGE4 only
    ee::Ptr<SceneObject> newModelObjectFromPose(uint32_t nameHash, int32_t poseName); // (model_objects.cpp)
    uint32_t loadModelFile(ee::Address fileName);
    uint32_t loadLibraryModelEntry(uint32_t library, uint32_t id);
    int32_t releaseSceneAssetsScene(ee::Ptr<Scene> scene);
    ee::Ptr<Scene> cloneModelScene(ee::Ptr<Scene> scene);
    uint32_t modelBoneCount(ee::Ptr<SceneObject> object);
    void modelBoneIds(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> ids);
    ee::Ptr<SceneBone> findModelBone(ee::Ptr<SceneObject> object, uint32_t nameHash);
    void modelBoneBindPosition(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<Vec4> out);
    ee::Ptr<AnimationChannel> findAnimatorBone(ee::Ptr<SceneObject> object, uint32_t nameHash);
    // The parent of one of the model's bones; ZGE4's (BMX XXX) answers the scene's root bone whatever the bone
    // (model_objects.cpp).
    ee::Ptr<SceneBone> modelBoneParent(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone);
    void setModelAnimator(ee::Ptr<SceneObject> object, ee::Ptr<Animator> animator);
}

namespace zge
{
    struct TagRecord; // (zge/assets/tag_table.h)
    struct ViewSphere; // (zge/scene/camera.h)

    // (objects.cpp)
    uint32_t setSceneObjectShown(ee::Ptr<SceneObject> object, uint32_t shown);
    ee::Ptr<TagRecord> objectComponent(ee::Ptr<SceneObject> object);
    uint32_t newAppInstanceNode(ee::Address geometry, uint32_t addToWorld);
    int32_t isLevelObject(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> nodeSceneObject(ee::Ptr<Node> node, ee::Ptr<ee::Ptr<SceneBone>> boneOut);
    ee::Ptr<TagRecord> levelObjectClassTag(ee::Ptr<SceneObject> object);
    ee::Ptr<Node> objectLevelNode(ee::Ptr<SceneObject> object);
    uint32_t objectNameHash(ee::Ptr<SceneObject> object);
    uint32_t levelObjectTypeHash(ee::Ptr<SceneObject> object);
    ee::Ptr<ee::Ptr<SceneObject>> findObjects(ee::Address namesAddress, uint32_t count);
    // The level object with the name (the name text's address), 0 if none (objects.cpp).
    ee::Ptr<SceneObject> findObjectByName(uint32_t name);
    int32_t objectVelocity(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out);
    uint32_t registerLevelObjectType(uint32_t name, uint32_t create, uint32_t init);
    // ZGE2's registerLevelObjectType (DM2's): a type by name with the init its entities get, 1 if registered or the
    // name has that init already, 0 if it has another one or the table is full (objects.cpp).
    uint32_t registerEntityInit(ee::Ptr<const char> name, uint32_t init);
    uint32_t spawnLevelObjects();
    uint32_t spawnWorldMeshObjects(); // (ZGE2's spawnLevelObjects: no locators' pass)
    // The plain entity: the node's model as an object.
    ee::Ptr<SceneObject> createEntityObject(ee::Ptr<Node> node);
    int32_t clearLevelObjectTypes();
    // forEachObject over the objects with the name's hash: callback(object, userData).
    int32_t forEachObjectNamed(uint32_t name, uint32_t callback, uint32_t userData);
    uint32_t removeLevelObjects();
    // The level objects' save: its size (0 when no object keeps any state),
    // and every object's record written into the save.
    int32_t levelObjectsSaveSize();
    uint32_t saveLevelObjects(uint32_t save);
}

namespace zge
{
    // (pose.cpp)
    uint32_t copyScenePose(ee::Ptr<Scene> scene, ee::Ptr<Scene> from);
    void updatePose(ee::Ptr<SceneObject> object);
    ee::Ptr<Vec4> objectBonePoint(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> point, ee::Ptr<Vec4> out, uint32_t space);
    ee::Ptr<Matrix> objectBoneMatrix(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<Matrix> out, uint32_t space);
    uint32_t shutdownModelPoses();
    uint32_t sceneBoneCount(ee::Ptr<Scene> scene);
    void modelPoseHook();
    ee::Ptr<SceneObject> lookFromAt(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> eye, ee::Ptr<Vec4> target, uint32_t roll);
    ee::Ptr<SceneBone> rootBone(ee::Ptr<Scene> scene);
    ee::Ptr<SceneBone> boneParent(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone);
    ee::Ptr<SceneBone> boneFirstChild(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone);
    ee::Ptr<SceneBone> boneNextSibling(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone);
    ee::Ptr<Node> boneNode(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone);
    uint32_t boneId(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone);
    ee::Ptr<Node> setSceneRootColour(ee::Ptr<Scene> scene, ee::Ptr<uint8_t> colour);
    // The scene's root node given the volume its shadow is tested against the view with (its 0x20 bytes), the test
    // asked for once (pose.cpp).
    ee::Ptr<Node> setSceneShadowViewTest(ee::Ptr<Scene> scene, ee::Ptr<const ViewSphere> volume);
    ee::Ptr<char> boneName(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone);
    uint32_t sceneBoneIds(ee::Ptr<Scene> scene, ee::Ptr<uint32_t> ids);
    ee::Ptr<SceneBone> findSceneModelBone(ee::Ptr<Scene> scene, uint32_t id);
    ee::Ptr<SceneBone> findSceneModelBoneThunk(ee::Ptr<Scene> scene, uint32_t id); // (ZGE4 only) a call of it
    ee::Ptr<Vec4> boneBindPosition(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone, ee::Ptr<Vec4> out);
    void setSceneObject(ee::Ptr<Scene> scene, ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> sceneObject(ee::Ptr<Scene> scene);
    uint32_t setScenePosition(ee::Ptr<Scene> scene, ee::Ptr<Vec4> position);
    uint32_t setSceneOrientation(ee::Ptr<Scene> scene, ee::Ptr<Quaternion> orientation);
    uint32_t setSceneScale(ee::Ptr<Scene> scene, ee::Ptr<Vec4> scale);
    ee::Ptr<Scene> findModelPose(uint32_t id, int32_t skip);
    void setupSceneModel(ee::Ptr<Node> node, uint32_t retainNodes, uint32_t makeBones);
    // ZGE4's setupSceneModel (setupSceneModelWithOptions, pose.cpp): option bits for the two words, and the MOBO
    // scene can be given (cloneScene gives the source's).
    constexpr uint32_t kSetupMakeBones = 1u;   // the node (unless kNodeSecondBucket) gets kNodeBoneTree first
    constexpr uint32_t kSetupFromSource = 2u;  // the MOBO scene is `source`, not the one found by the node's name
    void setupSceneModelWithOptions(ee::Ptr<Node> node, uint32_t options, ee::Ptr<Scene> source);
    ee::Ptr<Scene> setupNodeSceneModel(ee::Ptr<Node> node);
    ee::Ptr<Scene> setupGeometrySceneModel(ee::Ptr<Node> node, uint32_t retainNodes);
    // (ZGE2's cloneScene takes the scene alone: pose.cpp)
    ee::Ptr<Scene> cloneScene(ee::Ptr<Scene> scene READABLE_PARAM_EXCEPT(dm2, uint32_t flags));
    uint32_t releaseScene(ee::Ptr<Scene> scene);
    uint64_t bindSceneAnimator(ee::Ptr<Scene> scene, ee::Ptr<Animator> animator);
    uint32_t changeSceneState(ee::Ptr<Scene> scene, uint32_t statesOff, uint32_t statesOn);
    ee::Ptr<ZgePart> sceneFirstMesh(ee::Ptr<Scene> scene);
    uint32_t registerMoboChunkHandler();
}

namespace zge
{
    // (scene_objects.cpp)
    int32_t initSceneObjects();
    ee::Ptr<SceneObject> newSceneObject();
    ee::Ptr<SceneObject> initSceneObject(ee::Ptr<SceneObject> object, uint32_t kind);
    ee::Ptr<SceneObject> newObjectOfKind(uint32_t kind); // ZGE4's initSceneObject (scene_objects.cpp)
    void setObjectAlwaysUpdated(ee::Ptr<SceneObject> object, uint32_t on); // ZGE4 only (scene_objects.cpp)
    uint32_t deleteSceneObject(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> attachedParent(ee::Ptr<SceneObject> object); // the object it hangs from, 0 for none
    // The objects hanging from an object, in order (0 after the last).
    ee::Ptr<SceneObject> firstAttachedChild(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> nextAttachedSibling(ee::Ptr<SceneObject> object);
    ee::Ptr<Scene> sceneOf(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> setObjectPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position);
    // forEachAttached (scene_objects.cpp): visit(child, data) for each object attached to this one. Always 1.
    uint32_t forEachAttached(ee::Ptr<SceneObject> object, uint32_t visitFunction, ee::Address data);
    ee::Ptr<Vec4> objectPosition(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> setObjectOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation);
    ee::Ptr<Quaternion> objectOrientation(ee::Ptr<SceneObject> object);
    ee::Ptr<SceneObject> setObjectAngles(ee::Ptr<SceneObject> object, ee::Ptr<EulerAngles> angles);
    ee::Ptr<SceneObject> setObjectRotationMatrix(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> rotation);
    void setObjectOwner(ee::Ptr<SceneObject> object, ee::Address owner);
    uint64_t setObjectCastsShadow(ee::Ptr<SceneObject> object, uint32_t casts);
    uint32_t findSceneObject(uint32_t id, uint32_t /*unused*/, uint32_t after);
    int32_t shutDownSceneObjects();
    uint32_t forEachObjectOfKind(uint32_t visitFunction, uint32_t data, uint32_t kind);
    void attachObject(ee::Ptr<SceneObject> object, ee::Ptr<SceneObject> parent, ee::Ptr<SceneBone> bone);
    // attachObjectAt (scene_objects.cpp): attached after the parent's `after`-th child.
    ee::Ptr<SceneObject> attachObjectAt(ee::Ptr<SceneObject> object, ee::Ptr<SceneObject> parent, ee::Ptr<SceneBone> bone, uint32_t after);
    int32_t reportSceneObjects();
}
