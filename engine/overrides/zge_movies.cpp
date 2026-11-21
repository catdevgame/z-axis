// The Z-Axis engine's movies on the host, for every game on the engine
// (zge_host.h): the switch. The movie stream itself is engine code, one body
// for the three games at each one's functions and data: zge/host/
// movie_stream.cpp (the host forms of the player's stream parts: startMovie,
// stepMovie, endMovie and the movie's sound, over the movie decoding of
// engine/host/movies.h), installed over the PS2 form (zge/movie/ps2/: Sony's
// MPEG stream sample with its demuxer and decoder threads, the IPU, the sound
// through the IOP into the SPU2) by its own switch, whatever ZX_READABLE
// says. SDRDRV.IRX is then not loaded at all. What a game adds is its row of
// engine/host/movies.h's Game (aggressive_inline_movies.cpp: the Xbox movies;
// dave_mirra_movies.cpp: the intro movies; bmx_xxx_native.cpp: nothing).
//
// The switch: ZX_MOVIES=hle runs the PS2 form over the emulated MPEG
// decoder and the IOP's sound driver, as on the console.
// ZX_TRACE_MOVIES=1 logs movie starts, ends and dropped frames.
#include "zge_host.h"

#include "ee/game_id.h"
#include "host/movies.h"

#include "machine.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace game::host
{
    // Every host form of a system over the game's functions (ps2/ee/host_form.h).
    bool install(PS2Machine &runtime, Game game, const char *system);
}

namespace zge_host
{
    bool installNativeMovies(PS2Machine &runtime, game::Game id, const game::host::movies::Game &game)
    {
        const char *mode = std::getenv("ZX_MOVIES");
        if (mode && std::string(mode) == "hle")
        {
            setMovieForm(movieFormFor(true, false));
            std::cout << "[movie] libmpeg HLE and the IOP sound path (ZX_MOVIES=hle)" << std::endl;
            return false;
        }
        const char *trace = std::getenv("ZX_TRACE_MOVIES");
        game::host::movies::configure(game, trace && trace[0] == '1');
        if (!game::host::install(runtime, id, "movies"))
        {
            // (the PS2 form runs, its sound through the IOP: the sound driver stays there too)
            setMovieForm(movieFormFor(false, false));
            std::cerr << "[movie] " << game.name << ": native movie hooks not installed: the PS2 form runs, with "
                      << "the IOP's sound path" << std::endl;
            return false;
        }
        setMovieForm(movieFormFor(false, true));
        game::host::movies::attach();
        std::cout << "[movie] movies decoded on the host (ZX_MOVIES=hle for the libmpeg HLE)" << std::endl;
        return true;
    }
}
