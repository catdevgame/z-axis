// The action modes' frame functions for play and for a game screen
// (action.h; the level state's frame runs the mode's). DM2's own code
// (action.c).

#include "action.h"
#include "action_state.h"
#include "../camera/camera.h"
#include "../fx/effects_clock.h"
#include "../game/npcs.h"
#include "../game/players.h"
#include "../game/round.h"
#include "../level/level_collision.h"
#include "../level/object_sounds.h"
#include "../overlay/overlay.h"
#include "../replay/replays.h"
#include "../rider/rider.h"
#include "../sound/sound_mode.h"

#include "dm2/camera/follow_camera.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/scene/message.h"

#include <cstdint>
#include "sym/dm2/action/action_frame.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        ee::Ptr<Rider> riderOf(uint32_t index);                              // rider_state.cpp
        int32_t riderFrame(uint32_t index);                                  // actions/frame.cpp
        PadInput *padInput(uint32_t port);                                   // input.cpp
        int32_t padButtons(uint32_t port, uint32_t buttons, uint32_t mode);  // input.cpp
    }

    namespace
    {
        // What readPads answered this frame (level_frame.cpp keeps it):
        // bit 0, a pad was lost.
        inline ee::Global<uint32_t, sym::g_padReport> g_padReport;
        constexpr uint32_t kPadLost = 0x1;

        // The frames of play before Start pauses, and a lost pad's wait.
        constexpr uint32_t kPauseWaitFrames = 30;

        // A frame of the game's time as each mode's frame hands it to the
        // level's objects (a sixtieth of a second, kept once a mode).
        inline ee::Global<ee::Float, sym::playFrameSeconds> playFrameSeconds;
        inline ee::Global<ee::Float, sym::screenFrameSeconds> screenFrameSeconds;
        // A stick's deflection per step: the riders get their sticks in
        // 127 steps a side, as a replay records them.
        constexpr float kStickSteps = 127.0f;
        inline ee::Global<ee::Float, sym::stickStep> stickStep;


        // A menu's directions repeat while held: after a press, this long
        // until the first repeat, then one every few frames.
        constexpr uint16_t kRepeatDelayFrames = 20;
        constexpr uint16_t kRepeatEveryFrames = 3;
        constexpr uint32_t kDirectionButtons = zge::kPadUp | zge::kPadDown | zge::kPadLeft | zge::kPadRight;
        constexpr uint32_t kShoulderButtons = zge::kPadL1 | zge::kPadR1;

        // The game paused: its sounds, and the pause screen up.
        void pauseGame()
        {
            setGameMode(kSoundModePaused);
            showGameScreen(kScreenPause);
        }

        // A stick's deflection in whole steps (toward zero, the low eight
        // bits kept as a signed count), as a deflection again.
        ee::Float inSteps(ee::Float deflection)
        {
            const int8_t steps = static_cast<int8_t>(ee::toInt(deflection * ee::Float(kStickSteps)));
            return ee::toFloat(steps) * *stickStep;
        }
    }

    // actionFrame: play's frame. Start (the current player's pad), or a
    // lost pad, pauses the game and ends the frame (1). Otherwise the
    // level's objects and traffic get their frame, the replays record or
    // play, each shown rider takes its player's pad (the sticks in 127ths)
    // and has its frame, then the NPC riders, the level's contacts, the
    // game, the effects' clock and the camera (0).
    uint32_t actionFrame()
    {
        if (*g_padReport & kPadLost)
        {
            if (*g_actionStep == kPlayPausable)
            {
                pauseGame();
                return 1;
            }
            *g_actionStep = kPlayPadLost;
        }
        const uint8_t step = *g_actionStep;
        if (step == kPlayStarting || step == kPlayPadLost)
        {
            ++*g_actionStepFrames;
            if (*g_actionStepFrames == kPauseWaitFrames)
            {
                if (step != kPlayStarting)
                {
                    pauseGame();
                    return 1;
                }
                *g_actionStep = kPlayPausable;
            }
        }

        zge::updateObjects(*playFrameSeconds);
        objectSoundsFrame();
        for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            if ((rider->flags & rider::kFlagShown) && *g_actionStep == kPlayPausable
                && rider::padButtons(*g_currentPlayer, zge::kPadStart, rider::kAnyPressed))
            {
                pauseGame();
                return 1;
            }
        }

        replayUpdate();
        for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            const rider::PadInput *const pad = rider::padInput(player);
            if (!(rider->flags & rider::kFlagShown))
                continue;
            rider->input.held = pad->buttons.held;
            rider->input.changed = pad->buttons.changed;
            for (uint32_t stick = 0; stick < 2; ++stick)
            {
                const ee::Float previousX = inSteps(pad->buttons.previous[stick].x);
                const ee::Float previousY = inSteps(pad->buttons.previous[stick].y);
                rider->input.previous[stick].x = previousX;
                rider->input.previous[stick].y = previousY;
                const ee::Float x = inSteps(pad->buttons.sticks[stick].x);
                const ee::Float y = inSteps(pad->buttons.sticks[stick].y);
                rider->input.sticks[stick].x = x;
                rider->input.sticks[stick].y = y;
            }
            rider::riderFrame(riderIndexOf(player));
        }

        npcsFrame();
        runLevelContacts();
        gameRiderFrame();
        advanceEffectsClock();
        followCamera();
        return 0;
    }
    READABLE_FUNCTION(actionFrame, Verified);

    // screenModeFrame: the frame while a game screen is up. Each pad's
    // directions repeat for the menus (at once while L1 or R1 is held), and
    // the screen gets its frame. Under the round's end screens the round's
    // replay goes on behind: while it plays, its rider, the level and the
    // game have their frames as in play; once it has played out it starts
    // over from a ground camera. Answers 0 where the camera followed, 1
    // where the mode changed under it or the park is being tested.
    uint32_t screenModeFrame()
    {
        for (uint32_t port = 0; port < kMaxPlayers; ++port)
        {
            rider::PadInput *const pad = rider::padInput(port);
            if ((pad->buttons.changed & ~kShoulderButtons) != 0)
            {
                pad->repeatWait = kRepeatDelayFrames;
                *g_actionStepFrames = 0;
            }
            else if (pad->repeatWait != 0)
                --pad->repeatWait;
            else
            {
                const uint32_t held = pad->buttons.held;
                pad->buttons.changed = held & kDirectionButtons;
                pad->repeatWait = (held & kShoulderButtons) ? 0 : kRepeatEveryFrames;
            }
        }

        gameScreenFrame();
        if (actionMode() != kActionScreen)
            return 1;
        if (singlePlayerMode() == kParkTest)
            return 1;
        if (replayBehindScreen(gameScreen(), kScreenGameOver + 1))
        {
            replayUpdate();
            const uint8_t replay = shownReplay();
            const uint8_t player = rider::riderOf(replayRider(replay))->player;
            const uint8_t state = replayState(replay);
            if (state == kReplayPlaying)
            {
                zge::updateObjects(*screenFrameSeconds);
                objectSoundsFrame();
                rider::riderFrame(replayRider(replay));
                npcsFrame();
                runLevelContacts();
                gameRiderFrame();
                advanceEffectsClock();
            }
            else if (state == kReplayPlayedOut)
            {
                setReplayState(replay, kReplayPlaying);
                setNpcPlay(0);
                setNpcPlay(1);
                setCameraMode(player, 0, kCameraOnGround);
                placeGroundCamera(player, kGroundCameraKind10);
            }
        }
        followCamera();
        return 0;
    }
    READABLE_FUNCTION(screenModeFrame, Verified);
}
