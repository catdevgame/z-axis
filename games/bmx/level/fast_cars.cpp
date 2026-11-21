// BMX XXX's "FastCar" level object type: a car that follows one of the
// engine's fast paths (ZGE4's keyed paths: a follower has a time on its
// path and a rate, and gives the position and orientation there). BMX XXX's
// own code (its game block), Aggressive Inline's traffic car line
// (games/aggressive/level/traffic_cars.cpp) on the fast follower; no other
// build names its functions, so the names are from what each does and the
// type's strings ("FastCar", "zfastCar", "%s.%s.move").
//
// A car moves along its path every frame and wraps at its ends, is placed
// on it while it is drawn, is collided with only while a rider is near,
// turns its wheel bones by the distance moved, and carries its "move" sound.

#include "entities.h"
#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/game/player_riders.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/tag_table.h"
#include "zge/debug/profiler.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/fast_paths.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/fast_cars.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        // The car's own tag data (objectComponent), 0x28 bytes.
        constexpr uint32_t kFollowerTag = 0;      // word: its fast follower
        constexpr uint32_t kPathTag = 4;          // word: its path's id
        constexpr uint32_t kRateTag = 8;          // float: its follower's rate
        constexpr uint32_t kRandomTag = 0x18;     // word: a random number drawn at its first init
        constexpr uint32_t kSoundTag = 0x20;      // word: its move sound's handle
        constexpr uint32_t kStartFrameTag = 0x24; // float: where on the path it starts, in frames
        constexpr uint32_t kComponentBytes = 0x28;
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kWheelRadiusTag = 4;   // float
        constexpr uint32_t kSoundNameTag = 8;     // text: the middle of its sounds' names
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagWord = 8, kTagText = 0x10;

        constexpr uint32_t kTypeName = sym::fastCarTypeName;                       // "FastCar"
        constexpr uint32_t kMoveSoundFormat = sym::bmx::fastCarMoveSoundFormat;     // "%s.%s.move"
        constexpr uint32_t kTouchSoundFormat = sym::bmx::fastCarTouchSoundFormat; // "%s.%s"
        constexpr uint32_t kProfileLabel = sym::fastCarProfileLabel;               // "zfastCar"
        inline ee::Global<uint32_t, sym::fastCarProfileColour> g_profileColour;
        // The cars' profile bar (their updates' time), made with the type
        // and freed with the last car.
        inline ee::Global<uint32_t, sym::g_fastCarProfileBar> g_fastCarProfileBar;
        inline ee::Global<int32_t, sym::g_fastCarCount> g_fastCarCount;
        // How many players the game has.
        inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
        // The cars' random numbers' generator.
        inline ee::Global<uint32_t, sym::g_fastCarSeed> g_fastCarSeed;
        constexpr uint32_t kSeedOddFactor = 0x5EB0A82F, kSeedHalfFactor = 0x429EAF9D;

        constexpr ee::Float kPathFramesASecond(30.0f);
        // A car that is not drawn is still placed when its time is more
        // than this past its placedTime (-0.1).
        inline ee::Global<ee::Float, sym::g_fastCarUnseenStep> g_fastCarUnseenStep;
        inline ee::Global<ee::Float, sym::g_fastCarPi> g_fastCarPi;
        // What a part that is not a wheel turns by each frame, over the wheel's circumference (150000).
        inline ee::Global<ee::Float, sym::g_fastCarPartTurn> g_fastCarPartTurn;
        // A rider within 20 of a car (400 squared) makes it collidable.
        constexpr ee::Float kNearSquared(400.0f);
        // A full turn in the engine's angle units, as a double (65536.0).
        constexpr uint64_t kTurnUnits = 0x40F0000000000000ull;

        // The bones a car's model may have that turn, by name hash: the
        // first two are wheels (turned about x by the distance moved), the
        // third turns about x, the fourth about z and the fifth about -y at
        // a fixed rate. A name a bone was found under is spent (replaced by
        // kSpentBone) so that the next bone looked up gets the next name.
        constexpr uint32_t kTurningBones[5] = {0x09F7EE23, 0x00B8FD93, 0x0108EA73, 0x0109EA73, 0x010AEA73};
        constexpr uint32_t kSpentBone = 0x0AC6B0C2;
        constexpr uint32_t kMostTurningBones = 8;

        // A rider's flag byte 0. Scaffolding (unnamed-field): bit 0x02 has
        // no name yet; only riders with it set count as near a car.
        constexpr uint8_t kRiderFlag02 = 0x02;

        constexpr zge::MessageHandler kUpdateFastCar(sym::updateFastCar);
        constexpr zge::MessageHandler kFastCarTouched(sym::fastCarTouched);
        constexpr zge::MessageHandler kDisableFastCar(sym::disableFastCar);
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // What a car keeps in a saved level state.
        struct FastCarSave
        {
            uint32_t pathId;                    // +0x00
            uint32_t followerEndMode;           // +0x04 its follower's end mode
            ee::Float rate;                     // +0x08
            ee::Float time;                     // +0x0C
            uint8_t component[kComponentBytes]; // +0x10 its own tag data
        };
        static_assert(sizeof(FastCarSave) == 0x38);
    }

    ee::Ptr<zge::FastFollower> fastCarFollower(ee::Ptr<zge::SceneObject> car);
    ee::Float fastCarWheelRadius(ee::Ptr<zge::SceneObject> car);
    void setFastCarFollower(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::FastFollower> follower);

    // fastCarPathId: the id of the path the car is tagged with.
    uint32_t fastCarPathId(ee::Ptr<zge::SceneObject> car)
    {
        return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kPathTag));
    }
    READABLE_FUNCTION(fastCarPathId, Verified);

    // fastCarRate: the rate the car is tagged with.
    ee::Float fastCarRate(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::objectComponent(car), kTagWord, kRateTag);
    }
    READABLE_FUNCTION(fastCarRate, Verified);

    // fastCarWheelRadius: its class's wheel radius.
    ee::Float fastCarWheelRadius(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::levelObjectClassTag(car), kTagWord, kWheelRadiusTag);
    }
    READABLE_FUNCTION(fastCarWheelRadius, Verified);

    // fastCarStartTime: where on its path the car is tagged to start (its
    // start frame at 30 frames a second).
    ee::Float fastCarStartTime(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::objectComponent(car), kTagWord, kStartFrameTag) / kPathFramesASecond;
    }
    READABLE_FUNCTION(fastCarStartTime, Verified);

    // fastCarFollower: the car's fast follower.
    ee::Ptr<zge::FastFollower> fastCarFollower(ee::Ptr<zge::SceneObject> car)
    {
        return ee::Ptr<zge::FastFollower>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kFollowerTag)));
    }
    READABLE_FUNCTION(fastCarFollower, Verified);

    // setFastCarFollower.
    void setFastCarFollower(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::FastFollower> follower)
    {
        game::tail(zge::setTagWord, zge::objectComponent(car), kTagWord, kFollowerTag, follower);
    }
    READABLE_FUNCTION(setFastCarFollower, Verified);

    // initFastCar (the type's init): 0 when the level has no path of the
    // car's id. Else its messages subscribed, a follower made on the path
    // at the car's rate and start, its random number drawn (once), its
    // "move" sound started, the car shown and counted. 1.
    uint32_t initFastCar(ee::Ptr<zge::SceneObject> car)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const ee::Ptr<zge::FastPath> path = zge::findFastPath(fastCarPathId(car));
        if (!path)
            return 0;
        zge::subscribe(car, zge::kUpdateMessage, kUpdateFastCar);
        zge::subscribe(car, zge::kRemovedMessage, zge::MessageHandler(sym::fastCarRemoved));
        zge::subscribe(car, zge::kVelocityMessage, zge::MessageHandler(sym::fastCarVelocity));
        zge::subscribe(car, zge::kSaveSizeMessage, zge::MessageHandler(sym::fastCarSaveSize));
        zge::subscribe(car, zge::kSaveMessage, zge::MessageHandler(sym::saveFastCar));
        zge::subscribe(car, zge::kRestoreMessage, zge::MessageHandler(sym::restoreFastCar));
        zge::subscribe(car, kRiderTouchMessage, kFastCarTouched);
        zge::subscribe(car, kEnableMessage, zge::MessageHandler(sym::enableFastCar));
        zge::subscribe(car, kDisableMessage, kDisableFastCar);
        zge::subscribe(car, kIsEnabledMessage, zge::MessageHandler(sym::fastCarIsEnabled));

        const ee::Float rate = fastCarRate(car);
        const ee::Ptr<zge::FastFollower> follower = zge::newFastFollower(path, 2, rate, ee::Float(0.0f));
        setFastCarFollower(car, follower);
        zge::setFastFollowerTime(follower, fastCarStartTime(car));

        if (zge::tagWord(zge::objectComponent(car), kTagWord, kRandomTag) == 0)
        {
            const uint32_t seed = *g_fastCarSeed;
            *g_fastCarSeed = (seed & 1u) * kSeedOddFactor - (seed >> 1) * kSeedHalfFactor;
            zge::setTagWord(zge::objectComponent(car), kTagWord, kRandomTag, *g_fastCarSeed);
        }

        zge::tagString(zge::levelObjectClassTag(car), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kMoveSoundFormat, kTypeName, className.address());
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(car);
        const uint32_t sound = soundEvent(kObjectSoundEvent, position, kObjectSoundStart, soundName.address(), kFullVolume);
        zge::setTagWord(zge::objectComponent(car), kTagWord, kSoundTag, sound);
        zge::setSceneObjectShown(car, 1);
        *g_fastCarCount = *g_fastCarCount + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initFastCar, Verified);

    // fastCarRemoved (the engine's removed message): its follower deleted,
    // its sound stopped; the last car frees the cars' profile bar.
    int32_t fastCarRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        zge::deleteFastFollower(fastCarFollower(car));
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
        soundEvent(kObjectSoundEvent, position, kObjectSoundStop, sound, 0);
        *g_fastCarCount = *g_fastCarCount - 1;
        if (*g_fastCarCount == 0)
            zge::freeProfileBar(*g_fastCarProfileBar);
        return 0;
    }
    READABLE_FUNCTION_WAITS(fastCarRemoved, Verified);

    namespace
    {
        // The car put where its follower is.
        void placeFastCar(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::FastFollower> follower)
        {
            zge::fastFollowerPosition(follower, &follower->position);
            zge::setObjectPosition(car, &follower->position);
            zge::fastFollowerOrientation(follower, &follower->orientation);
            zge::setObjectOrientation(car, &follower->orientation);
        }
    }

    // updateFastCar (the frame's update; the data is the frame's seconds):
    // see the top of the file.
    int32_t updateFastCar(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> previousPosition;
        ee::Local<zge::Vec4> toRider;
        ee::Local<uint32_t[8]> boneNames;
        ee::Local<ee::Ptr<uint32_t>[8]> nameCursors;
        ee::Local<ee::Ptr<zge::AnimationChannel>[8]> bones;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<zge::Quaternion> turn;
        ee::Local<zge::Vec4> axis;
        ee::Local<zge::Vec4> moved;

        const ee::Ptr<zge::Node> node(static_cast<uint32_t>(objectNodeHandle(ee::Ptr<zge::SceneObject>(message->target))));
        zge::startProfileBar(*g_fastCarProfileBar);
        const ee::Ptr<zge::SceneObject> car(message->target);
        *previousPosition = *zge::objectPosition(car);
        zge::objectNameHash(car);
        const ee::Ptr<zge::FastFollower> follower = fastCarFollower(car);

        // Along the path by the frame's time at its rate; past either end
        // it comes in again from the other.
        const ee::Float seconds = *ee::Ptr<ee::Float>(message->data);
        const ee::Float rate = zge::fastFollowerRate(follower);
        const ee::Float endTime = zge::fastPathEndTime(zge::fastFollowerPath(follower));
        const ee::Float step = rate * seconds;
        const ee::Float time = zge::fastFollowerTime(follower) + step;
        if (time < ee::Float(0.0f) || endTime < time)
        {
            const ee::Float wrapped = ee::Float(0.0f) < rate ? time - endTime : endTime + time;
            zge::setFastFollowerTime(follower, wrapped);
            follower->placedTime = wrapped;
        }
        else
        {
            zge::setFastFollowerTime(follower, time);
        }

        if (zge::nodeNotDrawn(node) == 0)
            placeFastCar(car, follower);
        else if (*g_fastCarUnseenStep < zge::fastFollowerTime(follower) - follower->placedTime)
            placeFastCar(car, follower);

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

        // A drawn car's turning bones.
        if (zge::nodeNotDrawn(node) == 0)
        {
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

                    const ee::Float diameter = fastCarWheelRadius(car);
                    const ee::Float circumference = (diameter + diameter) * *g_fastCarPi;
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
                        angle = static_cast<zge::Angle>(ee::toInt(*g_fastCarPartTurn / circumference));
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
        }

        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(car), kObjectSoundMove, sound, kFullVolume);
        zge::stopProfileBar(*g_fastCarProfileBar);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateFastCar, Verified);

    // fastCarVelocity (the engine's velocity message): its follower's
    // velocity, (0, 0, 0, 1) without one.
    int32_t fastCarVelocity(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::Vec4> velocity(message->data);
        const ee::Ptr<zge::FastFollower> follower = fastCarFollower(ee::Ptr<zge::SceneObject>(message->target));
        if (follower)
            zge::fastFollowerVelocity(follower, velocity);
        else
            *velocity = zge::Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        return 0;
    }
    READABLE_FUNCTION_WAITS(fastCarVelocity, Verified);

    // fastCarSaveSize (the engine's save size message).
    int32_t fastCarSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(FastCarSave);
        return 0;
    }
    READABLE_FUNCTION(fastCarSaveSize, Verified);

    // saveFastCar (the engine's save message): its follower's path, word,
    // time and rate, and its own tag data.
    int32_t saveFastCar(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<FastCarSave> saved;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::FastFollower> follower = fastCarFollower(car);
        saved->followerEndMode = zge::fastFollowerEndMode(follower);
        saved->pathId = zge::fastPathId(zge::fastFollowerPath(follower));
        saved->time = zge::fastFollowerTime(follower);
        saved->rate = zge::fastFollowerRate(follower);
        zge::memoryCopy(saved->component, zge::objectComponent(car), kComponentBytes);
        zge::memoryCopy(message->data, saved, sizeof(FastCarSave));
        return 0;
    }
    READABLE_FUNCTION_WAITS(saveFastCar, Verified);

    // restoreFastCar (the engine's restore message): the saved state put
    // back; the follower is the car's present one.
    int32_t restoreFastCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::FastFollower> follower = fastCarFollower(car);
        const ee::Ptr<FastCarSave> saved(message->data);
        zge::setFastFollowerPath(follower, zge::findFastPath(saved->pathId));
        zge::setFastFollowerTime(follower, saved->time);
        zge::setFastFollowerRate(follower, saved->rate);
        zge::setFastFollowerEndMode(follower, saved->followerEndMode);
        zge::memoryCopy(zge::objectComponent(car), saved->component, kComponentBytes);
        setFastCarFollower(car, follower);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreFastCar, Verified);

    // fastCarTouched (kRiderTouchMessage): the entities' touch sound under
    // the car's "FastCar.<class>" name.
    int32_t fastCarTouched(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        zge::startProfileBar(*g_fastCarProfileBar);
        zge::tagString(zge::levelObjectClassTag(ee::Ptr<zge::SceneObject>(message->target)), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kTouchSoundFormat, kTypeName, className.address());
        playTouchSound(message, soundName);
        zge::stopProfileBar(*g_fastCarProfileBar);
        return 0;
    }
    READABLE_FUNCTION_WAITS(fastCarTouched, Verified);

    // enableFastCar (kEnableMessage): it runs again, shown and collidable.
    int32_t enableFastCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const uint64_t node = objectNodeHandle(car);
        zge::subscribe(car, zge::kUpdateMessage, kUpdateFastCar);
        zge::subscribe(car, kRiderTouchMessage, kFastCarTouched);
        zge::subscribe(car, kDisableMessage, kDisableFastCar);
        zge::setSceneObjectShown(car, 1);
        rider::collision::setNodeCollidable(node, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableFastCar, Verified);

    // disableFastCar (kDisableMessage): silent, hidden, not collidable and
    // no longer run; only an enable reaches it.
    int32_t disableFastCar(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const uint64_t node = objectNodeHandle(car);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(car), kObjectSoundMove, sound, 0);
        zge::setSceneObjectShown(car, 0);
        rider::collision::setNodeCollidable(node, 0);
        zge::unsubscribe(car, zge::kUpdateMessage, kUpdateFastCar);
        zge::unsubscribe(car, kRiderTouchMessage, kFastCarTouched);
        zge::unsubscribe(car, kDisableMessage, kDisableFastCar);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableFastCar, Verified);

    // fastCarIsEnabled (kIsEnabledMessage): whether its update runs.
    int32_t fastCarIsEnabled(ee::Ptr<zge::EngineMessage> message)
    {
        if (message->data != 0)
        {
            const uint32_t updating = zge::findSubscription(ee::Ptr<zge::MessageTarget>(message->target), zge::kUpdateMessage);
            *ee::Ptr<uint32_t>(message->data) = updating;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(fastCarIsEnabled, Verified);

    // registerFastCarType: the "FastCar" type, made as a plain entity and
    // set up by initFastCar, and the cars' profile bar "zfastCar".
    uint32_t registerFastCarType()
    {
        ee::Local<uint32_t> colour;
        *colour = *g_profileColour;
        zge::registerLevelObjectType(kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initFastCar));
        *g_fastCarProfileBar = zge::newProfileBar(0, 0, colour, kProfileLabel);
        return *g_fastCarProfileBar;
    }
    READABLE_FUNCTION_WAITS(registerFastCarType, Verified);
}
