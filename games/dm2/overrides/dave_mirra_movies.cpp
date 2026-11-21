// Dave Mirra Freestyle BMX 2 (SLUS-20159): movies decoded on the host, by
// the engine's movie stream on the host (zge/host/movie_stream.cpp, its
// switch zge_movies.cpp) at DM2's functions and data. DM2's ps2Movie.c is the
// same code one engine version earlier (ZGE2), its records AI's
// (zge/movie/ps2/pss_player.h). DM2 movies are 512x256 or 256x128 at 30 or
// 29.97 fps (size and rate read from the stream).
//
// Skip Intro Movies (the app's game.skip_intro_movies row, settings.ini
// [general] skip_intro_movies): the front end's boot chain (UILIB.ZAL
// MainMenu: Movie_Acclaim, Movie_ZAxis, Movie_Intro, each fading to the next
// when BmxUpdateMovieStatus says its movie ended, then bmxEndIntro to the
// main menu) gets each of its movies (ACCLAIM.PSS, ZINTRO.PSS, INTRO.PSS)
// ended at its first frame, the first time each plays; the Extras menu's
// Intro Movie later plays as ever.
//
// ZX_MOVIES=hle: none of this (the MPEG/IPU HLE and the IOP sound path,
// SDRDRV). ZX_TRACE_MOVIES=1 logs movie starts, ends and dropped frames.
#include "dave_mirra.h"
#include "overrides/zge_host.h"

#include "ee/game_id.h"

namespace dave_mirra
{
    bool installNativeMovies(PS2Machine &runtime)
    {
        static constexpr game::host::movies::Game kMovies{
            .name = "Dave Mirra Freestyle BMX 2",
            .introMovies = {"ACCLAIM.PSS", "ZINTRO.PSS", "INTRO.PSS"}, // the boot chain
        };
        return zge_host::installNativeMovies(runtime, game::Game::Dm2, kMovies);
    }
}
