// BMX XXX's "BirdObj" level object type: a bird of a flock (flocks.cpp).
// BMX XXX's own code (its game block); no other build names its functions,
// so the names are from what each does.
//
// A bird sits at its home (where the level put it) until its flock flies;
// then it is wary, and flies itself once a rider is close to it. Flying, it
// steers by its flock: with the other birds' velocity, apart from them when
// they are close, after the flock object, away from a rider, up while a
// rider is near its home and down when it is high over it. With the rider
// gone it returns: towards its home, down to it, and perched again when it
// is there. A bird that moved is turned a tenth of the way to where it goes.
//
// Up is -y.

#include "entity_messages.h"
#include "flocks.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/birds.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        // The engine's rotation that turns unit vector `from` onto `to` (zge/math/matrix.cpp), by address: the
        // engine's header does not declare it, as zge/collision/query.cpp keeps a wrapper of its own under the name
        // (DM2's instance compiles it, where the body is not proven), which a declaration would make ambiguous.
        // Scaffolding (address-call): a plain call once query.cpp's goes.
        ee::Ptr<zge::Matrix> rotationFromTo(ee::Ptr<zge::Matrix> matrix, ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to)
        {
            return ee::game<ee::Ptr<zge::Matrix>>(sym::rotationFromTo, matrix, from, to);
        }
    }

    namespace
    {
        constexpr uint32_t kTypeName = sym::birdTypeName; // "BirdObj"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // The working record every bird's update copies its tag data into.
        inline ee::Global<BirdRecord, sym::g_birdState> g_birdState;
        // Set to 1 by every bird's init; nothing reads it.
        inline ee::Global<uint32_t, sym::g_birdInitRan> g_birdInitRan;

        // A bird's animations loop and run only while it is on screen
        // (playAnimation's mode bits 0 and 1).
        constexpr uint32_t kPlayLooped = 1u << 0;
        constexpr uint32_t kPlayOnScreenOnly = 1u << 1;

        // A wary bird flies with a rider nearer than the root of 60; a
        // flying one returns with the rider further than 10; a returning
        // one flies again with the rider nearer than 8.
        constexpr ee::Float kFlyDistanceSquared(60.0f);
        constexpr ee::Float kReturnDistanceSquared(100.0f);
        constexpr ee::Float kFlyAgainDistance(8.0f);
        // Flying: how much of the other birds' mean velocity it takes
        // (0.7), of its way out of their middle (0.8), and of its way to the
        // flock object (0.9), which counts as one unit when further than 2.
        inline ee::Global<ee::Float, sym::g_birdMatchWeight> g_birdMatchWeight;
        inline ee::Global<ee::Float, sym::g_birdApartWeight> g_birdApartWeight;
        inline ee::Global<ee::Float, sym::g_birdFollowWeight> g_birdFollowWeight;
        constexpr ee::Float kNearOthers(2.0f);
        constexpr ee::Float kFarFromFlock(2.0f);
        // It flees a rider nearer than 15.
        constexpr ee::Float kFleeDistance(15.0f);
        constexpr ee::Float kFleeWeight(1.0f);
        // A rider within the root of 75 of its home lifts a flying bird (by
        // up to 2); further than that brings a returning one down (by up
        // to 8).
        constexpr ee::Float kHomeClearSquared(75.0f);
        constexpr ee::Float kMostDescent(4.0f);
        // A flying bird more than 10 over its home is pushed down, 3 for
        // each unit over.
        constexpr ee::Float kMostHeight(10.0f);
        constexpr ee::Float kDownASurplusUnit(3.0f);
        // A returning bird's descent fades out over its last 3 units of height.
        constexpr ee::Float kDescentFadeHeight(3.0f);
        // Returning: its way home counts as 6 when its square is over 6
        // (the original compares the square with the length), and it takes
        // this much of it (0.8).
        constexpr ee::Float kMostHomeward(6.0f);
        inline ee::Global<ee::Float, sym::g_birdHomeWeight> g_birdHomeWeight;
        // It is home within this distance squared (0.7).
        inline ee::Global<ee::Float, sym::g_birdLandDistanceSquared> g_birdLandDistanceSquared;
        constexpr ee::Float kMostVelocity(5.0f);
        // A frame's share of a second (1 / 60): the flying and the
        // returning state read their own.
        inline ee::Global<ee::Float, sym::g_birdFlightFrameSeconds> g_birdFlightFrameSeconds;
        inline ee::Global<ee::Float, sym::g_birdReturnFrameSeconds> g_birdReturnFrameSeconds;
        // The turn: only when its level velocity squared is over this
        // (0.01); through the turn from the model's forward to it while
        // their cosine is between these (-0.99, 0.99), else to a half turn;
        // this much of the way each update (0.1).
        inline ee::Global<ee::Float, sym::g_birdTurnSpeedSquared> g_birdTurnSpeedSquared;
        inline ee::Global<ee::Float, sym::g_birdTurnLeastCosine> g_birdTurnLeastCosine;
        inline ee::Global<ee::Float, sym::g_birdTurnMostCosine> g_birdTurnMostCosine;
        inline ee::Global<ee::Float, sym::g_birdTurnShare> g_birdTurnShare;
    }

    uint32_t initBird(ee::Ptr<zge::SceneObject> bird);
    void copyBirdVector(const zge::Vec3 *from, zge::Vec3 *to);
    void copyBirdVectorOut(const zge::Vec3 *from, zge::Vec3 *to);
    ee::Ptr<BirdRecord> loadBirdState(ee::Ptr<zge::SceneObject> bird);
    void storeBirdState(ee::Ptr<zge::SceneObject> bird, ee::Ptr<BirdRecord> record);
    int32_t birdActivated(ee::Ptr<zge::EngineMessage> message);
    int32_t updateBird(ee::Ptr<zge::EngineMessage> message);
    int32_t birdTouched(ee::Ptr<zge::EngineMessage> message);

    namespace
    {
        // The animation of that name hash played, if the libraries in use
        // have it.
        void playBirdAnimation(ee::Ptr<zge::SceneObject> bird, uint32_t animation)
        {
            const ee::Ptr<zge::Motion> motion = zge::findLibraryAnimation(animation);
            if (motion)
                zge::playAnimationWithHooks(bird, motion, kPlayLooped | kPlayOnScreenOnly, 0, 0, ee::Float(0.0f));
        }

        // The moved bird's step taken: its velocity cut to kMostVelocity,
        // its position moved by a frame of it at its speed, and held at its
        // home's height when `offset` (its home from where it was) says it
        // was under it. Its record takes both.
        void stepBird(ee::Ptr<zge::SceneObject> bird, ee::Ptr<BirdRecord> record, ee::Ptr<zge::Vec4> position,
                      zge::Vec4 &velocity, zge::Vec4 &step, const zge::Vec4 &offset, const zge::Vec4 &home, ee::Float frameSeconds)
        {
            if (kMostVelocity < zge::vec3Length(&velocity.xyz()))
            {
                velocity = zge::unit(velocity);
                velocity = zge::scaledAfterMeasure(velocity, kMostVelocity);
            }
            step = velocity * (record->speed * frameSeconds);
            *position = *position + step;
            zge::setObjectPosition(bird, position);
            if (offset.y < ee::Float(0.0f))
            {
                velocity.y = ee::Float(0.0f);
                position->y = home.y;
            }
            copyBirdVector(&velocity.xyz(), &record->velocity);
            copyBirdVector(&position->xyz(), &record->position);
        }

        // A flying bird's update. 1 when it moved.
        bool flyBird(ee::Ptr<zge::SceneObject> bird, ee::Ptr<BirdRecord> record, ee::Ptr<zge::Vec4> position,
                     ee::Ptr<zge::Vec4> riderPosition)
        {
            ee::Local<zge::Vec4> offset;
            ee::Local<zge::Vec4> spare;
            ee::Local<zge::Vec4> toFlock;
            ee::Local<zge::Vec4> middle;
            ee::Local<zge::Vec4> flockPositions;
            ee::Local<zge::Vec4> home;
            ee::Local<zge::Vec4> fromRider;
            ee::Local<zge::Vec4> step;
            ee::Local<zge::Vec4> flockVelocities;
            ee::Local<zge::Vec4> lift;
            ee::Local<zge::Vec4> velocity;
            ee::Local<zge::Vec4> apart;

            *offset = *position - *riderPosition;
            if (kReturnDistanceSquared < zge::squaredLength(*offset))
            {
                record->state = kBirdReturning;
                record->animation = record->returnAnimation;
                playBirdAnimation(bird, record->returnAnimation);
                return false;
            }

            const int32_t count = zge::tagWord(zge::objectComponent(record->flock), kFlockTagWord, offsetof(FlockRecord, birdCount));
            zge::tagVector(zge::objectComponent(record->flock), kFlockTagVector, offsetof(FlockRecord, birdPositionSum), flockPositions);
            zge::tagVector(zge::objectComponent(record->flock), kFlockTagVector, offsetof(FlockRecord, birdVelocitySum), flockVelocities);
            copyBirdVectorOut(&record->home, &home->xyz());
            copyBirdVectorOut(&record->velocity, &velocity->xyz());
            // Cleared and not used.
            *spare = zge::kZero;

            if (static_cast<uint32_t>(count) >= 2)
            {
                // With the others: their summed velocity, over the whole
                // count.
                *flockVelocities = *flockVelocities - *velocity;
                *flockVelocities = *flockVelocities * (ee::Float(1.0f) / ee::toFloat(count));
                *flockVelocities = *flockVelocities * *g_birdMatchWeight;
                *velocity = *velocity + *flockVelocities;
                // Apart from them: the others' summed positions less its
                // own, over the count, taken as their middle when that is
                // no longer than kNearOthers; otherwise nothing.
                *flockPositions = *flockPositions - *position;
                *flockPositions = *flockPositions * (ee::Float(1.0f) / ee::toFloat(count));
                if (kNearOthers < zge::vec3Length(&flockPositions->xyz()))
                    *middle = *position;
                else
                    *middle = *flockPositions;
                *apart = *position - *middle;
                *apart = *apart * *g_birdApartWeight;
                *velocity = *velocity + *apart;
            }

            // After the flock object.
            *toFlock = *zge::objectPosition(record->flock) - *position;
            if (kFarFromFlock < zge::vec3Length(&toFlock->xyz()))
                *toFlock = zge::unit(*toFlock);
            *toFlock = *toFlock * *g_birdFollowWeight;
            *velocity = *velocity + *toFlock;

            // Away from a close rider.
            *fromRider = *position - *riderPosition;
            if (zge::vec3Length(&fromRider->xyz()) < kFleeDistance)
            {
                *fromRider = *fromRider * kFleeWeight;
                *velocity = *velocity + *fromRider;
            }

            // Up while the rider is near its home; down when it is high
            // over its home.
            *offset = *home - *riderPosition;
            *lift = zge::kZero;
            if (zge::squaredLength(*offset) < kHomeClearSquared)
            {
                const ee::Float clear = ee::Float(1.0f) - zge::squaredLength(*offset) / kHomeClearSquared;
                lift->y = -(clear + clear);
            }
            *offset = *home - *position;
            if (kMostHeight < offset->y)
                lift->y = lift->y + (offset->y - kMostHeight) * kDownASurplusUnit;
            *velocity = *velocity + *lift;

            stepBird(bird, record, position, *velocity, *step, *offset, *home, *g_birdFlightFrameSeconds);
            return true;
        }

        // A returning bird's update. 1 when it moved.
        bool returnBird(ee::Ptr<zge::SceneObject> bird, ee::Ptr<BirdRecord> record, ee::Ptr<zge::Vec4> position,
                        ee::Ptr<zge::Vec4> riderPosition)
        {
            ee::Local<zge::Vec4> offset;
            ee::Local<zge::Vec4> spare;
            ee::Local<zge::Vec4> lift;
            ee::Local<zge::Vec4> toHome;
            ee::Local<zge::Vec4> flockPositions;
            ee::Local<zge::Vec4> home;
            ee::Local<zge::Vec4> step;
            ee::Local<zge::Vec4> velocity;

            *offset = *position - *riderPosition;
            if (zge::vec3Length(&offset->xyz()) < kFlyAgainDistance)
            {
                record->state = kBirdFlying;
                record->animation = record->flyAnimation;
                playBirdAnimation(bird, record->flyAnimation);
                return false;
            }

            // Its flock's count and sums are fetched and not used (the
            // summed velocity into the vector its step is worked out in).
            zge::tagWord(zge::objectComponent(record->flock), kFlockTagWord, offsetof(FlockRecord, birdCount));
            zge::tagVector(zge::objectComponent(record->flock), kFlockTagVector, offsetof(FlockRecord, birdPositionSum), flockPositions);
            zge::tagVector(zge::objectComponent(record->flock), kFlockTagVector, offsetof(FlockRecord, birdVelocitySum), step);
            copyBirdVectorOut(&record->velocity, &velocity->xyz());
            copyBirdVectorOut(&record->home, &home->xyz());

            *offset = *home - *position;
            if (zge::squaredLength(*offset) < *g_birdLandDistanceSquared)
            {
                // Home: perched there, still.
                record->animation = record->perchAnimation;
                playBirdAnimation(bird, record->perchAnimation);
                copyBirdVectorOut(&record->home, &position->xyz());
                zge::setObjectPosition(bird, position);
                record->position = record->home;
                record->state = kBirdPerched;
                record->velocity = zge::Vec3{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f)};
                return false;
            }
            // Cleared and not used.
            *spare = zge::kZero;

            // Towards its home, on the level.
            *toHome = *home - *position;
            toHome->y = ee::Float(0.0f);
            if (kMostHomeward < zge::squaredLength(*toHome))
            {
                *toHome = zge::unit(*toHome);
                *toHome = zge::scaledAfterMeasure(*toHome, kMostHomeward);
            }
            *toHome = *toHome * *g_birdHomeWeight;
            *velocity = *velocity + *toHome;

            // Down, the more the further the rider is from its home, and
            // less over its last units of height.
            *offset = *home - *riderPosition;
            *lift = zge::kZero;
            if (kHomeClearSquared < zge::squaredLength(*offset))
            {
                ee::Float descent = (zge::squaredLength(*offset) - kHomeClearSquared) / kHomeClearSquared;
                if (kMostDescent < descent)
                    descent = kMostDescent;
                lift->y = descent + descent;
            }
            *offset = *home - *position;
            ee::Float height = offset->y;
            if (height < kDescentFadeHeight)
            {
                if (height < ee::Float(0.0f))
                    height = ee::Float(0.0f);
                lift->y = lift->y * (height / kDescentFadeHeight);
            }
            *velocity = *velocity + *lift;

            stepBird(bird, record, position, *velocity, *step, *offset, *home, *g_birdReturnFrameSeconds);
            return true;
        }
    }

    // nextFlockBird: the flock's bird after this one, 0 for none.
    ee::Ptr<zge::SceneObject> nextFlockBird(ee::Ptr<zge::SceneObject> bird)
    {
        return ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, nextBird))));
    }
    READABLE_FUNCTION(nextFlockBird, Verified);

    // registerBirdType: the "BirdObj" type, made as a plain entity and set
    // up by initBird.
    uint32_t registerBirdType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initBird));
    }
    READABLE_FUNCTION(registerBirdType, Verified);

    // nearestRiderVisit (forEachRider's visit; the context is the object):
    // the rider nearest the object so far is kept, with its distance
    // squared (g_nearestRider, g_nearestRiderDistanceSquared: under 0 for
    // none yet). Always 1: every rider is visited.
    uint32_t nearestRiderVisit(uint32_t riderIndex, ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::Vec4> toRider;
        riderIndex &= 0xFFu;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex);
        if (*g_nearestRiderDistanceSquared < ee::Float(0.0f))
        {
            *g_nearestRider = static_cast<uint8_t>(riderIndex);
            const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider->messenger);
            *toRider = *riderPosition - *zge::objectPosition(object);
            *g_nearestRiderDistanceSquared = zge::dot(*toRider, *toRider);
        }
        else
        {
            const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider->messenger);
            *toRider = *riderPosition - *zge::objectPosition(object);
            const ee::Float distanceSquared = zge::dot(*toRider, *toRider);
            if (distanceSquared < *g_nearestRiderDistanceSquared)
            {
                *g_nearestRiderDistanceSquared = distanceSquared;
                *g_nearestRider = static_cast<uint8_t>(riderIndex);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(nearestRiderVisit, Verified);

    // initBird (the type's init): riders pass through it; it gets an
    // animator if it has none; its messages subscribed; where it is becomes
    // its home. Always 1.
    uint32_t initBird(ee::Ptr<zge::SceneObject> bird)
    {
        ee::Local<zge::Vec4> position;
        const uint64_t node = objectNodeHandle(bird);
        loadBirdState(bird);
        rider::collision::setNodeCollidable(node, 0);
        if (!bird->animator)
            zge::createObjectAnimator(bird);
        zge::subscribe(bird, kRiderTouchMessage, zge::MessageHandler(sym::birdTouched));
        zge::subscribe(bird, kActivateMessage, zge::MessageHandler(sym::birdActivated));
        *position = *zge::objectPosition(bird);
        zge::setTagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, home), position.address());
        zge::setTagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, position), position.address());
        zge::stopTracks(bird);
        *g_birdInitRan = 1;
        zge::subscribe(bird, zge::kUpdateMessage, zge::MessageHandler(sym::updateBird));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initBird, Verified);

    // copyBirdVector, copyBirdVectorOut: x, y and z copied. The same code
    // twice (and the flock's two again).
    void copyBirdVector(const zge::Vec3 *from, zge::Vec3 *to)
    {
        to->x = from->x;
        to->y = from->y;
        to->z = from->z;
    }
    READABLE_FUNCTION(copyBirdVector, Verified);

    void copyBirdVectorOut(const zge::Vec3 *from, zge::Vec3 *to)
    {
        to->x = from->x;
        to->y = from->y;
        to->z = from->z;
    }
    READABLE_FUNCTION(copyBirdVectorOut, Verified);

    // loadBirdState: the bird's tag data copied out into the working
    // record, which it answers.
    ee::Ptr<BirdRecord> loadBirdState(ee::Ptr<zge::SceneObject> bird)
    {
        ee::Local<zge::Vec4> vector;
        const ee::Ptr<BirdRecord> record = &*g_birdState;
        record->state = static_cast<uint8_t>(zge::tagByte(zge::objectComponent(bird), kFlockTagByte, offsetof(BirdRecord, state)));
        record->speed = zge::tagFloat(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, speed));
        record->perchAnimation = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, perchAnimation)));
        record->flyAnimation = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flyAnimation)));
        record->returnAnimation = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, returnAnimation)));
        record->flockName = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flockName)));
        record->flock = ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flock))));
        record->animation = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, animation)));
        record->nextBird = ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, nextBird))));
        record->unusedFloat = zge::tagFloat(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, unusedFloat));
        zge::tagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, home), vector);
        copyBirdVector(&vector->xyz(), &record->home);
        zge::tagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, velocity), vector);
        copyBirdVector(&vector->xyz(), &record->velocity);
        zge::tagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, position), vector);
        copyBirdVector(&vector->xyz(), &record->position);
        return record;
    }
    READABLE_FUNCTION_WAITS(loadBirdState, Verified);

    // storeBirdState: the record copied back into the bird's tag data.
    void storeBirdState(ee::Ptr<zge::SceneObject> bird, ee::Ptr<BirdRecord> record)
    {
        ee::Local<zge::Vec4> vector;
        zge::setTagByte(zge::objectComponent(bird), kFlockTagByte, offsetof(BirdRecord, state), record->state);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, perchAnimation), record->perchAnimation);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flyAnimation), record->flyAnimation);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, returnAnimation), record->returnAnimation);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, animation), record->animation);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, nextBird), record->nextBird);
        zge::setTagFloat(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, unusedFloat), record->unusedFloat);
        zge::setTagFloat(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, speed), record->speed);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flockName), record->flockName);
        zge::setTagWord(zge::objectComponent(bird), kFlockTagWord, offsetof(BirdRecord, flock), record->flock);
        copyBirdVectorOut(&record->home, &vector->xyz());
        zge::setTagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, home), vector.address());
        copyBirdVectorOut(&record->velocity, &vector->xyz());
        zge::setTagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, velocity), vector.address());
        copyBirdVectorOut(&record->position, &vector->xyz());
        zge::setTagVector(zge::objectComponent(bird), kFlockTagVector, offsetof(BirdRecord, position), vector.address());
    }
    READABLE_FUNCTION_WAITS(storeBirdState, Verified);

    // birdActivated (kActivateMessage): taken, and nothing done.
    int32_t birdActivated(ee::Ptr<zge::EngineMessage> /*message*/)
    {
        return 1;
    }
    READABLE_FUNCTION(birdActivated, Draft);

    // updateBird (the frame's update). Its first update starts its perched
    // animation and does no more. After that the rider nearest it is
    // found, and its flock, by name, the first time (it joins it); then,
    // by its state:
    //   perched    wary once its flock flies; its animator is freed once
    //              its animation has run past its start
    //   wary       the rider within the root of 60: it flies
    //   flying     flyBird
    //   returning  returnBird
    // A bird that moved is turned towards its level velocity. Always 1.
    int32_t updateBird(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> forward;
        ee::Local<zge::Quaternion> target;
        ee::Local<zge::Vec4> heading;
        ee::Local<zge::Matrix> turn;
        ee::Local<uint32_t> flockName;
        ee::Local<ee::Float> animationTime;
        bool moved = false;
        // The model's forward (its w is never set).
        forward->x = ee::Float(0.0f);
        forward->y = ee::Float(0.0f);
        forward->z = ee::Float(-1.0f);
        const ee::Ptr<zge::SceneObject> bird(message->target);
        const ee::Ptr<BirdRecord> record = loadBirdState(bird);
        if (record->animation == 0)
        {
            const ee::Ptr<zge::Motion> motion = zge::findLibraryAnimation(record->perchAnimation);
            if (motion)
            {
                zge::playAnimationWithHooks(bird, motion, kPlayLooped | kPlayOnScreenOnly, 0, 0, ee::Float(0.0f));
                record->animation = record->perchAnimation;
            }
        }
        else
        {
            *g_nearestRiderDistanceSquared = ee::Float(-1.0f);
            rider::forEachRider(rider::RiderVisit(sym::nearestRiderVisit), bird);
            const ee::Ptr<zge::Vec4> riderPosition = zge::objectPosition(rider::riderOf(*g_nearestRider)->messenger);
            const ee::Ptr<zge::Vec4> position = zge::objectPosition(bird);
            if (!record->flock)
            {
                *flockName = record->flockName;
                const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(flockName.address(), 1);
                if (found)
                {
                    record->flock = *found;
                    record->nextBird = firstFlockBird(*found);
                    addFlockBird(*found, bird);
                }
            }
            switch (record->state)
            {
            case kBirdPerched:
                if (zge::tagByte(zge::objectComponent(record->flock), kFlockTagByte, offsetof(FlockRecord, state)) == kFlockFlying)
                    record->state = kBirdWary;
                if (zge::objectHasAnimator(bird) != 0)
                {
                    zge::animationTime(bird, animationTime);
                    if (ee::Float(0.0f) < *animationTime)
                        zge::freeAnimatorOf(bird);
                }
                break;
            case kBirdWary:
            {
                ee::Local<zge::Vec4> fromRider;
                *fromRider = *position - *riderPosition;
                if (zge::squaredLength(*fromRider) < kFlyDistanceSquared)
                {
                    record->state = kBirdFlying;
                    record->animation = record->flyAnimation;
                    playBirdAnimation(bird, record->flyAnimation);
                }
                break;
            }
            case kBirdFlying:
                moved = flyBird(bird, record, position, riderPosition);
                break;
            case kBirdReturning:
                moved = returnBird(bird, record, position, riderPosition);
                break;
            default:
                break;
            }
        }

        if (moved)
        {
            const ee::Ptr<zge::Quaternion> orientation = zge::objectOrientation(bird);
            heading->y = ee::Float(0.0f);
            heading->x = record->velocity.x;
            heading->z = record->velocity.z;
            if (*g_birdTurnSpeedSquared < zge::squaredLength(*heading))
            {
                *heading = zge::unit(*heading);
                if (*g_birdTurnLeastCosine < zge::dot(*heading, *forward) && zge::dot(*heading, *forward) < *g_birdTurnMostCosine)
                {
                    rotationFromTo(turn, forward, heading);
                    zge::matrixToQuaternion(target, turn);
                }
                else
                {
                    // A half turn about the upright.
                    target->x = ee::Float(0.0f);
                    target->y = ee::Float(1.0f);
                    target->z = ee::Float(0.0f);
                    target->w = ee::Float(0.0f);
                }
                zge::quaternionNlerp(&*orientation, &*orientation, target, *g_birdTurnShare);
                zge::setObjectOrientation(bird, orientation);
            }
            zge::updatePose(bird);
        }
        storeBirdState(bird, record);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateBird, Verified);

    // birdTouched (a rider touches it): taken, and nothing done.
    int32_t birdTouched(ee::Ptr<zge::EngineMessage> /*message*/)
    {
        return 1;
    }
    READABLE_FUNCTION(birdTouched, Draft);
}
