// BMX XXX's posters (posters.h). BMX XXX's own code (fx.c's block),
// Aggressive Inline's line (games/aggressive/fx/posters.cpp).

#include "posters.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/posters.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The effects' budget of particles and how many are alive (every emitter's together).
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        // A poster's parameters when none are given.
        constexpr uint32_t kPosterDefaults = sym::posterDefaults;
    }

    // spawnPoster ("poster" +0x18): one poster (whatever the count) on the
    // parameters' corners, at the first, still.
    // Kept bug: with the effects' budget of particles used up no poster is
    // allocated, and BMX XXX goes on to write it at address 0 and to add
    // that to the emitter (Aggressive Inline stops there).
    void spawnPoster(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t, uint32_t,
                     ee::Ptr<PosterParameters> given)
    {
        const int32_t clock = effectsClock();
        const ee::Ptr<PosterParameters> poster = given ? given : ee::Ptr<PosterParameters>(kPosterDefaults);
        if (!effectClass || !emitter)
            return;
        uint32_t particle = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            particle = zge::allocateSized(emitter->particleBytes);
        }
        const ee::Ptr<Poster> decal(particle);
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
        game::tail(zge::pointerArrayAdd, emitter->particles, decal);
    }
    READABLE_FUNCTION_WAITS(spawnPoster, Verified);

    namespace
    {
        // A poster's alpha for each millisecond it has left (it fades over
        // its last second), and the alpha it has before that.
        inline ee::Global<ee::Float, sym::g_posterAlphaPerMs> g_posterAlphaPerMs;
        constexpr int32_t kFullAlpha = 0xFF;

        // An unsigned count as a float, as the compiler converts one.
        ee::Float unsignedToFloat(uint32_t count)
        {
            if (static_cast<int32_t>(count) >= 0)
                return ee::toFloat(static_cast<int32_t>(count));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((count >> 1) | (count & 1)));
            return half + half;
        }
    }

    // drawPosters ("poster" +0x20): each poster a quad on its four corners
    // in its colour, its alpha its time left (full until its last second).
    // Returns how many it drew. DM2's, word for word but the walk.
    int32_t drawPosters(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<uint8_t[4]> colour;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        // The quad's corners are the vertices from the last back to the first.
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(vertices) + (3 - i);
        int32_t drawn = 0;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const ee::Ptr<Poster> poster(particle);
            std::memcpy(&colour[0], poster->colour, 4);
            const uint32_t left = poster->lifetime - (now - poster->born);
            const int32_t alpha = ee::toInt(unsignedToFloat(left) * *g_posterAlphaPerMs);
            colour[3] = static_cast<uint8_t>(alpha < kFullAlpha + 1 ? alpha : kFullAlpha);
            ++drawn;
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = vertices[i];
                std::memcpy(vertex.colour, &colour[0], 4);
                vertex.u = ((i + 1) & 2) != 0 ? 1.0f : 0.0f;
                vertex.v = (i & 2) != 0 ? 1.0f : 0.0f;
                vertex.position = poster->corners[i];
            }
            quad->texture = poster->texture;
            zge::drawPrimitiveQuads(0, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawPosters, Verified);
}
