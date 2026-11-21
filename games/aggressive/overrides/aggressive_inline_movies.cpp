// Aggressive Inline (SLUS-20327): movies decoded on the host, by the
// engine's movie stream on the host (zge/host/movie_stream.cpp, its switch
// zge_movies.cpp) at AI's functions and data, with the Xbox release's movies
// (ZX_XBOX_PACK/movies, the settings row display.xbox_movies) in place of
// the PS2's (engine/host/movies.h). AI's boot plays no intro chain for Skip
// Intro Movies to end.
//
// ZX_MOVIES=hle: none of this (the MPEG/IPU HLE and the IOP sound path).
// ZX_TRACE_MOVIES=1 logs movie starts, ends and dropped frames.
#include "aggressive_inline_movies.h"
#include "overrides/zge_host.h"

#include "ee/game_id.h"

namespace aggressive_inline
{
    bool installNativeMovies(PS2Machine &runtime)
    {
        static constexpr game::host::movies::Game kMovies{
            .name = "Aggressive Inline",
            .xboxMovies = true,
        };
        return zge_host::installNativeMovies(runtime, game::Game::Ai, kMovies);
    }
}
