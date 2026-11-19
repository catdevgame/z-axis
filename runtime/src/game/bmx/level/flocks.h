#pragma once
// BMX XXX's "FlockObj" and "BirdObj" level object types: what each keeps in
// its own tag data, and the functions the two call of each other
// (flocks.cpp, birds.cpp). BMX XXX's own code (its game block); no other
// build names its functions or fields, so the names are from what the code
// does with each.
//
// Each type's update copies the object's tag data out into one working
// record of the game's (g_flockState, g_birdState), works on that, and
// copies it back: a record here is laid out as the tag data is, and a
// field's offset is its place in both.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/flocks.h.h"

namespace game::bmx
{
    // The tag accessors' second argument (the value's kind).
    constexpr uint32_t kFlockTagByte = 2, kFlockTagWord = 8, kFlockTagVector = 0x40;

    enum FlockState : uint8_t
    {
        kFlockResting = 0,   // where it rests, until a rider comes near
        kFlockReturning = 1, // its flight run backwards, down to where it rests
        kFlockFlying = 2,    // circling up from where it rests
    };

    // A flock: an object that is not drawn, which its birds follow.
    struct FlockRecord
    {
        uint8_t _00[4];
        // +0x04 not 0: the update leaves the flock as it is (it neither
        // rises, flies nor returns). Only the level's data sets it.
        uint8_t frozen;
        uint8_t state;        // +0x05 FlockState
        uint8_t _06[2];
        // +0x08 copied out and back with the rest; read nowhere (PS2,
        // GameCube).
        ee::Float unusedFloat;
        ee::Float climbRate;    // +0x0C how far up it is for each second of flightTime
        ee::Float circleRate;   // +0x10 degrees a second round its circle
        ee::Float circleRadius; // +0x14
        ee::Float bobRate;      // +0x18 degrees a second of its up and down swing
        ee::Float bobHeight;    // +0x1C
        ee::Ptr<zge::SceneObject> firstBird; // +0x20 the last bird that joined; each bird has the next
        ee::Float flightTime;   // +0x24 seconds: grows while it flies, falls while it returns
        uint32_t birdCount;     // +0x28 (in the tag data only: the working record's word is never set)
        zge::Vec3 restPosition; // +0x2C
        // +0x38 where the flock last came to rest: its place at its init
        // (initFlock) and its rest when it has returned. +0x44 cleared as it
        // comes back to rest. Nothing reads either (PS2, GameCube).
        zge::Vec3 restedAt;
        zge::Vec3 unusedVector;
        zge::Vec3 birdPositionSum; // +0x50 its birds' positions, added up at its last update
        zge::Vec3 birdVelocitySum; // +0x5C their velocities added up (no longer than 8 once it has left its rest)
    };
    static_assert(offsetof(FlockRecord, frozen) == 4 && offsetof(FlockRecord, state) == 5 && offsetof(FlockRecord, firstBird) == 0x20);
    static_assert(offsetof(FlockRecord, unusedFloat) == 8 && offsetof(FlockRecord, restedAt) == 0x38 && offsetof(FlockRecord, unusedVector) == 0x44);
    static_assert(offsetof(FlockRecord, restPosition) == 0x2C && offsetof(FlockRecord, birdPositionSum) == 0x50);
    static_assert(sizeof(FlockRecord) == 0x68);

    enum BirdState : uint8_t
    {
        kBirdPerched = 0,   // at its home, still
        kBirdWary = 1,      // its flock has left: it flies once a rider is close to it
        kBirdFlying = 2,    // with its flock
        kBirdReturning = 3, // back to its home
    };

    // A bird of a flock.
    struct BirdRecord
    {
        uint8_t _00[0x48];
        uint8_t state;            // +0x48 BirdState
        uint8_t _49[3];
        ee::Float speed;          // +0x4C what its velocity is scaled by
        uint32_t perchAnimation;  // +0x50 name hashes: the animation it has at its home,
        uint32_t flyAnimation;    // +0x54 with its flock,
        uint32_t returnAnimation; // +0x58 and on its way back
        uint32_t flockName;       // +0x5C its flock's name hash
        uint32_t _60;
        ee::Ptr<zge::SceneObject> flock;    // +0x64 0 until the update has found it by name
        uint32_t animation;       // +0x68 the one it plays, 0 before its first update
        ee::Ptr<zge::SceneObject> nextBird; // +0x6C the flock's next bird
        ee::Float unusedFloat;    // +0x70 copied out and back with the rest; read nowhere (PS2, GameCube)
        zge::Vec3 home;           // +0x74 where it was at its init
        zge::Vec3 position;       // +0x80 where its last update left it
        zge::Vec3 velocity;       // +0x8C
    };
    static_assert(offsetof(BirdRecord, state) == 0x48 && offsetof(BirdRecord, flock) == 0x64);
    static_assert(offsetof(BirdRecord, unusedFloat) == 0x70 && offsetof(BirdRecord, home) == 0x74 && offsetof(BirdRecord, velocity) == 0x8C);
    static_assert(sizeof(BirdRecord) == 0x98);

    // nearestRiderVisit's answer: the rider nearest the object it was last
    // run for, and that distance squared (set to -1 before a run).
    inline ee::Global<ee::Float, sym::g_nearestRiderDistanceSquared> g_nearestRiderDistanceSquared;
    inline ee::Global<uint8_t, sym::g_nearestRider> g_nearestRider;

    // (flocks.cpp)
    void addFlockBird(ee::Ptr<zge::SceneObject> flock, ee::Ptr<zge::SceneObject> bird);
    ee::Ptr<zge::SceneObject> firstFlockBird(ee::Ptr<zge::SceneObject> flock);
    // (birds.cpp)
    ee::Ptr<zge::SceneObject> nextFlockBird(ee::Ptr<zge::SceneObject> bird);
    uint32_t nearestRiderVisit(uint32_t riderIndex, ee::Ptr<zge::SceneObject> object);
}
