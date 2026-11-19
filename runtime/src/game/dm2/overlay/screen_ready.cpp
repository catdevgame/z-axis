// DM2's multiplayer ready screen (screens.h; "InGameUI.FrameMPReady"): what
// it shows as a turn is about to start (whose turn, what to beat, HORSE's
// line to match, the standings) and its menu's script function. DM2's own
// code (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "overlay.h"

#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "dm2/game/score.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_ready.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kReadyFrame{sym::g_textFrameMultiplayerReady}; // "InGameUI.FrameMPReady"
        // The menu's lines: ready (the turn starts), in HORSE a second line
        // (the round's replay to match once the first turn is ridden, the
        // trick list before it), and the exit. Without the HORSE line the
        // second is the exit.
        constexpr int32_t kReadyGo = 0, kReadyHorseLine = 1, kReadyExit = 2, kReadyHorseExit = 3;
        // What the HORSE line is: bit 0 the first turn has been ridden; bit 4 there is one.
        constexpr uint32_t kHorseLineReplay = 0x01, kHorseLineShown = 0x10;
    }

    namespace
    {
        constexpr ee::Ptr<const char> kTopTitle{sym::screenTopHeaderTitlePath}; // "CommonUI.TopHeader.Title"
        constexpr ee::Ptr<const char> kTitleName{sym::screenTitleName};         // "Title"
        constexpr ee::Ptr<const char> kMessageName{sym::g_textMessage};         // "Message"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};           // "Text"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};             // "Items"
        constexpr ee::Ptr<const char> kVisibleCountName{sym::g_textVisibleCount}; // "VisibleCount"
        // The top header: the round (a series, random spawns), "ready"
        // (modes 1 and HORSE), else which attempt.
        constexpr ee::Ptr<const char> kRoundTitle{sym::roundTitleKey};             // "$KSTR_ROUND %ld"
        constexpr ee::Ptr<const char> kReadyTitle{sym::readyTitleKey};             // "$KSTRMP_MPREADY_READY"
        constexpr ee::Ptr<const char> kAttemptTitle{sym::attemptTitleKey};         // "$KSTR_ORDINAL_%d $KSTR_ATTEMPT"
        constexpr ee::Ptr<const char> kLastAttemptTitle{sym::lastAttemptTitleKey}; // "$KSTR_ORDINAL_LAST $KSTR_ATTEMPT"
        constexpr ee::Ptr<const char> kPlayerTitle{sym::multiplayerPlayerKey};     // "$KSTR_PLAYER $KSTR_NUMBER_%d"
        // The message.
        constexpr ee::Ptr<const char> kHorseInstructions{sym::readyHorseInstructionsKey}; // "$KSTRMP_MPREADY_INST_HORSE"
        constexpr ee::Ptr<const char> kTrickListTitle{sym::readyTrickListKey};          // "$KSTRMP_MPREADY_TRICKLIST"
        constexpr ee::Ptr<const char> kTrickColour{sym::trickListColour};               // "#RGB{238 181 44}"
        constexpr uint32_t kTrickLineChars = 30;
        constexpr ee::Ptr<const char> kScoreText{sym::multiplayerScoreKey};    // "$KSTR_SCORE %6ld"
        constexpr ee::Ptr<const char> kScoreToBeat{sym::scoreToBeatKey};       // "$KSTR_SCORETOBEAT "
        constexpr ee::Ptr<const char> kAllTimeBest{sym::allTimeBestKey};       // "$KSTR_ALLTIMEBEST "
        constexpr ee::Ptr<const char> kPointsFormat{sym::pointsValueFormat};   // "%6ld"
        constexpr ee::Ptr<const char> kTricksFormat{sym::tricksValueFormat};   // "%2ld tricks"
        constexpr ee::Ptr<const char> kDegreesFormat{sym::degreesValueFormat}; // "%4ld degrees"
        constexpr uint32_t kDegreesPerHalfTurn = 180; // (mode 10 scores half turns)
        // The menu's lines (kReadyGo and the others).
        constexpr ee::Ptr<const char> kGoLine{sym::menuOption0Key};                   // "$KSTR_OPTIONS_0"
        constexpr ee::Ptr<const char> kHorseReplayLine{sym::menuOption1Key};          // "$KSTR_OPTIONS_1"
        constexpr ee::Ptr<const char> kTrickListLine{sym::roundOverTrickListKey};    // "$KSTR_ROUNDOVER_TRICKLIST"
        constexpr ee::Ptr<const char> kExitLine{sym::menuOption2Key};                 // "$KSTR_OPTIONS_2"
        constexpr ee::Ptr<const char> kLineJoin{sym::trickWordFormat}; // "%s%s"
        constexpr ee::Ptr<const char> kNoBreak{sym::g_textEmpty};
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};

        // `text` added to `out` as a line of its own (after a break unless
        // it is the first of `lines`).
        void addLine(ee::Ptr<char> out, int32_t lines, ee::Ptr<const char> text)
        {
            const int32_t length = zge::stringLength(out);
            zge::formatString(out + length, kLineJoin, lines > 0 ? kLineBreak : kNoBreak, text);
        }

        // A record's value written after `text` as the multiplayer mode
        // scores: a distance in feet and inches, tricks, degrees (half
        // turns), or points.
        void addRecordValue(ee::Ptr<char> text, ee::Float value)
        {
            switch (multiplayerMode())
            {
            case 6: case 7: case 8: case 11: case 12: case 13: // scored as a distance
                formatFeetAndInches(text + zge::stringLength(text), value);
                break;
            case 9: // scored in tricks
            {
                const ee::Ptr<char> end = text + zge::stringLength(text);
                zge::formatString(end, kTricksFormat, sce::libgcc::fptoui(value));
                break;
            }
            case 10: // scored in half turns
            {
                const ee::Ptr<char> end = text + zge::stringLength(text);
                zge::formatString(end, kDegreesFormat, sce::libgcc::fptoui(value) * kDegreesPerHalfTurn);
                break;
            }
            default:
            {
                const ee::Ptr<char> end = text + zge::stringLength(text);
                zge::formatString(end, kPointsFormat, sce::libgcc::fptoui(value));
                break;
            }
            }
        }
    }

    // showMultiplayerReadyScreen: as a turn is about to start, the current
    // player's pad answers and the screen shows the round or attempt and
    // whose turn it is. Its message: in HORSE, at the round's first turn its
    // instructions, else the line to match, trick by trick; else the
    // player's score (random spawns), the score to beat (the round's best
    // turn in a series, else the game's record of the mode's kind; not at a
    // first turn of a series or of the game's first round), the all-time
    // best (the saved record) and a series' standings after its first
    // round. Its menu: go, in HORSE the replay of the line to match (but at
    // the first turn) and the trick list, and the exit.
    void showMultiplayerReadyScreen(uint32_t shown)
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[0x400]> line;
        ee::Local<char[0x400]> message;
        ee::Local<int32_t> menuLines;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kReadyFrame);
        if (!shown)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        *g_uiModalPlayer = *g_currentPlayer;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(*g_currentPlayer);

        ee::Ptr<const char> title = text;
        switch (multiplayerMode())
        {
        case kSeriesMode:
        case kRandomSpawnMode:
            zge::formatString(text, kRoundTitle, roundNumber() + 1);
            break;
        case 1:
        case kHorseMode:
            title = kReadyTitle;
            break;
        default:
        {
            const uint32_t round = roundNumber();
            if (round < static_cast<uint16_t>(roundCount() - 1))
                zge::formatString(text, kAttemptTitle, roundNumber());
            else
                zge::formatString(text, kLastAttemptTitle);
            break;
        }
        }
        zge::zgui::setVariable(uiWidget(kTopTitle), kTextName, title);
        zge::formatString(text, kPlayerTitle, *g_currentPlayer);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, text);

        int32_t lines = 0;
        message[0] = 0;
        const uint32_t mode = multiplayerMode();
        if (mode == kHorseMode)
        {
            if (roundFlags(kFirstTurnFlag))
                addLine(message, lines, kHorseInstructions);
            else
            {
                // The line to match: each trick in its colour (unless its
                // text has one), broken into lines of 30.
                zge::formatString(text, kTrickListTitle);
                addLine(message, lines++, text);
                ee::Ptr<const rider::ScoreLine> kept = &(*g_keptLines)[0];
                for (int32_t i = 0; i < *g_keptLineCount; ++i, ++kept)
                {
                    dm2TrickText(text, kept, 0);
                    if (text[0] == '#')
                        line[0] = 0;
                    else
                        zge::formatString(line, kTrickColour);
                    breakTextLines(ee::Ptr<char>(line) + zge::stringLength(line), text, kTrickLineChars);
                    addLine(message, lines++, line);
                    if (i < *g_keptLineCount - 1)
                        addLine(message, lines++, text);
                }
            }
        }
        else
        {
            if (mode == kRandomSpawnMode)
            {
                zge::formatString(text, kScoreText, rider->score.gameScore);
                addLine(message, lines++, text);
            }
            if (!roundFlags(kFirstTurnFlag) || (multiplayerMode() != kSeriesMode && roundNumber() != 0))
            {
                uint32_t key;
                if (multiplayerMode() == kSeriesMode)
                {
                    const uint32_t series = multiplayerMode();
                    key = recordKey(series, singlePlayerMode(), kTurnScoreRecord, kRoundRecords);
                }
                else
                {
                    const uint32_t keyMode = multiplayerMode();
                    const uint32_t kind = (*g_modeRecordKinds)[multiplayerMode()];
                    key = recordKey(keyMode, singlePlayerMode(), kind, kGameRecords);
                }
                const ee::Ptr<GameRecord> toBeat = gameRecord(key);
                zge::formatString(text, kScoreToBeat);
                addRecordValue(text, toBeat->value);
                addLine(message, lines++, text);
            }
            const uint32_t bestMode = multiplayerMode();
            const uint32_t single = singlePlayerMode();
            const ee::Ptr<GameRecord> best = gameRecord(recordKey(bestMode, single, (*g_modeRecordKinds)[multiplayerMode()], kSavedRecords));
            zge::formatString(text, kAllTimeBest);
            addRecordValue(text, best->value);
            addLine(message, lines++, text);
            if (multiplayerMode() == kSeriesMode && roundNumber() != 0)
            {
                seriesStandingText(text);
                addLine(message, lines, text);
            }
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, 0), kTextName, message);

        *menuLines = 0;
        text[0] = 0;
        addLine(text, *menuLines, kGoLine);
        ++*menuLines;
        if (multiplayerMode() == kHorseMode)
        {
            if (!roundFlags(kFirstTurnFlag))
            {
                addLine(text, *menuLines, kHorseReplayLine);
                ++*menuLines;
            }
            addLine(text, *menuLines, kTrickListLine);
            ++*menuLines;
        }
        addLine(text, *menuLines, kExitLine);
        ++*menuLines;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kItemsName, text);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kVisibleCountName, menuLines);
        playWidgetAction(frame, kInitAction);
    }
    READABLE_FUNCTION(showMultiplayerReadyScreen, Verified);

    // bmxoverlayMPReadyHandler: the menu's line taken: the turn starts; in
    // HORSE the replay of the line to match (from the current player's view)
    // or, at the round's first turn, the trick list; or the exit
    // confirmation (which comes back to screen 25).
    void bmxoverlayMPReadyHandler()
    {
        ee::Local<int32_t> picked;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kReadyFrame);
        uint32_t horseLine = 0;
        if (multiplayerMode() == kHorseMode)
            horseLine = (roundFlags(kFirstTurnFlag) ? 0u : kHorseLineReplay) | kHorseLineShown;
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 0), kSelectedIndexName, picked);
        switch (*picked)
        {
        case kReadyGo:
            showGameScreen(kScreenNone);
            setRoundPhase(kTurnRunningPhase);
            return;
        case kReadyHorseLine:
            if (horseLine == 0)
                break; // (the exit)
            if (horseLine & kHorseLineReplay)
            {
                showGameScreen(kScreenNone);
                actionStart(kActionRoundReplay);
                *g_actionRequest = static_cast<int8_t>(*g_currentPlayer);
            }
            else if (horseLine & kHorseLineShown)
            {
                showGameScreen(kScreenReadyTrickList);
            }
            return;
        case kReadyExit:
        case kReadyHorseExit:
            break;
        default:
            return;
        }
        *g_screenBehindExitConfirm = kScreenMultiplayerReady;
        showGameScreen(kScreenExitConfirm);
    }
    READABLE_FUNCTION(bmxoverlayMPReadyHandler, Verified);
}
