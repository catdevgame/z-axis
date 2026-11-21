// BMX XXX's trick scoring (score.h): what a turn's lines are worth. BMX
// XXX's own code (game.c, its game block), DM2's line (games/dm2/game/
// score_points.cpp).
//
// A trick's points are its own and its modifier's, times the modifier's
// factor, the factor of how often its motion looped and (but in mode 3) the
// factor of how often the turn has landed it already, a fifth more as an
// alley-oop and three tenths more after a backwards take-off; plus points
// for each frame it was held. HORSE and the modes 8 .. 16 score no trick
// points. A line's points are its tricks', times its spin's factor plus the
// spin's points, a fifth more for each of its two marks, times its landing's
// factor, and half as much again when it made a gap. The run's points are
// the lines' sum times a factor that grows with the different moves in it.

#include "score.h"

#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"
#include "trick_types.h"

#include "bmx/overlay/displays.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/score_points.cpp.h"

namespace game::bmx
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
        // The tables, by trick or modifier plus one (kNoTrick is the first
        // entry): as many as ScoreBlock::trickRepeats has rows and columns.
        constexpr uint32_t kTrickEntries = rider::kRepeatRows;
        constexpr uint32_t kModifierEntries = rider::kRepeatRowBytes * 4;
        inline ee::Global<MovePoints[kTrickEntries], sym::g_trickPoints> g_trickPoints;
        inline ee::Global<MovePoints[kModifierEntries], sym::g_modifierPoints> g_modifierPoints;
        inline ee::Global<ee::Float[kModifierEntries], sym::g_modifierFactor> g_modifierFactor;
        // By the motion's loops (the last entry for more), by how often the
        // turn landed the trick before (0 .. 3).
        constexpr uint32_t kCountFactors = 8;
        inline ee::Global<ee::Float[kCountFactors], sym::g_countFactor> g_countFactor;
        inline ee::Global<ee::Float[4], sym::g_varietyFactor> g_varietyFactor;
        // By the line's spin in half turns.
        constexpr uint32_t kSpinEntries = 9;
        inline ee::Global<ee::Float[kSpinEntries], sym::g_spinFactor> g_spinFactor;
        inline ee::Global<uint16_t[kSpinEntries], sym::g_spinPoints> g_spinPoints;
        // By the line's landing grade (ScoreLine::landingGrade).
        inline ee::Global<ee::Float[4], sym::g_landingFactor> g_landingFactor;
        // By the run's different moves (the last entry for more).
        constexpr int32_t kLineFactors = 8;
        inline ee::Global<ee::Float[kLineFactors], sym::g_lineFactor> g_lineFactor;

        constexpr ee::Float kAlleyOopFactor = 1.2f;
        constexpr ee::Float kBackTakeoffFactor = 1.3f;
        constexpr ee::Float kLineMarkFactor = 1.2f;
        constexpr ee::Float kGapFactor = 1.5f;
        // ScoreLine::flags (what sets it: not read yet; DM2's).
        // Scaffolding (unnamed-field): docs/SCAFFOLDING.md
        constexpr uint8_t kScoreLineMark1 = 0x01;

        // (inferred) Mode 3 scores a trick landed again as the first time:
        // DM2's scoreLines skips the factor for its free ride here.
        constexpr uint8_t kNoVarietyMode = 3;
        // HORSE and the modes 8 .. 16 (the distance mode and those that score
        // a turn by a record) score no trick points.
        constexpr uint8_t kFirstUnscoredMode = 8, kLastUnscoredMode = 16;
        bool modeScoresTricks(uint8_t mode)
        {
            if (mode == kHorseMode)
                return false;
            return !(mode >= kFirstUnscoredMode && mode <= kLastUnscoredMode);
        }

        // The tricks with a distance of their own (trick_types.h's tap and
        // manual tricks).
        constexpr uint8_t kWallride = 0x47, kNoseWheelie = 0x49;
        // A grind or a stall counts a quarter of a move for the run's
        // factor, the other types a whole one.
        constexpr ee::Float kSurfaceMove = 0.25f;

        // The measure display (overlay display 8, playerScoreFrame's).
        constexpr uint32_t kOverlayMeasure = 8;

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
        bool sameMove(const ScoreTrick &a, const ScoreTrick &b)
        {
            return a.trick == b.trick && a.modifier == b.modifier;
        }

        ee::Float lineFactorAt(int32_t moves)
        {
            return (*g_lineFactor)[moves <= kLineFactors ? moves - 1 : kLineFactors - 1];
        }

        // A trick's points.
        uint32_t trickPoints(const ScoreBlock &score, const ScoreTrick &trick)
        {
            const uint8_t number = trick.trick;
            const uint8_t modifier = trick.modifier;
            uint32_t points = (*g_trickPoints)[entryOf(number)].points + (*g_modifierPoints)[entryOf(modifier)].points;
            if (number != rider::kNoTrick)
                points = scaled(points, (*g_modifierFactor)[entryOf(modifier)]);
            points = scaled(points, (*g_countFactor)[trick.loops < kCountFactors ? trick.loops : kCountFactors - 1]);

            // How often the turn landed this trick with this modifier: two
            // bits of its row.
            const int32_t column = entryOf(modifier);
            const uint8_t row = *(&score.trickRepeats[0][0] + entryOf(number) * static_cast<int32_t>(rider::kRepeatRowBytes) + (column >> 2));
            const uint32_t landed = (row >> ((column & 3) * 2)) & 3;
            if (gameMode() != kNoVarietyMode)
                points = scaled(points, (*g_varietyFactor)[landed]);

            if ((trick.flags & rider::kScoreTrickAlleyOop) != 0)
                points = scaled(points, kAlleyOopFactor);
            if ((trick.flags & rider::kScoreTrickBackTakeoff) != 0)
                points = scaled(points, kBackTakeoffFactor);

            const uint16_t trickPerFrame = (*g_trickPoints)[entryOf(trick.trick)].perFrame;
            const uint16_t modifierPerFrame = (*g_modifierPoints)[entryOf(trick.modifier)].perFrame;
            return points + trick.framesHeld * (modifierPerFrame < trickPerFrame ? trickPerFrame : modifierPerFrame);
        }

        // The run's moves a trick adds: a trick (or a modifier alone) that
        // the line before did not start with.
        ee::Float movesOf(const ScoreBlock &score, uint32_t index, const ScoreTrick &trick)
        {
            if (trick.trick == rider::kNoTrick)
            {
                const uint8_t alone = trick.modifier;
                if (alone == rider::kNoTrick || (index != 0 && score.lines[index - 1].tricks->modifier == alone))
                    return 0.0f;
                return 1.0f;
            }
            if (index != 0 && sameMove(*score.lines[index - 1].tricks, trick))
                return 0.0f;
            switch (trickType(trick.trick))
            {
            case kTrickAir:
            case kTrickTap:
            case kTrickManual:
            case kTrickFlatland:
                return 1.0f;
            case kTrickGrind:
            case kTrickStall:
                return kSurfaceMove;
            default:
                return 0.0f;
            }
        }

        // The line's distances by kind: a grind's, a wall ride's, a
        // manual's or a nose wheelie's.
        void addDistance(ScoreBlock &score, const ScoreTrick &trick)
        {
            if (trickType(trick.trick) == kTrickGrind)
                score.kindScores[0] = score.kindScores[0] + trick.distance;
            else if (trickType(trick.trick) == kTrickTap && trick.trick == kWallride)
                score.kindScores[3] = score.kindScores[3] + trick.distance;
            else if (trickType(trick.trick) == kTrickManual)
            {
                if (trick.trick == kNoseWheelie)
                    score.kindScores[2] = score.kindScores[2] + trick.distance;
                else
                    score.kindScores[1] = score.kindScores[1] + trick.distance;
            }
        }
    }

    // scoreLines: every trick and line of the player's turn scored, the
    // distances by kind summed, the run's last line's distance (a grind's,
    // a wall ride's, a manual's) on the measure display, and the run's
    // points.
    void scoreLines(uint32_t player)
    {
        ee::Local<RewardItemMessage> measure;
        player &= 0xFF;
        ScoreBlock &score = playerRider(player)->score;
        ee::Float moves = 0.0f;
        uint32_t runPoints = 0;
        score.kindScores[0] = 0.0f;
        score.kindScores[1] = 0.0f;
        score.kindScores[2] = 0.0f;
        score.kindScores[3] = 0.0f;

        for (uint32_t index = 0; index < score.lineCount; ++index)
        {
            ScoreLine &line = score.lines[index];
            uint32_t linePoints = 0;
            for (ee::Ptr<ScoreTrick> trick = line.tricks; trick; trick = trick->next)
            {
                if (modeScoresTricks(gameMode()))
                {
                    const uint32_t points = trickPoints(score, *trick);
                    linePoints += points;
                    trick->points = static_cast<int32_t>(points);
                    moves = moves + movesOf(score, index, *trick);
                }
                addDistance(score, *trick);
            }

            const uint8_t spin = line.spin;
            linePoints = scaled(linePoints, (*g_spinFactor)[spin]);
            linePoints += (*g_spinPoints)[spin];
            if ((line.flags & kScoreLineMark1) != 0)
                linePoints = scaled(linePoints, kLineMarkFactor);
            if ((line.flags & rider::kScoreLineSpinFlag) != 0)
                linePoints = scaled(linePoints, kLineMarkFactor);
            linePoints = scaled(linePoints, (*g_landingFactor)[line.landingGrade]);
            if (line.gap != rider::kNoGap)
                linePoints = scaled(linePoints, kGapFactor);
            line.points = linePoints;
            runPoints += linePoints;
        }

        // The distance of the run's last line (by its first trick) on the
        // measure display.
        if (score.lineCount != 0)
        {
            const ScoreLine &last = score.lines[score.lineCount - 1];
            if (trickType(last.tricks->trick) == kTrickGrind)
            {
                measure->player = static_cast<uint8_t>(player);
                measure->kind = kRecordGrindDistance;
                measure->value = score.kindScores[0];
                overlayMessage(kOverlayMeasure, measure);
            }
            else if (trickType(last.tricks->trick) == kTrickTap && last.tricks->trick == kWallride)
            {
                measure->player = static_cast<uint8_t>(player);
                measure->kind = kRecordWallrideDistance;
                measure->value = score.kindScores[3];
                overlayMessage(kOverlayMeasure, measure);
            }
            else if (trickType(last.tricks->trick) == kTrickManual)
            {
                // (A nose wheelie's on the manual's record kind.)
                measure->player = static_cast<uint8_t>(player);
                measure->value = last.tricks->trick == kNoseWheelie ? score.kindScores[2] : score.kindScores[1];
                measure->kind = kRecordManualDistance;
                overlayMessage(kOverlayMeasure, measure);
            }
        }

        // The run's factor: the table's entry for its whole moves, eased
        // towards the next entry by the part of a move over.
        if (moves < ee::Float(1.0f))
            moves = 1.0f;
        ee::Float factor = lineFactorAt(ee::toInt(moves));
        const ee::Float part = moves - ee::toFloat(ee::toInt(moves));
        if (part != ee::Float(0.0f))
        {
            const ee::Float next = lineFactorAt(ee::toInt(moves + ee::Float(1.0f)));
            factor = factor + part * (next - factor);
        }
        score.linePoints = static_cast<uint32_t>(ee::toInt(zge::unsignedToFloat(runPoints) * factor));
    }
    READABLE_FUNCTION(scoreLines, Verified);
}
