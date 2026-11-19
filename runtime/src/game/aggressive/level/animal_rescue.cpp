// Animal Rescue: the animals a skater carries.

#include "aggressive/level/animal_rescue.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/skater/skater.h"

#include "zge/scene/carried_objects.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "aggressive/level/props.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/animal_rescue.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kSetSkaterPoints = sym::setSkaterPoints;     // (player, points), hooked by a game override: by address
        constexpr uint32_t kLeftUpperArm = sym::g_textLUpperArm, kRightUpperArm = sym::g_textRUpperArm, kLeftHand = sym::g_textLHand; // bones
        constexpr uint32_t kMostCarried = 3;
    }

    // pickUpAnimal: a third of the skater's arms and hand hold one each. In
    // split screen the animals carried are the points.
    int32_t pickUpAnimal(ee::Ptr<skater::Player> player, ee::Ptr<zge::SceneObject> animal)
    {
        ee::Local<uint32_t> nameSlot;
        if (player->carriedAnimalCount >= kMostCarried)
            return 0;
        const uint32_t count = player->carriedAnimalCount;
        const auto bone = [&](uint32_t name) {
            *nameSlot = zge::nameHash(ee::Ptr<const char>(name));
        };
        if (count == 1)
            bone(kRightUpperArm);
        else if (count == 0)
            bone(kLeftUpperArm);
        else
            bone(kLeftHand);
        zge::attachCarriedAnimal(player->node, animal, nameSlot);
        player->carriedAnimals[player->carriedAnimalCount] = animal;
        player->carriedAnimalCount = static_cast<uint16_t>(player->carriedAnimalCount + 1);
        if (*g_gameType == GameType::SplitScreen)
            skater::setSkaterPoints(player, player->carriedAnimalCount);
        return 1;
    }
    READABLE_FUNCTION_WAITS(pickUpAnimal, Verified);

    namespace
    {
        namespace tail
        {
        int32_t setSkaterPoints(ee::Ptr<skater::Player> player, uint32_t points) { return ee::tailCall<int32_t>(kSetSkaterPoints, player, points); }
        }
    }

    // dropAnimals: every animal let go: rescued (`rescued`, a byte) into the
    // count and hidden, or sent back; in split screen the points (the animals
    // carried) back to 0 unless they were rescued. The result is what the
    // original leaves in its result register, which its callers hand on: 0
    // when it carried none, else the game state's address (it read the game
    // type through it) or setSkaterPoints'.
    int32_t dropAnimals(ee::Ptr<skater::Player> player, uint32_t rescued)
    {
        const bool wasRescued = (rescued & 0xFFu) != 0;
        if (player->carriedAnimalCount == 0)
            return 0;
        for (ee::Ptr<zge::SceneObject> &animal : player->carriedAnimals)
        {
            if (!animal)
                continue;
            zge::detachAnimal(animal);
            if (wasRescued)
            {
                player->droppedAnimals = static_cast<uint16_t>(player->droppedAnimals + 1);
                zge::setSceneObjectShown(animal, 0);
            }
            else
                level::returnRescueAnimal(animal);
            animal = {};
        }
        player->carriedAnimalCount = 0;
        if (g_gameState->type != GameType::SplitScreen || wasRescued)
            return static_cast<int32_t>(sym::g_debugDisplay);
        return tail::setSkaterPoints(player, 0);
    }
    READABLE_FUNCTION_WAITS(dropAnimals, Verified);
}
