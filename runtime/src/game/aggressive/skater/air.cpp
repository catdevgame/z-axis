// The air states' per-frame callbacks (skater.h StateRow::update): Air,
// Air QPipe (up a quarter pipe) and Air Spine (over a spine) share most of
// them.
//
//   Air        airUpright, airGravity, airMove, -, airActions
//   Air QPipe  -, airGravity, airQuarterPipeMove, airQuarterPipeUpright, airActions
//   Air Spine  airSpineTurn, airGravity, airMove, -, airActions
//
// The game's y axis points down: gravity adds to y, and the skater's up
// vector on flat ground is g_worldUp (0, -1, 0).
//
// Like the Ground physics (ground.cpp) the vector code here stores vectors
// whose w it never set: a scaled vector's w is what the last vector
// operation left. Those lanes are followed as the registers carry them.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/vector.h"
#include "aggressive/game_start.h"
#include "sym/aggressive/skater/air.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Game functions the air states call.
        constexpr uint32_t kTurnInAir = sym::turnInAir;     // (player, frame time) turns by Player +0x154, counts the spin
        constexpr uint32_t kSetSkaterState = sym::setSkaterState; // (player, state)

        // The world's up (0, -1, 0, 0).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // Up closer to the world's than this (a cosine, 0.99) is upright.
        inline ee::Global<ee::Float, sym::g_uprightDot> g_uprightDot;
        // Speeds below this (1e-5) leave the skater's heading alone.
        inline ee::Global<ee::Float, sym::g_minHeadingSpeed> g_minHeadingSpeed;
        // The closest the skater comes to a quarter pipe's plane (0.05).
        inline ee::Global<ee::Float, sym::g_minPipeOffset> g_minPipeOffset;

        // Controls::actions bits the air reads.
        constexpr uint32_t kActionSpinLeft = kActionButton100;
        constexpr uint32_t kActionSpinRight = kActionButton200;

        // Player::flags
        constexpr uint32_t kAcidDrop = 0x1; // in the air longer than Acid Drop Time

        // kSkaterStat's stats.

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        constexpr uint32_t kPhysics = 3;
        constexpr uint32_t kGrindWalls = 14;
        enum PhysicsEntry : uint32_t
        {
            kGravity = 0,
            kStepHeight = 8,
            kMinGravityPercent = 18,
            kMaxGravitySpeed = 19,
            kQPipeGravity = 31,
            kSpineGravity = 32,
            kAcidDropTime = 36,
        };
        enum GrindWallsEntry : uint32_t
        {
            kWallCheckRadius = 35,
            kMinWallAngle = 36,
            kSideJumpSpeed = 45,
        };

        // kSurfaceKind's kinds.
        constexpr uint32_t kSurfaceQuarterPipe = 1;
        constexpr uint32_t kSurfaceSpine = 2;

        // Player fields the air states use (not in skater.h yet).
        constexpr uint32_t kNoSpinTurn = 0x078;         // uint8_t, over a spine: no turn without a spin button
        constexpr uint32_t kLastPosition = 0x0F0;       // Vec4, last frame's position, velocity and orientation
        constexpr uint32_t kLastVelocity = 0x100;
        constexpr uint32_t kLastOrientation = 0x110;    // Matrix
        constexpr uint32_t kSpineSpeed = 0x150;         // float, degrees/s about the spine
        // float, the state's own: Air's late-jump window, Air QPipe's offset
        // from the pipe's plane (Manual: Manual Balance Delay left).
        constexpr uint32_t kAirTime = offsetof(Player, stateAirTime);          // float, zeroed by the air states' enter
        constexpr uint32_t kTotalAirTime = offsetof(Player, totalAirTime);       // float, all the air since the skater reset (resetSkaterPhysics)
    }

    int32_t turnInAir(ee::Ptr<Player> player, ee::Float frameTime);

    // airUpright, Air's first callback: the Ground state's steering, then
    // the skater rights itself (up turned toward the world's up at 5 a
    // second while it is off by more than g_uprightDot), then the turn and
    // spin in the air.
    void airUpright(ee::Ptr<Player> player, ee::Float frameTime)
    {
        groundTurn(player, frameTime);
        PlayerAxes &axes = player->axes;
        if (zge::dot(axes.up, *g_worldUp) < *g_uprightDot)
        {
            const Vec4 toUpright = *g_worldUp - axes.up;
            const Vec4 step = toUpright * (frameTime * 5.0f);
            axes.up = axes.up + step;
            axes.up = zge::unit(axes.up);
            orthonormalize(&axes.forward, &axes.right, &axes.up);
        }
        turnInAir(player, frameTime);
    }
    READABLE_FUNCTION(airUpright, Verified);

    // airGravity, the air states' second callback: the air timers, then
    // gravity (Gravity, QPipe Gravity or Spine Gravity), weaker near the top
    // of a jump: below Max Gravity Speed vertically it falls off toward Min
    // Gravity %.
    void airGravity(ee::Ptr<Player> player, ee::Float frameTime)
    {
        player->totalAirTime = player->totalAirTime + frameTime;
        player->stateAirTime = player->stateAirTime + frameTime;

        const Vec4 down = {0.0f, 1.0f, 0.0f, 1.0f};
        ee::Float gravity;
        if (player->state == State::AirQuarterPipe)
            gravity = tuning(kPhysics, kQPipeGravity);
        else if (player->state == State::AirSpine)
            gravity = tuning(kPhysics, kSpineGravity);
        else
            gravity = tuning(kPhysics, kGravity);
        ee::Float pull = gravity * frameTime;

        // Gravity fades towards Min Gravity Percent as the fall nears Max
        // Gravity Speed.
        const ee::Float vertical = ee::abs(player->velocity.y);
        const ee::Float maxGravitySpeed = tuning(kPhysics, kMaxGravitySpeed);
        ee::Float scale = 1.0f;
        if (vertical < maxGravitySpeed)
            scale = 1.0f - (vertical / maxGravitySpeed) * (1.0f - tuning(kPhysics, kMinGravityPercent));
        pull = pull * scale;

        const Vec4 fall = down * pull;
        player->velocity = player->velocity + fall;
        newSpeed(*player);
    }
    READABLE_FUNCTION(airGravity, Verified);

    // airMove, Air's and Air Spine's third callback: the move. Last
    // frame's position, velocity and orientation are kept, the landing check
    // runs, the skater moves by its velocity; a hit this frame (less free
    // time than a frame) lands it. Air longer than Acid Drop Time marks an
    // acid drop.
    void airMove(ee::Ptr<Player> player, ee::Float frameTime)
    {
        // (The original looks up the skater's scene object and never uses it.)
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        zge::sceneObject(scene);

        player->lastPosition = player->position;
        player->lastVelocity = player->velocity;
        player->lastOrientation = player->orientation;

        skaterMoveCollide(player, frameTime, 1);
        moveByVelocity(player, frameTime);
        const ee::Float &freeTime = player->airTime; // moved without a hit
        if (freeTime < frameTime)
            changeState(player, State::Ground);
        groundBelow(player);
        if (tuning(kPhysics, kAcidDropTime) < freeTime && player->state == State::Air)
            player->flags |= kAcidDrop;
    }
    READABLE_FUNCTION(airMove, Verified);

    // airQuarterPipeUpright, Air QPipe's fourth callback: up a quarter pipe the
    // skater's up lies across the pipe (the world's up crossed with the
    // pipe's edge), on the side it was; then the turn and spin in the air.
    void airQuarterPipeUpright(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> across;
        PlayerAxes &axes = player->axes;
        *across = zge::cross(*g_worldUp, player->railDirection);
        if (zge::dot(axes.up, across) < 0.0f)
            vec3Negate(across, across);
        axes.up = *across;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        turnInAir(player, frameTime);
    }
    READABLE_FUNCTION(airQuarterPipeUpright, Verified);

    // airSpineTurn, Air Spine's first callback: the turn about the skater's
    // up (Player::turnRate), the roll over the spine's edge until 170
    // degrees are done, the turn and spin in the air; a skater left with no
    // forward axis is set upright facing x.
    void airSpineTurn(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> down;
        PlayerAxes &axes = player->axes;
        zge::Matrix &rows = player->orientation;
        ee::Float &spineSpeed = player->spineRollRate;
        if (player->turnRate != 0.0f)
        {
            vec3Negate(down, axes.up);
            rotateOrientation(&rows, &down->xyz(), player->turnRate * frameTime);
        }
        if (spineSpeed != 0.0f)
        {
            rotateOrientation(&rows, &player->railDirection.xyz(), -(spineSpeed * frameTime));
            ee::Float &spineAngle = player->spineRolled;
            spineAngle = spineAngle + spineSpeed * frameTime;
            if (170.0f < spineAngle)
                spineSpeed = 0.0f;
        }
        turnInAir(player, frameTime);

        // No forward axis at all: start again from upright, facing +x.
        Vec4 &forward = axes.forward;
        if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f)
        {
            axes.up = *g_worldUp;
            forward.x = 1.0f;
            forward.y = 0.0f;
            forward.z = 0.0f;
            orthonormalize(&axes.right, &axes.up, &forward);
        }
    }
    READABLE_FUNCTION(airSpineTurn, Verified);

    // airActions, the air states' fifth callback (not while down): the
    // buttons in the air.
    // - Wall ride, next to a wall (Wall Check Radius, Min Wall Angle; not
    //   over a quarter pipe or a spine): a wall ride starts, the velocity's
    //   level part along the wall worked out on the way.
    // - Spin left or right: the turn speed is the skater's spin stat.
    // - Jump in Air while the late-jump window (Player +0x264, set when the
    //   skater left the ground) is open: the skater's jump speed straight up,
    //   plus Side Jump Speed off a quarter pipe's edge when the stick is
    //   pushed; a jump steep enough turns the skater's heading to the motion.
    void airActions(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> vertical;
        ee::Local<Vec4> level;
        ee::Local<Vec4> kick;
        ee::Local<Vec4> side;
        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        Controls &controls = player->controls;

        const State state = player->state;
        const bool overPipe = state == State::AirQuarterPipe || state == State::AirSpine;
        if (!overPipe && (controls.actions & kActionWallRide) && freeToAct(player))
        {
            WallContact &wall = player->wallContact;
            if (findWall(player, &wall, tuning(kGrindWalls, kWallCheckRadius)))
            {
                *vertical = *g_worldUp * zge::dot(velocity, *g_worldUp);
                *level = velocity - *vertical;
                alongPlane(level, &wall.normal, level);
                vec3Length(*level); // worked out and not used
                changeState(player, State::WallRide);
            }
        }

        const uint32_t actions = controls.actions;
        if (actions & (kActionSpinLeft | kActionSpinRight))
        {
            const ee::Float spin = skaterStat(player, kStatSpin, 0);
            player->turnRate = (actions & kActionSpinLeft) ? -spin : spin;
            if (player->state != State::AirSpine)
                player->noSpinTurn = 0;
        }
        else if (!player->noSpinTurn || player->state == State::AirSpine)
        {
            player->turnRate = 0.0f; // no spin: no turn (kept only by a no-spin turn outside a spine)
        }

        ee::Float &lateJumpTime = player->lateJumpTime;
        if (0.0f < lateJumpTime)
        {
            lateJumpTime = lateJumpTime - frameTime;
            if (lateJumpTime < 0.0f)
                lateJumpTime = 0.0f;
        }
        if (player->state != State::Air)
            return;
        if (!(controls.actions & kActionJump) || !(0.0f < lateJumpTime))
            return;

        // The late jump.
        player->settleFrames = 0;
        velocity.y = 0.0f;
        lateJumpTime = 0.0f;
        *kick = *g_worldUp * skaterStat(player, kStatJump, 0);
        velocity = velocity + *kick;

        // Off a quarter pipe's edge with the stick pushed: sideways too, across
        // the edge, away from the side pushed.
        const ee::Float stick = controls.stickX;
        if (0.0f < stick || stick < 0.0f)
        {
            *side = zge::unit(zge::cross(*g_worldUp, player->railDirection));
            if (0.0f < stick)
                vec3Negate(*side, *side);
            *kick = *side * tuning(kGrindWalls, kSideJumpSpeed);
            velocity = velocity + *kick;
        }

        newSpeed(*player);
        if (ee::abs(player->speed) < *g_minHeadingSpeed)
            return;
        // Moving along up by 1 or more: the heading follows the motion.
        if (1.0f <= ee::abs(zge::dot(axes.up, velocity)))
        {
            alongPlane(&axes.forward, &axes.up, &velocity);
            axes.forward = zge::unit(axes.forward);
            orthonormalize(&axes.right, &axes.up, &axes.forward);
        }
    }
    READABLE_FUNCTION(airActions, Verified);

    // airQuarterPipeMove, Air QPipe's third callback: the move over a quarter
    // pipe. The skater is held in the pipe's plane above its edge
    // (kHoldOverPipe); a sweep from a Step Height above it down past the
    // pipe's face finds the face below. A new face of the right kind (a
    // quarter pipe's, or a spine's in Air Spine) becomes the edge followed:
    // its normal and edge are read, the edge kept pointing the way the last
    // one did, the skater's offset from the plane brought in (no closer than
    // g_minPipeOffset), and in Air QPipe the skater's up laid across the
    // pipe. The height over the face is kept (and its peak); the substate
    // says whether the skater is over the face. Then the air move, and the
    // ground check while the state is unchanged.
    void airQuarterPipeMove(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> plane;
        ee::Local<Vec4> across;
        ee::Local<Vec4> lastEdge;
        ee::Local<Vec4> offset;
        ee::Local<Vec4> sweepDown;
        ee::Local<Vec4> swap;
        ee::Local<zge::QuerySegment> sweep;
        ee::Local<zge::QueryResults> results;
        ee::Local<uint32_t[2]> skipped; // the nodes the query skips: the skater's own, then 0
        ee::Local<RailRecord> foundEdge;

        // The pipe's edge the skater follows: its ends and direction.
        RailRecord &edge = railRecordOf(*player);
        PlayerAxes &axes = player->axes;
        const State stateBefore = player->state;
        skipped[0] = ee::Address(zge::sceneObject(zge::sceneOf(player->node))); // the skip list's word
        skipped[1] = 0;
        holdOverPipe(player, across);

        // From a step above the skater, down past the face by its last height.
        *offset = *across * tuning(kPhysics, kStepHeight);
        sweep->start = *offset + player->position;
        *sweepDown = *g_worldUp * -(player->pipeHeight + 35.0f);
        sweep->end = *sweepDown + sweep->start;

        const uint32_t flags = uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed;
        bool overFace = false;
        if (zge::collisionQuery(kQuerySegment, sweep, flags, 0, 0, skipped, results, 0))
        {
            const ee::Ptr<zge::Node> object = hitObject(*results);
            const uint32_t face = hitFace(*results);
            const uint32_t kind = surfacePhysics(object, face);
            if ((player->state == State::AirQuarterPipe && kind == kSurfaceQuarterPipe) ||
                (player->state == State::AirSpine && kind == kSurfaceSpine))
            {
                overFace = true;
                bool newEdge = false;
                if (face != player->surface)
                {
                    pipeEdgeOf(object, face, foundEdge);
                    if (foundEdge->surface != player->lastPipeSegment || foundEdge->node != player->lastPipeObject)
                    {
                        newEdge = true;
                        if (foundEdge->surface != edge.surface || foundEdge->object != edge.object)
                        {
                            player->lastPipeSegment = edge.surface;
                            player->lastPipeObject = edge.node;
                        }
                    }
                }
                if (newEdge)
                {
                    *lastEdge = edge.direction;
                    hitNormal(results, &player->surfaceNormal);
                    player->surface = face;
                    player->objectNode = object;
                    pipeEdgeOf(object, face, &edge);

                    // The skater's offset from the plane through the edge,
                    // across the pipe on the skater's side, brought in.
                    *across = zge::cross(*g_worldUp, edge.direction);
                    if (zge::dot(axes.up, *across) < 0.0f)
                        vec3Negate(*across, *across);
                    zge::planeThrough(plane, &edge.start, across);
                    const ee::Float distance = zge::planeDistance(plane, &player->position) - 0.25f;
                    if (distance < player->pipeOffset)
                    {
                        player->pipeOffset = distance;
                        if (distance < *g_minPipeOffset)
                            player->pipeOffset = *g_minPipeOffset;
                    }

                    // The edge keeps the way the last one pointed (its ends swapped with it).
                    if (zge::dot(edge.direction, *lastEdge) < 0.0f)
                    {
                        vec3Negate(edge.direction, edge.direction);
                        *swap = edge.start;
                        edge.start = edge.end;
                        edge.end = *swap;
                    }
                    if (player->state == State::AirQuarterPipe)
                    {
                        *across = zge::cross(*g_worldUp, edge.direction);
                        if (zge::dot(axes.up, *across) < 0.0f)
                            vec3Negate(*across, *across);
                        axes.up = *across;
                        orthonormalize(&axes.forward, &axes.right, &axes.up);
                    }
                    holdOverPipe(player, across);
                }

                // The height over the face: from the sweep's start to the
                // point the face was hit at.
                *offset = results->nodes[0]->faces->point - sweep->start;
                player->pipeHeight = vec3Length(*offset);
                if (player->pipePeakHeight < player->pipeHeight)
                    player->pipePeakHeight = player->pipeHeight;
            }
        }
        player->substate = overFace ? 1 : 0;

        airMove(player, frameTime);
        if (player->state == stateBefore)
            groundBelow(player);
    }
    READABLE_FUNCTION(airQuarterPipeMove, Verified);
}

// ---- the air turn's spin count (was the sweep's turnInAir) -----------------
namespace aggressive::skater
{
    namespace
    {
        // (stat, level, value, skater name) a level record of whole numbers.
        constexpr uint32_t kStatMostSpin = 8; // the half turns of one jump (inferred: the spin record)
        constexpr uint32_t kTuningLandingAngle = 34; // Physics / Landing Angle (a cosine)
        inline constexpr ee::Float kAngleToDegrees = 0.005493164f; // 360 / 65536
        // No turn under this rate; the half turns per degree, each way.
        inline ee::Global<ee::Float, sym::g_minTurnRate> g_minTurnRate;
        inline ee::Global<ee::Float, sym::g_halfTurnsPerDegreeClockwise> g_halfTurnsPerDegreeClockwise;
        inline ee::Global<ee::Float, sym::g_halfTurnsPerDegreeCounterClockwise> g_halfTurnsPerDegreeCounterClockwise;
        // The level index read as a word (with the three bytes after it).
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelIndexWord;
    }

    // turnInAir: counts the air turn (Player::turnRate over the frame)
    // into the spin each way, in degrees, and every half turn passed into
    // the spin score (setClockwiseSpins / setCounterClockwiseSpins) and
    // the level's spin record. The landing angle counts toward the turn (a
    // half turn short by that much lands as one). Not over a spine with no
    // spin pressed, nor while down.
    int32_t turnInAir(ee::Ptr<Player> player, ee::Float frameTime)
    {
        int32_t result = 0;
        bool counted = true;
        if (player->state != State::AirSpine)
        {
            result = player->noSpinTurn;
            if (!result)
            {
                result = player->flags & kDown;
                if (result)
                    return result;
            }
            counted = result == 0;
        }
        if (counted)
        {
            const int32_t landing = static_cast<int32_t>(static_cast<short>(zge::angleAcos(tuningValue(kTuningPhysics, kTuningLandingAngle))));
            const ee::Float rate = player->turnRate;
            const ee::Float landingDegrees = ee::toFloat(landing) * kAngleToDegrees;
            result = ee::abs(rate) < *g_minTurnRate;
            if (!result)
            {
                if (ee::Float(0.0f) < rate)
                {
                    const ee::Float before = player->spinDegreesClockwise;
                    const ee::Float perDegree = *g_halfTurnsPerDegreeClockwise;
                    const ee::Float after = before + rate * frameTime;
                    player->spinDegreesClockwise = after;
                    const ee::Float halfTurns = (after + landingDegrees) * perDegree;
                    result = ee::toInt(halfTurns);
                    if (ee::toInt((before + landingDegrees) * perDegree) == result)
                        return result;
                    const int32_t passed = ee::toInt(halfTurns);
                    setClockwiseSpins(player, passed);
                    result = aggressive::recordBestScore(kStatMostSpin, *g_levelIndexWord, passed, player->name);
                }
                else
                {
                    const ee::Float before = player->spinDegreesCounterClockwise;
                    const ee::Float perDegree = *g_halfTurnsPerDegreeCounterClockwise;
                    const ee::Float after = before + -rate * frameTime;
                    player->spinDegreesCounterClockwise = after;
                    const ee::Float halfTurns = (after + landingDegrees) * perDegree;
                    result = ee::toInt(halfTurns);
                    if (ee::toInt((before + landingDegrees) * perDegree) != result)
                    {
                        const int32_t passed = ee::toInt(halfTurns);
                        setCounterClockwiseSpins(player, passed);
                        result = aggressive::recordBestScore(kStatMostSpin, *g_levelIndexWord, passed, player->name);
                    }
                }
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(turnInAir, Verified);
}
