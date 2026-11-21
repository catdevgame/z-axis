// BMX XXX's own forms of two of the particle kinds' functions (ZGE4's; ZGE3's are zge/scene/particle_kinds.cpp
// and particle_emitter.cpp): the spark type's spawn, which starts each spark from the emitter's own matrix and
// spawns nothing while the particles are off, and the billboard kind's shut-down, which has no pool to destroy.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/scene/particle_types.h"

#include <cstdint>
#include "sym/bmx/fx/particle_kinds.cpp.h"

namespace game::bmx
{
    namespace
    {
        // Set while the particles are off: the spawns make none.
        inline ee::Global<uint32_t, sym::g_particlesOff> g_particlesOff;
        // The sparks' pool, and the look a spark of an object with no emitter data gets.
        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_sparkPool> g_sparkPool;
        constexpr uint32_t kSparkDefaults = sym::g_sparkDefaults;
        constexpr uint32_t kSparksByte = 0xFFu; // the spawn's count is its low byte
    }

    // emitSparksAtEmitterMatrix: BMX XXX's emitSparks (the spark type's
    // spawn): nothing while the particles are off, or without a type or an
    // effect; else count sparks (its low byte) at the emitter, each started
    // from the emitter's matrix (emitterMatrixOf: its translation the spawn
    // point's origin, the matrix its velocity's frame) with the look's colour,
    // size, life and material (the type's defaults for an object with no
    // data). 0.
    uint32_t emitSparksAtEmitterMatrix(ee::Ptr<zge::ParticleType> type, ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::ParticleEffect> effect,
                                       uint32_t count, ee::Ptr<zge::ParticleEmitterData> data)
    {
        if (*g_particlesOff != 0)
            return 0;
        const uint32_t sparks = count & kSparksByte;
        const ee::Ptr<zge::ParticleLook> look = data ? data->settings->look : ee::Ptr<zge::ParticleLook>(kSparkDefaults);
        if (!type || !effect)
            return 0;
        const ee::Ptr<zge::Matrix> frame = zge::emitterMatrixOf(object);
        for (uint32_t made = 0; made < sparks;)
        {
            const ee::Ptr<zge::Spark> spark(zge::poolAlloc(*g_sparkPool));
            spark->age = 0.0f;
            spark->colour = look->colour;
            zge::particleSpawnPoint(&spark->position, &frame->translation(), object);
            spark->previous = spark->position;
            ++made;
            zge::particleSpawnVelocity(&spark->velocity, frame, object);
            spark->size = look->size;
            spark->life = look->life;
            spark->material = look->material;
            zge::listPush(&effect->particles, &spark->node);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(emitSparksAtEmitterMatrix, Verified);

    // unregisterBillboardParticles: BMX XXX's (shutdownParticleTypes, between
    // the sparks' and the polys'): nothing, its billboard particles having no
    // pool (its registerBillboardParticles makes none).
    void unregisterBillboardParticles() {}
    READABLE_FUNCTION(unregisterBillboardParticles, Verified);
}
