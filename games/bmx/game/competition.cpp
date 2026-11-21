// BMX XXX's competitions (competition.h): a run judged, the field's marks and
// order. BMX XXX's own code (game.c).

#include "competition.h"
#include "player_riders.h"
#include "round.h"
#include "score.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/math/random.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/competition.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The turn score that takes the most of the score's share, by level
        // (the competitions' levels have theirs).
        inline ee::Global<uint32_t[kLevels], sym::g_levelTopScores> g_levelTopScores;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // A run's points before the judges' chance, by what it did: the
        // score's share of the level's top (up to 128); the tricks landed
        // (the first 51: up to 32, a tenth of the 32 each), the modifiers
        // used (up to 32, a thirteenth each), the cells of the level with
        // an air trick or a modifier (up to 16, a fifth each); then the trick
        // counts, each scaled and divided, up to its most (8 or 4), less the
        // bails' (the modifiers' count is not scaled: a third, up to 8).
        constexpr float kScoreShare = 128.0f;
        constexpr uint32_t kJudgedTricks = 51;
        constexpr uint32_t kRepeatColumns = 87; // the modifiers' columns of a trick's row, the trick alone first
        constexpr uint32_t kTricksMost = 32, kTricksPerPart = 10;
        constexpr uint32_t kModifiersMost = 32, kModifiersPerPart = 13;
        constexpr uint32_t kCellsMost = 16, kCellsPerPart = 5;
        struct CountPart
        {
            int32_t scale;
            int32_t perPart;
            int32_t most;
        };
        constexpr CountPart kAirTricksPart{8, 20, 8}, kModifiersPart{1, 3, 8}, kModifiedTricksPart{8, 12, 8}, kGrindsPart{8, 10, 8};
        constexpr CountPart kStallsPart{4, 5, 4}, kManualsPart{4, 6, 4}, kBailsPart{4, 3, 4};
        inline int32_t countPart(uint16_t count, CountPart part)
        {
            const int32_t points = count * part.scale / part.perPart;
            return points < part.most + 1 ? points : part.most;
        }
        inline uint32_t sharePart(uint32_t count, uint32_t most, uint32_t perPart)
        {
            const uint32_t points = count * most / perPart;
            return points < most + 1 ? points : most;
        }
        // A trick's two bits for a modifier (ScoreBlock::trickRepeats).
        inline uint32_t repeats(const uint8_t *row, uint32_t column)
        {
            return (row[column / 4] >> ((column & 3) * 2)) & 3;
        }

        // A judge's mark: half the points and a chance of 0 .. 7, from 128;
        // 255 at most.
        constexpr uint32_t kMarkFloor = 128, kMarkCeiling = 255, kMarkChance = 7;

        // The field (its ten rider characters), their marks and the order
        // they stand in; each rider's own bonus to a mark of chance.
        inline ee::Global<CompetitionScores[kRiderCharacters], sym::g_competitionScores> g_competitionScores;
        inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRiders> g_competitionRiders;
        inline ee::Global<uint8_t[kCompetitionRiders], sym::g_competitionRanking> g_competitionRanking;
        inline ee::Global<int8_t[kRiderCharacters], sym::g_competitorMarkBonus> g_competitorMarkBonus;
        // Another rider's mark: 175 and up to 75 of chance, 254 at most.
        // 1/255 (game.c's own copy, not the renderer's g_oneOver255).
        inline ee::Global<ee::Float, sym::g_competitionOneOver255> g_competitionOneOver255;
        constexpr float kRivalChance = 75.0f;
        constexpr int32_t kRivalFloor = 175, kRivalCeiling = 254;
        constexpr uint32_t kChanceBits = 0xFF;
    }

    void judgeScore(uint32_t player)
    {
        player &= 0xFF;
        const ee::Ptr<rider::Rider> rider = playerRider(player);
        rider::ScoreBlock &score = rider->score;
        if (gameMode() != kCompetitionMode)
            return;

        rider::CompetitionRun &run = rider->competitionRuns[roundNumber()];
        const int16_t *const counts = score.cellTrickCounts[kLevelCells];
        run.turnScore = ee::toInt(score.turnScore);
        const ee::Float share = score.turnScore * kScoreShare / zge::unsignedToFloat((*g_levelTopScores)[*g_currentLevel]);
        const int32_t scorePoints = ee::toInt(ee::min(share, ee::Float(kScoreShare)));

        uint32_t tricks = 0;
        for (uint32_t trick = 1; trick <= kJudgedTricks; ++trick)
        {
            for (uint32_t column = 0; column < kRepeatColumns; ++column)
            {
                if (repeats(score.trickRepeats[trick], column) != 0)
                {
                    ++tricks;
                    break;
                }
            }
        }
        int32_t points = scorePoints + static_cast<int32_t>(sharePart(tricks, kTricksMost, kTricksPerPart));

        uint32_t modifiers = 0;
        for (uint32_t column = 1; column < kRepeatColumns; ++column)
        {
            for (uint32_t trick = 0; trick <= kJudgedTricks; ++trick)
            {
                if (repeats(score.trickRepeats[trick], column) != 0)
                {
                    ++modifiers;
                    break;
                }
            }
        }
        points += static_cast<int32_t>(sharePart(modifiers, kModifiersMost, kModifiersPerPart));

        uint32_t cells = 0;
        for (uint32_t cell = 0; cell < kLevelCells; ++cell)
        {
            if (score.cellTrickCounts[cell][kCountAirTricks] != 0 || score.cellTrickCounts[cell][kCountModifiers] != 0)
                ++cells;
        }
        points += static_cast<int32_t>(sharePart(cells, kCellsMost, kCellsPerPart));
        points += countPart(static_cast<uint16_t>(counts[kCountAirTricks]), kAirTricksPart);
        points += countPart(static_cast<uint16_t>(counts[kCountModifiers]), kModifiersPart);
        points += countPart(static_cast<uint16_t>(counts[kCountModifiedTricks]), kModifiedTricksPart);
        points += countPart(static_cast<uint16_t>(counts[kCountGrinds]), kGrindsPart);
        points += countPart(static_cast<uint16_t>(counts[kCountStallsAndTaps]), kStallsPart);
        points += countPart(static_cast<uint16_t>(counts[kCountManuals]), kManualsPart);
        points -= countPart(static_cast<uint16_t>(counts[kCountBails]), kBailsPart);

        // The judges: a run with no score gets nothing. (A mark that fits
        // is drawn again: the original draws its chance twice.)
        const auto judgeMark = [&]() { return ((static_cast<uint32_t>(points) + (zge::randomNumber() & kMarkChance)) >> 1) + kMarkFloor; };
        uint32_t marks = 0;
        for (uint32_t judge = 0; judge < rider::kCompetitionJudges; ++judge)
        {
            if (score.turnScore > 0.0f)
                run.judges[judge] = static_cast<uint8_t>(judgeMark() < kMarkCeiling + 1 ? judgeMark() : kMarkCeiling);
            else
                run.judges[judge] = 0;
            marks += run.judges[judge];
        }
        run.mark = static_cast<uint8_t>(marks / rider::kCompetitionJudges);

        // The field: the player's character takes the run's mark, the others
        // one of chance; then each one's average of the runs so far.
        int32_t averages[kRiderCharacters];
        const uint8_t character = (*g_playerSetups)[player].character;
        for (uint32_t index = 0; index < kCompetitionRiders; ++index)
        {
            const uint8_t rival = (*g_competitionRiders)[index];
            CompetitionScores &scores = (*g_competitionScores)[rival];
            if (rival == character)
            {
                scores.run[roundNumber()] = run.mark;
            }
            else
            {
                const ee::Float chance = zge::unsignedToFloat(zge::randomNumber() & kChanceBits) * *g_competitionOneOver255 * kRivalChance;
                int32_t mark = ee::toInt(chance) + kRivalFloor + (*g_competitorMarkBonus)[rival];
                if (mark >= kRivalCeiling + 1)
                    mark = kRivalCeiling;
                scores.run[roundNumber()] = static_cast<uint8_t>(mark);
            }
            uint32_t total = 0;
            for (uint32_t past = 0; past < roundNumber() + 1u; ++past)
                total += scores.run[past];
            scores.average = static_cast<uint8_t>(total / (roundNumber() + 1u));
            averages[rival] = scores.average;
        }

        // The order: the best average first (each taken once).
        for (uint32_t place = 0; place < kCompetitionRiders; ++place)
        {
            int32_t best = -1;
            uint8_t bestRider = 0;
            for (uint32_t index = 0; index < kCompetitionRiders; ++index)
            {
                const uint8_t rival = (*g_competitionRiders)[index];
                if (best < averages[rival])
                {
                    best = averages[rival];
                    bestRider = rival;
                }
            }
            (*g_competitionRanking)[place] = bestRider;
            averages[bestRider] = -1;
        }
    }
    READABLE_FUNCTION(judgeScore, Verified);
}
