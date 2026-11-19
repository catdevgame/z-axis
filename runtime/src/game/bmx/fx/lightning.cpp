// BMX XXX's lightning (lightning.h). BMX XXX's own code (fx.c's block).

#include "lightning.h"
#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/lightning.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kLightningName = sym::lightningName;         // "lightning"
        constexpr uint32_t kLightningDefaults = sym::lightningDefaults;

        // The start's bolt.
        constexpr int32_t kStartSegments = 8;
        constexpr ee::Float kStartWidth = 0.05f;
        constexpr ee::Float kStartJitter = 10.0f;

        // A random byte less this is -128 .. 127; a point is pushed aside by
        // that many 128ths of the jitter.
        constexpr ee::Float kRandomByteCentre = 128.0f;
        constexpr ee::Float kRandomByteStep = 0.0078125f;
        // A segment lives 300 milliseconds, and 10 more each one down the bolt.
        constexpr uint32_t kSegmentLifetime = 300, kLifetimePerSegment = 10;
        // A segment's colour: pale blue (its alpha is drawn from its life left).
        constexpr uint8_t kBoltColour[4] = {0x10, 0x10, 0xAF, 0xFF};

        // How many particles are alive, and the effects' budget of them.
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;

        // What the bolts read of an emitter object's data (the engine's
        // EmitterObjectData, zge/scene/particle_emitter.cpp): the position
        // its moved hook was last given.
        struct EmitterDataPosition
        {
            uint8_t _00[0x10];
            ee::Float x, y, z; // +0x10
        };
        // An emitter object's data, where other objects have their animator.
        ee::Ptr<EmitterDataPosition> positionOf(ee::Ptr<zge::SceneObject> object)
        {
            return object->animator.cast<EmitterDataPosition>();
        }

        // A random byte as a float.
        ee::Float randomByte()
        {
            return ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFFu));
        }

        // drawPrimitiveQuads' and drawLines' flags for the world.
        constexpr uint32_t kWorldPrimitive = 2;
        // A segment is seen only over its last 256 milliseconds, its alpha
        // its milliseconds left (0 before that).
        constexpr uint32_t kFadeMs = 0x100;

        // What drawLightning keeps in its frame, as the original lays it out.
        struct LightningDraw
        {
            Quad quad;                 // +0x000
            uint8_t _14[0x0C];
            zge::PrimitiveLine line;   // +0x020 an untextured segment: corners[0] to corners[1]
            uint8_t _2c[4];
            QuadVertex corners[4];     // +0x030
            zge::Matrix camera;        // +0x0B0 the camera's matrix, moved to each segment
            zge::Matrix toCamera;      // +0x0F0 the world's directions in the camera's frame
            uint8_t colour[4];         // +0x130
            uint8_t _134[0x0C];
            ee::Float along[3];        // +0x140 from the segment's start to its end, in the camera's frame
            uint32_t _14c;
            zge::Vec4 end;             // +0x150 in the camera's frame
            zge::Vec4 start;           // +0x160
        };
        static_assert(offsetof(LightningDraw, line) == 0x20 && offsetof(LightningDraw, corners) == 0x30);
        static_assert(offsetof(LightningDraw, camera) == 0xB0 && offsetof(LightningDraw, toCamera) == 0xF0);
        static_assert(offsetof(LightningDraw, colour) == 0x130 && offsetof(LightningDraw, along) == 0x140);
        static_assert(offsetof(LightningDraw, end) == 0x150 && offsetof(LightningDraw, start) == 0x160);
    }

    // startLightningEffect: see lightning.h.
    uint32_t startLightningEffect(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<LightningParameters> bolt;
        bolt->fromX = 0.0f;
        bolt->fromY = 0.0f;
        bolt->fromZ = 0.0f;
        bolt->toX = randomByte() - kRandomByteCentre;
        bolt->toY = randomByte() - kRandomByteCentre;
        bolt->toZ = randomByte() - kRandomByteCentre;
        bolt->segments = kStartSegments;
        bolt->width = kStartWidth;
        bolt->jitter = kStartJitter;
        bolt->texture = 0;
        return zge::startNamedEffect(object, kLightningName, 1, 0, bolt);
    }
    READABLE_FUNCTION_WAITS(startLightningEffect, Draft);

    // spawnLightning ("lightning" +0x18): `count` bolts from the parameters
    // (the defaults when none), each its segments from the start to the
    // end, every point but the last pushed aside at random up to the
    // jitter, at the effect object's position; each segment 10 milliseconds
    // longer lived down the bolt. Kept bug: with the effects' budget of
    // particles used up no segment is allocated, and the segment is written
    // at address 0 and added to the emitter.
    void spawnLightning(uint32_t effectClass, ee::Ptr<zge::SceneObject> object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                        ee::Ptr<LightningParameters> given)
    {
        const uint32_t wanted = count & 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const ee::Ptr<EmitterDataPosition> origin = positionOf(object);
        const ee::Ptr<LightningParameters> bolt = given ? given : ee::Ptr<LightningParameters>(kLightningDefaults);
        if (!effectClass || !emitter || !wanted)
            return;
        // A step toward the end on one axis: what is left over the segments
        // left, then pushed aside.
        const auto nextPoint = [&](ee::Float to, ee::Float from, uint32_t segment)
        {
            const ee::Float pushed = randomByte();
            const ee::Float gap = to - from;
            const ee::Float step = gap / zge::unsignedToFloat(static_cast<uint32_t>(bolt->segments - static_cast<int32_t>(segment)));
            return from + step + (pushed - kRandomByteCentre) * (bolt->jitter * kRandomByteStep);
        };
        for (uint32_t bolts = 0; bolts < wanted; ++bolts)
        {
            ee::Float fromX = bolt->fromX, fromY = bolt->fromY, fromZ = bolt->fromZ;
            for (uint32_t segment = 0; static_cast<int32_t>(segment) < bolt->segments; ++segment)
            {
                uint32_t made = 0;
                if (*g_particlesAlive < *g_particleBudget)
                {
                    *g_particlesAlive = *g_particlesAlive + 1;
                    made = zge::allocateSized(emitter->particleBytes);
                }
                const ee::Ptr<Particle> piece(made);
                ee::Float toX, toY, toZ;
                if (static_cast<int32_t>(segment + 1) != bolt->segments)
                {
                    toX = nextPoint(bolt->toX, fromX, segment);
                    toY = nextPoint(bolt->toY, fromY, segment);
                    toZ = nextPoint(bolt->toZ, fromZ, segment);
                }
                else
                {
                    toX = bolt->toX;
                    toY = bolt->toY;
                    toZ = bolt->toZ;
                }
                std::memcpy(piece->colour, kBoltColour, sizeof(piece->colour));
                piece->born = now;
                piece->flags = 0;
                piece->tail.x = origin->x + fromX;
                piece->tail.y = origin->y + fromY;
                piece->tail.z = origin->z + fromZ;
                piece->position.x = origin->x + toX;
                piece->position.y = origin->y + toY;
                piece->position.z = origin->z + toZ;
                piece->size = bolt->width;
                piece->lifetime = segment * kLifetimePerSegment + kSegmentLifetime;
                piece->texture = bolt->texture;
                zge::pointerArrayAdd(emitter->particles, piece);
                fromX = toX;
                fromY = toY;
                fromZ = toZ;
            }
        }
    }
    READABLE_FUNCTION_WAITS(spawnLightning, Draft);

    // drawLightning ("lightning" +0x20): each segment, seen only over its
    // last 256 milliseconds (its alpha its milliseconds left, 0 before):
    // untextured, a line from its start to its end;
    // textured, a band along it facing the camera, twice its size across.
    // Returns how many it drew.
    int32_t drawLightning(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<LightningDraw> draw;
        int32_t drawn = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        QuadVertex *const corners = draw->corners;
        for (uint32_t i = 0; i < 4; ++i)
            draw->quad.corners[i] = &corners[i];
        draw->line.first = primitiveVertexOf(&corners[0]);
        draw->line.second = primitiveVertexOf(&corners[1]);
        const ee::Ptr<zge::SceneObject> cameraObject(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)));
        draw->camera = *zge::objectMatrix(cameraObject, 1);
        draw->toCamera = draw->camera;
        zge::matrixTranspose3(&draw->toCamera, &draw->toCamera);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles), ++drawn)
        {
            const Particle &piece = *ee::Ptr<Particle>(particle);
            draw->toCamera.translation() = zge::kZero;
            std::memcpy(draw->colour, piece.colour, 4);
            const uint32_t left = piece.lifetime - (now - piece.born);
            draw->colour[3] = left < kFadeMs ? static_cast<uint8_t>(left) : 0;
            if (piece.texture == 0)
            {
                draw->camera.translation() = piece.position;
                std::memcpy(corners[0].colour, draw->colour, 4);
                corners[0].position.x = piece.tail.x;
                corners[0].position.y = piece.tail.y;
                corners[0].position.z = piece.tail.z;
                corners[0].u = 0.0f;
                corners[0].v = 0.0f;
                std::memcpy(corners[1].colour, draw->colour, 4);
                corners[1].position.x = piece.position.x;
                corners[1].position.y = piece.position.y;
                corners[1].position.z = piece.position.z;
                corners[1].u = 0.0f;
                corners[1].v = 0.0f;
                draw->line.texture = piece.texture;
                zge::drawLines(kWorldPrimitive, ee::Address(&draw->line), 1, 0);
                continue;
            }
            // The band is laid out from the segment's start, in the camera's
            // frame, and put back in the world through the camera's matrix.
            draw->camera.translation() = piece.tail;
            draw->end.x = piece.position.x;
            draw->end.y = piece.position.y;
            draw->end.z = piece.position.z;
            draw->start.x = piece.tail.x;
            draw->start.y = piece.tail.y;
            draw->start.z = piece.tail.z;
            draw->end = zge::transformed(draw->toCamera, draw->end);
            draw->start = zge::transformed(draw->toCamera, draw->start);
            draw->along[0] = draw->end.x - draw->start.x;
            draw->along[1] = draw->end.y - draw->start.y;
            draw->along[2] = draw->end.z - draw->start.z;
            // The corners go round the other way for a segment that runs to the left.
            const bool rightward = ee::Float(0.0f) < draw->along[0];
            for (uint32_t i = 0; i < 4; ++i)
                draw->quad.corners[i] = &corners[rightward ? i : 3 - i];

            std::memcpy(corners[3].colour, draw->colour, 4);
            corners[3].position.x = 0.0f;
            corners[3].position.z = 0.0f;
            corners[3].position.y = -piece.size;
            corners[3].position = zge::transformed(draw->camera, corners[3].position);
            corners[3].u = 0.0f;
            corners[3].v = 0.0f;
            std::memcpy(corners[2].colour, draw->colour, 4);
            corners[2].position.x = 0.0f;
            corners[2].position.z = 0.0f;
            corners[2].position.y = piece.size;
            corners[2].position = zge::transformed(draw->camera, corners[2].position);
            corners[2].u = 0.0f;
            corners[2].v = 1.0f;
            std::memcpy(corners[1].colour, draw->colour, 4);
            corners[1].position.x = draw->along[0];
            corners[1].position.z = draw->along[2];
            corners[1].position.y = draw->along[1] + piece.size;
            corners[1].position = zge::transformed(draw->camera, corners[1].position);
            corners[1].u = 1.0f;
            corners[1].v = 1.0f;
            std::memcpy(corners[0].colour, draw->colour, 4);
            corners[0].position.x = draw->along[0];
            corners[0].position.z = draw->along[2];
            corners[0].position.y = draw->along[1] - piece.size;
            corners[0].position = zge::transformed(draw->camera, corners[0].position);
            corners[0].u = 1.0f;
            corners[0].v = 0.0f;
            draw->quad.texture = piece.texture;
            zge::drawPrimitiveQuads(kWorldPrimitive, &draw->quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawLightning, Draft);
}
