// DM2's competitions (game.c): a run judged.

#include "competition.h"

#include "goals.h"
#include "players.h"
#include "round.h"
#include "score.h"
#include "dm2/action/action_state.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/conversions.h"
#include "zge/math/random.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/competition.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The run's points that earn the judges' whole share for points (kPointsMarks), by level.
        inline ee::Global<uint32_t[kGoalLevels], sym::g_competitionTopScores> g_competitionTopScores;
        // Each drawn character's bias, added to its runs' scores (the first
        // fourteen, the characters a rival is drawn from).
        constexpr uint32_t kRivalCharacters = 14;
        inline ee::Global<int8_t[kRivalCharacters], sym::g_rivalScoreBias> g_rivalScoreBias;

        // The judges' marks for a run, out of 256 (a judge's score is half
        // of them and of a random 0 .. 7, above 128): the points' share, and
        // the variety's: the different air tricks landed, the different
        // motions landed with them, the level's cells tricks were landed in,
        // and the counts of each kind of trick; the bails count against it.
        constexpr float kPointsMarks = 128.0f;
        constexpr uint32_t kTricksMarks = 32, kTricksForAll = 10;
        constexpr uint32_t kMotionsMarks = 32, kMotionsForAll = 13;
        constexpr uint32_t kCellsMarks = 16, kCellsForAll = 5;
        constexpr int32_t kAirTricksMarks = 8, kModifiersMarks = 8, kModifiedTricksMarks = 8, kGrindsMarks = 8;
        constexpr int32_t kLipTricksMarks = 4, kWheeliesMarks = 4, kBailsMarks = 4;
        constexpr uint32_t kJudgeRandomMask = 7;
        constexpr uint32_t kJudgeFloor = 0x80;
        constexpr uint32_t kJudgeMost = 0xFF;
        // A rival's run: 185 and up to 65 more at random, its bias added, 254 at most.
        constexpr int32_t kRivalLeast = 0xB9;
        constexpr float kRivalSpread = 65.0f;
        constexpr float kByteToOne = 1.0f / 255.0f;
        constexpr int32_t kRivalMost = 0xFE;

        // ScoreBlock::trickRepeats' rows and columns: a row is a trick plus
        // one (the air tricks' rows 1 .. kFirstGrind), a column the trick's
        // motion plus one (0: none; 86 motions).
        constexpr uint32_t kRepeatColumns = 0x57;
        uint32_t landedTimes(const rider::ScoreBlock &score, uint32_t row, uint32_t column)
        {
            return (score.trickRepeats[row][column >> 2] >> ((column & 3) * 2)) & 3;
        }

        // The working words of judgeScore, as its frame holds them: the
        // competitors' averages by character, then the player's set-up, score
        // block, run and the run's judges' scores, which it keeps there
        // across its calls and reads back.
        //
        // A bug the game keeps: the averages have sixteen words for the
        // twenty-one characters. A player riding character 16 .. 20 writes
        // its average over the words after them (16 the set-up, 17 the score
        // block, 18 the run, 19 the judges' scores; 20 the frame's next word,
        // the loop's own count, which it saves again before reading it), and
        // the rest of the field is judged with what those words then hold:
        // after character 16's, each rival is compared against the byte 3
        // past the average's value as the player's character.
        constexpr uint32_t kAverageWords = 16;
        struct Judging
        {
            int32_t averages[kAverageWords];      // +0x00
            ee::Ptr<PlayerSetup> setup;           // +0x40
            ee::Ptr<rider::ScoreBlock> score;     // +0x44
            ee::Ptr<rider::CompetitionRun> run;   // +0x48
            ee::Ptr<uint8_t> judges;              // +0x4C
            // A character's average: past the sixteenth, the words after the averages.
            int32_t &averageOf(uint8_t character) { return ee::Ptr<int32_t>(averages)[character]; }
        };
        static_assert(sizeof(Judging) == 0x50 && offsetof(Judging, setup) == 0x40 && offsetof(Judging, judges) == 0x4C);
    }

    void judgeScore(uint32_t player)
    {
        ee::Local<Judging> judging;
        judging->setup = &(*g_playerSetups)[player];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        judging->score = &rider->score;
        if (multiplayerMode() != 0 || singlePlayerMode() != kCompetition)
            return;
        judging->run = &rider->competitionRuns[roundNumber()];
        judging->run->points = sce::libgcc::fptoui(rider->score.turnScore);

        // The points' share.
        const ee::Float share = rider->score.turnScore * ee::Float(kPointsMarks) /
                                zge::unsignedToFloat((*g_competitionTopScores)[*g_sessionLevel]);
        judging->judges = judging->run->judges;
        int32_t marks = ee::toInt(ee::min(share, ee::Float(kPointsMarks)));

        // The different air tricks landed, with any motion or none.
        const rider::ScoreBlock &score = *judging->score;
        uint32_t tricks = 0;
        for (uint32_t row = 1; row <= kFirstGrind; ++row)
        {
            for (uint32_t column = 0; column < kRepeatColumns; ++column)
            {
                if (landedTimes(score, row, column) != 0)
                {
                    ++tricks;
                    break;
                }
            }
        }
        const uint32_t tricksPart = (tricks << 5) / kTricksForAll;
        marks += tricksPart <= kTricksMarks ? tricksPart : kTricksMarks;

        // The different motions landed in them (or in row 0).
        uint32_t motions = 0;
        for (uint32_t column = 1; column < kRepeatColumns; ++column)
        {
            bool landed = landedTimes(score, 0, column) != 0;
            for (uint32_t row = 1; !landed && row <= kFirstGrind; ++row)
                landed = landedTimes(score, row, column) != 0;
            if (landed)
                ++motions;
        }
        const uint32_t motionsPart = (motions << 5) / kMotionsForAll;
        marks += motionsPart <= kMotionsMarks ? motionsPart : kMotionsMarks;

        // The level's cells with an air trick or a modifier landed in them.
        uint32_t cells = 0;
        for (uint32_t cell = 0; cell < kCells; ++cell)
            if (score.cellTrickCounts[cell][kCountAirTricks] != 0 || score.cellTrickCounts[cell][kCountModifiers] != 0)
                ++cells;
        const uint32_t cellsPart = (cells << 4) / kCellsForAll;
        marks += cellsPart <= kCellsMarks ? cellsPart : kCellsMarks;

        // The level's counts of each kind of trick.
        const int16_t *const level = score.cellTrickCounts[kCells];
        const auto count = [&](TrickCountKind kind) { return static_cast<int32_t>(static_cast<uint16_t>(level[kind])); };
        const auto part = [](int32_t value, int32_t most) { return value < most + 1 ? value : most; };
        marks += part((count(kCountAirTricks) << 3) / 20, kAirTricksMarks);
        marks += part(count(kCountModifiers) / 3, kModifiersMarks);
        marks += part((count(kCountModifiedTricks) << 3) / 12, kModifiedTricksMarks);
        marks += part((count(kCountGrinds) << 3) / 10, kGrindsMarks);
        marks += part((count(kCountLipTricks) << 2) / 5, kLipTricksMarks);
        marks += part((count(kCountWheelies) << 2) / 6, kWheeliesMarks);
        marks -= part((count(kCountBails) << 2) / 3, kBailsMarks);

        // The five judges (a run without points gets nothing from them).
        // Their arithmetic is unsigned: marks below 0 give a judge's most.
        uint32_t judged = 0;
        for (uint32_t judge = 0; judge < rider::kCompetitionJudges; ++judge)
        {
            if (ee::Float(0.0f) < judging->score->turnScore)
            {
                if (((static_cast<uint32_t>(marks) + (zge::randomNumber() & kJudgeRandomMask)) >> 1) + kJudgeFloor < kJudgeMost + 1)
                    judging->judges[judge] = static_cast<uint8_t>(
                        ((static_cast<uint32_t>(marks) + (zge::randomNumber() & kJudgeRandomMask)) >> 1) - kJudgeFloor);
                else
                    judging->judges[judge] = static_cast<uint8_t>(kJudgeMost);
            }
            else
            {
                judging->judges[judge] = 0;
            }
            judged += judging->judges[judge];
        }
        judging->run->average = static_cast<uint8_t>(judged / rider::kCompetitionJudges);

        // The field's runs: the player's as judged, each rival's drawn; then
        // each competitor's average over the runs so far.
        for (uint32_t entrant = 0; entrant < kCompetitionRiders; ++entrant)
        {
            const uint8_t character = (*g_competitionRiders)[entrant];
            CompetitorScores &scores = (*g_competitionScores)[character];
            if (character == judging->setup->character)
            {
                const uint8_t round = roundNumber();
                scores.runs[round] = judging->run->average;
            }
            else
            {
                const ee::Float drawn = ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFF)) *
                                        ee::Float(kByteToOne) * ee::Float(kRivalSpread);
                int32_t rivalRun = ee::toInt(drawn) + kRivalLeast + (*g_rivalScoreBias)[character];
                if (rivalRun > kRivalMost)
                    rivalRun = kRivalMost;
                const uint8_t round = roundNumber();
                scores.runs[round] = static_cast<uint8_t>(rivalRun);
            }
            uint32_t sum = 0;
            for (uint32_t run = 0; run < roundNumber() + 1u; ++run)
                sum += scores.runs[run];
            const uint32_t average = sum / (roundNumber() + 1u);
            scores.average = static_cast<uint8_t>(average);
            judging->averageOf(character) = static_cast<int32_t>(average & 0xFF);
        }

        // The ranking: the best average left, ten times (taken out as it is placed).
        for (uint32_t place = 0; place < kCompetitionRiders; ++place)
        {
            int32_t best = -1;
            uint8_t bestCharacter = 0;
            for (uint32_t entrant = 0; entrant < kCompetitionRiders; ++entrant)
            {
                const uint8_t character = (*g_competitionRiders)[entrant];
                if (best < judging->averageOf(character))
                {
                    best = judging->averageOf(character);
                    bestCharacter = character;
                }
            }
            (*g_competitionRanking)[place] = bestCharacter;
            judging->averageOf(bestCharacter) = -1;
        }
    }
    READABLE_FUNCTION(judgeScore, Verified);
}
