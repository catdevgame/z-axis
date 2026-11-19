// DM2's rewards (game.c).

#include "rewards.h"

#include "competition.h"
#include "goals.h"
#include "npcs.h"
#include "players.h"
#include "round.h"
#include "dm2/action/action_state.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/game/rewards.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<RiderCharacter[kCharacters], sym::g_riderCharacters> g_riderCharacters;
        // The characters the riders' set counts (the first fourteen), and the
        // created riders' (17 .. 20).
        constexpr uint8_t kRegularCharacters = 14;
        constexpr uint8_t kLastBuiltInCharacter = kBuiltInCharacters - 1;

        // ScoreBlock::goalFlags, the reward bits the reward screen shows: two
        // a challenge or medal (bits 0 .. 9), one a group's (from bit 10), and
        // the game's, the respect's.
        constexpr uint32_t kGroupRewardBit = 10;
        constexpr uint32_t kAllGapsBit = 0x8000;
        constexpr uint32_t kAllLevelsBit = 0x10000;
        constexpr uint32_t kBothAllBit = 0x20000;
        constexpr uint32_t kRespectStepBit = 0x40000;
        constexpr uint32_t kAllCharactersRespectBit = 0x80000;
        constexpr uint32_t kLastCharacterRespectBit = 1u << 20;
        // RiderCharacter::flags the rewards set: a group's challenges done (by
        // group from bit 4) or its medals (by group from bit 6), every
        // challenge done, every gold medal, full respect.
        constexpr uint32_t kCharacterGroupChallenges = 0x10;
        constexpr uint32_t kCharacterGroupMedals = 0x40;
        constexpr uint32_t kCharacterAllChallenges = 0x200;
        constexpr uint32_t kCharacterAllMedals = 0x400;
        constexpr uint32_t kCharacterFullRespect = 0x800;
        // The last challenge, and the medal index (3) the competition's own
        // checks name: its places give medals 0 .. 2 only, so its "last" is
        // never reached.
        constexpr uint32_t kLastChallenge = kLevelChallenges - 1;
        constexpr uint32_t kCompetitionLast = kMedals;
        // LevelGoals::flags the competition's "last" would set (never: see above).
        constexpr uint8_t kLevelCompetitionDone = 0x20;

        // Each character's own four tricks, six bytes an entry, the trick's
        // number first (0xFF for none).
        constexpr ee::Ptr<const uint8_t> kOwnTricks{sym::g_characterOwnTricks};
        constexpr uint32_t kOwnTricksEach = 4, kOwnTrickBytes = 6;
        constexpr uint8_t kNoTrick = 0xFF;
        // kGivesNpcTrick's NPC: the one giving the level's challenge 3.
        constexpr uint32_t kTrickNpcChallenge = 3;
        // The saved settings the park editor's rewards open: how many themes
        // are open, and up to which secret level its pieces are; each piece
        // reward's secret level.
        inline ee::Global<uint8_t, sym::g_openThemeCount> g_openThemeCount;
        inline ee::Global<uint8_t, sym::g_secretLevelUnlocked> g_secretLevelUnlocked;
        constexpr ee::Ptr<const uint8_t> kEditPieceSecretLevels{sym::g_editPieceSecretLevels};
        // The saved game's first halfword: a bit for each feature opened.
        inline ee::Global<uint16_t, sym::g_savedGame> g_openFeatures;
        // Opening level 1 turns the player's tips off.
        constexpr uint16_t kTipsOffLevel = 1;
        ee::Ptr<const Reward> rewardAt(const Reward &reward)
        {
            return ee::Ptr<const Reward>(&reward);
        }

        // Whether a level has the challenge (ProQuest: a later one is
        // current, or this one with all its goals done) or the medal (the
        // competition: a better one).
        bool levelHas(uint32_t player, uint32_t level, uint32_t won)
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, level);
            if (singlePlayerMode() == kLevelGoals)
            {
                if (won < goals->challenge)
                    return true;
                return goals->challenge == won && goals->goalsDone == (*g_challengeGoalCounts)[won];
            }
            if (singlePlayerMode() == kCompetition)
                return won < goals->medal;
            return true;
        }
    }

    void giveReward(uint32_t player, ee::Ptr<const Reward> reward)
    {
        PlayerSetup &setup = (*g_playerSetups)[player];
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        switch (reward->kind)
        {
        case kGivesAttributePoints:
            character->attributePoints = static_cast<uint8_t>(character->attributePoints + reward->value);
            break;
        case kGivesRespect:
        {
            character->levelRespect[*g_sessionLevel] += reward->value;
            character->respect += reward->value;
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            rider->score.respectEarned += reward->value;
            break;
        }
        case kGivesNpcTrick:
        {
            ee::Local<NpcInfo> npc;
            npcInfo(npcForChallenge(kTrickNpcChallenge), npc);
            const uint8_t trick = kOwnTricks[reward->value * kOwnTrickBytes + npc->character * (kOwnTricksEach * kOwnTrickBytes)];
            if (trick != kNoTrick)
                character->tricksOff[trick >> 3] &= static_cast<uint8_t>(~(1u << (trick & 7)));
            break;
        }
        case kGivesBike:
        {
            const int32_t open = reward->value + 1;
            character->flags |= kCharacterNewBike;
            character->openBikes = static_cast<uint8_t>(open < character->openBikes ? character->openBikes : open);
            break;
        }
        case kGivesLook:
        {
            const int32_t open = reward->value + 1;
            character->flags |= kCharacterNewLook;
            character->openLooks = static_cast<uint8_t>(open < character->openLooks ? character->openLooks : open);
            break;
        }
        case kGivesCharacter:
            (*g_riderCharacters)[reward->value].flags &= ~kCharacterLocked;
            break;
        case kGivesLevel:
            character->levels[reward->value].flags |= kLevelOpen;
            character->flags |= kCharacterNewLevel;
            if (reward->value == kTipsOffLevel)
                setup.flags &= static_cast<uint8_t>(~kPlayerTips);
            break;
        case kGivesCompetition:
            playerLevelGoals(player, reward->value)->flags |= kLevelCompetitionOpen;
            break;
        case kGivesEditTheme:
            *g_openThemeCount = static_cast<uint8_t>(reward->value < *g_openThemeCount ? *g_openThemeCount : reward->value);
            break;
        case kGivesEditPiece:
        {
            const uint8_t level = kEditPieceSecretLevels[reward->value];
            *g_secretLevelUnlocked = level < *g_secretLevelUnlocked ? *g_secretLevelUnlocked : level;
            break;
        }
        case kGivesSponsor:
            character->sponsorShown[reward->value] = 1;
            break;
        case kGivesMovie:
            character->flags |= kCharacterMovieOpen;
            break;
        case kGivesFeature:
            *g_openFeatures = static_cast<uint16_t>(*g_openFeatures | 1u << (reward->value & 31));
            break;
        case kGivesOfCharacter:
            if (setup.character >= kBuiltInCharacters && setup.character < kCharacters)
                return; // (a created rider)
            giveReward(player, rewardAt((*g_characterRewards)[setup.character][reward->value]));
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(giveReward, Verified);

    void roundRewards(uint32_t player)
    {
        if (multiplayerMode() != 0)
            return;
        const PlayerSetup &setup = (*g_playerSetups)[player];
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;

        // What was won: challenges (medals) `first` up to `end`.
        uint32_t first = 0;
        uint32_t end = 0;
        ee::Ptr<LevelGoals> goals;
        if (singlePlayerMode() == kLevelGoals)
        {
            goals = playerLevelGoals(player, *g_sessionLevel);
            const uint32_t challenge = goals->challenge;
            first = challenge;
            end = challenge;
            if (goals->goalsDone == (*g_challengeGoalCounts)[challenge])
            {
                // Done: rewarded when a goal of it was given by its NPC.
                uint32_t goal = 0;
                while (goal < (*g_challengeGoalCounts)[challenge] &&
                       !(goals->challenges[challenge].goals[goal].flags & kGoalGiven))
                    ++goal;
                first = goal < (*g_challengeGoalCounts)[challenge] ? challenge : challenge + 1;
                end = challenge + 1;
            }
        }
        else
        {
            if (singlePlayerMode() != kCompetition)
                return;
            goals = playerLevelGoals(player, *g_sessionLevel);
            first = goals->medal;
            if (roundFlags(kGameEndsFlag))
            {
                uint32_t medal = 0;
                for (uint32_t place = 0; place < kMedals; ++place)
                    if ((*g_competitionRanking)[place] == setup.character)
                        medal = kMedals - place;
                end = medal;
                if (first < medal)
                    goals->medal = static_cast<uint8_t>(medal);
            }
        }

        for (uint32_t won = first; won < end; ++won)
        {
            for (uint32_t i = 0; i < kRewardsEach; ++i)
            {
                score.goalFlags |= 1u << (won * kRewardsEach + i);
                const Reward *reward;
                if (singlePlayerMode() == kLevelGoals)
                {
                    reward = &(*g_challengeRewards)[*g_sessionLevel][won][i];
                    if (won == kLastChallenge)
                        goals->flags |= kLevelChallengesDone;
                }
                else
                {
                    reward = &(*g_medalRewards)[*g_sessionLevel][won][i];
                    if (won == kCompetitionLast)
                        goals->flags |= kLevelCompetitionDone;
                }
                giveReward(player, rewardAt(*reward));
            }

            // The group of four levels this one is in.
            const uint32_t group = *g_sessionLevel / kGroupLevels;
            bool all = true;
            for (uint32_t level = group * kGroupLevels; all && level < group * kGroupLevels + kGroupLevels; ++level)
                all = levelHas(player, level, won);
            if (!all)
                continue;
            const Reward &groupReward = singlePlayerMode() == kLevelGoals ? (*g_groupChallengeRewards)[group][won]
                                                                          : (*g_groupMedalRewards)[group][won];
            if (groupReward.kind != 0)
            {
                score.goalFlags |= 1u << (won + kGroupRewardBit);
                giveReward(player, rewardAt(groupReward));
            }
            if (singlePlayerMode() == kLevelGoals && won == kLastChallenge)
                character->flags |= kCharacterGroupChallenges << group;
            else if (singlePlayerMode() == kCompetition && won == kCompetitionLast)
                character->flags |= kCharacterGroupMedals << group;

            // Every level (the park editor's too).
            for (uint32_t level = 0; all && level < kLevels; ++level)
                all = levelHas(player, level, won);
            if (!all)
                continue;
            if (!(singlePlayerMode() == kLevelGoals && won == kLastChallenge) &&
                !(singlePlayerMode() == kCompetition && won == kCompetitionLast))
                continue;
            score.goalFlags |= kAllLevelsBit;
            ee::Ptr<const Reward> gameReward;
            if (singlePlayerMode() == kLevelGoals)
            {
                gameReward = rewardAt(*g_allChallengesReward);
                character->flags |= kCharacterAllChallenges;
            }
            else if (singlePlayerMode() == kCompetition)
            {
                gameReward = rewardAt(*g_allMedalsReward);
                character->flags |= kCharacterAllMedals;
            }
            giveReward(player, gameReward);
            if ((character->flags & (kCharacterAllChallenges | kCharacterAllMedals)) ==
                (kCharacterAllChallenges | kCharacterAllMedals))
            {
                score.goalFlags |= kBothAllBit;
                giveReward(player, rewardAt(*g_allLevelsReward));
            }
        }

        if (score.goalFlags & kAllGapsBit)
            giveReward(player, rewardAt((*g_allGapsRewards)[*g_sessionLevel]));

        if (score.respectEarned == 0)
            return;
        for (uint32_t step = kRespectStep; step <= kFullRespect; step += kRespectStep)
        {
            if (character->respect < step)
                return;
            if (character->respect - score.respectEarned >= step)
                continue;
            // The turn passed this step.
            score.goalFlags |= kRespectStepBit;
            for (uint32_t i = 0; i < kRewardsEach; ++i)
                giveReward(player, rewardAt((*g_respectRewards)[step / kRespectStep][i]));
            if (step != kFullRespect)
                continue;
            character->flags |= kCharacterFullRespect;
            const uint8_t who = setup.character;
            if (who >= kBuiltInCharacters && who < kCharacters)
                continue; // (a created rider)
            if (who < kRegularCharacters)
            {
                bool allFull = true;
                for (uint32_t other = 0; allFull && other < kRegularCharacters; ++other)
                    allFull = ((*g_riderCharacters)[other].flags & kCharacterFullRespect) != 0;
                if (allFull)
                {
                    score.goalFlags |= kAllCharactersRespectBit;
                    giveReward(player, rewardAt(*g_allCharactersRespectReward));
                }
            }
            else if (who == kLastBuiltInCharacter)
            {
                score.goalFlags |= kLastCharacterRespectBit;
                giveReward(player, rewardAt(*g_lastCharacterRespectReward));
            }
        }
    }
    READABLE_FUNCTION(roundRewards, Verified);
}
