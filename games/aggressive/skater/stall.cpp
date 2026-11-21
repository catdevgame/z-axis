// Holding on to an edge: the Stall state (8, a hand plant on a quarter
// pipe's lip) and the Skitch state (17, holding on to a vehicle) share the
// enter and exit callbacks and the fourth update; the stall's fifth update
// (the jump off and the balance input) is its own.
//
// The edge held is the block at Player +0x210 (as a grind's rail, grind.cpp
// Rail): its object and surface, its ends (+0x220, +0x230), its direction
// (+0x240) and the point the skater holds (+0x250). The enter turns the
// edge to run along the skater's right, stands the skater upright on the
// point and stops it; the object's script hears "StallStart" or
// "SkitchStart".
//
// A stall balances like a grind: Player::balance[1] drifts away from the
// middle faster the longer the stall lasts (Grind/Walls / Stall Balance
// Factor, from Stall Balance Delay on), the stick pushes it back (Stall
// Balance Adjust), and the skater leans with it about the edge (Grind Lean
// Angle). Past 1 the stall is lost: a crash one way, a fall back into the
// pipe (Air QPipe) the other. Jumping leaves it to the side the stick
// points, back into the pipe without the stick.

#include "moves.h"
#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "aggressive/hud/hud.h"
#include "aggressive/game_start.h"

#include "sym/aggressive/skater/stall.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCrashed = 3;          // skaterCrash's kind: "You crashed!"
        constexpr uint32_t kLevelRecordStall = 2; // the level record the stall's time goes to (inferred)

        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        inline ee::Global<ee::Float, sym::g_degreesToAngle7> g_degreesToAngle; // 65536 / 360
        inline ee::Global<ee::Float, sym::g_squareUpLean> g_squareUpLean;  // 0.99: right this close to the new up is re-squared first
        inline ee::Global<ee::Float, sym::g_squareUpJump> g_squareUpJump;  // 0.99, the same for the jump back into the pipe
        inline ee::Global<ee::Float, sym::g_stillSpeed6> g_stillSpeed;    // 1e-5: slower than this the skater keeps its facing

        // Grind/Walls tuning (DEVTOOLS.md).
        ee::Float grindWalls(uint32_t entry)
        {
            return (*g_tuningCategories)[14].entries[entry].value;
        }
        enum GrindWallsEntry : uint32_t
        {
            kStallBalanceAdjust = 15,
            kStallBalanceDelay = 17,
            kGrindLeanAngle = 26,
            kSideJumpSpeed = 45,
        };

        // The edge held (Player::rail, as grind.cpp's).
        using Edge = RailRecord;

        // The edge turned round: its direction negated, its ends swapped.
        void turnEdgeRound(Edge &edge, Vec4 &scratch)
        {
            vec3Negate(edge.direction, edge.direction);
            scratch = edge.start;
            edge.start = edge.end;
            edge.end = scratch;
        }

        // Up becomes the ground normal (all four words), and the other axes
        // are made square to it; when right was nearly along the new up, it
        // is squared to up first and up set again.
        void standOnGroundNormal(Player &player, ee::Float nearlyAlong)
        {
            PlayerAxes &axes = player.axes;
            axes.up = player.groundNormal;
            if (nearlyAlong < ee::abs(zge::dot(axes.right, axes.up)))
            {
                orthonormalize(&axes.right, &axes.up, &axes.forward);
                axes.up = player.groundNormal;
            }
            orthonormalize(&axes.forward, &axes.right, &axes.up);
        }
    }

    // holdEnter, the Stall and Skitch states' enter: the edge is turned
    // to run along the skater's right and becomes its right axis, the
    // skater stands upright (up the world's up) on the hold point and
    // stops; the balance starts drifting (the stat's first drift) from half
    // of where it was. A stall shows the balance meter, keeps the pipe it
    // began on, rides on the edge's object and tells its script
    // "StallStart"; a skitch tells the vehicle "SkitchStart". The pole grab
    // input is used up. Returns 1.
    int32_t holdEnter(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> scratch;
        ee::Local<uint32_t> eventData;
        PlayerAxes &axes = player->axes;
        Edge &edge = player->rail;

        player->groundNormal = axes.up;
        if (zge::dot(edge.direction, axes.right) < 0.0f)
            turnEdgeRound(edge, *scratch);

        player->airTime = 0.0f;
        player->controls.stallTime = 0.0f;
        player->onMovingSurface = 0;
        player->carrier.object = {};
        zge::matrixIdentity(&player->railLean);

        axes.right = zge::unit(edge.direction);
        orthonormalize(&axes.up, &axes.forward, &axes.right);
        axes.up = zge::unit(*g_worldUp);
        orthonormalize(&axes.forward, &axes.right, &axes.up);

        player->position = edge.joinPoint;
        player->velocity = zge::kZero;
        player->speed = 0.0f;
        const ee::Float drift = skaterStat(player, kStatGrind, kStatStallInit);
        player->balanceSpeed = -drift;
        player->balance[1] = player->balance[1] / 2.0f;

        if (player->state == State::Stall)
        {
            aggressive::hud::showBalanceMeter(player);
            aggressive::hud::setBalance(player, player->balance[1]);
            // The stall's time (the level record) from 0, Stall Balance
            // Delay before the balance drifts.
            player->stateDistance = 0.0f;
            player->stateValue = grindWalls(kStallBalanceDelay);
            player->pipeObject = player->objectNode;
            player->pipeSurface = player->surface;
            player->railEvent = railEventWord(edge.node, edge.surface);
            player->lastObject = player->object;
            player->surface = 0xFFFFFFFFu;
            player->object = edge.object;
            *eventData = player->railEvent;
            sendObjectEvent(player, edge.node, sym::stallStart, eventData);
        }
        else
        {
            *eventData = railEventWord(edge.node, edge.surface);
            sendObjectEvent(player, edge.node, sym::skitchStart, eventData);
        }
        player->controls.actions &= ~kActionPoleGrab;
        return 1;
    }
    // Waiting: the event goes to a script synchronously, and a handler can
    // wait (as the trigger zones' events, zones.cpp). So for the exit.
    READABLE_FUNCTION_WAITS(holdEnter, Verified);

    // holdUpdate, the Stall and Skitch states' fourth callback.
    //   - The time held; a stall's balance drifts (after Stall Balance
    //     Delay) away from the middle, faster the longer it lasts.
    //   - The ride's effects; on a moving object a stall moves with it, a
    //     skitch holds its point and faces the vehicle's way.
    //   - A stall shows the balance and leans by it about the edge (Grind
    //     Lean Angle at full balance). Past 1 it is lost: a quarter of the
    //     jump up, and then a crash (the balance below 0) or a fall back
    //     into the pipe (Air QPipe), standing on the ground normal again;
    //     either way half of Side Jump Speed out to the edge's side.
    void holdUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;
        ee::Local<Vec4> side;
        ee::Local<zge::Quaternion> leanTurn;
        PlayerAxes &axes = player->axes;
        Edge &edge = player->rail;
        Vec4 &velocity = player->velocity;

        player->holdTime = player->holdTime + frameTime;
        if (player->state == State::Stall)
        {
            player->stateDistance = player->stateDistance + frameTime;
            if (0.0f < player->stateValue)
                player->stateValue = player->stateValue - frameTime;
            else
            {
                ee::Float &drift = player->balanceSpeed;
                const bool rising = 0.0f < drift;
                const ee::Float growth = skaterStat(player, kStatGrind, kStatStallFactor);
                if (rising)
                    drift = drift + player->holdTime * growth;
                else
                    drift = drift - player->holdTime * growth;
                player->balance[1] = player->balance[1] + drift;
            }
        }

        carryRider(player, frameTime);
        if (player->onMovingSurface)
        {
            if (player->state == State::Stall)
                player->position = player->position + player->carriedMove;
            else
            {
                player->position = edge.joinPoint;
                axes.forward = zge::unit(player->carriedMove);
                orthonormalize(&axes.right, &axes.up, &axes.forward);
                axes.up = *g_worldUp;
                orthonormalize(&axes.forward, &axes.right, &axes.up);
            }
        }
        if (player->state != State::Stall)
            return;

        aggressive::hud::setBalance(player, player->balance[1]);
        // Up turned about the edge by the lean.
        const ee::Float degrees = grindWalls(kGrindLeanAngle) * player->balance[1];
        const zge::Angle angle = static_cast<zge::Angle>(ee::toInt(degrees * *g_degreesToAngle));
        zge::quaternionFromAxisAngle(leanTurn, xyz(edge.direction), angle);
        zge::quaternionToMatrix(&player->railLean, leanTurn);
        axes.up = zge::rotated(player->railLean, *g_worldUp);
        orthonormalize(&axes.forward, &axes.right, &axes.up);

        if (!(1.0f < ee::abs(player->balance[1])))
            return;

        // The balance is lost: a quarter of the jump up, and the way out to
        // the side of the edge (up x edge).
        *push = *g_worldUp * (skaterStat(player, kStatJump, 0) * 0.25f);
        velocity = velocity + *push;
        *side = zge::cross(*g_worldUp, edge.direction);
        *side = zge::unit(*side);
        if (player->balance[1] < 0.0f)
            skaterCrash(player, kCrashed);
        else
        {
            // Back into the pipe: its lip found again, the skater on the
            // ground normal, the edge run along the skater's right.
            pipeEdgeOf(player->pipeObject, player->pipeSurface, &edge);
            *push = zge::cross(*g_worldUp, edge.direction);
            standOnGroundNormal(*player, *g_squareUpLean);
            if (zge::dot(*push, axes.right) < 0.0f)
                turnEdgeRound(edge, *push);
            changeState(player, State::AirQuarterPipe);
        }
        *push = *side * (grindWalls(kSideJumpSpeed) * 0.5f);
        velocity = velocity + *push;
        newSpeed(*player);
    }
    READABLE_FUNCTION(holdUpdate, Verified);

    // stallControls, the Stall state's fifth callback (not while down): the
    // jump off and the balance input. A jump stops the skater and sends it
    // up with its jump speed and out by Side Jump Speed: the stick pulled
    // back one way across the edge, pushed forward the other, left or right
    // along the edge, all into the Air; without the stick half as far, back
    // into the pipe (Air QPipe, standing on the ground normal, the edge run
    // along the skater's right). Moving, it faces the way it goes. After
    // Stall Balance Delay the stick pushes the balance's drift (Stall
    // Balance Adjust).
    void stallControls(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;
        ee::Local<Vec4> side;
        PlayerAxes &axes = player->axes;
        Edge &edge = player->rail;
        Vec4 &velocity = player->velocity;

        if (player->controls.actions & kActionJump)
        {
            velocity = zge::kZero;
            *push = *g_worldUp * skaterStat(player, kStatJump, 0);
            velocity = velocity + *push;
            player->settleFrames = 0;

            // Out across the edge (up x edge), along it, or back into the
            // pipe.
            const ee::Float stickY = player->controls.stickY;
            State next = State::Air;
            const auto acrossEdge = [&]
            {
                *side = zge::cross(*g_worldUp, edge.direction);
                *side = zge::unit(*side);
            };
            if (0.0f < stickY)
            {
                acrossEdge();
                vec3Negate(*side, *side);
                *push = *side * grindWalls(kSideJumpSpeed);
            }
            else if (stickY < 0.0f)
            {
                acrossEdge();
                *push = *side * grindWalls(kSideJumpSpeed);
            }
            else if (0.0f < player->controls.stickX)
            {
                *side = zge::unit(edge.direction);
                *push = *side * grindWalls(kSideJumpSpeed);
            }
            else if (player->controls.stickX < 0.0f)
            {
                *side = zge::unit(edge.direction);
                vec3Negate(*side, *side);
                *push = *side * grindWalls(kSideJumpSpeed);
            }
            else
            {
                acrossEdge();
                vec3Negate(*side, *side);
                *push = *side * (grindWalls(kSideJumpSpeed) * 0.5f);
                velocity = velocity + *push;
                next = State::AirQuarterPipe;
                pipeEdgeOf(player->pipeObject, player->pipeSurface, &edge);
                *push = zge::cross(*g_worldUp, edge.direction);
                standOnGroundNormal(*player, *g_squareUpJump);
                if (zge::dot(*push, axes.right) < 0.0f)
                    turnEdgeRound(edge, *push);
            }
            if (next == State::Air)
                velocity = velocity + *push;

            newSpeed(*player);
            // Off the edge unless back into the pipe.
            const State leaving = player->state;
            if (leaving != State::Stall || next == State::Air)
            {
                edge.start = zge::kZero;
                edge.end = zge::kZero;
            }
            changeState(player, leaving == State::Stall ? next : State::Air);
            if (!(ee::abs(player->speed) < *g_stillSpeed))
            {
                axes.forward = zge::unit(velocity);
                orthonormalize(&axes.right, &axes.up, &axes.forward);
            }
        }

        if (player->stateValue <= 0.0f)
        {
            const ee::Float stickY = player->controls.stickY;
            if (stickY == 0.0f)
                return;
            player->balanceSpeed = player->balanceSpeed + frameTime * stickY * grindWalls(kStallBalanceAdjust);
        }
    }
    READABLE_FUNCTION(stallControls, Verified);

    // holdExit, the Stall and Skitch states' exit: off any moving
    // object. A stall hides the balance meter, stops the turn, tells the
    // edge's object (and the player's script) "StallEnd" with the edge's
    // tag and the time held, goes back to the object it stood on, and hands
    // the stall's time to the level record. A skitch lets go with half the
    // vehicle's speed, a unit back from it (against the way it goes) and a
    // unit down along up.
    void holdExit(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> push;
        ee::Local<GrindEndData> eventData;
        PlayerAxes &axes = player->axes;

        if (player->onMovingSurface)
        {
            player->onMovingSurface = 0;
            player->carrier.object = {};
        }
        if (player->state == State::Stall)
        {
            aggressive::hud::hideBalanceMeter(player);
            player->noSpinTurn = 0;
            // The pitch and turn rates (and the two words after them).
            zge::quadwordOf(ee::Ptr<ee::Float>(&player->pitchRate)) = zge::kZero;
            if (const ee::Ptr<zge::Node> object = player->objectNode)
            {
                eventData->railEvent = player->railEvent;
                eventData->distance = player->holdTime;
                sendObjectEvent(player, object, sym::stallEnd, eventData);
                if (const uint32_t script = player->script)
                    sendScriptEvent(player, script, sym::stallEnd, eventData);
            }
            player->object = player->lastObject;
            player->surface = 0xFFFFFFFFu;
            aggressive::updateLevelRecord(kLevelRecordStall, *g_levelWord, player->name, player->stateDistance);
            player->stateDistance = 0.0f;
        }
        else
        {
            if (player->state != State::Skitch)
                return;
            // The vehicle's move per second, half of it the skater's.
            Vec4 &velocity = player->velocity;
            const ee::Float one = 1.0f;
            player->carriedMove = player->carriedMove * (one / player->carrier.frameTime);
            velocity = player->carriedMove;
            velocity = velocity * 0.5f;
            const bool backward = zge::dot(axes.forward, velocity) < 0.0f;
            *push = axes.forward * (backward ? one : ee::Float(-1.0f));
            player->position = player->position + *push;
            *push = axes.up * ee::Float(-1.0f);
            player->position = player->position + *push;
        }
    }
    READABLE_FUNCTION_WAITS(holdExit, Verified);

    namespace
    {
        ee::Float vec3Length(ee::Ptr<const zge::Vec3> x0) { return zge::vec3Length(x0); }
    }

    // measureAwayFromHangPoint: whether the skater heads away from the point
    // it hangs from (hangFrom; a pole's): then substate 1, the swing back to
    // 0, the distance to the point (stateDistance) and the angle round it
    // in degrees from -180 (stateValue), and the angle (an engine angle) is
    // returned; else substate 2, and 2. Nothing calls it.
    int32_t measureAwayFromHangPoint(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> toHang;
        ee::Local<zge::Vec4> heading;
        *toHang = player->hangFrom - player->position;
        const ee::Float distance = vec3Length(ee::Ptr<const zge::Vec3>(toHang.address()));
        *heading = zge::unit(player->velocity);
        if (!(zge::dot(*toHang, *heading) < ee::Float(0.0f)))
        {
            player->substate = 2;
            return 2;
        }
        player->substate = 1;
        player->swingMade = ee::Float(0.0f);
        const zge::Angle angle = zge::angleAtan2(toHang->z, toHang->x);
        player->stateDistance = distance;
        player->stateValue = ee::toFloat(angle) * zge::kAngleToDegrees + ee::Float(180.0f);
        return angle;
    }
    READABLE_FUNCTION(measureAwayFromHangPoint, Verified);

}
