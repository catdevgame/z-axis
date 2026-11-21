#pragma once
// BMX XXX's trick text (bmx.c; trick_text.cpp): a scored line written out as
// the trick display and the multiplayer screens show it ("perfect 360
// opposite tailwhip to x-up barspin"). DM2's dm2TrickText (overlay.c) is
// the same code but for two things: BMX XXX writes only a line's last six
// tricks, and its lines have no gap's name after them.

#include "bmx/rider/rider.h"
#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The line's text into `text`: how it landed, its spin and each trick
    // with its modifier and loops, a pair with a name of its own named so.
    // With `takeoffWords` the take-off's words are in it too ("half-cab",
    // "opposite", "alley-oop").
    void trickText(ee::Ptr<char> text, const rider::ScoreLine *line, uint32_t takeoffWords);
}
