// BMX XXX (SLUS-20415): the GS texture memory audit (PS2X_GSMEM_AUDIT=1,
// developers; runtime/ps2_gs_texture_audit.h), as Aggressive Inline's
// (aggressive_inline_gsmem.cpp) and DM2's (dave_mirra_gsmem.cpp).
//
// The texture cache is the engine's (zge/render/texture.cpp): its level
// allocator placeTextureLevel and its state g_textureCache (TextureCache: low
// cursor +0x14, high +0x18), the same code as AI's and DM2's. The level: the
// game's level table (game/bmx/profile/players.h LevelInfo, g_levels: its
// files' name at +4) at g_currentLevel, while the game's mode is a level's
// (g_gameMode: 2 a level and its loading, 3 paused); level 12 is the park
// editor's park (game/bmx/action/level_state.cpp kParkLevel), "PARK".
#include "bmx_xxx.h"

#include "ps2_runtime.h"
#include "runtime/ps2_gs_texture_audit.h"
#include "runtime/ps2_memory.h"
#include "sym/game_overrides/bmx_xxx_gsmem.cpp.h"

#include <cstring>
#include <string>

namespace bmx_xxx
{
    namespace gsmem
    {
        constexpr uint32_t kModeLevel = 2u;  // g_gameMode: a level (and its loading)
        constexpr uint32_t kModePaused = 3u; // ... paused
        constexpr uint32_t kLevelCount = 12u;
        constexpr uint32_t kParkLevel = 12u; // the park editor's park: no row of its own
        constexpr uint32_t kLevelInfoBytes = 0x10u;
        constexpr uint32_t kLevelInfoName = 0x04u;
        constexpr uint32_t kNameMost = 24u;

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        std::string levelName(const uint8_t *rdram)
        {
            const uint32_t mode = readU32(rdram, sym::bmx::g_gameMode);
            if (mode != kModeLevel && mode != kModePaused)
                return "front end";
            const uint8_t *levelByte = getConstMemPtr(rdram, sym::bmx::g_currentLevel);
            const uint32_t level = levelByte ? *levelByte : 0xFFu;
            if (level == kParkLevel)
                return "PARK";
            std::string name;
            if (level < kLevelCount)
                if (const uint32_t text = readU32(rdram, sym::bmx::g_levels + level * kLevelInfoBytes + kLevelInfoName))
                    for (uint32_t i = 0; i < kNameMost; ++i)
                    {
                        const uint8_t *p = getConstMemPtr(rdram, text + i);
                        if (!p || *p < 0x20u || *p >= 0x7Fu)
                            break;
                        name += static_cast<char>(*p);
                    }
            return name.empty() ? "level " + std::to_string(level) : name;
        }
    }

    void installGsTextureAudit(PS2Runtime &runtime)
    {
        ps2x::gs_texture_audit::GameCache cache;
        cache.game = "BMX";
        cache.levelAllocator = sym::bmx::placeTextureLevel;
        cache.cacheState = sym::bmx::g_textureCache;
        ps2x::gs_texture_audit::install(runtime, cache, &gsmem::levelName);
    }
}
