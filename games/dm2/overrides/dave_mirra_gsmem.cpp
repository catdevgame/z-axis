// Dave Mirra Freestyle BMX 2 (SLUS-20159): the GS texture memory audit
// (ZX_GSMEM_AUDIT=1, developers; runtime/gs_texture_audit.h).
//
// DM2's texture cache level allocator placeTextureLevel is Aggressive Inline's
// placeTextureLevel instruction for instruction, its state at 0x25CD20 AI's
// TextureCache (low cursor 0x25CD34, high 0x25CD38, overflow 0x25CD44;
// dave_mirra_xboxlevel.cpp's trace). The level: the session's level byte
// 0x2566CA (SCENE.TXT's order: 0 SWAMPYTR, 1 TRAINYAR, 2 WOODWARD, 3
// COMMERCI, 4 DEVILSPE, 5 CLOVERLE, 6 GALLOON, 7 AIRPORT), or the Xbox
// level served in Woodward Camp's slot (dave_mirra_xboxlevel.cpp).
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_gsmem.cpp.h"

#include "machine.h"
#include "runtime/gs_texture_audit.h"
#include "runtime/memory.h"

#include <cstring>
#include <iterator>
#include <string>

namespace dave_mirra
{
    namespace gsmem
    {
        constexpr uint32_t kLevelAllocator = sym::dm2::placeTextureLevel;
        constexpr uint32_t kTextureCache = sym::dm2::g_textureCache;
        constexpr uint32_t kMode = sym::dm2::g_gameMode;         // 0 boot and movies, 1 front end, 2 a level (and its loading)
        constexpr uint32_t kSessionLevel = sym::dm2::g_sessionLevel; // u8
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

    void installGsTextureAudit(PS2Machine &runtime)
    {
        zx::gs_texture_audit::GameCache cache;
        cache.game = "DM2";
        cache.levelAllocator = gsmem::kLevelAllocator;
        cache.cacheState = gsmem::kTextureCache;
        zx::gs_texture_audit::install(runtime, cache, &gsmem::levelName);
    }
}
