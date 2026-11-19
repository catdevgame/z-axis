// The traffic cars (readable round gamehand2; the rest gamehand23): a car's
// update as its component runs it, the move along its path (the speed from
// driveTrafficCar, effects.cpp; a new segment at a junction), its node
// placed and turned on the path, its wheels and its engine sound; a car's
// making and its end, what the replay keeps of it, the cars hidden over a
// fly-through. Names past the offsets are inferred from what the code does;
// the path follower is the engine's (nextPathPoint ..; zge).

#include "aggressive/level/level_object.h"
#include "aggressive/level/traffic_car.h"
#include "aggressive/skater/skater.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/sweep.h"
#include "zge/scene/message.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/scene/follower.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/scene/animation.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/effects.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/scene_objects.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/traffic_cars.cpp.h"

namespace aggressive::skater
{
}

namespace aggressive
{
    int32_t carHash();

    namespace
    {
        // The engine's path follower (a car's +0x08): +0x00 its segment,
        // +0x10 its speed (path units a second, signed), +0x14 how far
        // along the segment (0 its start, 1 its end), +0x20 the position.

        constexpr uint32_t kCarHash = sym::carHash;
        constexpr uint32_t kForEachObject = sym::forEachObject;    // (visit(object, context), context)
        constexpr uint32_t kHideCarObject = sym::hideCarObject, kShowCarObject = sym::showCarObject; // the visits

        // A car's messages. The first is every object's frame (zge
        // kUpdateMessage); the 0x9FC .. 0x9FE ones are the replay's
        // (replay.cpp): the bytes of its state, its state saved and restored.
        constexpr uint32_t kMessageUpdate = 0x7000, kMessageVelocity = 28674, kMessageDestroy = 28673;
        constexpr uint32_t kReplayStateBytes = 0x9FC, kSaveReplayState = 0x9FD, kRestoreReplayState = 0x9FE;

        // The handlers of those messages.
        constexpr uint32_t kOnUpdate = sym::updateTrafficCar, kOnVelocity = sym::carVelocityMessage, kOnDestroy = sym::destroyTrafficCarComponent;
        constexpr uint32_t kOnReplayStateBytes = sym::carReplayStateBytesMessage, kOnSaveReplayState = sym::saveCarReplayMessage, kOnRestoreReplayState = sym::restoreCarReplayMessage;

        // The tags of a car: its component's word 28 is the car's block, its
        // float 4 the speed; the class's float 4 the wheel's radius, its
        // bytes 9 and 10 the engine samples.
        constexpr uint32_t kTagWords = 8, kTagBytes = 2;
        constexpr uint32_t kTagCar = 28, kTagSpeed = 4, kTagWheelRadius = 4, kTagEngineSample = 9, kTagSecondSample = 10;
        constexpr uint32_t kSoundBankCars = 5;
        constexpr uint32_t kFirstEngineSample = 0x20;
        constexpr uint32_t kFullEngineLevel = 127;
        constexpr uint32_t kSweepCars = 3;                  // sweepAddBody's kind
        constexpr uint32_t kNoSoundPosition = sym::noSoundPosition;  // the source's first position
        constexpr uint32_t kCarName = sym::g_textCar;          // "car"

        constexpr uint32_t kObjectComponent = sym::objectComponent;  // (object) -> its component (tag data)

        // The sound player's command for a car's engine (its source and level).
        constexpr uint32_t kEngineSound = 50;

        // The debug messages of the exclusion object's look-up (printed
        // into a buffer nobody reads).
        constexpr uint32_t kNotACarFormat = sym::notACarFormat;    // "'%s' is not a car which is specified in the tagging data of '%s' as the exclusion object"
        constexpr uint32_t kNoExclusionFormat = sym::noExclusionFormat; // "Could not find the exclusion object specified in the tagging data of '%s'."

        // A scene region holds this frame's mark while it is out of view.
        inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;
        // The up vector (0, -1, 0, 1), rewritten while the flag is set.
        inline ee::Global<uint32_t, sym::g_writeUp> g_writeUp;
        inline ee::Global<zge::Vec4, sym::g_up> g_up;
        // carHash's: set until the hash is made.
        inline ee::Global<int32_t, sym::g_carHashPending> g_carHashPending;
        inline ee::Global<int32_t, sym::g_carHash> g_carHash;

        constexpr uint32_t kCarReplayStateBytes = 12;

        // The scene's region a node is in (zge's Node::region), as far as a car needs it.
        struct SceneRegion
        {
            uint32_t _00;
            uint32_t viewMark;  // +0x04 g_outOfViewMark while the region is out of view this frame
        };
        static_assert(offsetof(SceneRegion, viewMark) == 0x04);

        // What the replay keeps of a car.
        struct CarReplayState
        {
            uint32_t segmentId;  // +0x00
            ee::Float along;     // +0x04
            ee::Float speed;     // +0x08
        };
        static_assert(sizeof(CarReplayState) == kCarReplayStateBytes);

        // What the car's component update is given.
        struct CarUpdate
        {
            uint8_t _00[0x08];
            ee::Ptr<zge::SceneObject> sceneObject; // +0x08
            uint8_t _0c[0x08];
            ee::Ptr<ee::Float> seconds;  // +0x14 the frame's time
        };
        static_assert(offsetof(CarUpdate, sceneObject) == 0x08 && offsetof(CarUpdate, seconds) == 0x14);

        // What the replay's messages to a car are given: the note's word at
        // +0x14 (ObjectReplayCursor's cursor at +0x04, replay.cpp).
        // What the replay's messages carry: how many bytes a car's state
        // takes, and where it goes.
        struct CarReplayData
        {
            uint32_t bytes;  // +0x00
            uint32_t cursor; // +0x04
        };
        struct CarReplayNote
        {
            uint8_t _00[0x08];
            ee::Ptr<zge::SceneObject> sceneObject; // +0x08
            uint8_t _0c[0x08];
            ee::Ptr<CarReplayData> data; // +0x14
        };
        static_assert(offsetof(CarReplayNote, sceneObject) == 0x08 && offsetof(CarReplayNote, data) == 0x14);

        // A fly-through's visit: the object hidden or shown again if it is
        // a car. `end`: the original's return block.
        int32_t setShownIfCar(uint32_t sceneObject, uint32_t shown, uint32_t end)
        {
            const uint32_t trafficCarType = static_cast<uint32_t>(carHash());
            const ee::Ptr<zge::SceneObject> object(sceneObject);
            if (trafficCarType == zge::levelObjectTypeHash(object))
                zge::setSceneObjectShown(object, shown);
            return 1;
        }

        // A car's path follower (zge/scene/follower.h), or none.
        ee::Ptr<zge::Follower> followerOf(ee::Ptr<TrafficCar> car) { return ee::Ptr<zge::Follower>(car->follower); }

        // The region an object's node is in; none without a node.
        ee::Ptr<SceneRegion> objectRegion(ee::Ptr<zge::SceneObject> object)
        {
            const ee::Ptr<zge::Node> node(objectNode(object));
            return node ? ee::Ptr<SceneRegion>(node->region) : ee::Ptr<SceneRegion>();
        }
    }

    // upVector: the up vector, (0, -1, 0, 1).
    uint32_t upVector()
    {
        if (*g_writeUp)
            *g_up = {0.0f, -1.0f, 0.0f, 1.0f};
        return sym::g_up;
    }
    READABLE_FUNCTION(upVector, Verified);

    // moveTrafficCar: a car's move for the frame: along its path by its
    // speed (driveTrafficCar sets the speed first); at a segment's end it
    // takes one of the segment's other exits (three at most; the one it
    // came on does not count), or turns back if there is none; then its
    // node is put at the follower's position and, when it was on screen,
    // turned to the path.
    //
    // Of the exits, the first there is: the original starts its search from an
    // uninitialised local, a value nothing on the frame's update path ever
    // sets, which is 0 there.
    void moveTrafficCar(ee::Ptr<TrafficCar> car, ee::Float seconds)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<uint32_t> exits;
        ee::Local<zge::Matrix> out;
        ee::Local<zge::Quaternion> orientation;
        const auto follower = [&] { return followerOf(car); };

        zge::advanceFollower(follower(), seconds);
        driveTrafficCar(car);
        zge::followerPosition(follower(), position);
        const ee::Float along = zge::followerAlong(follower());
        const ee::Float speed = zge::followerSpeed(follower());
        bool atEnd = ee::Float(1.0f) <= along && ee::Float(0.0f) < speed;
        if (!atEnd)
            atEnd = along <= ee::Float(0.0f) && speed < ee::Float(0.0f);
        if (atEnd)
        {
            uint32_t found = 0;
            const uint32_t from = ee::Address(zge::followerSegment(follower()));
            for (uint32_t i = 0;;)
            {
                const ee::Ptr<zge::PathRecord> segment = zge::followerSegment(follower());
                const uint32_t atStart = along <= ee::Float(0.0f) ? 1u : 0u;
                (&*exits)[i] = ee::Address(zge::segmentExit(segment, i, atStart));
                if ((&*exits)[i])
                {
                    if ((&*exits)[i] == from)
                        (&*exits)[i] = 0;
                    else
                        ++found;
                }
                ++i;
                if (!(i < 3u))
                    break;
            }
            if (found == 0)
                zge::setFollowerSpeed(follower(), -speed);
            else
            {
                uint32_t exit = 0;
                for (uint32_t i = 0; !exit; ++i)
                    exit = (&*exits)[i % 3u];
                zge::setFollowerSegment(follower(), ee::Ptr<zge::PathRecord>(exit));
                zge::followerSnap(follower(), position);
                // On the new segment at whichever end is nearer.
                ee::Float to = zge::followerAlong(follower());
                if (to < ee::Float(1.0f) && ee::Float(0.0f) < to)
                {
                    to = ee::Float(0.5f) < to ? ee::Float(1.0f) : ee::Float(0.0f);
                    zge::setFollowerAlong(follower(), to);
                }
                // Away from that end.
                const ee::Float newSpeed = zge::followerSpeed(follower());
                bool set = true;
                ee::Float away = ee::abs(newSpeed);
                if (!(to <= ee::Float(0.0f)))
                {
                    set = ee::Float(1.0f) <= to;
                    away = -ee::abs(newSpeed);
                }
                if (set)
                    zge::setFollowerSpeed(follower(), away);
            }
        }
        zge::followerPosition(follower(), position);
        zge::setObjectPosition(car->object, position);
        zge::updatePose(car->object);
        if (objectDrawn(car->object))
        {
            const ee::Ptr<const zge::Vec3> up(upVector());
            zge::followerOrientation(follower(), up, out);
            zge::matrixToQuaternion(orientation, out);
            zge::setObjectOrientation(car->object, orientation);
        }
        (void)*position;
    }
    READABLE_FUNCTION(moveTrafficCar, Verified);

    // updateTrafficCar: a traffic car's update (its component's): the first
    // time, the exclusion object its tag data names (word 24) is looked up
    // and, when it is a car, that car's list gets it; then, unless
    // the car's region is out of view, the car moves, its
    // collision box follows, its wheels turn, and its engine sound plays
    // at its position. Returns 0.
    int32_t updateTrafficCar(ee::Ptr<CarUpdate> update)
    {
        ee::Local<char[0x100]> message;
        ee::Local<uint32_t> exclusionName;
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(update->sceneObject);
        const ee::Float seconds = *update->seconds;
        if (car->findExclusion)
        {
            const auto ownName = [&] { return zge::objectLevelNode(update->sceneObject)->name; };
            *exclusionName = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(update->sceneObject), 8, 24));
            if (*exclusionName)
            {
                const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(exclusionName, 1);
                if (found)
                {
                    const ee::Ptr<TrafficCar> other = skater::trafficCarOf(*found);
                    if (other)
                        zge::sweepIgnoreBody(other->sweepBox, other);
                    else
                    {
                        const char *otherName = zge::objectLevelNode(*found)->name;
                        // (text for a debug message nothing shows)
                        zge::formatString(message, kNotACarFormat, otherName, ownName());
                    }
                }
                else
                    zge::formatString(message, kNoExclusionFormat, ownName());
            }
            car->findExclusion = 0;
        }

        // A car in a region out of view stands still (while player one is in a region).
        const ee::Ptr<skater::Player> player(parts::skaterHandle(0));
        const ee::Ptr<SceneRegion> playerRegion = objectRegion(player->node);
        const ee::Ptr<SceneRegion> carRegion = objectRegion(car->object);
        if (playerRegion && carRegion && carRegion->viewMark == *g_outOfViewMark)
            return 0;

        moveTrafficCar(car, seconds);
        moveObjectBox(update->sceneObject, car);
        if (car->follower)
            turnCarWheels(car->object, car->wheelCircumference, zge::followerSpeed(followerOf(car)), seconds);
        soundEvent(kEngineSound, static_cast<uint32_t>(car->soundSource), static_cast<uint32_t>(car->engineLevel), 0, 0);
        const uint32_t source = car->soundSource;
        zge::setSourcePosition(source, zge::objectPosition(car->object));
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateTrafficCar, Verified);

    // hideCarObject, a visit of forEachObject.
    int32_t hideCarObject(uint32_t sceneObject)
    {
        namespace here = points::hideCarObject;
        return setShownIfCar(sceneObject, 0, here::taken);
    }
    READABLE_FUNCTION_WAITS(hideCarObject, Verified);

    // showCarObject, a visit of forEachObject.
    int32_t showCarObject(uint32_t sceneObject)
    {
        namespace here = points::showCarObject;
        return setShownIfCar(sceneObject, 1, here::taken);
    }
    READABLE_FUNCTION_WAITS(showCarObject, Verified);

    // carVelocityMessage: the message's handler does nothing but ask for the
    // component (cut, or a leftover: its answer goes unused).
    int32_t carVelocityMessage(ee::Ptr<CarUpdate> note)
    {
        zge::objectComponent(ee::Ptr<zge::SceneObject>(note->sceneObject));
        return 1;
    }
    READABLE_FUNCTION(carVelocityMessage, Verified);

    // carReplayStateBytesMessage: how many bytes the replay keeps for a car.
    int32_t carReplayStateBytesMessage(ee::Ptr<CarReplayNote> note)
    {
        note->data->bytes = kCarReplayStateBytes;
        return 1;
    }
    READABLE_FUNCTION(carReplayStateBytesMessage, Verified);

    // saveCarReplayMessage: the follower's place on its path, at the cursor.
    int32_t saveCarReplayMessage(ee::Ptr<CarReplayNote> note)
    {
        ee::Local<CarReplayState> state;
        zge::memoryFill(state, 0, kCarReplayStateBytes);
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(note->sceneObject);
        const ee::Ptr<zge::PathRecord> segment = zge::followerSegment(followerOf(car));
        state->segmentId = zge::pathNameHash(segment);
        state->along = zge::followerAlong(followerOf(car));
        state->speed = zge::followerSpeed(followerOf(car));
        zge::memoryCopy(note->data->cursor, state, kCarReplayStateBytes);
        return 1;
    }
    READABLE_FUNCTION(saveCarReplayMessage, Verified);

    // restoreCarReplayMessage: put back on its path; a car with no follower gets a new one.
    int32_t restoreCarReplayMessage(ee::Ptr<CarReplayNote> note)
    {
        ee::Local<CarReplayState> state;
        zge::memoryCopy(state, note->data->cursor, kCarReplayStateBytes);
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(note->sceneObject);
        const ee::Ptr<zge::PathRecord> segment = zge::findPathRecord(state->segmentId);
        if (car->follower != 0)
        {
            zge::setFollowerSegment(followerOf(car), segment);
            zge::setFollowerAlong(followerOf(car), state->along);
            zge::setFollowerSpeed(followerOf(car), state->speed);
        }
        else
            car->follower = ee::Address(zge::newPathFollower(segment, 0, state->speed, state->along));
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreCarReplayMessage, Verified);

    // destroyTrafficCarComponent: a car's end: out of the sweep, its follower, its sound,
    // its block; the component's word cleared.
    int32_t destroyTrafficCarComponent(ee::Ptr<CarUpdate> note)
    {
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(ee::Ptr<zge::SceneObject>(note->sceneObject));
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(note->sceneObject);
        if (car)
        {
            if (car->sweepBox)
            {
                const ee::Ptr<zge::Sweep> sweep = levelSweep();
                zge::removeSweepBody(sweep, car->sweepBox);
                car->sweepBox = ee::Ptr<zge::SweepBox>();
            }
            if (car->follower != 0)
            {
                zge::deletePathFollower(followerOf(car));
                car->follower = 0;
            }
            zge::stopSource(car->soundSource);
            if (component != 0)
                zge::setTagWord(component, kTagWords, kTagCar, 0);
            zge::heapFree(car);
        }
        zge::freeObjectAnimator(ee::Ptr<zge::SceneObject>(note->sceneObject));
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyTrafficCarComponent, Verified);

    // newTrafficCar: the block of a car, its handlers, on the path nearest
    // it, in the level's sweep, its engine sounding.
    int32_t newTrafficCar(ee::Ptr<zge::SceneObject> sceneObject)
    {
        const auto subscribe = [&](uint32_t message, uint32_t handler)
        {
            zge::subscribe(sceneObject, message, handler);
        };
        subscribe(kMessageUpdate, kOnUpdate);
        subscribe(kMessageVelocity, kOnVelocity);
        subscribe(kReplayStateBytes, kOnReplayStateBytes);
        subscribe(kSaveReplayState, kOnSaveReplayState);
        subscribe(kRestoreReplayState, kOnRestoreReplayState);
        subscribe(kMessageDestroy, kOnDestroy);
        zge::createObjectAnimator(sceneObject);
        const ee::Ptr<zge::TagRecord> component = zge::objectComponent(sceneObject);
        const ee::Ptr<TrafficCar> car(zge::heapAllocate(sizeof(TrafficCar)));
        zge::memoryFill(car, 0, sizeof(TrafficCar));
        car->object = sceneObject;
        zge::setTagWord(component, kTagWords, kTagCar, car);
        const ee::Ptr<zge::TagRecord> classTags = zge::levelObjectClassTag(car->object);
        const ee::Float wheelRadius = zge::tagFloat(classTags, kTagWords, kTagWheelRadius);
        car->wheelCircumference = wheelRadius * ee::Float(6.2831855f);
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(car->object);
        const ee::Ptr<zge::PathRecord> path = zge::findNearestPath(position, ee::Ptr<ee::Float>());
        if (path)
        {
            const ee::Float speed = zge::tagFloat(component, kTagWords, kTagSpeed);
            car->follower = ee::Address(zge::newPathFollower(path, 0, speed, ee::Float(0.0f)));
            zge::followerSnap(followerOf(car), position);
        }
        else
            car->follower = 0;
        car->findExclusion = 1;
        const ee::Ptr<zge::Sweep> sweep = levelSweep();
        if (sweep != 0)
        {
            const ee::Ptr<zge::Box> box = zge::nodeBox(zge::objectLevelNode(sceneObject));
            car->sweepBox = zge::sweepAddBody(sweep, kSweepCars, car, &box->low.x, &box->high.x);
        }
        const ee::Ptr<zge::TagRecord> tags = zge::levelObjectClassTag(sceneObject);
        car->engineSample = static_cast<uint8_t>(zge::tagByte(tags, kTagBytes, kTagEngineSample) + kFirstEngineSample);
        car->secondSample = static_cast<uint8_t>(zge::tagByte(tags, kTagBytes, kTagSecondSample) + kFirstEngineSample);
        car->engineLevel = kFullEngineLevel;
        const uint32_t bank = soundBankOfKind(kSoundBankCars);
        const ee::Ptr<zge::BankSample> sample = zge::sample(bank, car->engineSample);
        car->soundSource = static_cast<uint16_t>(zge::startSource(sample, kNoSoundPosition));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newTrafficCar, Verified);

    // trafficCarFollower: nothing calls it.
    uint32_t trafficCarFollower(ee::Ptr<zge::SceneObject> sceneObject)
    {
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(sceneObject);
        return car->follower;
    }
    READABLE_FUNCTION(trafficCarFollower, Verified);

    // carHash: the hash of "car", made the first time.
    int32_t carHash()
    {
        if (*g_carHashPending != 0)
        {
            *g_carHash = zge::nameHash(ee::Ptr<const char>(kCarName));
            *g_carHashPending = 0;
        }
        return *g_carHash;
    }
    READABLE_FUNCTION(carHash, Verified);

    namespace
    {
        namespace tail
        {
        int32_t forEachObject(uint32_t visit, uint32_t data) { return ee::tailCall<int32_t>(kForEachObject, visit, data); }
        }
    }

    // hideCars
    int32_t hideCars()
    {
        return tail::forEachObject(kHideCarObject, 0);
    }
    READABLE_FUNCTION_WAITS(hideCars, Verified);

    // showCars
    int32_t showCars()
    {
        return tail::forEachObject(kShowCarObject, 0);
    }
    READABLE_FUNCTION_WAITS(showCars, Verified);

    // setTrafficCarVolume: the script's setVehicleVolume (a car by its name's hash).
    void setTrafficCarVolume(uint32_t name, uint32_t engineLevel)
    {
        ee::Local<uint32_t> names;
        *names = name;
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = zge::findObjects(names, 1);
        const ee::Ptr<TrafficCar> car = skater::trafficCarOf(*slot);
        car->engineLevel = static_cast<uint8_t>(engineLevel);
    }
    READABLE_FUNCTION_WAITS(setTrafficCarVolume, Draft);
}
