// BMX XXX's level state's frame and the frame functions of play and of a
// game screen (action.c). BMX XXX's own code, DM2's line
// (games/dm2/action: levelStateFrame, actionFrame, screenModeFrame).

#include "level_frame.h"

#include "action_state.h"
#include "camera_state.h"
#include "bmx/game/game_frame.h"
#include "bmx/game/npcs.h"
#include "bmx/fx/effects_clock.h"
#include "bmx/game/player_riders.h"
#include "bmx/game/round.h"
#include "bmx/input/input_record.h"
#include "bmx/input/pads.h"
#include "bmx/level/entities.h"
#include "bmx/level/level_collision.h"
#include "bmx/overlay/screens.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/ui/menu.h"
#include "bmx/weapon/player_weapon.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/action/level_frame.cpp.h"

namespace game::bmx
{
    using namespace action;

    namespace
    {
        // The level's frame runs once every `interval` game frames (1: every
        // frame); the countdown to its next run. An interval of -1 runs it
        // once more and holds R2 down on the first pad from then on
        // (nothing in the game sets it).
        inline ee::Global<int32_t, sym::g_actionFrameInterval> g_actionFrameInterval;
        inline ee::Global<int32_t, sym::g_actionFrameCountdown> g_actionFrameCountdown;
        constexpr int32_t kRunOnce = -1;

        // What readPads answered this frame (DM2's name).
        inline ee::Global<uint32_t, sym::g_padReport> g_padReport;
        // The camera states' frame functions.
        inline ee::Global<ee::FunctionPtr<int32_t()>[kCameraStates], sym::g_cameraStateFrames> g_cameraStateFrames;

        // Not 0: the overlay's trees are not drawn [inferred from this use:
        // their "Visible" is set from it every frame].
        inline ee::Global<uint8_t, sym::g_overlayHidden> g_overlayHidden;
        constexpr uint32_t kOverlayName = sym::overlayName;                 // "UI_overlay"
        constexpr uint32_t kSharedDisplaysName = sym::sharedDisplaysName;   // "UI_overDisp_com"
        constexpr uint32_t kFullDisplaysName = sym::fullDisplaysName;       // "UI_overDisp_P_full"
        constexpr uint32_t kLeftDisplaysName = sym::leftDisplaysName;       // "UI_overDisp_P_splitL"
        constexpr uint32_t kRightDisplaysName = sym::rightDisplaysName;     // "UI_overDisp_P_splitR"
        constexpr uint32_t kVisibleName = sym::visibleName;                 // "Visible"

        // A game screen idle this many frames goes to the feature screen;
        // 0: never (nothing read so far sets it).
        inline ee::Global<uint32_t, sym::g_screenIdleFrames> g_screenIdleFrames;
        // What the game was started for (bmx.c's; b7bmxc3's name).
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        // The player whose pad has the menus (the UI's "ModalPlayerNum").
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;

        // Start and Select do nothing in the first frames of play.
        constexpr uint32_t kPauseWaitFrames = 2;

        // A stick's deflection per step: the riders get their sticks in 127
        // steps a side, and the face levels in 127 steps, as a replay
        // records them.
        constexpr ee::Float kInputSteps = 127.0f;
        constexpr ee::Float kInputStep = 1.0f / 127.0f;

        // A menu's directions repeat while held: after a press, this long
        // until the first repeat, then one every few frames (at once while
        // L1 or R1 is held).
        constexpr uint16_t kRepeatDelayFrames = 20;
        constexpr uint16_t kRepeatEveryFrames = 3;
        constexpr uint32_t kShoulderButtons = zge::kPadL1 | zge::kPadR1;

        // The game paused for a player: its sounds, a screen up, the menus
        // that player's.
        int32_t pauseGame(uint32_t screen, uint32_t player)
        {
            setGameMode(kPausedMode);
            showGameScreen(screen);
            *g_modalPlayerNum = static_cast<int32_t>(player);
            return 1;
        }

        // A stick's deflection in whole steps (toward zero, the low eight
        // bits kept as a signed count), as a deflection again; a face
        // level's the same with an unsigned count.
        ee::Float stickInSteps(ee::Float deflection)
        {
            const int8_t steps = static_cast<int8_t>(ee::toInt(deflection * kInputSteps));
            return ee::toFloat(static_cast<int32_t>(steps)) * kInputStep;
        }
        ee::Float levelInSteps(ee::Float level)
        {
            const uint8_t steps = static_cast<uint8_t>(ee::toInt(level * kInputSteps));
            return ee::toFloat(static_cast<int32_t>(steps)) * kInputStep;
        }

        // The tree of the name below the root shown, or hidden.
        void showTree(ee::Address name, ee::Local<uint32_t> &visible)
        {
            const ee::Ptr<zge::zgui::Node> tree = zge::zgui::findNode(zge::zgui::g_zgui->root, name, 1);
            zge::zgui::setVariable(tree, kVisibleName, visible);
        }

        // While these game screens are up the level's objects and animators
        // go on (the NPC riders too under screen 41).
        bool levelRunsBehind(uint32_t screen)
        {
            switch (screen)
            {
            case kScreenPlayAgain:
            case kScreenEndObjectives:
            case kScreenObjectiveDetail:
            case kScreenReward:
            case kScreenHighScore:
            case kScreenNpr:
                return true;
            default:
                return (screen >= kScreenLeaderboard && screen <= kScreenObjectives) ||
                       (screen >= kScreenNewRecords && screen <= kScreenAfterTurn);
            }
        }
    }

    // levelStateFrame: the level state's frame (the game state's frame
    // table). When the level's frame is due the pads are read, the camera
    // state's frame function runs, then the menus' frame. Every frame the
    // overlay's trees are made visible or not. Always 0.
    int32_t levelStateFrame()
    {
        ee::Local<uint32_t> visible;

        const ee::Ptr<zge::Pad> firstPad = rider::padInput(0)->pad;
        --*g_actionFrameCountdown;
        if (*g_actionFrameCountdown == 0)
        {
            *g_actionFrameCountdown = *g_actionFrameInterval;
            *g_padReport = readPads();
            (*g_cameraStateFrames)[cameraState()]();
            menuFrame();
            if (*g_actionFrameInterval == kRunOnce)
            {
                firstPad->buttons |= zge::kPadR2;
                firstPad->changed |= zge::kPadR2;
            }
        }

        *visible = *g_overlayHidden == 0 ? 1 : 0;
        showTree(kOverlayName, visible);
        showTree(kSharedDisplaysName, visible);
        if (*g_playerCount == 1)
        {
            showTree(kFullDisplaysName, visible);
        }
        else
        {
            showTree(kLeftDisplaysName, visible);
            showTree(kRightDisplaysName, visible);
        }
        return 0;
    }
    READABLE_FUNCTION(levelStateFrame, Verified);

    // playStateFrame: play's frame. A shown rider's pad gone missing pauses
    // the game for that player; so does Start, and Select in the first game
    // mode shows the objectives (neither in the first two frames of play):
    // the frame ends there (1). Otherwise the level's objects and animators
    // get their frame, the replays record or play, each player's pad goes
    // (the sticks and face levels in 127ths) to the player's rider, which
    // has its frame, or to the player's weapon for a player who watches;
    // then the NPC riders, the level's contacts, the projectiles, the game,
    // the effects' clock and the cameras (0). With action flag 1 only the
    // game's frame runs.
    int32_t playStateFrame()
    {
        if ((*g_actionFlags & kActionFlag1) == 0)
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                // (A player with no rider reads its flags from low memory,
                // as the original does.)
                const ee::Ptr<rider::Rider> rider = playerRider(player);
                const rider::PadInput *const pad = rider::padInput(player);
                if (rider::hasFlag(&*rider, kRiderShown) && (pad->flags & rider::kPadMissing) != 0)
                    return pauseGame(kScreenPauseNoChoice, player);
            }

            zge::updateObjects(kFrameSeconds);
            entitiesFrameNoop();
            zge::updateAnimators(kFrameSeconds);

            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                // (A riding player's rider is looked up and not used.)
                if (playerSetup(player)->watches == 0)
                    playerRider(player);
                if (rider::padButtons(player, zge::kPadStart, 0) != 0 && *g_playFrames >= kPauseWaitFrames)
                    return pauseGame(kScreenPause, player);
                if (gameMode() == kChallengeMode && rider::padButtons(player, zge::kPadSelect, 0) != 0 &&
                    *g_playFrames >= kPauseWaitFrames)
                    return pauseGame(kScreenPauseObjectives, player);
            }

            replayUpdate();
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                const rider::PadInput *const pad = rider::padInput(player);
                const uint8_t watches = playerSetup(player)->watches;
                ee::Ptr<InputRecord> input;
                if (watches == 1)
                    input = ee::Ptr<InputRecord>(ee::Address(weaponInput(playerWeapon(player))));
                else if (watches == 0)
                    input = ee::Ptr<InputRecord>(ee::Address(&playerRider(player)->input));
                if (!input)
                    continue;

                input->buttons.held = pad->buttons.held;
                input->buttons.changed = pad->buttons.changed;
                for (uint32_t stick = 0; stick < 2; ++stick)
                {
                    const ee::Float previousX = stickInSteps(pad->buttons.previous[stick].x);
                    const ee::Float previousY = stickInSteps(pad->buttons.previous[stick].y);
                    input->buttons.previous[stick].x = previousX;
                    input->buttons.previous[stick].y = previousY;
                    const ee::Float x = stickInSteps(pad->buttons.sticks[stick].x);
                    const ee::Float y = stickInSteps(pad->buttons.sticks[stick].y);
                    input->buttons.sticks[stick].x = x;
                    input->buttons.sticks[stick].y = y;
                }
                for (uint32_t level = 0; level < rider::kPadFaceLevels; ++level)
                {
                    input->previousLevels[level] = levelInSteps(pad->previousFaceLevels[level]);
                    input->levels[level] = levelInSteps(pad->faceLevels[level]);
                }

                if (watches == 0)
                {
                    if (rider::hasFlag(&*playerRider(player), kRiderShown))
                        rider::riderFrame(riderIndexOf(player));
                }
                else if (watches == 1)
                {
                    updatePlayerWeapon(playerWeapon(player));
                }
            }

            npcsFrame();
            runLevelContacts();
            updateProjectiles();
        }

        gameRiderFrame();
        if ((*g_actionFlags & kActionFlag1) == 0)
        {
            advanceEffectsClock();
            updateCameras();
            calls::updateCarriedObjects(kFrameSeconds);
        }
        ++*g_playFrames;
        return 0;
    }
    READABLE_FUNCTION(playStateFrame, Verified);

    // screenStateFrame: the frame while a game screen is up. Each pad's
    // directions repeat for the menus, and the screens get their input. If
    // that left the screen state the frame ends (1). A screen idle long
    // enough goes to the feature screen where the game was started for
    // that (1). Under some screens the level's objects go on; the cameras
    // always do (0).
    int32_t screenStateFrame()
    {
        for (uint32_t port = 0; port < 2; ++port)
        {
            rider::PadInput *const pad = rider::padInput(port);
            if ((pad->buttons.changed & ~kShoulderButtons) != 0)
            {
                pad->repeatWait = kRepeatDelayFrames;
                *g_stepFrames = 0;
            }
            else if (pad->repeatWait != 0)
            {
                --pad->repeatWait;
            }
            else
            {
                const uint32_t held = pad->buttons.held;
                pad->buttons.changed = held & rider::kButtonDirections;
                pad->repeatWait = (held & kShoulderButtons) != 0 ? 0 : kRepeatEveryFrames;
            }
        }

        gameScreensInput();
        if (cameraState() != kCameraStateScreen)
            return 1;

        if (*g_screenIdleFrames != 0)
        {
            const uint32_t frames = *g_stepFrames + 1;
            *g_stepFrames = frames;
            if (*g_screenIdleFrames < frames && gameScreen() != kScreenFeature && gameScreen() != kScreenLeave &&
                gameScreen() != kScreenLeaveToMovie)
            {
                const uint8_t kind = *g_startUpKind;
                if (kind < 4 && kind != 0)
                {
                    for (uint32_t player = 0; player < *g_playerCount; ++player)
                        showCamera(playerActionCamera(player), 0);
                    showCamera(*g_gameCamera, 1);
                    showGameScreen(kScreenFeature);
                    return 1;
                }
            }
        }

        const uint32_t screen = gameScreen();
        if (levelRunsBehind(screen))
        {
            if (screen == kScreenNpr)
                npcsFrame();
            zge::updateObjects(kFrameSeconds);
            entitiesFrameNoop();
            zge::updateAnimators(kFrameSeconds);
        }
        updateCameras();
        return 0;
    }
    READABLE_FUNCTION(screenStateFrame, Verified);
}
