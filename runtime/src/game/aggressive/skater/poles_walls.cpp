// The Vertical Pole's way out (states 9 and 10, Vertical Pole and Vertical
// Pole A, share these two) and the Wall Ride's start (state 12).
//
// On a vertical pole the skater swings round it (the pole callbacks turn
// the orientation about the world's up), and leaves along Player::poleVector,
// which the state's enter callback sets to the reverse of the way the skater
// came in. The pole is a grind edge of a scene object (grindRail the object,
// grindSegment its face): its record (zge::EdgeRecord) has the edge's kind (3
// a vertical pole, 4 a horizontal one, 5 and 6 the two edges of a double
// rail), the pole's exit heading, exit speed and the word its "Swing" event
// carries.
//
// The aim (verticalPoleAim) acts only while Player::swingToMake is exactly
// 540; only the Horizontal Pole's callbacks write that field (its release
// angles from the tuning, 10 to 75 by default), so with the default tuning
// the aim returns at once in the game. Its other paths were recorded with
// the field poked to 540 (a test run).
//
// A wall ride starts from the air on a wall the air's actions found
// (Player::wallContact, wall.cpp): the skater is put half a unit out from
// it, lifted by Wall Ride Jump Speed and turned along it.
//
// A scaled vector's w is whatever the last vector operation left: the
// velocity, push and boost carry it, so the order of the statements below
// decides it (zge/math/vec.h runs on VU0).

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "moves.h"

#include "sym/aggressive/skater/poles_walls.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSwingEvent = sym::swing;     // "Swing"
        constexpr uint32_t kWallRideStart = sym::wallRideStart;  // "WallRideStart"
        constexpr uint32_t kWallTapStart = sym::wallTapStart;   // "WallTapStart"

        // (1, 0, 0, 0): the exit heading's zero.
        inline ee::Global<zge::Vec4, sym::g_xAxis> g_xAxis;
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        // 630 degrees (a turn and three quarters)
        // added to the swing still to make, one copy per case.
        inline ee::Global<ee::Float, sym::g_swingBackwardRight> g_swingBackwardRight;
        inline ee::Global<ee::Float, sym::g_swingForwardRight> g_swingForwardRight;
        inline ee::Global<ee::Float, sym::g_swingBackwardLeft> g_swingBackwardLeft;
        inline ee::Global<ee::Float, sym::g_swingForwardLeft> g_swingForwardLeft;

        // The swing round the pole (degrees): made so far and to make
        // before leaving (540 until the skater aims the way out).
        constexpr float kSwingUnaimed = 540.0f;
        constexpr float kOneTurn = 360.0f;

        // Controls::stickFlags: the air's wall ride request, cleared as a wall
        // ride starts.
        constexpr uint32_t kWallRideRequest = 0x2u;

        // Controls history: the d-pad code of the newest entry that aims
        // the pole's way out.
        constexpr uint8_t kHistoryAim = 1;

        ee::Float grindWalls(uint32_t entry)
        {
            return (*g_tuningCategories)[14].entries[entry].value;
        }
        constexpr uint32_t kWallRideJumpSpeed = 46; // Grind/Walls / Wall Ride Jump Speed

        // The first two axes made square to the third, which is kept.
        ee::Float vec3Length(zge::Vec4 &v) { return zge::vec3Length(&v.xyz()); }

        constexpr uint8_t kOnLeft = 0, kOnRight = 1; // Player::side, along a wall
    }

    // verticalPoleAim, the fifth callback of Vertical Pole and Vertical Pole A
    // (not while down): aiming the way out. While the swing still to make
    // is the unaimed 540 degrees and less than a turn is made, the d-pad's
    // aim (the newest history entry) sets the way out to the pole's exit
    // heading, and the swing to make so that the skater leaves along it:
    // the angle from the skater's side facing the pole (right, or left on
    // side 0) to the heading, one way or the other by whether the heading
    // is ahead, plus a turn and three quarters.
    void verticalPoleAim(ee::Ptr<Player> player, ee::Float frameTime)
    {
        (void)frameTime;
        ee::Local<zge::Vec4> wayOut, side;

        *wayOut = *g_xAxis;
        *side = player->axes.right;
        if (!(player->swingToMake == kSwingUnaimed))
            return;
        if (!(ee::abs(player->swingMade) < kOneTurn))
            return;
        if (player->controls.historyCount == 0 || player->controls.history[0] != kHistoryAim)
            return;

        const ee::Float heading = railLaunchMin(player->grindRail, player->grindSegment);
        rotateAbout(wayOut, g_worldUp.get(), heading);
        const zge::Vec4 &forward = player->axes.forward;
        const ee::Float made = player->swingMade;
        ee::Float toMake;
        if (player->side == 1)
        {
            const ee::Float angle = ee::toFloat(static_cast<short>(zge::angleAcos(zge::dot(*wayOut, *side)))) * zge::kAngleToDegrees;
            if (zge::dot(*wayOut, forward) < 0.0f)
                toMake = (made - angle) + *g_swingBackwardRight;
            else
                toMake = (made + angle) + *g_swingForwardRight;
        }
        else
        {
            zge::vec3Negate(&side->xyz(), &side->xyz());
            const ee::Float angle = ee::toFloat(static_cast<short>(zge::angleAcos(zge::dot(*wayOut, *side)))) * zge::kAngleToDegrees;
            if (zge::dot(*wayOut, forward) < 0.0f)
                toMake = (-made - angle) + *g_swingBackwardLeft;
            else
                toMake = (angle - made) + *g_swingForwardLeft;
        }
        player->swingToMake = toMake;
        player->poleVector = *wayOut;
    }
    READABLE_FUNCTION(verticalPoleAim, Verified);

    // verticalPoleExit, Vertical Pole's exit (Vertical Pole A's exit calls it
    // too): the skater faces the way out, upright, keeps its speed along
    // it, is moved two units to its side off the pole (right on side 1)
    // and drawn there at once, loses its velocity along the world's up,
    // faces the result, and gets the pole's exit speed on top. Then the
    // pole grab window starts again, the pole hears "Swing", the drawn
    // orientation snaps, and the speed is taken anew.
    void verticalPoleExit(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> boost;
        ee::Local<zge::Vec4> push;
        ee::Local<uint32_t> data;
        PlayerAxes &axes = player->axes;
        zge::Vec4 &velocity = player->velocity;

        axes.forward = player->poleVector;
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        axes.up = *g_worldUp;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        player->previousSpeed = player->speed;
        player->speed = vec3Length(velocity);
        velocity = axes.forward * player->speed;

        *push = axes.right * (player->side != 0 ? 2.0f : -2.0f);
        player->position = *push + player->position;
        nodeToPosition(player);

        const ee::Float upward = -zge::dot(velocity, *g_worldUp);
        *push = *g_worldUp * upward;
        velocity = velocity + *push;
        axes.forward = zge::unit(velocity);
        orthonormalize(&axes.right, &axes.up, &axes.forward);

        const ee::Float exitSpeed = railLaunchSpeed(player->grindRail, player->grindSegment);
        if (0.0f < exitSpeed)
        {
            *boost = axes.forward * exitSpeed;
            velocity = velocity + *boost;
        }
        player->controls.poleGrabTime = 0.0f;

        *data = railEventWord(player->grindRail, player->grindSegment);
        sendObjectEvent(player, player->grindRail, kSwingEvent, data);
        player->drawFlags |= kDrawnSnap;
        player->previousSpeed = player->speed;
        player->speed = vec3Length(velocity);
    }
    READABLE_FUNCTION_WAITS(verticalPoleExit, Verified);

    // Wall Ride's enter: the ride's timer, the request and the free time
    // cleared; the velocity along the wall and lifted by Wall Ride Jump Speed;
    // riding fakie flips when the skater heads against its facing along the
    // wall; it is put half a unit out from the wall point, faces the velocity
    // upright, and is on the wall's right or left (side); the wall's object
    // hears "WallRideStart" with its surface kind. Returns 1.
    int32_t wallRideEnter(Player *player)
    {
        using zge::Vec4;
        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        const WallContact &wall = player->wallContact;

        player->controls.wallRideTime = 0.0f;
        player->controls.stickFlags &= ~kWallRideRequest;
        player->airTime = 0.0f;
        player->skidFlags |= kSkidStart; // the ride's skid marks start next frame

        alongPlane(&velocity, &wall.normal, &velocity);
        player->previousSpeed = player->speed;
        velocity.y = -grindWalls(kWallRideJumpSpeed);
        player->speed = vec3Length(velocity);

        // Heading against the facing (both along the wall): fakie flips.
        ee::Local<Vec4> facing;
        alongPlane(facing, &wall.normal, &axes.forward);
        const Vec4 along = zge::unit(facing);
        const Vec4 heading = zge::unit(velocity);
        if (zge::dot(heading, along) < 0.0f)
        {
            player->drawnRefresh = 1;
            player->fakie = !player->fakie;
            player->drawFlags |= kDrawnSnap;
        }

        player->position = wall.point + wall.normal * 0.5f;
        carryRider(player, 0.0f);

        // Faces the velocity, upright.
        axes.forward = velocity;
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        axes.up = *g_worldUp;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        player->side = zge::dot(wall.normal, axes.right) < 0.0f ? kOnLeft : kOnRight;

        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;

        ee::Local<uint32_t> kind;
        *kind = surfaceKind(wall.object, wall.face);
        sendObjectEvent(player, wall.object, kWallRideStart, kind);
        return 1;
    }
    READABLE_FUNCTION_WAITS(wallRideEnter, Verified);

    // wallTapEnter, Wall Tap's enter (no code enters the state on the PS2,
    // wall.cpp): the skater stops, the wall ride request gone; the wall's object
    // hears "WallTapStart" with its surface kind; the skater faces into the
    // wall, is put half a unit out from the wall point and its ride effects
    // run once. Returns 1.
    int32_t wallTapEnter(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> offset;
        ee::Local<uint32_t> data;
        PlayerAxes &axes = player->axes;
        const WallContact &wall = player->wallContact;

        player->airTime = 0.0f;
        player->velocity = zge::kZero;
        player->speed = 0.0f;
        player->controls.wallRideTime = 0.0f;
        player->controls.stickFlags &= ~kWallRideRequest;

        *data = surfaceKind(wall.object, wall.face);
        sendObjectEvent(player, wall.object, kWallTapStart, data);

        axes.forward = zge::unit(wall.normal);
        zge::vec3Negate(&axes.forward.xyz(), &axes.forward.xyz());
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        *offset = wall.normal * 0.5f;
        player->position = wall.point + *offset;
        carryRider(player, 0.0f);
        return 1;
    }
    READABLE_FUNCTION_WAITS(wallTapEnter, Verified);

    // wallTapExit, Wall Tap's exit: the skater faces its velocity, upright;
    // off a moving wall. Returns what it read of onMovingSurface.
    int32_t wallTapExit(ee::Ptr<Player> player)
    {
        PlayerAxes &axes = player->axes;
        axes.forward = zge::unit(player->velocity);
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        axes.up = *g_worldUp;
        orthonormalize(&axes.forward, &axes.right, &axes.up);
        const int32_t wasMoving = player->onMovingSurface;
        player->airTime = 0.0f;
        if (wasMoving)
        {
            player->movingObject = 0;
            player->onMovingSurface = 0;
        }
        return wasMoving;
    }
    READABLE_FUNCTION(wallTapExit, Verified);
}
