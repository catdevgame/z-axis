// BMX XXX's dust (dust.h). BMX XXX's own code (fx.c's block), DM2's line
// (games/dm2/fx/dust.cpp, particles.cpp's drawParticleBillboard).

#include "dust.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/dust.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The dust's sixteen materials: a puff starts with the first and is
        // drawn with the one its age picks. Cut: BMX XXX never loads them
        // (DM2 loads MatFxDust00 on; nothing in BMX XXX writes the table), so
        // every puff is drawn with material 0.
        constexpr uint32_t kDustMaterials = 16;
        inline ee::Global<uint32_t[kDustMaterials], sym::g_dustMaterials> g_dustMaterials;
        constexpr uint32_t kDustDefaults = sym::dustEffectDefaults;

        // How many particles are alive, and the effects' budget of them.
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;

        // A puff's colour: mid grey, a quarter seen.
        constexpr uint8_t kPuffGrey = 0x80, kPuffAlpha = 0x40;
        // A puff's look goes by the milliseconds it has left: the materials
        // run over the last second, the fade over the last quarter.
        constexpr int32_t kLookTime = 1000, kFadeTime = 250;
        constexpr int32_t kLastMaterial = 15;
        // drawPrimitiveQuads' flags for a billboard (the quad is in the world already).
        constexpr uint32_t kBillboardQuads = 0;
    }

    // spawnDustPuff ("dust" +0x18): one puff (whatever the count) from the
    // parameters (the defaults when none). Kept bug: with the effects'
    // budget of particles used up no puff is allocated, and the puff is
    // written at address 0 and added to the emitter, as the other spawns'.
    void spawnDustPuff(uint32_t effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<DustParams> given)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const ee::Ptr<DustParams> params = given ? given : ee::Ptr<DustParams>(kDustDefaults);
        if (!effectClass || !emitter)
            return;
        uint32_t made = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            made = zge::allocateSized(emitter->particleBytes);
        }
        const ee::Ptr<Particle> puff(made);
        puff->born = now;
        puff->flags = kParticleMoves;
        puff->lifetime = static_cast<uint32_t>(params->lifetime);
        puff->texture = (*g_dustMaterials)[0];
        puff->size = params->size;
        puff->colour[2] = kPuffGrey;
        puff->colour[3] = kPuffAlpha;
        puff->colour[0] = kPuffGrey;
        puff->colour[1] = kPuffGrey;
        puff->position.x = params->position.x;
        puff->tail.x = params->position.x;
        puff->position.y = params->position.y;
        puff->tail.y = params->position.y;
        puff->position.z = params->position.z;
        puff->tail.z = params->position.z;
        puff->velocity = params->velocity;
        game::tail(zge::pointerArrayAdd, emitter->particles, puff);
    }
    READABLE_FUNCTION_WAITS(spawnDustPuff, Draft);

    // drawDust ("dust" +0x20): each puff with the material of its age and
    // its alpha faded over its last quarter second. Returns how many it drew.
    int32_t drawDust(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<uint8_t[4]> colour;
        int32_t drawn = 0;
        const int32_t now = effectsClock();
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const ee::Ptr<Particle> puff(particle);
            std::memcpy(&colour[0], puff->colour, 4);
            int32_t left = static_cast<int32_t>(puff->lifetime) - (now - static_cast<int32_t>(puff->born));
            if (left > kLookTime)
                left = kLookTime;
            if (left < 0)
                left = 0;
            if (left < kFadeTime)
                colour[3] = static_cast<uint8_t>(static_cast<uint32_t>(puff->colour[3] * left) / static_cast<uint32_t>(kFadeTime));
            ++drawn;
            const uint32_t material = (*g_dustMaterials)[((kLookTime - left) * kLastMaterial) / kLookTime];
            drawParticleBillboard(puff, material, ee::Ptr<uint8_t>(&colour[0]));
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawDust, Draft);

    // drawParticleBillboard: the square round the particle in the camera's
    // x and y (right low, left low, left high, right high), through the
    // camera's matrix put at the particle.
    void drawParticleBillboard(ee::Ptr<Particle> particle, uint32_t material, ee::Ptr<uint8_t> colour)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> corners;
        ee::Local<zge::Matrix> facing; // the camera's matrix, placed at the particle
        ee::Local<zge::Vec4> corner;   // a corner in the camera's frame (its w is what the stack held)
        quad->texture = material;
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &corners[i];
        const ee::Ptr<zge::SceneObject> cameraObject(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)));
        *facing = *zge::objectMatrix(cameraObject, 1);
        facing->translation() = particle->position;
        for (uint32_t i = 0; i < 4; ++i)
        {
            QuadVertex &vertex = corners[i];
            const bool left = ((i + 1) & 2) != 0, high = (i & 2) != 0;
            std::memcpy(vertex.colour, &colour[0], 4);
            vertex.u = left ? 0.0f : 1.0f;
            vertex.v = high ? 1.0f : 0.0f;
            const ee::Float size = particle->size;
            corner->x = left ? -size : size;
            corner->y = high ? size : -size;
            corner->z = 0.0f;
            vertex.position = zge::transformed(*facing, *corner);
        }
        zge::drawPrimitiveQuads(kBillboardQuads, quad, 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawParticleBillboard, Draft);
}
