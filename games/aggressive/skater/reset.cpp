// The skater's physics set back (were the sweep's resetSkaterPhysics and clearCarriedAnimals):
// at the level's start (the players' setup, setupSkaterForRun) and when the
// skater is put at its start (placeSkaterAtLevelStart): Ground, no surface,
// object or rail, no rates or timers, not down; and optionally its pose
// (upright at the origin, still). The Animal Rescue animals it carries
// are let go of the same way at the setup.
//

#include "skater.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "moves.h"
#include "sym/aggressive/skater/reset.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kNoFace = 0xFFFFFFFFu;
        // Player::flags bit 0: the reset clears it with kBraking (no code read so far sets it).
        constexpr uint32_t kFlagBit0 = 0x1;
        // The landing grade the reset leaves (Player::landing: 1, not clean).
        constexpr uint8_t kResetLanding = 1;
    }

    // resetSkaterPhysics: the skater's physics state set back; with `clearPose`
    // its orientation (upright: up is (0, -1, 0)), position, velocity and
    // last position too.
    void resetSkaterPhysics(ee::Ptr<Player> player, uint8_t clearPose)
    {
        player->resetBytes[0] = 0;
        player->resetBytes[1] = 0;
        player->resetBytes[2] = 0;
        player->resetBytes[3] = 0;
        player->state = State::Ground;
        player->previousState = 0;
        player->groundNormal = zge::kZero;
        player->surfaceNormal = zge::kZero;
        player->contactObject = 0;
        player->object = 0;
        player->surface = kNoFace;
        player->contact = kNoFace;
        player->lastObject = 0;
        player->lastRail = {};
        player->doubleGrindRails = {};
        player->overObject = 0;
        player->overSurfaceKind = 0;
        if (clearPose)
        {
            zge::matrixIdentity(&player->orientation);
            zge::matrixClearTranslation(&player->orientation);
            player->orientation.m[1][1] = -1.0f; // up is (0, -1, 0): the game's y points down
            player->position = zge::kZero;
            player->velocity = zge::kZero;
            player->lastPosition = zge::kZero;
        }
        // The rates (pitch, turn and the next two words) and the pole's vector.
        player->rates = zge::kZero;
        player->poleVector = zge::kZero;
        player->expectedAirTime = 0.0f;
        player->swingMade = 0.0f;
        player->airTime = 0.0f;
        player->speed = 0.0f;
        player->pushSpeed = 0.0f;
        player->onMovingSurface = 0;
        player->movingObject = 0;
        player->grindRail = {};
        player->grindMesh = 0;
        player->wallContact.object = {};
        player->wallContact.part = {};
        player->skidFlags = 0;
        player->landing = kResetLanding;
        setSkaterDown(player, 0);
        player->flags = player->flags & ~kBraking & ~kFlagBit0;
        player->balance[1] = 0.0f;
        player->grindTime = 0.0f;
        player->shortCount = 0;
        player->holdTime = 0.0f;
        player->manualTime = 0.0f;
        player->wallRideTime = 0.0f;
        player->totalAirTime = 0.0f;
        player->stateTime = 0.0f;
        player->balance[0] = 0.0f;
    }
    READABLE_FUNCTION_WAITS(resetSkaterPhysics, Verified);

    // clearCarriedAnimals: Animal Rescue: no animals carried (the three slots
    // Player::carriedAnimals emptied, the counts cleared).
    void clearCarriedAnimals(ee::Ptr<Player> player)
    {
        for (int32_t slot = 2;;)
        {
            player->carriedAnimals[slot] = {};
            if (--slot < 0)
                break;
        }
        player->droppedAnimals = 0;
        player->carriedAnimalCount = 0;
    }
    READABLE_FUNCTION(clearCarriedAnimals, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_parkCost> g_parkCost;
    }

    void resetParkCost()
    {
        *g_parkCost = 0;
    }
    READABLE_FUNCTION(resetParkCost, Verified);
}
