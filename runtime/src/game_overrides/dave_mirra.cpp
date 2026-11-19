// Dave Mirra Freestyle BMX 2 (SLUS-20159): which native modules replace
// which IOP modules. See dave_mirra_bmx2.cpp for the ELF identity.
#include "dave_mirra.h"
#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "zge_xbox_textures.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace dave_mirra
{
    void install(PS2Runtime &runtime)
    {
        installDisplay(runtime);
        installView(runtime);
        installRestart(runtime);
        installExitToCollection(runtime);
        installSettingsMenus(runtime);
        installCreatePlayer(runtime); // after the other main menu edits (Exit to Collection's line moves down)
        installPrompts(runtime);
        installMouseLook(runtime);
        installGameplay(runtime);
        installDevTools(runtime);
        installNativeSaves(runtime);
        installXboxLevels(runtime);
        zge_host::xbox_textures::install(runtime, game::Game::Dm2); // the extras' Xbox textures (Settings > Graphics)
        installXboxThemes(runtime);     // [extras] xbox_themes
        installRetroMovie(runtime);     // [extras] retro_movie
        installAttractDemo(runtime);    // [extras] attract_demo
        installNative(runtime);
        installXboxCareer(runtime);
        installOpenLevels(runtime);     // developers, PS2X_DEV_UNLOCK_ALL=1
        installDevStart(runtime);       // PS2X_DEV_START_LEVEL: developers, Custom Freeplay
        installGsTextureAudit(runtime); // developers, PS2X_GSMEM_AUDIT=1
        panel_state::install();         // the Thor's bottom screen reads DM2's state (dave_mirra_panel.cpp)
        // The GPU renderer reads mip levels that sit in a render target from
        // GS memory, as the GS does: DM2's level textures have level 0 at
        // TBP 0, inside the frame buffer (gs_gpu_backend.cpp). For DM2's
        // boot only, unless the environment or its BootConfig says.
        if (!ps2x::boot::text("PS2X_GS_MIP_TARGET_LEVELS"))
            ps2x::boot::change("PS2X_GS_MIP_TARGET_LEVELS", "memory");
        // Pad and memory card are HLE on the EE (scePad*, sceMc* bound by
        // the config), so their IOP modules have nothing to serve.
        std::vector<std::string> skipped{"SIO2MAN.IRX", "PADMAN.IRX", "MCMAN.IRX", "MCSERV.IRX"};
        if (installNativeDisc(runtime))
            skipped.push_back("ZCDDVD.IRX");
        if (installNativeMovies(runtime))
            skipped.push_back("SDRDRV.IRX");
        // LIBSD only serves ZAUDIO and SDRDRV.
        if (installNativeSound(runtime))
        {
            installGameVolumes(runtime);
            skipped.push_back("ZAUDIO.IRX");
            if (std::find(skipped.begin(), skipped.end(), "SDRDRV.IRX") != skipped.end())
                skipped.push_back("LIBSD.IRX");
        }
        // Every IRX the game loads skipped: nothing to run on the IOP
        // (PS2X_IOP=1 keeps it running anyway).
        const bool iopIdle = skipped.size() == 8u;
        runtime.setSkippedIopModules(std::move(skipped));
        const char *iop = std::getenv("PS2X_IOP");
        if (iopIdle && !(iop && iop[0] == '1'))
        {
            runtime.setIopStopped(true);
            std::cout << "[dave-mirra] the IOP is not run: nothing the game uses is on it (PS2X_IOP=1 runs it)" << std::endl;
        }
    }
}
