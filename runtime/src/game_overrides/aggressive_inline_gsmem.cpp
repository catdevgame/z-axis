// Aggressive Inline (SLUS-20327): the GS texture memory audit
// (ZX_GSMEM_AUDIT=1, developers; runtime/ps2_gs_texture_audit.h).
//
// The texture cache's level allocator FUN_00107C88 (zge/render/texture.cpp
// placeTextureLevel) and its state 0x27A930 (TextureCache). The level is
// named by the game's own level-name list 0x265440 (u32 pointers, by the
// level index 0x24C9B4's low byte: 0 Civic ... 7 Tutorial, 8 Zoo, 9 Local,
// 10-13 the park editor's), with level 7 "Estate" while Estate is played.
#include "aggressive_inline_estate.h"
#include "aggressive_inline_gsmem.h"

#include "ps2_runtime.h"
#include "runtime/ps2_gs_texture_audit.h"
#include "runtime/ps2_memory.h"

#include <cstring>
#include <string>

namespace aggressive_inline
{
    namespace gsmem
    {
        constexpr uint32_t kLevelAllocator = 0x00107C88u;
        constexpr uint32_t kTextureCache = 0x0027A930u;
        constexpr uint32_t kGameMode = 0x00276FE4u;   // 1 the front end, 2 a level (3 its message boxes)
        constexpr uint32_t kLevelIndex = 0x0024C9B4u; // low byte
        constexpr uint32_t kLevelNames = 0x00265440u; // char *[14]
        constexpr uint32_t kLevelCount = 14u;

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        std::string levelName(const uint8_t *rdram)
        {
            const uint32_t mode = readU32(rdram, kGameMode);
            if (mode != 2u && mode != 3u)
                return "front end";
            const uint32_t index = readU32(rdram, kLevelIndex) & 0xFFu;
            if (index == 7u && estateActive())
                return "Estate";
            std::string name;
            if (index < kLevelCount)
                if (const uint32_t text = readU32(rdram, kLevelNames + 4u * index))
                    for (uint32_t i = 0; i < 24u; ++i)
                    {
                        const uint8_t *p = getConstMemPtr(rdram, text + i);
                        if (!p || *p < 0x20u || *p >= 0x7Fu)
                            break;
                        name += static_cast<char>(*p);
                    }
            return name.empty() ? "level " + std::to_string(index) : name;
        }
    }

    void installGsTextureAudit(PS2Runtime &runtime)
    {
        zx::gs_texture_audit::GameCache cache;
        cache.game = "AI";
        cache.levelAllocator = gsmem::kLevelAllocator;
        cache.cacheState = gsmem::kTextureCache;
        zx::gs_texture_audit::install(runtime, cache, &gsmem::levelName);
    }
}
