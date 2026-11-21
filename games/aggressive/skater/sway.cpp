// The skater's procedural sway (skaterSway, from the player driver when
// the frame is longer than 0.005 s): bones of the skater's model (the
// Player's bone controllers, 0 where the model has none) moved a little on two sine waves whose speed and size grow with
// the skating speed, the animation controllers advanced, and the swayed
// bones' rotations eased back toward rest.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/quaternion.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "sym/aggressive/skater/sway.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        using zge::Quaternion;
        using zge::Vec4;

        // The two waves' phases, in turns (shared by all skaters).
        inline ee::Global<ee::Float, sym::g_swayPhase> g_swayPhase;
        inline ee::Global<ee::Float, sym::g_swayPhase2> g_swayPhase2;  // at half the speed
        inline ee::Global<ee::Float, sym::g_maxSway> g_maxSway;     // 0.04: the size at 30 a second or faster
        inline ee::Global<ee::Float, sym::g_degreesToAngle> g_degreesToAngle; // 65536 / 360
        inline ee::Global<ee::Float, sym::g_degreesToAngle2> g_degreesToAngle2;
        inline ee::Global<ee::Float, sym::g_phase2Lag> g_phase2Lag;   // 0.2 of a turn
        inline ee::Global<ee::Float, sym::g_centreEaseRate> g_centreEaseRate;  // 0.1
        inline ee::Global<ee::Float, sym::g_sideEaseRate> g_sideEaseRate;    // 0.3
        // BoobTech (Mem Check points at the same entries): the bob's forces and the
        // bobbed bones' scale.
        constexpr uint32_t kDisplacementForce = 0, kDampingForce = 1, kImplantSize = 2;
        ee::Float bobTuning(uint32_t entry)
        {
            return (*g_tuningCategories)[kTuningBoobTech].entries[entry].value;
        }

        // sin of a phase: `turns * degreesPerTurn / 360` turns (as the
        // original rounds it: a whole turn is 360, the centre bone's wave
        // 180), its fraction in degrees, then as an engine angle.
        ee::Float waveSin(ee::Float turns, ee::Float degreesPerTurn, ee::Float degreesToAngle)
        {
            const ee::Float degrees = ee::Float(360.0f);
            const ee::Float scaled = turns * degreesPerTurn / degrees;
            const ee::Float fraction = scaled - ee::toFloat(ee::toInt(scaled));
            const auto angle = static_cast<zge::Angle>(ee::toInt(fraction * degrees * degreesToAngle));
            return zge::angleSin(angle);
        }
    }

    int32_t skaterSway(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> offset;
        ee::Local<Quaternion> rest;
        ee::Local<Vec4> scale;

        // Faster with speed (6 turns a second at 30 a second and over),
        // larger too (up to Max Sway).
        const ee::Float relative = player->speed / ee::Float(30.0f);
        const ee::Float rate = relative * ee::Float(6.0f) < ee::Float(6.0f) ? relative * ee::Float(6.0f) : ee::Float(6.0f);
        const ee::Float size = relative * *g_maxSway < *g_maxSway ? relative * *g_maxSway : *g_maxSway;
        const ee::Float step = frameTime * rate;
        ee::Float phase = *g_swayPhase + step;
        *g_swayPhase = phase;
        *g_swayPhase2 = *g_swayPhase2 + step * ee::Float(0.5f);
        if (ee::Float(1.0f) < phase)
        {
            phase = phase - ee::Float(1.0f);
            *g_swayPhase = phase;
        }

        // The sway the bones are moved by; each wave below leaves its own for
        // the next, and the hair takes whatever the last one left.
        ee::Float sway = waveSin(phase, ee::Float(360.0f), *g_degreesToAngle) * size;

        // Shirt or coat, on a skater with no bob.
        if (player->bodyAnimation && !player->bobAnimation)
        {
            if (player->swayBoneLeft)
            {
                offset->x = -sway;
                offset->y = sway;
                offset->z = sway;
                zge::setBoneTranslation(player->swayBoneLeft, offset);
            }
            if (player->swayBoneRight)
            {
                offset->x = sway;
                offset->y = sway;
                offset->z = sway;
                zge::setBoneTranslation(player->swayBoneRight, offset);
            }
            if (player->swayBoneCentre)
            {
                const ee::Float twice = sway + sway;
                offset->y = -twice;
                offset->z = -twice;
                sway = waveSin(*g_swayPhase, ee::Float(180.0f), *g_degreesToAngle) * size;
                offset->x = sway + sway + sway;
                zge::setBoneTranslation(player->swayBoneCentre, offset);
            }
        }

        if (player->bobAnimation)
        {
            // The forces as tuned, on every pull of the bob.
            const ee::Float displacementForce = bobTuning(kDisplacementForce);
            const ee::Float dampingForce = bobTuning(kDampingForce);
            const ee::Ptr<zge::AnimateSystem> bob(player->bobAnimation);
            for (ee::Ptr<zge::AnimatePull> pull = bob->pulls; pull; pull = pull->next)
            {
                pull->strength = displacementForce;
                pull->damping = dampingForce;
            }

            sway = waveSin(*g_swayPhase2, ee::Float(360.0f), *g_degreesToAngle2) * size;
            offset->x = 0.0f;
            offset->z = 0.0f;
            offset->y = sway * ee::Float(1.5f);
            zge::setBoneTranslation(player->bobBoneLeft, offset);
            sway = waveSin(*g_swayPhase2 + *g_phase2Lag, ee::Float(360.0f), *g_degreesToAngle2) * size;
            offset->y = sway;
            zge::setBoneTranslation(player->bobBoneRight, offset);
        }

        if (player->upDownAnimation)
        {
            if (player->hairBone)
            {
                offset->x = 0.0f;
                offset->y = sway;
                offset->z = 0.0f;
                zge::setBoneTranslation(player->hairBone, offset);
            }
            if (player->hairBoneLower)
            {
                offset->x = 0.0f;
                offset->z = 0.0f;
                offset->y = -sway;
                zge::setBoneTranslation(player->hairBoneLower, offset);
            }
        }

        // The systems, and the swayed bones eased back toward rest.
        const auto easeToRest = [&](ee::Ptr<zge::AnimationChannel> bone, ee::Float easeRate) {
            const ee::Ptr<Quaternion> rotation = zge::boneRotation(bone);
            zge::quaternionIdentity(rest);
            zge::quaternionSlerp(rotation, rotation, rest, easeRate);
            return rotation;
        };
        bool first = true;
        if (player->bodyAnimation)
        {
            zge::animate(player->bodyAnimation, 1, frameTime);
            first = false;
            if (player->swayBoneCentre)
            {
                easeToRest(player->swayBoneCentre, *g_centreEaseRate);
                const ee::Float sideRate = *g_sideEaseRate;
                zge::setBoneRotation(player->swayBoneLeft, easeToRest(player->swayBoneLeft, sideRate));
                zge::setBoneRotation(player->swayBoneRight, easeToRest(player->swayBoneRight, sideRate));
            }
        }
        if (player->bobAnimation)
        {
            const ee::Float boneScale = bobTuning(kImplantSize);
            scale->x = boneScale;
            scale->y = boneScale;
            scale->z = boneScale;
            zge::setBoneScale(player->bobBoneLeft, scale);
            zge::setBoneScale(player->bobBoneRight, scale);
            zge::animate(player->bobAnimation, first ? 1 : 0, frameTime);
            first = false;
        }
        if (player->upDownAnimation)
            zge::animate(player->upDownAnimation, first ? 1 : 0, frameTime);
        return 1;
    }
    READABLE_FUNCTION(skaterSway, Verified);
}
