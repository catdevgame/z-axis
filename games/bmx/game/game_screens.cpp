// game.c's own game screens (game_screens.h). BMX XXX's own code (game.c).

#include "game_screens.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"
#include "standings.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/ui/strings.h"
#include "bmx/ui/trick_text.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/game/game_screens.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        constexpr uint32_t kFrameMPGameSelect = sym::g_textFrameMPGameSelect;     // "FrameMPGameSelect"
        constexpr uint32_t kModeMenu = sym::g_textFrameMenuMenu;                  // "FrameMenu.Menu"
        constexpr uint32_t kDescription = sym::g_textFrameDescriptionText;        // "FrameDescription.Description"
        // The menu's lines: the modes (their names and descriptions by
        // number), then the exit; the exit asks for the exit confirmation
        // over this screen.
        constexpr uint32_t kMpGameModes = 4;
        inline ee::Global<uint32_t[kMpGameModes], sym::g_mpGameSelectModes> g_mpGameSelectModes; // kSkillzMode, kPaintballMode, kStripMode, (0: the exit's)
        constexpr uint32_t kExitLine = 3;
        constexpr uint32_t kModeName = sym::g_textModeNameKey;                    // "$KSTRMP_MODENAME_MODE%02d"
        constexpr uint32_t kExitText = sym::g_textGameSelectExit;                 // "$KSTR_GAMEOVER_EXIT" (its own copy)
        constexpr uint32_t kModeDescription = sym::g_textModeDescriptionKey;      // "$KSTRMP_DESC_MODE%02d"
        constexpr int32_t kNothingSelected = -1;
        // The menus' pad: every player's.
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
        constexpr int32_t kEveryPlayer = -1;
    }

    // listMpGameModes: the lines one text, a line break before each but the
    // first, as the menu's "Items".
    void listMpGameModes(ee::Ptr<Node> menu)
    {
        ee::Local<char[0x200]> items;
        ee::Local<char[0x40]> line;
        items[0] = 0;
        for (uint32_t index = 0; index < kMpGameModes; ++index)
        {
            if (index == kExitLine)
                zge::formatString(line, kExitText);
            else
                zge::formatString(line, kModeName, (*g_mpGameSelectModes)[index]);
            zge::formatString(items.address() + static_cast<uint32_t>(zge::stringLength(items)), kSeparatorAndLine,
                              ee::Ptr<const char>(index != 0 ? kLineBreak : kNoSeparator), line);
        }
        zge::zgui::setVariable(menu, kItemsName, items);
    }
    READABLE_FUNCTION(listMpGameModes, Verified);

    // mpGameSelectScreenMessage: a mode chosen starts that game (B-M-X from
    // the screen before its first turn, the others from the game's start);
    // the exit asks for the exit confirmation, which comes back here.
    void mpGameSelectScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> description;
        ee::Local<int32_t> selected;
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameMPGameSelect, kFrameNode);
        if (!frame)
            return;
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kModeMenu, kFrameNode), kSelectedIndexName, selected);
        switch (message)
        {
        case kScreenEnter:
            *g_modalPlayerNum = kEveryPlayer;
            listMpGameModes(zge::zgui::findNode(frame, kModeMenu, kFrameNode));
            playUiNode(frame, kInitAction);
            return;
        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;
        case kScreenChosen:
            if (*selected == static_cast<int32_t>(kExitLine))
            {
                *g_screenBehindExitConfirm = kScreenAfterGame;
                showGameScreen(kScreenExitLevel);
                *g_gameScreenChanged = 0;
                return;
            }
            startGame((*g_mpGameSelectModes)[*selected]);
            if (gameMode() == kHorseMode)
                showGameScreen(kScreenBeforeGame);
            else
                setRoundPhase(kGameStartPhase);
            return;
        case kScreenSelectionChanged:
            if (*selected == kNothingSelected)
                return;
            zge::formatString(description, kModeDescription, (*g_mpGameSelectModes)[*selected]);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kDescription, kFrameNode), kTextName, description);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION(mpGameSelectScreenMessage, Verified);

    namespace
    {
        // The keyboard frame, its box, and the header over the screens.
        constexpr uint32_t kKeyboardFrame = sym::g_textOverlayKeyboardFrame; // "KeyboardFrame"
        constexpr uint32_t kKeyboard = sym::g_textKeyboard;             // "Keyboard"
        constexpr uint32_t kEditBox = sym::g_textKeyboardEditBox;       // "Keyboard.EditBox"
        constexpr uint32_t kTitle = sym::g_textTitle;                   // "Title"
        constexpr uint32_t kHeaderTitle = sym::g_textTopHeaderTitle;    // "UI_common.TopHeader.Title"
        constexpr uint32_t kStringName = sym::g_textString;             // "String"
        constexpr uint32_t kMaxItemsName = sym::g_textMaxItems;         // "MaxItems"
        constexpr uint32_t kOnOkName = sym::g_textOnOk;                 // "OnOk"
        constexpr uint32_t kOnCancelName = sym::g_textOnCancel;         // "OnCancel"
        constexpr uint32_t kSelectWord = sym::g_textSelectHorseWord;    // "$KSTRMP_MPHORSEWORD_SELECTWORD"
        // The box's answers: the screen's own messages, through the script
        // function bmxoverlayMPHorseWordHandler.
        constexpr uint32_t kOnOkScript = sym::g_textHorseWordOnOk;         // "SET Root.MsgType {0}\nCALL bmxoverlayMPHorseWordHandler"
        constexpr uint32_t kOnCancelScript = sym::g_textHorseWordOnCancel; // "SET Root.MsgType {-1}\nCALL bmxoverlayMPHorseWordHandler"
        // The word as it is typed (setHorseWord takes it).
        inline ee::Global<char[16], sym::g_typedHorseWord> g_typedHorseWord;
        constexpr uint32_t kWordLetters = 8;
        constexpr uint32_t kEmpty = sym::g_textNoSeparator;               // ""
    }

    // horseWordScreenMessage: the header shows B-M-X's name; the box takes
    // up to 8 letters and starts with the word typed last.
    void horseWordScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> header;
        ee::Local<uint32_t> letters;
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kKeyboardFrame, kFrameNode);
        if (!frame)
            return;
        const auto keyboard = [&]() { return zge::zgui::findNode(frame, kKeyboard, kFrameNode); };
        const auto editBox = [&]() { return zge::zgui::findNode(frame, kEditBox, kFrameNode); };
        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(keyboard(), kStringName, sym::g_typedHorseWord);
            if (zge::stringLength(sym::g_typedHorseWord) == 0)
            {
                soundEvent(kSoundMenuBlocked, 0, 0, 0, 0);
                return;
            }
            showGameScreen(kScreenPlay);
            *g_resumeAction = kResumeTurnStart;
            setHorseWord(ee::Ptr<const char>(sym::g_typedHorseWord));
            return;
        case kScreenCancelled:
            showGameScreen(kScreenAfterGame);
            return;
        case kScreenEnter:
            *g_modalPlayerNum = kEveryPlayer;
            zge::formatString(header, kModeName, kHorseMode);
            zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kHeaderTitle, kFrameNode), kTextName, header);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitle, kFrameNode), kTextName, kSelectWord);
            *letters = kWordLetters;
            zge::zgui::setVariable(editBox(), kMaxItemsName, letters);
            zge::zgui::setVariable(keyboard(), kMaxItemsName, letters);
            zge::zgui::setVariable(keyboard(), kOnOkName, kOnOkScript);
            zge::zgui::setVariable(keyboard(), kOnCancelName, kOnCancelScript);
            playUiNode(frame, kInitAction);
            zge::zgui::setVariable(keyboard(), kStringName, sym::g_typedHorseWord);
            zge::zgui::setVariable(editBox(), kTextName, sym::g_typedHorseWord);
            return;
        case kScreenLeaveNow:
            zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kHeaderTitle, kFrameNode), kTextName, kEmpty);
            playUiNode(frame, kShutdownAction);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION(horseWordScreenMessage, Draft);

    namespace
    {
        using rider::ScoreBlock;
        using rider::ScoreLine;

        constexpr uint32_t kFrameMPReady = sym::g_textFrameMPReady;               // "FrameMPReady"
        constexpr uint32_t kMessageName = sym::g_textMessage;                     // "Message"
        // The header: the round, the attempt, or READY! by the mode.
        constexpr uint32_t kRoundTitle = sym::g_textMpReadyRound;                 // "$KSTR_ROUND %ld"
        constexpr uint32_t kAttemptTitle = sym::g_textMpReadyAttempt;             // "$KSTR_ORDINAL_%d $KSTR_ATTEMPT"
        constexpr uint32_t kLastAttemptTitle = sym::g_textMpReadyLastAttempt;     // "$KSTR_ORDINAL_LAST $KSTR_ATTEMPT"
        constexpr uint32_t kReadyTitle = sym::g_textMpReadyReady;                 // "$KSTRMP_MPREADY_READY": "READY!"
        constexpr uint32_t kPlayerTitle = sym::g_textMpReadyPlayer;               // "$KSTR_PLAYER $KSTR_NUMBER_%d"
        // The message's lines.
        constexpr uint32_t kScoreLine = sym::g_textMpReadyScore;                  // "$KSTR_SCORE %6ld"
        constexpr uint32_t kScoreToBeat = sym::g_textMpReadyScoreToBeat;          // "$KSTR_SCORETOBEAT "
        constexpr uint32_t kAllTimeBest = sym::g_textMpReadyAllTimeBest;          // "$KSTR_ALLTIMEBEST "
        constexpr uint32_t kHorseInstructions = sym::g_textMpReadyHorseInstructions; // "$KSTRMP_MPREADY_INST_HORSE": "BUST OUT A TRICK COMBO THAT ..."
        constexpr uint32_t kTricksPerformed = sym::g_textMpReadyTricksPerformed;  // "$KSTRMP_MPREADY_TRICKLIST": "TRICKS PERFORMED:"
        constexpr uint32_t kTrickLineColour = sym::g_textMpReadyTrickLineColour;  // "#RGB{238 181 44}"
        constexpr uint32_t kTrickLineWidth = 30;
        // A record's value, by what the mode scores.
        constexpr uint32_t kRecordPoints = sym::g_textMpReadyRecordPoints;        // "%6ld"
        constexpr uint32_t kRecordTricks = sym::g_textMpReadyRecordTricks;        // "%2ld tricks"
        constexpr uint32_t kRecordDegrees = sym::g_textMpReadyRecordDegrees;      // "%4ld degrees"
        constexpr uint8_t kFirstDistanceMode = 9, kLastDistanceMode = 16, kTricksMode = 12, kDegreesMode = 13;
        constexpr uint32_t kDegreesAHalfTurn = 180;
        // Each mode's own kind of record (RecordKind), by mode (the 20 of g_gameModes).
        inline ee::Global<uint8_t[20], sym::g_modeRecordSlots> g_modeRecordSlots;
        // The menu's lines.
        constexpr uint32_t kContinueLine = sym::g_textMpReadyContinue;            // "$KSTR_OPTIONS_0": "CONTINUE"
        constexpr uint32_t kTrickListLine = sym::g_textMpReadyTrickList;          // "$KSTR_ROUNDOVER_TRICKLIST": "TRICK LIST"
        constexpr uint32_t kExitMenuLine = sym::g_textMpReadyExit;                   // "$KSTR_OPTIONS_2": "EXIT"
        enum MpReadyLine : int32_t
        {
            kReadyContinue = 0,
            kReadySecondLine = 1, // B-M-X's TRICK LIST, else EXIT
            kReadyThirdLine = 2,  // B-M-X's EXIT
            kReadyFourthLine = 3, // (no such line)
        };

        // B-M-X's line to match (score_end.cpp: the first player's first
        // line, kept as the turn ends).
        inline ee::Global<ScoreLine[1], sym::g_keptLines> g_keptLines;
        inline ee::Global<uint8_t, sym::g_keptLineCount> g_keptLineCount;
        // The player whose pad runs the replay shown.
        inline ee::Global<uint8_t, sym::g_replayPadPlayer> g_replayPadPlayer;

        ee::Ptr<char> endOf(ee::Ptr<char> text) { return text + zge::stringLength(text); }

        // A line added to a text of lines: a line break before it unless
        // it is the first.
        void appendLine(ee::Ptr<char> text, uint32_t &lines, ee::Address line)
        {
            const ee::Ptr<char> end = endOf(text);
            zge::formatString(end, kSeparatorAndLine, ee::Ptr<const char>(lines != 0 ? kLineBreak : kNoSeparator), line);
            ++lines;
        }

        // A record's value after the text: a distance in feet and inches, a
        // count of tricks, a spin in degrees, else points.
        void appendRecordValue(ee::Ptr<char> text, const GameRecord &record)
        {
            const uint8_t mode = gameMode();
            const ee::Ptr<char> end = endOf(text);
            if (mode == kTricksMode)
                zge::formatString(end, kRecordTricks, sce::libgcc::fptoui(record.value));
            else if (mode == kDegreesMode)
                zge::formatString(end, kRecordDegrees, sce::libgcc::fptoui(record.value) * kDegreesAHalfTurn);
            else if (mode >= kFirstDistanceMode && mode <= kLastDistanceMode)
                formatFeetInches(end, record.value);
            else
                zge::formatString(end, kRecordPoints, sce::libgcc::fptoui(record.value));
        }
    }

    // mpReadyScreenMessage: the frame between a multiplayer game's turns.
    // It comes in with the next player's number under a header (the round,
    // the attempt, or READY!), the score to beat and the all-time best (in
    // B-M-X the instructions, or the line to match), and a menu: continue,
    // B-M-X's TRICK LIST, exit.
    void mpReadyScreenMessage(int32_t message)
    {
        ee::Local<char[0x100]> line;
        ee::Local<char[0x400]> wrapped;
        ee::Local<char[0x400]> text;
        ee::Local<uint32_t> menuLines;
        ee::Local<int32_t> selected;
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameMPReady, kFrameNode);
        if (!frame)
            return;
        switch (message)
        {
        case kScreenChosen:
        {
            const bool laterHorseTurn = gameMode() == kHorseMode && !roundFlags(kFirstTurnFlag);
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kSelectedIndexName, selected);
            switch (*selected)
            {
            case kReadyContinue:
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeTurnRunning;
                return;
            case kReadySecondLine:
                // After B-M-X's first turn the second line shows the replay
                // (DM2's menu had VIEW REPLAY there; BMX XXX's says TRICK
                // LIST: kept); in B-M-X's first turn it is the exit, as
                // in the other modes.
                if (laterHorseTurn)
                {
                    showGameScreen(kScreenReplay);
                    *g_replayPadPlayer = *g_currentPlayer;
                    return;
                }
                [[fallthrough]];
            case kReadyThirdLine:
            case kReadyFourthLine:
                *g_screenBehindExitConfirm = kScreenNextPlayer;
                showGameScreen(kScreenExitGame);
                return;
            default:
                return;
            }
        }
        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;
        case kScreenEnter:
            break;
        default:
            return;
        }

        const ee::Ptr<rider::Rider> rider = rider::riderOf(*g_currentPlayer);
        ScoreBlock &score = rider->score;

        // The header.
        ee::Address title = line;
        const uint8_t mode = gameMode();
        if (mode == kHighFiveMode || mode == kRandomSpawnMode)
        {
            zge::formatString(line, kRoundTitle, roundNumber() + 1);
        }
        else if (mode == kSkillzMode || mode == kHorseMode)
        {
            title = kReadyTitle;
        }
        else
        {
            const uint8_t round = roundNumber();
            if (round < static_cast<uint16_t>(roundCount() - 1))
                zge::formatString(line, kAttemptTitle, roundNumber());
            else
                zge::formatString(line, kLastAttemptTitle);
        }
        zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kHeaderTitle, kFrameNode), kTextName, title);
        zge::formatString(line, kPlayerTitle, *g_currentPlayer);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kTitle, kFrameNode), kTextName, line);

        // The message.
        uint32_t lines = 0;
        text[0] = 0;
        const uint8_t played = gameMode();
        if (played == kHorseMode)
        {
            const bool firstTurn = roundFlags(kFirstTurnFlag);
            if (firstTurn)
            {
                appendLine(text, lines, kHorseInstructions);
            }
            else
            {
                // The line to match, each in orange unless its text starts
                // with a colour of its own, broken into lines.
                zge::formatString(line, kTricksPerformed);
                appendLine(text, lines, line);
                for (int32_t index = 0; index < *g_keptLineCount; ++index)
                {
                    trickText(line, &(*g_keptLines)[index], 0);
                    if (line[0] != '#')
                        zge::formatString(wrapped, kTrickLineColour);
                    else
                        wrapped[0] = 0;
                    wrapText(endOf(wrapped), line, kTrickLineWidth);
                    appendLine(text, lines, wrapped);
                    // (Between two kept lines the first is added again
                    // unwrapped; one line is kept at most.)
                    if (index < *g_keptLineCount - 1)
                        appendLine(text, lines, line);
                }
            }
        }
        else
        {
            if (played == kRandomSpawnMode)
            {
                zge::formatString(line, kScoreLine, score.gameScore);
                appendLine(text, lines, line);
            }
            // The score to beat: the game's record of the mode's kind (in
            // HIGH FIVE the round's best run), not in the first player's
            // turn of HIGH FIVE or of the game's first round.
            if (!roundFlags(kFirstTurnFlag) || (gameMode() != kHighFiveMode && roundNumber() != 0))
            {
                uint32_t key;
                if (gameMode() != kHighFiveMode)
                {
                    const uint8_t mode = gameMode();
                    key = recordKey(mode, (*g_modeRecordSlots)[gameMode()], kGameRecords);
                }
                else
                {
                    key = recordKey(gameMode(), kRecordRunScore, kRoundRecords);
                }
                const ee::Ptr<GameRecord> toBeat = gameRecord(key);
                zge::formatString(line, kScoreToBeat);
                appendRecordValue(line, *toBeat);
                appendLine(text, lines, line);
            }
            // The level's saved record of the mode's kind.
            const uint8_t mode = gameMode();
            const ee::Ptr<GameRecord> best = gameRecord(recordKey(mode, (*g_modeRecordSlots)[gameMode()], kSavedRecords));
            zge::formatString(line, kAllTimeBest);
            appendRecordValue(line, *best);
            appendLine(text, lines, line);
            // HIGH FIVE after its first round: the series so far.
            if (gameMode() == kHighFiveMode && roundNumber() != 0)
            {
                seriesStandingText(line);
                appendLine(text, lines, line);
            }
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, kFrameNode), kTextName, text);

        // The menu.
        *menuLines = 0;
        line[0] = 0;
        appendLine(line, *menuLines, kContinueLine);
        if (gameMode() == kHorseMode)
            appendLine(line, *menuLines, kTrickListLine);
        appendLine(line, *menuLines, kExitMenuLine);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kItemsName, line);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kVisibleCountName, menuLines);
        playUiNode(frame, kInitAction);
    }
    READABLE_FUNCTION(mpReadyScreenMessage, Verified);

    namespace
    {
        constexpr uint32_t kFrameMPTimeUp = sym::g_textFrameMPTimeUp;             // "FrameMPTimeUp"
        // The title: the player, the round's result or TIME UP.
        constexpr uint32_t kTieTitle = sym::g_textTie;                            // "$KSTR_TIE"
        constexpr uint32_t kWinnerTitle = sym::g_textMpTimeUpWinner;              // "$KSTRMP_MPTIMEUP_WINNER": "WINNER!"
        constexpr uint32_t kTimeUpTitle = sym::g_textMpTimeUpTimeUp;              // "$KSTRMP_MPTIMEUP_TIMEUP": "TIME UP"
        // The message's lines.
        constexpr uint32_t kTurnScore = sym::g_textMpReadyScore;                  // "$KSTR_SCORE %6ld"
        constexpr uint32_t kSeriesTurnScore = sym::g_textMpTimeUpSeriesScore;     // "$KSTR_SCORE %ld"
        constexpr uint32_t kTotalScore = sym::g_textMpTimeUpTotalScore;           // "$KSTR_TOTALSCORE %6ld"
        constexpr uint32_t kBestScore = sym::g_textMpTimeUpAllTimeBest;           // "$KSTR_ALLTIMEBEST %6ld"
        constexpr uint32_t kReplayRound = sym::g_textMpTimeUpReplayRound;         // "$KSTRMP_MPTIMEUP_REPLAYROUND": "REPLAY THE ROUND"
        constexpr uint32_t kPlayerScoreName = sym::g_textMpTimeUpPlayer;          // "$KSTR_PLAYER $KSTR_NUMBER_%d : "
        constexpr uint32_t kWinnerScore = sym::g_textMpTimeUpWinnerScore;         // "#RGB{238 181 44}%6ld#RGB{192 192 192}"
        constexpr uint32_t kPlayerScore = sym::g_textMpReadyRecordPoints;         // "%6ld"
        // roundWinner's rounds: the round's own winner.
        constexpr uint32_t kRoundsOfOne = 2;
        enum MpTimeUpLine : int32_t
        {
            kTimeUpContinue = 0,
            kTimeUpExit = 1,
        };
    }

    // mpTimeUpScreenMessage: the frame after a multiplayer turn. The turn's
    // score and the all-time best (RANDOM SPOT also the game's total and its
    // best); in HIGH FIVE, once every player has had the round, each
    // player's score with the winner's in orange (a drawn round is played
    // again), then the series; in B-M-X, at the round's end, the letters.
    // Continuing starts the next turn, or ends the game after its last
    // round.
    void mpTimeUpScreenMessage(int32_t message)
    {
        ee::Local<char[0x100]> line;
        ee::Local<char[0x400]> text;
        ee::Local<int32_t> selected;
        ee::Local<uint32_t> menuLines;
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameMPTimeUp, kFrameNode);
        if (!frame)
            return;
        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kSelectedIndexName, selected);
            if (*selected == kTimeUpContinue)
            {
                if (roundFlags(kGameEndsFlag))
                {
                    setRoundPhase(kGameOverPhase);
                    return;
                }
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeTurnStart;
            }
            else if (*selected == kTimeUpExit)
            {
                *g_screenBehindExitConfirm = kScreenAfterTurn;
                showGameScreen(kScreenExitGame);
            }
            return;
        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;
        case kScreenEnter:
            break;
        default:
            return;
        }

        const ee::Ptr<rider::Rider> rider = rider::riderOf(*g_currentPlayer);
        uint32_t lines = 0;
        text[0] = 0;
        ScoreBlock &score = rider->score;
        const auto setTitle = [&](ee::Address title) {
            zge::zgui::setVariable(zge::zgui::findNode(frame, kTitle, kFrameNode), kTextName, title);
        };
        const auto playerTitle = [&]() {
            zge::formatString(line, kPlayerTitle, *g_currentPlayer);
            setTitle(line);
        };
        switch (gameMode())
        {
        case kSkillzMode:
        {
            playerTitle();
            const ee::Ptr<GameRecord> best = gameRecord(recordKey(gameMode(), kRecordGameScore, kSavedRecords));
            zge::formatString(line, kTurnScore, sce::libgcc::fptoui(score.turnScore));
            appendLine(text, lines, line);
            zge::formatString(line, kBestScore, sce::libgcc::fptoui(best->value));
            appendLine(text, lines, line);
            break;
        }
        case kHighFiveMode:
        {
            const ee::Ptr<GameRecord> best = gameRecord(recordKey(gameMode(), kRecordRunScore, kSavedRecords));
            if (!roundFlags(kRoundDoneFlag))
            {
                playerTitle();
                zge::formatString(line, kSeriesTurnScore, sce::libgcc::fptoui(score.turnScore));
                appendLine(text, lines, line);
                zge::formatString(line, kBestScore, sce::libgcc::fptoui(best->value));
                appendLine(text, lines, line);
            }
            else if (roundFlags(kRoundDrawnFlag))
            {
                setTitle(kTieTitle);
                appendLine(text, lines, kReplayRound);
            }
            else
            {
                setTitle(kWinnerTitle);
                for (uint32_t player = 0; player < *g_playerCount; ++player)
                {
                    zge::formatString(line, kPlayerScoreName, player);
                    const bool won = player == roundWinner(roundNumber(), kRoundsOfOne);
                    const ee::Ptr<char> end = endOf(line);
                    const uint32_t points = sce::libgcc::fptoui(playerRider(player)->score.turnScore);
                    if (won)
                        zge::formatString(end, kWinnerScore, points);
                    else
                        zge::formatString(end, kPlayerScore, points);
                    appendLine(text, lines, line);
                }
                zge::formatString(line, kBestScore, sce::libgcc::fptoui(best->value));
                appendLine(text, lines, line);
            }
            // The series so far, once the round is done or after the first.
            if (roundFlags(kRoundDoneFlag) || roundNumber() != 0)
            {
                seriesStandingText(line);
                appendLine(text, lines, line);
            }
            break;
        }
        case kHorseMode:
            setTitle(kTimeUpTitle);
            if (roundFlags(kRoundDoneFlag))
            {
                wordGameStandingText(line);
                appendLine(text, lines, line);
            }
            break;
        case kRandomSpawnMode:
        {
            playerTitle();
            const ee::Ptr<GameRecord> bestTurn = gameRecord(recordKey(gameMode(), kRecordRunScore, kSavedRecords));
            zge::formatString(line, kTurnScore, sce::libgcc::fptoui(score.turnScore));
            appendLine(text, lines, line);
            zge::formatString(line, kBestScore, sce::libgcc::fptoui(bestTurn->value));
            appendLine(text, lines, line);
            const ee::Ptr<GameRecord> bestGame = gameRecord(recordKey(gameMode(), kRecordGameScore, kSavedRecords));
            zge::formatString(line, kTotalScore, score.gameScore);
            appendLine(text, lines, line);
            zge::formatString(line, kBestScore, sce::libgcc::fptoui(bestGame->value));
            appendLine(text, lines, line);
            break;
        }
        default:
            break;
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, kFrameNode), kTextName, text);

        // The menu.
        *menuLines = 0;
        line[0] = 0;
        appendLine(line, *menuLines, kContinueLine);
        appendLine(line, *menuLines, kExitMenuLine);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kItemsName, line);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kVisibleCountName, menuLines);
        playUiNode(frame, kInitAction);
    }
    READABLE_FUNCTION(mpTimeUpScreenMessage, Verified);
}
