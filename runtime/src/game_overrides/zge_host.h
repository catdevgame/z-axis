#pragma once

// The Z-Axis engine's (ZGE) host services shared by the games built on it:
// one body each, with the game's functions and data in a table its override
// fills from its symbols (the disc device's and the movie stream's are the
// engine code's own, at each game's symbols, with what the game adds):
// Aggressive Inline (aggressive_inline_disc.cpp, aggressive_inline_movies.cpp),
// Dave Mirra Freestyle BMX 2 (dave_mirra_disc.cpp, dave_mirra_movies.cpp)
// and BMX XXX (bmx_xxx_native.cpp). What only one game has is a row of its
// table (AI's Xbox movies, its Xbox textures and native asset readers; DM2's
// intro movies).
//
//   zge_disc.cpp    the switch of the engine's "dvd0" file device on host
//                   files (zge/host/disc_device.cpp: the game's files with
//                   the Mods folder over them), in place of Z-Axis' CD/DVD
//                   client and ZCDDVD.IRX on the IOP;
//   zge_movies.cpp  the switch of the engine's movie stream on the host
//                   (zge/host/movie_stream.cpp: the movie decoded by the host,
//                   game/host/movies.h; the picture through the game's own
//                   texture upload, the sound mixed into the game's), in place
//                   of the MPEG stream sample, the IPU and SDRDRV.IRX.

#include "../game/ee/game_id.h"
#include "../game/host/movies.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace zx::game_files
{
    class GameFile;
}

namespace zge_host
{
    // What a game adds to the engine's disc device (zge/host/disc_device.cpp,
    // the same in every game at each one's functions and data).
    struct DiscAdditions
    {
        const char *game;            // for the log
        game::Game id;               // whose host forms (ee/host_form.h)
        // A disc file was opened (the path as the game gave it, found or not);
        // null: nothing.
        void (*fileOpened)(const std::string &path) = nullptr;
        // `bytes` of the file at `path` from `offset` were read to guest
        // `address`; null: nothing.
        void (*fileRead)(const std::string &path, uint64_t offset, uint32_t address, uint32_t bytes) = nullptr;
    };

    // The game's "dvd0" device on host files, unless [host] disc = ps2 (then
    // ZCDDVD over the emulated IOP, as on the console). True when installed:
    // the game then never talks to ZCDDVD.IRX.
    bool installNativeDisc(PS2Runtime &runtime, const DiscAdditions &additions);

    // A file as the native disc device serves it (Mods first).
    std::shared_ptr<zx::game_files::GameFile> openGameFile(const std::string &path);

    // The engine's movie stream on the host, unless ZX_MOVIES=hle (then the
    // MPEG HLE and the IOP's SDRDRV for the sound): the host forms of
    // zge/host/movie_stream.cpp at the game's functions, with what the game
    // adds (game/host/movies.h: AI's Xbox movies, DM2's intro movies). True
    // when installed: the game then never talks to SDRDRV.IRX.
    bool installNativeMovies(PS2Runtime &runtime, game::Game id, const game::host::movies::Game &game);
    // How the game's movies run (zge_movie_form.cpp): their host form, or their PS2 form (ZX_MOVIES=hle, or
    // the host form not installed), whose sound goes through the IOP's SDRDRV into the SPU2 that ZAUDIO on the
    // IOP sets up. installNativeMovies decides it (movieFormFor) and notes it; before it: what ZX_MOVIES says.
    enum class MovieForm
    {
        Host,
        Ps2
    };
    MovieForm movieFormFor(bool hleRequested, bool hostFormInstalled);
    void setMovieForm(MovieForm form);
    MovieForm movieForm();
    // Where the sound driver runs: ZAUDIO on the IOP (true) with ZX_SOUND=spu2 (soundMode) or when the movies
    // run their PS2 form; else the native driver (zge_sound.cpp).
    bool soundOnIop(const char *soundMode, MovieForm movies);

    // ---- Exit to Collection (zge_exit.cpp) ---------------------------------
    //
    // In the collection (the app has a menu to come back to,
    // ps2_host::menuAvailable), DM2 and BMX XXX get an item in their own
    // menus that leaves the game for that menu, drawn by the game's menu
    // code in its font. Single-game apps and the runner never get one, so
    // their games run as ever.

    // The item's text (both games' menu fonts draw capitals).
    constexpr const char *kExitItem = "EXIT TO COLLECTION";
    // The script function an item's script CALLs: back to the collection's
    // menu.
    constexpr const char *kExitFunction = "pcExitToCollection";

    // The app has a menu to come back to.
    bool inCollection();

    // The engine's table of script functions (zgui: what a UI script's CALL
    // reaches by name) in one game's ELF.
    struct ScriptFunctions
    {
        const char *game;          // for the log
        uint32_t registerFunction; // (name, function): the game registers its own with it
        const char *after;         // one of the game's own, registered with its menu's: ours go
                                   // into the table right after it
        uint32_t slotsBegin;       // code inside one of the game's functions with no entry
        uint32_t slotsEnd;         // point there: our functions' addresses (nothing jumps there)
    };

    // A host function among the game's script functions, by name: the
    // first one added puts the hook on registerFunction; each gets an entry
    // of its own in the slots. False if it could not be added (another
    // table, no slot left).
    bool addScriptFunction(PS2Runtime &runtime, const ScriptFunctions &functions, const char *name,
                           void (*function)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime));

    // pcExitToCollection among the game's script functions. True when the
    // hook is in place.
    bool installExitFunction(PS2Runtime &runtime, const ScriptFunctions &functions);

    // The item was chosen (where: which menu, for the log): the game is
    // left for the collection's menu (ps2_host::requestExitToMenu).
    void exitToCollection(const char *game, const char *where);
}
