// DM2's trick scoring (game.c): a rider's scoring each frame. The turn's
// current trick line goes through its states (score.h's LineState) as the
// rider tricks, lands and bails; the overlay is told the score, the ruler's
// measure (an air's height, a skid's length) and the gauge's value (a
// wheelie's balance).

#include "score.h"
#include "players.h"
#include "records.h"
#include "round.h"

#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"

#include <cstdint>
#include "sym/dm2/game/score_frame.cpp.h"

namespace game::dm2
{
    namespace
    {
        // Rider::flags the scoring reads (the rider's files name them, each
        // for itself: inferred there).
        constexpr uint32_t kFlagActed = 0x100;             // an action was taken in the air
        constexpr uint32_t kFlagAlignAbout = 0x4000;       // the alignment turns the air: its yaw is no spin
        constexpr uint32_t kFlagOffLip = 0x8000;           // the air left a lip

        // Tricks from this one on are done on something (grinds, stalls,
        // lip tricks, wheelies); the ones below it are the air's.
        constexpr uint8_t kFirstSurfaceTrick = 0x33;
        constexpr uint8_t kGrindTricks = 9;                // 0x33 .. 0x3B
        constexpr uint8_t kHeldTricks = 3;                 // kFirstHeldTrick 0x43 .. 0x45: their longest hold is kept
        // What each trick is (a word a trick): the low three bits a turning
        // trick's kind (0: none), kTrickNotHeld a trick whose frames in the
        // air's second action do not count as held.
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickKinds;
        constexpr uint16_t kTurningKindBits = 0x7;
        constexpr uint16_t kTrickNotHeld = 0x20;
        // A turning trick is recorded, as its line ends on the ground, as
        // the trick its whole turns make it: none without a whole turn.
        constexpr uint8_t kTurningKindsOneTurnFrom = 2, kTurningKindsOneTurnEnd = 4, kTurningKindOther = 5;
        constexpr uint8_t kOneTurnTrick = 3, kOneTurnOtherTrick = 10, kTwoTurnTrick = 8;
        constexpr uint8_t kMostTrickTurns = 3;

        // An Angle's half turn and whole turn.
        constexpr uint32_t kHalfTurn = 0x8000;
        constexpr uint32_t kWholeTurn = 0x10000;
        constexpr uint32_t kMostHalfTurnsToBreak = 7;
        constexpr uint32_t kMostSpinHalfTurns = 8;

        constexpr uint32_t kMostOpenLines = 31;
        // The sounds of a line: opened (with which line it is), ended (a
        // cheer by its points when it was landed; a bail's).
        constexpr uint32_t kLineOpenSound = 0x28;
        constexpr uint32_t kLineEndSound = 0x2A;
        constexpr uint32_t kLineSoundCode = 2;
        constexpr uint32_t kCheerFromPoints = 4000;
        constexpr ee::Float kCheersPerPoint = 0.0005f;
        constexpr int32_t kLoudestCheer = 5;
        constexpr uint8_t kLinesCheeredByPoints = 3; // a turn's first two lines are cheered only over kCheerFromPoints

        // Multiplayer mode 5 scores how far a bail carries the rider.
        constexpr uint8_t kBailDistanceMode = 5;
        constexpr int32_t kPointsPerDistance = 100;
        // The modes whose turn score is the turn's record of the mode's own kind.
        constexpr uint8_t kFirstRecordScoreMode = 6, kLastRecordScoreMode = 13;

        constexpr uint8_t kActionSkid = 2;   // the riding action's second level: skidding
        constexpr uint8_t kContactGround = 1; // Rider::contactKind


        // The line's last trick (none: 0).
        ee::Ptr<rider::ScoreTrick> lastTrickOf(const rider::ScoreLine &line)
        {
            ee::Ptr<rider::ScoreTrick> trick = line.tricks;
            if (trick)
            {
                while (trick->next)
                    trick = trick->next;
            }
            return trick;
        }

        // The air's yaw so far in half turns, rounded.
        uint32_t spinHalfTurns(const rider::Rider &rider, uint32_t most)
        {
            const uint32_t turned = static_cast<uint32_t>(rider.yawTurned < 0 ? -rider.yawTurned : rider.yawTurned);
            const uint32_t halfTurns = (turned + kHalfTurn / 2) / kHalfTurn;
            return halfTurns > most ? most : halfTurns;
        }

        bool riderHasNoTrick(const rider::Rider &rider)
        {
            return rider.trick == rider::kNoTrick && rider.trickMotion == rider::kNoTrick;
        }
        bool surfaceTrick(uint8_t trick)
        {
            return trick != rider::kNoTrick && trick >= kFirstSurfaceTrick;
        }

        void tellTrickDisplay(ee::Local<OverlayTrick> &message, uint32_t action, rider::ScoreBlock &score)
        {
            message->action = action;
            message->turn = ee::Address(&score.turnScore);
            overlayMessage(kOverlayTrick, message);
        }

        // A landed line's cheer: louder with its points over
        // kCheerFromPoints; none for a turn's first lines below that.
        void cheerLine(const rider::ScoreBlock &score)
        {
            if (score.lineCount < kLinesCheeredByPoints && score.linePoints <= kCheerFromPoints)
                return;
            const int32_t over = static_cast<int32_t>(score.linePoints - kCheerFromPoints);
            int32_t cheer = ee::toInt(ee::toFloat(over) * kCheersPerPoint);
            if (cheer > kLoudestCheer)
                cheer = kLoudestCheer;
            soundEvent(kLineEndSound, cheer, 1, 0, 0);
        }

        // The line ended by what the rider is doing: on the ground it is
        // landed and cheered, in a bail it is lost; anything else leaves it.
        void endLineByAction(uint32_t player, const rider::Rider *rider, rider::ScoreBlock &score,
                             ee::Local<OverlayTrick> *nameFadeOut)
        {
            const uint8_t action = rider::actionState(rider, rider::kLevelAction);
            if (action == rider::kActionRiding)
            {
                if (nameFadeOut)
                    tellTrickDisplay(*nameFadeOut, kTrickDisplayNameFadeOut, score);
                trickLineEnd(player, 1);
                cheerLine(score);
            }
            else if (action == rider::kActionBail)
            {
                trickLineEnd(player, 0);
                soundEvent(kLineEndSound, kLineSoundCode, 0, 0, 0);
            }
        }

        // An open line breaks off when the rider goes from one surface
        // trick to another move (a line is one run): back to kLineMoveSeen,
        // the name fading out. Else the line waits for a trick, or follows
        // the one it has.
        void breakLineAtNewMove(const rider::Rider &rider, rider::ScoreBlock &score, ee::Local<OverlayTrick> &message)
        {
            const ee::Ptr<rider::ScoreTrick> last = lastTrickOf(score.lines[score.lineCount - 1]);
            const uint8_t trick = last->trick;
            if ((rider.flags & kFlagActed) == 0)
            {
                if (!surfaceTrick(trick))
                    return;
                if (spinHalfTurns(rider, kMostHalfTurnsToBreak) == 0)
                    return;
            }
            if (surfaceTrick(trick) || surfaceTrick(rider.trick))
            {
                score.lineState = kLineMoveSeen;
                tellTrickDisplay(message, kTrickDisplayNameFadeOut, score);
            }
            else if (last->trick == rider::kNoTrick && last->trickMotion == rider::kNoTrick)
            {
                score.lineState = kLineTrick;
            }
            else
            {
                score.lineState = kLineOpen;
            }
        }

        // A turning air trick recorded as the trick its whole turns make it.
        bool nameTurningTrick(rider::ScoreTrick &trick)
        {
            const uint16_t kind = (*g_trickKinds)[trick.trick] & kTurningKindBits;
            if (kind == 0)
                return false;
            if (trick.turns == 1)
            {
                if (kind >= kTurningKindsOneTurnFrom && kind < kTurningKindsOneTurnEnd)
                    trick.trick = kOneTurnTrick;
                else if (kind == kTurningKindOther)
                    trick.trick = kOneTurnOtherTrick;
                else
                    return false;
            }
            else if (trick.turns == 2)
            {
                trick.trick = kTwoTurnTrick;
            }
            else if (trick.turns == 0)
            {
                trick.trick = rider::kNoTrick;
            }
            else
            {
                return false;
            }
            return true;
        }

        // The line's last trick follows the rider's: what it is, its turns,
        // the frames it is held, a grind's or wheelie's distance.
        void followTrick(rider::Rider *rider, rider::ScoreBlock &score, rider::ScoreTrick &trick)
        {
            trick.trick = rider->trick;
            trick.trickMotion = rider->trickMotion;
            trick.loops = rider->trickLoops;
            const uint32_t turned = static_cast<uint32_t>(rider->trickTurned < 0 ? -rider->trickTurned : rider->trickTurned);
            const uint32_t turns = (turned + kWholeTurn / 2) / kWholeTurn;
            trick.turns = static_cast<uint8_t>(turns > kMostTrickTurns ? kMostTrickTurns : turns);

            bool held;
            const uint8_t action = rider::actionState(rider, rider::kLevelAction);
            if (action == rider::kActionAir)
            {
                const uint8_t airAction = rider::actionState(rider, 1);
                if (airAction == 1)
                {
                    held = ((*g_trickKinds)[rider->trick] & kTurningKindBits) == 0 && rider::actionState(rider, 2) == 2 &&
                           ((*g_trickKinds)[rider->trick] & kTrickNotHeld) == 0;
                }
                else if (airAction == 2)
                {
                    held = rider::actionState(rider, 2) == 2;
                }
                else
                {
                    held = false;
                }
            }
            else if (action == rider::kActionRiding || action >= rider::kActionLipTrick)
            {
                held = true;
            }
            else
            {
                held = rider::actionState(rider, 2) >= 3;
            }
            if (held)
                ++trick.framesHeld;

            if (rider::physicsState(rider) == rider::kPhysicsGrind ||
                rider::actionState(rider, rider::kLevelAction) == rider::kActionWheelie)
            {
                trick.distance = trick.distance + rider->speed;
            }
            if (static_cast<uint8_t>(rider->trick - kFirstSurfaceTrick) < kGrindTricks && (rider->flags & rider::kFlagFakie) != 0)
                trick.flags |= rider::kScoreTrickFakie;
            if (static_cast<uint8_t>(trick.trick - kFirstHeldTrick) < kHeldTricks && score.heldTrickHeight < rider->groundHeight)
                score.heldTrickHeight = rider->groundHeight;
        }

        // The line's spin follows the air's, and an air trick's spin off a
        // lip is marked by which way it turns against the rider's travel.
        void followSpin(rider::Rider *rider, rider::ScoreLine &line, rider::ScoreTrick &trick,
                        ee::Local<zge::Vec4> &across, ee::Local<zge::Vec4> &down)
        {
            const uint32_t spin = (rider->flags & kFlagAlignAbout) != 0 ? 0 : spinHalfTurns(*rider, kMostSpinHalfTurns);
            line.spinHalfTurns = static_cast<uint8_t>(spin);
            if ((rider->flags & rider::kFlagTakeoffBack) != 0 && spin != 0)
            {
                trick.flags |= rider::kScoreTrickBackTakeoff;
                rider->flags &= ~rider::kFlagTakeoffBack;
            }
            if (surfaceTrick(trick.trick))
                return;
            if (line.spinHalfTurns == 0 || rider->contactKind != kContactGround || (rider->flags & kFlagOffLip) == 0)
                return;
            down->x = 0.0f;
            down->y = -1.0f;
            down->z = 0.0f;
            *across = zge::cross(*down, *rider::vec4(rider->up));
            const bool travelsAgainst = zge::dot(*rider::vec4(rider->velocity), *across) < down->x;
            if (rider->yawTurned > 0 ? !travelsAgainst : travelsAgainst)
                line.flags |= rider::kScoreLineSpinFlag;
        }

        // Mode 5's score: while the rider is bailing, how far it has gone.
        void bailDistanceFrame(const rider::Rider *rider, rider::ScoreBlock &score)
        {
            if (score.lineState == kLineNone)
            {
                if (rider::physicsState(rider) == rider::kPhysicsBail)
                {
                    score.unreadLineByte = 0;
                    score.lineState = kLineMoveSeen;
                }
            }
            else if (score.lineState == kLineMoveSeen)
            {
                if (rider::physicsState(rider) != rider::kPhysicsBail)
                {
                    score.lineState = kLineNone;
                }
                else
                {
                    // (The product as an unsigned number.)
                    const int32_t points = rider->rideDistance * kPointsPerDistance;
                    if (points < 0)
                    {
                        const ee::Float half = ee::toFloat(static_cast<int32_t>(static_cast<uint32_t>(points) >> 1));
                        score.turnScore = half + half;
                    }
                    else
                    {
                        score.turnScore = ee::toFloat(points);
                    }
                }
            }
        }
    }

    void trickScoreFrame(uint32_t player)
    {
        ee::Local<zge::Vec4> across;
        ee::Local<zge::Vec4> down;
        ee::Local<OverlayTrick> trickMessage;
        ee::Local<OverlayScore> shownScore;
        ee::Local<OverlayRuler> measure;
        ee::Local<ee::Float> balance;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;

        if (multiplayerMode() == kBailDistanceMode)
        {
            bailDistanceFrame(rider, score);
        }
        else
        {
            if (score.lineState >= kLineOpen)
                breakLineAtNewMove(*rider, score, trickMessage);

            // The line's state, moved on as far as this frame takes it.
            bool lineGoesOn = score.lineState <= kLineSpin;
            if (lineGoesOn && score.lineState == kLineNone)
            {
                if (riderHasMoveToScore(rider) != 0)
                {
                    score.unreadLineByte = 0;
                    score.lineState = kLineMoveSeen;
                    score.heldTrickHeight = 0.0f;
                }
                else
                {
                    lineGoesOn = false;
                }
            }
            if (lineGoesOn && score.lineState == kLineMoveSeen)
            {
                if (riderHasMoveToScore(rider) != 0 && score.lineCount < kMostOpenLines)
                {
                    openScoreLine(&score);
                    score.lineState = kLineOpen;
                    soundEvent(kLineOpenSound, kLineSoundCode, score.lineCount - 1u, 0, 0);
                    tellTrickDisplay(trickMessage, kTrickDisplayTrickBegun, score);
                }
                else
                {
                    endLineByAction(player, rider, score, nullptr);
                    lineGoesOn = false;
                }
            }
            if (lineGoesOn && score.lineState == kLineOpen)
            {
                const rider::ScoreLine &line = score.lines[score.lineCount - 1];
                const bool wantsTrick = !line.tricks || (rider->flags & kFlagActed) != 0 || !riderHasNoTrick(*rider);
                if (wantsTrick && score.trickCount < rider::kScoreTricks)
                {
                    newScoreTrick(&score);
                    score.lineState = kLineTrick;
                }
                else
                {
                    endLineByAction(player, rider, score, &trickMessage);
                    lineGoesOn = false;
                }
            }
            if (lineGoesOn)
            {
                rider::ScoreLine &line = score.lines[score.lineCount - 1];
                const ee::Ptr<rider::ScoreTrick> last = lastTrickOf(line);
                const uint8_t action = rider::actionState(rider, rider::kLevelAction);
                if (action == rider::kActionAir)
                {
                    // The trick is over while the air goes on.
                    if (score.lineState == kLineTrick && riderHasNoTrick(*rider) &&
                        !(last->trick == rider::kNoTrick && last->trickMotion == rider::kNoTrick))
                    {
                        score.lineState = line.spinHalfTurns != 0 ? kLineSpin : kLineOpen;
                    }
                }
                else if (action == rider::kActionRiding)
                {
                    if (last->trick < kFirstSurfaceTrick && nameTurningTrick(*last))
                    {
                        scoreLines(&score);
                        tellTrickDisplay(trickMessage, kTrickDisplayTrickText, score);
                    }
                    tellTrickDisplay(trickMessage, kTrickDisplayNameFadeOut, score);
                    trickLineEnd(player, 1);
                    cheerLine(score);
                }
                else if (action == rider::kActionBail)
                {
                    trickLineEnd(player, 0);
                    soundEvent(kLineEndSound, kLineSoundCode, 0, 0, 0);
                }

                if (rider::actionState(rider, rider::kLevelAction) != rider::kActionLanding)
                {
                    if (score.lineState == kLineTrick)
                    {
                        followTrick(rider, score, *last);
                        followSpin(rider, line, *last, across, down);
                    }
                    else if (score.lineState == kLineSpin)
                    {
                        followSpin(rider, line, *last, across, down);
                    }
                    tellTrickDisplay(trickMessage, kTrickDisplayTrickText, score);
                }
            }
            markAirGap(player);
            scoreLines(&score);
        }

        shownScore->score = score.turnScore;
        shownScore->flags = 0;
        overlayMessage(kOverlayScore, shownScore);

        // The ruler: an air's height over its take-off while it rises (the
        // record is checked on the way down), a skid's length while it
        // lasts (checked as it ends).
        if (rider::actionState(rider, rider::kLevelAction) == rider::kActionAir && (rider->flags & rider::kFlagLeftRail) == 0)
        {
            const ee::Float takeOffY = rider->wheels[0].point[1];
            if (rider->position[1] < takeOffY)
            {
                if (rider->velocity[1] <= 0.0f)
                {
                    measure->recordKind = kAirHeightRecord;
                    measure->metres = takeOffY - rider->position[1];
                    overlayMessage(kOverlayRuler, measure);
                }
                else
                {
                    checkGameRecord(player, kAirHeightRecord, kRewardAirHeight, 0, takeOffY - rider->position[1]);
                }
            }
        }
        if (rider::actionState(rider, rider::kLevelAction) == rider::kActionRiding)
        {
            if (rider::actionState(rider, 1) == kActionSkid)
            {
                measure->recordKind = kSkidRecord;
                measure->metres = rider->skidDistance;
                overlayMessage(kOverlayRuler, measure);
            }
            else
            {
                if (rider->skidDistance != 0.0f)
                {
                    checkGameRecord(player, kSkidRecord, kRewardSkid, 0, rider->skidDistance);
                    rider->skidDistance = 0.0f;
                }
                measure->metres = 0.0f;
                overlayMessage(kOverlayRuler, measure);
            }
        }

        const uint8_t mode = multiplayerMode();
        if (mode >= kFirstRecordScoreMode && mode <= kLastRecordScoreMode)
        {
            const uint8_t keyMode = multiplayerMode();
            const uint8_t kindMode = multiplayerMode();
            const uint8_t single = singlePlayerMode();
            score.turnScore = gameRecord(recordKey(keyMode, single, (*g_modeRecordKinds)[kindMode], kTurnRecords))->value;
        }

        // The gauge: a wheelie's balance.
        if (rider::actionState(rider, rider::kLevelAction) == rider::kActionWheelie)
            balance = rider->balance;
        else
            balance = 0.0f;
        overlayMessage(kOverlayGauge, balance);
    }
    READABLE_FUNCTION(trickScoreFrame, Verified);
}
