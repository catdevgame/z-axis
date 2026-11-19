// BMX XXX's path men (pathman.h): the two level object types, "PathMan" and
// "FastPathMan", and the messages a path man takes: the engine's (update,
// removed, velocity, save and restore) and the level's (enable, disable,
// pause, resume, touched, a rider's trick or crash). BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_phrases.h"
#include "pathman_random.h"
#include "pathman_tags.h"

#include "bmx/game/player_riders.h"
#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/debug/debug.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/pathman/pathman_object.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::collision::nodeHandle;
    using rider::vec4;

    namespace
    {
        constexpr uint32_t kTypeName = sym::pathManRegisteredName;         // "PathMan"
        constexpr uint32_t kFastTypeName = sym::fastPathManRegisteredName; // "FastPathMan"
        constexpr uint32_t kCreateEntity = sym::createEntity;        // the level's plain entity maker

        // The objectives' icon models (the objectives manager keeps two):
        // the first, "icon.NPR_Available", is what a path man with an
        // objective to give has a copy of over its head.
        constexpr uint32_t kObjectiveIcons = 2;
        inline ee::Global<ee::Ptr<SceneObject>[kObjectiveIcons], sym::g_objectiveIcons> g_objectiveIcons;
        inline ee::Ptr<SceneObject> availableIcon() { return (*g_objectiveIcons)[0]; }
        // How much the game draws for debugging: from 2 on, a line from each
        // path man to the rider.
        inline ee::Global<uint8_t, sym::g_debugDisplay> g_debugDisplay;
        inline ee::Global<uint32_t, sym::pathManDebugColour> g_debugColour;
        // The pose hooks a saved path man can name, by their place here.
        constexpr uint32_t kPoseHooks = 2;
        inline ee::Global<PathManPoseHook[kPoseHooks], sym::pathManPoseHooks> g_poseHooks;

        // How far over the path man's bounding sphere the marker floats.
        constexpr ee::Float kMarkerGap = 0.5f;

        // What a path man keeps in a saved level state.
        struct PathManSave
        {
            uint8_t tags[0xC4];      // +0x00 its tag record, whole
            uint32_t path;           // +0xC4 the path it is on (a path's name hash, a fast path's id), 0: none
            ee::Float along;         // +0xC8 where on it (a fast path: the time)
            ee::Float speed;         // +0xCC (a fast path: the rate)
            uint32_t endMode;        // +0xD0 the follower's end mode (a fast follower: its word)
            ee::Float topSpeed;      // +0xD4
            ee::Float speedWanted;   // +0xD8
            ee::Float animationTime; // +0xDC
            char animationName[0x40]; // +0xE0 empty: no animation
            uint32_t playMode;       // +0x120
            uint32_t poseHook;       // +0x124 which of g_poseHooks
            ee::Float speedScale;    // +0x128
        };
        static_assert(sizeof(PathManSave) == 0x12C && offsetof(PathManSave, path) == 0xC4 &&
                      offsetof(PathManSave, animationName) == 0xE0 && offsetof(PathManSave, playMode) == 0x120);

        // The messages a path man takes only while it is enabled.
        void subscribeWhileEnabled(ee::Ptr<SceneObject> pathMan, bool withVelocity)
        {
            subscribe(pathMan, kUpdateMessage, MessageHandler(sym::updatePathMan));
            if (withVelocity)
                subscribe(pathMan, kVelocityMessage, MessageHandler(sym::pathManVelocity));
            subscribe(pathMan, pathman::kPauseMessage, MessageHandler(sym::pausePathMan));
            subscribe(pathMan, pathman::kResumeMessage, MessageHandler(sym::resumePathMan));
            subscribe(pathMan, pathman::kRiderDidMessage, MessageHandler(sym::pathManRiderDid));
        }
        void unsubscribeWhileDisabled(ee::Ptr<SceneObject> pathMan, bool withVelocity)
        {
            unsubscribe(pathMan, kUpdateMessage, MessageHandler(sym::updatePathMan));
            if (withVelocity)
                unsubscribe(pathMan, kVelocityMessage, MessageHandler(sym::pathManVelocity));
            unsubscribe(pathMan, pathman::kPauseMessage, MessageHandler(sym::pausePathMan));
            unsubscribe(pathMan, pathman::kResumeMessage, MessageHandler(sym::resumePathMan));
            unsubscribe(pathMan, pathman::kRiderDidMessage, MessageHandler(sym::pathManRiderDid));
        }

        // The path man's follower deleted (as freePathManRecord does).
        void deleteFollower(ee::Ptr<PathManRecord> record)
        {
            if (record->travel.follower == 0)
                return;
            if (record->travel.fast != 0)
                zge::deleteFastFollower(ee::Ptr<zge::FastFollower>(record->travel.follower));
            else
                deletePathFollower(ee::Ptr<Follower>(record->travel.follower));
            record->travel.follower = 0;
        }
    }

    // registerPathManTypes: both types are made as plain entities; each has
    // its own init.
    uint32_t registerPathManTypes()
    {
        registerLevelObjectType(kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initPathMan));
        return game::tail(registerLevelObjectType, kFastTypeName, kCreateEntity, static_cast<uint32_t>(sym::initFastPathMan));
    }
    READABLE_FUNCTION(registerPathManTypes, Verified);

    // subscribePathMan: riders pass through a path man; its messages are
    // subscribed, those of an enabled one only when it is not disabled in
    // the level's data (it is hidden then). Always 1.
    uint32_t subscribePathMan(ee::Ptr<SceneObject> pathMan)
    {
        rider::collision::setNodeCollidable(nodeHandle(ee::Address(objectLevelNode(pathMan))), 0);
        subscribe(pathMan, kRemovedMessage, MessageHandler(sym::pathManRemoved));
        subscribe(pathMan, kSaveSizeMessage, MessageHandler(sym::pathManSaveSize));
        subscribe(pathMan, kSaveMessage, MessageHandler(sym::savePathMan));
        subscribe(pathMan, pathman::kTouchedMessage, MessageHandler(sym::pathManTouched));
        subscribe(pathMan, pathman::kEnableMessage, MessageHandler(sym::enablePathMan));
        subscribe(pathMan, pathman::kDisableMessage, MessageHandler(sym::disablePathMan));
        subscribe(pathMan, pathman::kIsEnabledMessage, MessageHandler(sym::pathManIsEnabled));
        if (tagByte(objectComponent(pathMan), kPathManTagBytes, kPathManTagDisabled) == 0)
            subscribeWhileEnabled(pathMan, true);
        else
            setSceneObjectShown(pathMan, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(subscribePathMan, Verified);

    // initPathMan (the "PathMan" type's init): no marker yet; put on the
    // path its data names.
    uint32_t initPathMan(ee::Ptr<SceneObject> pathMan)
    {
        setTagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker, 0u);
        pathManTravel(pathMan)->fast = 0;
        putPathManOnPath(pathMan, static_cast<uint32_t>(pathManPath(pathMan)));
        subscribe(pathMan, kRestoreMessage, MessageHandler(sym::restorePathMan));
        return subscribePathMan(pathMan);
    }
    READABLE_FUNCTION_WAITS(initPathMan, Verified);

    // initFastPathMan (the "FastPathMan" type's init): the same on a fast
    // path.
    uint32_t initFastPathMan(ee::Ptr<SceneObject> pathMan)
    {
        setTagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker, 0u);
        pathManTravel(pathMan)->fast = 1;
        putPathManOnFastPath(pathMan, static_cast<uint32_t>(pathManPath(pathMan)));
        subscribe(pathMan, kRestoreMessage, MessageHandler(sym::restoreFastPathMan));
        return subscribePathMan(pathMan);
    }
    READABLE_FUNCTION_WAITS(initFastPathMan, Verified);

    // pathManRemoved (the engine's removed message): its record and its
    // marker freed.
    int32_t pathManRemoved(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> pathMan(message->target);
        freePathManRecord(pathMan);
        const ee::Ptr<Scene> marker(tagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker));
        if (marker)
            releaseScene(marker);
        return 0;
    }
    READABLE_FUNCTION_WAITS(pathManRemoved, Draft);

    // updatePathMan (the frame's update; the data points to the frame's
    // time): the riders are measured; a path man out of play goes back to
    // its path, one in play decides what to do (unless its animation has
    // just ended) and moves along its path. Then its marker: made the first
    // time for a path man that names an objective, shown over its head while
    // the current player may activate the objective and hidden otherwise.
    int32_t updatePathMan(ee::Ptr<EngineMessage> message)
    {
        ee::Local<Vec4> markerPosition;
        const ee::Ptr<SceneObject> pathMan(message->target);
        const ee::Ptr<ee::Float> frameTime(message->data);
        watchRiders(pathMan);
        if (pathManOutOfPlay(pathMan) != 0 && pathManActsUnseen(pathMan) == 0)
        {
            sendPathManBackToPath(pathMan);
        }
        else
        {
            if (pathManAnimationEnded(pathMan) == 0)
                decidePathMan(pathMan);
            movePathMan(pathMan, 0, *frameTime);
            if (*g_debugDisplay >= 2)
                drawPathManDebug(pathMan);
        }

        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        ee::Ptr<Scene> marker(tagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker));
        if (!marker)
        {
            if (availableIcon() && record->objectiveName[0] != 0)
            {
                marker = cloneScene(sceneOf(availableIcon()), 0);
                setTagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker, marker);
            }
            if (!marker)
                return 0;
        }
        if (record->objective && objectives::playerCanActivateObjective(currentPlayer(), record->objective) != 0)
        {
            copyScenePose(marker, sceneOf(availableIcon()));
            const ee::Ptr<Node> node = objectLevelNode(pathMan);
            const ee::Ptr<Node> markerNode = boneNode(marker, rootBone(marker));
            // Over the top of the path man's bounding sphere, clear of the
            // marker's own.
            *markerPosition = node->sphereCentre - node->origin;
            markerPosition->y = markerPosition->y - (node->radius + markerNode->radius) + kMarkerGap;
            setScenePosition(marker, markerPosition);
            poseScene(marker);
            changeSceneState(marker, 0, kSceneShown);
        }
        else
        {
            changeSceneState(marker, kSceneShown, 0);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(updatePathMan, Verified);

    // pathManVelocity (the engine's velocity message): the way it faces at
    // the speed of its follower. Not answered (0) with no follower.
    int32_t pathManVelocity(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<PathManTravel> travel = pathManTravel(ee::Ptr<SceneObject>(message->target));
        if (travel->follower == 0)
            return 0;
        const ee::Float speed = ee::abs(travel->fast != 0 ? zge::fastFollowerRate(ee::Ptr<zge::FastFollower>(travel->follower))
                                                         : followerSpeed(ee::Ptr<Follower>(travel->follower)));
        const ee::Ptr<Vec4> velocity(message->data);
        pathManFacing(ee::Ptr<SceneObject>(message->target), velocity);
        *velocity = *velocity * speed;
        return 1;
    }
    READABLE_FUNCTION_WAITS(pathManVelocity, Draft);

    // pathManSaveSize (the engine's save size message).
    int32_t pathManSaveSize(ee::Ptr<EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(PathManSave);
        return 0;
    }
    READABLE_FUNCTION(pathManSaveSize, Verified);

    // savePathMan (the engine's save message): its tags, where it is on its
    // path and its animation. What a path man has not got (no follower, no
    // animation) is left as the save's room held it.
    int32_t savePathMan(ee::Ptr<EngineMessage> message)
    {
        ee::Local<PathManSave> save;
        memoryCopy(save, objectComponent(ee::Ptr<SceneObject>(message->target)), sizeof(save->tags));
        const ee::Ptr<PathManRecord> record = pathManRecord(ee::Ptr<SceneObject>(message->target));
        if (record->travel.follower == 0)
        {
            save->path = 0;
        }
        else
        {
            if (record->travel.fast != 0)
            {
                const ee::Ptr<zge::FastFollower> follower(record->travel.follower);
                save->path = zge::fastPathId(zge::fastFollowerPath(follower));
                save->along = zge::fastFollowerTime(follower);
                save->speed = zge::fastFollowerRate(follower);
                save->endMode = zge::fastFollowerEndMode(follower);
            }
            else
            {
                const ee::Ptr<Follower> follower(record->travel.follower);
                save->path = pathNameHash(followerSegment(follower));
                save->along = followerAlong(follower);
                save->speed = followerSpeed(follower);
                save->endMode = zge::followerEndMode(follower);
            }
            save->topSpeed = record->travel.topSpeed;
            save->speedWanted = record->travel.speedWanted;
        }

        if (!record->animation.animation)
        {
            save->animationName[0] = 0;
        }
        else
        {
            animationTime(ee::Ptr<SceneObject>(message->target), ee::Ptr<ee::Float>(save.address() + offsetof(PathManSave, animationTime)));
            save->playMode = record->animation.playMode;
            save->speedScale = record->animation.speedScale;
            stringCopy(save.address() + offsetof(PathManSave, animationName),
                       record->animation.name);
            // A hook that is neither of the two is saved as the place after
            // them.
            uint32_t hook = 0;
            while (hook < kPoseHooks && (*g_poseHooks)[hook] != record->animation.poseHook)
                ++hook;
            save->poseHook = hook;
        }
        memoryCopy(message->data, save, sizeof(PathManSave));
        return 0;
    }
    READABLE_FUNCTION_WAITS(savePathMan, Verified);

    // restorePathManAs (the engine's restore message, for either type): the
    // saved tags over its own (its marker kept), its record set up afresh,
    // then put back on its path where it was and its animation's name, mode
    // and time restored; the next update starts the animation again. Shown
    // and subscribed, or hidden and unsubscribed, as the saved tags say.
    // (A saved pose hook place of 2, neither hook, reads the word after the
    // two hooks.)
    int32_t restorePathManAs(ee::Ptr<EngineMessage> message, int32_t fast)
    {
        const ee::Ptr<SceneObject> pathMan(message->target);
        const ee::Ptr<PathManSave> save(message->data);
        const int32_t marker = tagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker);
        memoryCopy(objectComponent(pathMan), save, sizeof(save->tags));
        setTagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagMarker, static_cast<uint32_t>(marker));

        const ee::Ptr<PathManRecord> record = pathManRecord(ee::Ptr<SceneObject>(message->target));
        deleteFollower(record);
        pathman::freeAnimatorOf(pathMan);
        freeBoneMirror(record->animation.boneMirror);
        record->animation.boneMirror = {};
        setUpPathManRecord(pathMan, record);
        record->travel.fast = fast;

        if (save->path != 0)
        {
            if (fast != 0)
            {
                putPathManOnFastPath(pathMan, save->path);
                zge::setFastFollowerTime(ee::Ptr<zge::FastFollower>(record->travel.follower), save->along);
                zge::setFastFollowerRate(ee::Ptr<zge::FastFollower>(record->travel.follower), save->speed);
                zge::setFastFollowerEndMode(ee::Ptr<zge::FastFollower>(record->travel.follower), save->endMode);
            }
            else
            {
                putPathManOnPath(pathMan, save->path);
                setFollowerAlong(ee::Ptr<Follower>(record->travel.follower), save->along);
                setFollowerSpeed(ee::Ptr<Follower>(record->travel.follower), save->speed);
                zge::setFollowerEndMode(ee::Ptr<Follower>(record->travel.follower), save->endMode);
            }
            record->travel.topSpeed = save->topSpeed;
            record->travel.speedWanted = save->speedWanted;
        }
        if (save->animationName[0] != 0)
        {
            stringCopy(record->animation.name, save->animationName);
            record->animation.playMode = save->playMode;
            record->animation.poseHook = (*g_poseHooks)[save->poseHook];
            record->animation.speedScale = save->speedScale;
            record->animation.animation = {};
            record->animation.mirrored = 0;
            record->animation.state = kPathManNoState;
            setAnimationTime(pathMan, save->animationTime);
        }

        if (tagByte(objectComponent(pathMan), kPathManTagBytes, kPathManTagDisabled) == 0)
        {
            setSceneObjectShown(pathMan, 1);
            subscribeWhileEnabled(pathMan, true);
        }
        else
        {
            unsubscribeWhileDisabled(pathMan, true);
            setSceneObjectShown(pathMan, 0);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(restorePathManAs, Verified);

    // restorePathMan, restoreFastPathMan (each type's restore message).
    int32_t restorePathMan(ee::Ptr<EngineMessage> message)
    {
        return restorePathManAs(message, 0);
    }
    READABLE_FUNCTION_WAITS(restorePathMan, Verified);

    int32_t restoreFastPathMan(ee::Ptr<EngineMessage> message)
    {
        return restorePathManAs(message, 1);
    }
    READABLE_FUNCTION_WAITS(restoreFastPathMan, Verified);

    // pausePathMan (kPauseMessage): its animation paused; it stays where it
    // is and decides nothing.
    int32_t pausePathMan(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(ee::Ptr<SceneObject>(message->target));
        pauseAnimation(ee::Ptr<SceneObject>(message->target));
        animation->paused = 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(pausePathMan, Verified);

    // resumePathMan (kResumeMessage).
    int32_t resumePathMan(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(ee::Ptr<SceneObject>(message->target));
        resumeAnimation(ee::Ptr<SceneObject>(message->target));
        animation->paused = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(resumePathMan, Verified);

    // pathManTouched (kTouchedMessage): the game is told the rider
    // activates it; when the game does not take that and the path man is
    // not down already, it is hit.
    int32_t pathManTouched(ee::Ptr<EngineMessage> message)
    {
        if (riderActivatesPathMan(ee::Ptr<SceneObject>(message->target), 1) == 0 &&
            pathManWasHit(ee::Ptr<SceneObject>(message->target)) == 0)
        {
            pathManIsHit(ee::Ptr<SceneObject>(message->target));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(pathManTouched, Draft);

    // enablePathMan (kEnableMessage): resumed, subscribed, and started
    // afresh on its path: a new motion from its start, put where its
    // follower is, shown.
    int32_t enablePathMan(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> pathMan(message->target);
        resumePathMan(message);
        subscribeWhileEnabled(pathMan, false);
        setPathManState(ee::Ptr<SceneObject>(message->target), kPathManOnPath, 0.0f);
        pathManAnimationEnded(ee::Ptr<SceneObject>(message->target));
        movePathMan(ee::Ptr<SceneObject>(message->target), 1, 0.0f);
        setAnimationTime(ee::Ptr<SceneObject>(message->target), 0.0f);
        if (pathman::objectHasAnimator(ee::Ptr<SceneObject>(message->target)) != 0)
            pathman::freeAnimatorOf(ee::Ptr<SceneObject>(message->target));
        setTagByte(objectComponent(pathMan), kPathManTagBytes, kPathManTagDisabled, 0);
        setSceneObjectShown(pathMan, 1);
        updatePose(pathMan);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enablePathMan, Verified);

    // pathManIsEnabled (kIsEnabledMessage).
    int32_t pathManIsEnabled(ee::Ptr<EngineMessage> message)
    {
        const uint32_t disabled = tagByte(objectComponent(ee::Ptr<SceneObject>(message->target)), kPathManTagBytes, kPathManTagDisabled);
        *ee::Ptr<uint32_t>(message->data) = disabled == 0;
        return 0;
    }
    READABLE_FUNCTION(pathManIsEnabled, Verified);

    // disablePathMan (kDisableMessage): hidden, paused and unsubscribed.
    int32_t disablePathMan(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> pathMan(message->target);
        setSceneObjectShown(pathMan, 0);
        pausePathMan(message);
        unsubscribeWhileDisabled(pathMan, false);
        setTagByte(objectComponent(pathMan), kPathManTagBytes, kPathManTagDisabled, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disablePathMan, Verified);

    // pathManRiderDid (kRiderDidMessage): it speaks of the rider's trick
    // (the data 0) or crash (1).
    int32_t pathManRiderDid(ee::Ptr<EngineMessage> message)
    {
        if (message->data == 0)
            sayPathManPhrase(ee::Ptr<SceneObject>(message->target), kPhraseTrick);
        else if (message->data == 1)
            sayPathManPhrase(ee::Ptr<SceneObject>(message->target), kPhraseCrash);
        return 0;
    }
    READABLE_FUNCTION_WAITS(pathManRiderDid, Verified);

    // drawPathManDebug: a debug line from the current player's drawn rider
    // to the path man.
    void drawPathManDebug(ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<Vec4> position;
        ee::Local<Vec4> riderPosition;
        ee::Local<uint32_t> colour;
        *colour = *g_debugColour;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(currentPlayer()));
        pathManPosition(pathMan, position);
        *riderPosition = *vec4(rider->drawnPosition) + *vec4(rider->drawnOffset);
        addDebugLine(riderPosition, position, colour, 2);
    }
    READABLE_FUNCTION_WAITS(drawPathManDebug, Verified);
}
