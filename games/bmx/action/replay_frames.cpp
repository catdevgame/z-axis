// BMX XXX's camera states' frame functions for the replays
// (replay_frames.h). BMX XXX's own code (action.c).

#include "replay_frames.h"

#include "action_state.h"
#include "camera_state.h"
#include "bmx/game/game_frame.h"
#include "bmx/fx/effects_clock.h"
#include "bmx/game/npcs.h"
#include "bmx/game/player_riders.h"
#include "bmx/game/round.h"
#include "bmx/level/entities.h"
#include "bmx/level/level_collision.h"
#include "bmx/overlay/displays.h"
#include "bmx/overlay/screens.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/weapon/player_weapon.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"

#include <cstdint>
#include "sym/bmx/action/replay_frames.cpp.h"

namespace game::bmx
{
    using namespace action;

    namespace
    {
        // A watched replay's course (g_cameraStateStep).
        enum ReplayStep : uint8_t
        {
            kStepComingIn = 0,   // the replay starts and the screen fades in
            kStepRunning = 1,    // it plays; the pads' buttons are read
            kStepLeaving = 2,    // the screen fades out, then the state's way out
            kStepOverlayOff = 3, // it is held with its overlay part off
            kStepStartingOver = 4, // a second, then running again
            kStepHeldShot = 5,   // it is held on the camera's first shot, the shot it had kept
            kStepPlayedOut = 6,  // a second, then leaving
            // Scaffolding (value-named): nothing read so far sets step 7:
            // the rider and the level go on for 480 frames with no replay
            // update, then the replay is left.
            kStep7 = 7,
        };
        constexpr uint32_t kFadeFrames = 120;
        constexpr uint32_t kHoldFrames = 60;
        constexpr uint32_t kStep7Frames = 480;
        // The camera's shots a button steps through.
        constexpr uint32_t kLastCameraShot = 12;
        // The shot the camera had when the replay was held on the first one.
        inline ee::Global<uint8_t, sym::g_replayKeptShot> g_replayKeptShot;

        constexpr uint32_t kFaceButtons = zge::kPadTriangle | zge::kPadSquare | zge::kPadCircle | zge::kPadCross;
        // The sound of a replay's overlay going off.
        constexpr uint32_t kOverlayOffSound = 0x45;

        // The camera mode of a replay's camera.
        constexpr uint32_t kCameraModeReplay = 1;

        // The demo's replay: the song display is told at these frames, and
        // the demo is left at the last.
        constexpr uint32_t kDemoSongFrame = 60, kDemoSongAgainFrame = 4800, kDemoLastFrame = 6000;
        enum DemoStep : uint8_t
        {
            kDemoComingIn = 0,
            kDemoRunning = 1,
            kDemoLeavingByButton = 2,
            kDemoLeaving = 3,
        };
        constexpr uint32_t kTrackSound = 2, kSongSound = 10;
        // What the game was started for (bmx.c's; b7bmxc3's name).
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;

        // Whether a replay's state reads this player's pad.
        bool padRead(uint32_t player)
        {
            const int8_t only = *g_replayPadPlayer;
            return only == kEveryPlayersPad || only == static_cast<int8_t>(player);
        }

        // The replay played from its start, the NPCs with it, the camera on
        // it: in its replay mode in state 2, on the shot it has otherwise.
        void startOver(uint8_t replay, ee::Ptr<ActionCamera> replayCamera)
        {
            setReplayState(replay, kReplayPlayedOut);
            setReplayState(replay, kReplayPlaying);
            setNpcPlay(kNpcPlayStopped);
            setNpcPlay(kNpcPlayTurn);
            if (cameraState() == kCameraStateReplay)
                setCameraMode(replayCamera, 0, kCameraModeReplay);
            else
                calls::setCameraShot(replayCamera, calls::cameraShot(replayCamera));
            updateCameras();
        }

        void runLevelObjects()
        {
            zge::updateObjects(kFrameSeconds);
            entitiesFrameNoop();
            zge::updateAnimators(kFrameSeconds);
        }

        // The replay's rider has its frame, or the weapon of a replay whose
        // player watches.
        void runReplayPlayer(uint8_t replay)
        {
            const uint8_t watches = replayPlayerWatches(replay);
            if (watches == 0)
                rider::riderFrame(replayRider(replay));
            else if (watches == 1)
                updatePlayerWeapon(replayWeapon(replay));
        }

        void tellReplayDisplay(ee::Local<uint8_t> &message, uint8_t what)
        {
            *message = what;
            overlayMessage(kOverlayReplayDisplay, message.address());
        }
    }

    // replayStateFrame: a watched replay's frame. It comes in over 120
    // frames. Running, Start leaves it, Square holds it with the overlay
    // off, Circle holds it on the camera's first shot, Cross takes the
    // camera's next shot, Select turns the players' own overlay part and
    // Triangle starts it over; the replay, its rider or weapon, the NPC
    // riders and the level have their frames (an input replay's level
    // contacts, projectiles and game too). Played out, it starts over in
    // states 3 and 5 and is left otherwise. Held, Start leaves it and a face
    // button runs it again. Leaving fades the screen out, then: the last
    // game screen again (state 2), the first game state (5), or play.
    int32_t replayStateFrame()
    {
        ee::Local<uint8_t> displayMessage;

        const uint32_t state = cameraState();
        uint8_t replay;
        if (state == kCameraState3)
            replay = *g_state3Replay;
        else if (state == kCameraState5)
            replay = *g_stateReplay;
        else
            replay = shownReplay();
        const ee::Ptr<ActionCamera> replayCamera = playerActionCamera(rider::riderOf(replayRider(replay))->player);

        switch (*g_cameraStateStep)
        {
        case kStepComingIn:
        {
            const uint32_t frames = *g_stepFrames + 1;
            *g_stepFrames = frames;
            if (frames == 1)
            {
                startOver(replay, replayCamera);
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeIn);
                updateCameras();
            }
            else if (frames == kFadeFrames)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
                *g_cameraStateStep = kStepRunning;
                *g_stepFrames = 0;
            }
            return 0;
        }

        case kStepRunning:
        case kStepStartingOver:
        case kStepPlayedOut:
        case kStep7:
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (!padRead(player))
                    continue;
                if (rider::padButtons(player, zge::kPadStart, 0) != 0)
                {
                    *g_cameraStateStep = kStepLeaving;
                    *displayMessage = kReplayDisplayLeaving;
                    *g_stepFrames = 0;
                    setReplayState(replay, kReplayPlayedOut);
                    setReplayState(replay, kReplayIdle);
                    overlayMessage(kOverlayReplayDisplay, displayMessage.address());
                    return 0;
                }
                if (rider::padButtons(player, zge::kPadSquare, 0) != 0)
                {
                    *g_stepFrames = 0;
                    *g_cameraStateStep = kStepOverlayOff;
                    showOverlayPart(kOverlayReplay, 0);
                    tellReplayDisplay(displayMessage, kReplayDisplayOverlayOff);
                    return 0;
                }
                if (rider::padButtons(player, zge::kPadCircle, 0) != 0)
                {
                    *g_replayKeptShot = static_cast<uint8_t>(calls::cameraShot(replayCamera));
                    calls::setCameraShot(replayCamera, 0);
                    *g_stepFrames = 0;
                    *g_cameraStateStep = kStepHeldShot;
                    showOverlayPart(kOverlayReplay, 0);
                    tellReplayDisplay(displayMessage, kReplayDisplayOverlayOff);
                    return 0;
                }
                if (rider::padButtons(player, zge::kPadCross, 0) != 0)
                {
                    if (calls::cameraShot(replayCamera) < kLastCameraShot)
                        calls::setCameraShot(replayCamera, calls::cameraShot(replayCamera) + 1);
                    else
                        calls::setCameraShot(replayCamera, 0);
                }
                if (rider::padButtons(player, zge::kPadSelect, 0) != 0)
                {
                    *g_actionFlags ^= kActionOwnOverlayShown;
                    showOverlayPart(kOverlayPlayersOwn, *g_actionFlags >> 31);
                }
            }

            const uint8_t step = *g_cameraStateStep;
            if (step == kStepStartingOver || step == kStepPlayedOut)
            {
                const uint32_t frames = *g_stepFrames + 1;
                *g_stepFrames = frames;
                if (frames == kHoldFrames)
                {
                    const uint8_t next = *g_cameraStateStep == kStepStartingOver ? kStepRunning : kStepLeaving;
                    *g_stepFrames = 0;
                    *g_cameraStateStep = next;
                }
                updateCameras();
                return 0;
            }

            if (step == kStep7)
            {
                const uint32_t frames = *g_stepFrames + 1;
                *g_stepFrames = frames;
                if (frames >= kStep7Frames)
                {
                    *g_stepFrames = 0;
                    *g_cameraStateStep = kStepLeaving;
                    return 0;
                }
                runLevelObjects();
                runReplayPlayer(replay);
                runLevelContacts();
                updateProjectiles();
                updateCameras();
                return 0;
            }

            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (!padRead(player))
                    continue;
                if (rider::padButtons(player, zge::kPadTriangle, 0) != 0)
                {
                    startOver(replay, replayCamera);
                    *g_cameraStateStep = kStepStartingOver;
                    *g_stepFrames = 0;
                    return 0;
                }
            }

            replayUpdate();
            const uint8_t playing = replayState(replay);
            if (playing == kReplayPlaying)
            {
                runLevelObjects();
                if (replayFormat(replay) == kInputReplay)
                    runReplayPlayer(replay);
                npcsFrame();
                if (replayFormat(replay) == kInputReplay)
                {
                    runLevelContacts();
                    updateProjectiles();
                    gameRiderFrame();
                }
                advanceEffectsClock();
            }
            else if (playing == kReplayPlayedOut)
            {
                uint8_t next = kStepPlayedOut;
                if (cameraState() == kCameraState3 || cameraState() == kCameraState5)
                {
                    setReplayState(replay, kReplayPlaying);
                    setNpcPlay(kNpcPlayStopped);
                    setNpcPlay(kNpcPlayTurn);
                    calls::setCameraShot(replayCamera, calls::cameraShot(replayCamera));
                    next = kStepStartingOver;
                }
                *g_stepFrames = 0;
                *g_cameraStateStep = next;
            }
            if (*g_cameraStateStep == kStepRunning)
                updateCameras();
            return 0;
        }

        case kStepLeaving:
        {
            const uint32_t frames = *g_stepFrames + 1;
            *g_stepFrames = frames;
            if (frames == 1)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeOut);
                if ((gameModeRecord()->flags & kModeMultiplayer) == 0 && cameraState() != kCameraState3)
                    setPlayPhase(0);
            }
            else if (frames == kFadeFrames)
            {
                const uint32_t leaving = cameraState();
                if (leaving == kCameraStateReplay)
                {
                    playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
                    showGameScreen(kScreenLast);
                }
                else if (leaving == kCameraState5)
                {
                    playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
                    setGameState(0);
                }
                else
                {
                    setCameraState(kCameraStatePlay);
                }
                updateCameras();
            }
            return 0;
        }

        case kStepOverlayOff:
        case kStepHeldShot:
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (!padRead(player))
                    continue;
                if (rider::padButtons(player, zge::kPadStart, 0) != 0)
                {
                    if (*g_cameraStateStep == kStepHeldShot)
                        calls::setCameraShot(replayCamera, *g_replayKeptShot);
                    *g_cameraStateStep = kStepLeaving;
                    *displayMessage = kReplayDisplayLeaving;
                    *g_stepFrames = 0;
                    overlayMessage(kOverlayReplayDisplay, displayMessage.address());
                    return 0;
                }
                if (*g_cameraStateStep == kStepOverlayOff && rider::padButtons(player, zge::kPadCircle, 0) != 0)
                {
                    *g_replayKeptShot = static_cast<uint8_t>(calls::cameraShot(replayCamera));
                    calls::setCameraShot(replayCamera, 0);
                    *g_stepFrames = 0;
                    *g_cameraStateStep = kStepHeldShot;
                    updateCameras();
                    return 0;
                }
                if (rider::padButtons(player, kFaceButtons, 0) != 0)
                {
                    if (*g_cameraStateStep == kStepHeldShot)
                        calls::setCameraShot(replayCamera, *g_replayKeptShot);
                    *g_cameraStateStep = kStepRunning;
                    *g_stepFrames = 0;
                    if ((*g_actionFlags & kActionOwnOverlayShown) != 0)
                    {
                        showOverlayPart(kOverlayReplay, 1);
                        showOverlayPart(kOverlayPlayersOwn, *g_actionFlags >> 31);
                    }
                    tellReplayDisplay(displayMessage, kReplayDisplayPlaying);
                    return 0;
                }
            }
            if (*g_cameraStateStep == kStepOverlayOff)
            {
                const uint32_t frames = *g_stepFrames + 1;
                *g_stepFrames = frames;
                if (frames == 1)
                    soundEvent(kOverlayOffSound, 0, 0, 0, 0);
            }
            else if (*g_cameraStateStep == kStepHeldShot)
            {
                updateCameras();
            }
            return 0;
        }

        default:
            return 0;
        }
    }
    READABLE_FUNCTION(replayStateFrame, Verified);

    // replayDemoStateFrame: the demo's replay. It comes in over 120 frames.
    // Running, a button newly down on a connected pad leaves it; the
    // replay, its rider or weapon (an input replay), the level and the
    // effects have their frames, the song display is told at frames 60 and
    // 4800, and the demo is left when the replay has played out or at frame
    // 6000. Leaving fades the screen out, then shows the feature screen
    // where the game was started for one, or enters the first game state.
    int32_t replayDemoStateFrame()
    {
        ee::Local<SongMessage> songMessage;

        const uint8_t replay = *g_stateReplay;
        switch (*g_cameraStateStep)
        {
        case kDemoComingIn:
        {
            const uint32_t frames = *g_stepFrames + 1;
            *g_stepFrames = frames;
            if (frames == 1)
            {
                updateCameras();
            }
            else if (frames == kFadeFrames)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
                *g_cameraStateStep = kDemoRunning;
                *g_stepFrames = 0;
            }
            return 0;
        }

        case kDemoRunning:
        {
            for (uint32_t port = 0; port < 2; ++port)
            {
                const rider::PadInput *const pad = rider::padInput(port);
                if ((pad->flags & rider::kPadMissing) != 0)
                    continue;
                if ((pad->buttons.held & pad->buttons.changed) != 0)
                {
                    *g_cameraStateStep = kDemoLeavingByButton;
                    *g_stepFrames = 0;
                    break;
                }
            }
            if (*g_cameraStateStep != kDemoRunning)
            {
                updateCameras();
                return 0;
            }

            replayUpdate();
            const uint8_t playing = replayState(replay);
            if (playing == kReplayPlayedOut)
            {
                *g_stepFrames = 0;
                *g_cameraStateStep = kDemoLeaving;
            }
            else if (playing == kReplayPlaying)
            {
                if (replayFormat(replay) == kInputReplay)
                {
                    runReplayPlayer(replay);
                    runLevelContacts();
                    updateProjectiles();
                }
                runLevelObjects();
                advanceEffectsClock();
                const uint32_t frames = *g_stepFrames + 1;
                *g_stepFrames = frames;
                if (frames == kDemoSongFrame || frames == kDemoSongAgainFrame)
                {
                    songMessage->state = static_cast<uint8_t>(soundEvent(kTrackSound, 0, 0, 0, 0));
                    songMessage->song = soundEvent(kSongSound, 0, 0, 0, 0);
                    overlayMessage(kOverlaySongInfo, songMessage.address());
                }
                else if (frames == kDemoLastFrame)
                {
                    *g_stepFrames = 0;
                    *g_cameraStateStep = kDemoLeaving;
                }
            }
            updateCameras();
            return 0;
        }

        case kDemoLeavingByButton:
        case kDemoLeaving:
        {
            const uint32_t frames = *g_stepFrames + 1;
            *g_stepFrames = frames;
            if (frames == 1)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeOut);
                setPlayPhase(0);
                return 0;
            }
            if (frames != kFadeFrames)
                return 0;
            if (*g_startUpKind != 0)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
                showGameScreen(kScreenFeature);
            }
            else
            {
                setGameState(0);
            }
            return 1;
        }

        default:
            return 0;
        }
    }
    READABLE_FUNCTION(replayDemoStateFrame, Draft);
}
