#pragma once

// The action (action.c): the in-level game's modes and frames: the level's
// start and end, the per-mode frame functions, the game's set-up.

#include <cstdint>

namespace game::dm2
{
    // The action mode (actionStart's argument): which per-mode frame
    // function runs; 0 is play.
    uint8_t actionMode();

    // The action leaves its mode and starts `mode` (an ActionMode,
    // action_state.h). Returns the mode.
    uint8_t actionStart(uint32_t mode);
    // Play's frame, and the frame while a game screen is up (the action
    // modes' frame functions). 1 when the mode changed under the frame.
    uint32_t actionFrame();
    uint32_t screenModeFrame();

    // The level state entered: the level loaded and set up, its riders,
    // overlay, camera and game made, and the action started. 1, or 0 when
    // the level could not be set up. And left: all of that taken down.
    // Answers the sound events' setGameMode(0).
    uint32_t levelStateEnter();
    int32_t levelStateExit();
    // The level's objects put back as the level's enter saved them (a
    // player's spawn; nothing when the level saved none).
    void restoreSavedLevelObjects();

    // The level's start: the ground looked up below every "Player" spawn
    // point (the answers are not kept).
    void probeSpawnGround();

    // The level state's frame (the game state's frame table): the pads read
    // and the action mode's frame function run. Always 0.
    uint32_t levelStateFrame();

    // The game's states (the enter, exit and frame tables the play frame
    // runs): the front end, the level, the park editor, the intro.
    constexpr uint32_t kGameStateFrontEnd = 0, kGameStateLevel = 1, kGameStateEditor = 2, kGameStateIntro = 3;
    // The game state to enter: the play frame leaves the state it is in and
    // enters this one on its next run. Answers the state (as a byte).
    uint8_t setGameState(uint32_t state);
    // The game state set last (the one entered, once the play frame has run).
    uint8_t gameState();
    // The game's end: the state left, the UI, the sound, the pads and the
    // riders' data files freed.
    void shutdownGame();
    // The game's frame: the debug stats line, the game state's frame (or a
    // pending change of state), then the music.
    void playFrame();
}
