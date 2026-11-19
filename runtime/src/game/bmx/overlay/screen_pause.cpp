// BMX XXX's pause menu (screens.h; "FramePause"): its lines for the game
// being played, and the chosen line done. BMX XXX's own code
// (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_pause.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        // What each line of the pause menu does, by the line's text
        // ("KSTR_PAUSE_CONTINUE" and the others).
        enum PauseChoice : uint8_t
        {
            kPauseContinue = 0,
            kPausePlayAgain = 1,
            kPauseGoals = 2,
            kPauseBikeParts = 3,
            kPauseRecords = 4,
            kPauseChangeRider = 5,
            kPauseOptions = 6,
            kPauseTrickList = 7,
            kPauseSaveGame = 8,
            kPauseLevelSelect = 9,
            kPauseExitGame = 10,
            kPauseExitTest = 11,
        };
        constexpr uint32_t kPauseChoices = 12;
        // The lines' texts, by PauseChoice.
        inline ee::Global<ee::Ptr<const char>[kPauseChoices], sym::g_pauseLineTexts> g_pauseLineTexts;

        constexpr uint32_t kFramePause = sym::g_textFramePause;               // "FramePause"
        constexpr uint32_t kPauseLineFormat = sym::bmx::g_textPauseLineFormat; // "%s$%s": a separator, then the line's key
    }

    // pauseScreenMessage: as the menu comes up its lines are made: continue
    // (play again once the game is over); the goals and the bike parts in
    // the game with goals; the records and the options; then exit, with
    // "save game" before it once the game is over, or the one exit of a
    // park's test. Chosen, a line shows its screen; the exits ask first.
    void pauseScreenMessage(int32_t message)
    {
        ee::Local<char[0x200]> text;
        ee::Local<ee::Ptr<const char>[kPauseChoices]> lineTexts;
        ee::Local<uint32_t> lines;
        ee::Local<uint32_t> selected;

        for (uint32_t i = 0; i < kPauseChoices; ++i)
            lineTexts[i] = (*g_pauseLineTexts)[i];
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFramePause, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
        {
            lines = 0;
            (*g_menuChoices)[lines] = gameScreen() == kScreenPlayAgain ? kPausePlayAgain : kPauseContinue;
            lines = lines + 1;
            if (gameMode() == calls::kGoalsMode)
            {
                (*g_menuChoices)[lines] = kPauseGoals;
                (*g_menuChoices)[lines + 1] = kPauseBikeParts;
                lines = lines + 2;
            }
            const uint8_t session = *calls::g_sessionKind;
            if (session != calls::kSessionParkTest)
            {
                (*g_menuChoices)[lines] = kPauseRecords;
                lines = lines + 1;
            }
            (*g_menuChoices)[lines] = kPauseOptions;
            lines = lines + 1;
            if (session != calls::kSessionParkTest)
            {
                if (gameScreen() == kScreenPlayAgain)
                {
                    (*g_menuChoices)[lines] = kPauseSaveGame;
                    lines = lines + 1;
                }
                (*g_menuChoices)[lines] = kPauseExitGame;
                lines = lines + 1;
            }
            else
            {
                (*g_menuChoices)[lines] = kPauseExitTest;
                lines = lines + 1;
            }

            text[0] = '\0';
            for (uint32_t i = 0; i < lines; ++i)
            {
                const ee::Ptr<char> end = ee::Ptr<char>(text) + zge::stringLength(text);
                zge::formatString(end, kPauseLineFormat, i == 0 ? kNoSeparator : kLineBreak,
                                  lineTexts[(*g_menuChoices)[i]]);
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 1), kItemsName, text);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 1), kVisibleCountName, lines);
            playUiNode(frame, kInitAction);
            if (gameScreen() == kScreenPlayAgain)
                setCameraMode(*calls::g_gameCamera, 0, static_cast<uint32_t>(CameraMode::Van));
            return;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenLeft:
            if (gameScreen() == kScreenPlayAgain)
                setCameraMode(*calls::g_gameCamera, 0, static_cast<uint32_t>(CameraMode::Idle));
            return;

        case kScreenChosen:
            if (gameScreen() == kScreenPauseNoChoice)
                selected = 0;
            else
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 1), kSelectedIndexName, selected);
            switch ((*g_menuChoices)[selected])
            {
            case kPausePlayAgain:
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeNewGame;
                return;
            case kPauseGoals:
                showGameScreen(kScreenPauseObjectives);
                return;
            case kPauseBikeParts:
                showGameScreen(kScreenBikeParts);
                return;
            case kPauseRecords:
                showGameScreen(kScreenRecords);
                return;
            case kPauseChangeRider:
                *g_screenBehindExitConfirm = kScreenPause;
                showGameScreen(kScreenExitLevel);
                *g_frontEndFirstMenu = kFrontEndChangeRider;
                return;
            case kPauseOptions:
                showGameScreen(kScreenOptions);
                return;
            case kPauseTrickList:
                showGameScreen(kScreenTrickList);
                return;
            case kPauseSaveGame:
                return;
            case kPauseLevelSelect:
                *g_screenBehindExitConfirm = kScreenPause;
                showGameScreen(kScreenExitLevel);
                *g_frontEndFirstMenu = kFrontEndLevelSelect;
                return;
            case kPauseExitGame:
                *g_screenBehindExitConfirm = kScreenPause;
                showGameScreen(kScreenExitGame);
                return;
            case kPauseExitTest:
                *g_screenBehindExitConfirm = kScreenPause;
                showGameScreen(kScreenExitLevel);
                return;
            default: // continue
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumePlay;
                return;
            }

        case kScreenButton14:
            showGameScreen(kScreenPlay);
            *g_resumeAction = kResumePlay;
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(pauseScreenMessage, Verified);
}
