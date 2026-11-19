// The action modes' frame function for the replays (action.h): the round's
// replay, mode 3's, the demo and a replay loaded from a memory card, and
// mode 7. DM2's own code (action.c).

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
#include "../overlay/screens.h"
#include "../replay/replays.h"
#include "../rider/rider.h"
#include "../sound/sound_mode.h"

#include "dm2/sound/sound_events.h"
#include "dm2/camera/follow_camera.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/scene/message.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/action/replay_frame.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        ee::Ptr<Rider> riderOf(uint32_t index);                              // rider_state.cpp
        int32_t riderFrame(uint32_t index);                                  // actions/frame.cpp
    }

    namespace
    {
        // The replay modes' steps (g_actionStep).
        enum ReplayStep : uint8_t
        {
            kStepStarting = 0,      // the replay restarted, the picture fading in from white (120 frames)
            kStepRunning = 1,       // it plays: Start leaves, Square hides the overlays, Circle the overlays
                                      // with a free camera, Cross the next ground camera, Select the overlay,
                                      // Triangle starts it over
            kStepLeaving = 2,       // fading out (120 frames), then the mode left
            kStepOverlaysHidden = 3, // the overlays hidden, the replay held: any of the four face buttons goes on
            kStepRestarting = 4,    // started over: 60 frames, then running again
            kStepFreeCamera = 5,    // the overlays hidden, the ground camera's kind 0 (the kind before kept)
            kStepPlayedOut = 6,     // played to its end (but in mode 3 or a viewed replay): 60 frames, then leaving
            kStepMode7 = 7,         // mode 7: the level and the replay's rider run 480 frames, then leaving
        };
        constexpr uint32_t kFadeFrames = 120;
        constexpr uint32_t kWaitFrames = 60;
        constexpr uint32_t kMode7Frames = 480;
        constexpr uint8_t kWhite = 0xFF;
        // The camera kinds Cross goes through (placeGroundCamera's), round to 0.
        constexpr uint32_t kLastGroundCameraKind = 10;
        // The replay mark's states the modes send: playing ("P"), held ("p"),
        // none.
        constexpr uint8_t kMarkHeld = 3;

        // The replay mode's buttons (any player's, or the one asked for).
        constexpr uint32_t kFaceButtons = zge::kPadTriangle | zge::kPadSquare | zge::kPadCircle | zge::kPadCross;

        // The ground camera's kind before the free camera (put back after it).
        inline ee::Global<uint8_t, sym::g_savedGroundCameraKind> g_savedGroundCameraKind;
        // The saved settings' flags (a halfword beside the volumes).
        // Scaffolding (unnamed-field): what sets bit 0 is not read yet; with
        // it set the replay's buttons do nothing but R2 holds the zoom.
        inline ee::Global<uint16_t, sym::g_settingFlags> g_settingFlags;
        constexpr uint16_t kSettingFlag1 = 0x1;
        inline ee::Global<uint32_t, sym::g_cameraZoomHeld> g_cameraZoomHeld;
        // A frame of the game's time as the replay and mode 7 hand it to the
        // level's objects (a sixtieth of a second).
        inline ee::Global<ee::Float, sym::replayFrameSeconds> replayFrameSeconds;
        inline ee::Global<ee::Float, sym::mode7FrameSeconds> mode7FrameSeconds;

        constexpr ee::Ptr<const char> kOverlayReplay{sym::overlayReplayPath};   // "InGameUI.OverlayReplay"
        constexpr ee::Ptr<const char> kOverlayToggle{sym::overlayTogglePath};   // "InGameUI.OverlayToggle"
        constexpr ee::Ptr<const char> kVisibleName{sym::actionTextVisible};     // "Visible"
        // A viewed replay's exit label and the fade filter's scripts.
        constexpr ee::Ptr<char> kExitLabelShown{sym::replayExitShownScript};   // "SET ...LabelReplayExit.Visible {1}"
        constexpr ee::Ptr<char> kExitLabelHidden{sym::replayExitHiddenScript}; // "SET ...LabelReplayExit.Visible {0}"
        constexpr ee::Ptr<char> kFadeOut{sym::replayFadeOutScript};            // "PLAY InGameUI.FadeFilter.ReplayFadeOut"
        constexpr ee::Ptr<char> kFadeFilterHidden{sym::fadeFilterHiddenScript}; // "SET InGameUI.FadeFilter.Visible {0}"

        // The sound's play phase (AI's setPlayPhase) as the replay leaves.
        constexpr uint32_t kPlayPhaseStopped = 4;
        // The sound event a held replay plays.
        constexpr int32_t kHeldSound = 0x33;

        bool takesInput(uint32_t player)
        {
            return *g_actionRequest == kNoActionRequest || *g_actionRequest == static_cast<int8_t>(player);
        }
        void showOverlays(ee::Local<uint32_t> &visible, uint32_t replayShown, uint32_t toggleShown)
        {
            *visible = replayShown;
            zge::zgui::setVariable(uiWidget(kOverlayReplay), kVisibleName, visible);
            *visible = toggleShown;
            zge::zgui::setVariable(uiWidget(kOverlayToggle), kVisibleName, visible);
        }
        void sendFade(ee::Local<ScreenFade> &fade, uint8_t flags)
        {
            fade->frames = kFadeFrames;
            fade->flags = flags;
            fade->blue = kWhite;
            fade->red = kWhite;
            fade->green = kWhite;
            overlayMessage(kOverlayFade, fade);
        }
        // The replay started over from its first frame, the NPCs with it, the
        // camera on the ground (the round's replay's: its place; the others'
        // kind kept).
        void restartReplay(uint8_t replay, uint8_t player)
        {
            setReplayState(replay, kReplayPlayedOut);
            setReplayState(replay, kReplayPlaying);
            setNpcPlay(kNpcPlayStopped);
            setNpcPlay(kNpcPlayTurn);
            if (actionMode() == kActionRoundReplay)
                setCameraMode(player, 0, kCameraOnGround);
            else
                placeGroundCamera(player, groundCameraKind(player));
            followCamera();
        }
        void setStep(uint8_t step)
        {
            *g_actionStep = step;
            *g_actionStepFrames = 0;
        }
    }

    // replayModeFrame: the frame of the replay modes, by step (ReplayStep):
    // the replay starts over with a fade in, then plays with its buttons;
    // played out, it waits and fades out, and the mode is left (the round's
    // replay to the last game screen, a viewed replay to the front end, the
    // others to play). Answers 0.
    uint32_t replayModeFrame()
    {
        ee::Local<ScreenFade> fade;
        ee::Local<uint8_t> mark;
        ee::Local<uint32_t> visible;
        uint8_t replay;
        const uint8_t mode = actionMode();
        if (mode == kActionMode3)
            replay = *g_mode3Replay;
        else if (mode == kActionViewReplay)
            replay = *g_viewedReplay;
        else
            replay = shownReplay();
        const uint8_t player = rider::riderOf(replayRider(replay))->player;

        switch (*g_actionStep)
        {
        case kStepStarting:
            *g_actionStepFrames = *g_actionStepFrames + 1;
            if (*g_actionStepFrames == 1)
            {
                restartReplay(replay, player);
                sendFade(fade, kFadeFromColour);
                followCamera();
            }
            else if (*g_actionStepFrames == kFadeFrames)
            {
                if (*g_actionMode == kActionViewReplay)
                    runUiScript(kExitLabelShown);
                setStep(kStepRunning);
            }
            return 0;

        case kStepRunning:
        case kStepRestarting:
        case kStepPlayedOut:
        case kStepMode7:
            for (uint32_t port = 0; port < *g_multiplayerPlayers; ++port)
            {
                if (!takesInput(port))
                    continue;
                if (rider::padButtons(port, zge::kPadStart, rider::kAnyPressed))
                {
                    *g_actionStep = kStepLeaving;
                    *mark = kReplayPlayedOut;
                    *g_actionStepFrames = 0;
                    setReplayState(replay, kReplayPlayedOut);
                    setReplayState(replay, kReplayIdle);
                    overlayMessage(kOverlayReplayMark, mark);
                    break;
                }
                if (*g_settingFlags & kSettingFlag1)
                {
                    *g_cameraZoomHeld = rider::padButtons(port, zge::kPadR2, rider::kAllDown);
                    continue;
                }
                if (rider::padButtons(port, zge::kPadSquare, rider::kAnyPressed))
                {
                    setStep(kStepOverlaysHidden);
                    showOverlays(visible, 0, 0);
                    *mark = kMarkHeld;
                    overlayMessage(kOverlayReplayMark, mark);
                    break;
                }
                if (rider::padButtons(port, zge::kPadCircle, rider::kAnyPressed))
                {
                    *g_savedGroundCameraKind = groundCameraKind(player);
                    placeGroundCamera(player, 0);
                    setStep(kStepFreeCamera);
                    showOverlays(visible, 0, 0);
                    *mark = kMarkHeld;
                    overlayMessage(kOverlayReplayMark, mark);
                    break;
                }
                if (rider::padButtons(port, zge::kPadCross, rider::kAnyPressed))
                {
                    if (groundCameraKind(player) < kLastGroundCameraKind)
                        placeGroundCamera(player, groundCameraKind(player) + 1);
                    else
                        placeGroundCamera(player, 0);
                }
                if (rider::padButtons(port, zge::kPadSelect, rider::kAnyPressed))
                {
                    *g_actionFlags = static_cast<uint16_t>(*g_actionFlags ^ kActionFlag1);
                    *visible = *g_actionFlags & kActionFlag1;
                    zge::zgui::setVariable(uiWidget(kOverlayToggle), kVisibleName, visible);
                }
            }

            if (*g_actionStep == kStepRestarting || *g_actionStep == kStepPlayedOut)
            {
                *g_actionStepFrames = *g_actionStepFrames + 1;
                if (*g_actionStepFrames == kWaitFrames)
                    setStep(*g_actionStep == kStepRestarting ? kStepRunning : kStepLeaving);
                followCamera();
                return 0;
            }
            if (*g_actionStep == kStepRunning)
            {
                for (uint32_t port = 0; port < *g_multiplayerPlayers; ++port)
                {
                    if (!takesInput(port))
                        continue;
                    if (rider::padButtons(port, zge::kPadTriangle, rider::kAnyPressed))
                    {
                        restartReplay(replay, player);
                        setStep(kStepRestarting);
                        break;
                    }
                }
            }
            if (*g_actionStep == kStepRunning)
            {
                replayUpdate();
                const uint8_t state = replayState(replay);
                if (state == kReplayPlaying)
                {
                    zge::updateObjects(*replayFrameSeconds);
                    objectSoundsFrame();
                    rider::riderFrame(replayRider(replay));
                    npcsFrame();
                    runLevelContacts();
                    gameRiderFrame();
                    advanceEffectsClock();
                }
                else if (state == kReplayPlayedOut)
                {
                    // Mode 3 and a viewed replay play again; the others are over.
                    if (actionMode() == kActionMode3 || actionMode() == kActionViewReplay)
                    {
                        setReplayState(replay, kReplayPlaying);
                        setNpcPlay(kNpcPlayStopped);
                        setNpcPlay(kNpcPlayTurn);
                        placeGroundCamera(player, groundCameraKind(player));
                        setStep(kStepRestarting);
                    }
                    else
                        setStep(kStepPlayedOut);
                }
                if (*g_actionStep == kStepRunning)
                    followCamera();
                return 0;
            }
            if (*g_actionStep == kStepMode7)
            {
                *g_actionStepFrames = *g_actionStepFrames + 1;
                if (*g_actionStepFrames >= kMode7Frames)
                {
                    setStep(kStepLeaving);
                    return 0;
                }
                zge::updateObjects(*mode7FrameSeconds);
                objectSoundsFrame();
                rider::riderFrame(replayRider(replay));
                runLevelContacts();
                followCamera();
            }
            return 0;

        case kStepLeaving:
            *g_actionStepFrames = *g_actionStepFrames + 1;
            if (*g_actionStepFrames == 1)
            {
                if (*g_actionMode == kActionViewReplay)
                    zge::zgui::runScriptLines({}, kFadeOut);
                else
                    sendFade(fade, 0);
                if (*g_actionMode == kActionViewReplay)
                    runUiScript(kExitLabelHidden);
                if (multiplayerMode() != 0 || actionMode() == kActionMode3)
                    return 0;
                setPlayPhase(kPlayPhaseStopped);
                return 0;
            }
            if (*g_actionStepFrames != kFadeFrames)
                return 0;
            switch (actionMode())
            {
            case kActionRoundReplay:
                showGameScreen(kScreenLast);
                break;
            case kActionViewReplay:
                zge::zgui::runScriptLines({}, kFadeFilterHidden);
                setGameState(kGameStateFrontEnd);
                break;
            default:
                actionStart(kActionPlay);
                break;
            }
            followCamera();
            return 0;

        case kStepOverlaysHidden:
        case kStepFreeCamera:
            for (uint32_t port = 0; port < *g_multiplayerPlayers; ++port)
            {
                if (!takesInput(port))
                    continue;
                if (rider::padButtons(port, zge::kPadStart, rider::kAnyPressed))
                {
                    if (*g_actionStep == kStepFreeCamera)
                        placeGroundCamera(player, *g_savedGroundCameraKind);
                    *g_actionStep = kStepLeaving;
                    *mark = kReplayPlayedOut;
                    *g_actionStepFrames = 0;
                    overlayMessage(kOverlayReplayMark, mark);
                    break;
                }
                if (*g_actionStep == kStepOverlaysHidden &&
                    rider::padButtons(port, zge::kPadCircle, rider::kAnyPressed))
                {
                    *g_savedGroundCameraKind = groundCameraKind(player);
                    placeGroundCamera(player, 0);
                    *g_actionStepFrames = 0;
                    *g_actionStep = kStepFreeCamera;
                    break;
                }
                if (rider::padButtons(port, kFaceButtons, rider::kAnyPressed))
                {
                    if (*g_actionStep == kStepFreeCamera)
                        placeGroundCamera(player, *g_savedGroundCameraKind);
                    setStep(kStepRunning);
                    if (*g_actionFlags & kActionFlag1)
                        showOverlays(visible, 1, *g_actionFlags & kActionFlag1);
                    *mark = kReplayPlaying;
                    overlayMessage(kOverlayReplayMark, mark);
                    break;
                }
            }
            if (*g_actionStep == kStepOverlaysHidden)
            {
                *g_actionStepFrames = *g_actionStepFrames + 1;
                if (*g_actionStepFrames == 1)
                    soundEvent(kHeldSound, 0, 0, 0, 0);
            }
            else if (*g_actionStep == kStepFreeCamera)
                followCamera();
            return 0;

        default:
            return 0;
        }
    }
    READABLE_FUNCTION(replayModeFrame, Verified);
}
