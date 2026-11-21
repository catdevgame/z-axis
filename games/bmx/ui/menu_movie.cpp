// The menus' movie (menu_movie.h): BMX XXX's own code.

#include "menu_movie.h"

#include "bmx/input/pads.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/ui_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/movie/movie.h"
#include "zge/render/texture.h"

#include <cstdint>
#include "sym/bmx/ui/menu_movie.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The movie that is open (0: none).
        inline ee::Global<uint32_t, sym::g_frontEndMovie> g_frontEndMovie;
        constexpr ee::Ptr<const char> kVisible{sym::g_textMovieVisible};         // "Visible"
        constexpr ee::Ptr<const char> kMovieMaterial{sym::g_textMovieMaterial}; // "Common.movie"

        // The UI's asset of the material the movie's picture goes on.
        ee::Ptr<zge::zgui::UiAsset> movieMaterialAsset()
        {
            return zge::zgui::findGuiAsset(kMovieMaterial, 0);
        }
        ee::Ptr<zge::Material> materialOf(ee::Ptr<zge::zgui::UiAsset> asset)
        {
            return ee::Ptr<zge::Material>(asset->object);
        }
    }

    int32_t playMenuMovie(ee::Address path, uint32_t loop)
    {
        // (The file's finding and the play's answer share a word.)
        ee::Local<uint32_t> found;
        ee::Local<uint32_t> ready;
        if (zge::findFile(path, found, 0) != 1)
            return 0;
        if (*found == 0)
            return 0;
        if (*g_frontEndMovie)
        {
            zge::movieStop(*g_frontEndMovie);
            zge::movieClose(*g_frontEndMovie);
            *g_frontEndMovie = 0;
        }
        const uint32_t movie = zge::movieOpen(path);
        *g_frontEndMovie = movie;
        if (movie == 0)
        {
            zge::movieClose(0);
            *g_frontEndMovie = 0;
            return 1;
        }
        *found = zge::moviePlay(movie, loop);
        const ee::Ptr<zge::zgui::UiAsset> asset = movieMaterialAsset();
        const ee::Ptr<uint32_t> picture = zge::movieTexture(*g_frontEndMovie, ee::Ptr<uint32_t>(ready.address()));
        zge::setMaterialPicture(materialOf(asset), *picture, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(playMenuMovie, Verified);

    int32_t menuMovieFrame(ee::Ptr<zge::zgui::Node> node)
    {
        ee::Local<uint32_t> visible;
        if (menuFrames() == 2)
        {
            *visible = 1;
            zge::zgui::setVariable(node, kVisible, visible);
        }
        if (menuFrames() < 3)
            return 1;
        if (anyButtonPressed())
            return 0;
        const uint32_t movie = *g_frontEndMovie;
        if (movie == 0)
            return 0;
        return zge::movieIsPlaying() != 0;
    }
    READABLE_FUNCTION_WAITS(menuMovieFrame, Verified);

    void stopMenuMovie()
    {
        const uint32_t movie = *g_frontEndMovie;
        if (movie == 0)
            return;
        *g_frontEndMovie = 0;
        if (zge::movieIsPlaying())
            zge::movieStop(movie);
        zge::movieClose(movie);
        game::tail(zge::setMaterialPicture, materialOf(movieMaterialAsset()), 0, 0);
    }
    READABLE_FUNCTION_WAITS(stopMenuMovie, Verified);
}
