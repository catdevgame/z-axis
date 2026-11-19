// BMX XXX's "Car" level object type: a traffic car on the engine's paths
// (the path followers Aggressive Inline's traffic cars use). BMX XXX's own
// code (its game block), Aggressive Inline's line (game/aggressive/level/
// traffic_cars.cpp, and driveTrafficCar in game/aggressive/effects.cpp): the
// names are that line's where the code matches, else from what each does
// and the type's strings ("Car", "%s.%s.move").
//
// A car moves along its segment every frame at its follower's speed and at
// the segment's end takes an exit the level entities pick (or carries on as
// the follower's end mode says); a class whose tag byte says so drives:
// it slows for the car ahead and for the traffic light at its segment's
// end. It is collided with only while a rider is near, turns its wheel
// bones by the distance moved, and carries its "move" sound.

#include "entities.h"
#include "entity_messages.h"
#include "level_calls.h"
#include "traffic_light_state.h"

#include "bmx/game/player_riders.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/cars.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        // The car's own tag data (objectComponent), 0x24 bytes.
        constexpr uint32_t kFollowerTag = 0;      // word: its path follower
        constexpr uint32_t kPathTag = 4;          // word: its first segment's id
        constexpr uint32_t kSpeedTag = 8;         // float: its speed (a driven car's most)
        constexpr uint32_t kExitTags = 0xC;       // what pickSegmentExit reads
        constexpr uint32_t kRandomTag = 0x18;     // word: a random number drawn at its first init
        constexpr uint32_t kSoundTag = 0x20;      // word: its move sound's handle
        constexpr uint32_t kComponentBytes = 0x24;
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kWheelRadiusTag = 4;   // float
        constexpr uint32_t kSoundNameTag = 8;     // text: the middle of its sounds' names
        constexpr uint32_t kDrivesTag = 0x1C;     // byte: it slows for cars and lights (driveCar)
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kTagText = 0x10;
        // newPathFollower's end mode for a car.
        constexpr uint32_t kCarEndMode = 2;

        constexpr uint32_t kTypeName = sym::carTypeName;                       // "Car"
        constexpr uint32_t kMoveSoundFormat = sym::bmx::carMoveSoundFormat;     // "%s.%s.move"
        constexpr uint32_t kTouchSoundFormat = sym::bmx::carTouchSoundFormat; // "%s.%s"
        // The cars' random numbers' generator, seeded from the collision clock when the type registers.
        inline ee::Global<uint32_t, sym::g_carSeed> g_carSeed;
        constexpr uint32_t kSeedOddFactor = 0x5EB0A82F, kSeedHalfFactor = 0x429EAF9D;
        // How many players the game has.
        inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;

        inline ee::Global<ee::Float, sym::g_carPi> g_carPi;
        // What a part that is not a wheel turns by each frame, over the wheel's circumference (150000).
        inline ee::Global<ee::Float, sym::g_carPartTurn> g_carPartTurn;
        // What a driven car's speed grows by in a frame (0.025).
        inline ee::Global<ee::Float, sym::g_carSpeedUp> g_carSpeedUp;
        // Speeds nearer than this are the same (1e-5).
        inline ee::Global<ee::Float, sym::g_carSameSpeed> g_carSameSpeed;
        // A rider within 20 of a car (400 squared) makes it collidable.
        constexpr ee::Float kNearSquared(400.0f);
        // A full turn in the engine's angle units, as a double (65536.0).
        constexpr uint64_t kTurnUnits = 0x40F0000000000000ull;

        // The bones a car's model may have that turn, by name hash (see fast_cars.cpp).
        constexpr uint32_t kTurningBones[5] = {0x09F7EE23, 0x00B8FD93, 0x0108EA73, 0x0109EA73, 0x010AEA73};
        constexpr uint32_t kSpentBone = 0x0AC6B0C2;
        constexpr uint32_t kMostTurningBones = 8;

        // A rider's flag byte 0. Scaffolding (unnamed-field): bit 0x02 has
        // no name yet; only riders with it set count as near a car.
        constexpr uint8_t kRiderFlag02 = 0x02;

        constexpr zge::MessageHandler kUpdateCar(sym::updateCar);
        constexpr zge::MessageHandler kCarTouched(sym::carTouched);
        constexpr zge::MessageHandler kDisableCar(sym::disableCar);
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // What a car keeps in a saved level state.
        struct CarSave
        {
            uint32_t segmentId;                 // +0x00
            uint32_t endMode;                   // +0x04
            ee::Float speed;                    // +0x08
            ee::Float along;                    // +0x0C
            uint8_t component[kComponentBytes]; // +0x10 its own tag data
        };
        static_assert(sizeof(CarSave) == 0x34);

        // What a driven car does with its speed (Aggressive Inline's Drive).
        enum Drive : uint32_t
        {
            kDriveEaseOff = 0, // 1 less
            kDriveBrake = 2,   // 0.25 less
            kDriveKeep = 3,
            kDriveSpeed = 4,   // g_carSpeedUp more, up to the car's speed
        };
    }

    uint32_t initCar(ee::Ptr<zge::SceneObject> car);
    ee::Float carSpeed(ee::Ptr<zge::SceneObject> car);
    ee::Ptr<zge::Follower> carFollower(ee::Ptr<zge::SceneObject> car);
    void setCarFollower(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::Follower> follower);
    void driveCar(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::Follower> follower);
    ee::Float carGap(ee::Ptr<zge::Follower> follower);
    uint32_t carCruise(ee::Float speed, ee::Float along, ee::Float toEnd);

    // registerCarType: the cars' random seed taken from the collision
    // clock, and the "Car" type, made as a plain entity and set up by initCar.
    uint32_t registerCarType()
    {
        *g_carSeed = zge::collisionClock();
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initCar));
    }
    READABLE_FUNCTION_WAITS(registerCarType, Verified);

    // initCar (the type's init): 0 when the level has no path segment of
    // the car's id. Else its messages subscribed, a follower made on the
    // segment at the car's speed and put where the car stands, its random
    // number drawn (once), its "move" sound started, the car shown. 1.
    uint32_t initCar(ee::Ptr<zge::SceneObject> car)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const ee::Ptr<zge::PathRecord> segment =
            zge::findPathRecord(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kPathTag)));
        if (!segment)
            return 0;
        zge::subscribe(car, zge::kUpdateMessage, kUpdateCar);
        zge::subscribe(car, zge::kRemovedMessage, zge::MessageHandler(sym::carRemoved));
        zge::subscribe(car, zge::kSaveSizeMessage, zge::MessageHandler(sym::carSaveSize));
        zge::subscribe(car, zge::kSaveMessage, zge::MessageHandler(sym::saveCar));
        zge::subscribe(car, zge::kRestoreMessage, zge::MessageHandler(sym::restoreCar));
        zge::subscribe(car, zge::kVelocityMessage, zge::MessageHandler(sym::carVelocity));
        zge::subscribe(car, kRiderTouchMessage, kCarTouched);
        zge::subscribe(car, kEnableMessage, zge::MessageHandler(sym::enableCar));
        zge::subscribe(car, kDisableMessage, kDisableCar);

        const ee::Float speed = carSpeed(car);
        const ee::Ptr<zge::Follower> follower = zge::newPathFollower(segment, kCarEndMode, speed, ee::Float(0.0f));
        setCarFollower(car, follower);
        zge::followerSnap(follower, zge::objectPosition(car));

        if (zge::tagWord(zge::objectComponent(car), kTagWord, kRandomTag) == 0)
        {
            const uint32_t seed = *g_carSeed;
            *g_carSeed = (seed & 1u) * kSeedOddFactor - (seed >> 1) * kSeedHalfFactor;
            zge::setTagWord(zge::objectComponent(car), kTagWord, kRandomTag, *g_carSeed);
        }

        zge::tagString(zge::levelObjectClassTag(car), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kMoveSoundFormat, kTypeName, className.address());
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(car);
        const uint32_t sound = soundEvent(kObjectSoundEvent, position, kObjectSoundStart, soundName.address(), kFullVolume);
        zge::setTagWord(zge::objectComponent(car), kTagWord, kSoundTag, sound);
        zge::setSceneObjectShown(car, 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCar, Draft);

    // carRemoved (the engine's removed message): its follower deleted, its
    // sound stopped.
    int32_t carRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        zge::deletePathFollower(carFollower(car));
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
        soundEvent(kObjectSoundEvent, position, kObjectSoundStop, sound, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(carRemoved, Draft);

    // updateCar (the frame's update; the data is the frame's seconds): see
    // the top of the file.
    int32_t updateCar(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> up;
        ee::Local<zge::Matrix> placing;
        ee::Local<zge::Vec4> previousPosition;
        ee::Local<zge::Vec4> toRider;
        ee::Local<uint32_t[8]> boneNames;
        ee::Local<ee::Ptr<uint32_t>[8]> nameCursors;
        ee::Local<ee::Ptr<zge::AnimationChannel>[8]> bones;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<zge::Quaternion> turn;
        ee::Local<zge::Vec4> axis;
        ee::Local<zge::Vec4> moved;

        const ee::Ptr<zge::SceneObject> car(message->target);
        zge::objectNameHash(car);
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        *previousPosition = *zge::objectPosition(car);
        const ee::Float seconds = *ee::Ptr<ee::Float>(message->data);
        if (zge::tagByte(zge::levelObjectClassTag(car), kTagByte, kDrivesTag) != 0)
            driveCar(car, follower);

        // Along the segment; past either end, onto the exit picked for that
        // end, else for the other end, else on as the follower does alone.
        const ee::Float speed = zge::followerSpeed(follower);
        const ee::Float length = zge::segmentEnd(zge::followerSegment(follower));
        const ee::Float next = zge::followerAlong(follower) * length + speed * seconds;
        if (next < ee::Float(0.0f) || length < next)
        {
            const uint32_t atStart = speed < ee::Float(0.0f) ? 1 : 0;
            uint32_t exit = pickSegmentExit(car, follower, atStart, kExitTags);
            if (exit == kNoSegmentExit)
                exit = pickSegmentExit(car, follower, atStart ^ 1u, kExitTags);
            if (exit == kNoSegmentExit)
            {
                zge::advanceFollower(follower, seconds);
            }
            else
            {
                zge::setFollowerSegment(follower, zge::findPathRecord(exit));
                zge::setFollowerSpeed(follower, speed);
                zge::setFollowerAlong(follower, ee::Float(0.0f) < speed ? ee::Float(0.0f) : ee::Float(1.0f));
            }
        }
        else
        {
            zge::advanceFollower(follower, seconds);
        }

        // Turned to the path (up is -y) and put on it.
        up->x = ee::Float(0.0f);
        up->y = ee::Float(-1.0f);
        up->z = ee::Float(0.0f);
        zge::followerOrientation(follower, &up->xyz(), placing);
        zge::setObjectRotationMatrix(car, placing);
        zge::setObjectPosition(car, &placing->translation());

        // Collidable only with a rider near: the first player's rider found
        // nearer than the limit settles it.
        ee::Float nearest = kNearSquared;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            if ((rider->flags[0] & kRiderFlag02) == 0)
                continue;
            *toRider = *rider::vec4(rider->position) - *zge::objectPosition(car);
            const ee::Float distanceSquared = zge::dot(*toRider, *toRider);
            if (distanceSquared < nearest)
            {
                nearest = distanceSquared;
                break;
            }
        }
        const uint64_t collisionNode = objectNodeHandle(car);
        if (nearest < kNearSquared)
            rider::collision::setNodeCollidable(collisionNode, 1);
        else
            rider::collision::setNodeCollidable(collisionNode, 0);

        // Its turning bones.
        const uint32_t modelBones = zge::modelBoneCount(car);
        if (modelBones >= 2)
        {
            for (uint32_t i = 0; i < 5; ++i)
                boneNames[i] = kTurningBones[i];
            const uint32_t count = modelBones < kMostTurningBones + 1 ? modelBones : kMostTurningBones;
            for (uint32_t i = 0; i < count; ++i)
            {
                // The first name left that a bone of the model has.
                nameCursors[i] = ee::Ptr<uint32_t>(boneNames.address());
                for (;;)
                {
                    bones[i] = zge::findAnimatorBone(car, *nameCursors[i]);
                    if (bones[i])
                        break;
                    const uint32_t tried = *nameCursors[i];
                    nameCursors[i] = nameCursors[i] + 1;
                    if (tried == boneNames[4])
                        break;
                }
                if (!bones[i])
                    continue;

                const ee::Float diameter = zge::tagFloat(zge::levelObjectClassTag(car), kTagWord, kWheelRadiusTag);
                const ee::Float circumference = (diameter + diameter) * *g_carPi;
                const uint32_t name = *nameCursors[i];
                zge::Angle angle;
                if (name == boneNames[0] || name == boneNames[1])
                {
                    *moved = *zge::objectPosition(car) - *previousPosition;
                    const ee::Float distance = zge::vec3Length(&moved->xyz());
                    const uint64_t turns = sce::libgcc::dpmul(sce::libgcc::fptodp(distance), kTurnUnits);
                    angle = static_cast<zge::Angle>(sce::libgcc::dptoli(sce::libgcc::dpdiv(turns, sce::libgcc::fptodp(circumference))));
                    axis->y = ee::Float(0.0f);
                    axis->x = ee::Float(1.0f);
                    axis->z = ee::Float(0.0f);
                }
                else
                {
                    angle = static_cast<zge::Angle>(ee::toInt(*g_carPartTurn / circumference));
                    if (name == boneNames[3])
                    {
                        axis->x = ee::Float(0.0f);
                        axis->z = ee::Float(1.0f);
                        axis->y = ee::Float(0.0f);
                    }
                    else if (name == boneNames[4])
                    {
                        axis->x = ee::Float(0.0f);
                        axis->y = ee::Float(-1.0f);
                        axis->z = ee::Float(0.0f);
                    }
                    else
                    {
                        axis->y = ee::Float(0.0f);
                        axis->x = ee::Float(1.0f);
                        axis->z = ee::Float(0.0f);
                    }
                }
                zge::quaternionFromAxisAngle(turn, &axis->xyz(), angle);
                zge::quaternionCopy(rotation, zge::boneRotation(bones[i]));
                zge::quaternionMultiply(rotation, rotation, turn);
                zge::setBoneRotation(bones[i], rotation);
                *nameCursors[i] = kSpentBone;
            }
        }

        zge::updatePose(car);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(car), kObjectSoundMove, sound, kFullVolume);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateCar, Draft);

    // carVelocity (the engine's velocity message): its speed along where it
    // faces (its orientation's third row, negated).
    int32_t carVelocity(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Matrix> facing;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Vec4> velocity(message->data);
        zge::quaternionToMatrix(facing, zge::objectOrientation(car));
        zge::vec3Negate(&velocity->xyz(), &facing->row(2).xyz());
        const ee::Float speed = ee::abs(zge::followerSpeed(carFollower(car)));
        *velocity = *velocity * speed;
        return 0;
    }
    READABLE_FUNCTION_WAITS(carVelocity, Draft);

    // carSaveSize (the engine's save size message).
    int32_t carSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(CarSave);
        return 0;
    }
    READABLE_FUNCTION(carSaveSize, Draft);

    // saveCar (the engine's save message): its follower's end mode,
    // segment, place and speed, and its own tag data.
    int32_t saveCar(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<CarSave> saved;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        saved->endMode = zge::followerEndMode(follower);
        saved->segmentId = zge::pathNameHash(zge::followerSegment(follower));
        saved->along = zge::followerAlong(follower);
        saved->speed = zge::followerSpeed(follower);
        zge::memoryCopy(saved->component, zge::objectComponent(car), kComponentBytes);
        zge::memoryCopy(message->data, saved, sizeof(CarSave));
        return 0;
    }
    READABLE_FUNCTION_WAITS(saveCar, Draft);

    // restoreCar (the engine's restore message): the saved state put back;
    // the follower is the car's present one.
    int32_t restoreCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        const ee::Ptr<CarSave> saved(message->data);
        zge::setFollowerSegment(follower, zge::findPathRecord(saved->segmentId));
        zge::setFollowerAlong(follower, saved->along);
        zge::setFollowerSpeed(follower, saved->speed);
        zge::setFollowerEndMode(follower, saved->endMode);
        zge::memoryCopy(zge::objectComponent(car), saved->component, kComponentBytes);
        setCarFollower(car, follower);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreCar, Draft);

    // carTouched (kRiderTouchMessage): the entities' touch sound under the
    // car's "Car.<class>" name.
    int32_t carTouched(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        zge::tagString(zge::levelObjectClassTag(ee::Ptr<zge::SceneObject>(message->target)), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kTouchSoundFormat, kTypeName, className.address());
        playTouchSound(message, soundName);
        return 0;
    }
    READABLE_FUNCTION_WAITS(carTouched, Draft);

    // enableCar (kEnableMessage): it runs again, shown and collidable.
    int32_t enableCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const uint64_t node = objectNodeHandle(car);
        zge::subscribe(car, zge::kUpdateMessage, kUpdateCar);
        zge::subscribe(car, kRiderTouchMessage, kCarTouched);
        zge::subscribe(car, kDisableMessage, kDisableCar);
        zge::setSceneObjectShown(car, 1);
        rider::collision::setNodeCollidable(node, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableCar, Draft);

    // disableCar (kDisableMessage): silent, hidden, not collidable and no
    // longer run; only an enable reaches it.
    int32_t disableCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const uint64_t node = objectNodeHandle(car);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(car), kObjectSoundMove, sound, 0);
        zge::setSceneObjectShown(car, 0);
        rider::collision::setNodeCollidable(node, 0);
        zge::unsubscribe(car, zge::kUpdateMessage, kUpdateCar);
        zge::unsubscribe(car, kRiderTouchMessage, kCarTouched);
        zge::unsubscribe(car, kDisableMessage, kDisableCar);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableCar, Draft);

    // carSpeed: the speed the car is tagged with.
    ee::Float carSpeed(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::objectComponent(car), kTagWord, kSpeedTag);
    }
    READABLE_FUNCTION(carSpeed, Draft);

    // carFollower: the car's path follower.
    ee::Ptr<zge::Follower> carFollower(ee::Ptr<zge::SceneObject> car)
    {
        return ee::Ptr<zge::Follower>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kFollowerTag)));
    }
    READABLE_FUNCTION(carFollower, Draft);

    // setCarFollower.
    void setCarFollower(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::Follower> follower)
    {
        game::tail(zge::setTagWord, zge::objectComponent(car), kTagWord, kFollowerTag, follower);
    }
    READABLE_FUNCTION(setCarFollower, Draft);

    // driveCar: a driven car's speed for the frame (Aggressive Inline's
    // driveTrafficCar). With a car ahead, by how soon it would reach it
    // (under 1, 3 seconds or more) and how near it is (under 10, 20 or
    // more, less 5); else by the traffic light at its segment's end:
    // braking when it could not pass in time (2 s more than a fifteenth of
    // its speed), or before the amber's time runs out. Its speed then
    // falls by 1 (easing off) or 0.25 (braking), never below 0, or grows
    // by g_carSpeedUp up to the car's tagged speed.
    void driveCar(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::Follower> follower)
    {
        const ee::Float gap = carGap(follower);
        const ee::Float speed = zge::followerSpeed(follower);
        uint32_t drive = kDriveKeep;
        if (gap < ee::Float(0.0f))
        {
            // By the traffic light at the segment's end.
            const ee::Float along = zge::followerDistance(follower);
            const ee::Float toEnd = zge::segmentEnd(zge::followerSegment(follower)) - along;
            bool cruise = false;
            if (zge::segmentJunction(zge::followerSegment(follower)) == 0)
            {
                cruise = true;
            }
            else
            {
                const uint32_t state = lightState(ee::Address(zge::followerSegment(follower))) & 0xFFFFu;
                if (state >= 2)
                {
                    cruise = true;
                }
                else if (ee::Float(0.0f) < speed)
                {
                    const ee::Float time = toEnd / speed;
                    const ee::Float stopTime = speed / ee::Float(15.0f) + ee::Float(2.0f);
                    bool decide = true;
                    if (state == 1)
                    {
                        const uint32_t light = zge::segmentJunction(zge::followerSegment(follower));
                        const ee::Float cycle = lightCycle(light);
                        const ee::Float left = cycle - lightElapsed(light);
                        decide = left < time;
                    }
                    if (decide)
                    {
                        if (time < stopTime)
                            drive = kDriveBrake;
                        else
                            cruise = true;
                    }
                }
            }
            if (cruise)
                drive = carCruise(speed, along, toEnd);
        }
        else
        {
            // By the car ahead: how soon it would be reached, how near it is.
            const ee::Float room = gap - ee::Float(5.0f);
            const ee::Ptr<zge::Follower> other =
                ee::Float(0.0f) <= speed ? zge::followerAhead(follower) : zge::followerBehind(follower);
            const ee::Float closing = speed - zge::followerSpeed(other);
            bool closingIn = ee::Float(0.0f) < speed && ee::Float(0.0f) < closing;
            if (!closingIn)
                closingIn = speed < ee::Float(0.0f) && closing < ee::Float(0.0f);
            const ee::Float time = closingIn ? ee::abs(room) / ee::abs(closing) : ee::Float(-1.0f);
            const int soon = time == ee::Float(-1.0f) ? 3 : time < ee::Float(1.0f) ? 0 : time < ee::Float(3.0f) ? 1 : 2;
            const int near = room < ee::Float(10.0f) ? 0 : room < ee::Float(20.0f) ? 1 : 2;
            if (soon == 3) // not closing in
                drive = near == 0 ? kDriveKeep : kDriveSpeed;
            else if (soon == 0) // within 1 s
                drive = kDriveEaseOff;
            else if (soon == 2 && near == 2)
                drive = kDriveSpeed;
            else if (near == 0)
                drive = kDriveBrake;
            else
                drive = soon == 1 && near == 1 ? kDriveBrake : kDriveKeep;
        }

        const ee::Float sign = speed < ee::Float(0.0f) ? ee::Float(-1.0f) : ee::Float(1.0f);
        ee::Float wanted = ee::abs(speed);
        switch (drive)
        {
        case kDriveBrake:
            if (ee::Float(0.0f) < wanted)
            {
                wanted = wanted - ee::Float(0.25f);
                if (wanted < ee::Float(0.0f))
                    wanted = ee::Float(0.0f);
            }
            break;
        case kDriveEaseOff:
            if (ee::Float(0.0f) < wanted)
            {
                wanted = wanted - ee::Float(1.0f);
                if (wanted < ee::Float(0.0f))
                    wanted = ee::Float(0.0f);
            }
            break;
        case kDriveSpeed:
        {
            const ee::Float most = ee::abs(carSpeed(car));
            wanted = wanted + *g_carSpeedUp;
            if (most < wanted)
                wanted = most;
            break;
        }
        default:
            break;
        }
        if (ee::abs(speed - wanted) < *g_carSameSpeed)
            return;
        game::tail(zge::setFollowerSpeed, follower, wanted * sign);
    }
    READABLE_FUNCTION_WAITS(driveCar, Draft);

    // carGap: how far the next car is the way this one goes (ahead, or
    // behind when it runs backwards), -1 with none on its path.
    ee::Float carGap(ee::Ptr<zge::Follower> follower)
    {
        const ee::Ptr<zge::Follower> other =
            ee::Float(0.0f) <= zge::followerSpeed(follower) ? zge::followerAhead(follower) : zge::followerBehind(follower);
        if (!other)
            return ee::Float(-1.0f);
        const ee::Float here = zge::followerDistance(follower);
        return ee::abs(here - zge::followerDistance(other));
    }
    READABLE_FUNCTION_WAITS(carGap, Draft);

    // carCruise: with no reason to stop: faster up to 10, and above it
    // faster only away from the segment's ends (25 from each).
    uint32_t carCruise(ee::Float speed, ee::Float along, ee::Float toEnd)
    {
        if (!(ee::Float(10.0f) < speed))
            return kDriveSpeed;
        if (along < ee::Float(25.0f) || toEnd < ee::Float(25.0f))
            return kDriveKeep;
        return kDriveSpeed;
    }
    READABLE_FUNCTION(carCruise, Draft);
}
