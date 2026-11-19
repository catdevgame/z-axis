// The traffic cars (DM2's COMMERCI): the "Car" level objects, each on a path
// follower: made, its messages (the frame, its velocity, its save, its
// removal, a touch), its speed for the frame by the car ahead on its path or
// the traffic light at its segment's end, which segment it takes at a
// junction, and its wheels turned. DM2's own code (its game block);
// Aggressive Inline's driveTrafficCar (effects.cpp) is its ancestor (the
// same decisions; DM2 brakes by 0.25, eases off by 1 and speeds up by 0.025
// a frame, and asks the car's top speed only to speed up).

#include "dynamics_objects.h"
#include "object_components.h"
#include "object_sounds.h"
#include "traffic_cars.h"
#include "traffic_light_state.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
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
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/traffic_cars.cpp.h"

namespace game::dm2
{
    namespace
    {
        // What a car does with its speed (driveTrafficCar).
        enum CarDrive : uint32_t
        {
            kCarEaseOff = 0, // 1 less
            kCarBrake = 2,   // 0.25 less
            kCarKeep = 3,
            kCarSpeedUp = 4, // 0.025 more, up to the car's top speed
        };
        constexpr float kEaseOffStep = 1.0f, kBrakeStep = 0.25f, kSpeedUpStep = 0.025f;
        // A speed this near the wanted one is left alone.
        constexpr float kSameSpeed = 1e-5f;
        // The car's component: its top speed (a float at 8).
        constexpr uint32_t kCarTagWords = 8, kCarTagTopSpeed = 8;
        // The car ahead: room is its distance less 5; how soon it is reached
        // (under 1, 3 seconds or more) and how near (under 10, 20 or more).
        constexpr float kCarLength = 5.0f;
        constexpr float kSoonTime = 1.0f, kLaterTime = 3.0f, kNearRoom = 10.0f, kFarRoom = 20.0f;
        constexpr float kNotClosing = -1.0f;
        // The light ahead: a car brakes when it would reach it sooner than
        // a fifteenth of its speed and 2 seconds.
        constexpr float kStopTimePerSpeed = 15.0f, kStopTimeLeast = 2.0f;

        constexpr ee::Ptr<const char> kCarTypeName{sym::carTypeName}; // "Car"

        // The car's component, more of it: its path's name hash (word 4), the
        // weights of a junction's three exits (floats at 0x0C, 0x10, 0x14),
        // its random number (0x18: made once, then each junction's draw) and
        // its sound (0x1C, object_sounds.h).
        constexpr uint32_t kCarTagPath = 4, kCarTagExitWeights = 0x0C, kCarTagSound = 0x1C;
        constexpr uint32_t kCarTagRandom = kCarTagExitWeights + 0x0C;
        // The "Car" class's tags: its wheels' radius (a float at 4), and
        // whether its cars drive themselves (a byte at 0x20: driveTrafficCar).
        constexpr uint32_t kCarClassTagWheelRadius = 4, kCarClassTagDrives = 0x20, kCarClassTagBytes = 2;
        // The follower's end mode for a car: on from the path's other end
        // (follower.cpp's kFollowerLoops).
        constexpr uint32_t kCarFollowerLoops = 2;

        // The cars' random numbers: L'Ecuyer's multiplier 1588635695 modulo
        // the prime 2^32 - 5, by Schrage's method (the quotient and the
        // remainder of the modulus by the multiplier). The first car's seed is
        // the collision clock's.
        constexpr uint32_t kRandomMultiplier = 1588635695u, kRandomModulus = 0xFFFFFFFBu;
        constexpr uint32_t kRandomQuotient = 2u, kRandomRemainder = 1117695901u;
        constexpr uint32_t nextRandom(uint32_t seed)
        {
            return kRandomMultiplier * (seed % kRandomQuotient) - kRandomRemainder * (seed / kRandomQuotient);
        }
        inline ee::Global<uint32_t, sym::g_carSeedStarted> g_carSeedStarted;
        inline ee::Global<uint32_t, sym::g_carSeed> g_carSeed;
        // A junction's exits: with all weights about 0, a third each (33.3
        // of 99.9; the sum's bits are 99.9 rounded down, as its compiler
        // stored it).
        constexpr float kNoWeights = 1e-5f, kEvenWeight = 33.3f, kPercent = 100.0f;
        constexpr uint32_t kEvenWeightsBits = 0x42C7CCCCu;
        // A draw's least (it is above the weights' 0).
        constexpr float kLeastDraw = 1e-5f;

        // The car's wheels and rotators: the model's bones of these names
        // turn each frame (the wheels by the ground they cover, a rotator
        // by a fixed spin); a name found is crossed out with the no-rotator
        // name, so the next bone takes the next name. Eight bones at most.
        constexpr ee::Ptr<const char> kNoRotatorName{sym::noRotatorName}; // "RotatorThereIsNoRotator"
        constexpr ee::Ptr<const char> kFrontWheelsName{sym::frontWheelsName}; // "FrontWheels"
        constexpr ee::Ptr<const char> kBackWheelsName{sym::backWheelsName}; // "BackWheels"
        constexpr ee::Ptr<const char> kRotatorXAxisName{sym::rotatorXAxisName}; // "RotatorXAxis"
        constexpr ee::Ptr<const char> kRotatorYAxisName{sym::rotatorYAxisName}; // "RotatorYAxis"
        constexpr ee::Ptr<const char> kRotatorZAxisName{sym::rotatorZAxisName}; // "RotatorZAxis"
        enum CarBoneName : uint32_t
        {
            kFrontWheels,
            kBackWheels,
            kRotatorXAxis,
            kRotatorYAxis,
            kRotatorZAxis,
            kCarBoneNames,
        };
        constexpr uint32_t kMostTurnedBones = 8;
        constexpr float kPi = 3.14159274f;
        // A wheel's turn: 65536 a whole turn of its circumference (in
        // doubles); a rotator's: 150000 over the wheels' circumference.
        constexpr uint64_t kTurnUnitsDouble = 0x40F0000000000000ull; // 65536.0
        constexpr float kRotatorSpin = 150000.0f;

        // A car's save (the save message): where its follower is, then its
        // component's record as it is.
        constexpr uint32_t kCarComponentBytes = 0x24;
        struct CarSave
        {
            uint32_t segment;  // +0x00 its segment's name hash
            uint32_t endMode;  // +0x04 the follower's
            ee::Float speed;   // +0x08
            ee::Float along;   // +0x0C
            uint8_t component[kCarComponentBytes]; // +0x10
        };
        static_assert(offsetof(CarSave, component) == 0x10 && sizeof(CarSave) == 0x34);

        // A car's follower (its component's first word) and sound.
        ee::Ptr<zge::Follower> carFollower(ee::Ptr<zge::SceneObject> car)
        {
            return ee::Ptr<zge::Follower>(static_cast<uint32_t>(objectComponentKind(car)));
        }
        uint32_t carSound(ee::Ptr<zge::SceneObject> car)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kCarTagWords, kCarTagSound));
        }

        // An unsigned word as a float (the compiled code's two ways).
        ee::Float unsignedToFloat(uint32_t w)
        {
            if (int32_t(w) >= 0)
                return ee::toFloat(int32_t(w));
            const ee::Float half = ee::toFloat(int32_t((w & 1u) | (w >> 1)));
            return half + half;
        }
    }

    // carPathName: the name hash of the path a car starts on (its
    // component's word 4).
    int32_t carPathName(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagWord(zge::objectComponent(car), kCarTagWords, kCarTagPath);
    }
    READABLE_FUNCTION(carPathName, Verified);

    // carWheelRadius: the radius of the car's wheels (its class's float at 4).
    ee::Float carWheelRadius(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::levelObjectClassTag(car), kCarTagWords, kCarClassTagWheelRadius);
    }
    READABLE_FUNCTION(carWheelRadius, Verified);

    // carTopSpeed: a car's top speed (its component's float at 8).
    ee::Float carTopSpeed(ee::Ptr<zge::SceneObject> car)
    {
        return zge::tagFloat(zge::objectComponent(car), kCarTagWords, kCarTagTopSpeed);
    }
    READABLE_FUNCTION(carTopSpeed, Verified);

    // followerGap: how far the next car is the way the follower goes (ahead
    // moving forwards, behind backwards), or -1 with none.
    ee::Float followerGap(ee::Ptr<zge::Follower> follower)
    {
        const ee::Ptr<zge::Follower> other = ee::Float(0.0f) <= zge::followerSpeed(follower) ? zge::followerAhead(follower)
                                                                                              : zge::followerBehind(follower);
        if (!other)
            return ee::Float(-1.0f);
        const ee::Float here = zge::followerDistance(follower);
        return ee::abs(here - zge::followerDistance(other));
    }
    READABLE_FUNCTION(followerGap, Verified);

    // cruiseDrive (speed, along, toEnd): no reason to stop: speed up to 10;
    // keep it under 25 from the segment's ends; else speed up.
    uint32_t cruiseDrive(ee::Float speed, ee::Float along, ee::Float toEnd)
    {
        if (!(ee::Float(10.0f) < speed))
            return kCarSpeedUp;
        if (along < ee::Float(25.0f) || toEnd < ee::Float(25.0f))
            return kCarKeep;
        return kCarSpeedUp;
    }
    READABLE_FUNCTION(cruiseDrive, Verified);

    // driveTrafficCar (car, follower): a traffic car's speed for the frame.
    // With a car ahead, by how soon it would reach it and how near it is;
    // else by the traffic light at its segment's end: braking when it could
    // not pass in time, or before the amber's time runs out (cruiseDrive
    // without a light, or past it in time). Then the speed changed by the
    // drive (sped up no further than the car's top speed), unless it is the
    // speed it has.
    void driveTrafficCar(ee::Ptr<zge::SceneObject> car, ee::Ptr<zge::Follower> follower)
    {
        uint32_t drive = kCarKeep;
        const ee::Float gap = followerGap(follower);
        const ee::Float speed = zge::followerSpeed(follower);
        if (gap < ee::Float(0.0f))
        {
            // By the traffic light at the segment's end.
            const ee::Float along = zge::followerDistance(follower);
            const ee::Float toEnd = zge::segmentEnd(zge::followerSegment(follower)) - along;
            if (!zge::segmentJunction(zge::followerSegment(follower)))
                drive = cruiseDrive(speed, along, toEnd);
            else
            {
                const uint32_t state = lightState(ee::Address(zge::followerSegment(follower))) & 0xFFFFu;
                if (state == kLightAmber)
                {
                    if (ee::Float(0.0f) < speed)
                    {
                        const ee::Float time = toEnd / speed;
                        const ee::Float stopTime = speed / ee::Float(kStopTimePerSpeed) + ee::Float(kStopTimeLeast);
                        const uint32_t light = zge::segmentJunction(zge::followerSegment(follower));
                        const ee::Float left = lightCycle(light) - lightElapsed(light);
                        if (left < time)
                            drive = time < stopTime ? static_cast<uint32_t>(kCarBrake) : cruiseDrive(speed, along, toEnd);
                    }
                }
                else if (state == kLightRed)
                {
                    if (ee::Float(0.0f) < speed)
                    {
                        const ee::Float time = toEnd / speed;
                        const ee::Float stopTime = speed / ee::Float(kStopTimePerSpeed) + ee::Float(kStopTimeLeast);
                        drive = time < stopTime ? static_cast<uint32_t>(kCarBrake) : cruiseDrive(speed, along, toEnd);
                    }
                }
                else
                    drive = cruiseDrive(speed, along, toEnd);
            }
        }
        else
        {
            // By the car ahead: how soon it would be reached, how near it is.
            const ee::Float room = gap - ee::Float(kCarLength);
            const ee::Ptr<zge::Follower> other = ee::Float(0.0f) <= speed ? zge::followerAhead(follower) : zge::followerBehind(follower);
            const ee::Float closing = speed - zge::followerSpeed(other);
            bool closingIn = ee::Float(0.0f) < speed && ee::Float(0.0f) < closing;
            if (!closingIn)
                closingIn = speed < ee::Float(0.0f) && closing < ee::Float(0.0f);
            const ee::Float time = closingIn ? ee::abs(room) / ee::abs(closing) : ee::Float(kNotClosing);
            const int soon = time == ee::Float(kNotClosing) ? 3 : time < ee::Float(kSoonTime) ? 0 : time < ee::Float(kLaterTime) ? 1 : 2;
            const int near = room < ee::Float(kNearRoom) ? 0 : room < ee::Float(kFarRoom) ? 1 : 2;
            if (soon == 3) // not closing in
                drive = near == 0 ? kCarKeep : kCarSpeedUp;
            else if (soon == 0) // within 1 s
                drive = kCarEaseOff;
            else if (soon == 2 && near == 2)
                drive = kCarSpeedUp;
            else if (near == 0)
                drive = kCarBrake;
            else
                drive = soon == 1 && near == 1 ? kCarBrake : kCarKeep;
        }

        const ee::Float sign = speed < ee::Float(0.0f) ? ee::Float(-1.0f) : ee::Float(1.0f);
        ee::Float wanted = ee::abs(speed);
        if (drive == kCarBrake || drive == kCarEaseOff)
        {
            if (ee::Float(0.0f) < wanted)
            {
                wanted = wanted - ee::Float(drive == kCarBrake ? kBrakeStep : kEaseOffStep);
                if (wanted < ee::Float(0.0f))
                    wanted = ee::Float(0.0f);
            }
        }
        else if (drive == kCarSpeedUp)
        {
            const ee::Float top = ee::abs(carTopSpeed(car));
            wanted = wanted + ee::Float(kSpeedUpStep);
            if (top < wanted)
                wanted = top;
        }
        if (!(ee::abs(speed - wanted) < ee::Float(kSameSpeed)))
            game::tail(zge::setFollowerSpeed, follower, wanted * sign);
    }
    READABLE_FUNCTION(driveTrafficCar, Verified);

    // registerCarType: the "Car" level objects made traffic cars.
    uint32_t registerCarType()
    {
        return game::tail(zge::registerEntityInit, kCarTypeName, sym::newTrafficCar);
    }
    READABLE_FUNCTION(registerCarType, Verified);

    // chooseJunctionExit (object, words, weights): which of a junction's three exits
    // to take, drawn by the object's weights (three floats of its component
    // from `weights`, in percent of their sum; all about 0 as a third each)
    // with its random number (the word after them, drawn on): exit 0 by the
    // first weight, 1 by the third, else 2.
    uint32_t chooseJunctionExit(ee::Ptr<zge::SceneObject> object, uint32_t words, uint32_t weights)
    {
        const uint32_t randomTag = weights + (kCarTagRandom - kCarTagExitWeights);
        ee::Float first = zge::tagFloat(zge::objectComponent(object), words, weights);
        const ee::Float second = zge::tagFloat(zge::objectComponent(object), words, weights + 4);
        ee::Float third = zge::tagFloat(zge::objectComponent(object), words, weights + 8);
        ee::Float sum = first + third + second;
        const uint32_t seed = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(object), words, randomTag));
        if (ee::abs(sum) < ee::Float(kNoWeights))
        {
            first = ee::Float(kEvenWeight);
            sum = ee::Float::fromBits(kEvenWeightsBits);
            third = first;
        }
        const ee::Float percent = ee::Float(kPercent) / sum;
        const ee::Float firstShare = first / sum * percent;
        const ee::Float upToThird = third / sum * percent + firstShare;
        const uint32_t drawn = nextRandom(seed);
        zge::setTagWord(zge::objectComponent(object), words, randomTag, drawn);
        const ee::Float draw = unsignedToFloat(drawn) / unsignedToFloat(kRandomModulus) + ee::Float(kLeastDraw);
        if (draw <= firstShare)
            return 0;
        return draw <= upToThird ? 1 : 2;
    }
    READABLE_FUNCTION(chooseJunctionExit, Verified);

    // chooseNextSegment (object, follower, backwards, words, weights): the
    // name hash of the segment the follower goes on to at its segment's end
    // (the start's exits going backwards), -1 with none: an exit whose
    // start's tags hold the object's name, else the only one, else one
    // drawn by chooseJunctionExit. Of two, the draw's 0 takes the first of exits 0
    // and 1 there is, its 1 the first of exits 1 and 2.
    int32_t chooseNextSegment(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Follower> follower, uint32_t backwards, uint32_t words,
                              uint32_t weights)
    {
        uint32_t exits[3];
        const uint32_t name = zge::objectNameHash(object);
        for (uint32_t exit = 0; exit < 3; ++exit)
        {
            const ee::Ptr<zge::PathRecord> segment = zge::segmentExit(zge::followerSegment(follower), exit, backwards);
            if (!segment)
            {
                exits[exit] = 0;
                continue;
            }
            exits[exit] = zge::pathNameHash(segment);
            if (zge::pathStartTagsInclude(segment, static_cast<int32_t>(name)))
                return static_cast<int32_t>(exits[exit]);
        }
        uint32_t count = 0, only = 0;
        for (uint32_t exit = 0; exit < 3; ++exit)
            if (exits[exit] != 0)
            {
                only = exits[exit];
                ++count;
            }
        if (count == 0)
            return kNoSegment;
        if (count == 1)
            return static_cast<int32_t>(only);
        if (count == 2)
        {
            uint32_t drawn;
            do
                drawn = chooseJunctionExit(object, words, weights);
            while (drawn == 2);
            if (drawn == 0)
                return static_cast<int32_t>(exits[0] != 0 ? exits[0] : exits[1]);
            return static_cast<int32_t>(exits[1] != 0 ? exits[1] : exits[2]);
        }
        return static_cast<int32_t>(exits[chooseJunctionExit(object, words, weights)]);
    }
    READABLE_FUNCTION(chooseNextSegment, Verified);

    // trafficCarFrame (the update message): the car's frame. A car of a
    // class that drives itself sets its speed (driveTrafficCar); then it
    // moves along its segment by the frame's step, or at the segment's end
    // goes on to the next one (the way it goes, else back; it stays on its
    // follower's own course with none). It is put where its follower is,
    // turned to the path; its wheels turn by the ground it covered and its
    // rotators by their spin; then its sound has its turn.
    int32_t trafficCarFrame(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec3> up;
        ee::Local<zge::Matrix> placed;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<zge::Quaternion> turn;
        ee::Local<zge::Vec3> axis;
        ee::Local<zge::Vec4> moved;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const zge::Vec4 start = *zge::objectPosition(car);
        zge::objectNameHash(car); // (its answer unused)
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        const ee::Float step = *ee::Ptr<ee::Float>(message->data);
        if (zge::tagByte(zge::levelObjectClassTag(car), kCarClassTagBytes, kCarClassTagDrives))
            driveTrafficCar(car, follower);

        const ee::Float speed = zge::followerSpeed(follower);
        const ee::Float length = zge::segmentEnd(zge::followerSegment(follower));
        const ee::Float next = zge::followerAlong(follower) * length + speed * step;
        if (next < ee::Float(0.0f) || length < next)
        {
            const uint32_t backwards = speed < ee::Float(0.0f) ? 1u : 0u;
            int32_t segment = chooseNextSegment(car, follower, backwards, kCarTagWords, kCarTagExitWeights);
            if (segment == kNoSegment)
                segment = chooseNextSegment(car, follower, backwards ^ 1u, kCarTagWords, kCarTagExitWeights);
            if (segment == kNoSegment)
                zge::advanceFollower(follower, step);
            else
            {
                zge::setFollowerSegment(follower, zge::findPathRecord(static_cast<uint32_t>(segment)));
                zge::setFollowerSpeed(follower, speed);
                zge::setFollowerAlong(follower, ee::Float(0.0f) < speed ? ee::Float(0.0f) : ee::Float(1.0f));
            }
        }
        else
            zge::advanceFollower(follower, step);

        // Turned to the path (its forward and side axes the other way) and
        // put at the follower's point.
        *up = zge::Vec3{ee::Float(0.0f), ee::Float(1.0f), ee::Float(0.0f)};
        zge::followerOrientation(follower, up, placed);
        for (const int row : {2, 0})
        {
            zge::Vec4 &axisRow = placed->row(row);
            axisRow.x = -axisRow.x;
            axisRow.y = -axisRow.y;
            axisRow.z = -axisRow.z;
        }
        zge::setObjectRotationMatrix(car, placed);
        zge::setObjectPosition(car, &placed->translation());

        uint32_t bones = zge::modelBoneCount(car);
        if (bones >= 2)
        {
            const uint32_t noRotator = zge::nameHash(&*kNoRotatorName);
            uint32_t names[kCarBoneNames];
            names[kFrontWheels] = zge::nameHash(&*kFrontWheelsName);
            names[kBackWheels] = zge::nameHash(&*kBackWheelsName);
            names[kRotatorXAxis] = zge::nameHash(&*kRotatorXAxisName);
            names[kRotatorYAxis] = zge::nameHash(&*kRotatorYAxisName);
            names[kRotatorZAxis] = zge::nameHash(&*kRotatorZAxisName);
            if (bones > kMostTurnedBones)
                bones = kMostTurnedBones;
            for (uint32_t index = 0; index < bones; ++index)
            {
                // The first name left with a bone (up to the last name).
                uint32_t *name = names;
                ee::Ptr<zge::AnimationChannel> bone;
                for (;;)
                {
                    bone = zge::findAnimatorBone(car, *name);
                    if (bone)
                        break;
                    const uint32_t tried = *name++;
                    if (tried == names[kRotatorZAxis])
                        break;
                }
                if (!bone)
                    continue;

                const ee::Float radius = carWheelRadius(car);
                const ee::Float circumference = (radius + radius) * ee::Float(kPi);
                zge::Angle angle;
                if (*name == names[kFrontWheels] || *name == names[kBackWheels])
                {
                    *moved = *zge::objectPosition(car) - start;
                    const ee::Float covered = zge::vec3Length(&moved->xyz());
                    const uint64_t units = sce::libgcc::dpmul(sce::libgcc::fptodp(covered), kTurnUnitsDouble);
                    angle = static_cast<zge::Angle>(sce::libgcc::dptoli(sce::libgcc::dpdiv(units, sce::libgcc::fptodp(circumference))));
                    *axis = zge::Vec3{ee::Float(1.0f), ee::Float(0.0f), ee::Float(0.0f)};
                }
                else
                {
                    angle = static_cast<zge::Angle>(ee::toInt(ee::Float(kRotatorSpin) / circumference));
                    if (*name == names[kRotatorYAxis])
                        *axis = zge::Vec3{ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
                    else if (*name == names[kRotatorZAxis])
                        *axis = zge::Vec3{ee::Float(0.0f), ee::Float(-1.0f), ee::Float(0.0f)};
                    else
                        *axis = zge::Vec3{ee::Float(1.0f), ee::Float(0.0f), ee::Float(0.0f)};
                }
                zge::quaternionFromAxisAngle(turn, axis, angle);
                zge::quaternionCopy(rotation, zge::boneRotation(bone));
                zge::quaternionMultiply(rotation, rotation, turn);
                zge::setBoneRotation(bone, rotation);
                *name = noRotator;
            }
        }
        zge::updatePose(car);
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarFrame, Verified);

    // trafficCarRemoved (the removed message): the car's follower freed; its
    // sound has its turn (and goes).
    int32_t trafficCarRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        zge::freePathFollower(carFollower(ee::Ptr<zge::SceneObject>(message->target)));
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarRemoved, Verified);

    // trafficCarVelocity (the velocity message): the car's velocity: its
    // forward axis (the orientation's third row, the other way) at its
    // follower's speed; w is what the scaling leaves. Its sound has its turn.
    int32_t trafficCarVelocity(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Matrix> orientation;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Vec4> velocity(message->data);
        zge::quaternionToMatrix(orientation, &*zge::objectOrientation(car));
        *velocity = orientation->row(2);
        zge::vec3Negate(&velocity->xyz(), &velocity->xyz());
        const ee::Float speed = ee::abs(zge::followerSpeed(carFollower(car)));
        *velocity = *velocity * speed;
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarVelocity, Verified);

    // trafficCarSaveSize (the save-size message): a car's save is 52 bytes;
    // its sound has its turn.
    int32_t trafficCarSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(CarSave);
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarSaveSize, Verified);

    // trafficCarSave (the save message): where its follower is and the
    // car's component, made in the frame and copied out; its sound has its
    // turn.
    int32_t trafficCarSave(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<CarSave> save;
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        save->endMode = zge::followerEndMode(follower);
        save->segment = zge::pathNameHash(zge::followerSegment(follower));
        save->along = zge::followerAlong(follower);
        save->speed = zge::followerSpeed(follower);
        zge::memoryCopy(save->component, zge::objectComponent(car), kCarComponentBytes);
        zge::memoryCopy(message->data, save, sizeof(CarSave));
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarSave, Verified);

    // trafficCarRestore (the restore message): the follower put back where
    // the save says, the car's component copied back (its follower word set
    // again); its sound has its turn.
    int32_t trafficCarRestore(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> car(message->target);
        const ee::Ptr<zge::Follower> follower = carFollower(car);
        const ee::Ptr<CarSave> save(message->data);
        zge::setFollowerSegment(follower, zge::findPathRecord(save->segment));
        zge::setFollowerAlong(follower, save->along);
        zge::setFollowerSpeed(follower, save->speed);
        zge::setFollowerEndMode(follower, save->endMode);
        zge::memoryCopy(zge::objectComponent(car), save->component, kCarComponentBytes);
        setObjectComponentWord(car, follower);
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarRestore, Verified);

    // trafficCarTouched (the touch message): its sound has its turn.
    int32_t trafficCarTouched(ee::Ptr<zge::EngineMessage> message)
    {
        objectSoundMessage(message, carSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficCarTouched, Verified);

    // newTrafficCar: the object made a car: its messages; a follower on its
    // path at its top speed (on from the path's other end at the last
    // point), snapped to where it stands; a car with no random number yet
    // gets the next (the first from the collision clock); shown; its sound
    // started.
    int32_t newTrafficCar(ee::Ptr<zge::SceneObject> car)
    {
        zge::subscribe(car, zge::kUpdateMessage, zge::MessageHandler(sym::trafficCarFrame));
        zge::subscribe(car, zge::kRemovedMessage, zge::MessageHandler(sym::trafficCarRemoved));
        zge::subscribe(car, zge::kVelocityMessage, zge::MessageHandler(sym::trafficCarVelocity));
        zge::subscribe(car, zge::kSaveSizeMessage, zge::MessageHandler(sym::trafficCarSaveSize));
        zge::subscribe(car, zge::kSaveMessage, zge::MessageHandler(sym::trafficCarSave));
        zge::subscribe(car, zge::kRestoreMessage, zge::MessageHandler(sym::trafficCarRestore));
        zge::subscribe(car, kTouchMessage, zge::MessageHandler(sym::trafficCarTouched));
        const ee::Ptr<zge::PathRecord> path = zge::findPathRecord(static_cast<uint32_t>(carPathName(car)));
        const ee::Ptr<zge::Follower> follower = zge::newPathFollower(path, kCarFollowerLoops, carTopSpeed(car), ee::Float(0.0f));
        setObjectComponentWord(car, follower);
        zge::followerSnap(follower, zge::objectPosition(car));
        if (zge::tagWord(zge::objectComponent(car), kCarTagWords, kCarTagRandom) == 0)
        {
            if (*g_carSeedStarted == 0)
            {
                *g_carSeedStarted = 1;
                *g_carSeed = zge::collisionClock();
            }
            *g_carSeed = nextRandom(*g_carSeed);
            zge::setTagWord(zge::objectComponent(car), kCarTagWords, kCarTagRandom, *g_carSeed);
        }
        zge::setSceneObjectShown(car, 1);
        startObjectSound(car, static_cast<uint32_t>(zge::tagWord(zge::objectComponent(car), kCarTagWords, kCarTagSound)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newTrafficCar, Verified);
}
