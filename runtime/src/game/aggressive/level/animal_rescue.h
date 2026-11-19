#pragma once

// Animal Rescue (animal_rescue.cpp): the animals a skater picks up and drops.

#include "aggressive/skater/skater.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive
{
    // (animal_rescue.cpp)
    int32_t dropAnimals(ee::Ptr<skater::Player> player, uint32_t rescued);
    int32_t pickUpAnimal(ee::Ptr<skater::Player> player, ee::Ptr<zge::SceneObject> animal);
}
