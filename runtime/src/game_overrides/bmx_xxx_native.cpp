// BMX XXX (SLUS-20415): the disc files and the movies on the host, by the
// engine's disc device's host form (zge/host/disc_device.cpp) and its movie
// stream's (zge/host/movie_stream.cpp) at BMX's functions and data.
// Its level meshes drawn natively from the engine's draw records
// (zge/host/class_draws.cpp, render_path = records, as AI's and DM2's can):
// BMX XXX's native drawing and its default, as it has no packet observer;
// render_path = strict (the Native Meshes row's Off) every mesh through VU1.
//
// BMX's CD/DVD client and ps2Movie.c are Aggressive Inline's code: every
// function with a host form is an identical counterpart of AI's in the work
// database (exact matches), its data addresses paired with AI's
// instruction by instruction, and its IOP side is AI's too (BMX's
// ZCDDVD.IRX and SDRDRV.IRX are byte for byte AI's). One difference: BMX
// has no setAudioDecVolume (nothing sets the movie sound's volume, and its
// host form is not installed; the app's movie volume still applies, in the
// host mix).
//
// [host] disc = ps2, PS2X_MOVIES=hle: the IOP's modules instead.
// The Xbox textures follow the files the game opens (zge_xbox_textures.h).
// PS2X_MODS_DIR, PS2X_TRACE_DISC, PS2X_TRACE_MOVIES as for AI.
#include "bmx_xxx.h"
#include "zge_host.h"
#include "zge_xbox_textures.h"

#include "../game/ee/game_id.h"
#include "../game/host/native_draws.h"

#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_gs_shots.h"

#include <cstring>
#include <iostream>

namespace bmx_xxx
{
    bool installNativeDisc(PS2Runtime &runtime)
    {
        return zge_host::installNativeDisc(runtime, {.game = "BMX XXX",
                                                     .id = game::Game::Bmx,
                                                     .fileOpened = &zge_host::xbox_textures::noteFileOpened});
    }

    bool installNativeMovies(PS2Runtime &runtime)
    {
        static constexpr game::host::movies::Game kMovies{
            .name = "BMX XXX", // (no intro movies: BMX's boot plays none)
        };
        return zge_host::installNativeMovies(runtime, game::Game::Bmx, kMovies);
    }

    namespace
    {
        bool g_addedSetting = false; // the Native Meshes row is this boot's

        void removeSetting(PS2Runtime &)
        {
            if (g_addedSetting)
                ps2x::settings::remove("display.native_meshes");
            g_addedSetting = false;
        }
    }

    bool installNativeDraw(PS2Runtime &runtime)
    {
        // The render path (PS2X_RENDER_PATH, the app's render_path): anything but strict draws from the records.
        const char *path = ps2x::boot::text("PS2X_RENDER_PATH");
        const bool strict = path && (std::strcmp(path, "strict") == 0 || std::strcmp(path, "0") == 0);
        const bool records = !strict;
        // The rigid clipping program's form: VU1 0x06 at 0x1168, AI's passes; the rigid builder (ZGE4's buildRigid)
        // sends a part's normals where its colours go, which VU1 0x04 / 0x06 light with its node's own lights; the
        // skinned builder (ZGE4's buildSkinned) limits a vertex to three bones first; the Xbox release lights its
        // riders with Aggressive Inline's Xbox lights (the same block in its XBE).
        game::host::native_draws::attach(runtime, records,
                                         {.clipped = GSNativeClip::NearSplit, .normalsForColours = true},
                                         {.weights = ps2x::zaxis::SkinnedDrawForm::Weights::ThreeBones,
                                          .xboxLights = true},
                                         /*followInterpolation: its meshes are native only from their records*/ !strict);
        ps2x::gs_shots::install(runtime); // (PS2X_GS_SHOT_VSYNC, developers)
        // The Native Meshes row (Aggressive Inline's key), unless the app added one; switched while the game runs:
        // off, every mesh through VU1 (the strict path) until it is on again.
        if (!ps2x::settings::has("display.native_meshes"))
        {
            ps2x::settings::Setting setting;
            setting.key = "display.native_meshes";
            setting.label = "Native Meshes";
            setting.help = "Level meshes drawn natively. Off: through the PS2's VU1, as on the console.";
            setting.current = []() { return game::host::native_draws::recordsWanted() ? 1 : 0; };
            setting.apply = [](int value) { game::host::native_draws::setRecords(value != 0); };
            ps2x::settings::add(setting);
            g_addedSetting = true;
            runtime.addStoppedHook(&removeSetting);
        }
        std::cout << "[bmx-native] level meshes " << (records ? "native from the draw records" : "through VU1")
                  << std::endl;
        return records;
    }
}
