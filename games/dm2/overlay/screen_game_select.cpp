// DM2's multiplayer game select screen (screens.h; "InGameUI.FrameMPGameSelect"):
// the menu of the multiplayer games, its description of the game the cursor
// is on, and its script function. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_game_select.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kGameSelectFrame{sym::gameSelectFramePath};   // "InGameUI.FrameMPGameSelect"
        constexpr ee::Ptr<const char> kGameSelectMenu{sym::gameSelectMenuPath};     // "FrameMenu.Menu"
        constexpr ee::Ptr<const char> kDescriptionFrame{sym::descriptionFramePath}; // "FrameDescription"
        constexpr ee::Ptr<const char> kDescriptionText{sym::descriptionTextPath};   // "FrameDescription.Description"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};               // "Text"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};                 // "Items"
        constexpr ee::Ptr<const char> kPositionName{sym::g_textItemPosition};       // "Position"
        // The menu's lines: the thirteen multiplayer modes (the first with
        // mode 1's type), then the exit.
        constexpr ee::Ptr<const char> kModeAndTypeKey{sym::multiplayerModeAndTypeKey}; // "$KSTRMP_MODENAME_MODE%02d - $KSTRMP_MODE01TYPENAME_TYPE%02d"
        constexpr ee::Ptr<const char> kModeNameKey{sym::multiplayerModeNameKey};       // "$KSTRMP_MODENAME_MODE%02d"
        constexpr ee::Ptr<const char> kExitKey{sym::gameOverExitKey};                  // "$KSTR_GAMEOVER_EXIT"
        constexpr ee::Ptr<const char> kModeDescriptionKey{sym::multiplayerInstructionsKey}; // "$KSTRMP_INST_MODE%02d"
        constexpr ee::Ptr<const char> kLineJoin{sym::trickWordFormat}; // "%s%s"
        constexpr ee::Ptr<const char> kNoBreak{sym::g_textEmpty};
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};
        constexpr uint32_t kModeLines = 13;
        constexpr int32_t kExitLine = kModeLines;
        constexpr uint32_t kMenuLineBytes = 0x40, kMenuTextBytes = 0x200;
        // The description's place: beside the menu, level with its line.
        constexpr float kDescriptionX = 370.0f, kDescriptionFirstY = 170.0f;
        inline ee::Global<ee::Float, sym::g_gameSelectLineSpacing> g_gameSelectLineSpacing; // 11 (a step over)

        // What the menu's script asks (g_uiMessageType): the line taken, the
        // cursor moved, a left or right press.
        inline ee::Global<int32_t, sym::g_uiMessageType> g_uiMessageType;
        constexpr int32_t kLineTaken = 0, kCursorMoved = 1, kSideways = 2;
        constexpr int32_t kNoLine = -1;
    }

    // setGameSelectMenuItems: the menu's lines made: each multiplayer mode's
    // name (the first with mode 1's type), and the exit.
    void setGameSelectMenuItems(ee::Ptr<zge::zgui::Node> menu)
    {
        ee::Local<char[kMenuTextBytes]> text;
        ee::Local<char[kMenuLineBytes]> line;
        text[0] = 0;
        for (uint32_t i = 0; i <= kModeLines; ++i)
        {
            if (i == 0)
                zge::formatString(line, kModeAndTypeKey, 1, *g_mode1Type);
            else if (i == kModeLines)
                zge::formatString(line, kExitKey);
            else
                zge::formatString(line, kModeNameKey, i + 1);
            const int32_t length = zge::stringLength(text);
            zge::formatString(text.address() + length, kLineJoin, i == 0 ? kNoBreak : kLineBreak, line);
        }
        zge::zgui::setVariable(menu, kItemsName, text);
    }
    READABLE_FUNCTION(setGameSelectMenuItems, Verified);

    // showGameSelectScreen: the screen shown (any player's pad answers; its
    // menu, the first mode's description) or closed.
    void showGameSelectScreen(uint32_t shown)
    {
        ee::Local<char[0x40]> description;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kGameSelectFrame);
        if (!shown)
        {
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        *g_uiModalPlayer = kAnyPlayer;
        setGameSelectMenuItems(zge::zgui::findNode(frame, kGameSelectMenu, 0));
        zge::formatString(description, kModeDescriptionKey, 1);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kDescriptionText, 0), kTextName, description);
        playWidgetAction(frame, kInitAction);
    }
    READABLE_FUNCTION(showGameSelectScreen, Verified);

    // bmxoverlayMPGameSelectHandler: the menu's script function. A mode's
    // line taken starts its game (mode 1 with its type: HORSE first asks
    // for the word, the others start their first turn); the exit line asks
    // to confirm. The cursor moved shows its mode's description beside its
    // line. On mode 1's line a player's left or right changes its type.
    void bmxoverlayMPGameSelectHandler()
    {
        ee::Local<char[0x40]> description;
        ee::Local<ee::Float[3]> position;
        ee::Local<int32_t> picked;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kGameSelectFrame);
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kGameSelectMenu, 0), kSelectedIndexName, picked);
        switch (*g_uiMessageType)
        {
        case kLineTaken:
            if (*picked == kExitLine)
            {
                *g_screenBehindExitConfirm = kScreenMultiplayerGameSelect;
                showGameScreen(kScreenExitConfirm);
                *g_currentPlayerOnly = 0;
                return;
            }
            setSinglePlayerMode(static_cast<uint8_t>(*g_mode1Type));
            startGame(static_cast<uint32_t>(*picked + 1));
            if (multiplayerMode() == kHorseMode)
                showGameScreen(kScreenHorseWord);
            else
            {
                showGameScreen(kScreenNone);
                setRoundPhase(kTurnStartPhase);
            }
            return;
        case kCursorMoved:
            if (*picked == kNoLine)
                return;
            zge::formatString(description, kModeDescriptionKey, *picked + 1);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kDescriptionText, 0), kTextName, description);
            position[0] = kDescriptionX;
            position[1] = ee::toFloat(*picked) * *g_gameSelectLineSpacing + ee::Float(kDescriptionFirstY);
            position[2] = 0.0f;
            zge::zgui::setVariable(zge::zgui::findNode(frame, kDescriptionFrame, 0), kPositionName, position);
            return;
        case kSideways:
            if (*picked != 0)
                return;
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                if (rider::padButtons(player, zge::kPadLeft, rider::kAnyPressed))
                {
                    if (--*g_mode1Type < 0)
                        *g_mode1Type = kMode1Types - 1;
                    break;
                }
                if (rider::padButtons(player, zge::kPadRight, rider::kAnyPressed))
                {
                    if (++*g_mode1Type == kMode1Types)
                        *g_mode1Type = 0;
                    break;
                }
            }
            setGameSelectMenuItems(zge::zgui::findNode(frame, kGameSelectMenu, 0));
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION(bmxoverlayMPGameSelectHandler, Verified);
}
