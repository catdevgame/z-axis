// DM2's game over screen (screens.h; "InGameUI.FrameGameOver"): its show
// (the title, the scores and the menu built for the game played) and its
// menu's script function. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "dm2/camera/camera.h"
#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "dm2/menu/front_end.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_game_over.cpp.h"

namespace game::dm2::rider::physics
{
    void riderReset(uint32_t index, uint32_t shown); // respawn.cpp
}

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kGameOverMenu{sym::gameOverMenuPath}; // "InGameUI.FrameGameOver.Menu"
        constexpr int32_t kCancelled = -1;
        // Where the front end opens when the game is left (front_end.h).
        inline ee::Global<uint8_t, sym::g_frontEndOpens> g_frontEndOpens;
        // The camera mode a multiplayer game's choice of game is shown with.
        constexpr uint32_t kGameSelectCameraMode = 4;

        // The game left through the exit confirmation, over this screen, for
        // the front end at `opens`; the menus answer any player.
        void confirmLeaving(uint8_t opens)
        {
            *g_screenBehindExitConfirm = kScreenGameOver;
            showGameScreen(kScreenExitConfirm);
            *g_frontEndOpens = opens;
            *g_currentPlayerOnly = 0;
        }
    }

    namespace
    {
        constexpr ee::Ptr<const char> kGameOverFrame{sym::g_textFrameGameOver};   // "InGameUI.FrameGameOver"
        constexpr ee::Ptr<char> kGameOverInit{sym::gameOverInitScript};          // "PLAY Root.InGameUI.FrameGameOver.Init"
        constexpr ee::Ptr<char> kGameOverDeInit{sym::gameOverDeInitScript};      // "PLAY Root.InGameUI.FrameGameOver.DeInit"
        // The frame's children and their variables.
        constexpr ee::Ptr<const char> kTitleName{sym::screenTitleName};          // "Title"
        constexpr ee::Ptr<const char> kMessageName{sym::g_textMessage};          // "Message"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};            // "Text"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};              // "Items"
        constexpr ee::Ptr<const char> kVisibleCountName{sym::g_textVisibleCount}; // "VisibleCount"
        // The title: a single-player game's, a multiplayer game's winner (a
        // player number), or a tie.
        constexpr ee::Ptr<const char> kTitleText{sym::g_textGameOverTitle};      // "$KSTR_GAMEOVER_TITLE"
        constexpr ee::Ptr<const char> kWinnerFormat{sym::g_textSeriesWinFormat}; // "$KSTR_PLAYER $KSTR_NUMBER_%ld $KSTR_SERIES_WIN_1"
        constexpr ee::Ptr<const char> kTieText{sym::g_textTie};                  // "$KSTR_TIE"
        // The message: the score, or each player's.
        constexpr ee::Ptr<const char> kYourScoreFormat{sym::g_textYourScoreFormat};   // "%s$KSTR_YOURSCORE %ld"
        constexpr ee::Ptr<const char> kPlayerScoreFormat{sym::g_textPlayerScoreFormat}; // "%s$KSTR_PLAYER %d $KSTR_SCORE "
        constexpr ee::Ptr<const char> kNumberFormat{sym::recordNumberFormat};          // "%ld"
        constexpr ee::Ptr<const char> kTricksFormat{sym::g_textTricksCountFormat};     // "%d $KSTR_TRICKS"
        constexpr ee::Ptr<const char> kDegreesFormat{sym::g_textDegreesCountFormat};   // "%d %s"
        constexpr ee::Ptr<const char> kDegreesText{sym::g_textDegrees};                // "degrees"
        constexpr ee::Ptr<const char> kStringFormat{sym::screenStringFormat};          // "%s"
        // The menu's lines' texts ("%s$KSTR_GAMEOVER_...": each after its separator).
        constexpr ee::Ptr<const char> kPlayAgain0Text{sym::g_textGameOverPlayAgain0};
        constexpr ee::Ptr<const char> kPlayAgain1Text{sym::g_textGameOverPlayAgain1};
        constexpr ee::Ptr<const char> kLevelSelectText{sym::g_textGameOverLevelSelect};
        constexpr ee::Ptr<const char> kBikeShopText{sym::g_textGameOverBikeShop};
        constexpr ee::Ptr<const char> kSaveText{sym::g_textGameOverSave};
        constexpr ee::Ptr<const char> kSaveReplayText{sym::g_textGameOverSaveReplay};
        constexpr ee::Ptr<const char> kViewReplayText{sym::g_textGameOverViewReplay};
        constexpr ee::Ptr<const char> kTrickListText{sym::g_textGameOverTrickList};
        constexpr ee::Ptr<const char> kViewRecordsText{sym::g_textGameOverViewRecords};
        constexpr ee::Ptr<const char> kExitText{sym::g_textGameOverExit};
        constexpr ee::Ptr<const char> kTestText{sym::g_textGameOverTest};
        constexpr ee::Ptr<const char> kGameSelectText{sym::g_textGameOverGameSelect};
        constexpr ee::Ptr<const char> kNoSeparator{sym::g_textEmpty}; // ""
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine}; // a line feed

        // (RiderCharacter::flags kCharacterNewBike, kCharacterNewLook: the
        // menu offers the bike shop for them, opening at the bike choice or
        // at the rider choice.)
        // The menu offers the level choice once two levels are open, the
        // bike shop (a session's, free ride's) once two bikes are.
        constexpr uint32_t kChoicesWorthOffering = 2;
        constexpr uint32_t kSaveLevels = kParkLevel;   // (the park editor's park is not saved from here)
        // The degrees of a half turn (mode 10 scores half turns).
        constexpr int32_t kDegreesPerHalfTurn = 180;

        // A line added to the menu: what it does (g_gameOverLines) and its
        // text, after a line break unless it is the first.
        void addMenuLine(ee::Ptr<char> menu, ee::Local<uint32_t> &lines, uint16_t line, ee::Ptr<const char> text)
        {
            (*g_gameOverLines)[*lines] = line;
            const ee::Ptr<char> end = menu + static_cast<int32_t>(zge::stringLength(menu));
            zge::formatString(end, text, *lines == 0 ? kNoSeparator : kLineBreak);
            *lines = *lines + 1;
        }
        ee::Ptr<char> textEnd(ee::Ptr<char> text)
        {
            return text + static_cast<int32_t>(zge::stringLength(text));
        }

        // The title of a multiplayer game but a series or HORSE: the player
        // who holds the game's record of its mode's kind, or a tie.
        void winnerTitle(ee::Ptr<char> title)
        {
            if (roundFlags(kTieFlag))
            {
                zge::formatString(title, kTieText);
                return;
            }
            const uint32_t mode = multiplayerMode();
            const uint32_t kind = (*g_modeRecordKinds)[multiplayerMode()];
            const uint32_t single = singlePlayerMode();
            zge::formatString(title, kWinnerFormat, gameRecord(recordKey(mode, single, kind, kGameRecords))->player);
        }
        // A series' or HORSE's title: the game's winner.
        void seriesWinnerTitle(ee::Ptr<char> title)
        {
            zge::formatString(title, kWinnerFormat, roundWinner(roundNumber(), 1));
        }
    }

    void formatModeScore(ee::Ptr<char> out, uint32_t gameScore)
    {
        switch (multiplayerMode())
        {
        case 6: case 7: case 8: case 11: case 12: case 13: // scored as a distance
            formatFeetAndInches(out, zge::unsignedToFloat(gameScore));
            break;
        case 9: // scored in tricks
            zge::formatString(out, kTricksFormat, gameScore);
            break;
        case 10: // scored in half turns
            zge::formatString(out, kDegreesFormat, static_cast<int32_t>(gameScore) * kDegreesPerHalfTurn, kDegreesText);
            break;
        default:
            zge::formatString(out, kNumberFormat, gameScore);
            break;
        }
    }

    // showGameOverScreen: as the screen opens, its title, message and menu
    // for the game played. A single-player game: the score (but in a
    // competition, free ride or park test), then play again, the level
    // choice (two levels open), the bike shop (a ProQuest or competition
    // game: when the character's flags offer it; else two bikes open), save
    // and save the replay (but in a free ride, a park test or the park
    // editor's level), view the replay. A series or HORSE: the winner (or a
    // tie) and the standings; any other multiplayer game: its record's
    // holder (or a tie) and each player's score. Then the trick list and the
    // records (but in a park test; a park test has no trick list), and exit
    // (the game select in a multiplayer game, "test" in a park test). As it
    // closes, the frame's DeInit.
    void showGameOverScreen(uint32_t shown)
    {
        ee::Local<char[0x200]> menuText;
        ee::Local<char[0x200]> messageText;
        ee::Local<char[0x40]> titleText;
        ee::Local<char[0x40]> scoreText;
        ee::Local<uint32_t> lines;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kGameOverFrame);
        if (shown == 0)
        {
            runUiScript(kGameOverDeInit);
            return;
        }
        *g_uiModalPlayer = *g_multiplayerPlayers == 1 ? 0 : -1;
        const ee::Ptr<char> menu{menuText};
        const ee::Ptr<char> message{messageText};
        const ee::Ptr<char> title{titleText};
        messageText[0] = 0;
        menuText[0] = 0;
        *lines = 0;
        const uint8_t mode = multiplayerMode();
        if (mode == 0)
        {
            const ee::Ptr<RiderCharacter> character = playerCharacter(*g_currentPlayer);
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
            zge::formatString(title, kTitleText);
            const uint8_t single = singlePlayerMode();
            if (single != kCompetition && single != kFreeRide && single != kParkTest)
                zge::formatString(textEnd(message), kYourScoreFormat, kNoSeparator, rider->score.gameScore);
            addMenuLine(menu, lines, kGameOverPlayAgain, kPlayAgain0Text);
            uint32_t openLevels = 0;
            for (uint32_t level = 0; level < kRespectLevels; ++level)
                openLevels += character->levels[level].flags & kLevelOpen;
            const uint8_t game = singlePlayerMode();
            if (game == kLevelGoals || game == kCompetition)
            {
                if (openLevels >= kChoicesWorthOffering)
                    addMenuLine(menu, lines, kGameOverLevelSelect, kLevelSelectText);
                if (character->flags & kCharacterNewBike)
                    addMenuLine(menu, lines, kGameOverBikeShop, kBikeShopText);
                else if (character->flags & kCharacterNewLook)
                    addMenuLine(menu, lines, kGameOverRiderShop, kBikeShopText);
            }
            else if (game != kParkTest)
            {
                if (openLevels >= kChoicesWorthOffering)
                    addMenuLine(menu, lines, kGameOverLevelSelect, kLevelSelectText);
                if (character->openBikes >= kChoicesWorthOffering)
                    addMenuLine(menu, lines, kGameOverBikeShop, kBikeShopText);
            }
            if (singlePlayerMode() != kFreeRide && singlePlayerMode() != kParkTest && *g_sessionLevel < kSaveLevels)
            {
                addMenuLine(menu, lines, kGameOverSave, kSaveText);
                addMenuLine(menu, lines, kGameOverSaveReplay, kSaveReplayText);
            }
            addMenuLine(menu, lines, kGameOverViewReplay, kViewReplayText);
        }
        else if (mode == kSeriesMode)
        {
            if (roundFlags(kTieFlag))
                zge::formatString(title, kTieText);
            else
                seriesWinnerTitle(title);
            seriesStandingText(message);
            addMenuLine(menu, lines, kGameOverPlayAgain, kPlayAgain1Text);
            addMenuLine(menu, lines, kGameOverViewReplay, kViewReplayText);
        }
        else if (mode == kHorseMode)
        {
            seriesWinnerTitle(title);
            horseLettersText(message);
            addMenuLine(menu, lines, kGameOverPlayAgain, kPlayAgain1Text);
            addMenuLine(menu, lines, kGameOverViewReplay, kViewReplayText);
        }
        else
        {
            winnerTitle(title);
            uint32_t players = 0;
            for (uint32_t player = 0; player < *g_multiplayerPlayers;)
            {
                const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
                const ee::Ptr<char> end = textEnd(message);
                const ee::Ptr<const char> separator = players++ == 0 ? kNoSeparator : kLineBreak;
                ++player;
                zge::formatString(end, kPlayerScoreFormat, separator, player);
                const ee::Ptr<char> score{scoreText};
                formatModeScore(score, rider->score.gameScore);
                zge::formatString(textEnd(message), kStringFormat, score);
            }
            addMenuLine(menu, lines, kGameOverPlayAgain, kPlayAgain1Text);
            if (!roundFlags(kTieFlag))
                addMenuLine(menu, lines, kGameOverViewReplay, kViewReplayText);
        }

        if (multiplayerMode() == 0 && singlePlayerMode() != kParkTest)
            addMenuLine(menu, lines, kGameOverTrickList, kTrickListText);
        if (multiplayerMode() != 0 || singlePlayerMode() != kParkTest)
            addMenuLine(menu, lines, kGameOverRecords, kViewRecordsText);
        (*g_gameOverLines)[*lines] = kGameOverExit;
        ee::Ptr<const char> exitText = kGameSelectText;
        if (multiplayerMode() == 0)
            exitText = singlePlayerMode() == kParkTest ? kTestText : kExitText;
        addMenuLine(menu, lines, kGameOverExit, exitText);

        zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kTextName, title);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kItemsName, menu);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, 0), kVisibleCountName, lines);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMessageName, 0), kTextName, message);
        runUiScript(kGameOverInit);
    }
    READABLE_FUNCTION(showGameOverScreen, Verified);

    // bmxoverlayGameOverHandler: the menu's line taken (backing out takes
    // the first): play again, leave for the front end's level, bike or
    // rider choice or its main menu (a multiplayer game goes back to
    // choosing the game), save, watch the round's replay, the trick list,
    // the records.
    void bmxoverlayGameOverHandler()
    {
        ee::Local<int32_t> picked;
        zge::zgui::copyVariable(uiWidget(kGameOverMenu), kSelectedIndexName, picked);
        if (*picked == kCancelled)
            *picked = 0;
        switch ((*g_gameOverLines)[*picked])
        {
        case kGameOverPlayAgain:
        case kGameOverNoLine:
            showGameScreen(kScreenNone);
            if (multiplayerMode() != 0)
                setSinglePlayerMode(static_cast<uint8_t>(*g_mode1Type));
            startGame(multiplayerMode());
            if (multiplayerMode() != 0 || singlePlayerMode() == kLevelGoals)
                setRoundPhase(kTurnStartPhase);
            break;
        case kGameOverLevelSelect:
            confirmLeaving(kOpensLevelChoice);
            break;
        case kGameOverBikeShop:
            confirmLeaving(kOpensBikeChoice);
            break;
        case kGameOverRiderShop:
            confirmLeaving(kOpensRiderChoice);
            break;
        case kGameOverSave:
            showGameScreen(kScreenSaveGame);
            break;
        case kGameOverViewReplay:
        {
            // The round's replay, watched from the current player's view
            // (the pad test below keeps it whatever is held: it asks the
            // current player's pad each time round).
            showGameScreen(kScreenNone);
            actionStart(kActionRoundReplay);
            *g_actionRequest = static_cast<int8_t>(*g_currentPlayer);
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                if (rider::padButtons(*g_currentPlayer, zge::kPadCross, rider::kAllDown))
                    *g_actionRequest = static_cast<int8_t>(player);
            break;
        }
        case kGameOverTrickList:
            showGameScreen(kScreenGameOverTrickList);
            break;
        case kGameOverRecords:
            showGameScreen(kScreenGameOverRecords);
            break;
        case kGameOverExit:
            if (multiplayerMode() == 0)
            {
                *g_screenBehindExitConfirm = kScreenGameOver;
                showGameScreen(kScreenExitConfirm);
                *g_frontEndOpens = kOpensMainMenu;
            }
            else
            {
                for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                    rider::physics::riderReset(riderIndexOf(player), 0);
                setCameraPlayer(0);
                setCameraMode(0, 0, kGameSelectCameraMode);
                showGameScreen(kScreenMultiplayerGameSelect);
            }
            *g_currentPlayerOnly = 0;
            break;
        case kGameOverSaveReplay:
            showGameScreen(kScreenSaveReplay);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayGameOverHandler, Verified);
}
