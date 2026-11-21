// The engine's movies (zge/movie/movie.h; zmovie, movieInterfaceNoop-movieTexture):
// what a game calls, each a call of the platform's player (ps2Movie.c,
// zge/movie/ps2/pss_player.h) once movieInit has run, and the end callback a
// stopped movie tells.

#include "movie.h"
#include "zge/movie/ps2/pss_player.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/movie/movie_player.cpp.h"

namespace zge
{
    namespace
    {
        // What the movie's end callback is handed: how it ended (0: it ran
        // out; the front end's runMovieEndScript acts on that only), with the
        // movie and argument setMovieEndCallback gave.
        struct MovieEnd
        {
            int32_t status;
            uint32_t movie;
            uint32_t argument;
        };
        // (DM2's three words are bound one by one from its table, not yet checked as one record; BMX XXX's
        // rows are the callback and the status, the two words after g_movieReady as AI's: no BMX
        // setMovieEndCallback was found)
        static_assert(game::readable::kIsDm2 || game::readable::kIsBmx ||
                      (sym::g_movieEndFrontEnd - sym::g_movieEndStatus == offsetof(MovieEnd, movie) &&
                       sym::g_movieEndCallbackArgument - sym::g_movieEndStatus == offsetof(MovieEnd, argument)));

        inline ee::Global<ee::FunctionPtr<uint32_t(ee::Ptr<MovieEnd> end)>, sym::g_movieEndCallback> g_movieEndCallback;
        inline ee::Global<MovieEnd, sym::g_movieEndStatus> g_movieEnd;
        inline ee::Global<int32_t, sym::g_movieReady> g_movieReady;
    }


    void movieInterfaceNoop()
    {
    }
    READABLE_FUNCTION(movieInterfaceNoop, Draft);

    uint32_t setMovieEndCallback(uint32_t frontEndMovie, uint32_t movieEndCallback2, uint32_t movieEndCallbackArgument)
    {
        g_movieEnd->argument = movieEndCallbackArgument;
        *g_movieEndCallback = movieEndCallback2;
        g_movieEnd->movie = frontEndMovie;
        return 1;
    }
    READABLE_FUNCTION(setMovieEndCallback, Verified);

    // movieEndCallback: a movie stopped (ps2MovieStop): the end callback set
    // (setMovieEndCallback), if any, is handed how it ended. The movie the
    // stop names is not read.
    void movieEndCallback(uint32_t movie, int32_t status)
    {
        (void)movie;
        if (!*g_movieEndCallback)
            return;
        g_movieEnd->status = status;
        (*g_movieEndCallback)(ee::Ptr<MovieEnd>(sym::g_movieEndStatus));
    }
    READABLE_FUNCTION(movieEndCallback, Verified);

    int32_t movieInit()
    {
        *g_movieReady = 1;
        return zge::ps2MovieInit();
    }
    READABLE_FUNCTION_WAITS(movieInit, Verified);

    uint32_t movieShutdown()
    {
        int32_t ps2MovieShutdown = 0;
        if (*g_movieReady == 0)
        {
            ps2MovieShutdown = 0;
            return ps2MovieShutdown;
        }
        *g_movieReady = 0;
        ps2MovieShutdown = zge::ps2MovieShutdown();
        return ps2MovieShutdown;
        return ps2MovieShutdown;
    }
    READABLE_FUNCTION_WAITS(movieShutdown, Verified);

    uint32_t movieOpen(uint32_t path)
    {
        if (*g_movieReady == 0)
        {
            return 0;
        }
        return zge::ps2MovieOpen(path);
        return 0;
    }
    READABLE_FUNCTION_WAITS(movieOpen, Verified);

    // movieClose: the open movie closed, once movieInit has run (was
    // games/aggressive's, front_end.cpp; the blocks say it is the engine's:
    // layer2).
    uint32_t movieClose(uint32_t handedOn)
    {
        if (*g_movieReady == 0)
        {
            return 0;
        }
        return ps2MovieClose(handedOn);
        return 0;
    }
    READABLE_FUNCTION_WAITS(movieClose, Verified);

    // moviePlay: the open movie played, once movieInit has run. (Both words go
    // on as they came: ZGE2's ps2MoviePlay, playOpenMovie, reads the first as
    // its loop, and DM2's callers pass the loop first.)
    uint32_t moviePlay(uint32_t movie, uint32_t loop)
    {
        if (*g_movieReady == 0)
        {
            return 0;
        }
        return zge::ps2MoviePlay(movie, loop);
        return 0;
    }
    READABLE_FUNCTION_WAITS(moviePlay, Verified);

    uint32_t movieIsPlaying()
    {
        int32_t ps2MovieIsPlaying = 0;
        if (*g_movieReady == 0)
        {
            ps2MovieIsPlaying = 0;
            return ps2MovieIsPlaying;
        }
        ps2MovieIsPlaying = zge::ps2MovieIsPlaying();
        return ps2MovieIsPlaying;
        return ps2MovieIsPlaying;
    }
    READABLE_FUNCTION(movieIsPlaying, Verified);

    // movieStop: the playing movie stopped, once movieInit has run (was
    // games/aggressive's, front_end.cpp; the blocks say it is the engine's:
    // layer2).
    uint32_t movieStop(uint32_t handedOn)
    {
        int32_t ps2MovieStopResult = 0;
        if (*g_movieReady == 0)
        {
            ps2MovieStopResult = 0;
            return ps2MovieStopResult;
        }
        ps2MovieStopResult = zge::ps2MovieStop(handedOn);
        return ps2MovieStopResult;
        return ps2MovieStopResult;
    }
    READABLE_FUNCTION_WAITS(movieStop, Verified);

    // SetMovieAudioVolume.
    uint32_t movieDeadCall(uint32_t movie, uint32_t flags)
    {
        const int32_t volume = flags & 0xFFu;
        if (*g_movieReady == 0)
        {
            return 0;
        }
        return zge::setMovieAudioVolume(movie, volume);
        return 0;
    }

    // setMovieVolumeIfReady: ZGE2's movieDeadCall, one word: the movie audio's
    // volume from its low byte (setMovieVolume), once movieInit has run.
    uint32_t setMovieVolumeIfReady(uint32_t flags)
    {
        if (*g_movieReady == 0)
            return 0;
        return setMovieVolume(flags & 0xFFu);
    }
    READABLE_FUNCTION_WAITS_OWN(movieDeadCall, Draft, dm2, setMovieVolumeIfReady);

    ee::Ptr<uint32_t> movieTexture(uint32_t movie, ee::Ptr<uint32_t> ready)
    {
        if (*g_movieReady == 0)
            return nullptr;
        return zge::ps2MovieTexture(movie, ready);
    }
    READABLE_FUNCTION(movieTexture, Verified);

    // moviePlayerTexture: ZGE2 only: its movieTexture: the player's texture once movieInit has run, else 0.
    inline uint32_t moviePlayerTexture()
    {
        if (*g_movieReady == 0)
            return 0;
        return ps2MoviePlayerTexture();
    }
    READABLE_FUNCTION_ONLY(dm2, moviePlayerTexture);
}
