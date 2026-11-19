#pragma once
// The movie the menus play on the UI's "Common.movie" material (the intro's
// movie, the front end's): BMX XXX's own code (its menu block).

#include "ee/ee.h"
#include "zge/zgui/widget.h"

#include <cstdint>

namespace game::bmx
{
    // A movie file opened and played on the movie material, in place of any
    // movie open. 0 when there is no such file, else 1 (also when the movie
    // could not be opened).
    int32_t playMenuMovie(ee::Address path, uint32_t loop);
    // The movie's frame while its menu has the screen: at the menu's second
    // frame the node is made visible; from the third on, 0 once a button is
    // pressed or the movie is over, else 1.
    int32_t menuMovieFrame(ee::Ptr<zge::zgui::Node> node);
    // The movie stopped and closed, and its picture taken off the movie
    // material.
    void stopMenuMovie();
}
