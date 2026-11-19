// BMX XXX's dynamics objects met by a rider (dynamics_objects.h): the push
// that wakes one, and the level sweep's handler for two bodies touching, of
// which one is a rider's and the other a dynamics object's. BMX XXX's own
// code; the push is Aggressive Inline's line
// (game/aggressive/level/dynamics_objects.cpp) with BMX XXX's numbers.

#include "dynamics_objects.h"
#include "entity_messages.h"
#include "level_calls.h"
#include "object_components.h"
#include "water_wake.h"

#include "bmx/game/player_riders.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/dynamics_contacts.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The push: how much of its strength lifts the object, the random
        // part more on x and z, and the random spins' steps.
        inline ee::Global<ee::Float, sym::g_pushLift> g_pushLift; // 0.7
        inline ee::Global<ee::Float, sym::g_pushSpreadX> g_pushSpreadX;
        inline ee::Global<ee::Float, sym::g_pushSpreadZ> g_pushSpreadZ;
        inline ee::Global<ee::Float, sym::g_pushSpinX> g_pushSpinX;
        inline ee::Global<ee::Float, sym::g_pushSpinZ> g_pushSpinZ;
        inline ee::Global<ee::Float, sym::g_pushSpinY> g_pushSpinY;
        // The riders' gravity, a frame; a pushed body weighs twice its mass
        // under it, a second.
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        constexpr ee::Float kFramesPerSecond(60.0f);

        // A touch carries the bodies' relative velocity over this (a frame's seconds).
        inline ee::Global<ee::Float, sym::g_bodyTouchSeconds> g_bodyTouchSeconds;
        // The rider's mass as an awake body is pushed by it.
        inline ee::Global<ee::Float, sym::g_riderKnockMass> g_riderKnockMass; // 750
        // The touch's velocity times this pushes the object.
        constexpr ee::Float kTouchPushScale(40.0f);

        // A body's record word names a rider (its index in the low byte)
        // when its top four bits are set; any other is its DynamicsRecord.
        constexpr uint32_t kRiderBodyMark = 0xF0000000u;
        constexpr uint8_t kNoRider = 0xFF;

        // How a rider touches an object, by the rider's physics state less
        // one (the states' own names are the rider's); bump for any other.
        constexpr TouchKind kTouchByPhysicsState[] = {
            kTouchBump2, kTouchCrash, kTouchBump2, kTouchBump, kTouchBump2, kTouchBump, kTouchBump2,
            kTouchBump2, kTouchBump, kTouchBump, kTouchCrash, kTouchCrash, kTouchCrash,
        };
        constexpr uint32_t kTouchPhysicsStates = sizeof(kTouchByPhysicsState) / sizeof(kTouchByPhysicsState[0]);
    }

    // pushDynamicsObject: a dynamics object knocked by `impulse` (its
    // strength given, else the impulse's length): woken, its body weighing
    // twice its mass, moving with the impulse lifted by the strength and a
    // random part more on x and z, with a random spin about each axis, and
    // awake. The rider that pushes it is not read. Returns 1.
    uint32_t pushDynamicsObject(ee::Ptr<rider::Rider>, ee::Address object, ee::Ptr<zge::Vec4> impulse, ee::Ptr<ee::Float> strengthGiven)
    {
        const auto random = [](uint32_t mask) { return ee::toFloat(static_cast<int32_t>(zge::randomNumber() & mask)); };
        const ee::Float strength = strengthGiven ? *strengthGiven : zge::vec3Length(&impulse->xyz());
        const ee::Ptr<DynamicsRecord> record(static_cast<uint32_t>(objectComponentKind(ee::Ptr<zge::SceneObject>(object))));
        const ee::Ptr<zge::RigidBody> body = record->dynamics->bodies;
        zge::wakeDynamics(ee::Ptr<zge::SceneObject>(record->sceneObject), record->dynamics, 1);
        body->force.x = 0.0f;
        body->force.z = 0.0f;
        body->force.y = *g_gravity * kFramesPerSecond * kFramesPerSecond * (body->mass + body->mass);
        body->velocity.y = impulse->y - strength * *g_pushLift;
        body->velocity.x = impulse->x;
        body->velocity.x = body->velocity.x + impulse->x * (random(3) * *g_pushSpreadX);
        body->velocity.z = impulse->z;
        body->velocity.z = body->velocity.z + impulse->z * (random(3) * *g_pushSpreadZ);
        body->spin.x = random(7) * *g_pushSpinX + ee::Float(1.0f);
        body->spin.z = random(7) * *g_pushSpinZ + ee::Float(1.0f);
        body->spin.y = random(7) * *g_pushSpinY + ee::Float(1.0f);
        record->flags = kDynamicsAwake;
        return 1;
    }
    READABLE_FUNCTION_WAITS(pushDynamicsObject, Verified);

    // bodiesTouch (the level sweep's handler for a pair of rigid bodies): a
    // rider's body touching a dynamics object's. Nothing for two bodies of
    // one record, two riders' or two objects', nor when the engine says the
    // contact no longer holds, nor for a rider whose first flag is set.
    // The object is sent the rider's touch (the bodies' relative velocity
    // over a frame, the kind by the rider's physics state, the contact's
    // normal); when the game takes the touch as an activation nothing more
    // happens. Otherwise an object asleep is pushed by the rider and woken,
    // an awake one is pushed at the contact's point; the object hears of the
    // touch, and unless it lets the rider through the engine keeps the
    // contact.
    void bodiesTouch(ee::Ptr<zge::RigidBody> a, ee::Ptr<zge::RigidBody> b)
    {
        ee::Local<RiderTouch> touch;
        ee::Local<zge::EngineMessage> message;
        ee::Local<zge::OrientedBox> riderBox;
        ee::Local<zge::OrientedBox> objectBox;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> pushNormal;
        ee::Local<zge::Vec4> push;
        ee::Local<ee::Float> depth;
        if (a->record == b->record)
            return;
        const ee::Ptr<zge::RigidBody> bodies[2] = {a, b};
        ee::Ptr<DynamicsRecord> records[2];
        ee::Ptr<zge::Dynamics> dynamics[2]; // each body's dynamics: a rider's model's, or the object's
        uint8_t riders[2];
        for (uint32_t i = 0; i < 2; ++i)
        {
            const uint32_t word = bodies[i]->record;
            riders[i] = kNoRider;
            if ((word & kRiderBodyMark) == kRiderBodyMark)
            {
                riders[i] = static_cast<uint8_t>(word);
                dynamics[i] = rider::riderOf(riders[i])->riderDynamics;
            }
            else
            {
                records[i] = ee::Ptr<DynamicsRecord>(word);
                dynamics[i] = records[i]->dynamics;
            }
        }
        if (riders[0] != kNoRider && riders[1] != kNoRider)
            return;
        if (records[0] && records[1])
            return;
        if (zge::pairStillTouching(a, b) == 0)
            return;

        for (uint32_t i = 0; i < 2; ++i)
        {
            if (riders[i] == kNoRider)
                continue;
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riders[i]);
            if (rider::hasFlag(&*rider, rider::kFlagNoPlayer))
                continue;
            const uint32_t other = i != 0 ? 0 : 1;
            if (!records[other])
                continue;
            message->target = dynamics[other]->sceneObject;
            message->sender = ee::Address(rider->messenger);
            message->type = kRiderTouchMessage;
            message->flags = zge::kMessageNow;
            message->data = touch;
            zge::memoryFill(touch, 0, sizeof(RiderTouch));
            const uint32_t state = rider::physicsState(&*rider) - 1u;
            touch->kind = state < kTouchPhysicsStates ? kTouchByPhysicsState[state] : kTouchBump;
            touch->pushOff = 1;
            touch->velocity = bodies[i]->velocity - bodies[other]->velocity;
            touch->velocity = touch->velocity * *g_bodyTouchSeconds;
            *depth = 0.0f;
            zge::memoryFill(riderBox, 0, sizeof(zge::OrientedBox));
            zge::memoryFill(objectBox, 0, sizeof(zge::OrientedBox));
            if (zge::bodyBox(bodies[other], objectBox) == 0)
                return;
            if (zge::bodyBox(bodies[i], riderBox) == 0)
                return;
            zge::orientedBoxContact(riderBox, objectBox, &*point, &touch->normal, &*depth);
            touch->hasNormal = 1;
            if (riderActivatedObject(message) != 0)
                return;
            *push = touch->velocity * kTouchPushScale;
            if ((records[other]->flags & kDynamicsAwake) == 0)
            {
                pushDynamicsObject(rider, message->target, push, ee::Ptr<ee::Float>());
                objectWoken(records[other]);
                records[other]->flags |= kDynamicsAwake;
            }
            else
            {
                *depth = 0.0f;
                zge::memoryFill(riderBox, 0, sizeof(zge::OrientedBox));
                zge::memoryFill(objectBox, 0, sizeof(zge::OrientedBox));
                if (zge::bodyBox(bodies[other], objectBox) == 0)
                    return;
                if (zge::bodyBox(bodies[i], riderBox) == 0)
                    return;
                zge::orientedBoxContact(riderBox, objectBox, &*point, &*pushNormal, &*depth);
                zge::pushBodyFromPoint(riderBox, bodies[other], push, point, *g_riderKnockMass);
            }
            if (dynamics[other]->sceneObject)
                zge::sendEngineMessage(message);
            if (touch->pushOff == 0)
                return;
            zge::pairTouched(bodies[i], bodies[other]);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(bodiesTouch, Verified);

    namespace
    {
        // The faces a body touches on one collision part, summed up by
        // dynamicsObjectContact: how many, their normals, their centres.
        inline ee::Global<uint32_t, sym::g_contactFaceCount> g_contactFaceCount;
        inline ee::Global<zge::Vec4, sym::g_contactNormalSum> g_contactNormalSum;
        inline ee::Global<zge::Vec4, sym::g_contactCentreSum> g_contactCentreSum;

        // The level object types a body's contact tells apart, by their names' hashes.
        constexpr uint32_t kDynamicsObjectType = 0x0A10F094; // "DynamicsObject"
        constexpr uint32_t kCarType = 0x4762;                // "Car"
        constexpr uint32_t kFastCarType = 0x0A688722;        // "FastCar"
        constexpr uint32_t kWaterSurfaceType = 0x0D102F05;   // "WaterSurface"
        // The fast cars a body does not leave faster than kNamedCarsMostZ
        // along z: nine names that differ in a last digit, 1 to 9, by their
        // hashes (the names are the level's; not read here).
        constexpr uint32_t kNamedCarsAfter = 0x0E047AD0, kNamedCarsLast = 0x0E047AD9;
        constexpr ee::Float kNamedCarsMostZ(-10.0f);

        // A summed normal shorter than this (squared) is replaced by the
        // direction from the car to the body.
        inline ee::Global<ee::Float, sym::g_carNormalSmall> g_carNormalSmall;         // 0.01
        inline ee::Global<ee::Float, sym::g_fastCarNormalSmall> g_fastCarNormalSmall; // 0.01
        // A body moving down off a fast car faster than this is sent up instead (up is -y).
        inline ee::Global<ee::Float, sym::g_fastCarMostDown> g_fastCarMostDown;       // -0.02
        // What a body in water keeps of its velocity and spin, each face.
        inline ee::Global<ee::Float, sym::g_waterBodyDamping> g_waterBodyDamping;     // 0.65
        inline ee::Global<ee::Float, sym::g_faceThird> g_faceThird;                   // 1 / 3
        // A touch's and a wake's velocity: the body's over a frame.
        inline ee::Global<ee::Float, sym::g_faceTouchSeconds> g_faceTouchSeconds;     // 1 / 60
        inline ee::Global<ee::Float, sym::g_wakeSeconds> g_wakeSeconds;               // 1 / 60

        // The rider's contact kind set when the rider's body is in water
        // (the rider's code has no name for it yet).
        constexpr uint32_t kContactKind10 = 10;

        // The car's push on the body it met: at the faces' average centre,
        // along their summed normal made a unit (the direction from the car
        // to the body when the sum is too short), with the car's velocity.
        void carPushesBody(ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::SceneObject> car, ee::Float normalSmall, ee::Ptr<zge::Vec4> carVelocity)
        {
            zge::objectVelocity(car, carVelocity);
            if (!(normalSmall < zge::dot(*g_contactNormalSum, *g_contactNormalSum)))
                *g_contactNormalSum = body->position - *zge::objectPosition(car);
            ee::Float length;
            *g_contactNormalSum = zge::unit(*g_contactNormalSum, length);
            zge::contactImpulse(body, &*g_contactCentreSum, &*g_contactNormalSum, &*carVelocity);
        }
    }

    // dynamicsObjectContact (the contact callback of a dynamics, asked about
    // each face a body touches, and once more with no object when the search
    // is over): the faces of one collision part are counted and summed.
    //
    // When the part changes, the part before is settled first, by its
    // object's type: a car or a fast car pushes the body (and a fast car
    // turns a falling body upwards, some of them holding its z speed); a
    // water surface damps the body's velocity and spin. Then the sums start
    // again.
    //
    // The face's object is then told of the touch (a crash's, at the
    // face's centre, with the body's velocity over a frame), from the rider
    // when the dynamics is a rider's and the game does not take the touch as
    // an activation. Answers 0 for a face to ignore: another dynamics
    // object's (the sweep's pairs handle those), water (which starts a wake
    // and, for a rider, sets the rider's water contact), any face of a
    // dynamics that is no rider's, and what the touched object lets through.
    uint32_t dynamicsObjectContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> carVelocity;
        ee::Local<zge::Vec4> toPlane;
        ee::Local<zge::Vec4> centre;
        ee::Local<RiderTouch> touch;
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint64_t> waterFace;
        const ee::Ptr<zge::HitNode> lastPart = zge::dynamicsContactObject();
        if (lastPart != contact->object)
        {
            if (lastPart)
            {
                const ee::Ptr<zge::SceneObject> lastOwner = zge::nodeSceneObject(lastPart->node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
                if (!lastOwner)
                    return 1;
                const uint32_t faces = *g_contactFaceCount;
                if (faces == 0)
                    return 1;
                *g_contactCentreSum = *g_contactCentreSum * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(faces)));
                switch (zge::levelObjectTypeHash(lastOwner))
                {
                case kCarType:
                    carPushesBody(body, lastOwner, *g_carNormalSmall, carVelocity);
                    break;
                case kFastCarType:
                    carPushesBody(body, lastOwner, *g_fastCarNormalSmall, carVelocity);
                    if (*g_fastCarMostDown < body->velocity.y)
                        body->velocity.y = -body->velocity.y;
                    if (kNamedCarsAfter < zge::objectNameHash(lastOwner) && !(kNamedCarsLast < zge::objectNameHash(lastOwner)))
                    {
                        if (kNamedCarsMostZ < body->velocity.z)
                            body->velocity.z = kNamedCarsMostZ;
                    }
                    break;
                case kWaterSurfaceType:
                    body->velocity = body->velocity * *g_waterBodyDamping;
                    body->spin = body->spin * *g_waterBodyDamping;
                    break;
                default: // a dynamics object's among them
                    break;
                }
            }
            *g_contactFaceCount = 0;
            *g_contactNormalSum = zge::kZero;
            *g_contactCentreSum = zge::kZero;
        }
        if (!contact->object)
            return 1;
        const ee::Ptr<zge::Node> node = contact->object->node;
        const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
        if (!owner)
            return 1;
        ee::Ptr<rider::Rider> rider;
        const uint32_t riderIndex = rider::playerIndexOfNode(ee::Address(dynamics->sceneObject));
        if (riderIndex != 0xFF)
            rider = rider::riderOf(riderIndex);

        *centre = contact->vertices[0];
        *centre = *centre + contact->vertices[1];
        *centre = *centre + contact->vertices[2];
        *centre = *centre * *g_faceThird;
        *g_contactNormalSum = *g_contactNormalSum + contact->normal;
        *g_contactCentreSum = *g_contactCentreSum + *centre;
        touch->point = *centre;
        touch->velocity = body->velocity;
        touch->node = static_cast<uint32_t>(rider::collision::nodeHandle(ee::Address(node)));
        touch->face = contact->face;
        touch->kind = kTouchCrash;
        touch->pushOff = 1;
        touch->velocity = touch->velocity * *g_faceTouchSeconds;
        message->type = kRiderTouchMessage;
        message->flags = zge::kMessageNow;
        message->data = touch;
        message->target = owner;
        message->sender = 0;
        if (rider && !rider::hasFlag(&*rider, rider::kFlagNoPlayer))
        {
            message->sender = ee::Address(rider->messenger);
            if (riderActivatedObject(message) != 0)
                touch->pushOff = 0;
        }
        if (touch->pushOff == 0)
            return 0;

        *g_contactFaceCount = *g_contactFaceCount + 1;
        const uint32_t ownerType = zge::levelObjectTypeHash(owner);
        if (ownerType == kDynamicsObjectType)
            touch->pushOff = 0;
        else if (ownerType == kWaterSurfaceType)
        {
            // The wake starts where the body's box meets the water's plane,
            // under the face's first corner.
            zge::boxPointOnFacePlane(body, centre);
            *toPlane = contact->vertices[0] - *centre;
            const ee::Float off = zge::dot(contact->normal, *toPlane);
            *toPlane = zge::scaledAfterMeasure(contact->normal, off);
            *centre = *centre + *toPlane;
            touch->point = *centre;
            touch->velocity = body->velocity;
            touch->velocity = touch->velocity * *g_wakeSeconds;
            startWaterWake(ee::Ptr<zge::Vec4>(touch.address() + offsetof(RiderTouch, point)), ee::Ptr<zge::Vec4>(touch.address() + offsetof(RiderTouch, velocity)));
            touch->pushOff = 0;
            if (rider)
            {
                const uint32_t touchedNode = touch->node;
                const uint32_t touchedFace = touch->face;
                if (rider::collision::nodeFaceCount(static_cast<uint64_t>(static_cast<int32_t>(touchedNode))) == 0)
                    *waterFace = 0;
                else
                    *waterFace = (static_cast<uint64_t>(touchedFace) << 32) | touchedNode;
                rider::setContact(&*rider, &*waterFace, kContactKind10);
            }
        }
        else if (!rider)
            touch->pushOff = 0;
        if (touch->pushOff == 0)
            return 0;
        if (contact->object == lastPart)
            return touch->pushOff;
        zge::sendEngineMessage(message);
        return touch->pushOff;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectContact, Verified);
}
