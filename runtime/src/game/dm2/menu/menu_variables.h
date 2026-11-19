#pragma once

// The front end's script variables (the menu set-up binds each to a zgui
// variable of the same name, as a Sint32): what the menus choose and what
// the game reads back. One global each, shared by the menu files.

#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/menu/menu_variables.h.h"

namespace game::dm2
{
    // "CurrentRider1", "CurrentRider2": the character each player picked.
    inline ee::Global<uint32_t[2], sym::g_currentRiders> g_currentRiders;
    // "CurrentBike1", "CurrentBike2" and "CurrentRiderOutfit1",
    // "CurrentRiderOutfit2": each player's bike and outfit.
    inline ee::Global<uint32_t[2], sym::g_currentBikes> g_currentBikes;
    inline ee::Global<uint32_t[2], sym::g_currentRiderOutfits> g_currentRiderOutfits;
    // "CurrentRiderCount": how many players the menus are set up for.
    inline ee::Global<uint32_t, sym::g_currentRiderCount> g_currentRiderCount;
    // "CurrentRideType": the single-player game picked for the level (a
    // SinglePlayerMode, game/round.h: BmxStartGame makes it the session's).
    inline ee::Global<uint32_t, sym::g_currentRideType> g_currentRideType;
    // "CurrentLevel": the level menu's entry; "CurrentGameLevel": the game's
    // level number for it (bmxmenuGameLevelUpdate).
    inline ee::Global<uint32_t, sym::g_currentLevel> g_currentLevel;
    inline ee::Global<uint32_t, sym::g_currentGameLevel> g_currentGameLevel;
    // The game level of each level menu entry (nine).
    inline ee::Global<uint8_t[9], sym::g_levelMenuGameLevels> g_levelMenuGameLevels;
    // "MovieID": the movie the front end's script asks for (bmxStartMovie).
    inline ee::Global<uint32_t, sym::g_movieId> g_movieId;
    // "MovieStatus": whether the front end's movie is playing
    // (bmxUpdateMovieStatus).
    inline ee::Global<uint32_t, sym::g_movieStatus> g_movieStatus;
    // "CurrentSFXVolume", "CurrentMusicVolume": the options menu's volumes
    // (bmxUpdateVolumeInfo moves them into the game's settings).
    inline ee::Global<uint32_t, sym::g_currentSfxVolume> g_currentSfxVolume;
    inline ee::Global<uint32_t, sym::g_currentMusicVolume> g_currentMusicVolume;
}
