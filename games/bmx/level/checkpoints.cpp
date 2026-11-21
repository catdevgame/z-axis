// BMX XXX's "CheckPoint" level object type: a place a rider reaches by
// riding within its radius. BMX XXX's own code (its game block); no other
// build names its functions, so the names are from what each does, the
// type's strings ("CheckPoint", "%s.activate") and the script commands that
// reach it (entity_messages.h).
//
// A checkpoint is disabled, enabled or reached. Only an enabled one is
// watched for riders and shows a marker: a clone of the marker model's
// scene, made the first time it is enabled and kept at the checkpoint in
// the model's pose. Its own node is never drawn nor collided with.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/checkpoints.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        enum CheckPointState : uint32_t
        {
            kCheckPointDisabled = 0,
            kCheckPointEnabled = 1,
            kCheckPointReached = 2,
        };

        // The checkpoint's own tag data (objectComponent).
        constexpr uint32_t kRadiusTag = 0; // float: how close a rider reaches it
        constexpr uint32_t kStateTag = 4;  // byte: its CheckPointState
        constexpr uint32_t kMarkerTag = 8; // word: its marker's scene, 0 until first enabled
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagWord = 8;

        constexpr uint32_t kTypeName = sym::checkPointTypeName;                         // "CheckPoint"
        constexpr uint32_t kActivateSoundFormat = sym::bmx::checkPointActivateSoundFormat; // "%s.activate"
        // The objectives' two icon models (games/bmx/objectives loads them:
        // "icon.NPR_Available", "icon.Checkpoint"). Every checkpoint's
        // marker is a clone of the second one's scene (0: no markers).
        constexpr uint32_t kObjectiveIcons = 2, kCheckPointIcon = 1;
        inline ee::Global<ee::Ptr<zge::SceneObject>[kObjectiveIcons], sym::g_objectiveIcons> g_objectiveIcons;

        // A rider's flag byte 0: both bits are read as one value here.
        // Scaffolding (unnamed-field): bit 0x02 has no name yet; riders reach
        // checkpoints only with it set and kFlagNoPlayer clear.
        constexpr uint8_t kRiderFlag02 = 0x02;

        constexpr zge::MessageHandler kUpdateCheckPoint(sym::updateCheckPoint);
        constexpr uint32_t kCreateEntity = sym::createEntity;
    }

    uint32_t initCheckPoint(ee::Ptr<zge::SceneObject> checkPoint);
    int32_t activateCheckPoint(ee::Ptr<zge::EngineMessage> message);
    uint32_t checkPointState(ee::Ptr<zge::SceneObject> checkPoint);
    void setCheckPointState(ee::Ptr<zge::SceneObject> checkPoint, uint32_t state);

    // registerCheckPointType: the "CheckPoint" type, made as a plain entity
    // and set up by initCheckPoint.
    uint32_t registerCheckPointType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initCheckPoint));
    }
    READABLE_FUNCTION(registerCheckPointType, Verified);

    // initCheckPoint (the type's init): its messages subscribed, no marker
    // yet, disabled; its node not collided with and not drawn. Always 1.
    uint32_t initCheckPoint(ee::Ptr<zge::SceneObject> checkPoint)
    {
        zge::subscribe(checkPoint, zge::kSaveSizeMessage, zge::MessageHandler(sym::checkPointSaveSize));
        zge::subscribe(checkPoint, zge::kRemovedMessage, zge::MessageHandler(sym::checkPointRemoved));
        zge::subscribe(checkPoint, zge::kSaveMessage, zge::MessageHandler(sym::saveCheckPoint));
        zge::subscribe(checkPoint, zge::kRestoreMessage, zge::MessageHandler(sym::restoreCheckPoint));
        zge::subscribe(checkPoint, kActivateMessage, zge::MessageHandler(sym::activateCheckPoint));
        zge::subscribe(checkPoint, kEnableMessage, zge::MessageHandler(sym::enableCheckPoint));
        zge::subscribe(checkPoint, kDisableMessage, zge::MessageHandler(sym::disableCheckPoint));
        zge::setTagWord(zge::objectComponent(checkPoint), kTagWord, kMarkerTag, 0);
        setCheckPointState(checkPoint, kCheckPointDisabled);
        const uint64_t node = objectNodeHandle(checkPoint);
        rider::collision::setNodeCollidable(node, 0);
        rider::collision::showNode(node, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCheckPoint, Verified);

    // checkPointRemoved (the engine's removed message): its marker released.
    int32_t checkPointRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::Scene> marker(static_cast<uint32_t>(
            zge::tagWord(zge::objectComponent(ee::Ptr<zge::SceneObject>(message->target)), kTagWord, kMarkerTag)));
        if (marker)
            zge::releaseScene(marker);
        return 0;
    }
    READABLE_FUNCTION_WAITS(checkPointRemoved, Verified);

    // activateCheckPoint (kActivateMessage): reached, with the type's
    // "activate" sound unless the event's first word is set.
    int32_t activateCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        const ee::Ptr<zge::SceneObject> checkPoint(message->target);
        if (ee::Ptr<ActivationEvent>(message->data)->quiet == 0)
        {
            zge::formatString(soundName, kActivateSoundFormat, kTypeName);
            const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
            soundEvent(kObjectSoundEvent, position, kObjectSoundOnce, soundName.address(), kFullVolume);
        }
        setCheckPointState(checkPoint, kCheckPointReached);
        return 0;
    }
    READABLE_FUNCTION_WAITS(activateCheckPoint, Verified);

    // enableCheckPoint (kEnableMessage).
    int32_t enableCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        setCheckPointState(ee::Ptr<zge::SceneObject>(message->target), kCheckPointEnabled);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableCheckPoint, Verified);

    // disableCheckPoint (kDisableMessage).
    int32_t disableCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        setCheckPointState(ee::Ptr<zge::SceneObject>(message->target), kCheckPointDisabled);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableCheckPoint, Verified);

    // updateCheckPoint (the frame's update, while enabled): every rider
    // that reaches it activates it; its marker takes the marker model's
    // pose at the checkpoint's place and is shown.
    int32_t updateCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> checkPoint(message->target);
        rider::forEachRider(rider::RiderVisit(sym::riderReachesCheckPoint), checkPoint);
        const ee::Ptr<zge::Scene> marker(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(checkPoint), kTagWord, kMarkerTag)));
        if (marker)
        {
            zge::copyScenePose(marker, zge::sceneOf((*g_objectiveIcons)[kCheckPointIcon]));
            zge::setScenePosition(marker, zge::objectPosition(checkPoint));
            zge::poseScene(marker);
            zge::changeSceneState(marker, 0, zge::kSceneShown);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateCheckPoint, Verified);

    // checkPointSaveSize (the engine's save size message): its state, a word.
    int32_t checkPointSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(uint32_t);
        return 0;
    }
    READABLE_FUNCTION(checkPointSaveSize, Verified);

    // restoreCheckPoint (the engine's restore message): the saved state entered.
    int32_t restoreCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        setCheckPointState(ee::Ptr<zge::SceneObject>(message->target), *ee::Ptr<uint32_t>(message->data));
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreCheckPoint, Verified);

    // saveCheckPoint (the engine's save message).
    int32_t saveCheckPoint(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<uint32_t> saved(message->data);
        *saved = checkPointState(ee::Ptr<zge::SceneObject>(message->target));
        return 0;
    }
    READABLE_FUNCTION(saveCheckPoint, Verified);

    // checkPointState: its CheckPointState.
    uint32_t checkPointState(ee::Ptr<zge::SceneObject> checkPoint)
    {
        return zge::tagByte(zge::objectComponent(checkPoint), kTagByte, kStateTag);
    }
    READABLE_FUNCTION(checkPointState, Verified);

    // setCheckPointState: the state entered. Enabled: the update runs and
    // the marker is shown, cloned from the marker model first if this
    // checkpoint has none yet. Any other state: no update, the marker hidden.
    void setCheckPointState(ee::Ptr<zge::SceneObject> checkPoint, uint32_t state)
    {
        zge::unsubscribe(checkPoint, zge::kUpdateMessage, kUpdateCheckPoint);
        uint32_t shown = 0;
        if (state == kCheckPointEnabled)
        {
            zge::subscribe(checkPoint, zge::kUpdateMessage, kUpdateCheckPoint);
            shown = 1;
        }
        zge::setTagByte(zge::objectComponent(checkPoint), kTagByte, kStateTag, state & 0xFFu);
        ee::Ptr<zge::Scene> marker(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(checkPoint), kTagWord, kMarkerTag)));
        if (!marker)
        {
            if ((*g_objectiveIcons)[kCheckPointIcon] && shown != 0)
            {
                marker = zge::cloneScene(zge::sceneOf((*g_objectiveIcons)[kCheckPointIcon]), 0);
                zge::setTagWord(zge::objectComponent(checkPoint), kTagWord, kMarkerTag, marker);
            }
            if (!marker)
                return;
        }
        game::tail(zge::changeSceneState, marker, shown ^ 1u, shown);
    }
    READABLE_FUNCTION_WAITS(setCheckPointState, Verified);

    // riderReachesCheckPoint (forEachRider's visit; the context is the
    // checkpoint): a rider within its radius activates it: the game is
    // asked first, and a checkpoint the game does not take is activated
    // here, which ends the walk (0). 1: the next rider.
    uint32_t riderReachesCheckPoint(uint32_t riderIndex, ee::Ptr<zge::SceneObject> checkPoint)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> offset;
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex & 0xFFu);
        if ((rider->flags[0] & (rider::kFlagNoPlayer.mask | kRiderFlag02)) != kRiderFlag02)
            return 1;
        ee::Float radius = zge::tagFloat(zge::objectComponent(checkPoint), kTagWord, kRadiusTag);
        *at = *rider::vec4(rider->position) + *rider::vec4(rider->rootOffset);
        *offset = *at - *zge::objectPosition(checkPoint);
        const ee::Float distanceSquared = zge::dot(*offset, *offset);
        radius = radius * radius;
        if (!(distanceSquared <= radius))
            return 1;
        zge::memoryFill(event, 0, sizeof(ActivationEvent));
        event->quiet = 0;
        event->player = rider->player;
        activation->target = checkPoint;
        activation->sender = ee::Address(rider->messenger);
        activation->type = kActivateMessage;
        activation->data = event;
        activation->flags = zge::kMessageNow;
        if (riderActivatedObject(activation) != 0)
            return 1;
        activateCheckPoint(activation);
        return 0;
    }
    READABLE_FUNCTION_WAITS(riderReachesCheckPoint, Verified);
}
