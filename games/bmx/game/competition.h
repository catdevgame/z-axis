#pragma once
// BMX XXX's competitions (game.c, competition.cpp): a run judged by five
// judges, the field of ten riders' marks, and the leaderboard's order. BMX
// XXX's own code (DM2's judgeScore is the same line).

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A competition's field: ten rider characters (the player's own among
    // them), and the order they stand in.
    constexpr uint32_t kCompetitionRiders = 10;

    // A competition's marks by rider character: each run's, and their
    // average (a byte each, 255 the best).
    struct CompetitionScores
    {
        uint8_t run[2];
        uint8_t average;
    };
    static_assert(sizeof(CompetitionScores) == 3);

    // The player's competition run judged (a competition only): the five
    // judges mark it from its score against the level's top score, the
    // tricks and modifiers used, the level's cells ridden and the trick
    // counts (less the bails), each with a little chance; the field's other
    // riders get a mark of chance and their own bonus, every rider's average
    // over the runs so far is taken and the field is put in order.
    void judgeScore(uint32_t player);
}
