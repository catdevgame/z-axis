#pragma once

// What the skater's moves share: the typed calls of the vector functions they
// all use, by the Vec4s the skater's state holds. (The engine's vec3 functions
// take three lanes; every vector here is a Vec4 whose first three they read.)

#include "skater.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "sym/aggressive/skater/moves.h.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/math/matrix.h"
#include "zge/math/plane.h"


namespace aggressive::skater
{
    using zge::Vec4;

    inline zge::Vec3 *xyz(Vec4 &v) { return &v.xyz(); }
    inline const zge::Vec3 *xyz(const Vec4 &v) { return &v.xyz(); }

    // A matrix's row as the vector it is.
    inline Vec4 &row(zge::Matrix &m, int i) { return m.row(i); }

    inline ee::Float vec3Length(const Vec4 &v) { return zge::vec3Length(xyz(v)); }
    inline void vec3Negate(Vec4 &out, const Vec4 &v) { zge::vec3Negate(xyz(out), xyz(v)); }
    inline void vec3RotateY(Vec4 &out, const Vec4 &v, zge::Angle angle) { zge::vec3RotateY(xyz(out), xyz(v), angle); }

    // rotateOrientation: the matrix's rows turned about the axis by `degrees`
    // (orientation.cpp).
    void rotateOrientation(ee::Ptr<zge::Matrix> orientation, ee::Ptr<const zge::Vec3> axis, ee::Float degrees);

    // The skater changes state (setSkaterState with the State itself).
    inline void changeState(ee::Ptr<Player> player, State state) { setSkaterState(player, static_cast<uint32_t>(state)); }

    // The speed moves on to a new one, from the velocity.
    inline void newSpeed(Player &player)
    {
        player.previousSpeed = player.speed;
        player.speed = vec3Length(player.velocity);
    }

    // carryRider: what the skater rides carries it (carried.cpp).
    void carryRider(ee::Ptr<Player> player, ee::Float frameTime);

    // A segment swept through the level (collisionQuery's shape kind).
    constexpr uint32_t kQuerySegment = 1;

    // The first node a query hit, its collision part and its first face.
    inline ee::Ptr<zge::Node> hitObject(const zge::QueryResults &results) { return results.nodes[0]->node; }
    inline ee::Ptr<zge::ZgePart> hitPart(const zge::QueryResults &results) { return results.nodes[0]->part; }
    inline uint32_t hitFace(const zge::QueryResults &results) { return results.nodes[0]->faces->face; }

    // What a surface's script gets with "Collide"; it clears goOn to cancel the
    // rest of the collision.
    struct CollideEvent
    {
        uint32_t surfaceKind;
        ee::Float speed;
        uint8_t alignment[8]; // never written; scripts read only the first two words (getCollideData)
        Vec4 velocity;
        int32_t goOn;
    };
    static_assert(sizeof(CollideEvent) == 0x30 && offsetof(CollideEvent, velocity) == 0x10 && offsetof(CollideEvent, goOn) == 0x20);

    // What "GrindEnd" carries.
    struct GrindEndData
    {
        uint32_t railEvent;
        ee::Float distance;
    };

    // skaterCrash: the skater goes down (crash.cpp); kind says how.
    int32_t skaterCrash(ee::Ptr<Player> player, uint32_t kind);

    // The collision functions of move_collide.cpp.
    // skaterTouchSurface: a touched face's hazard crashes the skater.
    void skaterTouchSurface(ee::Ptr<Player> player, ee::Ptr<zge::Node> object, uint32_t face);
    // wallSpeedLoss: the speed a wall hit leaves.
    void wallSpeedLoss(ee::Ptr<Player> player, ee::Float amount);
    // vec3Reflect: out = v less twice its part along the unit normal.
    void vec3Reflect(ee::Ptr<const Vec4> v, ee::Ptr<const Vec4> normal, ee::Ptr<Vec4> out);
    // rideObject: the object hit or ridden carries the skater.
    void rideObject(ee::Ptr<Player> player, ee::Ptr<zge::Node> object, ee::Float frameTime);
    // alongPlane: out = v less its part along the unit normal. out.w comes
    // from what the last vector operation left, so callers' order matters.
    void alongPlane(ee::Ptr<Vec4> out, ee::Ptr<const Vec4> normal, ee::Ptr<const Vec4> v);
    // rotateAbout: the vector turned about the axis by `degrees`.
    void rotateAbout(ee::Ptr<Vec4> vector, ee::Ptr<const Vec4> axis, ee::Float degrees);
    // hitNormal: the first face a query hit: its normal in world space.
    void hitNormal(ee::Ptr<zge::QueryResults> results, ee::Ptr<Vec4> out);
}

namespace aggressive::skater
{
    // (action_target.cpp)
    int32_t actionRequest(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (axes.cpp)
    void orthonormalize(ee::Ptr<zge::Vec4> forward, ee::Ptr<zge::Vec4> right, ee::Ptr<const zge::Vec4> up);
}

namespace aggressive::skater
{
    // (collision.cpp)
    int32_t skaterCollisionResponse(ee::Ptr<Player> player, ee::Ptr<zge::QueryResults> hit, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (contact.cpp)
    void keepContact(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (control_steps.cpp)
    void cessSlideButton(ee::Ptr<Player> player, ee::Float frameTime);
    void ensureGroundObject(ee::Ptr<Player> player);
    void readStick(ee::Ptr<Controls> controls);
    uint32_t stickDirection(ee::Float x, ee::Float y);
    void trickRelease(ee::Ptr<Player> player, ee::Float frameTime);
    void twoButtonCommand(ee::Ptr<Player> player);
    void updateHistory(ee::Ptr<Controls> controls, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (crash.cpp)
    int32_t setSkaterDown(ee::Ptr<Player> player, uint8_t down);
}

namespace aggressive::skater
{
    // (events.cpp)
    int32_t sendObjectEvent(ee::Ptr<Player> /*player*/, ee::Ptr<zge::Node> object, ee::Address name, ee::Address data);
    int32_t sendScriptEvent(ee::Ptr<Player> /*player*/, ee::Address script, ee::Address name, ee::Address data);
}

namespace aggressive::skater
{
    // (experience.cpp)
    void levelUpEffect(ee::Ptr<Player> player);
    ee::Float skaterStat(ee::Ptr<Player> player, uint32_t stat, uint32_t use);
    void statExperience(ee::Ptr<Player> player, uint32_t stat, ee::Float amount);
}

namespace aggressive::skater
{
    // (grind_search.cpp)
    uint32_t closestRailEdge(ee::Ptr<Player> player, ee::Ptr<RailRecord> edge, ee::Ptr<zge::QueryResults> hits, uint32_t kind, ee::Float minFacing);
    int32_t findDoubleRail(ee::Ptr<Player> player);
    int32_t findGrindRail(ee::Ptr<Player> player);
    void grindTrickMove(ee::Ptr<Player> player, int32_t move, ee::Ptr<int32_t> result);
    int32_t joinNextRail(ee::Ptr<Player> player);
    int32_t searchRails(ee::Ptr<Player> player, ee::Ptr<RailRecord> rail, uint32_t tag, uint32_t vault, ee::Float radius, ee::Float minAngle);
}

namespace aggressive::skater
{
    // (ground.cpp)
    void groundPhysics(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (ground_below.cpp)
    void groundBelow(ee::Ptr<Player> player);
    void probeGround(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (ground_move.cpp)
    void groundMove(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (juice.cpp)
    void awardPotentialJuice(ee::Ptr<zge::zgui::Node> meter);
    int32_t clearPotentialJuice(ee::Ptr<zge::zgui::Node> meter);
    int32_t crashStopJizzed(ee::Ptr<zge::zgui::Node> meter);
    int32_t juiceStep(ee::Ptr<Player> player, ee::Float frameTime);
    uint32_t setCartonSize(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> size);
    int32_t setPotentialJuice(ee::Ptr<zge::zgui::Node> meter, ee::Float juice);
    uint32_t updateCurrentObject(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (leave_ground.cpp)
    void leaveGround(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (move.cpp)
    int32_t moveByVelocity(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (move_collide.cpp)
    int32_t skaterMoveCollide(ee::Ptr<Player> player, ee::Float frameTime, uint32_t landingCheck);
}

namespace aggressive::skater
{
    // (over_pipe.cpp)
    void holdOverPipe(ee::Ptr<Player> player, ee::Ptr<zge::Vec4> normal);
}

namespace aggressive::skater
{
    // (player_setup.cpp)
    void skaterAnimationEvent(ee::Ptr<Player> player, ee::Ptr<zge::AnimationEvent> event, ee::Float time);
}

namespace aggressive::skater
{
    // (poles_walls.cpp)
    void verticalPoleExit(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (reset.cpp)
    void clearCarriedAnimals(ee::Ptr<Player> player);
    void resetSkaterPhysics(ee::Ptr<Player> player, uint8_t clearPose);
}

namespace aggressive::skater
{
    // (score.cpp)
    void bankTrickScore(ee::Ptr<Player> player);
    int32_t endCombo(ee::Ptr<Player> player);
    int32_t endTrickChain(ee::Ptr<Player> player);
    int32_t freeSkaterScore(ee::Ptr<Player> player);
    uint32_t freeToAct(ee::Ptr<Player> player);
    int32_t juiceDrains();
    int32_t loseTrickScore(ee::Ptr<Player> player);
    int32_t playerComboState(ee::Ptr<Player> player);
    void resetLineCounters(ee::Ptr<Player> player);
    uint32_t resetPlayerTricks(ee::Ptr<Player> player);
    int32_t setClockwiseSpins(ee::Ptr<Player> player, uint32_t halfTurns);
    int32_t setCounterClockwiseSpins(ee::Ptr<Player> player, uint32_t halfTurns);
    int32_t setPlayerComboState(ee::Ptr<Player> player, uint32_t count);
    void setupSkaterScore(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (skid_marks.cpp)
    void wallRideContact(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (slide_off.cpp)
    int32_t slideOffEdge(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (state_enters.cpp)
    int32_t groundEnter(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (surface.cpp)
    uint32_t classTypeName(ee::Ptr<zge::GeoObject> objectClass);
    uint32_t isMover(ee::Ptr<zge::Node> object);
    uint32_t pipeEdgeOf(ee::Ptr<zge::Node> object, uint32_t face, ee::Ptr<RailRecord> edge);
    uint32_t railEdgeAnyKind(ee::Ptr<zge::Node> node, uint32_t edge);
    uint32_t railEdgeKind(ee::Ptr<zge::Node> rail, uint32_t segment);
    uint32_t railEventWord(ee::Ptr<zge::Node> rail, uint32_t segment);
    ee::Float railLaunchMax(ee::Ptr<zge::Node> rail, uint32_t segment);
    ee::Float railLaunchMin(ee::Ptr<zge::Node> rail, uint32_t segment);
    ee::Float railLaunchSpeed(ee::Ptr<zge::Node> rail, uint32_t segment);
    uint32_t railMaterial(ee::Ptr<zge::Node> rail, uint32_t segment);
    void railToWorld(ee::Ptr<RailRecord> rail);
    ee::Float surfaceGravity(ee::Ptr<zge::Node> node, uint32_t face);
    ee::Float surfaceLaunchCap(ee::Ptr<zge::Node> node, uint32_t face);
    ee::Float surfaceLaunchPush(ee::Ptr<zge::Node> node, uint32_t face);
    uint32_t surfaceSound(ee::Ptr<zge::Node> node, uint32_t face);
    void wallToWorld(ee::Ptr<WallContact> wall);
}

namespace aggressive::skater
{
    // (sway.cpp)
    int32_t skaterSway(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (turn.cpp)
    void groundTurn(ee::Ptr<Player> player, ee::Float frameTime);
}

namespace aggressive::skater
{
    // (wall_search.cpp)
    int32_t findWall(ee::Ptr<Player> player, ee::Ptr<WallContact> wall, ee::Float radius);
}

namespace aggressive::skater
{
    // (zones.cpp)
    void updateTriggerZones(ee::Ptr<Player> player);
}

namespace aggressive::skater
{
    // (air.cpp)
    void airGravity(ee::Ptr<Player> player, ee::Float frameTime);
}
