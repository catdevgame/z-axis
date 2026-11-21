// BMX XXX's side of what the engine calls in the game (zge/game.h): its start,
// frame and end (bmx.c), and its packed libraries' reader, as its originals by
// address until each has a readable body here (the game's own start and end
// are game_start.cpp's). BMX XXX's own code, in its instance only, so
// these define the instance's engine names (zge is zge_bmx there). In a game's
// folder a name is that game's own row: these are BMX XXX's own functions.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "bmx/game_start.h"
#include "bmx/states/game_state.h"
#include "zge/collision/dynamics.h"
#include "zge/game.h"
#include "zge/input/pads.h"
#include "zge/platform/platform.h"

#include <cstdint>
#include "sym/bmx/game_main.cpp.h"

namespace zge
{
    namespace
    {
        // The callback BMX XXX's asset reader hands its file reads and seeks
        // (fileRead's, fileSeek's last argument): none, as its animation
        // library loader passes it.
        constexpr uint32_t kNoReadCallback = 0;
    }

    // The engine's main.
    // gameStartup: the game's start (before the main loop): the dynamics
    // file's chunk handlers registered, then the game's start-up. Always 1.
    uint32_t gameStartup()
    {
        registerDynamicsChunkHandlers();
        game::bmx::initGame();
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameStartup, Verified);

    // steadyPlayFrame (seconds): the frame callback the engine is handed:
    // the pads read, then the game's frame. Always 1. BMX XXX's takes no
    // frame time, as DM2's (AI's holds a steady one).
    int32_t steadyPlayFrame(ee::Float /*seconds*/)
    {
        readPads();
        game::bmx::playFrame();
        return 1;
    }
    READABLE_FUNCTION_WAITS(steadyPlayFrame, Verified);

    // gameShutdown: the game's end (after the main loop), then the
    // dynamics' pool freed and the engine's objects left reported. Always 1.
    uint32_t gameShutdown()
    {
        game::bmx::shutDownGame();
        shutdownDynamicsPool();
        reportEngineObjects();
        return 1;
    }
    READABLE_FUNCTION_WAITS(gameShutdown, Verified);
    // BMX XXX's engine shows no start-up picture: its initEngine never calls
    // this. The shared initEngine (AI's shape) does, so the instance needs
    // the name. BMX XXX has no row of its own for it, so sym::legalScreen is
    // AI's name, at the address this instance binds it to (none).
    void legalScreen(ee::Ptr<uint32_t> image, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height, ee::Ptr<uint32_t> depth)
    {
        ee::game(sym::legalScreen, image, width, height, depth);
    }

    // BMX XXX's packed library reader is its own code (none of AI's pairs
    // with it): the functions its animation library loader calls where AI's
    // calls AI's. Its asset read takes one argument more, the file reads'
    // callback.
    ee::Ptr<ZfileLibrary> openZfileLibrary(ee::Address path) { return ee::game<ee::Ptr<ZfileLibrary>>(sym::openZfileLibrary, path); }
    void closeZfileLibrary(ee::Ptr<ZfileLibrary> library) { ee::game(sym::closeZfileLibrary, library); }
    uint32_t zfileLibraryGroupCount(ee::Ptr<ZfileLibrary> library) { return ee::game<uint32_t>(sym::zfileLibraryGroupCount, library); }
    uint32_t nextZfileLibraryGroup(ee::Ptr<ZfileLibrary> library) { return ee::game<uint32_t>(sym::nextZfileLibraryGroup, library); }
    uint32_t zfileLibraryAssetBytes(ee::Ptr<ZfileLibrary> library) { return ee::game<uint32_t>(sym::zfileLibraryAssetBytes, library); }
    uint32_t readZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, ee::Address dst)
    {
        return ee::game<uint32_t>(sym::readZfileLibraryAsset, library, dst, kNoReadCallback);
    }
}
