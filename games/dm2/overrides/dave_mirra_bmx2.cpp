// Game override: Dave Mirra Freestyle BMX 2 (USA), SLUS-20159, ELF CRC32
// 0x79A48597 (Z-Axis ZGE2 engine, one version before Aggressive Inline's
// ZGE3).
//
// Address bindings here are only valid for this exact ELF build. The
// mapping from Aggressive Inline's hooks to this ELF is the work database's
// (`workdb.py card NAME`, its counterparts) and games/dm2/docs/BRINGUP.md. Selected by
// ELF name, entry and CRC32, so nothing here runs for any other game.
#include "ee/game_id.h"
#include "sym/game_overrides/dave_mirra_bmx2.cpp.h"
#include "dave_mirra.h"
#include "game_overrides.h"
#include "machine.h"
#include "runtime/pad.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace game::readable
{
    // The readable game (ps2/ee/readable.h) for DM2: its own
    // registrations only (AI's never install here), the call recorder
    // (ZX_FNREC) and the profiler (ZX_FNPROF).
    void install(PS2Machine &runtime, game::Game game);
}

// Named, so that a unity build does not mix these with the other games'
// override helpers.
namespace dave_mirra::boot
{
    struct Binding
    {
        uint32_t address;
        const char *handler;
        const char *why;
    };

    constexpr Binding kBindings[] = {
        // libmpeg accessors the SCE scan did not name (between
        // sceMpegGetDecodeMode 0x1CF5B0 and sceMpegReset 0x1CF5F0, the same
        // code as Aggressive Inline's 0x207D78 / 0x207D88). Recompiled, they
        // read decoder state only the real decoder updates; the MPEG HLE
        // tracks it instead.
        {sym::dm2::sceMpegIsEnd, "sceMpegIsEnd", "returns mp->sys->isEnd"},
        {sym::dm2::sceMpegIsRefBuffEmpty, "sceMpegIsRefBuffEmpty", "returns mp->sys->refCount == 0"},
        // libgraph sceGsGetGParam (unnamed by the SCE scan: returns the
        // static 0x247110). DM2 runs the original libgraph double-buffer
        // code (recomp/tools/make-config.py), which reads the video mode
        // from here; the runtime's sceGsResetGraph keeps it in its own copy.
        {sym::dm2::sceGsGetGParam, "sceGsGetGParam", "libgraph video mode parameters"},
    };

    void applyDaveMirra(PS2Machine &runtime)
    {
        // First, as AI's override does: hooks below wrap what it installs.
        game::readable::install(runtime, game::Game::Dm2);
        for (const Binding &binding : kBindings)
        {
            if (!ps2_game_overrides::bindAddressHandler(runtime, binding.address, binding.handler))
                std::cerr << "[dave-mirra] failed to bind 0x" << std::hex << binding.address << std::dec << " -> "
                          << binding.handler << std::endl;
        }
        // DM2's controls (its defaults and action names,
        // game_controls.cpp); controls.ini and the other host files live
        // in this game's own folders (runtime/host_input.h).
        PSPadBackend::setGame("dm2");
        dave_mirra::install(runtime);
        std::cout << "[dave-mirra] override applied (" << std::size(kBindings) << " bindings)" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Dave Mirra Freestyle BMX 2 (USA)", "SLUS_201.59", sym::dm2::start, 0x79A48597u, &dave_mirra::boot::applyDaveMirra);
