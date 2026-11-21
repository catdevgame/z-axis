// The skater's effects each frame (inlineFx.c, readable round gamehand5):
// skaterEffects, called by the player's update after the move. Four points
// of the skater's model (bones by the name hashes at g_pointBones: the two
// hands, then the two skates, by how they are used) are found in the world
// each frame and kept with last frame's; from them the grind sparks fly
// (Player::lastGrindState: a grind from the skates, a hand grind from a
// hand, a double grind from both skates), the wall tap's sparks, the
// jizzed skater's fire trails and the speed and bail trails. In two levels
// (indices 2 and 3) the breakable objects the skater hits break.
//
// Field names past the offsets are inferred from what the code does.

#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/globals.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/assets/geo.h"
#include "zge/scene/particle_types.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/breakables.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/fx/skater_effects.cpp.h"

namespace aggressive::fx
{
    void touchBreakable(ee::Address breakable);

    namespace
    {
        using skater::Player;

        // The four points of each player's skater, this frame's and last
        // frame's.
        enum EffectPoint : uint32_t { kLeftHand = 0, kRightHand = 1, kLeftSkate = 2, kRightSkate = 3 };
        inline ee::Global<zge::Vec4[4][2], sym::pointsNow> g_pointsNow;
        inline ee::Global<zge::Vec4[4][2], sym::pointsLast> g_pointsLast;
        inline ee::Global<uint32_t[4], sym::g_pointBones> g_pointBones; // the bones' name hashes

        zge::Vec4 &pointNow(uint32_t point, ee::Ptr<Player> player) { return (*g_pointsNow)[point][player->number]; }
        zge::Vec4 &pointLast(uint32_t point, ee::Ptr<Player> player) { return (*g_pointsLast)[point][player->number]; }

        inline ee::Global<uint32_t, sym::g_clearFiretrails> g_clearFiretrails;
        // The level index as the original reads it here: a word (globals.h
        // g_levelIndex is its low byte).
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelWord;
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;          // the effects' clock
        inline ee::Global<ee::Float, sym::g_lastWallTapSparks> g_lastWallTapSparks; // its time at the wall tap's last sparks
        inline ee::Global<ee::Float, sym::g_wallTapDrop> g_wallTapDrop;      // the sparks' height below the position
        inline ee::Global<ee::Float, sym::g_wallTapSpeed> g_wallTapSpeed;     // of the skater's velocity
        inline ee::Global<ee::Float, sym::g_handGrindSpeed> g_handGrindSpeed;
        inline ee::Global<ee::Float, sym::g_leftGrindDrop> g_leftGrindDrop;
        inline ee::Global<ee::Float, sym::g_leftGrindSpeed> g_leftGrindSpeed;
        inline ee::Global<ee::Float, sym::g_rightGrindDrop> g_rightGrindDrop;
        inline ee::Global<ee::Float, sym::g_rightGrindSpeed> g_rightGrindSpeed;
        inline ee::Global<ee::Float, sym::g_doubleGrindDrop> g_doubleGrindDrop;
        inline ee::Global<ee::Float, sym::g_doubleGrindSpeed> g_doubleGrindSpeed;
        inline ee::Global<zge::Vec4, sym::origin> g_origin; // a zero point (bonePoint's offset)

        constexpr uint32_t kBreakSound = 47;
        constexpr uint32_t kPhysicsMaxSpeed = 3;           // Physics / MaxSpeed
        constexpr uint32_t kSkatesTrailFlag = 0x1;         // Player::flags bit 0: the skates' trail kind 2
        constexpr zge::Angle kSparkTurn = 0xAAA;           // 15 degrees
        constexpr int32_t kDebrisKind = 3;

        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }

        // This frame's points become last frame's.
        void keepPoints(ee::Ptr<Player> player)
        {
            for (uint32_t point = kLeftHand; point <= kRightSkate; ++point)
                pointLast(point, player) = pointNow(point, player);
        }

        // A spark's start (`at`: the point, raised by `drop`) and velocity
        // (the skater's, by `speed`, turned by `turn` about up).
        void sparkFrom(zge::Vec4 &at, zge::Vec4 &velocity, ee::Ptr<Player> player, uint32_t point, const ee::Float *drop,
                       ee::Float speed, zge::Angle turn)
        {
            at = pointNow(point, player);
            if (drop)
                at.y = at.y + *drop;
            velocity = player->velocity * speed;
            zge::vec3RotateY(asVec3(velocity), asVec3(velocity), turn);
        }

        // The sparks fly 11.5 up (y down).
        void lift(zge::Vec4 &velocity)
        {
            velocity.y = velocity.y - ee::Float(11.5f);
        }

        // More sparks the faster the skater goes: 4 at top speed.
        int32_t sparkCount(ee::Ptr<Player> player)
        {
            return ee::toInt(player->speed * (ee::Float(4.0f) / physics(kPhysicsMaxSpeed)));
        }

        // A broken object's debris: from each face of its collision mesh,
        // at the face's centre, along its normal (in the world), thrown with
        // the skater's velocity.
        void breakIntoDebris(ee::Ptr<Player> player, ee::Ptr<zge::Node> object, ee::Ptr<zge::CollisionFrame> model)
        {
            const ee::Ptr<zge::ZgePart> mesh(zge::framePartMesh(zge::frameLevelPart(model, 0)));
            const ee::Ptr<zge::Matrix> placed = zge::nodeMatrix(ee::Ptr<zge::Node>(object), 1u);
            ee::Local<zge::Matrix> world;
            ee::Local<ee::Ptr<zge::Matrix>> worldAt;   // (the original keeps these addresses in its frame)
            ee::Local<uint32_t> faceCount;
            ee::Local<ee::Ptr<uint16_t>> corners;       // the face's corners' vertex numbers
            ee::Local<ee::Ptr<ee::Ptr<uint16_t>>> cornersAt;
            ee::Local<ee::Ptr<zge::Vec4>> velocityAt;
            ee::Local<zge::Vec4> normal;
            ee::Local<zge::Vec4> centre;
            ee::Local<zge::Vec4> corner;
            *worldAt = world;
            *world = *placed;
            *faceCount = mesh->faceCount;
            if (!*faceCount)
                return;
            *cornersAt = corners;
            *velocityAt = &player->velocity;
            for (uint32_t face = 0; face < *faceCount; ++face)
            {
                const uint32_t count = zge::faceVertices(mesh, face, &*corners) & 0xFFu;
                zge::faceNormal(mesh, face, normal);
                *normal = zge::rotated(*world, *normal);
                *centre = zge::kZero;
                for (uint32_t i = 0; i < count; ++i)
                {
                    zge::vertexPosition(mesh, (*corners)[i], corner);
                    *corner = zge::transformed(*world, *corner);
                    *centre = *centre + *corner;
                }
                const ee::Float share = ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(count));
                *centre = *centre * share;
                debris(centre, &player->velocity, normal, kDebrisKind);
            }
        }

        // Levels 2 and 3: the breakable the skater hit (its last object, or
        // its last rail) breaks; level 3 counts it touched, elsewhere the
        // first whole one hit is broken with a sound and its debris, and the
        // walk stops.
        void breakHit(ee::Ptr<Player> player, ee::Local<uint32_t> &broke)
        {
            ee::Local<ee::Ptr<level::Breakable>> breakable;
            *breakable = aggressive::level::firstBreakable();
            while (*breakable)
            {
                const uint32_t object = aggressive::level::breakableObject(*breakable);
                const ee::Ptr<zge::Node> node(object); // the breakable's node word
                if (object == player->lastObject || node == player->lastRail)
                {
                    if (*g_levelWord == 3)
                        touchBreakable(*breakable);
                    else
                    {
                        *broke = 1;
                        if (aggressive::level::breakableWhole(*breakable))
                        {
                            const ee::Ptr<zge::CollisionFrame> model = zge::classModel(zge::objectClass(node), 0);
                            aggressive::level::setBreakable(*breakable, 0);
                            zge::setNodeShown(ee::Ptr<zge::Node>(object), 0, 0);
                            zge::setNodeCollides(ee::Ptr<zge::Node>(object), 0, 0);
                            aggressive::soundEvent(kBreakSound, &player->position, 49, 127, 0);
                            if (model && zge::frameLevels(model))
                                breakIntoDebris(player, node, model);
                            zge::releaseNode(node, 1);
                            player->lastObject = 0;
                            if (object == player->object)
                            {
                                player->object = 0;
                                player->surface = 0xFFFFFFFFu; // none
                            }
                        }
                    }
                }
                *breakable = aggressive::level::nextBreakable(*breakable);
                if (*broke)
                    break;
            }
        }
    }

    int32_t startTrail(uint32_t playerNumber, uint32_t point, uint32_t kind);

    // skaterEffects (player): the skater's effects each frame (see the top
    // of the file).
    void skaterEffects(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> tapAt;
        ee::Local<zge::Vec4> tapVelocity;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> rootPosed;
        ee::Local<zge::Vec4> rootRest;
        ee::Local<zge::Vec4> rootTurned;
        ee::Local<zge::Vec4> poseOffset;
        ee::Local<zge::Matrix> rotation;
        ee::Local<zge::Vec4> sparkAt;
        ee::Local<zge::Vec4> sparkVelocity;
        ee::Local<uint32_t> broke; // a breakable broke this frame
        ee::Local<ee::Ptr<uint8_t>> playerKept; // (the original keeps the player in its frame, as its fields from +0x70)
        *playerKept = player->resetBytes;
        *broke = 0;
        if (*g_levelWord == 7)
            reflectSkater(player);
        if (player->splashed == 1)
            *g_clearFiretrails = 1;
        if (!*g_scriptedCamera)
            keepPoints(player);

        // The four points in the world: each bone's posed point moved by
        // where the model's root is against where its pose puts it.
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        zge::updatePose(player->node);
        const ee::Ptr<zge::SceneBone> root = zge::rootBone(scene);
        zge::bonePoint(ee::Ptr<zge::Scene>(scene), ee::Ptr<zge::SceneBone>(root), g_origin.get(), rootPosed, 2u);
        zge::bonePoint(ee::Ptr<zge::Scene>(scene), ee::Ptr<zge::SceneBone>(root), g_origin.get(), rootRest, 0u);
        zge::quaternionToMatrix(&*rotation, zge::objectOrientation(player->node));
        *rootTurned = zge::rotated(*rotation, *rootPosed);
        *point = *rootTurned + *zge::objectPosition(player->node);
        *poseOffset = *point - *rootRest;
        for (uint32_t which = kLeftHand; which <= kRightSkate; ++which)
        {
            const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, (*g_pointBones)[which]);
            zge::bonePoint(ee::Ptr<zge::Scene>(scene), ee::Ptr<zge::SceneBone>(bone), g_origin.get(), point, 0u);
            pointNow(which, player) = *point + *poseOffset;
        }
        if (*g_scriptedCamera)
        {
            // A cut: no trail from where the points were.
            keepPoints(player);
            *g_scriptedCamera = 0;
        }
        updateLevelUpFlash(player);

        // The wall tap's sparks, from both skates, at most every 25 ms, with
        // no vertical speed.
        if (player->wallTapping && ee::Float(25.0f) < *g_clockMs - *g_lastWallTapSparks)
        {
            for (uint32_t skate = kLeftSkate; skate <= kRightSkate; ++skate)
            {
                *tapAt = pointNow(skate, player);
                tapAt->y = player->position.y - *g_wallTapDrop;
                // The speed reaches VU0 with the point's z and w in its register's upper half.
                *tapVelocity = zge::scaledCarrying(player->velocity, *g_wallTapSpeed, upperHalf(*tapAt));
                tapVelocity->y = 0.0f;
                spawnWallTapRipple(player, tapAt, tapVelocity);
            }
            *g_lastWallTapSparks = *g_clockMs;
        }

        // The grind's sparks (or the jizzed skater's fire trails).
        const uint32_t grind = player->lastGrindState;
        if (grind == static_cast<uint32_t>(skater::State::HandGrind))
        {
            // From the leading hand.
            sparkFrom(sparkAt, sparkVelocity, player, player->fakie ? kRightHand : kLeftHand, nullptr, *g_handGrindSpeed, kSparkTurn);
            lift(sparkVelocity);
            grindSparks(player, sparkAt, sparkVelocity, sparkCount(player));
        }
        else if (grind == static_cast<uint32_t>(skater::State::Grind))
        {
            // From each skate below the skater's middle.
            const ee::Float low = player->position.y - ee::Float(0.25f);
            *sparkAt = pointNow(kLeftSkate, player);
            if (low <= sparkAt->y)
            {
                const ee::Float drop = *g_leftGrindDrop;
                sparkFrom(sparkAt, sparkVelocity, player, kLeftSkate, &drop, *g_leftGrindSpeed, kSparkTurn);
                lift(sparkVelocity);
                grindSparks(player, sparkAt, sparkVelocity, sparkCount(player));
            }
            *sparkAt = pointNow(kRightSkate, player);
            if (low <= sparkAt->y)
            {
                const ee::Float drop = *g_rightGrindDrop;
                sparkFrom(sparkAt, sparkVelocity, player, kRightSkate, &drop, *g_rightGrindSpeed, kSparkTurn);
                lift(sparkVelocity);
                grindSparks(player, sparkAt, sparkVelocity, sparkCount(player));
            }
        }
        else if (grind == static_cast<uint32_t>(skater::State::DoubleGrind))
        {
            // From both skates, turned apart.
            const ee::Float drop = *g_doubleGrindDrop;
            const ee::Float speed = *g_doubleGrindSpeed;
            sparkFrom(sparkAt, sparkVelocity, player, kLeftSkate, &drop, speed, static_cast<zge::Angle>(-kSparkTurn));
            lift(sparkVelocity);
            grindSparks(player, sparkAt, sparkVelocity, sparkCount(player));
            sparkFrom(sparkAt, sparkVelocity, player, kRightSkate, &drop, speed, kSparkTurn);
            lift(sparkVelocity);
            grindSparks(player, sparkAt, sparkVelocity, sparkCount(player));
        }
        else if (aggressive::skater::isJizzed(player) && player->splashed != 1)
        {
            fireTrail(&pointNow(kLeftSkate, player), &pointLast(kLeftSkate, player));
            fireTrail(&pointNow(kRightSkate, player), &pointLast(kRightSkate, player));
        }
        player->lastGrindState = 0;

        // The trails: the hands' at top speed, the skates' two kinds.
        if (physics(kPhysicsMaxSpeed) <= player->speed)
        {
            startTrail(player->number, kLeftHand, 0);
            startTrail(player->number, kRightHand, 0);
        }
        if (player->flags & kSkatesTrailFlag)
        {
            startTrail(player->number, kLeftSkate, 2);
            startTrail(player->number, kRightSkate, 2);
        }
        if (player->startTrails)
        {
            startTrail(player->number, kLeftSkate, 1);
            startTrail(player->number, kRightSkate, 1);
            player->startTrails = 0;
        }
        const int32_t level = *g_levelWord;
        if (level == 2 || level == 3)
            breakHit(player, broke);
    }
    READABLE_FUNCTION_WAITS(skaterEffects, Verified);

    namespace
    {
        // A trail's spawn record (a "vapor"): the template, then the point
        // now and last frame, and by kind its texture (and for the hands'
        // its alpha and life).
        inline ee::Global<VaporParams, sym::trailTemplate> g_trailTemplate;
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject; // the scene object the named effects are spawned on (0: none yet; fx/spawns.cpp)
        constexpr uint8_t kTopSpeedTrailAlpha = 0x10;
        constexpr uint32_t kTopSpeedTrailLife = 600;    // milliseconds
    }

    // startTrail (player number, point, kind): a trail from one of the
    // four points (its position now and last frame): kind 0 the hands' at
    // top speed (the turbo streak, faint, living 600 ms), 1 the trick
    // streak and 2 the drop streak from the skates. Returns the spawn's
    // result (with no effects object, the texture set, or 2 for another kind).
    int32_t startTrail(uint32_t playerNumber, uint32_t point, uint32_t kind)
    {
        ee::Local<VaporParams> trail;
        std::memcpy(&*trail, g_trailTemplate.get(), sizeof(VaporParams));
        trail->head = (*g_pointsNow)[point & 0xFFu][playerNumber & 0xFFu];
        trail->tail = (*g_pointsLast)[point & 0xFFu][playerNumber & 0xFFu];
        int32_t result = 0;
        if (kind == 1)
        {
            result = static_cast<int32_t>(*g_trickStreakTexture);
            trail->texture = *g_trickStreakTexture;
        }
        else if (kind == 0)
        {
            result = static_cast<int32_t>(*g_turboStreakTexture);
            trail->colour[3] = kTopSpeedTrailAlpha;
            trail->texture = *g_turboStreakTexture;
            trail->lifetime = kTopSpeedTrailLife;
        }
        else if (kind == 2)
        {
            result = static_cast<int32_t>(*g_dropStreakTexture);
            trail->texture = *g_dropStreakTexture;
        }
        else
            result = 2;
        if (*g_effectsObject)
            result = zge::spawnNamedEffect(*g_vaporHash, *g_effectsObject, trail);
        return result;
    }
    READABLE_FUNCTION(startTrail, Verified);

    // ---- the skater's reflection (the Tutorial's floor) --------------------
    namespace
    {
        // Per player: the skater's scene, and a copy of it drawn upside down.
        struct SkaterReflection
        {
            ee::Ptr<zge::Scene> scene; // +0
            ee::Ptr<zge::Scene> copy;  // +4
        };
        static_assert(sizeof(SkaterReflection) == 8);
        inline ee::Global<SkaterReflection[2], sym::g_skaterReflections> g_skaterReflections;
        inline ee::Global<ee::Float, sym::g_reflectionHeight> g_reflectionHeight; // the floor's y
        constexpr uint32_t kSetNodeMatrix = sym::setNodeLocalMatrix;    // (node, matrix *)
        constexpr uint32_t kCopyBitsOff = 4;
    }

    // makeSkaterReflection: the player's reflection made, once.
    void makeSkaterReflection(ee::Ptr<Player> player)
    {
        SkaterReflection &reflection = (*g_skaterReflections)[player->number];
        if (reflection.copy)
            return;
        reflection.scene = zge::sceneOf(player->node);
        reflection.copy = zge::cloneScene(reflection.scene, 0);
        game::tail(zge::changeSceneState, reflection.copy, kCopyBitsOff, 0);
    }
    READABLE_FUNCTION_WAITS(makeSkaterReflection, Verified);

    // releaseSkaterReflection: the player's reflection let go.
    void releaseSkaterReflection(ee::Ptr<Player> player)
    {
        SkaterReflection &reflection = (*g_skaterReflections)[player->number];
        if (reflection.copy != 0)
        {
            zge::releaseScene(reflection.copy);
            reflection.scene = {};
            reflection.copy = {};
        }
    }
    READABLE_FUNCTION_WAITS(releaseSkaterReflection, Verified);

    // reflectSkater: the copy takes the skater's pose, its root mirrored
    // about the floor.
    void reflectSkater(ee::Ptr<Player> player)
    {
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Vec3> flip;
        const SkaterReflection &reflection = (*g_skaterReflections)[player->number];
        if (reflection.copy != 0)
        {
            zge::copyScenePose(reflection.copy, reflection.scene);
            const ee::Ptr<zge::Node> root = zge::sceneRootNode(reflection.scene);
            const ee::Ptr<zge::Node> copyRoot = zge::sceneRootNode(reflection.copy);
            *matrix = root->local;
            const ee::Float height = *g_reflectionHeight;
            matrix->m[3][1] = height - (matrix->m[3][1] - height);
            flip->x = ee::Float(1.0f);
            flip->y = ee::Float(-1.0f);
            flip->z = ee::Float(1.0f);
            zge::matrixScale(matrix, matrix, flip);
            zge::setNodeLocalMatrix(copyRoot, matrix);
        }
    }
    READABLE_FUNCTION_WAITS(reflectSkater, Verified);

    // clearTrailsAfterRespawn: at a respawn, the trails cleared and the camera's scripted
    // flag set.
    void clearTrailsAfterRespawn()
    {
        *g_scriptedCamera = 1;
        *g_clearFiretrails = 1;
    }
    READABLE_FUNCTION(clearTrailsAfterRespawn, Verified);

    namespace
    {
        // The breakables touched in level 3 ("parkEditorAllEditorPiecesTouched"
        // asks whether they all are): 20 slots, and how many are taken.
        constexpr uint32_t kTouchedBreakables = sym::touchedBreakables;
        inline ee::Global<uint32_t[20], sym::touchedBreakables> g_touchedBreakables;
        constexpr uint32_t kTouchedSlots = 20;
        inline ee::Global<int32_t, sym::g_touchedBreakableCount> g_touchedBreakableCount;
    }

    // countBreakables: the level's breakables, counted.
    int32_t countBreakables()
    {
        int32_t count = 0;
        ee::Ptr<level::Breakable> breakable = aggressive::level::firstBreakable();
        if (breakable)
        {
            for (;;)
            {
                ++count;
                breakable = aggressive::level::nextBreakable(breakable);
                if (!breakable)
                    break;
            }
        }
        return count;
    }
    READABLE_FUNCTION(countBreakables, Verified);

    // clearTouchedBreakables: "parkEditorResetTouchesPiece": none touched. (0x50
    // bytes: the whole of the slots.)
    void clearTouchedBreakables()
    {
        zge::memoryFill(kTouchedBreakables, 0, kTouchedSlots * 4u);
        *g_touchedBreakableCount = 0;
    }
    READABLE_FUNCTION(clearTouchedBreakables, Verified);

    // touchBreakable: a breakable touched: counted once, in the last free slot
    // (all 20 taken: the one past them, as the original has it).
    void touchBreakable(ee::Address breakable)
    {
        const ee::Ptr<uint32_t> slots(&(*g_touchedBreakables)[0]);
        uint32_t freeSlot = kTouchedSlots;
        for (uint32_t slot = 0; slot < kTouchedSlots; ++slot)
        {
            const uint32_t held = slots[slot];
            if (!held)
                freeSlot = slot;
            if (held == breakable)
                return;
        }
        slots[freeSlot] = breakable;
        *g_touchedBreakableCount = *g_touchedBreakableCount + 1;
    }
    READABLE_FUNCTION(touchBreakable, Verified);
}
