// DM2's "poster" effect class (posters.h). DM2's own code (fx.c).

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
#include "sym/dm2/fx/posters.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kPosterEffectDefaults = sym::posterEffectDefaults;

        // A poster's alpha for each millisecond it has left (0.255: it fades
        // over its last second), and the alpha it has before that.
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

    void spawnPoster(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<PosterParams> params)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<PosterParams>(kPosterEffectDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<Poster> poster(zge::poolAlloc(emitter->pool));
        poster->born = now;
        poster->flags = 0; // it neither moves nor spins
        poster->lifetime = static_cast<uint32_t>(params->lifetime);
        for (int i = 0; i < 4; ++i)
            poster->colour[i] = params->colour[i];
        poster->size = 0.0f;
        poster->material = params->material;
        poster->velocity = zge::kZero;
        poster->position = params->corners[0];
        for (int i = 0; i < 4; ++i)
            poster->corners[i] = params->corners[i];
        game::tail(zge::pointerArrayAdd, emitter->particles, poster);
    }
    READABLE_FUNCTION_WAITS(spawnPoster, Verified);

    int32_t drawPosters(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<uint8_t[4]> colour;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        // The quad's corners are the vertices from the last back to the first.
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(vertices) + (3 - i);
        int32_t drawn = 0;
        for (ee::Ptr<Poster> poster(zge::arrayFirst(emitter->particles)); poster; poster = ee::Ptr<Poster>(zge::arrayNext(emitter->particles)))
        {
            for (int i = 0; i < 4; ++i)
                colour[i] = poster->colour[i];
            // Its alpha is its time left, whatever its colour's says.
            const uint32_t left = poster->lifetime - (now - poster->born);
            const int32_t alpha = ee::toInt(unsignedToFloat(left) * *g_posterAlphaPerMs);
            colour[3] = static_cast<uint8_t>(alpha < kFullAlpha + 1 ? alpha : kFullAlpha);
            ++drawn;
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = vertices[i];
                for (int byte = 0; byte < 4; ++byte)
                    vertex.colour[byte] = colour[byte];
                vertex.u = ((i + 1) & 2) != 0 ? 1.0f : 0.0f;
                vertex.v = (i & 2) != 0 ? 1.0f : 0.0f;
                vertex.position = poster->corners[i];
            }
            quad->material = poster->material;
            zge::drawPrimitiveQuads(0, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawPosters, Verified);
}
