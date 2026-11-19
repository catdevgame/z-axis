// BMX XXX's tracers (tracers.h). BMX XXX's own code (fx.c's block): the
// sparks' spawn and draw (particles.cpp) with a trail.

#include "tracers.h"
#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/tracers.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kTracerEffectDefaults = sym::tracerEffectDefaults;
        // The tracers are drawn with the sparks' material and colours.
        inline ee::Global<uint32_t, sym::g_sparkMaterial> g_sparkMaterial;
        constexpr uint32_t kSparkShades = 16;
        inline ee::Global<uint8_t[kSparkShades][4], sym::g_sparkColours> g_sparkColours;
        // How many particles are alive, and the effects' budget of them.
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;

        // Each piece of the trail is drawn as if 60 milliseconds younger than
        // the one before; a tracer lives on for the six of them.
        constexpr int32_t kTrailStepMs = 60;
        constexpr ee::Float kTrailMs = 360.0f;
        // drawPrimitiveQuads' flags for a quad in the world, through a matrix.
        constexpr uint32_t kWorldQuads = 2;

        // What drawTracers keeps in its frame, as the original lays it out.
        struct TracerDraw
        {
            QuadVertex corners[4];            // +0x00
            Quad quad;                        // +0x80 the corners from the last back to the first
            uint8_t _94[0x0C];
            zge::Matrix camera;               // +0xA0 the camera's matrix, moved to each piece
            zge::Matrix toCamera;             // +0xE0 the world's directions in the camera's frame
            uint8_t colour[4];                // +0x120
            uint8_t _124[0x0C];
            zge::Vec4 streak;                 // +0x130 from the head to the piece's far end, in the camera's frame
            zge::Vec4 along;                  // +0x140 the streak's direction on screen
            zge::Vec4 across;                 // +0x150 the direction across it
            zge::Vec4 head;                   // +0x160 the tracer's position in the camera's frame
            zge::Vec4 tail;                   // +0x170 the piece's far end
            zge::Vec4 alongOffset;            // +0x180
            zge::Vec4 acrossOffset;           // +0x190
        };
        static_assert(offsetof(TracerDraw, quad) == 0x80 && offsetof(TracerDraw, camera) == 0xA0);
        static_assert(offsetof(TracerDraw, colour) == 0x120 && offsetof(TracerDraw, streak) == 0x130);
        static_assert(offsetof(TracerDraw, acrossOffset) == 0x190);
    }

    // spawnTracers ("tracer" +0x18): one tracer (whatever the count) from the
    // parameters (the defaults when none), its trail all at its start. Kept
    // bug: with the effects' budget of particles used up no tracer is
    // allocated, and the tracer is written at address 0 and added to the
    // emitter.
    void spawnTracers(uint32_t effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t,
                      ee::Ptr<TracerEffectParameters> given)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const ee::Ptr<TracerEffectParameters> params = given ? given : ee::Ptr<TracerEffectParameters>(kTracerEffectDefaults);
        if (!effectClass || !emitter)
            return;
        uint32_t made = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            made = zge::allocateSized(emitter->particleBytes);
        }
        const ee::Ptr<Tracer> tracer(made);
        tracer->born = now;
        tracer->flags = kParticleMoves;
        tracer->texture = *g_sparkMaterial;
        tracer->size = params->size;
        std::memcpy(tracer->colour, params->colour, 4);
        tracer->lifetime = static_cast<uint32_t>(ee::toInt(ee::toFloat(params->lifetime) + kTrailMs));
        tracer->tail = params->position;
        tracer->position = params->position;
        tracer->velocity = params->velocity;
        for (uint32_t i = 0; i < kTracerTrail; ++i)
            tracer->trail[i] = params->position;
        game::tail(zge::pointerArrayAdd, emitter->particles, tracer);
    }
    READABLE_FUNCTION_WAITS(spawnTracers, Draft);

    // drawTracers ("tracer" +0x20): each tracer's trail moves on a place (the
    // newest is where its tail is), then six pieces are drawn as the sparks'
    // streaks are: the first from the tracer's head, each later one from the
    // trail's place before it, to the trail's next place; each in the
    // sparks' colour of its age, 60 milliseconds younger down the trail, and
    // not at all outside the 16 shades; the last piece fades to nothing at
    // its far end. Kept as it is: every piece's streak is measured from the
    // tracer's head in the camera's frame, found for the first piece only (a
    // tracer whose first piece is not drawn uses the one before it).
    // Returns how many tracers it looked at.
    int32_t drawTracers(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<TracerDraw> draw;
        int32_t drawn = 0;
        const ee::Ptr<zge::SceneObject> cameraObject(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)));
        draw->camera = *zge::objectMatrix(cameraObject, 1);
        draw->toCamera = draw->camera;
        zge::matrixTranspose3(&draw->toCamera, &draw->toCamera);
        draw->toCamera.translation() = zge::kZero;
        for (uint32_t i = 0; i < 4; ++i)
            draw->quad.corners[i] = &draw->corners[3 - i];
        const int32_t now = effectsClock();
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            Tracer &tracer = *ee::Ptr<Tracer>(particle);
            ++drawn;
            for (uint32_t place = kTracerTrail - 1; place > 0; --place)
                tracer.trail[place] = tracer.trail[place - 1];
            tracer.trail[0] = tracer.tail;

            for (uint32_t piece = 0; piece < kTracerTrail; ++piece)
            {
                const int32_t age = now - static_cast<int32_t>(piece) * kTrailStepMs - static_cast<int32_t>(tracer.born);
                const int32_t shade = ee::toInt(ee::toFloat(age) * ee::Float(15.0f) / ee::toFloat(static_cast<int32_t>(tracer.lifetime)));
                if (static_cast<uint32_t>(shade) >= kSparkShades)
                    continue;
                std::memcpy(draw->colour, (*g_sparkColours)[shade], 4);
                if (piece == 0)
                {
                    draw->camera.translation() = tracer.position;
                    draw->head = zge::transformed(draw->toCamera, tracer.position);
                }
                else
                    draw->camera.translation() = tracer.trail[piece - 1];
                draw->tail = zge::transformed(draw->toCamera, tracer.trail[piece]);
                draw->streak = draw->tail - draw->head;
                // Its direction on screen (x, y), and the one across it.
                draw->along = draw->streak;
                draw->along.z = ee::Float(0.0f);
                const ee::Float length = zge::vec3Length(&draw->along.xyz());
                if (ee::Float(0.0f) < length)
                    draw->along = draw->along * (ee::Float(1.0f) / length);
                else
                {
                    draw->along.z = ee::Float(0.0f);
                    draw->along.x = ee::Float(1.0f);
                    draw->along.y = ee::Float(0.0f);
                }
                draw->across.z = ee::Float(0.0f);
                draw->across.x = draw->along.y;
                draw->across.y = -draw->along.x;
                // The corners: 0 and 1 at the near end, 2 and 3 at the far
                // end, the tracer's size either side.
                for (uint32_t i = 0; i < 4; ++i)
                {
                    const bool farEnd = (i & 2u) != 0;
                    const bool otherSide = ((i + 1) & 2u) != 0;
                    QuadVertex &corner = draw->corners[i];
                    std::memcpy(corner.colour, draw->colour, 4);
                    corner.u = farEnd ? ee::Float(1.0f) : ee::Float(0.0f);
                    corner.v = otherSide ? ee::Float(1.0f) : ee::Float(0.0f);
                    draw->alongOffset = draw->along * (farEnd ? -tracer.size : tracer.size);
                    draw->acrossOffset = draw->across * (otherSide ? -tracer.size : tracer.size);
                    corner.position = draw->alongOffset + draw->acrossOffset;
                    if (farEnd)
                    {
                        corner.position = corner.position + draw->streak;
                        if (piece == kTracerTrail - 1)
                            corner.colour[3] = 0;
                    }
                }
                draw->quad.texture = tracer.texture;
                zge::drawPrimitiveQuads(kWorldQuads, &draw->quad, 1, &draw->camera);
            }
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawTracers, Draft);
}
