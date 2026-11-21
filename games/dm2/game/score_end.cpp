// DM2's trick scoring (game.c): a run's end.

#include "score.h"
#include "gaps.h"
#include "players.h"
#include "records.h"
#include "round.h"

#include "dm2/action/action.h"
#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/score_end.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The level's quarters (gameSetup measures them): a rider's cell
        // is its quarter in x times four plus its quarter in z.
        inline ee::Global<uint16_t, sym::g_levelQuarterX> g_levelQuarterX;
        inline ee::Global<uint16_t, sym::g_levelQuarterZ> g_levelQuarterZ;
        constexpr uint32_t kQuarters = 4;

        constexpr uint8_t kMostLandedCounted = 3; // a trick's two bits of ScoreBlock::trickRepeats

        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        inline ee::Global<uint8_t[9], sym::g_levelGapCounts> g_levelGapCounts;
        constexpr uint32_t kGoalFlagAllGaps = 0x8000; // ScoreBlock::goalFlags

        // HORSE's line to match: the first player's first line and its
        // tricks, kept as the turn ends.
        constexpr uint32_t kKeptTricks = 16;
        inline ee::Global<rider::ScoreTrick[kKeptTricks], sym::g_keptTricks> g_keptTricks;

        constexpr uint32_t kRunEndSound = 0x2B;


        uint32_t bit(uint32_t index) { return 1u << (index & 31); }

        // A trick landed: the turn's count of it (three at most), its
        // kind's counts in the rider's cell and over the level, a grind's
        // or wheelie's distance, the character's landed moves.
        void countLandedTrick(uint32_t player, rider::ScoreBlock &score, RiderCharacter &character,
                              const rider::ScoreTrick &trick, int16_t *cell, int16_t *level)
        {
            const int32_t row = static_cast<int8_t>(trick.trick) + 1;
            const int32_t column = static_cast<int8_t>(trick.trickMotion) + 1;
            uint8_t &repeats = score.trickRepeats[row][column >> 2];
            const uint32_t shift = (column & 3) * 2;
            if (((repeats >> shift) & 3) < kMostLandedCounted)
                repeats = static_cast<uint8_t>(repeats + (1u << shift));

            const auto count = [&](TrickCountKind kind) {
                ++cell[kind];
                ++level[kind];
            };
            const uint8_t number = trick.trick;
            if (number < kFirstGrind)
            {
                count(kCountAirTricks);
            }
            else if (number < kFirstStall)
            {
                score.kindScores[0] = score.kindScores[0] + trick.distance;
                count(kCountGrinds);
            }
            else if (number < kFirstHeldTrick)
            {
                count(kCountLipTricks);
            }
            else if (number < kManual)
            {
                checkGameRecord(player, kHoldRecord, kRewardHold, 0, score.heldTrickHeight);
                count(kCountLipTricks);
            }
            else if (number < kTricksEnd)
            {
                if (number == kManual)
                    score.kindScores[1] = score.kindScores[1] + trick.distance;
                else if (number == kNoseWheelie)
                    score.kindScores[2] = score.kindScores[2] + trick.distance;
                count(kCountWheelies);
            }

            uint8_t *const landed = character.movesLanded;
            if (trick.trick != rider::kNoTrick)
                landed[trick.trick >> 3] |= static_cast<uint8_t>(1u << (trick.trick & 7));
            if (trick.trickMotion != rider::kNoTrick)
            {
                const uint32_t modifierBit = trick.trickMotion + kFirstModifierBit;
                landed[modifierBit >> 3] |= static_cast<uint8_t>(1u << (modifierBit & 7));
                count(kCountModifiers);
                if (trick.trick != rider::kNoTrick)
                    count(kCountModifiedTricks);
            }
        }

        // A line's gap, in a ProQuest level's goals or competition: found
        // for the first time it is marked for the character and the game,
        // and the reward display says how many of the level's are found.
        void markGapFound(rider::ScoreBlock &score, RiderCharacter &character, const rider::ScoreLine &line,
                          ee::Local<OverlayReward> &reward)
        {
            if (line.gap == kNoGap)
                return;
            if (multiplayerMode() != 0)
                return;
            if (singlePlayerMode() != kLevelGoals && singlePlayerMode() != kCompetition)
                return;
            LevelGoals &goals = character.levels[*g_sessionLevel];
            uint32_t &found = (&goals.gapsFound)[line.gap >> 5];
            if ((found & bit(line.gap)) != 0)
                return;
            found |= bit(line.gap);
            (&score.gapsFound)[line.gap >> 5] |= bit(line.gap); // (the gap's word, as the character's)

            const uint8_t level = *g_sessionLevel;
            uint32_t count = 0;
            for (uint32_t gap = 0; gap < (*g_levelGapCounts)[level]; ++gap)
                count += ((&character.levels[level].gapsFound)[gap >> 5] >> (gap & 31)) & 1;
            if (count < (*g_levelGapCounts)[level])
            {
                reward->kind = kRewardGapFound;
            }
            else
            {
                character.levels[level].flags |= kLevelAllGapsFound;
                reward->kind = kRewardAllGapsFound;
                score.goalFlags |= kGoalFlagAllGaps;
            }
            reward->value = zge::unsignedToFloat(count);
            overlayMessage(kOverlayReward, reward);
        }

        // HORSE, the round's first turn: the run's first line and its
        // tricks kept for the others to match; the turn scores 1.
        void keepLineToMatch(rider::ScoreBlock &score)
        {
            uint32_t keptTricks = 0;
            uint32_t index = 0;
            while (score.lineCount < 2 ? index < score.lineCount : index == 0)
            {
                rider::ScoreLine &kept = (*g_keptLines)[index];
                const rider::ScoreLine &line = score.lines[index];
                zge::memoryCopy(&kept, &line, sizeof(rider::ScoreLine));
                kept.tricks = ee::Ptr<rider::ScoreTrick>();
                ++index;
                ee::Ptr<rider::ScoreTrick> before;
                for (ee::Ptr<rider::ScoreTrick> trick = line.tricks; trick; trick = trick->next)
                {
                    const ee::Ptr<rider::ScoreTrick> copy = &(*g_keptTricks)[keptTricks++];
                    zge::memoryCopy(copy, trick, sizeof(rider::ScoreTrick));
                    copy->next = ee::Ptr<rider::ScoreTrick>();
                    if (before)
                        before->next = copy;
                    else
                        kept.tricks = copy;
                    before = copy;
                }
            }
            *g_keptLineCount = score.lineCount < 2 ? score.lineCount : 1;
            score.turnScore = 1.0f;
        }

        // HORSE, a later turn: whether the run matches the kept line: each
        // of its tricks and modifiers (where the kept one has one), its gap
        // (where the kept line made one), and at least its spin.
        bool matchesKeptLine(const rider::ScoreBlock &score)
        {
            if (*g_keptLineCount == 0)
                return true;
            uint32_t index = 0;
            do
            {
                const rider::ScoreLine &kept = (*g_keptLines)[index];
                const rider::ScoreLine &line = score.lines[index];
                ee::Ptr<rider::ScoreTrick> trick = line.tricks;
                ee::Ptr<rider::ScoreTrick> wanted = kept.tricks;
                ++index;
                while (trick && wanted)
                {
                    if (wanted->trick != rider::kNoTrick && trick->trick != wanted->trick)
                        return false;
                    if (wanted->trickMotion != rider::kNoTrick && trick->trickMotion != wanted->trickMotion)
                        return false;
                    if (kept.gap != kNoGap && line.gap != kept.gap)
                        return false;
                    if (line.spinHalfTurns < kept.spinHalfTurns)
                        return false;
                    trick = trick->next;
                    if (!trick)
                        break;
                    wanted = wanted->next;
                }
            } while (index < *g_keptLineCount);
            return true;
        }
    }

    void trickLineEnd(uint32_t player, uint32_t landed)
    {
        ee::Local<OverlayTrick> shown;
        ee::Local<OverlayReward> reward;
        ee::Local<OverlayRuler> noMeasure;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);

        if (actionMode() == 0)
        {
            // The rider's cell of the level, for the turn's counts.
            const rider::Bounds *const bounds = rider::levelBounds();
            uint32_t quarterX = static_cast<uint32_t>(ee::toInt(rider->position[0] - bounds->min[0]) / static_cast<int32_t>(*g_levelQuarterX));
            if (quarterX >= kQuarters)
                quarterX = kQuarters - 1;
            const int32_t quarterZ = ee::toInt(rider->position[2] - bounds->min[2]) / static_cast<int32_t>(*g_levelQuarterZ);
            uint32_t cell = quarterX * kQuarters + static_cast<uint32_t>(quarterZ);
            if (cell >= kCells)
                cell = kCells - 1;
            int16_t *const cellCounts = score.cellTrickCounts[cell];
            int16_t *const levelCounts = score.cellTrickCounts[kCells];

            if (landed == 0)
            {
                soundEvent(kRunEndSound, 0, 0, 0, 0);
                overlayShownMessage(kOverlayTrick, ee::Address());
                ++cellCounts[kCountBails];
                ++levelCounts[kCountBails];
                shown->action = kTrickDisplayCrash;
            }
            else
            {
                score.kindScores[0] = 0.0f;
                score.kindScores[1] = 0.0f;
                score.kindScores[2] = 0.0f;
                for (uint32_t index = 0; index < score.lineCount; ++index)
                {
                    const rider::ScoreLine &line = score.lines[index];
                    for (ee::Ptr<rider::ScoreTrick> trick = line.tricks; trick; trick = trick->next)
                        countLandedTrick(player, score, *character, *trick, cellCounts, levelCounts);
                    markGapFound(score, *character, line, reward);
                }

                // The turn's score, by the game.
                const uint8_t mode = multiplayerMode();
                if (mode == kHorseMode)
                {
                    if (roundFlags(kFirstTurnFlag))
                        keepLineToMatch(score);
                    else if (matchesKeptLine(score))
                        score.turnScore = 1.0f;
                }
                else if (!(mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode))
                {
                    if (singlePlayerMode() == kFreeRide)
                    {
                        // A free ride shows its last run alone.
                        score.turnScore = zge::unsignedToFloat(score.linePoints);
                    }
                    else
                    {
                        score.turnScore = score.turnScore + zge::unsignedToFloat(score.linePoints);
                        checkGameRecord(player, kTurnScoreRecord, kRewardTurnScore, kAnnounceOnce, score.turnScore);
                        checkGameRecord(player, kRunPointsRecord, kRewardRunPoints, 0, zge::unsignedToFloat(score.linePoints));
                        for (uint32_t index = 0; index < score.lineCount; ++index)
                        {
                            checkGameRecord(player, kLinePointsRecord, kRewardLinePoints, 0,
                                            zge::unsignedToFloat(score.lines[index].points));
                        }
                    }
                }

                uint32_t tricks = 0;
                const uint32_t lines = score.lineCount;
                for (uint32_t index = 0; index < lines; ++index)
                {
                    for (ee::Ptr<rider::ScoreTrick> trick = score.lines[index].tricks; trick; trick = trick->next)
                        ++tricks;
                }
                if (tricks >= 2)
                {
                    reward->kind = kRewardRunTricks;
                    reward->value = zge::unsignedToFloat(tricks);
                    overlayMessage(kOverlayReward, reward);
                }
                checkGameRecord(player, kRunTricksRecord, kRewardRunTricksRecord, 0, zge::unsignedToFloat(tricks));
                checkGameRecord(player, kGrindDistanceRecord, kRewardGrindDistance, 0, score.kindScores[0]);
                checkGameRecord(player, kWheelieDistanceRecord, kRewardWheelieDistance, 0, score.kindScores[1]);
                checkGameRecord(player, kNoseWheelieDistanceRecord, kRewardNoseWheelieDistance, 0, score.kindScores[2]);
                for (uint32_t index = 0; index < score.lineCount; ++index)
                    checkGameRecord(player, kSpinRecord, kRewardSpin, 0, ee::toFloat(score.lines[index].spinHalfTurns));

                soundEvent(kRunEndSound, 1, score.linePoints, 0, 0);
                shown->action = kTrickDisplayScoreFadeOut;
            }
            shown->turn = ee::Address(&score.turnScore);
            overlayMessage(kOverlayTrick, shown);
        }

        // The ruler emptied and the run's lines cleared.
        noMeasure->metres = 0.0f;
        overlayMessage(kOverlayRuler, noMeasure);
        score.kindScores[2] = 0.0f;
        score.lineState = kLineNone;
        score.lineCount = 0;
        score.trickCount = 0;
        score.kindScores[0] = 0.0f;
        score.kindScores[1] = 0.0f;
    }
    READABLE_FUNCTION(trickLineEnd, Verified);
}
