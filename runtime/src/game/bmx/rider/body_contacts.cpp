// BMX XXX's rider's bodies against the world (physics.c): the contact
// callbacks newRider gives the rider's three dynamics (the bail's
// "dynamics.rider", the air's motions' "dynamics.riderAnim", the bike's
// "dynamics.bike"; each dynamics' owner word is the rider). BMX XXX's own:
// DM2's rider has no callbacks of its own. Each asks the level objects'
// callback too (dynamicsObjectContact), which says which faces count.

#include "collision/collision.h"
#include "engine_calls.h"
#include "game_calls.h"
#include "rider.h"

#include "bmx/level/level_collision.h"

#include "bmx/sound/events.h"
#include "bmx/game/pickups.h"
#include "bmx/fx/emitter_objects.h"
#include "bmx/fx/spawns.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/body_contacts.cpp.h"

namespace game::bmx::rider
{
    using collision::FaceRef;

    namespace
    {
        // A hit's speed: the body's velocity into the face (a second's). The rider's bodies' hits count from 4,
        // and sound at a level by it (from 9, 17 and 30 a level more); the bike's from 5, and sound from 9.
        constexpr ee::Float kBodyHitLeast = 4.0f;
        constexpr ee::Float kBodyHitLevel1 = 9.0f;
        constexpr ee::Float kBodyHitLevel2 = 17.0f;
        constexpr ee::Float kBodyHitLevel3 = 30.0f;
        constexpr ee::Float kBikeHitLeast = 5.0f;
        constexpr ee::Float kBikeHitSounds = 9.0f;

        // The body parts a hit sounds by (BodyTouches::bumpPart): the rider model's bones' names.
        enum BodyPart : uint32_t
        {
            kPartFoot = 0,
            kPartHips = 1,
            kPartLowerArm = 2,
            kPartHead = 3,
            kPartNone = 0xFFFFFFFFu, // any other body: no hit
        };
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsName;
        inline ee::Global<uint32_t, sym::g_headBoneHash> g_headName;
        inline ee::Global<uint32_t, sym::g_leftLowArmBoneHash> g_leftLowArmName;
        inline ee::Global<uint32_t, sym::g_rightLowArmBoneHash> g_rightLowArmName;
        inline ee::Global<uint32_t, sym::g_leftFootBoneHash> g_leftFootName;
        inline ee::Global<uint32_t, sym::g_rightFootBoneHash> g_rightFootName;

        // The hit's surface by the face's sound (0 .. 15).
        struct BumpSurfaces
        {
            uint8_t bySound[16];
        };
        inline ee::Global<BumpSurfaces, sym::g_bumpSurfaces> g_bumpSurfaces;
        // The faces' sounds a hit raises dust off (dirt and the like: groundImpact's 5, 10 and 11, and 8 and 12);
        // the bike's other hits throw sparks.
        bool dustySound(uint32_t sound) { return sound == 5 || sound == 8 || (sound >= 10 && sound <= 12); }
        constexpr uint32_t kNoFaceSound = 0xFFFFu;
        // A water surface's hits do not sound (the level object type "WaterSurface").
        constexpr uint32_t kWaterSurfaceType = 0x0D102F05;

        // The dust's motion: the body's velocity with the hit taken off, a frame's (1/60).
        inline ee::Global<ee::Float, sym::g_bodyDustMotionScale> g_bodyDustMotionScale;
        // The pad's rumble on the large motor by the hit's speed (765 a unit, at most 255).
        inline ee::Global<ee::Float, sym::g_bodyHitRumble> g_bodyHitRumble;
        constexpr int32_t kRumbleMost = 0xFF;
        constexpr uint32_t kRumbleLargeMotor = 1;
        // The bike's: its dust's motion as the rider's; its sparks' motion the velocity with twice the hit turned
        // back, a frame's, 0.4 of them a unit of speed; its hit sound's level 25/127 a unit of speed, at most 127.
        inline ee::Global<ee::Float, sym::g_bikeDustMotionScale> g_bikeDustMotionScale;
        inline ee::Global<ee::Float, sym::g_bikeSparkMotionScale> g_bikeSparkMotionScale;
        inline ee::Global<ee::Float, sym::g_bikeSparksPerSpeed> g_bikeSparksPerSpeed;
        inline ee::Global<ee::Float, sym::g_bikeHitLevelPerSpeed> g_bikeHitLevelPerSpeed;
        constexpr int32_t kBikeHitLevelMost = 0x7F;
        constexpr uint32_t kSoundBikeHit = 35;

        // The face a contact touched: none for a scene object's (or no object), or one of a node without faces.
        FaceRef contactFace(ee::Ptr<zge::FaceContact> contact)
        {
            const uint32_t node = ee::Address(contact->object->node);
            if (!collision::nodeFaceCount(collision::widen(node)))
                return 0;
            return collision::makeFace(node, int32_t(contact->face));
        }
        // The velocity into a contact's face.
        ee::Float speedInto(ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
        {
            return -zge::dot(body->velocity, contact->normal);
        }

        BodyPart bodyPart(uint32_t name)
        {
            if (name == *g_hipsName)
                return kPartHips;
            if (name == *g_headName)
                return kPartHead;
            if (name == *g_leftLowArmName || name == *g_rightLowArmName)
                return kPartLowerArm;
            if (name == *g_leftFootName || name == *g_rightFootName)
                return kPartFoot;
            return kPartNone;
        }
    }

    namespace
    {
        // airBodyContact's tuning: the touch's velocity a frame's, the hit's speed a frame's (both 1/60); a body
        // keeps 0.1 off a wall; a wall's push back is 1.05 of the speed into it head on (else 1.25), and along it
        // half of it.
        inline ee::Global<ee::Float, sym::g_airTouchVelocityScale> g_airTouchVelocityScale;
        inline ee::Global<ee::Float, sym::g_airHitSpeedScale> g_airHitSpeedScale;
        inline ee::Global<ee::Float, sym::g_airWallGap> g_airWallGap;
        inline ee::Global<ee::Float, sym::g_airWallHeadOnPush> g_airWallHeadOnPush;
        constexpr ee::Float kAirWallGlancingPush = 1.25f;
        constexpr ee::Float kAirWallAlongPush = 0.5f;
        // Under this (0.01) a row is square to the wall: the next row tells the side.
        inline ee::Global<ee::Float, sym::g_airWallSquare> g_airWallSquare;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        constexpr ee::Float kHalf = 0.5f;
        constexpr ee::Float kFramesASecond = 60.0f;
        // A face steeper than about 75 degrees is a wall; a velocity within 30 degrees of its normal meets it head
        // on; the rider realigns along it when within 30 degrees of upright, and bails off the ground tipped past
        // 45.
        constexpr zge::Angle kWallSteepest = 0x3556;
        constexpr zge::Angle kHeadOnMost = 0x1555;
        constexpr zge::Angle kUprightMost = 0x1555;
        constexpr zge::Angle kTippedMost = 0x2001;
        constexpr int16_t kWallAlignFrames = 7;
        constexpr uint32_t kShapeBox = 0; // RigidBody::shape
        // The touch message's word for an air body's touch (touchObject's callers give theirs).
        constexpr uint32_t kTouchAirBody = 2;
        // bailKind for a bail off the air's bodies hitting the ground tipped over.
        constexpr uint8_t kBailBodiesTipped = 5;
    }

    // airBodyContact (rider, dynamics, body, contact): a body of the air's motions against a face, in the air
    // states (1 and 8; else 1). A scene object's face is touched first (the touch message, shown to the game
    // unless the rider has no player): one that is not taken answers 0. Then the touches note the face, its
    // nearest point, its point and normal. Moving into a wall: the rider pushed off it (by the body's corner
    // deepest in it), the speed into it turned back (head on with a push along the wall to the side the rider
    // is going), and nearly upright and not spinning the rows realigned along it over 7 frames; the bodies take
    // the velocity; 0. Off the ground tipped past 45 degrees: a bail, 1. Else 0.
    int32_t airBodyContact(Rider *rider, ee::Ptr<zge::Dynamics> /*dynamics*/, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> onPlane;  // the body's point on the face's plane
        ee::Local<zge::Vec4> toPoint;  // from the body's centre to it
        ee::Local<zge::Vec4> step;     // the touches' step, a corner's direction, the push, the way along the wall
        ee::Local<zge::Vec4> corner;
        ee::Local<zge::Matrix> along;  // the rows along the wall (right, up, forward)
        ee::Local<zge::EngineMessage> message;
        ee::Local<collision::TouchData> data;
        ee::Local<zge::Vec4> deepest;  // the corner deepest in the wall (as the frame left it when none is)
        const uint32_t state = physicsState(rider);
        if (state != kPhysicsAir && state != kPhysicsLipTrick)
            return 1;
        data->accept = 1;
        data->how = kTouchAirBody;
        const ee::Ptr<zge::SceneObject> object = zge::nodeSceneObject(contact->object->node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
        if (object)
        {
            const uint32_t node = ee::Address(contact->object->node);
            data->face = int32_t(contact->face);
            data->node = node;
            const FaceRef face = collision::nodeFaceCount(collision::widen(node)) ? collision::makeFace(node, data->face) : 0;
            collision::faceCentre(face, data->point);
            *vec4(data->velocity) = body->velocity * *g_airTouchVelocityScale;
            data->accept = 1;
            if (!hasFlag(rider, kFlagNoPlayer))
            {
                message->target = object;
                message->sender = ee::Address(rider->messenger);
                message->type = collision::kMessageTouch;
                message->flags = zge::kMessageNow;
                message->data = data;
                riderActivatedObject(message);
            }
            if (data->accept == 0)
                return 0;
            zge::sendEngineMessage(message);
        }
        if (data->accept == 0)
            return 0;

        BodyTouches &touches = rider->touches;
        setFlag(rider, kFlagHitHard);
        touches.flags |= kTouchFace;
        *normal = contact->normal;
        const ee::Float speed = -(zge::dot(body->velocity, *normal) * *g_airHitSpeedScale);
        zge::boxPointOnFacePlane(body, onPlane);
        *toPoint = *onPlane - body->position;
        const ee::Float distance = zge::dot(*toPoint, *toPoint);
        if (distance < touches.nearest)
        {
            touches.nearestNode = ee::Address(contact->object->node);
            touches.nearest = distance;
            touches.nearestFace = contact->face;
        }
        *step = *onPlane - *vec4(touches.point);
        *step = *step * kHalf;
        *vec4(touches.point) = *vec4(touches.point) + *step;
        *vec4(touches.normal) = *vec4(touches.normal) + *normal;
        *vec4(touches.normal) = *vec4(touches.normal) * kHalf;

        const bool wall = !(zge::angleAcos(ee::abs(normal->y)) < kWallSteepest);
        if (!wall || !(ee::Float(0.0f) <= speed))
        {
            if (zge::angleAcos(ee::abs(normal->y)) < kWallSteepest && !(zge::angleAcos(-rider->up[1]) < kTippedMost))
            {
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                rider->bailKind = kBailBodiesTipped;
                return 1;
            }
            return 0;
        }

        // Out of the wall: by the box's corner deepest in it (in the rows' space), to the gap.
        touches.flags |= kTouchWall;
        *corner = zge::kZero;
        if (body->shape == kShapeBox)
        {
            ee::Float deepestDepth = 0.0f;
            *corner = zge::kZero;
            for (uint32_t i = 0; i < 8; ++i)
            {
                *corner = body->halfExtents->half;
                *step = *toPoint;
                if (i & 1)
                    step->z = -step->z;
                if (i & 2)
                    step->x = -step->x;
                if (i & 4)
                    step->y = -step->y;
                *corner = zge::transformed(*matrix(rider->right), *corner);
                const ee::Float depth = zge::dot(*corner, *step);
                if (deepestDepth < depth)
                {
                    *deepest = *corner;
                    deepestDepth = depth;
                }
            }
            *corner = *deepest;
        }
        const ee::Float cornerDepth = zge::dot(*corner, *normal);
        const ee::Float gap = *g_airWallGap - cornerDepth;
        const ee::Float out = gap + zge::dot(*step, *normal);
        *step = *normal * out;
        *vec4(rider->position) = *vec4(rider->position) + *step;
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *step;

        // The speed into it turned back: the way the rider goes on the ground against the wall's normal.
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &force = *vec4(rider->force);
        *step = velocity;
        step->y = 0.0f;
        if (!(step->x == ee::Float(0.0f) && step->z == ee::Float(0.0f)))
            *step = zge::unit(*step);
        if (zge::angleAcos(ee::abs(zge::dot(*step, *normal))) < kHeadOnMost)
        {
            touches.flags |= kTouchWallHeadOn;
            force = *normal * (speed * *g_airWallHeadOnPush);
            // A push along the wall, to the side the velocity (or the forward row, or the up row) leans.
            *step = zge::cross(*normal, *g_worldUp);
            ee::Float side = zge::dot(velocity, *step);
            bool sided = *g_airWallSquare < ee::abs(side);
            if (!sided)
            {
                side = zge::dot(*vec4(rider->forward), *step);
                sided = *g_airWallSquare < ee::abs(side);
            }
            if (!sided)
            {
                side = zge::dot(*vec4(rider->up), *step);
                sided = *g_airWallSquare < ee::abs(side);
            }
            if (sided && side < ee::Float(0.0f))
                zge::vec3Negate(&step->xyz(), &step->xyz());
            step->y = 0.0f;
            *step = zge::unit(*step);
            *step = *step * (speed * kAirWallAlongPush);
            force = force + *step;
        }
        else
        {
            force = *normal * (speed * kAirWallGlancingPush);
        }
        velocity = velocity + force;
        rider->speed = zge::vec3Length(&velocity.xyz());

        // Nearly upright and not spinning: the rows realigned along the wall over 7 frames.
        if (zge::angleAcos(-rider->up[1]) < kUprightMost && ee::toFloat(int32_t(rider->yawRate)) == ee::Float(0.0f))
        {
            along->row(2) = zge::unit(velocity);
            along->row(1) = *vec4(rider->up);
            if (rider->snapFrames != 0 && !hasFlag(rider, kFlagSnapAlign))
                along->row(1) = *g_worldUp;
            along->row(0) = zge::cross(along->row(1), along->row(2));
            along->row(0) = zge::unit(along->row(0));
            along->row(2) = zge::cross(along->row(0), along->row(1));
            airRotationTrack(rider->right, &along->m[0][0], kWallAlignFrames, rider->alignAngles);
            *matrix(rider->alignFrom) = *matrix(rider->right);
            rider->snapFrames = kWallAlignFrames;
            clearFlag(rider, kFlagSnapAlign);
        }
        body->velocity = velocity * kFramesASecond;
        return 0;
    }
    READABLE_FUNCTION(airBodyContact, Verified);

    // riderBodyContact (rider, fromAirMotions, dynamics, body, contact): a body of the rider's against a face, once the
    // level objects' callback lets it through (else 0). Hit faster than 4: the hit noted in the touches (by its
    // level, the body's part and the face's surface; not a water surface's, a face the filter refuses, or a body of
    // no part), dust off a dusty face, the pad's rumble (the air's motions' bodies, or the hips) and the speed into
    // the ride distance. 1 (also for a dynamics with no rider).
    int32_t riderBodyContact(Rider *rider, int32_t fromAirMotions, ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body,
                             ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<BumpSurfaces> surfaces;
        ee::Local<zge::Vec4> dustMotion;
        ee::Local<calls::Rumble> request;
        *surfaces = *g_bumpSurfaces;
        if (!rider)
            return 1;
        BodyTouches &touches = rider->touches;
        if (!dynamicsObjectContact(dynamics, body, contact))
            return 0;
        bool dust = false;
        const ee::Float speed = speedInto(body, contact);
        if (!(kBodyHitLeast < speed))
            return 1;
        bool hits = true;
        uint32_t level;
        if (speed < kBodyHitLevel1)
            level = 0;
        else if (speed < kBodyHitLevel2)
            level = 1;
        else if (speed < kBodyHitLevel3)
            level = 2;
        else
            level = 3;
        uint32_t surface = 0;
        if (contact->object)
        {
            const ee::Ptr<zge::SceneObject> object = zge::nodeSceneObject(contact->object->node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
            if (object)
            {
                if (zge::levelObjectTypeHash(object) == kWaterSurfaceType)
                    hits = false;
            }
            else if (const FaceRef face = contactFace(contact))
            {
                if (!collision::acceptAll())
                {
                    hits = false;
                }
                else
                {
                    const uint32_t sound = collision::faceSound(face);
                    if (sound != kNoFaceSound)
                        surface = surfaces->bySound[sound];
                    dust = dustySound(sound);
                }
            }
        }
        const BodyPart part = bodyPart(body->nameHash);
        if (part == kPartNone)
            hits = false;
        if (hits)
        {
            touches.flags |= kTouchBump;
            if (touches.bumpLevel < level)
            {
                touches.bumpSurface = surface;
                touches.bumpLevel = level;
                touches.bumpPart = part;
            }
        }
        if (dust)
        {
            dustMotion->x = body->velocity.x + speed * contact->normal.x;
            dustMotion->y = body->velocity.y + speed * contact->normal.y;
            dustMotion->z = body->velocity.z + speed * contact->normal.z;
            *dustMotion = *dustMotion * *g_bodyDustMotionScale;
            spawnDust(&body->position); // (and the motion: a word spawnDust does not take)
        }
        if (!hasFlag(rider, kFlagNoPlayer) && (fromAirMotions == 1 || body->nameHash == *g_hipsName))
        {
            request->motor = kRumbleLargeMotor;
            request->timing = 0;
            const ee::Float strength = speed * *g_bodyHitRumble;
            request->strength = static_cast<uint8_t>(ee::toInt(strength) < kRumbleMost ? ee::toInt(strength) : kRumbleMost);
            calls::rumble(rider->player, &*request);
        }
        addRideDistance(rider, speed);
        return 1;
    }
    READABLE_FUNCTION(riderBodyContact, Verified);

    // riderDynamicsContact: the bail's bodies' callback ("dynamics.rider").
    int32_t riderDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        return riderBodyContact(ee::Ptr<Rider>(dynamics->owner).get(), 0, dynamics, body, contact);
    }
    READABLE_FUNCTION(riderDynamicsContact, Verified);

    // riderAirMotionsContact: the air's motions' bodies' callback ("dynamics.riderAnim"): the air's own test first;
    // the hit only when it counts. 1.
    int32_t riderAirMotionsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        Rider *rider = ee::Ptr<Rider>(dynamics->owner).get();
        if (airBodyContact(rider, dynamics, body, contact))
            riderBodyContact(ee::Ptr<Rider>(dynamics->owner).get(), 1, dynamics, body, contact);
        return 1;
    }
    READABLE_FUNCTION(riderAirMotionsContact, Verified);

    // bikeDynamicsContact: the bike's bodies' callback ("dynamics.bike"): its root body hit faster than 5 raises dust
    // off a dusty face or throws sparks off another (a scene object's faces neither), and from 9 sounds at the hit.
    // Then the level objects' callback answers.
    int32_t bikeDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> motion;
        if (dynamics->bodies == body)
        {
            const ee::Float speed = speedInto(body, contact);
            if (kBikeHitLeast < speed)
            {
                bool dust = false;
                bool sparks = false;
                if (contact->object && !zge::nodeSceneObject(contact->object->node, ee::Ptr<ee::Ptr<zge::SceneBone>>()))
                {
                    if (const FaceRef face = contactFace(contact))
                    {
                        if (dustySound(collision::faceSound(face)))
                            dust = true;
                        else
                            sparks = true;
                    }
                }
                if (dust)
                {
                    *at = body->position;
                    motion->x = body->velocity.x + speed * contact->normal.x;
                    motion->y = body->velocity.y + speed * contact->normal.y;
                    motion->z = body->velocity.z + speed * contact->normal.z;
                    *motion = *motion * *g_bikeDustMotionScale;
                    spawnDust(at); // (and the motion: a word spawnDust does not take)
                }
                if (sparks)
                {
                    *at = body->position;
                    *motion = contact->normal * (speed + speed);
                    *motion = *motion + body->velocity;
                    *motion = *motion * *g_bikeSparkMotionScale;
                    bmx::sparks(&at->x, &motion->x, ee::toInt(speed * *g_bikeSparksPerSpeed));
                }
                if (kBikeHitSounds < speed)
                {
                    const int32_t level = ee::toInt(speed * *g_bikeHitLevelPerSpeed);
                    soundEvent(kSoundBikeHit, &contact->vertices[0], 0u, uint32_t(level < kBikeHitLevelMost ? level : kBikeHitLevelMost), 0u);
                }
            }
        }
        return int32_t(dynamicsObjectContact(dynamics, body, contact));
    }
    READABLE_FUNCTION(bikeDynamicsContact, Verified);
}
