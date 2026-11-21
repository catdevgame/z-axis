// The skater's move with its collisions (skaterMoveCollide), once a frame from
// the Ground move (groundMove: (player, frame time, 0)), the Air update's
// landing check ((player, frame time, 1)) and the Grind and Double Grind
// moves ((player, frame time, 0)).
//
// First the head: a segment from the body (0.75 up from the position) to
// the head bone; a hit pushes the skater back out (moveSkater). Then up to
// six probes, each a short segment through the collision query:
//
//   0, 1  from the head, forward and to the right / left (Head Ray Length)
//   2     from the head, along the forward axis (-2.3 on the ground, in the
//         air and in a manual or cess slide; -Head Ray Length else)
//   3, 4  from the body (0.7 up), forward and to the left / right (0.3)
//   5     from the body, along the forward axis (-0.3)
//
// Just off a stall or a horizontal pole (into the air, under a second of
// air) the head probes are left out. A probe that hits a surface facing up
// (more than 0.8 along the world's up) is a floor: in the landing check
// the skater craters (crash kind 5). A wall hit tells the wall's object
// ("Collide", whose script can cancel the rest), pushes the skater off it
// (rideObject) and back out, and plays the hit sound; a car (an object
// of type CAR) moving more than 40 units a second faster than the skater
// knocks it down ("Stop playing in traffic!", crash kind 10) and throws it
// along the car's way. Then the velocity: over a quarter pipe or a spine
// it is put back up the face; else, heading into the wall, it slides along
// it (turned off the wall by Bounce Vector Angle when close to sideways), a
// grind ends in the air, and the skater turns to face the way it goes.
// Returns whether a probe hit anything.
//
// Vectors built by scaling keep the w of the last vector operation, and those
// lanes reach the velocity and the position: the order of the vector
// statements here is the game's, and is part of what it does.

#include "moves.h"
#include "aggressive/level/traffic_car.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/follower.h"
#include "aggressive/skater/skater.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/library.h"

#include <cstring>
#include "sym/aggressive/skater/move_collide.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCratered = 5;   // skaterCrash kinds: "You cratered!"
        constexpr uint32_t kGoneSwimming = 7, kBurnedUp = 8, kOutOfBounds = 9;
        constexpr uint32_t kTraffic = 10;   // "Stop playing in traffic!"
        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;
        constexpr uint32_t kHitSound = 0x1D;
        constexpr int32_t kQuietestHit = 0x40;
        constexpr zge::Angle kEighthTurn = 0x2000;
        constexpr ee::Float kCarKnockDownSpeed = 40.0f;
        constexpr ee::Float kGrindHopSpeed = 3.0f;
        constexpr ee::Float kHeadBodyHeight = 0.75f;
        constexpr ee::Float kLoudHitSpeed = 5.0f; // slower hits make no sound

        constexpr Vec4 kOrigin{0.0f, 0.0f, 0.0f, 1.0f};

        inline ee::Global<char, sym::headBoneName> g_headBoneName;        // "Head"
        inline ee::Global<char, sym::carTypeName> g_carTypeName;          // "CAR"
        inline ee::Global<char, sym::surfaceEventTable> g_collideEvent;   // "Collide"

        inline ee::Global<ee::Float, sym::g_shortRay> g_shortRay;          // 1e-5: a head segment shorter is not tested
        inline ee::Global<ee::Float, sym::g_headPushBack> g_headPushBack;      // 1.01
        inline ee::Global<ee::Float, sym::g_lowProbeLength> g_lowProbeLength;    // 2.3
        inline ee::Global<ee::Float, sym::g_bodyHeight3> g_bodyHeight3;       // 0.7 (one copy per probe)
        inline ee::Global<ee::Float, sym::g_bodyHeight4> g_bodyHeight4;       // 0.7
        inline ee::Global<ee::Float, sym::g_bodyProbeLength> g_bodyProbeLength;   // 0.3
        inline ee::Global<ee::Float, sym::g_bodyHeight5> g_bodyHeight5;       // 0.7
        inline ee::Global<ee::Float, sym::g_backProbeLength> g_backProbeLength;   // -0.3
        inline ee::Global<ee::Float, sym::g_probePushBack> g_probePushBack;     // 1.01
        inline ee::Global<ee::Float, sym::g_floorAgainstUp2> g_floorAgainstUp;    // 0.8
        inline ee::Global<ee::Float, sym::g_trafficThrow> g_trafficThrow;      // 1.3
        inline ee::Global<ee::Float, sym::g_pipeNormalPush> g_pipeNormalPush;    // 0.1
        inline ee::Global<ee::Float, sym::g_squareToUp> g_squareToUp;        // 0.99
        inline ee::Global<ee::Float, sym::g_stillSpeed2> g_stillSpeed;        // 1e-5
        inline ee::Global<ee::Float, sym::g_degreesToAngle6> g_degreesToAngle;    // 65536 / 360
        inline ee::Global<ee::Float, sym::g_uprightAgainstUp> g_uprightAgainstUp;  // 0.7
        inline ee::Global<ee::Float, sym::g_headingAgainstUp> g_headingAgainstUp;  // 0.8
        inline ee::Global<ee::Float, sym::g_stillSpeedTurn> g_stillSpeedTurn;    // 1e-5
        inline ee::Global<ee::Float, sym::g_squareToUpTurn> g_squareToUpTurn;    // 0.99
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;           // (0, -1, 0, 0): up (y grows downward)

        enum PhysicsEntry : uint32_t
        {
            kBounceVectorAngle = 30, // degrees
            kHeadRayLength = 35,
        };
        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }

        // The six probes, in the order they are tried.
        enum Probe : int32_t
        {
            kHeadRight,
            kHeadLeft,
            kHeadForwardAxis,
            kBodyLeft,
            kBodyRight,
            kBodyForwardAxis,
            kProbes,
        };

        // The hit sound's volume: 64 and 4 a unit of speed, 127 at most.
        int32_t hitVolume(ee::Float speed)
        {
            ee::Float volume = speed * ee::Float(4.0f) + ee::Float(64.0f);
            if (ee::Float(127.0f) < volume)
                volume = 127.0f;
            return ee::toInt(volume);
        }

        bool overPipe(State state) { return state == State::AirQuarterPipe || state == State::AirSpine; }
        bool grinding(State state) { return state == State::Grind || state == State::HandGrind || state == State::DoubleGrind; }

        // Just off a stall (into the air or a quarter pipe's air) or a
        // horizontal pole (into the air), under a second of air.
        bool justOffAHold(const Player &player)
        {
            const State left = static_cast<State>(player.previousState);
            const bool fromHold = left == State::Stall ? player.state == State::Air || player.state == State::AirQuarterPipe
                                                       : left == State::HorizontalPole && player.state == State::Air;
            return fromHold && player.airTime < ee::Float(1.0f);
        }

        // The second, forward-axis head probe's length.
        ee::Float headForwardLength(State state)
        {
            switch (state)
            {
            case State::Ground:
            case State::Stand:
            case State::Air:
            case State::Manual:
            case State::CessSlide:
                return -*g_lowProbeLength;
            default:
                return -physics(kHeadRayLength);
            }
        }

        // The skater faces the velocity, unless that is square to its up
        // (`square`: how close counts).
        void faceVelocity(ee::Ptr<Player> player, Vec4 &scratch, ee::Float square)
        {
            PlayerAxes &axes = player->axes;
            scratch = zge::unit(player->velocity);
            if (!(ee::abs(zge::dot(axes.up, scratch)) < square))
                return;
            scratch = axes.up;
            axes.forward = zge::unit(player->velocity);
            orthonormalize(&axes.right, &axes.up, &axes.forward);
            axes.up = scratch;
            orthonormalize(&axes.forward, &axes.right, &axes.up);
        }

        // The hit's sound. A car faster than the skater by more than 40
        // knocks it down and throws it the car's way, an eighth of a turn off.
        void wallHitSound(ee::Ptr<Player> player, ee::Ptr<zge::Node> wallObject, ee::Local<Vec4> &scratch)
        {
            Vec4 &velocity = player->velocity;
            const Vec4 &position = player->position;
            const uint32_t typeName = classTypeName(zge::objectClass(wallObject));
            if (typeName == zge::nameHash(g_carTypeName.get()))
            {
                const ee::Ptr<TrafficCar> car = trafficCarOf(zge::nodeSceneObject(wallObject, 0));
                trafficCarVelocity(car, scratch);
                *scratch = *scratch - velocity;
                if (!(kCarKnockDownSpeed < vec3Length(scratch)))
                {
                    aggressive::soundEvent(kHitSound, &position, 0, kQuietestHit, 0);
                    return;
                }
                skaterCrash(player, kTraffic);
                trafficCarVelocity(car, &velocity);
                velocity = velocity * *g_trafficThrow;
                vec3RotateY(velocity, velocity, kEighthTurn);
                newSpeed(*player);
                aggressive::soundEvent(kHitSound, &position, 0, hitVolume(player->speed), 0);
                return;
            }
            int32_t variant = 0;
            if (player->object && player->surface != kNoSurface)
                variant = surfaceSound(player->objectNode, player->surface);
            const ee::Float speed = player->speed;
            if (kLoudHitSpeed < speed)
                aggressive::soundEvent(kHitSound, &position, variant, hitVolume(speed), 0);
        }

        // Heading into the wall: the velocity slides along it. Close to
        // sideways it turns off the wall by Bounce Vector Angle about up: on
        // the ground along the wall at the speed, the way the skater went;
        // elsewhere, upright or heading up, the heading along the wall.
        void slideAlongWall(ee::Ptr<Player> player, bool mover, ee::Local<Vec4> &scratch, ee::Local<Vec4> &along, ee::Local<Vec4> &heading)
        {
            Vec4 &velocity = player->velocity;
            const PlayerAxes &axes = player->axes;
            Vec4 &wallNormal = player->lastHitNormal;

            vec3Reflect(&velocity, &wallNormal, &velocity);
            const bool still = ee::abs(player->speed) < *g_stillSpeed;
            if (still || mover)
                return;

            *heading = zge::unit(velocity);
            const ee::Float sideways = zge::dot(axes.right, heading);
            const ee::Float bounce = physics(kBounceVectorAngle);
            const zge::Angle maxAngle = static_cast<zge::Angle>(ee::toInt(bounce * *g_degreesToAngle));
            if (ee::abs(sideways) < zge::angleCos(maxAngle))
            {
                if (player->state == State::Ground || player->state == State::Manual)
                {
                    *scratch = zge::cross(axes.up, wallNormal);
                    const ee::Float way = zge::dot(scratch, velocity);
                    velocity = *scratch * player->speed;
                    if (way < ee::Float(0.0f))
                    {
                        vec3Negate(velocity, velocity);
                        rotateAbout(&velocity, &axes.up, physics(kBounceVectorAngle));
                    }
                    else
                        rotateAbout(&velocity, &axes.up, -physics(kBounceVectorAngle));
                }
                else if (*g_uprightAgainstUp < zge::dot(axes.up, *g_worldUp) || *g_headingAgainstUp < zge::dot(heading, axes.up))
                {
                    alongPlane(along, &wallNormal, heading);
                    if (sideways < ee::Float(0.0f))
                        rotateAbout(along, &axes.up, physics(kBounceVectorAngle));
                    else
                        rotateAbout(along, &axes.up, -physics(kBounceVectorAngle));
                    velocity = *along * player->speed;
                }
            }
            wallSpeedLoss(player, zge::dot(wallNormal, heading));
        }
    }

    int32_t skaterMoveCollide(ee::Ptr<Player> player, ee::Float frameTime, uint32_t landingCheck)
    {
        const bool landing = (landingCheck & 0xFFu) != 0;

        // Game code reaches these through pointers; they stay in this order.
        ee::Local<zge::QuerySegment> segment;
        ee::Local<zge::QueryResults> results;
        ee::Local<uint32_t[2]> skipped; // the nodes the query skips: the skater's own, then 0
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> before;         // the position before a push back
        ee::Local<Vec4> origin;         // the head, then the probes' start
        ee::Local<Vec4> ray;            // the segment's direction
        ee::Local<Vec4> normal;         // the face hit's
        ee::Local<Vec4> hitPoint;
        ee::Local<Vec4> pushBack;       // from the hit back out
        ee::Local<CollideEvent> collide;
        ee::Local<Vec4> along;
        ee::Local<Vec4> heading;

        Vec4 &position = player->position;
        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        Vec4 &lastHitNormal = player->lastHitNormal;

        const auto scene = [&] { return zge::sceneOf(player->node); };
        const auto query = [&] {
            const uint32_t flags = uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed;
            return zge::collisionQuery(kQuerySegment, segment, flags, 0, 0, skipped, results, 0) != 0;
        };

        // The hit's normal and point; pushBack gets the way out: past the hit
        // by the segment's length and a unit when the face is turned away from
        // the segment, else `margin` of the way from the segment's end.
        const auto measureHit = [&](ee::Float margin) {
            hitNormal(results, normal);
            *hitPoint = results->nodes[0]->faces->point;
            *scratch = zge::unit(ray);
            if (ee::Float(0.0f) < zge::dot(scratch, normal))
            {
                *pushBack = *hitPoint - segment->start;
                const ee::Float past = vec3Length(ray) + ee::Float(1.0f);
                *pushBack = *pushBack * past;
            }
            else
            {
                *pushBack = *hitPoint - segment->end;
                *pushBack = *pushBack * margin;
            }
        };

        // The head bone in world space.
        zge::updatePose(player->node);
        const ee::Ptr<zge::Scene> model = scene();
        const uint32_t headHash = zge::nameHash(g_headBoneName.get());
        const ee::Ptr<zge::SceneBone> headBone = zge::findSceneModelBone(model, headHash);
        const auto headPoint = [&] {
            *scratch = kOrigin;
            zge::bonePoint(scene(), headBone, scratch, origin, 0);
        };
        headPoint();
        skipped[0] = ee::Address(sceneObject(scene())); // the skip list's word
        skipped[1] = 0;
        lastHitNormal = kOrigin;
        *pushBack = kOrigin;
        headPoint();

        // From the body to the head: a hit pushes the skater back out.
        segment->start = position;
        *scratch = *g_worldUp * kHeadBodyHeight;
        segment->start = segment->start + *scratch;
        segment->end = origin;
        *ray = segment->end - segment->start;
        const bool headClose = ee::abs(vec3Length(ray)) < *g_shortRay;
        if (!headClose && query())
        {
            measureHit(*g_headPushBack);
            *scratch = position + *pushBack;
            moveSkater(player, &position, scratch, frameTime);
        }

        const auto headRay = [&](const Vec4 &sideways) {
            *ray = sideways + axes.forward;
            *ray = zge::unit(ray);
            *ray = *ray * physics(kHeadRayLength);
        };
        const auto bodyStart = [&](ee::Float height) {
            *origin = position;
            *scratch = axes.up * height;
            *origin = *origin + *scratch;
            segment->start = origin;
        };
        const auto bodyRay = [&](const Vec4 &sideways) {
            *ray = sideways + axes.forward;
            *ray = zge::unit(ray);
            *ray = *ray * *g_bodyProbeLength;
        };

        int32_t result = 0; // a probe hit something
        for (int32_t probe = justOffAHold(*player) ? kBodyLeft : kHeadRight; probe < kProbes; ++probe)
        {
            switch (probe)
            {
            case kHeadRight:
                headPoint(); // (posed again)
                segment->start = origin;
                headRay(axes.right);
                break;
            case kHeadLeft:
                segment->start = origin;
                vec3Negate(scratch, axes.right);
                headRay(scratch);
                break;
            case kHeadForwardAxis:
                segment->start = origin;
                *ray = axes.forward;
                *ray = zge::unit(ray);
                *ray = *ray * headForwardLength(player->state);
                break;
            case kBodyLeft:
                bodyStart(*g_bodyHeight3);
                vec3Negate(scratch, axes.right);
                bodyRay(scratch);
                break;
            case kBodyRight:
                bodyStart(*g_bodyHeight4);
                *scratch = axes.right;
                bodyRay(scratch);
                break;
            default:
                bodyStart(*g_bodyHeight5);
                *ray = axes.forward;
                *ray = zge::unit(ray);
                *ray = *ray * *g_backProbeLength;
                break;
            }
            segment->end = *origin + *ray;

            if (!query())
                continue;

            skaterTouchSurface(player, hitObject(*results), hitFace(*results));
            measureHit(*g_probePushBack);
            const bool floor = *g_floorAgainstUp < zge::dot(*g_worldUp, normal);
            lastHitNormal = normal;
            result = 1;
            if (floor)
            {
                if (!landing)
                    continue;
                skaterCrash(player, kCratered);
                break;
            }

            // (Read again here: the records belong to the query's pool, which
            // what the touch set off may have used.)
            const ee::Ptr<zge::Node> object = hitObject(*results);
            const uint32_t face = hitFace(*results);
            const int32_t mover = isMover(object);
            // In the air the forward head probe counts only within Head Ray
            // Length of its start, unless the object moves by itself.
            if (probe == kHeadForwardAxis && player->state == State::Air && !mover)
            {
                *scratch = *hitPoint - segment->start;
                if (physics(kHeadRayLength) < vec3Length(scratch))
                    continue;
            }

            collide->surfaceKind = surfaceKind(object, face);
            collide->speed = player->speed;
            collide->velocity = velocity;
            collide->goOn = 1;
            sendObjectEvent(player, object, g_collideEvent.get(), collide);
            if (collide->goOn == 0)
                break;

            // Pushed off and back out; the probes' start moves with the skater.
            rideObject(player, object, frameTime);
            *before = position;
            *scratch = position + *pushBack;
            moveSkater(player, &position, scratch, frameTime);
            *scratch = position - *before;
            *origin = *origin + *scratch;

            wallHitSound(player, object, scratch);

            if (overPipe(player->state))
            {
                // Over a quarter pipe or a spine: the velocity back along the
                // world's up, a little off the wall, the skater turned to it.
                const ee::Float upward = zge::dot(*g_worldUp, velocity);
                velocity = *g_worldUp * upward;
                *scratch = lastHitNormal * *g_pipeNormalPush;
                velocity = velocity + *scratch;
                newSpeed(*player);
                faceVelocity(player, scratch, *g_squareToUp);
                player->noSpinTurn = 0;
                continue;
            }

            if (zge::dot(axes.forward, lastHitNormal) <= ee::Float(0.0f))
                slideAlongWall(player, mover != 0, scratch, along, heading);

            // A grind ends in the air, a little up.
            if (grinding(player->state))
            {
                changeState(player, State::Air);
                *scratch = *g_worldUp * kGrindHopSpeed;
                velocity = velocity + *scratch;
            }
            if (!(ee::abs(player->speed) < *g_stillSpeedTurn))
                faceVelocity(player, scratch, *g_squareToUpTurn);
        }

        if (!result)
        {
            player->ride.object = {};
            player->ride.moving = 0;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(skaterMoveCollide, Verified);

    // ---- its callees -----------------------------------------------------------

    namespace
    {
        // The surface sound of a hazard (surfaceSound).
        enum HazardSound : int32_t
        {
            kWater = 5,
            kFire = 12,
            kOutOfBoundsSound = 14,
            kOutOfBoundsHidden = 15,
        };
        constexpr uint32_t kCarComponent = 0x1C;

        // Physics / Collision V Min%, Max%, Angle, V% Air: the speed kept
        // after a wall hit.
        enum CollisionEntry : uint32_t
        {
            kCollisionVMin = 13,
            kCollisionVMax = 14,
            kCollisionVAngle = 15,
            kCollisionVAir = 16,
        };
        inline ee::Global<ee::Float, sym::g_degreesToAngleSpeed> g_degreesToAngleSpeed; // 65536 / 360
    }

    // skaterTouchSurface: the skater touched a face of an object: a hazard's
    // surface sound crashes it (unless it is down already): water splashes,
    // goes swimming once (Player::splashed) and hides the skater, fire burns it
    // up, out of bounds (hidden for 15). Each but fire and plain out of
    // bounds asks for the respawn at a locator.
    void skaterTouchSurface(ee::Ptr<Player> player, ee::Ptr<zge::Node> object, uint32_t face)
    {
        if (!object || face == kNoSurface)
            return;
        surfacePhysics(object, face);
        switch (surfaceSound(object, face))
        {
        case kWater:
            if (player->splashed == 1)
                return;
            player->splashed = 1;
            aggressive::fx::splash(&player->position, &player->velocity);
            if (!(player->flags & kDown))
                skaterCrash(player, kGoneSwimming);
            zge::setSceneObjectShown(player->node, 0);
            player->flags |= kRespawnAtLocator;
            return;
        case kFire:
            if (!(player->flags & kDown))
                game::tail(aggressive::skater::skaterCrash, player, kBurnedUp);
            return;
        case kOutOfBoundsSound:
            if (!(player->flags & kDown))
                game::tail(aggressive::skater::skaterCrash, player, kOutOfBounds);
            return;
        case kOutOfBoundsHidden:
            if (!(player->flags & kDown))
            {
                skaterCrash(player, kOutOfBounds);
                zge::setSceneObjectShown(player->node, 0);
            }
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(skaterTouchSurface, Verified);

    // wallSpeedLoss: the speed a wall hit leaves (amount: how square to the
    // wall the skater went, the normal against the heading). On the ground
    // Collision V Max% of it when within Collision V Angle of along the
    // wall, down to Collision V Min% square to it; elsewhere Collision V%
    // Air. The velocity keeps its direction at the new speed.
    void wallSpeedLoss(ee::Ptr<Player> player, ee::Float amount)
    {
        const ee::Float limit = zge::angleCos(static_cast<zge::Angle>(ee::toInt(physics(kCollisionVAngle) * *g_degreesToAngleSpeed)));
        ee::Float &speed = player->speed;
        if (player->state != State::Ground)
            speed = physics(kCollisionVAir) * speed;
        else if (limit < ee::abs(amount))
        {
            const ee::Float t = (amount - limit) / (ee::Float(1.0f) - limit);
            const ee::Float speedFactor = physics(kCollisionVMax);
            const ee::Float lost = (speedFactor - physics(kCollisionVMin)) * t;
            speed = (speedFactor - lost) * speed;
        }
        else
            speed = physics(kCollisionVMax) * speed;

        Vec4 &velocity = player->velocity;
        velocity = zge::unit(velocity);
        velocity = zge::scaledAfterMeasure(velocity, speed);
    }
    READABLE_FUNCTION_VU0(wallSpeedLoss, Verified);

    // vec3Reflect: out = v less twice its part along the unit normal (a
    // reflection off the plane). out.w stays as it was.
    void vec3Reflect(ee::Ptr<const Vec4> v, ee::Ptr<const Vec4> normal, ee::Ptr<Vec4> out)
    {
        const ee::Float along = zge::dot(*normal, *v);
        const ee::Float twice = along + along;
        out->x = v->x - twice * normal->x;
        out->y = v->y - twice * normal->y;
        out->z = v->z - twice * normal->z;
    }
    READABLE_FUNCTION_VU0(vec3Reflect, Verified);

    // trafficCarOf: a scene object's traffic car (its component of kind
    // 0x1C), 0 for none.
    ee::Ptr<TrafficCar> trafficCarOf(ee::Ptr<zge::SceneObject> sceneObject)
    {
        const ee::Ptr<zge::TagRecord> components = zge::objectComponent(sceneObject);
        if (!components)
            return {};
        // The component's word is the car.
        const ee::Ptr<TrafficCar> car(zge::tagWord(components, 8, kCarComponent));
        return car;
    }
    READABLE_FUNCTION(trafficCarOf, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_matrixSame> g_matrixSame; // matrix entries closer than this are the same
        constexpr Vec4 kNoMove{0.0f, 0.0f, 0.0f, 1.0f};
        constexpr uint32_t kWorldMatrix = 1, kInverseMatrix = 2; // nodeMatrix's `which`
    }

    // rideObject: the object the skater hits or rides carries it: on a
    // new object its matrices are kept; on the next frame they are compared
    // with the object's (moved or not); while it moves, the skater's
    // orientation turns with it and its move of the skater (where last
    // frame's position went with the object) is kept for the move
    // (RiddenObject::move). Without an object, or once it stops, that ends.
    void rideObject(ee::Ptr<Player> player, ee::Ptr<zge::Node> object, ee::Float frameTime)
    {
        ee::Local<Vec4> inObject;
        ee::Local<Vec4> moved;
        ee::Local<zge::Matrix> now;
        ee::Local<zge::Matrix> lastInverse;
        RiddenObject &ride = player->ride;
        ride.frameTime = frameTime;
        if (object)
            zge::updateMovedNode(zge::rootNode(object));

        bool carried;
        if (object != ride.object)
        {
            ride.object = object;
            ride.kept = object ? 0 : 1;
            carried = object != 0;
        }
        else if (ride.kept)
            carried = ride.moving != 0;
        else
        {
            // The second frame on it: has it moved? (The three rows and the
            // translation, entry by entry, stopping at the first that differs.)
            *now = *zge::nodeMatrix(object, kWorldMatrix);
            bool same = true;
            for (int i = 0; same && i < 12; ++i)
                same = ee::abs(ride.world.m[i / 3][i % 3] - now->m[i / 3][i % 3]) < *g_matrixSame;
            ride.moving = same ? 0 : 1;
            ride.kept = 1;
            carried = ride.moving != 0;
        }

        if (!carried)
        {
            if (ride.moving)
            {
                ride.moving = 0;
                ride.object = {};
                ride.move = kNoMove;
            }
            return;
        }
        if (!ride.kept)
        {
            // A new object: its matrices and the skater's position kept.
            ride.inverse = *zge::nodeMatrix(ride.object, kInverseMatrix);
            ride.world = *zge::nodeMatrix(ride.object, kWorldMatrix);
            ride.lastPosition = player->position;
            ride.move = kNoMove;
            ride.moving = 1;
            return;
        }

        // Moving: the object's change since last frame (last inverse, this
        // world) turns the skater, and takes last frame's position along.
        zge::matrixMultiply(now, &ride.inverse, zge::nodeMatrix(ride.object, kWorldMatrix));
        zge::matrixMultiply(&player->orientation, now, &player->orientation);
        *lastInverse = ride.inverse;
        *inObject = zge::transformed(*lastInverse, ride.lastPosition);
        ride.inverse = *zge::nodeMatrix(ride.object, kInverseMatrix);
        ride.world = *zge::nodeMatrix(ride.object, kWorldMatrix);
        *moved = zge::transformed(ride.world, inObject);
        ride.move = *moved - ride.lastPosition;
        ride.lastPosition = player->position;
        ride.moving = 1;
    }
    READABLE_FUNCTION(rideObject, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_degreesToAngleTurn> g_degreesToAngleTurn;
    }

    // alongPlane: out = v less its part along the unit normal (v along the
    // plane). The part keeps the w the last vector operation left, and out's
    // w comes from it.
    void alongPlane(ee::Ptr<Vec4> out, ee::Ptr<const Vec4> normal, ee::Ptr<const Vec4> v)
    {
        ee::Local<Vec4> part;
        const ee::Float along = zge::dot(*v, *normal);
        *part = *normal * along;
        *out = *v - *part;
    }
    READABLE_FUNCTION_VU0(alongPlane, Verified);

    // rotateAbout: the vector turned about the axis by `degrees`. Its w is
    // what making the matrix left.
    void rotateAbout(ee::Ptr<Vec4> vector, ee::Ptr<const Vec4> axis, ee::Float degrees)
    {
        ee::Local<zge::Quaternion> turn;
        ee::Local<zge::Matrix> matrix;
        const zge::Angle angle = static_cast<zge::Angle>(ee::toInt(degrees * *g_degreesToAngleTurn));
        zge::quaternionFromAxisAngle(turn, &axis->xyz(), angle);
        zge::quaternionToMatrix(matrix, turn);
        *vector = zge::rotated(*matrix, *vector);
    }
    READABLE_FUNCTION_VU0(rotateAbout, Verified);

    // hitNormal: the normal of the first face a query hit, in world space:
    // the face's normal in its part, turned by the hit node's world matrix
    // with the translation taken out. Its w is what clearing that left.
    void hitNormal(ee::Ptr<zge::QueryResults> results, ee::Ptr<Vec4> out)
    {
        ee::Local<zge::Matrix> rotation;
        const ee::Ptr<zge::HitNode> first = results->nodes[0];
        zge::faceNormal(first->part, first->faces->face, out);
        *rotation = *zge::nodeMatrix(results->nodes[0]->node, kWorldMatrix);
        zge::matrixClearTranslation(rotation);
        *out = zge::transformed(*rotation, *out);
    }
    READABLE_FUNCTION_VU0(hitNormal, Verified);

    // trafficCarVelocity: a traffic car's velocity: its path follower's heading
    // made unit and scaled by the follower's speed; (0, 0, 0, 1) for a car
    // with no follower. Returns 1.
    int32_t trafficCarVelocity(ee::Ptr<TrafficCar> car, ee::Ptr<Vec4> out)
    {
        const ee::Ptr<zge::Follower> follower(car->follower); // the car record's word
        if (!follower)
        {
            *out = kNoMove;
            return 1;
        }
        zge::followerHeading(follower, out);
        *out = zge::unit(*out);
        const ee::Float speed = zge::followerSpeed(follower);
        *out = zge::scaledAfterMeasure(*out, speed); // (followerSpeed leaves VU0 alone)
        return 1;
    }
    READABLE_FUNCTION_VU0(trafficCarVelocity, Verified);

    void gameHookNoop()
    {
    }
    READABLE_FUNCTION(gameHookNoop, Verified);
}
