// BMX XXX's score frame (score.h): a player's scoring each frame of play.
// BMX XXX's own code (game.c, its game block). Most modes score the trick
// line (trickScoreFrame, DM2's line: the turn's current line goes through
// its states, LineState, as the rider tricks, lands and bails); the
// wipeout and the paintball have their own, and the air's height
// and the skid's length go to their records and the overlay.

#include "score.h"

#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"
#include "trick_types.h"

#include "bmx/overlay/displays.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/vec.h"

#include <cstdint>
#include "sym/bmx/game/score_frame.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The wipeout (round.h's kWipeoutMode) and the paintball score their
        // own way.
        // The modes 9 .. 16 score the turn by their own record's value.
        constexpr uint8_t kFirstRecordScoredMode = 9;
        constexpr uint8_t kLastRecordScoredMode = 16;
        // The wipeout's score: the crash's ride distance in hundredths.
        constexpr int32_t kDistanceScoreScale = 100;

        // The records of the air's height and of a skid's length, and the
        // reward display's items for beating their saved records.
        constexpr uint32_t kAirHeightRecord = 6;
        constexpr uint32_t kSkidRecord = 0x0C;
        constexpr uint32_t kAirHeightRecordItem = 0x18;
        constexpr uint32_t kSkidRecordItem = 0x16;
        // The overlay's display of the air's height and the skid's length as
        // they go (display 8, which the overlay's code does not name yet).
        constexpr uint32_t kOverlayMeasure = 8;
        // The riding action's sub-state of a skid.
        constexpr uint8_t kRidingSkid = 2;

        // The coast's start (rider::kFlagCoastStarted) ends the line at once
        // as a bail of this kind.
        constexpr uint32_t kForcedBailKind = 11;

        // A line closes when the rider goes from a trick done on something
        // (a grind's type or past it) to another move; the air's tricks
        // (and none) keep it open.
        constexpr uint32_t kFirstSurfaceType = kTrickGrind;
        // At most 127 lines open in a turn.
        constexpr uint8_t kMostLines = 0x7F;
        // The sound of a line opening (the game bank's chime, pitched by
        // its first word), told which line it is.
        constexpr uint32_t kSoundLineOpen = 57;
        constexpr uint32_t kLineOpenPitch = 2;
        // A landed line is cheered from the turn's third line, or for more
        // than 4000 points.
        constexpr uint8_t kLinesCheeredByPoints = 3;
        constexpr uint32_t kCheerFromPoints = 4000;

        // An Angle's half turn and whole turn; the most half turns that
        // keep a line from breaking and that the line's spin counts.
        constexpr uint32_t kHalfTurn = 0x8000;
        constexpr uint32_t kWholeTurn = 0x10000;
        constexpr uint32_t kMostHalfTurnsToBreak = 7;
        constexpr uint32_t kMostSpinHalfTurns = 8;
        constexpr uint8_t kMostTrickTurns = 3;

        // A turning air trick landed is recorded as the trick its whole
        // turns make it (DM2's): a turn of kinds 2 and 3 trick 3, of kind 5
        // trick 10, two turns trick 8, none no trick.
        constexpr uint32_t kTurningKindsOneTurnFrom = 2, kTurningKindsOneTurnEnd = 4, kTurningKindOther = 5;
        constexpr uint8_t kOneTurnTrick = 3, kOneTurnOtherTrick = 10, kTwoTurnTrick = 8;
        // The tricks 0x43 .. 0x47 keep the line's longest hold.
        constexpr uint8_t kFirstHeldTrick = 0x43;
        constexpr uint8_t kHeldTricks = 5;

        // The air's second level with the trick buttons' tricks, and the
        // phase of a held one.
        constexpr uint8_t kAirTrickHeldPhase = 2;
        // A grind's or stall's phase from which it is held.
        constexpr uint8_t kSurfaceHeldPhase = 3;
        constexpr uint8_t kContactGround = 1; // Rider::contactKind

        uint8_t action(const rider::Rider *rider)
        {
            return rider::actionState(rider, rider::kLevelAction);
        }

        // The block's last line (the one before the first when it has none,
        // as the original reads it).
        ScoreLine &currentLine(ee::Ptr<ScoreBlock> score)
        {
            return *(ee::Ptr<ScoreLine>(&score->lines[0]) + (static_cast<int32_t>(score->lineCount) - 1));
        }

        // The line's last trick (0 when it has none).
        ee::Ptr<ScoreTrick> lastTrickOf(const ScoreLine &line)
        {
            ee::Ptr<ScoreTrick> trick = line.tricks;
            if (trick)
            {
                while (trick->next)
                    trick = trick->next;
            }
            return trick;
        }

        // The air's yaw so far in half turns, rounded, at most `most`.
        uint32_t spinHalfTurns(const rider::Rider *rider, uint32_t most)
        {
            const uint32_t yaw = static_cast<uint32_t>(rider->yawTurned);
            const uint32_t turned = rider->yawTurned < 0 ? 0u - yaw : yaw;
            const uint32_t halfTurns = (turned + kHalfTurn / 2) / kHalfTurn;
            return halfTurns > most ? most : halfTurns;
        }

        bool riderHasNoTrick(const rider::Rider *rider)
        {
            return rider->trick == rider::kNoTrick && rider->trickMotion == rider::kNoTrick;
        }
        bool trickIsEmpty(const ScoreTrick &trick)
        {
            return trick.trick == rider::kNoTrick && trick.modifier == rider::kNoTrick;
        }

        void tellTrickDisplay(ee::Local<TrickDisplayMessage> &message, uint8_t action, uint32_t player,
                              ee::Ptr<ScoreBlock> score)
        {
            message->player = static_cast<uint8_t>(player);
            message->action = action;
            message->score = score;
            overlayMessage(kOverlayTrick, message);
        }

        // A line landed: ended, and cheered from the turn's third line or
        // for its points.
        void endLandedLine(uint32_t player, ee::Ptr<ScoreBlock> score)
        {
            trickLineEnd(player, 1);
            if (score->lineCount >= kLinesCheeredByPoints || score->linePoints > kCheerFromPoints)
                lineReaction(1, score);
        }
        // A line lost in a bail.
        void endBailedLine(uint32_t player, ee::Ptr<ScoreBlock> score)
        {
            trickLineEnd(player, 0);
            lineReaction(0, score);
        }

        // An open line breaks off (back to kLineMoveSeen, the name fading
        // out) when its last trick was done on something, or the rider's
        // trick is not an air trick, unless the rider has neither acted nor
        // spun: an air trick (or none) last keeps it as it is. Else the line
        // waits for a trick, or follows the one it has.
        void breakLineAtNewMove(uint32_t player, rider::Rider *rider, ee::Ptr<ScoreBlock> score,
                                ee::Local<TrickDisplayMessage> &trickMessage)
        {
            const ee::Ptr<ScoreTrick> last = lastTrickOf(currentLine(score));
            const uint32_t lastType = trickType(last->trick);
            if (!rider::hasFlag(rider, kFlagActed))
            {
                if (lastType < kFirstSurfaceType)
                    return;
                if (spinHalfTurns(rider, kMostHalfTurnsToBreak) == 0)
                    return;
            }
            // (trickType never answers kNoTrick: a rider with no trick breaks
            // the line too.)
            if (lastType >= kFirstSurfaceType ||
                (trickType(rider->trick) != rider::kNoTrick && trickType(rider->trick) != kTrickAir))
            {
                score->lineState = kLineMoveSeen;
                tellTrickDisplay(trickMessage, kTrickDisplayNameFadeOut, player, score);
            }
            else
            {
                score->lineState = trickIsEmpty(*last) ? kLineTrick : kLineOpen;
            }
        }

        // As a line opens, the balance display shown for a grind or a stall
        // (not for a trick with no balance; turned round by kFlagBalanceTurnedRound, and
        // the wheelie's display turned round for a trick whose lean is about
        // the right row) or for a wheelie or the flatland.
        void showBalance(uint32_t player, const rider::Rider *rider, ee::Local<BalanceMessage> &balance)
        {
            balance->player = static_cast<uint8_t>(player);
            balance->flags = kBalanceShow;
            if (action(rider) == rider::kActionGrind || action(rider) == rider::kActionStall)
            {
                if (((*g_trickFlags)[rider->trick] & kTrickNoBalance) != 0)
                    return;
                balance->value = rider->grindBalance;
                if (rider::hasFlag(rider, rider::kFlagBalanceTurnedRound))
                    balance->flags = balance->flags | kBalanceTurnedRound;
                if (((*g_trickFlags)[rider->trick] & kTrickRowsByRight) != 0)
                    balance->flags = balance->flags | kBalanceTurnedRound | kBalanceWheelie;
                overlayMessage(kOverlayBalance, balance);
            }
            else if (action(rider) == rider::kActionWheelie || action(rider) == rider::kActionFlatland)
            {
                balance->flags = balance->flags | kBalanceWheelie;
                balance->value = rider->wheelieBalance;
                overlayMessage(kOverlayBalance, balance);
            }
        }

        // Each frame of a line: the balance display moved with the grind's
        // or the wheelie's balance, else hidden.
        void moveBalance(uint32_t player, const rider::Rider *rider, ee::Local<BalanceMessage> &balance)
        {
            balance->player = static_cast<uint8_t>(player);
            balance->flags = 0;
            if (action(rider) == rider::kActionGrind || action(rider) == rider::kActionStall)
            {
                if (((*g_trickFlags)[rider->trick] & kTrickNoBalance) != 0)
                    return;
                balance->value = rider->grindBalance;
            }
            else if (action(rider) == rider::kActionWheelie || action(rider) == rider::kActionFlatland)
            {
                balance->value = rider->wheelieBalance;
            }
            else
            {
                // (The value is left as the frame had it: hiding reads none.)
                balance->flags = kBalanceHide;
            }
            overlayMessage(kOverlayBalance, balance);
        }

        // A turning air trick landed, recorded as the trick its whole turns
        // make it.
        bool nameTurningTrick(ScoreTrick &trick)
        {
            if (trick.trick == rider::kNoTrick || trickType(trick.trick) != kTrickAir)
                return false;
            const uint32_t kind = (*g_trickFlags)[trick.trick] & kTrickTurningKind;
            if (kind == 0)
                return false;
            if (trick.turns == 1)
            {
                if (kind < kTurningKindsOneTurnFrom)
                    return false;
                if (kind < kTurningKindsOneTurnEnd)
                    trick.trick = kOneTurnTrick;
                else if (kind == kTurningKindOther)
                    trick.trick = kOneTurnOtherTrick;
                else
                    return false;
            }
            else if (trick.turns == 0)
            {
                trick.trick = rider::kNoTrick;
            }
            else if (trick.turns == 2)
            {
                trick.trick = kTwoTurnTrick;
            }
            else
            {
                return false;
            }
            return true;
        }

        // Whether the rider holds the trick this frame: riding, and past the
        // grind's and stall's, always; a grind or a stall from its held
        // phase; in the air a trick button's trick in its held phase (the
        // first button's only for a trick that neither turns nor loops
        // once).
        bool trickHeld(const rider::Rider *rider)
        {
            const uint8_t now = action(rider);
            if (now == rider::kActionAir)
            {
                const uint8_t airAction = rider::actionState(rider, rider::kLevelSubState);
                if (airAction == rider::kAirTrick)
                {
                    return ((*g_trickFlags)[rider->trick] & kTrickTurningKind) == 0 &&
                           rider::actionState(rider, rider::kLevelPhase) == kAirTrickHeldPhase &&
                           ((*g_trickFlags)[rider->trick] & kTrickNotHeld) == 0;
                }
                if (airAction == rider::kAirTrickB)
                    return rider::actionState(rider, rider::kLevelPhase) == kAirTrickHeldPhase;
                return false;
            }
            if (now == rider::kActionRiding || now >= rider::kActionLipTrick)
                return true;
            return rider::actionState(rider, rider::kLevelPhase) >= kSurfaceHeldPhase;
        }

        // The line's last trick follows the rider's: what it is, its turns,
        // the frames it is held, a grind's, lip trick's or wheelie's
        // distance, a grind ridden backwards, the longest hold.
        void followTrick(rider::Rider *rider, ScoreBlock &score, ScoreTrick &trick)
        {
            trick.trick = rider->trick;
            trick.modifier = rider->trickMotion;
            trick.loops = rider->trickLoops;
            const uint32_t turn = static_cast<uint32_t>(rider->trickTurned);
            const uint32_t turned = rider->trickTurned < 0 ? 0u - turn : turn;
            const uint32_t turns = (turned + kWholeTurn / 2) / kWholeTurn;
            trick.turns = static_cast<uint8_t>(turns > kMostTrickTurns ? kMostTrickTurns : turns);
            if (trickHeld(rider))
                trick.framesHeld = static_cast<uint16_t>(trick.framesHeld + 1);
            if (rider::physicsState(rider) == rider::kPhysicsGrind || action(rider) == rider::kActionWheelie ||
                action(rider) == rider::kActionLipTrick)
            {
                trick.distance = trick.distance + rider->speed;
            }
            if (trickType(rider->trick) == kTrickGrind && rider::hasFlag(rider, rider::kFlagFakie))
                trick.flags |= rider::kScoreTrickAlleyOop;
            if (static_cast<uint8_t>(trick.trick - kFirstHeldTrick) < kHeldTricks && score.longestHold < rider->groundHeight)
                score.longestHold = rider->groundHeight;
        }

        // The line's spin follows the air's (none while the alignment turns
        // it); a spin after a take-off rolling backwards marks the trick, and
        // an air trick's spin off a lip is marked by which way it turns
        // against the rider's travel.
        void followSpin(rider::Rider *rider, ScoreLine &line, ScoreTrick &trick)
        {
            const uint32_t spin = rider::hasFlag(rider, kFlagAlignAbout) ? 0 : spinHalfTurns(rider, kMostSpinHalfTurns);
            line.spin = static_cast<uint8_t>(spin);
            if (rider::hasFlag(rider, kFlagTakeoffBack) && line.spin != 0)
            {
                trick.flags |= rider::kScoreTrickBackTakeoff;
                rider::clearFlag(rider, kFlagTakeoffBack);
            }
            if (trick.trick != rider::kNoTrick && trickType(trick.trick) != kTrickAir)
                return;
            if (line.spin == 0 || rider->contactKind != kContactGround || !rider::hasFlag(rider, rider::kFlagLipLanding))
                return;
            const zge::Vec4 down{0.0f, -1.0f, 0.0f, 0.0f};
            const zge::Vec4 across = zge::cross(down, *rider::vec4(rider->up));
            const bool travelsAgainst = zge::dot(*rider::vec4(rider->velocity), across) < ee::Float(0.0f);
            if (rider->yawTurned > 0 ? !travelsAgainst : travelsAgainst)
                line.flags |= rider::kScoreLineSpinFlag;
        }
    }

    // trickScoreFrame: the trick line's frame. A line ended at once by the
    // coast's start (kFlagCoastStarted) is a bail; an open line breaks off at a new move. Then the
    // line moves on: a move seen, a line opened (its sound, the trick and
    // balance displays), a trick added, the trick followed; each landed
    // (cheered) or lost in a bail as the rider lands or bails. While not
    // landing, the line is scored and the trick and balance displays told.
    void trickScoreFrame(uint32_t player)
    {
        ee::Local<TrickDisplayMessage> trickMessage;
        ee::Local<BalanceMessage> balance;
        ee::Local<RewardItemMessage> forcedBail;

        player &= 0xFF;
        rider::Rider *const rider = &*playerRider(player);
        const ee::Ptr<ScoreBlock> score(&rider->score);

        if (rider::hasFlag(rider, rider::kFlagCoastStarted) && score->lineState != kLineNone)
        {
            endBailedLine(player, score);
            forcedBail->kind = kRewardItemBail;
            forcedBail->value = ee::toFloat(static_cast<int32_t>(kForcedBailKind));
            forcedBail->player = static_cast<uint8_t>(player);
            overlayMessage(kOverlayReward, forcedBail);
        }
        if (score->lineState >= kLineOpen)
            breakLineAtNewMove(player, rider, score, trickMessage);

        switch (score->lineState)
        {
        case kLineNone:
            if (!riderHasMoveToScore(rider))
                return;
            score->unreadLineByte = 0;
            score->lineState = kLineMoveSeen;
            score->longestHold = 0.0f;
            [[fallthrough]];
        case kLineMoveSeen:
            if (!riderHasMoveToScore(rider) || score->lineCount >= kMostLines)
            {
                const uint8_t now = action(rider);
                if (now == rider::kActionRiding)
                    endLandedLine(player, score);
                else if (now == rider::kActionBail)
                    endBailedLine(player, score);
                return;
            }
            openScoreLine(score);
            score->lineState = kLineOpen;
            soundEvent(kSoundLineOpen, kLineOpenPitch, score->lineCount - 1u, 0, 0);
            tellTrickDisplay(trickMessage, kTrickDisplayTrickBegun, player, score);
            showBalance(player, rider, balance);
            [[fallthrough]];
        case kLineOpen:
        {
            const ScoreLine &line = currentLine(score);
            if (line.tricks && !rider::hasFlag(rider, kFlagActed) && riderHasNoTrick(rider))
            {
                const uint8_t now = action(rider);
                if (now == rider::kActionRiding)
                {
                    tellTrickDisplay(trickMessage, kTrickDisplayNameFadeOut, player, score);
                    endLandedLine(player, score);
                }
                else if (now == rider::kActionBail)
                {
                    endBailedLine(player, score);
                }
                return;
            }
            addScoreTrick(score);
            score->lineState = kLineTrick;
            [[fallthrough]];
        }
        case kLineTrick:
        case kLineSpin:
        {
            ScoreLine &line = currentLine(score);
            const ee::Ptr<ScoreTrick> last = lastTrickOf(line);
            const uint8_t now = action(rider);
            if (now == rider::kActionAir)
            {
                // The trick is over while the air goes on.
                if (score->lineState == kLineTrick && riderHasNoTrick(rider) && !trickIsEmpty(*last))
                    score->lineState = line.spin != 0 ? kLineSpin : kLineOpen;
            }
            else if (now == rider::kActionRiding)
            {
                if (nameTurningTrick(*last))
                {
                    scoreLines(player);
                    tellTrickDisplay(trickMessage, kTrickDisplayTrickText, player, score);
                }
                tellTrickDisplay(trickMessage, kTrickDisplayNameFadeOut, player, score);
                endLandedLine(player, score);
            }
            else if (now == rider::kActionBail)
            {
                endBailedLine(player, score);
            }

            if (action(rider) == rider::kActionLanding)
                return;
            if (score->lineState == kLineTrick)
            {
                followTrick(rider, *score, *last);
                followSpin(rider, line, *last);
            }
            else if (score->lineState == kLineSpin)
            {
                followSpin(rider, line, *last);
            }
            scoreLines(player);
            tellTrickDisplay(trickMessage, kTrickDisplayTrickText, player, score);
            moveBalance(player, rider, balance);
            return;
        }
        default:
            return;
        }
    }
    READABLE_FUNCTION(trickScoreFrame, Verified);


    // wipeoutScoreFrame: the wipeout's scoring ("the most bodily harm in a
    // single crash"): the line opens as the rider's physics bails (the
    // score's open-line byte cleared), and while the bail lasts the turn's
    // score is the ride's distance in hundredths; out of it the line closes.
    void wipeoutScoreFrame(uint32_t player)
    {
        const ee::Ptr<rider::Rider> rider = playerRider(player & 0xFF);
        rider::ScoreBlock &score = rider->score;
        if (score.lineState == 0)
        {
            if (rider::physicsState(&*rider) == rider::kPhysicsBail)
            {
                score.unreadLineByte = 0;
                score.lineState = 1;
            }
        }
        else if (score.lineState == 1)
        {
            if (rider::physicsState(&*rider) != rider::kPhysicsBail)
                score.lineState = 0;
            else
                score.turnScore = ee::toFloat(rider->rideDistance * kDistanceScoreScale);
        }
    }
    READABLE_FUNCTION(wipeoutScoreFrame, Verified);

    // paintballScoreFrame: the paintball scores nothing here (the player's
    // rider is looked up and not used).
    void paintballScoreFrame(uint32_t player)
    {
        playerRider(player & 0xFF);
    }
    READABLE_FUNCTION(paintballScoreFrame, Verified);

    // playerScoreFrame: a player's scoring for the frame: the mode's own
    // (the trick line's in most), the score display; in the air, over the
    // front wheel's last contact, the height on the measure display (rising)
    // or against its record (falling); riding, a skid's length on the
    // measure display, and once the skid is over against its record. The
    // modes 9 .. 16 take the turn's score from their own record.
    void playerScoreFrame(uint32_t player)
    {
        ee::Local<ValueMessage> scoreMessage;
        ee::Local<RewardItemMessage> measure;

        player &= 0xFF;
        const ee::Ptr<rider::Rider> rider = playerRider(player);
        rider::ScoreBlock &score = rider->score;
        const uint8_t mode = gameMode();
        if (mode == kWipeoutMode)
            wipeoutScoreFrame(player);
        else if (mode == kPaintballMode)
            paintballScoreFrame(player);
        else
            trickScoreFrame(player);

        scoreMessage->player = static_cast<uint8_t>(player);
        scoreMessage->value = score.turnScore;
        scoreMessage->flags = 0;
        overlayMessage(kOverlayScore, scoreMessage);

        if (rider::actionState(&*rider, rider::kLevelAction) == rider::kActionAir && !rider::hasFlag(&*rider, rider::kFlagLeftRail) &&
            rider->position[1] < rider->wheels[0].point[1])
        {
            if (rider->velocity[1] <= ee::Float(0.0f))
            {
                measure->player = static_cast<uint8_t>(player);
                measure->kind = static_cast<uint8_t>(kAirHeightRecord);
                measure->value = rider->wheels[0].point[1] - rider->position[1];
                overlayMessage(kOverlayMeasure, measure);
            }
            else
            {
                checkGameRecord(player, kAirHeightRecord, kAirHeightRecordItem, 0, rider->wheels[0].point[1] - rider->position[1]);
            }
        }

        if (rider::actionState(&*rider, rider::kLevelAction) == rider::kActionRiding)
        {
            if (rider::actionState(&*rider, rider::kLevelSubState) == kRidingSkid)
            {
                measure->player = static_cast<uint8_t>(player);
                measure->kind = static_cast<uint8_t>(kSkidRecord);
                measure->value = rider->skidDistance;
                overlayMessage(kOverlayMeasure, measure);
            }
            else
            {
                const ee::Float skid = rider->skidDistance;
                if (skid != ee::Float(0.0f))
                {
                    checkGameRecord(player, kSkidRecord, kSkidRecordItem, 0, skid);
                    rider->skidDistance = 0.0f;
                }
                // The display told of no skid (the message's kind is left as
                // the frame had it, as the original's).
                measure->player = static_cast<uint8_t>(player);
                measure->value = 0.0f;
                overlayMessage(kOverlayMeasure, measure);
            }
        }

        const uint8_t now = gameMode();
        if (now >= kFirstRecordScoredMode && now <= kLastRecordScoredMode)
        {
            const uint32_t own = gameMode();
            const uint32_t kind = ee::Ptr<uint8_t>(sym::g_modeRecordSlots)[gameMode()];
            score.turnScore = gameRecord(recordKey(own, kind, kTurnRecords))->value;
        }
    }
    READABLE_FUNCTION(playerScoreFrame, Verified);
}
