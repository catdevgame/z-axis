#pragma once

// The particle effects of inlineFx.c (readable round gamehand): the effect
// classes the level's effects library (assets\fx.zal) makes its emitters
// from, registered by name at its load (initInlineFx: "snow", "rain",
// "spark", "userspark", "vapor", "smoke", "usersmoke", "firetrail" and
// the rest). A class is a 0x38-byte table of methods; the engine's
// particle emitter object (zobjParticleEmitter.c) calls them: +0x10 makes
// an emitter, +0x18 spawns particles into it, +0x1C moves them and +0x20
// draws them (the draws' results, particles drawn, are summed by the
// caller). Particles come from the heap's size-class pools, as many at
// once as the effects' budget allows.
//
// Field names past the offsets are inferred from what the code does.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/container/cursor_item.h"
#include "zge/memory/pool.h"
#include "zge/math/types.h"
#include "aggressive/fx/effects_clock.h"

#include <cstddef>
#include <cstring>
#include <cstdint>
#include "sym/aggressive/fx/particles.h.h"

namespace zge
{
    struct Node;        // (zge/scene/node.h)
    struct SceneObject; // (zge/scene/scene.h)
}

namespace aggressive::skater
{
    struct Player;
}

namespace aggressive::fx
{
    // The engine's array with a cursor (zge's PointerArray: newCursorArray
    // makes one, current and the rest walk it).
    using CursorArray = zge::PointerArray;

    // The array's first item (the cursor on it), 0 if empty: the engine's
    // inline walk, as every effect method has it.
    inline uint32_t first(CursorArray &a, uint32_t emptyLeader = 0, uint32_t joinLeader = 0)
    {
        uint32_t item = 0;
        if (a.count)
        {
            a.cursor = 0;
            item = a.items[0];
        }
        else
            a.cursor = -1;
        return item;
    }
    // The cursor moved to the next item, 0 past the end .
    inline uint32_t next(CursorArray &a, uint32_t endLeader = 0)
    {
        const int32_t at = a.cursor;
        if (at >= 0)
        {
            a.cursor = at + 1;
            if (at + 1 < static_cast<int32_t>(a.count))
                return a.items[at + 1];
        }
        a.cursor = -1;
        return 0;
    }

    // Particle flags.
    // A vector's x, y and z, for the engine's Vec3 functions.
    inline zge::Vec3 *asVec3(zge::Vec4 &v) { return reinterpret_cast<zge::Vec3 *>(&v); }
    inline const zge::Vec3 *asVec3(const zge::Vec4 &v) { return reinterpret_cast<const zge::Vec3 *>(&v); }

    enum : uint32_t
    {
        kParticleMoves = 1u, // moved by its velocity each tick (the tail left where it was)
        kParticleSpins = 2u, // its axis turned by its spin each tick
    };

    // What every class's particle starts with; the classes add their own
    // fields past +0x70 (the emitter's particleBytes says how many).
    struct Particle
    {
        uint32_t flags;          // +0x00
        uint8_t _04[0x0C];
        zge::Vec4 position;      // +0x10 the head
        zge::Vec4 tail;          // +0x20 last tick's position (a streak's tail)
        zge::Vec4 velocity;      // +0x30 units a second
        zge::Vec4 axis;          // +0x40 spinning kinds: turned by the spin each tick
        zge::EulerAngles spin;   // +0x50
        uint8_t colour[4];       // +0x56 RGBA (unaligned)
        uint8_t _5a[2];
        ee::Float size;          // +0x5C half a square's side
        uint32_t texture;        // +0x60 0: a line
        uint32_t lifetime;       // +0x64 milliseconds
        uint32_t born;           // +0x68 the effects' clock
        uint32_t _6c;
    };
    static_assert(offsetof(Particle, position) == 0x10 && offsetof(Particle, tail) == 0x20);
    static_assert(offsetof(Particle, velocity) == 0x30 && offsetof(Particle, axis) == 0x40);
    static_assert(offsetof(Particle, spin) == 0x50 && offsetof(Particle, colour) == 0x56 && offsetof(Particle, size) == 0x5C);
    static_assert(offsetof(Particle, lifetime) == 0x64 && offsetof(Particle, born) == 0x68);
    static_assert(sizeof(Particle) == 0x70);

    // What a puff of "smoke" starts from (the named effect's parameters;
    // spawnSmokeBlast's, fx/spawns.cpp).
    struct SmokeParameters
    {
        zge::Vec4 position;          // +0x00
        zge::Vec4 positionSpread;    // +0x10
        zge::Vec4 velocity;          // +0x20
        zge::Vec4 velocitySpread;    // +0x30
        zge::Vec4 drift;             // +0x40 the puffs' own motion (units a second)
        union
        {
            uint8_t count;           // +0x50 puffs a start
            int32_t countWord;       // the Smoke Blast tuning writes the whole word
        };
        ee::Float size;              // +0x54
        int32_t lifetime;            // +0x58 milliseconds (each puff's within half of it either way)
        uint32_t texture;            // +0x5C
        uint32_t _60;
        ee::Float colourRate[4];     // +0x64 (Puff::colourRate)
        uint8_t keyColour[4][4];     // +0x74 (Puff::keyColour; the first is the puff's colour)
        uint32_t keyTime[4];         // +0x84 (Puff::keyTime)
        ee::Float growth;            // +0x94 (Puff::growth)
        uint32_t fade;               // +0x98 (Puff::fade)
    };
    static_assert(offsetof(SmokeParameters, count) == 0x50 && offsetof(SmokeParameters, texture) == 0x5C);
    static_assert(offsetof(SmokeParameters, colourRate) == 0x64 && offsetof(SmokeParameters, keyTime) == 0x84);
    static_assert(offsetof(SmokeParameters, growth) == 0x94 && offsetof(SmokeParameters, fade) == 0x98);

    // The spawn parameters of "spark" (the effect's, or the class's
    // defaults at sparkDefaults).
    struct SparkParams
    {
        zge::Vec4 position;      // +0x00
        zge::Vec4 velocity;      // +0x10
        uint32_t count;          // +0x20 sparks a spawn [inferred: grindSparks sets 5]
        ee::Float size;          // +0x24
        ee::Float spread;        // +0x28 the velocity's random part, each axis
        int32_t lifetime;        // +0x2C milliseconds, +-50 % at random
        uint8_t colour[4];       // +0x30
        ee::Ptr<skater::Player> source; // +0x34 the skater the sparks come off (they follow a grinding one)
    };
    static_assert(offsetof(SparkParams, count) == 0x20 && offsetof(SparkParams, spread) == 0x28 && offsetof(SparkParams, source) == 0x34);

    // "vapor": a streak between two points, still (the skater's trails are
    // these).
    struct VaporParams
    {
        zge::Vec4 head;          // +0x00
        zge::Vec4 tail;          // +0x10
        ee::Float size;          // +0x20
        uint32_t lifetime;       // +0x24 milliseconds
        uint8_t colour[4];       // +0x28 RGBA
        uint32_t texture;        // +0x2C
        uint32_t flags;          // +0x30
    };
    static_assert(offsetof(VaporParams, size) == 0x20 && offsetof(VaporParams, flags) == 0x30 && sizeof(VaporParams) == 0x40);

    // The upper half of a vector's quadword (its z and w): where a float
    // reaches VU0 through an integer register, that register's upper half
    // lands in VU0's upper lanes (vec.h scaledCarrying).
    inline uint64_t upperHalf(const zge::Vec4 &v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v.z, 8);
        return bits;
    }

    // What a poster starts from (the named effect's parameters; with
    // none, the zeros at posterDefaults).
    struct PosterParameters
    {
        zge::Vec4 corners[4];  // +0x00
        uint32_t lifetime;     // +0x40 milliseconds
        uint8_t colour[4];     // +0x44 RGBA
        uint32_t texture;      // +0x48
    };
    static_assert(offsetof(PosterParameters, lifetime) == 0x40 && offsetof(PosterParameters, texture) == 0x48);

    // What a ripple starts from (the named effect's parameters; with
    // none, the defaults at rippleDefaults: lifetime 3000, size 0.1, growth
    // 0.05, white at alpha 0x70).
    struct RippleParameters
    {
        ee::Float x, y, z;       // +0x00
        uint32_t _0c;            // not written by the spawns: the ripple reads x, y and z
        uint32_t lifetime;       // +0x10 milliseconds
        ee::Float size;          // +0x14 half the square's side
        ee::Float growth;        // +0x18 size added a second
        uint8_t colour[4];       // +0x1C RGBA
        zge::Vec4 velocity;      // +0x20
        uint32_t texture;        // +0x30
        ee::Ptr<skater::Player> follows; // +0x34 the skater it follows (0: none)
    };
    static_assert(offsetof(RippleParameters, lifetime) == 0x10 && offsetof(RippleParameters, colour) == 0x1C);
    static_assert(offsetof(RippleParameters, velocity) == 0x20 && offsetof(RippleParameters, follows) == 0x34);

    // A flame of a fire trail ("firetrail", 0x80 bytes): the animation's
    // frame and the time it still shows; it shrinks and fades over its life.
    struct Flame : Particle
    {
        uint32_t frame;          // +0x70 0 .. 9
        ee::Float frameLeft;     // +0x74 seconds
        ee::Float shrink;        // +0x78 size lost a millisecond (half its size over its life)
        ee::Float fade;          // +0x7C alpha lost a millisecond (all of it over its life)
    };
    static_assert(offsetof(Flame, frame) == 0x70 && offsetof(Flame, frameLeft) == 0x74);
    static_assert(offsetof(Flame, shrink) == 0x78 && offsetof(Flame, fade) == 0x7C && sizeof(Flame) == 0x80);

    // A puff of smoke ("smoke", 0xC0 bytes) drifts with a velocity of its
    // own; its colour goes through four keys at their times, it grows with
    // age and fades by its rate.
    struct Puff : Particle
    {
        zge::Vec4 drift;         // +0x70 units a second
        ee::Float colourRate[4]; // +0x80 RGBA change a millisecond toward the next key
        uint8_t keyColour[4][4]; // +0x90 RGBA at each key
        uint32_t keyTime[4];     // +0xA0 milliseconds of age
        uint8_t key;             // +0xB0 the key it is past
        uint8_t _b1[3];
        ee::Float growth;        // +0xB4 size times this per millisecond of age (at least its size)
        uint32_t fade;           // +0xB8 alpha lost a second
        uint32_t _bc;
    };
    static_assert(offsetof(Puff, drift) == 0x70 && offsetof(Puff, colourRate) == 0x80 && offsetof(Puff, keyColour) == 0x90);
    static_assert(offsetof(Puff, keyTime) == 0xA0 && offsetof(Puff, key) == 0xB0 && offsetof(Puff, growth) == 0xB4);
    static_assert(offsetof(Puff, fade) == 0xB8 && sizeof(Puff) == 0xC0);

    // An emitter (0x40 bytes, from the heap).
    struct Emitter
    {
        uint8_t _00[0x20];
        ee::Ptr<CursorArray> particles; // +0x20
        uint32_t particleBytes;         // +0x24 its class's particle size (the pool it allocates from)
        uint32_t lastMoved;             // +0x28 the clock at the last move (moves happen once a tick)
        uint32_t owner;                 // +0x2C what it belongs to (the spark and smoke draws' cull: particle_draws.cpp)
        uint32_t effectClass;           // +0x30 the class table it was made from
        uint8_t _34[0x0C];
    };
    static_assert(offsetof(Emitter, particles) == 0x20 && offsetof(Emitter, lastMoved) == 0x28);
    static_assert(offsetof(Emitter, effectClass) == 0x30 && sizeof(Emitter) == 0x40);

    // An affector (gravity, the floor ...): its function, run on an emitter
    // with the argument its link to the class gives.
    struct Affector
    {
        // +0x00 affectorNothing (effects_clock.cpp) in every affector, an empty function that
        // nothing calls (the links use only +0x04).
        uint32_t _00;
        ee::FunctionPtr<void(ee::Ptr<Affector> affector, ee::Ptr<Emitter> emitter, uint32_t argument)> apply; // +0x04
    };
    static_assert(offsetof(Affector, apply) == 0x04);

    // One of an effect class's affectors, in the class's list.
    struct AffectorLink
    {
        ee::Ptr<AffectorLink> next; // +0x00
        uint32_t _04;
        ee::Ptr<Affector> affector; // +0x08
        uint32_t argument;          // +0x0C
    };
    static_assert(offsetof(AffectorLink, affector) == 0x08 && offsetof(AffectorLink, argument) == 0x0C);

    // An effect class (its 0x38-byte method table, zge's EffectClass) as the
    // moves see it: the affectors the level's effects library gave it.
    struct ClassAffectors
    {
        uint8_t _00[0x24];
        ee::Ptr<AffectorLink> affectors; // +0x24
    };
    static_assert(offsetof(ClassAffectors, affectors) == 0x24);

    // Each of the class's affectors run on the emitter, in the list's order.
    inline void runAffectors(uint32_t effectClass, ee::Ptr<Emitter> emitter)
    {
        for (ee::Ptr<AffectorLink> link = ee::Ptr<ClassAffectors>(effectClass)->affectors; link; link = link->next)
            link->affector->apply(link->affector, emitter, link->argument);
    }

    // One effect placed in the level (user_effects.cpp: loadUserEffects fills
    // it from the locator's tag record, TGFX, by offset). The user classes
    // ("userspark", "usersmoke", "userbubble", "userfirefly") spawn from it:
    // each value with its spread, a random part in [-1, 1) of it.
    struct UserEffect
    {
        uint32_t name;               // +0x00 the locator's name hash
        uint8_t kind;                // +0x04 the class: 1, 2 smoke, 3 bubbles, 4 fireflies, else sparks
        uint8_t _05[0x0B];
        zge::Vec4 velocity;          // +0x10
        zge::Vec4 velocitySpread;    // +0x20
        zge::Vec4 position;          // +0x30 the emitter's (the node's point, when it follows one)
        zge::Vec4 positionSpread;    // +0x40
        zge::Vec4 offset;            // +0x50 from the followed node, in its frame
        uint32_t ownTexture;         // +0x60 nonzero: the texture below, else the class's
        uint32_t texture;            // +0x64
        ee::Float size;              // +0x68
        uint8_t rate;                // +0x6C particles a second
        uint8_t _6d[3];
        ee::Float lifetime;          // +0x70 milliseconds
        ee::Float lifetimeSpread;    // +0x74
        uint8_t _78[8];
        zge::Vec4 drift;             // +0x80 the bubbles' own motion (units a second)
        uint8_t colour[4];           // +0x90 RGBA
        ee::Float growth;            // +0x94 the smoke's puffs: their growth (Puff::growth)
        uint32_t fade;               // +0x98 and their fade (Puff::fade)
        ee::Ptr<zge::SceneObject> emitter; // +0x9C its particle emitter (0: none)
        uint32_t enabled;            // +0xA0
        uint32_t hidden;             // +0xA4 (its object was not found; the draws' cull sets it a frame)
        uint8_t due;                 // +0xA8 particles to spawn this frame
        uint8_t _a9;
        uint16_t spawned;            // +0xAA this second's so far
        ee::Float time;              // +0xAC into this second
        ee::Ptr<zge::Node> node;     // +0xB0 the node it follows (0: none)
        uint32_t area;               // +0xB4
        ee::Float range;             // +0xB8 in split screen, player one farther than this: not run
        uint32_t next;               // +0xBC
    };
    static_assert(offsetof(UserEffect, velocitySpread) == 0x20 && offsetof(UserEffect, positionSpread) == 0x40);
    static_assert(offsetof(UserEffect, offset) == 0x50 && offsetof(UserEffect, ownTexture) == 0x60 && offsetof(UserEffect, size) == 0x68);
    static_assert(offsetof(UserEffect, rate) == 0x6C && offsetof(UserEffect, lifetime) == 0x70 && offsetof(UserEffect, drift) == 0x80);
    static_assert(offsetof(UserEffect, colour) == 0x90 && offsetof(UserEffect, growth) == 0x94 && offsetof(UserEffect, emitter) == 0x9C && offsetof(UserEffect, due) == 0xA8);
    static_assert(offsetof(UserEffect, time) == 0xAC && offsetof(UserEffect, next) == 0xBC && sizeof(UserEffect) == 0xC0);

    // A bubble ("bubble", "userbubble"): it drifts by its own motion as well.
    struct Bubble : Particle
    {
        zge::Vec4 drift;             // +0x70 units a second
    };
    static_assert(offsetof(Bubble, drift) == 0x70 && sizeof(Bubble) == 0x80);

    // A firefly ("firefly", "userfirefly", 0xA0 bytes): it flies inside its
    // box (the spawn point with the placed effect's spread either way),
    // turned back at the box's sides, and takes a new random velocity every
    // quarter second.
    struct Firefly : Particle
    {
        zge::Vec4 boxMin;            // +0x70
        zge::Vec4 boxMax;            // +0x80
        ee::Float untilNewVelocity;  // +0x90 seconds
        uint8_t _94[0x0C];
    };
    static_assert(offsetof(Firefly, boxMin) == 0x70 && offsetof(Firefly, boxMax) == 0x80);
    static_assert(offsetof(Firefly, untilNewVelocity) == 0x90 && sizeof(Firefly) == 0xA0);

    // What "bubble" spawns from (startBubbles' parameters): 1 to mostPerSpawn
    // bubbles, each at the position with its spread either way.
    struct BubbleParameters
    {
        zge::Vec4 position;       // +0x00
        zge::Vec4 positionSpread; // +0x10
        zge::Vec4 drift;          // +0x20 the bubbles' own motion (Bubble::drift)
        ee::Float smallest;       // +0x30 size: from this
        ee::Float largest;        // +0x34 to this, at random
        ee::Float speedY;         // +0x38 the bubble's speed on the y axis
        ee::Float speedYSpread;   // +0x3C either way, at random
        int32_t lifetime;         // +0x40 milliseconds, half of it either way at random
        uint32_t mostPerSpawn;    // +0x44
        uint8_t colour[4];        // +0x48 RGBA
    };
    static_assert(offsetof(BubbleParameters, drift) == 0x20 && offsetof(BubbleParameters, smallest) == 0x30);
    static_assert(offsetof(BubbleParameters, lifetime) == 0x40 && offsetof(BubbleParameters, colour) == 0x48);

    // What "firefly" spawns from (startFireflies' parameters): fireflies in
    // the box of the position with its spread either way.
    struct FireflyParameters
    {
        zge::Vec4 position;       // +0x00
        zge::Vec4 positionSpread; // +0x10
        uint8_t colour[4];        // +0x20 RGBA
        int32_t lifetime;         // +0x24 milliseconds, half of it either way at random
    };
    static_assert(offsetof(FireflyParameters, colour) == 0x20 && offsetof(FireflyParameters, lifetime) == 0x24);

    // What a blood pool starts from ("bloodpool": startBloodPool's
    // parameters): a flat pool on the ground that spreads.
    struct BloodPoolParameters
    {
        zge::Vec4 position;          // +0x00
        zge::Quaternion orientation; // +0x10 the pool lies in its x-z plane (normalized by the spawn)
        ee::Float size;              // +0x20 half the square's side at the start
        ee::Float growth;            // +0x24 (BloodPool::growth)
        uint32_t lifetime;           // +0x28 milliseconds
        ee::Float largest;           // +0x2C the size it stops growing at
    };
    static_assert(offsetof(BloodPoolParameters, orientation) == 0x10 && offsetof(BloodPoolParameters, size) == 0x20);
    static_assert(offsetof(BloodPoolParameters, lifetime) == 0x28 && offsetof(BloodPoolParameters, largest) == 0x2C);

    // A blood pool ("bloodpool", 0xC0 bytes): a square in its frame's x-z
    // plane, growing with age up to its largest size, fading over its last
    // second.
    struct BloodPool : Particle
    {
        zge::Matrix frame;           // +0x70 its orientation, its position the translation
        ee::Float growth;            // +0xB0 its size times this, a millisecond of age, added to it
        ee::Float largest;           // +0xB4
        uint8_t _b8[8];
    };
    static_assert(offsetof(BloodPool, frame) == 0x70 && offsetof(BloodPool, growth) == 0xB0);
    static_assert(offsetof(BloodPool, largest) == 0xB4 && sizeof(BloodPool) == 0xC0);

    // "firetrail"'s spawn parameters: flames from the tail to the head.
    struct FiretrailParams
    {
        zge::Vec4 head;          // +0x00
        zge::Vec4 tail;          // +0x10
        ee::Float size;          // +0x20
        uint32_t lifetime;       // +0x24 milliseconds
    };
    static_assert(offsetof(FiretrailParams, tail) == 0x10 && offsetof(FiretrailParams, lifetime) == 0x24);

    // What the blood effects ("bloodsquirt", "bloodcloud") start from (the
    // named effect's parameters).
    struct BloodParameters
    {
        zge::Vec4 position;          // +0x00
        zge::Vec4 velocity;          // +0x10
        ee::Float size;              // +0x20
        ee::Float floorOrGrowth;     // +0x24 a squirt's: the height it lands at; a cloud's: its growth a millisecond
        uint32_t lifetime;           // +0x28 milliseconds
    };
    static_assert(offsetof(BloodParameters, size) == 0x20 && offsetof(BloodParameters, lifetime) == 0x28);

    // A drop of a squirt of blood ("bloodsquirt", 0x80 bytes): it falls
    // (the y axis points down) until it lands at its floor's height.
    struct BloodDrop : Particle
    {
        ee::Float floor;             // +0x70 the height it lands at
        uint8_t landed;              // +0x74
        uint8_t _75[0x0B];
    };
    static_assert(offsetof(BloodDrop, floor) == 0x70 && offsetof(BloodDrop, landed) == 0x74 && sizeof(BloodDrop) == 0x80);

    // A cloud of blood ("bloodcloud", 0x80 bytes): it grows with age.
    struct BloodCloud : Particle
    {
        ee::Float growth;            // +0x70 its size times this, a millisecond of age, added to it
        uint8_t _74[0x0C];
    };
    static_assert(offsetof(BloodCloud, growth) == 0x70 && sizeof(BloodCloud) == 0x80);

    // What a splash's root starts from ("splashroot": the named effect's
    // parameters).
    struct SplashParameters
    {
        zge::Vec4 position;          // +0x00 on the water
        zge::Vec4 velocity;          // +0x10
        ee::Float radius;            // +0x20 of its rings
    };
    static_assert(offsetof(SplashParameters, velocity) == 0x10 && offsetof(SplashParameters, radius) == 0x20);

    // A splash's root ("splashroot", 0xC0 bytes, never drawn itself): thrown
    // up from the water (y down), it falls back; on the way it leaves a ring
    // every third tick between where it was and where it is, and where it
    // falls back through the water a ring of drops.
    struct SplashRoot : Particle
    {
        ee::Float radius;            // +0x70 its rings'
        uint8_t _74[0x0C];
        zge::Vec4 origin;            // +0x80 the water's point it came up from
        zge::Vec4 origin2;           // +0x90 (a copy of it, not read)
        zge::Vec4 lastRing;          // +0xA0 where the last ring was left
        uint32_t ticks;              // +0xB0 since the last ring
        uint32_t dropsMade;          // +0xB4
        uint8_t _b8[8];
    };
    static_assert(offsetof(SplashRoot, radius) == 0x70 && offsetof(SplashRoot, origin) == 0x80 && offsetof(SplashRoot, lastRing) == 0xA0);
    static_assert(offsetof(SplashRoot, ticks) == 0xB0 && offsetof(SplashRoot, dropsMade) == 0xB4 && sizeof(SplashRoot) == 0xC0);

    // A splash's ring ("splashring", 0x280 bytes): a band between two rings
    // of 16 points around the splash's origin, one at the root's height when
    // it was left and one at the height of the ring before; drawn with the
    // splash ring texture, the second one from its third tick.
    struct SplashRing : Particle
    {
        uint32_t ticks;              // +0x70
        uint8_t _74[0x0C];
        zge::Vec4 ring[16];          // +0x080 at the root's height
        zge::Vec4 ringBefore[16];    // +0x180 at the last ring's
    };
    static_assert(offsetof(SplashRing, ticks) == 0x70 && offsetof(SplashRing, ring) == 0x80);
    static_assert(offsetof(SplashRing, ringBefore) == 0x180 && sizeof(SplashRing) == 0x280);

    // A class's emitter: room for `capacity` particles of `particleBytes`
    // (its pool); `copy` the copy's address (the emitters are copies of one
    // code; effect_classes.cpp).
    uint32_t newClassEmitter(uint32_t effectClass, uint32_t capacity, uint32_t particleBytes, uint32_t copy);

    // The effects' particle budget (the heap's pools are shared).
    inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget; // most particles alive at once
    inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
    // Seconds a tick of the effects takes (the frame's time; a streak's tail lags this much).
    inline ee::Global<ee::Float, sym::g_effectsTickTime> g_tickTime;

    // A particle back to its emitter's pool, and one fewer alive.
    void freeParticle(ee::Ptr<Emitter> emitter, ee::Address particle);

    // A particle for the emitter from its class's pool, if the effects'
    // budget has room for one more (it counts as alive from here); else 0.
    template <typename P = Particle>
    inline ee::Ptr<P> newParticle(const Emitter &emitter)
    {
        uint32_t particle = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            particle = zge::allocateSized(emitter.particleBytes);
        }
        return ee::Ptr<P>(particle);
    }

    // The particle at the walk's cursor taken out of the emitter and freed;
    // the walk goes on from the particle moved into its place.
    inline uint32_t freeCurrent(ee::Ptr<Emitter> emitter, uint32_t particle)
    {
        zge::pointerArrayRemoveCurrent(emitter->particles);
        freeParticle(emitter, particle);
        return zge::current(emitter->particles);
    }
}

namespace aggressive::fx
{
    // (particles.cpp)
    uint32_t alwaysZero();
    void debugMenuSecondHook();
    int32_t moveParticles(uint32_t effect, ee::Ptr<Emitter> emitter);
}
