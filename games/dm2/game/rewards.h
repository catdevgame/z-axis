#pragma once
// DM2's rewards (game.c): what a single-player turn's end gives a character
// for the challenges and medals won, the levels' groups and the whole game
// done, the gaps found and the respect earned.

#include "goals.h"
#include "players.h"
#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/game/rewards.h.h"

namespace game::dm2
{
    // What a reward gives, by the reward screen's text of each
    // ("KSTRSP_REWARD_ATTR00", "_RESPECT00", "_TIPCHAR%02d_%d", "_TRICK02",
    // "_BIKE%02d", "_LOOK%02d", "_CHAR%02d", "_LEVEL%02d", "_CONTEST_LEV%02d",
    // "_EDITTHEME%02d", "_EDITPIECE%02d", "_SPONSOR%02d", "_TIP%02d",
    // "_MOVIE%02d", "_CODE%02d", "_FEATURE%02d"). The ones marked "(shown
    // only)" change nothing in the saved game: the screen just tells them.
    enum RewardGives : uint16_t
    {
        kGivesNone = 0,
        kGivesAttributePoints = 1, // the value's points to spend on the stats
        kGivesRespect = 2,         // the value's respect, in the level and in all
        kGivesNpcTrick = 3,        // the own trick (by the value) of the character of the level's challenge-3 NPC
        kGivesTrick = 4,           // (shown only)
        kGivesBike = 5,            // the character's bikes open up to the value's
        kGivesLook = 6,            // the character's looks open up to the value's
        kGivesCharacter = 7,       // the value's character unlocked
        kGivesLevel = 8,           // the value's level opened to the character
        kGivesCompetition = 9,     // the value's level's competition opened to the character
        kGivesEditTheme = 10,      // the park editor's themes open up to the value
        kGivesEditPiece = 11,      // the park editor's pieces open up to the value's secret level
        kGivesSponsor = 12,        // the value's sponsor's logo shown for the character
        kGivesTip = 13,            // (shown only)
        kGivesCharacterTip = 14,   // (shown only) a tip of the player's character
        kGivesMovie = 15,          // the character's movie opened
        kGivesCode = 16,           // (shown only)
        kGivesFeature = 17,        // the value's feature opened (a bit of the saved game's first halfword)
        kGivesOfCharacter = 18,    // the player's character's own reward of the value's two (none for a created rider)
    };

    // A reward the game's tables hold: its kind (RewardGives) and its value
    // (the points, the trick, the bike, the level, ...). Kind 0: none.
    struct Reward
    {
        uint16_t kind;
        uint16_t value;
    };
    static_assert(sizeof(Reward) == 4);

    // The game's tables of rewards (roundRewards gives them, the reward
    // screen shows them).
    // Two rewards each: a level's challenges (ProQuest) and its medals
    // (bronze, silver, gold: the competition), by level.
    constexpr uint32_t kRewardsEach = 2;
    constexpr uint32_t kMedals = 3;
    inline ee::Global<Reward[kGoalLevels][kLevelChallenges][kRewardsEach], sym::g_challengeRewards> g_challengeRewards;
    inline ee::Global<Reward[kGoalLevels][kMedals][kRewardsEach], sym::g_medalRewards> g_medalRewards;
    // One for a group of four levels all having a challenge (or medal),
    // by group and challenge (medal); kind 0 for none.
    constexpr uint32_t kGroupLevels = 4;
    constexpr uint32_t kGroups = kGoalLevels / kGroupLevels;
    inline ee::Global<Reward[kGroups][kLevelChallenges], sym::g_groupChallengeRewards> g_groupChallengeRewards;
    inline ee::Global<Reward[kGroups][kLevelChallenges], sym::g_groupMedalRewards> g_groupMedalRewards;
    // The whole game's: every level's last challenge, every gold medal, both.
    inline ee::Global<Reward, sym::g_allChallengesReward> g_allChallengesReward;
    inline ee::Global<Reward, sym::g_allMedalsReward> g_allMedalsReward;
    inline ee::Global<Reward, sym::g_allLevelsReward> g_allLevelsReward;
    // A level's gaps all found, by level.
    inline ee::Global<Reward[kGoalLevels], sym::g_allGapsRewards> g_allGapsRewards;
    // Two for each 5,000 of respect, by the steps from 1 (the row of step
    // 0 is never read: it is the gaps' last two).
    constexpr uint32_t kRespectStep = 5000;
    constexpr uint32_t kFullRespect = 40000;
    inline ee::Global<Reward[kFullRespect / kRespectStep + 1][kRewardsEach], sym::g_respectRewards> g_respectRewards;
    // At full respect: the regular riders' set all at full respect, or the
    // last built-in character.
    inline ee::Global<Reward, sym::g_allCharactersRespectReward> g_allCharactersRespectReward;
    inline ee::Global<Reward, sym::g_lastCharacterRespectReward> g_lastCharacterRespectReward;

    // Each character's own reward pair (kGivesOfCharacter), by character.
    inline ee::Global<Reward[kCharacters][kRewardsEach], sym::g_characterRewards> g_characterRewards;

    // A reward given to the player's character (RewardGives says what each does).
    void giveReward(uint32_t player, ee::Ptr<const Reward> reward);

    // A single-player turn's rewards at its end (setRoundPhase): in a
    // ProQuest level, the current challenge done with a goal its NPC gave;
    // in a competition's last run, each medal above the one the level had
    // (the ranking's place). Each gives its two rewards (and its goal flags'
    // bits, which the reward screen shows), then the levels' group's reward
    // when its four levels have it too, and the game's when all nine levels
    // do (the last challenge, the gold medal). Then the gaps' reward when
    // the turn found the level's last, and the respect's for each 5,000
    // the turn's respect passed (at 40,000 one more for the riders' whole
    // set, or the last built-in character).
    void roundRewards(uint32_t player);
}
