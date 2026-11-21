// Dave Mirra Freestyle BMX 2 (SLUS-20159): the GS texture memory audit
// (ZX_GSMEM_AUDIT=1, developers; runtime/ps2_gs_texture_audit.h).
//
// DM2's texture cache level allocator FUN_00106EA8 is Aggressive Inline's
// FUN_00107C88 instruction for instruction, its state at 0x25CD20 AI's
// TextureCache (low cursor 0x25CD34, high 0x25CD38, overflow 0x25CD44;
// dave_mirra_xboxlevel.cpp's trace). The level: the session's level byte
// 0x2566CA (SCENE.TXT's order: 0 SWAMPYTR, 1 TRAINYAR, 2 WOODWARD, 3
// COMMERCI, 4 DEVILSPE, 5 CLOVERLE, 6 GALLOON, 7 AIRPORT), or the Xbox
// level served in Woodward Camp's slot (dave_mirra_xboxlevel.cpp).
#include "dave_mirra.h"

#include "ps2_runtime.h"
#include "runtime/ps2_gs_texture_audit.h"
#include "runtime/ps2_memory.h"

#include <cstring>
#include <iterator>
#include <string>

namespace dave_mirra
{
    namespace gsmem
    {
        constexpr uint32_t kLevelAllocator = 0x00106EA8u;
        constexpr uint32_t kTextureCache = 0x0025CD20u;
        constexpr uint32_t kMode = 0x00259770u;         // 0 boot and movies, 1 front end, 2 a level (and its loading)
        constexpr uint32_t kSessionLevel = 0x002566CAu; // u8
        const char *const kLevels[] = {"SWAMPYTR", "TRAINYAR", "WOODWARD", "COMMERCI",
                                       "DEVILSPE", "CLOVERLE", "GALLOON",  "AIRPORT"};

        std::string levelName(const uint8_t *rdram)
        {
            uint32_t mode = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, kMode))
                std::memcpy(&mode, p, 4);
            if (mode != 2u)
                return "front end";
            const std::string extra = xboxlevel::chosenLevel();
            if (!extra.empty())
                return extra;
            const uint8_t *level = getConstMemPtr(rdram, kSessionLevel);
            const unsigned index = level ? *level : 0xFFu;
            return index < std::size(kLevels) ? std::string(kLevels[index]) : "level " + std::to_string(index);
        }
    }

    void installGsTextureAudit(PS2Runtime &runtime)
    {
        zx::gs_texture_audit::GameCache cache;
        cache.game = "DM2";
        cache.levelAllocator = gsmem::kLevelAllocator;
        cache.cacheState = gsmem::kTextureCache;
        zx::gs_texture_audit::install(runtime, cache, &gsmem::levelName);
    }
}
