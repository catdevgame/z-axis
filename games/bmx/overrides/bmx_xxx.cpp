// Game override: BMX XXX (USA), SLUS-20415, ELF CRC32 0xE423568C (Z-Axis ZGE3,
// the same engine as Aggressive Inline). Selected by ELF name, entry and CRC32,
// so nothing here runs for any other game.
//
// The readable game for BMX's registrations (with the call recorder,
// ZX_FNREC, and the profiler, ZX_FNPROF), the host's controls for this
// game, then the native services that replace the IOP's modules: disc files
// and movies (bmx_xxx_native.cpp) and the ZAudio sound driver
// (bmx_xxx_sound.cpp). Pad and memory card are HLE on the EE (scePad*,
// sceMc* bound by the config), so with those three nothing the game uses is
// on the IOP, and it is not run.
#include "ee/game_id.h"
#include "sym/game_overrides/bmx_xxx.cpp.h"
#include "bmx_xxx.h"
#include "game_overrides.h"
#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/pad.h"
#include "overrides/zge_xbox_textures.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace game::readable
{
    void install(PS2Machine &runtime, game::Game game);
}

namespace bmx_xxx
{
    void apply(PS2Machine &runtime)
    {
        // First, as AI's and DM2's overrides do: hooks below wrap what it
        // installs.
        game::readable::install(runtime, game::Game::Bmx);
        // BMX XXX's controls (its defaults and action names,
        // game_controls.cpp), in its own config folder.
        PSPadBackend::setGame("bmx");
        installExitToCollection(runtime);
        installSettingsMenus(runtime);
        installPrompts(runtime);
        installGameplay(runtime);
        installDevTools(runtime);
        installView(runtime);
        installMouseLook(runtime);
        installNativeSaves(runtime);
        installReleases(runtime);
        zge_host::xbox_textures::install(runtime, game::Game::Bmx); // the extras' Xbox textures (Settings > Graphics)
        installGsTextureAudit(runtime); // developers, ZX_GSMEM_AUDIT=1
        installDevStart(runtime); // ZX_DEV_START_LEVEL: developers, Custom Freeplay
        installAllMultiplayerModes(runtime);
        installLevelNpcs(runtime); // ZX_BMX_LEVEL_NPCS=0: a Custom Freeplay map without the slot's NPC riders
        panel_state::install(); // the Thor's bottom screen reads BMX XXX's state (bmx_xxx_panel.cpp)
        // The GPU renderer may keep its 16-bit Z buffers as D16 targets (the
        // Depth Test setting's Fast, gs_gpu_backend.cpp depth16) on the strict
        // path: BMX XXX's native meshes (from the draw records, the default)
        // need a float Z for their precise depth.
        if (!installNativeDraw(runtime) && !zx::boot::text("ZX_GS_D16_TARGETS"))
            zx::boot::change("ZX_GS_D16_TARGETS", "1");
        // The eight IRX modules BMX loads (the same as AI's). Pad and memory
        // card are HLE on the EE: the game never binds PADMAN's or MCSERV's
        // RPC servers.
        std::vector<std::string> skipped{"SIO2MAN.IRX", "PADMAN.IRX", "MCMAN.IRX", "MCSERV.IRX"};
        if (installNativeDisc(runtime))
            skipped.push_back("ZCDDVD.IRX");
        if (installNativeMovies(runtime))
            skipped.push_back("SDRDRV.IRX");
        // LIBSD only serves ZAUDIO and SDRDRV.
        if (installNativeSound(runtime))
        {
            skipped.push_back("ZAUDIO.IRX");
            if (std::find(skipped.begin(), skipped.end(), "SDRDRV.IRX") != skipped.end())
                skipped.push_back("LIBSD.IRX");
        }
        // Every IRX the game loads skipped: nothing to run on the IOP
        // (ZX_IOP=1 keeps it running anyway).
        const bool iopIdle = skipped.size() == 8u;
        runtime.setSkippedIopModules(std::move(skipped));
        const char *iop = std::getenv("ZX_IOP");
        if (iopIdle && !(iop && iop[0] == '1'))
        {
            runtime.setIopStopped(true);
            std::cout << "[bmx-xxx] the IOP is not run: nothing the game uses is on it (ZX_IOP=1 runs it)" << std::endl;
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("BMX XXX (USA)", "SLUS_204.15", sym::bmx::start, 0xE423568Cu, &bmx_xxx::apply);
