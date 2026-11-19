#pragma once

// The level's physics objects (gamehand12): the level objects of type
// "DynamicsObject" that move as rigid bodies when something knocks them
// (each has a dynamics definition in the level's dynamics file, ZDYN /
// DDEF / DBDY), the engine's rigid bodies under them (zge's dynamics: only
// what the game touches), the game's record of each, and the level's
// contact sweep (the engine's collision table: boxes on three axes, a
// handler for each pair of kinds that overlap).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/dynamics.h.h"

namespace zge
{
    struct Sweep;
    struct SweepBox;
}

namespace aggressive::level
{
    struct DynamicsRecord;

    // The engine's rigid body (0x3E0 bytes: a dynamics definition's body
    // copied for the object, cloneDynamicsDefinition; a list through next).
    struct RigidBody
    {
        // +0x000 its shape: 0 a box (the only one the game's contact tests
        // and the sweep take: bodyBox and addDynamicsBody pass over others).
        uint32_t shape;
        uint32_t _004;
        ee::Float mass;                 // +0x008
        uint8_t _00c[0xD4];
        zge::Vec4 position;             // +0x0E0
        zge::Vec4 velocity;             // +0x0F0
        zge::Quaternion orientation;    // +0x100
        uint8_t _110[0x50];
        zge::Matrix rotation;           // +0x160 the orientation's matrix
        zge::Vec4 spin;                 // +0x1A0 its angular velocity
        uint8_t _1b0[0xF0];
        // +0x2A0 the force on it: (0, gravity * mass, 0) while it is awake
        // (bodiesTouch and pushDynamicsObject set it when they wake it).
        zge::Vec4 force;
        uint8_t _2b0[0x4C];
        ee::Ptr<zge::SweepBox> sweepEntry; // +0x2FC its record in the level's contact sweep (sweepAddBody's)
        uint8_t _300[0xD4];
        // +0x3D4 its box's half extents (their length is the radius it is
        // put in the sweep with).
        ee::Ptr<zge::Vec3> halfExtents;
        ee::Ptr<RigidBody> next;        // +0x3D8 the next body of its dynamics
        ee::Ptr<DynamicsRecord> record; // +0x3DC the game's record of its object
    };
    static_assert(offsetof(RigidBody, mass) == 0x008 && offsetof(RigidBody, position) == 0x0E0);
    static_assert(offsetof(RigidBody, velocity) == 0x0F0 && offsetof(RigidBody, orientation) == 0x100);
    static_assert(offsetof(RigidBody, rotation) == 0x160 && offsetof(RigidBody, spin) == 0x1A0);
    static_assert(offsetof(RigidBody, force) == 0x2A0 && offsetof(RigidBody, sweepEntry) == 0x2FC);
    static_assert(offsetof(RigidBody, halfExtents) == 0x3D4 && offsetof(RigidBody, next) == 0x3D8);
    static_assert(offsetof(RigidBody, record) == 0x3DC && sizeof(RigidBody) == 0x3E0);

    // The engine's dynamics of one object (0x40 bytes: its definition's
    // record copied, cloneDynamicsDefinition).
    struct Dynamics
    {
        uint32_t _00;
        // +0x04 the name hash its definition is found by; the copy's, its
        // node's (nodeNameHash).
        uint32_t nameHash;
        // +0x08 how much of a contact's speed it keeps (updateDynamics hands
        // it to resolveBodyContacts at g_contactRestitution); the game's dynamics objects
        // are given 0.5 (addDynamicsBody).
        ee::Float restitution;
        ee::Float _0c;                  // +0x0C updateDynamics' g_contactDamping beside it
        ee::Ptr<RigidBody> bodies;      // +0x10 its first body
        uint8_t _14[0x20];
        // +0x34 the contact callback (body, ?, contact) -> 0 to ignore a
        // face contact (setDynamicsContactCallback's)
        uint32_t contactCallback;
        uint32_t _38;
        uint32_t sceneObject;           // +0x3C the scene object it moves (wakeDynamics sets it)
    };
    static_assert(offsetof(Dynamics, restitution) == 0x08 && offsetof(Dynamics, bodies) == 0x10 && offsetof(Dynamics, contactCallback) == 0x34);
    static_assert(offsetof(Dynamics, sceneObject) == 0x3C && sizeof(Dynamics) == 0x40);

    // The game's record of a dynamics object (0xB0 bytes from the heap,
    // createDynamicsObject; the object's component word holds it).
    struct DynamicsRecord
    {
        uint8_t _00[0x08];
        uint32_t sceneObject;           // +0x08
        ee::Ptr<Dynamics> dynamics;     // +0x0C
        uint32_t reserved;              // +0x10 cleared at the creation; nothing in this build reads it [inferred]
        uint8_t _14[0x0C];
        zge::Vec4 startPosition;        // +0x20 where the level puts it (resetDynamicsObject puts it back)
        zge::Quaternion startOrientation; // +0x30
        zge::Vec4 bodyStartPosition;    // +0x40 its body's, at the creation
        zge::Quaternion bodyStartOrientation; // +0x50
        // +0x60 how long it has been nearly still (resetDynamicsObject: half
        // a second of it puts it to sleep).
        ee::Float restTime;
        uint8_t _64[0x0C];
        zge::Vec4 restPosition;         // +0x70 its body's position when it came nearly to rest
        zge::Quaternion restInverse;    // +0x80 the inverse of its body's orientation then
        uint32_t flags;                 // +0x90 kDynamicsAwake, kDynamicsHitBefore
        uint32_t firstImpactSound;      // +0x94 the sound source bank's sample of its first knock
        uint32_t impactSound;           // +0x98 ... of the knocks after it
        ee::Float impactWait;           // +0x9C seconds until it sounds again (updateDynamicsObject counts it down)
        // +0xA0 the frame handler subscribed to its scene object while it
        // moves (updateDynamicsObject, objectWoken; 0: none).
        uint32_t frameHandler;
        uint8_t _a4[0x0C];
    };
    static_assert(offsetof(DynamicsRecord, sceneObject) == 0x08 && offsetof(DynamicsRecord, dynamics) == 0x0C);
    static_assert(offsetof(DynamicsRecord, startPosition) == 0x20 && offsetof(DynamicsRecord, bodyStartPosition) == 0x40);
    static_assert(offsetof(DynamicsRecord, restTime) == 0x60 && offsetof(DynamicsRecord, restPosition) == 0x70);
    static_assert(offsetof(DynamicsRecord, restInverse) == 0x80 && offsetof(DynamicsRecord, flags) == 0x90);
    static_assert(offsetof(DynamicsRecord, impactWait) == 0x9C && offsetof(DynamicsRecord, frameHandler) == 0xA0);
    static_assert(sizeof(DynamicsRecord) == 0xB0);

    constexpr uint32_t kDynamicsAwake = 0x1;     // DynamicsRecord::flags: it moves (its body is simulated)
    constexpr uint32_t kDynamicsHitBefore = 0x2; // ... its first knock has sounded

    // The kinds of the level's contact sweep (sweepAddBody's second
    // argument): each pair's handler is set at the level's setup
    // (setupLevelContacts).
    enum SweepKind : uint32_t
    {
        kSweepRigidBody = 0, // a dynamics object's body
        kSweepNpc = 2,       // a level NPC (the pickups' records)
        kSweepCar = 3,       // a traffic car
        kSweepSkater = 4,    // a player
    };

    // The level's contact sweep (newCollisionTable's, made at the level's
    // setup) and the step rate the contacts use (1 / the frame's seconds).
    inline ee::Global<ee::Ptr<zge::Sweep>, sym::g_levelSweep> g_levelSweep;
    inline ee::Global<ee::Float, sym::g_stepRate> g_stepRate;

    // The first word of a scene object's component, where a dynamics object
    // keeps its record (object_components.cpp).
    int32_t objectComponentKind(uint32_t sceneObject);
    int32_t setObjectComponentWord(uint32_t sceneObject, ee::Address value);
}

namespace aggressive::level
{
    // (dynamics_objects.cpp)
    int32_t hideDynamicsObjects();
    void levelObjectsFrame(ee::Float seconds);
    void objectWoken(ee::Ptr<DynamicsRecord> record);
    uint32_t setupLevelContacts();
    int32_t showDynamicsObjects();
}
