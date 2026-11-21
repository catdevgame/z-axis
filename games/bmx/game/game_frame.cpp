// BMX XXX's game frame (game_frame.h): BMX XXX's own code (game.c, its game
// block), DM2's line (games/dm2/game/game_frame.cpp: the same steps, where DM2
// keeps the turn's end in gameRiderFrame and BMX XXX in a frame of each
// mode's, and where BMX XXX's round flags carry what DM2 tests in the
// stage).

#include "game_frame.h"

#include "level_exit.h"
#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"
#include "round.h"
#include "score.h"
#include "tips.h"

#include "bmx/action/camera_state.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/overlay/screens.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/game_frame.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The player whose pad has the menus (the UI's "ModalPlayerNum").
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
        // Not zero: the running turn's clock stands still (DM2's; nothing
        // read sets it).
        inline ee::Global<uint32_t, sym::g_clockStopped> g_clockStopped;

        // Each mode's frame of the running turn's end, by mode: turnEndFrame
        // for the first 17, the last three their own.
        constexpr uint32_t kGameModes = 20;
        inline ee::Global<ee::FunctionPtr<void(uint32_t)>[kGameModes], sym::g_modeTurnEndFrames> g_modeTurnEndFrames;

        // GameMode::scored of a turn of one line (inferred, DM2's one-line
        // modes): with kModeTurns, the turn's end comes once its line is
        // open, with or without a clock.
        constexpr uint8_t kScoredOneLine = 2;


        // The clock's last five seconds are counted aloud (the countdown
        // sound with the seconds left, 0 once, when the time is out).
        constexpr uint32_t kCountdownSound = 0x38;
        constexpr uint32_t kFramesPerSecond = 60;
        constexpr uint32_t kCountedSeconds = 5;

    }

    // gameRiderFrame: the game's frame for the riders.
    //
    // Each player's rider that is shown: its trick line and score (not once
    // the turn's line is in), the tips (a one-rider mode's), the level's
    // goals, the health display (where the players take turns of a round,
    // the current player's alone), a bail's item on the reward display as
    // the bail starts, a beaten record's sparkle and the level's way out;
    // while a turn runs, the mode's frame of the turn's end.
    //
    // Then, while a turn with a clock runs and the last line is not riding
    // on, the clock: its last five seconds counted aloud while the game
    // plays, and the timer display. The clock counts down in every phase,
    // but stands still in a running turn while it is stopped. A turn that
    // has ended goes over while the game plays; a movie won pauses the game
    // for its confirmation screen. Returns the round's phase.
    uint8_t gameRiderFrame()
    {
        ee::Local<ValueMessage> health;
        ee::Local<RewardItemMessage> bail;
        ee::Local<uint32_t> clock;

        setRoundFlags(kMovieWonFlag, 0);
        const GameMode *const mode = gameModeRecord().get();
        for (uint8_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<rider::Rider> rider = playerRider(player);
            if (!rider::hasFlag(&*rider, rider::kFlagShown))
                continue;

            if (!roundFlags(kTurnAtRestFlag))
                playerScoreFrame(rider->player);
            if ((mode->flags & kModeMultiplayer) == 0)
                tipsFrame(player);
            objectives::updatePlayerObjectives(player);
            if ((mode->flags & kModeRoundOfTurns) == 0 || player == *g_currentPlayer)
            {
                health->player = player;
                health->value = ee::toFloat(static_cast<int32_t>(rider->health));
                health->flags = 0;
                overlayMessage(kOverlayHealth, health);
            }
            if (rider::actionState(&*rider, rider::kLevelAction) == rider::kActionBail && rider->actionFrames[0] == 1)
            {
                bail->player = player;
                bail->kind = kRewardItemBail;
                bail->value = ee::toFloat(static_cast<int32_t>(rider->bailKind));
                overlayMessage(kOverlayReward, bail);
            }
            recordEffectFrame(player);
            checkLevelExit(player);
            if (roundPhase() == kTurnRunningPhase)
                (*g_modeTurnEndFrames)[gameMode()](player);
        }

        RoundState &round = *g_round;
        if (roundPhase() == kTurnRunningPhase && mode->turnSeconds != ee::Float(0.0f) && !roundFlags(kLastLineFlag))
        {
            if (cameraState() == kCameraStatePlay)
            {
                const uint32_t left = round.phaseFramesLeft;
                if (left == 0)
                {
                    if (!roundFlags(kTimeUpSoundFlag))
                    {
                        soundEvent(kCountdownSound, round.phaseFramesLeft / kFramesPerSecond, 0, 0, 0);
                        setRoundFlags(kTimeUpSoundFlag, 1);
                    }
                }
                else if (left % kFramesPerSecond == 0 && left <= kCountedSeconds * kFramesPerSecond)
                {
                    soundEvent(kCountdownSound, round.phaseFramesLeft / kFramesPerSecond, 0, 0, 0);
                }
            }
            *clock = round.phaseFramesLeft;
            overlayMessage(kOverlayTimer, clock);
        }

        if (round.phaseFramesLeft != 0 && (*g_clockStopped == 0 || roundPhase() != kTurnRunningPhase))
            round.phaseFramesLeft = round.phaseFramesLeft - 1;
        round.turnEndFrames = round.turnEndFrames + 1;

        if (cameraState() == kCameraStatePlay && roundFlags(kTurnEndedFlag))
            setRoundPhase(kTurnOverPhase);
        if (roundFlags(kMovieWonFlag))
        {
            setGameMode(kPausedMode);
            showGameScreen(kScreenMovieConfirm);
            *g_modalPlayerNum = *g_currentPlayer;
        }
        return roundPhase();
    }
    READABLE_FUNCTION(gameRiderFrame, Verified);

    // turnEndFrame: a player's frame of the running turn's end. A stage that
    // moves on is run again at once, from the start:
    //   riding     until the time is out (a turn with a clock) or a line is
    //              begun;
    //   line begun once the time is out (a one-line turn: also once its line
    //              is open): the last line while a line is open or the rider
    //              is in the air, else at rest;
    //   last line  no clock; once the line is in and the rider is riding, at
    //              rest;
    //   at rest    tricks are no longer scored; 30 frames on, ended;
    //   decided    (a rider with kFlagDowned sends a turn not yet at rest
    //              here, its player kept) 240 frames on, ended;
    //   ended      the turn's end is flagged for the game's frame.
    void turnEndFrame(uint32_t player)
    {
        player &= 0xFF;
        const GameMode *const mode = gameModeRecord().get();
        const ee::Ptr<rider::Rider> rider = playerRider(player);
        RoundState &round = *g_round;

        bool moved;
        do
        {
            moved = false;
            if (rider::hasFlag(&*rider, rider::kFlagDowned) && round.turnEndStage < kTurnAtRest)
            {
                round.decidedPlayer = static_cast<uint8_t>(player);
                round.turnEndStage = kTurnDecided;
                round.turnEndFrames = 0;
            }

            const auto moveTo = [&](uint8_t stage) {
                round.turnEndStage = stage;
                round.turnEndFrames = 0;
                moved = true;
            };
            switch (round.turnEndStage)
            {
            case kTurnRiding:
                if ((mode->turnSeconds != ee::Float(0.0f) && round.phaseFramesLeft == 0) || rider->score.lineState != 0)
                    moveTo(kTurnLineBegun);
                break;
            case kTurnLineBegun:
            {
                const bool oneLine = (mode->flags & kModeTurns) != 0 && mode->scored == kScoredOneLine;
                const bool timeOut = mode->turnSeconds != ee::Float(0.0f) && round.phaseFramesLeft == 0;
                if (oneLine ? !(timeOut || rider->score.lineState != 0) : !timeOut)
                    break;
                if (rider->score.lineState != 0 || rider::physicsState(&*rider) == rider::kPhysicsAir)
                    moveTo(kTurnLastLine);
                else
                    moveTo(kTurnAtRest);
                break;
            }
            case kTurnLastLine:
                setRoundFlags(kLastLineFlag, 1);
                if (rider->score.lineState == 0 && rider::actionState(&*rider, rider::kLevelAction) == rider::kActionRiding)
                    moveTo(kTurnAtRest);
                break;
            case kTurnAtRest:
                setRoundFlags(kTurnAtRestFlag, 1);
                setRoundFlags(kLastLineFlag, 0);
                if (round.turnEndFrames == kTurnRestFrames)
                    moveTo(kTurnEnded);
                break;
            case kTurnDecided:
                if (round.turnEndFrames == kTurnDecidedFrames)
                    moveTo(kTurnEnded);
                break;
            case kTurnEnded:
                setRoundFlags(kTurnEndedFlag, 1);
                break;
            default:
                break;
            }
        } while (moved);
    }
    READABLE_FUNCTION(turnEndFrame, Verified);

    namespace
    {
        // The paintball's own stages of a turn's end (RoundState::
        // turnEndStage; inferred from what moves each on). The player whose
        // turn it is rides; the other snipes (round.cpp's sniper camera).
        enum PaintballTurnStage : uint8_t
        {
            kPaintballStart = 0,     // 120 frames before the turn's rider counts
            kPaintballRiding = 1,    // riding: until the time is out, or a bail
            kPaintballStage2 = 2,     // (what sets it is not read) 60 frames, then riding again
            kPaintballBailed = 3,    // the rider's bail of kind 14: until it is over, then the start again
            kPaintballTimeUp = 4,    // the time is out: at rest, 30 frames
            kPaintballDecided = 5,   // the rider with kFlagDowned bailed (kind 14): decided, 240 frames
            kPaintballEnded = 6,
        };
        constexpr uint32_t kPaintballStartFrames = 120, kPaintballRecoverFrames = 60, kPaintballRestFrames = 30, kPaintballDecidedFrames = 240;
        constexpr uint8_t kPaintballHitBail = 14; // Rider::bailKind (inferred: the sniper's hit)
    }

    // paintballTurnEndFrame: the PAINTBALL's turn end ("collect all the
    // boomboxes before your opponent snipes you"): the current player's
    // rider decides the stages (PaintballTurnStage); a stage that moves on is
    // run again at once.
    void paintballTurnEndFrame(uint32_t player)
    {
        player &= 0xFF;
        const GameMode *const mode = gameModeRecord().get();
        const ee::Ptr<rider::Rider> rider = playerRider(*g_currentPlayer);
        RoundState &round = *g_round;
        bool moved;
        do
        {
            moved = false;
            const auto moveTo = [&](uint8_t stage) {
                round.turnEndFrames = 0;
                round.turnEndStage = stage;
                moved = true;
            };
            switch (round.turnEndStage)
            {
            case kPaintballStart:
                if (round.turnEndFrames == kPaintballStartFrames)
                    moveTo(kPaintballRiding);
                break;
            case kPaintballRiding:
            case kPaintballStage2:
                if (mode->turnSeconds != ee::Float(0.0f) && round.phaseFramesLeft == 0)
                {
                    moveTo(kPaintballTimeUp);
                    break;
                }
                if (rider::actionState(&*rider, rider::kLevelAction) == rider::kActionBail && rider->bailKind == kPaintballHitBail)
                {
                    if (rider::hasFlag(&*rider, rider::kFlagDowned))
                    {
                        round.turnEndStage = kPaintballDecided;
                        round.decidedPlayer = static_cast<uint8_t>(player);
                        round.turnEndFrames = 0;
                        moved = true;
                    }
                    else
                    {
                        moveTo(kPaintballBailed);
                    }
                    break;
                }
                if (round.turnEndStage == kPaintballStage2 && round.turnEndFrames == kPaintballRecoverFrames)
                    moveTo(kPaintballRiding);
                break;
            case kPaintballBailed:
                if (rider::actionState(&*rider, rider::kLevelAction) != rider::kActionBail)
                    moveTo(kPaintballStart);
                break;
            case kPaintballTimeUp:
                setRoundFlags(kTurnAtRestFlag, 1);
                if (round.turnEndFrames == kPaintballRestFrames)
                    moveTo(kPaintballEnded);
                break;
            case kPaintballDecided:
                if (round.turnEndFrames == kPaintballDecidedFrames)
                    moveTo(kPaintballEnded);
                setRoundFlags(kLastLineFlag, 1);
                break;
            case kPaintballEnded:
                setRoundFlags(kTurnEndedFlag, 1);
                break;
            default:
                break;
            }
        } while (moved);
    }
    READABLE_FUNCTION(paintballTurnEndFrame, Verified);

    namespace
    {
        // The ghost ride duel's stages of a turn's end: turnEndFrame's, with
        // the rider's action state 10 in the line's place (RoundState::
        // turnEndStage).
        enum GhostRideTurnStage : uint8_t
        {
            kGhostRideRiding = 0,  // until the time is out
            kGhostRideHeld = 1,    // the time is out while the rider is in action state 10: until it leaves it
            kGhostRideAtRest = 2,  // 30 frames
            kGhostRideDecided = 3, // a rider with kFlagDowned before the rest: 240 frames
            kGhostRideEnded = 4,
        };
    }

    // ghostRideTurnEndFrame: the GHOSTRIDE DUEL's turn end ("the first to
    // take out 10 pedestrians by ghost riding a bike"): turnEndFrame's stages
    // (GhostRideTurnStage); a stage that moves on is run again at once.
    void ghostRideTurnEndFrame(uint32_t player)
    {
        player &= 0xFF;
        const GameMode *const mode = gameModeRecord().get();
        const ee::Ptr<rider::Rider> rider = playerRider(player);
        RoundState &round = *g_round;
        bool moved;
        do
        {
            moved = false;
            if (rider::hasFlag(&*rider, rider::kFlagDowned) && round.turnEndStage < kGhostRideAtRest)
            {
                round.decidedPlayer = static_cast<uint8_t>(player);
                round.turnEndStage = kGhostRideDecided;
                round.turnEndFrames = 0;
            }
            const auto moveTo = [&](uint8_t stage) {
                round.turnEndFrames = 0;
                round.turnEndStage = stage;
                moved = true;
            };
            switch (round.turnEndStage)
            {
            case kGhostRideRiding:
                if (mode->turnSeconds != ee::Float(0.0f) && round.phaseFramesLeft == 0)
                    moveTo(rider::actionState(&*rider, rider::kLevelAction) == rider::kActionGhostRide ? kGhostRideHeld : kGhostRideAtRest);
                break;
            case kGhostRideHeld:
                setRoundFlags(kLastLineFlag, 1);
                if (rider::actionState(&*rider, rider::kLevelAction) != rider::kActionGhostRide)
                    moveTo(kGhostRideAtRest);
                break;
            case kGhostRideAtRest:
                setRoundFlags(kTurnAtRestFlag, 1);
                setRoundFlags(kLastLineFlag, 0);
                if (round.turnEndFrames == kTurnRestFrames)
                    moveTo(kGhostRideEnded);
                break;
            case kGhostRideDecided:
                if (round.turnEndFrames == kTurnDecidedFrames)
                    moveTo(kGhostRideEnded);
                break;
            case kGhostRideEnded:
                setRoundFlags(kTurnEndedFlag, 1);
                break;
            default:
                break;
            }
        } while (moved);
    }
    READABLE_FUNCTION(ghostRideTurnEndFrame, Draft);

    // gameTaskFrame: the game task's frame (gameSetup makes the task; DM2's
    // name): the level objectives' highlights drawn. Always 1 (the task goes
    // on).
    uint32_t gameTaskFrame()
    {
        objectives::drawObjectiveHighlights();
        return 1;
    }
    READABLE_FUNCTION(gameTaskFrame, Verified);
}
