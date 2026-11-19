#pragma once
// BMX XXX's movie state (bmx.c, movie_state.cpp): a movie from
// assets\movies played over "UI_Common.Movie" (a reward's movie, after the
// level's screen fades out), then the level state again. BMX XXX's own code.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The state's enter: the movie of g_rewardKind16Value's row opened, its
    // picture put on the "Common.movie" material, and played. 1, or 0 when
    // the movie does not open.
    int32_t movieEnter();
    // The state's exit: the movie's frame hidden, the movie stopped, its
    // picture taken off the material, and closed.
    void movieExit();
    // The state's frame: from its third frame the movie's frame shown; the
    // level state again once the movie ends or either pad presses cross or
    // start (at once with no movie open). Always 1.
    int32_t movieFrame();
}
