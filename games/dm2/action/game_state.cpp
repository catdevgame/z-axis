// The game's state machine (action.h): DM2's own code (action.c). The play
// frame runs the state's frame function each frame; a state set here is
// entered on its next run (the old state's exit, then the new one's enter).

#include "action.h"
#include "rider_files.h"
#include "ui_frame.h"
#include "../input/pads.h"
#include "../sound/sound_mode.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/collision/collision.h"
#include "zge/debug/debug.h"
#include "zge/debug/profiler.h"
#include "zge/math/conversions.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/action/game_state.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The state asked for, the state the game is in, whether a change
        // is pending (1: the play frame makes it), and the frames spent in
        // the state.
        inline ee::Global<uint8_t, sym::g_nextGameState> g_nextGameState;
        inline ee::Global<uint8_t, sym::g_gameStateChange> g_gameStateChange;
        inline ee::Global<uint32_t, sym::g_gameStateFrames> g_gameStateFrames;
    }

    uint8_t setGameState(uint32_t state)
    {
        *g_nextGameState = static_cast<uint8_t>(state);
        *g_gameStateChange = 1;
        *g_gameStateFrames = 0;
        return static_cast<uint8_t>(state);
    }
    READABLE_FUNCTION(setGameState, Verified);

    uint8_t gameState()
    {
        return *g_nextGameState;
    }
    READABLE_FUNCTION(gameState, Verified);

    namespace
    {
        // The game state the play frame is in (kNoGameState: none), and each
        // state's enter, exit and frame functions (the front end, the level,
        // the park editor, the intro).
        inline ee::Global<uint8_t, sym::g_currentGameState> g_currentGameState;
        constexpr uint8_t kNoGameState = 0xFF;
        constexpr uint32_t kGameStates = 4;
        using GameStateStep = ee::FunctionPtr<void()>;
        inline ee::Global<GameStateStep[kGameStates], sym::g_gameStateExits> g_gameStateExits;
        inline ee::Global<GameStateStep[kGameStates], sym::g_gameStateEnters> g_gameStateEnters;
        inline ee::Global<GameStateStep[kGameStates], sym::g_gameStateFrameSteps> g_gameStateFrameSteps;
        constexpr uint8_t kStateChangePending = 1;

        // The debug stats line (shown while the flag is set; the profiler's
        // display and the debug menu follow it): the frame rate, measured
        // every 60 frames by the collision clock (milliseconds), and the
        // texture cache's free bytes in KB.
        inline ee::Global<uint8_t, sym::g_debugStatsShown> g_debugStatsShown;
        inline ee::Global<int32_t, sym::g_statsFrames> g_statsFrames;
        inline ee::Global<ee::Float, sym::g_statsFramesPerSecond> g_statsFramesPerSecond;
        inline ee::Global<ee::Float, sym::g_statsClock> g_statsClock;
        constexpr int32_t kStatsEvery = 60;
        constexpr float kStatsFramesPerMeasure = 60.0f;
        constexpr float kClockSeconds = 0.001f;
        constexpr uint32_t kStatsX = 0, kStatsY = 250;
        constexpr ee::Ptr<const char> kStatsFormat{sym::statsLineFormat}; // "FPS: %2.2f VramFree:%d"

        // ZGE2's frame counts (zge/render/renderer.h's FrameCounts in the
        // engine lane's stack; only the field read here).
        struct FrameCounts
        {
            uint32_t totals[3];
            uint32_t view[2][3];
            uint32_t frame[2];
            int32_t vramFreeBytes; // +0x2C
            uint32_t unused;
        };
        static_assert(sizeof(FrameCounts) == 0x34 && offsetof(FrameCounts, vramFreeBytes) == 0x2C);
        // Scaffolding (address-call): the engine's summedFrameCounts (ZGE2
        // only; written in the engine lane's stack).
        namespace calls
        {
            ee::Ptr<FrameCounts> summedFrameCounts() { return ee::game<ee::Ptr<FrameCounts>>(sym::summedFrameCounts); }
        }
    }

    // playFrame: the game's frame (the engine's frame callback's): the debug
    // stats line when it is on, the profiler's display and the debug menu
    // set by it, then the game state's frame, or a pending state change
    // (the old state's exit and the new one's enter, on the change's first
    // frame), then the music.
    void playFrame()
    {
        if (*g_debugStatsShown != 0)
        {
            if (*g_statsFrames == kStatsEvery)
            {
                const ee::Float now = zge::unsignedToFloat(zge::collisionClock());
                *g_statsFrames = 0;
                *g_statsFramesPerSecond = ee::Float(kStatsFramesPerMeasure) / ((now - *g_statsClock) * ee::Float(kClockSeconds));
                *g_statsClock = zge::unsignedToFloat(zge::collisionClock());
            }
            const uint64_t framesPerSecond = sce::libgcc::fptodp(*g_statsFramesPerSecond);
            const int32_t vramFree = calls::summedFrameCounts()->vramFreeBytes;
            zge::debugPrint(kStatsX, kStatsY, kStatsFormat, framesPerSecond, vramFree / 1024);
            ++*g_statsFrames;
        }
        zge::setProfileDisplayMode(*g_debugStatsShown);
        zge::setDebugMenuSelection(*g_debugStatsShown);
        if (*g_gameStateChange == 0)
        {
            ++*g_gameStateFrames;
            (*g_gameStateFrameSteps)[*g_currentGameState]();
        }
        else if (*g_gameStateChange == kStateChangePending && ++*g_gameStateFrames == 1)
        {
            if (*g_currentGameState < kGameStates)
                (*g_gameStateExits)[*g_currentGameState]();
            const uint8_t next = *g_nextGameState;
            *g_gameStateChange = 0;
            *g_currentGameState = next;
            (*g_gameStateEnters)[next]();
            *g_gameStateFrames = 0;
        }
        game::tail(musicFrame);
    }
    READABLE_FUNCTION_WAITS(playFrame, Verified);

    // shutdownGame: the game's end: the state it is in left (its exit), no
    // state, the common UI unloaded, the sound shut down, the pads freed and
    // the rider and bike data files given back.
    void shutdownGame()
    {
        if (*g_currentGameState < kGameStates)
            (*g_gameStateExits)[*g_currentGameState]();
        *g_currentGameState = kNoGameState;
        unloadCommonUi();
        shutdownSound();
        freePads();
        if (*g_riderDataFile != 0)
        {
            zge::heapFree(*g_riderDataFile);
            *g_riderDataFile = 0;
        }
        if (*g_bikeDataFile != 0)
        {
            zge::heapFree(*g_bikeDataFile);
            *g_bikeDataFile = 0;
        }
    }
    READABLE_FUNCTION_WAITS(shutdownGame, Verified);
}
