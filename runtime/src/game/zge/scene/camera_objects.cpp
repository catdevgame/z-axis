// Camera objects (zobj's camera class, g_cameraClass): a scene object whose
// scene word holds a camera record (newSceneRecord) where a model's holds
// its scene. The class's hooks put the object's place into the camera.

#include "scene.h"

#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/scene/animation.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/zge/scene/camera_objects.cpp.h"

namespace zge
{

    namespace
    {
        // A new camera's: kind 3 for initSceneObject, a 55 degree field of
        // view (16-bit angle units), looking from g_newCameraFrom to
        // g_newCameraAt.
        // (ZGE4's kinds are numbered anew: its camera is kind 5.)
        constexpr uint32_t kCameraObjectKind = game::readable::perGame(3u, 3u, 5u);
        // ZGE2 gives each camera object a cleared 0x130-byte record of matrices of its own, from this pool
        // (pose.cpp makes it), where other objects keep their animator.
        inline ee::Global<ee::Ptr<Pool>, sym::dm2::g_cameraObjectMatricesPool> g_cameraObjectMatricesPool;
        constexpr uint32_t kNewCameraFieldOfView = 10012;
        inline ee::Global<Vec4, sym::g_newCameraFrom> g_newCameraFrom;
        inline ee::Global<Vec4, sym::g_newCameraAt> g_newCameraAt;
        inline ee::Global<Vec4, sym::g_newSceneCameraFrom> g_newSceneCameraFrom;
        inline ee::Global<Vec4, sym::g_newSceneCameraAt> g_newSceneCameraAt;

        ee::Ptr<Camera> cameraOf(ee::Ptr<SceneObject> object) { return object->scene.cast<Camera>(); }
        void setCameraOf(ee::Ptr<SceneObject> object, ee::Ptr<Camera> camera) { object->scene = camera.cast<Scene>(); }

        // A camera keeps its matrices as a MatrixCache does: matrix 1 is
        // where it is.
        ee::Ptr<MatrixCache> matricesOf(ee::Ptr<Camera> camera) { return camera.cast<MatrixCache>(); }

        // ZGE2's camera object's own record of its place and matrices (where other objects keep their animator).
        ee::Ptr<CameraObjectMatrices> placeRecordOf(ee::Ptr<SceneObject> object) { return object->animator.cast<CameraObjectMatrices>(); }
    }

    // destroyCameraObject: the class's destroy hook: the camera freed
    // (ZGE4: the object's animator first).
    uint32_t destroyCameraObject(ee::Ptr<SceneObject> object)
    {
        if constexpr (game::readable::kIsBmx)
            freeObjectAnimator(object);
        freeSceneRecord(cameraOf(object));
        if constexpr (game::readable::kIsDm2) // ZGE2: the object's record of its place handed back
            poolFree(*g_cameraObjectMatricesPool, placeRecordOf(object));
        return 1;
    }
    READABLE_FUNCTION(destroyCameraObject, Verified);

    // updateObjectCamera: the update hook: the camera put where the object
    // is.
    uint32_t updateObjectCamera(ee::Ptr<SceneObject> object)
    {
        setCameraMatrix(cameraOf(object), objectMatrix(object, 1));
        return 1;
    }
    READABLE_FUNCTION(updateObjectCamera, Verified);

    // setCameraObjectPosition: the moved hook: the camera moved there,
    // turned as it was.
    uint32_t setCameraObjectPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        ee::Local<Matrix> matrix;
        matrix = *cachedMatrix(matricesOf(cameraOf(object)), 1);
        matrix->translation() = quadwordOf(position);
        setCameraMatrix(cameraOf(object), matrix);
        return 1;
    }

    // storeCameraObjectPosition: ZGE2's setCameraObjectPosition: the place
    // kept in the object's record, its matrices to be made again
    // (objectMatrix makes them from it).
    uint32_t storeCameraObjectPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        CameraObjectMatrices &record = *placeRecordOf(object);
        record.position = quadwordOf(position);
        record.valid = 0;
        return 1;
    }
    READABLE_FUNCTION_OWN(setCameraObjectPosition, Verified, dm2, storeCameraObjectPosition);

    // setCameraObjectOrientation: the rotated hook: the camera turned so,
    // where it was.
    uint32_t setCameraObjectOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        ee::Local<Matrix> matrix;
        quaternionToMatrix(&*matrix, orientation);
        matrix->translation() = cachedMatrix(matricesOf(cameraOf(object)), 1)->translation();
        setCameraMatrix(cameraOf(object), matrix);
        return 1;
    }

    // storeCameraObjectOrientation: ZGE2's setCameraObjectOrientation: the
    // turn kept in the object's record, its matrices to be made again.
    uint32_t storeCameraObjectOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        quaternionCopy(&placeRecordOf(object)->orientation, orientation);
        placeRecordOf(object)->valid = 0;
        return 1;
    }
    READABLE_FUNCTION_OWN(setCameraObjectOrientation, Verified, dm2, storeCameraObjectOrientation);

    // setFov: the camera's field of view (a 16-bit angle); the old one.
    uint32_t setFov(ee::Ptr<SceneObject> object, uint32_t fieldOfView)
    {
        return game::tail(setCameraFieldOfView, cameraOf(object), static_cast<uint32_t>(static_cast<int16_t>(fieldOfView)));
    }
    READABLE_FUNCTION(setFov, Verified);

    // newCamera: a camera object (0 if there is no room), looking from
    // g_newCameraFrom at g_newCameraAt.
    ee::Ptr<SceneObject> newCamera()
    {
        ee::Ptr<SceneObject> object;
        if constexpr (game::readable::kIsBmx) // ZGE4's initSceneObject takes the kind alone and makes the object
        {
            object = newObjectOfKind(kCameraObjectKind);
            if (!object)
                return ee::Ptr<SceneObject>();
        }
        else
        {
            object = ee::Ptr<SceneObject>(zge::newSceneObject());
            if (!object)
                return ee::Ptr<SceneObject>();
            zge::initSceneObject(object, kCameraObjectKind);
        }
        object->objectClass = ee::Ptr<SceneObjectClass>(sym::g_cameraClass);
        const ee::Ptr<Camera> camera = newSceneRecord(kCameraKindView, 0, object READABLE_PARAM_EXCEPT(dm2, 0));
        setCameraOf(object, camera);
        if (!camera)
        {
            deleteSceneObject(object);
            return ee::Ptr<SceneObject>();
        }
        if constexpr (game::readable::kIsDm2)
        {
            const uint32_t data = poolAlloc(*g_cameraObjectMatricesPool);
            object->animator = ee::Ptr<Animator>(data);
            memoryFill(data, 0, kCameraObjectMatricesBytes);
        }
        setFov(object, kNewCameraFieldOfView);
        lookFromAt(object, g_newCameraFrom.get(), g_newCameraAt.get(), 0);
        return object;
    }
    READABLE_FUNCTION_WAITS(newCamera, Verified);

    namespace
    {
        // newSceneCameraWith's record kind: 1 (renderFrame's walks take
        // kind 0, the 2D pass kind 2; none in this build takes 1).
        constexpr uint32_t kOwnedCameraKind = 1;

    }

    // newSceneCameraWith (nothing calls it in AI): a camera object as
    // newCamera makes one, its record of kind 1 and belonging to owner,
    // looking from g_newSceneCameraFrom at g_newSceneCameraAt. 0 if there
    // is no room.
    ee::Ptr<SceneObject> newSceneCameraWith(uint32_t owner)
    {
        const ee::Ptr<SceneObject> object(zge::newSceneObject());
        if (!object)
            return ee::Ptr<SceneObject>();
        zge::initSceneObject(object, kCameraObjectKind);
        object->objectClass = ee::Ptr<SceneObjectClass>(sym::g_cameraClass);
        const ee::Ptr<Camera> camera = newSceneRecord(kOwnedCameraKind, 0, object READABLE_PARAM_EXCEPT(dm2, owner));
        setCameraOf(object, camera);
        if (!camera)
        {
            deleteSceneObject(object);
            return ee::Ptr<SceneObject>();
        }
        setFov(object, kNewCameraFieldOfView);
        lookFromAt(object, g_newSceneCameraFrom.get(), g_newSceneCameraAt.get(), 0);
        return object;
    }
    READABLE_FUNCTION_WAITS(newSceneCameraWith, Draft);

    // setSceneCameraViewport (nothing calls it in AI): the camera object's
    // screen rectangle (setCameraViewport, a tail call).
    uint32_t setSceneCameraViewport(ee::Ptr<SceneObject> object, ee::Address rectangle)
    {
        return game::tail(setCameraViewport, cameraOf(object), rectangle);
    }
    READABLE_FUNCTION(setSceneCameraViewport, Draft);
}
