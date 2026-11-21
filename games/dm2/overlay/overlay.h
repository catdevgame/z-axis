#pragma once
// DM2's in-game overlay (overlay.c, the "InGameUI.Overlay" widgets): the
// frames the game code shows, hides and sends values to.
//
// The overlay keeps a table of eighteen frames, each with its handlers (an
// init, a per-frame update, a show/hide, a message); the game code names a
// frame by its index. The names are the widgets' ("InGameUI.Overlay.LabelScore"
// and the others each frame's handlers read); a frame with no widget string
// has no name here.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2::rider
{
    struct ScoreLine;
}

namespace game::dm2
{
    enum OverlayFrame : uint32_t
    {
        kOverlayScore = 1,        // LabelScore: the score, "%ld tricks", "%ld degrees"
        kOverlayTimer = 2,        // FrameTimer
        kOverlayTrick = 4,        // OverlayToggle.DisplayTrick: the trick's name and score
        kOverlayRuler = 5,        // no widget: a measure in feet on a ruler, with the record's mark
        kOverlayGauge = 6,        // no widget: a bar whose marker goes up and down with the value sent
        kOverlayFall = 7,         // LabelCrash: "$KSTR_FALL_%02d", the bail's kind
        kOverlayEmpty = 8,        // no widget: its handlers do nothing but keep the byte sent (the rider's health)
        kOverlayReplayMark = 9,   // no widget: a blinking ">" while the replay recorder records or plays
        kOverlayTip = 10,         // LabelTip: a "$KSTR_" tip
        kOverlayObjective = 11,   // LabelObj: a level goal, "$KSTRSP_GOAL_LEV%02d_%s_%d"
        kOverlayReward = 12,      // DisplayReward: a goal's reward
        kOverlayExitWarning = 13, // LabelExitWarn
        kOverlaySongInfo = 14,    // OverlayReplay.Demo.LabelSongInfo
        kOverlayFade = 17,        // no widget: the screen faded to or from a colour (ScreenFade)
    };

    // What the score frame takes: the score to show (4294967296: none, the
    // label hidden), and flags it keeps until its frame has seen them.
    struct OverlayScore
    {
        ee::Float score;
        uint8_t flags;
    };
    static_assert(offsetof(OverlayScore, flags) == 0x04);
    constexpr uint8_t kScoreAtOnce = 0x01; // shown at once, where the label would count up to it (a turn's first 0)

    // What the reward display takes: what to show (a reward kind) and its
    // number (the points, the distance, the count or the bail's kind).
    // `state` is the display's own: it is written in the display's copy
    // (kRewardWaiting, then the item's "State" variable), never read from
    // what is sent. A first byte of kOverlayNone clears the display.
    struct OverlayReward
    {
        uint8_t kind;
        uint8_t pad01; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
        uint16_t state;
        ee::Float value;
    };
    static_assert(offsetof(OverlayReward, state) == 0x02 && offsetof(OverlayReward, value) == 0x04 &&
                  sizeof(OverlayReward) == 0x08);
    // The reward kinds the scoring sends ("$KSTR_REWARD%02d" names each).
    enum RewardKind : uint8_t
    {
        kRewardGapFound = 11,          // a gap found, with how many of the level's are found now
        kRewardAllGapsFound = 12,      // the level's last gap found
        kRewardRunTricks = 13,         // a run landed, with its tricks counted
        kRewardFall = 14,              // a bail: its kind's text ("$KSTR_FALL_%02d")
        // A saved record beaten, by record:
        kRewardTurnScore = 17,
        kRewardLinePoints = 18,
        kRewardRunPoints = 19,
        kRewardRunTricksRecord = 20,
        kRewardSpin = 21,
        kRewardGrindDistance = 22,
        kRewardWheelieDistance = 23,
        kRewardNoseWheelieDistance = 24,
        kRewardSkid = 25,
        kRewardHold = 26,
        kRewardAirHeight = 27,
        kRewardContactDistance = 28,
    };

    // What the tip's and the objective's frames keep, and copy out as
    // their state (overlayFrameState): what was sent (kOverlayNone: nothing;
    // the low seven bits are the tip or the goal) and whether the label is
    // visible. What is sent them is the same record.
    struct LabelState
    {
        uint8_t value;
        uint8_t pad01; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
        uint16_t visible;
    };
    static_assert(sizeof(LabelState) == 4 && offsetof(LabelState, visible) == 2);

    // What the ruler takes: a measure in metres (0: none, the ruler hidden)
    // and the kind of game record to mark on it (a record key's kind).
    // `feet` is the ruler's own: the measure in feet, written in its copy.
    struct OverlayRuler
    {
        ee::Float metres;
        ee::Float feet;
        uint16_t pad08; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
        uint16_t recordKind;
    };
    static_assert(offsetof(OverlayRuler, recordKind) == 0x0A && sizeof(OverlayRuler) == 0x0C);

    // What the trick display takes: what to do (kTrickDisplayClear and five
    // more: a trick begun, its text, its end) and the turn it shows (the
    // address of the rider's score block, Rider::score).
    struct OverlayTrick
    {
        uint32_t action;
        uint32_t turn;
    };
    enum TrickDisplayAction : uint32_t
    {
        kTrickDisplayClear = 0,       // the display starts over, its name labels hidden
        kTrickDisplayTrickBegun = 1,  // a trick of the turn's current line began: the next name label fades in
        kTrickDisplayTrickText = 2,   // the line's text and points written
        kTrickDisplayNameFadeOut = 3, // the current name label fades out
        kTrickDisplayScoreFadeOut = 4, // the points fade out
        kTrickDisplayCrash = 5,       // the name and the points play "Crash"
    };

    // What the fade takes: the colour, and the frames it takes to reach it
    // (kFadeFromColour: to leave it).
    struct ScreenFade
    {
        uint8_t flags;
        uint8_t red, green, blue;
        uint16_t frames;
    };
    static_assert(offsetof(ScreenFade, frames) == 0x04);
    constexpr uint8_t kFadeFromColour = 0x01;

    // The game's screens (showGameScreen), by the "InGameUI" frame each
    // shows; the ones the round's code asks for.
    enum GameScreen : uint32_t
    {
        kScreenNone = 0,              // play
        kScreenPause = 1,             // FramePause
        kScreenOptions = 2,           // FrameOptions
        kScreenTrickList = 3,         // FrameTrickList, asked for from the pause menu
        kScreenRoundOverTrickList = 4, // FrameTrickList, to go back to the round over screen
        kScreenGameOverTrickList = 5, // FrameTrickList, to go back to the game over screen
        kScreenReadyTrickList = 6,    // FrameTrickList, to go back to the multiplayer ready screen
        kScreenExitConfirm = 7,       // FrameExitConf
        kScreenLeaderboard = 8,       // FrameLeaderboard
        kScreenRoundOver = 9,         // FrameRoundOver
        kScreenGameOver = 10,         // FrameGameOver
        kScreenSaveGame = 11,         // the memory card UI frame, saving the game (from the game over screen)
        kScreenSaveReplay = 12,       // the memory card UI frame, saving the shown replay
        kScreenInstructions = 13,     // FrameInstructions
        kScreenObjectives = 14,       // FrameObjectives, before the turn: going on starts it
        kScreenPauseObjectives = 15,  // FrameObjectives, asked for from the pause menu
        kScreenEndObjectives = 16,    // FrameObjectives, to go on to the game over screen
        kScreenReward = 17,           // FrameReward
        kScreenHighScore = 18,        // CommonUI.KeyboardFrame, "$KSTR_HIGHSCORE_%02d": a record's name entered
        kScreenRecords = 19,          // FrameRecords, asked for from the pause menu
        kScreenTurnRecords = 20,      // FrameRecords, at a turn's end: going on moves the game on
        kScreenRoundOverRecords = 21, // FrameRecords, to go back to the round over screen
        kScreenGameOverRecords = 22,  // FrameRecords, to go back to the game over screen
        kScreenMultiplayerGameSelect = 23, // FrameMPGameSelect
        kScreenHorseWord = 24,        // CommonUI.KeyboardFrame, "$KSTRMP_MPHORSEWORD_SELECTWORD": the HORSE word entered
        kScreenMultiplayerReady = 25, // FrameMPReady
        kScreenMultiplayerTimeUp = 26, // FrameMPTimeUp
        kScreenNpc = 27,              // FrameNPC
        kScreenFeature = 28,          // FrameFeature
        kScreenLast = 29,             // (asked for only) the last screen shown, again
    };
    // A screen shown in place of the one that is up; kScreenNone closes it
    // and play goes on.
    void showGameScreen(uint32_t screen);

    // What is asked for around a screen (setScreenHeaders; the screens' frame
    // makes it so): the play overlay, the top and bottom headers dropped down
    // for a screen, or neither.
    constexpr uint32_t kHeadersPlay = 0;
    constexpr uint32_t kHeadersDown = 1;
    constexpr uint32_t kHeadersNone = 2;
    void setScreenHeaders(uint32_t headers);
    // The screen shown; under the exit confirmation, the screen it was asked over.
    uint8_t gameScreen();
    // The screens' input, each frame of the action's screen mode: once the
    // screen has been up its input delay, its own input runs (what it
    // answers, when not 0, is answered), then the players' pads (only the
    // current player's when the screen says so) move its menu's choice down
    // and up, round its choices. Answers 1 when the choice moved, 0 when
    // nothing was taken (counting the screen's frames on).
    uint32_t gameScreenFrame();

    // No value for a frame that takes a byte (hides what it shows).
    constexpr uint8_t kOverlayNone = 0xFF;

    // A frame of the table: whether it is hidden, and its handlers (0: none).
    struct OverlayFrameRecord
    {
        uint32_t flags;                                // +0x00 kOverlayFrameHidden
        ee::FunctionPtr<void(uint32_t)> setUp;         // +0x04 (1) as the overlay starts, (0) as it ends
        ee::FunctionPtr<void()> frame;                 // +0x08 every frame while shown
        ee::FunctionPtr<void(uint32_t)> show;          // +0x0C (shown) as it is shown or hidden
        ee::FunctionPtr<void(ee::Address)> message;    // +0x10 a value for it (overlayMessage)
        ee::FunctionPtr<void(ee::Address)> state;      // +0x14 its state copied out (overlayFrameState)
        ee::FunctionPtr<void(ee::Address)> shownMessage; // +0x18 a value for it while it is shown (overlayShownMessage;
                                                       //       only the trick display has one, and it is empty)
    };
    static_assert(sizeof(OverlayFrameRecord) == 0x1C && offsetof(OverlayFrameRecord, message) == 0x10);
    constexpr uint32_t kOverlayFrames = 18;
    constexpr uint32_t kOverlayFrameHidden = 0x1;

    // The overlay starts: every frame told, and hidden.
    void openOverlayFrames();
    // Every frame told the overlay ends; every shown frame's frame.
    void closeOverlayFrames();
    void updateOverlayFrames();
    // A frame shown or hidden (its handler told only on a change).
    void showOverlayFrame(uint32_t frame, uint32_t shown);
    // A value sent to a frame's message handler (what each frame takes is its
    // own: a byte, a word, a byte and a float).
    void overlayMessage(uint32_t frame, ee::Address message);
    // A value sent to a frame's second message handler, only while the frame is shown.
    void overlayShownMessage(uint32_t frame, ee::Address message);
    // A frame's state copied to `out` (the tip's and the objective's: four
    // bytes, what they show and whether they are visible).
    void overlayFrameState(uint32_t frame, ee::Address out);

    // The overlay's start with the level (overlay_start.cpp): the loading
    // screen stopped, the in-game UI's script loaded, the overlay items, the
    // screens and the frames set up, the screens' script functions and
    // variables given to zgui, and the screens' task made; the load's
    // progress hook told of each of its four steps.
    void initOverlay();
    // Its end: the task deleted, the open screen told it closes, the
    // screens' items, the frames and the overlay items given up, the in-game
    // UI deleted and zgui's heap tidied (what dmmCompact answers).
    uint32_t shutdownOverlay();

    // A scored line written into `text` (trick_text.cpp): its landing and
    // spin, then each trick with its modifier ("to" between them), the
    // pairs with names of their own ("flair", "truck driver", ...) in
    // their place, and its gap. With `takeoffWords` the take-off's
    // "half-cab", "opposite" and "alley-oop" are written too (the trick
    // display's text has them; the HORSE screen's does not).
    void dm2TrickText(ee::Ptr<char> text, const rider::ScoreLine *line, uint32_t takeoffWords);
}
