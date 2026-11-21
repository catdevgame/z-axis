// BMX XXX's game over screen (screens.h; "FrameGameOver"): who won or
// what was scored, and the menu of what to do next. BMX XXX's own code
// (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/records.h"
#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "bmx/weapon/player_weapon.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_game_over.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameGameOver = sym::g_textFrameGameOver; // "FrameGameOver"
        constexpr uint32_t kTitleWidget = sym::g_textTitle;           // "Title"
        constexpr uint32_t kMessageWidget = sym::g_textMessage;       // "Message"
        constexpr uint32_t kTitleText = sym::bmx::g_textGameOverTitle;          // "$KSTR_GAMEOVER_TITLE"
        constexpr uint32_t kWinnerText = sym::bmx::g_textGameOverWinner;        // "$KSTR_PLAYER $KSTR_NUMBER_%ld $KSTR_SERIES_WIN_1"
        constexpr uint32_t kTieText = sym::bmx::g_textTie;                      // "$KSTR_TIE"
        constexpr uint32_t kYourScoreLine = sym::bmx::g_textGameOverYourScore;  // "%s$KSTR_YOURSCORE %ld"
        constexpr uint32_t kPlayerScoreLine = sym::bmx::g_textGameOverPlayerScore; // "%s$KSTR_PLAYER %d $KSTR_SCORE "
        constexpr uint32_t kScoreFormat = sym::bmx::g_textRewardScore;          // "%ld"
        constexpr uint32_t kTricksScore = sym::bmx::g_textRewardTricksScore;    // "%d $KSTR_TRICKS"
        constexpr uint32_t kDegreesScore = sym::bmx::g_textRewardDegreesScore;  // "%d %s"
        constexpr uint32_t kDegrees = sym::g_textDegrees;                       // "degrees"
        constexpr uint32_t kStringFormat = sym::bmx::g_textStringFormat;        // "%s"
        // The menu's lines, each after a separator.
        constexpr uint32_t kPlayAgain0Line = sym::bmx::g_textGameOverPlayAgain0;     // "%s$KSTR_GAMEOVER_PLAYAGAIN0"
        constexpr uint32_t kPlayAgain1Line = sym::bmx::g_textGameOverPlayAgain1;     // "%s$KSTR_GAMEOVER_PLAYAGAIN1"
        constexpr uint32_t kLevelSelectLine = sym::bmx::g_textGameOverLevelSelect;   // "%s$KSTR_GAMEOVER_LEVELSELECT"
        constexpr uint32_t kSaveLine = sym::bmx::g_textGameOverSave;                 // "%s$KSTR_GAMEOVER_SAVE"
        constexpr uint32_t kViewRecordsLine = sym::bmx::g_textGameOverViewRecords;   // "%s$KSTR_GAMEOVER_VIEWRECORDS"
        constexpr uint32_t kExitLine = sym::bmx::g_textGameOverExit;                 // "%s$KSTR_GAMEOVER_EXIT"
        constexpr uint32_t kGameSelectLine = sym::bmx::g_textGameOverGameSelect;     // "%s$KSTR_GAMEOVER_GAMESELECT"

        // What a line of the menu does. The screen puts up five of them;
        // the others are handled all the same.
        enum GameOverChoice : uint8_t
        {
            kGameOverPlayAgain = 0,
            kGameOverLevelSelect = 1,
            kGameOverChangeRider = 2, // (never in the menu)
            kGameOverMenu17 = 3,      // (never in the menu) leaves for the front end's menu 17
            kGameOverSave = 4,
            kGameOverViewReplay = 5,  // (never in the menu)
            kGameOverTrickList = 6,   // (never in the menu)
            kGameOverViewRecords = 7,
            kGameOverExit = 8,
            kGameOverSaveReplay = 10, // (never in the menu)
        };
        constexpr uint8_t kFrontEndMenu17 = 17;

        // The game modes with their own endings: the series (5) and the
        // word game (6); the single-player games are below 4.
        constexpr uint8_t kFirstMultiplayerMode = 4, kSeriesMode = 5, kWordGameMode = 6;
        // The modes scored in other than points.
        constexpr uint8_t kFirstDistanceMode = 9, kLastDistanceMode = 16, kTricksMode = 12, kDegreesMode = 13;
        constexpr uint32_t kDegreesAScore = 180;
        // The span of the record that names a multiplayer game's winner.
        constexpr uint32_t kWinnerRecordSpan = 1;
        // A character needs two levels open for "level select".
        constexpr uint32_t kLevelsForSelect = 2;
        // The button that takes the shown replay's pad (padButtons' "held").
        constexpr uint32_t kReplayButton = 0x80, kButtonHeld = 2;

        ee::Ptr<char> endOf(ee::Ptr<char> text) { return text + zge::stringLength(text); }
        uint32_t separatorAfter(uint32_t lines) { return lines != 0 ? kLineBreak : kNoSeparator; }
    }

    void gameOverScreenMessage(int32_t message)
    {
        ee::Local<char[0x200]> items;
        ee::Local<char[0x200]> text;
        ee::Local<char[0x40]> title;
        ee::Local<char[0x40]> score;
        ee::Local<uint32_t> selected;
        ee::Local<uint32_t> lines;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameGameOver, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 1), kSelectedIndexName, selected);
            switch ((*g_menuChoices)[selected])
            {
            case kGameOverLevelSelect:
                *g_screenBehindExitConfirm = kScreenGameOver;
                showGameScreen(kScreenExitLevel);
                *g_gameScreenChanged = 0;
                *g_frontEndFirstMenu = kFrontEndLevelSelect;
                return;
            case kGameOverChangeRider:
                *g_screenBehindExitConfirm = kScreenGameOver;
                showGameScreen(kScreenExitLevel);
                *g_gameScreenChanged = 0;
                *g_frontEndFirstMenu = kFrontEndChangeRider;
                return;
            case kGameOverMenu17:
                *g_screenBehindExitConfirm = kScreenGameOver;
                showGameScreen(kScreenExitLevel);
                *g_gameScreenChanged = 0;
                *g_frontEndFirstMenu = kFrontEndMenu17;
                return;
            case kGameOverSave:
                showGameScreen(kScreenSaveGame);
                return;
            case kGameOverViewReplay:
                // The replay's pad is the current player's, or the last
                // player's for whom the current player's pad has the
                // button down (the pad asked is the current player's each
                // time: kept).
                showGameScreen(kScreenReplay);
                if (*calls::g_playerCount == 0)
                    return;
                *calls::g_replayPadPlayer = *calls::g_currentPlayer;
                for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
                {
                    if (rider::padButtons(*calls::g_currentPlayer, kReplayButton, kButtonHeld) != 0)
                        *calls::g_replayPadPlayer = static_cast<uint8_t>(player);
                }
                return;
            case kGameOverTrickList:
                showGameScreen(kScreenGameOverTrickList);
                return;
            case kGameOverViewRecords:
                showGameScreen(kScreenGameOverRecords);
                return;
            case kGameOverExit:
                if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
                {
                    *g_screenBehindExitConfirm = kScreenGameOver;
                    showGameScreen(kScreenExitLevel);
                    *g_frontEndFirstMenu = kFrontEndAfterRound;
                }
                else
                {
                    // Back to the multiplayer game's select: the riders,
                    // weapons and cameras put away for the game camera.
                    for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
                    {
                        rider::riderReset(riderIndexOf(player), 0);
                        resetWeapon(playerWeapon(player), 0);
                        showCamera(playerActionCamera(player), 0);
                    }
                    showCamera(*calls::g_gameCamera, 1);
                    showGameScreen(kScreenAfterGame);
                }
                *g_gameScreenChanged = 0;
                return;
            case kGameOverSaveReplay:
                showGameScreen(kScreenSaveReplay);
                return;
            default: // play again
                if (gameMode() == calls::kGoalsMode)
                {
                    startGame(gameMode());
                    setRoundPhase(calls::kTurnStartPhase);
                }
                else
                {
                    showGameScreen(kScreenPlay);
                    *g_resumeAction = kResumeNewGame;
                }
                return;
            }

        case kScreenEnter:
        {
            text[0] = '\0';
            uint32_t scoreLines = 0;
            items[0] = '\0';
            lines = 0;
            const uint8_t mode = gameMode();
            if (mode == kSeriesMode)
            {
                if (!roundFlags(calls::kTieFlag))
                    zge::formatString(title, kWinnerText, roundWinner(roundNumber(), 1));
                else
                    zge::formatString(title, kTieText);
                calls::seriesStandingText(text);
                (*g_menuChoices)[lines] = kGameOverPlayAgain;
                zge::formatString(endOf(ee::Ptr<char>(items)), kPlayAgain1Line, separatorAfter(lines));
                lines = lines + 1;
            }
            else if (mode == kWordGameMode)
            {
                zge::formatString(title, kWinnerText, roundWinner(roundNumber(), 1));
                calls::wordGameStandingText(text);
                (*g_menuChoices)[lines] = kGameOverPlayAgain;
                zge::formatString(endOf(ee::Ptr<char>(items)), kPlayAgain1Line, separatorAfter(lines));
                lines = lines + 1;
            }
            else if (mode >= kFirstMultiplayerMode)
            {
                // The winner is the player of the mode's own record; then
                // every player's score, in a mode that keeps one.
                if (!roundFlags(calls::kTieFlag))
                {
                    const uint32_t recordMode = gameMode();
                    const uint32_t slotMode = gameMode(); // (asked for twice: kept)
                    const ee::Ptr<GameRecord> record =
                        gameRecord(recordKey(recordMode, calls::modeRecordSlots()[slotMode], kWinnerRecordSpan));
                    zge::formatString(title, kWinnerText, record->player);
                }
                else
                {
                    zge::formatString(title, kTieText);
                }
                if (gameModeRecord()->scored != 0)
                {
                    for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
                    {
                        const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(player));
                        zge::formatString(endOf(ee::Ptr<char>(text)), kPlayerScoreLine, separatorAfter(scoreLines), player + 1);
                        ++scoreLines;
                        const uint8_t scored = gameMode();
                        if (scored == kTricksMode)
                            zge::formatString(score, kTricksScore, playerRider->score.gameScore);
                        else if (scored == kDegreesMode)
                            zge::formatString(score, kDegreesScore, playerRider->score.gameScore * kDegreesAScore, kDegrees);
                        else if (scored >= kFirstDistanceMode && scored <= kLastDistanceMode)
                            formatFeetInches(score, zge::unsignedToFloat(playerRider->score.gameScore));
                        else
                            zge::formatString(score, kScoreFormat, playerRider->score.gameScore);
                        zge::formatString(endOf(ee::Ptr<char>(text)), kStringFormat, score);
                    }
                }
                (*g_menuChoices)[lines] = kGameOverPlayAgain;
                zge::formatString(endOf(ee::Ptr<char>(items)), kPlayAgain1Line, separatorAfter(lines));
                lines = lines + 1;
            }
            else
            {
                // A single-player game: the score (the competition and the
                // free ride show none), play again, the level select for a
                // character with two levels open, and the save.
                playerCharacter(*calls::g_currentPlayer); // (asked for; not used)
                const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(*calls::g_currentPlayer));
                zge::formatString(title, kTitleText);
                const uint8_t played = gameMode();
                if (played != kCompetitionMode && played != calls::kFreeMode)
                    zge::formatString(endOf(ee::Ptr<char>(text)), kYourScoreLine, kNoSeparator, playerRider->score.gameScore);

                (*g_menuChoices)[lines] = kGameOverPlayAgain;
                zge::formatString(endOf(ee::Ptr<char>(items)), kPlayAgain0Line, separatorAfter(lines));
                lines = lines + 1;

                uint32_t levelsOpen = 0;
                for (uint32_t level = 0; level < calls::kLevels; ++level)
                    levelsOpen += playerLevelGoals(*calls::g_currentPlayer, level)->flags & kLevelGoalsOpen;
                const bool selectable = gameMode() < calls::kSessionMode || *calls::g_sessionKind != calls::kSessionParkTest;
                if (selectable && levelsOpen >= kLevelsForSelect)
                {
                    (*g_menuChoices)[lines] = kGameOverLevelSelect;
                    zge::formatString(endOf(ee::Ptr<char>(items)), kLevelSelectLine, separatorAfter(lines));
                    lines = lines + 1;
                }

                const bool startedPlain = static_cast<uint8_t>(*calls::g_startUpKind - 1) >= 3;
                if (startedPlain && gameMode() != calls::kFreeMode && *calls::g_currentLevel < calls::kLevels)
                {
                    (*g_menuChoices)[lines] = kGameOverSave;
                    zge::formatString(endOf(ee::Ptr<char>(items)), kSaveLine, separatorAfter(lines));
                    lines = lines + 1;
                }
            }

            if (*calls::g_sessionKind != calls::kSessionParkTest)
            {
                (*g_menuChoices)[lines] = kGameOverViewRecords;
                zge::formatString(endOf(ee::Ptr<char>(items)), kViewRecordsLine, separatorAfter(lines));
                lines = lines + 1;
            }
            (*g_menuChoices)[lines] = kGameOverExit;
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
                zge::formatString(endOf(ee::Ptr<char>(items)), kExitLine, separatorAfter(lines));
            else
                zge::formatString(endOf(ee::Ptr<char>(items)), kGameSelectLine, separatorAfter(lines));
            lines = lines + 1;

            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleWidget, 1), kTextName, title);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 1), kItemsName, items);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 1), kVisibleCountName, lines);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageWidget, 1), kTextName, text);
            playUiNode(frame, kInitAction);
            setCameraState(calls::kCameraStateScreen);
            return;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(gameOverScreenMessage, Verified);
}
