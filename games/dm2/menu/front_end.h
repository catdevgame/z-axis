#pragma once

// The front end (front_end.cpp): the game state the menus run in, its
// enter, frame and exit.

#include <cstdint>

namespace game::dm2
{
    // The menu scene entered and its camera made, once (the front end's and
    // the intro's enter): the camera at the origin, looking down +Z.
    void enterMenuScene();
    // The front end's enter (the game state's enter table): the menus'
    // scripts loaded, the menu scene entered, the sound in its front end
    // mode, and the menus opened where the game left for them: the main
    // menu's intro, then the main menu or, coming back from a game, the bike,
    // rider or level choice with the menus' variables put back from the
    // session; a watched replay's end opens the Cool Stuff frame. Answers 1.
    uint32_t frontEndEnter();
    // Where the front end opens (the pause menu's ways out of a game set it).
    enum FrontEndOpens : uint8_t
    {
        kOpensMainMenu = 0,
        kOpensBikeChoice = 1,
        kOpensRiderChoice = 2,
        kOpensLevelChoice = 3,
    };
    // The front end's exit (the game state's exit table): the movie closed,
    // the ParkMenu and MainMenu nodes deleted, the scene left; the main
    // menu's script and the menu scene are to be made again.
    void frontEndExit();
    // "BmxStartGame": the menus' choices made the session's (the game
    // picked, each player's rider, bike and outfit, the player count, the
    // level), the menu mode back to the front end's, the level state asked
    // for and the load screen's progress started from nothing. A replay to
    // watch (menu mode kMenuViewReplay) keeps its own set-up.
    uint32_t bmxStartGame();
    // "bmxmenuPlayPark": the same start for the park editor's park (the
    // park level), with the game's single-player mode set at once too.
    uint32_t bmxmenuPlayPark();
    // The front end's frame (the game state's frame table): the pads read,
    // the main menu's multiplayer entry opened while both pads are connected
    // and locked while one is not, and the memory card frame run under the
    // menu modes that use it. Answers 1.
    uint32_t frontEndFrame();
}
