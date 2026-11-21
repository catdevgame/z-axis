// BMX XXX's snap onto a rail, a stall or a lip (physics states 3, 5 and 7,
// the approaches): its forces and its move. DM2's (games/dm2/rider/actions/
// approach.cpp).

#include "../rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/actions/approach.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // Gravity a frame on the snap's step (y grows downwards).
        inline ee::Global<ee::Float, sym::g_gravity> g_approachGravity; // 0.0055
        // The wheels' spin kept a frame of the snap.
        inline ee::Global<ee::Float, sym::g_approachSpinKept> g_approachSpinKept; // 0.9967
    }

    // States 3, 5 and 7's forces: gravity on the snap's step (its y).
    void approachGravity(Rider *rider)
    {
        rider->snapStep[1] = rider->snapStep[1] + *g_approachGravity;
    }
    READABLE_FUNCTION(approachGravity, Verified);

    // States 3, 5 and 7's move: the wheels spin down; the snap's step added
    // to the drawn body and the position for the snap's frames, then the
    // state after the approach (4 the grind, 6 the stall, 8 the lip).
    // Returns the position (or the state, as setPhysicsState's).
    uint32_t approachMove(Rider *rider)
    {
        const ee::Float kept = *g_approachSpinKept;
        for (WheelPose &wheel : rider->wheelPoses)
            wheel.spin = ee::toInt(ee::toFloat(wheel.spin) * kept);
        if (--rider->snapFrames == 0)
        {
            // The original's last act: a jump to setPhysicsState, its frame
            // given back first.
            const uint8_t state = physicsState(rider);
            return game::tail(setPhysicsState, rider, uint32_t(state) + 1u);
        }
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(rider->snapStep);
        *vec4(rider->position) = *vec4(rider->position) + *vec4(rider->snapStep);
        return ee::Address(rider->position);
    }
    READABLE_FUNCTION(approachMove, Verified);

    namespace
    {
        // A rail edge's contact kind.
        constexpr uint8_t kContactRailEdge = 5;
        // The trick flags: this bit, the trick's rows go by the right row (its lean a turn about the right row).
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;
        constexpr uint32_t kTrickRowsByRight = 0x80u;
        // The lean at a balance of 1: 35 degrees about the rail, 7.5 about the right row for those tricks.
        inline ee::Global<int16_t, sym::g_railLeanMost> g_railLeanMost;
        inline ee::Global<int16_t, sym::g_railLeanMostByRight> g_railLeanMostByRight;
        // The rows go this share of the way to the target's a frame (0.085).
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_approachRowsShare;
        const ee::Float kTurnUnit(1.0f / 65536.0f);
    }

    // approachTurn (rider): states 3, 5 and 7's turn: the forward and up rows a share of the way to the snap
    // target's (onto a rail edge, the target leaning by the grind balance as leanRowsToRail leans it), then made
    // square again (the right row from them, the forward row from it).
    void approachTurn(Rider *rider)
    {
        ee::Local<zge::Matrix> target;
        ee::Local<zge::Matrix> lean;
        if (rider->contactKind == kContactRailEdge)
        {
            zge::matrixIdentity(lean);
            if (!((*g_trickFlags)[rider->trick] & kTrickRowsByRight))
            {
                const int16_t angle = static_cast<int16_t>(
                    ee::toInt(rider->grindBalance * (ee::toFloat(int32_t(*g_railLeanMost)) * kTurnUnit) * ee::Float(65536.0f)));
                zge::matrixRotateZ(lean, lean, angle);
            }
            else
            {
                const int16_t angle = static_cast<int16_t>(
                    ee::toInt(rider->grindBalance * (ee::toFloat(int32_t(*g_railLeanMostByRight)) * kTurnUnit) * ee::Float(65536.0f)));
                zge::matrixRotateX(lean, lean, angle);
            }
            zge::matrixMultiply(target, ee::Ptr<zge::Matrix>(matrix(rider->snapRows)), lean);
        }
        else
        {
            *target = *matrix(rider->snapRows);
        }
        const ee::Float share = *g_approachRowsShare;
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        const zge::Vec4 &targetUp = target->row(1);
        const zge::Vec4 &targetForward = target->row(2);
        const ee::Float fx = forward.x + (targetForward.x - forward.x) * share;
        const ee::Float fy = forward.y + (targetForward.y - forward.y) * share;
        const ee::Float fz = forward.z + (targetForward.z - forward.z) * share;
        const ee::Float ux = up.x + (targetUp.x - up.x) * share;
        const ee::Float uy = up.y + (targetUp.y - up.y) * share;
        const ee::Float uz = up.z + (targetUp.z - up.z) * share;
        forward.x = fx;
        forward.y = fy;
        forward.z = fz;
        up.x = ux;
        up.y = uy;
        up.z = uz;
        right = zge::cross(up, forward);
        right = zge::unit(right);
        forward = zge::cross(right, up);
    }
    READABLE_FUNCTION(approachTurn, Verified);
}
