// The level's physics objects (gamehand12; were the sweep's drafts of
// blocks_001bec40 and two of blocks_001bcf90): the "DynamicsObject" level
// objects (crates, cones, barrels: a rigid body each, asleep until knocked),
// their creation, knocks, rest and impact sounds, and the level's contact
// sweep with its handler for each pair of kinds whose boxes overlap: bodies
// with bodies (level/bodies.cpp), NPCs, traffic cars and skaters; NPCs with
// NPCs and skaters; skaters with skaters (the slower one falls).
//

#include "zge/collision/dynamics.h"
#include "aggressive/conversions.h"
#include "aggressive/level/dynamics.h"
#include "aggressive/level/traffic_car.h"
#include "aggressive/skater/skater.h"
#include "aggressive/skater/vectors.h"
#include "aggressive/tuning.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/memory/regions.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/collision/sweep.h"
#include "zge/text/cstring.h"
#include "aggressive/skater/moves.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/sound/events.h"
#include "aggressive/npc.h"
#include "aggressive/level/scene_objects.h"
#include "aggressive/level/object_box.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/dynamics_objects.cpp.h"

namespace aggressive::level { uint32_t createDynamicsObject(uint32_t, ee::Ptr<zge::Node>, uint32_t); }
namespace aggressive::level { void resetDynamicsObject(ee::Ptr<DynamicsRecord>, uint32_t, ee::Float); }

namespace aggressive
{
}
namespace aggressive::skater
{
}

namespace aggressive::level
{
    using skater::copyQuadword;
    using skater::highDoubleword;

    namespace
    {
        // The engine's (zge).
        constexpr uint32_t kHeapAllocate = sym::heapAllocate;          // (bytes) -> a heap block
        constexpr uint32_t kRunDeferredCalls = sym::runContactHandlers;      // (sweep): each overlapping pair's handler called

        // The game's.
        constexpr uint32_t kObjectWoken = sym::objectWoken;           // (record): its frame handler subscribed
        constexpr uint32_t kUnsubscribeFrameHandler = sym::unsubscribeDynamicsContact; // (record): its frame handler unsubscribed, cleared

        // The pair handlers this file has (setupLevelContacts sets them).
        constexpr uint32_t kBodiesTouch = sym::bodiesTouch;
        constexpr uint32_t kCollideNpcWithRigidBody = sym::collideNpcWithRigidBody;
        constexpr uint32_t kCollideCarWithRigidBody = sym::collideCarWithRigidBody;
        constexpr uint32_t kCollideSkaterWithRigidBody = sym::collideSkaterWithRigidBody;
        constexpr uint32_t kBothObjectsShown = sym::bothObjectsShown;
        constexpr uint32_t kNpcMeetsCar = sym::npcMeetsCar;
        constexpr uint32_t kCollideIfBothShown = sym::collideIfBothShown;
        constexpr uint32_t kCarMeetsCar = sym::carMeetsCar;
        constexpr uint32_t kCarMeetsSkater = sym::carMeetsSkater;
        constexpr uint32_t kSkatersCollide = sym::skatersCollide;
        constexpr uint32_t kPushDynamicsObject = sym::pushDynamicsObject;
        constexpr uint32_t kAddDynamicsBody = sym::addDynamicsBody;
        constexpr uint32_t kPlayDynamicsImpactSound = sym::playDynamicsImpactSound;

        // The dynamics objects made (written, never read in this build) and
        // how many; the time since the level's setup, while it is under a
        // second the contacts are not run (the objects settle).
        constexpr uint32_t kDynamicsObjects = sym::dynamicsObjects;       // DynamicsRecord *[]
        inline ee::Global<uint32_t, sym::g_dynamicsObjectCount> g_dynamicsObjectCount;
        inline ee::Global<ee::Float, sym::g_dynamicsSettleTime> g_dynamicsSettleTime;

        // A knock's push: the impulse's height lifted by its strength, and
        // random parts of it and of a spin about each axis.
        inline ee::Global<ee::Float, sym::g_pushLift> g_pushLift;        // 0.7
        inline ee::Global<ee::Float, sym::g_pushSpreadX> g_pushSpreadX;     // 0.2
        inline ee::Global<ee::Float, sym::g_pushSpreadZ> g_pushSpreadZ;     // 0.2
        inline ee::Global<ee::Float, sym::g_pushSpinX> g_pushSpinX;       // 0.3
        inline ee::Global<ee::Float, sym::g_pushSpinZ> g_pushSpinZ;       // 0.3
        inline ee::Global<ee::Float, sym::g_pushSpinY> g_pushSpinY;       // 0.3
        // At rest: moved less than this (squared) and turned less than the
        // quaternion's w below, for half a second.
        inline ee::Global<ee::Float, sym::g_restDistanceSquared> g_restDistanceSquared; // 0.01
        inline ee::Global<ee::Float, sym::g_restTurnCosine> g_restTurnCosine;      // 0.9
        constexpr float kRestSeconds = 0.5f;
        // The movers' masses when they knock a body: an NPC, a car, a skater.
        inline ee::Global<ee::Float, sym::g_npcKnockMass> g_npcKnockMass;    // 750
        inline ee::Global<ee::Float, sym::g_carKnockMass> g_carKnockMass;    // 750
        inline ee::Global<ee::Float, sym::g_skaterKnockMass> g_skaterKnockMass; // 750

        // The face contacts' sums for the part touched last (the average of
        // the faces' normals and centres; nothing in this build reads them).
        // The running sums of a part's faces, and what they start from.
        inline ee::Global<zge::Vec4, sym::contactNormalSum> g_contactNormalSum;
        inline ee::Global<zge::Vec4, sym::contactCentreSum> g_contactCentreSum;
        const zge::Vec4 kEmptySum = {ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
        inline ee::Global<int32_t, sym::g_contactFaceCount> g_contactFaceCount;
        inline ee::Global<ee::Float, sym::g_oneThird> g_oneThird;  // 1 / 3: a face's centre from its corners
        constexpr uint32_t kDynamicsObjectName = sym::dynamicsObjectName;  // "DynamicsObject"

        // The impact sound: the sound sources' event, placed once at the
        // body, at full level.
        constexpr uint32_t kSoundSources = 56;
        constexpr uint32_t kPlaceOnce = 3;
        constexpr uint32_t kFullLevel = 127;
        // The level object class's tag record and bytes the impact sounds
        // come from, and where those numbers start in the source bank.
        constexpr uint32_t kSoundTags = 2;
        constexpr uint32_t kFirstImpactTag = 4;
        constexpr uint32_t kImpactTag = 5;
        constexpr uint32_t kImpactSampleBase = 0x40;

        // A skater run into by a faster one: skaterCrash's kind 6, "KnockedOut".
        constexpr uint32_t kCrashKnockedOut = 6;

        // The box the contact tests compare (bodyBox, npcBox, carBox,
        // collectSkaterBonePoints fill it; 0x30 bytes).
        using zge::OrientedBox;

        // A level NPC as the contacts see it (pickups.cpp's record).
        ee::Ptr<RigidBody> firstBody(ee::Ptr<DynamicsRecord> record)
        {
            return record->dynamics->bodies;
        }

        // The engine's views (zge/collision/dynamics.h) of the level's body and dynamics: the same records.
        ee::Ptr<zge::RigidBody> engineBody(ee::Ptr<RigidBody> body) { return body.cast<zge::RigidBody>(); }
        ee::Ptr<zge::Dynamics> engineDynamics(ee::Ptr<Dynamics> dynamics) { return dynamics.cast<zge::Dynamics>(); }
        ee::Ptr<Dynamics> levelDynamics(ee::Ptr<zge::Dynamics> dynamics) { return dynamics.cast<Dynamics>(); }
        ee::Ptr<zge::SceneObject> sceneObjectAt(uint32_t object) { return ee::Ptr<zge::SceneObject>(object); }
    }

    // setupLevelContacts (the level's load): the level's contact sweep made, no
    // dynamics objects yet, and each pair of kinds given its handler.
    uint32_t setupLevelContacts()
    {
        const ee::Ptr<zge::Sweep> sweep = zge::newCollisionTable();
        *g_levelSweep = sweep;
        *g_dynamicsObjectCount = 0;
        *g_dynamicsSettleTime = 0.0f;
        zge::setCollisionPairHandler(sweep, kSweepRigidBody, kSweepRigidBody, kBodiesTouch);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepRigidBody, kSweepNpc, kCollideNpcWithRigidBody);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepRigidBody, kSweepCar, kCollideCarWithRigidBody);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepRigidBody, kSweepSkater, kCollideSkaterWithRigidBody);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepNpc, kSweepNpc, kBothObjectsShown);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepNpc, kSweepCar, kNpcMeetsCar);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepNpc, kSweepSkater, kCollideIfBothShown);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepCar, kSweepCar, kCarMeetsCar);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepCar, kSweepSkater, kCarMeetsSkater);
        zge::setCollisionPairHandler(*g_levelSweep, kSweepSkater, kSweepSkater, kSkatersCollide);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupLevelContacts, Verified);

    void addDynamicsBody(ee::Ptr<Dynamics> dynamics, uint32_t sceneObject);

    // createDynamicsObject (a DynamicsObject level object's creation): the node's
    // dynamics definition copied (none: 0); the game's record of it, where
    // the object and its body start (to be put back there), its impact
    // sounds from its class's tags; the body put in the sweep, the contact
    // callback set. Returns 1 when made.
    uint32_t createDynamicsObject(uint32_t sceneObject, ee::Ptr<zge::Node> node, uint32_t definitionName)
    {
        const ee::Ptr<Dynamics> dynamics = levelDynamics(zge::cloneDynamicsDefinition(node, definitionName));
        if (!dynamics)
            return 0;
        const ee::Ptr<DynamicsRecord> record(zge::heapAllocate(sizeof(DynamicsRecord)));
        *(ee::Ptr<ee::Ptr<DynamicsRecord>>(kDynamicsObjects) + *g_dynamicsObjectCount) = record;
        record->sceneObject = sceneObject;
        record->dynamics = dynamics;
        record->startPosition = *zge::objectPosition(sceneObjectAt(record->sceneObject));
        record->reserved = 0;
        record->flags = 0;
        const ee::Ptr<zge::TagRecord> tags = zge::levelObjectClassTag(sceneObjectAt(sceneObject));
        record->firstImpactSound = zge::tagByte(tags, kSoundTags, kFirstImpactTag) + kImpactSampleBase;
        record->impactSound = zge::tagByte(tags, kSoundTags, kImpactTag) + kImpactSampleBase;
        record->impactWait = 0.0f;
        record->startPosition = *zge::objectPosition(sceneObjectAt(sceneObject));
        zge::quaternionCopy(&record->startOrientation, zge::objectOrientation(sceneObjectAt(sceneObject)));
        dynamics->bodies->record = record;
        setObjectComponentWord(sceneObject, record);
        addDynamicsBody(dynamics, sceneObject);
        setDynamicsContactCallback(engineDynamics(dynamics));
        const ee::Ptr<RigidBody> body = dynamics->bodies;
        record->bodyStartPosition = body->position;
        zge::quaternionCopy(&record->bodyStartOrientation, &body->orientation);
        *g_dynamicsObjectCount = *g_dynamicsObjectCount + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(createDynamicsObject, Verified);

    namespace
    {
        namespace tail
        {
        void runContactHandlers(ee::Ptr<zge::Sweep> boxSweep) { ee::tailCall(kRunDeferredCalls, boxSweep); }
        }
    }

    // levelObjectsFrame (each frame of a level, `seconds` its time): the step
    // rate the contacts use; for the first second after the setup only the
    // time counts (the objects settle), then the sweep's overlapping pairs
    // get their handlers.
    void levelObjectsFrame(ee::Float seconds)
    {
        const ee::Float settled = *g_dynamicsSettleTime;
        *g_stepRate = ee::Float(1.0f) / seconds;
        if (!(settled < ee::Float(1.0f)))
            return tail::runContactHandlers(*g_levelSweep);
        *g_dynamicsSettleTime = settled + seconds;
    }
    READABLE_FUNCTION_WAITS(levelObjectsFrame, Verified);

    // pushDynamicsObject: a dynamics object knocked by `impulse` (its `strength`
    // given, else the impulse's length): woken, its body moving with the
    // impulse lifted by the strength and a random part more on x and z, a
    // random spin about each axis, its weight as the force on it, awake and
    // not at rest. Returns 1.
    uint32_t pushDynamicsObject(uint32_t sceneObject, ee::Ptr<zge::Vec4> impulse, ee::Ptr<ee::Float> strengthGiven)
    {
        const auto random = [](uint32_t mask) { return ee::toFloat(static_cast<int32_t>(zge::randomNumber() & mask)); };
        const ee::Ptr<DynamicsRecord> record(static_cast<uint32_t>(objectComponentKind(sceneObject)));
        const ee::Float strength = strengthGiven ? *strengthGiven : zge::vec3Length(&impulse->xyz());
        zge::wakeDynamics(sceneObjectAt(record->sceneObject), engineDynamics(record->dynamics), 1);
        const ee::Ptr<RigidBody> body = firstBody(record);
        body->velocity.y = impulse->y - strength * *g_pushLift;
        body->velocity.x = impulse->x;
        body->velocity.x = body->velocity.x + impulse->x * (random(3) * *g_pushSpreadX);
        body->velocity.z = impulse->z;
        body->velocity.z = body->velocity.z + impulse->z * (random(3) * *g_pushSpreadZ);
        body->spin.x = random(7) * *g_pushSpinX + ee::Float(1.0f);
        body->spin.z = random(7) * *g_pushSpinZ + ee::Float(1.0f);
        body->spin.y = random(7) * *g_pushSpinY + ee::Float(1.0f);
        body->force.y = body->mass * tuningValue(kTuningPhysics, 0 /* Gravity */);
        record->flags = kDynamicsAwake;
        record->restTime = 0.0f;
        return 1;
    }
    READABLE_FUNCTION_WAITS(pushDynamicsObject, Verified);

    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record);

    // resetDynamicsObject (updateDynamicsObject's, every frame it is subscribed;
    // `seconds` the frame's): with `reset`, the object and its body put
    // back where they started, woken. Then, awake and moving slower than
    // General / Rigid Body Stop Speed (its speed and spin squared): where it
    // is kept the first time; half a second without moving or turning from
    // there puts it to sleep (a move or a turn starts the half second
    // again). Asleep, its frame handler goes.
    void resetDynamicsObject(ee::Ptr<DynamicsRecord> record, uint32_t reset, ee::Float seconds)
    {
        ee::Local<zge::Vec4> offset;
        ee::Local<zge::Quaternion> turn;
        if (reset != 0)
        {
            record->flags = 0;
            record->restTime = 0.0f;
            zge::setObjectPosition(sceneObjectAt(record->sceneObject), &record->startPosition);
            zge::setObjectOrientation(sceneObjectAt(record->sceneObject), &record->startOrientation);
            zge::updatePose(sceneObjectAt(record->sceneObject));
            zge::wakeDynamics(sceneObjectAt(record->sceneObject), engineDynamics(record->dynamics), 1);
            firstBody(record)->position = record->bodyStartPosition;
            zge::quaternionCopy(&firstBody(record)->orientation, &record->bodyStartOrientation);
            zge::quaternionToMatrix(&firstBody(record)->rotation, &record->bodyStartOrientation);
            zge::sweepUpdateBody(*g_levelSweep, engineBody(firstBody(record)));
        }
        if (record->flags & kDynamicsAwake)
        {
            const ee::Ptr<RigidBody> body = firstBody(record);
            const ee::Float speedSquared = zge::squaredLength(body->velocity);
            const ee::Float spinSquared = zge::squaredLength(body->spin);
            if (speedSquared + spinSquared < tuningValue(kTuningGeneral, 4 /* Rigid Body Stop Speed */))
            {
                const ee::Float restTime = record->restTime;
                bool moved = false;
                if (restTime == ee::Float(0.0f))
                {
                    // Nearly still: where it is and how it is turned kept.
                    record->restPosition = body->position;
                    zge::quaternionInverse(&record->restInverse, &firstBody(record)->orientation);
                }
                else if (restTime < ee::Float(kRestSeconds))
                {
                    *offset = record->restPosition - body->position;
                    bool still = false;
                    if (zge::squaredLength(*offset) < *g_restDistanceSquared)
                    {
                        // The turn since then: restInverse * orientation.
                        zge::quaternionMultiply(turn, &record->restInverse, &firstBody(record)->orientation);
                        still = !(turn->w < *g_restTurnCosine);
                    }
                    moved = !still;
                }
                else
                {
                    // Half a second still: asleep.
                    record->restTime = 0.0f;
                    record->flags = 0;
                }
                if (moved)
                    record->restTime = 0.0f;
                else
                    record->restTime = record->restTime + seconds;
            }
        }
        if (record->frameHandler != 0 && !(record->flags & kDynamicsAwake))
            unsubscribeDynamicsContact(record);
    }
    READABLE_FUNCTION_WAITS(resetDynamicsObject, Verified);

    // addDynamicsBody (createDynamicsObject's): a box body woken and put in
    // the level's sweep as a body, its box the radius of its half extents
    // about its position; its dynamics' restitution 0.5.
    void addDynamicsBody(ee::Ptr<Dynamics> dynamics, uint32_t sceneObject)
    {
        ee::Local<zge::Vec4> low;
        ee::Local<zge::Vec4> high;
        const ee::Ptr<RigidBody> body = dynamics->bodies;
        if (body->shape != 0)
            return;
        const ee::Float radius = zge::vec3Length(body->halfExtents);
        zge::wakeDynamics(sceneObjectAt(sceneObject), engineDynamics(dynamics), 1);
        const ee::Ptr<zge::Sweep> sweep = *g_levelSweep;
        low->x = body->position.x - radius;
        low->y = body->position.y - radius;
        low->z = body->position.z - radius;
        high->x = body->position.x + radius;
        high->y = body->position.y + radius;
        high->z = body->position.z + radius;
        body->sweepEntry = zge::sweepAddBody(sweep, kSweepRigidBody, body, &low->x, &high->x);
        zge::setDynamicsRestitution(engineDynamics(dynamics), ee::Float(0.5f));
    }
    READABLE_FUNCTION_WAITS(addDynamicsBody, Verified);

    // dynamicsObjectContact (a dynamics object's contact callback, for each face its
    // body touches): the faces of one part are summed (normals, centres)
    // and counted; a new part starts the sums again. Returns 0 (the face
    // ignored) for a part of another DynamicsObject (the sweep's pairs
    // handle those), else 1. The part before is the contact object the engine
    // showed this callback last time (it keeps the same record for it).
    uint32_t dynamicsObjectContact(ee::Ptr<Dynamics> dynamics, uint32_t, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> centre;
        const ee::Ptr<zge::HitNode> lastPart = zge::dynamicsContactObject();
        if (lastPart != contact->object && lastPart)
        {
            const ee::Ptr<zge::SceneObject> lastOwner = zge::nodeSceneObject(lastPart->node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
            const int32_t faces = *g_contactFaceCount;
            if (!lastOwner || faces == 0)
                return 1;
            // The last part's centre averaged, then both sums start again.
            *g_contactCentreSum = *g_contactCentreSum * (ee::Float(1.0f) / ee::toFloat(faces));
            *g_contactFaceCount = 0;
            *g_contactNormalSum = kEmptySum;
            *g_contactCentreSum = kEmptySum;
        }
        if (!contact->object)
            return 1;
        const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(contact->object->node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
        if (!owner)
            return 1;
        aggressive::skater::playerIndexOfNode(ee::Ptr<zge::SceneObject>(dynamics->sceneObject)); // the dynamics' word
        *g_contactNormalSum = *g_contactNormalSum + contact->normal;
        *centre = contact->vertices[0];
        *centre = *centre + contact->vertices[1];
        *centre = *centre + contact->vertices[2];
        *centre = *centre * *g_oneThird;
        *g_contactCentreSum = *g_contactCentreSum + *centre;
        *g_contactFaceCount = *g_contactFaceCount + 1;
        const uint32_t ownerType = zge::levelObjectTypeHash(owner);
        const uint32_t dynamicsObject = zge::nameHash(ee::Ptr<const char>(kDynamicsObjectName));
        return ownerType != dynamicsObject ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(dynamicsObjectContact, Verified);

    void playDynamicsImpactSound(ee::Ptr<RigidBody> body);

    // collideNpcWithRigidBody (the sweep's body and NPC pair): an NPC shown and
    // overlapping a body knocks it by its velocity along its path.
    void collideNpcWithRigidBody(ee::Ptr<RigidBody> body, ee::Ptr<Npc> npc)
    {
        ee::Local<OrientedBox> npcBox;
        ee::Local<OrientedBox> bodyBox;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> secondAxes;
        ee::Local<zge::Vec4> velocity;
        ee::Local<ee::Float> depth;
        const uint32_t node = objectNode(npc->object);
        if (zge::nodeShown(ee::Ptr<zge::Node>(node)) == 0)
            return;
        zge::memoryFill(npcBox, 0, static_cast<uint32_t>(sizeof(OrientedBox)));
        zge::memoryFill(bodyBox, 0, static_cast<uint32_t>(sizeof(OrientedBox)));
        if (zge::bodyBox(engineBody(body), bodyBox) == 0)
            return;
        if (aggressive::npcBox(npc, npcBox) == 0)
            return;
        if (zge::orientedBoxesOverlap(npcBox, bodyBox) != 0)
        {
            const ee::Ptr<DynamicsRecord> record = body->record;
            *depth = ee::Float(0.0f);
            aggressive::npcVelocity(npc, velocity);
            zge::orientedBoxContact(npcBox, bodyBox, point, secondAxes, depth);
            if (!(record->flags & kDynamicsAwake))
            {
                pushDynamicsObject(record->dynamics->sceneObject, velocity, 0);
                objectWoken(record);
            }
            else
                zge::pushBodyFromPoint(npcBox, engineBody(body), velocity, point, *g_npcKnockMass);
            playDynamicsImpactSound(body);
        }
    }
    READABLE_FUNCTION_WAITS(collideNpcWithRigidBody, Verified);

    // collideCarWithRigidBody (the sweep's body and car pair): a traffic car
    // overlapping a body knocks it by its velocity.
    void collideCarWithRigidBody(ee::Ptr<RigidBody> body, ee::Ptr<TrafficCar> car)
    {
        ee::Local<OrientedBox> carBox;
        ee::Local<OrientedBox> boxOfBody;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> wakingVelocity;
        ee::Local<zge::Vec4> pushingVelocity;
        ee::Local<ee::Float> depth;
        zge::memoryFill(carBox, 0, sizeof(OrientedBox));
        zge::memoryFill(boxOfBody, 0, sizeof(OrientedBox));
        if (zge::bodyBox(engineBody(body), boxOfBody) == 0 || aggressive::trafficCarBox(car, carBox) == 0 || zge::orientedBoxesOverlap(carBox, boxOfBody) == 0)
            return;
        const ee::Ptr<DynamicsRecord> record = body->record;
        *depth = 0.0f;
        zge::orientedBoxContact(carBox, boxOfBody, point, normal, depth);
        if (!(record->flags & kDynamicsAwake))
        {
            aggressive::skater::trafficCarVelocity(car, wakingVelocity);
            pushDynamicsObject(record->dynamics->sceneObject, wakingVelocity, 0);
            objectWoken(record);
        }
        else
        {
            aggressive::skater::trafficCarVelocity(car, pushingVelocity);
            zge::pushBodyFromPoint(carBox, engineBody(body), pushingVelocity, point, *g_carKnockMass);
        }
        playDynamicsImpactSound(body);
    }
    READABLE_FUNCTION_WAITS(collideCarWithRigidBody, Verified);

    // collideSkaterWithRigidBody (the sweep's body and skater pair): a skater
    // overlapping a body knocks it by the skater's velocity, as hard as the
    // skater's speed.
    void collideSkaterWithRigidBody(ee::Ptr<RigidBody> body, ee::Ptr<skater::Player> player)
    {
        ee::Local<OrientedBox> skaterBox;
        ee::Local<OrientedBox> boxOfBody;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> normal;
        ee::Local<ee::Float> depth;
        zge::memoryFill(skaterBox, 0, sizeof(OrientedBox));
        zge::memoryFill(boxOfBody, 0, sizeof(OrientedBox));
        if (zge::bodyBox(engineBody(body), boxOfBody) == 0 || aggressive::skater::skaterOrientedBox(player, skaterBox) == 0 || zge::orientedBoxesOverlap(skaterBox, boxOfBody) == 0)
            return;
        const ee::Ptr<DynamicsRecord> record = body->record;
        *depth = 0.0f;
        zge::orientedBoxContact(skaterBox, boxOfBody, point, normal, depth);
        if (!(record->flags & kDynamicsAwake))
        {
            pushDynamicsObject(record->dynamics->sceneObject, &player->velocity, &player->speed);
            objectWoken(record);
        }
        else
            zge::pushBodyFromPoint(skaterBox, engineBody(body), &player->velocity, point, *g_skaterKnockMass);
        playDynamicsImpactSound(body);
    }
    READABLE_FUNCTION_WAITS(collideSkaterWithRigidBody, Verified);

    // bothObjectsShown (the sweep's NPC pair): nothing but whether both are
    // shown (the result goes nowhere).
    uint32_t bothObjectsShown(ee::Ptr<Npc> a, ee::Ptr<Npc> b)
    {
        const uint32_t nodeA = objectNode(a->object);
        const uint32_t shown = zge::nodeShown(ee::Ptr<zge::Node>(nodeA));
        if (shown != 0)
        {
            const uint32_t nodeB = objectNode(b->object);
            return game::tail(zge::nodeShown, ee::Ptr<zge::Node>(nodeB));
        }
        return shown;
    }
    READABLE_FUNCTION(bothObjectsShown, Verified);

    // npcMeetsCar (the sweep's NPC and car pair): nothing.
    void npcMeetsCar()
    {
    }
    READABLE_FUNCTION(npcMeetsCar, Verified);

    // collideIfBothShown (the sweep's NPC and skater pair): the NPC's contact
    // with the skater, while both are shown.
    void collideIfBothShown(ee::Ptr<Npc> npc, ee::Ptr<skater::Player> player)
    {
        const uint32_t npcNode = objectNode(npc->object);
        if (zge::nodeShown(ee::Ptr<zge::Node>(npcNode)) != 0)
        {
            const uint32_t skaterNode = objectNode(player->node);
            if (zge::nodeShown(ee::Ptr<zge::Node>(skaterNode)) != 0)
                game::tail(aggressive::npcSkaterCollision, npc, player);
        }
    }
    READABLE_FUNCTION_WAITS(collideIfBothShown, Verified);

    // carMeetsCar (the sweep's car pair): nothing.
    void carMeetsCar()
    {
    }
    READABLE_FUNCTION(carMeetsCar, Verified);

    // carMeetsSkater (the sweep's car and skater pair): nothing (the cars
    // knock skaters down elsewhere: skater/move_collide.cpp).
    void carMeetsSkater()
    {
    }
    READABLE_FUNCTION(carMeetsSkater, Verified);

    // skatersCollide (the sweep's skater pair): two skaters' boxes
    // overlapping: the slower one is knocked out, unless the faster one is
    // down.
    void skatersCollide(ee::Ptr<skater::Player> first, ee::Ptr<skater::Player> second)
    {
        ee::Local<OrientedBox[2]> boxes;
        ee::Local<ee::Ptr<skater::Player>[2]> players;
        players[0] = first;
        players[1] = second;
        zge::memoryFill(boxes, 0, 2 * sizeof(OrientedBox));
        for (int i = 0; i < 2; ++i)
            if (aggressive::skater::skaterOrientedBox(players[i], &boxes[i]) == 0)
                return;
        if (zge::orientedBoxesOverlap(&boxes[0], &boxes[1]) == 0)
            return;

        const ee::Ptr<skater::Player> a(players[0]);
        const ee::Ptr<skater::Player> b(players[1]);
        const bool aIsFaster = b->speed < a->speed;
        const ee::Ptr<skater::Player> faster = aIsFaster ? a : b;
        const ee::Ptr<skater::Player> slower = aIsFaster ? b : a;
        if (!(faster->flags & skater::kDown))
            skater::skaterCrash(slower, kCrashKnockedOut);
    }
    READABLE_FUNCTION_WAITS(skatersCollide, Verified);

    // playDynamicsImpactSound: a knocked object's impact sound at its body, once until
    // its wait runs out (a second): the first knock's sample, then the
    // later knocks' one.
    void playDynamicsImpactSound(ee::Ptr<RigidBody> body)
    {
        const ee::Ptr<DynamicsRecord> record = body->record;
        if (!(record->impactWait == ee::Float(0.0f)))
            return;
        const uint32_t flags = record->flags;
        uint32_t sample;
        if (flags & kDynamicsHitBefore)
            sample = record->impactSound;
        else
        {
            sample = record->firstImpactSound;
            record->flags = flags | kDynamicsHitBefore;
        }
        soundEvent(kSoundSources, &body->position, kPlaceOnce, sample, kFullLevel);
        record->impactWait = 1.0f;
    }
    READABLE_FUNCTION_WAITS(playDynamicsImpactSound, Verified);

    namespace
    {
        constexpr uint32_t kForEachObject = sym::forEachObject;     // (visit(object, context), context)
        constexpr uint32_t kDynamicsObjectHash = sym::dynamicsObjectHash;
        constexpr uint32_t kCollide = sym::g_textCollide;           // "Collide"
        constexpr uint32_t kDynamicsObject = sym::g_textDynamicsObject;    // "DynamicsObject"
        constexpr uint32_t kMessageUpdate = 0x7000, kMessageDestroy = 28673;
        constexpr uint32_t kDestroyDynamicsComponent = sym::destroyDynamicsComponent, kDynamicsMessageTrue = sym::dynamicsMessageTrue;
        constexpr uint32_t kUpdateDynamicsObject = sym::updateDynamicsObject;
        constexpr uint32_t kHideDynamicsObject = sym::hideDynamicsObject, kShowDynamicsObject = sym::showDynamicsObject;
        inline ee::Global<int32_t, sym::g_dynamicsObjectHashPending> g_dynamicsObjectHashPending;
        inline ee::Global<int32_t, sym::g_dynamicsObjectHash> g_dynamicsObjectHash;
    }

    // destroyDynamicsComponent: the object's end: out of the sweep, its dynamics and
    // record freed.
    int32_t destroyDynamicsComponent(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<DynamicsRecord> record(static_cast<uint32_t>(objectComponentKind(message->target)));
        const ee::Ptr<Dynamics> dynamics = record->dynamics;
        zge::removeSweepBody(levelSweep(), dynamics->bodies->sweepEntry);
        zge::freeDynamicsRecord(engineDynamics(dynamics));
        zge::heapFree(record);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyDynamicsComponent, Verified);

    // initDynamicsLevelObject: a level object with dynamics: its geometry shown but not
    // collided with (the body answers instead).
    bool initDynamicsLevelObject(uint32_t object)
    {
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(sceneObjectAt(object));
        zge::setNodeShown(node, 1, 0);
        zge::setNodeCollides(node, 0, 0);
        // Its dynamics definition is the one named as its class (the node's model) is.
        if (createDynamicsObject(object, node, zge::objectClass(node)->nameHash) != 1)
            return false;
        zge::subscribe(object, kMessageDestroy, kDestroyDynamicsComponent);
        zge::subscribe(object, zge::nameHash(ee::Ptr<const char>(kCollide)), kDynamicsMessageTrue);
        return true;
    }
    READABLE_FUNCTION_WAITS(initDynamicsLevelObject, Verified);

    int32_t dynamicsMessageTrue() { return 1; }
    READABLE_FUNCTION(dynamicsMessageTrue, Verified);

    // dynamicsObjectHash: "DynamicsObject"'s hash, once.
    int32_t dynamicsObjectHash()
    {
        if (*g_dynamicsObjectHashPending != 0)
        {
            *g_dynamicsObjectHash = zge::nameHash(ee::Ptr<const char>(kDynamicsObject));
            *g_dynamicsObjectHashPending = 0;
        }
        return *g_dynamicsObjectHash;
    }
    READABLE_FUNCTION(dynamicsObjectHash, Verified);

    namespace
    {
        // Shows or hides a level object if it is a dynamics object: the
        // fly-throughs visit every object with this.
        void setDynamicsVisible(uint32_t object, uint32_t visible)
        {
            const uint32_t dynamicsHash = dynamicsObjectHash();
            if (dynamicsHash == zge::levelObjectTypeHash(sceneObjectAt(object)))
                zge::setSceneObjectShown(sceneObjectAt(object), visible);
        }
    }

    int32_t hideDynamicsObject(uint32_t object)
    {
        setDynamicsVisible(object, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(hideDynamicsObject, Verified);

    int32_t showDynamicsObject(uint32_t object)
    {
        setDynamicsVisible(object, 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(showDynamicsObject, Verified);

    // updateDynamicsObject: a frame of an object awake: the wait after an impact runs
    // down, the dynamics step while it moves, then the node follows.
    int32_t updateDynamicsObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ee::Float> seconds(message->data);
        const ee::Ptr<DynamicsRecord> record(static_cast<uint32_t>(objectComponentKind(message->target)));
        const ee::Ptr<Dynamics> dynamics = record->dynamics;
        const ee::Float wait = record->impactWait;
        if (ee::Float(0.0f) < wait)
        {
            const ee::Float left = wait - *seconds;
            record->impactWait = left;
            if (left < ee::Float(0.0f))
                record->impactWait = ee::Float(0.0f);
        }
        if (record->flags & kDynamicsAwake)
            zge::updateDynamics(engineDynamics(dynamics), levelSweep(), *seconds);
        resetDynamicsObject(record, 0, *seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateDynamicsObject, Verified);

    // objectWoken: woken by a push: the frame's update subscribed.
    void objectWoken(ee::Ptr<DynamicsRecord> record)
    {
        zge::subscribe(record->sceneObject, kMessageUpdate, kUpdateDynamicsObject);
        record->frameHandler = kUpdateDynamicsObject;
    }
    READABLE_FUNCTION_WAITS(objectWoken, Verified);

    // unsubscribeDynamicsContact: asleep: the update it subscribed to is dropped.
    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record)
    {
        zge::unsubscribe(record->sceneObject, kMessageUpdate, record->frameHandler);
        record->frameHandler = 0;
    }
    READABLE_FUNCTION_WAITS(unsubscribeDynamicsContact, Verified);

    namespace
    {
        namespace tail
        {
        int32_t forEachObject(uint32_t visit, uint32_t data) { return ee::tailCall<int32_t>(kForEachObject, visit, data); }
        }
    }

    // hideDynamicsObjects, showDynamicsObjects: over a fly-through.
    int32_t hideDynamicsObjects()
    {
        return tail::forEachObject(kHideDynamicsObject, 0);
    }
    READABLE_FUNCTION_WAITS(hideDynamicsObjects, Verified);

    int32_t showDynamicsObjects()
    {
        return tail::forEachObject(kShowDynamicsObject, 0);
    }
    READABLE_FUNCTION_WAITS(showDynamicsObjects, Verified);
}
