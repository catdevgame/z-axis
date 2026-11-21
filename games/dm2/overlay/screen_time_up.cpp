// DM2's multiplayer time up screen (screens.h; "InGameUI.FrameMPTimeUp"): what
// it shows as a turn's time is up (the turn's score against the best, the
// round's winner, the standings) and its menu's script function. DM2's own
// code (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_time_up.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kTimeUpFrame{sym::g_textFrameMultiplayerTimeUp}; // "InGameUI.FrameMPTimeUp"
        // The menu's lines, in its order.
        enum TimeUpLine : int32_t
        {
            kTimeUpGoOn = 0,       // the next turn, or the game over when this round was the last
            kTimeUpViewReplay = 1, // the round's replay, from the current player's view
            kTimeUpExit = 2,       // leave the game (the exit confirmation)
        };
    }

    namespace
    {
        constexpr ee::Ptr<const char> kTitleName{sym::screenTitleName};           // "Title"
        constexpr ee::Ptr<const char> kMessageName{sym::g_textMessage};           // "Message"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};             // "Text"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};               // "Items"
        constexpr ee::Ptr<const char> kVisibleCountName{sym::g_textVisibleCount}; // "VisibleCount"
        constexpr ee::Ptr<const char> kPlayerTitle{sym::multiplayerPlayerKey};    // "$KSTR_PLAYER $KSTR_NUMBER_%d"
        constexpr ee::Ptr<const char> kTieTitle{sym::g_textTie};                  // "$KSTR_TIE"
        constexpr ee::Ptr<const char> kWinnerTitle{sym::timeUpWinnerKey};         // "$KSTRMP_MPTIMEUP_WINNER"
        constexpr ee::Ptr<const char> kTimeUpTitle{sym::timeUpTitleKey};          // "$KSTRMP_MPTIMEUP_TIMEUP"
        constexpr ee::Ptr<const char> kReplayRound{sym::timeUpReplayRoundKey};    // "$KSTRMP_MPTIMEUP_REPLAYROUND"
        constexpr ee::Ptr<const char> kScoreText{sym::multiplayerScoreKey};       // "$KSTR_SCORE %6ld"
        constexpr ee::Ptr<const char> kTurnScoreText{sym::scoreNumberKey};        // "$KSTR_SCORE %ld"
        constexpr ee::Ptr<const char> kTotalScoreText{sym::totalScoreKey};        // "$KSTR_TOTALSCORE %6ld"
        constexpr ee::Ptr<const char> kAllTimeBest{sym::allTimeBestPointsKey};    // "$KSTR_ALLTIMEBEST %6ld"
        constexpr ee::Ptr<const char> kPlayerScoreLine{sym::playerScoreLineKey};  // "$KSTR_PLAYER $KSTR_NUMBER_%d : "
        constexpr ee::Ptr<const char> kPointsFormat{sym::pointsValueFormat};      // "%6ld"
        constexpr ee::Ptr<const char> kWinnerPoints{sym::winnerPointsFormat};     // "#RGB{238 181 44}%6ld#RGB{192 192 192}"
        constexpr ee::Ptr<const char> kGoOnLine{sym::menuOption0Key};             // "$KSTR_OPTIONS_0"
        constexpr ee::Ptr<const char> kViewReplayLine{sym::menuOption1Key};       // "$KSTR_OPTIONS_1"
        constexpr ee::Ptr<const char> kExitLine{sym::menuOption2Key};             // "$KSTR_OPTIONS_2"
        constexpr ee::Ptr<const char> kLineJoin{sym::trickWordFormat}; // "%s%s"
        constexpr ee::Ptr<const char> kNoBreak{sym::g_textEmpty};
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};
        // roundWinner's count for the round's own winner.
        constexpr uint32_t kRoundsOfOne = 2;

        // `text` added to `out` as a line of its own (after a break unless
        // it is the first of `lines`).
        void addLine(ee::Ptr<char> out, int32_t lines, ee::Ptr<const char> text)
        {
            const int32_t length = zge::stringLength(out);
            zge::formatString(out + length, kLineJoin, lines != 0 ? kLineBreak : kNoBreak, text);
        }
        // The current game's saved record of `kind`.
        ee::Ptr<GameRecord> savedRecord(uint32_t kind)
        {
            const uint32_t mode = multiplayerMode();
            return gameRecord(recordKey(mode, singlePlayerMode(), kind, kSavedRecords));
        }
    }

    // showMultiplayerTimeUpScreen: as a turn's time is up, the current
    // player's pad answers. Mode 1: whose turn, its score and the all-time
    // best game score. A series: before the round's last turn whose turn,
    // its score and the all-time best turn; at the round's end the winner
    // (each player's turn score, the winner's in colour) and the all-time
    // best turn, or a tie and the round's replay; then the standings after
    // the first round. HORSE: at the round's end each player's letters.
    // Random spawns: whose turn, its score, the best turn, the total and the
    // best game score. The menu: go on, the replay, the exit.
    void showMultiplayerTimeUpScreen(uint32_t shown)
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[0x400]> message;
        ee::Local<int32_t> menuLines;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kTimeUpFrame);
        if (!shown)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        int32_t lines = 0;
        *g_uiModalPlayer = *g_currentPlayer;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(*g_currentPlayer);
        const rider::ScoreBlock &score = rider->score;
        message[0] = 0;
        switch (multiplayerMode())
        {
        case 1:
        {
            zge::formatString(text, kPlayerTitle, *g_currentPlayer);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, text);
            const ee::Ptr<GameRecord> best = savedRecord(kGameScoreRecord);
            zge::formatString(text, kScoreText, sce::libgcc::fptoui(score.turnScore));
            addLine(message, lines++, text);
            zge::formatString(text, kAllTimeBest, sce::libgcc::fptoui(best->value));
            addLine(message, lines++, text);
            break;
        }
        case kSeriesMode:
        {
            const ee::Ptr<GameRecord> bestTurn = savedRecord(kTurnScoreRecord);
            if (!roundFlags(kRoundDoneFlag))
            {
                zge::formatString(text, kPlayerTitle, *g_currentPlayer);
                zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, text);
                zge::formatString(text, kTurnScoreText, sce::libgcc::fptoui(score.turnScore));
                addLine(message, lines++, text);
                zge::formatString(text, kAllTimeBest, sce::libgcc::fptoui(bestTurn->value));
                addLine(message, lines++, text);
            }
            else if (roundFlags(kRoundDrawnFlag))
            {
                zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, kTieTitle);
                addLine(message, lines++, kReplayRound);
            }
            else
            {
                zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, kWinnerTitle);
                for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                {
                    zge::formatString(text, kPlayerScoreLine, player);
                    const uint32_t round = roundNumber();
                    const bool won = player == roundWinner(round, kRoundsOfOne);
                    const int32_t length = zge::stringLength(text);
                    const ee::Ptr<rider::Rider> ridden = rider::riderOf(riderIndexOf(player));
                    zge::formatString(ee::Ptr<char>(text) + length, won ? kWinnerPoints : kPointsFormat,
                                      sce::libgcc::fptoui(ridden->score.turnScore));
                    addLine(message, lines++, text);
                }
                zge::formatString(text, kAllTimeBest, sce::libgcc::fptoui(bestTurn->value));
                addLine(message, lines++, text);
            }
            if (roundFlags(kRoundDoneFlag) || roundNumber() != 0)
            {
                seriesStandingText(text);
                addLine(message, lines, text);
            }
            break;
        }
        case kHorseMode:
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, kTimeUpTitle);
            if (roundFlags(kRoundDoneFlag))
            {
                horseLettersText(text);
                addLine(message, lines, text);
            }
            break;
        case kRandomSpawnMode:
        {
            zge::formatString(text, kPlayerTitle, *g_currentPlayer);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, text);
            const ee::Ptr<GameRecord> bestTurn = savedRecord(kTurnScoreRecord);
            zge::formatString(text, kScoreText, sce::libgcc::fptoui(score.turnScore));
            addLine(message, lines++, text);
            zge::formatString(text, kAllTimeBest, sce::libgcc::fptoui(bestTurn->value));
            addLine(message, lines++, text);
            const ee::Ptr<GameRecord> bestGame = savedRecord(kGameScoreRecord);
            zge::formatString(text, kTotalScoreText, score.gameScore);
            addLine(message, lines++, text);
            zge::formatString(text, kAllTimeBest, sce::libgcc::fptoui(bestGame->value));
            addLine(message, lines++, text);
            break;
        }
        default:
            break;
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, 0), kTextName, message);

        *menuLines = 0;
        text[0] = 0;
        addLine(text, *menuLines, kGoOnLine);
        ++*menuLines;
        addLine(text, *menuLines, kViewReplayLine);
        ++*menuLines;
        addLine(text, *menuLines, kExitLine);
        ++*menuLines;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kItemsName, text);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kVisibleCountName, menuLines);
        playWidgetAction(frame, kInitAction);
    }
    READABLE_FUNCTION(showMultiplayerTimeUpScreen, Verified);

    // bmxoverlayMPTimeUpHandler: the menu's line taken: going on (the next
    // turn, or the game over at the game's last round), the round's replay,
    // or the exit confirmation (which comes back to this screen).
    void bmxoverlayMPTimeUpHandler()
    {
        ee::Local<int32_t> picked;
        zge::zgui::copyVariable(zge::zgui::findNode(uiWidget(kTimeUpFrame), kMenuName, 0), kSelectedIndexName, picked);
        switch (*picked)
        {
        case kTimeUpGoOn:
            showGameScreen(kScreenNone);
            setRoundPhase(roundFlags(kGameEndsFlag) ? kGameOverPhase : kTurnStartPhase);
            break;
        case kTimeUpViewReplay:
            showGameScreen(kScreenNone);
            actionStart(kActionRoundReplay);
            *g_actionRequest = static_cast<int8_t>(*g_currentPlayer);
            break;
        case kTimeUpExit:
            *g_screenBehindExitConfirm = kScreenMultiplayerTimeUp;
            showGameScreen(kScreenExitConfirm);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayMPTimeUpHandler, Verified);
}
