// The posters ("poster"; inlineFx.c, were the sweep's drafts of startPoster ..
// drawPosters): a textured quad on four given corners (a decal), fading out
// over its last second; all of them cleared at once when the level asks
// (g_clearPosters). The start, the class's emitter, spawn, move and draw.
//

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/scene/particle_types.h"

#include <cstring>
#include "sym/aggressive/fx/posters.cpp.h"

namespace aggressive::fx
{
    namespace
    {

        constexpr uint32_t kPosterDefaults = sym::posterDefaults;

        // A poster (0xB0 bytes): its corners.
        struct Poster : Particle
        {
            zge::Vec4 corners[4];  // +0x70
        };
        static_assert(offsetof(Poster, corners) == 0x70 && sizeof(Poster) == 0xB0);

        // Set: every poster freed at the next move (the level's, as the fire
        // trails' g_clearFiretrails).
        inline ee::Global<uint32_t, sym::g_clearPosters> g_clearPosters;
        // Alpha a millisecond of the time a poster has left (0.255: full
        // until its last second).
        inline ee::Global<ee::Float, sym::g_posterFade> g_posterFade;
    }

    // startPoster: a poster (the named effect's start; the zero
    // parameters when none are given).
    uint32_t startPoster(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        const ee::Ptr<zge::EffectClass> posterClass = *g_posterClass;
        if (!parameters)
            parameters = kPosterDefaults;
        return game::tail(zge::startEffect, object, posterClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startPoster, Verified);

    // newPosterEmitter ("poster" +0x10): the class's emitter.
    uint32_t newPosterEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, sizeof(Poster), sym::newPosterEmitter);
    }
    READABLE_FUNCTION_WAITS(newPosterEmitter, Verified);

    // spawnPoster ("poster" +0x18): one poster (whatever the count) on the
    // parameters' corners, at the first, still.
    void spawnPoster(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<PosterParameters> given)
    {
        const int32_t clock = effectsClock();
        const ee::Ptr<PosterParameters> poster = given ? given : ee::Ptr<PosterParameters>(kPosterDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<Poster> decal = newParticle<Poster>(*emitter);
        if (!decal)
            return;
        decal->born = static_cast<uint32_t>(clock);
        decal->flags = 0;
        decal->lifetime = poster->lifetime;
        std::memcpy(decal->colour, poster->colour, 4);
        decal->size = 0.0f;
        decal->texture = poster->texture;
        decal->velocity = zge::kZero;
        decal->position = poster->corners[0];
        for (uint32_t i = 0; i < 4; ++i)
            decal->corners[i] = poster->corners[i];
        zge::pointerArrayAdd(emitter->particles, decal);
    }
    READABLE_FUNCTION_WAITS(spawnPoster, Verified);

    // movePosters ("poster" +0x1C): when the level asks, every poster
    // freed (the request cleared); else once a tick the move the classes
    // share (moveParticles' code: life over, the step by the velocity, the
    // effect's affectors). Returns how many it freed or looked at.
    int32_t movePosters(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t counted = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (*g_clearPosters)
        {
            if (emitter && emitter->particles && emitter->lastMoved < clock)
            {
                for (uint32_t particle = first(*emitter->particles); particle; ++counted)
                    particle = freeCurrent(emitter, particle);
                emitter->lastMoved = clock;
            }
            *g_clearPosters = 0;
            return counted;
        }
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++counted)
        {
            Particle &p = *ee::Ptr<Particle>(particle);
            if (clock - p.born >= p.lifetime)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            if (p.flags & kParticleMoves)
            {
                p.tail = p.position;
                step = p.velocity * *g_tickTime;
                p.position = p.position + *step;
            }
            particle = next(*emitter->particles);
        }
        runAffectors(effect, emitter);
        emitter->lastMoved = clock;
        return counted;
    }
    READABLE_FUNCTION_WAITS(movePosters, Verified);

    // drawPosters ("poster" +0x20): each poster its quad on its corners,
    // its alpha fading over its last second. Returns how many it drew.
    int32_t drawPosters(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<uint8_t[4]> colour;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        listCorners(quad);
        int32_t drawn = 0;
        uint32_t particle = first(*emitter->particles);
        // The game reads the fade time only once there is a poster.
        const ee::Float rate = particle ? *g_posterFade : ee::Float(0.0f);
        for (; particle; particle = next(*emitter->particles))
        {
            const Poster &decal = *ee::Ptr<Poster>(particle);
            std::memcpy(&colour[0], decal.colour, 4);
            const uint32_t left = decal.lifetime - (clock - decal.born);
            colour[3] = fadedAlpha(left, rate, rate, 255);
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &corner = quad->corners[i];
                std::memcpy(corner.colour, &colour[0], 4);
                corner.u = ((i + 1) & 2u) ? 1.0f : 0.0f;
                corner.v = (i & 2u) ? 1.0f : 0.0f;
                corner.position = decal.corners[i];
            }
            quad->record.texture = decal.texture;
            zge::drawPrimitives(kWorldPrimitive, &quad->record, 1, 0);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawPosters, Verified);
}
