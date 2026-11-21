// BMX XXX's rewards at a turn's end (turn_rewards.h). BMX XXX's own code
// (game.c, its game block).

#include "turn_rewards.h"

#include "player_riders.h"
#include "round.h"

#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/bmx/game/turn_rewards.cpp.h"

namespace game::bmx
{
    using objectives::giveReward;
    using objectives::Reward;
    using objectives::RiderObjectives;
    using objectives::riderObjectives;

    namespace
    {
        using namespace objectives; // the goal marks (GoalMark)

        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // The reward tables: twenty a level for its goals (the slot is how
        // many goals were done before it), three a level for a competition's
        // places (third, second, first), one a level for all its gaps, and
        // the three of the whole game.
        constexpr uint32_t kLevelGoalRewards = 20, kLevelPlaceRewards = 3;
        ee::Ptr<Reward> goalReward(uint32_t level, uint32_t slot)
        {
            return ee::Ptr<Reward>(sym::g_goalRewards) + (level * kLevelGoalRewards + slot);
        }
        ee::Ptr<Reward> placeReward(uint32_t level, uint32_t slot)
        {
            return ee::Ptr<Reward>(sym::g_placeRewards) + (level * kLevelPlaceRewards + slot);
        }
        ee::Ptr<Reward> allGapsReward(uint32_t level) { return ee::Ptr<Reward>(sym::g_allGapsRewards) + level; }
        constexpr ee::Ptr<Reward> kAllGoalsReward{sym::g_allGoalsReward};
        constexpr ee::Ptr<Reward> kAllContestsReward{sym::g_allContestsReward};
        constexpr ee::Ptr<Reward> kAllGame02Reward{sym::g_allGame02Reward};
        // The place reward that is the first place's.
        constexpr uint32_t kFirstPlaceSlot = kLevelPlaceRewards - 1;

        // The competition's field by place, first to tenth (a rider
        // character each); the first three win a place reward.
        constexpr uint32_t kCompetitionRiders = 10, kPlaces = 3;
        inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRanking> g_competitionRanking;

        // The levels a game's whole-game reward counts: the six challenge
        // levels and the two competitions (LevelInfo::gameMode tells them
        // apart).
        constexpr uint32_t kTourLevels = 8;
        const ee::Ptr<LevelInfo> kLevelInfos(sym::g_levels);

        // A turn's reward slot marked in the awards: its bit, set in the
        // 32-bit word of the awards that holds it.
        // The turn awards as the 32-bit words the marks are set in.
        ee::Ptr<uint32_t> awardWords(ee::Ptr<RiderObjectives> objectives) { return ee::Ptr<uint64_t>(&objectives->turnAwards).cast<uint32_t>(); }
        void markSlotAward(ee::Ptr<RiderObjectives> objectives, uint32_t slot)
        {
            const ee::Ptr<uint32_t> words = awardWords(objectives);
            words[slot / 32] |= 1u << (slot % 32);
        }
    }

    // giveTurnRewards: in the challenges, the goal rewards from the count of
    // goals done before the turn to the count done now, and the level's gap
    // reward once a gap done this turn makes all its gaps done. In the
    // competition, at the game's last round, the place rewards from the
    // character's best place before to the place won now (the best place
    // kept). The level's last goal reward, or its first place's, marks the
    // level; once all the challenge levels (or both competitions) are
    // marked, the game's reward for them, and the third reward once the
    // character has both. Each reward given is marked in the rider's turn
    // awards.
    void giveTurnRewards(uint8_t player)
    {
        if ((gameModeRecord()->flags & kModeMultiplayer) != 0)
            return;
        const PlayerSetup &setup = (*g_playerSetups)[player];
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        const ee::Ptr<RiderObjectives> objectives = riderObjectives(playerRider(player));

        // The reward slots the turn reached: from `first` up to `reached`.
        ee::Ptr<LevelGoals> goals;
        uint32_t first = 0;
        uint32_t reached = 0;
        if (gameMode() == kChallengeMode)
        {
            goals = playerLevelGoals(player, *g_currentLevel);
            for (uint32_t goal = 0; goal < kLevelObjectives; ++goal)
            {
                const uint8_t mark = goals->marks[goal];
                if ((mark & kGoalSaved) == 0 || (mark & kGoalDone) == 0)
                    continue;
                if ((mark & kGoalGap) == 0)
                {
                    ++reached;
                    if ((mark & kGoalDoneThisTurn) == 0)
                        ++first;
                }
                else if ((mark & kGoalDoneThisTurn) != 0 && goals->gapsDone == goals->gaps)
                {
                    objectives->turnAwards |= kAwardAllGaps;
                    giveReward(player, allGapsReward(*g_currentLevel));
                }
            }
        }
        else if (gameMode() == kCompetitionMode)
        {
            goals = playerLevelGoals(player, *g_currentLevel);
            first = goals->placeRewards;
            if (!roundFlags(kGameEndsFlag))
                return;
            // The place rewards the place won reaches: 3 for the first place,
            // 1 for the third, 0 below.
            for (uint32_t place = 0; place < kPlaces; ++place)
            {
                if ((*g_competitionRanking)[place] == setup.character)
                    reached = kPlaces - place;
            }
            if (first >= reached)
                return;
            goals->placeRewards = static_cast<uint8_t>(reached);
        }
        else
        {
            return;
        }
        if (first >= reached)
            return;

        for (uint32_t slot = first; slot < reached; ++slot)
        {
            markSlotAward(objectives, slot);
            ee::Ptr<Reward> reward;
            if (gameMode() == kChallengeMode)
            {
                reward = goalReward(*g_currentLevel, slot);
                if (slot == goals->goals - 1u)
                {
                    objectives->turnAwards |= kAwardLevelGoals;
                    goals->flags |= kLevelGoalsAllDone;
                }
            }
            else
            {
                reward = placeReward(*g_currentLevel, slot);
                if (slot == kFirstPlaceSlot)
                {
                    objectives->turnAwards |= kAwardContestWon;
                    goals->flags |= kLevelGoalsContestWon;
                }
            }
            giveReward(player, reward);

            if ((objectives->turnAwards & (kAwardLevelGoals | kAwardContestWon)) == 0)
                continue;
            // The levels of the mode's kind, and how many of them are marked.
            // (The level records read here take `goals`' place for the
            // slots still to give, as in the original.)
            uint32_t levels = 0;
            uint32_t marked = 0;
            for (uint32_t level = 0; level < kTourLevels; ++level)
            {
                goals = playerLevelGoals(player, level);
                const ee::Ptr<LevelInfo> info = kLevelInfos + level;
                if (gameMode() == kChallengeMode && info->gameMode == kChallengeMode)
                {
                    ++levels;
                    if ((goals->flags & kLevelGoalsAllDone) != 0)
                        ++marked;
                }
                else if (gameMode() == kCompetitionMode && info->gameMode == kCompetitionMode)
                {
                    ++levels;
                    if ((goals->flags & kLevelGoalsContestWon) != 0)
                        ++marked;
                }
            }
            if (marked != levels || levels == 0)
                continue;

            ee::Ptr<Reward> gameReward;
            if (gameMode() == kChallengeMode)
            {
                objectives->turnAwards |= kAwardAllGoals;
                gameReward = kAllGoalsReward;
                character->flags |= kCharacterAllGoals;
            }
            else if (gameMode() == kCompetitionMode)
            {
                objectives->turnAwards |= kAwardAllContests;
                gameReward = kAllContestsReward;
                character->flags |= kCharacterAllContests;
            }
            if (!gameReward)
                continue;
            giveReward(player, gameReward);
            if ((character->flags & (kCharacterAllGoals | kCharacterAllContests)) == (kCharacterAllGoals | kCharacterAllContests))
            {
                objectives->turnAwards |= kAwardAllGame02;
                character->flags |= kCharacterAllGame;
                giveReward(player, kAllGame02Reward);
            }
        }
    }
    READABLE_FUNCTION(giveTurnRewards, Verified);
}
