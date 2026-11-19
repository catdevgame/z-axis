// How the engine's movies run and where the sound driver goes with them (zge_host.h): a movie in its PS2 form
// (Sony's MPEG stream sample, zge/movie/ps2/) sends its sound to the IOP (SDRDRV into the SPU2 core 1 input), so
// ZAUDIO stays on the IOP then: with the native driver nothing drains the movie's audio ring and the intro waits in
// sceMpegGetPicture.
#include "zge_host.h"

#include <cstdlib>
#include <string>

namespace zge_host
{
    namespace
    {
        MovieForm &noted()
        {
            static MovieForm form = []
            {
                const char *mode = std::getenv("PS2X_MOVIES");
                return mode && std::string(mode) == "hle" ? MovieForm::Ps2 : MovieForm::Host;
            }();
            return form;
        }
    }

    MovieForm movieFormFor(bool hleRequested, bool hostFormInstalled)
    {
        return !hleRequested && hostFormInstalled ? MovieForm::Host : MovieForm::Ps2;
    }

    void setMovieForm(MovieForm form) { noted() = form; }

    MovieForm movieForm() { return noted(); }

    bool soundOnIop(const char *soundMode, MovieForm movies)
    {
        return (soundMode && std::string(soundMode) == "spu2") || movies == MovieForm::Ps2;
    }
}
