// An explosion's effects (inlineFx.c): the "impactRing" class (a flat ring on
// the ground that widens and fades), "explosion" (puffs flung out from the
// centre, growing, their colour stepping through a fire's sixteen colours) and
// "debris" (pieces of a model thrown up, spinning, falling under gravity and
// bouncing off a floor). Each class's methods as the engine's particle
// emitter object calls them (particles.h): make an emitter, spawn, move, draw.
// spawns.cpp's spawnExplosion starts the explosion and its debris; the starts
// are in particle_draws.cpp.

#include "aggressive/conversions.h"
#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/render/draw.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/collision/collision.h"
#include "zge/scene/camera.h"
#include "zge/math/random.h"
#include "zge/container/cursor_array.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/fx/explosions.cpp.h"

namespace zge
{
    struct Camera;
}

namespace aggressive::fx
{
    namespace
    {
        // What an impact ring starts from ("impactRing": the named effect's
        // parameters; with none, the defaults at impactRingDefaults: size 1,
        // growth 0.032).
        struct ImpactRingParameters
        {
            zge::Vec4 position;          // +0x00
            zge::Vec4 velocity;          // +0x10
            zge::Quaternion orientation; // +0x20 the ring's turn: it lies in its x-z plane
            ee::Float size;              // +0x30 half the ring's side
            ee::Float growth;            // +0x34 its size times this, a millisecond of age, added to it
            ee::Float unread;            // +0x38 copied to the ring; nothing reads it (0.048 by default)
            uint32_t _3c;
        };
        static_assert(offsetof(ImpactRingParameters, orientation) == 0x20 && offsetof(ImpactRingParameters, size) == 0x30);
        static_assert(offsetof(ImpactRingParameters, unread) == 0x38 && sizeof(ImpactRingParameters) == 0x40);

        // An impact ring (0xC0 bytes): a square on its frame's x-z plane.
        struct ImpactRing : Particle
        {
            zge::Matrix frame;           // +0x70 the parameters' turn, at its position when spawned
            ee::Float growth;            // +0xB0
            ee::Float unread;            // +0xB4
            uint8_t _b8[8];
        };
        static_assert(offsetof(ImpactRing, frame) == 0x70 && offsetof(ImpactRing, growth) == 0xB0 && sizeof(ImpactRing) == 0xC0);

        // What an explosion starts from ("explosion": the named effect's
        // parameters; the defaults at explosionDefaults): its puffs come out
        // round the position and with the velocity, each within its spread
        // either way.
        struct ExplosionParameters
        {
            zge::Vec4 position;          // +0x00
            zge::Vec4 positionSpread;    // +0x10
            zge::Vec4 velocity;          // +0x20
            zge::Vec4 velocitySpread;    // +0x30
            ee::Float size;              // +0x40 a puff's half side when it starts
            ee::Float growth;            // +0x44 its size times this, a millisecond of age, added to it
            ee::Float unread;            // +0x48 copied to each puff; nothing reads it (8 by default)
            uint32_t lifetime;           // +0x4C milliseconds: each puff lives from half of it up to it
        };
        static_assert(offsetof(ExplosionParameters, size) == 0x40 && offsetof(ExplosionParameters, lifetime) == 0x4C);

        // A puff of an explosion (0x80 bytes).
        struct ExplosionPuff : Particle
        {
            ee::Float growth;            // +0x70
            ee::Float unread;            // +0x74
            uint8_t _78[8];
        };
        static_assert(offsetof(ExplosionPuff, growth) == 0x70 && sizeof(ExplosionPuff) == 0x80);

        // What debris starts from ("debris": the named effect's parameters; the
        // defaults at debrisDefaults): as an explosion's, and the floor the
        // pieces bounce off.
        struct DebrisParameters
        {
            zge::Vec4 position;          // +0x00
            zge::Vec4 positionSpread;    // +0x10
            zge::Vec4 velocity;          // +0x20
            zge::Vec4 velocitySpread;    // +0x30
            ee::Float unread;            // +0x40 nothing reads it (0.1 by default)
            uint32_t lifetime;           // +0x44 milliseconds: each piece lives from half of it up to it
            ee::Float floorHeight;       // +0x48 (y down) where the pieces bounce
        };
        static_assert(offsetof(DebrisParameters, lifetime) == 0x44 && offsetof(DebrisParameters, floorHeight) == 0x48);

        // A piece of debris (0x80 bytes): an instance of the debris model.
        struct DebrisPiece : Particle
        {
            ee::Ptr<zge::Node> node;     // +0x70
            ee::Float floorHeight;       // +0x74
            uint8_t _78[8];
        };
        static_assert(offsetof(DebrisPiece, node) == 0x70 && offsetof(DebrisPiece, floorHeight) == 0x74 && sizeof(DebrisPiece) == 0x80);

        // A quad for the primitive API: its texture and its corners' vertices,
        // in strip order.
        struct QuadList
        {
            uint32_t texture;
            ee::Ptr<ee::Ptr<QuadVertex>> corners;
            uint32_t cornerCount;
        };
        static_assert(sizeof(QuadList) == 0x0C);

        // An impact ring and an explosion's puff start white at a quarter and
        // at half alpha.
        inline ee::Global<uint8_t[4], sym::flameColour> g_quarterWhite;
        inline ee::Global<uint8_t[4], sym::explosionPuffColour> g_halfWhite;
        constexpr uint32_t kImpactRingLifetime = 230;
        // The ring's alpha: 64 until its last 230 ms, then down to nothing.
        constexpr uint8_t kImpactRingAlpha = 0x40;
        constexpr float kImpactRingFade = 64.0f / 230.0f;
        // An explosion's colours over a puff's life, in sixteen steps.
        inline ee::Global<uint8_t[16][4], sym::g_explosionColours> g_explosionColours;
        constexpr uint32_t kExplosionSteps = 16;

        // The model the debris pieces are instances of (0: no debris).
        inline ee::Global<uint32_t, sym::g_debrisObject> g_debrisObject;
        constexpr float kDebrisGravity = 60.0f;  // units a second, each second
        constexpr float kDebrisBounce = -0.6f;   // the fall's speed kept, turned up, at the floor
        // A piece spins 500, 1000, 1500 or 2000 on each axis a tick, and is
        // 0.05 to 0.2 of the model's size.
        constexpr int32_t kDebrisSpinStep = 500;
        constexpr float kDebrisScaleStep = 0.05f;
    }

    namespace
    {
        // The engine's own, through the game's table (so a hook or the replay
        // answers them as the original's calls).
        uint32_t cameraObject(uint32_t camera) { return zge::cameraObject(ee::Ptr<zge::Camera>(camera)); }

        // A particle from the emitter's pool, while the effects' budget allows
        // one more; 0 when not.
        uint32_t newParticle(ee::Ptr<Emitter> emitter)
        {
            if (*g_particlesAlive >= *g_particleBudget)
                return 0;
            *g_particlesAlive = *g_particlesAlive + 1;
            return zge::allocateSized(emitter->particleBytes);
        }

        // A random part of a spread: -1 up to 1 in 128 steps.
        ee::Float randomSpread()
        {
            return ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0xFF) - 0x80)) * ee::Float(1.0f / 128.0f);
        }

        // A lifetime from half of `lifetime` up to it, at random (one under 2
        // divides by zero, as the original's does).
        uint32_t randomLifetime(uint32_t lifetime)
        {
            const uint32_t random = zge::randomNumber();
            const uint32_t half = lifetime >> 1;
            return half + random % half;
        }

        // The class's emitter (its +0x10): room for `capacity` particles of
        // `particleBytes`.
        ee::Ptr<Emitter> newEmitter(uint32_t effectClass, uint32_t capacity, uint32_t particleBytes)
        {
            const ee::Ptr<Emitter> emitter(zge::heapAllocate(sizeof(Emitter)));
            if (emitter)
            {
                emitter->particles = zge::newCursorArray(capacity & 0xFFFF);
                emitter->effectClass = effectClass;
                emitter->particleBytes = particleBytes;
                emitter->lastMoved = static_cast<uint32_t>(effectsClock());
            }
            return emitter;
        }

        // The four corners' vertices of a square, in the order the strip
        // draws them (the vertices are made round the square).
        void listCorners(QuadList &quad, ee::Ptr<ee::Ptr<QuadVertex>> corners, ee::Ptr<QuadVertex> vertices)
        {
            quad.corners = corners;
            quad.cornerCount = 4;
            corners[0] = vertices;
            corners[1] = vertices + 1;
            corners[2] = vertices + 3;
            corners[3] = vertices + 2;
        }
    }

    // ---- "impactRing" -------------------------------------------------------

    ee::Ptr<Emitter> newImpactRingEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newEmitter(effectClass, capacity, sizeof(ImpactRing));
    }
    READABLE_FUNCTION_WAITS(newImpactRingEmitter, Verified);

    // The spawn (+0x18): one ring, whatever the count, at the parameters'
    // position, turned by their orientation; it lives 230 ms.
    void spawnImpactRings(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t /*count*/, uint32_t,
                           ee::Ptr<ImpactRingParameters> parameters)
    {
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        ee::Local<uint8_t[4]> colour;
        std::memcpy(&colour[0], *g_quarterWhite, sizeof(uint8_t[4]));
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<ImpactRing> ring(newParticle(emitter));
        if (!ring)
            return;
        ring->born = clock;
        ring->flags = kParticleMoves;
        ring->texture = *g_impactRingTexture;
        ring->size = parameters->size;
        std::memcpy(ring->colour, &colour[0], sizeof(ring->colour));
        ring->lifetime = kImpactRingLifetime;
        ring->tail = parameters->position;
        ring->position = ring->tail;
        ring->velocity = parameters->velocity;
        zge::matrixIdentity(&ring->frame);
        zge::quaternionToMatrix(&ring->frame, &parameters->orientation);
        zge::matrixClearTranslation(&ring->frame);
        ring->frame.translation() = ring->position;
        ring->growth = parameters->growth;
        ring->unread = parameters->unread;
        zge::pointerArrayAdd(emitter->particles, ring);
    }
    READABLE_FUNCTION_WAITS(spawnImpactRings, Verified);

    // The draw (+0x20): each ring a textured square on its plane, growing with
    // its age; its alpha 64 until its last 230 ms, then fading out. Returns
    // the rings drawn.
    int32_t drawImpactRings(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        ee::Local<QuadList> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<ee::Ptr<QuadVertex>[4]> corners;
        ee::Local<zge::Vec4> corner;  // in the ring's frame
        ee::Local<uint8_t[4]> colour;
        listCorners(*quad, corners, vertices);
        int32_t drawn = 0;
        for (uint32_t item = first(*emitter->particles); item; item = next(*emitter->particles))
        {
            ImpactRing &ring = *ee::Ptr<ImpactRing>(item);
            std::memcpy(&colour[0], ring.colour, sizeof(ring.colour));
            const uint32_t age = clock - ring.born;
            const int32_t alpha = ee::toInt(unsignedToFloat(ring.lifetime - age) * ee::Float(kImpactRingFade));
            colour[3] = alpha <= kImpactRingAlpha ? static_cast<uint8_t>(alpha) : kImpactRingAlpha;
            const ee::Float half = ring.size + ring.size * (ring.growth * ee::toFloat(static_cast<int32_t>(age)));
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = vertices[i];
                std::memcpy(vertex.colour, &colour[0], sizeof(vertex.colour));
                const bool left = (i + 1) & 2; // corners 1 and 2
                const bool far = i & 2;        // corners 2 and 3
                vertex.u = ee::Float(left ? 1.0f : 0.0f);
                vertex.v = ee::Float(far ? 1.0f : 0.0f);
                corner->x = left ? -half : half;
                corner->z = far ? half : -half;
                corner->y = ee::Float(0.0f);
                vertex.position = zge::transformed(ring.frame, *corner);
            }
            quad->texture = ring.texture;
            zge::drawPrimitives(kWorldPrimitive, quad, 1, 0);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawImpactRings, Verified);

    // ---- "explosion" --------------------------------------------------------

    ee::Ptr<Emitter> newExplosionEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newEmitter(effectClass, capacity, sizeof(ExplosionPuff));
    }
    READABLE_FUNCTION_WAITS(newExplosionEmitter, Verified);

    // The spawn (+0x18): `count` puffs round the position, flung out with the
    // velocity, each within the spreads, as long as the budget lasts.
    void spawnExplosionParticle(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                                 ee::Ptr<ExplosionParameters> explosion)
    {
        const uint32_t wanted = count & 0xFF;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        ee::Local<uint8_t[4]> colour;
        std::memcpy(&colour[0], *g_halfWhite, sizeof(uint8_t[4]));
        if (!effectClass || !emitter)
            return;
        for (uint32_t made = 0; made < wanted;)
        {
            const ee::Ptr<ExplosionPuff> puff(newParticle(emitter));
            if (!puff)
                return;
            puff->born = clock;
            puff->flags = kParticleMoves;
            puff->texture = *g_explosionTexture;
            puff->size = explosion->size;
            std::memcpy(puff->colour, &colour[0], sizeof(puff->colour));
            ++made;
            puff->lifetime = randomLifetime(explosion->lifetime);
            puff->tail.x = explosion->position.x + explosion->positionSpread.x * randomSpread();
            puff->tail.y = explosion->position.y + explosion->positionSpread.y * randomSpread();
            puff->tail.z = explosion->position.z + explosion->positionSpread.z * randomSpread();
            puff->position = puff->tail;
            puff->velocity.x = explosion->velocity.x + explosion->velocitySpread.x * randomSpread();
            puff->velocity.y = explosion->velocity.y + explosion->velocitySpread.y * randomSpread();
            puff->velocity.z = explosion->velocity.z + explosion->velocitySpread.z * randomSpread();
            puff->growth = explosion->growth;
            puff->unread = explosion->unread;
            zge::pointerArrayAdd(emitter->particles, puff);
        }
    }
    READABLE_FUNCTION_WAITS(spawnExplosionParticle, Verified);

    // The draw (+0x20): each puff a square facing the camera, growing with its
    // age, in the colour of its step of sixteen through its life. Returns the
    // puffs drawn.
    int32_t drawExplosion(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Matrix> facing;   // the camera's turn, at the puff
        ee::Local<QuadList> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<ee::Ptr<QuadVertex>[4]> corners;
        ee::Local<zge::Vec4> corner;     // in the facing frame
        ee::Local<uint8_t[4]> colour;
        int32_t drawn = 0;
        effectsClock(); // asked twice; the first answer goes unused
        listCorners(*quad, corners, vertices);
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        for (uint32_t item = first(*emitter->particles); item; item = next(*emitter->particles))
        {
            ExplosionPuff &puff = *ee::Ptr<ExplosionPuff>(item);
            const int32_t age = static_cast<int32_t>(clock - puff.born);
            const uint32_t step = static_cast<uint32_t>(ee::toInt(ee::toFloat(age) * ee::Float(15.0f) / ee::toFloat(static_cast<int32_t>(puff.lifetime))));
            std::memcpy(&colour[0], (*g_explosionColours)[step < kExplosionSteps ? step : kExplosionSteps - 1], sizeof(uint8_t[4]));
            const ee::Float half = puff.size + puff.size * (puff.growth * ee::toFloat(age));
            quad->texture = puff.texture;
            *facing = zge::aligned(zge::objectMatrix(ee::Ptr<zge::SceneObject>(cameraObject(*zge::g_currentCamera)), 1));
            facing->translation() = puff.position;
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = vertices[i];
                std::memcpy(vertex.colour, &colour[0], sizeof(vertex.colour));
                const bool left = (i + 1) & 2; // corners 1 and 2
                const bool top = i & 2;        // corners 2 and 3
                vertex.u = ee::Float(left ? 0.0f : 1.0f);
                vertex.v = ee::Float(top ? 1.0f : 0.0f);
                corner->x = left ? -half : half;
                corner->y = top ? half : -half;
                corner->z = ee::Float(0.0f);
                vertex.position = zge::transformed(*facing, *corner);
            }
            zge::drawPrimitives(kWorldPrimitive, quad, 1, 0);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawExplosion, Verified);

    // ---- "debris" -----------------------------------------------------------

    ee::Ptr<Emitter> newDebrisEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newEmitter(effectClass, capacity, sizeof(DebrisPiece));
    }
    READABLE_FUNCTION_WAITS(newDebrisEmitter, Verified);

    // The spawn (+0x18): `count` pieces, each an instance of the debris model
    // (shown, not colliding), thrown as an explosion's puffs are, spinning at
    // random and scaled 0.05 to 0.2. Nothing while there is no debris model.
    void spawnDebris(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                      ee::Ptr<DebrisParameters> debris)
    {
        const uint32_t wanted = count & 0xFF;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (!*g_debrisObject || !effectClass || !emitter)
            return;
        ee::Local<zge::Matrix> place; // the piece's scale and place
        const auto randomSpin = [] { return static_cast<zge::Angle>((zge::randomNumber() & 3) * kDebrisSpinStep + kDebrisSpinStep); };
        for (uint32_t made = 0; made < wanted;)
        {
            const ee::Ptr<DebrisPiece> piece(newParticle(emitter));
            if (!piece)
                return;
            piece->born = clock;
            piece->texture = 0;
            piece->flags = kParticleMoves;
            piece->lifetime = randomLifetime(debris->lifetime);
            piece->tail.x = debris->position.x + debris->positionSpread.x * randomSpread();
            piece->tail.y = debris->position.y + debris->positionSpread.y * randomSpread();
            piece->tail.z = debris->position.z + debris->positionSpread.z * randomSpread();
            piece->position = piece->tail;
            piece->velocity.x = debris->velocity.x + debris->velocitySpread.x * randomSpread();
            piece->velocity.y = debris->velocity.y + debris->velocitySpread.y * randomSpread();
            piece->velocity.z = debris->velocity.z + debris->velocitySpread.z * randomSpread();
            piece->spin.x = randomSpin();
            piece->spin.y = randomSpin();
            piece->spin.z = randomSpin();
            piece->node = ee::Ptr<zge::Node>(zge::newAppInstanceNode(*g_debrisObject, 1));
            zge::setNodeShown(piece->node, 1, 1);
            zge::setNodeCollides(piece->node, 0, 0);
            zge::matrixIdentity(place);
            place->translation() = piece->position;
            const ee::Float scale = ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 3)) * ee::Float(kDebrisScaleStep) + ee::Float(kDebrisScaleStep);
            ++made;
            zge::scaleMatrixRotation(place, place, scale);
            zge::setNodeLocalMatrix(piece->node, place);
            piece->floorHeight = debris->floorHeight;
            zge::pointerArrayAdd(emitter->particles, piece);
        }
    }
    READABLE_FUNCTION_WAITS(spawnDebris, Verified);

    // The move (+0x1C), once a clock tick at most: pieces whose life is over
    // go (their node released); the others fall under gravity, step by their
    // velocity, bounce off the floor on the way down, turn by their spin and
    // put their node there. Returns how many pieces it looked at.
    uint32_t moveDebris(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        uint32_t looked = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (!emitter || !emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        ee::Local<zge::Vec4> step; // this tick's
        uint32_t item = first(*emitter->particles);
        while (item)
        {
            DebrisPiece &piece = *ee::Ptr<DebrisPiece>(item);
            if (clock - piece.born >= piece.lifetime)
            {
                zge::releaseNode(piece.node, 0);
                zge::pointerArrayRemoveCurrent(emitter->particles);
                freeParticle(emitter, item);
                item = zge::current(emitter->particles);
            }
            else
            {
                piece.velocity.y = piece.velocity.y + *g_tickTime * ee::Float(kDebrisGravity);
                piece.tail = piece.position;
                *step = piece.velocity * *g_tickTime;
                piece.position = piece.position + *step;
                if (piece.floorHeight <= piece.position.y && ee::Float(0.0f) < piece.velocity.y)
                    piece.velocity.y = piece.velocity.y * ee::Float(kDebrisBounce);
                const ee::Ptr<zge::Matrix> place = zge::nodeMatrix(piece.node, 0);
                zge::matrixRotateXYZ(place, place, &piece.spin);
                place->translation() = piece.position;
                zge::setNodeLocalMatrix(piece.node, place);
                item = next(*emitter->particles);
            }
            ++looked;
        }
        emitter->lastMoved = clock;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveDebris, Verified);
}
