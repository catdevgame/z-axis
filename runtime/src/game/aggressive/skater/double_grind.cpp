// The Double Grind state (7): both feet on two parallel rails, and the
// Horizontal Pole's contact callback.
//
// A level's double rails are pairs of grind edges tagged 5 and 6 (the
// edge stream of a collision part, part +0x4C, byte +8 of each record; the
// Museum's beams, the Boardwalk's roller coaster). The trick system starts
// a double grind for the trick type 0x18 (the d-pad up three times, then
// the grind button: the button table g_trickButtons's entry 0): the air's
// contact callback looks for a double rail within Grind Radius
// (findDoubleRail) and keeps the pair's record in Player::doubleGrindRails;
// tools/routes.py doublegrind (a test copy) reaches it on a Museum beam.
//
// The state's callbacks are the grind's (grind.cpp) on the pair's middle
// line: the speed along the rails (gravity downhill, Forward Friction,
// Max Grind Speed), the move along them, a balance of its own (the HUD's
// meter; falling past 1 is a crash), and the controls (a jump off along the
// rails' up, a side jump with the stick, the balance with the stick forward
// and back).

#include "aggressive/level/double_rails.h"
#include "moves.h"
#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/collision/closest.h"
#include "aggressive/hud/hud.h"
#include "aggressive/game_start.h"
#include "aggressive/level/objectives.h"

#include "sym/aggressive/skater/double_grind.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCrashed = 3; // skaterCrash's kind: "You crashed!"
        // The level records the double grind's time and distance go to
        // (inferred).
        constexpr uint32_t kLevelRecordGrindTime = 0;
        constexpr uint32_t kLevelRecordGrindDistance = 1;
        inline ee::Global<ee::Float, sym::g_balanceDead2> g_balanceDead; // 1e-5: a balance this small leans neither way
        inline ee::Global<ee::Float, sym::g_steepRail3> g_steepRail;   // 0.97: rails this close to vertical
        inline ee::Global<ee::Float, sym::g_stillSpeed5> g_stillSpeed;  // 1e-5: slower than this the skater's forward row is its heading
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        ee::Float gravity() { return tuning(3, 0); } // Physics / Gravity
        enum GrindWallsEntry : uint32_t
        {
            kMaxGrindSpeed = 2,
            kGrindBalanceAdjust = 4,
            kGrindBalanceDelay = 6,
            kGrindBalanceBestReset = 7,
            kGrindBalanceReset = 8,
            kGrindBalanceSpinDead = 9,
            kGrindBalanceSpinReset = 10,
            kGrindBalanceShortTime = 11,
            kGrindBalanceShortInit = 12,
            kGrindBalanceShortDead = 13,
            kForwardFriction = 32,
            kSideJumpSpeed = 45,
            kGrindHillGravity = 47,
            kGrindExtensionTime = 48,
        };
        ee::Float grindWalls(uint32_t entry) { return tuning(14, entry); }

        // A double rail (pairedRail's record, made at the level's load from a
        // pair of edges tagged 5 and 6, level/double_rails.h): the line
        // between the two edges.
        using level::DoubleRail;

        // CVT.S.W of an unsigned word, as the compiler does it: halved
        // (the low bit kept) and doubled back when the top bit is set.
        ee::Float unsignedToFloat(uint32_t u)
        {
            if (static_cast<int32_t>(u) >= 0)
                return ee::toFloat(static_cast<int32_t>(u));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((u >> 1) | (u & 1u)));
            return half + half;
        }

        // Controls::actions: the spin buttons (a spin keeps the balance).
        constexpr uint32_t kActionSpin = kActionButton100 | kActionButton200;
    }

    void ontoDoubleRail(ee::Ptr<Player> player);

    // doubleGrindEnter, Double Grind's enter: the side the rails' middle is on
    // (side 0: to the skater's right), onto the rails, the balance's drift
    // (the stat plus Grind Balance Short Init for each short grind before,
    // leaning the way the balance already leans, or by the speed's parity),
    // a lean past Grind Balance Best Reset eased back by Grind Balance Reset
    // (unless spinning with Spin Reset on), the meter shown, and Grind
    // Balance Delay before the balance moves (none after more than Short
    // Dead short grinds). Returns 1.
    int32_t doubleGrindEnter(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> toRails;
        ee::Local<Vec4> closest;
        DoubleRail &rails = *player->doubleGrindRails;

        zge::closestOnSegment(closest, &rails.start, &rails.end, &player->position);
        *toRails = *closest - player->position;
        player->side = 0.0f < zge::dot(*toRails, player->axes.right) ? 0 : 1;
        ontoDoubleRail(player);

        const ee::Float stat = skaterStat(player, kStatGrind, kStatGrindInit);
        const ee::Float drift = stat + grindWalls(kGrindBalanceShortInit) * ee::toFloat(player->shortCount);
        const ee::Float lean = player->balance[1];
        if (ee::abs(lean) < *g_balanceDead)
            player->balanceSpeed = (ee::toInt(player->speed) & 1) ? drift : -drift;
        else
            player->balanceSpeed = 0.0f < lean ? drift : -drift;

        const bool spinKeeps = (player->controls.actions & kActionSpin) && ee::toInt(grindWalls(kGrindBalanceSpinReset)) != 0;
        if (!spinKeeps && grindWalls(kGrindBalanceBestReset) < ee::abs(lean))
        {
            const ee::Float eased = lean * grindWalls(kGrindBalanceReset);
            player->balance[1] = eased;
            if (ee::abs(eased) < grindWalls(kGrindBalanceBestReset))
                player->balance[1] = eased < 0.0f ? -grindWalls(kGrindBalanceBestReset) : grindWalls(kGrindBalanceBestReset);
        }
        player->balance[0] = player->balance[0] + player->balanceDrift;
        aggressive::hud::showBalanceMeter(player);
        player->lastRail = {};

        const bool spinStill = (player->controls.actions & kActionSpin) && ee::toInt(grindWalls(kGrindBalanceSpinDead)) != 0;
        if (!spinStill)
            player->stateValue = grindWalls(kGrindBalanceDelay);
        if (grindWalls(kGrindBalanceShortDead) < unsignedToFloat(static_cast<uint32_t>(player->shortCount)))
            player->stateValue = 0.0f;
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
        return 1;
    }
    READABLE_FUNCTION(doubleGrindEnter, Verified);

    // doubleGrindPhysics, Double Grind's second callback: the speed along the
    // rails: gravity's part along them (times Grind Hill Gravity), the
    // velocity kept on them, Forward Friction, at most Max Grind Speed.
    void doubleGrindPhysics(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> pull;
        ee::Local<Vec4> friction;
        const DoubleRail &rails = *player->doubleGrindRails;
        Vec4 &velocity = player->velocity;

        // Gravity down, along the rails.
        *pull = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        *pull = *pull * (gravity() * frameTime);
        const ee::Float downhill = zge::dot(rails.direction, *pull);
        *pull = zge::scaledAfterMeasure(rails.direction, downhill);
        *pull = *pull * grindWalls(kGrindHillGravity);
        velocity = velocity + *pull;

        const ee::Float along = zge::dot(velocity, rails.direction);
        velocity = zge::scaledAfterMeasure(rails.direction, along);
        *friction = player->axes.forward * (grindWalls(kForwardFriction) * along);
        velocity = velocity - *friction;
        newSpeed(*player);
        if (grindWalls(kMaxGrindSpeed) < player->speed)
        {
            player->speed = grindWalls(kMaxGrindSpeed);
            velocity = zge::unit(velocity);
            velocity = zge::scaledAfterMeasure(velocity, player->speed);
        }
    }
    READABLE_FUNCTION(doubleGrindPhysics, Verified);

    // doubleGrindMove, Double Grind's third callback: the move along the
    // rails (last frame's position, velocity and orientation kept), the
    // distances, the frame's collisions, and past either end of the rails
    // onto the next double rail within reach, or into the Air with Grind
    // Extension Time to get back on.
    void doubleGrindMove(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> step;
        ee::Local<Vec4> toEnd;
        Vec4 &position = player->position;
        const DoubleRail &rails = *player->doubleGrindRails;

        player->airTime = player->airTime + frameTime;
        player->shortTime = player->shortTime + frameTime;
        player->lastPosition = position;
        player->lastVelocity = player->velocity;
        player->lastOrientation = player->orientation;

        *step = player->velocity * frameTime;
        position = position + *step;
        player->distance = player->distance + vec3Length(*step);
        player->stateDistance = player->stateDistance + vec3Length(*step);
        player->groundNormal = zge::kZero;
        skaterMoveCollide(player, frameTime, 0);

        // Before the start or past the end of the rails?
        *toEnd = rails.start - position;
        bool off = 0.0f < zge::dot(*toEnd, rails.direction);
        if (!off)
        {
            *toEnd = rails.end - position;
            off = zge::dot(*toEnd, rails.direction) < 0.0f;
        }
        if (!off)
            return;
        if (findDoubleRail(player))
        {
            ontoDoubleRail(player);
            return;
        }
        changeState(player, State::Air);
        player->stateValue = grindWalls(kGrindExtensionTime);
    }
    READABLE_FUNCTION(doubleGrindMove, Verified);

    // doubleGrindBalance, Double Grind's fourth callback: the balance. After the
    // delay it drifts the way it leans, faster the longer the grind (the
    // stat times the time on the rails); the meter shows it; the skater
    // faces along the rails the way it faced, upright (or square to steep
    // rails); a balance past 1 is a crash.
    void doubleGrindBalance(ee::Ptr<Player> player, ee::Float frameTime)
    {
        const DoubleRail &rails = *player->doubleGrindRails;
        PlayerAxes &axes = player->axes;
        ee::Float &balance = player->balance[1];

        player->grindTime = player->grindTime + frameTime;
        if (0.0f < player->stateValue)
            player->stateValue = player->stateValue - frameTime;
        else
        {
            ee::Float &drift = player->balanceSpeed;
            const bool leaningPositive = 0.0f < drift;
            const ee::Float stat = skaterStat(player, kStatGrind, kStatGrindFactor);
            if (leaningPositive)
                drift = drift + player->grindTime * stat;
            else
                drift = drift - player->grindTime * stat;
            balance = balance + drift;
        }
        aggressive::hud::setBalance(player, balance);

        if (zge::dot(rails.direction, axes.forward) < 0.0f)
            vec3Negate(axes.forward, rails.direction);
        else
            axes.forward = rails.direction;
        if (*g_steepRail < ee::abs(zge::dot(*g_worldUp, axes.forward)))
            axes.up = zge::cross(axes.right, axes.forward);
        else
            axes.up = *g_worldUp;
        orthonormalize(&axes.right, &axes.up, &axes.forward);

        if (1.0f < ee::abs(balance))
            game::tail(skaterCrash, player, kCrashed);
    }
    READABLE_FUNCTION(doubleGrindBalance, Verified);

    // doubleGrindControls, Double Grind's fifth callback (not while down): the
    // jump off along the rails' up (with a side jump of Side Jump Speed
    // the way the stick leans across; a jump with the stick centred soon
    // after getting on counts a short grind), and, after the delay, the
    // stick forward or back moving the balance (Grind Balance Adjust).
    void doubleGrindControls(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> push;
        ee::Local<Vec4> across;
        Vec4 &velocity = player->velocity;

        if (player->controls.actions & kActionJump)
        {
            const DoubleRail &rails = *player->doubleGrindRails;
            *push = rails.up * skaterStat(player, kStatJump, 0);
            velocity = velocity + *push;
            player->settleFrames = 0;
            const ee::Float grindTime = player->shortTime; // the exit clears it
            changeState(player, State::Air);

            const ee::Float stick = player->controls.stickX;
            if (0.0f < stick || stick < 0.0f)
            {
                *across = zge::cross(rails.up, rails.direction);
                *across = zge::unit(*across);
                if (0.0f < stick)
                    vec3Negate(*across, *across);
                *push = *across * grindWalls(kSideJumpSpeed);
                velocity = velocity + *push;
            }
            else if (grindTime < grindWalls(kGrindBalanceShortTime))
                player->shortCount = player->shortCount + 1;
        }

        if (!(player->stateValue <= 0.0f))
            return;
        const ee::Float stickY = player->controls.stickY;
        if (stickY == 0.0f)
            return;
        player->balanceSpeed = player->balanceSpeed + frameTime * stickY * grindWalls(kGrindBalanceAdjust);
    }
    READABLE_FUNCTION(doubleGrindControls, Verified);

    // doubleGrindExit, Double Grind's exit: the rails let go (their object
    // kept), the meter hidden, the object ridden (and the player's script)
    // told "GrindEnd" with the rail's event word and the distance, the rail and
    // object forgotten, and the grind's time and distance offered to the
    // level records.
    void doubleGrindExit(ee::Ptr<Player> player)
    {
        ee::Local<GrindEndData> eventData;

        player->lastRail = player->doubleGrindRails->node;
        player->doubleGrindRails = {};
        aggressive::hud::hideBalanceMeter(player);
        if (const ee::Ptr<zge::Node> object = player->objectNode)
        {
            eventData->railEvent = player->railEvent;
            eventData->distance = player->distance;
            sendObjectEvent(player, object, sym::grindEnd, eventData);
            if (const uint32_t script = player->script)
                sendScriptEvent(player, script, sym::grindEnd, eventData);
        }
        player->distance = 0.0f;
        player->object = 0;
        player->surface = 0xFFFFFFFFu;
        player->doubleGrindRails = {};
        player->railStart = zge::kZero;
        player->railEnd = zge::kZero;
        player->airTime = 0.0f;
        aggressive::updateLevelRecord(kLevelRecordGrindTime, *g_levelWord, player->name, player->shortTime);
        aggressive::updateLevelRecord(kLevelRecordGrindDistance, *g_levelWord, player->name, player->stateDistance);
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
    }
    READABLE_FUNCTION_WAITS(doubleGrindExit, Verified);

    // horizontalPoleContact, Horizontal Pole's contact callback (StateRow::request):
    // contact 11 lets go of the bar: off to the side the stick leans across
    // (Air kind 3 or 7), up when pushed forward (1, HPole Release Angle Up),
    // else 5, with HPole Release Angle; contact 12 drops off (5, HPole
    // Release Angle Drop). Both go to the Air and return 2; other contacts
    // return -1.
    int32_t horizontalPoleContact(ee::Ptr<Player> player, uint32_t contact)
    {
        enum ActionEntry : uint32_t
        {
            kHPoleReleaseAngle = 17,
            kHPoleReleaseAngleUp = 18,
            kHPoleReleaseAngleDrop = 19,
        };
        const auto action = [](uint32_t entry) { return tuning(15, entry); };
        if (contact == 11)
        {
            const ee::Float stickX = player->controls.stickX;
            if (0.0f < stickX)
            {
                player->barDirection = 3;
                player->releaseAngle = action(kHPoleReleaseAngle);
            }
            else if (stickX < 0.0f)
            {
                player->barDirection = 7;
                player->releaseAngle = action(kHPoleReleaseAngle);
            }
            else if (player->controls.stickY < 0.0f)
            {
                player->barDirection = 1;
                player->releaseAngle = action(kHPoleReleaseAngleUp);
            }
            else
            {
                player->barDirection = 5;
                player->releaseAngle = action(kHPoleReleaseAngle);
            }
        }
        else if (contact == 12)
        {
            player->barDirection = 5;
            player->releaseAngle = action(kHPoleReleaseAngleDrop);
        }
        else
            return -1;
        changeState(player, State::Air);
        return 2;
    }
    READABLE_FUNCTION(horizontalPoleContact, Verified);

    // ontoDoubleRail: the skater put onto the double rail (Player::
    // doubleGrindRails): the rails turned to run the way it heads, its
    // velocity along them, its rows square to them (forward along, up the
    // rails' up), the position on the line between them. A rail other than
    // the one ground (another object, or another event word) ends that
    // grind ("GrindEnd" with its event word and distance, to the object and
    // the player's script) and starts this one ("GrindStart", the goals told
    // of the named object it belongs to).
    void ontoDoubleRail(ee::Ptr<Player> player)
    {
        ee::Local<GrindEndData> grindEndData;
        ee::Local<Vec4> heading;
        ee::Local<Vec4> along;
        ee::Local<Vec4> firstEnd; // the rails' start while their ends swap
        ee::Local<uint32_t> grindStartData;
        DoubleRail &rails = *player->doubleGrindRails;
        PlayerAxes &axes = player->axes;

        // The way the skater heads: its forward row when it hardly moves.
        if (ee::abs(player->speed) < *g_stillSpeed)
            *heading = axes.forward;
        else
            *heading = zge::unit(player->velocity);
        // The rails run that way: their direction turned, their ends swapped.
        if (zge::dot(rails.direction, *heading) < 0.0f)
        {
            vec3Negate(rails.direction, rails.direction);
            *firstEnd = rails.start;
            rails.start = rails.end;
            rails.end = *firstEnd;
        }
        zge::matrixIdentity(&player->railLean);
        // Only the velocity's part along the rails is kept.
        const ee::Float speedAlong = zge::dot(player->velocity, rails.direction);
        *along = zge::scaledAfterMeasure(rails.direction, speedAlong);
        player->velocity = *along;
        // Forward along the rails, up their up, right square to both.
        if (zge::dot(rails.direction, axes.forward) < 0.0f)
            vec3Negate(axes.forward, rails.direction);
        else
            axes.forward = rails.direction;
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        if (zge::dot(rails.up, axes.up) < 0.0f)
            vec3Negate(rails.up, rails.up);
        axes.up = rails.up;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        zge::closestOnSegment(&player->position, &rails.start, &rails.end, &player->position);

        // Still on the same rail: nothing to tell.
        if (player->object == rails.object && player->railEvent == railEventWord(player->grindRail, player->grindSegment))
            return;
        if (player->object != rails.object)
            player->lastRail = player->objectNode;
        if (const ee::Ptr<zge::Node> object = player->objectNode)
        {
            grindEndData->railEvent = player->railEvent;
            grindEndData->distance = player->distance;
            sendObjectEvent(player, object, sym::grindEnd, grindEndData);
            if (const uint32_t script = player->script)
                sendScriptEvent(player, script, sym::grindEnd, grindEndData);
        }

        // Onto this rail.
        const uint32_t event = railEventWord(player->grindRail, player->grindSegment);
        const ee::Ptr<zge::Node> rail = player->grindRail;
        player->distance = 0.0f;
        player->railEvent = event;
        player->surface = 0xFFFFFFFFu;
        *grindStartData = event;
        player->objectNode = rail;
        sendObjectEvent(player, rail, sym::grindStart, grindStartData);
        const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(player->grindRail, 0);
        if (owner)
        {
            const uint32_t name = zge::objectNameHash(owner);
            aggressive::level::goalGrind(name, *grindStartData);
        }
    }
    READABLE_FUNCTION_WAITS(ontoDoubleRail, Verified);

}
