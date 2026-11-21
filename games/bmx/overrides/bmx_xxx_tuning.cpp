// BMX XXX (SLUS-20415): the developer tuning table, its GameCube release's, on
// the PS2's values (Mods/tuning.ini, as Aggressive Inline's).
//
// The GameCube's debug menu has a TUNING screen (its builder 0x800518A0: 114
// rows of name, kind, variable, step, least, most and value); the PS2 has
// neither the screen nor the table. Each row's variable is paired with the
// PS2 global it is by the code that uses them (tooling's tuning pairing: the
// two builds' matched functions refer to both, the data blocks' order kept;
// then each one left read in both builds' matched functions, the same term;
// workdb notes, topic layout, at each PS2 global): 99 of the 114, and eight
// of the nine rider abilities, fields of the rider record (the GameCube's
// getter 0x800745BC is the PS2's riderOf; each stat's PS2 readers, by the
// readable code's names, and their GameCube matches read the same offset:
// air, speed, spin, flip, balance, technique, manual and wallride, the
// PS2's Rider airStat .. wallRideStat).
//
// The camera's ten are one row of the follow mode's settings and its two
// small tables (games/bmx/camera/action_camera.h CameraSettings; follow.cpp
// g_followLeads, g_followAirHeights): the one-view layout's camera type 2,
// whose twelve words are the GameCube's, in a row the PS2 widens from 0x2C
// to 0x30 bytes; both builds' follow mode reads that row's offset, rates and
// field of view and the two tables by the same index.
//
// Paired but not rows: far Z clip plane (g_cameraFarClip: the camera's start
// sets it from the level's camera file at every level, so a value set before
// would never stay), and the two special feature flags rows (the saved
// game's features, read from the save): they are no tuning. Kept but read by
// nothing on the PS2 (its defaults there, in the GameCube's order, so a row
// would change nothing): rotation max - grind, grind-to-grind ang tlrnce and
// wallride angle tolerance. Unpaired: rider abil - land (+0x3C: no rider
// function of either build reads it), display rider position (the PS2 has
// no code of it), NPR to record and run length (sec) (no GameCube code but
// the TUNING screen reads them).
//
// Every PS2 default is the GameCube's but four floats: three a last bit
// apart (grind min velocity, pedal hysteresis, max stall speed) and col box
// - air - Y (the PS2's 0.25, the GameCube's 0.7).
//
// The table's names are the GameCube's, one section, [TUNING]; tuning.ini,
// tuning-reference.ini and the TUNING screen (F2) are the engine's games'
// (zge_tuning.h), as DM2's (dave_mirra_tuning.cpp). The camera rows are the
// tables the gameplay page's Camera Distance and Camera Height write when
// picked (bmx_xxx_gameplay.cpp): a picked setting is applied over them.
//
// The rider abilities are the riding rider's (riderOf(0)), made from the
// character's stats at each ride, so they are the screen's alone, after the
// table's rows, while a rider rides; tuning.ini has no row of them.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_tuning.cpp.h"
#include "overrides/zge_tuning.h"

#include <cstddef>
#include <cstdint>

namespace bmx_xxx
{
    namespace
    {
        using zge_host::tuning::Kind;
        using zge_host::tuning::Row;

        // A surface's friction (games/bmx/rider/physics/physics.h Surface:
        // grip, slip, rolling loss, grind drag): the GameCube's concrete
        // friction rows are the table's first surface's.
        struct SurfaceFriction
        {
            float grip;
            float slip;
            float rollingLoss;
            float grindDrag;
        };
        static_assert(sizeof(SurfaceFriction) == 0x10 && offsetof(SurfaceFriction, grindDrag) == 0xC);

        // The rider's air bodies' size (games/bmx/rider/physics/takeoff.cpp
        // g_airBodyHalfExtents, at g_cornerSide): the GameCube's col box -
        // air - X, Y and Z are its x, y and z.
        struct AirBodyHalfExtents
        {
            float x, y, z, w;
        };
        static_assert(sizeof(AirBodyHalfExtents) == 0x10 && offsetof(AirBodyHalfExtents, y) == 4);

        // A row of the follow mode's settings (games/bmx/camera/action_camera.h
        // CameraSettings), as the host reads it.
        struct CameraSettings
        {
            float right, up, forward, w; // the eye's offset in the mode's space
            float distance;
            float spring, damping, eyeShare, lookAtShare, fovShare;
            float fov; // degrees
            uint32_t unused;
        };
        static_assert(sizeof(CameraSettings) == 0x30 && offsetof(CameraSettings, spring) == 0x14 &&
                      offsetof(CameraSettings, fov) == 0x28);
        // The GameCube's camera rows: the one-view layout's (the first of the
        // follow mode's layout groups) camera type 2.
        constexpr uint32_t kTunedCameraType = 2;
        constexpr uint32_t kTunedCamera = kTunedCameraType * sizeof(CameraSettings);
        constexpr uint32_t kTunedCameraEntry = kTunedCameraType * sizeof(float); // in g_followLeads, g_followAirHeights

        const Row kRows[] = {
        {"display profiling info", sym::bmx::g_debugDisplay, 0, Kind::Byte, 1, 0, 2},
        {"hide game menus", sym::bmx::g_overlayHidden, 0, Kind::Byte, 1, 0, 1},
        {"NPR flags", sym::bmx::g_npcFrameFlags, 0, Kind::Word, 1, 0, 7},
        {"damage scalar", sym::bmx::g_rideDistanceScale, 0, Kind::Float, 0.005f, 0.0f, 100.0f},
        {"dyn bounce rider", sym::bmx::g_bailRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction rider", sym::bmx::g_bailForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"dyn bounce bbox", sym::bmx::g_riderTumbleRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction bbox", sym::bmx::g_riderTumbleForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"dyn bounce bike", sym::bmx::g_bikeTumbleRestitution, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"dyn friction bike", sym::bmx::g_bikeTumbleForce, 0, Kind::Float, 0.005f, -10.0f, 10.0f},
        {"col box - air - X", sym::bmx::g_cornerSide, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"col box - air - Y", sym::bmx::g_cornerSide, offsetof(AirBodyHalfExtents, y), Kind::Float, 0.005f, 0.0f, 10.0f},
        {"col box - air - Z", sym::bmx::g_cornerAlong, 0, Kind::Float, 0.005f, 0.0f, 10.0f},
        {"NPR clip distance", sym::bmx::g_npcDrawDistance, 0, Kind::Float, 1.0f, 1.0f, 1000.0f},
        {"max NPRs riding around", sym::bmx::g_mostRidingNpcs, 0, Kind::Word, 1, 0, 16},
        {"timer disable", sym::bmx::g_clockStopped, 0, Kind::Word, 1, 0, 1},
        {"draw physics", sym::bmx::g_drawBodies, 0, Kind::Word, 1, 0, 1},
        {"display sound channels", sym::bmx::g_showVoiceStates, 0, Kind::Word, 1, 0, 1},
        {"grind off balance factor", sym::bmx::g_grindTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"grind balance correction", sym::bmx::g_grindStickLean, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"grind balance speed factor", sym::bmx::g_grindSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"grind balance speed factor cutoff", sym::bmx::g_grindSlowSpeed, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
        {"grind detection eccentricity", sym::bmx::g_railAcrossReach, 0, Kind::Float, 0.01f, 0.0f, 1.0f},
        {"grind jumpoff dir timeout", sym::bmx::g_airStickHoldFrames, 0, Kind::Word, 1, 0, 600},
        {"overall balance time factor (BTF)", sym::bmx::g_flatlandTipGrowth, 0, Kind::Float, 0.01f, -10.0f, 10.0f},
        {"overall BTF timeout", sym::bmx::g_flatlandTipFrames, 0, Kind::Word, 60, 0, 12000},
        {"grind speed boost", sym::bmx::g_railBoost, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"grind min velocity", sym::bmx::g_railBoostSpeedLeast, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"grind gravity scalar", sym::bmx::g_railGravityShare, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"wallride speed boost", sym::bmx::g_stallKeptSpeedShare, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"wallride min velocity", sym::bmx::g_stallKeptSpeedLeast, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"manual off balance factor", sym::bmx::g_flatlandTip, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"manual balance correction", sym::bmx::g_flatlandStickLean, 0, Kind::Float, 0.0001f, -1.0f, 1.0f},
        {"manual balance speed factor", sym::bmx::g_flatlandSlowTip, 0, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"manual balance speed factor cutoff", sym::bmx::g_flatlandSlowSpeed, 0, Kind::Float, 0.0016666667f, 0.0f, 1.6666666f},
        {"manual slowdown friction", sym::bmx::g_flatlandDrag, 0, Kind::Float, 1e-05f, -1.0f, 1.0f},
        {"walltap/fastplant off balance factor", sym::bmx::g_lipTip, 0, Kind::Float, 0.001f, -100.0f, 100.0f},
        {"max pedal speed", sym::bmx::g_pedalSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"pedal acceleration", sym::bmx::g_pedalPush, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"pedal hysteresis", sym::bmx::g_pedalRepeat, 0, Kind::Float, 0.001f, 0.0f, 0.5f},
        {"max stall speed", sym::bmx::g_railMostAcross, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"rhythm jump speed", sym::bmx::g_launchSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"spine vel adjust", sym::bmx::g_vertKeep, 0, Kind::Float, 0.001f, 0.0f, 10.0f},
        {"grind/stall vel", sym::bmx::g_stallSpeed, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"level out frames", sym::bmx::g_alignFrames, 0, Kind::Word, 1, 1, 300},
        {"max jump force", sym::bmx::g_hopPerStat, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"min jump force", sym::bmx::g_hopLeast, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"ramp jump bonus", sym::bmx::g_hopSlopeLoss, 0, Kind::Float, 0.001f, -1.0f, 1.0f},
        {"brake friction scalar", sym::bmx::g_brakeDrag, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
        {"rotation adjust - ground", sym::bmx::g_wheelieSteerRate, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
        {"rotation max - ground", sym::bmx::g_wheelieSteerMost, 0, Kind::Float, 1.0f, 0.0f, 180.0f},
        {"rotation speed factor", sym::bmx::g_landSteerSpeed, 0, Kind::Float, 0.0001f, 0.0f, 100.0f},
        {"rotation adjust - air turn", sym::bmx::g_airStickTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"rotation adjust - air spin", sym::bmx::g_airButtonTurn, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"rotation max - air", sym::bmx::g_airTurnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"pitch adjust - air", sym::bmx::g_turnLeast, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"pitch max - air", sym::bmx::g_turnMost, 0, Kind::Float, 0.01f, 0.0f, 180.0f},
        {"jump max timer", sym::bmx::g_hopFullFrames, 0, Kind::Word, 1, 1, 300},
        {"friction - concrete static", sym::bmx::g_surfaces, offsetof(SurfaceFriction, grip), Kind::Float, 0.01f, 0.0f, 1000.0f},
        {"friction - concrete sliding", sym::bmx::g_surfaces, offsetof(SurfaceFriction, slip), Kind::Float, 0.01f, 0.0f, 1000.0f},
        {"friction - concrete roll", sym::bmx::g_surfaces, offsetof(SurfaceFriction, rollingLoss), Kind::Float, 1e-06f, 0.0f, 1.0f},
        {"friction - concrete grind", sym::bmx::g_surfaces, offsetof(SurfaceFriction, grindDrag), Kind::Float, 1e-06f, 0.0f, 1.0f},
        {"friction - turning penalty", sym::bmx::g_slideLoss, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"brake bias - towards rear", sym::bmx::g_wheelieSlide, 0, Kind::Float, 0.001f, 0.0f, 1.0f},
        {"grind around corners factor", sym::bmx::g_railSpeedKept, 0, Kind::Float, 0.1f, 0.0f, 1.0f},
        {"roll angle tolerance", sym::bmx::g_wallMostAngle, 0, Kind::Angle, 182, 0, 16384},
        {"land angle tolerance", sym::bmx::g_landWithin, 0, Kind::Angle, 18, 0, 16384},
        {"land angle tolerance (fakie)", sym::bmx::g_rollWithin, 0, Kind::Angle, 18, 0, 16384},
        {"grind balance angle tlrnce", sym::bmx::g_railLeanMost, 0, Kind::Angle, 18, 0, 32585},
        {"grind edge-to-edge angle tlrnce", sym::bmx::g_nextRailMostTurn, 0, Kind::Angle, 18, 0, 32749},
        {"roll over factor", sym::bmx::g_slopeReachScale, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"turn down factor", sym::bmx::g_wheelieTurnDegrees, 0, Kind::Float, 0.1f, 0.0f, 1000.0f},
        {"orient interpolation", sym::bmx::g_railAlignShare, 0, Kind::Float, 0.0001f, 0.0f, 1.0f},
        {"max lean angle", sym::bmx::g_leanDegrees, 0, Kind::Float, 0.1f, 0.0f, 90.0f},
        {"approach frames - grind", sym::bmx::g_approachGrindSteps, 0, Kind::Word, 1, 1, 300},
        {"approach frames - walltap/wallride", sym::bmx::g_approachOtherSteps, 0, Kind::Word, 1, 1, 300},
        {"approach frames - min", sym::bmx::g_leastSnapFrames, 0, Kind::Word, 1, 0, 300},
        {"overall edge detect radius", sym::bmx::g_approachRailReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection distance - air", sym::bmx::g_approachAirReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection distance - rolling", sym::bmx::g_approachGroundReach, 0, Kind::Float, 0.01f, 0.0f, 100.0f},
        {"detection increment - edge", sym::bmx::g_approachReachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"detection increment - wall", sym::bmx::g_approachLipReachGrowth, 0, Kind::Float, 0.001f, 0.0f, 100.0f},
        {"wallride traction", sym::bmx::g_lipGravityScale, 0, Kind::Float, 0.01f, 0.0f, 1.0f},
        {"cam clip dist - rider", sym::bmx::g_cameraRiderGap, 0, Kind::Float, 0.1f, 0.0f, 10.0f},
        {"cam clip dist - terrain", sym::bmx::g_cameraWallRadius, 0, Kind::Float, 0.1f, 0.0f, 10.0f},
        {"camera behind FOV", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, fov), Kind::Float, 1.0f, 0.0f, 180.0f},
        {"camera target offset", sym::bmx::g_followLeads, kTunedCameraEntry, Kind::Float, 0.01f, 0.0f, 50.0f},
        {"cam distance behind", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, forward), Kind::Float, 0.01f, -100.0f, 100.0f},
        {"cam distance above roll", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, up), Kind::Float, 0.01f, -100.0f, 100.0f},
        {"cam distance side roll", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, right), Kind::Float, 0.01f, -100.0f, 100.0f},
        {"cam distance above air", sym::bmx::g_followAirHeights, kTunedCameraEntry, Kind::Float, 0.01f, -100.0f, 100.0f},
        {"cam constant - spring", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, spring), Kind::Float, 0.001f, 0.0f, 1.0f},
        {"cam constant - damping", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, damping), Kind::Float, 0.001f, 0.0f, 1.0f},
        {"cam constant - spring end", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, eyeShare), Kind::Float, 0.001f, 0.0f, 1.0f},
        {"cam constant - target", sym::bmx::g_followCameraSettings, kTunedCamera + offsetof(CameraSettings, lookAtShare), Kind::Float, 0.001f, 0.0f, 1.0f},
        {"spline camera test", sym::bmx::g_cameraObjectsOn, 0, Kind::Word, 1, 0, 1},
        };

        // The rider abilities: a field of the riding rider's record
        // (games/bmx/rider/rider.h Rider: airStat .. wallRideStat, as far as
        // them), the GameCube's names and ranges.
        struct RiderStats
        {
            uint8_t _000[0x20];
            float air, speed, spin, flip, balance, technique, manual;
            uint8_t _03c[4]; // (the GameCube's land: not paired)
            float wallRide;
        };
        static_assert(offsetof(RiderStats, air) == 0x20 && offsetof(RiderStats, manual) == 0x38 &&
                      offsetof(RiderStats, wallRide) == 0x40);
        const Row kRiderRows[] = {
            {"rider abil - air", 0, offsetof(RiderStats, air), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - speed", 0, offsetof(RiderStats, speed), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - spin", 0, offsetof(RiderStats, spin), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - flip", 0, offsetof(RiderStats, flip), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - balance", 0, offsetof(RiderStats, balance), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - technique", 0, offsetof(RiderStats, technique), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - manual", 0, offsetof(RiderStats, manual), Kind::Float, 0.001f, 0.0f, 10.0f},
            {"rider abil - wallride", 0, offsetof(RiderStats, wallRide), Kind::Float, 0.001f, 0.0f, 10.0f},
        };

        const zge_host::tuning::Table kTable = {
            "BMX XXX", "the GameCube release's developer tuning table on the PS2's values, every\n",
            kRows, sizeof(kRows) / sizeof(kRows[0]), kRiderRows, sizeof(kRiderRows) / sizeof(kRiderRows[0]),
            sym::bmx::riderOf, sym::bmx::drawColouredDebugText, sym::bmx::debugPrintInColour, sym::bmx::g_debugEnabled,
        };
    }

    void installTuning(PS2Machine &runtime)
    {
        zge_host::tuning::install(runtime, kTable);
    }
}
