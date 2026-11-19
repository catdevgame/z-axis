// BMX XXX's "DynamicsObject" level object type (dynamics_objects.h): a level
// object the engine's rigid-body dynamics move when a rider knocks it.
// BMX XXX's own code (its game block), Aggressive Inline's line
// (game/aggressive/level/dynamics_objects.cpp) over BMX XXX's record and with
// BMX XXX's own rules: an object is put back where it started once no rider
// is near it, it can be enabled and disabled by the level's scripts
// (entity_messages.h), and a rider's knock sounds by the kind of the knock.

#include "dynamics_objects.h"
#include "entities.h"
#include "entity_messages.h"
#include "level_calls.h"
#include "level_collision.h"
#include "object_components.h"

#include "bmx/game/player_riders.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/dynamics_objects.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        constexpr uint32_t kTypeName = sym::dynamicsObjectTypeName;            // "DynamicsObject"
        constexpr uint32_t kSoundFormat = sym::bmx::dynamicsObjectSoundFormat; // "%s.%s"
        constexpr uint32_t kCreateEntity = sym::createEntity;
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kSoundNameTag = 4; // text: the second part of its sounds' names
        constexpr uint32_t kTagText = 0x10;   // the tag accessors' second argument (the value's kind)

        inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
        // A frame's share of a second (1 / 60): the dynamics' step.
        inline ee::Global<ee::Float, sym::g_dynamicsFrameSeconds> g_dynamicsFrameSeconds;
        // Where a removed and a disabled object's body is put, on every axis
        // (10000 each: out of every other box's reach in the sweep).
        inline ee::Global<ee::Float, sym::g_removedBodyAt> g_removedBodyAt;
        inline ee::Global<ee::Float, sym::g_disabledBodyAt> g_disabledBodyAt;
        // How much of a contact's speed the objects' bodies keep (0.2).
        inline ee::Global<ee::Float, sym::g_dynamicsRestitution> g_dynamicsRestitution;
        // resetDynamicsObject's: the nearest rider's distance squared past
        // which an object is put back (10000); what a body's speed and spin
        // squared are scaled by with two or more players (0.1), and under
        // what they count as nearly still (0.01).
        inline ee::Global<ee::Float, sym::g_resetDistanceSquared> g_resetDistanceSquared;
        inline ee::Global<ee::Float, sym::g_sharedScreenStillScale> g_sharedScreenStillScale;
        inline ee::Global<ee::Float, sym::g_stillSpeedSquared> g_stillSpeedSquared;
        // With two or more players the rider's distance squared counts 25 times.
        constexpr float kSharedScreenDistanceScale = 25.0f;

        constexpr zge::MessageHandler kDynamicsObjectTouched(sym::dynamicsObjectTouched);
        const zge::MessageHandler kUpdateDynamicsObject(sym::updateDynamicsObject);

        // The C library's.
        void memset(ee::Address to, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, to, byte, bytes); }

        ee::Ptr<DynamicsRecord> recordOf(ee::Address object)
        {
            return ee::Ptr<DynamicsRecord>(static_cast<uint32_t>(objectComponentKind(ee::Ptr<zge::SceneObject>(object))));
        }
    }

    int32_t updateDynamicsObject(ee::Ptr<zge::EngineMessage> message);

    // createDynamicsObject (initDynamicsObject's): the node's dynamics
    // definition of that name copied for the object; the game's record of it
    // made, listed and tied to the object and to the body; where the object
    // and the body start kept for the resets; the body in the level's sweep,
    // its contacts the game's. 0 when the node has no such definition.
    uint32_t createDynamicsObject(ee::Ptr<zge::SceneObject> object, uint64_t node, uint32_t definitionName)
    {
        const ee::Ptr<zge::Dynamics> dynamics = zge::cloneDynamicsDefinition(ee::Ptr<zge::Node>(static_cast<uint32_t>(node)), definitionName);
        if (!dynamics)
            return 0;
        const ee::Ptr<DynamicsRecord> record(zge::heapAllocate(sizeof(DynamicsRecord)));
        dynamicsRecords()[*g_dynamicsObjectCount] = record;
        record->sceneObject = ee::Address(object);
        record->dynamics = dynamics;
        record->startPosition = *zge::objectPosition(ee::Ptr<zge::SceneObject>(record->sceneObject));
        record->riderDistanceSquared = ee::Float::fromBits(0);
        record->flags = 0;
        record->startPosition = *zge::objectPosition(object);
        zge::quaternionCopy(&record->startOrientation, zge::objectOrientation(object));
        dynamics->bodies->record = ee::Address(record);
        setObjectComponentWord(object, record);
        addDynamicsBody(dynamics, object);
        setDynamicsContactCallback(dynamics);
        zge::setDynamicsRestitution(dynamics, *g_dynamicsRestitution);
        record->bodyStartPosition = dynamics->bodies->position;
        zge::quaternionCopy(&record->bodyStartOrientation, &dynamics->bodies->orientation);
        *g_dynamicsObjectCount = *g_dynamicsObjectCount + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(createDynamicsObject, Verified);

    // resetDynamicsObject (the object's frame update, and its enabling and
    // restoring with `reset`). How far the body is from the nearest rider is
    // kept. With `reset`, or once that is past the reset distance, the object
    // and its body are put back where they started, woken, its bone unturned,
    // and the frame update it had subscribed is dropped. An awake body that
    // moves and spins slower than the still speed counts the call; after
    // sixteen such calls it is asleep.
    void resetDynamicsObject(ee::Ptr<DynamicsRecord> record, uint32_t reset)
    {
        ee::Local<zge::Vec4> riderAt;
        ee::Local<zge::Vec4> offset;
        ee::Local<zge::Vec4> otherAt;
        ee::Local<zge::Vec4> otherOffset;
        ee::Local<zge::Quaternion> unturned;

        *riderAt = *rider::vec4(rider::riderOf(riderIndexOf(0))->position);
        *offset = record->dynamics->bodies->position - *riderAt;
        record->riderDistanceSquared = zge::dot(*offset, *offset);
        for (uint32_t player = 1; player < *g_playerCount; ++player)
        {
            const ee::Ptr<rider::Rider> other = rider::riderOf(riderIndexOf(player));
            *otherAt = *rider::vec4(other->position);
            *otherOffset = record->dynamics->bodies->position - *otherAt;
            const ee::Float distanceSquared = zge::dot(*otherOffset, *otherOffset);
            if (distanceSquared < record->riderDistanceSquared)
            {
                *riderAt = *rider::vec4(other->position);
                *offset = *otherOffset;
                record->riderDistanceSquared = distanceSquared;
            }
        }
        if (*g_playerCount >= 2)
            record->riderDistanceSquared = record->riderDistanceSquared * ee::Float(kSharedScreenDistanceScale);
        zge::objectNameHash(ee::Ptr<zge::SceneObject>(record->sceneObject));

        if (*g_resetDistanceSquared < record->riderDistanceSquared || reset != 0)
        {
            memset(unturned, 0, sizeof(zge::Quaternion));
            record->flags = 0;
            unturned->w = ee::Float(1.0f);
            zge::setObjectPosition(ee::Ptr<zge::SceneObject>(record->sceneObject), &record->startPosition);
            zge::setObjectOrientation(ee::Ptr<zge::SceneObject>(record->sceneObject), &record->startOrientation);
            const ee::Ptr<zge::AnimationChannel> bone = record->dynamics->bodies->boneChannel;
            if (bone)
                zge::setBoneRotation(bone, unturned);
            zge::updatePose(ee::Ptr<zge::SceneObject>(record->sceneObject));
            zge::wakeDynamics(ee::Ptr<zge::SceneObject>(record->sceneObject), record->dynamics, 1);
            record->dynamics->bodies->position = record->bodyStartPosition;
            reset = 1;
            zge::quaternionCopy(&record->dynamics->bodies->orientation, &record->bodyStartOrientation);
            zge::quaternionToMatrix(&record->dynamics->bodies->rotation, &record->bodyStartOrientation);
            zge::sweepUpdateBody(ee::Ptr<zge::Sweep>(*g_levelSweep), record->dynamics->bodies);
        }

        const uint32_t flags = record->flags;
        if (flags & kDynamicsAwake)
        {
            const ee::Ptr<zge::RigidBody> body = record->dynamics->bodies;
            const ee::Float speedSquared = zge::dot(body->velocity, body->velocity);
            const ee::Float spinSquared = zge::dot(body->spin, body->spin);
            ee::Float moving = speedSquared + spinSquared;
            if (*g_playerCount >= 2)
                moving = moving * *g_sharedScreenStillScale;
            if (moving < *g_stillSpeedSquared)
            {
                const uint32_t stillCalls = flags >> kDynamicsStillShift;
                if (stillCalls & kDynamicsStillCalls)
                    record->flags = 0;
                else
                    record->flags = ((stillCalls + 1) << kDynamicsStillShift) | kDynamicsAwake;
            }
        }
        if (record->frameHandler && reset != 0)
            unsubscribeDynamicsContact(record);
    }
    READABLE_FUNCTION_WAITS(resetDynamicsObject, Verified);

    // addDynamicsBody (createDynamicsObject's): a box body woken and put in
    // the level's sweep as a body, its box the radius of its half extents
    // about its position.
    void addDynamicsBody(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::Vec4> low;
        ee::Local<zge::Vec4> high;
        const ee::Ptr<zge::RigidBody> body = dynamics->bodies;
        if (body->shape != 0)
            return;
        const ee::Float radius = zge::vec3Length(&body->halfExtents->half.xyz());
        zge::wakeDynamics(object, dynamics, 1);
        const ee::Ptr<zge::Sweep> sweep(*g_levelSweep);
        low->x = body->position.x - radius;
        low->y = body->position.y - radius;
        low->z = body->position.z - radius;
        high->x = body->position.x + radius;
        high->y = body->position.y + radius;
        high->z = body->position.z + radius;
        body->sweepEntry = zge::sweepAddBody(sweep, kSweepRigidBody, body, &low->x, &high->x);
    }
    READABLE_FUNCTION_WAITS(addDynamicsBody, Verified);

    // dynamicsObjectRemoved (the engine's removed message): its body moved
    // out of every box's reach, which ends its contacts; the body's ends
    // taken off the sweep's three axes and freed, its box record, its
    // dynamics and the game's record freed. The engine's removeSweepBody
    // written out in place, with two differences kept as they are: an axis
    // whose first end was the body's is left with no first end at all, and
    // the end after the body's is not told what now comes before it.
    int32_t dynamicsObjectRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        constexpr uint32_t kAxes = 3;
        const ee::Ptr<DynamicsRecord> record = recordOf(message->target);
        const ee::Ptr<zge::Dynamics> dynamics = record->dynamics;
        dynamics->bodies->position.x = *g_removedBodyAt;
        dynamics->bodies->position.y = *g_removedBodyAt;
        dynamics->bodies->position.z = *g_removedBodyAt;
        zge::sweepUpdateBody(levelSweep(), dynamics->bodies);
        for (uint32_t axis = 0; axis < kAxes; ++axis)
        {
            const ee::Ptr<zge::SweepEnd> low = dynamics->bodies->sweepEntry->lowEnds[axis];
            if (low->prev)
                low->prev->next = low->other->next;
            else
                levelSweep()->axes[axis] = ee::Ptr<zge::SweepEnd>();
            zge::poolFree(ee::Ptr<zge::Pool>(levelSweep()->endPool), dynamics->bodies->sweepEntry->lowEnds[axis]->other);
            zge::poolFree(ee::Ptr<zge::Pool>(levelSweep()->endPool), dynamics->bodies->sweepEntry->lowEnds[axis]);
        }
        zge::heapFree(dynamics->bodies->sweepEntry);
        zge::freeDynamicsRecord(dynamics);
        zge::heapFree(record);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectRemoved, Verified);

    // enableDynamicsObject (kEnableMessage): put back where it started,
    // knocks taken again, drawn. Always 1.
    int32_t enableDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        const uint64_t node = objectNodeHandle(ee::Ptr<zge::SceneObject>(message->target));
        const ee::Ptr<DynamicsRecord> record = recordOf(message->target);
        if (record)
            resetDynamicsObject(record, 1);
        zge::subscribe(message->target, kRiderTouchMessage, kDynamicsObjectTouched);
        rider::collision::showNode(node, 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enableDynamicsObject, Draft);

    // disableDynamicsObject (kDisableMessage): its body moved out of every
    // box's reach, knocks no longer taken, not drawn. Always 1.
    int32_t disableDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<DynamicsRecord> record = recordOf(message->target);
        const uint64_t node = objectNodeHandle(ee::Ptr<zge::SceneObject>(message->target));
        if (record)
        {
            const ee::Ptr<zge::Dynamics> dynamics = record->dynamics;
            dynamics->bodies->position.x = *g_disabledBodyAt;
            dynamics->bodies->position.y = *g_disabledBodyAt;
            dynamics->bodies->position.z = *g_disabledBodyAt;
            zge::sweepUpdateBody(levelSweep(), dynamics->bodies);
        }
        zge::unsubscribe(message->target, kRiderTouchMessage, kDynamicsObjectTouched);
        rider::collision::showNode(node, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(disableDynamicsObject, Verified);

    // dynamicsObjectIsEnabled (kIsEnabledMessage): answers whether it takes knocks.
    int32_t dynamicsObjectIsEnabled(ee::Ptr<zge::EngineMessage> message)
    {
        if (message->data != 0)
            *ee::Ptr<uint32_t>(message->data) = zge::findSubscription(ee::Ptr<zge::MessageTarget>(message->target), kRiderTouchMessage);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectIsEnabled, Draft);

    // initDynamicsObject (the type's init): its node drawn but not collided
    // with (the body answers instead); its dynamics made from the definition
    // its model names; its messages subscribed. 0 when it has no dynamics.
    uint32_t initDynamicsObject(ee::Ptr<zge::SceneObject> object)
    {
        const uint64_t node = objectNodeHandle(object);
        rider::collision::showNode(node, 1);
        rider::collision::setNodeCollidable(node, 0);
        // Its dynamics definition is the one named as its model is.
        const uint32_t definitionName = ee::Ptr<zge::Node>(static_cast<uint32_t>(node))->model->nameHash;
        if (createDynamicsObject(object, node, definitionName) != 1)
            return 0;
        zge::subscribe(object, zge::kRemovedMessage, zge::MessageHandler(sym::dynamicsObjectRemoved));
        zge::subscribe(object, zge::kRestoreMessage, zge::MessageHandler(sym::restoreDynamicsObject));
        zge::subscribe(object, zge::kSaveMessage, zge::MessageHandler(sym::saveDynamicsObject));
        zge::subscribe(object, zge::kVelocityMessage, zge::MessageHandler(sym::dynamicsObjectVelocity));
        zge::subscribe(object, zge::kSaveSizeMessage, zge::MessageHandler(sym::dynamicsObjectSaveSize));
        zge::subscribe(object, kRiderTouchMessage, kDynamicsObjectTouched);
        zge::subscribe(object, kEnableMessage, zge::MessageHandler(sym::enableDynamicsObject));
        zge::subscribe(object, kDisableMessage, zge::MessageHandler(sym::disableDynamicsObject));
        zge::subscribe(object, kIsEnabledMessage, zge::MessageHandler(sym::dynamicsObjectIsEnabled));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initDynamicsObject, Verified);

    // registerDynamicsObjectType: the "DynamicsObject" type, made as a plain
    // entity and set up by initDynamicsObject.
    uint32_t registerDynamicsObjectType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initDynamicsObject));
    }
    READABLE_FUNCTION(registerDynamicsObjectType, Verified);

    // dynamicsObjectTouched (kRiderTouchMessage): the game is told the
    // sender's rider activated the object (its answer is not used), and the
    // knock sounds: "DynamicsObject.<the class's sound name>" and the kind of
    // knock.
    int32_t dynamicsObjectTouched(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(message->sender));
        event->quiet = 0;
        event->player = rider->player;
        event->velocity = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        activation->target = message->target;
        activation->sender = message->sender;
        activation->type = kActivateMessage;
        activation->flags = zge::kMessageNow;
        activation->data = event;
        riderActivatedObject(activation);
        zge::tagString(zge::levelObjectClassTag(ee::Ptr<zge::SceneObject>(message->target)), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kSoundFormat, kTypeName, className.address());
        playTouchSound(message, soundName);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectTouched, Verified);

    // dynamicsObjectVelocity (the engine's velocity message): its body's.
    int32_t dynamicsObjectVelocity(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::Vec4> velocity(message->data);
        const ee::Ptr<DynamicsRecord> record = recordOf(message->target);
        *velocity = record->dynamics->bodies->velocity;
        return 0;
    }
    READABLE_FUNCTION(dynamicsObjectVelocity, Draft);

    // dynamicsObjectSaveSize (the engine's save size message): 0x20 bytes
    // are asked for, though the save writes nothing.
    int32_t dynamicsObjectSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        constexpr uint32_t kSaveBytes = 0x20;
        *ee::Ptr<uint32_t>(message->data) = kSaveBytes;
        return 0;
    }
    READABLE_FUNCTION(dynamicsObjectSaveSize, Verified);

    // restoreDynamicsObject (the engine's restore message): put back where
    // it started.
    int32_t restoreDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        resetDynamicsObject(recordOf(message->target), 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreDynamicsObject, Verified);

    // saveDynamicsObject (the engine's save message): its record is looked
    // up and nothing is written.
    int32_t saveDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        recordOf(message->target);
        return 0;
    }
    READABLE_FUNCTION(saveDynamicsObject, Verified);

    // updateDynamicsObject (the frame's update, subscribed while it is
    // awake): with one player, an object that was not drawn is put back
    // where it started; otherwise an awake one's dynamics step a frame, and
    // resetDynamicsObject looks at how near the riders are and how still it
    // is.
    int32_t updateDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<DynamicsRecord> record = recordOf(object);
        const ee::Ptr<zge::Dynamics> dynamics = record->dynamics;
        const uint64_t node = objectNodeHandle(object);
        if (*g_playerCount == 1 && zge::nodeNotDrawn(ee::Ptr<zge::Node>(static_cast<uint32_t>(node))) != 0)
        {
            resetDynamicsObject(record, 1);
            return 0;
        }
        if (record->flags & kDynamicsAwake)
        {
            const ee::Ptr<zge::Sweep> sweep = levelSweep();
            zge::updateDynamics(dynamics, sweep, *g_dynamicsFrameSeconds);
        }
        resetDynamicsObject(record, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateDynamicsObject, Verified);

    // objectWoken: woken by a push: the frame's update subscribed.
    void objectWoken(ee::Ptr<DynamicsRecord> record)
    {
        zge::subscribe(record->sceneObject, zge::kUpdateMessage, kUpdateDynamicsObject);
        record->frameHandler = kUpdateDynamicsObject;
    }
    READABLE_FUNCTION_WAITS(objectWoken, Verified);

    // unsubscribeDynamicsContact: asleep: the update it subscribed to is dropped.
    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record)
    {
        zge::unsubscribe(record->sceneObject, zge::kUpdateMessage, record->frameHandler);
        record->frameHandler = zge::MessageHandler();
    }
    READABLE_FUNCTION_WAITS(unsubscribeDynamicsContact, Verified);
}
