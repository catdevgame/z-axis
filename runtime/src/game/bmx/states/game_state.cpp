// BMX XXX's game states (game_state.h): BMX XXX's own code (bmx.c).

#include "game_state.h"

#include "bmx/sound/events.h"
#include "bmx/ui/menu.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/collision/collision.h"
#include "zge/debug/debug.h"
#include "zge/debug/profiler.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/render/primitives.h"
#include "zge/scene/animation.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/states/game_state.cpp.h"

namespace zge
{
    ChunkProgressHook chunkProgressHook(); // assets/chunk_reader.cpp
}

namespace game::bmx
{
    namespace
    {
        // The state asked for and the state the game is in.
        inline ee::Global<uint8_t, sym::g_nextGameState> g_nextGameState;
        inline ee::Global<uint8_t, sym::g_currentGameState> g_currentGameState;
        // Where the machine is: the state's frames run, a change is under way
        // (four frames), or the new state is loading.
        inline ee::Global<uint8_t, sym::g_gameStatePhase> g_gameStatePhase;
        constexpr uint8_t kStateRunning = 0, kStateChanging = 1, kStateLoading = 2;
        // The frames spent in the phase.
        inline ee::Global<uint32_t, sym::g_gameStateFrames> g_gameStateFrames;
        // A change's frames: the old state's exit, a frame with nothing, the
        // pools emptied, the new state's enter.
        constexpr uint32_t kChangeExit = 1, kChangeIdle = 2, kChangeFreePools = 3, kChangeEnter = 4;

        // The states' functions, by state.
        inline ee::Global<ee::FunctionPtr<void()>[kGameStates], sym::g_gameStateEnter> g_gameStateEnter;
        inline ee::Global<ee::FunctionPtr<int32_t()>[kGameStates], sym::g_gameStateLoad> g_gameStateLoad;
        inline ee::Global<ee::FunctionPtr<void()>[kGameStates], sym::g_gameStateExit> g_gameStateExit;
        inline ee::Global<ee::FunctionPtr<void()>[kGameStates], sym::g_gameStateFrame> g_gameStateFrame;
        // What a state's loading step answers.
        constexpr int32_t kLoadDone = 0, kLoadMore = 1, kLoadQuit = 2;

        // A word the end of a state's loading clears; nothing in the game
        // sets it.
        inline ee::Global<uint32_t, sym::g_clearedWhenLoaded> g_clearedWhenLoaded;

        // The frame rate, measured every 60 frames from the engine's clock
        // (milliseconds): the frames counted, the clock at the last measure,
        // the rate.
        inline ee::Global<uint32_t, sym::g_rateFrames> g_rateFrames;
        inline ee::Global<ee::Float, sym::g_rateClock> g_rateClock;
        inline ee::Global<ee::Float, sym::g_frameRate> g_frameRate;
        inline ee::Global<ee::Float, sym::g_rateSpan> g_rateSpan; // 60000: 60 frames' milliseconds a second
        constexpr uint32_t kRateFrames = 60;

        // The debug display: 2 shows the profiler (and its backdrop) and
        // opens the debug menu's selection.
        inline ee::Global<uint8_t, sym::g_debugDisplay> g_debugDisplay;
        constexpr uint8_t kDebugDisplayProfiler = 2;

        // The frames the game has run.
        inline ee::Global<uint32_t, sym::g_gameFrames> g_gameFrames;

        // The C library's memset, by address (ee/plain.h).
        // Scaffolding (address-call): until the C library has a declaration to call.
        void memset(ee::Address dst, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, dst, byte, bytes); }

        // The profiler's backdrop: one quad, its corners after it.
        struct BackdropQuad
        {
            uint32_t texture;                             // +0x00 none
            ee::Ptr<zge::PrimitiveVertex> vertices[4];    // +0x04
            uint8_t _14[0x20 - 0x14];
            zge::PrimitiveVertex corners[4];              // +0x20
        };
        static_assert(offsetof(BackdropQuad, corners) == 0x20 && sizeof(BackdropQuad) == 0xA0);
        constexpr uint32_t kScreenQuads = 0x02000002;
        constexpr int32_t kBackdropLeft = 20, kBackdropRight = 452, kBackdropTop = 348, kBackdropBottom = 438;
    }

    uint8_t setGameState(uint32_t state)
    {
        *g_nextGameState = static_cast<uint8_t>(state);
        *g_gameStatePhase = kStateChanging;
        *g_gameStateFrames = 0;
        return static_cast<uint8_t>(state);
    }
    READABLE_FUNCTION(setGameState, Verified);

    uint8_t gameState()
    {
        return *g_nextGameState;
    }
    READABLE_FUNCTION(gameState, Verified);

    void playFrame()
    {
        ee::Local<Menu> noMenu;
        ee::Local<uint8_t[4]> black;
        memset(black, 0, 4);
        black[3] = 0xFF;

        const ee::Float now = ee::toFloat(static_cast<int32_t>(zge::collisionClock()));
        if (++*g_rateFrames == kRateFrames)
        {
            const ee::Float span = now - *g_rateClock;
            *g_rateClock = now;
            *g_rateFrames = 0;
            *g_frameRate = *g_rateSpan / span;
        }

        zge::setProfileDisplayMode(*g_debugDisplay);
        zge::setDebugMenuSelection(*g_debugDisplay == kDebugDisplayProfiler);

        switch (*g_gameStatePhase)
        {
        case kStateChanging:
        {
            const uint32_t frames = ++*g_gameStateFrames;
            if (frames == kChangeExit)
            {
                const uint8_t state = *g_currentGameState;
                if (state < kGameStates)
                    (*g_gameStateExit)[state]();
                zge::setClearColour(ee::Ptr<uint8_t>(black.address()));
            }
            else if (frames == kChangeFreePools)
            {
                zge::freeAnimationPools();
                zge::releaseAllEmptyPoolBlocks();
            }
            else if (frames == kChangeEnter)
            {
                *g_currentGameState = *g_nextGameState;
                // No menu has the screen while the state loads.
                noMenu->handler = 0;
                noMenu->id = kNoMenu;
                noMenu->data = 0;
                setMenu(noMenu);
                (*g_gameStateEnter)[*g_nextGameState]();
                *g_gameStatePhase = kStateLoading;
                *g_gameStateFrames = 0;
            }
            break;
        }
        case kStateLoading:
        {
            const ee::FunctionPtr<int32_t()> load = (*g_gameStateLoad)[*g_currentGameState];
            if (load)
            {
                const int32_t more = load();
                if (more == kLoadMore)
                    break;
                if (more != kLoadDone)
                {
                    if (more == kLoadQuit)
                    {
                        *g_currentGameState = kNoGameState;
                        zge::requestQuit();
                    }
                    break;
                }
            }
            *g_gameStatePhase = kStateRunning;
            *g_gameStateFrames = 0;
            *g_clearedWhenLoaded = 0;
            break;
        }
        case kStateRunning:
        {
            const ee::FunctionPtr<void()> frame = (*g_gameStateFrame)[*g_currentGameState];
            ++*g_gameStateFrames;
            frame();
            break;
        }
        default:
            break;
        }

        updateGameFrame();
        ++*g_gameFrames;
    }
    READABLE_FUNCTION_WAITS(playFrame, Verified);

    int32_t drawProfilerBackdrop()
    {
        if (*g_debugDisplay != kDebugDisplayProfiler)
            return 1;
        ee::Local<BackdropQuad> quad;
        ee::Local<uint8_t[4]> colour;
        colour[0] = 0;
        colour[3] = 0x80;
        colour[1] = 0;
        colour[2] = 0;
        quad->texture = 0;
        // Corners in turn (left top, right top, right bottom, left bottom);
        // the quad lists them backwards. Only a corner's place and colour
        // are set: the rest of it is what the stack held.
        for (uint32_t i = 0; i < 4; ++i)
        {
            zge::PrimitiveVertex &corner = quad->corners[i];
            corner.x = ee::toFloat(((i + 1) & 2) ? kBackdropRight : kBackdropLeft);
            corner.y = ee::toFloat((i & 2) ? kBackdropBottom : kBackdropTop);
            corner.z = ee::Float(1.0f);
            for (uint32_t c = 0; c < 4; ++c)
                corner.rgba[c] = colour[c];
            quad->vertices[i] = &quad->corners[3 - i];
        }
        zge::drawPrimitiveQuads(kScreenQuads, ee::Ptr<uint32_t>(&quad->texture), 1, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawProfilerBackdrop, Verified);

    namespace
    {
        // A state's loading step (the front end's and the level's), and what
        // its loading bar counts: the bytes the step has read, the file
        // being read and the bytes read of it (the step's frames: the
        // header's).
        inline ee::Global<uint32_t, sym::g_loadStep> g_loadStep;
        inline ee::Global<uint32_t, sym::g_loadStepBytes> g_loadStepBytes;
        inline ee::Global<char, sym::g_loadingFile> g_loadingFile;
        inline ee::Global<uint32_t, sym::g_loadingFileBytes> g_loadingFileBytes;
        constexpr ee::Ptr<const char> kNoFile{sym::g_textNoLoadingFile}; // ""
        // The chunk reader's progress hook as BMX XXX's engine calls it: the
        // file's name, the bytes read and in all, and the hook's context.
        using LoadingBarHook = ee::FunctionPtr<void(const char *name, uint32_t bytesDone, uint32_t totalBytes, uint32_t context)>;
    }

    // setLoadStep: a loading step begun, its counts cleared, and the
    // loading bar's hook (when one is set) told of nothing read yet.
    void setLoadStep(uint32_t step)
    {
        *g_loadStep = step;
        *g_loadStepFrames = 0;
        *g_loadStepBytes = 0;
        *g_loadingFile = 0;
        *g_loadingFileBytes = 0;
        const LoadingBarHook hook(zge::chunkProgressHook());
        if (hook)
            hook(kNoFile.get(), 0, 0, 0);
    }
    READABLE_FUNCTION(setLoadStep, Verified);

    uint32_t loadStep()
    {
        return *g_loadStep;
    }
    READABLE_FUNCTION(loadStep, Verified);
}
