// DM2's instructions screen (screens.h; "InGameUI.FrameInstructions"): shown
// as a game starts, with the level's name and what the game asks; its menu
// starts the game or goes back. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/action_state.h"
#include "dm2/game/round.h"
#include "dm2/menu/front_end.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_instructions.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kInstructionsFrame{sym::instructionsFramePath}; // "InGameUI.FrameInstructions"
        constexpr ee::Ptr<const char> kInstructionsMenu{sym::instructionsMenuPath};   // "InGameUI.FrameInstructions.Menu"
        constexpr ee::Ptr<const char> kLevelNameLabel{sym::g_textLevelName};         // "LevelName"
        constexpr ee::Ptr<const char> kInstructionsLabel{sym::g_textInstructionStr}; // "InstructionStr"
        constexpr ee::Ptr<const char> kText{sym::screenTextName};                    // "Text"
        // The string table's keys: the level's name, and what each
        // single-player game asks on the level (the free ride and the park
        // test say the same on every level), or what a multiplayer mode asks.
        constexpr uint32_t kLevelNameKey = sym::levelNameKeyFormat;                // "$KSTR_LEVELNAME_%02d"
        constexpr uint32_t kLevelGoalsKey = sym::instructionsLevelKey;             // "$KSTRSP_INST_LEV%02d"
        constexpr uint32_t kCompetitionKey = sym::instructionsCompetitionKey;      // "$KSTRSP_INST_COMP_LEV%02d"
        constexpr uint32_t kSessionKey = sym::instructionsSessionKey;              // "$KSTRSP_INST_SESSION_LEV%02d"
        constexpr uint32_t kFreeRideKey = sym::instructionsFreeRideKey;            // "$KSTRSP_INST_FREE"
        constexpr uint32_t kParkTestKey = sym::instructionsParkTestKey;            // "$KSTRSP_INST_TEST"
        constexpr uint32_t kMultiplayerKey = sym::multiplayerInstructionsKey;      // "$KSTRMP_INST_MODE%02d"

        // Where the front end opens when the game is left (front_end.h).
        inline ee::Global<uint8_t, sym::g_frontEndOpens> g_frontEndOpens;

        // The menu's choices: go on, and back (cancelling: -1 too).
        constexpr int32_t kChoiceGoOn = 0, kChoiceBack = 1, kCancelled = -1;
    }

    void showInstructionsScreen(uint32_t shown)
    {
        ee::Local<char[0x40]> text;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kInstructionsFrame);
        if (!frame)
            return;
        if (shown == 0)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        zge::formatString(text, kLevelNameKey, *g_sessionLevel);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kLevelNameLabel, 0), kText, text);
        if (multiplayerMode() == 0)
        {
            // (Every key is passed the level, as the original does; the free
            // ride's and the park test's read none.)
            switch (singlePlayerMode())
            {
            case kLevelGoals:
                zge::formatString(text, kLevelGoalsKey, *g_sessionLevel);
                break;
            case kCompetition:
                zge::formatString(text, kCompetitionKey, *g_sessionLevel);
                break;
            case kFreeRide:
                zge::formatString(text, kFreeRideKey, *g_sessionLevel);
                break;
            case kParkTest:
                zge::formatString(text, kParkTestKey, *g_sessionLevel);
                break;
            default:
                zge::formatString(text, kSessionKey, *g_sessionLevel);
                break;
            }
        }
        else
        {
            zge::formatString(text, kMultiplayerKey, multiplayerMode());
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kInstructionsLabel, 0), kText, text);
        playWidgetAction(frame, kInitAction);
    }
    READABLE_FUNCTION(showInstructionsScreen, Verified);

    // The menu's script function: going on starts the turn (HORSE asks for
    // its word first); going back asks whether to leave for the level
    // choice, or in a multiplayer game goes back to picking the game.
    void bmxoverlayInstructionsHandler()
    {
        ee::Local<int32_t> picked;
        zge::zgui::copyVariable(uiWidget(kInstructionsMenu), kSelectedIndexName, picked);
        if (*picked == kChoiceGoOn)
        {
            if (multiplayerMode() == kHorseMode)
            {
                showGameScreen(kScreenHorseWord);
            }
            else
            {
                showGameScreen(kScreenNone);
                setRoundPhase(kTurnStartPhase);
            }
            return;
        }
        if (*picked != kChoiceBack && *picked != kCancelled)
            return;
        if (multiplayerMode() == 0)
        {
            *g_screenBehindExitConfirm = kScreenInstructions;
            showGameScreen(kScreenExitConfirm);
            *g_frontEndOpens = kOpensLevelChoice;
        }
        else
        {
            showGameScreen(kScreenMultiplayerGameSelect);
        }
    }
    READABLE_FUNCTION(bmxoverlayInstructionsHandler, Verified);
}
