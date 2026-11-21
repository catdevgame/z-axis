// BMX XXX's sleep marks (sleep.h). BMX XXX's own code (fx.c's block), DM2's
// line (games/dm2/fx/sleep.cpp), with BMX XXX's particle budget.

#include "sleep.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/sleep.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kSleepDefaults = sym::sleepEffectDefaults;
        // The marks' two materials (library.cpp loads them); a mark is drawn
        // with the first.
        inline ee::Global<uint32_t, sym::g_sleepMaterials> g_firstSleepMaterial;
        // How many marks have been spawned: each is turned a seventh of a
        // turn on from the one before.
        inline ee::Global<uint16_t, sym::g_sleepMarksSpawned> g_sleepMarksSpawned;
        constexpr uint32_t kSeventhOfATurn = 0x2492;
        constexpr ee::Float kTurnsPerAngleUnit = 1.0f / 65536.0f;
        constexpr ee::Float kAngleUnitsPerTurn = 65536.0f;
        // A mark's rise a move (y is down).
        constexpr ee::Float kSleepRise = -0.0023333335f;

        // How many particles are alive, and the effects' budget of them.
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;

        // The draw: the grey a new mark has; its sways a life; how far out
        // it sways (from the start to the end of its life, scaled); its half
        // width (the same way).
        constexpr ee::Float kNewMarkGrey = 63.0f;
        constexpr ee::Float kSwaysALife = 4.0f;
        constexpr ee::Float kSwayGrowth = 0.7f, kSwayStart = 0.3f, kSwayScale = 0.2f;
        constexpr ee::Float kWidthGrowth = 0.85f, kWidthStart = 0.15f, kWidthScale = 0.1f;
        // drawPrimitiveQuads' flags: the quad is in the world already.
        constexpr uint32_t kPlacedQuads = 0;

        // A part of a turn as an angle.
        zge::Angle angleOfTurns(ee::Float turns)
        {
            return static_cast<zge::Angle>(ee::toInt(turns * kAngleUnitsPerTurn));
        }
    }

    // spawnSleep ("sleep" +0x18): one mark (whatever the count) from the
    // parameters (the defaults when none). Kept bug: with the effects'
    // budget of particles used up no mark is allocated, and the mark is
    // written at address 0 and added to the emitter, as the other spawns'.
    void spawnSleep(uint32_t effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<SleepParameters> given)
    {
        const ee::Ptr<SleepParameters> params = given ? given : ee::Ptr<SleepParameters>(kSleepDefaults);
        if (!effectClass || !emitter)
            return;
        uint32_t made = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            made = zge::allocateSized(emitter->particleBytes);
        }
        const ee::Ptr<SleepMark> mark(made);
        mark->born = static_cast<uint32_t>(effectsClock());
        mark->flags = kParticleMoves;
        mark->lifetime = params->lifetime;
        mark->texture = *g_firstSleepMaterial;
        mark->vHigh = 1.0f;
        mark->uHigh = 1.0f;
        mark->uLow = 0.0f;
        mark->vLow = 0.0f;
        mark->position = params->position;
        mark->tail = mark->position;
        const uint32_t spawned = *g_sleepMarksSpawned;
        mark->velocity.x = 0.0f;
        mark->velocity.y = kSleepRise;
        mark->velocity.z = 0.0f;
        // Its turn: sevenths of a turn, one more each mark (kept in the size field).
        mark->size = ee::toFloat(static_cast<int16_t>(spawned * kSeventhOfATurn)) * kTurnsPerAngleUnit;
        zge::pointerArrayAdd(emitter->particles, mark);
        *g_sleepMarksSpawned = static_cast<uint16_t>(*g_sleepMarksSpawned + 1);
    }
    READABLE_FUNCTION_WAITS(spawnSleep, Verified);

    // drawSleep ("sleep" +0x20): see sleep.h. A mark past its life is not
    // drawn (nor counted).
    int32_t drawSleep(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> sway;   // how far the mark is off its position (x, y and z are used)
        ee::Local<zge::Vec4> corner; // a corner from the mark's middle (the same)
        int32_t drawn = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        // The quad's corners are the vertices from the last back to the first.
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &vertices[3 - i];
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const ee::Ptr<SleepMark> mark(particle);
            const uint32_t age = now - mark->born;
            if (static_cast<int32_t>(mark->lifetime - age) < 0)
                continue;
            // How much of its life is over, 0 to 1.
            const ee::Float lived = ee::toFloat(static_cast<int32_t>(age)) / ee::toFloat(static_cast<int32_t>(mark->lifetime));
            const uint8_t grey = static_cast<uint8_t>(ee::toInt((ee::Float(1.0f) - lived) * kNewMarkGrey));
            colour[2] = grey;
            colour[3] = 0xFF;
            colour[0] = grey;
            colour[1] = grey;
            const zge::Angle swayAngle = angleOfTurns(lived * kSwaysALife);
            sway->y = 0.0f;
            sway->z = 0.0f;
            sway->x = (lived * kSwayGrowth + kSwayStart) * kSwayScale;
            sway->x = sway->x * zge::angleSin(swayAngle);
            zge::vec3RotateY(&sway->xyz(), &sway->xyz(), angleOfTurns(mark->size));
            const ee::Float half = (lived * kWidthGrowth + kWidthStart) * kWidthScale;

            // Two squares, the second the first seen from behind.
            for (uint32_t side = 0; side < 2; ++side)
            {
                for (uint32_t i = 0; i < 4; ++i)
                {
                    QuadVertex &vertex = vertices[i];
                    const bool right = ((i + 1) & 2) != 0, upper = (i & 2) != 0;
                    std::memcpy(vertex.colour, &colour[0], 4);
                    vertex.u = right ? mark->uHigh : mark->uLow;
                    vertex.v = upper ? mark->vHigh : mark->vLow;
                    corner->x = half;
                    if (!right)
                        corner->x = -half;
                    if (side != 0)
                        corner->x = -corner->x;
                    corner->y = half;
                    if (!upper)
                        corner->y = -half;
                    corner->z = 0.0f;
                    // It turns back about the upright as it ages: a whole turn in its life.
                    zge::vec3RotateY(&corner->xyz(), &corner->xyz(), angleOfTurns(-lived));
                    vertex.position.x = mark->position.x + sway->x + corner->x;
                    vertex.position.y = mark->position.y + sway->y + corner->y;
                    vertex.position.z = mark->position.z + sway->z + corner->z;
                }
                quad->texture = mark->texture;
                zge::drawPrimitiveQuads(kPlacedQuads, quad, 1, 0);
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSleep, Verified);
}
