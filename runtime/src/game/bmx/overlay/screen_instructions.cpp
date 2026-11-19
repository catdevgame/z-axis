// BMX XXX's instructions screen (screens.h; "FrameInstructions"): the
// level's or the game mode's name and what to do there, before the game
// starts. BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_instructions.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameInstructions = sym::g_textFrameInstructions; // "FrameInstructions"
        constexpr uint32_t kLevelNameWidget = sym::g_textLevelName;           // "LevelName"
        constexpr uint32_t kInstructionWidget = sym::g_textInstructionStr;    // "InstructionStr"
        // The texts' keys: a multiplayer mode's name and instructions by the
        // mode, a single-player game's instructions by the level.
        constexpr uint32_t kModeNameKey = sym::bmx::g_textModeNameKey;                   // "$KSTRMP_MODENAME_MODE%02d"
        constexpr uint32_t kModeInstructionsKey = sym::bmx::g_textModeInstructionsKey;   // "$KSTRMP_INST_MODE%02d"
        constexpr uint32_t kLevelInstructionsKey = sym::bmx::g_textLevelInstructionsKey; // "$KSTRSP_INST_LEV%02d"
        constexpr uint32_t kCompetitionInstructionsKey = sym::bmx::g_textCompetitionInstructionsKey; // "$KSTRSP_INST_COMP_LEV%02d"
        constexpr uint32_t kSessionInstructionsKey = sym::bmx::g_textSessionInstructionsKey; // "$KSTRSP_INST_SESSION_LEV%02d"
        constexpr uint32_t kFreeInstructionsKey = sym::bmx::g_textFreeInstructionsKey;   // "$KSTRSP_INST_FREE"
        constexpr uint32_t kTestInstructionsKey = sym::bmx::g_textTestInstructionsKey;   // "$KSTRSP_INST_TEST"

        // The mode whose game.c screen comes before its first turn.
        constexpr uint8_t kModeWithOwnStart = 6;

        // The park's theme: which of the four rows after the twelve levels
        // ("Mullaly Park" .. "Z-Axis Warehouse") a park is in.
        inline ee::Global<uint8_t, sym::g_parkTheme> g_parkTheme;
    }

    void instructionsScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> text;
        ee::Local<char[0x100]> title;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameInstructions, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenChosen:
        {
            const uint8_t mode = gameMode();
            if (mode == calls::kGoalsMode)
            {
                setRoundPhase(calls::kTurnStartPhase);
            }
            else if (mode == kModeWithOwnStart)
            {
                showGameScreen(kScreenBeforeGame);
            }
            else
            {
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeTurnStart;
            }
            return;
        }

        case kScreenEnter:
        {
            // The name: the level's title in capitals, or the multiplayer mode's.
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
            {
                const uint32_t level = *calls::g_currentLevel;
                const ee::Ptr<calls::LevelInfo> info =
                    level < calls::kLevels ? calls::levelInfo(level) : calls::levelInfo(calls::kLevels + *g_parkTheme);
                zge::stringCopy(title, info->title);
                const ee::Ptr<Node> name = zge::zgui::findNode(frame, kLevelNameWidget, 1);
                zge::zgui::setVariable(name, kTextName, zge::stringToUpper(title));
            }
            else
            {
                zge::formatString(text, kModeNameKey, gameMode());
                zge::zgui::setVariable(zge::zgui::findNode(frame, kLevelNameWidget, 1), kTextName, text);
            }

            switch (gameMode())
            {
            case calls::kGoalsMode:
                zge::formatString(text, kLevelInstructionsKey, *calls::g_currentLevel);
                break;
            case kCompetitionMode:
                zge::formatString(text, kCompetitionInstructionsKey, *calls::g_currentLevel);
                break;
            case calls::kSessionMode:
                zge::formatString(text, kSessionInstructionsKey, *calls::g_currentLevel);
                break;
            case calls::kFreeMode:
                if (*calls::g_sessionKind == calls::kSessionParkTest)
                    zge::formatString(text, kTestInstructionsKey);
                else
                    zge::formatString(text, kFreeInstructionsKey);
                break;
            default:
                zge::formatString(text, kModeInstructionsKey, gameMode());
                break;
            }
            zge::zgui::setVariable(zge::zgui::findNode(frame, kInstructionWidget, 1), kTextName, text);
            playUiNode(frame, kInitAction);
            return;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenCancelled:
            // Back out: a single-player game asks before leaving for the
            // level select; a multiplayer game goes to its end's screen.
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
            {
                *g_screenBehindExitConfirm = kScreenInstructions;
                showGameScreen(kScreenExitLevel);
                *g_frontEndFirstMenu = kFrontEndLevelSelect;
            }
            else
            {
                showGameScreen(kScreenAfterGame);
            }
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(instructionsScreenMessage, Verified);
}
