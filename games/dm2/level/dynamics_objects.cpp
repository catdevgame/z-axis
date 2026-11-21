// DM2's dynamics objects (dynamics_objects.h). DM2's own code (its game block).

#include "dynamics_objects.h"
#include "dm2/game/goals.h"

#include "level_collision.h"
#include "object_node.h"
#include "object_sounds.h"
#include "water_wake.h"

#include "dm2/game/players.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/dm2/level/dynamics_objects.cpp.h"

namespace game::dm2
{
    namespace calls
    {
        // The engine's point of a box body on the plane of the face it touches, called as the original calls it:
        // the scale after it carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void boxPointOnFacePlaneByCall(ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::Vec4> out) { ee::game(sym::boxPointOnFacePlane, body, out); }
        // Scaffolding (address-call): ZGE2's pose of an object's model (the scene's pose asked for the object's scene).
        inline void modelPose(ee::Ptr<zge::SceneObject> object) { ee::game(sym::modelPose, object); }
    }

    // objectDynamicsRecord, setObjectDynamicsRecord: a dynamics object's
    // record, its component's first word (the code of
    // object_components.cpp's pair, word for word, a copy of its own).
    ee::Ptr<DynamicsRecord> objectDynamicsRecord(ee::Ptr<zge::SceneObject> sceneObject)
    {
        return ee::Ptr<DynamicsRecord>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(sceneObject), kDynamicsTagWords, 0)));
    }
    READABLE_FUNCTION(objectDynamicsRecord, Verified);

    int32_t setObjectDynamicsRecord(ee::Ptr<zge::SceneObject> sceneObject, ee::Ptr<DynamicsRecord> record)
    {
        return game::tail(zge::setTagWord, zge::objectComponent(sceneObject), kDynamicsTagWords, 0, record);
    }
    READABLE_FUNCTION(setObjectDynamicsRecord, Verified);

    namespace
    {
        // The dynamics objects made, and how many.
        inline ee::Global<ee::Ptr<DynamicsRecord>[1], sym::dynamicsObjects> g_dynamicsObjects;
        inline ee::Global<uint32_t, sym::g_dynamicsObjectCount> g_dynamicsObjectCount;
        inline ee::Global<uint32_t, sym::g_levelSweep> g_levelSweep;
        constexpr uint32_t kSweepRigidBody = 0;

        // Further than this from the rider (squared: 100 units) an object goes back to its start.
        inline ee::Global<ee::Float, sym::g_dynamicsResetDistanceSquared> g_resetDistanceSquared;
        // Nearly still: its speed and spin squared under this (0.01).
        inline ee::Global<ee::Float, sym::g_dynamicsRestSpeedSquared> g_restSpeedSquared;

        // A launched object's lift (units a frame, upwards), spin and weight for each unit of mass.
        constexpr ee::Float kLaunchLift = 6.0f;
        constexpr ee::Float kLaunchSpinX = 0.5f;
        inline ee::Global<ee::Float, sym::g_launchSpinZ> g_launchSpinZ; // 0.7
        inline ee::Global<ee::Float, sym::g_launchSpinY> g_launchSpinY; // 0.3
        constexpr ee::Float kLaunchWeight = 10.0f;

        ee::Ptr<DynamicsRecord> &madeObject(uint32_t index) { return (*g_dynamicsObjects)[index]; }
    }

    uint32_t createDynamicsObject(ee::Ptr<zge::SceneObject> sceneObject, ee::Ptr<zge::Node> node, uint32_t definitionName)
    {
        const ee::Ptr<zge::Dynamics> dynamics = zge::cloneDynamicsDefinition(node, definitionName);
        if (!dynamics)
            return 0;
        const ee::Ptr<DynamicsRecord> record(zge::heapAllocate(sizeof(DynamicsRecord)));
        madeObject(*g_dynamicsObjectCount) = record;
        record->sceneObject = sceneObject;
        record->dynamics = dynamics;
        record->startPosition = *zge::objectPosition(record->sceneObject);
        record->riderDistanceSquared = 0.0f;
        record->flags = 0;
        record->startPosition = *zge::objectPosition(sceneObject);
        zge::quaternionCopy(ee::Ptr<zge::Quaternion>(&record->startOrientation), zge::objectOrientation(sceneObject));
        dynamics->bodies->record = ee::Address(record);
        setObjectDynamicsRecord(sceneObject, record);
        addDynamicsBody(dynamics, sceneObject);
        setDynamicsContactCallback(dynamics);
        const ee::Ptr<zge::RigidBody> body = dynamics->bodies;
        record->bodyStartPosition = body->position;
        zge::quaternionCopy(ee::Ptr<zge::Quaternion>(&record->bodyStartOrientation), ee::Ptr<const zge::Quaternion>(&body->orientation));
        *g_dynamicsObjectCount = *g_dynamicsObjectCount + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(createDynamicsObject, Verified);

    void launchDynamicsObjectFromRider(ee::Ptr<rider::Rider> rider, ee::Ptr<zge::SceneObject> sceneObject)
    {
        const ee::Ptr<DynamicsRecord> record = objectDynamicsRecord(sceneObject);
        zge::wakeDynamics(record->sceneObject, record->dynamics, 1);
        const ee::Ptr<zge::RigidBody> body = record->dynamics->bodies;
        body->velocity.y = rider->velocity[1] - kLaunchLift; // (y grows downwards)
        body->velocity.x = rider->velocity[0];
        body->velocity.z = rider->velocity[2];
        body->spin.x = kLaunchSpinX;
        body->spin.z = *g_launchSpinZ;
        body->spin.y = *g_launchSpinY;
        body->force.y = body->mass * kLaunchWeight;
        record->flags = kDynamicsAwake;
    }
    READABLE_FUNCTION_WAITS(launchDynamicsObjectFromRider, Draft);

    void resetDynamicsObject(ee::Ptr<DynamicsRecord> record, uint32_t reset)
    {
        ee::Local<zge::Vec4> riderPosition;
        ee::Local<zge::Vec4> offset;
        const ee::Ptr<rider::Rider> nearRider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        *riderPosition = *rider::vec4(nearRider->position);
        *offset = record->dynamics->bodies->position - *riderPosition;
        const ee::Float distanceSquared = zge::lengthSquared(&offset->xyz());
        record->riderDistanceSquared = distanceSquared;
        bool putBack = false;
        if (*g_resetDistanceSquared < distanceSquared || reset != 0)
        {
            record->flags = 0;
            zge::setObjectPosition(record->sceneObject, ee::Ptr<zge::Vec4>(&record->startPosition));
            zge::setObjectOrientation(record->sceneObject, ee::Ptr<zge::Quaternion>(&record->startOrientation));
            calls::modelPose(record->sceneObject);
            zge::wakeDynamics(record->sceneObject, record->dynamics, 1);
            const ee::Ptr<zge::RigidBody> body = record->dynamics->bodies;
            body->position = record->bodyStartPosition;
            putBack = true;
            zge::quaternionCopy(ee::Ptr<zge::Quaternion>(&body->orientation), ee::Ptr<const zge::Quaternion>(&record->bodyStartOrientation));
            zge::quaternionToMatrix(&record->dynamics->bodies->rotation, &record->bodyStartOrientation);
            zge::sweepUpdateBody(ee::Ptr<zge::Sweep>(*g_levelSweep), record->dynamics->bodies);
        }
        if (record->flags & kDynamicsAwake)
        {
            const ee::Float speedSquared = zge::lengthSquared(&record->dynamics->bodies->velocity.xyz());
            const ee::Float spinSquared = zge::lengthSquared(&record->dynamics->bodies->spin.xyz());
            if (speedSquared + spinSquared < *g_restSpeedSquared)
            {
                const uint32_t stillFrames = record->flags >> kStillFramesShift;
                if (stillFrames & kStillFramesAsleep)
                    record->flags = 0;
                else
                    record->flags = (stillFrames + 1) << kStillFramesShift | kDynamicsAwake;
            }
        }
        if (record->frameHandler != 0 && putBack)
            unsubscribeDynamicsContact(record);
    }
    READABLE_FUNCTION_WAITS(resetDynamicsObject, Verified);

    void addDynamicsBody(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::SceneObject> sceneObject)
    {
        ee::Local<zge::Vec4> low;
        ee::Local<zge::Vec4> high;
        const ee::Ptr<zge::RigidBody> body = dynamics->bodies;
        if (body->shape != 0)
            return;
        const ee::Float radius = zge::vec3Length(&body->halfExtents->half.xyz());
        zge::wakeDynamics(sceneObject, dynamics, 1);
        low->x = body->position.x - radius;
        low->y = body->position.y - radius;
        low->z = body->position.z - radius;
        high->x = body->position.x + radius;
        high->y = body->position.y + radius;
        high->z = body->position.z + radius;
        body->sweepEntry = zge::sweepAddBody(ee::Ptr<zge::Sweep>(*g_levelSweep), kSweepRigidBody, body, ee::Ptr<const ee::Float>(&low->x),
                                             ee::Ptr<const ee::Float>(&high->x));
    }
    READABLE_FUNCTION_WAITS(addDynamicsBody, Verified);

    namespace
    {
        // The running sums of the faces of the part being touched: their
        // normals, their centres, and how many.
        inline ee::Global<zge::Vec4, sym::contactNormalSum> g_contactNormalSum;
        inline ee::Global<zge::Vec4, sym::contactCentreSum> g_contactCentreSum;
        inline ee::Global<int32_t, sym::g_contactFaceCount> g_contactFaceCount;

        // The level object types the contact tells apart.
        constexpr uint32_t kCarName = sym::carName;                       // "Car"
        constexpr uint32_t kGlassName = sym::glassName;                   // "glass"
        constexpr uint32_t kWaterSurfaceName = sym::waterSurfaceName;     // "waterSurface"
        constexpr uint32_t kDynamicsObjectName = sym::dynamicsObjectName; // "DynamicsObject"
        constexpr uint32_t kExploderName = sym::exploderName;             // "exploder"

        // A summed normal shorter than this (squared: 0.01) is no direction:
        // the body is knocked straight away from the car instead.
        inline ee::Global<ee::Float, sym::g_contactNormalMinSquared> g_normalMinSquared;
        // What water leaves of a body's speed and spin at each part (0.8).
        inline ee::Global<ee::Float, sym::g_waterDrag> g_waterDrag;
        inline ee::Global<ee::Float, sym::g_contactThird> g_third;
        // A touch's velocity is the body's over a step (1 / 60), a splash's 0.03 of it.
        inline ee::Global<ee::Float, sym::g_glassTouchSeconds> g_glassTouchSeconds;
        inline ee::Global<ee::Float, sym::g_exploderTouchSeconds> g_exploderTouchSeconds;
        inline ee::Global<ee::Float, sym::g_splashVelocityScale> g_splashVelocityScale;

        // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose upper half the last call left.
        zge::Vec4 scaled(const zge::Vec4 &v, ee::Float factor)
        {
            return zge::scaledCarrying(v, factor, zge::scaleLeftover(zge::LeftBy::lastCall));
        }

        bool isA(ee::Ptr<zge::SceneObject> object, uint32_t typeName)
        {
            const uint32_t type = zge::levelObjectTypeHash(object);
            return type == zge::nameHash(ee::Ptr<const char>(typeName));
        }
    }

    uint32_t dynamicsObjectContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> centre;      // this face's centre
        ee::Local<zge::Vec4> toFace;
        ee::Local<zge::Vec4> carVelocity;
        ee::Local<TouchData> touch;
        // (The original keeps these two in its frame.)
        ee::Local<ee::Ptr<zge::Dynamics>> dynamicsKept;
        ee::Local<ee::Ptr<rider::Rider>> riderKept;
        *dynamicsKept = dynamics;

        const ee::Ptr<zge::HitNode> lastPart = zge::dynamicsContactObject();
        if (lastPart != contact->object && lastPart)
        {
            // The part before is done: its owner gets its due.
            const ee::Ptr<zge::SceneObject> lastOwner = zge::nodeSceneObject(lastPart->node, {});
            if (!lastOwner)
                return 1;
            const int32_t faces = *g_contactFaceCount;
            if (faces == 0)
                return 1;
            *g_contactCentreSum = scaled(*g_contactCentreSum, ee::Float(1.0f) / ee::toFloat(faces));
            if (isA(lastOwner, kCarName))
            {
                zge::objectVelocity(lastOwner, carVelocity);
                if (!(*g_normalMinSquared < zge::dot(*g_contactNormalSum, *g_contactNormalSum)))
                    *g_contactNormalSum = body->position - *zge::objectPosition(lastOwner);
                *g_contactNormalSum = zge::unit(*g_contactNormalSum);
                zge::contactImpulse(body, &*g_contactCentreSum, &*g_contactNormalSum, &*carVelocity);
            }
            else if (isA(lastOwner, kGlassName))
            {
            }
            else if (isA(lastOwner, kWaterSurfaceName))
            {
                const ee::Float drag = *g_waterDrag;
                body->velocity = scaled(body->velocity, drag);
                body->spin = scaled(body->spin, drag);
            }
            else
                isA(lastOwner, kDynamicsObjectName); // (asked; the answer goes unused)
            *g_contactFaceCount = 0;
            *g_contactNormalSum = zge::kZero;
            *g_contactCentreSum = zge::kZero;
        }

        if (!contact->object)
            return 1;
        const ee::Ptr<zge::Node> node = contact->object->node;
        const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(node, {});
        if (!owner)
            return 1;
        uint32_t keepFace = 1;
        *riderKept = {};
        const uint32_t player = rider::playerIndexOfNode((*dynamicsKept)->sceneObject);
        if (player != 0xFF)
            *riderKept = rider::riderOf(player);

        *g_contactNormalSum = *g_contactNormalSum + contact->normal;
        *centre = contact->vertices[0];
        *centre = *centre + contact->vertices[1];
        *centre = *centre + contact->vertices[2];
        *centre = scaled(*centre, *g_third);
        *g_contactCentreSum = *g_contactCentreSum + *centre;
        *g_contactFaceCount = *g_contactFaceCount + 1;

        // The touch told to the owner, once a part: where, and the body's motion over `seconds`.
        const auto tellTouch = [&](ee::Float seconds)
        {
            if (contact->object == lastPart)
                return;
            touch->node = static_cast<uint32_t>(rider::collision::nodeHandle(ee::Address(node)));
            touch->face = contact->face;
            touch->accept = 1;
            touch->point = *centre;
            touch->velocity = body->velocity;
            touch->velocity = scaled(touch->velocity, seconds);
            zge::sendNewMessage((*dynamicsKept)->sceneObject, owner, kTouchMessage, touch, zge::kMessageNow);
        };

        if (isA(owner, kCarName))
        {
            // Only a rider's body tells a car.
            if (!*riderKept)
                return keepFace;
            tellTouch(*g_exploderTouchSeconds);
        }
        else if (isA(owner, kGlassName))
            tellTouch(*g_glassTouchSeconds);
        else if (isA(owner, kExploderName))
            tellTouch(*g_exploderTouchSeconds);
        else if (isA(owner, kWaterSurfaceName))
        {
            // A wake (water_wake.h) where the body's box meets the face's plane.
            touch->node = static_cast<uint32_t>(rider::collision::nodeHandle(ee::Address(node)));
            touch->face = contact->face;
            touch->accept = 1;
            calls::boxPointOnFacePlaneByCall(body, centre);
            *toFace = contact->vertices[0] - *centre;
            const ee::Float off = zge::dot(contact->normal, *toFace);
            *toFace = scaled(contact->normal, off);
            *centre = *centre + *toFace;
            touch->point = *centre;
            touch->velocity = body->velocity;
            touch->velocity = scaled(touch->velocity, *g_splashVelocityScale);
            keepFace = 0;
            startWaterWake(ee::Ptr<zge::Vec4>(&touch->point), ee::Ptr<zge::Vec4>(&touch->velocity));
        }
        else if (isA(owner, kDynamicsObjectName))
            keepFace = 0; // (the sweep's pairs handle two objects)

        const ee::Ptr<rider::Rider> touching = *riderKept;
        if (touching && !(touching->flags & rider::kFlagNoPlayer))
            gameTouchHandler(touching->player, owner);
        return keepFace;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectContact, Verified);

    namespace
    {
        // The dynamics objects' step: a frame (1/60 s).
        inline ee::Global<ee::Float, sym::g_dynamicsObjectStep> g_dynamicsObjectStep;
        // A dynamics object's sound (its tag word at 4: the level object's
        // sound, object_sounds.h's startObjectSound, and objectSoundMessage).
        constexpr uint32_t kObjectTagWords = 8, kObjectTagSound = 4;
    }

    // updateDynamicsObject: an object's frame message while it moves: an
    // awake object's dynamics stepped a frame in the level's sweep, its
    // frame (resetDynamicsObject), then its sound's turn.
    int32_t updateDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<DynamicsRecord> record = objectDynamicsRecord(ee::Ptr<zge::SceneObject>(message->target));
        const ee::Ptr<zge::Dynamics> dynamics = record->dynamics;
        if (record->flags & kDynamicsAwake)
            zge::updateDynamics(dynamics, levelSweep(), *g_dynamicsObjectStep);
        resetDynamicsObject(record, 0);
        const uint32_t sound = static_cast<uint32_t>(
            zge::tagWord(zge::objectComponent(ee::Ptr<zge::SceneObject>(message->target)), kObjectTagWords, kObjectTagSound));
        objectSoundMessage(message, sound);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateDynamicsObject, Verified);

    // objectWoken: woken by a push: the frame message subscribed.
    void objectWoken(ee::Ptr<DynamicsRecord> record)
    {
        const zge::MessageHandler handler(sym::updateDynamicsObject);
        zge::subscribe(record->sceneObject, zge::kUpdateMessage, handler);
        record->frameHandler = handler;
    }
    READABLE_FUNCTION_WAITS(objectWoken, Verified);

    // unsubscribeDynamicsContact: asleep: the frame message it subscribed to
    // dropped.
    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record)
    {
        zge::unsubscribe(record->sceneObject, zge::kUpdateMessage, record->frameHandler);
        record->frameHandler = 0;
    }
    READABLE_FUNCTION_WAITS(unsubscribeDynamicsContact, Verified);

    // registerDynamicsObjectType: the "DynamicsObject" level objects made
    // dynamics objects (initDynamicsLevelObject).
    uint32_t registerDynamicsObjectType()
    {
        return game::tail(zge::registerEntityInit, ee::Ptr<const char>(sym::dynamicsObjectTypeName), sym::initDynamicsLevelObject);
    }
    READABLE_FUNCTION(registerDynamicsObjectType, Verified);

    namespace
    {
        // Where a removed object's body is left before its box leaves the
        // sweep: far out on every axis (as freeRider's bail body).
        constexpr float kParkedBodyCoordinate = 10000.0f;
        // A dynamics object's save: its record keeps none of it here (the
        // save message writes nothing).
        constexpr uint32_t kDynamicsSaveBytes = 0x20;
        uint32_t dynamicsObjectSound(ee::Ptr<zge::SceneObject> object)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(object), kObjectTagWords, kObjectTagSound));
        }
    }

    // dynamicsObjectRemoved (the removed message): its body parked far out,
    // its box's ends re-sorted there and cut off the sweep's axis lists
    // (as freeRider's bail body), its sweep entry, its dynamics and its
    // record freed; its sound has its turn (and goes).
    int32_t dynamicsObjectRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<DynamicsRecord> record = objectDynamicsRecord(ee::Ptr<zge::SceneObject>(message->target));
        const ee::Ptr<zge::Dynamics> dynamics = record->dynamics;
        const ee::Float parked(kParkedBodyCoordinate);
        dynamics->bodies->position.x = parked;
        dynamics->bodies->position.y = parked;
        dynamics->bodies->position.z = parked;
        zge::sweepUpdateBody(levelSweep(), dynamics->bodies);
        for (uint32_t axis = 0; axis < 3; ++axis)
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
        objectSoundMessage(message, dynamicsObjectSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectRemoved, Verified);

    // dynamicsObjectTouched (the touch message): its sound has its turn.
    int32_t dynamicsObjectTouched(ee::Ptr<zge::EngineMessage> message)
    {
        objectSoundMessage(message, dynamicsObjectSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectTouched, Verified);

    // dynamicsObjectSaveSize (the save-size message): 32 bytes; its sound
    // has its turn.
    int32_t dynamicsObjectSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = kDynamicsSaveBytes;
        objectSoundMessage(message, dynamicsObjectSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectSaveSize, Verified);

    // restoreDynamicsObject (the restore message): the object put back
    // where it started (resetDynamicsObject's reset); its sound has its
    // turn.
    int32_t restoreDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        resetDynamicsObject(objectDynamicsRecord(ee::Ptr<zge::SceneObject>(message->target)), 1);
        objectSoundMessage(message, dynamicsObjectSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreDynamicsObject, Verified);

    // saveDynamicsObject (the save message): the record asked for (its
    // answer unused: nothing is written); its sound has its turn.
    int32_t saveDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        objectDynamicsRecord(ee::Ptr<zge::SceneObject>(message->target));
        objectSoundMessage(message, dynamicsObjectSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveDynamicsObject, Verified);

    // initDynamicsLevelObject (the "DynamicsObject" type's set-up): the
    // node shown but not tested (its body collides for it); its dynamics
    // made from the definition its class's name hash names; when made, its
    // removed, restore, save, save-size and touch messages and its sound.
    // Whether it was made.
    uint32_t initDynamicsLevelObject(ee::Ptr<zge::SceneObject> sceneObject)
    {
        const uint64_t node = objectNodeHandle(sceneObject);
        rider::collision::showNode(node, 1);
        rider::collision::setNodeCollidable(node, 0);
        const ee::Ptr<zge::Node> nodeRecord(static_cast<uint32_t>(node));
        const uint32_t made = createDynamicsObject(sceneObject, nodeRecord, zge::objectClass(nodeRecord)->nameHash);
        if (made != 1)
            return 0;
        zge::subscribe(sceneObject, zge::kRemovedMessage, zge::MessageHandler(sym::dynamicsObjectRemoved));
        zge::subscribe(sceneObject, zge::kRestoreMessage, zge::MessageHandler(sym::restoreDynamicsObject));
        zge::subscribe(sceneObject, zge::kSaveMessage, zge::MessageHandler(sym::saveDynamicsObject));
        zge::subscribe(sceneObject, zge::kSaveSizeMessage, zge::MessageHandler(sym::dynamicsObjectSaveSize));
        zge::subscribe(sceneObject, kTouchMessage, zge::MessageHandler(sym::dynamicsObjectTouched));
        startObjectSound(sceneObject, dynamicsObjectSound(sceneObject));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initDynamicsLevelObject, Verified);
}
