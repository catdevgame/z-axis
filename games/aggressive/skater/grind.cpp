// The Grind and Hand Grind states' per-frame callbacks (states 5 and 6
// share them).
//
// The rail the skater is on is a segment the grind's enter callback keeps
// in the Player (RailRecord): its start and end points and the unit direction
// from start to end. The skater moves along it, pulled downhill by Grind Hill
// Gravity and slowed by Forward Friction, up to Max Grind Speed; past either
// end it goes on to the next segment or leaves the rail.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "aggressive/pads.h"
#include "aggressive/hud/hud.h"
#include "sym/aggressive/skater/grind.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCrashed = 3;     // skaterCrash's kind: "You crashed!"
        // What losing the balance does on a rail (railEdgeKind): off the side,
        // or just off; anything else is a crash.
        constexpr int32_t kEdgeOffTheSide = 1, kEdgeOffTheSide2 = 9, kEdgeOff = 2;

        inline ee::Global<ee::Float, sym::g_stillSpeed4> g_stillSpeed;  // 1e-5: slower than this the skater's heading is where it faces
        inline ee::Global<ee::Float, sym::g_squareToRail2> g_squareToRail; // 1e-5: a heading this close to square to the rail is a tie
        inline ee::Global<ee::Float, sym::g_jumpStill> g_jumpStill;    // 1e-5: a jump slower than this keeps the facing
        inline ee::Global<ee::Float, sym::g_steepRail2> g_steepRail;    // 0.97: a rail this close to vertical
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        ee::Float gravity() { return tuning(3, 0); }             // Physics / Gravity
        enum GrindWallsEntry : uint32_t
        {
            kMaxGrindSpeed = 2,
            kGrindBalanceAdjust = 4,
            kGrindBalanceShortTime = 11,
            kForwardFriction = 32,
            kGrindHillGravity = 47,
            kSideJumpSpeed = 45,
            kGrindExtensionTime = 48,
        };
        ee::Float grindWalls(uint32_t entry) { return tuning(14, entry); }
        // Vibrate / Grind Wood and Grind Metal: Big, Small, Time.
        enum VibrateEntry : uint32_t
        {
            kGrindWoodBig = 0,
            kGrindWoodSmall = 1,
            kGrindWoodTime = 2,
            kGrindMetalBig = 3,
            kGrindMetalSmall = 4,
            kGrindMetalTime = 5,
        };
        ee::Float vibrateTuning(uint32_t entry) { return tuning(18, entry); }

        // Rail materials (railMaterial).
        constexpr uint32_t kRailMetal = 0, kRailWood = 1, kRailMetal2 = 2, kRailMetal8 = 8;
    }

    // grindPhysics, the grind's second callback: the ride's effects, then
    // the speed along the rail: the rail turned to run the way the skater
    // goes, gravity's part along it (Gravity times Grind Hill Gravity), the
    // velocity kept on the rail, Forward Friction, and at most Max Grind
    // Speed.
    void grindPhysics(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> pull;
        ee::Local<Vec4> friction;
        ee::Local<Vec4> heading;

        Vec4 &velocity = player->velocity;
        const Vec4 &forward = player->axes.forward;
        RailRecord &rail = railRecordOf(*player);

        carryRider(player, frameTime);

        // Where the skater heads: along its velocity, or where it faces
        // when (nearly) still.
        if (ee::abs(player->speed) < *g_stillSpeed)
            *heading = forward;
        else
            *heading = zge::unit(velocity);

        // The rail runs the way the skater heads: turned round (start and end
        // swapped) when the skater heads back along it, or, heading square to
        // it, when the skater came from the end.
        const ee::Float along = zge::dot(rail.direction, *heading);
        bool turn = along < 0.0f;
        if (!turn && ee::abs(along) < *g_squareToRail)
        {
            const ee::Float epsilon = *g_squareToRail;
            turn = ee::abs(rail.joinPoint.x - rail.end.x) < epsilon && ee::abs(rail.joinPoint.y - rail.end.y) < epsilon &&
                   ee::abs(rail.joinPoint.z - rail.end.z) < epsilon;
        }
        if (turn)
        {
            vec3Negate(rail.direction, rail.direction);
            const Vec4 start = rail.start;
            rail.start = rail.end;
            rail.end = start;
        }

        // Gravity's part along the rail, times Grind Hill Gravity.
        *pull = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        *pull = *pull * (gravity() * frameTime);
        *pull = rail.direction * zge::dot(rail.direction, *pull);
        *pull = *pull * grindWalls(kGrindHillGravity);
        velocity = velocity + *pull;

        // Only the velocity along the rail is kept, less Forward Friction
        // (along where the skater faces).
        const ee::Float speedAlong = zge::dot(velocity, rail.direction);
        velocity = rail.direction * speedAlong;
        *friction = forward * (grindWalls(kForwardFriction) * speedAlong);
        velocity = velocity - *friction;
        newSpeed(*player);

        if (grindWalls(kMaxGrindSpeed) < player->speed)
        {
            player->speed = grindWalls(kMaxGrindSpeed);
            velocity = zge::unit(velocity);
            velocity = velocity * player->speed;
        }
    }
    READABLE_FUNCTION(grindPhysics, Verified);

    // grindMove, the grind's third callback: the frame's move. Last
    // frame's position, velocity and orientation are kept, the skater moves
    // by its velocity (and by the rail's own move, on a moving rail), the
    // distance ground adds up, and the collisions are checked; past an end
    // of the rail the skater goes on to the next one, or flies off it (Air)
    // with Grind Extension Time to get back on.
    void grindMove(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> step;
        Vec4 &position = player->position;
        const RailRecord &rail = railRecordOf(*player);

        player->lastPosition = position;
        player->lastVelocity = player->velocity;
        player->lastOrientation = player->orientation;

        *step = player->velocity * frameTime;
        position = position + *step;
        // A rail that moves takes the skater along by its own move this frame.
        if (player->onMovingSurface == 1)
            position = position + player->carriedMove;

        player->distance = player->distance + vec3Length(*step);
        player->stateDistance = player->stateDistance + vec3Length(*step);
        player->groundNormal = Vec4{0.0f, 0.0f, 0.0f, 1.0f};

        if (skaterMoveCollide(player, frameTime, 0))
            return;

        // Before the start or past the end of the segment?
        const Vec4 toStart = rail.start - position;
        bool off = 0.0f < zge::dot(toStart, rail.direction);
        if (!off)
        {
            const Vec4 toEnd = rail.end - position;
            off = zge::dot(toEnd, rail.direction) < 0.0f;
        }
        if (!off)
            return;
        if (findGrindRail(player))
        {
            joinNextRail(player);
            return;
        }
        changeState(player, State::Air);
        player->graceTime = grindWalls(kGrindExtensionTime);
    }
    READABLE_FUNCTION(grindMove, Verified);

    // grindBalance, the grind's fourth callback: the balance. Unless the
    // pause runs, the drift grows with the time on the rail (the skater's
    // balance stat) and moves the balance; the rail's material rumbles
    // (wood, metal) and the HUD shows the meter. The skater faces along
    // the rail, upright (on a rail close to vertical, up is square to it).
    // Past 1 either way the balance is lost: off the side (turned 25
    // degrees and pushed down), off the rail, or a crash.
    void grindBalance(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;

        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        const RailRecord &rail = railRecordOf(*player);
        ee::Float &balance = player->balance[0];

        player->grindTime = player->grindTime + frameTime;
        player->shortTime = player->shortTime + frameTime;
        if (0.0f < player->graceTime)
            player->graceTime = player->graceTime - frameTime;
        else
        {
            ee::Float &drift = player->balanceDrift;
            const bool leaningRight = 0.0f < drift;
            const ee::Float stat = skaterStat(player, kStatGrind, kStatGrindFactor);
            drift = leaningRight ? drift + player->grindTime * stat : drift - player->grindTime * stat;
            balance = balance + drift;
        }

        const auto rumble = [&](uint32_t time, uint32_t small, uint32_t big) {
            const int32_t length = sce::libgcc::fptoui(vibrateTuning(time));
            aggressive::vibrate(&player->controls, length, vibrateTuning(small), vibrateTuning(big));
        };
        switch (railMaterial(rail.node, rail.surface))
        {
        case kRailMetal:
        case kRailMetal2:
        case kRailMetal8:
            rumble(kGrindMetalTime, kGrindMetalSmall, kGrindMetalBig);
            break;
        case kRailWood:
            rumble(kGrindWoodTime, kGrindWoodSmall, kGrindWoodBig);
            break;
        default:
            break;
        }
        aggressive::hud::setGrindBalance(player, balance);

        // Facing along the rail, the way the skater faced.
        if (zge::dot(rail.direction, axes.forward) < 0.0f)
            vec3Negate(axes.forward, rail.direction);
        else
            axes.forward = rail.direction;
        if (*g_steepRail < ee::abs(zge::dot(*g_worldUp, axes.forward)))
            axes.up = zge::cross(axes.right, axes.forward);
        else
            axes.up = *g_worldUp;
        orthonormalize(&axes.right, &axes.up, &axes.forward);

        if (!(1.0f < ee::abs(balance)))
            return;
        // The balance is lost.
        const bool offTheSide = railEdgeKind(rail.node, rail.surface) == kEdgeOffTheSide ||
                                railEdgeKind(rail.node, rail.surface) == kEdgeOffTheSide2;
        if (!offTheSide)
        {
            if (railEdgeKind(rail.node, rail.surface) == kEdgeOff)
                changeState(player, State::Air);
            else
                skaterCrash(player, kCrashed);
            return;
        }
        // Off the side the balance leans to: turned 25 degrees about up,
        // and pushed down by 15.
        changeState(player, State::Air);
        const ee::Float degrees = balance < 0.0f ? ee::Float(25.0f) : ee::Float(-25.0f);
        rotateOrientation(&player->orientation, &axes.up.xyz(), degrees);
        rotateAbout(&velocity, &axes.up, degrees);
        *push = *g_worldUp * -15.0f;
        velocity = velocity + *push;
    }
    READABLE_FUNCTION(grindBalance, Verified);

    // grindControls, the grind's fifth callback (not while down): the
    // controls. Jumping leaves the rail (Air) with the jump up and, with
    // the stick to a side, Side Jump Speed sideways; a straight jump soon
    // after getting on (Grind Balance Short Time) counts a short grind. A
    // moving skater faces along its velocity when that runs along up. The
    // stick steers the balance (Grind Balance Adjust) unless the pause
    // runs.
    void grindControls(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;
        ee::Local<Vec4> side;

        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        const RailRecord &rail = railRecordOf(*player);
        Controls &controls = player->controls;

        if (controls.actions & kActionJump)
        {
            *push = *g_worldUp * skaterStat(player, kStatJump, 0);
            velocity = velocity + *push;
            newSpeed(*player);
            player->settleFrames = 0;
            const ee::Float shortTime = player->shortTime;
            changeState(player, State::Air);

            // Sideways: square to the rail and up.
            const bool right = 0.0f < controls.stickX;
            if (right || controls.stickX < 0.0f)
            {
                *side = zge::unit(zge::cross(*g_worldUp, rail.direction));
                if (right)
                    vec3Negate(*side, *side);
                *push = *side * grindWalls(kSideJumpSpeed);
                velocity = velocity + *push;
                newSpeed(*player);
            }
            else if (shortTime < grindWalls(kGrindBalanceShortTime))
            {
                ++player->shortCount;
            }

            const bool still = ee::abs(player->speed) < *g_jumpStill;
            if (!still && 1.0f <= ee::abs(zge::dot(axes.up, velocity)))
            {
                alongPlane(&axes.forward, &axes.up, &velocity);
                axes.forward = zge::unit(axes.forward);
                orthonormalize(&axes.right, &axes.up, &axes.forward);
            }
        }

        if (player->graceTime <= 0.0f && controls.stickX != 0.0f)
            player->balanceDrift = player->balanceDrift + frameTime * controls.stickX * grindWalls(kGrindBalanceAdjust);
    }
    READABLE_FUNCTION(grindControls, Verified);
}
