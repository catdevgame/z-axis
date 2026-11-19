#pragma once

// BMX XXX's rider physics (physics.c): what its files share. DM2's
// (game/dm2/rider/physics/physics.h), read in BMX XXX's machine code.

#include "../rider.h"

#include "ee/abi.h"
#include "zge/collision/dynamics.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/rider/physics/physics.h.h"

namespace game::bmx::rider::collision
{
    struct RayHit;
}

namespace game::bmx::rider::physics
{
    // The surfaces by kind (a wheel's material sound, an edge's wall kind,
    // 0 .. 15): how a wheel grips, slips and slows rolling on it, and how
    // much a grind along it drags (DM2's record).
    struct Surface
    {
        ee::Float grip;
        ee::Float slip;
        ee::Float rollingLoss; // a rolling wheel's speed lost
        ee::Float grindDrag;   // the share of a grind's velocity lost a frame at full balance, 11 times it at none (grindSlope)
    };
    static_assert(sizeof(Surface) == 0x10 && offsetof(Surface, grindDrag) == 0xC);
    inline ee::Global<Surface[16], sym::g_surfaces> g_surfaces;

    // The upper half of a vector's quadword (z and w), as the engine's
    // vector macro carries it into VU0 with the next factor.
    inline uint64_t upperHalf(const zge::Vec4 &v) { return v.z.bits() | static_cast<uint64_t>(v.w.bits()) << 32; }

    // (ground.cpp)
    void groundTurn(Rider *rider);
    void physicsFrame(Rider *rider);
    void rideTouchedObject(Rider *rider, ee::Float *moved);
    int32_t pushCornersClear(Rider *rider);
    collision::RayHit *groundAhead(Rider *rider, uint32_t wheelIndex, ee::Float *point, ee::Float *height, ee::Float *clearance);
    int32_t groundProbe(Rider *rider, uint32_t wheelIndex, const ee::Float *from);
}

namespace game::bmx::rider
{
    namespace collision
    {
        struct RayHit;
    }
    // The first body of a dynamics named `name` (none: 0).
    inline ee::Ptr<zge::RigidBody> bodyNamed(ee::Ptr<zge::Dynamics> dynamics, uint32_t name)
    {
        ee::Ptr<zge::RigidBody> body = dynamics->bodies;
        while (body && body->nameHash != name)
            body = body->next;
        return body;
    }
    // Landing on the ground clears the hop's two flags (kFlagHopUp and
    // kFlagHopDrop, both of the fourth flag byte; DM2's pair too).
    static_assert(kFlagHopUp.byte == 3 && kFlagHopDrop.byte == 3);
    constexpr uint8_t kHopFlags = kFlagHopUp.mask | kFlagHopDrop.mask;
    // (landing.cpp)
    int32_t landingTest(Rider *rider, const collision::RayHit *ray);
    // (impact.cpp)
    int32_t groundHit(Rider *rider, const collision::RayHit *ray);
    int32_t groundImpact(Rider *rider, uint32_t wheelIndex, const collision::RayHit *ray);
}
