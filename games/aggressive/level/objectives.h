#pragma once

// A level's challenges (objectives.cpp): the script data's objectives, their
// saved state, and what the skater's gaps and grinds do to them.

#include "aggressive/skater/skater.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::level
{
    // An objective's state in a level's saved block.
    struct SavedObjective
    {
        uint32_t state;
        uint32_t done;
    };
    static_assert(sizeof(SavedObjective) == 8);
}

namespace aggressive::level
{
    // (objectives.cpp)
    void awardObjective(uint32_t index, uint32_t state);
    uint32_t bonusObjectiveCount();
    uint32_t bonusObjectives();
    void completeChallenge(uint32_t index, uint32_t state);
    uint32_t currentObjectives();
    void goalGrind(uint32_t rail, uint32_t surface);
    void goalJump(uint32_t object, uint32_t surface);
    void initObjectives();
    void loadObjectiveState(ee::Address saved);
    uint32_t objectiveCount();
    void objectiveCrash(ee::Ptr<skater::Player> player, uint32_t landed);
    ee::Ptr<char> objectiveTitle(uint32_t index);
    uint32_t objectivesComplete();
    ee::Ptr<SavedObjective> saveObjectiveState(ee::Ptr<uint32_t> bytes);
    int32_t showObjectiveText(uint32_t index);
    void tellGoalsOver(ee::Ptr<skater::Player> player, uint32_t object, uint32_t surface);
}
