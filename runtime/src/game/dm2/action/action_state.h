#pragma once

// The action's own state (action.c), shared by its files: the mode, where
// the mode's frame function is in its course, and the replays the level's
// set-up makes for the demo and for a replay loaded from a memory card.

#include "../overlay/overlay.h"

#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/action/action_state.h.h"

namespace game::dm2
{
    // The action modes (actionStart's argument, actionMode): which frame
    // function the level state runs.
    enum ActionMode : uint8_t
    {
        kActionPlay = 0,        // the riders ride
        kActionScreen = 1,      // a game screen is up (the pause menu, the round's screens)
        kActionRoundReplay = 2, // the round's replay (the game's shownReplay) plays
        // Scaffolding (unnamed-field): mode 3 plays the replay kept in
        // g_mode3Replay from a ground camera; nothing read so far starts it.
        kActionMode3 = 3,
        kActionDemo = 4,        // the demo: one of the game's own replays plays ("InGameUI.OverlayReplay.Demo")
        kActionViewReplay = 5,  // a replay loaded from a memory card plays (the menu's "BmxViewReplay")
        // Scaffolding (unnamed-field): mode 7's start and end call two empty
        // functions of the effects' code; nothing read so far starts it.
        kActionMode7 = 7,
    };

    inline ee::Global<uint8_t, sym::g_actionMode> g_actionMode;
    // Where the mode's frame function is in its course (each mode counts its
    // own steps; actionStart puts it back to 0), and the frames it has
    // counted there.
    inline ee::Global<uint8_t, sym::g_actionStep> g_actionStep;
    inline ee::Global<uint32_t, sym::g_actionStepFrames> g_actionStepFrames;
    // A word every start clears; nothing read so far reads it.
    inline ee::Global<uint32_t, sym::g_actionStartCleared> g_actionStartCleared;
    // Play's steps: Start does nothing for the first frames of play; from
    // then on it pauses the game. A pad lost while Start is live pauses at
    // once; one lost before that pauses when the wait is over.
    constexpr uint8_t kPlayStarting = 0;
    constexpr uint8_t kPlayPausable = 1;
    constexpr uint8_t kPlayPadLost = 2;

    // Scaffolding (unnamed-field): two bits actionStart keeps. Bit 0 is
    // cleared when play ends and when the demo or a viewed replay starts;
    // play's start sets it and then keeps bit 1 alone. Bit 1 is cleared when
    // mode 3 ends. Their readers are the replay modes' frame (not read yet).
    inline ee::Global<uint16_t, sym::g_actionFlags> g_actionFlags;
    constexpr uint16_t kActionFlag1 = 0x1;
    constexpr uint16_t kActionFlag2 = 0x2;

    // What the overlay's script functions ask of the replay modes' frame
    // (a signed byte; kNoActionRequest: nothing asked). What each value asks:
    // the replay modes' frame, not read yet.
    inline ee::Global<int8_t, sym::g_actionRequest> g_actionRequest;
    constexpr int8_t kNoActionRequest = -1;

    // The replays the level's set-up makes to be watched: the demo's (one
    // of the game's own, from its replay library) and the one a memory card
    // gave; and mode 3's.
    inline ee::Global<uint8_t, sym::g_demoReplay> g_demoReplay;
    inline ee::Global<uint8_t, sym::g_viewedReplay> g_viewedReplay;
    inline ee::Global<uint8_t, sym::g_mode3Replay> g_mode3Replay;

    // The level of the session (8: the park editor's park) and its
    // single-player game.
    inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
    inline ee::Global<uint8_t, sym::g_sessionSinglePlayerMode> g_sessionSinglePlayerMode;
    constexpr uint8_t kParkLevel = 8;

    // Whether the round's replay runs behind a game screen: the screens from
    // just past the leaderboard to `last`, and the multiplayer time-up
    // screen.
    constexpr bool replayBehindScreen(uint32_t screen, uint32_t last)
    {
        return screen > kScreenLeaderboard && (screen <= last || screen == kScreenMultiplayerTimeUp);
    }

    // Scaffolding (unnamed-field): the ground camera's kinds the action asks
    // for (placeGroundCamera; camera.h has no names for them yet).
    constexpr uint32_t kGroundCameraKind1 = 1;
    constexpr uint32_t kGroundCameraKind10 = 10;
}
