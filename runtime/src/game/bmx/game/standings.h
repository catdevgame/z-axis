#pragma once
// The game over screen's standings (game.c, standings.cpp): each player's
// line of the game's result, as text for the screen. BMX XXX's own code.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // HORSE's standings: a line a player, "PLAYER NUMBER n:" and the letters
    // of the word the player has, the newest in red when the turn scored
    // nothing (the letter just taken).
    void wordGameStandingText(ee::Ptr<char> out);
    // A game of rounds' standings: the leader ("PLAYER NUMBER n") and the
    // series' result ($KSTR_SERIES_WIN_<n>; a tie: $KSTR_SERIES_TIE), then
    // the rounds won: "a-b" for two players, else each player's, by "-".
    void seriesStandingText(ee::Ptr<char> out);
}
