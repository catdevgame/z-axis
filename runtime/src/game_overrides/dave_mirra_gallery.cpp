// Dave Mirra Freestyle BMX 2 (SLUS-20159): the Retro movie as the movie
// gallery's last line ([extras] retro_movie, PS2X_DM2_RETRO_MOVIE=1; off by
// default).
//
// The PS2's menu movie table already has it: g_menuMovies[15] is
// assets\movie\retro.pss, and the gallery's line becomes Root.MovieID
// (UILIB MainMenu.MovieFrame.Menu: SET Root.MovieID Menu.SelectedIndex), but
// bmxmenuGameMovieUpdate writes 15 lines (Intro Movie and the fourteen
// riders) and the disc has no RETRO.PSS. The Xbox's and the GameCube's write
// a 16th, Retro, open once their saved game's feature bit 3 is
// (KSTRSP_REWARD_FEATURE03, "Check the video gallery for the retro BMX
// movie!": a reward the PS2's career never gives); their discs have the
// movie (Xbox movie/retro.bik). Here the line is added after the game's,
// open, and assets\movie\retro.pss is the Xbox's movie made into a PSS at app
// build (tooling/ai/tools/dm2_xbox.py: <PS2X_DM2_XBOX_LEVELS_DIR>/movies/
// ASSETS/MOVIE/RETRO.PSS, the picture at its 512x256 and 30 frames a second,
// its sound as the PS2 movies' 48 kHz PCM).

#include "dave_mirra.h"
#include "../game/host/movies.h"
#include "sym/game_overrides/dave_mirra_gallery.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_session.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace dave_mirra::gallery
{
    namespace
    {
        namespace gf = ps2x::game_files;
        namespace fs = std::filesystem;

        constexpr const char kTag[] = "[gallery]";
        constexpr const char kOwner[] = "dm2-gallery";
        constexpr int kRegSp = 29;

        constexpr const char kRetroFile[] = "ASSETS/MOVIE/RETRO.PSS";
        // The 16th line after the game's 15 (MovieID 15, retro.pss).
        constexpr const char kRetroLine[] = "STRCAT Root.MainMenu.MovieFrame.Menu.Items {\"\\nRetro\"}";

        struct State
        {
            fs::path movie;
            PS2Runtime::RecompiledFunction movieUpdate = nullptr;
        };
        State &state() { return ps2x::session::state<State>(); }

        // The movie, read at each open (not kept: 82 MB; a Bink file the host
        // plays: served so the game finds it).
        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            if (path != kRetroFile)
                return nullptr;
            std::ifstream in(state().movie, std::ios::binary);
            if (!in)
                return nullptr;
            return std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // bmxmenuGameMovieUpdate: the game's 15 lines, then Retro's.
        void movieUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.movieUpdate(rdram, ctx, runtime);
            const uint32_t stack = (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu;
            const uint32_t text = stack + 0x100u;
            uint32_t root = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, sym::dm2::g_uiRoot))
                std::memcpy(&root, p, 4);
            if (uint8_t *p = getMemPtr(rdram, text))
                std::memcpy(p, kRetroLine, sizeof(kRetroLine));
            runtime->callGuestFunction(rdram, ctx, sym::dm2::runScript, {root, text}, stack);
        }
    }

    bool install(PS2Runtime &runtime)
    {
        State &st = state();
        const char *on = ps2x::boot::text("PS2X_DM2_RETRO_MOVIE");
        const char *dir = ps2x::boot::text("PS2X_DM2_XBOX_LEVELS_DIR");
        if (!on || on[0] != '1')
            return false;
        std::error_code ec;
        // The Retro movie: a PSS the workshop's build made (tools/dm2_xbox.py),
        // or the Xbox disc's Bink file the setup copied, which the movie player
        // plays for the gallery's retro.pss (game/host/movies.h setHostMovie).
        const fs::path movies = fs::path(dir ? dir : "") / "movies" / "ASSETS" / "MOVIE";
        st.movie = movies / "RETRO.PSS";
        if (!dir || !*dir || !fs::is_regular_file(st.movie, ec))
        {
            st.movie = movies / "RETRO.BIK";
            if (!dir || !*dir || !fs::is_regular_file(st.movie, ec) || !game::host::movies::playedOnHost())
            {
                std::cout << kTag << " no Retro movie: no " << (movies / "RETRO.PSS").string() << " or RETRO.BIK"
                          << std::endl;
                return false;
            }
            const std::u8string file = st.movie.u8string();
            game::host::movies::setHostMovie(kRetroFile, std::string(file.begin(), file.end()),
                                             game::host::movies::kPs2MovieWidth, game::host::movies::kPs2MovieHeight);
        }
        st.movieUpdate = runtime.lookupFunction(sym::dm2::bmxmenuGameMovieUpdate);
        if (!st.movieUpdate || !runtime.replaceFunction(sym::dm2::bmxmenuGameMovieUpdate, &movieUpdateHook))
        {
            std::cerr << kTag << " bmxmenuGameMovieUpdate not hooked: no Retro movie" << std::endl;
            return false;
        }
        gf::registerProvider("ASSETS/MOVIE/", &provide, kOwner);
        std::cout << kTag << " the Retro movie as the gallery's 16th line (" << st.movie.string() << ")" << std::endl;
        return true;
    }
}

namespace dave_mirra
{
    bool installRetroMovie(PS2Runtime &runtime)
    {
        return gallery::install(runtime);
    }
}
