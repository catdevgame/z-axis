// The front end's movies (movie_menu.h): DM2's own code (its game block,
// registered as script functions by the menu set-up).

#include "movie_menu.h"
#include "menu_variables.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/movie/movie.h"
#include "zge/render/material.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/menu/movie_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node: the scripts run on it.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        // The material the menus' background movie plays on.
        constexpr uint32_t kMovieMaterialName = sym::movieMaterialName; // "Common.Movie1"
        constexpr uint32_t kMaterialAsset = 0;
        // The background shows the movie's material again and the still
        // pictures' library is unloaded:
        // "SET Root.CommonUI.Background.Movie.Material {"Common.Movie1"}",
        // "UNLOADLIB UIBack".
        constexpr ee::Ptr<char> kMovieBackgroundScript{sym::movieBackgroundScript};

        // The front end's movies by "MovieID".
        struct MenuMovie
        {
            uint32_t path;   // the .pss file
            uint32_t _04[2]; // 0x200 and 0x100 (the last entry: 0x100 and 0x80); nothing here reads them
            uint32_t loop;   // moviePlay's word
        };
        static_assert(sizeof(MenuMovie) == 0x10);
        constexpr uint32_t kMenuMovies = 20; // the last one is never played: its number is the still background's
        inline ee::Global<MenuMovie[kMenuMovies], sym::g_menuMovies> g_menuMovies;
        // MovieID 0x13 is no movie: one of ten still pictures behind the
        // menus, picked at random:
        // "LOADIMAGE UIBack UILIB:UIBack%d.bmp",
        // "SET Root.CommonUI.Background.Movie.Material {"UIBack.UIBack"}".
        constexpr uint32_t kStillBackground = 0x13;
        constexpr uint32_t kStillBackgrounds = 10;
        constexpr uint32_t kStillBackgroundFormat = sym::stillBackgroundScriptFormat;
    }

    // bmxUpdateMovieStatus: the script's "BmxUpdateMovieStatus": the
    // MovieStatus variable set to whether a movie is playing.
    void bmxUpdateMovieStatus()
    {
        *g_movieStatus = zge::movieIsPlaying();
    }
    READABLE_FUNCTION(bmxUpdateMovieStatus, Verified);

    // bmxStartMovie: the script's "BmxStartMovie": the movie MovieID names
    // opened, put on the movie material and played; or, for the still
    // background, a random picture loaded and shown in its place. Nothing
    // without the movie material, or when the movie does not open. (DM2's
    // moviePlay takes the loop word alone, first.)
    void bmxStartMovie()
    {
        const ee::Ptr<zge::zgui::UiAsset> asset = zge::zgui::findGuiAsset(kMovieMaterialName, kMaterialAsset);
        if (!asset)
            return;
        const uint32_t movie = *g_movieId;
        if (movie == kStillBackground)
        {
            ee::Local<char[256]> script;
            zge::formatString(script, kStillBackgroundFormat,
                              zge::randomNumber() % kStillBackgrounds);
            zge::zgui::runScriptLines(*g_uiRoot, ee::Ptr<char>(script));
            return;
        }
        const MenuMovie &entry = (*g_menuMovies)[movie];
        if (zge::movieOpen(entry.path))
        {
            zge::setMaterialTexture(ee::Ptr<zge::Material>(asset->object), zge::moviePlayerTexture(), 0, 0);
            zge::moviePlay(entry.loop, 0);
        }
    }
    READABLE_FUNCTION_WAITS(bmxStartMovie, Verified);

    // bmxStopMovie: the script's "BmxStopMovie": the movie stopped, its
    // picture taken off the movie material, the movie closed and the
    // background put back to the movie's material. The word the script
    // hands the function goes on to movieStop untouched, as in the original
    // (DM2's movieStop reads none; neither do its movieClose and
    // setMaterialTexture read the shared declarations' last word: 0).
    void bmxStopMovie(uint32_t scriptWord)
    {
        zge::movieStop(scriptWord);
        const ee::Ptr<zge::zgui::UiAsset> asset = zge::zgui::findGuiAsset(kMovieMaterialName, kMaterialAsset);
        zge::setMaterialTexture(ee::Ptr<zge::Material>(asset->object), 0, 0, 0);
        zge::movieClose(0);
        game::tail(zge::zgui::runScriptLines, *g_uiRoot, kMovieBackgroundScript);
    }
    READABLE_FUNCTION_WAITS(bmxStopMovie, Verified);
}
