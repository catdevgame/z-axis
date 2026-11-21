// BMX XXX's trick scoring (score.h): a run's end. BMX XXX's own code
// (game.c, its game block), DM2's line (games/dm2/game/score_end.cpp): the
// run's tricks counted (by the rider's cell of the level, and over the
// level), the turn scored by the mode, the game's records checked, the
// trick, measure and balance displays told, and the run's lines cleared.

#include "score.h"

#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"
#include "trick_types.h"

#include "bmx/action/camera_state.h"
#include "bmx/overlay/displays.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/score_end.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The level's quarters (gameSetup measures them): a rider's cell is
        // its quarter in x times four plus its quarter in z.
        inline ee::Global<uint16_t, sym::g_levelQuarterX> g_levelQuarterX;
        inline ee::Global<uint16_t, sym::g_levelQuarterZ> g_levelQuarterZ;
        constexpr uint32_t kQuarters = 4;
        constexpr uint32_t kCells = 16;

        // The tricks with a distance of their own: the wall ride (a tap
        // trick), the manual and the nose wheelie.
        constexpr uint8_t kWallride = 0x47, kManual = 0x48, kNoseWheelie = 0x49;
        // A trick's two bits of ScoreBlock::trickRepeats count to three.
        constexpr uint32_t kMostLandedCounted = 3;
        // SavedGame::movesLanded: the modifiers' bits after the tricks'.
        constexpr uint32_t kFirstModifierBit = 100;

        // The reward display's items: the run's tricks (from three), and
        // each record's when its saved one is beaten.
        constexpr uint8_t kRewardItemCombo = 7;
        constexpr uint32_t kMostTricksUncounted = 2;
        constexpr uint32_t kRewardRunScore = 0x0E, kRewardTrickScore = 0x0F, kRewardStackScore = 0x10,
                           kRewardCombo = 0x11, kRewardSpin = 0x12, kRewardGrindDistance = 0x13,
                           kRewardManualDistance = 0x14, kRewardNoseWheelieDistance = 0x15,
                           kRewardTapDistance = 0x17, kRewardWallrideDistance = 0x1A;
        constexpr uint32_t kAnnounceOnce = 0x1; // checkGameRecord's flags

        // The distance mode (8) and the modes 9 .. 16 score a turn their own
        // way (playerScoreFrame): the run adds nothing to it.
        constexpr uint8_t kFirstRecordScoredMode = 8, kLastRecordScoredMode = 16;

        // HORSE's line to match: the first player's first line and its
        // tricks, kept as the turn ends.
        constexpr uint32_t kKeptTricks = 16;
        inline ee::Global<ScoreTrick[kKeptTricks], sym::g_keptTricks> g_keptTricks;
        inline ee::Global<ScoreLine[1], sym::g_keptLines> g_keptLines;
        inline ee::Global<uint8_t, sym::g_keptLineCount> g_keptLineCount;

        // The sound of a line's end (the game bank's: landed, or lost in a
        // bail), told the run's points.
        constexpr uint32_t kSoundLineEnd = 60;

        // The measure display (overlay display 8, playerScoreFrame's) and
        // the balance display, put back as the run ends.
        constexpr uint32_t kOverlayMeasure = 8;
        constexpr uint16_t kBalanceReset = 0x20; // BalanceMessage::flags: the display's "Init"

        // The rider's cell of the level, for the turn's counts.
        uint32_t riderCell(const rider::Rider *rider)
        {
            const rider::Bounds *const bounds = rider::levelBounds();
            uint32_t quarterX = static_cast<uint32_t>(ee::toInt(rider->position[0] - bounds->min[0]) /
                                                      static_cast<int32_t>(*g_levelQuarterX));
            if (quarterX >= kQuarters)
                quarterX = kQuarters - 1;
            const int32_t quarterZ = ee::toInt(rider->position[2] - bounds->min[2]) / static_cast<int32_t>(*g_levelQuarterZ);
            uint32_t cell = quarterX * kQuarters + static_cast<uint32_t>(quarterZ);
            if (cell >= kCells)
                cell = kCells - 1;
            return cell;
        }

        // A trick landed: the turn's count of it with its modifier (three
        // at most), its type's counts in the rider's cell and over the
        // level, a grind's, manual's, nose wheelie's or wall ride's distance
        // (a tap trick's hold against its record), and the saved game's
        // moves landed.
        void countLandedTrick(uint32_t player, ScoreBlock &score, const ScoreTrick &trick, int16_t *cell, int16_t *level)
        {
            const int32_t row = static_cast<int8_t>(trick.trick) + 1;
            const int32_t column = static_cast<int8_t>(trick.modifier) + 1;
            uint8_t &repeats = *(&score.trickRepeats[0][0] + row * static_cast<int32_t>(rider::kRepeatRowBytes) + (column >> 2));
            const uint32_t shift = static_cast<uint32_t>(column & 3) * 2;
            if (((repeats >> shift) & 3) < kMostLandedCounted)
                repeats = static_cast<uint8_t>(repeats + (1u << shift));

            const auto count = [&](TrickCountKind kind) {
                ++cell[kind];
                ++level[kind];
            };
            switch (trickType(trick.trick))
            {
            case kTrickAir:
                count(kCountAirTricks);
                break;
            case kTrickGrind:
                score.kindScores[0] = score.kindScores[0] + trick.distance;
                count(kCountGrinds);
                break;
            case kTrickStall:
                count(kCountStallsAndTaps);
                break;
            case kTrickTap:
                if (trick.trick == kWallride)
                    score.kindScores[3] = score.kindScores[3] + trick.distance;
                else
                    checkGameRecord(player, kRecordTapDistance, kRewardTapDistance, 0, score.longestHold);
                count(kCountStallsAndTaps);
                break;
            case kTrickManual:
                if (trick.trick == kManual)
                    score.kindScores[1] = score.kindScores[1] + trick.distance;
                else if (trick.trick == kNoseWheelie)
                    score.kindScores[2] = score.kindScores[2] + trick.distance;
                count(kCountManuals);
                break;
            default:
                break;
            }

            uint8_t *const landed = g_savedGame->movesLanded;
            if (trick.trick != rider::kNoTrick)
                landed[trick.trick >> 3] |= static_cast<uint8_t>(1u << (trick.trick & 7));
            if (trick.modifier != rider::kNoTrick)
            {
                const uint32_t modifierBit = trick.modifier + kFirstModifierBit;
                landed[modifierBit >> 3] |= static_cast<uint8_t>(1u << (modifierBit & 7));
                count(kCountModifiers);
                if (trick.trick != rider::kNoTrick)
                    count(kCountModifiedTricks);
            }
        }

        // HORSE, the round's first turn: the run's first line and its
        // tricks kept for the others to match; the turn scores 1.
        void keepLineToMatch(ScoreBlock &score)
        {
            uint32_t keptTricks = 0;
            uint32_t index = 0;
            while (score.lineCount < 2 ? index < score.lineCount : index == 0)
            {
                ScoreLine &kept = (*g_keptLines)[index];
                const ScoreLine &line = score.lines[index];
                zge::memoryCopy(&kept, &line, sizeof(ScoreLine));
                kept.tricks = {};
                ++index;
                ee::Ptr<ScoreTrick> before;
                for (ee::Ptr<ScoreTrick> trick = line.tricks; trick; trick = trick->next)
                {
                    const ee::Ptr<ScoreTrick> copy(&(*g_keptTricks)[keptTricks++]);
                    zge::memoryCopy(copy, trick, sizeof(ScoreTrick));
                    copy->next = {};
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
        bool matchesKeptLine(const ScoreBlock &score)
        {
            if (*g_keptLineCount == 0)
                return true;
            uint32_t index = 0;
            do
            {
                const ScoreLine &kept = (*g_keptLines)[index];
                const ScoreLine &line = score.lines[index];
                ee::Ptr<ScoreTrick> trick = line.tricks;
                ee::Ptr<ScoreTrick> wanted = kept.tricks;
                ++index;
                while (trick && wanted)
                {
                    if (wanted->trick != rider::kNoTrick && trick->trick != wanted->trick)
                        return false;
                    if (wanted->modifier != rider::kNoTrick && trick->modifier != wanted->modifier)
                        return false;
                    if (kept.gap != rider::kNoGap && line.gap != kept.gap)
                        return false;
                    if (line.spin < kept.spin)
                        return false;
                    trick = trick->next;
                    wanted = wanted->next;
                }
            } while (index < *g_keptLineCount);
            return true;
        }

        uint32_t runTricks(const ScoreBlock &score)
        {
            uint32_t tricks = 0;
            const uint32_t lines = score.lineCount;
            for (uint32_t index = 0; index < lines; ++index)
            {
                for (ee::Ptr<ScoreTrick> trick = score.lines[index].tricks; trick; trick = trick->next)
                    ++tricks;
            }
            return tricks;
        }
    }

    // trickLineEnd: the run of lines ends, landed or lost in a bail. In
    // play, a bail is counted and crashes the trick display; a landed run
    // has its tricks counted, scores the turn (HORSE: the line kept, or the
    // turn scored 1 when it matches; the modes 8 .. 16 nothing; the others,
    // when they judge a turn's score, its points) and checks the game's
    // records, and the trick display fades the points out. The measure and
    // balance displays are put back and the run's lines cleared.
    void trickLineEnd(uint32_t player, uint32_t landed)
    {
        ee::Local<TrickDisplayMessage> shown;
        ee::Local<RewardItemMessage> reward;
        ee::Local<RewardItemMessage> noMeasure;
        ee::Local<BalanceMessage> balance;

        player &= 0xFF;
        const ee::Ptr<GameMode> mode = gameModeRecord();
        rider::Rider *const rider = &*playerRider(player);
        ScoreBlock &score = rider->score;

        if (cameraState() == kCameraStatePlay)
        {
            const uint32_t cell = riderCell(rider);
            int16_t *const cellCounts = score.cellTrickCounts[cell];
            int16_t *const levelCounts = score.cellTrickCounts[kCells];

            if (landed == 0)
            {
                soundEvent(kSoundLineEnd, 0, 0, 0, 0);
                ++cellCounts[kCountBails];
                ++levelCounts[kCountBails];
                shown->action = kTrickDisplayCrash;
            }
            else
            {
                score.kindScores[0] = 0.0f;
                score.kindScores[1] = 0.0f;
                score.kindScores[2] = 0.0f;
                score.kindScores[3] = 0.0f;
                for (uint32_t index = 0; index < score.lineCount; ++index)
                {
                    for (ee::Ptr<ScoreTrick> trick = score.lines[index].tricks; trick; trick = trick->next)
                        countLandedTrick(player, score, *trick, cellCounts, levelCounts);
                }

                // The turn's score, by the mode.
                const uint8_t now = gameMode();
                if (now == kHorseMode)
                {
                    if (roundFlags(kFirstTurnFlag))
                        keepLineToMatch(score);
                    else if (matchesKeptLine(score))
                        score.turnScore = 1.0f;
                }
                else if (!(now >= kFirstRecordScoredMode && now <= kLastRecordScoredMode) && mode->scored != 0)
                {
                    score.turnScore = score.turnScore + zge::unsignedToFloat(score.linePoints);
                    checkGameRecord(player, kRecordRunScore, kRewardRunScore, kAnnounceOnce, score.turnScore);
                    checkGameRecord(player, kRecordStackScore, kRewardStackScore, 0, zge::unsignedToFloat(score.linePoints));
                    for (uint32_t index = 0; index < score.lineCount; ++index)
                        checkGameRecord(player, kRecordTrickScore, kRewardTrickScore, 0, zge::unsignedToFloat(score.lines[index].points));
                }

                const uint32_t tricks = runTricks(score);
                if (tricks > kMostTricksUncounted)
                {
                    reward->player = static_cast<uint8_t>(player);
                    reward->kind = kRewardItemCombo;
                    reward->value = zge::unsignedToFloat(tricks);
                    overlayMessage(kOverlayReward, reward);
                }
                checkGameRecord(player, kRecordCombo, kRewardCombo, 0, zge::unsignedToFloat(tricks));
                checkGameRecord(player, kRecordGrindDistance, kRewardGrindDistance, 0, score.kindScores[0]);
                checkGameRecord(player, kRecordManualDistance, kRewardManualDistance, 0, score.kindScores[1]);
                checkGameRecord(player, kRecordNoseWheelieDistance, kRewardNoseWheelieDistance, 0, score.kindScores[2]);
                checkGameRecord(player, kRecordWallrideDistance, kRewardWallrideDistance, 0, score.kindScores[3]);
                for (uint32_t index = 0; index < score.lineCount; ++index)
                    checkGameRecord(player, kRecordSpin, kRewardSpin, 0, ee::toFloat(static_cast<int32_t>(score.lines[index].spin)));

                soundEvent(kSoundLineEnd, 1, score.linePoints, 0, 0);
                shown->action = kTrickDisplayScoreFadeOut;
            }
            shown->player = static_cast<uint8_t>(player);
            shown->score = ee::Ptr<ScoreBlock>(&score);
            overlayMessage(kOverlayTrick, shown);
        }

        // The measure and balance displays put back, the run's lines
        // cleared.
        noMeasure->player = static_cast<uint8_t>(player);
        noMeasure->value = 0.0f;
        overlayMessage(kOverlayMeasure, noMeasure);
        balance->player = static_cast<uint8_t>(player);
        balance->flags = kBalanceReset;
        overlayMessage(kOverlayBalance, balance);
        score.kindScores[3] = 0.0f;
        score.lineState = kLineNone;
        score.lineCount = 0;
        score.trickCount = 0;
        score.kindScores[0] = 0.0f;
        score.kindScores[1] = 0.0f;
        score.kindScores[2] = 0.0f;
    }
    READABLE_FUNCTION(trickLineEnd, Verified);
}
