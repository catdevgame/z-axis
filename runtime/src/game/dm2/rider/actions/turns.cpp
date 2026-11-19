// DM2's wheelie turn (physics states 9 and 10's turn callback) and the
// bail of an air trick not finished on landing.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8007D2F4 (wheelieTurn),
// 0x8008AFEC (unfinishedTrickBail); BMX XXX PS2 wheelieTurn, unfinishedTrickBail; DM2 Xbox
// 0x05D8C0 (unfinishedTrickBail).

#include "actions.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "sym/dm2/rider/actions/turns.cpp.h"

namespace game::dm2::rider
{
    namespace
    {

        // The wheelie's turn: on a slope (the up row more than 30 degrees
        // off upright) with the right row within 60 of level, under 0.95 x
        // the Turn tuning's 0.35 the rider turns down the slope by the right
        // row's slope, (0.35 - speed) x 42.5 degrees.
        inline ee::Global<ee::Float, sym::g_onSlope> g_onSlope;          // 5461 (30 degrees)
        inline ee::Global<ee::Float, sym::g_rightRowLevel> g_rightRowLevel;   // 10922 (60 degrees)
        inline ee::Global<ee::Float, sym::g_degreesToTurn> g_degreesToTurn;   // 65536 / 360
        inline ee::Global<ee::Float, sym::g_topSpeed> g_turnSpeed;       // 0.35 (the ground's Turn tuning)
        inline ee::Global<ee::Float, sym::g_wheelieTurnShare> g_wheelieTurnShare;   // 0.95
        inline ee::Global<ee::Float, sym::g_wheelieTurnDegrees> g_wheelieTurnDegrees; // 42.5
        constexpr uint8_t kContactKinds4 = 4; // contact kinds 1 .. 3 turn (0: nothing; 4 on: not a face of the ground)

        // The trick flags the bail reads (a u16 a trick): low 3 bits set,
        // the trick may be landed unfinished.
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickLandFlags;
        constexpr uint16_t kTrickLandsUnfinished = 0x7;
        constexpr uint32_t kFlagTrickHeld = 0x200u;
        // Bail kinds (rider +0x178): 7 a trick let go too late, 8 one held
        // (or with two tricks or more in the rider's trick list).
        constexpr uint8_t kBailTrickLate = 7;
        constexpr uint8_t kBailTrickHeld = 8;
        constexpr uint8_t kTrickEnding = 3; // the air tricks' phase 3: ending
        constexpr uint8_t kTrickHeld = 2;
    }

    // States 9 and 10's turn: see above. Returns the last angle or turn it
    // worked out (the callers do not read it). GameCube 0x8007D2F4.
    int32_t wheelieTurn(Rider *rider)
    {
        ee::Local<zge::Matrix> unused;
        zge::matrixIdentity(unused); // a matrix the original sets up and never reads
        const uint8_t kind = rider->contactKind;
        if (kind >= kContactKinds4)
            return 0;
        if (kind == 0)
            return 1;
        int32_t angle = zge::angleAcos(ee::abs(rider->up[1]));
        if (!(*g_onSlope < ee::abs(ee::toFloat(angle))))
            return angle;
        angle = zge::angleAcos(ee::abs(rider->right[1]));
        const ee::Float turnSpeed = *g_turnSpeed;
        if (!(ee::abs(ee::toFloat(angle)) < *g_rightRowLevel))
            return angle;
        const ee::Float speed = rider->speed;
        if (!(speed < turnSpeed * *g_wheelieTurnShare))
            return angle;
        const int32_t turn = ee::toInt(rider->right[1] * ((turnSpeed - speed) * *g_wheelieTurnDegrees) * *g_degreesToTurn);
        rider->yawRate = static_cast<int16_t>(rider->yawRate + turn);
        return turn;
    }
    READABLE_FUNCTION(wheelieTurn, Verified);

    // Landing in an air trick (either button's) not past its start or held
    // phase: a bail (physics 2) of kind 7 or 8, unless the trick may land
    // unfinished. Returns 1 when it bailed. GameCube 0x8008AFEC; BMX XXX
    // PS2 unfinishedTrickBail.
    int32_t unfinishedTrickBail(Rider *rider)
    {
        if (actionState(rider, uint32_t(kLevelAction)) != kActionAir)
            return 0;
        const uint8_t subState = actionState(rider, uint32_t(kLevelSubState));
        if (subState != kAirTrick && subState != kAirTrickB)
            return 0;
        if (actionState(rider, uint32_t(kLevelPhase)) >= kTrickEnding)
            return 0;
        if (subState == kAirTrick && ((*g_trickLandFlags)[rider->trick] & kTrickLandsUnfinished))
            return 0;
        setPhysicsState(rider, uint32_t(kPhysicsBail));
        uint8_t kind = kBailTrickHeld;
        if (rider->score.lineCount < 2)
        {
            if (actionState(rider, uint32_t(kLevelPhase)) != kTrickHeld)
                kind = kBailTrickLate;
            else
                kind = (rider->flags & kFlagTrickHeld) ? kBailTrickHeld : kBailTrickLate;
        }
        rider->bailKind = kind;
        return 1;
    }
    READABLE_FUNCTION(unfinishedTrickBail, Verified);
}
