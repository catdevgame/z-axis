// The pose update of an object's model (zge/scene/scene.h): once a frame
// (kUpdatePose, updatePose) the object's position, orientation and scale
// become a matrix, the bones' keys their local matrices, and the model's
// node (or, for a rigid model, its top bone's node) gets the matrix; the
// scene tree's update then brings the world matrices along (a rigid model's
// bones are nodes), and a skinned model's bone matrices are rebuilt in the
// model's space for its renderer. Bone queries (a bone's matrix or a point
// on it, in the world, the model or its parent) read them. In BMX XXX
// (PS2) the same code word for word, DM2's is older (its scene has no
// skinned kind); module zobjModel for the object's entries.

#include "scene.h"

#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/angle.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/scene/camera.h"
#include "zge/scene/skeleton.h"
#include "zge/scene/animation.h"
#include "zge/assets/chunk_reader.h"
#include "zge/render/part_clone.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/leave.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/terrain.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/pose.cpp.h"

namespace zge
{
    namespace
    {
        // (out, scale): a matrix that scales by scale's x, y and z
        // (matrixScaling, not readable yet).
        constexpr uint32_t kMatrixScaling = sym::matrixScaling;
        constexpr uint32_t kUpdateWorldMatrices = sym::updateWorldMatrices; // updateWorldMatrices (zge/scene/transform.cpp)

        // A skinned model's pivot: the top bone's place less its bind position,
        // turned by the matrix.
        void skinnedPivot(ee::Ptr<SceneBone> top, ee::Ptr<Matrix> matrix, ee::Ptr<Vec4> pivot)
        {
            *pivot = top->matrices->local.translation() - top->bindPosition;
            *pivot = rotated(*matrix, *pivot);
        }

        // The key's rotation, scale and translation (added to the bone's
        // bind position) into the bone's local matrix (the two loops of
        // updateBoneLocalMatrices and placeScene have this code each).
        void localMatrixFromKey(ee::Ptr<SceneBone> bone, ee::Ptr<BoneKey> key, ee::Ptr<Matrix> local)
        {
            quaternionToMatrix(local, &key->rotation);
            matrixScale(local, local, &key->scale.xyz());
            local->translation() = bone->bindPosition + key->translation;
        }
    }

    namespace
    {
        // ZGE2's matrix copy: the rotation's and the translation's x, y and z (the w column left).
        void copyRotationAndTranslation(Matrix *out, const Matrix *matrix)
        {
            ee::game(sym::dm2::copyRotationAndTranslation, ee::Ptr<Matrix>(out), ee::Ptr<const Matrix>(matrix));
        }
    }

    // sceneRootNode: the node that carries the scene: a skinned model's own
    // node, else its top bone's.
    ee::Ptr<Node> sceneRootNode(ee::Ptr<Scene> scene)
    {
        if (scene->flags & kSceneSkinned)
            return scene->node;
        return scene->bones->node;
    }
    READABLE_FUNCTION(sceneRootNode, Verified);

    // markBonesPosed: the scene's node marked as having its bones posed.
    ee::Ptr<Node> markBonesPosed(ee::Ptr<Scene> scene)
    {
        const ee::Ptr<Node> node = sceneRootNode(scene);
        node->flagsDoubleword() |= kNodeBonesPosedBit;
        return node;
    }
    READABLE_FUNCTION(markBonesPosed, Verified);

    // sceneWorldMatrix: the world matrix of the scene's node.
    ee::Ptr<Matrix> sceneWorldMatrix(ee::Ptr<Scene> scene)
    {
        ee::Ptr<Node> node;
        if (scene->flags & kSceneSkinned)
            node = scene->node;
        else
            node = scene->bones->node;
        return nodeMatrix(node, 1);
    }
    READABLE_FUNCTION(sceneWorldMatrix, Verified);

    // updateBoneModelMatrices: every bone's model matrix from its local one and its
    // parent's model matrix, parents first: a walk of the bone tree with a
    // stack of bones in the original's frame (each bone's slot taken by its
    // first child, its next sibling pushed after).
    void updateBoneModelMatrices(ee::Ptr<Scene> scene)
    {
        ee::Local<ee::Ptr<SceneBone>> stack;
        const ee::Ptr<ee::Ptr<SceneBone>> bottom = stack;
        ee::Ptr<ee::Ptr<SceneBone>> top = bottom;
        *top = scene->bones;
        for (;;)
        {
            const ee::Ptr<SceneBone> bone = *top;
            const ee::Ptr<BoneMatrices> matrices = bone->matrices;
            if (bone->parent)
                matrixMultiply(&matrices->model, &bone->parent->matrices->model, &matrices->local);
            else if constexpr (game::readable::kIsDm2) // (a top bone: its local matrix, through ZGE2's copy routine)
                copyRotationAndTranslation(&matrices->model, &matrices->local);
            else
                matrices->model = matrices->local;
            if (const ee::Ptr<SceneBone> child = bone->firstChild)
            {
                *top = child;
                ++top;
            }
            if (const ee::Ptr<SceneBone> sibling = bone->nextSibling)
            {
                *top = sibling;
                ++top;
            }
            const bool more = bottom < top;
            top -= 1;
            if (!more)
                break;
        }
    }
    READABLE_FUNCTION(updateBoneModelMatrices, Verified);

    // dropPoseChanges: the animation's pending changes dropped (a mirrored
    // instance: its pose is not built).
    void dropPoseChanges(ee::Ptr<Scene> scene)
    {
        if (const ee::Ptr<PoseChanges> changes = sceneChanges(scene))
            changes->bits &= kPoseChangesKept;
    }
    READABLE_FUNCTION(dropPoseChanges, Verified);

    // rebuildBoneMatrices: ZGE2's updateBoneLocalMatrices, which keeps no changes to test: every bone's local
    // matrix rebuilt, from its key's rotation (no scale) and its bind position plus the key's translation, or with
    // no key its rotation cleared and its bind position; then a skinned scene's model matrices, and its node
    // marked as posed.
    void rebuildBoneMatrices(ee::Ptr<Scene> scene)
    {
        ee::Ptr<SceneBone> bone = scene->bones;
        for (int32_t left = scene->boneCount - 1; left != -1; --left, ++bone)
        {
            Matrix &local = bone->matrices->local;
            if (const ee::Ptr<BoneKey> key = bone->key)
            {
                quaternionToMatrix(&local, &key->rotation);
                local.translation() = bone->bindPosition + key->translation;
            }
            else
            {
                matrixClearRotation(&local);
                local.translation() = bone->bindPosition;
            }
        }
        if (!(scene->flags & kSceneSkinned))
            return;
        updateBoneModelMatrices(scene);
        game::tail(markBonesPosed, scene);
    }

    // updateBoneLocalMatrices: if the animation changed anything, each bone with a key
    // gets its local matrix from it.
    void updateBoneLocalMatrices(ee::Ptr<Scene> scene)
    {
        uint32_t pending = 0;
        ee::Ptr<SceneBone> bone = scene->bones;
        if (const ee::Ptr<PoseChanges> changes = sceneChanges(scene))
        {
            pending = changes->bits;
            changes->bits = pending & kPoseChangesKept;
            pending &= kPoseChangesPending;
        }
        if (pending == 0)
            return;
        for (int32_t left = scene->boneCount - 1; left != -1;)
        {
            if (const ee::Ptr<BoneKey> key = bone->key)
            {
                localMatrixFromKey(bone, key, &bone->matrices->local);
            }
            ++bone;
            if (--left == -1)
                break;
        }
    }
    READABLE_FUNCTION_OWN(updateBoneLocalMatrices, Verified, dm2, rebuildBoneMatrices);

    namespace
    {
        void keyedLocalMatrix(ee::Ptr<SceneBone> bone, ee::Ptr<BoneKey> key, ee::Ptr<Matrix> local)
        {
            localMatrixFromKey(bone, key, local);
        }
    }

    // placeScene: the scene put where matrix says (the object's matrix;
    // 0: nothing moves). A skinned model: its node gets the matrix, and its
    // pivot (the top bone's place less its bind position, turned by the
    // matrix); then its world matrices and the bones' model matrices, and
    // the node marked. A rigid model: the top bone's node gets the matrix
    // times the top bone's local matrix (from its key, or its bind position
    // alone), unless the top bone hangs from another node (it gets the
    // matrix as it is); then the tree's world matrices.
    void placeScene(ee::Ptr<Scene> scene, ee::Ptr<Matrix> matrix)
    {
        ee::Local<Vec4> pivot;
        ee::Local<Matrix> placed;
        if (scene->flags & kSceneSkinned)
        {
            const ee::Ptr<Node> node = scene->node;
            setNodeLocalMatrix(node, matrix);
            const ee::Ptr<SceneBone> top = scene->bones;
            skinnedPivot(top, matrix, pivot);
            node->origin = *pivot; // (two doublewords)
            updateWorldMatrices(node); // by address: a readable callee called directly makes its tail call as a call, which returns twice when it unwinds
            updateBoneModelMatrices(scene);
            markBonesPosed(scene);
            if constexpr (game::readable::kIsBmx)
                scene->flags &= ~kScenePoseCopied;
            return;
        }
        if (matrix)
        {
            const ee::Ptr<Node> root = rootNode(scene->bones->node);
            const ee::Ptr<Node> topNode = scene->bones->node;
            if (topNode == root)
            {
                // (ZGE4 takes the root's local matrix itself, not through nodeMatrix.)
                const ee::Ptr<Matrix> local = game::readable::kIsBmx ? ee::Ptr<Matrix>(&root->local) : nodeMatrix(root, 0);
                const ee::Ptr<SceneBone> top = scene->bones;
                if (const ee::Ptr<BoneKey> key = top->key)
                    keyedLocalMatrix(top, key, local);
                else if (!game::readable::kIsBmx || !(scene->flags & kScenePoseCopied)) // ZGE4: a copied pose kept
                {
                    matrixClearRotation(local);
                    local->translation() = top->bindPosition;
                }
                matrixMultiply(placed, matrix, local);
                setNodeLocalMatrix(scene->bones->node, &*placed);
            }
            else
                setNodeLocalMatrix(topNode, matrix);
            updateWorldMatrices(root); // by address (see above)
        }
        // ZGE4: the copied pose is placed now.
        if constexpr (game::readable::kIsBmx)
            scene->flags &= ~kScenePoseCopied;
    }
    READABLE_FUNCTION_WAITS(placeScene, Verified);

    // copyScenePose: another scene's pose copied (position, scale,
    // orientation, every bone's local matrix), then the scene placed: where
    // the other's node is for a skinned model, else not moved.
    uint32_t copyScenePose(ee::Ptr<Scene> scene, ee::Ptr<Scene> from)
    {
        scene->position = from->position;
        scene->scale = from->scale;
        quaternionCopy(&scene->orientation, &from->orientation);
        ee::Ptr<SceneBone> bone = scene->bones;
        ee::Ptr<SceneBone> source = from->bones;
        for (int32_t left = scene->boneCount - 1; left != -1;)
        {
            bone->matrices->local = source->matrices->local;
            ++bone;
            ++source;
            if (--left == -1)
                break;
        }
        if (scene->flags & kSceneSkinned)
        {
            const ee::Ptr<Matrix> fromMatrix = nodeMatrix(from->node, 0);
            placeScene(scene, fromMatrix);
        }
        else
            placeScene(scene, ee::Ptr<Matrix>());
        if constexpr (game::readable::kIsBmx) // (ZGE4 marks the scene whose pose was copied in)
            scene->flags |= kScenePoseCopied;
        return 1;
    }
    READABLE_FUNCTION_WAITS(copyScenePose, Verified);

    // poseAndPlaceScene: ZGE2's poseScene. The object's matrix as ZGE3's makes it; then every bone's local matrix
    // rebuilt (ZGE2 keeps no changes to test: rebuildBoneMatrices), and the scene placed inline, more simply than
    // ZGE3's placeScene: a skinned model's node takes the matrix; a rigid model's top bone's node takes it times the
    // root's own matrix when the top bone's node is the root of its tree, else the matrix as it is. No
    // mirrored-instance test, no pivot, and the world matrices are left to the moved nodes' update. Returns 1.
    uint32_t poseAndPlaceScene(ee::Ptr<Scene> scene)
    {
        ee::Local<Matrix> matrix;
        ee::Local<Matrix> scaling;
        ee::Local<Matrix> placed;
        quaternionToMatrix(&*matrix, &scene->orientation);
        matrix->translation() = scene->position;
        if (scene->flags & kSceneScaled)
        {
            matrixScaling(scaling, &scene->scale.xyz());
            matrixMultiply(matrix, matrix, scaling);
        }
        rebuildBoneMatrices(scene);
        if (scene->flags & kSceneSkinned)
        {
            setNodeLocalMatrix(scene->node, matrix);
            return 1;
        }
        const ee::Ptr<Node> root = rootNode(scene->bones->node);
        const ee::Ptr<Matrix> rootMatrix = nodeMatrix(root, 0);
        if (scene->bones->node == root)
        {
            matrixMultiply(placed, matrix, rootMatrix);
            setNodeLocalMatrix(scene->bones->node, &*placed);
        }
        else
            setNodeLocalMatrix(scene->bones->node, matrix);
        return 1;
    }

    // poseScene: the scene's pose for this frame: the object's matrix
    // from its orientation, position and (if set) scale, the bones' local
    // matrices if their keys changed, then placed. A mirrored instance is
    // not posed (its pending changes dropped). Returns 1.
    uint32_t poseScene(ee::Ptr<Scene> scene)
    {
        ee::Local<Matrix> matrix;
        ee::Local<Matrix> scaling;
        const ee::Ptr<Node> root = sceneRootNode(scene);
        if (root->flags & kNodeMirrored)
        {
            dropPoseChanges(scene);
            return 1;
        }
        quaternionToMatrix(&*matrix, &scene->orientation);
        matrix->translation() = scene->position;
        if (scene->flags & kSceneScaled)
        {
            matrixScaling(scaling, &scene->scale.xyz());
            matrixMultiply(matrix, matrix, scaling);
        }
        if (scene->flags & kSceneKeysChanged)
            updateBoneLocalMatrices(scene);
        placeScene(scene, matrix);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(poseScene, Verified, dm2, poseAndPlaceScene);

    // updatePose (kUpdatePose): the object's model posed for this frame.
    void updatePose(ee::Ptr<SceneObject> object)
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2 keeps no pose-stale flag: the pose is its last step)
        {
            game::tail(poseScene, object->scene);
            return;
        }
        poseScene(object->scene);
        object->flags &= ~kObjectPoseStale;
    }
    READABLE_FUNCTION_WAITS(updatePose, Verified);

    // boneMatrix: the bone's matrix in the world, in the model's space or
    // relative to its parent (space & 3; 3: out left alone). A rigid
    // model's bone matrices are its nodes' (in the world); a skinned
    // model's are in the model's space. Returns out.
    ee::Ptr<Matrix> boneMatrix(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, ee::Ptr<Matrix> out, uint32_t space)
    {
        ee::Local<Matrix> out2;
        space &= 3u;
        const ee::Ptr<BoneMatrices> matrices = bone->matrices;
        const bool skinned = (scene->flags & kSceneSkinned) != 0;
        // (ZGE2 copies a matrix out through copyRotationAndTranslation.)
        if (space == kBoneInParent)
        {
            if constexpr (game::readable::kIsDm2)
                copyRotationAndTranslation(&*out, &matrices->local);
            else
                *out = matrices->local;
        }
        else if (space == kBoneInWorld || space == kBoneInScene)
        {
            const ee::Ptr<Matrix> model = &matrices->model;
            // Already in the space asked for: a rigid model's in the world,
            // a skinned model's in the model's.
            if (skinned == (space == kBoneInScene))
            {
                if constexpr (game::readable::kIsDm2)
                    copyRotationAndTranslation(&*out, &*model);
                else
                    *out = *model;
            }
            else
            {
                ee::Ptr<Matrix> to;
                if (space == kBoneInWorld)
                    to = sceneWorldMatrix(scene);
                else
                {
                    const ee::Ptr<Matrix> world = sceneWorldMatrix(scene);
                    to = matrixInvertRigid(out2, world);
                }
                matrixMultiply(out, to, model);
            }
        }
        return out;
    }
    READABLE_FUNCTION(boneMatrix, Verified);

    // bonePoint: a point given in the bone's space, in the space asked for
    // (boneMatrix's): rotated by the bone's matrix, then its translation
    // added. Returns out. Its callers read the VU0 registers it leaves.
    ee::Ptr<Vec4> bonePoint(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> point, ee::Ptr<Vec4> out,
                            uint32_t space)
    {
        ee::Local<Matrix> matrix;
        boneMatrix(scene, bone, matrix, space);
        aligned(out) = rotated(*matrix, *point);
        aligned(out) += matrix->translation();
        return out;
    }
    READABLE_FUNCTION_VU0(bonePoint, Verified);

    // objectBonePoint: bonePoint on the object's scene (a tail call).
    ee::Ptr<Vec4> objectBonePoint(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> point,
                                  ee::Ptr<Vec4> out, uint32_t space)
    {
        return game::tail(zge::bonePoint, object->scene, bone, point, out, space);
    }
    READABLE_FUNCTION(objectBonePoint, Verified);

    // objectBoneMatrix: boneMatrix on the object's scene (a tail call).
    ee::Ptr<Matrix> objectBoneMatrix(ee::Ptr<SceneObject> object, ee::Ptr<SceneBone> bone, ee::Ptr<Matrix> out, uint32_t space)
    {
        return game::tail(zge::boneMatrix, object->scene, bone, out, space);
    }
    READABLE_FUNCTION(objectBoneMatrix, Verified);

    // cameraObjectMatrix: ZGE2's objectMatrix: matrix `which` (0 .. 3) of a camera object's own record
    // (CameraObjectMatrices at +0x3C), made first if it is not yet (0, 2 and 3 make matrix 1 first); null for
    // another `which`.
    ee::Ptr<Matrix> cameraObjectMatrix(ee::Ptr<SceneObject> object, uint32_t which)
    {
        const ee::Ptr<CameraObjectMatrices> record(ee::Address(object->animator));
        const ee::Ptr<Matrix> matrices = &record->matrices[0];
        if (!(static_cast<int32_t>(record->valid) >> (which & 31u) & 1))
        {
            if (which == 1)
            {
                quaternionToMatrix(&record->matrices[1], &record->orientation);
                record->matrices[1].translation() = record->position;
            }
            else if (which == 0)
            {
                cameraObjectMatrix(object, 1);
                matrixInvertRigid(matrices, matrices + 1);
            }
            else if (which == 2)
            {
                cameraObjectMatrix(object, 1);
                const ee::Ptr<Matrix> out = matrices + 2;
                matrixTranspose3(out, matrices);
                matrixClearTranslation(out);
            }
            else if (which == 3)
            {
                cameraObjectMatrix(object, 1);
                const ee::Ptr<Matrix> out = matrices + 3;
                const Matrix &m = record->matrices[0];
                const int32_t heading = static_cast<int32_t>(static_cast<short>(angleAtan2(m.m[2][0], m.m[2][2])));
                matrixIdentity(out);
                matrixRotateY(out, out, static_cast<int32_t>(static_cast<int16_t>(-heading)));
            }
            else
                return ee::Ptr<Matrix>();
            record->valid = record->valid | 1u << (which & 31u);
        }
        return matrices + which;
    }

    // objectMatrix: the object's scene's matrix `which` (cachedMatrix, zge/utilities.cpp:
    // 0 .. 3, made when first asked for).
    ee::Ptr<Matrix> objectMatrix(ee::Ptr<SceneObject> object, uint32_t which)
    {
        return cachedMatrix(sceneOf(object), which);
    }
    READABLE_FUNCTION_OWN(objectMatrix, Verified, dm2, cameraObjectMatrix);

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_modelPosePool> g_modelPosePool;
        // setupSceneModel's for addModelBone: the next bone of the scene
        // being made, and whether its nodes are retained.
        inline ee::Global<ee::Ptr<SceneBone>, sym::g_nextModelBone> g_nextModelBone;
        inline ee::Global<uint32_t, sym::g_retainModelNodes> g_retainModelNodes;
        inline ee::Global<int32_t, sym::g_modelPosesReady> g_modelPosesReady;
        // The scene setupSceneModel made or found (0: none).
        inline ee::Global<ee::Ptr<Scene>, sym::g_setupScene> g_setupScene;
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<int32_t, sym::g_cameraObjectsReady> g_cameraObjectsReady;
        // ZGE2's pool of the camera objects' matrix records (CameraObjectMatrices), four a block.
        inline ee::Global<ee::Ptr<Pool>, sym::dm2::g_cameraObjectMatricesPool> g_cameraObjectMatricesPool;
        constexpr uint32_t kCameraObjectMatricesABlock = 4;
        constexpr uint32_t kLookAt = sym::lookAtMatrix; // lookAtMatrix ((out, eye, target, 0) (engine); readable: lookAtMatrix, Verified; hooked by a game override: called by address)
    }

    // allocateSceneBones: room for count bones, zeroed.
    ee::Ptr<SceneBone> allocateSceneBones(int32_t count)
    {
        const uint32_t bytes = count * sizeof(SceneBone);
        return ee::Ptr<SceneBone>(memoryFill(heapAllocate(bytes), 0, bytes));
    }
    READABLE_FUNCTION_WAITS(allocateSceneBones, Verified);

    // resetBonePose (initPoseBones): a skinned bone's three matrices made the
    // identity. Returns the last.
    ee::Ptr<Matrix> resetBonePose(ee::Ptr<BonePose> pose)
    {
        matrixIdentity(&pose->local);
        matrixIdentity(&pose->matrix);
        return game::tail(zge::matrixIdentity, &pose->unread); // the original's tail call
    }
    READABLE_FUNCTION(resetBonePose, Verified);

    // freeSceneBones: allocateSceneBones' room given back.
    void freeSceneBones(ee::Ptr<SceneBone> bones)
    {
        heapFree(bones);
    }
    READABLE_FUNCTION_WAITS(freeSceneBones, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        Vec3 * vec3SetOne(ee::Ptr<Vec3> x20) { return zge::vec3SetOne(x20); }
    }

    uint32_t shutdownModelPoses()
    {
        if (*g_modelPosesReady != 0)
        {
            destroyPool(*g_modelPosePool);
            *g_modelPosesReady = 0;
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownModelPoses, Verified);

    uint32_t sceneBoneCount(ee::Ptr<Scene> scene)
    {
        return scene->boneCount;
    }
    READABLE_FUNCTION(sceneBoneCount, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int forEachNodeInSubtree(ee::Ptr<Node> node, uint32_t visit, uint32_t value) { return zge::forEachNodeInSubtree(node, visit, value); }
    }

    // countSceneMeshStrips (no caller in AI; its name is older than what it
    // was found to do): the scene handed on to walkSceneBones, which does
    // nothing with its bones. Returns what that leaves.
    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
            // walkSceneBones (void), by address: its result register is the result here.
            int32_t walkSceneBonesKeepingResult(ee::Ptr<Scene> scene) { return ee::tailCall<int32_t>(sym::walkSceneBones, scene); }
        }
    }

    int32_t countSceneMeshStrips(ee::Ptr<Scene> scene)
    {
        return tail::walkSceneBonesKeepingResult(scene);
    }
    READABLE_FUNCTION(countSceneMeshStrips, Draft);

    // modelPoseVisit (ZGE4 only): modelPoseHook's visitor, each model pose's scene walked
    // (countSceneMeshStrips); 1, go on.
    inline uint32_t modelPoseVisit(ee::Ptr<Scene> scene)
    {
        countSceneMeshStrips(scene);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, modelPoseVisit);

    // reportEngineObjects' model poses' report: empty in ZGE3; ZGE4 walks every
    // model pose's scene (the original's tail jump into poolForEach).
    void modelPoseHook()
    {
        if constexpr (game::readable::kIsBmx)
            game::tail(poolForEach, *g_modelPosePool, sym::bmx::modelPoseVisit, 0);
    }
    READABLE_FUNCTION(modelPoseHook, Verified);

    // Zobj: initSceneObjects': initScenes and g_cameraObjectsReady = 1.
    int32_t initCameraObjects()
    {
        const int32_t initScenesResult = zge::initScenes();
        int32_t result = 0;
        if (initScenesResult == 0)
        {
            return 0;
        }
        result = 1;
        // ZGE2 makes its camera objects' matrix records' pool too.
        if constexpr (game::readable::kIsDm2)
            *g_cameraObjectMatricesPool = ee::Ptr<Pool>(createPool(kCameraObjectMatricesBytes, kCameraObjectMatricesABlock, 0));
        *g_cameraObjectsReady = 1;
        return result;
    }
    READABLE_FUNCTION_WAITS(initCameraObjects, Verified);

    uint32_t shutdownSceneRecords()
    {
        if (*g_cameraObjectsReady != 0)
        {
            const int32_t shutDownScenes = zge::shutDownScenes();
            if (shutDownScenes == 0)
            {
                return 0;
            }
            if constexpr (game::readable::kIsDm2) // (and its camera objects' matrix records' pool)
                destroyPool(*g_cameraObjectMatricesPool);
            *g_cameraObjectsReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownSceneRecords, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void lookAtMatrix(ee::Ptr<Matrix> matrix, ee::Ptr<const Vec4> eye, ee::Ptr<const Vec4> target, uint32_t roll) { ee::game(kLookAt, matrix, eye, target, roll); }
    }

    ee::Ptr<SceneObject> lookFromAt(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> eye, ee::Ptr<Vec4> target, uint32_t roll)
    {
        ee::Local<Matrix> matrix;
        ee::Local<Quaternion> rotation;
        lookAtMatrix(matrix, eye, target, roll); // the roll goes on in its register, unread
        matrixToQuaternion(ee::Ptr<Quaternion>(rotation.address()), ee::Ptr<const Matrix>(matrix.address()));
        setObjectPosition(object, eye);
        return setObjectOrientation(object, rotation);
    }
    READABLE_FUNCTION_WAITS(lookFromAt, Verified);

    // A scene's bones (zobjModel's bone queries): an array, Scene::bones,
    // linked as a tree. A bone is found by its id, its name's hash.

    // rootBone: the first bone, the top of the tree.
    ee::Ptr<SceneBone> rootBone(ee::Ptr<Scene> scene)
    {
        return scene->bones;
    }
    READABLE_FUNCTION(rootBone, Verified);

    ee::Ptr<SceneBone> boneParent(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone)
    {
        return bone->parent;
    }
    READABLE_FUNCTION(boneParent, Verified);

    ee::Ptr<SceneBone> boneFirstChild(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone)
    {
        return bone->firstChild;
    }
    READABLE_FUNCTION(boneFirstChild, Verified);

    ee::Ptr<SceneBone> boneNextSibling(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone)
    {
        return bone->nextSibling;
    }
    READABLE_FUNCTION(boneNextSibling, Verified);

    // boneNode: a rigid model's node of the bone.
    ee::Ptr<Node> boneNode(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone)
    {
        return bone->node;
    }
    READABLE_FUNCTION(boneNode, Verified);

    // boneId: the bone's id, or with no bone the scene's own.
    uint32_t boneId(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone)
    {
        if (bone)
            return bone->id;
        return scene->id;
    }
    READABLE_FUNCTION(boneId, Verified);

    // boneName: the bone's name, or with no bone the scene's own (the
    // models' debug report prints it).
    // boneNameById (ZGE2 only; boneName): the name of the scene's bone with this id, 0 when none has it.
    inline ee::Ptr<char> boneNameById(ee::Ptr<Scene> scene, uint32_t id)
    {
        ee::Ptr<SceneBone> each = scene->bones;
        for (int32_t left = scene->boneCount - 1; left != -1; --left, ++each)
            if (each->id == id)
                return each->name;
        return ee::Ptr<char>();
    }
    READABLE_FUNCTION_ONLY(dm2, boneNameById);

    ee::Ptr<char> boneName(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone)
    {
        // (ZGE2 looks the bone's name up among the scene's bones by its id.)
        if constexpr (game::readable::kIsDm2)
            if (bone)
                return boneNameById(scene, bone->id);
        if (bone)
            return bone->name;
        return scene->name;
    }
    READABLE_FUNCTION(boneName, Verified);

    // sceneBoneIds: every bone's id into ids, in the bones' order. Returns
    // the last one, -1 with no bones.
    uint32_t sceneBoneIds(ee::Ptr<Scene> scene, ee::Ptr<uint32_t> ids)
    {
        uint32_t id = ~0u;
        for (int32_t i = 0; i < scene->boneCount; ++i)
        {
            id = scene->bones[i].id;
            ids[i] = id;
        }
        return id;
    }
    READABLE_FUNCTION(sceneBoneIds, Verified);

    // findSceneModelBone: the bone with that id, 0 if the scene has none.
    ee::Ptr<SceneBone> findSceneModelBone(ee::Ptr<Scene> scene, uint32_t id)
    {
        for (int32_t i = 0; i < scene->boneCount; ++i)
            if (scene->bones[i].id == id)
                return scene->bones + i;
        return ee::Ptr<SceneBone>();
    }
    READABLE_FUNCTION(findSceneModelBone, Verified);

    // findSceneModelBoneThunk (ZGE4 only; findModelBone and the game call it): findSceneModelBone's answer handed
    // on (what is left of a look-up the other builds report a missing bone from: "Part (0x%x)not found in model").
    inline ee::Ptr<SceneBone> findSceneModelBoneThunk(ee::Ptr<Scene> scene, uint32_t id)
    {
        return findSceneModelBone(scene, id);
    }
    READABLE_FUNCTION_ONLY(bmx, findSceneModelBoneThunk);

    // boneBindPosition: the bone's place in its parent at rest, into out.
    ee::Ptr<Vec4> boneBindPosition(ee::Ptr<Scene> /*scene*/, ee::Ptr<SceneBone> bone, ee::Ptr<Vec4> out)
    {
        aligned(out) = bone->bindPosition;
        return out;
    }
    READABLE_FUNCTION(boneBindPosition, Verified);

    void setSceneObject(ee::Ptr<Scene> scene, ee::Ptr<SceneObject> object)
    {
        scene->object = object;
    }
    READABLE_FUNCTION(setSceneObject, Verified);

    ee::Ptr<SceneObject> sceneObject(ee::Ptr<Scene> scene)
    {
        return scene->object;
    }
    READABLE_FUNCTION(sceneObject, Verified);

    // Where the object puts its model: position, orientation and scale (a
    // scale within 1e-5 of one on every axis is no scale at all).
    uint32_t setScenePosition(ee::Ptr<Scene> scene, ee::Ptr<Vec4> position)
    {
        scene->position = quadwordOf(position);
        return 1;
    }
    READABLE_FUNCTION(setScenePosition, Verified);

    uint32_t setSceneOrientation(ee::Ptr<Scene> scene, ee::Ptr<Quaternion> orientation)
    {
        // The orientation may sit off a 16-byte boundary (the original copies
        // it by unaligned doublewords), so it is read as bytes: a Quaternion
        // read whole is an aligned quadword load on the host, which faults
        // there on x86.
        scene->orientation = loadUnaligned<Quaternion>(ee::Address(orientation));
        return 1;
    }
    READABLE_FUNCTION(setSceneOrientation, Verified);

    uint32_t setSceneScale(ee::Ptr<Scene> scene, ee::Ptr<Vec4> scale)
    {
        scene->scale = quadwordOf(scale);
        const ee::Float tolerance = 1e-05f;
        const bool unit = ee::abs(scene->scale.x - 1.0f) < tolerance && ee::abs(scene->scale.y - 1.0f) < tolerance &&
                          ee::abs(scene->scale.z - 1.0f) < tolerance;
        if (unit)
            scene->flags &= ~kSceneScaled;
        else
            scene->flags |= kSceneScaled;
        return 1;
    }
    READABLE_FUNCTION(setSceneScale, Verified);

    // setNodeCastsShadow: a node-tree walk's visitor (a model object's
    // shadow switch): the node's kNodeCastsShadow set to on's low bit.
    uint32_t setNodeCastsShadow(ee::Ptr<Node> node, uint32_t on)
    {
        if (on & 1u)
            node->drawFlags |= kNodeCastsShadow;
        else
            node->drawFlags &= ~kNodeCastsShadow;
        return 1;
    }
    READABLE_FUNCTION(setNodeCastsShadow, Verified);

    namespace
    {
        // isSceneRoot's search: the node asked about, and whether a scene
        // has it as its root.
        struct RootSearch
        {
            ee::Ptr<Node> node;
            uint32_t found;
        };

        // findModelPose's: the scene id asked for, how many matches to pass
        // over first, and the scene found.
        struct SceneIdSearch
        {
            uint32_t id;
            int32_t skip;
            ee::Ptr<Scene> found;
        };
    }

    // matchSceneRoot: a scene-pool walk's visitor: whether the scene's root
    // node is the one asked about (which stops the walk).
    uint32_t matchSceneRoot(ee::Ptr<Scene> scene, ee::Ptr<RootSearch> search)
    {
        if (sceneRootNode(scene) == search->node)
        {
            search->found = 1;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(matchSceneRoot, Verified);

    // isSceneRoot: whether some scene's root node is this one.
    uint32_t isSceneRoot(ee::Ptr<Node> node)
    {
        ee::Local<RootSearch> search;
        search->node = node;
        search->found = 0;
        poolForEach(*g_modelPosePool, sym::matchSceneRoot, search.address());
        return search->found;
    }
    READABLE_FUNCTION_WAITS(isSceneRoot, Verified);

    // matchSceneId: a scene-pool walk's visitor: a scene with the id asked
    // for is a match; the search's skip-th match is kept, which stops the
    // walk.
    uint32_t matchSceneId(ee::Ptr<Scene> scene, ee::Ptr<SceneIdSearch> search)
    {
        if (scene->id != search->id)
            return 1;
        search->skip = search->skip - 1;
        if (search->skip != -1)
            return 1;
        search->found = scene;
        return 0;
    }
    READABLE_FUNCTION(matchSceneId, Verified);

    // findModelPose: the scene with that id (skip: how many such to pass
    // over), the newest first; 0 if none.
    ee::Ptr<Scene> findModelPose(uint32_t id, int32_t skip)
    {
        ee::Local<SceneIdSearch> search;
        search->id = id;
        search->skip = skip;
        search->found = ee::Ptr<Scene>();
        poolForEach(*g_modelPosePool, sym::matchSceneId, search.address());
        return search->found;
    }
    READABLE_FUNCTION_WAITS(findModelPose, Verified);

    namespace
    {
        // A scene's bone lists (scene.h SceneBoneList; Scene::boneListCount of them from
        // Scene::boneCount, ZGE4's from the capacity before it).
        ee::Ptr<SceneBoneList> boneList(ee::Ptr<Scene> scene, uint32_t index)
        {
            return ee::Ptr<int32_t>(&boneListsStart(*scene)).cast<SceneBoneList>() + index;
        }

        // The bone of `to` at the place `bone` has in `from` (0 stays 0).
        ee::Ptr<SceneBone> sameBoneIn(ee::Ptr<SceneBone> to, ee::Ptr<SceneBone> from, ee::Ptr<SceneBone> bone)
        {
            if (!bone)
                return ee::Ptr<SceneBone>();
            return to + (bone.get() - from.get());
        }

        namespace tail
        {
            void poolFree(ee::Ptr<Pool> pool, ee::Ptr<Scene> scene) { ee::tailCall(sym::poolFree, pool, scene); }
        }
    }

    // newModelPose: a scene from its pool with room for boneCount bones,
    // at the origin, unturned and unscaled. A skinned one also gets its
    // bones' matrices and names (Scene::bonePoses, Scene::boneNames).
    ee::Ptr<Scene> newModelPose(int32_t boneCount, uint32_t skinned)
    {
        const ee::Ptr<Scene> scene(poolAlloc(*g_modelPosePool));
        memoryFill(scene, 0, sizeof(Scene));
        scene->type = kSceneType;
        quaternionIdentity(&scene->orientation);
        scene->position = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        vec3SetOne(&scene->scale.xyz());
        scene->boneCount = boneCount;
        scene->bones = allocateSceneBones(boneCount);
        if (skinned == 1)
        {
            const uint32_t poseBytes = boneCount * sizeof(BonePose);
            scene->bonePoses = ee::Ptr<BonePose>(memoryFill(heapAllocate(poseBytes), 0, poseBytes));
            const uint32_t nameBytes = boneCount * kBoneNameBytes;
            scene->boneNames = ee::Ptr<char>(memoryFill(heapAllocate(nameBytes), 0, nameBytes));
            scene->flags |= kSceneSkinned;
        }
        return scene;
    }

    // newModelPoseWithCapacity: ZGE4's newModelPose: the scene has room for
    // boneCapacity bones (Scene::boneCapacity: the bones, matrices and names
    // allocated for that many) of which boneCount are in use; ZGE2 and ZGE3
    // allocate the count.
    ee::Ptr<Scene> newModelPoseWithCapacity(int32_t boneCount, int32_t boneCapacity, uint32_t skinned)
    {
        const ee::Ptr<Scene> scene(poolAlloc(*g_modelPosePool));
        memoryFill(scene, 0, sizeof(Scene));
        scene->type = kSceneType;
        quaternionIdentity(&scene->orientation);
        scene->position = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        vec3SetOne(&scene->scale.xyz());
        scene->boneCount = boneCount;
        sceneBoneCapacity(*scene) = boneCapacity;
        scene->bones = allocateSceneBones(boneCapacity);
        if (skinned == 1)
        {
            const uint32_t poseBytes = boneCapacity * sizeof(BonePose);
            scene->bonePoses = ee::Ptr<BonePose>(memoryFill(heapAllocate(poseBytes), 0, poseBytes));
            const uint32_t nameBytes = boneCapacity * kBoneNameBytes;
            scene->boneNames = ee::Ptr<char>(memoryFill(heapAllocate(nameBytes), 0, nameBytes));
            scene->flags |= kSceneSkinned;
        }
        return scene;
    }
    READABLE_FUNCTION_WAITS_OWN(newModelPose, Verified, bmx, newModelPoseWithCapacity);

    // initPoseBones: a skinned scene's bones given their matrices and their
    // names' room, in order (the matrices reset).
    void initPoseBones(ee::Ptr<SceneBone> bones, uint32_t count, ee::Ptr<BonePose> poses, ee::Ptr<char> names)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            bones[i].matrices = skinnedBoneMatrices(poses + i);
            resetBonePose(poses + i);
            bones[i].name = names + i * kBoneNameBytes;
        }
    }
    READABLE_FUNCTION(initPoseBones, Verified);

    namespace
    {
        // count bones copied from `from` to `to`: their nodes, ids, names and bind positions, their tree links
        // pointed at the same places in `to`; no key and no matrices yet (copyPoseBones, copiedPoseBones).
        void copyBones(ee::Ptr<SceneBone> to, ee::Ptr<SceneBone> from, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                SceneBone &bone = to[i];
                const SceneBone &source = from[i];
                bone.id = source.id;
                bone.node = source.node;
                bone.key = ee::Ptr<BoneKey>();
                bone.name = source.name;
                bone.matrices = ee::Ptr<BoneMatrices>();
                bone.bindPosition = source.bindPosition;
                bone.firstChild = sameBoneIn(to, from, source.firstChild);
                bone.nextSibling = sameBoneIn(to, from, source.nextSibling);
                bone.parent = sameBoneIn(to, from, source.parent);
            }
        }
    }

    // copyPoseBones: count bones copied from `from` to `to`.
    void copyPoseBones(ee::Ptr<SceneBone> to, ee::Ptr<SceneBone> from, uint32_t count)
    {
        copyBones(to, from, count);
    }

    // copiedPoseBones: ZGE2's copyPoseBones, which makes the copy's room itself: count bones allocated and copied
    // from `from` as copyPoseBones copies them. The new bones.
    ee::Ptr<SceneBone> copiedPoseBones(ee::Ptr<SceneBone> from, uint32_t count)
    {
        const ee::Ptr<SceneBone> to = allocateSceneBones(static_cast<int32_t>(count));
        copyBones(to, from, count);
        return to;
    }
    READABLE_FUNCTION_OWN(copyPoseBones, Verified, dm2, copiedPoseBones);

    // copyBoneList: the source's bone list `index` copied into the scene's,
    // its bones on the scene's own matrices and names.
    void copyBoneList(ee::Ptr<Scene> scene, ee::Ptr<Scene> source, uint32_t index)
    {
        const ee::Ptr<SceneBoneList> to = boneList(scene, index);
        const ee::Ptr<SceneBoneList> from = boneList(source, index);
        to->takeCountsOf(*from);
        if constexpr (game::readable::kIsDm2) // (ZGE2's copy allocates the bones: the list takes its room)
            to->bones = copiedPoseBones(from->bones, from->count);
        else
            copyPoseBones(to->bones, from->bones, from->count);
        initPoseBones(to->bones, to->allocated(), scene->bonePoses, scene->boneNames);
    }
    READABLE_FUNCTION(copyBoneList, Verified);

    // cloneModelPose: a skinned copy of the scene: its flags, bones, id,
    // name and bone names, on the same node; no object yet.
    ee::Ptr<Scene> cloneModelPose(ee::Ptr<Scene> scene)
    {
        ee::Ptr<Scene> copy;
        if constexpr (game::readable::kIsBmx) // (ZGE4's newModelPose takes the room: the copy keeps the source's)
            copy = newModelPoseWithCapacity(scene->boneCount, sceneBoneCapacity(*scene), 1);
        else
            copy = newModelPose(scene->boneCount, 1);
        copy->object = ee::Ptr<SceneObject>();
        // The scene flags (the word's upper half: kSceneScaled ..) as the
        // source's, the type kept.
        constexpr uint64_t kTypeHalf = 0xFFFFFFFFull;
        copy->flags = (copy->flags & kTypeHalf) | (scene->flags & ~kTypeHalf);
        copyBoneList(copy, scene, 0);
        copy->id = scene->id;
        copy->name = scene->name;
        copy->boneListCount = scene->boneListCount;
        memoryCopy(copy->boneNames, scene->boneNames, copy->boneCount * kBoneNameBytes);
        copy->node = scene->node;
        return copy;
    }
    READABLE_FUNCTION_WAITS(cloneModelPose, Verified);

    // freeModelPose: the scene's bones, names and matrices freed, the scene
    // marked freed and given back to its pool. ZGE2 frees only the bones here
    // (a skinned scene's names and matrices are releaseScene's) and clears
    // nothing.
    void freeModelPose(ee::Ptr<Scene> scene)
    {
        freeSceneBones(scene->bones);
        if constexpr (!game::readable::kIsDm2)
        {
            scene->bones = ee::Ptr<SceneBone>();
            if (scene->boneNames)
            {
                heapFree(scene->boneNames);
                scene->boneNames = ee::Ptr<char>();
            }
            if (scene->bonePoses)
            {
                heapFree(scene->bonePoses);
                scene->bonePoses = ee::Ptr<BonePose>();
            }
        }
        scene->type = kFreedSceneType;
        tail::poolFree(*g_modelPosePool, scene);
    }
    READABLE_FUNCTION_WAITS(freeModelPose, Verified);

    // countModelNodes: a node-tree walk's visitor: a node of the bone tree
    // (kNodeBoneTree) counted, then its children.
    uint32_t countModelNodes(ee::Ptr<Node> node, ee::Ptr<int32_t> count)
    {
        if (nodeTreeFlags(*node) & kNodeBoneTree)
        {
            *count += 1;
            if (forEachChild(node, sym::countModelNodes, count) == 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(countModelNodes, Verified);

    // addModelBone: a node-tree walk's visitor: a node of the bone tree
    // takes the next of the new scene's bones (g_nextModelBone), a child of
    // parent's, then its children do the same.
    uint32_t addModelBone(ee::Ptr<Node> node, ee::Ptr<SceneBone> parent)
    {
        if (nodeTreeFlags(*node) & kNodeBoneTree)
        {
            const ee::Ptr<SceneBone> bone = *g_nextModelBone;
            *g_nextModelBone = bone + 1;
            if (*g_retainModelNodes)
                zge::retainNode(node);
            bone->node = node;
            bone->id = node->nameHash;
            bone->name = ee::Ptr<char>(node->name);
            bone->key = ee::Ptr<BoneKey>();
            bone->matrices = rigidBoneMatrices(node);
            bone->bindPosition = node->local.translation();
            bone->parent = parent;
            if (parent)
            {
                bone->nextSibling = parent->firstChild;
                parent->firstChild = bone;
            }
            if (forEachChild(node, sym::addModelBone, bone) == 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(addModelBone, Verified);

    // setupSceneModel: the scene of a model's node, made or found, left in
    // g_setupScene (0 if none). With makeBones a node that is not
    // kNodeSecondBucket gets kNodeBoneTree first; without, a node with
    // neither is left alone, and so is one some scene has as its root.
    // - kNodeBoneTree: a new rigid scene, a bone per node of the bone tree
    //   from this one (addModelBone), each node retained with retainNodes.
    // - Else kNodeSecondBucket: the MOBO scene of the node's name (a copy if
    //   that one has a node already), on this node, its matrices the node's
    //   bone poses; the node retained with retainNodes.
    void setupSceneModel(ee::Ptr<Node> node, uint32_t retainNodes, uint32_t makeBones)
    {
        *g_setupScene = ee::Ptr<Scene>();
        if (makeBones)
        {
            if (!(nodeTreeFlags(*node) & kNodeSecondBucket))
                nodeTreeFlags(*node) |= kNodeBoneTree;
        }
        else if (!(nodeTreeFlags(*node) & (kNodeBoneTree | kNodeSecondBucket)))
            return;
        // (ZGE2 has no isSceneRoot: a node some scene has as its root is set up again)
        if constexpr (!game::readable::kIsDm2)
            if (isSceneRoot(node))
                return;
        const uint32_t id = zge::nodeNameHash(node);
        const ee::Ptr<char> name = zge::nodeName(node);
        if (nodeTreeFlags(*node) & kNodeBoneTree)
        {
            ee::Local<int32_t> boneCount;
            boneCount = 0;
            if (countModelNodes(node, boneCount) == 0)
                return;
            const ee::Ptr<Scene> scene = newModelPose(*boneCount, 0);
            scene->id = id;
            scene->boneListCount = 1;
            scene->name = name;
            *g_setupScene = scene;
            *g_retainModelNodes = retainNodes;
            *g_nextModelBone = scene->bones;
            addModelBone(node, ee::Ptr<SceneBone>());
            return;
        }
        if (!(nodeTreeFlags(*node) & kNodeSecondBucket))
            return;
        ee::Ptr<Scene> scene = findModelPose(id, 0);
        if (!scene)
            return;
        if (scene->node)
            scene = cloneModelPose(scene);
        else
            *g_setupScene = scene;
        scene->node = node;
        node->bonePoses = scene->bonePoses;
        *g_setupScene = scene;
        scene->id = id;
        scene->name = name;
        scene->boneListCount = 1;
        if (retainNodes)
            zge::retainNode(node);
        scene->node->flagsDoubleword() |= kNodeBonesPosedBit;
    }

    // setupSceneModelWithOptions: ZGE4's setupSceneModel (the scene of a
    // model's node, made or found, left in g_setupScene). What changed since
    // ZGE3: the two words are option bits (kSetupMakeBones is makeBones); the
    // MOBO scene can be the one given (kSetupFromSource, cloneScene's source)
    // instead of the one of the node's name; no node is retained (addModelBone's
    // g_retainModelNodes is 0); a new scene's room is its bone count; the
    // node's name hash is its field (ZGE4 has no nodeNameHash).
    void setupSceneModelWithOptions(ee::Ptr<Node> node, uint32_t options, ee::Ptr<Scene> source)
    {
        *g_setupScene = ee::Ptr<Scene>();
        if (options & kSetupMakeBones)
        {
            if (!(nodeTreeFlags(*node) & kNodeSecondBucket))
                nodeTreeFlags(*node) |= kNodeBoneTree;
        }
        else if (!(nodeTreeFlags(*node) & (kNodeBoneTree | kNodeSecondBucket)))
            return;
        if (isSceneRoot(node))
            return;
        const uint32_t id = node->nameHash;
        const ee::Ptr<char> name = zge::nodeName(node);
        if (nodeTreeFlags(*node) & kNodeBoneTree)
        {
            ee::Local<int32_t> boneCount;
            boneCount = 0;
            if (countModelNodes(node, boneCount) == 0)
                return;
            const ee::Ptr<Scene> scene = newModelPoseWithCapacity(*boneCount, *boneCount, 0);
            scene->boneListCount = 1;
            scene->id = id;
            scene->name = name;
            *g_setupScene = scene;
            *g_retainModelNodes = 0;
            *g_nextModelBone = scene->bones;
            addModelBone(node, ee::Ptr<SceneBone>());
            return;
        }
        if (!(nodeTreeFlags(*node) & kNodeSecondBucket))
            return;
        ee::Ptr<Scene> scene = (options & kSetupFromSource) ? source : findModelPose(id, 0);
        if (!scene)
            return;
        if (scene->node)
            scene = cloneModelPose(scene);
        *g_setupScene = scene;
        scene->node = node;
        node->bonePoses = scene->bonePoses;
        scene->id = id;
        scene->name = name;
        scene->boneListCount = 1;
        scene->node->flagsDoubleword() |= kNodeBonesPosedBit;
    }
    READABLE_FUNCTION_WAITS_OWN(setupSceneModel, Verified, bmx, setupSceneModelWithOptions);

    // setupNodeSceneModel: the node's scene (setupSceneModel, its bone tree
    // made and retained).
    ee::Ptr<Scene> setupNodeSceneModel(ee::Ptr<Node> node)
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4: options, and no node retained)
            setupSceneModelWithOptions(node, kSetupMakeBones, ee::Ptr<Scene>());
        else
            setupSceneModel(node, 1, 1);
        return *g_setupScene;
    }
    READABLE_FUNCTION_WAITS(setupNodeSceneModel, Verified);

    // setupGeometrySceneModel: a geometry node's scene, if it has a model
    // (setupSceneModel without making a bone tree).
    ee::Ptr<Scene> setupGeometrySceneModel(ee::Ptr<Node> node, uint32_t retainNodes)
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4 tests retainNodes and passes no option either way: it retains none)
            setupSceneModelWithOptions(node, 0, ee::Ptr<Scene>());
        else
            setupSceneModel(node, retainNodes, 0);
        return *g_setupScene;
    }
    READABLE_FUNCTION_WAITS(setupGeometrySceneModel, Verified);

    // cloneScene: a new scene on a copy of the scene's whole node tree
    // (named as the source, with the bones' streams copied; `flags` add
    // instanceGeometryNode's), posed as the source; 0 if no copy was made.
    // ZGE2's cloneScene takes the scene alone and always clones as cloneModelScene asks (kNodeKeepsRegion, which its
    // instanceGeometryNode does not read), with no bone streams to copy; ZGE3 added the caller's flags, the clone's
    // name hash and the pose copy.
    ee::Ptr<Scene> cloneScene(ee::Ptr<Scene> scene READABLE_PARAM_EXCEPT(dm2, uint32_t flags = kNodeKeepsRegion))
    {
        const ee::Ptr<Node> top = rootNode(sceneRootNode(scene));
        const uint32_t instanceFlags = flags | kInstanceKeepName | kInstanceWithFamily | (game::readable::kIsDm2 ? 0u : kCopyBones);
        const ee::Ptr<Node> copy = zge::instanceGeometryNode(top, instanceFlags);
        if (!copy)
            return ee::Ptr<Scene>();
        if constexpr (!game::readable::kIsDm2)
            copy->nameHash = scene->id;
        if constexpr (game::readable::kIsBmx) // (ZGE4 gives the clone's scene its source, not a lookup by the name)
            setupSceneModelWithOptions(copy, kSetupFromSource, scene);
        else
            setupSceneModel(copy, 0, 0);
        const ee::Ptr<Scene> clone = *g_setupScene;
        clone->flags |= kSceneCloned;
        if constexpr (!game::readable::kIsDm2)
            copyScenePose(clone, scene);
        return clone;
    }
    READABLE_FUNCTION_WAITS(cloneScene, Verified);

    // releaseScene: the scene's nodes released (a clone's whole tree, a
    // skinned model's node, else each bone's), then the scene freed (the
    // scene pool's destroy callback).
    uint32_t releaseScene(ee::Ptr<Scene> scene)
    {
        if (scene->flags & kSceneCloned)
            zge::releaseNode(sceneRootNode(scene), 1);
        else if (scene->flags & kSceneSkinned)
        {
            zge::releaseNode(scene->node, 0);
            if constexpr (game::readable::kIsDm2)
            {
                // ZGE2 frees a skinned scene's names and matrices here, ZGE3 in freeModelPose.
                heapFree(scene->boneNames);
                heapFree(scene->bonePoses);
            }
        }
        else
            for (int32_t i = 0; i < scene->boneCount; ++i)
                zge::releaseNode(scene->bones[i].node, 0);
        freeModelPose(scene);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseScene, Verified);

    // bindSceneAnimator: the animator drives the scene: each bone of its
    // bone lists keys on the animator's channel of the bone's id (none if
    // it has no such channel), and the keys count as changed. Returns the
    // scene's flags.
    uint64_t bindSceneAnimator(ee::Ptr<Scene> scene, ee::Ptr<Animator> animator)
    {
        setSceneChanges(scene, animatorChanges(animator)); // (DM2's scenes keep no changes word)
        for (int32_t list = 0; list < scene->boneListCount; ++list)
        {
            const SceneBoneList &bones = *boneList(scene, list);
            for (int32_t i = 0; i < bones.count; ++i)
            {
                SceneBone &bone = bones.bones[i];
                bone.key = ee::Ptr<BoneKey>();
                for (uint32_t c = 0; c < animator->channelCount; ++c)
                    if (channelRecord(animator->channels, c).boneId == bone.id)
                    {
                        bone.key = channelKey(channelAt(animator->channels, c));
                        break;
                    }
            }
        }
        scene->flags |= kSceneKeysChanged;
        return scene->flags;
    }
    READABLE_FUNCTION(bindSceneAnimator, Verified);

    // changeSceneState: SceneStates switched off, then on, over the scene's
    // whole node tree: shown, casting a shadow, lit by its own lights.
    uint32_t changeSceneState(ee::Ptr<Scene> scene, uint32_t statesOff, uint32_t statesOn)
    {
        const ee::Ptr<Node> top = rootNode(sceneRootNode(scene));
        if (statesOn & kSceneShown)
            setNodeShown(top, 1, 1);
        if (statesOff & kSceneShown)
            setNodeShown(top, 0, 1);
        if (statesOn & kSceneCastsShadow)
            forEachNodeInSubtree(top, sym::setNodeCastsShadow, 1);
        if (statesOff & kSceneCastsShadow)
            forEachNodeInSubtree(top, sym::setNodeCastsShadow, 0);
        if (statesOn & kSceneOwnLights)
            top->flagsDoubleword() |= kNodeOwnLightsBit;
        if (statesOff & kSceneOwnLights)
            top->flagsDoubleword() &= ~kNodeOwnLightsBit;
        return 0;
    }
    READABLE_FUNCTION_WAITS(changeSceneState, Verified);

    // setSceneRootColour (nothing calls it in AI): the colour word of the
    // scene's root node (R, G, B, A; a node whose word is 0 casts no
    // shadow, queuePartDraw), copied from colour at any alignment. Returns
    // the node (DM2's rider returns what its copy gives).
    ee::Ptr<Node> setSceneRootColour(ee::Ptr<Scene> scene, ee::Ptr<uint8_t> colour)
    {
        const ee::Ptr<Node> node = sceneRootNode(scene);
        std::memcpy(node->colour, colour.get(), sizeof(node->colour));
        return node;
    }
    READABLE_FUNCTION(setSceneRootColour, Draft);

    namespace
    {
        // Node::drawFlags bit 21 (render/draw.cpp's kShadowViewTest): the
        // caster is tested against the view at its next queueing.
        constexpr uint32_t kNodeShadowViewTest = 1u << 21;
        constexpr uint32_t kDm2ShadowViewTestAsked = 1u << 0; // DM2's Node::shadowViewTest
        // What setSceneShadowViewTest copies: Node::shadowViewSphere and the
        // 16 bytes after it, as four doublewords.
        constexpr uint32_t kShadowViewTestBytes = 0x20;
    }

    // setSceneShadowViewTest (nothing calls it in AI): the scene's root node
    // given the volume its shadow is tested against the view with
    // (queuePartDraw's partInView), the test asked for once. Returns the
    // node.
    ee::Ptr<Node> setSceneShadowViewTest(ee::Ptr<Scene> scene, ee::Ptr<const ViewSphere> volume)
    {
        const ee::Ptr<Node> node = sceneRootNode(scene);
        node->shadowViewTestRequest() |= game::readable::perGame(kNodeShadowViewTest, kDm2ShadowViewTestAsked); // (DM2 keeps the request in a word of its own)
        std::memcpy(&node->shadowViewSphere, volume.get(), kShadowViewTestBytes);
        return node;
    }
    READABLE_FUNCTION(setSceneShadowViewTest, Draft);

    // sceneFirstMesh (nothing calls it in AI): the mesh of the first level
    // of detail of the scene's root node's model.
    ee::Ptr<ZgePart> sceneFirstMesh(ee::Ptr<Scene> scene)
    {
        const ee::Ptr<Node> node = sceneRootNode(scene);
        return framePartMesh(frameLevelPart(classModel(nodeClass(node), 0), 0));
    }
    READABLE_FUNCTION(sceneFirstMesh, Draft);

    // copySceneBoneIds (mergeSceneMeshes'; nothing calls it in AI): the ids
    // of the scene's first bones into ids, at most max of them.
    void copySceneBoneIds(ee::Ptr<Scene> scene, ee::Ptr<uint32_t> ids, uint32_t max)
    {
        const uint32_t count = scene->boneCount;
        for (uint32_t i = 0; i < count && i != max; ++i)
            ids[i] = scene->bones[i].id;
    }
    READABLE_FUNCTION(copySceneBoneIds, Draft);

    // The parts' and the material tables' functions mergeSceneMeshes calls
    // (render/part.cpp, render/terrain.cpp, render/material_load.cpp).

    namespace
    {
        // The merged scene: the template's whole tree copied (every one of
        // copyGeoObject's copy flags) with its node's name.
        constexpr uint32_t kMergeCopyFlags = kInstanceKeepName | 0xFFFFFu;
        constexpr uint32_t kMergeBones = 32; // the bone id lists' room
        // ZGE4 marks the joined mesh (the part flags' bit 0x80; no reader of it found).
        constexpr uint16_t kPartMerged = 0x80;
        // A bone addSceneBone makes from none.
        constexpr uint32_t kNameNotSet = sym::bmx::g_textNameNotSet; // "NameNotSet"
    }

    // sceneBoneAt (ZGE4 only): the scene's bone of that number (0: the top one; unchecked).
    inline ee::Ptr<SceneBone> sceneBoneAt(ee::Ptr<Scene> scene, uint32_t index)
    {
        return scene->bones + index;
    }
    READABLE_FUNCTION_ONLY(bmx, sceneBoneAt);

    // bonePoseNumber (ZGE4 only): the place of the bone's matrices among the scene's bone poses, counted from 1:
    // the number a part's bone record names its bone by (findPartBoneRecord).
    inline uint32_t bonePoseNumber(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone)
    {
        const int32_t bytes = static_cast<int32_t>(bone->matrices.address - scene->bonePoses.address);
        return static_cast<uint32_t>(bytes / static_cast<int32_t>(sizeof(BonePose)) + 1);
    }
    READABLE_FUNCTION_ONLY(bmx, bonePoseNumber);

    // addSceneBone (ZGE4 only; mergeSceneBone's): a bone more at the end of the scene's list (no node), a copy of
    // `from`'s id, place at rest and name (none: id 0, at the origin, "NameNotSet"), under `parent` (0: none), as
    // its first child. Returns the new bone.
    inline ee::Ptr<SceneBone> addSceneBone(ee::Ptr<Scene> scene, ee::Ptr<SceneBone> parent, ee::Ptr<SceneBone> from)
    {
        const uint32_t index = sceneBoneCount(scene);
        const ee::Ptr<SceneBone> bone = scene->bones + index;
        scene->boneCount = scene->boneCount + 1;
        bone->node = {};
        if (from)
        {
            bone->id = from->id;
            bone->bindPosition = from->bindPosition;
            stringCopy(bone->name, from->name);
        }
        else
        {
            bone->id = 0;
            bone->bindPosition = kZero;
            stringCopy(bone->name, ee::Address(kNameNotSet));
        }
        bone->parent = parent;
        if (parent)
        {
            bone->nextSibling = parent->firstChild;
            parent->firstChild = bone;
        }
        return bone;
    }
    READABLE_FUNCTION_ONLY(bmx, addSceneBone);

    // mergeSceneBone (ZGE4 only; mergeSceneMeshes'): the scene's bone added to the merged scene, under the merged
    // scene's bone of its parent's id, its part bone record copied into the merged scene's first mesh. Returns the
    // new bone.
    inline ee::Ptr<SceneBone> mergeSceneBone(ee::Ptr<Scene> merged, ee::Ptr<Scene> scene, ee::Ptr<SceneBone> bone)
    {
        const ee::Ptr<SceneBone> mergedParent = findSceneModelBone(merged, boneId(scene, boneParent(scene, bone)));
        const ee::Ptr<SceneBone> added = addSceneBone(merged, mergedParent, bone);
        const ee::Ptr<ZgePart> mergedMesh = sceneFirstMesh(merged);
        const ee::Ptr<ZgePart> mesh = sceneFirstMesh(scene);
        copyPartBoneRecord(mergedMesh, mesh, bonePoseNumber(scene, bone));
        return added;
    }
    READABLE_FUNCTION_ONLY(bmx, mergeSceneBone);

    // mergeSceneMeshes (nothing calls it in AI): a copy of `scene` whose
    // first mesh is rebuilt as the first meshes of `count` scenes joined
    // into one: their vertices, faces and strip groups one after another,
    // the copy's old material tables dropped for one table of all their
    // materials, each mesh's material and vertex numbers moved past the
    // meshes before it, and its skin weights' bones moved from its scene's
    // bone list to the copy's. Returns the copy.
    // ZGE4 (BMX XXX) first drops the copy's mesh's packets and gives the copy every bone of the joined scenes it
    // has not got, each one's bone record moved by how far its scene's top bone sits from the copy's bone of the
    // same id; and it marks the joined mesh (kPartMerged).
    ee::Ptr<Scene> mergeSceneMeshes(ee::Ptr<ee::Ptr<Scene>> scenes, uint32_t count, ee::Ptr<Scene> scene)
    {
        ee::Local<uint32_t[kMergeBones]> mergedBoneIds; // the copy's bones
        ee::Local<uint32_t[kMergeBones]> meshBoneIds;   // the scene being joined's

        const ee::Ptr<Scene> merged = cloneScene(scene READABLE_PARAM_EXCEPT(dm2, kMergeCopyFlags));
        const ee::Ptr<ZgePart> mesh = sceneFirstMesh(merged);
        if constexpr (game::readable::kIsBmx)
        {
            releasePartPackets(mesh);
            ee::Local<Vec4> topOffset; // the copy's top bone's record from the joined scene's
            for (uint32_t i = 0; i < count; ++i)
            {
                const ee::Ptr<Scene> joined = scenes[i];
                const ee::Ptr<SceneBone> top = sceneBoneAt(joined, 0);
                const uint32_t topId = boneId(joined, top);
                const ee::Ptr<ZgePart> joinedMesh = sceneFirstMesh(joined);
                const ee::Ptr<SceneBone> mergedTop = findSceneModelBone(merged, topId);
                if (!mergedTop)
                {
                    // (the report of a top bone the copy has not got: both scenes' bones walked, nothing printed)
                    countSceneMeshStrips(joined);
                    countSceneMeshStrips(merged);
                }
                const uint32_t mergedTopPose = bonePoseNumber(merged, mergedTop);
                const uint32_t topPose = bonePoseNumber(joined, top);
                // (ZGE4's bone record starts with its offset)
                const ee::Ptr<BoneRecord> mergedTopRecord = findPartBoneRecord(mesh, mergedTopPose);
                *topOffset = quadwordOf(mergedTopRecord) - quadwordOf(findPartBoneRecord(joinedMesh, topPose));
                for (uint32_t bone = 1; bone < sceneBoneCount(joined); ++bone)
                {
                    const ee::Ptr<SceneBone> joinedBone = sceneBoneAt(joined, bone);
                    if (findSceneModelBone(merged, boneId(joined, joinedBone)))
                        continue;
                    const ee::Ptr<SceneBone> added = mergeSceneBone(merged, joined, joinedBone);
                    Vec4 &offset = quadwordOf(findPartBoneRecord(mesh, bonePoseNumber(merged, added)));
                    offset = offset + *topOffset;
                }
            }
        }
        copySceneBoneIds(merged, ee::Ptr<uint32_t>(mergedBoneIds), kMergeBones);

        // The joined mesh's size.
        uint32_t vertexCount = 0, faceCount = 0, materialCount = 0, groupCount = 0, groupBytes = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const ee::Ptr<ZgePart> part = sceneFirstMesh(scenes[i]);
            vertexCount += part->vertexCount;
            faceCount += part->faceCount;
            materialCount += zge::materialCount(meshMaterials(part, 0));
            groupCount += partStripGroupCount(part);
            groupBytes += partStripGroupBytes(part);
        }
        allocateMeshStreams(mesh, vertexCount, faceCount, groupCount, groupBytes);
        while (materialSetCount(mesh) != 0)
            removePartMaterialTable(mesh, meshMaterials(mesh, 0));
        attachMaterialTable(mesh, newMaterialTable(materialCount));

        // Each mesh joined in, after the ones before it.
        uint32_t firstVertex = 0, firstFace = 0, firstMaterial = 0;
        ee::Ptr<StripGroup> groups = partStripGroups(mesh); // where the next mesh's strip groups go
        for (uint32_t i = 0; i < count; ++i)
        {
            const ee::Ptr<ZgePart> part = sceneFirstMesh(scenes[i]);
            copySceneBoneIds(scenes[i], ee::Ptr<uint32_t>(meshBoneIds), kMergeBones);
            const ee::Ptr<StripGroup> partGroups = partStripGroups(part);
            const uint32_t partGroupCount = partStripGroupCount(part);
            groups = copyStripGroups(partGroups, groups, partGroupCount, firstMaterial, firstVertex);
            copyFaces(part, mesh, part->faceCount, firstFace, firstMaterial, firstVertex);
            firstFace += part->faceCount;
            const uint32_t meshBoneCount = sceneBoneCount(scenes[i]);
            const uint32_t mergedBoneCount = sceneBoneCount(merged);
            copyMeshVertices(part, ee::Ptr<uint32_t>(meshBoneIds), meshBoneCount, mesh,
                                    ee::Ptr<uint32_t>(mergedBoneIds), mergedBoneCount, part->vertexCount, firstVertex);
            firstVertex += part->vertexCount;
            const ee::Ptr<MaterialTable> partMaterials = meshMaterials(part, 0);
            copyMaterialsInto(partMaterials, meshMaterials(mesh, 0), firstMaterial);
            firstMaterial += zge::materialCount(meshMaterials(part, 0));
        }
        if constexpr (game::readable::kIsBmx)
            mesh->flags |= kPartMerged;
        return merged;
    }
    READABLE_FUNCTION_WAITS(mergeSceneMeshes, Draft);

    // walkSceneBones (nothing calls it in AI but countSceneMeshStrips): every
    // bone visited depth first, from the top, on a stack of 12 in the frame;
    // the visit itself is compiled out, so it does nothing.
    void walkSceneBones(ee::Ptr<Scene> scene)
    {
        ee::Local<ee::Ptr<SceneBone>[12]> stack;
        int32_t depth = 0;
        stack[depth++] = scene->bones;
        while (depth > 0)
        {
            const ee::Ptr<SceneBone> bone = stack[--depth];
            if (bone->nextSibling)
                stack[depth++] = bone->nextSibling;
            if (bone->firstChild)
                stack[depth++] = bone->firstChild;
        }
    }
    READABLE_FUNCTION(walkSceneBones, Draft);

    // attachPoseChild: bone hung from parent, first among its children; with
    // no parent it is only marked as having none.
    void attachPoseChild(ee::Ptr<SceneBone> parent, ee::Ptr<SceneBone> bone)
    {
        bone->parent = parent;
        if (!parent)
            return;
        bone->nextSibling = parent->firstChild;
        parent->firstChild = bone;
    }
    READABLE_FUNCTION(attachPoseChild, Verified);

    namespace
    {
        // ZGE4's MOBO scene has room for this many bones (newModelPoseWithCapacity), whatever the chunk's count.
        constexpr int32_t kMoboBoneCapacity = 0x20;
    }

    // readMoboChunk (the MOBO chunk's handler): a skinned scene of the
    // chunk's bones: each one's id, name and bind position (also its local
    // matrix's translation), hung from the first earlier bone with its
    // parent's id (DM2: from the bone at its parent's place); then the bones'
    // model matrices. 1.
    // Kept bug (AI): a bone whose parent is not among the bones before it (or
    // the first bone, if it claims a parent) is hung from itself.
    uint32_t readMoboChunk(ChunkReader *reader)
    {
        ee::Local<MoboHeader> header;
        const uint32_t recordBytes = chunkDataSize(reader) - sizeof(MoboHeader);
        readChunk(reader, &*header, sizeof(MoboHeader));
        ee::Ptr<Scene> scene;
        if constexpr (game::readable::kIsBmx) // (ZGE4: room for kMoboBoneCapacity bones, every one set up)
            scene = newModelPoseWithCapacity(header->boneCount, kMoboBoneCapacity, 1);
        else
            scene = newModelPose(header->boneCount, 1);
        scene->id = header->id;
        const ee::Ptr<MoboBone> records(allocateMemory(header->boneCount * sizeof(MoboBone)));
        readChunk(reader, records.get(), recordBytes);
        const ee::Ptr<SceneBone> bones = scene->bones;
        const uint32_t setUp = game::readable::kIsBmx ? uint32_t(sceneBoneCapacity(*scene)) : uint32_t(header->boneCount);
        initPoseBones(bones, setUp, scene->bonePoses, scene->boneNames);
        for (int32_t i = 0; i < header->boneCount; ++i)
        {
            const ee::Ptr<MoboBone> record = records + i;
            const ee::Ptr<SceneBone> bone = bones + i;
            bone->node = ee::Ptr<Node>();
            bone->id = record->id;
            bone->key = ee::Ptr<BoneKey>();
            bone->matrices->local.translation() = Vec4{record->x, record->y, record->z, 1.0f};
            stringCopyN(bone->name, record->name, sizeof(record->name) - 1);
            bone->bindPosition = Vec4{record->x, record->y, record->z, 1.0f};
            attachPoseChild(record->parentAmong(bones, i), bone);
        }
        updateBoneModelMatrices(scene);
        freeMemory(records);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readMoboChunk, Verified);

    // registerMoboChunkHandler: the scene pool made (a freed scene's
    // nodes released) and the MOBO chunk (a skinned model's bones) read
    // by readMoboChunk.
    namespace
    {
        constexpr uint32_t kModelPosesABlock = game::readable::perGame(0x100u, 0x100u, 0x40u); // ZGE4 keeps fewer a block
    }

    uint32_t registerMoboChunkHandler()
    {
        *g_modelPosesReady = 1;
        *g_modelPosePool = ee::Ptr<Pool>(createPool(sizeof(Scene), kModelPosesABlock, sym::releaseScene));
        addChunkHandler(chunkTag("MOBO"), sym::readMoboChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerMoboChunkHandler, Verified);
}
