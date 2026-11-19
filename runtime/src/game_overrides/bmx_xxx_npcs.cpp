// BMX XXX (SLUS-20415): the levels' own NPC riders left out
// (PS2X_BMX_LEVEL_NPCS=0).
//
// A level's NPC riders (game/bmx/game/npcs.cpp, setupNpcs) are not in the
// level's folder: the game has a row of NPC records for each level number
// in its executable, reads their models' definitions from
// assets/create/defs.zal ("def.NPR.<folder>") and their rides from
// assets/replay.zal ("NPR.Lev<short name>.Rider<n>"). The level's people
// and traffic (the PathMan and Car objects) are its own files' objects.
// Files that take a level's folder (a Custom Freeplay map in its slot,
// collection/maps.h) so get that level's NPC riders, standing and riding
// where they stood in the level the files replaced.
//
// With PS2X_BMX_LEVEL_NPCS=0, setupNpcs answers as it does for a
// multiplayer game: the NPC list empty (its first rider kNoNpc), nothing
// loaded, 1. Unset or any other value: nothing is hooked.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_npcs.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace bmx_xxx::npcs
{
    namespace
    {
        constexpr int kRegRa = 31;
        // The NPC list (game/bmx/game/npcs.h Npc): each NPC's rider index
        // at +0x00; kNoNpc in the first ends the list.
        constexpr uint8_t kNoNpc = 0xFF;

        void setupNoNpcs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (uint8_t *firstRider = getMemPtr(rdram, sym::bmx::g_npcs))
                *firstRider = kNoNpc;
            setReturnU32(ctx, 1u);
            ctx->pc = getRegU32(ctx, kRegRa);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = ps2x::boot::text("PS2X_BMX_LEVEL_NPCS");
        if (!on || std::strcmp(on, "0") != 0)
            return;
        if (!runtime.replaceFunction(sym::bmx::setupNpcs, &setupNoNpcs))
        {
            std::cerr << "[bmx-xxx] level NPCs: the setupNpcs hook not installed" << std::endl;
            return;
        }
        std::cout << "[bmx-xxx] level NPCs: off (the levels' own NPC riders left out)" << std::endl;
    }
}

namespace bmx_xxx
{
    void installLevelNpcs(PS2Runtime &runtime)
    {
        npcs::install(runtime);
    }
}
