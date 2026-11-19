// DM2's rider meeting the world on the ground (physics.c): what a ray's hit
// does while riding (groundHit), and the impact of a landing (groundImpact).
//
// Counterparts: DM2 GameCube 0x80084628
// (groundHit: its debug print names it bmxphysUpdatePositionRoll),
// 0x80084A4C (groundImpact); BMX XXX PS2 groundHit, groundImpact.

#include "physics.h"

#include "../collision/collision.h"
#include "dm2/level/water_wake.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/physics/impact.cpp.h"

namespace game::dm2::rider::physics
{
    using collision::EdgeRef;
    using collision::FaceRef;

    namespace
    {
        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kHitObjectFlag = 0x2u;

        // Surface kinds (faceSurfaceKind): 0 ground; 1 .. 3 with a linked
        // edge (a lip's); 4 kept as it is; 5 a bounce (the face's tag word
        // its strength, inferred).
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceKind4 = 4;
        constexpr uint32_t kSurfaceBounce = 5;
        constexpr uint32_t kNoContact = 0xFFFFFFFFu;
        // Landing on the ground clears these two flags (as landingTest).
        constexpr uint32_t kFlagsGroundClears = 0x30000u;

        // A lip met head on: the forward row within 30 degrees of the lip's
        // plane's normal, the bike within 15 degrees of upright, faster than
        // g_lipClankSpeed (0.15).
        constexpr int32_t kLipHeadOn = 0x1555;
        constexpr int32_t kLipUpright = 0xAAA;
        inline ee::Global<ee::Float, sym::g_lipClankSpeed> g_lipClankSpeed;
        // The sounds (the rider sound table's; inferred): a lip hit, a bounce.
        constexpr uint32_t kSoundLipHit = 21;
        constexpr uint32_t kSoundBounce = 18;
        constexpr uint32_t kSoundVolume = 127;
    }

    // A ray's hit while riding (the ground move's): the contact the rider
    // gets by the surface. A face: ground (or a kind above 5) clears the two
    // landing flags; kinds 1 .. 3 the face (kind 0 without a linked edge),
    // and a lip met head on, upright and fast clanks; kind 4 kept; a bounce
    // (5) takes the velocity into the face away, adds the face's normal
    // times its tag word, and sends the rider into the air (no contact
    // kept). An object: its contact (kind 8). Nothing: the hit point read
    // (a print in the GameCube build) and the flags cleared. Returns the
    // contact kind given, -1 for none. GameCube 0x80084628.
    int32_t groundHit(Rider *rider, const collision::RayHit *ray)
    {
        ee::Local<ee::Float[4]> point; // the hit point (or the lip's normal)
        ee::Local<ee::Float[4]> plane; // the face's plane through the hit
        ee::Local<ee::Float[4]> lipPlane;
        ee::Local<FaceRef> contact;
        ee::Local<FaceRef> face;
        ee::Local<uint32_t> object;

        uint32_t kind = kNoContact;
        void *record = nullptr;
        *face = collision::hitFace(ray);
        collision::hitPlane(ray, plane);

        if ((collision::hitFlags(ray) & kHitFaceFlag) != 0 && *face != 0)
        {
            const uint32_t surface = collision::faceSurfaceKind(*face);
            if (surface == kSurfaceBounce)
            {
                const ee::Float into = zge::dot(*vec4(rider->velocity), *vec4(plane));
                const ee::Float x = plane[0] * into;
                const ee::Float y = plane[1] * into;
                const ee::Float z = plane[2] * into;
                rider->velocity[0] = rider->velocity[0] - x;
                rider->velocity[1] = rider->velocity[1] - y;
                rider->velocity[2] = rider->velocity[2] - z;
                for (int i = 0; i < 3; ++i)
                {
                    const ee::Float strength = ee::toFloat(collision::faceTagWord(*face));
                    rider->velocity[i] = rider->velocity[i] + plane[i] * strength;
                }
                rider->speed = zge::vec3Length(&vec4(rider->velocity)->xyz());
                setPhysicsState(rider, uint32_t(kPhysicsAir));
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 9u);
                soundEvent(kSoundBounce, rider->position, code, kSoundVolume);
                setContact(rider, face, 0u);
                return 0;
            }
            if (surface == kSurfaceKind4)
            {
                kind = kSurfaceKind4;
                *contact = *face;
                record = contact;
            }
            else if (surface > kSurfaceGround && surface < kSurfaceKind4)
            {
                const EdgeRef edge = collision::faceLinkedEdge(*face);
                kind = collision::faceSurfaceKind(*face);
                *contact = *face;
                record = contact;
                if (edge == 0)
                {
                    kind = 0;
                }
                else
                {
                    collision::edgePlane(edge, lipPlane);
                    point[0] = lipPlane[0];
                    point[1] = lipPlane[1];
                    point[2] = lipPlane[2];
                    if (rider->contactKind == 0 && kind != 0 &&
                        zge::angleAcos(ee::abs(zge::dot(*vec4(rider->forward), *vec4(point)))) < kLipHeadOn &&
                        zge::angleAcos(-rider->up[1]) < kLipUpright && *g_lipClankSpeed < rider->speed)
                    {
                        collision::hitPoint(ray, point);
                        const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 2u);
                        soundEvent(kSoundLipHit, point, code, kSoundVolume);
                    }
                }
            }
            else
            {
                rider->flags &= ~kFlagsGroundClears;
                kind = 0;
                *contact = *face;
                record = contact;
            }
        }
        else
        {
            if ((collision::hitFlags(ray) & kHitObjectFlag) != 0)
            {
                kind = kContactObject;
                *object = collision::hitObject(ray);
                record = object;
            }
            else
            {
                collision::hitPoint(ray, point);
                rider->flags &= ~kFlagsGroundClears;
            }
        }
        if (kind != kNoContact)
            setContact(rider, record, kind);
        return static_cast<int32_t>(kind);
    }
    READABLE_FUNCTION(groundHit, Verified);
    namespace
    {
        // The game side's effects: the "dust" effect at a point with a
        // motion (the water's wake is level/water_wake.h's).
        constexpr uint32_t kNoFaceSound = 0xFFFFu;

        // The surface sounds an impact reacts to (inferred from what each does).
        enum ImpactSound : uint32_t
        {
            kSoundDirt = 5,       // dust (also 10, 11)
            kSoundDeepWater = 6,  // a bail (kind 9)
            kSoundWater = 7,      // a splash at the water's level
            kSoundDirtB = 10,
            kSoundDirtC = 11,
            kSoundMetal = 12,     // the pad rumbles
        };
        constexpr uint8_t kBailWater = 9;
        constexpr uint32_t kContactWater = 9;

        // Dust: 15 puffs a unit of the wheel's speed, one in 16 made, up to
        // 0.31 from the hit at a random angle, moving at 0.75 of the wheel.
        constexpr ee::Float kDustPerSpeed = 15.0f;
        constexpr ee::Float kDustMotionShare = 0.75f;
        inline ee::Global<ee::Float, sym::g_dustSpread2> g_dustSpread; // 0.01 a step of the 0 .. 31
        // The rumble: the speed times 1.7, at most 64.
        inline ee::Global<ee::Float, sym::g_rumblePerSpeed> g_rumblePerSpeed;
        constexpr int32_t kRumbleMost = 64;
        // The water's level (y), set by the level (inferred).
        inline ee::Global<ee::Float, sym::g_waterLevel> g_waterLevel;
    }

    // A wheel's impact with the ground (the ground probe's, the hit under
    // the wheel): the wheel's surface sound from the face's material, then
    // by it dust at the hit, a splash or a bail in water, or the pad's
    // rumble on metal. Returns 2 when the rider bails (the new physics
    // state), else 0. GameCube 0x80084A4C.
    int32_t groundImpact(Rider *rider, uint32_t wheelIndex, uint32_t hit)
    {
        ee::Local<ee::Float[4]> point;
        ee::Local<ee::Float[4]> spread;
        ee::Local<ee::Float[4]> splashAt;
        ee::Local<ee::Float[4]> dustAt;
        ee::Local<ee::Float[4]> dustMotion;
        ee::Local<FaceRef> face;
        const ee::Ptr<collision::RayHit> ray(hit);
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
                const int32_t random = zge::randomNumber();
                ++puff;
                if ((random & 0xF) != 0)
                    continue;
                spread[0] = ee::toFloat(random & 0x1F) * *g_dustSpread;
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
                dust(dustAt, dustMotion);
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
                setContact(rider, face, kContactWater);
                collision::touchObject(rider, ray);
                return kPhysicsBail;
            }
            collision::touchObject(rider, ray);
            *vec4(splashAt) = *vec4(rider->position);
            splashAt[1] = *g_waterLevel;
            startWaterWake(vec4(splashAt), vec4(rider->velocity));
            break;
        case kSoundMetal:
            if ((rider->flags & kFlagNoPlayer) == 0)
            {
                const int32_t strength = ee::toInt(rider->speed * *g_rumblePerSpeed);
                rumble(uint32_t(rider->player), 1u, uint32_t(strength < kRumbleMost ? strength : kRumbleMost));
            }
            return 0;
        default:
            break;
        }
        return 0;
    }
    READABLE_FUNCTION(groundImpact, Verified);
}
