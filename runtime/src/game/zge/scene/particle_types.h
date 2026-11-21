#pragma once

// The engine's particle types (spark, billboard, poly: registerParticleType)
// and their behaviours, and what a particle emitter object
// (particle_emitter_objects.cpp) keeps of them. A type is a 0x44-byte table
// of methods; a behaviour (0x18 bytes: registerParticleBehaviour) is one of
// the three kinds "path follow", "acceleration" and "deflector" written for
// one type, and a type runs the behaviours bound to it
// (bindParticleBehaviour) on its particles.

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "zge/container/array.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace zge { struct Task; } // (zge/platform/tasks.h)

namespace zge
{
    struct ParticleType;
    struct ParticleEffect;
    struct SceneObject;
    struct ZpedRecord;
    struct EmitterDefinition;
    struct ParticleLook;
    struct PolyLook;
    struct BillboardEffect; // (zge/render/billboards.h)

    // What a particle type read from the ZPED chunk for one emitter (its
    // readSettings); the rest is the type's own.
    struct ParticleSettings
    {
        ee::Ptr<ParticleType> type; // +0x00
        union                       // +0x04 the type's own
        {
            ee::Ptr<ParticleLook> look;   // the spark and billboard types'
            ee::Ptr<PolyLook> polyLook;   // the poly type's
        };
    };
    static_assert(sizeof(ParticleSettings) == 0x08);

    // An effect a particle type made for an emitter object (its start): the
    // particles of that type the object has spawned.
    struct ParticleEffect
    {
        // +0x00, +0x10 the box its particles were in this frame, x, y and z
        // (the type's update sets it while it has particles)
        Vec4 boundsLow;
        Vec4 boundsHigh;
        ee::Ptr<ParticleType> type;  // +0x20 (the start method sets it)
        ee::Ptr<SceneObject> object; // +0x24 the emitter object (startEmitterEffect sets it)
        uint32_t _28, _2c;
        union
        {
            List particles;          // +0x30 the type's particles, from the type's pool
            // +0x30 ZGE4's billboard type keeps no list: its billboards are in the billboard queue's effect record
            // (zge/render/billboards.cpp), made by its start
            ee::Ptr<BillboardEffect> billboardEffect;
        };
        uint32_t _3c;
    };
    static_assert(offsetof(ParticleEffect, boundsHigh) == 0x10 && offsetof(ParticleEffect, object) == 0x24);
    static_assert(offsetof(ParticleEffect, particles) == 0x30 && sizeof(ParticleEffect) == 0x40);

    // A particle type (0x44 bytes, copied from its definition by
    // registerParticleType; findParticleType finds one by its name's hash).
    // ZGE4 (BMX XXX) has one method more, copySettings after readSettings,
    // so its types are 0x48 bytes and every field from releaseSettings on
    // sits 4 bytes further (the spark type's definition in both ELFs).
    struct ParticleType
    {
        uint32_t _00;
        // +0x04 called as the type is registered and again as it goes (all
        // three types' do nothing)
        ee::FunctionPtr<void(ee::Ptr<ParticleType> type)> registryHook;
        // +0x08 (type, 0x100) -> a new effect (the spark type's reads the type alone)
        ee::FunctionPtr<uint32_t(ee::Ptr<ParticleType> type, uint32_t word)> start;
        ee::FunctionPtr<void(ee::Ptr<ParticleType> type, ee::Ptr<ParticleEffect> effect)> endEffect; // +0x0C as the object goes
        // +0x10 the type's settings for an emitter, from its ZPED record (readZpedChunk)
        ee::FunctionPtr<ee::Ptr<ParticleSettings>(ee::Ptr<ParticleType> type, ee::Ptr<ZpedRecord> record)> readSettings;
#if ZX_GAME_BMX
        // +0x14 (type, settings) -> a copy of the settings and of their look, the look's material copied too
        ee::FunctionPtr<ee::Ptr<ParticleSettings>(ee::Ptr<ParticleType> type, ee::Ptr<ParticleSettings> settings)> copySettings;
#endif
        ee::FunctionPtr<void(ee::Ptr<ParticleType> type, ee::Ptr<ParticleSettings> settings)> releaseSettings; // +0x14 as the object goes
        // +0x18 (type, settings, material): the material set up for the type, 0: nothing to do
        ee::FunctionPtr<void(ee::Ptr<ParticleType> type, ee::Ptr<ParticleSettings> settings, uint32_t material)> setMaterialTextures;
        // +0x1C (type, object, effect, count, the object's data): count particles spawned
        ee::FunctionPtr<uint32_t(ee::Ptr<ParticleType> type, ee::Ptr<SceneObject> object, ee::Ptr<ParticleEffect> effect, uint32_t count, uint32_t data)> spawn;
        // +0x20 (type, effect, word, the object's data) -> the particles still alive
        ee::FunctionPtr<uint32_t(ee::Ptr<ParticleType> type, ee::Ptr<ParticleEffect> effect, uint32_t word, uint32_t data)> update;
        ee::FunctionPtr<uint32_t(ee::Ptr<ParticleType> type, ee::Ptr<ParticleEffect> effect)> draw; // +0x24
        uint32_t nameHash;   // +0x28 its name's ("spark", "regular billboard", "poly")
        uint32_t _2c;        // +0x2C 0x200 in all three types; nothing here reads it
        uint16_t typeIndex;  // +0x30 1 spark, 2 billboard, 3 poly: the behaviours written for it carry it
        uint16_t _32;
        List behaviours;     // +0x34 BehaviourLink
        uint32_t _40;        // +0x40 0 in all three types; nothing here reads it
    };
    static_assert(offsetof(ParticleType, setMaterialTextures) == game::readable::perGame(0x18u, 0x18u, 0x1Cu) &&
                  offsetof(ParticleType, draw) == game::readable::perGame(0x24u, 0x24u, 0x28u));
    static_assert(offsetof(ParticleType, nameHash) == game::readable::perGame(0x28u, 0x28u, 0x2Cu) &&
                  offsetof(ParticleType, typeIndex) == game::readable::perGame(0x30u, 0x30u, 0x34u));
    static_assert(offsetof(ParticleType, behaviours) == game::readable::perGame(0x34u, 0x34u, 0x38u) &&
                  sizeof(ParticleType) == game::readable::perGame(0x44u, 0x44u, 0x48u));

    // The behaviours' kinds (behaviourKindHash's numbers; their names' hashes
    // are the kinds' keys).
    enum ParticleBehaviourKind : uint32_t
    {
        kPathFollowBehaviour = 0,   // "path follow"
        kAccelerationBehaviour = 1, // "acceleration"
        kDeflectorBehaviour = 2,    // "deflector"
        kParticleBehaviourKinds = 3,
    };

    // A behaviour (0x18 bytes, copied from its definition by
    // registerParticleBehaviour; found by its kind and its type's index).
    struct ParticleBehaviour
    {
        ee::FunctionPtr<void(ee::Ptr<ParticleBehaviour> behaviour)> registered; // +0x00 as it is registered (none has one)
        ee::FunctionPtr<void(ee::Ptr<ParticleBehaviour> behaviour)> released;   // +0x04 as it goes (none has one)
        // +0x08 what it does to an effect's particles, with the emitter
        // definition it is bound by (the type's update calls it each frame)
        ee::FunctionPtr<void(ee::Ptr<ParticleBehaviour> behaviour, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)> apply;
        uint16_t typeIndex;  // +0x0C the type it is written for (ParticleType::typeIndex)
        uint16_t _0e;
        uint32_t kindHash;   // +0x10 behaviourKindHash of its kind
        uint32_t _14;
    };
    static_assert(offsetof(ParticleBehaviour, typeIndex) == 0x0C && offsetof(ParticleBehaviour, kindHash) == 0x10);
    static_assert(sizeof(ParticleBehaviour) == 0x18);

    // A behaviour bound to a type (0x10 bytes from the heap, on its list).
    struct BehaviourLink
    {
        ListNode node;                       // +0x00
        ee::Ptr<ParticleBehaviour> behaviour; // +0x08
        uint32_t data;                       // +0x0C what the binding passed
    };
    static_assert(offsetof(BehaviourLink, behaviour) == 0x08 && sizeof(BehaviourLink) == 0x10);

    // An emitter definition (findEmitterDefinition, by its name's hash).
    struct EmitterDefinition
    {
        uint32_t nameHash;      // +0x00
        uint32_t behaviourKind; // +0x04 ParticleBehaviourKind: with the type's index, its behaviour
        uint32_t _08, _0c;
        union
        {
            // +0x10 an acceleration's: x, y and z in g (9.81 a second
            // squared); a deflector's: its plane (the normal, then the
            // distance planeDistance takes)
            Vec4 acceleration;
            Vec4 plane;
        };
    };
    static_assert(offsetof(EmitterDefinition, acceleration) == 0x10 && offsetof(EmitterDefinition, plane) == 0x10);

    // An emitter definition the object uses, with the behaviour found for it
    // (0x10 bytes from the heap; its first two words are not set).
    struct EmitterBinding
    {
        uint32_t _00, _04;
        ee::Ptr<ParticleBehaviour> behaviour;  // +0x08 findParticleBehaviour's
        ee::Ptr<EmitterDefinition> definition; // +0x0C
    };
    static_assert(sizeof(EmitterBinding) == 0x10);

    // Where an emitter's particles start (ParticleEmitterData::spawnShape).
    enum ParticleSpawnShape : uint32_t
    {
        kSpawnInSphere = 0, // radius spawnSize.x around the point (0: the point itself)
        kSpawnInBox = 1,    // spawnSize the box's sides, centred on the point
    };

    // What axis a poly particle turns about (ParticleEmitterData::spinAxisKind).
    enum ParticleSpinAxisKind : uint32_t
    {
        kSpinAboutRandomAxis = 0, // a new random axis for each poly
        // The chunk gives one value (spinAxis.x, kept) and the poly sets no
        // axis: it turns about whatever its pool slot last held.
        kSpinAxisNotSet = 1,
        kSpinAboutGivenAxis = 2,  // spinAxis, turned at random by up to spinAxisSpread
    };

    // A particle's colour, as the primitive API takes a vertex's.
    struct ParticleColour
    {
        uint8_t red, green, blue, alpha;
    };
    static_assert(sizeof(ParticleColour) == 4);

    // A spark (0x60 bytes from the spark pool: the spark type's particle).
    struct Spark
    {
        ListNode node;         // +0x00 on its effect's list
        uint32_t _08, _0c;
        Vec4 position;         // +0x10
        Vec4 previous;         // +0x20 where it was a frame ago: its line's other end
        Vec4 velocity;         // +0x30 a second
        ParticleColour colour; // +0x40 (drawn with full alpha)
        ee::Float size;        // +0x44 the look's, kept but not drawn
        uint32_t material;     // +0x48
        ee::Float life;        // +0x4C the seconds it lives
        ee::Float age;         // +0x50
        uint8_t _54[0x0C];
    };
    static_assert(offsetof(Spark, position) == 0x10 && offsetof(Spark, velocity) == 0x30);
    static_assert(offsetof(Spark, colour) == 0x40 && offsetof(Spark, age) == 0x50 && sizeof(Spark) == 0x60);

    // What the spark and billboard types keep for an emitter (0x40 bytes from
    // the heap, ParticleSettings::look): from its ZPED record, or the type's
    // defaults for an object with no data.
    struct ParticleLook
    {
        uint32_t bytes;        // +0x00 its size
        uint32_t typeHash;     // +0x04 the record's particle type
        uint32_t _08, _0c;
        Vec4 cleared;          // +0x10 cleared from a record; nothing reads it
        // +0x20 billboards: the record's speed straight up (y is down), turned
        // by the record's orientation; sparks: cleared. Nothing reads it.
        Vec4 upVelocity;
        ee::Float size;        // +0x30
        ee::Float life;        // +0x34
        ParticleColour colour; // +0x38
        uint32_t material;     // +0x3C the type's setMaterialTextures, released with the look
#if ZX_GAME_BMX
        // BMX +0x40 ZGE4's billboard look is 0x50 bytes: bit 0 set when the look changed (readBillboardSettings,
        // setBillboardMaterial) and cleared by the billboard type's update as it hands the look to the effect
        uint64_t changed;
        uint8_t _48[0x08];
#endif
    };
    static_assert(offsetof(ParticleLook, upVelocity) == 0x20 && offsetof(ParticleLook, size) == 0x30);
    static_assert(offsetof(ParticleLook, material) == 0x3C && sizeof(ParticleLook) == game::readable::perGame(0x40u, 0x40u, 0x50u));

    // What the poly type keeps for an emitter (0x50 bytes from the heap,
    // ParticleSettings::look): a ParticleLook with one more vector.
    struct PolyLook
    {
        uint32_t bytes;        // +0x00 its size
        uint32_t typeHash;     // +0x04 the record's particle type
        uint32_t _08, _0c;
        Vec4 cleared;          // +0x10 cleared from a record; nothing reads it
        Vec4 upVelocity;       // +0x20 the record's speed straight up, turned by its orientation; nothing reads it
        // +0x30 (0, 0, 0, 1) in the type's defaults, not set from a record;
        // nothing reads it
        Vec4 origin;
        ee::Float size;        // +0x40
        ee::Float life;        // +0x44
        ParticleColour colour; // +0x48
        uint32_t material;     // +0x4C the type's setMaterialTextures, released with the look
    };
    static_assert(offsetof(PolyLook, upVelocity) == 0x20 && offsetof(PolyLook, size) == 0x40);
    static_assert(offsetof(PolyLook, material) == 0x4C && sizeof(PolyLook) == 0x50);

    // How an emitter launches its particles and what they are like: one block,
    // the same in the ZPED chunk's record and in the object's data. The types
    // read life and size from the record itself (their readSettings), and
    // nothing in the game reads the word after rate, speed, life and size.
    struct ParticleParams
    {
        Angle yaw;           // +0x00 the particles' direction about Z, then about Y
        int16_t yawSpread;   // +0x02 up to this much either way, at random
        Angle pitch;         // +0x04
        int16_t pitchSpread; // +0x06
        ee::Float rate;      // +0x08 particles a second it spawns by itself
        uint32_t _0c;
        ee::Float speed;     // +0x10 along the turned direction (up: y is down)
        // +0x14 the speed's spread, a fraction of it: ZGE4's particleSpawnVelocity starts a particle at
        // speed * (1 - speedSpread * u), u at random in [-1, 1); ZGE3 reads nothing here
        ee::Float speedSpread;
        ee::Float life;      // +0x18 the seconds a particle lives
        uint32_t _1c;
        // +0x20 a billboard's or poly's full size (a spark keeps it but is
        // drawn as a line)
        ee::Float size;
        // +0x24 ZGE4's billboards end their life at this size (updateBillboards hands size and endSize to the
        // effect's look); ZGE3 reads nothing here
        ee::Float endSize;
        // +0x28 billboards and polys: the seconds they grow over, from a
        // tenth of their size to all of it, as they start
        ee::Float growTime;
        ee::Float shrinkTime; // +0x2C and shrink over, back to a tenth, as they end
    };
    static_assert(offsetof(ParticleParams, rate) == 0x08 && offsetof(ParticleParams, speed) == 0x10);
    static_assert(offsetof(ParticleParams, life) == 0x18 && offsetof(ParticleParams, size) == 0x20);
    static_assert(offsetof(ParticleParams, shrinkTime) == 0x2C && sizeof(ParticleParams) == 0x30);

    // The five words ZGE4's ZPED record has after the params, and its emitter
    // data keeps after them (BMX record +0x6C, data +0xC0): the billboards'
    // view sphere radius, the view distance past which the effect fades out
    // and is not drawn, and the seconds a billboard fades in over and out over
    // (updateBillboards hands them to the effect's look). The first word is in
    // no function read.
    struct BillboardViewWords
    {
        uint32_t _00;
        ee::Float viewRadius;
        ee::Float viewDistance;
        ee::Float fadeInSeconds;
        ee::Float fadeOutSeconds;
    };
    static_assert(sizeof(BillboardViewWords) == 0x14);

    // ZGE4's live spawning (BMX data +0x138): the rate, particles a second
    // (the chunk's params.rate as read, the rate message's after; ZGE3 writes
    // params.rate itself), whether a start message's burst is running, and the
    // seconds of it left (counted down; below 0 the object stops emitting).
    struct EmitterBurst
    {
        ee::Float rate;
        int32_t bursting;
        ee::Float burstLeft;
    };

    // What ZGE4's ZPED chunk holds past its record (BMX data +0x144): where
    // readZpedChunk read it to (the data's own last word) and how many bytes
    // (chunkDataSize less the record's own count; 0: none, and nothing was
    // read).
    struct EmitterTrailingChunk
    {
        ee::Ptr<uint32_t> at;
        uint32_t bytes;
        uint32_t data;
    };

    // A ZPED chunk's record (0xF0 bytes, read whole by readZpedChunk): one
    // emitter object's settings. readZpedChunk copies them into the object's
    // data, the particle type named here reads its own from the record (its
    // readSettings: life, size, colour) and setupEmitterEffects binds the
    // emitter definitions it names. ZGE4's (BMX XXX, 0x12C bytes) starts with
    // its own byte count, has the billboards' five words after the params and
    // a start message's burst time after the colour: the BMX offsets in the
    // comments (readZpedChunk's copies and readBillboardSettings' reads).
    struct ZpedRecord
    {
#if ZX_GAME_BMX
        // BMX +0x00 the record's own byte count: what the chunk holds past it is the trailing data readZpedChunk
        // reads after the record (chunkDataSize less this)
        uint32_t recordBytes;
#endif
        uint32_t nameHash;        // +0x00 (BMX +0x04)
        uint32_t parentHash;      // +0x04
        uint32_t boneHash;        // +0x08
        Vec3 position;            // +0x0C where the object is placed
        ee::Float orientation[4]; // +0x18 how it is turned, a quaternion
        uint32_t spawnShape;      // +0x28 ParticleSpawnShape
        Vec3 spawnSize;           // +0x2C
        ParticleParams params;    // +0x38 (BMX +0x3C)
#if ZX_GAME_BMX
        BillboardViewWords billboardWords; // BMX +0x6C (readZpedChunk copies them to the data's)
#endif
        uint32_t typeHash;        // +0x68 (BMX +0x80) its particle type's name hash ("spark", "regular billboard", "poly")
        // +0x6C, +0x70 a kind and, for kinds 0 and 1, a value: copied into
        // the object's data, but nothing in the game reads either
        uint32_t unreadKind;
        ee::Float unreadValue;
        uint32_t _74;
        ee::Float spinPeriod;       // +0x78 (BMX +0x90)
        ee::Float spinPeriodSpread; // +0x7C
        Angle startAngle;           // +0x80
        uint16_t _82;
        ee::Float startAngleSpread; // +0x84
        uint32_t spinAxisKind;      // +0x88 ParticleSpinAxisKind (BMX +0xA0)
        Vec3 spinAxis;              // +0x8C
        Angle spinAxisSpread;       // +0x98
        uint16_t _9a;
        Vec3 unreadVector;          // +0x9C copied into the object's data; nothing reads it
        int32_t definitionCount;    // +0xA8 (BMX +0xC0)
        uint32_t definitions[16];   // +0xAC the emitter definitions' name hashes
        // +0xEC (BMX +0x104) the particles' colour (the types set the alpha themselves)
        ParticleColour colour;
#if ZX_GAME_BMX
        uint32_t _108;
        // BMX +0x10C the seconds a start message's burst runs (readZpedChunk copies it to the data's burstSeconds)
        ee::Float burstSeconds;
        uint8_t _110[0x1C];
#endif
    };
    static_assert(offsetof(ZpedRecord, orientation) == game::readable::perGame(0x18u, 0x18u, 0x1Cu) &&
                  offsetof(ZpedRecord, spawnShape) == game::readable::perGame(0x28u, 0x28u, 0x2Cu));
    static_assert(offsetof(ZpedRecord, params) == game::readable::perGame(0x38u, 0x38u, 0x3Cu));
    static_assert(offsetof(ZpedRecord, typeHash) == game::readable::perGame(0x68u, 0x68u, 0x80u) &&
                  offsetof(ZpedRecord, spinPeriod) == game::readable::perGame(0x78u, 0x78u, 0x90u));
    static_assert(offsetof(ZpedRecord, spinAxisKind) == game::readable::perGame(0x88u, 0x88u, 0xA0u) &&
                  offsetof(ZpedRecord, spinAxisSpread) == game::readable::perGame(0x98u, 0x98u, 0xB0u));
    static_assert(offsetof(ZpedRecord, unreadVector) == game::readable::perGame(0x9Cu, 0x9Cu, 0xB4u) &&
                  offsetof(ZpedRecord, definitions) == game::readable::perGame(0xACu, 0xACu, 0xC4u));
    static_assert(offsetof(ZpedRecord, colour) == game::readable::perGame(0xECu, 0xECu, 0x104u) &&
                  sizeof(ZpedRecord) == game::readable::perGame(0xF0u, 0xF0u, 0x12Cu));

    // The object's data (0x110 bytes from the heap, at SceneObject +0x3C).
    // From nameHash to unreadVector it holds what readZpedChunk copies from
    // the chunk. ZGE4's (BMX XXX, 0x150 bytes) keeps the emitter's matrix in
    // it, the chunk's five more words, the live rate apart from the params'
    // and a start message's burst (its readZpedChunk and
    // updateParticleEmitterObject): the BMX offsets in the comments.
    struct ParticleEmitterData
    {
        ee::Ptr<PointerArray> effects;        // +0x00 ParticleEffect: one a type, made on its first spawn
        ee::Ptr<PointerArray> bindings;       // +0x04 EmitterBinding
        ee::Ptr<Task> updateTask;             // +0x08 the task running updateEmitterEffects
#if ZX_GAME_BMX
        // BMX +0x0C the object the data belongs to (newParticleEmitterSceneObject sets it; the billboard type's
        // update hands it to the billboards' spawner as its context)
        ee::Ptr<SceneObject> object;
#endif
        ee::Ptr<SceneObject> parent;          // +0x0C (BMX +0x10) what it hangs from, found by parentHash on its first update
        ee::Ptr<SceneBone> bone;              // +0x10 (BMX +0x14) the parent's bone, by boneHash
        ee::Ptr<ParticleSettings> settings;   // +0x14 (BMX +0x18) its particle type's settings (0: the chunk named no known type)
#if ZX_GAME_BMX
        uint32_t _1c;
#else
        uint8_t _18[0x08];
#endif
        uint32_t nameHash;                    // +0x20 its id (particleEmitterObjectId)
        uint32_t parentHash;                  // +0x24 0: it hangs from nothing
        uint32_t boneHash;                    // +0x28
#if ZX_GAME_BMX
        uint32_t _2c;
        // BMX +0x30 the emitter's matrix (ZGE4's emitterMatrixOf; setEmitterOrientation writes it from a
        // quaternion, setEmitterPosition its fourth row; updateParticleEmitterObject multiplies by it)
        Matrix matrix;
#else
        uint8_t _2c[0x24];
#endif
        uint32_t spawnShape;                  // +0x50 (BMX +0x70) ParticleSpawnShape
        uint8_t _54[0x0C];
        Vec4 spawnSize;                       // +0x60 (BMX +0x80) (a box's: w 1)
        ParticleParams params;                // +0x70 (BMX +0x90)
#if ZX_GAME_BMX
        BillboardViewWords billboardWords;    // BMX +0xC0 the chunk's five words after the params (ZpedRecord +0x6C)
        uint32_t _d4;
#else
        uint32_t _a0;
#endif
        uint32_t unreadKind;                  // +0xA4 (BMX +0xD8) (ZpedRecord::unreadKind)
        ee::Float unreadValue;                // +0xA8 (BMX +0xDC)
        uint32_t _ac;
        // +0xB0 (BMX +0xE4) polys: the seconds one takes to turn once about its axis (0:
        // it does not turn), plus spinPeriodSpread times that times a random
        // factor (spawnPolys: meant as -1 to 1, it runs from -2 to 0)
        ee::Float spinPeriod;
        ee::Float spinPeriodSpread;           // +0xB4
        // +0xB8 (BMX +0xEC) polys: the angle about the axis one starts at, plus
        // startAngleSpread times that angle times the same kind of factor
        Angle startAngle;
        uint16_t _ba;
        ee::Float startAngleSpread;           // +0xBC
        uint32_t spinAxisKind;                // +0xC0 (BMX +0xF4) ParticleSpinAxisKind
#if ZX_GAME_BMX
        uint8_t _f8[0x08];
#else
        uint8_t _c4[0x0C];
#endif
        Vec4 spinAxis;                        // +0xD0 (BMX +0x100) (a given axis: w 1)
        Angle spinAxisSpread;                 // +0xE0 (BMX +0x110) each of three turns of a given axis, up to this either way
        uint16_t _e2;
        Vec3 unreadVector;                    // +0xE4 (BMX +0x114) (ZpedRecord::unreadVector)
#if ZX_GAME_BMX
        uint8_t _120[0x08];
        // BMX +0x128 the seconds a start message's burst runs (ZpedRecord::burstSeconds; emitterStartMessage
        // starts burstLeft from it)
        ee::Float burstSeconds;
        uint32_t _12c;
#else
        uint8_t _f0[0x10];
#endif
        int32_t emitting;                     // +0x100 (BMX +0x130) 0 while the rate is 0 (kEmitterRateMessage)
        ee::Float owed;                       // +0x104 (BMX +0x134) the part of a particle the rate has built up
#if ZX_GAME_BMX
        EmitterBurst burst;                   // BMX +0x138 the live rate and a start message's burst
        EmitterTrailingChunk trailingChunk;   // BMX +0x144 what the chunk held past the record
#else
        uint8_t _108[0x08];
#endif
    };
    static_assert(offsetof(ParticleEmitterData, nameHash) == 0x20 && offsetof(ParticleEmitterData, startAngle) == offsetof(ParticleEmitterData, spinPeriod) + 8);
    static_assert(offsetof(ParticleEmitterData, parent) == game::readable::perGame(0x0Cu, 0x0Cu, 0x10u) &&
                  offsetof(ParticleEmitterData, settings) == game::readable::perGame(0x14u, 0x14u, 0x18u));
    static_assert(offsetof(ParticleEmitterData, spawnShape) == game::readable::perGame(0x50u, 0x50u, 0x70u) &&
                  offsetof(ParticleEmitterData, spawnSize) == game::readable::perGame(0x60u, 0x60u, 0x80u));
    static_assert(offsetof(ParticleEmitterData, params) == game::readable::perGame(0x70u, 0x70u, 0x90u) &&
                  offsetof(ParticleEmitterData, unreadKind) == game::readable::perGame(0xA4u, 0xA4u, 0xD8u));
    static_assert(offsetof(ParticleEmitterData, spinPeriod) == game::readable::perGame(0xB0u, 0xB0u, 0xE4u) &&
                  offsetof(ParticleEmitterData, spinAxisKind) == game::readable::perGame(0xC0u, 0xC0u, 0xF4u));
    static_assert(offsetof(ParticleEmitterData, spinAxis) == game::readable::perGame(0xD0u, 0xD0u, 0x100u) &&
                  offsetof(ParticleEmitterData, spinAxisSpread) == game::readable::perGame(0xE0u, 0xE0u, 0x110u));
    static_assert(offsetof(ParticleEmitterData, unreadVector) == game::readable::perGame(0xE4u, 0xE4u, 0x114u) &&
                  offsetof(ParticleEmitterData, emitting) == game::readable::perGame(0x100u, 0x100u, 0x130u));
    static_assert(sizeof(ParticleEmitterData) == game::readable::perGame(0x110u, 0x110u, 0x150u));
#if ZX_GAME_BMX
    static_assert(offsetof(ParticleEmitterData, matrix) == 0x30 && offsetof(ParticleEmitterData, burstSeconds) == 0x128 &&
                  offsetof(ParticleEmitterData, burst) == 0x138 && offsetof(ParticleEmitterData, trailingChunk) == 0x144 &&
                  offsetof(ParticleEmitterData, billboardWords) == 0xC0 && offsetof(ParticleEmitterData, object) == 0x0C &&
                  offsetof(ZpedRecord, billboardWords) == 0x6C && offsetof(ZpedRecord, burstSeconds) == 0x10C);
#endif

    // An emitter object's data, where other objects have their animator.
    inline ee::Ptr<ParticleEmitterData> emitterData(ee::Ptr<SceneObject> object)
    {
        return object->animator.cast<ParticleEmitterData>();
    }

    // What ZGE4's emitter data and ZPED record have that ZGE3's have not: the
    // shared bodies read them through these, so a body compiles in every game
    // and reaches them only where kIsBmx (ZGE2's and ZGE3's answer nothing, and
    // are never reached).
#if ZX_GAME_BMX
    inline ee::Ptr<SceneObject> &emitterObject(ParticleEmitterData &data) { return data.object; }
    inline Matrix &emitterMatrixIn(ParticleEmitterData &data) { return data.matrix; }
    inline BillboardViewWords &billboardWords(ParticleEmitterData &data) { return data.billboardWords; }
    inline const BillboardViewWords &billboardWords(const ZpedRecord &record) { return record.billboardWords; }
    inline ee::Float &burstSeconds(ParticleEmitterData &data) { return data.burstSeconds; }
    inline ee::Float burstSeconds(const ZpedRecord &record) { return record.burstSeconds; }
    inline EmitterBurst &emitterBurst(ParticleEmitterData &data) { return data.burst; }
    inline EmitterTrailingChunk &trailingChunk(ParticleEmitterData &data) { return data.trailingChunk; }
    inline uint32_t recordBytes(const ZpedRecord &record) { return record.recordBytes; }
    inline uint64_t &lookChanged(ParticleLook &look) { return look.changed; }
#else
    namespace detail
    {
        template <typename T>
        inline T &noZge4Field() { static T none{}; return none; }
    }
    inline ee::Ptr<SceneObject> &emitterObject(ParticleEmitterData &) { return detail::noZge4Field<ee::Ptr<SceneObject>>(); }
    inline Matrix &emitterMatrixIn(ParticleEmitterData &) { return detail::noZge4Field<Matrix>(); }
    inline BillboardViewWords &billboardWords(ParticleEmitterData &) { return detail::noZge4Field<BillboardViewWords>(); }
    inline const BillboardViewWords &billboardWords(const ZpedRecord &) { return detail::noZge4Field<BillboardViewWords>(); }
    inline ee::Float &burstSeconds(ParticleEmitterData &) { return detail::noZge4Field<ee::Float>(); }
    inline ee::Float burstSeconds(const ZpedRecord &) { return ee::Float(0.0f); }
    inline EmitterBurst &emitterBurst(ParticleEmitterData &) { return detail::noZge4Field<EmitterBurst>(); }
    inline EmitterTrailingChunk &trailingChunk(ParticleEmitterData &) { return detail::noZge4Field<EmitterTrailingChunk>(); }
    inline uint32_t recordBytes(const ZpedRecord &) { return 0; }
    inline uint64_t &lookChanged(ParticleLook &) { return detail::noZge4Field<uint64_t>(); }
#endif
}

namespace zge
{
    struct EffectClass;       // (zge/scene/effects.h)
    struct EmitterObjectData; // (particle_emitter.cpp)

    // (particle_emitter.cpp)
    uint32_t startEffect(ee::Ptr<SceneObject> object, ee::Ptr<EffectClass> effectClass, uint32_t start, ee::Address a, ee::Address b);
    uint32_t spawnNamedEffect(uint32_t nameHash, ee::Address a, ee::Address b);
    ee::Ptr<EmitterObjectData> setEmitterOwner(ee::Ptr<SceneObject> object, ee::Address owner);
    uint32_t effectNameAllocate(uint32_t bytes);
    void freeEmitterList();
    void clearEmitterList();
    ee::Ptr<SceneObject> newParticleEmitterObject();
    uint32_t startNamedEffect(ee::Ptr<SceneObject> object, uint32_t name, uint32_t start, ee::Address a, ee::Address b);
}

namespace zge
{
    // (particle_emitter_objects.cpp)
    uint32_t registerParticleEmitterChunkHandlers();
    ee::Ptr<Vec4> emitterPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out);
    ee::Ptr<Matrix> emitterMatrix(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> matrix);
}

namespace zge
{
    // (particle_types.cpp)
    ee::Ptr<ParticleType> findParticleType(uint32_t nameHash);
    void registerParticleType(ee::Ptr<ParticleType> definition);
    ee::Ptr<ParticleBehaviour> findParticleBehaviour(uint32_t kindHash, uint16_t typeIndex);
    void registerParticleBehaviour(ee::Ptr<ParticleBehaviour> definition);
    uint32_t behaviourKindHash(uint32_t kind);
    uint32_t bindParticleBehaviour(uint32_t typeHash, uint32_t kindHash, uint32_t data);
    uint32_t spawnParticleEffect(ee::Ptr<ParticleEffect> effect, ee::Ptr<SceneObject> object, uint8_t count, ee::Address data);
    uint32_t updateParticleEffect(ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Address data);
    uint32_t drawParticleEffect(ee::Ptr<ParticleEffect> effect);
    ee::Ptr<Vec4> particleSpawnPoint(ee::Ptr<Vec4> out, ee::Ptr<Vec4> point, ee::Ptr<SceneObject> object);
    ee::Ptr<Vec4> particleSpawnVelocity(ee::Ptr<Vec4> out, ee::Ptr<Matrix> nodeMatrix, ee::Ptr<SceneObject> object);
    ee::Ptr<EmitterDefinition> findEmitterDefinition(uint32_t nameHash);

    // An emitter object's frame (particle_emitter_objects.cpp; ZGE4's updateParticleEmitterObjects calls it for
    // every emitter object).
    uint32_t updateParticleEmitterObject(ee::Ptr<SceneObject> object);
    // ZGE4 only (particle_emitter_objects.cpp): the emitter's matrix, kept in its data.
    ee::Ptr<Matrix> emitterMatrixOf(ee::Ptr<SceneObject> object);

    // The particle kinds' pools taken down (particle_emitter.cpp; ZGE4's
    // shutdownParticleTypes calls them before the registries go).
    int32_t unregisterSparkParticles();
    int32_t unregisterBillboardParticles();
    int32_t unregisterPolyParticles();
}
