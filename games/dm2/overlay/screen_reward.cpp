// DM2's reward screen (screens.h; "InGameUI.FrameReward"): its show (the
// turn's score, respect, gaps and records, then its first reward) and its
// script function, which goes on through the turn's rewards. DM2's own code
// (overlay.c).

#include "screens.h"

#include "dm2/action/card_frame.h"
#include "dm2/action/saved_records.h"
#include "dm2/action/action_state.h"
#include "dm2/game/competition.h"
#include "dm2/game/goals.h"
#include "dm2/game/npcs.h"
#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/rewards.h"
#include "dm2/game/round.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_reward.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kRewardFrame{sym::g_textFrameReward}; // "InGameUI.FrameReward"
        constexpr ee::Ptr<const char> kLockUserAction{sym::g_textLockUser}; // "LockUser"
        // The frame's script asks for the reward's sound with this MsgType
        // (inferred: the call plays nothing else).
        constexpr int32_t kRewardSoundMessage = 3;
        constexpr int32_t kRewardSound = 0x32;

        // The reward steps (showRewardStep's, g_screenStep[0]): the
        // challenge's (or medal's), a group of levels', every level's,
        // both games', the gaps', the respect's, the riders' set at full
        // respect, the last character's. The goal flags' bit each asks for.
        enum RewardStep : uint8_t
        {
            kRewardChallengeStep = 1,
            kRewardGroupStep = 2,
            kRewardAllLevelsStep = 3,
            kRewardBothAllStep = 4,
            kRewardGapsStep = 5,
            kRewardRespectStep = 6,
            kRewardAllCharactersStep = 7,
            kRewardLastCharacterStep = 8,
        };
        constexpr uint32_t kGroupRewardBits = 0x7C00;
        constexpr uint32_t kAllLevelsBit = 0x10000;
        constexpr uint32_t kBothAllBit = 0x20000;
        constexpr uint32_t kAllGapsBit = 0x8000;
        constexpr uint32_t kRespectStepBit = 0x40000;
        constexpr uint32_t kAllCharactersRespectBit = 0x80000;
        constexpr uint32_t kLastCharacterRespectBit = 1u << 20;


        // The step after `step` the turn's goal flags ask for, or 0.
        uint8_t nextRewardStep(uint8_t step, uint32_t flags)
        {
            switch (step)
            {
            case kRewardChallengeStep:
                return (flags & kGroupRewardBits) ? kRewardGroupStep : 0;
            case kRewardGroupStep:
                return (flags & kAllLevelsBit) ? kRewardAllLevelsStep : 0;
            case kRewardAllLevelsStep:
                return (flags & kBothAllBit) ? kRewardBothAllStep : 0;
            case kRewardRespectStep:
                if (flags & kAllCharactersRespectBit)
                    return kRewardAllCharactersStep;
                return (flags & kLastCharacterRespectBit) ? kRewardLastCharacterStep : 0;
            case kRewardAllCharactersStep:
                return (flags & kLastCharacterRespectBit) ? kRewardLastCharacterStep : 0;
            default:
                return 0;
            }
        }
    }

    namespace
    {
        // The frame's status panel ("FrameStatus"): the score ("Title1"),
        // and four lines: the respect, the gaps, the records, the turn's
        // tricks ("Item00.Desc.Value" .. "Item03.Desc.Value").
        constexpr ee::Ptr<const char> kStatusName{sym::g_textFrameStatus};
        constexpr ee::Ptr<const char> kScoreName{sym::g_textTitle1};
        constexpr ee::Ptr<const char> kRespectItem{sym::g_textItem00DescValue};
        constexpr ee::Ptr<const char> kGapsItem{sym::g_textItem01DescValue};
        constexpr ee::Ptr<const char> kRecordsItem{sym::g_textItem02DescValue};
        constexpr ee::Ptr<const char> kTricksItem{sym::g_textItem03DescValue};
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};             // "Text"
        // "%d\n%d (%d%%)\n%d (%d%%)": the turn's respect, the level's and its
        // share of the level's 5,000, the character's and its share of 40,000.
        constexpr ee::Ptr<const char> kRespectFormat{sym::g_textRespectFormat};
        constexpr ee::Ptr<const char> kCountsFormat{sym::g_textCountsFormat};    // "%d / %d\n%d"
        constexpr ee::Ptr<const char> kTricksFormat{sym::g_textTricksLowerFormat}; // "%d tricks"
        constexpr uint32_t kLevelRespectPerCent = 5000 / 100;
        constexpr uint32_t kRespectPerCent = 40000 / 100;
        inline ee::Global<uint8_t[kLevels], sym::g_levelGapCounts> g_levelGapCounts;

        // The current game's key of a kind's record (the modes asked for again each time).
        uint32_t currentKey(uint32_t kind)
        {
            const uint32_t mode = multiplayerMode();
            const uint32_t single = singlePlayerMode();
            return recordKey(mode, single, kind, kSavedRecords);
        }
    }

    namespace
    {
        // The reward items' frame and its parts: the item's icon ("Text": a
        // character of the frame's font, by the reward's kind) and its
        // description ("Desc"), which fades in.
        constexpr ee::Ptr<const char> kRewardItemsFrame{sym::g_rewardItemsFramePath}; // "InGameUI.FrameReward.FrameReward"
        constexpr ee::Ptr<const char> kItemFormat{sym::g_textItemFormat};              // "Item%02d"
        constexpr ee::Ptr<const char> kItemDescName{sym::g_textItemDesc};              // "Desc"
        constexpr ee::Ptr<const char> kFadeInAction{sym::g_textFadeInAction};          // "FadeIn"
        constexpr uint32_t kRewardKinds = kGivesOfCharacter + 1;
        inline ee::Global<char[kRewardKinds], sym::g_rewardIcons> g_rewardIcons;

        // The string table's key of each kind's text (its words follow: the
        // reward's value, or as below).
        constexpr ee::Ptr<const char> kAttributePointsKey{sym::g_rewardAttributePointsKey}; // "KSTRSP_REWARD_ATTR00"
        constexpr ee::Ptr<const char> kRespectKey{sym::g_rewardRespectKey};       // "KSTRSP_REWARD_RESPECT00"
        constexpr ee::Ptr<const char> kCharacterTipKey{sym::g_rewardCharacterTipKey}; // "KSTRSP_REWARD_TIPCHAR%02d_%d"
        constexpr ee::Ptr<const char> kTrickKey{sym::g_rewardTrickKey};           // "KSTRSP_REWARD_TRICK02"
        constexpr ee::Ptr<const char> kBikeKey{sym::g_rewardBikeKey};             // "KSTRSP_REWARD_BIKE%02d"
        constexpr ee::Ptr<const char> kLookKey{sym::g_rewardLookKey};             // "KSTRSP_REWARD_LOOK%02d"
        constexpr ee::Ptr<const char> kSecretLookKey{sym::g_rewardSecretLookKey}; // "KSTRSP_REWARD_LOOKSECRET"
        constexpr ee::Ptr<const char> kCharacterKey{sym::g_rewardCharacterKey};   // "KSTRSP_REWARD_CHAR%02d"
        constexpr ee::Ptr<const char> kLevelKey{sym::g_rewardLevelKey};           // "KSTRSP_REWARD_LEVEL%02d"
        constexpr ee::Ptr<const char> kCompetitionKey{sym::g_rewardCompetitionKey}; // "KSTRSP_REWARD_CONTEST_LEV%02d"
        constexpr ee::Ptr<const char> kEditThemeKey{sym::g_rewardEditThemeKey};   // "KSTRSP_REWARD_EDITTHEME%02d"
        constexpr ee::Ptr<const char> kEditPieceKey{sym::g_rewardEditPieceKey};   // "KSTRSP_REWARD_EDITPIECE%02d"
        constexpr ee::Ptr<const char> kSponsorKey{sym::g_rewardSponsorKey};       // "KSTRSP_REWARD_SPONSOR%02d"
        constexpr ee::Ptr<const char> kTipKey{sym::g_rewardTipKey};               // "KSTRSP_REWARD_TIP%02d"
        constexpr ee::Ptr<const char> kMovieKey{sym::g_rewardMovieKey};           // "KSTRSP_REWARD_MOVIE%02d"
        constexpr ee::Ptr<const char> kCodeKey{sym::g_rewardCodeKey};             // "KSTRSP_REWARD_CODE%02d"
        constexpr ee::Ptr<const char> kFeatureKey{sym::g_rewardFeatureKey};       // "KSTRSP_REWARD_FEATURE%02d"

        // The characters whose looks reward is the secret one (the built-in
        // ones from the fifteenth), and the created riders (17 .. 20).
        constexpr uint8_t kFirstSecretLookCharacter = 14;
        bool createdRider(uint8_t character) { return character >= kBuiltInCharacters && character < kCharacters; }
        // Each character's sponsors' numbers (by its sponsorShown index;
        // kSponsors + 1 a character); a created rider's is sponsor 54.
        inline ee::Global<uint8_t[kCharacters][kSponsors + 1], sym::g_characterSponsorNumbers> g_characterSponsorNumbers;
        constexpr uint32_t kCreatedRiderSponsor = 0x36;
        // The character numbers the characters' reward counts from (the
        // first that can be unlocked).
        constexpr int32_t kFirstRewardCharacter = 14;
        // Each character's own four tricks, six bytes an entry, the trick's number first (0xFF none).
        constexpr ee::Ptr<const uint8_t> kOwnTricks{sym::g_characterOwnTricks};
        constexpr uint32_t kOwnTricksEach = 4, kOwnTrickBytes = 6;
        constexpr uint8_t kNoTrick = 0xFF;
        constexpr uint32_t kTrickNpcChallenge = 3;
        // The tricks' and modifiers' names; a value from 0x56 names a modifier.
        constexpr ee::Ptr<ee::Ptr<char>> kTrickNames{sym::trickNames};
        constexpr ee::Ptr<ee::Ptr<char>> kModifierNames{sym::modifierNames};
        constexpr uint16_t kFirstModifierValue = 0x56;
    }

    // addRewardItem: the reward frame's next item ("Item%02d", by *items)
    // takes a reward: its kind's icon and its text from the string table,
    // with the reward's value (an NPC's trick: the trick's name), and fades
    // in; *items counts it. A kind with no text (none, an NPC trick no one
    // has, a created rider's own reward) takes no item; the character's own
    // reward is shown as the reward it is.
    void addRewardItem(uint32_t player, ee::Ptr<const Reward> reward, ee::Ptr<int32_t> items)
    {
        ee::Local<char[2]> icon;
        ee::Local<char[0x200]> line;
        ee::Local<char[0x200]> key;
        ee::Local<NpcInfo> npc;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kRewardItemsFrame);
        zge::formatString(line, kItemFormat, *items);
        const ee::Ptr<zge::zgui::Node> item = zge::zgui::findNode(frame, line, 0);
        icon[1] = 0;
        key[0] = 0;
        const PlayerSetup &setup = (*g_playerSetups)[player];
        uint8_t trick = 0;
        switch (reward->kind)
        {
        case kGivesAttributePoints:
            zge::stringCopy(key, kAttributePointsKey);
            break;
        case kGivesRespect:
            zge::stringCopy(key, kRespectKey);
            break;
        case kGivesNpcTrick:
            npcInfo(npcForChallenge(kTrickNpcChallenge), npc);
            trick = kOwnTricks[reward->value * kOwnTrickBytes + npc->character * (kOwnTricksEach * kOwnTrickBytes)];
            if (trick != kNoTrick)
                zge::formatString(key, kCharacterTipKey, npc->character, reward->value);
            break;
        case kGivesTrick:
            zge::formatString(key, kTrickKey);
            break;
        case kGivesBike:
            zge::formatString(key, kBikeKey, reward->value);
            break;
        case kGivesLook:
            if (createdRider(setup.character) || setup.character < kFirstSecretLookCharacter)
                zge::formatString(key, kLookKey, reward->value);
            else
                zge::formatString(key, kSecretLookKey);
            break;
        case kGivesCharacter:
            zge::formatString(key, kCharacterKey, static_cast<int32_t>(reward->value) - kFirstRewardCharacter);
            break;
        case kGivesLevel:
            zge::formatString(key, kLevelKey, reward->value);
            break;
        case kGivesCompetition:
            zge::formatString(key, kCompetitionKey, reward->value);
            break;
        case kGivesEditTheme:
            zge::formatString(key, kEditThemeKey, reward->value);
            break;
        case kGivesEditPiece:
            zge::formatString(key, kEditPieceKey, reward->value);
            break;
        case kGivesSponsor:
            if (createdRider(setup.character))
                zge::formatString(key, kSponsorKey, kCreatedRiderSponsor);
            else
                zge::formatString(key, kSponsorKey, (*g_characterSponsorNumbers)[setup.character][reward->value]);
            break;
        case kGivesTip:
            zge::formatString(key, kTipKey, reward->value);
            break;
        case kGivesCharacterTip:
            zge::formatString(key, kCharacterTipKey, setup.character, reward->value);
            break;
        case kGivesMovie:
            zge::formatString(key, kMovieKey, reward->value);
            break;
        case kGivesCode:
            zge::formatString(key, kCodeKey, reward->value);
            break;
        case kGivesFeature:
            zge::formatString(key, kFeatureKey, reward->value);
            break;
        case kGivesOfCharacter:
            if (createdRider(setup.character))
                break;
            reward = ee::Ptr<const Reward>(&(*g_characterRewards)[setup.character][reward->value]);
            addRewardItem(player, reward, items);
            break;
        default:
            break;
        }
        if (key[0] == 0)
            return;

        icon[0] = (*g_rewardIcons)[reward->kind];
        zge::zgui::setVariable(item, kTextName, icon);
        const ee::Address text = zge::stringTableText(zge::nameHash(key));
        if (reward->kind != kGivesNpcTrick)
            zge::formatString(line, text, reward->value);
        else if (reward->value < kFirstModifierValue)
            zge::formatString(line, text, kTrickNames[trick]);
        else
            zge::formatString(line, text, kModifierNames[trick - kFirstModifierValue]);
        zge::zgui::setVariable(zge::zgui::findNode(item, kItemDescName, 0), kTextName, line);
        playWidgetAction(item, kFadeInAction);
        *items += 1;
    }
    READABLE_FUNCTION(addRewardItem, Verified);

    namespace
    {
        // The reward step's message ("Message") by step: a challenge's or
        // medal's, a group's, every level's, both games', the gaps', the
        // respect's (its total in thousands of five), the riders' set's or the
        // last character's.
        constexpr ee::Ptr<const char> kMessageName{sym::g_textMessage};              // "Message"
        constexpr ee::Ptr<const char> kPositionName{sym::g_textItemPosition};        // "Position"
        constexpr ee::Ptr<const char> kChallengeMessage{sym::g_rewardChallengeMessage};    // "$KSTRSP_REWARD_OBJCTV%02d"
        constexpr ee::Ptr<const char> kGroupMessage{sym::g_rewardGroupMessage};            // "$KSTRSP_REWARD_ALLCITY%d_%2d"
        constexpr ee::Ptr<const char> kMedalMessage{sym::g_rewardMedalMessage};            // "$KSTRSP_REWARD_CONTEST%02d"
        constexpr ee::Ptr<const char> kGroupMedalMessage{sym::g_rewardGroupMedalMessage};  // "KSTRSP_REWARD_ALLCITY0_%02d"
        constexpr ee::Ptr<const char> kAllChallengesMessage{sym::g_rewardAllChallengesMessage}; // "$KSTRSP_REWARD_ALLGAME00"
        constexpr ee::Ptr<const char> kAllMedalsMessage{sym::g_rewardAllMedalsMessage};    // "$KSTRSP_REWARD_ALLGAME01"
        constexpr ee::Ptr<const char> kAllLevelsMessage{sym::g_rewardAllLevelsMessage};    // "$KSTRSP_REWARD_ALLGAME02"
        constexpr ee::Ptr<const char> kAllGapsMessage{sym::g_rewardAllGapsMessage};        // "$KSTRSP_REWARD_ALLGAPS00"
        constexpr ee::Ptr<const char> kRespectTotalKey{sym::g_rewardRespectTotalKey};      // "KSTRSP_REWARD_RESPECTTOTAL00"
        constexpr ee::Ptr<const char> kAllCharactersMessage{sym::g_rewardAllCharactersMessage}; // "$KSTRSP_REWARD_ALLCHAR%02d"
        constexpr uint32_t kRewardItems = 4;
        // The challenges a ProQuest level has, the medals a competition has.
        constexpr uint32_t kChallengeSteps = kLevelChallenges, kMedalSteps = kMedals;
        // The items' places in the frame: 15 across; down from 45, 51 apart,
        // the column centred (25.5 up for each item after the first).
        constexpr float kItemX = 15.0f, kFirstItemY = 45.0f, kItemSpacing = 51.0f, kItemCentring = 25.5f;
        // ScoreBlock::goalFlags: a group's reward bits from bit 10.
        constexpr uint32_t kGroupRewardBit = 10;

        // The reward pair of a challenge (level goals) or medal (competition).
        ee::Ptr<const Reward> wonRewards(uint32_t won)
        {
            if (singlePlayerMode() == kLevelGoals)
                return ee::Ptr<const Reward>(&(*g_challengeRewards)[*g_sessionLevel][won][0]);
            return ee::Ptr<const Reward>(&(*g_medalRewards)[*g_sessionLevel][won][0]);
        }
        ee::Ptr<const Reward> rewardAt(const Reward &reward)
        {
            return ee::Ptr<const Reward>(&reward);
        }
        // The medal the player's character won in the competition (3 gold ..
        // 1 bronze; 0 none), by the ranking's first three places.
        uint32_t medalWon(uint8_t character)
        {
            if ((*g_competitionRanking)[0] == character)
                return kMedals;
            for (uint32_t place = 1; place < kMedals; ++place)
                if ((*g_competitionRanking)[place] == character)
                    return kMedals - place;
            return 0;
        }
    }

    // showRewardStep: a reward step of the reward screen shown (0: none):
    // the frame's four items reset, the step's message, its items' places
    // (as many as the step's rewards, centred), and the rewards themselves
    // (addRewardItem). The challenge step shows the respect its rewards give
    // as one item. Bug kept: the respect steps go up to 35,000, so the
    // rewards at full respect (40,000) are never shown.
    void showRewardStep(uint32_t step)
    {
        ee::Local<char[0x40]> message;
        ee::Local<char[0x200]> path;
        ee::Local<ee::Float[4]> position;
        ee::Local<Reward> respect;
        ee::Local<int32_t> items;
        const uint32_t index = step - 1;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kRewardItemsFrame);
        ee::Ptr<zge::zgui::Node> widgets[kRewardItems];
        for (uint32_t i = 0; i < kRewardItems; ++i)
        {
            zge::formatString(path, kItemFormat, i);
            widgets[i] = zge::zgui::findNode(frame, path, 0);
            if (widgets[i])
                playWidgetAction(widgets[i], kInitAction);
        }
        startScreenStep(step);
        const PlayerSetup &setup = (*g_playerSetups)[*g_currentPlayer];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        const rider::ScoreBlock &score = rider->score;
        const ee::Ptr<RiderCharacter> character = playerCharacter(*g_currentPlayer);

        switch (step)
        {
        case kRewardChallengeStep:
        case kRewardGroupStep:
            if (singlePlayerMode() == kLevelGoals)
            {
                const ee::Ptr<LevelGoals> goals = playerLevelGoals(*g_currentPlayer, *g_sessionLevel);
                uint32_t challenge = goals->challenge;
                if (goals->goalsDone == (*g_challengeGoalCounts)[challenge])
                    challenge = goals->challenge + 1;
                if (step == kRewardChallengeStep)
                    zge::formatString(message, kChallengeMessage, static_cast<int32_t>(challenge) - 1);
                else
                    zge::formatString(message, kGroupMessage, *g_sessionLevel >> 2, static_cast<int32_t>(challenge) - 1);
            }
            else
            {
                const uint32_t medal = medalWon(setup.character);
                zge::formatString(message, step == kRewardChallengeStep ? kMedalMessage : kGroupMedalMessage, static_cast<int32_t>(medal) - 1);
            }
            break;
        case kRewardAllLevelsStep:
            zge::formatString(message, singlePlayerMode() == kLevelGoals ? kAllChallengesMessage : kAllMedalsMessage);
            break;
        case kRewardBothAllStep:
            zge::formatString(message, kAllLevelsMessage);
            break;
        case kRewardGapsStep:
            zge::formatString(message, kAllGapsMessage);
            break;
        case kRewardRespectStep:
            zge::formatString(message, zge::stringTableText(zge::nameHash(kRespectTotalKey)),
                              character->respect / kRespectStep * kRespectStep);
            break;
        case kRewardAllCharactersStep:
        case kRewardLastCharacterStep:
            zge::formatString(message, kAllCharactersMessage, static_cast<int32_t>(step) - kRewardAllCharactersStep);
            break;
        default:
            message[0] = 0;
            break;
        }
        uint32_t shown = 0;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, 0), kTextName, message);

        // How many items the step shows (the challenge step's respect rewards as one).
        uint32_t wonCount = kMedalSteps;
        uint32_t respectGiven = 0;
        if (singlePlayerMode() == kLevelGoals)
            wonCount = kChallengeSteps;
        switch (index)
        {
        case kRewardChallengeStep - 1:
            for (uint32_t won = 0; won < wonCount; ++won)
            {
                for (uint32_t i = 0; i < kRewardsEach; ++i)
                {
                    if (!((score.goalFlags >> (won * kRewardsEach + i)) & 1))
                        continue;
                    const Reward &reward = wonRewards(won)[i];
                    if (reward.kind == kGivesNone)
                        continue;
                    if (reward.kind == kGivesRespect)
                    {
                        if (respectGiven == 0)
                            ++shown;
                        respectGiven += reward.value;
                    }
                    else
                    {
                        ++shown;
                    }
                }
            }
            break;
        case kRewardGroupStep - 1:
            for (uint32_t won = 0; won < wonCount; ++won)
                shown += (score.goalFlags >> (won + kGroupRewardBit)) & 1;
            break;
        case kRewardAllLevelsStep - 1:
            shown = (score.goalFlags & kAllLevelsBit) != 0;
            break;
        case kRewardBothAllStep - 1:
            shown = (score.goalFlags & kBothAllBit) != 0;
            break;
        case kRewardGapsStep - 1:
            shown = (score.goalFlags & kAllGapsBit) != 0;
            break;
        case kRewardRespectStep - 1:
            if (score.goalFlags & kRespectStepBit)
            {
                const uint32_t total = character->respect;
                for (uint32_t passed = kRespectStep; passed < kFullRespect && total >= passed; passed += kRespectStep)
                {
                    if (total - score.respectEarned >= passed)
                        continue;
                    for (uint32_t i = 0; i < kRewardsEach; ++i)
                        if ((*g_respectRewards)[passed / kRespectStep][i].kind != kGivesNone)
                            ++shown;
                }
            }
            break;
        case kRewardAllCharactersStep - 1:
            shown = (score.goalFlags & kAllCharactersRespectBit) != 0;
            break;
        case kRewardLastCharacterStep - 1:
            shown = (score.goalFlags & kLastCharacterRespectBit) != 0;
            break;
        default:
            break;
        }
        for (uint32_t i = 0; i < shown; ++i)
        {
            const ee::Float down = zge::unsignedToFloat(i) * ee::Float(kItemSpacing) + ee::Float(kFirstItemY);
            position[2] = 0.0f;
            position[0] = kItemX;
            position[1] = down - zge::unsignedToFloat(shown - 1) * ee::Float(kItemCentring);
            zge::zgui::setVariable(widgets[i], kPositionName, position);
        }

        *items = 0;
        switch (index)
        {
        case kRewardChallengeStep - 1:
            if (respectGiven != 0)
            {
                respect->kind = kGivesRespect;
                respect->value = static_cast<uint16_t>(respectGiven);
                addRewardItem(*g_currentPlayer, respect, items);
            }
            for (uint32_t won = 0; won < kChallengeSteps; ++won)
            {
                for (uint32_t i = 0; i < kRewardsEach; ++i)
                {
                    if (!((score.goalFlags >> (won * kRewardsEach + i)) & 1))
                        continue;
                    const ee::Ptr<const Reward> reward = wonRewards(won) + static_cast<ptrdiff_t>(i);
                    if (reward->kind != kGivesRespect)
                        addRewardItem(*g_currentPlayer, reward, items);
                }
            }
            break;
        case kRewardGroupStep - 1:
            for (uint32_t won = 0; won < wonCount; ++won)
            {
                if (!((score.goalFlags >> (won + kGroupRewardBit)) & 1))
                    continue;
                const uint32_t group = *g_sessionLevel / kGroupLevels;
                const Reward &reward = singlePlayerMode() == kLevelGoals ? (*g_groupChallengeRewards)[group][won]
                                                                         : (*g_groupMedalRewards)[group][won];
                addRewardItem(*g_currentPlayer, rewardAt(reward), items);
            }
            break;
        case kRewardAllLevelsStep - 1:
            if (score.goalFlags & kAllLevelsBit)
            {
                ee::Ptr<const Reward> reward;
                if (singlePlayerMode() == kLevelGoals)
                    reward = rewardAt(*g_allChallengesReward);
                else if (singlePlayerMode() == kCompetition)
                    reward = rewardAt(*g_allMedalsReward);
                addRewardItem(*g_currentPlayer, reward, items);
            }
            break;
        case kRewardBothAllStep - 1:
            if (score.goalFlags & kBothAllBit)
                addRewardItem(*g_currentPlayer, rewardAt(*g_allLevelsReward), items);
            break;
        case kRewardGapsStep - 1:
            if (score.goalFlags & kAllGapsBit)
                addRewardItem(*g_currentPlayer, rewardAt((*g_allGapsRewards)[*g_sessionLevel]), items);
            break;
        case kRewardRespectStep - 1:
            if (score.goalFlags & kRespectStepBit)
            {
                for (uint32_t passed = kRespectStep; passed < kFullRespect && character->respect >= passed; passed += kRespectStep)
                {
                    if (character->respect - score.respectEarned >= passed)
                        continue;
                    for (uint32_t i = 0; i < kRewardsEach; ++i)
                        addRewardItem(*g_currentPlayer, rewardAt((*g_respectRewards)[passed / kRespectStep][i]), items);
                }
            }
            break;
        case kRewardAllCharactersStep - 1:
            if (score.goalFlags & kAllCharactersRespectBit)
                addRewardItem(*g_currentPlayer, rewardAt(*g_allCharactersRespectReward), items);
            break;
        case kRewardLastCharacterStep - 1:
            if (score.goalFlags & kLastCharacterRespectBit)
                addRewardItem(*g_currentPlayer, rewardAt(*g_lastCharacterRespectReward), items);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(showRewardStep, Verified);

    // showRewardScreen: as it opens, the status panel (when the frame has
    // one): the turn's score; then the respect, the gaps (the level's found
    // of its count, and found this turn), the records (the saved ones set of
    // the 13 kinds, and the game's set this turn) and the turn's best run of
    // tricks; the frame's Init and its first reward: the challenge's, else
    // the gaps', the respect's or none. As it closes, its DeInit.
    void showRewardScreen(uint32_t shown)
    {
        ee::Local<char[0x100]> text;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kRewardFrame);
        if (!frame)
            return;
        const ee::Ptr<RiderCharacter> character = playerCharacter(*g_currentPlayer);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        if (shown == 0)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        const ee::Ptr<zge::zgui::Node> status = zge::zgui::findNode(frame, kStatusName, 0);
        if (status)
        {
            formatModeScore(text, rider->score.gameScore);
            zge::zgui::setVariable(zge::zgui::findNode(status, kScoreName, 0), kTextName, text);
        }
        // (the status panel's lines are set whether it is there or not: the
        // engine's findNode answers 0 for no node)
        // (by the session level: the park editor's, 8, would reach `respect`)
        const uint32_t levelRespect = (&character->levelRespect[0])[*g_sessionLevel];
        zge::formatString(text, kRespectFormat, rider->score.respectEarned, levelRespect,
                          levelRespect / kLevelRespectPerCent, character->respect, character->respect / kRespectPerCent);
        zge::zgui::setVariable(zge::zgui::findNode(status, kRespectItem, 0), kTextName, text);

        const uint8_t level = *g_sessionLevel;
        uint32_t gaps = 0;
        uint32_t gapsThisTurn = 0;
        for (uint32_t gap = 0; gap < (*g_levelGapCounts)[level]; ++gap)
        {
            const uint32_t word = gap >> 5;
            const uint32_t shift = gap & 31;
            if ((static_cast<int32_t>((&character->levels[level].gapsFound)[word]) >> shift) & 1)
            {
                ++gaps;
                gapsThisTurn += (static_cast<int32_t>((&rider->score.gapsFound)[word]) >> shift) & 1;
            }
        }
        zge::formatString(text, kCountsFormat, gaps, (*g_levelGapCounts)[level], gapsThisTurn);
        zge::zgui::setVariable(zge::zgui::findNode(status, kGapsItem, 0), kTextName, text);

        uint32_t savedSet = 0;
        uint32_t gameSet = 0;
        for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
        {
            const ee::Ptr<SavedRecord> saved = savedRecord(currentKey(kind));
            if (saved)
                savedSet += saved->flags & kSavedRecordSet;
            const ee::Ptr<GameRecord> record = gameRecord(currentKey(kind));
            if (record)
                gameSet += record->marks & kRecordSet;
        }
        zge::formatString(text, kCountsFormat, savedSet, kRecordKinds, gameSet);
        zge::zgui::setVariable(zge::zgui::findNode(status, kRecordsItem, 0), kTextName, text);

        const ee::Ptr<GameRecord> runTricks =
            gameRecord(currentKey(kRunTricksRecord));
        zge::formatString(text, kTricksFormat, sce::libgcc::fptoui(runTricks->value));
        zge::zgui::setVariable(zge::zgui::findNode(status, kTricksItem, 0), kTextName, text);

        playWidgetAction(frame, kInitAction);
        if (challengeRewardEarned(*g_currentPlayer))
            showRewardStep(kRewardChallengeStep);
        else if (rider->score.goalFlags & kAllGapsBit)
            showRewardStep(kRewardGapsStep);
        else if (rider->score.goalFlags & kRespectStepBit)
            showRewardStep(kRewardRespectStep);
        else
            showRewardStep(0);
    }
    READABLE_FUNCTION(showRewardScreen, Verified);

    // bmxoverlayRewardHandler: the frame's sound asked for (the last
    // character's reward has its own), or the next reward the turn earned
    // shown (its frame locked for input again); with none left, the game
    // over screen at the game's end, else the round over screen.
    void bmxoverlayRewardHandler()
    {
        if (*g_uiMessageType == kRewardSoundMessage)
        {
            game::tail(soundEvent, kRewardSound, (*g_screenStep)[0] == kRewardLastCharacterStep ? 1 : 0, 0, 0, 0);
            return;
        }
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        const uint8_t step = (*g_screenStep)[0];
        uint8_t next = nextRewardStep(step, rider->score.goalFlags);
        if (next == 0 && step < kRewardGapsStep && (rider->score.goalFlags & kAllGapsBit))
            next = kRewardGapsStep;
        if (next == 0 && step < kRewardRespectStep && (rider->score.goalFlags & kRespectStepBit))
            next = kRewardRespectStep;
        if (next == 0)
        {
            showGameScreen(roundFlags(kGameEndsFlag) ? kScreenGameOver : kScreenRoundOver);
            return;
        }
        showRewardStep(next);
        stopWidgetAction(uiWidget(kRewardFrame), kLockUserAction);
        playWidgetAction(uiWidget(kRewardFrame), kLockUserAction);
    }
    READABLE_FUNCTION(bmxoverlayRewardHandler, Verified);
}
