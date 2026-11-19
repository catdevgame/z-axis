#pragma once
// BMX XXX's camera states (action.c): what the game's cameras and overlay
// are doing as a whole in a level: play, a game screen, a replay, a cut
// scene. Each state has a frame function the level's frame runs
// (action/level_frame.cpp); setCameraState leaves one and enters another.
// DM2's line: its action mode (game/dm2/action: actionStart, actionMode).

#include <cstdint>

namespace game::bmx
{
    // The camera state (cameraState reads it). Play, Screen, Replay and
    // ReplayDemo are named by the game screens that set them
    // (overlay/screen_calls.h); CutScene by what entering it shows (the
    // "CutScene.Subtitle" display) [inferred]. States 3, 5, 6 and 8 have no
    // name yet (value-named until who sets them is read): 3 plays a second
    // replay on the current player's camera with a camera shot; 5 is a
    // replay like 2 and 4 that goes to another game state at its end; 6
    // waits 600 frames and goes to play; 8 is physics.c's (its enter does
    // nothing, its exit shows the first player's camera).
    constexpr uint32_t kCameraStatePlay = 0;
    constexpr uint32_t kCameraStateScreen = 1;
    constexpr uint32_t kCameraStateReplay = 2;
    constexpr uint32_t kCameraState3 = 3;
    constexpr uint32_t kCameraStateReplayDemo = 4;
    constexpr uint32_t kCameraState5 = 5;
    constexpr uint32_t kCameraState6 = 6;
    constexpr uint32_t kCameraStateCutScene = 7;
    constexpr uint32_t kCameraState8 = 8;
    constexpr uint32_t kCameraStates = 9;

    // The camera state set last.
    uint32_t cameraState();

    // The state left (its cameras, replays and displays put away) and
    // `state` entered. Answers the state (as a byte).
    uint8_t setCameraState(uint32_t state);

    // The level's objects put back as they were saved at its start, if they
    // were saved.
    void restoreLevel();

    // A cut scene, by its name's hash [name inferred: the engine call it
    // makes looks the hash up in a table and runs what it finds]. `now`:
    // played at once on the game's camera (shown for it, then as it was),
    // and the PathMen refreshed. Otherwise it is kept as the pending cut
    // scene and the camera state becomes kCameraStateCutScene, which plays it.
    void playCutScene(uint32_t cutScene, uint32_t now);

    // The frame functions of the cut scene's state, state 6 and state 8
    // (the level's frame runs the state's; not 0: the state ended).
    int32_t cutSceneStateFrame();
    int32_t state6Frame();
    int32_t state8Frame();
}
