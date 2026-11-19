// A player's frame (playerUpdate, called for each player by the level's
// update updatePlayers): the controls and the physics, then what the world
// sees of it: the scene node turned smoothly toward the physics'
// orientation (turned round when fakie), the rolling sound, the trigger zones, a fall out of the
// level, the bone sway and the score.

#include "skater.h"
#include "zge/collision/dynamics.h"
#include "aggressive/level/double_rails.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/sweep.h"
#include "zge/math/vec.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/platform/platform.h"
#include "aggressive/level/level_collision.h"
#include "sce/libgcc.h"
#include "moves.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/library.h"
#include "sym/aggressive/skater/player.cpp.h"

namespace aggressive::skater
{
    int32_t updateSkaterBox(ee::Ptr<Player> player);

    namespace
    {
        constexpr uint32_t kRespawnFall = 1;                  // respawn's third argument after a fall

        // Rolling sound kinds (updateRollSound's second argument): a surface's
        // or a rail's sound with these bits added.
        constexpr int32_t kRollGrind = 0x100;
        constexpr int32_t kRollStall = 0x200;
        constexpr int32_t kRollWall = 0x400;    // a wall ride, or the ground's scrape when braking
        constexpr int32_t kRollAir = 0x2000;
        constexpr int32_t kRollCessSlide = 0x4000;
        constexpr uint8_t kRollWallTap = 0x0D; // a surface sound (low byte) that marks Player::wallTapping
        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;
        constexpr int32_t kNoRailSound = 1;

        // The level index (g_levelNumber) and the frame time scaled to 60 Hz.
        inline ee::Global<ee::Float, sym::g_growthLevel0> g_growthLevel0;   // 0.01 a second into levelGrowth[1]
        inline ee::Global<ee::Float, sym::g_growthLevel3a> g_growthLevel3a;  // 0.007 into levelGrowth[0]
        inline ee::Global<ee::Float, sym::g_growthLevel3b> g_growthLevel3b;  // 0.01 into levelGrowth[2]
        // The drawn orientation is turned only once it is this far from the
        // physics' (squared distance of the right and up axes summed).
        inline ee::Global<ee::Float, sym::g_turnThreshold> g_turnThreshold;  // 0.04
        inline ee::Global<ee::Float, sym::g_turnRateSlow> g_turnRateSlow;   // 0.1 of the way per 60th: grinds, ramps' air
        inline ee::Global<ee::Float, sym::g_turnRate2> g_turnRate;       // 0.2
        inline ee::Global<ee::Float, sym::g_minSwayTime> g_minSwayTime;    // 0.005: no sway for shorter frames
        // General / Death Plane: the height below which a skater has fallen
        // out of the level (y grows downward).
        constexpr uint32_t kTuningDeathPlane = 0;

        // The node follows the physics: position as is, orientation turned
        // toward the physics' a part of the way each frame (snapped once
        // after kDrawnSnap), and drawn half turned when riding fakie.
        void updateDrawnOrientation(ee::Ptr<Player> player, ee::Float frameTime)
        {
            ee::Local<zge::Matrix> target, drawn;
            ee::Local<zge::Quaternion> turned, halfTurn;
            ee::Local<zge::Vec4> secondApart, firstApart;
            ee::Local<zge::Matrix> turnedMatrix;
            zge::Quaternion *drawnOrientation = &player->drawnOrientation;

            // The physics' orientation as a rotation matrix, its axes flipped
            // (the model faces down its -z). `target` is the frame's first
            // local, where the original keeps it, and it outlives this frame:
            // the lowest byte of its third row's x is what soundEvent 30 takes
            // as its volume when an NPC is hit later in the same game frame
            // (npcSkaterCollision, npcHitBySkater; nothing writes that byte in
            // between).
            zge::matrixIdentity(&*target);
            zge::matrixCopyRotation(target, &player->orientation);
            for (int row = 0; row < 3; ++row)
            {
                zge::Vec3 *axis = &target->row(row).xyz();
                zge::vec3Negate(axis, axis);
            }
            zge::quaternionToMatrix(&*drawn, drawnOrientation);

            *firstApart = drawn->row(0) - target->row(0);
            *secondApart = drawn->row(1) - target->row(1);
            const ee::Float first = zge::squaredLength(*firstApart);
            const ee::Float apart = first + zge::squaredLength(*secondApart);
            if (*g_turnThreshold < apart || player->drawnRefresh)
            {
                zge::matrixToQuaternion(&*turned, &*target);
                if (player->drawFlags & kDrawnSnap)
                {
                    zge::quaternionCopy(drawnOrientation, &*turned);
                    player->drawFlags &= ~kDrawnSnap;
                }
                else
                {
                    const State state = player->state;
                    const bool slow = state == State::Grind || state == State::HandGrind ||
                                      state == State::AirQuarterPipe || state == State::AirSpine;
                    if (slow)
                    {
                        const ee::Float t = frameTime * ee::Float(60.0f) * *g_turnRateSlow;
                        zge::quaternionSlerp(drawnOrientation, drawnOrientation, &*turned, t);
                    }
                    else
                    {
                        const ee::Float t = frameTime * ee::Float(60.0f) * *g_turnRate;
                        zge::quaternionSlerp(drawnOrientation, drawnOrientation, &*turned, t);
                    }
                }
                zge::quaternionCopy(&*turned, drawnOrientation);

                if (player->fakie)
                {
                    // Half a turn about the drawn up axis.
                    zge::quaternionToMatrix(&*turnedMatrix, &*turned);
                    zge::quaternionFromAxisAngle(&*halfTurn, &turnedMatrix->row(1).xyz(), zge::Angle(-0x8000));
                    zge::quaternionMultiply(&*turned, &*halfTurn, &*turned);
                }
                player->drawnRefresh = 0;
                zge::setObjectOrientation(player->node, turned);
            }
            zge::setObjectPosition(player->node, &player->drawnPosition);
        }

        // The rolling sound's kind for the state the skater is in; sets the
        // sound's speed (rollSoundSpeed) where the state has one.
        int32_t rollingSound(ee::Ptr<Player> player)
        {
            const ee::Ptr<zge::Node> object = player->objectNode;
            const uint32_t surface = player->surface;
            const bool onSurface = object && surface != kNoSurface;
            int32_t sound = 0;
            switch (player->state)
            {
            case State::Stand:
                player->rollSoundSpeed = 0;
                break;
            case State::Air:
            case State::AirQuarterPipe:
            case State::AirSpine:
            case State::VerticalPole:
            case State::VerticalPoleA:
            case State::HorizontalPole:
            case State::Vault:
                sound = kRollAir;
                break;
            case State::Stall:
                sound = railMaterial(player->grindRail, player->grindSegment) + kRollStall;
                break;
            case State::Grind:
            case State::HandGrind:
                sound = railMaterial(player->grindRail, player->grindSegment);
                if (sound != kNoRailSound)
                    player->lastGrindState = static_cast<uint32_t>(player->state);
                sound += kRollGrind;
                break;
            case State::DoubleGrind:
            {
                const level::DoubleRail &rails = *player->doubleGrindRails;
                sound = railMaterial(rails.node, rails.edge);
                if (sound != kNoRailSound)
                    player->lastGrindState = static_cast<uint32_t>(State::DoubleGrind);
                sound += kRollGrind;
                break;
            }
            case State::WallRide:
                wallRideContact(player);
                if (object)
                {
                    if (surface != kNoSurface)
                        sound = skater::surfaceSound(object, surface);
                }
                sound += kRollWall;
                break;
            case State::WallTap:
                break;
            case State::CessSlide:
                sound = kRollCessSlide;
                break;
            case State::Skitch:
                player->rollSoundSpeed = sce::libgcc::fptoui(zge::vec3Length(&player->carriedMove.xyz()) *
                                     ee::Float(3600.0f));
                if (!onSurface)
                    return 0;
                sound = skater::surfaceSound(object, surface);
                break;
            case State::Manual:
                if (!onSurface)
                    return 0;
                sound = skater::surfaceSound(object, surface);
                break;
            case State::Ground:
            default: // (Ground the table's; past the table a branch)
                player->rollSoundSpeed = sce::libgcc::fptoui(player->speed * ee::Float(60.0f));
                if (onSurface)
                    sound = skater::surfaceSound(object, surface);
                if (player->flags & kBraking)
                    sound += kRollWall;
                break;
            }
            return sound;
        }
    }

    void playerUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        if (!player->active)
            return;

        // Time played, in all and in this level; levels 0 and 3 grow levelGrowth.
        player->timePlayed = player->timePlayed + frameTime;
        ee::Float &levelTime = player->statsByLevel[*g_levelWord].secondsPlayed;
        levelTime = levelTime + frameTime;
        if (*g_levelWord == 0)
            player->levelGrowth[1] = player->levelGrowth[1] + frameTime * *g_growthLevel0;
        else if (*g_levelWord == 3)
        {
            const ee::Float a = frameTime * *g_growthLevel3a;
            const ee::Float b = frameTime * *g_growthLevel3b;
            player->levelGrowth[0] = player->levelGrowth[0] + a;
            player->levelGrowth[2] = player->levelGrowth[2] + b;
        }

        skaterControlsUpdate(player, frameTime);
        skaterUpdate(player, frameTime);

        player->drawnPosition = player->position;
        updateDrawnOrientation(player, frameTime);
        updateSkaterBox(player);

        const uint32_t onObject = player->object, onSurface = player->surface;
        const int32_t sound = rollingSound(player);
        player->wallTapping = (sound & 0xFF) == kRollWallTap ? 1u : 0u;
        aggressive::updateRollSound(player->rollSound, sound, &player->drawnPosition, player->rollSoundSpeed);

        updateTriggerZones(player);

        // Fallen out of the level: back at a locator.
        if ((*g_tuningCategories)[0].entries[kTuningDeathPlane].value < player->drawnPosition.y)
        {
            player->flags |= kRespawnAtLocator;
            respawn(player, 0, kRespawnFall);
        }

        aggressive::fx::skaterEffects(player);
        if (*g_minSwayTime < frameTime)
            skaterSway(player, frameTime);
        juiceStep(player, frameTime);
    }
    READABLE_FUNCTION_WAITS(playerUpdate, Verified);

    namespace
    {
        // The box around the skater's node, in the node's frame: 1.5 wide
        // and deep, from the node down 2.5 (y grows downward).
        inline constexpr ee::Float kBoxHalfWidth = 0.75f;
        inline constexpr ee::Float kBoxHeight = 2.5f;
    }

    // updateSkaterBox: the skater's box in the level's sweep follows its node:
    // the eight corners of the box under the node, turned by the node's
    // rotation, bounded along the world's axes (the bounds start at the
    // node's origin, so they always hold it), moved to the node's position.
    // Returns 1. (playerUpdate passes the frame time too; it is not read.)
    int32_t updateSkaterBox(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> low;
        ee::Local<zge::Vec4> high;
        ee::Local<zge::Vec4> corner;
        ee::Local<zge::Matrix> rotation;

        zge::quaternionToMatrix(&*rotation, zge::objectOrientation(player->node).get());
        *low = zge::kZero;
        *high = zge::kZero;
        constexpr uint32_t kCorners = 8;
        for (uint32_t i = 0; i < kCorners; ++i)
        {
            *corner = zge::kZero;
            // Corners 0-3 at the bottom, 4-7 at the node; bit 1 picks x's
            // side, bit 0 z's.
            if (i & 2)
                corner->x = kBoxHalfWidth;
            else
                corner->x = -kBoxHalfWidth;
            if (i < 4)
                corner->y = -kBoxHeight;
            else
                corner->y = 0.0f;
            if (i & 1)
                corner->z = kBoxHalfWidth;
            else
                corner->z = -kBoxHalfWidth;
            *corner = zge::rotated(*rotation, *corner);
            const ee::Float x = corner->x, y = corner->y, z = corner->z;
            if (x < low->x)
                low->x = x;
            if (y < low->y)
                low->y = y;
            if (z < low->z)
                low->z = z;
            if (high->x < x)
                high->x = x;
            if (high->y < y)
                high->y = y;
            if (high->z < z)
                high->z = z;
        }

        // The node's position (a quadword copy), added to both bounds.
        *corner = zge::aligned(zge::objectPosition(player->node));
        *low = *low + *corner;
        *high = *high + *corner;

        // The box: three low end points, each with its high one.
        const auto &lowEnds = player->sweepBox->lowEnds;
        lowEnds[0]->value = low->x;
        lowEnds[1]->value = low->y;
        lowEnds[2]->value = low->z;
        lowEnds[0]->other->value = high->x;
        lowEnds[1]->other->value = high->y;
        lowEnds[2]->other->value = high->z;
        const ee::Ptr<zge::Sweep> sweep = levelSweep();
        zge::sweepUpdateBox(sweep, player->sweepBox);
        return 1;
    }
    READABLE_FUNCTION(updateSkaterBox, Verified);

    namespace
    {
        constexpr uint32_t kBoxBoneCount = 6;
        // The bones the box goes round (g_boxBoneNames): "Head", "R_Hand",
        // "L_Hand", "R_Foot", "L_Foot", "Hips"; their hashes, made once.
        inline ee::Global<uint32_t[kBoxBoneCount], sym::g_boxBoneNames> g_boxBoneNames;
        inline ee::Global<uint32_t[kBoxBoneCount], sym::g_boxBoneHashes> g_boxBoneHashes;
        inline ee::Global<uint32_t, sym::g_boxBoneHashesNeeded> g_boxBoneHashesNeeded;

    }

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
    }

    // skaterOrientedBox: the box the rigid bodies hit the skater by: round its six
    // bones' positions, turned as it is drawn. (The centre the original
    // gives is the last bone's position: kept.) 0 without a model.
    uint32_t skaterOrientedBox(ee::Ptr<Player> player, ee::Ptr<zge::OrientedBox> box)
    {
        ee::Local<uint32_t[kBoxBoneCount]> names;
        ee::Local<zge::Vec4> lowest, highest, size, position, origin;
        ee::Local<ee::Ptr<Player>> holder;
        bool first = true;
        *holder = player; // (kept in the frame across the calls, never read back)
        std::memcpy(&names[0], g_boxBoneNames.get(), sizeof(*g_boxBoneNames));
        memset(origin, 0, 0x10);
        if (*g_boxBoneHashesNeeded != 0)
        {
            for (uint32_t bone = 0; bone < kBoxBoneCount; ++bone)
                (*g_boxBoneHashes)[bone] = zge::nameHash(ee::Ptr<const char>(names[bone]));
            *g_boxBoneHashesNeeded = 0;
        }
        zge::memoryFill(box, 0, sizeof(zge::OrientedBox));
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        if (!scene)
            return 0;
        for (uint32_t index = 0; index < kBoxBoneCount; ++index)
        {
            const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, (*g_boxBoneHashes)[index]);
            if (!bone)
                return 0;
            zge::bonePoint(scene, bone, origin, position, 0);
            if (first)
            {
                *lowest = *position;
                *highest = *position;
                first = false;
            }
            else
            {
                highest->x = ee::max(position->x, highest->x);
                lowest->x = ee::min(position->x, lowest->x);
                highest->y = ee::max(position->y, highest->y);
                lowest->y = ee::min(position->y, lowest->y);
                highest->z = ee::max(position->z, highest->z);
                lowest->z = ee::min(position->z, lowest->z);
            }
        }
        *size = *highest - *lowest;
        *size = *size * 0.5f;
        box->halfExtents = *size;
        box->centre = *position;
        zge::quaternionCopy(&box->orientation, &player->drawnOrientation);
        return 1;
    }
    READABLE_FUNCTION(skaterOrientedBox, Verified);
}
