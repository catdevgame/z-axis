#pragma once
// Steering on the ground with the stick (the landing's, the riding's and the ghost ride's, the same code in each
// state function of the original): the yaw rate turned by the stick toward a most that shrinks with speed, and
// falling off once the stick is let go. In degrees a frame, as each state's own share and units say.

#include "../rider.h"

#include "ee/plain.h"
#include "sym/bmx/rider/actions/steering.h.h"

namespace game::bmx::rider::steering
{
    // 15 times the stick a frame toward 75 times it, both less with speed: by 1 / (1 + 30 speed).
    inline ee::Global<ee::Float, sym::g_landSteerSpeed> g_landSteerSpeed;
    inline ee::Global<ee::Float, sym::g_wheelieSteerRate> g_wheelieSteerRate;
    inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_wheelieSteerMost;
    inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnFalloff; // 0.35
    // Still: the most the yaw rate may be (6 degrees).
    constexpr int16_t kLandStillMost = 0x444;
    constexpr int32_t kTurnStopped = 91; // half a degree

    // The yaw rate turned by the stick, held within -most .. most.
    inline void steerYawRate(Rider *rider, int32_t turned, int16_t most)
    {
        const int16_t below = static_cast<int16_t>(int16_t(turned) < most ? int16_t(turned) : most);
        rider->yawRate = below < -most ? static_cast<int16_t>(-most) : below;
    }
    // The stick let go: the yaw rate falls off, to 0 under half a degree.
    inline void yawRateFalloff(Rider *rider)
    {
        const int32_t yawRate = rider->yawRate;
        rider->yawRate = static_cast<int16_t>(
            yawRate - ee::toInt(ee::toFloat(yawRate) * ee::Float(1.0f / 65536.0f) * *g_turnFalloff * ee::Float(65536.0f)));
        if (ee::abs(ee::toFloat(int32_t(rider->yawRate))) < ee::Float(float(kTurnStopped)))
            rider->yawRate = 0;
    }
    // The stick's steer at a speed: the share the speed leaves (least at least), in the state's units.
    inline void steerAtSpeed(Rider *rider, ee::Float steer, ee::Float speed, ee::Float least, ee::Float scale)
    {
        ee::Float share = ee::Float(1.0f) / (speed * *g_landSteerSpeed + ee::Float(1.0f));
        if (!(least <= share))
            share = least;
        const int32_t turned = int32_t(rider->yawRate) - ee::toInt(steer * (*g_wheelieSteerRate * share) * scale);
        steerYawRate(rider, turned, static_cast<int16_t>(ee::toInt(ee::abs(steer) * *g_wheelieSteerMost * share * scale)));
    }
}
