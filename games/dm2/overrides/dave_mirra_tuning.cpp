// Dave Mirra Freestyle BMX 2 (SLUS-20159): a developer tuning table on its
// riding values, by the names of the GameCube BMX XXX's TUNING screen
// (Mods/tuning.ini, as BMX XXX's and Aggressive Inline's).
//
// No release of DM2 has a tuning table or screen; its rates are separate
// variables, mostly one block of small data (0x2593A0 .. 0x259528) in the
// order BMX XXX's PS2 keeps the same variables in. BMX XXX's GameCube TUNING
// rows (bmx_xxx_tuning.cpp) are carried to DM2 by the code: the two PS2
// builds' matched functions that refer to both (an order-keeping alignment
// of each data block), and where DM2's functions have no match, the readable
// bodies of both games reading the variable in the same term (each a workdb
// note, topic layout, at the DM2 global): 60 rows. Not here: the bail's
// bounce and friction (DM2's bail has no such variables), the grind and
// wallride speed boosts, grind detection eccentricity, grind jumpoff dir
// timeout and grind balance correction (BMX XXX's own riding), damage
// scalar (DM2's 8.33 at that place is a unit change, not the GameCube's
// 0.45), grind edge-to-edge angle tolerance (a constant in DM2's code), and
// grind-to-grind ang tlrnce and wallride angle tolerance: DM2 keeps the
// GameCube's defaults for both (12014 and 10922, at 0x2593F8 in the block's
// order) but no code reads them. The camera's and the debug rows are not
// riding.
//
// DM2's grind balance has another scale than BMX XXX's: its "grind off
// balance factor" is 10 where the GameCube's range is -1 .. 1, so the
// screen's range there reaches DM2's own value. Rows DM2 names by its own
// code (dm2/rider): "manual" is the one-wheel balance (wheelieSubState),
// "brake friction scalar" the stick-back state's drag (manualState, BMX
// XXX's brakeState), "rotation max - grind" the stick's turn of a grind's
// and a stall's tilt.
//
// The rider abilities are the riding rider's (riderOf(0)), made from the
// character's stats at each ride, so they are the screen's alone, after the
// table's rows, while a rider rides; tuning.ini has no row of them. DM2's
// rider has seven of the GameCube's nine (no flip; its landing stat, as BMX
// XXX's land, has no reader to pair).
//
// tuning.ini, tuning-reference.ini and the TUNING screen (F2): zge_tuning.h.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_tuning.cpp.h"
#include "overrides/zge_tuning.h"

#include <cstddef>
#include <cstdint>

namespace dave_mirra
{
    namespace
    {
        using zge_host::tuning::Kind;
        using zge_host::tuning::Row;

        // A surface's friction (games/dm2/rider/physics/physics.h Surface:
        // grip, slip, rolling loss, grind drag): the GameCube's concrete
        // friction rows are the table's first surface's, as BMX XXX's.
        struct SurfaceFriction
        {
            float grip;
            float slip;
            float rollingLoss;
            float grindDrag;
        };
        static_assert(sizeof(SurfaceFriction) == 0x10 && offsetof(SurfaceFriction, grindDrag) == 0xC);

        // The GameCube's rows in its order, each with DM2's global.
        const Row kRows[] = {
            {"col box - air - X", sym::dm2::g_railSideReach, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
            {"col box - air - Y", sym::dm2::g_probeY, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
            {"col box - air - Z", sym::dm2::g_probeZ, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
            {"grind off balance factor", sym::dm2::g_railTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
            {"grind balance speed factor", sym::dm2::g_railSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
            {"grind balance speed factor cutoff", sym::dm2::g_railSteady, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
            {"overall balance time factor (BTF)", sym::dm2::g_railTimeTip, 0, Kind::Float, 0.01f, -10.0f, 10.0f},
            {"overall BTF timeout", sym::dm2::g_railTimeFull, 0, Kind::Word, 60, 0, 12000},
            {"manual off balance factor", sym::dm2::g_wheelieTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
            {"manual balance correction", sym::dm2::g_wheelieStickBalance, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
            {"manual balance speed factor", sym::dm2::g_wheelieSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
            {"manual balance speed factor cutoff", sym::dm2::g_wheelieSlow, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
            {"manual slowdown friction", sym::dm2::g_wheelieDrag, 0, Kind::Float, 1e-05f, -1.0f, 1.0f},
            {"walltap/fastplant off balance factor", sym::dm2::g_lipTip, 0, Kind::Float, 0.001f, -100.0f, 100.0f},
            {"max pedal speed", sym::dm2::g_pedalSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"pedal acceleration", sym::dm2::g_pedalPush, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
            {"pedal hysteresis", sym::dm2::g_pedalRepeat, 0, Kind::Float, 0.001f, 0.0f, 0.5f},
            {"max stall speed", sym::dm2::g_railMostAcross, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"rhythm jump speed", sym::dm2::g_launchSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"spine vel adjust", sym::dm2::g_vertKeep, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
            {"grind/stall vel", sym::dm2::g_stallSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"level out frames", sym::dm2::g_alignFrames, 0, Kind::Word, 1, 1, 300},
            {"max jump force", sym::dm2::g_hopPerStat, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"min jump force", sym::dm2::g_hopLeast, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"ramp jump bonus", sym::dm2::g_hopSlopeLoss, 0, Kind::Float, 0.001f, -1.0f, 1.0f},
            {"brake friction scalar", sym::dm2::g_manualDrag, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
            {"rotation adjust - ground", sym::dm2::g_wheelieSteerRate, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
            {"rotation max - ground", sym::dm2::g_wheelieSteerMost, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
            {"rotation speed factor", sym::dm2::g_landSteerSpeed, 0, Kind::Float, 0.0001f, 0.0f, 100.0f},
            {"rotation adjust - air turn", sym::dm2::g_airStickTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"rotation adjust - air spin", sym::dm2::g_airButtonTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"rotation max - air", sym::dm2::g_airTurnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"pitch adjust - air", sym::dm2::g_turnLeast, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"pitch max - air", sym::dm2::g_turnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"rotation max - grind", sym::dm2::g_grindTurnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
            {"jump max timer", sym::dm2::g_hopFullFrames, 0, Kind::Word, 1, 1, 300},
            {"friction - concrete static", sym::dm2::surfaces, offsetof(SurfaceFriction, grip), Kind::Float, 0.01f, 0.0f, 1000.0f},
            {"friction - concrete sliding", sym::dm2::surfaces, offsetof(SurfaceFriction, slip), Kind::Float, 0.01f, 0.0f, 1000.0f},
            {"friction - concrete roll", sym::dm2::surfaces, offsetof(SurfaceFriction, rollingLoss), Kind::Float, 1e-06f, 0.0f, 1.0f},
            {"friction - concrete grind", sym::dm2::surfaces, offsetof(SurfaceFriction, grindDrag), Kind::Float, 1e-06f, 0.0f, 1.0f},
            {"friction - turning penalty", sym::dm2::g_slideLoss, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"brake bias - towards rear", sym::dm2::g_wheelieSlide, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
            {"grind around corners factor", sym::dm2::g_railSpeedKept, 0, Kind::Float, 0.1f, 0.0f, 1.0f},
            {"roll angle tolerance", sym::dm2::g_wallMostAngle, 0, Kind::Angle, 182, 0, 16384},
            {"land angle tolerance", sym::dm2::g_landWithin, 0, Kind::Angle, 18, 0, 16384},
            {"land angle tolerance (fakie)", sym::dm2::g_rollWithin, 0, Kind::Angle, 18, 0, 16384},
            {"grind balance angle tlrnce", sym::dm2::g_railFallAngle, 0, Kind::Angle, 18, 0, 32585},
            {"roll over factor", sym::dm2::g_slopeReachScale, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
            {"turn down factor", sym::dm2::g_wheelieTurnDegrees, 0, Kind::Float, 0.1f, 0.0f, 1000.0f},
            {"orient interpolation", sym::dm2::g_railAlignShare, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
            {"max lean angle", sym::dm2::g_leanDegrees, 0, Kind::Float, 0.1f, 0.0f, 90.0f},
            {"approach frames - grind", sym::dm2::g_grindSteps, 0, Kind::Word, 1, 1, 300},
            {"approach frames - walltap/wallride", sym::dm2::g_otherSteps, 0, Kind::Word, 1, 1, 300},
            {"approach frames - min", sym::dm2::g_leastSnapFrames, 0, Kind::Word, 1, 0, 300},
            {"overall edge detect radius", sym::dm2::g_railReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
            {"detection distance - air", sym::dm2::g_airReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
            {"detection distance - rolling", sym::dm2::g_groundReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
            {"detection increment - edge", sym::dm2::g_reachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
            {"detection increment - wall", sym::dm2::g_lipReachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
            {"wallride traction", sym::dm2::g_lipGravityScale, 0, Kind::Float, 0.01f, 0.0f, 1.0f},
        };

        // The rider abilities: a field of the riding rider's record
        // (games/dm2/rider/rider.h Rider: airStat .. wallRideStat), the
        // GameCube's names and ranges.
        struct RiderStats
        {
            uint8_t _000[0x20];
            float air, speed, spin, balance, technique, manual;
            uint8_t _038[4]; // the landing stat (the GameCube's land: not paired)
            float wallRide;
        };
        static_assert(offsetof(RiderStats, air) == 0x20 && offsetof(RiderStats, manual) == 0x34 &&
                      offsetof(RiderStats, wallRide) == 0x3C);
        const Row kRiderRows[] = {
            {"rider abil - air", 0, offsetof(RiderStats, air), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - speed", 0, offsetof(RiderStats, speed), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - spin", 0, offsetof(RiderStats, spin), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - balance", 0, offsetof(RiderStats, balance), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - technique", 0, offsetof(RiderStats, technique), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - manual", 0, offsetof(RiderStats, manual), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - wallride", 0, offsetof(RiderStats, wallRide), Kind::Float, 0.001f, 0.0f, 10.0f},
        };

        const zge_host::tuning::Table kTable = {
            "Dave Mirra BMX 2", "the GameCube BMX XXX's developer tuning table on DM2's values, every\n",
            kRows, sizeof(kRows) / sizeof(kRows[0]), kRiderRows, sizeof(kRiderRows) / sizeof(kRiderRows[0]),
            sym::dm2::riderOf, sym::dm2::drawDebugText, sym::dm2::debugPrint, sym::dm2::g_debugEnabled,
        };
    }

    void installTuning(PS2Machine &runtime)
    {
        zge_host::tuning::install(runtime, kTable);
    }
}
