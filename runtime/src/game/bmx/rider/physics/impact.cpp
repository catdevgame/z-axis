// BMX XXX's rider meeting the ground (physics.c): the contact a ray's hit
// gives the rider. DM2's (game/dm2/rider/physics/impact.cpp), read in BMX
// XXX's machine code.

#include "physics.h"

#include "../collision/collision.h"
#include "../engine_calls.h"
#include "../game_calls.h"
#include "../rider.h"

#include "bmx/sound/events.h"
#include "bmx/fx/emitter_objects.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/physics/impact.cpp.h"

namespace game::bmx::rider
{
    using collision::EdgeRef;
    using collision::FaceRef;

    namespace
    {
        // Surface kinds (faceSurfaceKind): 0 ground; 1 .. 3 with a linked
        // edge (a lip's); 4 kept as it is; 5 a bounce (the face's tag word
        // its strength). DM2's.
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceKind4 = 4;
        constexpr uint32_t kSurfaceBounce = 5;
        constexpr uint32_t kNoContact = 0xFFFFFFFFu;
        // A lip met head on: the forward row within 30 degrees of the lip's
        // plane's normal, the bike within 15 degrees of upright, faster than
        // g_lipClankSpeed (0.15).
        constexpr int32_t kLipHeadOn = 0x1555;
        constexpr int32_t kLipUpright = 0xAAA;
        inline ee::Global<ee::Float, sym::g_lipClankSpeed> g_lipClankSpeed;
        // The sounds (the rider sound table's): a lip hit, a bounce.
        constexpr uint32_t kSoundLipHit = 34;
        constexpr uint32_t kSoundBounce = 31;
        constexpr uint32_t kSoundVolume = 127;
    }

    // A ray's hit while riding (the ground move's): the contact the rider
    // gets by the surface. A face: ground (or a kind above 5) clears the two
    // landing flags; kinds 1 .. 3 the face (kind 0 without a linked edge),
    // and a lip met head on, upright and fast clanks; kind 4 kept; a bounce
    // (5) takes the velocity into the face away, adds the face's normal
    // times its tag word, and sends the rider into the air (no contact
    // kept). An object: its contact. Nothing: the hit point read and the
    // flags cleared. Returns the contact kind given, -1 for none. DM2's.
    int32_t groundHit(Rider *rider, const collision::RayHit *ray)
    {
        ee::Local<zge::Vec4> point; // the hit point (or the lip's normal)
        ee::Local<zge::Vec4> plane; // the face's plane through the hit
        ee::Local<zge::Vec4> lipPlane;
        ee::Local<FaceRef> contact;
        ee::Local<FaceRef> face;
        ee::Local<uint32_t> object;

        uint32_t kind = kNoContact;
        const void *record = nullptr;
        *face = collision::hitFace(ray);
        collision::hitPlane(ray, &plane->x);

        if ((collision::hitFlags(ray) & collision::kHitFace) != 0 && *face != 0)
        {
            const uint32_t surface = collision::faceSurfaceKind(*face);
            if (surface == kSurfaceBounce)
            {
                zge::Vec4 &velocity = *vec4(rider->velocity);
                const ee::Float into = zge::dot(velocity, *plane);
                const ee::Float x = plane->x * into;
                const ee::Float y = plane->y * into;
                const ee::Float z = plane->z * into;
                velocity.x = velocity.x - x;
                velocity.y = velocity.y - y;
                velocity.z = velocity.z - z;
                velocity.x = velocity.x + plane->x * ee::toFloat(int32_t(collision::faceTagWord(*face)));
                velocity.y = velocity.y + plane->y * ee::toFloat(int32_t(collision::faceTagWord(*face)));
                velocity.z = velocity.z + plane->z * ee::toFloat(int32_t(collision::faceTagWord(*face)));
                rider->speed = zge::vec3Length(&velocity.xyz());
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 9u);
                soundEvent(kSoundBounce, rider->position, ee::Address(code), kSoundVolume, 0u);
                setContact(rider, &*face, 0u);
                return 0;
            }
            if (surface == kSurfaceKind4)
            {
                kind = kSurfaceKind4;
                *contact = *face;
                record = &*contact;
            }
            else if (surface > kSurfaceGround && surface < kSurfaceKind4)
            {
                const EdgeRef edge = collision::faceLinkedEdge(*face);
                kind = collision::faceSurfaceKind(*face);
                *contact = *face;
                record = &*contact;
                if (edge == 0)
                {
                    kind = 0;
                }
                else if (collision::edgePlane(edge, &lipPlane->x) != 0)
                {
                    point->x = lipPlane->x;
                    point->y = lipPlane->y;
                    point->z = lipPlane->z;
                    if (rider->contactKind == 0 && kind != 0 &&
                        zge::angleAcos(ee::abs(zge::dot(*vec4(rider->forward), *point))) < kLipHeadOn &&
                        zge::angleAcos(-rider->up[1]) < kLipUpright && *g_lipClankSpeed < rider->speed)
                    {
                        collision::hitPoint(ray, &point->x);
                        const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 2u);
                        soundEvent(kSoundLipHit, &point->x, ee::Address(code), kSoundVolume, 0u);
                    }
                }
            }
            else
            {
                kind = 0;
                *contact = *face;
                record = &*contact;
                rider->flags[3] &= static_cast<uint8_t>(~kHopFlags);
            }
        }
        else
        {
            if ((collision::hitFlags(ray) & collision::kHitObject) != 0)
            {
                kind = kContactObject;
                *object = collision::hitObject(ray);
                record = &*object;
            }
            else
            {
                collision::hitPoint(ray, &point->x);
                rider->flags[3] &= static_cast<uint8_t>(~kHopFlags);
            }
        }
        if (kind != kNoContact)
            setContact(rider, record, kind);
        return static_cast<int32_t>(kind);
    }
    READABLE_FUNCTION(groundHit, Verified);

    namespace
    {
        constexpr uint32_t kNoFaceSound = 0xFFFFu;
        // The surface sounds an impact reacts to (DM2's).
        enum ImpactSound : uint32_t
        {
            kSoundDirt = 5,      // dust (also 10, 11)
            kSoundDeepWater = 6, // a bail
            kSoundWater = 7,     // shallow water: ridden through
            kSoundDirtB = 10,
            kSoundDirtC = 11,
            kSoundMetal = 12,    // the pad rumbles
        };
        constexpr uint8_t kBailWater = 9;
        // How the water touches its object (touchObject's third argument,
        // sent with the touch): 6 for the bail in deep water, 0 riding
        // through the shallow.
        constexpr uint32_t kTouchWaterBail = 6;
        constexpr uint32_t kTouchPlain = 0;

        // Dust: 15 puffs a unit of the wheel's speed, one in 16 made, up to
        // 0.31 from the hit at a random angle, moving at 0.75 of the wheel.
        constexpr ee::Float kDustPerSpeed = 15.0f;
        constexpr ee::Float kDustMotionShare = 0.75f;
        inline ee::Global<ee::Float, sym::g_dustSpread> g_dustSpread; // 0.01 a step of the 0 .. 31
        // The rumble: the large motor, the speed times 1.7, at most 64.
        inline ee::Global<ee::Float, sym::g_rumblePerSpeed> g_rumblePerSpeed;
        constexpr int32_t kRumbleMost = 64;
        constexpr uint32_t kRumbleLargeMotor = 1;
    }

    // A wheel's impact with the ground (the ground probe's, the hit under
    // the wheel): the wheel's surface sound from the face's material, then
    // by it dust at the hit, a bail in deep water, or the pad's rumble on
    // metal. Returns 2 when the rider bails (the new physics state), else
    // 0. DM2's, without its splash in shallow water: BMX XXX works out the
    // velocity's square there and makes nothing of it.
    int32_t groundImpact(Rider *rider, uint32_t wheelIndex, const collision::RayHit *ray)
    {
        ee::Local<ee::Float[4]> point;
        ee::Local<ee::Float[4]> spread;
        ee::Local<ee::Float[4]> dustAt;
        ee::Local<ee::Float[4]> dustMotion;
        ee::Local<FaceRef> face;
        ee::Local<calls::Rumble> request; // where dustAt is
        WheelPose &wheel = rider->wheelPoses[wheelIndex];

        *face = collision::hitFace(ray);
        collision::hitPoint(ray, point);
        wheel.sound = 0;
        if (collision::faceSound(*face) != kNoFaceSound)
            wheel.sound = collision::faceSound(*face);
        switch (wheel.sound)
        {
        case kSoundDirt:
        case kSoundDirtB:
        case kSoundDirtC:
            for (uint32_t puff = 0; puff < static_cast<uint32_t>(ee::toInt(wheel.speed * kDustPerSpeed));)
            {
                const uint32_t random = zge::randomNumber();
                ++puff;
                if ((random & 0xF) != 0)
                    continue;
                spread[0] = ee::toFloat(int32_t(random & 0x1F)) * *g_dustSpread;
                spread[1] = 0.0f;
                spread[2] = 0.0f;
                const int16_t angle = static_cast<int16_t>(zge::randomNumber());
                zge::vec3RotateY(&vec4(spread)->xyz(), &vec4(spread)->xyz(), int32_t(angle));
                dustAt[0] = point[0] + spread[0];
                dustAt[1] = point[1] + spread[1];
                dustAt[2] = point[2] + spread[2];
                dustMotion[0] = wheel.velocity[0] * kDustMotionShare;
                dustMotion[1] = wheel.velocity[1] * kDustMotionShare;
                dustMotion[2] = wheel.velocity[2] * kDustMotionShare;
                spawnDust(vec4(dustAt)); // (and the motion: a word spawnDust does not take)
            }
            return 0;
        case kSoundDeepWater:
        case kSoundWater:
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(rider->messenger), 0u);
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(rider->bikeModel), 0u);
            if (collision::faceSound(*face) != kSoundWater)
            {
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                rider->bailKind = kBailWater;
                setContact(rider, &*face, kContactWater);
                collision::touchObject(rider, ray, kTouchWaterBail);
                return kPhysicsBail;
            }
            collision::touchObject(rider, ray, kTouchPlain);
            break;
        case kSoundMetal:
            if (!hasFlag(rider, kFlagNoPlayer))
            {
                const int32_t strength = ee::toInt(rider->speed * *g_rumblePerSpeed);
                request->motor = kRumbleLargeMotor;
                request->timing = 0;
                request->strength = static_cast<uint8_t>(strength < kRumbleMost ? strength : kRumbleMost);
                calls::rumble(uint32_t(rider->player), request);
            }
            return 0;
        default:
            break;
        }
        return 0;
    }
    READABLE_FUNCTION(groundImpact, Verified);
}
