#pragma once

// BMX XXX's game screens (./src/com/action/overlay.c): the "UI_overlay"
// frames put up over a level (the pause menu, the instructions, the
// objectives, the exit confirmation ...) and the table of them. DM2's game
// screens (game/dm2/overlay/screens.h), run by BMX XXX's menu machine.
//
// A screen is a menu of the machine (bmx.c): showGameScreen asks the machine
// for the screen's record, the machine takes the old one out and brings the
// new one in, and tells each screen's handler what happens to it (the
// messages below). The frames' scripts send the rest: every frame's "Init"
// action ends with "came in", its "Shutdown" with "left", and a frame's menu
// sends what was chosen. The screens' names are the frames each handler
// looks up ("FramePause", "FrameExitConf" ...).

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/overlay/screens.h.h"

namespace game::bmx
{
    enum GameScreen : uint32_t
    {
        kScreenNone = 0,           // FrameNone: as the overlay opens
        kScreenPlay = 1,           // FrameNone: play goes on once it has come in (g_resumeAction first)
        kScreenReplay = 2,         // FrameNone: a replay is shown
        kScreenReplayDemo = 3,     // FrameNone: the demo's replay is shown
        kScreenLeave = 4,          // the screen fades out, then the front end (the park editor after a park's test)
        kScreenLeaveToMovie = 5,   // the same, then the movie state
        kScreenLeaveToEditor = 6,  // the same, then the park editor
        kScreenPause = 7,          // FramePause
        kScreenPauseNoChoice = 8,  // FramePause: its first line is taken whatever is selected
        kScreenPlayAgain = 9,      // FramePause with "play again" in place of "continue", the game camera circling
        kScreenOptions = 10,       // FrameOptions
        kScreenBikeParts = 11,     // FrameBikeParts, asked for from the pause menu
        kScreenGameOverBikeParts = 12, // FrameBikeParts, to go back to the game over screen
        kScreenTrickList = 13,     // FrameTrickList, asked for from the pause menu
        kScreenRoundOverTrickList = 14, // FrameTrickList, to go back to the round over screen
        kScreenGameOverTrickList = 15,  // FrameTrickList, to go back to the game over screen
        kScreenNextPlayerTrickList = 16, // FrameTrickList, to go back to game.c's screen before a multiplayer turn
        kScreenExitGame = 17,      // FrameExitConf: the game being played ends
        kScreenExitLevel = 18,     // FrameExitConf: the level is left
        kScreenMovieConfirm = 19,  // the movie's confirmation ("$KSTR_MOVIECONF_...")
        kScreenLeaderboard = 20,   // FrameLeaderboard
        kScreenRoundOver = 21,     // FrameRoundOver
        kScreenGameOver = 22,      // "$KSTR_GAMEOVER_..."
        kScreenSaveGame = 23,      // the save system's screens, saving the game
        kScreenSaveReplay = 24,    // the same, for the shown replay
        kScreenInstructions = 25,  // FrameInstructions
        kScreenObjectives = 26,    // FrameObjectives, before the turn
        kScreenPauseObjectives = 27, // FrameObjectives, asked for from the pause menu
        kScreenEndObjectives = 28,   // FrameObjectives, at the game's end: going on shows the game over screen
        kScreenObjectiveDetail = 29, // FrameObjectiveDetail
        kScreenPauseObjectiveDetail = 30, // FrameObjectiveDetail, asked for from the pause menu's objectives
        kScreenReward = 31,        // FrameReward
        kScreenHighScore = 32,     // the keyboard frame: a record's name entered
        kScreenRecords = 33,       // FrameRecords, asked for from the pause menu
        kScreenNewRecords = 34,    // FrameRecords, after a turn that beat some: going on is the mode's next screen
        kScreenRoundOverRecords = 35, // FrameRecords, asked for from the round over screen
        kScreenGameOverRecords = 36, // FrameRecords, to go back to the game over screen
        kScreenAfterGame = 37,     // game.c's: after a multiplayer game
        kScreenBeforeGame = 38,    // game.c's: before the first mode's turn
        kScreenNextPlayer = 39,    // game.c's: before a multiplayer turn
        kScreenAfterTurn = 40,     // game.c's: after a turn
        kScreenNpr = 41,           // FrameNPR: a rider met in the level
        kScreenFeature = 42,       // "$KSTR_FEATURE_%02d"
        kGameScreens = 43,
        kScreenLast = 44,          // (asked for only) the last screen of 7 and up shown, again
    };

    // The messages a screen's handler gets. The machine's own are the four
    // of a change and the pad's two buttons; a frame's script sends "came
    // in" and "left" as its actions end, and its own as its menu is used
    // (most frames: the selected line was chosen, the menu was cancelled).
    enum ScreenMessage : int32_t
    {
        kScreenCancelled = -1, // a frame's menu: "OnCancel"
        kScreenChosen = 0,     // a frame's menu: "OnItemSelect"
        kScreenSelectionChanged = 1, // a frame's menu: "OnChange" plays its "Update" script (FrameMPGameSelect)
        kScreenButton6 = 4,    // the machine: the pad's button bit 6 went down
        kScreenButton14 = 5,   // the machine: the pad's button bit 14 went down
        kScreenEnter = 10,     // the machine: the screen has the screen
        kScreenCameIn = 11,    // "Init" ended
        kScreenLeaveNow = 12,  // the machine: the screen is being taken out
        kScreenLeft = 13,      // "Shutdown" ended
    };

    // A screen of the table: its handler, and what it runs each frame.
    struct GameScreenRecord
    {
        ee::FunctionPtr<void(int32_t message, uint32_t data)> handler; // +0x00 the machine's handler
        ee::FunctionPtr<void()> input; // +0x04 run before the frame while it is up (no screen of the table has one)
        ee::FunctionPtr<void()> frame; // +0x08 its own frame (no screen of the table has one)
    };
    static_assert(sizeof(GameScreenRecord) == 0x0C);
    // The table. Indexed through the address: the machine's "no menu" (-1)
    // reads the record before it, as the original does.
    inline ee::Ptr<GameScreenRecord> gameScreenRecord(int32_t screen)
    {
        return ee::Ptr<GameScreenRecord>(sym::g_gameScreens) + screen;
    }

    // The "UI_overlay" tree the screens' frames are in.
    inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_overlayUi> g_overlayUi;
    // The screen asked for (a GameScreen; all ones as the overlay opens), the
    // last one of kScreenPause and up asked for, and the screen an exit
    // confirmation was asked over.
    inline ee::Global<uint32_t, sym::g_gameScreen> g_gameScreen;
    inline ee::Global<uint32_t, sym::g_lastGameScreen> g_lastGameScreen;
    inline ee::Global<uint32_t, sym::g_screenBehindExitConfirm> g_screenBehindExitConfirm;
    // Set by every change of screen (game.c's screens and the game over
    // screen clear it).
    inline ee::Global<uint32_t, sym::g_gameScreenChanged> g_gameScreenChanged;

    // The step the screen that is up has reached, each screen's own count
    // (the reward screen's page, the NPR screen's).
    inline ee::Global<uint32_t, sym::g_screenStep> g_screenStep;

    // What kScreenPlay does once it has come in, before play goes on.
    enum ResumeAction : uint32_t
    {
        kResumePlay = 0,       // nothing more
        kResumeRespawn = 1,    // the menus' player's rider and physics reset, their camera back to its first mode
        kResumeTurnRunning = 2, // the round's phase set to the running turn
        kResumeNewGame = 3,    // the game mode started again, the round's phase set to a turn's start
        kResumeTurnStart = 4,  // the round's phase set to a turn's start
    };
    inline ee::Global<uint32_t, sym::g_resumeAction> g_resumeAction;

    // What each line of the menu a screen made does (the pause menu's
    // lines, the movie confirmation's two): the screen's own numbers.
    constexpr uint32_t kMenuChoices = 12;
    inline ee::Global<uint8_t[kMenuChoices], sym::g_menuChoices> g_menuChoices;

    // The menu the front end opens with when the level is left for it: the
    // pause menu's "change rider" and "level select" name theirs.
    inline ee::Global<uint8_t, sym::g_frontEndFirstMenu> g_frontEndFirstMenu;
    constexpr uint8_t kFrontEndAfterRound = 4; // (the round over screen's exit, and the front end's own start)
    constexpr uint8_t kFrontEndChangeRider = 7;
    constexpr uint8_t kFrontEndLevelSelect = 11;

    // What the frames and their menus share: a frame's "Menu" child and its
    // variables, and the actions a frame plays as it comes in and leaves.
    constexpr uint32_t kMenuName = sym::g_textMenu;                   // "Menu"
    constexpr uint32_t kSelectedIndexName = sym::g_textSelectedIndex; // "SelectedIndex"
    constexpr uint32_t kItemsName = sym::g_textItems;                 // "Items"
    constexpr uint32_t kVisibleCountName = sym::g_textVisibleCount;   // "VisibleCount"
    constexpr uint32_t kTextName = sym::g_textText;                   // "Text"
    constexpr uint32_t kInitAction = sym::g_textInit;                 // "Init"
    constexpr uint32_t kShutdownAction = sym::g_textShutdown;         // "Shutdown"
    // A menu's lines are one text: "%s%s" adds a line after a separator, a
    // line break ("\n" as the string tables write one) or nothing for the
    // first.
    constexpr uint32_t kSeparatorAndLine = sym::bmx::g_textSeparatorAndLine; // "%s%s"
    constexpr uint32_t kLineBreak = sym::g_textLineBreak;                    // "\n"
    constexpr uint32_t kNoSeparator = sym::g_textNoSeparator;                // ""
    // The two frames below a screen: one of them is shown with it.
    constexpr uint32_t kFrameContinue = sym::g_textFrameContinue; // "FrameContinue"
    constexpr uint32_t kFrameGoBack = sym::g_textFrameGoBack;     // "FrameGoBack"
    constexpr uint32_t kVisibleName = sym::g_textVisible;         // "Visible"

    // A competition's judged score is a byte, 255 for 100 points, and is
    // written to a tenth of a point ("%02d.%d").
    struct JudgedPoints
    {
        int32_t whole, tenths;
    };
    inline JudgedPoints judgedPoints(uint8_t score)
    {
        const ee::Float points = ee::toFloat(score) * ee::Float::fromBits(0x3EC8C8CAu); // 100 / 255
        const ee::Float whole = ee::toFloat(ee::toInt(points));
        return {ee::toInt(whole), ee::toInt((points - whole) * ee::Float(10.0f))};
    }

    // (displays.cpp; its header does not declare it yet) The displays'
    // widgets loaded and every display set up.
    void openOverlayDisplays();

    // (screens.cpp)
    // The overlay opened for a level: the level's strings, the "UI_overlay"
    // tree, the displays, the trick list's script functions and the screens'
    // task. No screen is up after.
    void openOverlay();
    // The overlay closed: the screen that is up told to leave, the task, the
    // displays, the tree and the level's strings freed.
    void closeOverlay();
    // The screens' input, every frame before the game's: a pad that is out
    // asked for over the screens that can wait for it, then the input of the
    // screen that is up. Always 0.
    uint32_t gameScreensInput();
    // The screens' task: the displays' frames while play is on screen, or
    // the frame of the screen that is up. Always 1.
    uint32_t gameScreensFrame();
    // The screen asked for, or the one an exit confirmation is over.
    uint32_t gameScreen();
    // A screen asked for in place of the one that is up.
    void showGameScreen(uint32_t screen);

    // The screens' handlers, by frame.
    void noneScreenMessage(int32_t message);
    void leaveScreenMessage(int32_t message);
    void pauseScreenMessage(int32_t message);
    void instructionsScreenMessage(int32_t message);
    void exitConfirmScreenMessage(int32_t message);
    void objectivesScreenMessage(int32_t message);
    void objectiveDetailScreenMessage(int32_t message);
    void recordsScreenMessage(int32_t message);
    void trickListScreenMessage(int32_t message);
    void bmxoverlayTrickListUIUpdateTrick();    // the trick list's script functions
    void bmxoverlayTrickListUIUpdateCategory();
    void saveGameScreenMessage(int32_t message);
    void saveReplayScreenMessage(int32_t message);
    void optionsScreenMessage(int32_t message, uint32_t data);
    void rewardScreenMessage(int32_t message);
    void highScoreScreenMessage(int32_t message);
    void nprScreenMessage(int32_t message);
    void gameOverScreenMessage(int32_t message);
    void bikePartsScreenMessage(int32_t message);
    void movieConfirmScreenMessage(int32_t message);
    void featureScreenMessage(int32_t message, uint32_t data);
    void roundOverScreenMessage(int32_t message);
    void leaderboardScreenMessage(int32_t message);
}
