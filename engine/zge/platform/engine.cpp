// The engine as a whole (zge/platform/platform.h; scanArguments-heapAllocateHook, the
// start of its zge.c block): start-up in order, the frame's step (frame rate,
// the render stages around the game's frame callback), shut-down in reverse.
//
// The game is called where the engine has no name for it: its start-up
// picture and its frame function, by address until the add-ins give the
// engine a hook for them.

#include "platform.h"
#include "tasks.h"

#include "zge/memory/pool.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/input/pads.h"
#include "zge/memory/dmm.h"
#include "zge/memory/heaps.h"
#include "zge/memory/regions.h"
#include "zge/math/random.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/assets/tag_table.h"
#include "zge/assets/txr.h"
#include "zge/collision/collision.h"
#include "zge/render/display.h"
#include "zge/render/material.h"
#include "zge/render/primitives.h"
#include "zge/render/texture.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/message.h"
#include "zge/debug/profiler.h"
#include "zge/debug/debug.h"
#include "zge/game.h"
#include "sce/libgcc.h"

#include <cstdint>
#include "sym/zge/platform/engine.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kScanArguments = sym::scanArguments;
        constexpr uint32_t kSetFrameCallback = sym::setFrameCallback;
        constexpr uint32_t kRequestQuit = sym::requestQuit;
        constexpr uint32_t kRegisterZffdChunkHandler = sym::registerZffdChunkHandler;
        constexpr uint32_t kInitTokenPool = sym::initTokenPool;
        constexpr uint32_t kShutdownSearchTrees = sym::shutdownSearchTrees;
        constexpr uint32_t kInitEffectManager = sym::initEffectManager;
        constexpr uint32_t kShutdownEffectManager = sym::shutdownEffectManager;
        // The game's frame function (zge/game.h), handed on by its address.
        constexpr uint32_t kGameFrame = sym::steadyPlayFrame;

        constexpr uint32_t kEngineStateHigh = ee::high(sym::g_engineSystemsReady); // %hi of the state below, kept in s0
        constexpr uint32_t kTimers = 0x40u;

        constexpr uint32_t kSceneDatabasePathBuffer = sym::g_sceneDatabasePathBuffer;  // 128 bytes
        inline ee::Global<int8_t, kSceneDatabasePathBuffer> g_sceneDatabasePathFirst;
        inline ee::Global<uint32_t, sym::g_engineSystemsReady> g_engineSystemsReady;
        inline ee::Global<uint32_t, sym::g_sceneDatabasePath> g_sceneDatabasePath;
        inline ee::Global<uint32_t, sym::g_frameCallback> g_frameCallback;   // (seconds) -> 0 to quit
        inline ee::Global<ee::Float, sym::g_frameRateSeconds> g_frameRateSeconds; // since the rate was last worked out
        inline ee::Global<ee::Float, sym::g_framesPerSecond> g_framesPerSecond;
        inline ee::Global<ee::Float, sym::g_frameSeconds7> g_frameSeconds;
        inline ee::Global<int32_t, sym::g_frameRateFrames> g_frameRateFrames;
        inline ee::Global<uint32_t, sym::g_frameRateFirstCall> g_frameRateFirstCall;
        inline ee::Global<uint32_t, sym::g_quitRequested> g_quitRequested;
    }

    // scanArguments: the program's arguments, last to first: the first that
    // does not start with '/' is the scene database's path, spaces ending
    // words (its first word that is not empty).
    uint32_t scanArguments(uint32_t argumentCount, uint32_t arguments)
    {
        int32_t index = static_cast<int32_t>(argumentCount) - 1;
        if (index <= 0)
            return 1;
        uint32_t slot = static_cast<uint32_t>(index) << 2;
        for (;;)
        {
            const uint32_t argument = *ee::Ptr<uint32_t>(slot + arguments);
            const int8_t first = *ee::Ptr<int8_t>(argument);
            bool skipped = true;
            if (first != '/' && first != 0)
            {
                skipped = false;
                --index;
                uint32_t read = argument;
                uint32_t out = kSceneDatabasePathBuffer;
                uint8_t c = static_cast<uint8_t>(first);
                for (;;)
                {
                    if (static_cast<int8_t>(c) == ' ')
                        *ee::Ptr<uint8_t>(out) = 0;
                    else
                    {
                        *ee::Ptr<uint8_t>(out) = c;
                        for (;;)
                        {
                            ++read;
                            ++out;
                            const int8_t next = *ee::Ptr<int8_t>(read);
                            if (next == 0 || next == ' ')
                                break;
                            *ee::Ptr<uint8_t>(out) = static_cast<uint8_t>(next);
                        }
                        *ee::Ptr<uint8_t>(out) = 0;
                    }
                    ++read;
                    if (*g_sceneDatabasePathFirst != 0)
                    {
                        *g_sceneDatabasePath = kSceneDatabasePathBuffer;
                        break;
                    }
                    c = *ee::Ptr<uint8_t>(read);
                    out = kSceneDatabasePathBuffer;
                    if (c == 0)
                        break;
                }
            }
            if (skipped)
                --index;
            slot = static_cast<uint32_t>(index) << 2;
            if (index <= 0)
                break;
        }
        return 1;
    }
    READABLE_FUNCTION(scanArguments, Verified);

    int32_t startEngine(); // below

    namespace // the game functions below call: ee/plain.h
    {
        int32_t initTokenPool() { return ee::game<int32_t>(kInitTokenPool); }
        int32_t initEffectManager() { return ee::game<int32_t>(kInitEffectManager); }
        int32_t registerZffdChunkHandler() { return ee::game<int32_t>(kRegisterZffdChunkHandler); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t runFrameCallback(uint32_t callback, ee::Float seconds) { return ee::game<int32_t>(callback, seconds); }
    }

    // engineSystemsInitNoop (ZGE4 only): an empty set-up, 1, initEngine's second to last; its shut-down is
    // engineSystemsShutdownNoop (no build has a body or a name for either).
    inline uint32_t engineSystemsInitNoop()
    {
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, engineSystemsInitNoop);

    // initEngine: everything up, in order, stopping at the first that fails.
    uint32_t initEngine(uint32_t argumentCount, uint32_t arguments)
    {
        // What the start-up picture is: legalScreen fills them in.
        ee::Local<uint32_t> picturePath;
        ee::Local<uint32_t> pictureWidth;
        ee::Local<uint32_t> pictureHeight;
        ee::Local<uint32_t> pictureFormat;
        *g_engineSystemsReady = 1;
        if (static_cast<int32_t>(scanArguments(argumentCount, arguments)) == 0)
            return 0u;
        if (initMemoryRegions() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no token pool and no allocators to choose)
        {
            if (initTokenPool() == 0)
                return 0u;
            if (setDefaultAllocators() == 0)
                return 0u;
        }
        if (initDmmRegions() == 0)
            return 0u;
        if (initMath() == 0)
            return 0u;
        if (initFileSystem() == 0)
            return 0u;
        if (initStringTables() == 0)
            return 0u;
        if (initChunkParser() == 0)
            return 0u;
        if (initAssetLibraries() == 0)
            return 0u;
        if (startEngine() == 0)
            return 0u;

        // The start-up picture, up while the rest loads (ZGE4 shows none; ZGE2 names its file itself).
        if constexpr (game::readable::kIsDm2)
        {
            showScreenSizedRawImage(sym::dm2::g_textLegalPicturePath);
        }
        else if constexpr (!game::readable::kIsBmx)
        {
            legalScreen(picturePath, pictureWidth, pictureHeight, pictureFormat);
            if (*picturePath != 0)
                showRawImage(*picturePath, *pictureWidth, *pictureHeight, *pictureFormat);
        }
        if (initPads() == 0)
            return 0u;
        if (initDebug() == 0)
            return 0u;
        if (initSceneObjects() == 0)
            return 0u;
        if (initTokenReader() == 0)
            return 0u;
        if (initSceneDatabase(*g_sceneDatabasePath) == 0)
            return 0u;
        if (initEngineTimers(kTimers) == 0)
            return 0u;
        if (initProfiler() == 0)
            return 0u;
        if (initMessages() == 0)
            return 0u;
        if (initFonts() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no effect manager and no ZFFD chunks)
        {
            if (initEffectManager() == 0)
                return 0u;
            if (registerZffdChunkHandler() == 0)
                return 0u;
        }
        if constexpr (game::readable::kIsBmx) // ZGE4's two last set-ups: the carried animals and an empty one
        {
            if (initCarriedAnimals() == 0)
                return 0u;
            if (engineSystemsInitNoop() == 0)
                return 0u;
        }
        setFrameCallback(kGameFrame);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initEngine, Verified);

    // frameSeconds.
    ee::Float frameSeconds()
    {
        return *g_frameSeconds;
    }
    READABLE_FUNCTION(frameSeconds, Verified);

    // frameMilliseconds (no caller in this build).
    int32_t frameMilliseconds()
    {
        return static_cast<int32_t>(sce::libgcc::fptoui(*g_frameSeconds * ee::Float(1e+03f)));
    }
    READABLE_FUNCTION(frameMilliseconds, Draft);

    // updateFrameRate: a frame's step, once a frame from engineMain: the time
    // kept, the frames per second worked out once a second, the stages and the
    // game's frame between them. 0 when the game asks to quit. ZGE2 keeps no
    // frame's seconds (frameSeconds is ZGE3's) and calls the game's frame
    // with no test that there is one.
    uint32_t updateFrameRate(ee::Float seconds)
    {
        const uint32_t firstCall = *g_frameRateFirstCall;
        if constexpr (!game::readable::kIsDm2)
            *g_frameSeconds = seconds;
        if (firstCall != 0)
        {
            *g_frameRateFirstCall = 0;
            if constexpr (game::readable::kIsBmx) // ZGE4's first frame brings every world mesh's moved node up to date
                forEachWorldMesh(sym::bmx::engineMovedNodeVisitor, 0);
            *g_frameRateSeconds = ee::Float(0.0f);
            *g_framesPerSecond = ee::Float(0.0f);
        }
        constexpr ee::Float kOne = ee::Float(1.0f);
        const ee::Float counted = *g_frameRateSeconds + seconds;
        const int32_t frames = *g_frameRateFrames + 1;
        *g_frameRateFrames = frames;
        *g_frameRateSeconds = counted;
        if (kOne <= counted)
        {
            ee::Float rate;
            if (frames >= 0)
                rate = ee::toFloat(frames);
            else
            {
                const ee::Float half = ee::toFloat(static_cast<int32_t>((frames & 1) | (static_cast<uint32_t>(frames) >> 1)));
                rate = half + half;
            }
            const ee::Float left = *g_frameRateSeconds;
            *g_framesPerSecond = rate;
            *g_frameRateFrames = 0;
            *g_frameRateSeconds = left - kOne;
        }
        runRenderStage(kStageBeforeFrame);
        const uint32_t frameCallback = *g_frameCallback;
        if (game::readable::kIsDm2 || frameCallback != 0)
        {
            if (runFrameCallback(frameCallback, seconds) == 0)
            {
                requestQuit();
                return 0;
            }
        }
        runRenderStage(kStageAfterFrame);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateFrameRate, Verified); // handed on: none: the game's frame takes the seconds alone

    // restartFrameRate (ZGE4 only; enterScene calls it): the next frame counts as the first again, so
    // updateFrameRate starts its count afresh (ZGE4's first frame also brings every world mesh's node up to date).
    inline void restartFrameRate()
    {
        *g_frameRateFirstCall = 1;
    }
    READABLE_FUNCTION_ONLY(bmx, restartFrameRate);

    // engineMovedNodeVisitor (ZGE4 only): the engine's own copy of updateMovedNodeVisitor, which ZGE4's
    // updateFrameRate hands forEachWorldMesh on the first frame: the node's matrices and bounds brought up to
    // date (updateMovedNode). Always 1 (go on).
    inline uint32_t engineMovedNodeVisitor(ee::Ptr<Node> node)
    {
        updateMovedNode(node);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, engineMovedNodeVisitor);

    // setFrameCallback (initEngine: the game's frame function).
    void setFrameCallback(uint32_t callback)
    {
        *g_frameCallback = callback;
    }
    READABLE_FUNCTION(setFrameCallback, Verified);

    // requestQuit: engineMain's loop ends after this frame.
    void requestQuit()
    {
        *g_quitRequested = 1;
    }
    READABLE_FUNCTION(requestQuit, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t shutdownEffectManager() { return ee::game<int32_t>(kShutdownEffectManager); }
        int32_t shutdownSearchTrees() { return ee::game<int32_t>(kShutdownSearchTrees); }
    }

    // engineSystemsShutdownNoop (ZGE4 only): an empty shut-down, 1, that shutdownEngineSystems calls first (no build
    // has a body or a name for it; its set-up is engineSystemsInitNoop, initEngine's second to last).
    inline uint32_t engineSystemsShutdownNoop()
    {
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, engineSystemsShutdownNoop);

    // shutdownEngineSystems: everything down, in about the reverse of initEngine,
    // stopping at the first that fails. ZGE4 first calls an empty shut-down and frees the carried animals.
    uint32_t shutdownEngineSystems()
    {
        if (*g_engineSystemsReady == 0)
            return 1;
        if constexpr (game::readable::kIsBmx)
        {
            if (engineSystemsShutdownNoop() == 0)
                return 0u;
            if (freeCarriedAnimals() == 0)
                return 0u;
        }
        if (shutDownProfiler() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 shuts down no effect manager here)
        {
            if (shutdownEffectManager() == 0)
                return 0u;
        }
        if (shutdownEngineTimers() == 0)
            return 0u;
        if (shutDownSceneDatabase() == 0)
            return 0u;
        if (shutDownTokenReader() == 0)
            return 0u;
        if (shutDownSceneObjects() == 0)
            return 0u;
        if (shutdownFonts() == 0)
            return 0u;
        if (shutdownMessages() == 0)
            return 0u;
        if (shutdownDebugSystem() == 0)
            return 0u;
        if (shutDownPads() == 0)
            return 0u;
        if (shutdownEngine() == 0)
            return 0u;
        if (shutDownAssetLibraries() == 0)
            return 0u;
        if (shutDownChunkParser() == 0)
            return 0u;
        if (shutdownStringTables() == 0)
            return 0u;
        if (shutDownFileSystem() == 0)
            return 0u;
        if (shutdownMath() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 shuts down no heaps here)
        {
            if (shutDownHeaps() == 0)
                return 0u;
        }
        if (shutdownDmmRegions() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 shuts down no search trees here)
        {
            if (shutdownSearchTrees() == 0)
                return 0u;
        }
        if (shutdownMemoryRegions() == 0)
            return 0u;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 shuts down no chunk handlers here)
        {
            if (shutdownChunkHandlers() == 0)
                return 0u;
        }
        *g_engineSystemsReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownEngineSystems, Verified);

    // reportEngineObjects: what the systems have loaded, each system's report
    // (hooks with nothing in them in this build).
    uint32_t reportEngineObjects()
    {
        reportSceneObjects();
        fileSystemReportHook();
        runEngineHooksAlias();
        terrainNoop();
        refreshAllPictures();
        modelPoseHook();
        return game::tail(touchLoadedTextures); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(reportEngineObjects, Verified);

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_taskNodePool> g_taskNodePool;
        // The engine's state (tasks.h): this flag, the render stages, the task
        // pool and the deferred pools, cleared together.
        inline ee::Global<int32_t, sym::g_engineStarted> g_engineStarted;
        constexpr uint32_t kEngineStateBytes = game::readable::perGame(0x94u, 0x68u, 0x94u); // (ZGE2: seven stages of 12 bytes, no switches)
        // The frame's state (quit request, screen size, frame rate, colour
        // scale; display.cpp), from g_cameraDefaults.
        // ZGE2's is 0x18 bytes, and its frame counts are a record of their own after it.
        constexpr uint32_t kFrameStateBytes = game::readable::perGame(0x50u, 0x18u, 0x50u);
        constexpr uint32_t kFrameCountsBytes = 0x34;
        // (ZGE2's startEngine makes the task pool, g_taskNodePool, itself.)
        constexpr uint32_t kTaskPoolBlockRecords = 0x20;
    }

    // Empty; runEngineHooks calls it eight times.
    void engineNoop()
    {
    }
    READABLE_FUNCTION(engineNoop, Verified);

    // EngineNoop eight times (ZGE2: seven, its stage count); runEngineHooksAlias.
    int32_t runEngineHooks()
    {
        int32_t i = 0;
        int32_t result = 0;
        for (;;)
        {
            engineNoop();
            i = i + 1;
            result = static_cast<int32_t>(i < static_cast<int32_t>(game::readable::perGame(8u, 7u)));
            if (result != 0)
                continue;
            return result;
        }
        return result;
    }
    READABLE_FUNCTION(runEngineHooks, Verified);

    uint32_t engineStopHook();

    namespace
    {
        constexpr uint32_t kFrameStateHigh = ee::high(sym::g_queryState);
        inline ee::Global<ee::Float, sym::g_targetFrameRate> g_targetFrameRate; // what the display's frame paces to (display.cpp)
    }

    // setMotionRate: the rate frames are paced to (0: off; the game's steady
    // frame sets one over a held frame time). It takes the float alone (the
    // original leaves its integer argument unread) and returns the %hi left
    // in its result register. Was games/aggressive's (game_frame.cpp); the
    // blocks say it is the engine's (layer2), BMX XXX (PS2) has it word for
    // word.
    int32_t setMotionRate(ee::Float targetFrameRate)
    {
        *g_targetFrameRate = targetFrameRate;
        return kFrameStateHigh;
    }
    READABLE_FUNCTION(setMotionRate, Verified);

    // startEngine (initEngine): the engine's and the frame's state cleared
    // and the engine marked started; then, once the game's start hook allows
    // it, the graphics up, the cameras at the platform's screen size, the
    // start clear colour, frames at each vsync with no fixed step, colours
    // unscaled, the render stages and deferred pools made, and the systems
    // that can fail started in turn. 1, or 0 at the first that fails.
    int32_t startEngine()
    {
        memoryFill(sym::g_engineStarted, 0, kEngineStateBytes);
        *g_engineStarted = 1;
        memoryFill(sym::g_cameraDefaults, 0, kFrameStateBytes);
        if constexpr (game::readable::kIsDm2)
            memoryFill(sym::dm2::g_frameCounts, 0, kFrameCountsBytes);
        if constexpr (!game::readable::kIsBmx) // (ZGE4 has no start hook)
            if (engineStartHook() == 0)
                return 0;
        initGraphicsSystems();
        resetCameraViewports(g_platformSettings->width, g_platformSettings->height);
        setClearColour(ee::Ptr<uint8_t>(sym::g_startClearColour));
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 has no frame pacing or colour scale to set, and its stages need no set-up: the task pool alone.
            *g_taskNodePool = ee::Ptr<Pool>(createPool(sizeof(Task), kTaskPoolBlockRecords, 0));
        }
        else
        {
            setMotionRate(ee::Float(0.0f));
            setGlobalTimeScale(ee::Float(0.0f));
            setGlobalColourScale(ee::Float(1.0f), ee::Float(1.0f), ee::Float(1.0f));
            initRenderStages();
        }
        initDeferredPools();
        if (registerTextureHeaderChunkHandlers() == 0)
            return 0;
        if (initTextures() == 0)
            return 0;
        if (registerMoboChunkHandler() == 0)
            return 0;
        if (registerLightChunkHandlers() == 0)
            return 0;
        if (initQueries() == 0)
            return 0;
        if (initPrimitiveRenderer() == 0)
            return 0;
        if (initTerrain() == 0)
            return 0;
        if constexpr (game::readable::kIsDm2)
            if (initWorldCellTree() == 0)
                return 0;
        return initTagTables() != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(startEngine, Verified);

    // shutdownEngine: startEngine's parts down in reverse, stopping at the first that fails. ZGE2 takes the graphics
    // systems and the deferred pools down first (ZGE3 and ZGE4 last) and its world cell tree after the tag tables.
    uint32_t shutdownEngine()
    {
        *g_engineStarted = 0;
        if constexpr (game::readable::kIsDm2)
        {
            zge::shutDownGraphicsSystems();
            destroyDeferredPools();
        }
        const int32_t shutdownTagTables = zge::shutdownTagTables();
        int32_t result = 0;
        if (shutdownTagTables != 0)
        {
            if constexpr (game::readable::kIsDm2)
            {
                if (shutdownWorldCellTree() == 0)
                    return 0;
            }
            const int32_t shutdownTerrain = zge::shutdownTerrain();
            result = 0;
            if (shutdownTerrain != 0)
            {
                const int32_t shutdownPrimitiveRenderer = zge::shutdownPrimitiveRenderer();
                result = 0;
                if (shutdownPrimitiveRenderer != 0)
                {
                    const int32_t shutdownCollisionQueries = zge::shutdownCollisionQueries();
                    result = 0;
                    if (shutdownCollisionQueries != 0)
                    {
                        const int32_t shutdownLitoChunks = zge::shutdownLitoChunks();
                        result = 0;
                        if (shutdownLitoChunks != 0)
                        {
                            const int32_t shutdownModelPoses = zge::shutdownModelPoses();
                            result = 0;
                            if (shutdownModelPoses != 0)
                            {
                                const int32_t shutdownMaterialPool = zge::shutdownMaterialPool();
                                result = 0;
                                if (shutdownMaterialPool != 0)
                                {
                                    const int32_t shutdownTextures = zge::shutdownTextures();
                                    result = 0;
                                    if (shutdownTextures != 0)
                                    {
                                        if constexpr (!game::readable::kIsDm2)
                                            destroyDeferredPools();
                                        if constexpr (game::readable::kIsBmx) // (ZGE4 has no stop hook)
                                        {
                                            zge::shutDownGraphicsSystems();
                                            destroyPool(*g_taskNodePool);
                                            return 1;
                                        }
                                        const int32_t engineStopHook = static_cast<int32_t>(zge::engineStopHook());
                                        result = 0;
                                        if (engineStopHook != 0)
                                        {
                                            if constexpr (!game::readable::kIsDm2)
                                                zge::shutDownGraphicsSystems();
                                            destroyPool(*g_taskNodePool);
                                            return 1;
                                        }
                                        return result;
                                    }
                                    else
                                        return result;
                                }
                                else
                                    return result;
                            }
                            else
                                return result;
                        }
                        else
                            return result;
                    }
                    else
                        return result;
                }
                else
                    return result;
            }
            else
                return result;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(shutdownEngine, Verified);

    // RunEngineHooks; reportEngineObjects.
    int32_t runEngineHooksAlias()
    {
        return game::tail(runEngineHooks);
    }
    READABLE_FUNCTION(runEngineHooksAlias, Verified);

    // passThroughHook (no caller in this build): its float argument back.
    ee::Float passThroughHook(ee::Float value)
    {
        return value;
    }
    READABLE_FUNCTION(passThroughHook, Draft);

    // The game's say before the engine starts (startEngine): 1 to go on.
    uint32_t engineStartHook()
    {
        return 1;
    }
    READABLE_FUNCTION(engineStartHook, Verified);

    uint32_t engineStopHook()
    {
        return 1;
    }
    READABLE_FUNCTION(engineStopHook, Verified);
}
