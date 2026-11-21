// The per-frame effects (readable round 13). Names past the offsets are
// inferred from what the code does: a particle emitter's frame (its
// class's two methods: expire, draw), a level
// object's collision box following its node.

#include "aggressive/fx/particles.h"
#include "aggressive/globals.h"
#include "aggressive/level/traffic_car.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/math/box.h"
#include "zge/collision/sweep.h"
#include "zge/scene/scene.h"
#include "zge/scene/follower.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/assets/tag_table.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/render/texture.h"
#include "aggressive/level/level_collision.h"
#include "aggressive/fx/library.h"
#include "aggressive/level/scene_objects.h"
#include "aggressive/level/traffic_light_state.h"
#include "aggressive/level/traffic_lights.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/effects.cpp.h"

namespace aggressive
{
    using fx::CursorArray;
    using fx::Emitter;
    using fx::Particle;
    using fx::first;
    using fx::next;

    namespace
    {
        constexpr uint32_t kWorldPrimitive = 2;           // the API's kind: in the world

        inline ee::Global<ee::Float, sym::g_dimAlpha> g_dimAlpha;  // 0.1: a dying particle's alpha, of its own
        inline ee::Global<ee::Float, sym::g_fadeAlpha> g_fadeAlpha; // 0.9: faded over the fade's time
        inline ee::Global<uint32_t, sym::g_fadeStart> g_fadeStart;  // 720 ms of life left: the fade starts
        inline ee::Global<uint32_t, sym::g_dimStart> g_dimStart;   // 360 ms left: dim from there
    }

    // expireParticles: frees the emitter's particles whose life is over, once
    // a clock tick at most. Returns how many it looked at.
    int32_t expireParticles(uint32_t /*effect*/, ee::Ptr<Emitter> emitter)
    {
        int32_t looked = 0;
        const uint32_t now = fx::effectsClock();
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < now))
            return 0;
        CursorArray &particles = *emitter->particles;
        for (ee::Ptr<Particle> particle(first(particles)); particle; ++looked)
        {
            if (now - particle->born >= particle->lifetime)
            {
                // The last item takes its place under the cursor.
                zge::pointerArrayRemoveCurrent(emitter->particles);
                aggressive::fx::freeParticle(emitter, particle);
                particle = ee::Ptr<Particle>(zge::current(emitter->particles));
            }
            else
                particle = ee::Ptr<Particle>(next(particles));
        }
        emitter->lastMoved = now;
        return looked;
    }
    READABLE_FUNCTION(expireParticles, Verified);

    namespace
    {
        // The primitive API's vertex (zge/render/ps2/primitive_packets.cpp's
        // PrimitiveVertex). It reads the position, the texture coordinates
        // and the colour; the two other words are not read.
        struct Vertex
        {
            ee::Float x, y, z;
            uint32_t unread0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t unread1c;
        };
        static_assert(sizeof(Vertex) == 0x20);
        // A strip of vertices (the API's quads), and one line.
        struct Strip
        {
            uint32_t texture;
            ee::Ptr<ee::Ptr<Vertex>> vertices;
            int32_t count;
        };
        static_assert(sizeof(Strip) == 0x0C);
        struct Line
        {
            uint32_t texture;
            ee::Ptr<Vertex> first, second;
        };
        static_assert(sizeof(Line) == 0x0C);

        // A corner `dy` above or below p, in `colour`.
        void cornerAt(Vertex &v, const zge::Vec4 &p, ee::Float dy, bool above, const uint8_t *colour)
        {
            std::memcpy(v.colour, colour, 4);
            v.x = p.x;
            v.y = above ? p.y + dy : p.y - dy;
            v.z = p.z;
        }
    }

    // drawParticles: draws the emitter's particles, their alpha faded
    // toward the end of their life (to 0.1 of it over 720 to 360 ms left;
    // the tail by its lag behind the head): a line from tail to head, or
    // with a texture an upright and a flat square from tail to head.
    // Returns how many it drew.
    int32_t drawParticles(uint32_t /*effect*/, ee::Ptr<Emitter> emitter)
    {
        // The square's corners: 0 and 1 the head's (below, above), 2 and 3
        // the tail's (above, below); a line is 0 (the tail) to 1 (the head).
        ee::Local<Strip> square;
        ee::Local<Vertex[4]> corner;
        ee::Local<ee::Ptr<Vertex>[4]> strip; // the corners in the strip's order
        ee::Local<Line> line;
        ee::Local<uint8_t[4]> colour;
        ee::Local<uint8_t[4]> laggedColour; // the tail's
        int32_t drawn = 0;
        const uint32_t now = fx::effectsClock();
        CursorArray &particles = *emitter->particles;
        square->vertices = strip;
        square->count = 4;
        strip[2] = &corner[3];
        strip[3] = &corner[2];
        line->first = &corner[0];
        line->second = &corner[1];
        strip[0] = &corner[0];
        strip[1] = &corner[1];
        ee::Ptr<Particle> particle(first(particles));
        if (!particle)
            return 0;
        const ee::Float dim = *g_dimAlpha;
        for (; particle; ++drawn)
        {
            const Particle &p = *particle;
            std::memcpy(&laggedColour[0], p.colour, 4);
            std::memcpy(&colour[0], &laggedColour[0], 4);
            const uint32_t left = p.lifetime - (now - p.born);
            const uint32_t lag = static_cast<uint32_t>(ee::toInt(*fx::g_tickTime * ee::Float(1000.0f)));
            const uint8_t alpha = p.colour[3];
            const uint32_t tailLeft = lag < left ? left - lag : 0;
            const uint32_t dimStart = *g_dimStart;
            const ee::Float rate = ee::toFloat(alpha) * *g_fadeAlpha / ee::Float(360.0f);
            if (left < dimStart)
                colour[3] = static_cast<uint8_t>(ee::toInt(ee::toFloat(alpha) * dim));
            else if (left < *g_fadeStart)
                colour[3] = static_cast<uint8_t>(ee::toInt(ee::toFloat(alpha) - rate * ee::toFloat(static_cast<int32_t>(*g_fadeStart - left))));
            const uint32_t fadeStart = *g_fadeStart;
            if (tailLeft < dimStart)
            {
                const ee::Float lagged = ee::toFloat(static_cast<int32_t>(p.colour[3])) * dim;
                laggedColour[3] = static_cast<uint8_t>(ee::toInt(lagged));
            }
            else if (tailLeft < fadeStart)
            {
                const ee::Float lagged = ee::toFloat(static_cast<int32_t>(p.colour[3])) - rate * ee::toFloat(static_cast<int32_t>(fadeStart - tailLeft));
                laggedColour[3] = static_cast<uint8_t>(ee::toInt(lagged));
            }

            Vertex &headLow = corner[0];
            Vertex &headHigh = corner[1];
            Vertex &tailHigh = corner[2];
            Vertex &tailLow = corner[3];
            if (p.texture == 0)
            {
                // A line, tail to head.
                Vertex &tail = corner[0];
                Vertex &head = corner[1];
                std::memcpy(tail.colour, &colour[0], 4);
                tail.x = p.tail.x, tail.y = p.tail.y;
                tail.u = 0.0f, tail.z = p.tail.z, tail.v = 0.0f;
                std::memcpy(head.colour, &colour[0], 4);
                head.x = p.position.x, head.y = p.position.y;
                head.u = 0.0f, head.z = p.position.z, head.v = 0.0f;
                line->texture = p.texture;
                zge::drawLines(kWorldPrimitive, line.address(), 1, 0);
            }
            else
            {
                // An upright square from the tail (in its lagged colour) to
                // the head, then a flat one.
                const ee::Float s = p.size;
                cornerAt(tailLow, p.tail, s, false, &laggedColour[0]);
                tailLow.u = 1.0f, tailLow.v = 0.0f;
                cornerAt(tailHigh, p.tail, s, true, &laggedColour[0]);
                tailHigh.u = 0.0f, tailHigh.v = 0.0f;
                cornerAt(headHigh, p.position, s, true, &colour[0]);
                headHigh.u = 0.0f, headHigh.v = 1.0f;
                cornerAt(headLow, p.position, s, false, &colour[0]);
                headLow.u = 1.0f, headLow.v = 1.0f;
                square->texture = p.texture;
                zge::drawPrimitives(kWorldPrimitive, square, 1, 0);
                tailLow.x = p.tail.x - s, tailLow.y = p.tail.y, tailLow.z = p.tail.z - s;
                tailHigh.x = p.tail.x + s, tailHigh.y = p.tail.y, tailHigh.z = p.tail.z + s;
                headHigh.x = p.position.x + s, headHigh.y = p.position.y, headHigh.z = p.position.z + s;
                headLow.x = p.position.x - s, headLow.y = p.position.y, headLow.z = p.position.z - s;
                zge::drawPrimitives(kWorldPrimitive, square, 1, 0);
            }
            particle = ee::Ptr<Particle>(next(*emitter->particles));
        }
        return drawn;
    }
    READABLE_FUNCTION(drawParticles, Verified);

    // moveObjectBox (object, car): a traffic car's box in the level's sweep
    // (updateTrafficCar, as the car moves) set to its scene object's node's
    // bounds, lowered by half their height. Returns 1.
    int32_t moveObjectBox(ee::Ptr<zge::SceneObject> object, ee::Ptr<TrafficCar> car)
    {
        ee::Local<zge::Box> bounds;
        ee::Local<zge::Vec4> half;
        zge::memoryCopy(bounds, zge::nodeBox(ee::Ptr<zge::Node>(objectNode(object))), sizeof(zge::Box));
        zge::Vec4 &min = bounds->low;
        zge::Vec4 &max = bounds->high;
        *half = max - min;
        *half = *half * ee::Float(0.5f);
        min.y = min.y - half->y;
        max.y = max.y - half->y;
        const ee::Ptr<zge::SweepBox> box = car->sweepBox;
        box->lowEnds[0]->value = min.x;
        box->lowEnds[1]->value = min.y;
        box->lowEnds[2]->value = min.z;
        box->lowEnds[0]->other->value = max.x;
        box->lowEnds[1]->other->value = max.y;
        box->lowEnds[2]->other->value = max.z;
        const ee::Ptr<zge::Sweep> sweep = levelSweep();
        zge::sweepUpdateBox(sweep, box);
        return 1;
    }
    READABLE_FUNCTION(moveObjectBox, Verified);

}

namespace aggressive
{
    namespace
    {
        // A traffic car's path follower (car +0x08; engine followerHeading ..).
        constexpr uint32_t kSetFollowerSpeed = sym::setFollowerSpeed; // (follower, speed)
        inline ee::Global<ee::Float, sym::g_sameSpeed> g_sameSpeed; // 1e-5

        // What a car does with its speed.
        enum class Drive
        {
            EaseOff = 0,  // 1 less
            Brake = 2,    // 15 less
            Keep = 3,
            Speed = 4,    // 1.5 more
        };

        // No reason to stop: on up to 10, on below 25 from the segment's
        // ends, else keep.
        Drive cruise(ee::Float speed, ee::Float along, ee::Float toEnd)
        {
            if (!(ee::Float(10.0f) < speed))
                return Drive::Speed;
            if (along < ee::Float(25.0f) || toEnd < ee::Float(25.0f))
                return Drive::Keep;
            return Drive::Speed;
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void setFollowerSpeed(uint32_t follower, ee::Float speed) { ee::tailCall(kSetFollowerSpeed, follower, speed); }
        }
    }

    // driveTrafficCar: a traffic car's speed for the frame. With a car ahead,
    // by how soon it would reach it (under 1, 3 seconds or more) and how
    // near it is (under 10, 20 or more, less 5); else by the traffic light
    // at its segment's end: braking when it could not pass in time (2 s
    // more than a fifteenth of its speed), or before the amber's time runs
    // out; then kept to its maximum speed (its component's float at 8).
    void driveTrafficCar(ee::Ptr<TrafficCar> car)
    {
        const auto follower = [&] { return ee::Ptr<zge::Follower>(car->follower); };
        // The next car the way `speed` goes.
        const auto nextCar = [&](ee::Ptr<zge::Follower> from, ee::Float speed)
        {
            if (speed >= ee::Float(0.0f))
                return zge::followerAhead(from);
            return zge::followerBehind(from);
        };
        const ee::Ptr<zge::Follower> self = follower();
        const ee::Ptr<zge::Follower> ahead = nextCar(self, zge::followerSpeed(self));
        ee::Float gap = -1.0f;
        if (ahead)
        {
            const ee::Float here = zge::followerDistance(self);
            gap = ee::abs(here - zge::followerDistance(ahead));
        }
        const ee::Float speed = zge::followerSpeed(follower());

        const auto decide = [&]() -> Drive
        {
            if (gap < ee::Float(0.0f))
            {
                // By the traffic light at the segment's end.
                const ee::Ptr<zge::PathRecord> segment = zge::followerSegment(follower());
                const ee::Float along = zge::followerDistance(follower());
                const ee::Float toEnd = zge::segmentEnd(segment) - along;
                const uint32_t light = zge::segmentJunction(segment);
                if (!light)
                    return cruise(speed, along, toEnd);
                const int32_t state = lightState(ee::Address(segment));
                if (state == 2)
                    return cruise(speed, along, toEnd);
                if (state != 0 && state != 1)
                    return Drive::Keep;
                if (!(ee::Float(0.0f) < speed))
                    return Drive::Keep;
                const ee::Float time = toEnd / speed;
                const ee::Float stopTime = speed / ee::Float(15.0f) + ee::Float(2.0f);
                if (state == 1)
                {
                    const ee::Float phase = lightCycle(light);
                    if (!(phase - lightElapsed(light) < time))
                        return Drive::Keep;
                }
                return time < stopTime ? Drive::Brake : cruise(speed, along, toEnd);
            }

            // By the car ahead: how soon it would be reached, how near it is.
            const ee::Float room = gap - ee::Float(5.0f);
            const ee::Ptr<zge::Follower> other = nextCar(follower(), speed);
            const ee::Float closing = speed - zge::followerSpeed(other);
            bool closingIn = ee::Float(0.0f) < speed && ee::Float(0.0f) < closing;
            if (!closingIn)
                closingIn = speed < ee::Float(0.0f) && closing < ee::Float(0.0f);
            const ee::Float time = closingIn ? ee::abs(room) / ee::abs(closing) : ee::Float(-1.0f);
            const int soon = time == ee::Float(-1.0f) ? 3 : time < ee::Float(1.0f) ? 0 : time < ee::Float(3.0f) ? 1 : 2;
            const int near = room < ee::Float(10.0f) ? 0 : room < ee::Float(20.0f) ? 1 : 2;
            if (soon == 3) // not closing in
            {
                if (near == 0)
                    return Drive::Keep;
                return Drive::Speed;
            }
            if (soon == 0) // within 1 s
                return Drive::EaseOff;
            if (soon == 2 && near == 2)
                return Drive::Speed;
            if (near == 0)
                return Drive::Brake;
            return soon == 1 && near == 1 ? Drive::Brake : Drive::Keep;
        };
        const Drive drive = decide();

        const ee::Float sign = speed < ee::Float(0.0f) ? ee::Float(-1.0f) : ee::Float(1.0f);
        ee::Float wanted = ee::abs(speed);
        switch (drive)
        {
        case Drive::Brake: wanted = wanted - ee::Float(15.0f); break;
        case Drive::EaseOff: wanted = wanted - ee::Float(1.0f); break;
        case Drive::Speed:  wanted = wanted + ee::Float(1.5f); break;
        default: break;
        }
        // Up to the car's maximum (asked for again each time it is used).
        const auto maximum = [&] {
            return ee::abs(zge::tagFloat(objectComponent(car->object), 8, 4));
        };
        bool moving;
        if (maximum() <= wanted)
            moving = ee::Float(0.0f) < maximum();
        else
            moving = ee::Float(0.0f) < wanted;
        ee::Float set = 0.0f;
        if (moving)
            set = maximum() <= wanted ? maximum() : wanted;
        const bool same = ee::abs(speed - set) < *g_sameSpeed;
        if (!same)
            return tail::setFollowerSpeed(car->follower, set * sign);
    }
    READABLE_FUNCTION(driveTrafficCar, Verified);
}
namespace aggressive
{
    namespace
    {
        // The effects' clock (particles.cpp g_clockMs, fx/particles.h g_tickTime).
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_effectsClockMs;
        inline ee::Global<ee::Float, sym::g_effectsTickTime> g_effectsTickTime;
    }

    // updateEffects2: the level effects' frame (dt the frame's time): the
    // clock on by dt (in milliseconds) and the tick's time set, then the
    // placed effects, the movers and the three steps after them. Returns 1.
    int32_t updateLevelEffects(ee::Float dt)
    {
        const ee::Float ms = dt * 1000.0f;
        *g_effectsTickTime = dt;
        *g_effectsClockMs = *g_effectsClockMs + ms;
        aggressive::fx::updateUserEffects(dt);
        moveMovers(dt);
        aggressive::fx::updateLevelWeather(dt);
        aggressive::fx::updatePisserPuff(dt);
        aggressive::fx::updateScreenFlash(dt);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sym::updateEffects2, updateLevelEffects, Verified);
}
