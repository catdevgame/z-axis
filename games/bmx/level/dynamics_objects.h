#pragma once
// BMX XXX's level dynamics objects (dynamics_objects.cpp): a level object the
// engine's rigid-body dynamics move, and its record. Aggressive Inline's line
// (games/aggressive/level/dynamics_objects.cpp, dynamics.h) with BMX XXX's
// record: every field is at the offset BMX XXX's own code reads.

#include "ee/ee.h"
#include "zge/collision/dynamics.h"
#include "zge/math/types.h"
#include "zge/scene/message.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/dynamics_objects.h.h"

namespace game::bmx::rider
{
    struct Rider;
}

namespace game::bmx
{
    // The game's record of a dynamics object (0x70 bytes from the heap,
    // createDynamicsObject; the object's component word holds it, and its
    // body's record word).
    struct DynamicsRecord
    {
        uint8_t _00[0x08];
        uint32_t sceneObject;             // +0x08 the scene object it moves
        ee::Ptr<zge::Dynamics> dynamics;  // +0x0C
        // +0x10 how far its body is from the nearest rider, squared (by 25
        // with two or more players): resetDynamicsObject works it out each
        // call and puts the object back past the reset distance.
        ee::Float riderDistanceSquared;
        uint8_t _14[0x0C];
        zge::Vec4 startPosition;          // +0x20 where the level puts it (resetDynamicsObject puts it back)
        zge::Quaternion startOrientation; // +0x30
        zge::Vec4 bodyStartPosition;      // +0x40 its body's, at the creation
        zge::Quaternion bodyStartOrientation; // +0x50
        // +0x60 kDynamicsAwake, and from kDynamicsStillShift up the calls in a
        // row its body was nearly still.
        uint32_t flags;
        uint8_t _64[4];
        zge::MessageHandler frameHandler; // +0x68 (AI +0xA0) the frame update it subscribed to; 0 asleep
        uint8_t _6c[4];
    };
    static_assert(offsetof(DynamicsRecord, sceneObject) == 0x08 && offsetof(DynamicsRecord, dynamics) == 0x0C);
    static_assert(offsetof(DynamicsRecord, riderDistanceSquared) == 0x10 && offsetof(DynamicsRecord, startPosition) == 0x20);
    static_assert(offsetof(DynamicsRecord, startOrientation) == 0x30 && offsetof(DynamicsRecord, bodyStartPosition) == 0x40);
    static_assert(offsetof(DynamicsRecord, bodyStartOrientation) == 0x50 && offsetof(DynamicsRecord, flags) == 0x60);
    static_assert(offsetof(DynamicsRecord, frameHandler) == 0x68 && sizeof(DynamicsRecord) == 0x70);

    constexpr uint32_t kDynamicsAwake = 0x1; // DynamicsRecord::flags: it moves (its body is simulated)
    constexpr uint32_t kDynamicsStillShift = 2;
    constexpr uint32_t kDynamicsStillCalls = 0x10; // that many nearly still calls put it to sleep

    // The level's box sweep (the engine's sweep and prune of the objects'
    // collision boxes): the level's set-up makes it (level_collision.cpp).
    inline ee::Global<uint32_t, sym::g_levelSweep> g_levelSweep;
    // How many dynamics objects the level has made (its set-up clears it,
    // each object made adds one), and their records in the order made.
    inline ee::Global<uint32_t, sym::g_dynamicsObjectCount> g_dynamicsObjectCount;
    inline ee::Ptr<ee::Ptr<DynamicsRecord>> dynamicsRecords()
    {
        return ee::Ptr<ee::Ptr<DynamicsRecord>>(sym::g_dynamicsRecords);
    }

    // The sweep's kinds (AI's SweepKind): a dynamics object's body is 0.
    constexpr uint32_t kSweepRigidBody = 0;

    // A dynamics object made for a level object: 1, or 0 when the node has
    // no dynamics definition of that name.
    uint32_t createDynamicsObject(ee::Ptr<zge::SceneObject> object, uint64_t node, uint32_t definitionName);
    // A box body woken and put in the level's sweep.
    void addDynamicsBody(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::SceneObject> object);
    // The object put back where it started when asked or when no rider is
    // near; an awake one that stays nearly still put to sleep.
    void resetDynamicsObject(ee::Ptr<DynamicsRecord> record, uint32_t reset);
    // (dynamics_contacts.cpp) A dynamics object knocked by a rider: woken and moving with the impulse; 1.
    uint32_t pushDynamicsObject(ee::Ptr<rider::Rider> rider, ee::Address object, ee::Ptr<zge::Vec4> impulse, ee::Ptr<ee::Float> strengthGiven);
    // The level sweep's handler for a rider's body touching a dynamics object's.
    void bodiesTouch(ee::Ptr<zge::RigidBody> a, ee::Ptr<zge::RigidBody> b);
    // Woken by a push: the frame's update subscribed.
    void objectWoken(ee::Ptr<DynamicsRecord> record);
    // Asleep: the update it subscribed to is dropped.
    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record);
}
