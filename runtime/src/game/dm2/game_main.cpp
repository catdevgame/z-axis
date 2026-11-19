// DM2's side of what the engine calls in the game (zge/game.h): its start,
// frame and end (action.c; its packed libraries' reader is
// zfile_library.cpp). DM2's own code, in its instance only, so these define
// the instance's engine names (zge is zge_dm2 there).

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "action/action.h"
#include "action/game_start.h"
#include "zge/collision/dynamics.h"
#include "zge/game.h"
#include "zge/input/pads.h"

#include <cstdint>
#include "sym/dm2/game_main.cpp.h"

namespace zge
{
    // The engine's main.
    // gameStartup: the game's start (before the main loop): the dynamics
    // file's chunk handlers registered, then the game's start-up. Always 1
    // (AI's also turns the profiler's display off first).
    uint32_t gameStartup()
    {
        registerDynamicsChunkHandlers();
        game::dm2::initGame(); // the UI, the sound, the riders' set-up
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameStartup, Verified);

    // steadyPlayFrame (seconds): the frame callback the engine is handed:
    // the pads read, then the game's frame. Always 1. DM2's takes no frame
    // time (AI's holds a steady one).
    int32_t steadyPlayFrame(ee::Float /*seconds*/)
    {
        readPads();
        game::dm2::playFrame();
        return 1;
    }
    READABLE_FUNCTION_WAITS(steadyPlayFrame, Verified);

    namespace
    {
        // Scaffolding (address-call): the system library's _InitSys (Sony's
        // libkernel, not rewritten) by its address.
        namespace calls
        {
            void initSys() { ee::game(sym::_InitSys); }
        }
    }

    // gameShutdown: the game's end (after the main loop): the game's parts
    // (shutdownGame), the dynamics' pool, then the system library's
    // _InitSys. Always 1.
    uint32_t gameShutdown()
    {
        game::dm2::shutdownGame();
        shutdownDynamicsPool();
        calls::initSys();
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameShutdown, Verified);
    // DM2's engine shows no start-up picture: its initEngine never calls
    // this. The shared initEngine (AI's shape) does, so the instance needs
    // the name. DM2 has no row of its own for it, so sym::legalScreen is AI's
    // name, at the address this instance binds it to (none).
    void legalScreen(ee::Ptr<uint32_t> image, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height, ee::Ptr<uint32_t> depth)
    {
        ee::game(sym::legalScreen, image, width, height, depth);
    }
}
