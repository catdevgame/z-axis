// The ripples ("ripple"; inlineFx.c, were the sweep's drafts of startRipples ..
// drawRipples): flat rings on the water that grow and fade, the ones a skater
// makes in the water following it. The start (three rings at once), the
// class's emitter, spawn, move and draw.
//

#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/skater/skater.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/math/vec.h"
#include "zge/render/draw.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"

#include <cstring>
#include "sym/aggressive/fx/ripples.cpp.h"

namespace aggressive::fx
{

    namespace
    {
        constexpr uint32_t kListAppend = sym::pointerArrayAdd;    // (CursorArray *, item)
        constexpr uint32_t kWorldPrimitive = 2;          // the primitive API's kind: in the world

        constexpr uint32_t kRippleDefaults = sym::rippleDefaults;

        // A ripple (0x90 bytes).
        struct Ripple : Particle
        {
            ee::Float growth;        // +0x70 size added a second
            ee::Ptr<skater::Player> follows; // +0x74 the skater it follows (0: none)
            uint8_t _78[8];
            zge::Vec4 followedAt;    // +0x80 the player's position the last tick
        };
        static_assert(offsetof(Ripple, growth) == 0x70 && offsetof(Ripple, follows) == 0x74);
        static_assert(offsetof(Ripple, followedAt) == 0x80 && sizeof(Ripple) == 0x90);
        constexpr uint32_t kRippleBytes = sizeof(Ripple);

        // The start's three rings: each the next one's size, and the next
        // one higher (y down) by its own size step.
        inline ee::Global<ee::Float, sym::g_firstRippleSize> g_firstRippleSize;   // 0.3
        inline ee::Global<ee::Float, sym::g_secondRippleRise> g_secondRippleRise;  // 0.05
        inline ee::Global<ee::Float, sym::g_secondRippleSize> g_secondRippleSize;  // 0.2
        inline ee::Global<ee::Float, sym::g_thirdRippleSize> g_thirdRippleSize;   // 0.1, also the third's rise

        // The primitive API's vertex and quad (the ripple's own frame layout).
        struct RippleVertex
        {
            ee::Float x, y, z;
            uint32_t _0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(RippleVertex) == 0x20 && offsetof(RippleVertex, colour) == 0x18);
        struct RippleQuad
        {
            uint32_t texture;  // +0x00
            uint32_t vertices; // +0x04 -> four vertex addresses
            uint32_t count;    // +0x08
        };
    }

    // startRipples: a ripple effect (the named effect's start): with no
    // parameters, one ring of the defaults; else three rings of the given
    // ones, each later one longer lived, smaller, slower to grow and a
    // little higher.
    uint32_t startRipples(ee::Ptr<zge::SceneObject> object, ee::Ptr<RippleParameters> ripple)
    {
        if (!ripple)
            return game::tail(zge::startEffect, object, *g_rippleClass, 1, 0, kRippleDefaults);
        ripple->texture = *g_rippleTexture;
        ripple->lifetime = 1000;
        ripple->size = *g_firstRippleSize;
        ripple->growth = 2.5f;
        zge::startEffect(object, *g_rippleClass, 1, 0, ripple);
        ripple->lifetime = 1250;
        ripple->size = *g_secondRippleSize;
        ripple->growth = 2.0f;
        ripple->y = ripple->y - *g_secondRippleRise;
        zge::startEffect(object, *g_rippleClass, 1, 0, ripple);
        const ee::Float third = *g_thirdRippleSize;
        ripple->lifetime = 1500;
        ripple->growth = 1.5f;
        ripple->y = ripple->y - third;
        ripple->size = third;
        return game::tail(zge::startEffect, object, *g_rippleClass, 1, 0, ripple);
    }
    READABLE_FUNCTION_WAITS(startRipples, Verified);

    // newRippleEmitter ("ripple" +0x10): the class's emitter.
    uint32_t newRippleEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kRippleBytes, sym::newRippleEmitter);
    }
    READABLE_FUNCTION_WAITS(newRippleEmitter, Verified);

    // spawnRipple ("ripple" +0x18): one ripple (whatever the count) from
    // the parameters (the defaults when none), at the point following its
    // player's position when it has one.
    void spawnRipple(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<RippleParameters> given)
    {
        const int32_t clock = effectsClock();
        const ee::Ptr<RippleParameters> ripple = given ? given : ee::Ptr<RippleParameters>(kRippleDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<Ripple> ring = newParticle<Ripple>(*emitter);
        if (!ring)
            return;
        ring->born = static_cast<uint32_t>(clock);
        ring->flags = kParticleMoves;
        std::memcpy(ring->colour, ripple->colour, 4);
        ring->position.x = ripple->x;
        ring->position.y = ripple->y;
        ring->position.z = ripple->z;
        ring->size = ripple->size;
        ring->lifetime = ripple->lifetime;
        ring->texture = ripple->texture;
        ring->velocity = ripple->velocity;
        ring->growth = ripple->growth;
        ring->follows = ripple->follows;
        if (ring->follows)
            ring->followedAt = ring->follows->position;
        zge::pointerArrayAdd(emitter->particles, ring);
    }
    READABLE_FUNCTION_WAITS(spawnRipple, Verified);

    // moveRipples ("ripple" +0x1C): once a tick, ripples whose life is over
    // are freed; one following a skater (while its wallTapping is set) moves
    // on the level (x, z) by as much as the skater did; moving ones step by
    // their velocity; each grows. Returns how many it looked at.
    int32_t moveRipples(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t looked = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; ++looked)
            {
                Ripple &ring = *ee::Ptr<Ripple>(particle);
                if (clock - ring.born >= ring.lifetime)
                {
                    particle = freeCurrent(emitter, particle);
                    continue;
                }
                if (ring.follows && ring.follows->wallTapping)
                {
                    const ee::Float height = ring.position.y;
                    ring.position = ring.position - ring.followedAt;
                    ring.followedAt = ring.follows->position;
                    ring.position = ring.position + ring.followedAt;
                    ring.position.y = height;
                }
                if (ring.flags & kParticleMoves)
                {
                    ring.tail = ring.position;
                    step = ring.velocity * *g_tickTime;
                    ring.position = ring.position + *step;
                }
                ring.size = ring.size + ring.growth * *g_tickTime;
                particle = next(*emitter->particles);
            }
        }
        emitter->lastMoved = clock; // (with no emitter too: the original's store lands at 0x28)
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveRipples, Verified);

    // drawRipples ("ripple" +0x20): each ripple a square flat on the level
    // (x, z) of its size, its alpha fading over its last 2 seconds (at most
    // 0x80; one following a player over its last second, at most 0x40).
    // Returns how many it drew.
    int32_t drawRipples(uint32_t, ee::Ptr<Emitter> emitter)
    {
        namespace here = points::drawRipples;
        ee::Local<RippleQuad> quad;
        ee::Local<RippleVertex> plusXMinusZ;
        ee::Local<RippleVertex> minusXMinusZ;
        ee::Local<RippleVertex> minusXPlusZ;
        ee::Local<RippleVertex> plusXPlusZ;
        ee::Local<uint32_t> list;
        ee::Local<uint8_t[4]> colour;
        // The corners (+0x10, +0x30, +0x50, +0x70), listed at +0x90 in the
        // order +0x10, +0x30, +0x70, +0x50.
        int32_t drawn = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        quad->vertices = list.address();
        quad->count = 4;
        (&*list)[0] = plusXMinusZ.address();
        (&*list)[1] = minusXMinusZ.address();
        (&*list)[2] = plusXPlusZ.address();
        (&*list)[3] = minusXPlusZ.address();
        uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
        if (particle)
        {
            for (;;)
            {
                const Ripple &ring = *ee::Ptr<Ripple>(particle);
                std::memcpy(&colour[0], ring.colour, 4);
                const uint32_t left = ring.lifetime - (clock - ring.born);
                uint32_t alpha;
                if (ring.follows)
                    alpha = left < 0x400u ? left >> 4 : 0x40u;
                else
                {
                    const uint32_t kept = left >= 30001u ? 0u : left;
                    alpha = kept < 0x800u ? kept >> 4 : 0x80u;
                }
                colour[3] = static_cast<uint8_t>(alpha);
                std::memcpy(plusXPlusZ->colour, &colour[0], 4);
                plusXPlusZ->x = ring.position.x + ring.size;
                plusXPlusZ->y = ring.position.y;
                plusXPlusZ->u = 0.0f;
                plusXPlusZ->v = 0.0f;
                plusXPlusZ->z = ring.position.z + ring.size;
                std::memcpy(minusXPlusZ->colour, &colour[0], 4);
                minusXPlusZ->x = ring.position.x - ring.size;
                minusXPlusZ->y = ring.position.y;
                minusXPlusZ->u = 0.0f;
                minusXPlusZ->v = 1.0f;
                minusXPlusZ->z = ring.position.z + ring.size;
                std::memcpy(minusXMinusZ->colour, &colour[0], 4);
                minusXMinusZ->x = ring.position.x - ring.size;
                minusXMinusZ->y = ring.position.y;
                minusXMinusZ->z = ring.position.z - ring.size;
                minusXMinusZ->u = 1.0f;
                minusXMinusZ->v = 1.0f;
                std::memcpy(plusXMinusZ->colour, &colour[0], 4);
                plusXMinusZ->x = ring.position.x + ring.size;
                plusXMinusZ->y = ring.position.y;
                plusXMinusZ->u = 1.0f;
                plusXMinusZ->v = 0.0f;
                plusXMinusZ->z = ring.position.z - ring.size;
                quad->texture = ring.texture;
                zge::drawPrimitives(kWorldPrimitive, quad, 1, 0);
                particle = next(*emitter->particles, here::join2);
                ++drawn;
                if (!particle)
                    break;
            }
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawRipples, Verified);
}
