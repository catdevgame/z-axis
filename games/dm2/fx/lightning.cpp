// DM2's "lightning" effect class (lightning.h). DM2's own code (fx.c);
// Aggressive Inline's lightning (games/aggressive/fx/lightning.cpp) is the
// same lineage.

#include "lightning.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/particle_types.h"

#include <cstdint>
#include "sym/dm2/fx/lightning.cpp.h"

namespace game::dm2::calls
{
    // Scaffolding (address-call): a scene object's matrix of the kind `which`, worked out when it is stale (ZGE2's
    // own function: the engine's cachedMatrix is another).
    inline ee::Ptr<zge::Matrix> objectCachedMatrix(uint32_t object, uint32_t which) { return ee::game<ee::Ptr<zge::Matrix>>(sym::objectCachedMatrix, object, which); }
}

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kLightningEffectDefaults = sym::lightningEffectDefaults;
        constexpr uint32_t kLightningName = sym::lightningName; // "lightning"

        // The starter's bolt.
        constexpr int32_t kStarterSegments = 8;
        constexpr ee::Float kStarterWidth = 0.05f, kStarterJitter = 10.0f;
        // A random byte's middle, and a jitter's part for each step off it.
        constexpr ee::Float kByteMiddle = 128.0f;
        constexpr ee::Float kJitterStep = 0.0078125f;

        // A segment's colour, and its lifetime (ms): the first's, and how much
        // longer each next one's is.
        constexpr uint8_t kBoltColour[4] = {0x10, 0x10, 0xAF, 0xFF};
        constexpr uint32_t kFirstSegmentLifetime = 300, kSegmentLifetimeStep = 10;
        // The draw's fade (ms): the alpha is the time left.
        constexpr uint32_t kFadeMs = 0x100;
        // The primitive API's kind: in the world.
        constexpr uint32_t kWorldPrimitive = 2;

        // The engine's emitter object record, which an emitter object has
        // where other objects have their animator (zge/scene/particle_emitter.cpp
        // keeps its layout to itself): where the object was last moved to.
        struct EmitterPlace
        {
            uint8_t _00[0x10];
            zge::Vec4 position; // +0x10
        };
        // An emitter object keeps that data where other objects keep an animator.
        ee::Ptr<EmitterPlace> emitterPlaceOf(ee::Ptr<zge::SceneObject> object) { return object->animator.cast<EmitterPlace>(); }
        // A vertex's position: its first quadword (x, y, z and the word after) as the Vec4 the transforms take.
        ee::Ptr<zge::Vec4> placeOf(ee::Ptr<QuadVertex> vertex) { return vertex.cast<zge::Vec4>(); }

        // A line of the primitive API: its material and its two ends.
        struct Line
        {
            uint32_t material;
            ee::Ptr<QuadVertex> ends[2];
        };
        static_assert(sizeof(Line) == 0x0C);

        // A random byte about its middle: -128 to 127.
        ee::Float randomByteOff()
        {
            return ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFF)) - kByteMiddle;
        }

        // An unsigned count as a float, as the compiler converts one.
        ee::Float unsignedToFloat(uint32_t count)
        {
            if (static_cast<int32_t>(count) >= 0)
                return ee::toFloat(static_cast<int32_t>(count));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((count & 1) | (count >> 1)));
            return half + half;
        }

        // The next joint on one axis: a share of the way left to the end, and a random stray.
        ee::Float nextJoint(ee::Float from, ee::Float end, uint32_t segmentsLeft, ee::Float jitter)
        {
            const ee::Float off = ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFF));
            const ee::Float step = (end - from) / unsignedToFloat(segmentsLeft);
            return from + step + (off - kByteMiddle) * (jitter * kJitterStep);
        }
    }

    uint32_t startLightningEffect(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<LightningParams> bolt;
        bolt->start.x = 0.0f;
        bolt->start.y = 0.0f;
        bolt->start.z = 0.0f;
        bolt->end.x = randomByteOff();
        bolt->end.y = randomByteOff();
        bolt->end.z = randomByteOff();
        bolt->segments = kStarterSegments;
        bolt->width = kStarterWidth;
        bolt->jitter = kStarterJitter;
        bolt->material = 0;
        return zge::startNamedEffect(object, kLightningName, 1, 0, bolt);
    }
    READABLE_FUNCTION_WAITS(startLightningEffect, Draft);

    void spawnLightning(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<zge::SceneObject> object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                        ee::Ptr<LightningParams> params)
    {
        ee::Local<zge::Vec4> from; // the joint the next segment starts at, and the one it ends at
        ee::Local<zge::Vec4> to;   // (x, y and z are used)
        const uint8_t bolts = static_cast<uint8_t>(count);
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const ee::Ptr<EmitterPlace> place = emitterPlaceOf(object);
        if (!params)
            params = ee::Ptr<LightningParams>(kLightningEffectDefaults);
        if (!effectClass || !emitter)
            return;
        for (uint32_t bolt = 0; bolt < bolts; ++bolt)
        {
            from->x = params->start.x;
            from->y = params->start.y;
            from->z = params->start.z;
            for (int32_t segment = 0; segment < params->segments; ++segment)
            {
                const ee::Ptr<Particle> piece(zge::poolAlloc(emitter->pool));
                if (segment + 1 == params->segments)
                {
                    to->x = params->end.x;
                    to->y = params->end.y;
                    to->z = params->end.z;
                }
                else
                {
                    const uint32_t left = static_cast<uint32_t>(params->segments - segment);
                    to->x = nextJoint(from->x, params->end.x, left, params->jitter);
                    to->y = nextJoint(from->y, params->end.y, left, params->jitter);
                    to->z = nextJoint(from->z, params->end.z, left, params->jitter);
                }
                piece->colour[1] = kBoltColour[1];
                piece->colour[2] = kBoltColour[2];
                piece->colour[3] = kBoltColour[3];
                piece->born = now;
                piece->flags = 0; // it neither moves nor spins
                piece->colour[0] = kBoltColour[0];
                piece->tail.x = place->position.x + from->x;
                piece->tail.y = place->position.y + from->y;
                piece->tail.z = place->position.z + from->z;
                piece->position.x = place->position.x + to->x;
                piece->position.y = place->position.y + to->y;
                piece->position.z = place->position.z + to->z;
                piece->lifetime = static_cast<uint32_t>(segment) * kSegmentLifetimeStep + kFirstSegmentLifetime;
                piece->size = params->width;
                piece->material = params->material;
                zge::pointerArrayAdd(emitter->particles, piece);
                from->x = to->x;
                from->y = to->y;
                from->z = to->z;
            }
        }
    }
    READABLE_FUNCTION_WAITS(spawnLightning, Draft);

    int32_t drawLightning(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<Line> line;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> facing;   // the camera's turn, placed at the segment's tail
        ee::Local<zge::Matrix> viewTurn; // the world's directions in the camera's space
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> along;      // from the tail to the head, in the camera's space (x, y and z are used)
        ee::Local<zge::Vec4> head;       // the segment's two ends in the camera's space
        ee::Local<zge::Vec4> tail;
        const ee::Ptr<QuadVertex> vertex(vertices);
        int32_t drawn = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = vertex + i;
        line->ends[0] = vertex;
        line->ends[1] = vertex + 1;
        const ee::Ptr<zge::Matrix> cameraMatrix = calls::objectCachedMatrix(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)), 1);
        zge::copyRotationAndTranslation(facing, cameraMatrix);
        zge::copyRotationAndTranslation(viewTurn, facing);
        zge::matrixTranspose3(viewTurn, viewTurn);

        for (ee::Ptr<Particle> piece(zge::arrayFirst(emitter->particles)); piece; piece = ee::Ptr<Particle>(zge::arrayNext(emitter->particles)))
        {
            viewTurn->translation() = zge::kZero;
            for (int i = 0; i < 4; ++i)
                colour[i] = piece->colour[i];
            // Its alpha is its time left, once that is under the fade's length; before that, none.
            const uint32_t left = piece->lifetime - (now - piece->born);
            colour[3] = left < kFadeMs ? static_cast<uint8_t>(left) : uint8_t(0);

            if (piece->material == 0)
            {
                facing->translation() = piece->position;
                for (int end = 0; end < 2; ++end)
                {
                    QuadVertex &point = vertices[end];
                    const zge::Vec4 &at = end == 0 ? piece->tail : piece->position;
                    for (int byte = 0; byte < 4; ++byte)
                        point.colour[byte] = colour[byte];
                    point.position.x = at.x;
                    point.position.y = at.y;
                    point.position.z = at.z;
                    point.u = 0.0f;
                    point.v = 0.0f;
                }
                line->material = piece->material;
                zge::drawLines(kWorldPrimitive, line.address(), 1, 0);
            }
            else
            {
                facing->translation() = piece->tail;
                head->x = piece->position.x;
                head->y = piece->position.y;
                head->z = piece->position.z;
                tail->x = piece->tail.x;
                tail->y = piece->tail.y;
                tail->z = piece->tail.z;
                zge::transformPoint(head, viewTurn, head);
                zge::transformPoint(tail, viewTurn, tail);
                along->x = head->x - tail->x;
                along->y = head->y - tail->y;
                along->z = head->z - tail->z;
                // The strip is wound the other way when it runs to the camera's left.
                const bool rightward = ee::Float(0.0f) < along->x;
                for (uint32_t i = 0; i < 4; ++i)
                    quad->corners[i] = rightward ? vertex + i : vertex + (3 - i);

                // The corners: the tail's two, then the head's two, each the
                // segment's width above or below, turned to face the camera.
                const auto corner = [&](uint32_t index, bool atHead, bool above, ee::Float u, ee::Float v) {
                    QuadVertex &made = vertices[index];
                    for (int byte = 0; byte < 4; ++byte)
                        made.colour[byte] = colour[byte];
                    if (atHead)
                    {
                        made.position.x = along->x;
                        made.position.z = along->z;
                        made.position.y = above ? along->y + piece->size : along->y - piece->size;
                    }
                    else
                    {
                        made.position.x = 0.0f;
                        made.position.z = 0.0f;
                        made.position.y = above ? piece->size : -piece->size;
                    }
                    const ee::Ptr<zge::Vec4> position = placeOf(vertex + index);
                    zge::transformPoint(position, facing, position);
                    made.u = u;
                    made.v = v;
                };
                corner(3, false, false, 0.0f, 0.0f);
                corner(2, false, true, 0.0f, 1.0f);
                corner(1, true, true, 1.0f, 1.0f);
                corner(0, true, false, 1.0f, 0.0f);
                quad->material = piece->material;
                zge::drawPrimitiveQuads(kWorldPrimitive, quad, 1, 0);
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawLightning, Draft);
}
