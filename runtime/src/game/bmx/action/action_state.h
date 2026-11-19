#pragma once
// BMX XXX's action.c: the state its camera states share (camera_state.cpp,
// the states' frame functions) and the stand-ins for what other lanes own.

#include "bmx/camera/action_camera.h"
#include "bmx/overlay/displays.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/cut_scenes.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/action/action_state.h.h"

namespace game::bmx::action
{
    inline ee::Global<uint32_t, sym::g_cameraState> g_cameraState;
    // Where the camera state is in its own course (each state's frame
    // function counts it its own way), and the frames since that step began.
    inline ee::Global<uint8_t, sym::g_cameraStateStep> g_cameraStateStep;
    inline ee::Global<uint32_t, sym::g_stepFrames> g_stepFrames;
    // The frames the play state has run since it was entered.
    inline ee::Global<uint32_t, sym::g_playFrames> g_playFrames;
    // The frames since a cut scene's end began.
    inline ee::Global<uint32_t, sym::g_cutSceneEndFrames> g_cutSceneEndFrames;
    // action.c's flags (value-named but for the first: what sets and reads
    // the others is in the frame functions).
    inline ee::Global<uint32_t, sym::g_actionFlags> g_actionFlags;
    constexpr uint32_t kActionFlag1 = 0x00000001; // the play state's frame runs only the game's step
    constexpr uint32_t kActionFlag40000000 = 0x40000000; // leaving state 3 clears it; the only one entering play keeps
    // Each player's own overlay part is shown (set entering play; a replay's
    // button turns it).
    constexpr uint32_t kActionOwnOverlayShown = 0x80000000;
    // (setActionFlags: game::bmx's, below)

    // The replay states 4 and 5 play, and the one state 3 plays.
    inline ee::Global<uint8_t, sym::g_stateReplay> g_stateReplay;
    inline ee::Global<uint8_t, sym::g_state3Replay> g_state3Replay;
    // The only player whose pad a replay's state reads; -1: every player's.
    inline ee::Global<int8_t, sym::g_replayPadPlayer> g_replayPadPlayer;
    constexpr int8_t kEveryPlayersPad = -1;

    inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
    inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;
    // The camera on the level (camera.c's), not any player's.
    inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
    // The cut scene the cut scene state plays.
    inline ee::Global<uint32_t, sym::g_pendingCutScene> g_pendingCutScene;

    constexpr ee::Float kFrameSeconds = 1.0f / 60.0f;
    // b7rider5's kFlagShown: the rider is shown.
    constexpr rider::RiderFlag kRiderShown{0, 0x02};

    // A rumble asked of a player's pad (the rider's Rumble, rider/pose.cpp).
    struct Rumble
    {
        uint32_t motor;  // 0 the small one, 1 the large one
        uint32_t timing; // which of the game's timings (a "VIBRATION" command's TYPE)
        uint8_t strength;
    };
    static_assert(offsetof(Rumble, strength) == 8);

    // The overlay's display action.c sends the replay's state (overlay.c's
    // table; b7cam4's name). Its other displays, parts and fades are the
    // overlay's (bmx/overlay/displays.h).
    constexpr uint32_t kOverlayReplayDisplay = 2;
    // What the replay display is told (display 2's message, a byte; the
    // values' names follow where action.c sends them): no replay, one
    // playing, one watched with its overlay part off, one being left.
    constexpr uint8_t kReplayDisplayOff = 0, kReplayDisplayPlaying = 2, kReplayDisplayOverlayOff = 3, kReplayDisplayLeaving = 4;
    // The song display's message: a byte (what sound event 2 answers; 0xFF
    // as a replay ends) and what sound event 10 answers [the song, by the
    // display's name].
    struct SongMessage
    {
        uint8_t state;
        uint8_t _01[3];
        uint32_t song;
    };
    static_assert(sizeof(SongMessage) == 8);

    // Scaffolding (address-call): functions with no Verified body yet (the
    // action camera's cameraShown, cameraShot and setCameraShot are Draft),
    // and engine functions with no readable body in BMX XXX (animationTimeLeft
    // is the engine's and has no name from any build: inferred from its code).
    namespace calls
    {
        inline uint32_t cameraShown(ee::Ptr<ActionCamera> camera) { return ee::game<uint32_t>(sym::cameraShown, camera); }
        inline uint32_t cameraShot(ee::Ptr<ActionCamera> camera) { return ee::game<uint32_t>(sym::cameraShot, camera); }
        inline void setCameraShot(ee::Ptr<ActionCamera> camera, uint32_t shot) { ee::game(sym::setCameraShot, camera, shot); }
        inline void rumble(uint32_t player, ee::Address request) { ee::game(sym::rumble, player, request); }
        // ZGE4's carried objects' frame (zge/scene/carried_objects.cpp; inferred
        // name): the step kept (carriedObjectsStep answers it) and every
        // carried object's updates run.
        inline void updateCarriedObjects(ee::Float seconds) { ee::game(sym::updateCarriedObjects, seconds); }
        // (the original's tail jump: restoreLevel's)
        inline void restoreLevelObjectsTail(ee::Address save) { ee::tailCall<void>(sym::restoreLevelObjects, save); }
        inline ee::Float animationTimeLeft(ee::Ptr<zge::SceneObject> object)
        {
            return ee::game<ee::Float>(sym::animationTimeLeft, object);
        }
        // physics.c's part of camera state 8: its enter (empty), its exit
        // and whether it has ended (always 1).
        inline void enterCameraState8() { ee::game(sym::enterCameraState8); }
        inline void leaveCameraState8() { ee::game(sym::leaveCameraState8); }
        inline int32_t cameraState8Ended() { return ee::game<int32_t>(sym::cameraState8Ended); }
    }
}

namespace game::bmx
{
    // (inferred name) Flags of the low 29 of action.c's set (on) or
    // cleared: the top three cannot be (camera_state.cpp; the strip
    // challenge holds the play with kActionFlag1).
    void setActionFlags(uint32_t flags, uint32_t on);
}
