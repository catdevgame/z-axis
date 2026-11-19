// The play modes around the skating: the toggled overlay, the cinematic
// camera and the level's fly-through. Each has the top state machine's
// enter, update and exit functions.

#include "aggressive/camera.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/render/texture_effects.h"

#include <cstddef>
#include <cstdint>
#include "aggressive/sound/events.h"
#include "aggressive/timers.h"
#include "aggressive/pads.h"
#include "aggressive/replay.h"
#include "aggressive/effects.h"
#include "aggressive/game_frame.h"
#include "aggressive/level/dynamics.h"
#include "aggressive/level/pause.h"
#include "aggressive/level/props.h"
#include "aggressive/level/scene_objects.h"

#include "sym/aggressive/play_modes.cpp.h"

namespace aggressive
{
    namespace
    {
        using zgui::Node;

        constexpr uint32_t kApplyLevelPropState = sym::applyLevelPropState;
        constexpr uint32_t kCartonSizeName = sym::g_textCartonSize3;   // the juice meter's "CartonSize"

        constexpr uint32_t kStartButton = 0x4000;
        constexpr uint32_t kControlsVibrate = 0x1u; // Controls::stickFlags: the Vibration option is on for the pad
        constexpr uint32_t kPadFromSnapshot = 0x1u, kPadIgnoreHeld = 0x8u, kPadTakeSnapshot = 0x2u, kPadConnected = 0x20u; // PadState::options
        constexpr uint32_t kTuningCameraReplayType = 0x590u; // the Camera category's Replay Type
        constexpr uint32_t kGameModeCinematic = 2u;          // setGameMode's

        constexpr uint32_t kMultiplayerToggleVisible = sym::g_textMultiplayerUIOverlayToggle2; // "MultiplayerUI.OverlayToggle.Visible"
        constexpr uint32_t kInGameToggleVisible = sym::g_textInGameUIOverlayToggleVisible;      // "InGameUI.OverlayToggle.Visible"
        constexpr uint32_t kHideToggle = sym::g_textSetInGameUIOverlayToggleVisible;               // "set InGameUI.OverlayToggle.visible {0}"
        constexpr uint32_t kHideOverlay = sym::g_textSetInGameUIOverlayVisible3;              // "set InGameUI.Overlay.visible {0}"
        constexpr uint32_t kMultiplayerLetterboxIn = sym::g_textPlayRootMultiplayerUILetterbox;   // "PLAY Root.MultiplayerUI.Letterbox.Init"
        constexpr uint32_t kInGameLetterboxIn = sym::g_textPlayRootInGameUILetterboxInit;        // "PLAY Root.InGameUI.Letterbox.Init"
        constexpr uint32_t kMultiplayerLetterboxOut = sym::g_textPlayRootMultiplayerUILetterbox2;  // "PLAY Root.MultiplayerUI.Letterbox.DeInit"
        constexpr uint32_t kInGameLetterboxCut = sym::g_textPlayRootInGameUILetterboxHideImmed;       // "PLAY Root.InGameUI.Letterbox.HideImmediate"
        constexpr uint32_t kInGameLetterboxOut = sym::g_textPlayRootInGameUILetterboxDeInit;       // "PLAY Root.InGameUI.Letterbox.DeInit"

        // A new round's length by split-screen game; every game has a row.
        constexpr uint32_t kSplitGames = 5u;
        inline constexpr float kSplitGameSeconds[kSplitGames] = {120.0f, 60.0f, 120.0f, 60.0f, 120.0f};
        constexpr float kTimedRunSeconds = 120.0f, kFreakOutSeconds = 10.0f;

        inline ee::Global<ee::Ptr<Node>, sym::g_uiRoot> g_uiRootNode;
        inline ee::Global<uint32_t, sym::g_cameraTuning> g_cameraTuning;
        inline ee::Global<uint32_t, sym::g_flyThruSkipped> g_flyThruSkipped;
        inline ee::Global<uint32_t, sym::g_flyThruActive> g_flyThruActive;

        void run(uint32_t script)
        {
            zgui::runScript(*g_uiRootNode, ee::Ptr<char>(script), 0);
        }

        // The skaters back at the level's start as a fly-through starts or
        // ends: player two too in split screen.
        void respawnPlayers()
        {
            for (int32_t index = 0; index < playerCount(); ++index)
            {
                const ee::Ptr<skater::Player> player = levelPlayer(index);
                player->flags |= skater::kRespawnAtLocator;
                skater::respawn(player, 0, 1);
            }
        }

        ee::Ptr<FollowCamera> followCamera()
        {
            return &g_gameState->cameras[0];
        }
    }

    // ---- the overlay toggle ---------------------------------------------------

    // enterPlayMode: skating starts: the overlay's toggle shown.
    uint32_t enterPlayMode()
    {
        ee::Local<int32_t> value;
        *value = 1;
        const uint32_t toggle = isSplitScreen() ? kMultiplayerToggleVisible : kInGameToggleVisible;
        zgui::setVariable(*g_uiRootNode, ee::Ptr<const char>(toggle), value.address());
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterPlayMode, Verified);

    // exitPlayMode: skating stops: the toggle hidden and a buzz on a pad that
    // has the Vibration option on. Bug kept: in split screen the loop runs for
    // two players but reads player one both times, so only player one's pad
    // ever buzzes, twice. Returns 0 in split screen, else 1.
    int32_t exitPlayMode()
    {
        ee::Local<int32_t> value;
        *value = 0;
        const uint32_t toggle = isSplitScreen() ? kMultiplayerToggleVisible : kInGameToggleVisible;
        zgui::setVariable(*g_uiRootNode, ee::Ptr<const char>(toggle), value.address());

        for (int32_t buzzed = 0; buzzed < playerCount(); ++buzzed)
        {
            const ee::Ptr<skater::Player> player = levelPlayer(0);
            const bool connected = (g_gameState->pads[player->controls.port].options & kPadConnected) != 0;
            if (connected && (player->controls.stickFlags & kControlsVibrate))
                vibrate(&player->controls, 5, ee::Float(0.0f), ee::Float(0.1f));
        }
        return isSplitScreen() ? 0 : 1;
    }
    READABLE_FUNCTION_WAITS(exitPlayMode, Verified);

    // ---- the cinematic camera ---------------------------------------------------

    // enterCinematicCameraMode: the overlay away and the follow camera in the tuning's
    // replay type.
    uint32_t enterCinematicCameraMode()
    {
        setGameMode(kGameModeCinematic);
        run(kHideToggle);
        run(kHideOverlay);
        const ee::Float replayType = *ee::Ptr<ee::Float>(*g_cameraTuning + kTuningCameraReplayType);
        setFollowCameraMode(followCamera(), ee::toInt(replayType) & 0xFFu);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterCinematicCameraMode, Verified);

    int32_t updatePlayMode(ee::Float seconds);

    uint32_t updateCinematicCameraMode(ee::Float seconds)
    {
        if (currentLevel())
            updatePlayMode(seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateCinematicCameraMode, Verified);

    namespace
    {
        namespace tail
        {
        uint32_t setFollowCameraMode(ee::Ptr<FollowCamera> camera, uint32_t newMode) { return game::tail(aggressive::setFollowCameraMode, camera, newMode); }
        }
    }

    int32_t exitCinematicCameraMode()
    {
        return static_cast<int32_t>(tail::setFollowCameraMode(followCamera(), 0));
    }
    READABLE_FUNCTION_WAITS(exitCinematicCameraMode, Verified);

    // ---- the fly-through --------------------------------------------------------

    // enterFlyThruMode: the pads take a snapshot (a pad not there has none), the
    // letterbox comes in. Started from the pause menu (the level state says
    // so) it is a challenge's preview; else the skaters go to their start.
    uint32_t enterFlyThruMode()
    {
        for (skater::PadState &pad : g_gameState->pads)
        {
            if (pad.options & kPadConnected)
                pad.options |= kPadTakeSnapshot;
            else
                pad.heldAtSnapshot = 0xFFFFFFFFu;
        }
        run(isSplitScreen() ? kMultiplayerLetterboxIn : kInGameLetterboxIn);
        if ((currentLevel()->flags & kLevelStopReasonBits) == kStopFlyThru)
            *g_flyThruActive = 1;
        if (*g_flyThruActive == 0)
            respawnPlayers();
        *g_flyThruSkipped = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterFlyThruMode, Verified);

    namespace
    {
        namespace tail
        {
        void stopAudioSync() { game::tail(aggressive::stopAudioSync); }
        }
    }

    // checkFlyThruSkip: START skips it: the camera back, the animators run far
    // past the end, the audio sync stopped.
    void checkFlyThruSkip()
    {
        if (!skater::padHeld(kStartButton, 0))
            return;
        if (!skater::padPressed(kStartButton, 0))
            return;
        *g_flyThruSkipped = 1;
        setFollowCameraMode(followCamera(), 0);
        zge::updateAnimators(ee::Float(3600.0f));
        tail::stopAudioSync();
    }
    READABLE_FUNCTION_WAITS(checkFlyThruSkip, Verified);

    namespace
    {
        void updateEffectsFrame(ee::Float seconds) { updateLevelEffects(seconds); }
    }

    // updateLevel: the level's own frame: its objects, the animators, the
    // effects, the textures and the props. A preview fly-through freezes it.
    void updateLevel(ee::Float seconds)
    {
        if (*g_flyThruActive == 0)
        {
            level::levelObjectsFrame(seconds);
            zge::updateAnimators(seconds);
            zge::updateObjects(seconds);
            updateEffectsFrame(seconds);
            zge::animateTextures(seconds);
            level::animateProps(seconds);
            game::tail(levelFrame3, seconds);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(updateLevel, Verified);

    // updateFlyThruMode: START can skip it; the level runs only while the
    // fly-through is still the level's mode.
    uint32_t updateFlyThruMode(uint32_t /*unused*/, ee::Float seconds)
    {
        checkFlyThruSkip();
        if (currentLevel()->mode->id == kModeFlyThru)
            updateLevel(seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateFlyThruMode, Verified);

    // exitFlyThruMode: the buttons held since the snapshot don't count, the
    // letterbox goes (at once when skipped), the skaters go to their start
    // and the follow camera cuts to them on its next update.
    uint32_t exitFlyThruMode()
    {
        for (skater::PadState &pad : g_gameState->pads)
            pad.options |= kPadFromSnapshot | kPadIgnoreHeld;
        const bool skipped = *g_flyThruSkipped != 0;
        if (isSplitScreen())
            run(kMultiplayerLetterboxOut);
        else
            run(skipped ? kInGameLetterboxCut : kInGameLetterboxOut);
        if (*g_flyThruActive == 0)
            respawnPlayers();
        g_gameState->cameras[0].snap = 1;
        *g_flyThruSkipped = 0;
        *g_flyThruActive = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(exitFlyThruMode, Verified);

    // enterGameMode: the level changes play mode: the old mode's exit, then
    // the new one's enter.
    int32_t enterGameMode(uint32_t index)
    {
        const ee::Ptr<Level> level = currentLevel();
        if (level->mode)
            level->mode->exit();
        level->mode = ee::Ptr<PlayMode>(sym::modeRecords) + index;
        return level->mode->enter();
    }
    READABLE_FUNCTION(enterGameMode, Verified);

    namespace
    {
        int32_t applyLevelPropState() { return ee::game<int32_t>(kApplyLevelPropState); }
    }

    // playAgain: Play Again, from a timed run's or a split-screen round's
    // pause item (the Restart override's key too): the players back at their
    // start with scores and juice cleared, half their juice carton filled,
    // and a new timer, by the split-screen game's table in split screen.
    int32_t playAgain()
    {
        const ee::Ptr<Level> level = currentLevel();
        if (level->timer != 0)
        {
            hideVisibleTimer();
            releaseTimer(level->timer);
        }
        ee::Float juice = ee::Float(0.0f);
        for (int32_t index = 0; index < playerCount(); ++index)
        {
            const ee::Ptr<skater::Player> player = levelPlayer(index);
            player->flags |= skater::kRespawnAtLocator;
            skater::respawn(player, 1, 1);
            skater::setSkaterPoints(player, 0);
            skater::setSkaterScore(player, 0, 0);
            skater::setSplitScore(player, 0);
            ee::Local<int32_t> cartonSize;
            *cartonSize = -1;
            if (zgui::copyVariable(player->juiceMeter, ee::Ptr<const char>(kCartonSizeName), ee::Ptr<char>(cartonSize.address())) != 0)
                juice = ee::toFloat(*cartonSize);
            juice = juice / ee::Float(2.0f);
            skater::setJuiceMeter(player->juiceMeter, juice);
            skater::setJizzed(player, 0);
        }

        ee::Float seconds = ee::Float(kTimedRunSeconds);
        if (isSplitScreen())
            seconds = level->splitGame < kSplitGames ? ee::Float(kSplitGameSeconds[level->splitGame]) : ee::Float(0.0f);
        const uint32_t timer = createTimer(sym::endTimedRun, 0, sym::timerFreakOut, 0, seconds, ee::Float(kFreakOutSeconds));
        level->timer = timer;
        setVisibleTimer(timer);
        return applyLevelPropState();
    }
    READABLE_FUNCTION_WAITS(playAgain, Verified);

    // updatePlayMode: a frame of play: replay or pads (a replay can change the
    // frame's time), the pause buttons, and in play the objects, the players
    // (unless the level holds them) and the effects.
    int32_t updatePlayMode(ee::Float frameSeconds)
    {
        ee::Local<ee::Float> seconds;
        *seconds = frameSeconds;
        replayAndPads(seconds);
        checkPauseButtons();
        if (currentLevel()->mode->id == kModePlaying)
        {
            zge::updateObjects(*seconds);
            if ((currentLevel()->flags & kLevelPlayersHeld) == 0)
                updatePlayers(*seconds);
            levelEffectsFrame(*seconds);
            stepParticleEmitters();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updatePlayMode, Verified);
}
