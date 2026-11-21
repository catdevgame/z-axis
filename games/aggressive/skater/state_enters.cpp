// The enter callbacks of the three main states (StateRow::enter, run by
// setSkaterState after it has written the new state; Player
// +0x75 then holds the state left):
//
// - Ground (also Cess Slide's): the landing. Riding backwards flips fakie;
//   the angle between the way the skater moves and the way it faces grades
//   the landing (Physics / Landing Angle), and too far off is a bail
//   ("You landed sideways!"); the landing sound, and the object landed on
//   hears "Land".
// - Air: the take-off. The jump sound from the ground; a take-off from a
//   ramp or rail with launch values sets the speed; the object left hears
//   "Jump" (not from a grind, double grind, stall or wall ride, whose exits
//   send their own events) and the goals hear of it.
// - Grind: onto the rail. Facing turned to the way the skater moves, the
//   side the rail is on, the grind balance's start (drift, ease back,
//   delay), the HUD's balance meter, the motion blur reset.
//
// Each sends synchronous engine events (an object's script handlers run in
// them; the Grind's through joinNextRail), so all three are registered as
// waiting, as the other states' enters and exits that send events.
//
#include "skater.h"

#include "aggressive/camera.h"
#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "moves.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/hud/hud.h"
#include "aggressive/level/objectives.h"

#include "sym/aggressive/skater/state_enters.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        using zge::Vec4;

        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;

        constexpr uint32_t kLandedSideways = 1;        // its kind: "You landed sideways!"
        constexpr uint32_t kSurfaceSound = sym::surfaceSound; // (object, surface) -> its sound
        constexpr int32_t kJumpSound = 0x19;
        constexpr int32_t kLandSound = 0x1B;
        constexpr int32_t kBailLandSound = 0x1D;
        // (player, frame time) -> 1 when the skater, tilted off the world's
        // up, finds no ground near on either side and is moved sideways off
        // the edge (moveSkater); 0 otherwise (inferred from its code).
        // (player, stat, amount): experience toward a stat (Skater / Jump
        // Trick XP, Top Speed XP, Spin XP ... by the stat; amounts below 0
        // are ignored; experience.cpp statExperience).

        // A scene object's surface record (faceTagRecord finds it), its fields
        // by accessor: +0x8 the kind its events carry, +0xC its physics kind
        // (1 a quarter pipe's lip, 3 a slope with its own gravity), +0x10,
        // +0x14, +0x18 three values; a take-off reads them as the least and
        // most speed a launch needs (0: no most) and the launch speed.
        constexpr uint32_t kQuarterPipeLip = 1;
        // The same for a rail's edge record (railEdgeRecord): +0x8 the edge's
        // tag (4 a horizontal pole), then +0xC, +0x10, +0x14 (a pole's exit
        // heading, lift and exit speed: poles_walls.cpp, pole_vault.cpp).
        constexpr uint32_t kHorizontalPoleEdge = 4;

        // (player, object, event name, data*): the object's script gets the
        // named event, synchronously.
        constexpr uint32_t kLand = sym::land; // "Land"
        constexpr uint32_t kJump = sym::jumpEvent; // "Jump"
        // (scene object, 0) -> the object it belongs to (its +0x15C record's
        // +0x8; inferred), 0 for none.
        // (object name hash, surface kind): the goals hear of a jump off the
        // object (inferred: it matches a list of goals by both).

        // (player) onto Player::rail (grind.cpp names it for
        // the next rail); a rail it cannot take puts the skater in the Air.
        // (player, stat, which) -> a speed from the skater's stats and the
        // tuning; stat 3 which 1 the grind balance's drift.
        // (follow camera, blur alpha, 1, 1): the camera's motion blur
        // (docs/TUNING.md "Camera", Normal Blur).
        inline ee::Global<ee::Float, sym::g_halfTurnsPerDegree> g_halfTurnsPerDegree; // 1/180
        inline ee::Global<ee::Float, sym::g_stillSpeed3> g_stillSpeed;         // 1e-5
        inline ee::Global<ee::Float, sym::g_balanceDead> g_balanceDead;        // 1e-5: a balance this small leans neither way
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        constexpr uint32_t kCameraNormalBlur = 42; // Camera / Normal Blur
        constexpr uint32_t kPhysicsLandingAngle = 34;
        ee::Float physics(uint32_t entry) { return tuning(3, entry); }
        enum GrindWallsEntry : uint32_t
        {
            kGrindBalanceDelay = 6,
            kGrindBalanceBestReset = 7,
            kGrindBalanceReset = 8,
            kGrindBalanceSpinDead = 9,
            kGrindBalanceSpinReset = 10,
            kGrindBalanceShortInit = 12,
            kGrindBalanceShortDead = 13,
        };
        ee::Float grindWalls(uint32_t entry) { return tuning(14, entry); }
        enum VibrateEntry : uint32_t
        {
            kCessSlideBig = 6,
            kCessSlideSmall = 7,
            kCessSlideTime = 8,
        };
        ee::Float vibrate(uint32_t entry) { return tuning(18, entry); }

        constexpr uint32_t kActionSpin = kActionButton100 | kActionButton200;

        // CVT.S.W of an unsigned word, as the compiler does it: halved
        // (the low bit kept) and doubled back when the top bit is set.
        ee::Float unsignedToFloat(uint32_t u)
        {
            if (static_cast<int32_t>(u) >= 0)
                return ee::toFloat(static_cast<int32_t>(u));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((u >> 1) | (u & 1u)));
            return half + half;
        }

        // The landing and take-off sound's volume: 64, and 4 a unit of speed,
        // to 127 .
        int32_t soundVolume(ee::Ptr<Player> player)
        {
            ee::Float volume = player->speed * ee::Float(4.0f) + ee::Float(64.0f);
            if (ee::Float(127.0f) < volume)
                volume = 127.0f;
            return ee::toInt(volume);
        }
        // The sound of the surface under the skater (0 for none).
        int32_t surfaceSound(ee::Ptr<Player> player)
        {
            if (player->object == 0 || player->surface == kNoSurface)
                return 0;
            return skater::surfaceSound(player->objectNode, player->surface);
        }
    }

    // groundEnter, Ground's enter (Cess Slide's too). Entering a cess
    // slide rumbles (Vibrate / CessSlide). On an object (unless the skater
    // is slid off an edge it balances on): the motion and the facing along
    // the ground; facing against the motion flips fakie (turned half round
    // about up, the drawn orientation snapped); how close they are grades
    // the landing (Player +0x7C): within Landing Angle is a bail, landed
    // sideways (the spin's experience offered back negative, which the
    // experience function ignores); then the landing sound, a bail's own
    // one. Landing from the air without a bail, the object hears "Land"
    // with its surface kind. Returns 1.
    int32_t groundEnter(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> motion;
        ee::Local<Vec4> groundNormal;
        ee::Local<Vec4> facing;
        ee::Local<uint32_t> data;
        PlayerAxes &axes = player->axes;
        bool bailed = false;

        if (player->state == State::CessSlide)
        {
            player->skidFlags |= kSkidStart; // the slide's skid marks start next frame
            const int32_t time = sce::libgcc::fptoui(vibrate(kCessSlideTime));
            aggressive::vibrate(&player->controls, time, vibrate(kCessSlideSmall), vibrate(kCessSlideBig));
        }

        player->pushing = 0;
        if (player->object != 0 && slideOffEdge(player, ee::Float(0.0f)) == 0)
        {
            // The motion along the ground, unless barely moving.
            *groundNormal = player->surfaceNormal;
            alongPlane(motion, groundNormal, &player->velocity);
            if (ee::Float(1.0f) < zge::dot(*motion, *motion))
            {
                *motion = zge::unit(*motion);
                const auto facingAlongGround = [&]
                {
                    alongPlane(facing, groundNormal, &axes.forward);
                    *facing = zge::unit(*facing);
                    return zge::dot(*motion, *facing);
                };
                ee::Float alignment = facingAlongGround();
                if (alignment < 0.0f)
                {
                    // Landed backwards: fakie flips, the skater turned round.
                    player->drawnRefresh = 1;
                    player->fakie = player->fakie == 0 ? 1 : 0;
                    player->drawFlags |= kDrawnSnap;
                    rotateOrientation(&player->orientation, &axes.up.xyz(), ee::Float(180.0f));
                    alignment = facingAlongGround();
                }

                const ee::Float landingAngle = physics(kPhysicsLandingAngle);
                if (alignment < landingAngle)
                {
                    player->landing = 3;
                    const uint32_t halfTurns = static_cast<uint32_t>(ee::toInt(player->spinDegreesCounterClockwise * *g_halfTurnsPerDegree)) +
                                               static_cast<uint32_t>(ee::toInt(player->spinDegreesClockwise * *g_halfTurnsPerDegree));
                    statExperience(player, kStatSpin, ee::toFloat(static_cast<int32_t>(0u - halfTurns)));
                    bailed = true;
                    skaterCrash(player, kLandedSideways);
                }
                else if (landingAngle * ee::Float(3.0f) < alignment)
                    player->landing = 0;
                else
                {
                    const ee::Float twice = landingAngle + landingAngle;
                    player->landing = twice < alignment ? 1 : 2;
                }

                const int32_t sound = surfaceSound(player);
                const int32_t volume = soundVolume(player);
                aggressive::soundEvent(player->landing == 3 ? kBailLandSound : kLandSound, &player->position, sound, volume, 0);
            }
        }

        // Down from the air, without a bail: the object hears "Land".
        const uint8_t from = player->previousState;
        const bool fromAir = from == uint8_t(State::Air) || from == uint8_t(State::AirQuarterPipe) ||
                             from == uint8_t(State::AirSpine);
        if (fromAir)
        {
            if (bailed || player->object == 0 || player->surface == kNoSurface)
                return 1;
            *data = surfaceKind(player->objectNode, player->surface);
            sendObjectEvent(player, player->objectNode, kLand, data);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(groundEnter, Verified);

    // airEnter, Air's enter: the quarter pipe's heights, the turn rates
    // and the air time cleared. From the ground, a stand or anything but the
    // air states: the jump sound. Within 10 frames of an ollie (Player
    // +0x79) and facing upward: a launch, when what the skater left (the
    // rail ground, or the surface; not a horizontal pole or a quarter
    // pipe's lip) has launch values and the speed is within them: the
    // velocity gets the launch speed. Unless leaving a grind, double grind,
    // stall or wall ride: the object left hears "Jump" with its surface
    // kind, and the goals hear of it by the owning object's name. Returns 1.
    int32_t airEnter(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> data;

        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        // The pitch and turn rates (and the two words after them).
        zge::quadwordOf(ee::Ptr<ee::Float>(&player->pitchRate)) = zge::kZero;

        const uint8_t from = player->previousState;
        if (from < uint8_t(State::Air) || from > uint8_t(State::AirSpine))
        {
            const int32_t sound = surfaceSound(player);
            const int32_t volume = soundVolume(player);
            aggressive::soundEvent(kJumpSound, &player->position, sound, volume, 0);
        }

        player->stateDistance = 0.0f;
        if (player->settleFrames < 10 && 0.0f < zge::dot(player->axes.forward, *g_worldUp))
        {
            ee::Float least = 0.0f;
            ee::Float most = 0.0f;
            ee::Float launch = 0.0f;
            if (player->previousState == uint8_t(State::Grind))
            {
                if (railEdgeKind(player->grindRail, player->grindSegment) != kHorizontalPoleEdge)
                {
                    least = railLaunchMin(player->grindRail, player->grindSegment);
                    most = railLaunchMax(player->grindRail, player->grindSegment);
                    launch = railLaunchSpeed(player->grindRail, player->grindSegment);
                }
            }
            else if (player->object != 0 && surfacePhysics(player->objectNode, player->surface) != kQuarterPipeLip)
            {
                least = surfaceGravity(player->objectNode, player->surface);
                most = surfaceLaunchCap(player->objectNode, player->surface);
                launch = surfaceLaunchPush(player->objectNode, player->surface);
            }
            const ee::Float speed = player->speed;
            if (least < speed && (most == 0.0f || speed < most) && launch != 0.0f)
            {
                Vec4 &velocity = player->velocity;
                velocity = zge::unit(velocity);
                velocity = zge::scaledAfterMeasure(velocity, launch);
            }
        }

        // Grinds, stalls and wall rides' exits send their own events.
        const State left = State(player->previousState);
        const bool ownEvents = left == State::Grind || left == State::DoubleGrind || left == State::Stall || left == State::WallRide;
        if (!ownEvents && player->object != 0 && player->surface != kNoSurface)
        {
            *data = surfaceKind(player->objectNode, player->surface);
            sendObjectEvent(player, player->objectNode, kJump, data);
            if (const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(ee::Ptr<zge::Node>(player->object), 0))
            {
                const uint32_t hash = zge::objectNameHash(owner);
                aggressive::level::goalJump(hash, *data);
            }
        }
        player->heightAboveGround = 0.0f;

        player->overObject = 0;
        player->overSurfaceKind = 0;
        player->flags &= ~1u;
        return 1;
    }
    READABLE_FUNCTION_WAITS(airEnter, Verified);

    // grindEnter, Grind's enter: off the object (none, no surface), the
    // speed taken anew; moving against the facing turns the skater to face
    // the motion upright (fakie flips, the drawn orientation snapped); the
    // side the rail point is on (side 0: to the skater's right); onto the
    // rail (joinNextRail; a rail it cannot take leaves the skater in the
    // Air: returns 0). Then the balance: its drift (the stat plus Grind
    // Balance Short Init for each short grind before, leaning the way the
    // balance leans, or by the speed's parity), a lean past Grind Balance
    // Best Reset eased back by Grind Balance Reset (unless spinning with
    // Spin Reset on), the HUD's grind meter, Grind Balance Delay before it
    // moves (not while spinning with Spin Dead on; none after more than
    // Short Dead short grinds), and the camera's motion blur back to Normal
    // Blur. Returns 1.
    int32_t grindEnter(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> toRail;
        ee::Local<Vec4> heading;
        PlayerAxes &axes = player->axes;

        player->railEvent = 0;
        player->object = 0;
        player->lastRail = {};
        player->onMovingSurface = 0;
        player->carrier.object = {};
        player->surface = kNoSurface;
        player->previousSpeed = player->speed;
        player->speed = zge::vec3Length(&player->velocity.xyz());

        if (!(ee::abs(player->speed) < *g_stillSpeed))
        {
            *heading = zge::unit(player->velocity);
            if (zge::dot(*heading, axes.forward) < 0.0f)
            {
                // Facing the motion, upright: right = up x forward, then
                // forward = right x world up.
                axes.forward = *heading;
                orthonormalize(&axes.right, &axes.up, &axes.forward);
                axes.up = *g_worldUp;
                orthonormalize(&axes.forward, &axes.right, &axes.up);
                player->drawnRefresh = 1;
                player->fakie = player->fakie == 0 ? 1 : 0;
                player->drawFlags |= kDrawnSnap;
            }
        }

        *toRail = player->railCameFrom - player->position;
        player->side = 0.0f < zge::dot(*toRail, axes.right) ? 0 : 1;
        joinNextRail(player);
        if (player->state == State::Air)
            return 0;

        // The balance's drift and start.
        const ee::Float stat = skaterStat(player, kStatGrind, kStatGrindInit);
        const ee::Float drift = stat + grindWalls(kGrindBalanceShortInit) * ee::toFloat(player->shortCount);
        const ee::Float lean = player->balance[0];
        if (ee::abs(lean) < *g_balanceDead)
            player->balanceDrift = (ee::toInt(player->speed) & 1) ? drift : -drift;
        else
            player->balanceDrift = 0.0f < lean ? drift : -drift;

        const bool spinKeeps = (player->controls.actions & kActionSpin) && ee::toInt(grindWalls(kGrindBalanceSpinReset)) != 0;
        if (!spinKeeps && grindWalls(kGrindBalanceBestReset) < ee::abs(lean))
        {
            const ee::Float eased = lean * grindWalls(kGrindBalanceReset);
            player->balance[0] = eased;
            if (ee::abs(eased) < grindWalls(kGrindBalanceBestReset))
                player->balance[0] = eased < 0.0f ? -grindWalls(kGrindBalanceBestReset) : grindWalls(kGrindBalanceBestReset);
        }
        player->balance[0] = player->balance[0] + player->balanceDrift;
        aggressive::hud::showGrindMeter(player);

        const bool spinStill = (player->controls.actions & kActionSpin) && ee::toInt(grindWalls(kGrindBalanceSpinDead)) != 0;
        if (!spinStill)
            player->stateValue = grindWalls(kGrindBalanceDelay);
        if (grindWalls(kGrindBalanceShortDead) < unsignedToFloat(static_cast<uint32_t>(player->shortCount)))
            player->stateValue = 0.0f;
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;

        const int32_t blur = ee::toInt(tuning(2, kCameraNormalBlur)) & 0xFF;
        aggressive::setCameraBlur(&(*g_followCameras)[player->number], blur, ee::Float(1.0f), ee::Float(1.0f));
        return 1;
    }
    READABLE_FUNCTION_WAITS(grindEnter, Verified);
}
