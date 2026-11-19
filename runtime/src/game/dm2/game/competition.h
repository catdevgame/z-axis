#pragma once
// DM2's competitions (game.c): a level's competition (single-player mode
// kCompetition) is two runs against nine rivals, each run scored by five
// judges; the field, the scores and the ranking the leaderboard shows.

#include "players.h"

#include "dm2/rider/rider.h"
#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/game/competition.h.h"

namespace game::dm2
{
    // A competition's field: ten rider characters, the player's own first,
    // the rivals drawn from the first fourteen (startGame).
    constexpr uint32_t kCompetitionRiders = 10;
    inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRiders> g_competitionRiders;

    // The competitors' scores by character: each run's and their average
    // (judgeScore), as a judge's byte (255 the most).
    struct CompetitorScores
    {
        uint8_t runs[rider::kCompetitionRuns];
        uint8_t average;
    };
    static_assert(sizeof(CompetitorScores) == 3);
    inline ee::Global<CompetitorScores[kCharacters], sym::g_competitionScores> g_competitionScores;
    // The field ranked by average, highest first (judgeScore).
    inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRanking> g_competitionRanking;

    // A competition's run judged, as a single-player turn's time is up
    // (setRoundPhase): the five judges score the player's run (Rider's
    // competitionRuns for the round) from its points against the level's
    // and the variety of its tricks; each rival's run is drawn at random
    // about its character's own bias; then every competitor's average so far
    // and the field's ranking. Nothing in a multiplayer game or another mode.
    void judgeScore(uint32_t player);
}
