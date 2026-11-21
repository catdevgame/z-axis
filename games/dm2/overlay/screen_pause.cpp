// DM2's pause screen (screens.h; "InGameUI.FramePause"): its menu built for
// the game being played, and the menu's choice taken. DM2's own code
// (overlay.c).

#include "screens.h"

#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_mode.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_pause.cpp.h"

namespace game::dm2
{
    namespace
    {
        // What each line of the pause menu does, in the menu's order.
        enum PauseChoice : uint8_t
        {
            kPauseContinue = 0,
            kPauseGoals = 1,
            kPauseOptions = 2,
            kPauseTrickList = 3,
            kPauseRecords = 4,
            kPauseExitGame = 5,
        };
        constexpr uint32_t kPauseChoices = 6;
        inline ee::Global<uint8_t[kPauseChoices], sym::g_pauseChoices> g_pauseChoices;

        constexpr ee::Ptr<const char> kFramePause{sym::g_textFramePause};         // "InGameUI.FramePause"
        constexpr ee::Ptr<const char> kFramePauseMenu{sym::g_textFramePauseMenu}; // "InGameUI.FramePause.Menu"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};               // "Items"
        constexpr ee::Ptr<const char> kVisibleCountName{sym::g_textVisibleCount}; // "VisibleCount"
        // The menu's lines: "$KSTR_PAUSEMENU_CONTINUE_TEXT", and
        // "%s$KSTR_PAUSEMENU_GOALS_TEXT" and the others, each after a
        // separator.
        constexpr ee::Ptr<const char> kContinueText{sym::g_textPauseContinue};
        constexpr ee::Ptr<const char> kGoalsText{sym::g_textPauseGoals};
        constexpr ee::Ptr<const char> kOptionsText{sym::g_textPauseOptions};
        constexpr ee::Ptr<const char> kTrickListText{sym::g_textPauseTrickList};
        constexpr ee::Ptr<const char> kRecordsText{sym::g_textPauseRecords};
        constexpr ee::Ptr<const char> kExitGameText{sym::g_textPauseExitGame};
        constexpr ee::Ptr<const char> kNoSeparator{sym::g_textEmpty};  // ""
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};  // a line feed

        // A line added to the menu's text, after a line break unless it is
        // the menu's first.
        void appendMenuLine(ee::Ptr<char> text, ee::Ptr<const char> line, uint32_t linesBefore)
        {
            const ee::Ptr<char> end = text + static_cast<int32_t>(zge::stringLength(text));
            zge::formatString(end, line, linesBefore == 0 ? kNoSeparator : kLineBreak);
        }

        // Scaffolding (value-named): a pad button the handler tests for the
        // current player, whose answer it does not use (rider.h names the
        // directions only).
        constexpr uint32_t kTestedButton = 0x80;
    }

    // showPauseScreen: as the screen opens, the menu's lines for this game:
    // continue; the goals in a ProQuest level; then options and exit in a
    // park test, or options, the trick list, the records and exit in any
    // other game. As it closes, the frame's closing action.
    void showPauseScreen(uint32_t shown)
    {
        ee::Local<char[0x200]> text;
        ee::Local<uint32_t> lines;

        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kFramePause);
        if (!frame)
            return;
        if (shown == 0)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }

        const ee::Ptr<char> menuText{text};
        lines = 0;
        (*g_pauseChoices)[0] = kPauseContinue;
        zge::formatString(menuText, kContinueText);
        lines = lines + 1;

        if (multiplayerMode() == 0 && singlePlayerMode() == kLevelGoals)
        {
            (*g_pauseChoices)[lines] = kPauseGoals;
            appendMenuLine(menuText, kGoalsText, lines);
            lines = lines + 1;
        }

        if (multiplayerMode() == 0 && singlePlayerMode() == kParkTest)
        {
            (*g_pauseChoices)[lines] = kPauseOptions;
            appendMenuLine(menuText, kOptionsText, lines);
            lines = lines + 1;
            (*g_pauseChoices)[lines] = kPauseExitGame;
            appendMenuLine(menuText, kExitGameText, lines);
            lines = lines + 1;
        }
        else
        {
            for (uint8_t choice = kPauseOptions; choice <= kPauseExitGame; ++choice)
            {
                (*g_pauseChoices)[lines] = choice;
                lines = lines + 1;
            }
            appendMenuLine(menuText, kOptionsText, lines);
            appendMenuLine(menuText, kTrickListText, lines);
            appendMenuLine(menuText, kRecordsText, lines);
            appendMenuLine(menuText, kExitGameText, lines);
        }

        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kItemsName, menuText);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kVisibleCountName, lines);
        playWidgetAction(frame, kInitAction);
    }
    READABLE_FUNCTION(showPauseScreen, Verified);

    // bmxoverlayPauseHandler: the menu's selected line done: play goes on,
    // or the line's screen opens (the exit confirmation over this one).
    void bmxoverlayPauseHandler()
    {
        ee::Local<uint32_t> selected;
        rider::padButtons(*g_currentPlayer, kTestedButton, rider::kAnyPressed);
        zge::zgui::copyVariable(uiWidget(kFramePauseMenu), kSelectedIndexName, selected);
        switch ((*g_pauseChoices)[selected])
        {
        case kPauseGoals:
            showGameScreen(kScreenPauseObjectives);
            break;
        case kPauseOptions:
            showGameScreen(kScreenOptions);
            break;
        case kPauseTrickList:
            showGameScreen(kScreenTrickList);
            break;
        case kPauseRecords:
            showGameScreen(kScreenRecords);
            break;
        case kPauseExitGame:
            *g_screenBehindExitConfirm = kScreenPause;
            showGameScreen(kScreenExitConfirm);
            break;
        default: // continue (and a line with no meaning)
            showGameScreen(kScreenNone);
            setGameMode(kSoundModeLevel);
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayPauseHandler, Verified);
}
