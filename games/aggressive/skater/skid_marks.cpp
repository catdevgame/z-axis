// The skid marks of a wall ride and a cess slide (was the sweep's
// wallRideContact; the player driver runs it in those states): the feet
// (the model's "L_Foot" and "R_Foot" bones) put onto the surface ridden,
// the wall's plane in a wall ride, else the ground's under the position,
// a twentieth off it, and a mark from last frame's points to them (the
// skid effect, spawnWallRideEffect: DM2's "skid"), a tenth wide. The first frame of a
// ride or slide (kSkidStart) only takes the points.
//

#include "moves.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "aggressive/fx/library.h"
#include "sym/aggressive/skater/skid_marks.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kLeftFoot = sym::leftFoot;  // "L_Foot"
        constexpr uint32_t kRightFoot = sym::rightFoot; // "R_Foot"
        // How far off the surface the marks lie (0.05) and their
        // width (0.1).
        inline ee::Global<ee::Float, sym::g_skidLift> g_skidLift;
        inline ee::Global<ee::Float, sym::g_skidWidth> g_skidWidth;
    }

    // wallRideContact: the feet's points on the surface ridden
    // (Player::skidPoints, last frame's kept in lastSkidPoints) and the
    // skid marks between them.
    void wallRideContact(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> plane, foot, lift, origin;

        player->lastSkidPoints[0] = player->skidPoints[0];
        player->lastSkidPoints[1] = player->skidPoints[1];
        ee::Ptr<zge::Vec4> normal = &player->groundNormal;
        if (player->state == State::WallRide)
        {
            normal = &player->wallContact.normal;
            zge::planeThrough(plane, player->wallContact.corners, normal);
        }
        else
            zge::planeThrough(plane, &player->position, normal);

        // A foot onto the plane, lifted off it.
        const auto onSurface = [&](uint32_t boneName, zge::Vec4 &point)
        {
            const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
            const uint32_t name = zge::nameHash(ee::Ptr<const char>(boneName));
            const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, name);
            *origin = zge::kZero;
            const ee::Ptr<zge::Scene> boneScene = zge::sceneOf(player->node);
            zge::bonePoint(boneScene, bone, origin, foot, 0);
            const ee::Float distance = zge::planeDistance(plane, foot);
            *lift = *normal * distance;
            *foot = *foot - *lift;
            *lift = *normal * *g_skidLift;
            point = *foot + *lift;
        };
        onSurface(kLeftFoot, player->skidPoints[0]);
        onSurface(kRightFoot, player->skidPoints[1]);

        if (player->skidFlags & kSkidStart)
        {
            // The ride's first frame: no mark from stale points.
            player->skidFlags = player->skidFlags & ~kSkidStart;
            return;
        }
        const ee::Float width = *g_skidWidth;
        aggressive::fx::spawnSkidMark(&player->skidPoints[0], &player->lastSkidPoints[0], &normal->xyz(), width, width);
        aggressive::fx::spawnSkidMark(&player->skidPoints[1], &player->lastSkidPoints[1], &normal->xyz(), width, width);
    }
    READABLE_FUNCTION_WAITS(wallRideContact, Verified);
}
