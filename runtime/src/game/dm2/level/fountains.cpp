// The fountains: the "Fountain" level objects, left unfinished in DM2 (the
// Xbox build has the same code). newFountain makes each a record of
// settings, kept in its component's first word, that nothing reads; the
// frame and the touch do nothing; the removed message frees the record.
// The fountain's particle class (a record of DM2's data: its emitter, an empty spawn, its
// move and a draw whose loop never ends while a particle exists) is never
// registered, so no fountain particle is ever made. No recording reaches a
// fountain. DM2's own code (its game block, the level objects).

#include "dynamics_objects.h"
#include "object_node.h"

#include "dm2/fx/particles.h"
#include "dm2/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/container/cursor_item.h"
#include "zge/container/pointer_array.h"
#include "zge/game.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/effects.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/fountains.cpp.h"

namespace game::dm2
{
    namespace
    {

        // A fountain's particle (0x90 bytes): the effects' particle
        // (fx/particles.h) as far as the fields go that both have, its age
        // counted in moves, then words of its own nothing reads.
        struct FountainParticle
        {
            uint32_t flags;          // +0x00 zge::kParticleMoves
            uint8_t _04[0x0C];
            zge::Vec4 position;      // +0x10
            zge::Vec4 tail;          // +0x20 the position before the last move
            zge::Vec4 velocity;      // +0x30 units a move
            uint8_t _40[0x10];
            zge::EulerAngles spin;   // +0x50
            uint8_t colour[4];       // +0x56 R, G, B, A (unaligned)
            uint8_t _5a[2];
            ee::Float size;          // +0x5C
            uint32_t material;       // +0x60
            uint32_t lifetime;       // +0x64 moves
            uint32_t age;            // +0x68 moves so far
            uint8_t ownBytes[4];     // +0x6C
            ee::Float ownFloats[5];  // +0x70
            uint32_t ownWord;        // +0x84
            uint8_t _88[0x08];
        };
        static_assert(offsetof(FountainParticle, spin) == 0x50 && offsetof(FountainParticle, colour) == 0x56);
        static_assert(offsetof(FountainParticle, lifetime) == 0x64 && offsetof(FountainParticle, ownBytes) == 0x6C);
        static_assert(offsetof(FountainParticle, ownWord) == 0x84 && sizeof(FountainParticle) == 0x90);

        // A fountain's record (0x930 bytes, from the heap): a head of
        // settings, then room for 16 particles of which newFountain sets the
        // first. Nothing in any DM2 build (PS2, Xbox) reads it.
        struct FountainRecord
        {
            uint32_t headWords[4];            // +0x00
            ee::Ptr<zge::SceneObject> object; // +0x10 the fountain
            uint32_t headWords2[2];           // +0x14
            ee::Float headFloats[3];          // +0x1C
            uint8_t _28[0x08];
            FountainParticle particles[16];   // +0x30
        };
        static_assert(offsetof(FountainRecord, object) == 0x10 && offsetof(FountainRecord, particles) == 0x30);
        static_assert(sizeof(FountainRecord) == 0x930);

        constexpr uint32_t kFountainLibraryPath = sym::fountainLibraryPath; // "assets\fx.zal"
        // The settings newFountain writes (0.2, 1.2, 0.007, 0.1).
        constexpr ee::Float kFifth = std::bit_cast<float>(0x3E4CCCCDu);
        constexpr ee::Float kOnePointTwo = std::bit_cast<float>(0x3F99999Au);
        constexpr ee::Float kSevenThousandths = std::bit_cast<float>(0x3BE56042u);
        constexpr ee::Float kTenth = std::bit_cast<float>(0x3DCCCCCDu);
        constexpr uint32_t kFountainLifetime = 60; // moves
        constexpr uint8_t kWhite = 0xFF, kFountainAlpha = 200;
        constexpr uint8_t kFountainGrey = 100, kFountainBlue = 0xD2;

        // The particles' move: by the velocity times the step (1 / 600), then
        // by the whole velocity; the velocity's fall grows by the step times
        // 9.8 every move.
        constexpr ee::Float kMoveStep = std::bit_cast<float>(0x3ADA740Eu);
        constexpr ee::Float kGravity = std::bit_cast<float>(0x411CCCCDu);
        constexpr uint32_t kFountainParticleBytes = sizeof(FountainParticle);
        constexpr ee::Ptr<const char> kFountainTypeName{sym::fountainTypeName}; // "Fountain"
        // The fountain's component: its effect's record (word 0).
        constexpr uint32_t kFountainTagWords = 8, kFountainTagEffect = 0;
    }

    // newFountain (the type's set-up): its update, removed and touch
    // messages; the effects' library opened and closed again (nothing taken
    // from it); its record made and kept in the component; its node not
    // collided with.
    int32_t newFountain(ee::Ptr<zge::SceneObject> fountain)
    {
        zge::subscribe(fountain, zge::kUpdateMessage, zge::MessageHandler(sym::fountainFrame));
        zge::subscribe(fountain, zge::kRemovedMessage, zge::MessageHandler(sym::fountainRemoved));
        zge::subscribe(fountain, kTouchMessage, zge::MessageHandler(sym::fountainTouched));
        zge::closeZfileLibrary(zge::openZfileLibrary(kFountainLibraryPath));

        const ee::Ptr<FountainRecord> record(zge::heapAllocate(sizeof(FountainRecord)));
        FountainParticle &first = record->particles[0];
        record->headWords[3] = 2;
        record->headFloats[0] = kSevenThousandths;
        first.colour[2] = kWhite;
        first.colour[3] = kFountainAlpha;
        first.ownBytes[1] = kFountainGrey;
        first.ownBytes[2] = kFountainBlue;
        first.lifetime = kFountainLifetime;
        first.size = kTenth;
        first.ownFloats[0] = kOnePointTwo;
        first.ownWord = 1;
        first.velocity.y = -kFifth;
        record->headWords[2] = 1;
        record->headFloats[1] = kFifth;
        record->headFloats[2] = kOnePointTwo;
        first.colour[0] = kWhite;
        first.colour[1] = kWhite;
        first.ownBytes[0] = kFountainGrey;
        first.ownFloats[4] = kFifth;
        first.ownFloats[3] = kFifth;
        record->object = fountain;
        record->headWords2[0] = 0;
        record->headWords2[1] = 0;
        record->headWords[0] = 0;
        record->headWords[1] = 0;
        first.ownBytes[3] = 0;
        first.age = 0;
        first.material = 0;
        first.ownFloats[1] = 0.0f;
        first.spin = zge::EulerAngles{0, 0, 0};
        first.ownFloats[2] = 0.0f;
        first.velocity.x = 0.0f;
        first.velocity.z = 0.0f;
        zge::setTagWord(zge::objectComponent(fountain), kFountainTagWords, kFountainTagEffect, record);
        rider::collision::setNodeCollidable(objectNodeHandle(fountain), 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(newFountain, Draft);

    // The fountain's particle class (never registered): its emitter, room
    // for `capacity` particles.
    ee::Ptr<Emitter> newFountainEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity)
    {
        const ee::Ptr<Emitter> emitter(zge::heapAllocate(sizeof(Emitter)));
        if (emitter)
        {
            emitter->particles = zge::newPointerArray(capacity);
            emitter->pool = ee::Ptr<zge::Pool>(zge::createPool(kFountainParticleBytes, capacity, 0));
            emitter->effectClass = effectClass;
            emitter->lastMoved = zge::collisionClock();
        }
        return emitter;
    }
    READABLE_FUNCTION_WAITS(newFountainEmitter, Draft);

    // Its spawn: nothing.
    void spawnFountain()
    {
    }
    READABLE_FUNCTION(spawnFountain, Draft);

    // Its move: each moving particle moved (kept: twice, see kMoveStep) and
    // pulled down; a particle as old as its lifetime freed. Returns how many
    // particles it went through.
    int32_t moveFountainParticles(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> fall; // its w is never set
        int32_t visited = 0;
        if (!emitter || !emitter->particles)
            return 0;
        ee::Ptr<FountainParticle> particle(zge::arrayFirst(emitter->particles));
        while (particle)
        {
            ++visited;
            if (particle->flags & zge::kParticleMoves)
            {
                particle->tail = particle->position;
                const ee::Float stepX = particle->velocity.x * kMoveStep;
                const ee::Float stepY = particle->velocity.y * kMoveStep;
                const ee::Float stepZ = particle->velocity.z * kMoveStep;
                particle->position.x = particle->position.x + stepX;
                particle->position.y = particle->position.y + stepY;
                particle->position.z = particle->position.z + stepZ;
                particle->position = particle->position + particle->velocity;
                fall->x = 0.0f;
                fall->z = 0.0f;
                fall->y = kMoveStep * kGravity;
                particle->velocity = particle->velocity + *fall;
            }
            ++particle->age;
            if (particle->age < particle->lifetime)
                particle = ee::Ptr<FountainParticle>(zge::arrayNext(emitter->particles));
            else
            {
                zge::pointerArrayRemoveCurrent(emitter->particles);
                zge::poolFree(emitter->pool, particle);
                particle = ee::Ptr<FountainParticle>(zge::current(emitter->particles));
            }
        }
        return visited;
    }
    READABLE_FUNCTION_WAITS(moveFountainParticles, Draft);

    // registerFountainType: the "Fountain" level objects made fountains.
    uint32_t registerFountainType()
    {
        return game::tail(zge::registerEntityInit, kFountainTypeName, sym::newFountain);
    }
    READABLE_FUNCTION(registerFountainType, Verified);

    // fountainRemoved (the removed message): its effect's record (the
    // component's first word) freed.
    int32_t fountainRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        zge::heapFree(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(ee::Ptr<zge::SceneObject>(message->target)), kFountainTagWords, kFountainTagEffect)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(fountainRemoved, Draft);

    // fountainFrame, fountainTouched (the update and touch messages):
    // nothing.
    int32_t fountainFrame(ee::Ptr<zge::EngineMessage>)
    {
        return 1;
    }
    READABLE_FUNCTION(fountainFrame, Draft);

    int32_t fountainTouched(ee::Ptr<zge::EngineMessage>)
    {
        return 1;
    }
    READABLE_FUNCTION(fountainTouched, Draft);
}
