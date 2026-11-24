// BMX XXX's movie state (movie_state.h). BMX XXX's own code (bmx.c).

#include "movie_state.h"

#include "game_state.h"

#include "bmx/rider/rider.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/movie/movie.h"
#include "zge/render/texture.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/bmx/states/movie_state.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        // The movie to play: a row of the movies' table (its file's name),
        // as the reward that won it set it.
        inline ee::Global<uint8_t, sym::g_rewardKind16Value> g_rewardKind16Value;
        struct MovieRow
        {
            ee::Ptr<const char> name;
            uint32_t flags;
        };
        inline ee::Global<MovieRow[20], sym::g_movieRows> g_movieRows;
        // The movie's file, and the movie that is open (0: none).
        inline ee::Global<char[0x40], sym::g_moviePath> g_moviePath;
        inline ee::Global<uint32_t, sym::g_stateMovie> g_stateMovie;
        constexpr uint32_t kMovieFile = sym::g_textMovieFile;                       // "assets\\movies\\%s.%s"
        constexpr uint32_t kMovieExtension = sym::g_textMovieExtension;             // "pss"
        constexpr ee::Ptr<const char> kMovieMaterial{sym::g_textStateMovieMaterial}; // "Common.movie"
        constexpr uint32_t kMovieFrame = sym::g_textUiCommonMovie;                  // "UI_Common.Movie"
        constexpr uint32_t kVisibleName = sym::g_textStateMovieVisible;             // "Visible"
        constexpr uint32_t kNoLoop = 0;
        // The frames the state has run (game_state.cpp's).
        inline ee::Global<uint32_t, sym::g_gameStateFrames> g_gameStateFrames;
        // The frame the movie's frame is shown at, and the buttons that end it.
        constexpr uint32_t kShowFrame = 2;
        constexpr uint32_t kSkipButtons = zge::kPadCross | zge::kPadStart;
        constexpr uint32_t kPorts = 2;

        ee::Ptr<zge::Material> movieMaterial()
        {
            return ee::Ptr<zge::Material>(zge::zgui::findGuiAsset(kMovieMaterial, 0)->object);
        }
        void showMovieFrame(ee::Local<uint32_t> &visible)
        {
            zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kMovieFrame, kFrameNode), kVisibleName, visible);
        }
    }

    int32_t movieEnter()
    {
        ee::Local<uint32_t> ready;
        const MovieRow &row = (*g_movieRows)[*g_rewardKind16Value];
        zge::formatString(sym::g_moviePath, kMovieFile, row.name, kMovieExtension);
        *g_stateMovie = zge::movieOpen(sym::g_moviePath);
        if (*g_stateMovie == 0)
            return 0;
        const ee::Ptr<zge::Material> material = movieMaterial();
        const ee::Ptr<uint32_t> picture = zge::movieTexture(*g_stateMovie, ee::Ptr<uint32_t>(ready.address()));
        zge::setMaterialPicture(material, *picture, 0);
        zge::moviePlay(*g_stateMovie, kNoLoop);
        return 1;
    }
    READABLE_FUNCTION_WAITS(movieEnter, Verified);

    void movieExit()
    {
        ee::Local<uint32_t> visible;
        *visible = 0;
        showMovieFrame(visible);
        const uint32_t movie = *g_stateMovie;
        if (movie == 0)
            return;
        zge::movieStop(movie);
        zge::setMaterialPicture(movieMaterial(), 0, 0);
        zge::movieClose(*g_stateMovie);
    }
    READABLE_FUNCTION_WAITS(movieExit, Verified);

    int32_t movieFrame()
    {
        ee::Local<uint32_t> visible;
        if (*g_stateMovie == 0)
        {
            setGameState(kGameStateLevel);
            return 1;
        }
        const uint32_t frames = *g_gameStateFrames;
        if (frames < kShowFrame)
            return 1;
        if (frames == kShowFrame)
        {
            *visible = 1;
            showMovieFrame(visible);
            return 1;
        }
        for (uint32_t port = 0; port < kPorts; ++port)
        {
            const ee::Ptr<zge::Pad> pad = rider::padInput(port)->pad;
            if ((pad->buttons & pad->changed & kSkipButtons) != 0 || !zge::movieIsPlaying())
            {
                setGameState(kGameStateLevel);
                return 1;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(movieFrame, Verified);
}
