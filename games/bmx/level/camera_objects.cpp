// BMX XXX's "Camera" level object type: a camera the level moves along a
// path, looking at a node, which shows the first player's view while the
// game has camera objects switched on. BMX XXX's own code (its game block);
// no other build names its functions, so the names are from what each does.
//
// The object's own tag data holds its path follower (0 until first used),
// its path's name, its speed and what it looks at: a node's name in the
// level's file, the node itself once the camera is set up.

#include "level_calls.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/player_riders.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/math/types.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/camera_objects.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The camera's own tag data (objectComponent), 0x10 bytes.
        struct CameraObjectTags
        {
            uint32_t follower; // +0x00 its path follower, 0 until it is set up
            uint32_t pathName; // +0x04 the name hash of the path it follows
            ee::Float speed;   // +0x08 how fast it follows it
            uint32_t target;   // +0x0C what it looks at: a node's name hash, then the node (0: nothing)
        };
        static_assert(sizeof(CameraObjectTags) == 0x10);
        constexpr uint32_t kTagWord = 8; // the tag accessors' second argument (the value's kind)

        // What a camera object keeps in a saved level state.
        struct CameraObjectSave
        {
            uint32_t pathName;     // +0x00 the name hash of the path segment its follower is on
            uint32_t endMode;      // +0x04 the follower's
            ee::Float speed;       // +0x08 the follower's
            ee::Float along;       // +0x0C the follower's
            CameraObjectTags tags; // +0x10 its tag data, the target as a node's name hash
        };
        static_assert(sizeof(CameraObjectSave) == 0x20 && offsetof(CameraObjectSave, tags) == 0x10);

        constexpr uint32_t kTypeName = sym::cameraObjectTypeName; // "Camera"
        constexpr uint32_t kCreateEntity = sym::createEntity;
        // newPathFollower's end mode for a camera's follower: the engine's
        // FollowerEndMode (zge/scene/follower.cpp), on from the path's other end.
        constexpr uint32_t kFollowerLoops = 1u << 1;

        // Non-zero while camera objects show the first player's view.
        inline ee::Global<uint32_t, sym::g_cameraObjectsOn> g_cameraObjectsOn;
        // The first player's camera mode before a camera object took the
        // view, kNoSavedMode while none has.
        inline ee::Global<uint32_t, sym::g_cameraModeBeforeObject> g_cameraModeBeforeObject;
        constexpr uint32_t kNoSavedMode = 0xFFFFFFFFu;

        // A camera object's view: a quarter turn wide.
        constexpr zge::Angle kCameraObjectFov = 0x4000;
        // The camera mode in which other code places the view.
        constexpr uint32_t kHeldCameraMode = static_cast<uint32_t>(CameraMode::Held);
    }

    ee::Ptr<zge::Follower> cameraObjectFollower(ee::Ptr<zge::SceneObject> camera);
    void setCameraObjectFollower(ee::Ptr<zge::SceneObject> camera, ee::Ptr<zge::Follower> follower);
    void setCameraObjectTarget(ee::Ptr<zge::SceneObject> camera, ee::Ptr<zge::Node> node);
    uint32_t initCameraObject(ee::Ptr<zge::SceneObject> camera);

    // cameraObjectPathName: the name hash of the path it follows.
    uint32_t cameraObjectPathName(ee::Ptr<zge::SceneObject> camera)
    {
        return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(camera), kTagWord, offsetof(CameraObjectTags, pathName)));
    }
    READABLE_FUNCTION(cameraObjectPathName, Draft);

    // cameraObjectSpeed: how fast it follows its path.
    ee::Float cameraObjectSpeed(ee::Ptr<zge::SceneObject> camera)
    {
        return zge::tagFloat(zge::objectComponent(camera), kTagWord, offsetof(CameraObjectTags, speed));
    }
    READABLE_FUNCTION(cameraObjectSpeed, Draft);

    // cameraObjectFollower: its path follower, 0 before it is set up.
    ee::Ptr<zge::Follower> cameraObjectFollower(ee::Ptr<zge::SceneObject> camera)
    {
        return ee::Ptr<zge::Follower>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(camera), kTagWord, offsetof(CameraObjectTags, follower))));
    }
    READABLE_FUNCTION(cameraObjectFollower, Draft);

    // setCameraObjectFollower.
    void setCameraObjectFollower(ee::Ptr<zge::SceneObject> camera, ee::Ptr<zge::Follower> follower)
    {
        game::tail(zge::setTagWord, zge::objectComponent(camera), kTagWord, static_cast<uint32_t>(offsetof(CameraObjectTags, follower)), ee::Address(follower));
    }
    READABLE_FUNCTION(setCameraObjectFollower, Draft);

    // cameraObjectTargetName: what it looks at, as its tag data has it (a
    // node's name hash until the camera is set up).
    uint32_t cameraObjectTargetName(ee::Ptr<zge::SceneObject> camera)
    {
        return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(camera), kTagWord, offsetof(CameraObjectTags, target)));
    }
    READABLE_FUNCTION(cameraObjectTargetName, Draft);

    // setCameraObjectTarget: the node it looks at (0: none).
    void setCameraObjectTarget(ee::Ptr<zge::SceneObject> camera, ee::Ptr<zge::Node> node)
    {
        game::tail(zge::setTagWord, zge::objectComponent(camera), kTagWord, static_cast<uint32_t>(offsetof(CameraObjectTags, target)), ee::Address(node));
    }
    READABLE_FUNCTION(setCameraObjectTarget, Draft);

    // cameraObjectTarget: the node it looks at, once set up (the same word
    // as cameraObjectTargetName reads).
    ee::Ptr<zge::Node> cameraObjectTarget(ee::Ptr<zge::SceneObject> camera)
    {
        return ee::Ptr<zge::Node>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(camera), kTagWord, offsetof(CameraObjectTags, target))));
    }
    READABLE_FUNCTION(cameraObjectTarget, Draft);

    // setUpCameraObject: a camera with no follower yet gets one on its
    // path, at its speed, put at the point of the path nearest the object;
    // then the node its target names is found and kept (none found: no
    // target). A camera that already has its follower is left alone unless
    // `again` is set (a restore: the target is a name once more).
    void setUpCameraObject(ee::Ptr<zge::SceneObject> camera, uint32_t again)
    {
        if (cameraObjectFollower(camera))
        {
            if (again == 0)
                return;
        }
        else
        {
            const ee::Ptr<zge::PathRecord> path = zge::findPathRecord(cameraObjectPathName(camera));
            const ee::Float speed = cameraObjectSpeed(camera);
            const ee::Ptr<zge::Follower> follower = zge::newPathFollower(path, kFollowerLoops, speed, ee::Float(0.0f));
            setCameraObjectFollower(camera, follower);
            zge::followerSnap(follower, zge::objectPosition(camera));
        }
        const uint32_t targetName = cameraObjectTargetName(camera);
        if (targetName == 0)
            return;
        const ee::Ptr<zge::Node> node = zge::findWorldNodeByHash(targetName, 0, 0);
        if (node)
            game::tail(setCameraObjectTarget, camera, node);
        else
            game::tail(setCameraObjectTarget, camera, ee::Ptr<zge::Node>());
    }
    READABLE_FUNCTION_WAITS(setUpCameraObject, Draft);

    // registerCameraObjectType: the "Camera" type, made as a plain entity
    // and set up by initCameraObject.
    uint32_t registerCameraObjectType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initCameraObject));
    }
    READABLE_FUNCTION(registerCameraObjectType, Verified);

    // initCameraObject (the type's init): its messages subscribed; not
    // drawn. Answers 0.
    uint32_t initCameraObject(ee::Ptr<zge::SceneObject> camera)
    {
        zge::subscribe(camera, zge::kUpdateMessage, zge::MessageHandler(sym::updateCameraObject));
        zge::subscribe(camera, zge::kRemovedMessage, zge::MessageHandler(sym::cameraObjectRemoved));
        zge::subscribe(camera, zge::kSaveSizeMessage, zge::MessageHandler(sym::cameraObjectSaveSize));
        zge::subscribe(camera, zge::kSaveMessage, zge::MessageHandler(sym::saveCameraObject));
        zge::subscribe(camera, zge::kRestoreMessage, zge::MessageHandler(sym::restoreCameraObject));
        zge::setSceneObjectShown(camera, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(initCameraObject, Draft);

    // cameraObjectRemoved (the engine's removed message): its follower deleted.
    int32_t cameraObjectRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        zge::deletePathFollower(cameraObjectFollower(ee::Ptr<zge::SceneObject>(message->target)));
        return 0;
    }
    READABLE_FUNCTION_WAITS(cameraObjectRemoved, Draft);

    // updateCameraObject (the frame's update; the data is the frame's
    // seconds): the follower moves on, and the object takes its place and
    // its heading, upright. While camera objects are on, the first
    // player's camera is put in the mode that shows a given view (its mode
    // before kept) and given this one: from the object, towards its target
    // (the origin without one), a quarter turn wide. Once they are off, the
    // camera gets its mode back.
    int32_t updateCameraObject(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> up;
        ee::Local<zge::Matrix> placement;
        ee::Local<zge::Vec4> lookAt;
        ee::Local<CameraView> view;
        const ee::Ptr<zge::SceneObject> camera(message->target);
        setUpCameraObject(camera, 0);
        const ee::Ptr<zge::Follower> follower = cameraObjectFollower(camera);
        zge::advanceFollower(follower, *ee::Ptr<ee::Float>(message->data));
        up->y = ee::Float(1.0f);
        up->x = ee::Float(0.0f);
        up->z = ee::Float(0.0f);
        zge::followerOrientation(follower, ee::Ptr<const zge::Vec3>(up.address()), placement);
        const ee::Ptr<zge::Node> target = cameraObjectTarget(camera);
        if (target)
            *lookAt = zge::nodeMatrix(target, 1)->translation();
        else
            *lookAt = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        zge::setObjectRotationMatrix(camera, placement);
        zge::setObjectPosition(camera, &placement->translation());
        zge::updatePose(camera);

        const ee::Ptr<ActionCamera> playerView = playerActionCamera(0);
        if (*g_cameraObjectsOn != 0)
        {
            if (*g_cameraModeBeforeObject == kNoSavedMode)
                *g_cameraModeBeforeObject = cameraMode(playerView, 0);
            setCameraMode(playerView, 0, kHeldCameraMode);
            view->fov = kCameraObjectFov;
            view->eye = *zge::objectPosition(camera);
            view->lookAt = *lookAt;
            view->axes = *placement;
            setCameraView(playerView, view);
        }
        else if (*g_cameraModeBeforeObject != kNoSavedMode)
        {
            setCameraMode(playerView, 0, *g_cameraModeBeforeObject);
            *g_cameraModeBeforeObject = kNoSavedMode;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateCameraObject, Draft);

    // cameraObjectSaveSize (the engine's save size message).
    int32_t cameraObjectSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(CameraObjectSave);
        return 0;
    }
    READABLE_FUNCTION(cameraObjectSaveSize, Draft);

    // saveCameraObject (the engine's save message): where its follower is
    // on which path, and its tag data with the target as a name again.
    int32_t saveCameraObject(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<CameraObjectSave> saved;
        const ee::Ptr<zge::SceneObject> camera(message->target);
        setUpCameraObject(camera, 0);
        const ee::Ptr<zge::Follower> follower = cameraObjectFollower(camera);
        saved->endMode = zge::followerEndMode(follower);
        saved->pathName = zge::pathNameHash(zge::followerSegment(follower));
        saved->along = zge::followerAlong(follower);
        saved->speed = zge::followerSpeed(follower);
        zge::memoryCopy(&saved->tags, zge::objectComponent(camera), sizeof(CameraObjectTags));
        if (saved->tags.target != 0)
            saved->tags.target = ee::Ptr<zge::Node>(saved->tags.target)->nameHash;
        zge::memoryCopy(message->data, saved, sizeof(CameraObjectSave));
        return 0;
    }
    READABLE_FUNCTION_WAITS(saveCameraObject, Draft);

    // restoreCameraObject (the engine's restore message): its follower put
    // back on the saved path, its tag data copied back with the follower
    // it has, and its target found again.
    int32_t restoreCameraObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> camera(message->target);
        const ee::Ptr<zge::Follower> follower = cameraObjectFollower(camera);
        const ee::Ptr<CameraObjectSave> saved(message->data);
        zge::setFollowerSegment(follower, zge::findPathRecord(saved->pathName));
        zge::setFollowerAlong(follower, saved->along);
        zge::setFollowerSpeed(follower, saved->speed);
        zge::setFollowerEndMode(follower, saved->endMode);
        zge::memoryCopy(zge::objectComponent(camera), &saved->tags, sizeof(CameraObjectTags));
        setCameraObjectFollower(camera, follower);
        setUpCameraObject(camera, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreCameraObject, Draft);
}
