// DM2's "sleep" effect class (sleep.h). DM2's own code (fx.c).

#include "sleep.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"

#include <cstdint>
#include "sym/dm2/fx/sleep.cpp.h"

namespace game::dm2::calls
{
    // Scaffolding (address-call): ZGE2's sine of an angle (a table), which the engine's header does not declare.
    inline ee::Float sinTurnOf(zge::Angle angle) { return ee::game<ee::Float>(sym::sinTurnOf, angle); }
}

namespace game::dm2
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject;
        constexpr uint32_t kSleepName = sym::sleepName; // "sleep"
        constexpr uint32_t kSleepEffectDefaults = sym::sleepEffectDefaults;
        inline ee::Global<uint32_t, sym::g_sleepMaterial> g_sleepMaterial;
        // How many marks have been spawned: each is turned a seventh of a
        // turn on from the one before.
        inline ee::Global<uint16_t, sym::g_sleepMarksSpawned> g_sleepMarksSpawned;
        constexpr uint32_t kSeventhOfATurn = 0x2492;
        constexpr ee::Float kTurnsPerAngleUnit = 1.0f / 65536.0f;
        constexpr ee::Float kAngleUnitsPerTurn = 65536.0f;
        // A mark's rise a move (y is down).
        constexpr ee::Float kSleepRise = -0.0023333335f;

        // The draw: the grey a new mark has; its sways a life; how far out
        // it sways (from the start to the end of its life, scaled); its half
        // width (the same way).
        constexpr ee::Float kNewMarkGrey = 63.0f;
        constexpr ee::Float kSwaysALife = 4.0f;
        constexpr ee::Float kSwayGrowth = 0.7f, kSwayStart = 0.3f, kSwayScale = 0.2f;
        constexpr ee::Float kWidthGrowth = 0.85f, kWidthStart = 0.15f, kWidthScale = 0.1f;

        // A part of a turn as an angle.
        zge::Angle angleOfTurns(ee::Float turns)
        {
            return static_cast<zge::Angle>(ee::toInt(turns * kAngleUnitsPerTurn));
        }
    }

    void sleep(ee::Ptr<zge::Vec4> position, int32_t lifetime)
    {
        ee::Local<SleepParams> spawn;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        spawn->lifetime = lifetime;
        spawn->position = zge::aligned(position);
        zge::spawnNamedEffect(kSleepName, object, spawn);
    }
    READABLE_FUNCTION_WAITS(sleep, Verified);

    void spawnSleep(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<SleepParams> params)
    {
        if (!params)
            params = ee::Ptr<SleepParams>(kSleepEffectDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<SleepMark> mark(zge::poolAlloc(emitter->pool));
        mark->born = static_cast<uint32_t>(effectsClock());
        mark->flags = zge::kParticleMoves;
        mark->lifetime = static_cast<uint32_t>(params->lifetime);
        mark->material = *g_sleepMaterial;
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

    int32_t drawSleep(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> sway;   // how far the mark is off its position (x, y and z are used)
        ee::Local<zge::Vec4> corner; // a corner from the mark's middle (the same)
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        // The quad's corners are the vertices from the last back to the first.
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(vertices) + (3 - i);
        int32_t drawn = 0;
        for (ee::Ptr<SleepMark> mark(zge::arrayFirst(emitter->particles)); mark; mark = ee::Ptr<SleepMark>(zge::arrayNext(emitter->particles)))
        {
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
            sway->x = sway->x * calls::sinTurnOf(swayAngle);
            zge::vec3RotateY(&sway->xyz(), &sway->xyz(), angleOfTurns(mark->size));
            const ee::Float half = (lived * kWidthGrowth + kWidthStart) * kWidthScale;

            // Two squares, the second the first seen from behind.
            for (uint32_t side = 0; side < 2; ++side)
            {
                for (uint32_t i = 0; i < 4; ++i)
                {
                    QuadVertex &vertex = vertices[i];
                    const bool right = ((i + 1) & 2) != 0, upper = (i & 2) != 0;
                    for (int byte = 0; byte < 4; ++byte)
                        vertex.colour[byte] = colour[byte];
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
                quad->material = mark->material;
                zge::drawPrimitiveQuads(0, quad, 1, 0);
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSleep, Verified);
}
