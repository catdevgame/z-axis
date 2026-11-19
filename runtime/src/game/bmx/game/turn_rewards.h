#pragma once
// BMX XXX's rewards at a turn's end (game.c): what a one-rider game's turn
// won, given into the saved game and marked in the rider's turn awards for
// the reward screen (overlay/screen_reward.cpp shows them a page each).
// BMX XXX's own code.

#include <cstdint>

namespace game::bmx
{
    // RiderObjectives::turnAwards (objectives/objectives.h): a bit for each
    // reward the turn's end gave. Bits 0 .. 19: the turn's reward slots (the
    // level's goal rewards, or its three place rewards); the rest by name.
    constexpr uint64_t kAwardAllGaps = 1ull << 32;      // the level's gaps all done (g_allGapsRewards)
    constexpr uint64_t kAwardLevelGoals = 1ull << 33;   // the level's last goal reward: all its goals done
    constexpr uint64_t kAwardContestWon = 1ull << 34;   // the level's first place reward
    constexpr uint64_t kAwardAllGoals = 1ull << 49;     // every challenge level's goals done (g_allGoalsReward)
    constexpr uint64_t kAwardAllContests = 1ull << 50;  // every competition won (g_allContestsReward)
    constexpr uint64_t kAwardAllGame02 = 1ull << 52;    // both of those (g_allGame02Reward)

    // The turn's rewards for a player (setRoundPhase, at a turn's end), in a
    // game that is not multiplayer.
    void giveTurnRewards(uint8_t player);
}
