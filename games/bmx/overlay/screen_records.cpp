// BMX XXX's records and round over screens (screens.h; "FrameRecords",
// "FrameRoundOver"): the level's records, and a finished run's score with
// what to do next. BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_records.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameRecords = sym::g_textFrameRecords;     // "FrameRecords"
        constexpr uint32_t kFrameRoundOver = sym::g_textFrameRoundOver; // "FrameRoundOver"
        constexpr uint32_t kMessageWidget = sym::g_textMessage;         // "Message"
        constexpr uint32_t kYourScore = sym::bmx::g_textYourScore;             // "$KSTR_YOURSCORE %ld"
        constexpr uint32_t kYourJudgedScore = sym::bmx::g_textYourJudgedScore; // "$KSTR_YOURSCORE %02d.%d"

        // The first of the multiplayer modes that end a turn with the
        // game's own next turn (8 .. 15), and the one before them with a
        // screen after each round.
        constexpr uint8_t kModeWithRoundScreen = 6;
        constexpr uint8_t kFirstPlainTurnsMode = 8, kLastPlainTurnsMode = 15;


        // The round over menu's lines ("$KSTR_ROUNDOVER_CONTINUE",
        // "_LEADERB", "_VIEWRECORDS", "_EXIT").
        enum RoundOverLine : uint32_t
        {
            kRoundOverContinue = 0,
            kRoundOverLeaderboard = 1,
            kRoundOverViewRecords = 2,
            kRoundOverExit = 3,
        };

        void showFrameBelow(uint32_t frame, ee::Local<uint32_t> &visible)
        {
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, frame, 1), kVisibleName, visible);
        }
    }

    // recordsScreenMessage: the level's records, with "continue" below
    // after a turn that beat some and "go back" below otherwise. Going on
    // after such a turn is the game mode's: the goals' reward, the
    // competition's leaderboard, the game over screen (at once in the
    // session and free modes, in a multiplayer game when the game ends),
    // or the multiplayer game's next screen or turn.
    void recordsScreenMessage(int32_t message)
    {
        ee::Local<uint32_t> visible;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameRecords, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
            fillRecordsFrame(frame, *calls::g_currentLevel);
            if (*g_gameScreen == kScreenNewRecords)
            {
                visible = 1;
                showFrameBelow(kFrameContinue, visible);
            }
            else if (*g_gameScreen == kScreenRecords || *g_gameScreen == kScreenRoundOverRecords ||
                     *g_gameScreen == kScreenGameOverRecords)
            {
                visible = 1;
                showFrameBelow(kFrameGoBack, visible);
            }
            playUiNode(frame, kInitAction);
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenLeft:
            visible = 0;
            showFrameBelow(kFrameContinue, visible);
            showFrameBelow(kFrameGoBack, visible);
            return;

        case kScreenChosen:
        {
            if (*g_gameScreen != kScreenNewRecords)
                return;
            const uint8_t mode = gameMode();
            if (mode == calls::kGoalsMode)
            {
                showGameScreen(kScreenReward);
            }
            else if (mode == kCompetitionMode)
            {
                showGameScreen(kScreenLeaderboard);
            }
            else if (mode == calls::kSessionMode || mode == calls::kFreeMode)
            {
                showGameScreen(kScreenGameOver);
            }
            else if (mode == kModeWithRoundScreen)
            {
                if (roundFlags(calls::kGameEndsFlag))
                {
                    showGameScreen(kScreenGameOver);
                }
                else if (roundFlags(calls::kRoundDoneFlag))
                {
                    showGameScreen(kScreenAfterTurn);
                }
                else
                {
                    showGameScreen(kScreenPlay);
                    *g_resumeAction = kResumeTurnStart;
                }
            }
            else if (mode >= kFirstPlainTurnsMode && mode <= kLastPlainTurnsMode)
            {
                if (roundFlags(calls::kGameEndsFlag))
                {
                    showGameScreen(kScreenGameOver);
                }
                else
                {
                    showGameScreen(kScreenPlay);
                    *g_resumeAction = kResumeTurnStart;
                }
            }
            else
            {
                if (roundFlags(calls::kGameEndsFlag))
                    showGameScreen(kScreenGameOver);
                else
                    showGameScreen(kScreenAfterTurn);
            }
            return;
        }

        case kScreenCancelled:
            if (*g_gameScreen == kScreenRecords)
                showGameScreen(kScreenPause);
            else if (*g_gameScreen == kScreenRoundOverRecords)
                showGameScreen(kScreenRoundOver);
            else if (*g_gameScreen == kScreenGameOverRecords)
                showGameScreen(kScreenGameOver);
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(recordsScreenMessage, Verified);

    // roundOverScreenMessage: the current player's score (a competition
    // run's is the judges', to a tenth of a point), and the menu's line
    // done: the next turn, the leaderboard, the records, or the exit (asked
    // first).
    void roundOverScreenMessage(int32_t message)
    {
        ee::Local<char[0x200]> text;
        ee::Local<uint32_t> selected;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameRoundOver, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 1), kSelectedIndexName, selected);
            switch (selected)
            {
            case kRoundOverContinue:
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeTurnStart;
                return;
            case kRoundOverLeaderboard:
                showGameScreen(kScreenLeaderboard);
                return;
            case kRoundOverViewRecords:
                showGameScreen(kScreenRoundOverRecords);
                return;
            case kRoundOverExit:
                *g_screenBehindExitConfirm = kScreenRoundOver;
                showGameScreen(kScreenExitLevel);
                *g_frontEndFirstMenu = kFrontEndAfterRound;
                return;
            default:
                return;
            }

        case kScreenEnter:
        {
            const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));
            if (gameMode() == kCompetitionMode)
            {
                const uint8_t judged = playerRider->competitionRuns[roundNumber()].mark;
                const JudgedPoints points = judgedPoints(judged);
                zge::formatString(text, kYourJudgedScore, points.whole, points.tenths);
            }
            else
            {
                zge::formatString(text, kYourScore, playerRider->score.gameScore);
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageWidget, 1), kTextName, text);
            playUiNode(frame, kInitAction);
            return;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(roundOverScreenMessage, Verified);
}
