// The game's side of the engine's main (zge/game.h declares them, each game
// defines its own): AI's start, legal screen, steady frame and end
// (gamehand13; were the sweep's drafts of gameStartup .. gameShutdown, then
// game_frame.cpp's). They are the engine's names, so they sit in its
// namespace; the bodies are AI's.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/game.h"
#include "zge/collision/dynamics.h"
#include "zge/debug/profiler.h"
#include "zge/platform/platform.h"
#include "aggressive/game_start.h"
#include "aggressive/game_frame.h"

#include <cstdint>
#include "sym/aggressive/game_main.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<ee::Float, sym::g_longestFrame> g_longestFrame;   // a frame counts at most this long
        inline ee::Global<ee::Float, sym::g_heldFrameReset> g_heldFrameReset; // the held frame time once the hold is over
        inline ee::Global<ee::Float, sym::g_frameHold> g_frameHold;      // seconds left holding the frame time
        inline ee::Global<ee::Float, sym::g_heldFrame> g_heldFrame;      // the frame time held
    }

    namespace
    {
        // The legal screen the engine shows at the boot: its image and size.
        constexpr uint32_t kLegalScreenImage = sym::legalScreenImage;             // "ASSETS\LEGAL1.RAW"
        constexpr uint32_t kLegalScreenWidth = 512, kLegalScreenHeight = 448, kLegalScreenDepth = 24;
    }

    // gameStartup: the game's start (before the main loop): the profiler's
    // display off, the debug menu's functions and the dynamics file's chunk
    // handlers registered. Always 1.
    uint32_t gameStartup()
    {
        zge::setProfileDisplayMode(0);
        aggressive::initGame();
        zge::registerDynamicsChunkHandlers();
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameStartup, Verified);

    // legalScreen (image, width, height, bits): the boot's legal screen.
    void legalScreen(ee::Ptr<uint32_t> image, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height, ee::Ptr<uint32_t> depth)
    {
        *image = kLegalScreenImage;
        *width = kLegalScreenWidth;
        *height = kLegalScreenHeight;
        *depth = kLegalScreenDepth;
    }
    READABLE_FUNCTION(legalScreen, Verified);

    // gameShutdown: the game's end (after the main loop): its systems,
    // then the engine's, shut down. Always 1.
    uint32_t gameShutdown()
    {
        aggressive::shutdownGame();
        zge::reportEngineObjects();
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameShutdown, Verified);

    // steadyPlayFrame (seconds): a frame of play at a steady time: the frame
    // time at most g_longestFrame; a longer frame than the one held is held
    // for half a second, during which play runs at the held time (and the
    // engine's rate setMotionRate is one over it); when the hold runs out the
    // rate goes back to 0 and the held time to g_heldFrameReset. Then
    // playFrame. Returns 1.
    int32_t steadyPlayFrame(ee::Float seconds)
    {
        ee::Float dt = seconds;
        if (*g_longestFrame < dt)
            dt = *g_longestFrame;
        ee::Float held = *g_heldFrame;
        if (held < dt)
        {
            held = dt;
            *g_heldFrame = dt;
            *g_frameHold = 0.5f;
        }
        const ee::Float hold = *g_frameHold;
        if (ee::Float(0.0f) < hold)
        {
            const ee::Float left = hold - dt;
            *g_frameHold = left;
            if (left < ee::Float(0.0f))
            {
                *g_frameHold = 0.0f;
                *g_heldFrame = *g_heldFrameReset;
                setMotionRate(ee::Float(0.0f));
            }
            else
            {
                dt = held;
                setMotionRate(ee::Float(1.0f) / dt);
            }
        }
        aggressive::playFrame(dt);
        return 1;
    }
    READABLE_FUNCTION_WAITS(steadyPlayFrame, Verified);
}
