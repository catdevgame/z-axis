#pragma once
// DM2's game screens (overlay.c): what the screens' files share. A game
// screen is an "InGameUI" frame put up over play (the pause menu, the
// objectives, the game over screen ...); showGameScreen (overlay.h) switches
// between them, and the screens' task runs the one that is up.

#include "overlay.h"

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/screens.h.h"

namespace game::dm2
{
    // A game screen: its handlers and its menu.
    struct GameScreenRecord
    {
        ee::FunctionPtr<void(uint32_t)> show; // +0x00 (1) as the screen opens, (0) as it closes
        ee::FunctionPtr<uint32_t()> input;    // +0x04 its own input handling (the screens' input frame calls it)
        ee::FunctionPtr<void()> frame;        // +0x08 its own frame, run in place of the play overlay's frames; a screen
                                              //       with one also colours its own menu items (no screen of the table has one)
        uint8_t choices;                      // +0x0C the choices of its menu (up and down wrap round them)
        uint8_t choice;                       // +0x0D the choice its menu is on
        uint8_t _0e[2];
    };
    static_assert(offsetof(GameScreenRecord, choices) == 0x0C && sizeof(GameScreenRecord) == 0x10);
    constexpr uint32_t kGameScreens = 30;
    inline ee::Global<ee::Ptr<GameScreenRecord>[kGameScreens], sym::g_gameScreens> g_gameScreens;

    // The game screen shown (a GameScreen), and the screen the exit
    // confirmation was asked over.
    inline ee::Global<uint8_t, sym::g_gameScreen> g_gameScreen;
    inline ee::Global<uint8_t, sym::g_screenBehindExitConfirm> g_screenBehindExitConfirm;
    // The record of the screen that is up (0: none), and the last screen asked for.
    inline ee::Global<ee::Ptr<GameScreenRecord>, sym::g_gameScreenRecord> g_gameScreenRecord;
    inline ee::Global<uint8_t, sym::g_lastGameScreen> g_lastGameScreen;

    // The overlay items a screen made (zoverlay handles), given up as it closes.
    constexpr uint32_t kScreenItems = 24;
    constexpr uint16_t kNoScreenItem = 0xFFFF;
    inline ee::Global<uint16_t[kScreenItems], sym::g_screenItems> g_screenItems;

    // The step the screen that is up has reached (each screen's own count:
    // the NPC screen's page, the reward screen's reward). The byte after it
    // is cleared with it and read by nothing.
    inline ee::Global<uint8_t[2], sym::g_screenStep> g_screenStep;
    // The frames the screen's step has taken input for, and the frames since
    // its menu's choice last moved.
    inline ee::Global<uint32_t, sym::g_screenFrames> g_screenFrames;
    inline ee::Global<uint32_t, sym::g_screenFramesSinceMove> g_screenFramesSinceMove;
    // The frames a screen is up before it takes input, and whether only
    // the current player's pad moves its menu (else any player's).
    inline ee::Global<uint32_t, sym::g_screenInputDelay> g_screenInputDelay;
    inline ee::Global<uint32_t, sym::g_currentPlayerOnly> g_currentPlayerOnly;

    // The player whose pad the UI's modal screens answer (-1: any).
    inline ee::Global<int32_t, sym::g_uiModalPlayer> g_uiModalPlayer;
    constexpr int32_t kAnyPlayer = -1;

    // Multiplayer mode 1's type, as its game select picks it (0 .. 3:
    // "$KSTRMP_MODE01TYPENAME_TYPE%02d"); a new game of it takes it as its
    // single-player mode.
    inline ee::Global<int8_t, sym::g_mode1Type> g_mode1Type;
    constexpr int8_t kMode1Types = 4;

    // What is shown around the screens now, and what was asked for
    // (setScreenHeaders): kHeadersPlay and the others.
    inline ee::Global<uint8_t, sym::g_shownScreenHeaders> g_shownScreenHeaders;
    inline ee::Global<uint8_t, sym::g_wantedScreenHeaders> g_wantedScreenHeaders;

    // zgui's root node (Zgui +0x0C, as AI's g_uiRoot): the screens' widgets
    // are found below it and their scripts run on it.
    inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

    // The widget at `path` (DM2's findNode takes no kind: the shared
    // declaration's last parameter is passed as 0).
    inline ee::Ptr<zge::zgui::Node> uiWidget(ee::Ptr<const char> path)
    {
        return zge::zgui::findNode(*g_uiRoot, path, 0);
    }
    inline void runUiScript(ee::Ptr<char> text)
    {
        zge::zgui::runScriptLines(*g_uiRoot, text);
    }

    // The screens' task: the headers and the play overlay made what was
    // asked for, then the frame of the screen that is up, or the play
    // overlay's frames when none is.
    uint32_t gameScreensFrame();
    // The screen that is up starts a step: its frame counts and its menu's
    // choice start over.
    void startScreenStep(uint32_t step);

    // A widget's action `name` started or stopped (the root's PlayAction and
    // StopAction messages); nothing when the widget has no such action.
    uint32_t playWidgetAction(ee::Ptr<zge::zgui::Node> widget, ee::Ptr<const char> name);
    uint32_t stopWidgetAction(ee::Ptr<zge::zgui::Node> widget, ee::Ptr<const char> name);

    // A distance in metres written as feet and inches (5' 11"), rounded to
    // the inch; the text's length.
    int32_t formatFeetAndInches(ee::Ptr<char> out, ee::Float metres);
    // `text` copied to `out` in lines: once a line has `lineChars`
    // characters, its next space becomes a line break (the two characters
    // "\n" the UI's text takes). Text in braces counts for nothing and is
    // never broken. The number of breaks made.
    int32_t breakTextLines(ee::Ptr<char> out, ee::Ptr<const char> text, uint32_t lineChars);
    // `text` copied to `out` as one line: each line feed becomes a space and
    // carriage returns are left out.
    void joinTextLines(ee::Ptr<char> out, ee::Ptr<const char> text);

    // What the screens' widgets share: a frame's "Menu" child and its
    // variables, and the actions a frame plays as it opens and closes.
    constexpr ee::Ptr<const char> kMenuName{sym::g_textMenu};                   // "Menu"
    constexpr ee::Ptr<const char> kSelectedIndexName{sym::g_textSelectedIndex}; // "SelectedIndex"
    constexpr ee::Ptr<const char> kInitAction{sym::g_textInit};                 // "Init"
    constexpr ee::Ptr<const char> kDeInitAction{sym::g_textDeInit};             // "DeInit"

    // The pause screen's show handler and its menu's script function.
    void showPauseScreen(uint32_t shown);
    void bmxoverlayPauseHandler();
    // The instructions screen's show handler (the level's name and what
    // the game asks) and its menu's script function.
    void showInstructionsScreen(uint32_t shown);
    void bmxoverlayInstructionsHandler();
    // The options screen's show handler and its menu's script function.
    void showOptionsScreen(uint32_t shown);
    void bmxoverlayOptionsHandler();
    // The script functions of the trick list's and the objectives' menus:
    // each goes back to the screen it was asked from.
    void bmxoverlayTrickListHandler();
    // The trick list screens' show handler, and the tree view's script
    // functions: a category's moves listed, and the selected one's buttons.
    void showTrickListScreen(uint32_t shown);
    void bmxtrickSetUpdateCategory();
    void bmxtrickSetGetTrick();
    void bmxoverlayObjectivesHandler();

    // The exit confirmation's show handler, and its script function that
    // leaves the game.
    void showExitConfirmScreen(uint32_t shown);
    void bmxoverlayExitConfReturnToMenu();
    void bmxoverlayExitConfHandler();
    // What each line of the game over menu does (the show handler gives each
    // line one, in g_gameOverLines; the line's text is "$KSTR_GAMEOVER_..."
    // of the same name).
    enum GameOverLine : uint16_t
    {
        kGameOverPlayAgain = 0,   // PLAYAGAIN0, PLAYAGAIN1
        kGameOverLevelSelect = 1, // LEVELSELECT
        kGameOverBikeShop = 2,    // BIKESHOP, opening at the bike choice
        kGameOverRiderShop = 3,   // BIKESHOP, opening at the rider choice
        kGameOverSave = 4,        // SAVE
        kGameOverViewReplay = 5,  // VIEWREPLAY
        kGameOverTrickList = 6,   // TRICKLIST
        kGameOverRecords = 7,     // VIEWRECORDS
        kGameOverExit = 8,        // EXIT, TEST, GAMESELECT
        kGameOverNoLine = 9,      // given to no line; the menu plays again with it
        kGameOverSaveReplay = 10, // SAVEREPLAY
    };
    constexpr uint32_t kGameOverLineCount = 12;
    inline ee::Global<uint16_t[kGameOverLineCount], sym::g_gameOverLines> g_gameOverLines;
    void bmxoverlayGameOverHandler();
    void bmxoverlayRecordsHandler();
    // The reward screen's show handler and script function (screen 17).
    void showRewardScreen(uint32_t shown);
    void bmxoverlayRewardHandler();
    // The standings' texts the end and ready screens show: a series' (its
    // leader or a tie, the result once it is over, the rounds won) and
    // HORSE's (each player's letters, the turn's new one blinking red).
    void seriesStandingText(ee::Ptr<char> out);
    void horseLettersText(ee::Ptr<char> out);
    // A player's game score as the end screens write it, by the multiplayer
    // mode: a distance in feet and inches, tricks, degrees (half turns), or
    // points (screen_game_over.cpp).
    void formatModeScore(ee::Ptr<char> out, uint32_t gameScore);
    // The leaderboard's show handler (screen 8).
    void showLeaderboardScreen(uint32_t shown);
    // The round over screen's show handler and script function (screen 9).
    void showRoundOverScreen(uint32_t shown);
    void bmxoverlayRoundOverHandler();
    // The NPC screen's show handler and script function (screen 27).
    void showNpcScreen(uint32_t shown);
    void bmxoverlayNPCHandler();
    // The records screens' show handler (19 .. 22): the level's saved
    // records, kind by kind.
    void showRecordsScreen(uint32_t shown);
    void showHighScoreScreen(uint32_t shown);
    void bmxoverlayHighScoreHandler();
    void showObjectivesScreen(uint32_t shown);

    // The save screens' show handlers, and the callback of their UI frame's
    // closing.
    void showSaveGameScreen(uint32_t shown);
    void showSaveReplayScreen(uint32_t shown);
    void saveScreenClosed();
    uint32_t saveScreenInput();
}
