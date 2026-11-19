// DM2's trick scoring (game.c): what a turn's lines are worth.
//
// A trick's points are its own and its modifier's, times the modifier's
// factor, the factor of how often its motion looped and (but in a free
// ride) the factor of how often the turn has landed it already, a fifth
// more ridden fakie and three tenths more after a backwards take-off; plus
// points for each frame it was held. A line's points are its tricks',
// times its spin's factor plus the spin's points, a fifth more for each of
// its two marks, and half as much again plus the gap's points when it made
// a gap. The run's points are the lines' sum times a factor that grows with
// the different moves in it.

#include "score.h"
#include "gaps.h"
#include "records.h"
#include "round.h"

#include "dm2/overlay/overlay.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/score_points.cpp.h"

namespace game::dm2
{
    namespace
    {
        // A trick's or a modifier's points, and its points a frame held.
        struct MovePoints
        {
            uint16_t points;
            uint16_t perFrame;
        };
        static_assert(sizeof(MovePoints) == 4);
        // The tables, by trick or modifier plus one (kNoTrick is the first entry).
        constexpr uint32_t kTrickEntries = 74;
        constexpr uint32_t kModifierEntries = 88;
        inline ee::Global<MovePoints[kTrickEntries], sym::trickPoints> g_trickPoints;
        inline ee::Global<MovePoints[kModifierEntries], sym::modifierPoints> g_modifierPoints;
        inline ee::Global<ee::Float[kModifierEntries], sym::modifierFactor> g_modifierFactor;
        // By the motion's loops (the last entry for more), by how often the
        // turn landed the trick before (0 .. 3).
        constexpr uint32_t kCountFactors = 8;
        inline ee::Global<ee::Float[kCountFactors], sym::countFactor> g_countFactor;
        inline ee::Global<ee::Float[4], sym::varietyFactor> g_varietyFactor;
        // By the line's spin in half turns.
        constexpr uint32_t kSpinEntries = 9;
        inline ee::Global<ee::Float[kSpinEntries], sym::spinFactor> g_spinFactor;
        inline ee::Global<uint16_t[kSpinEntries], sym::spinPoints> g_spinPoints;
        // By the run's different moves (the last entry for more).
        constexpr int32_t kLineFactors = 8;
        inline ee::Global<ee::Float[kLineFactors], sym::lineFactor> g_lineFactor;

        constexpr ee::Float kFakieFactor = 1.2f;
        constexpr ee::Float kBackTakeoffFactor = 1.3f;
        constexpr ee::Float kLineMarkFactor = 1.2f;
        constexpr ee::Float kGapFactor = 1.5f;
        constexpr uint8_t kScoreLineMark1 = 0x01; // ScoreLine::flags (what sets it: not read yet)

        // The tricks by number: the air's, then the grinds and stalls and
        // the first lip tricks (a quarter of a move each for the run's
        // factor), then the last lip tricks and the two wheelies.
        constexpr uint8_t kFirstSurfaceTrick = 0x33;
        constexpr uint32_t kGrindTricks = 9;
        constexpr uint8_t kFirstWholeMoveSurfaceTrick = 0x44;
        constexpr uint8_t kFirstWheelie = 0x46;
        constexpr uint32_t kWheelies = 2;
        constexpr ee::Float kSurfaceMove = 0.25f;

        // The games that score no trick points: HORSE and the best-turn modes.
        bool modeScoresTricks(uint8_t mode)
        {
            if (mode == kHorseMode)
                return false;
            return !(mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode);
        }

        inline ee::Global<ee::Ptr<LevelGap>, sym::g_levelGaps> g_levelGaps;

        uint32_t scaled(uint32_t points, ee::Float factor)
        {
            return static_cast<uint32_t>(ee::toInt(zge::unsignedToFloat(points) * factor));
        }
        // A trick's (or a modifier's) entry: its number as a signed byte, plus one.
        int32_t entryOf(uint8_t move)
        {
            return static_cast<int8_t>(move) + 1;
        }
        // The trick and modifier bytes of a trick's first word.
        bool sameMove(const rider::ScoreTrick &a, const rider::ScoreTrick &b)
        {
            return a.trick == b.trick && a.trickMotion == b.trickMotion;
        }

        ee::Float lineFactorAt(int32_t moves)
        {
            return (*g_lineFactor)[moves <= kLineFactors ? moves - 1 : kLineFactors - 1];
        }
    }

    void scoreLines(rider::ScoreBlock *score)
    {
        ee::Local<OverlayRuler> measure;
        ee::Float moves = 0.0f;
        uint32_t runPoints = 0;
        score->kindScores[0] = 0.0f;
        score->kindScores[1] = 0.0f;
        score->kindScores[2] = 0.0f;

        for (uint32_t index = 0; index < score->lineCount; ++index)
        {
            rider::ScoreLine &line = score->lines[index];
            uint32_t linePoints = 0;
            for (ee::Ptr<rider::ScoreTrick> trick = line.tricks; trick; trick = trick->next)
            {
                if (modeScoresTricks(multiplayerMode()))
                {
                    const uint8_t number = trick->trick;
                    const uint8_t modifier = trick->trickMotion;
                    uint32_t points =
                        (*g_trickPoints)[entryOf(number)].points + (*g_modifierPoints)[entryOf(modifier)].points;
                    if (number != rider::kNoTrick)
                        points = scaled(points, (*g_modifierFactor)[entryOf(modifier)]);
                    points = scaled(points, (*g_countFactor)[trick->loops < kCountFactors ? trick->loops : kCountFactors - 1]);

                    // How often the turn landed this trick with this modifier: two bits of its row.
                    const int32_t column = entryOf(modifier);
                    const uint8_t landed = (score->trickRepeats[entryOf(number)][column >> 2] >> ((column & 3) * 2)) & 3;
                    if (singlePlayerMode() != kFreeRide)
                        points = scaled(points, (*g_varietyFactor)[landed]);

                    if ((trick->flags & rider::kScoreTrickFakie) != 0)
                        points = scaled(points, kFakieFactor);
                    if ((trick->flags & rider::kScoreTrickBackTakeoff) != 0)
                        points = scaled(points, kBackTakeoffFactor);

                    const uint16_t trickPerFrame = (*g_trickPoints)[entryOf(trick->trick)].perFrame;
                    const uint16_t modifierPerFrame = (*g_modifierPoints)[entryOf(trick->trickMotion)].perFrame;
                    points += trick->framesHeld * (modifierPerFrame < trickPerFrame ? trickPerFrame : modifierPerFrame);
                    linePoints += points;
                    trick->points = points;

                    // The run's moves: a trick (or a modifier alone) that
                    // the line before did not start with.
                    const uint8_t first = trick->trick;
                    if (first == rider::kNoTrick)
                    {
                        const uint8_t alone = trick->trickMotion;
                        if (alone != rider::kNoTrick && (index == 0 || score->lines[index - 1].tricks->trickMotion != alone))
                            moves = moves + ee::Float(1.0f);
                    }
                    else if (index == 0 || !sameMove(*score->lines[index - 1].tricks, *trick))
                    {
                        if (first < kFirstSurfaceTrick)
                            moves = moves + ee::Float(1.0f);
                        else if (first < kFirstWholeMoveSurfaceTrick)
                            moves = moves + kSurfaceMove;
                        else if (first < kFirstWheelie + kWheelies)
                            moves = moves + ee::Float(1.0f);
                    }
                }

                const uint32_t number = trick->trick;
                if (number - kFirstSurfaceTrick < kGrindTricks)
                {
                    score->kindScores[0] = score->kindScores[0] + trick->distance;
                }
                else if (number - kFirstWheelie < kWheelies)
                {
                    score->kindScores[1] = score->kindScores[1] + trick->distance;
                    if (trick->trick == kNoseWheelie)
                        score->kindScores[2] = score->kindScores[2] + trick->distance;
                }
            }

            const uint8_t spin = line.spinHalfTurns;
            linePoints = scaled(linePoints, (*g_spinFactor)[spin]);
            linePoints += (*g_spinPoints)[spin];
            if ((line.flags & kScoreLineMark1) != 0)
                linePoints = scaled(linePoints, kLineMarkFactor);
            if ((line.flags & rider::kScoreLineSpinFlag) != 0)
                linePoints = scaled(linePoints, kLineMarkFactor);
            if (line.gap != kNoGap)
            {
                const LevelGap &gap = (*g_levelGaps)[line.gap];
                linePoints = scaled(linePoints, kGapFactor);
                linePoints += gap.points;
            }
            line.points = linePoints;
            runPoints += linePoints;
        }

        // A run that ends in a grind or a wheelie: its distance on the ruler.
        if (score->lineCount != 0)
        {
            const uint32_t last = score->lines[score->lineCount - 1].tricks->trick;
            if (last - kFirstSurfaceTrick < kGrindTricks)
            {
                measure->recordKind = kGrindDistanceRecord;
                measure->metres = score->kindScores[0];
                overlayMessage(kOverlayRuler, measure);
            }
            else if (static_cast<uint8_t>(last - kFirstWheelie) < kWheelies)
            {
                measure->recordKind = kWheelieDistanceRecord;
                measure->metres = score->kindScores[1];
                overlayMessage(kOverlayRuler, measure);
            }
        }

        // The run's factor: the table's entry for its whole moves, eased
        // towards the next entry by the part of a move over.
        if (moves < ee::Float(1.0f))
            moves = 1.0f;
        ee::Float factor = lineFactorAt(ee::toInt(moves));
        const ee::Float part = moves - ee::toFloat(ee::toInt(moves));
        if (part != 0.0f)
        {
            const ee::Float next = lineFactorAt(ee::toInt(moves + ee::Float(1.0f)));
            factor = factor + part * (next - factor);
        }
        score->linePoints = static_cast<uint32_t>(ee::toInt(zge::unsignedToFloat(runPoints) * factor));
    }
    READABLE_FUNCTION(scoreLines, Verified);
}
