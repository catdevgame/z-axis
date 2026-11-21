// BMX XXX's camera states (camera_state.h): the state's change, the cut
// scenes, and the frame functions of the cut scene's state and of states 6
// and 8. BMX XXX's own code (action.c).

#include "camera_state.h"

#include "action_state.h"
#include "bmx/fx/effects_clock.h"
#include "bmx/game/npcs.h"
#include "bmx/game/player_riders.h"
#include "bmx/game/round.h"
#include "bmx/input/pads.h"
#include "bmx/level/entities.h"
#include "bmx/level/level_collision.h"
#include "bmx/overlay/displays.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/action/camera_state.cpp.h"

namespace game::bmx
{
    using namespace action;

    namespace
    {
        // Whether the level's objects were saved at its start, and the save.
        inline ee::Global<uint32_t, sym::g_levelObjectsSaved> g_levelObjectsSaved;
        constexpr uint32_t kLevelObjectsSave = sym::g_levelObjectsSave;

        // The mode where the players shoot at each other [value-named: the
        // game mode a replay's end restores the level in].
        constexpr uint8_t kGameMode6 = 6;

        // The sound events: the song a replay's end names, a cut scene's
        // start (with its name) and its end.
        constexpr uint32_t kSongSound = 0x0A, kCutSceneStartSound = 0x4B, kCutSceneEndSound = 0x4C;

        // The cut scene state's course, by its frames: the screen fades out,
        // the level is set for the scene, the screen fades in; then the
        // scene may be skipped; its end fades out and puts the level back.
        constexpr uint32_t kCutSceneSetUpFrame = 90, kCutSceneShownFrame = 91, kCutSceneFadeResetFrame = 181,
                           kCutScenePlayingFrame = 187, kCutSceneSkippableFrame = 241;
        enum CutSceneStep : uint8_t
        {
            kCutSceneFadingOut = 0,
            kCutSceneFadingIn = 1,
            kCutScenePlaying = 2,
            kCutSceneSkippable = 3,
            kCutSceneEnding = 4,
        };
        constexpr uint32_t kCutSceneEndFrames = 90;
        // The scene ends when its camera's animation has this long left.
        constexpr ee::Float kCutSceneEndSeconds = 4.0f / 60.0f;
        // The buttons that skip a cut scene.
        constexpr uint32_t kSkipButtons = 0x4080;
        // The camera mode a cut scene's camera takes, and the player's
        // camera's after it (level 1: mode 3, then 0).
        constexpr uint32_t kCameraModeCutScene = 8;
        // The level's objects hidden for a cut scene, by name.
        constexpr uint32_t kPathManName = sym::g_textPathMan;         // "PathMan"
        constexpr uint32_t kFastPathManName = sym::g_textFastPathMan; // "FastPathMan"
        constexpr uint32_t kCarName = sym::g_textCar;                 // "car"
        constexpr uint32_t kFastCarName = sym::g_textFastCar;         // "fastcar"
        constexpr uint32_t kPickupName = sym::g_textPickup;           // "pickup"

        constexpr uint32_t kState6Frames = 600;
        // Not 0: state 8's frame runs the level's objects and animators (a
        // value of the game's data that no code writes).
        inline ee::Global<uint32_t, sym::g_state8RunsObjects> g_state8RunsObjects;
        constexpr uint8_t kPhysicsStateTwo = 2;

        void stopRumble(uint32_t player, ee::Local<Rumble> &request)
        {
            request->motor = 0;
            calls::rumble(player, request.address());
            request->motor = 1;
            calls::rumble(player, request.address());
        }
    }

    uint32_t cameraState()
    {
        return *g_cameraState;
    }
    READABLE_FUNCTION(cameraState, Verified);

    void restoreLevel()
    {
        if (*g_levelObjectsSaved != 0)
            calls::restoreLevelObjectsTail(kLevelObjectsSave);
    }
    READABLE_FUNCTION(restoreLevel, Verified);

    // setCameraState: the state left, then the new one entered.
    //   Leaving play hides every overlay part. Leaving a replay (2, 4, 5)
    //   stops it, puts its rider's player back unshown, hides that player's
    //   camera, restores the level in game mode 6, tells the replay and song
    //   displays, and shows the game's camera. Leaving state 3 stops its
    //   replay and gives the current player's camera its first mode back.
    //   Leaving a cut scene hides its overlay part and stops every pad's
    //   rumble.
    //   Entering play shows the overlay and gives the pads back; a screen
    //   takes the pads. A replay puts every player away, starts the NPCs,
    //   plays the replay on its rider's player's camera (the whole screen
    //   outside state 2) and shows the replay's overlay part. State 3 plays
    //   its replay on that player's camera with the first camera shot. A cut
    //   scene clears the subtitle and shows its overlay part.
    uint8_t setCameraState(uint32_t state)
    {
        ee::Local<uint8_t> replayMessage;
        ee::Local<SongMessage> songMessage;
        ee::Local<Rumble> noRumble;
        ee::Local<uint32_t> noSubtitle;

        const uint32_t before = *g_cameraState;
        *g_replayPadPlayer = kEveryPlayersPad;
        *g_stepFrames = 0;
        *g_cutSceneEndFrames = 0;
        *g_playFrames = 0;

        switch (before)
        {
        case kCameraStatePlay:
            showOverlayPart(kOverlayAllParts, 0);
            *g_actionFlags &= ~kActionOwnOverlayShown;
            break;

        case kCameraStateReplay:
        case kCameraStateReplayDemo:
        case kCameraState5:
        {
            uint8_t replay = *g_stateReplay;
            if (before == kCameraStateReplay)
                replay = shownReplay();
            setReplayState(replay, kReplayIdle);
            spawnPlayer(rider::riderOf(replayRider(replay))->player, 0);
            showCamera(playerActionCamera(rider::riderOf(replayRider(replay))->player), 0);
            if (gameMode() == kGameMode6)
            {
                restoreLevel();
                spawnPlayer(*g_currentPlayer, 1);
            }
            *replayMessage = kReplayDisplayOff;
            overlayMessage(kOverlayReplayDisplay, replayMessage.address());
            songMessage->state = 0xFF;
            songMessage->song = soundEvent(kSongSound, 0, 0, 0, 0);
            overlayMessage(kOverlaySongInfo, songMessage.address());
            showOverlayPart(kOverlayAllParts, 0);
            showCamera(*g_gameCamera, 1);
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
                setPlayPhase(0);
            break;
        }

        case kCameraState3:
            setReplayState(*g_state3Replay, kReplayIdle);
            *replayMessage = kReplayDisplayOff;
            overlayMessage(kOverlayReplayDisplay, replayMessage.address());
            setCameraMode(playerActionCamera(*g_currentPlayer), 0, 0);
            showOverlayPart(kOverlayAllParts, 0);
            *g_actionFlags &= ~kActionFlag40000000;
            break;

        case kCameraStateCutScene:
            showOverlayPart(kOverlayCutScene, 0);
            noRumble->timing = 0;
            noRumble->strength = 0;
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                stopRumble(player, noRumble);
            break;

        case kCameraState8:
            calls::leaveCameraState8();
            break;

        default:
            break;
        }

        switch (state)
        {
        case kCameraStatePlay:
            showOverlayPart(kOverlayShared, 1);
            showOverlayPart(kOverlayPlayersOwn, 1);
            *g_actionFlags |= kActionOwnOverlayShown;
            takePads(0);
            *g_actionFlags &= kActionFlag40000000;
            break;

        case kCameraStateScreen:
            takePads(1);
            break;

        case kCameraStateReplay:
        case kCameraStateReplayDemo:
        case kCameraState5:
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                spawnPlayer(static_cast<uint8_t>(player), 0);
            setNpcPlay(kNpcPlayTurn);
            uint8_t replay;
            if (state == kCameraStateReplayDemo || state == kCameraState5)
            {
                *g_currentPlayer = 0;
                replay = *g_stateReplay;
            }
            else
            {
                replay = shownReplay();
            }
            showCamera(*g_gameCamera, 0);
            uint8_t player;
            if (state == kCameraStateReplay)
            {
                player = rider::riderOf(replayRider(replay))->player;
            }
            else
            {
                // The whole screen is the first player's.
                player = 0;
                rider::riderOf(replayRider(replay))->player = 0;
            }
            setReplayState(replay, kReplayPlaying);
            *replayMessage = kReplayDisplayPlaying;
            overlayMessage(kOverlayReplayDisplay, replayMessage.address());
            const ee::Ptr<ActionCamera> replayCamera = playerActionCamera(player);
            setCameraViewportLayout(replayCamera, state != kCameraStateReplay ? 1 : 0);
            setCameraHearing(replayCamera, 0);
            showCamera(replayCamera, 1);
            setCameraMode(replayCamera, 0, 1);
            showOverlayPart(kOverlayReplay, 1);
            showOverlayPart(kOverlayReplayDemo, state == kCameraStateReplayDemo ? 1 : 0);
            if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
                setPlayPhase(3);
            break;
        }

        case kCameraState3:
        {
            const uint8_t replay = *g_state3Replay;
            const uint8_t player = rider::riderOf(replayRider(replay))->player;
            setReplayState(replay, kReplayPlaying);
            *replayMessage = kReplayDisplayPlaying;
            overlayMessage(kOverlayReplayDisplay, replayMessage.address());
            const ee::Ptr<ActionCamera> replayCamera = playerActionCamera(player);
            setCameraMode(replayCamera, 0, 1);
            calls::setCameraShot(replayCamera, 1);
            showOverlayPart(kOverlayAllParts, 0);
            break;
        }

        case kCameraStateCutScene:
            *noSubtitle = 0;
            overlayMessage(kOverlaySubtitle, noSubtitle.address());
            showOverlayPart(kOverlayCutScene, 1);
            break;

        case kCameraState8:
            calls::enterCameraState8();
            break;

        default:
            break;
        }

        *g_cameraStateStep = 0;
        *g_cameraState = state & 0xFF;
        return static_cast<uint8_t>(state);
    }
    READABLE_FUNCTION(setCameraState, Verified);

    void playCutScene(uint32_t cutScene, uint32_t now)
    {
        if (now == 0)
        {
            *g_pendingCutScene = cutScene;
            game::tail(setCameraState, kCameraStateCutScene);
            return;
        }
        const uint32_t shown = calls::cameraShown(*g_gameCamera);
        showCamera(*g_gameCamera, 1);
        zge::setCutSceneCamera(cameraSceneObject(*g_gameCamera));
        zge::startCutScene(cutScene, 1);
        showCamera(*g_gameCamera, shown);
        game::tail(refreshPathMen);
    }
    READABLE_FUNCTION(playCutScene, Verified);

    // cutSceneStateFrame: the cut scene state's frame. By the frames since
    // the state began: the screen fades out; at 90 the walkers, cars and
    // pickups are hidden, the NPCs kept, the objects' animations paused, the
    // current player's rider and bike and camera hidden, the game's camera
    // made the scene's and the scene started (its animators run six frames
    // on); at 91 the scene's sound starts and the screen fades in. From
    // frame 241 a button, or the camera's animation near its end, ends the
    // scene: the screen fades out, and 90 frames on the scene is run to its
    // end, the rumble stopped, play entered, the rider reset at its spawn,
    // the player's camera and the level's objects put back. Returns 1 on
    // the frame the scene ends.
    int32_t cutSceneStateFrame()
    {
        ee::Local<Rumble> noRumble;

        const uint32_t frames = *g_stepFrames + 1;
        *g_stepFrames = frames;
        switch (frames)
        {
        case 1:
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeOut);
            setPlayPhase(2);
            break;

        case kCutSceneSetUpFrame:
        {
            showObjectsNamed(kPathManName, 0);
            showObjectsNamed(kFastPathManName, 0);
            showObjectsNamed(kCarName, 0);
            showObjectsNamed(kFastCarName, 0);
            showObjectsNamed(kPickupName, 0);
            setNpcPlay(kNpcPlayKept);
            pauseObjectAnimations();
            const ee::Ptr<rider::Rider> rider = playerRider(*g_currentPlayer);
            zge::setSceneObjectShown(rider->messenger, 0);
            zge::setSceneObjectShown(rider->bikeModel, 0);
            showCamera(playerActionCamera(*g_currentPlayer), 0);
            zge::setCutSceneCamera(cameraSceneObject(*g_gameCamera));
            showCamera(*g_gameCamera, 1);
            setCameraMode(*g_gameCamera, 0, kCameraModeCutScene);
            setCameraViewportLayout(*g_gameCamera, 1);
            setCameraHearing(*g_gameCamera, 0);
            zge::startCutScene(*g_pendingCutScene, 0);
            for (uint32_t frame = 0; frame < 6; ++frame)
                zge::updateAnimators(kFrameSeconds);
            updateCameras();
            *g_cameraStateStep = kCutSceneFadingIn;
            break;
        }

        case kCutSceneShownFrame:
            soundEvent(kCutSceneStartSound, *g_pendingCutScene, 0, 0, 0);
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeIn);
            break;

        case kCutSceneFadeResetFrame:
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
            soundEvent(kCutSceneEndSound, 0, 0, 0, 0);
            break;

        case kCutScenePlayingFrame:
            *g_cameraStateStep = kCutScenePlaying;
            break;

        case kCutSceneSkippableFrame:
            *g_cameraStateStep = kCutSceneSkippable;
            break;

        default:
            break;
        }

        switch (*g_cameraStateStep)
        {
        case kCutSceneSkippable:
            if (rider::padButtons(*g_currentPlayer, kSkipButtons, 0) != 0 ||
                calls::animationTimeLeft(cameraSceneObject(*g_gameCamera)) <= kCutSceneEndSeconds)
            {
                playOverlayFade(kOverlayWholeScreen, kOverlayFadeOut);
                *g_cutSceneEndFrames = 0;
                *g_cameraStateStep = kCutSceneEnding;
                setPlayPhase(0);
                return 0;
            }
            [[fallthrough]];
        case kCutScenePlaying:
            zge::updateAnimators(kFrameSeconds);
            advanceEffectsClock();
            updateCameras();
            return 0;

        case kCutSceneEnding:
        {
            const uint32_t endFrames = *g_cutSceneEndFrames + 1;
            *g_cutSceneEndFrames = endFrames;
            if (endFrames != kCutSceneEndFrames)
                return 0;
            zge::startCutScene(*g_pendingCutScene, 1);
            zge::updateAnimators(kFrameSeconds);
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                noRumble->timing = 2;
                noRumble->strength = 0;
                stopRumble(player, noRumble);
            }
            setCameraState(kCameraStatePlay);
            rider::resetPhysics(playerRider(*g_currentPlayer), 0);
            rider::riderReset(riderIndexOf(*g_currentPlayer), 1);
            setCameraViewportLayout(*g_gameCamera, 0);
            showCamera(*g_gameCamera, 0);
            showCamera(playerActionCamera(*g_currentPlayer), 1);
            setCameraMode(playerActionCamera(*g_currentPlayer), 1, 3);
            setCameraMode(playerActionCamera(*g_currentPlayer), 1, 0);
            refreshPathMen();
            showObjectsNamed(kCarName, 1);
            showObjectsNamed(kFastCarName, 1);
            showObjectsNamed(kPickupName, 1);
            resumeObjectAnimations();
            setNpcPlay(kNpcPlayTurn);
            zge::endCutScene();
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeIn);
            setPlayPhase(1);
            return 1;
        }

        default:
            return 0;
        }
    }
    READABLE_FUNCTION(cutSceneStateFrame, Verified);

    // state6Frame: 600 frames, then play. Returns 1 on the frame it ends.
    int32_t state6Frame()
    {
        const uint32_t frames = *g_stepFrames + 1;
        *g_stepFrames = frames;
        if (frames == 1 || frames != kState6Frames)
            return 0;
        setCameraState(kCameraStatePlay);
        return 1;
    }
    READABLE_FUNCTION(state6Frame, Draft);

    // state8Frame: physics.c says whether the state has ended (it always
    // has); the level's contacts run, the level's objects and animators too
    // when a setting of the game's data says so, and a shown rider whose
    // physics state is 2 has its frame. Ended, play is entered. Returns
    // whether it ended.
    int32_t state8Frame()
    {
        const int32_t ended = calls::cameraState8Ended();
        runLevelContacts();
        if (*g_state8RunsObjects != 0)
        {
            zge::updateObjects(kFrameSeconds);
            entitiesFrameNoop();
            zge::updateAnimators(kFrameSeconds);
        }
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const ee::Ptr<rider::Rider> rider = playerRider(player);
            if (rider::hasFlag(rider.get(), kRiderShown) && rider::physicsState(rider.get()) == kPhysicsStateTwo)
                rider::riderFrame(riderIndexOf(player));
        }
        advanceEffectsClock();
        if (ended != 0)
            setCameraState(kCameraStatePlay);
        return ended;
    }
    READABLE_FUNCTION(state8Frame, Draft);

    namespace
    {
        constexpr uint32_t kSettableActionFlags = 0x1FFFFFFF;
    }

    void setActionFlags(uint32_t flags, uint32_t on)
    {
        const uint32_t settable = flags & kSettableActionFlags;
        if (on)
            *g_actionFlags |= settable;
        else
            *g_actionFlags &= ~settable;
    }
    READABLE_FUNCTION(setActionFlags, Verified);
}
