#pragma once

// The engine's movies (zmovie: zge/movie/movie_player.cpp), what a game
// calls to play one: the player made ready once (movieInit), a movie opened
// from its path, played into a texture (looping or not) while the game draws
// that texture, asked whether it still plays, stopped (the end callback told
// how it ended) and closed. Each call does nothing (0) before movieInit.
//
// Behind it is the platform's player: on the PS2 Sony's MPEG stream sample
// driven by the engine (zge/movie/ps2/ps2_movie.h), whose stream the host
// decodes itself unless PS2X_MOVIES=hle (zge/host/movie_stream.cpp).

#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    // (movie_player.cpp)
    uint32_t setMovieEndCallback(uint32_t frontEndMovie, uint32_t movieEndCallback2, uint32_t movieEndCallbackArgument);
    int32_t movieInit();
    uint32_t movieShutdown();
    uint32_t movieOpen(uint32_t path);
    uint32_t moviePlay(uint32_t movie, uint32_t loop);
    uint32_t movieIsPlaying();
    uint32_t movieClose(uint32_t handedOn);
    uint32_t movieStop(uint32_t handedOn);
    ee::Ptr<uint32_t> movieTexture(uint32_t movie, ee::Ptr<uint32_t> ready);
    uint32_t moviePlayerTexture(); // ZGE2 only: the player's texture (a Texture), 0 before movieInit
}
