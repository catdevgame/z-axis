// actionStart (action.h): the action leaves the mode it is in and starts
// another. DM2's own code (action.c).

#include "action.h"
#include "action_state.h"
#include "../camera/camera.h"
#include "../game/npcs.h"
#include "../game/players.h"
#include "../game/round.h"
#include "../sound/sound_mode.h"
#include "../input/pads.h"
#include "../overlay/overlay.h"
#include "../replay/replays.h"
#include "../rider/rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/action/action_start.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        ee::Ptr<Rider> riderOf(uint32_t index); // rider_state.cpp
    }

    namespace
    {
        // zgui's root node (Zgui +0x0C, as AI's g_uiRoot).
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        // The in-game UI's layers the modes show and hide, and the variable
        // that does it.
        constexpr ee::Ptr<const char> kPlayOverlay{sym::inGameOverlayPath};         // "InGameUI.Overlay"
        constexpr ee::Ptr<const char> kOverlayToggle{sym::overlayTogglePath};       // "InGameUI.OverlayToggle"
        constexpr ee::Ptr<const char> kReplayOverlay{sym::overlayReplayPath};       // "InGameUI.OverlayReplay"
        constexpr ee::Ptr<const char> kDemoOverlay{sym::overlayReplayDemoPath};     // "InGameUI.OverlayReplay.Demo"
        constexpr ee::Ptr<const char> kVisible{sym::actionTextVisible};             // "Visible"

        // Scaffolding (unnamed-field): overlay frames overlay.h has no name
        // for yet. Frame 9 is told a replay's state (kReplayIdle,
        // kReplayPlaying) by the replay modes.
        constexpr uint32_t kOverlayFrame5 = 5;
        constexpr uint32_t kOverlayFrame6 = 6;
        constexpr uint32_t kOverlayFrame9 = 9;
        constexpr uint32_t kOverlayFrame15 = 15;
        constexpr uint32_t kOverlayFrame16 = 16;

        // The overlay frames of play, in the order they are shown and hidden.
        constexpr uint32_t kPlayFrames[] = {
            kOverlayFrame6, kOverlayFrame5, kOverlayTrick, kOverlayScore,
            kOverlayTimer, kOverlayObjective, kOverlayReward, kOverlayExitWarning,
        };

        // Scaffolding (unnamed-field): what the action tells the sound
        // events (setPlayPhase) in a single-player game: 2 as a replay
        // starts, 4 as one ends. The sound events' own names: not read yet.
        constexpr uint32_t kSoundPhaseReplayStart = 2;
        constexpr uint32_t kSoundPhaseReplayEnd = 4;

        // Scaffolding (address-call): the effects' two empty functions have
        // no readable body yet.
        namespace calls
        {
            void fxStub() { ee::game(sym::fxStub); }
            void fxStubB() { ee::game(sym::fxStubB); }
        }

        void showPlayFrames(uint32_t shown)
        {
            for (const uint32_t frame : kPlayFrames)
                showOverlayFrame(frame, shown);
        }

        void hidePlayers()
        {
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                spawnPlayer(player, 0);
        }

        // The player whose rider a replay follows.
        uint8_t replayPlayer(uint8_t replay)
        {
            return rider::riderOf(replayRider(replay))->player;
        }
    }

    // actionStart (mode): the mode the action is in is wound up (play's
    // overlay hidden; a replay stopped and its rider taken out, the camera
    // back at the level's point), then the new one set up (play's overlay
    // shown and the pads given back; a replay started from a ground camera
    // with the replay overlay). Returns the mode.
    uint8_t actionStart(uint32_t mode)
    {
        ee::Local<uint8_t> replayState;
        ee::Local<uint8_t> noSongInfo;
        ee::Local<uint8_t> noTip;
        ee::Local<uint32_t> visible;
        // DM2's findNode takes no kind: the shared declaration's last
        // parameter is passed as 0.
        const auto setLayerShown = [&](ee::Ptr<const char> path, uint32_t shown) {
            *visible = shown;
            zge::zgui::setVariable(zge::zgui::findNode(*g_uiRoot, path, 0), kVisible, visible);
        };

        *g_actionRequest = kNoActionRequest;
        *g_actionStepFrames = 0;
        *g_actionStartCleared = 0;

        switch (*g_actionMode)
        {
        case kActionPlay:
            setLayerShown(kPlayOverlay, 0);
            setLayerShown(kOverlayToggle, 0);
            showPlayFrames(0);
            *g_actionFlags &= ~kActionFlag1;
            break;
        case kActionScreen:
            if (replayBehindScreen(gameScreen(), kScreenGameOver))
            {
                const uint8_t replay = shownReplay();
                setReplayState(replay, kReplayIdle);
                spawnPlayer(replayPlayer(replay), 0);
                setCameraMode(*g_currentPlayer, 0, kCameraLevelPoint);
                setCameraPlayer(*g_currentPlayer);
            }
            break;
        case kActionRoundReplay:
        case kActionDemo:
        case kActionViewReplay:
        {
            uint8_t replay;
            if (*g_actionMode == kActionRoundReplay)
                replay = shownReplay();
            else
                replay = *g_actionMode == kActionViewReplay ? *g_viewedReplay : *g_demoReplay;
            setReplayState(replay, kReplayIdle);
            spawnPlayer(replayPlayer(replay), 0);
            if (multiplayerMode() == kHorseMode)
                spawnPlayer(*g_currentPlayer, 1);
            *replayState = kReplayIdle;
            overlayMessage(kOverlayFrame9, replayState);
            *noSongInfo = kOverlayNone;
            overlayMessage(kOverlaySongInfo, noSongInfo);
            setLayerShown(kOverlayToggle, 0);
            setLayerShown(kReplayOverlay, 0);
            setCameraMode(*g_currentPlayer, 0, kCameraLevelPoint);
            setCameraPlayer(*g_currentPlayer);
            if (multiplayerMode() == 0)
                setPlayPhase(kSoundPhaseReplayEnd);
            break;
        }
        case kActionMode3:
            setReplayState(*g_mode3Replay, kReplayIdle);
            *replayState = kReplayIdle;
            overlayMessage(kOverlayFrame9, replayState);
            setCameraMode(*g_currentPlayer, 0, kCameraFollows);
            setLayerShown(kOverlayToggle, 0);
            setLayerShown(kReplayOverlay, 0);
            *g_actionFlags &= ~kActionFlag2;
            break;
        case kActionMode7:
            calls::fxStubB();
            break;
        default:
            break;
        }

        switch (mode)
        {
        case kActionPlay:
            setLayerShown(kPlayOverlay, 1);
            setLayerShown(kOverlayToggle, 1);
            showPlayFrames(1);
            // The first flag is set, the pads go back to the play, and then
            // only the second flag is kept: the first is clear again.
            *g_actionFlags |= kActionFlag1;
            takePads(0);
            *g_actionFlags &= kActionFlag2;
            break;
        case kActionScreen:
            if (replayBehindScreen(gameScreen(), kScreenGameOver))
            {
                hidePlayers();
                const uint8_t replay = shownReplay();
                const uint8_t player = replayPlayer(replay);
                setCameraPlayer(player);
                setReplayState(replay, kReplayPlaying);
                spawnPlayer(player, 1);
                setNpcPlay(1);
                setCameraMode(player, 0, kCameraOnGround);
                placeGroundCamera(player, kGroundCameraKind10);
            }
            takePads(1);
            break;
        case kActionRoundReplay:
        case kActionDemo:
        case kActionViewReplay:
        {
            const bool ownReplay = mode == kActionDemo || mode == kActionViewReplay;
            hidePlayers();
            setNpcPlay(1);
            uint8_t replay;
            if (ownReplay)
            {
                // The demo is a free ride with no players.
                if (mode == kActionDemo)
                {
                    *g_multiplayerPlayers = 0;
                    setSinglePlayerMode(kFreeRide);
                }
                *g_currentPlayer = 0;
                replay = mode == kActionViewReplay ? *g_viewedReplay : *g_demoReplay;
            }
            else
                replay = shownReplay();
            const uint8_t player = replayPlayer(replay);
            setCameraPlayer(player);
            setReplayState(replay, kReplayPlaying);
            *replayState = kReplayPlaying;
            overlayMessage(kOverlayFrame9, replayState);
            *noTip = kOverlayNone;
            overlayMessage(kOverlayTip, noTip);
            setCameraMode(player, 0, kCameraOnGround);
            setLayerShown(kReplayOverlay, 1);
            if (ownReplay)
                *g_actionFlags &= ~kActionFlag1;
            setLayerShown(kOverlayToggle, ownReplay ? 0 : 1);
            setLayerShown(kDemoOverlay, mode == kActionDemo ? 1 : 0);
            if (multiplayerMode() == 0)
                setPlayPhase(kSoundPhaseReplayStart);
            break;
        }
        case kActionMode3:
        {
            const uint8_t replay = *g_mode3Replay;
            const uint8_t player = replayPlayer(replay);
            setReplayState(replay, kReplayPlaying);
            *replayState = kReplayPlaying;
            overlayMessage(kOverlayFrame9, replayState);
            showOverlayFrame(kOverlayFrame9, 1);
            showOverlayFrame(kOverlayFrame15, 0);
            showOverlayFrame(kOverlayFrame16, 0);
            setCameraMode(player, 0, kCameraOnGround);
            placeGroundCamera(player, kGroundCameraKind1);
            setLayerShown(kOverlayToggle, 0);
            setLayerShown(kReplayOverlay, 0);
            break;
        }
        case kActionMode7:
            calls::fxStub();
            break;
        default:
            break;
        }

        *g_actionMode = static_cast<uint8_t>(mode);
        *g_actionStep = 0;
        return static_cast<uint8_t>(mode);
    }
    READABLE_FUNCTION(actionStart, Verified);
}
