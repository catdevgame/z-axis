// The Wall Ride (state 12) and Wall Tap (state 13) per-frame callbacks.
//
// The skater rides on a wall whose normal and contact point the state's
// enter callback keeps (Player::wallContact). Jumping off (the jump
// action, or a wall tap's time running out) adds the jump along the world's
// up and Wall Tap Out Speed along the wall's normal, and the skater is in
// the Air.
//
// Wall Tap is never entered on the PS2: no code passes state 13 to
// setSkaterState or returns it from a contact callback, and the
// skater code only reads it (the rolling sound, the player driver). Its two
// callbacks here stay Draft, with nothing to check them against, and its
// enter and exit (poles_walls.cpp) were only ever run by guest calls; the
// Wall Ride ones were recorded in the Tutorial's Wall Rides lesson
// (games/aggressive/tools/routes.py wallride, walljump).

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/scene/node.h"

#include "sym/aggressive/skater/wall.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint8_t kWallOnRight = 0, kWallOnLeft = 1; // Player::side: where the wall is, from the skater

        inline ee::Global<ee::Float, sym::g_floorAgainstUp3> g_floorAgainstUp; // 0.8: a surface this close to facing up is a floor
        inline ee::Global<ee::Float, sym::g_wallProbe> g_wallProbe;      // 0.6: how far into the wall the skater looks for it
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;

        // Grind/Walls tuning (DEVTOOLS.md).
        ee::Float grindWalls(uint32_t entry)
        {
            return (*g_tuningCategories)[14].entries[entry].value;
        }
        enum GrindWallsEntry : uint32_t
        {
            kWallTapTime = 38,
            kWallTapJumpFactor = 39,
            kWallTapOutSpeed = 40,
            kWallRideRollAngle = 41,
            kWallRideOffset = 44,
        };

        // Forward is the velocity (all four words), the axes square again,
        // up the world's up.
        void faceVelocity(Player &player)
        {
            PlayerAxes &axes = player.axes;
            axes.forward = player.velocity;
            orthonormalize(&axes.right, &axes.up, &axes.forward);
            axes.up = *g_worldUp;
            orthonormalize(&axes.forward, &axes.right, &axes.up);
        }
    }

    // wallRideLift, Wall Ride's second callback: the ride's effects, and the
    // wall ride's lift along the wall (up, without its part into the wall).
    void wallRideLift(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> lift;
        carryRider(player, frameTime);
        *lift = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
        const ee::Float speed = skaterStat(player, kStatWallRide, 0) * frameTime;
        *lift = *lift * speed;
        alongPlane(lift, &player->wallContact.normal, lift);
        player->velocity = player->velocity + *lift;
        newSpeed(*player);
    }
    READABLE_FUNCTION(wallRideLift, Verified);

    // wallRideJump, Wall Ride's fifth callback (not while down): the jump
    // off the wall. The skater leaves with Wall Tap Out Speed along the
    // wall's normal (what it had along it made up to that) and faces the
    // way it goes, upright.
    void wallRideJump(ee::Ptr<Player> player, ee::Float frameTime)
    {
        (void)frameTime;
        if (!(player->controls.actions & kActionJump))
            return;
        ee::Local<Vec4> push;
        Vec4 &velocity = player->velocity;
        const WallContact &wall = player->wallContact;

        *push = *g_worldUp * skaterStat(player, kStatJump, 0);
        velocity = velocity + *push;
        const ee::Float out = grindWalls(kWallTapOutSpeed) - zge::dot(wall.normal, velocity);
        *push = wall.normal * out;
        velocity = velocity + *push;
        changeState(player, State::Air);
        player->settleFrames = 0;
        faceVelocity(*player);
    }
    READABLE_FUNCTION(wallRideJump, Verified);

    // wallTapHold, Wall Tap's third callback: the skater is held half a
    // unit out from the wall; after Wall Tap Time it springs off (the jump
    // times Wall Tap Jump Factor up, Wall Tap Out Speed out of the wall).
    void wallTapHold(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> offset;
        ee::Local<Vec4> push;
        Vec4 &velocity = player->velocity;
        const WallContact &wall = player->wallContact;

        carryRider(player, frameTime);
        *offset = wall.normal * 0.5f;
        player->position = wall.point + *offset;

        player->airTime = player->airTime + frameTime;
        if (!(grindWalls(kWallTapTime) < player->airTime))
            return;
        *push = *g_worldUp * (skaterStat(player, kStatJump, 0) * grindWalls(kWallTapJumpFactor));
        velocity = velocity + *push;
        *push = wall.normal * grindWalls(kWallTapOutSpeed);
        velocity = velocity + *push;
        changeState(player, State::Air);
    }
    READABLE_FUNCTION(wallTapHold, Verified);

    // wallTapJump, Wall Tap's fifth callback (not while down): jumping
    // off the wall at once (the jump up, Wall Tap Out Speed out).
    void wallTapJump(ee::Ptr<Player> player, ee::Float frameTime)
    {
        (void)frameTime;
        if (!(player->controls.actions & kActionJump))
            return;
        ee::Local<Vec4> push;
        Vec4 &velocity = player->velocity;

        *push = *g_worldUp * skaterStat(player, kStatJump, 0);
        velocity = velocity + *push;
        *push = player->wallContact.normal * grindWalls(kWallTapOutSpeed);
        velocity = velocity + *push;
        changeState(player, State::Air);
        player->settleFrames = 0;
    }
    READABLE_FUNCTION(wallTapJump, Verified);

    // wallRide, Wall Ride's third callback: the ride along the wall.
    // The frame's step is swept from Wall Ride Offset out of the wall:
    //   - nothing in the way: the skater moves on, facing its velocity,
    //     and stays on the wall while a probe (0.6 into it) still finds it
    //     (else Air);
    //   - a floor ahead: back on the Ground;
    //   - more wall, turned less than Wall Ride Roll Angle: the skater
    //     rides on along it (the new triangle, normal and point kept), half
    //     a unit out;
    //   - a sharper corner: it flies off between the two walls (Air).
    void wallRide(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> offset;
        ee::Local<zge::QueryResults> results;
        ee::Local<zge::QuerySegment> segment;
        ee::Local<uint32_t[2]> skipped; // the nodes the query skips: the skater's own, then 0
        ee::Local<Vec4> step;
        ee::Local<Vec4> newNormal;
        ee::Local<zge::Matrix> objectMatrix;
        ee::Local<ee::Ptr<uint16_t>> indices;

        Vec4 &position = player->position;
        Vec4 &velocity = player->velocity;
        PlayerAxes &axes = player->axes;
        WallContact &wall = player->wallContact;

        const auto sweep = [&] {
            const uint32_t flags = uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed;
            return zge::collisionQuery(kQuerySegment, segment, flags, 0, 0, skipped, results, 0) != 0;
        };

        player->wallRideTime = player->wallRideTime + frameTime;
        skipped[0] = ee::Address(zge::sceneObject(zge::sceneOf(player->node))); // the skip list's word
        skipped[1] = 0;

        *step = velocity * frameTime;
        player->stateDistance = player->stateDistance + vec3Length(*step);
        player->shortTime = player->shortTime + frameTime;
        // On a moving wall the step takes its move too.
        if (player->onMovingSurface)
            *step = *step + player->carriedMove;

        // The step, swept from Wall Ride Offset out of the wall.
        *offset = wall.normal * grindWalls(kWallRideOffset);
        segment->start = position + *offset;
        segment->end = segment->start + *step;
        if (!sweep())
        {
            // Nothing in the way: on along the wall, while it is still there.
            position = position + *step;
            faceVelocity(*player);
            *step = wall.normal * *g_wallProbe;
            vec3Negate(*step, *step);
            segment->start = position;
            segment->end = segment->start + *step;
            if (!sweep())
                changeState(player, State::Air);
            return;
        }

        hitNormal(results, newNormal);
        if (*g_floorAgainstUp < zge::dot(*newNormal, *g_worldUp))
        {
            changeState(player, State::Ground);
            return;
        }
        if (!(grindWalls(kWallRideRollAngle) < zge::dot(*newNormal, wall.normal)))
        {
            // Too sharp a corner: off between the two walls.
            *newNormal = zge::unit(*newNormal + wall.normal);
            velocity = *newNormal * player->speed;
            axes.forward = *newNormal;
            orthonormalize(&axes.right, &axes.up, &axes.forward);
            changeState(player, State::Air);
            return;
        }

        // More wall: the side it is on, the velocity along it at the same speed.
        *step = wall.point - position;
        player->side = 0.0f < zge::dot(*step, axes.right) ? kWallOnRight : kWallOnLeft;
        alongPlane(step, &wall.normal, &velocity);
        *step = zge::unit(*step);
        velocity = *step * player->speed;

        // The new wall: its triangle's corners in world space, its normal
        // and the hit point.
        const ee::Ptr<zge::Node> object = results->nodes[0]->node;
        wall.face = hitFace(*results);
        wall.object = object;
        wall.part = hitPart(*results);
        const ee::Ptr<zge::ZgePart> mesh(collisionMeshOf(object));
        zge::faceVertices(mesh, wall.face, &*indices);
        *objectMatrix = *zge::nodeMatrix(object, 1);
        for (uint32_t i = 0; i < 3; ++i)
        {
            zge::vertexPosition(mesh, zge::faceVertexIndex(mesh, wall.face, *indices, i), &wall.corners[i]);
            wall.corners[i] = zge::transformed(*objectMatrix, wall.corners[i]);
        }
        hitNormal(results, &wall.normal);
        wall.point = results->nodes[0]->faces->point;
        *offset = wall.normal * 0.5f;
        position = wall.point + *offset;
        faceVelocity(*player);
    }
    READABLE_FUNCTION(wallRide, Verified);
}
