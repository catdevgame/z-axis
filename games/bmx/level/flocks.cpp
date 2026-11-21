// BMX XXX's "FlockObj" level object type: an object that is not drawn,
// which a flock of birds (birds.cpp) follows. BMX XXX's own code (its game
// block); no other build names its functions, so the names are from what
// each does.
//
// A flock rests where the level put it until a rider comes within 10 of it.
// Then it flies: it climbs from its rest, circling and swinging up and down,
// by the time it has flown, and its birds are sent up after it. Once the
// rider is far enough it runs that flight backwards and rests again. Each
// update it adds up its birds' positions and velocities for them to steer by.
//
// Up is -y.

#include "entity_messages.h"
#include "flocks.h"
#include "level_calls.h"

#include "bmx/game/player_riders.h"
#include "bmx/profile/players.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/flocks.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        constexpr uint32_t kTypeName = sym::flockTypeName; // "FlockObj"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // The working record every flock's update copies its tag data into.
        inline ee::Global<FlockRecord, sym::g_flockState> g_flockState;

        // A frame's share of a second (1 / 60) and the angle units in a
        // degree (65536 / 360): the returning and the flying state read
        // their own.
        inline ee::Global<ee::Float, sym::g_flockReturnFrameSeconds> g_flockReturnFrameSeconds;
        inline ee::Global<ee::Float, sym::g_flockReturnDegreesToAngle> g_flockReturnDegreesToAngle;
        inline ee::Global<ee::Float, sym::g_flockFlightFrameSeconds> g_flockFlightFrameSeconds;
        inline ee::Global<ee::Float, sym::g_flockFlightDegreesToAngle> g_flockFlightDegreesToAngle;

        // A resting flock flies with a rider nearer than 10; a returning one
        // with a rider nearer than the root of 90; a flying one returns with
        // the rider further than the root of 128.
        constexpr ee::Float kRiseDistanceSquared(100.0f);
        constexpr ee::Float kRiseAgainDistanceSquared(90.0f);
        constexpr ee::Float kReturnDistanceSquared(128.0f);
        // The flight reaches its full circle and swing after 4 seconds.
        constexpr ee::Float kSpreadASecond(0.25f);
        // The birds' summed velocity is cut to this length when its square
        // is over this (the original compares the square with the length).
        constexpr ee::Float kMostVelocitySum(8.0f);

        // updateFlock's vectors, as its frame holds them: each state has
        // its own.
        struct FlightVectors
        {
            zge::Vec4 climb;
            zge::Vec4 circle;
            zge::Vec4 bob;
            zge::Vec4 offset; // the three added up; before that, the flock to the rider
        };
        struct RestingVectors
        {
            zge::Vec4 positions;
            zge::Vec4 velocities;
            zge::Vec4 toRider;
            zge::Vec4 birdVelocity;
        };
        struct ReturningVectors
        {
            zge::Vec4 velocities;
            zge::Vec4 at;
            zge::Vec4 _20;
            zge::Vec4 positions;
            FlightVectors flight;
            zge::Vec4 birdVelocity;
        };
        struct FlyingVectors
        {
            zge::Vec4 positions;
            zge::Vec4 velocities;
            zge::Vec4 at;
            FlightVectors flight;
            zge::Vec4 birdVelocity;
        };

        // Degrees as an engine angle, as the compiled code turns them
        // (towards zero, then the low 16 bits).
        zge::Angle angleOfDegrees(ee::Float degrees, ee::Float degreesToAngle)
        {
            return static_cast<zge::Angle>(ee::toInt(degrees * degreesToAngle));
        }
    }

    ee::Ptr<zge::SceneObject> nextFlockBird(ee::Ptr<zge::SceneObject> bird);
    uint32_t initFlock(ee::Ptr<zge::SceneObject> flock);
    void copyFlockVector(const zge::Vec3 *from, zge::Vec3 *to);
    void copyFlockVectorOut(const zge::Vec3 *from, zge::Vec3 *to);
    ee::Ptr<FlockRecord> loadFlockState(ee::Ptr<zge::SceneObject> flock);
    void storeFlockState(ee::Ptr<zge::SceneObject> flock, ee::Ptr<FlockRecord> record);
    int32_t flockActivated(ee::Ptr<zge::EngineMessage> message);
    int32_t updateFlock(ee::Ptr<zge::EngineMessage> message);
    int32_t flockTouched(ee::Ptr<zge::EngineMessage> message);

    namespace
    {
        // The flying flock put where its flight has it: its rest, up by the
        // flight's time, and a circle and an up and down swing that both
        // grow to their full size over the flight's first seconds. `at`'s w
        // and the three parts' are never set: the object's position gets
        // what the frame held there, as in the original.
        void placeFlyingFlock(ee::Ptr<zge::SceneObject> flock, ee::Ptr<FlockRecord> record, zge::Vec4 &at,
                              FlightVectors &flight, ee::Float degreesToAngle)
        {
            at.xyz() = record->restPosition;
            ee::Float spread = record->flightTime * kSpreadASecond;
            if (ee::Float(1.0f) < spread)
                spread = ee::Float(1.0f);

            flight.climb.x = ee::Float(0.0f);
            flight.climb.z = ee::Float(0.0f);
            flight.climb.y = -(record->flightTime * record->climbRate);

            flight.circle.x = zge::angleSin(angleOfDegrees(record->flightTime * record->circleRate, degreesToAngle)) * record->circleRadius;
            flight.circle.y = ee::Float(0.0f);
            flight.circle.z = zge::angleCos(angleOfDegrees(record->flightTime * record->circleRate, degreesToAngle)) * record->circleRadius;
            flight.circle = flight.circle * spread;

            flight.bob.x = ee::Float(0.0f);
            flight.bob.z = ee::Float(0.0f);
            flight.bob.y = zge::angleCos(angleOfDegrees(record->flightTime * record->bobRate, degreesToAngle)) * record->bobHeight;
            flight.bob = flight.bob * spread;

            flight.offset = flight.circle + flight.bob;
            flight.offset = flight.offset + flight.climb;
            at = flight.offset + at;
            zge::setObjectPosition(flock, &at);
        }

        // The flock's birds' positions and velocities added up for them
        // (birdPositionSum, birdVelocitySum), the velocities cut to
        // kMostVelocitySum.
        void sumFlockBirds(ee::Ptr<FlockRecord> record, zge::Vec4 &positions, zge::Vec4 &velocities, zge::Vec4 &birdVelocity)
        {
            positions = zge::kZero;
            velocities = zge::kZero;
            for (ee::Ptr<zge::SceneObject> bird = record->firstBird; bird; bird = nextFlockBird(bird))
            {
                positions = positions + *zge::objectPosition(bird);
                zge::tagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, velocity), &birdVelocity);
                velocities = velocities + birdVelocity;
            }
            if (kMostVelocitySum < zge::squaredLength(velocities))
            {
                velocities = zge::unit(velocities);
                velocities = zge::scaledAfterMeasure(velocities, kMostVelocitySum);
            }
            record->birdPositionSum = positions.xyz();
            record->birdVelocitySum = velocities.xyz();
        }
    }

    // registerFlockType: the "FlockObj" type, made as a plain entity and set
    // up by initFlock.
    uint32_t registerFlockType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initFlock));
    }
    READABLE_FUNCTION(registerFlockType, Verified);

    // addFlockBird: the bird becomes the flock's first, and the flock counts
    // one more. (The bird takes the first before it as its next itself.)
    void addFlockBird(ee::Ptr<zge::SceneObject> flock, ee::Ptr<zge::SceneObject> bird)
    {
        const uint32_t count = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, birdCount))) + 1;
        zge::setTagWord(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, firstBird), bird);
        game::tail(zge::setTagWord, zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, birdCount), count);
    }
    READABLE_FUNCTION(addFlockBird, Verified);

    // firstFlockBird: the flock's first bird, 0 for none.
    ee::Ptr<zge::SceneObject> firstFlockBird(ee::Ptr<zge::SceneObject> flock)
    {
        return ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, firstBird))));
    }
    READABLE_FUNCTION(firstFlockBird, Verified);

    // initFlock (the type's init): riders pass through it and it is not
    // drawn; its messages subscribed; where it is becomes its rest. Always 1.
    uint32_t initFlock(ee::Ptr<zge::SceneObject> flock)
    {
        ee::Local<zge::Vec4> position;
        // Its node is looked up twice; the first is not used.
        zge::objectLevelNode(flock);
        rider::collision::setNodeCollidable(objectNodeHandle(flock), 0);
        zge::setSceneObjectShown(flock, 0);
        loadFlockState(flock);
        zge::subscribe(flock, kRiderTouchMessage, zge::MessageHandler(sym::flockTouched));
        zge::subscribe(flock, kActivateMessage, zge::MessageHandler(sym::flockActivated));
        zge::subscribe(flock, zge::kUpdateMessage, zge::MessageHandler(sym::updateFlock));
        *position = *zge::objectPosition(flock);
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restPosition), position.address());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restedAt), position.address());
        *position = zge::kZero;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initFlock, Verified);

    // copyFlockVector, copyFlockVectorOut: x, y and z copied. The same code
    // twice: loadFlockState calls the first, storeFlockState the second.
    void copyFlockVector(const zge::Vec3 *from, zge::Vec3 *to)
    {
        to->x = from->x;
        to->y = from->y;
        to->z = from->z;
    }
    READABLE_FUNCTION(copyFlockVector, Verified);

    void copyFlockVectorOut(const zge::Vec3 *from, zge::Vec3 *to)
    {
        to->x = from->x;
        to->y = from->y;
        to->z = from->z;
    }
    READABLE_FUNCTION(copyFlockVectorOut, Verified);

    // loadFlockState: the flock's tag data copied out into the working
    // record (all but its bird count), which it answers.
    ee::Ptr<FlockRecord> loadFlockState(ee::Ptr<zge::SceneObject> flock)
    {
        ee::Local<zge::Vec4> vector;
        const ee::Ptr<FlockRecord> record = &*g_flockState;
        record->frozen = static_cast<uint8_t>(zge::tagByte(zge::objectComponent(flock), kFlockTagByte, offsetof(FlockRecord, frozen)));
        record->state = static_cast<uint8_t>(zge::tagByte(zge::objectComponent(flock), kFlockTagByte, offsetof(FlockRecord, state)));
        record->firstBird = ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, firstBird))));
        record->flightTime = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, flightTime));
        record->unusedFloat = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, unusedFloat));
        record->climbRate = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, climbRate));
        record->bobHeight = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, bobHeight));
        record->bobRate = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, bobRate));
        record->circleRadius = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, circleRadius));
        record->circleRate = zge::tagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, circleRate));
        zge::tagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restPosition), vector);
        copyFlockVector(&vector->xyz(), &record->restPosition);
        zge::tagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, unusedVector), vector);
        copyFlockVector(&vector->xyz(), &record->unusedVector);
        zge::tagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restedAt), vector);
        copyFlockVector(&vector->xyz(), &record->restedAt);
        zge::tagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, birdPositionSum), vector);
        copyFlockVector(&vector->xyz(), &record->birdPositionSum);
        zge::tagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, birdVelocitySum), vector);
        copyFlockVector(&vector->xyz(), &record->birdVelocitySum);
        return record;
    }
    READABLE_FUNCTION_WAITS(loadFlockState, Verified);

    // storeFlockState: the record copied back into the flock's tag data
    // (all but its bird count).
    void storeFlockState(ee::Ptr<zge::SceneObject> flock, ee::Ptr<FlockRecord> record)
    {
        ee::Local<zge::Vec4> vector;
        zge::setTagByte(zge::objectComponent(flock), kFlockTagByte, offsetof(FlockRecord, frozen), record->frozen);
        zge::setTagByte(zge::objectComponent(flock), kFlockTagByte, offsetof(FlockRecord, state), record->state);
        zge::setTagWord(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, firstBird), record->firstBird);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, flightTime), record->flightTime);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, unusedFloat), record->unusedFloat);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, climbRate), record->climbRate);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, bobHeight), record->bobHeight);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, bobRate), record->bobRate);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, circleRadius), record->circleRadius);
        zge::setTagFloat(zge::objectComponent(flock), kFlockTagWord, offsetof(FlockRecord, circleRate), record->circleRate);
        copyFlockVectorOut(&record->restPosition, &vector->xyz());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restPosition), vector.address());
        copyFlockVectorOut(&record->unusedVector, &vector->xyz());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, unusedVector), vector.address());
        copyFlockVectorOut(&record->restedAt, &vector->xyz());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, restedAt), vector.address());
        copyFlockVectorOut(&record->birdPositionSum, &vector->xyz());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, birdPositionSum), vector.address());
        copyFlockVectorOut(&record->birdVelocitySum, &vector->xyz());
        zge::setTagVector(zge::objectComponent(flock), kFlockTagVector, offsetof(FlockRecord, birdVelocitySum), vector.address());
    }
    READABLE_FUNCTION_WAITS(storeFlockState, Verified);

    // flockActivated (kActivateMessage): taken, and nothing done.
    int32_t flockActivated(ee::Ptr<zge::EngineMessage> /*message*/)
    {
        return 1;
    }
    READABLE_FUNCTION(flockActivated, Draft);

    // updateFlock (the frame's update), by its state:
    //   resting    the rider nearest it within 10: it flies, and its perched
    //              birds turn wary; else where it is stays its rest
    //   returning  the flight's time runs down, the flock placed by it; at 0
    //              it rests at its rest. The current player's rider close
    //              again: it flies
    //   flying     the flight's time runs, the flock placed by it; the
    //              current player's rider far away: it returns
    // Its birds' positions and velocities are added up for them in every
    // state that moves it. Always 1.
    int32_t updateFlock(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<FlyingVectors> flying;
        const ee::Ptr<zge::SceneObject> flock(message->target);
        const ee::Ptr<FlockRecord> record = loadFlockState(flock);
        if (record->frozen == 0)
        {
            switch (record->state)
            {
            case kFlockResting:
            {
                ee::Local<RestingVectors> resting;
                *g_nearestRiderDistanceSquared = ee::Float(-1.0f);
                rider::forEachRider(rider::RiderVisit(sym::nearestRiderVisit), flock);
                const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider::riderOf(*g_nearestRider)->messenger);
                const ee::Ptr<zge::Vec4> position = zge::objectPosition(flock);
                resting->toRider = *riderPosition - *position;
                if (zge::squaredLength(resting->toRider) < kRiseDistanceSquared)
                {
                    record->state = kFlockFlying;
                    resting->positions = zge::kZero;
                    resting->velocities = zge::kZero;
                    for (ee::Ptr<zge::SceneObject> bird = record->firstBird; bird; bird = nextFlockBird(bird))
                    {
                        const ee::Ptr<zge::Vec4> birdPosition = zge::objectPosition(bird);
                        zge::tagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, velocity), &resting->birdVelocity);
                        resting->velocities = resting->velocities + resting->birdVelocity;
                        resting->positions = resting->positions + *birdPosition;
                        if (zge::tagByte(zge::objectComponent(bird), kFlockTagByte, offsetof(BirdRecord, state)) == kBirdPerched)
                            zge::setTagByte(zge::objectComponent(bird), kFlockTagByte, offsetof(BirdRecord, state), kBirdWary);
                    }
                    record->birdPositionSum = resting->positions.xyz();
                    record->birdVelocitySum = resting->velocities.xyz();
                }
                else
                {
                    record->restPosition = position->xyz();
                }
                break;
            }
            case kFlockReturning:
            {
                ee::Local<ReturningVectors> returning;
                const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider::riderOf(riderIndexOf(currentPlayer()))->messenger);
                returning->flight.offset = *riderPosition - *zge::objectPosition(flock);
                if (zge::squaredLength(returning->flight.offset) < kRiseAgainDistanceSquared)
                    record->state = kFlockFlying;
                record->flightTime = record->flightTime - *g_flockReturnFrameSeconds;
                if (record->flightTime < ee::Float(0.0f))
                {
                    record->flightTime = ee::Float(0.0f);
                    record->state = kFlockResting;
                    returning->at.xyz() = record->restPosition;
                    zge::setObjectPosition(flock, &returning->at);
                    record->restedAt = returning->at.xyz();
                    record->unusedVector = zge::Vec3{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f)};
                }
                placeFlyingFlock(flock, record, returning->at, returning->flight, *g_flockReturnDegreesToAngle);
                sumFlockBirds(record, returning->positions, returning->velocities, returning->birdVelocity);
                break;
            }
            case kFlockFlying:
            {
                const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider::riderOf(riderIndexOf(currentPlayer()))->messenger);
                flying->flight.offset = *riderPosition - *zge::objectPosition(flock);
                if (kReturnDistanceSquared < zge::squaredLength(flying->flight.offset))
                    record->state = kFlockReturning;
                record->flightTime = record->flightTime + *g_flockFlightFrameSeconds;
                placeFlyingFlock(flock, record, flying->at, flying->flight, *g_flockFlightDegreesToAngle);
                sumFlockBirds(record, flying->positions, flying->velocities, flying->birdVelocity);
                break;
            }
            default:
                break;
            }
        }
        storeFlockState(flock, record);
        zge::updatePose(flock);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateFlock, Verified);

    // flockTouched (a rider touches it): it flies. Always 1.
    int32_t flockTouched(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> flock(message->target);
        const ee::Ptr<FlockRecord> record = loadFlockState(flock);
        record->state = kFlockFlying;
        storeFlockState(flock, record);
        zge::updatePose(flock);
        return 1;
    }
    READABLE_FUNCTION_WAITS(flockTouched, Draft);
}
