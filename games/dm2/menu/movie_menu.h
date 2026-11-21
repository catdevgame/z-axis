#pragma once

// The front end's movies (movie_menu.cpp): the script functions that start,
// stop and report the menu background's movie.

#include <cstdint>

namespace game::dm2
{
    // "BmxUpdateMovieStatus": MovieStatus set to whether a movie is playing.
    void bmxUpdateMovieStatus();
    // "BmxStartMovie": the movie MovieID names started on the menus'
    // background, or a random still picture shown there.
    void bmxStartMovie();
    // "BmxStopMovie": the front end's movie stopped and closed, the
    // background back on the movie's material.
    void bmxStopMovie(uint32_t scriptWord);
}
