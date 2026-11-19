// The skater's collision response (skaterCollisionResponse): what a hit found by the
// frame's move (moveSkater) does to the skater.
//
// Vectors built by scaling keep the w of the last vector operation, which after
// a call is what the callee left. Those lanes reach the velocity and the
// position, so the order of the vector statements here is the game's.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "sym/aggressive/skater/collision.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCratered = 5; // skaterCrash's kind: a wall that is really a floor

        inline ee::Global<char, sym::surfaceEventTable> g_collideEvent; // "Collide"
        inline ee::Global<ee::Float, sym::g_minBounceSpeed> g_minBounceSpeed;    // 1e-5
        inline ee::Global<ee::Float, sym::g_degreesToAngle5> g_degreesToAngle;   // 65536 / 360
        inline ee::Global<ee::Float, sym::g_floorAgainstUp> g_floorAgainstUp;    // 0.8
        inline ee::Global<ee::Float, sym::g_floorPushOut> g_floorPushOut;        // 0.01
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;                       // (0, -1, 0, 0): up (y grows downward)

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        ee::Float bounceAngle() { return tuning(3, 11); }       // Physics / Bounce Angle
        ee::Float bounceVectorAngle() { return tuning(3, 30); } // Physics / Bounce Vector Angle (degrees)

    }

    int32_t skaterCollisionResponse(ee::Ptr<Player> player, ee::Ptr<zge::QueryResults> hit, ee::Float frameTime)
    {
        ee::Local<Vec4> normal;
        ee::Local<Vec4> hitPoint;
        ee::Local<Vec4> offset;
        ee::Local<Vec4> turned;
        ee::Local<Vec4> direction;
        ee::Local<CollideEvent> event;
        ee::Local<Vec4> intoSurface;

        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;

        skaterTouchSurface(player, hitObject(*hit), hitFace(*hit));
        hitNormal(hit, normal);
        *hitPoint = hit->nodes[0]->faces->point;

        // A surface tilted away from the skater's up by more than Bounce Angle
        // is a wall.
        const ee::Float upAlongNormal = zge::dot(normal, axes.up);
        const bool wall = upAlongNormal < bounceAngle();

        // The surface's "Collide" event decides whether the hit counts.
        const auto surfaceEvent = [&](bool reversed) {
            event->surfaceKind = surfaceKind(hitObject(*hit), hitFace(*hit));
            event->speed = player->speed;
            if (reversed)
                vec3Negate(event->velocity, velocity);
            else
                event->velocity = velocity;
            event->goOn = 1;
            sendObjectEvent(player, hitObject(*hit), g_collideEvent.get(), event);
            return event->goOn != 0;
        };

        if (wall)
        {
            if (!surfaceEvent(false))
                return 0;
            vec3Reflect(&velocity, normal, &velocity);
            if (!(ee::abs(player->speed) < *g_minBounceSpeed))
            {
                *direction = zge::unit(velocity);
                // Sliding along the wall: turned by Bounce Vector Angle about up,
                // away from the wall, when the move is close enough to sideways.
                const ee::Float sideways = zge::dot(axes.right, direction);
                const ee::Float limit = zge::angleCos(static_cast<zge::Angle>(ee::toInt(bounceVectorAngle() * *g_degreesToAngle)));
                if (ee::abs(sideways) < limit)
                {
                    alongPlane(turned, normal, &axes.right);
                    if (sideways < 0.0f)
                    {
                        vec3Negate(turned, turned);
                        rotateAbout(turned, &axes.up, bounceVectorAngle());
                    }
                    else
                        rotateAbout(turned, &axes.up, -bounceVectorAngle());
                    velocity = *turned * player->speed;
                }
                // Speed lost to the wall, then face the way the skater now goes.
                wallSpeedLoss(player, zge::dot(player->lastHitNormal, direction));
                *offset = axes.up;
                axes.forward = zge::unit(velocity);
                orthonormalize(&axes.right, &axes.up, &axes.forward);
                axes.up = *offset;
                orthonormalize(&axes.forward, &axes.right, &axes.up);
            }
            player->airTime = player->airTime + frameTime;
            // A wall that faces up enough (a floor) craters the skater.
            if (*g_floorAgainstUp < zge::dot(*g_worldUp, normal))
            {
                skaterCrash(player, kCratered);
                return 1;
            }
        }
        else
        {
            const State state = player->state;
            const bool inTheAir = state == State::Air || state == State::AirQuarterPipe || state == State::AirSpine;
            if (inTheAir && !surfaceEvent(true))
                return 0;
            // On the floor: the normal becomes the ground normal, the contact
            // is the surface, and the velocity loses its part into the floor.
            player->groundNormal = normal;
            player->contact = hitFace(*hit);
            player->contactNode = hitObject(*hit);
            const ee::Float into = zge::dot(velocity, normal);
            *intoSurface = zge::scaledAfterMeasure(normal, into);
            velocity = velocity - *intoSurface;
            newSpeed(*player);
            player->airTime = 0.0f;
        }

        // Out of the surface: the hit point, pushed along the normal (a wall by
        // a whole unit).
        const ee::Float push = wall ? ee::Float(1.0f) : *g_floorPushOut;
        *offset = zge::scaledAfterMeasure(normal, push);
        player->position = *hitPoint + *offset;
        return wall ? 1 : 0;
    }
    READABLE_FUNCTION_VU0(skaterCollisionResponse, Verified);
}
