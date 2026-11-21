// DM2's stall and lip (physics states 6 and 8; MIXING 14.1's stall rows):
// onto the target the approach found.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8007BEB4 (stallEnter); BMX
// XXX PS2 stallEnter.

#include "../collision/collision.h"
#include "actions.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/actions/stall.cpp.h"

namespace game::dm2::rider
{
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface);

    namespace
    {
        constexpr int32_t kStallSound = 20;
        constexpr int32_t kStallVolume = 127;
        // The contact kinds (+0xFF1) the approach leaves: a rail edge (5),
        // a stall target (6), a lip (7, 8; inferred which).
        constexpr uint8_t kContactRailEdge = 5;
        constexpr uint8_t kContactLipFirst = 7;
        constexpr uint8_t kContactPastLip = 9;
        constexpr float kLipProbeDepth = 100.0f; // the ray down from the lip's top (y grows downwards)
        // A trick's kind bits (g_trickKindsR): 0x100 a plant (its sound's kind 5).
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickKinds;
        constexpr uint16_t kTrickPlant = 0x100;
        constexpr uint8_t kTrickLipFirst = 61; // tricks 61, 62: the sound's kind 3; 67: kind 4
        constexpr uint8_t kTrickLipLast = 62;
        constexpr uint8_t kTrickLipTurn = 67;
        constexpr uint8_t kTrickLipStill = 0x45; // the lip trick that keeps moving along the forward row
        constexpr uint32_t kFlagSnapAlign = 0x4000u;
    }

    // Physics states 6 and 8's enter: the rows the approach's, the angles
    // still; on a lip the drop below it (a ray down) into groundHeight; the
    // position at the target; the sound by the trick and the rail's
    // surface; the rider stopped (the lip trick 0x45 keeps his speed along
    // his forward row, the alignment ended). Returns the state asked for.
    uint32_t stallEnter(Rider *rider, uint32_t /*from*/, uint32_t state)
    {
        ee::Local<ee::Float[4]> from;      // the lip's ray: from the target's top
        ee::Local<ee::Float[4]> to;        // down
        ee::Local<zge::Vec4> along;        // the rail's way the rider faces (kept in the frame; not read after)
        ee::Local<ee::Float[4]> direction;
        ee::Local<ee::Float[4]> below;     // the lip ray's hit
        const uint8_t kind = rider->contactKind;
        if (kind == kContactRailEdge)
        {
            collision::edgeDirection(rider->contactRef(), direction);
            *along = *vec4(direction);
            if (zge::dot(*vec4(rider->forward), *along) < ee::Float(0.0f))
                zge::vec3Negate(&along->xyz(), &along->xyz());
        }
        else if (kind > kContactRailEdge && kind < kContactPastLip)
            *along = *vec4(rider->forward);
        collision::touchObject(rider, nullptr);
        physics::engine::copyMatrix(rider->right, rider->snapRows);
        uint8_t contact = rider->contactKind;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        if (contact < kContactPastLip && contact >= kContactLipFirst)
        {
            // A lip: a ray from the target's top (its up row added) down;
            // the drop to the hit into groundHeight.
            const ee::Float *const target = rider->snapPoint;
            const ee::Float *const up = &rider->snapRows[4];
            const ee::Float y = target[1] + up[1];
            const ee::Float upZ = up[2];
            const ee::Float z = target[2];
            const ee::Float x = target[0] + up[0];
            from[1] = y;
            const ee::Float zz = z + upZ;
            to[0] = x;
            from[0] = x;
            to[2] = zz;
            to[1] = y + ee::Float(kLipProbeDepth);
            from[2] = zz;
            const collision::RayHit *const hit = collision::rayCast(from, to);
            if (hit != nullptr)
            {
                collision::hitPoint(hit, below);
                contact = rider->contactKind;
                rider->groundHeight = below[1] - from[1];
            }
            else
                contact = rider->contactKind;
        }
        else
            rider->groundHeight = 0.0f;
        *vec4(rider->position) = *vec4(rider->snapPoint);
        *vec4(rider->drawnPosition) = *vec4(rider->snapPoint);
        rider->balance = 0.0f;

        // The sound: by the trick, on a rail by its surface.
        uint32_t surface = 0;
        if (contact == kContactRailEdge)
        {
            const int32_t s = collision::edgeWallKind(rider->contactRef());
            surface = s == 1 ? 7u : s == 2 ? 2u : 0u;
        }
        const uint8_t trick = rider->trick;
        uint32_t soundKind = 2;
        if ((*g_trickKinds)[trick] & kTrickPlant)
            soundKind = 5;
        else if (trick >= kTrickLipFirst && trick <= kTrickLipLast)
            soundKind = 3;
        else if (trick == kTrickLipTurn)
            soundKind = 4;
        const int32_t code = surfaceSoundFlags(0u, 0u, soundKind, surface);
        soundEvent(kStallSound, rider->position, code, kStallVolume);
        // The scale carries what the last call (the sound's) left beside the
        // speed, as airGravity's does.
        const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
        if (rider->trick != kTrickLipStill)
        {
            *vec4(rider->velocity) = zge::kZero;
            *vec4(rider->force) = zge::kZero;
            rider->speed = 0.0f;
            return state;
        }
        const ee::Float speed = zge::dot(*vec4(rider->velocity), *vec4(rider->forward));
        *vec4(rider->velocity) = zge::scaledCarrying(*vec4(rider->forward), speed, carried);
        const uint32_t flags = rider->flags;
        rider->speed = speed;
        rider->alignAngles[1] = 0;
        rider->flags = flags & ~kFlagSnapAlign;
        rider->alignAngles[2] = 0;
        rider->alignAngles[0] = 0;
        rider->snapFrames = 0;
        return state;
    }
    READABLE_FUNCTION_WAITS(stallEnter, Verified);
}
