// BMX XXX's competition leaderboard (screens.h; "FrameLeaderboard"): the
// five judges' scores of the run just ridden, the player's runs and
// average, and the field of ten with each rider's runs and average. BMX
// XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/round.h"
#include "bmx/game/turn_rewards.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_leaderboard.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameLeaderboard = sym::g_textFrameLeaderboard; // "FrameLeaderboard"
        constexpr uint32_t kFrameScores = sym::g_textFrameScores;           // "FrameScores"
        constexpr uint32_t kFrameBoard = sym::g_textFrameBoard;             // "FrameBoard"
        constexpr uint32_t kDataName = sym::g_textDataName;                 // "DataName"
        constexpr uint32_t kDataRun1 = sym::g_textDataRun1;                 // "DataRun1"
        constexpr uint32_t kDataRun2 = sym::g_textDataRun2;                 // "DataRun2"
        constexpr uint32_t kDataAverage = sym::g_textDataAverage;           // "DataAverage"
        constexpr uint32_t kFadeInAction = sym::g_textFadeIn;               // "FadeIn"
        constexpr uint32_t kPoints = sym::bmx::g_textPoints;                // "%02d.%d"
        constexpr uint32_t kItemValue = sym::bmx::g_textItemValue;          // "Item%02d.Value"
        constexpr uint32_t kItem = sym::bmx::g_textItem;                    // "Item%02d"
        constexpr uint32_t kAverageItemValue = sym::bmx::g_textItem07Value; // "Item07.Value"
        constexpr uint32_t kAverageItem = sym::bmx::g_textItem07;           // "Item07"
        // "FrameScores" has an item a judge (0 .. 4), an item a run after
        // them, and the average's.
        constexpr uint32_t kJudges = 5;
        constexpr uint32_t kFirstRunItem = 5;

        // A competition's scores, by rider character: each run's, and
        // their average (a byte each, 255 for 100 points).
        struct CompetitionScores
        {
            uint8_t run[kCompetitionRuns];
            uint8_t average;
        };
        inline ee::Ptr<CompetitionScores> competitionScores(uint32_t character)
        {
            return ee::Ptr<CompetitionScores>(sym::g_competitionScores) + character;
        }
        // The field of ten in the leaderboard's order.
        constexpr uint32_t kCompetitionRiders = 10;
        inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRanking> g_competitionRanking;


        // The frame's own message: an item of "FrameScores" starts to fade in.
        constexpr int32_t kLeaderboardItemShown = 3;
        constexpr uint32_t kItemSound = 0x41;
    }

    void leaderboardScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> name;
        ee::Local<char[0x100]> value;
        ee::Local<char[0x100]> names;
        ee::Local<char[kCompetitionRuns][0x40]> runs;
        ee::Local<char[0x40]> averages;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameLeaderboard, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kLeaderboardItemShown:
            soundEvent(kItemSound, 0, 0, 0, 0);
            return;

        case kScreenChosen:
        {
            if (roundFlags(calls::kGameEndsFlag))
            {
                showGameScreen(kScreenReward);
                return;
            }
            const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));
            // With the level's gaps all done the leaderboard goes on to the
            // reward screen, as it does at the game's end.
            if ((objectives::riderObjectives(playerRider)->turnAwards & kAwardAllGaps) != 0)
                showGameScreen(kScreenReward);
            else
                showGameScreen(kScreenRoundOver);
            return;
        }

        case kScreenEnter:
        {
            const ee::Ptr<calls::PlayerSetup> setup = calls::playerSetup(*calls::g_currentPlayer);
            const uint32_t character = setup->character;
            ee::Ptr<Node> scores = zge::zgui::findNode(frame, kFrameScores, 1);
            const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));

            // The run just ridden: each judge's score.
            const uint32_t run = roundNumber();
            for (uint32_t judge = 0; judge < kJudges; ++judge)
            {
                const JudgedPoints points = judgedPoints(playerRider->competitionRuns[run].judges[judge]);
                zge::formatString(value, kPoints, points.whole, points.tenths);
                zge::formatString(name, kItemValue, judge);
                zge::zgui::setVariable(zge::zgui::findNode(scores, name, 1), kTextName, value);
            }
            // The player's runs so far, and their average after the first.
            for (uint32_t ridden = 0; ridden <= roundNumber(); ++ridden)
            {
                const JudgedPoints points = judgedPoints(playerRider->competitionRuns[ridden].mark);
                zge::formatString(value, kPoints, points.whole, points.tenths);
                zge::formatString(name, kItemValue, ridden + kFirstRunItem);
                zge::zgui::setVariable(zge::zgui::findNode(scores, name, 1), kTextName, value);
            }
            if (roundNumber() != 0)
            {
                const JudgedPoints points = judgedPoints(competitionScores(character)->average);
                zge::formatString(value, kPoints, points.whole, points.tenths);
                zge::formatString(name, kAverageItemValue);
                zge::zgui::setVariable(zge::zgui::findNode(scores, name, 1), kTextName, value);
            }

            // The board: a line a rider in the ranking's order. A custom
            // rider is listed under the player's name.
            const ee::Ptr<Node> board = zge::zgui::findNode(frame, kFrameBoard, 1);
            names[0] = '\0';
            runs[0][0] = '\0';
            runs[1][0] = '\0';
            averages[0] = '\0';
            for (uint32_t row = 0; row < kCompetitionRiders; ++row)
            {
                const uint32_t listed = (*g_competitionRanking)[row];
                const uint32_t separator = row != 0 ? kLineBreak : kNoSeparator;
                ee::Ptr<const char> riderName;
                if (listed == character && setup->character >= calls::kFirstCustomCharacter)
                    riderName = ee::Ptr<const char>(&setup->name[0]);
                else
                    riderName = calls::characterNames(listed)->boardName;
                zge::formatString(ee::Ptr<char>(names) + zge::stringLength(names), kSeparatorAndLine, separator, riderName);

                for (uint32_t ridden = 0; ridden < static_cast<uint32_t>(roundNumber()) + 1; ++ridden)
                {
                    const JudgedPoints points = judgedPoints(competitionScores(listed)->run[ridden]);
                    zge::formatString(value, kPoints, points.whole, points.tenths);
                    const ee::Ptr<char> column{&runs[ridden][0]};
                    zge::formatString(column + zge::stringLength(column), kSeparatorAndLine, separator, value);
                }
                const JudgedPoints averagePoints = judgedPoints(competitionScores(listed)->average);
                zge::formatString(value, kPoints, averagePoints.whole, averagePoints.tenths);
                zge::formatString(ee::Ptr<char>(averages) + zge::stringLength(averages), kSeparatorAndLine, separator, value);
            }
            zge::zgui::setVariable(zge::zgui::findNode(board, kDataName, 1), kTextName, names);
            zge::zgui::setVariable(zge::zgui::findNode(board, kDataRun1, 1), kTextName, ee::Ptr<char>(&runs[0][0]));
            zge::zgui::setVariable(zge::zgui::findNode(board, kDataRun2, 1), kTextName, ee::Ptr<char>(&runs[1][0]));
            zge::zgui::setVariable(zge::zgui::findNode(board, kDataAverage, 1), kTextName, averages);
            playUiNode(frame, kInitAction);

            // The runs' items and the average's fade in.
            scores = zge::zgui::findNode(frame, kFrameScores, 1);
            for (uint32_t ridden = 0; ridden <= roundNumber(); ++ridden)
            {
                zge::formatString(name, kItem, ridden + kFirstRunItem);
                playUiNode(zge::zgui::findNode(scores, name, 1), kFadeInAction);
            }
            if (roundNumber() != 0)
            {
                zge::formatString(name, kAverageItem);
                playUiNode(zge::zgui::findNode(scores, name, 1), kFadeInAction);
            }
            return;
        }

        case kScreenLeaveNow:
        {
            const ee::Ptr<Node> scores = zge::zgui::findNode(frame, kFrameScores, 1);
            for (uint32_t ridden = 0; ridden < static_cast<uint32_t>(roundNumber()) + 1; ++ridden)
            {
                zge::formatString(name, kItem, ridden + kFirstRunItem);
                playUiNode(zge::zgui::findNode(scores, name, 1), kShutdownAction);
            }
            if (roundNumber() != 0)
            {
                zge::formatString(name, kAverageItem);
                playUiNode(zge::zgui::findNode(scores, name, 1), kShutdownAction);
            }
            playUiNode(frame, kShutdownAction);
            return;
        }

        default:
            return;
        }
    }
    READABLE_FUNCTION(leaderboardScreenMessage, Verified);
}
