// Dave Mirra Freestyle BMX 2 (SLUS-20159): the game's disc files read
// straight from the host, by the engine's disc device's host form
// (zge/host/disc_device.cpp; zge_disc.cpp, its switch) at DM2's functions and
// data.
//
// ZGE2's Z-Axis CD/DVD client is the same code as ZGE3's, 0x6800 lower
// (every function an exact match). The init is the same as ZGE3's plus
// sceCdDiskReady/sceFsReset, which only matter for the IOP's cdvdman.
//
// The Xbox textures follow the files the game opens (zge_xbox_textures.h).
//
// [host] disc = ps2 keeps ZCDDVD over the emulated IOP. ZX_MODS_DIR and
// ZX_TRACE_DISC as for Aggressive Inline.
#include "dave_mirra.h"
#include "zge_host.h"
#include "zge_xbox_textures.h"

#include "../game/ee/game_id.h"

namespace dave_mirra
{
    bool installNativeDisc(PS2Runtime &runtime)
    {
        return zge_host::installNativeDisc(runtime, {.game = "Dave Mirra Freestyle BMX 2",
                                                     .id = game::Game::Dm2,
                                                     .fileOpened = &zge_host::xbox_textures::noteFileOpened});
    }
}
