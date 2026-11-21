#pragma once
// BMX XXX's game states (bmx.c, game_state.cpp): the state the game is in and
// the machine that moves it from one to the next. BMX XXX's own code, DM2's
// line (games/dm2/action: setGameState, gameState, playFrame) with a state's
// loading made a step of its own.
//
// A state is four functions, each in a table by the state's number: its
// enter, its loading step (run each frame until it says it is done), its
// frame and its exit. The states' numbers are named from what their
// functions load and from motview.c's source path; state 3's is not
// settled.

#include "ee/ee.h"

#include <cstdint>
#include "sym/bmx/states/game_state.h.h"

namespace game::bmx
{
    constexpr uint32_t kGameStates = 8;
    constexpr uint32_t kGameStateFrontEnd = 0;     // the menus ("UI_menu"); the intro ends in it
    constexpr uint32_t kGameStateLevel = 1;        // the level being ridden (action.c's frame)
    constexpr uint32_t kGameStateParkEditor = 2;   // "ParkEditor"
    // A park from the card ridden: the park editor's enter and loading,
    // with its saved-park flag set (g_playingSavedPark; the front end's park
    // load leaves for it when the park was loaded to ride).
    constexpr uint32_t kGameStateRidePark = 3;
    constexpr uint32_t kGameStateMovie = 4;        // a movie from assets\movies
    constexpr uint32_t kGameStateIntro = 5;        // "UI_intro"
    constexpr uint32_t kGameStateAvMenu = 6;       // "UI_AVMenu"
    constexpr uint32_t kGameStateMotionViewer = 7; // motview.c
    // No state: before the first is entered, and once a state's loading has
    // asked the game to quit.
    constexpr uint8_t kNoGameState = 0xFF;

    // The game state to enter: the game's frame leaves the state it is in and
    // enters this one over its next four frames. Answers the state (as a byte).
    uint8_t setGameState(uint32_t state);
    // The game state set last (the one entered, once the change has run).
    uint8_t gameState();

    // The game's frame (the engine's frame callback runs it after the pads
    // are read): the frame rate measured, the debug displays set, the game
    // state's step, the sound's frame.
    void playFrame();

    // The dark panel behind the profiler's bars: a task of the game's, drawn
    // only while the debug display is the profiler's (2).
    int32_t drawProfilerBackdrop();
    // The loading step of a state that loads in steps (the front end's, the
    // level's): setting one begins it (its frames and the loading bar's
    // counts cleared).
    void setLoadStep(uint32_t step);
    uint32_t loadStep();
    // The loading bars (loading_bar.cpp): the chunk reader's progress hook
    // while the level loads, and while the front end does.
    void levelLoadingBar(const char *name, int32_t bytesDone, uint32_t totalBytes, uint32_t context);
    void frontEndLoadingBar(const char *name, int32_t bytesDone, uint32_t totalBytes, uint32_t context);
    // The frames the loading step has had (the loading counts them).
    inline ee::Global<uint32_t, sym::g_loadStepFrames> g_loadStepFrames;
}
