// BMX XXX's reward screen (screens.h; "FrameReward"): the game's status
// (goals, gaps, records, tricks) and, a page at a time, what the turn won.
// BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/records.h"
#include "bmx/game/round.h"
#include "bmx/game/turn_rewards.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_reward.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    // (the two below are this file's own)
    void showRewardItem(uint32_t player, ee::Ptr<calls::Reward> reward, uint32_t *items);
    void showRewardPage(uint32_t page);

    namespace
    {
        constexpr uint32_t kFrameReward = sym::g_textFrameReward;       // "FrameReward"
        constexpr uint32_t kRewardBox = sym::g_textFrameRewardBox;      // "FrameReward.FrameReward"
        constexpr uint32_t kFrameStatus = sym::g_textFrameStatus;       // "FrameStatus"
        constexpr uint32_t kTitle1Widget = sym::g_textTitle1;           // "Title1"
        constexpr uint32_t kGoalsValue = sym::g_textItem00DescValue;    // "Item00.Desc.Value"
        constexpr uint32_t kGapsValue = sym::g_textItem01DescValue;     // "Item01.Desc.Value"
        constexpr uint32_t kRecordsValue = sym::g_textItem02DescValue;  // "Item02.Desc.Value"
        constexpr uint32_t kTricksValue = sym::g_textItem03DescValue;   // "Item03.Desc.Value"
        constexpr uint32_t kMessageWidget = sym::g_textMessage;         // "Message"
        constexpr uint32_t kDescWidget = sym::g_textDesc;               // "Desc"
        constexpr uint32_t kPositionName = sym::g_textPosition;         // "Position"
        constexpr uint32_t kFadeInAction = sym::g_textFadeIn;           // "FadeIn"
        constexpr uint32_t kLockUserAction = sym::g_textLockUser;       // "LockUser"
        constexpr uint32_t kItem = sym::bmx::g_textItem;                // "Item%02d"
        constexpr uint32_t kScoreFormat = sym::bmx::g_textRewardScore;  // "%ld"
        constexpr uint32_t kTricksScore = sym::bmx::g_textRewardTricksScore;   // "%d $KSTR_TRICKS"
        constexpr uint32_t kDegreesScore = sym::bmx::g_textRewardDegreesScore; // "%d %s"
        constexpr uint32_t kDegrees = sym::g_textDegrees;                      // "degrees"
        constexpr uint32_t kDoneOfAndNew = sym::bmx::g_textDoneOfAndNew;       // "%d / %d\n%d"
        constexpr uint32_t kNotCounted = sym::bmx::g_textNotCounted;           // "N/A\nN/A"
        constexpr uint32_t kTricksCount = sym::bmx::g_textTricksCount;         // "%d tricks"
        constexpr uint32_t kGoalsMessage = sym::bmx::g_textRewardObjectives;   // "$KSTRSP_REWARD_OBJCTV%02d"
        constexpr uint32_t kContestMessage = sym::bmx::g_textRewardContest;    // "$KSTRSP_REWARD_CONTEST%02d"
        constexpr uint32_t kAllGame00 = sym::bmx::g_textRewardAllGame00;       // "$KSTRSP_REWARD_ALLGAME00"
        constexpr uint32_t kAllGame01 = sym::bmx::g_textRewardAllGame01;       // "$KSTRSP_REWARD_ALLGAME01"
        constexpr uint32_t kAllGame02 = sym::bmx::g_textRewardAllGame02;       // "$KSTRSP_REWARD_ALLGAME02"
        constexpr uint32_t kAllGaps00 = sym::bmx::g_textRewardAllGaps00;       // "$KSTRSP_REWARD_ALLGAPS00"
        constexpr uint32_t kAllCharacters = sym::bmx::g_textRewardAllChar;     // "$KSTRSP_REWARD_ALLCHAR%02d"

        // The reward box's pages, by the message each has (the screen's step).
        enum RewardPage : uint32_t
        {
            kRewardNone = 0,
            kRewardTurn = 1,          // the turn's own rewards ("OBJCTV", "CONTEST"), up to four
            kRewardAllGame = 2,       // "ALLGAME00" / "ALLGAME01"
            kRewardAllGame02 = 3,     // "ALLGAME02"
            kRewardAllGaps = 4,       // "ALLGAPS00"
            kRewardAllCharacters = 5, // "ALLCHAR00"; 6 is "ALLCHAR01" (nothing here shows either)
        };
        constexpr uint32_t kRewardItems = 4;

        // The rider's turn awards (game/turn_rewards.h): a bit each reward
        // the turn's end gave.
        constexpr uint64_t kAwardAllGame = kAwardAllGoals | kAwardAllContests; // either of the two
        uint64_t turnAwards(ee::Ptr<rider::Rider> rider) { return objectives::riderObjectives(rider)->turnAwards; }

        // The reward tables: twenty a level for its goals, three a level for
        // a competition's places, one a level for all its gaps, and the
        // three of the whole game.
        constexpr uint32_t kLevelGoalRewards = 20, kLevelPlaceRewards = 3;
        inline ee::Ptr<calls::Reward> goalReward(uint32_t level, uint32_t slot)
        {
            return ee::Ptr<calls::Reward>(sym::g_goalRewards) + (level * kLevelGoalRewards + slot);
        }
        inline ee::Ptr<calls::Reward> placeReward(uint32_t level, uint32_t slot)
        {
            return ee::Ptr<calls::Reward>(sym::g_placeRewards) + (level * kLevelPlaceRewards + slot);
        }
        inline ee::Ptr<calls::Reward> allGapsReward(uint32_t level) { return ee::Ptr<calls::Reward>(sym::g_allGapsRewards) + level; }
        constexpr ee::Ptr<calls::Reward> kAllGoalsReward{sym::g_allGoalsReward};
        constexpr ee::Ptr<calls::Reward> kAllContestsReward{sym::g_allContestsReward};
        constexpr ee::Ptr<calls::Reward> kAllGame02Reward{sym::g_allGame02Reward};
        // The reward a turn's slot stands for in the game being played.
        ee::Ptr<calls::Reward> turnReward(uint32_t slot)
        {
            return gameMode() == calls::kGoalsMode ? goalReward(*calls::g_currentLevel, slot)
                                                   : placeReward(*calls::g_currentLevel, slot);
        }

        // A reward's letter in the items' font, by its kind.
        inline ee::Global<char[0x15], sym::g_rewardKindLetters> g_rewardKindLetters; // " rrvxeiasdddfcbbttggg"
        constexpr uint32_t kRewardLineWidth = 0x11;

        // The first three of a competition's ranking win a place reward.
        constexpr uint32_t kPlaces = 3;
        inline ee::Global<uint8_t[10], sym::g_competitionRanking> g_competitionRanking;

        // An item's place in the box: 41 apart down from 18, the items centred.
        constexpr ee::Float kItemStep = 41.0f, kItemTop = 18.0f, kItemHalfStep = 20.5f;
        struct WidgetPosition
        {
            ee::Float x, y;
            uint32_t z;
            uint32_t notSet;
        };

        // The game modes scored in other than points: a distance, tricks,
        // degrees.
        constexpr uint8_t kFirstDistanceMode = 9, kLastDistanceMode = 16, kTricksMode = 12, kDegreesMode = 13;
        constexpr uint32_t kDegreesAScore = 180;
        // The record kinds counted as "records", and the kind that counts tricks.
        constexpr uint32_t kCountedRecordKinds = 14, kTricksRecordKind = 4;

        // The frame's own message: an item starts to fade in.
        constexpr int32_t kRewardItemShown = 3;
        constexpr uint32_t kRewardSound = 0x44;
    }

    // showRewardItem: a reward's letter and wrapped text in the box's next
    // item, which fades in; nothing once four are shown or for a reward
    // with no text.
    void showRewardItem(uint32_t player, ee::Ptr<calls::Reward> reward, uint32_t *items)
    {
        ee::Local<char[0x200]> line;
        ee::Local<char[0x200]> text;
        ee::Local<char[0x10]> letter;
        ee::Local<char[0x15]> letters;

        for (uint32_t i = 0; i < sizeof(*g_rewardKindLetters); ++i)
            letters[i] = (*g_rewardKindLetters)[i];
        const ee::Ptr<Node> box = zge::zgui::findNode(*g_overlayUi, kRewardBox, 1);
        zge::formatString(text, kItem, *items);
        const ee::Ptr<Node> item = zge::zgui::findNode(box, text, 1);
        if (*items >= kRewardItems || calls::rewardText(player, reward, text) == 0)
            return;

        letter[0] = letters[reward->kind];
        letter[1] = '\0';
        zge::zgui::setVariable(item, kTextName, letter);
        copyOnOneLine(line, text, 0);
        wrapText(text, line, kRewardLineWidth);
        zge::zgui::setVariable(zge::zgui::findNode(item, kDescWidget, 1), kTextName, text);
        playUiNode(item, kFadeInAction);
        *items = *items + 1;
    }
    READABLE_FUNCTION(showRewardItem, Verified);

    // showRewardPage: the box's four items start over, the page's message
    // fades in, as many items as the page will show are spread down the
    // box, and the page's rewards are shown in them.
    void showRewardPage(uint32_t page)
    {
        ee::Local<char[0x40]> message;
        ee::Local<char[0x200]> name;
        ee::Local<ee::Ptr<Node>[kRewardItems]> itemWidgets;
        ee::Local<WidgetPosition> position;
        ee::Local<uint32_t> items;

        const ee::Ptr<Node> box = zge::zgui::findNode(*g_overlayUi, kRewardBox, 1);
        for (uint32_t i = 0; i < kRewardItems; ++i)
        {
            zge::formatString(name, kItem, i);
            const ee::Ptr<Node> item = zge::zgui::findNode(box, name, 1);
            itemWidgets[i] = item;
            if (item)
                playUiNode(item, kInitAction);
        }
        *g_screenStep = page;
        const ee::Ptr<calls::PlayerSetup> setup = calls::playerSetup(*calls::g_currentPlayer);
        const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));
        playerCharacter(*calls::g_currentPlayer); // (asked for; not used)

        switch (page)
        {
        case kRewardTurn:
            if (gameMode() == calls::kGoalsMode)
            {
                zge::formatString(message, kGoalsMessage,
                                  playerLevelGoals(*calls::g_currentPlayer, *calls::g_currentLevel)->goalsDone);
            }
            else
            {
                // The message of the place won: 2 for the first, 0 for the
                // third, and -1 below them.
                int32_t won = 0;
                for (uint32_t place = 0; place < kPlaces; ++place)
                {
                    if ((*g_competitionRanking)[place] == setup->character)
                    {
                        won = static_cast<int32_t>(kPlaces - place);
                        break;
                    }
                }
                zge::formatString(message, kContestMessage, won - 1);
            }
            break;
        case kRewardAllGame:
            if (gameMode() == calls::kGoalsMode)
                zge::formatString(message, kAllGame00);
            else
                zge::formatString(message, kAllGame01);
            break;
        case kRewardAllGame02:
            zge::formatString(message, kAllGame02);
            break;
        case kRewardAllGaps:
            zge::formatString(message, kAllGaps00);
            break;
        case kRewardAllCharacters:
        case kRewardAllCharacters + 1:
            zge::formatString(message, kAllCharacters, page - kRewardAllCharacters);
            break;
        default:
            message[0] = '\0';
            break;
        }
        zge::zgui::setVariable(zge::zgui::findNode(box, kMessageWidget, 1), kTextName, message);
        playUiNode(zge::zgui::findNode(box, kMessageWidget, 1), kFadeInAction);

        // How many items the page shows.
        const uint32_t slots = gameMode() == calls::kGoalsMode ? kLevelGoalRewards : kLevelPlaceRewards;
        uint32_t shown = 0;
        switch (page)
        {
        case kRewardTurn:
            for (uint32_t slot = 0; slot < slots; ++slot)
            {
                if ((turnAwards(playerRider) >> slot & 1) != 0 && turnReward(slot)->kind != 0)
                    ++shown;
            }
            break;
        case kRewardAllGame:
            shown = (turnAwards(playerRider) & kAwardAllGame) != 0 ? 1 : 0;
            break;
        case kRewardAllGame02:
            shown = (turnAwards(playerRider) & kAwardAllGame02) != 0 ? 1 : 0;
            break;
        case kRewardAllGaps:
            shown = (turnAwards(playerRider) & kAwardAllGaps) != 0 ? 1 : 0;
            break;
        default:
            break;
        }
        if (shown > kRewardItems)
            shown = kRewardItems;
        for (uint32_t i = 0; i < shown; ++i)
        {
            const ee::Float down = zge::unsignedToFloat(i) * kItemStep + kItemTop;
            position->x = ee::Float::fromBits(0);
            position->z = 0;
            position->y = down - zge::unsignedToFloat(shown - 1) * kItemHalfStep;
            zge::zgui::setVariable(itemWidgets[i], kPositionName, position);
        }

        // The page's rewards.
        items = 0;
        switch (page)
        {
        case kRewardTurn:
            for (uint32_t slot = 0; slot < kLevelGoalRewards; ++slot)
            {
                if ((turnAwards(playerRider) >> slot & 1) != 0)
                    showRewardItem(*calls::g_currentPlayer, turnReward(slot), items);
            }
            break;
        case kRewardAllGame:
            if ((turnAwards(playerRider) & kAwardAllGame) != 0)
            {
                ee::Ptr<calls::Reward> reward;
                if (gameMode() == calls::kGoalsMode)
                    reward = kAllGoalsReward;
                else
                    reward = gameMode() == kCompetitionMode ? kAllContestsReward : ee::Ptr<calls::Reward>();
                showRewardItem(*calls::g_currentPlayer, reward, items);
            }
            break;
        case kRewardAllGame02:
            if ((turnAwards(playerRider) & kAwardAllGame02) != 0)
                showRewardItem(*calls::g_currentPlayer, kAllGame02Reward, items);
            break;
        case kRewardAllGaps:
            if ((turnAwards(playerRider) & kAwardAllGaps) != 0)
                showRewardItem(*calls::g_currentPlayer, allGapsReward(*calls::g_currentLevel), items);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(showRewardPage, Verified);

    // rewardScreenMessage: the status as the screen enters (the player's
    // score its game's way, the goals and the gaps done of all with those
    // done this turn, the records set of fourteen with this game's, the
    // tricks), then the first page of rewards. Going on turns the pages
    // (the turn's, the whole game's two, the gaps'), each only when its
    // award was given, then leaves for the game over or round over screen.
    void rewardScreenMessage(int32_t message)
    {
        ee::Local<char[0x100]> text;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameReward, 1);
        if (!frame)
            return;
        playerCharacter(*calls::g_currentPlayer); // (asked for; not used)
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(*calls::g_currentPlayer, *calls::g_currentLevel);
        const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));

        switch (message)
        {
        case kRewardItemShown:
            soundEvent(kRewardSound, *g_screenStep == kRewardAllCharacters + 1 ? 1 : 0, 0, 0, 0);
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenEnter:
        {
            const ee::Ptr<Node> status = zge::zgui::findNode(frame, kFrameStatus, 1);
            if (status)
            {
                const uint8_t mode = gameMode();
                if (mode == kTricksMode)
                    zge::formatString(text, kTricksScore, playerRider->score.gameScore);
                else if (mode == kDegreesMode)
                    zge::formatString(text, kDegreesScore, playerRider->score.gameScore * kDegreesAScore, kDegrees);
                else if (mode >= kFirstDistanceMode && mode <= kLastDistanceMode)
                    formatFeetInches(text, zge::unsignedToFloat(playerRider->score.gameScore));
                else
                    zge::formatString(text, kScoreFormat, playerRider->score.gameScore);
                zge::zgui::setVariable(zge::zgui::findNode(status, kTitle1Widget, 1), kTextName, text);
            }

            // The goals that are not gaps, then the gaps: done of all, and
            // those done this turn. Only the game with goals counts them.
            for (uint32_t gaps = 0; gaps < 2; ++gaps)
            {
                if (gameMode() == calls::kGoalsMode)
                {
                    uint32_t doneThisTurn = 0;
                    for (uint32_t place = 0; place < sizeof(goals->marks); ++place)
                    {
                        const uint8_t marks = goals->marks[place];
                        if ((marks & calls::kGoalSaved) != 0 && ((marks & calls::kGoalGap) != 0) == (gaps != 0))
                            doneThisTurn += (marks & calls::kGoalDoneThisTurn) != 0 ? 1 : 0;
                    }
                    if (gaps == 0)
                        zge::formatString(text, kDoneOfAndNew, goals->goalsDone, goals->goals, doneThisTurn);
                    else
                        zge::formatString(text, kDoneOfAndNew, goals->gapsDone, goals->gaps, doneThisTurn);
                }
                else
                {
                    zge::formatString(text, kNotCounted);
                }
                zge::zgui::setVariable(zge::zgui::findNode(status, gaps == 0 ? kGoalsValue : kGapsValue, 1), kTextName, text);
            }

            // The records: the level's saved ones that are set, of
            // fourteen, and those this game set.
            uint32_t savedSet = 0, gameSet = 0;
            for (uint32_t kind = 0; kind < kCountedRecordKinds; ++kind)
            {
                const ee::Ptr<SavedRecord> saved = savedRecord(recordKey(gameMode(), kind, 0));
                if (saved)
                    savedSet += saved->flags & kSavedRecordNew;
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, 0));
                if (record)
                    gameSet += record->marks & kRecordSet;
            }
            zge::formatString(text, kDoneOfAndNew, savedSet, kCountedRecordKinds, gameSet);
            zge::zgui::setVariable(zge::zgui::findNode(status, kRecordsValue, 1), kTextName, text);

            zge::formatString(text, kTricksCount, sce::libgcc::fptoui(gameRecord(recordKey(gameMode(), kTricksRecordKind, 0))->value));
            zge::zgui::setVariable(zge::zgui::findNode(status, kTricksValue, 1), kTextName, text);
            playUiNode(frame, kInitAction);

            if (objectives::playerHasTurnAwards(*calls::g_currentPlayer) != 0)
                showRewardPage(kRewardTurn);
            else if ((turnAwards(playerRider) & kAwardAllGaps) != 0)
                showRewardPage(kRewardAllGaps);
            else
                showRewardPage(kRewardNone);
            return;
        }

        case kScreenChosen:
        {
            bool turned = false;
            if (*g_screenStep == kRewardAllGame)
            {
                if ((turnAwards(playerRider) & kAwardAllGame02) != 0)
                {
                    showRewardPage(kRewardAllGame02);
                    turned = true;
                }
            }
            else if (*g_screenStep == kRewardTurn)
            {
                if ((turnAwards(playerRider) & kAwardAllGame) != 0)
                {
                    showRewardPage(kRewardAllGame);
                    turned = true;
                }
            }
            if (!turned && *g_screenStep < kRewardAllGaps && (turnAwards(playerRider) & kAwardAllGaps) != 0)
            {
                showRewardPage(kRewardAllGaps);
                turned = true;
            }

            if (turned)
            {
                // The frame keeps the pad out for a moment again.
                stopUiNode(zge::zgui::findNode(*g_overlayUi, kFrameReward, 1), kLockUserAction);
                playUiNode(zge::zgui::findNode(*g_overlayUi, kFrameReward, 1), kLockUserAction);
            }
            else if (roundFlags(calls::kGameEndsFlag))
            {
                showGameScreen(kScreenGameOver);
            }
            else
            {
                showGameScreen(kScreenRoundOver);
            }
            return;
        }

        default:
            return;
        }
    }
    READABLE_FUNCTION(rewardScreenMessage, Verified);
}
