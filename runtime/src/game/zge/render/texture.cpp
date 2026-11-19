#include "texture.h"

#include "ee/mirror.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/memory/alignment.h"
#include "zge/text/cstring.h"
#include "zge/scene/camera.h"

#include <cstddef>
#include "sym/zge/render/texture.cpp.h"

namespace zge
{
    namespace
    {
        // The words initTextureCache gives as the pool's bottom before a level's (ZGE2 sets it; ZGE3 passes it).
        constexpr uint32_t kInitialPoolBottom = 0x54000u;
        constexpr uint32_t kListAppend = sym::pointerArrayAdd;  // (list, item)

        // The TEX0 every material using this texture gets: level 0's base,
        // the texture's format and size, RGBA, modulate, its CLUT, and "load
        // the CLUT buffer at every draw" (the CLUT may have moved since).
        uint64_t textureTex0(const GsTexture &texture, uint32_t clutGsAddress)
        {
            gs::Tex0 tex0;
            tex0.tbp0 = gs::blockOf(texture.levels[0].gsAddress);
            tex0.tbw = texture.levels[0].bufferWidth;
            tex0.psm = texture.psm;
            tex0.tw = texture.log2Width;
            tex0.th = texture.log2Height;
            tex0.tcc = 1;
            tex0.cbp = gs::blockOf(clutGsAddress);
            tex0.cpsm = texture.clutPsm;
            tex0.cld = 1;
            return tex0.bits();
        }

        // Where mip levels 1-3 are (only those reach the GS registers).
        uint64_t textureMipTbp1(const GsTexture &texture)
        {
            gs::MipTbp1 mip;
            mip.tbp1 = gs::blockOf(texture.levels[1].gsAddress);
            mip.tbw1 = texture.levels[1].bufferWidth;
            mip.tbp2 = gs::blockOf(texture.levels[2].gsAddress);
            mip.tbw2 = texture.levels[2].bufferWidth;
            mip.tbp3 = gs::blockOf(texture.levels[3].gsAddress);
            mip.tbw3 = texture.levels[3].bufferWidth;
            return mip.bits();
        }

        // Free space between the cursors, flagging a crossing. The space is
        // unsigned: once the cursors cross it wraps to a huge number, so the
        // "under 384 KB: flush" test (advanceTextureCache) no longer fires.
        void updateFreeSpace(TextureCache &cache, uint32_t after)
        {
            if (cache.high < cache.low)
                cache.overflow = 1;
            cache.freeBytes = (cache.high - cache.low) * 4u;
        }
    }

    void placeTextureLevel(ee::Ptr<GsTexture> texture, ee::Ptr<TextureLevel> level)
    {
        namespace here = points::placeTextureLevel;
        TextureCache &cache = *g_textureCache;

        // Levels of one format pack at block granularity; a level in
        // another format starts on a new page (the GS swizzles each format
        // differently within a page).
        uint32_t address;
        if (cache.lastPsm == texture->psm)
            address = roundUp(cache.low, gs::kWordsPerBlock);
        else
        {
            address = roundUp(cache.low, gs::kWordsPerPage);
            cache.lastPsm = texture->psm;
        }
        level->gsAddress = address;
        cache.low = address + level->gsWords;
        updateFreeSpace(cache, here::taken3);

        // The first level placed puts the texture on the resident list, so
        // a flush can take its GS memory back.
        if ((texture->cacheFlags & 1u) == 0)
        {
            texture->cacheFlags |= 1u;
            pointerArrayAdd(cache.residentList, texture);
        }

        texture->tex0 = textureTex0(*texture, texture->clutGsAddress);
        texture->miptbp1 = textureMipTbp1(*texture);
    }
    // The VIF1 handler hands the texture over through the uncached mirror
    // of its address.
    READABLE_FUNCTION(placeTextureLevel, Verified);

    void placeTextureClut(ee::Ptr<GsTexture> texture)
    {
        namespace here = points::placeTextureClut;
        TextureCache &cache = *g_textureCache;
        cache.high -= kClutWords;
        updateFreeSpace(cache, here::taken);
        texture->clutGsAddress = cache.high;
        texture->tex0 = textureTex0(*texture, cache.high);
    }
    READABLE_FUNCTION(placeTextureClut, Verified);

    void resetTextureCache(uint32_t poolBottom)
    {
        TextureCache &cache = *g_textureCache;
        cache.poolBottom = poolBottom;
        cache.freeBytes = (gs::kMemoryWords - poolBottom) * 4u;
        cache.poolTop = gs::kMemoryWords;
        cache.low = poolBottom;
        cache.high = gs::kMemoryWords;
    }
    READABLE_FUNCTION(resetTextureCache, Verified);

    // ---- the cache's set-up, flushes and evictions -----------------------------

    namespace
    {
        // A texture epoch: bumped when a flush is requested; the resident
        // textures are surveyed once an epoch (surveyResidentTextures).
        inline ee::Global<uint32_t, sym::g_textureEpoch> g_textureEpoch;
        inline ee::Global<uint32_t, sym::g_surveyedTextureEpoch> g_surveyedTextureEpoch;
        // 1 from advanceTextureCache's low-space request to its flush.
        inline ee::Global<uint32_t, sym::g_textureFlushPending> g_textureFlushPending;
        inline ee::Global<ee::Ptr<Camera>, sym::g_currentCamera> g_currentCamera;
        // ZGE2's frame statistics keep the cache's free bytes, which its debug line shows as "FPS: %2.2f VramFree:%d"
        // (in KB); ZGE3 has no such record.
        inline ee::Global<int32_t, sym::dm2::g_statsVramFree> g_statsVramFree;

        constexpr uint32_t kTextureCacheAddress = sym::g_textureCache;
        // The upper half of the cache's address, which the original keeps in
        // a saved register (its callees save it).
        constexpr uint32_t kTextureCacheHigh = ee::high(sym::g_textureCache);
        constexpr uint32_t kLowSpaceBytes = 0x60000u;     // under 384 KB free: a flush is requested
        constexpr uint32_t kLowSpaceFlushFrames = 2;
        constexpr uint32_t kResidentListCapacity = 0x180;

        constexpr uint32_t kDisableInterrupts = sym::DIntr; // DIntr
        constexpr uint32_t kEnableInterrupts = sym::EIntr;  // EIntr
        constexpr uint32_t kFlushCache = sym::flushCache;        // FlushCache(0): the data cache written back

        // An area's flag (+0x0A bit 0): leaving it flushes the textures.
        constexpr uint16_t kAreaFlushesTextures = 1u << 0;
        constexpr uint32_t kAreaFlags = 0x0A; // an area record's 16-bit flags

        // A GS page's width and height in texels by format class (two tables,
        // class 0: none, 1 PSMCT32, 2 PSMCT24, 3 PSMCT16, 5 PSMT8,
        // 6 PSMT4).
        constexpr uint8_t kPageWidths[8] = {0, 64, 64, 64, 64, 128, 128, 0};
        constexpr uint8_t kPageHeights[8] = {0, 32, 32, 64, 64, 64, 128, 0};

        PointerArray &residentList() { return *g_textureCache->residentList; }
    }

    ee::Ptr<PointerArray> newCursorArray(uint32_t capacity); // container/array.cpp
    void freePointerArray(ee::Ptr<PointerArray> array);      // container/array.cpp

    namespace
    {
        // The texture's first two words, as the original stores them.
        uint64_t &flagsDoubleword(GsTexture &texture) { return *reinterpret_cast<uint64_t *>(&texture); }
    }

    // surveyResidentTextures: once an epoch, every resident texture's levels walked
    // (the other builds count what they hold; nothing is kept here: a
    // division by each absent level's bit is all that is left).
    void surveyResidentTextures()
    {
        if (*g_surveyedTextureEpoch == *g_textureEpoch)
            return;
        PointerArray &list = residentList();
        *g_surveyedTextureEpoch = *g_textureEpoch;
        uint32_t item = 0;
        if (list.count != 0)
        {
            list.cursor = 0;
            item = list.items[0];
        }
        else
            list.cursor = -1;
        if (item == 0)
            return;
        for (;;)
        {
            textureOfGsTexture(item);
            PointerArray &walked = residentList();
            uint32_t next = 0;
            walkNext(walked, next) != WalkStep::Item;
            item = next;
            if (item == 0)
                break;
        }
    }
    READABLE_FUNCTION(surveyResidentTextures, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void DIntr() { ee::game(kDisableInterrupts); }
        namespace tail
        {
        void EIntr() { ee::tailCall(kEnableInterrupts); }
        }
    }

    // flushTextureCache: every resident texture evicted (with interrupts off) and
    // both cursors back to the pool's ends.
    void flushTextureCache()
    {
        DIntr();
        if (g_textureCache->residentList)
        {
            uint32_t first = 0;
            for (bool evicting = false;;)
            {
                if (evicting)
                    evictGsTexture(ee::Ptr<GsTexture>(first), 1);
                evicting = true;
                if constexpr (game::readable::kIsDm2)
                    first = arrayFirst(g_textureCache->residentList); // (ZGE2 calls it; ZGE3 writes it in)
                else
                {
                    PointerArray &list = residentList();
                    if (list.count != 0)
                    {
                        list.cursor = 0;
                        first = list.items[0];
                    }
                    else
                    {
                        list.cursor = -1;
                        first = 0;
                    }
                }
                if (first == 0)
                    break;
            }
        }
        TextureCache &cache = *g_textureCache;
        cache.low = cache.poolBottom;
        cache.high = cache.poolTop;
        tail::EIntr(); // the original's tail call
    }
    READABLE_FUNCTION(flushTextureCache, Verified);

    // initTextureCache: the cache cleared and set up empty (its pool starts at
    // the top until a level gives it the space above the frame buffers),
    // the resident list made, a first epoch. 1.
    uint32_t initTextureCache()
    {
        memoryFill(kTextureCacheAddress, 0, static_cast<uint32_t>(sizeof(TextureCache)));
        g_textureCache->ready = 1;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 sets the pool's bounds only: the cursors and the free bytes stay 0 until a level resets the pool.
            g_textureCache->poolBottom = kInitialPoolBottom;
            g_textureCache->poolTop = gs::kMemoryWords;
        }
        else
            resetTextureCache(gs::kMemoryWords); // (the original passes kInitialPoolBottom too, which it does not read)
        *g_textureEpoch = 0;
        const ee::Ptr<PointerArray> list = newCursorArray(kResidentListCapacity);
        const uint32_t epoch = *g_textureEpoch;
        g_textureCache->residentList = list;
        *g_textureEpoch = epoch + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initTextureCache, Verified);

    // shutDownTextureCache: the resident list freed. 1.
    uint32_t shutDownTextureCache()
    {
        if (g_textureCache->ready == 0)
            return 1;
        freePointerArray(g_textureCache->residentList);
        g_textureCache->residentList = {};
        g_textureCache->ready = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownTextureCache, Verified);

    // requestTextureCacheFlush: a flush for the next frame (and a new epoch), unless one
    // is counting down already (a camera cut, a level's set-up, a movie). ZGE2 does not
    // look: its request always sets the countdown to 1 and starts a new epoch.
    void requestTextureCacheFlush()
    {
        TextureCache &cache = *g_textureCache;
        if (game::readable::kIsDm2 || cache.flushCountdown == 0)
        {
            const uint32_t epoch = *g_textureEpoch;
            cache.flushCountdown = 1;
            *g_textureEpoch = epoch + 1;
        }
    }
    READABLE_FUNCTION(requestTextureCacheFlush, Verified);

    // requestTextureCacheFlushThunk (ZGE4 only): a jump to requestTextureCacheFlush (the zgui's caller's).
    inline void requestTextureCacheFlushThunk()
    {
        game::tail(requestTextureCacheFlush);
    }
    READABLE_FUNCTION_ONLY(bmx, requestTextureCacheFlushThunk);

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
    }

    // advanceTextureCache: the frame's step: the residents surveyed after an
    // overflow, a flush in two frames requested when under 384 KB are free
    // (once until it is done), the countdown run and the flush (and the
    // data cache written back) when it ends.
    // ZGE2 keeps the free bytes for its statistics first and tests that copy, signed: the space before the survey,
    // and a cache past full (high below low) asks for a flush too.
    void advanceTextureCache()
    {
        TextureCache &cache = *g_textureCache;
        if constexpr (game::readable::kIsDm2)
            *g_statsVramFree = static_cast<int32_t>(cache.freeBytes);
        if (cache.overflow != 0)
        {
            cache.overflow = 0;
            surveyResidentTextures();
        }
        bool lowSpace = cache.freeBytes < kLowSpaceBytes;
        if constexpr (game::readable::kIsDm2)
            lowSpace = *g_statsVramFree < static_cast<int32_t>(kLowSpaceBytes);
        if (lowSpace)
        {
            if (*g_textureFlushPending == 0)
            {
                const uint32_t epoch = *g_textureEpoch;
                cache.flushCountdown = kLowSpaceFlushFrames;
                *g_textureFlushPending = 1;
                *g_textureEpoch = epoch + 1;
            }
        }
        const uint32_t countdown = cache.flushCountdown;
        if (countdown != 0)
        {
            cache.flushCountdown = countdown - 1;
            if (countdown - 1 == 0)
            {
                flushTextureCache();
                flushCache(0);
                *g_textureFlushPending = 0;
            }
        }
    }
    READABLE_FUNCTION(advanceTextureCache, Verified);

    // flushTexturesOnAreaExit: a flush requested when the current camera has left
    // `area` (the one it was in) and that area asks for it.
    void flushTexturesOnAreaExit(uint32_t area)
    {
        const uint32_t now = cameraArea(*g_currentCamera);
        if (now != area)
        {
            if (now != 0 && area == 0)
                return;
            if ((*ee::Ptr<uint16_t>(area + kAreaFlags) & kAreaFlushesTextures) != 0)
            {
                game::tail(zge::requestTextureCacheFlush); // the original's tail call
                return;
            }
        }
    }
    READABLE_FUNCTION(flushTexturesOnAreaExit, Verified);

    // resetTextureCacheAndFlush: the pool set up for a level and a flush
    // requested. ZGE2 sets the pool's two ends alone (no cursors or free
    // bytes).
    void resetTextureCacheAndFlush(uint32_t poolBottom)
    {
        if constexpr (game::readable::kIsDm2)
        {
            TextureCache &cache = *g_textureCache;
            cache.poolTop = gs::kMemoryWords;
            cache.poolBottom = poolBottom;
        }
        else
            resetTextureCache(poolBottom);
        game::tail(zge::requestTextureCacheFlush); // the original's tail call
    }
    READABLE_FUNCTION(resetTextureCacheAndFlush, Verified);

    // evictGsTexture: a texture's GS memory given up: every level and the
    // CLUT without a GS address and marked to upload again; with unlist,
    // off the resident list (its flag cleared through a doubleword store,
    // then pointerArrayRemove of its uncached address: the original's tail
    // call).
    void evictGsTexture(ee::Ptr<GsTexture> texture, uint32_t unlist)
    {
        GsTexture &gs = *texture;
        const ee::Ptr<GsTexture> listed = ee::uncachedMirror(texture);
        const uint32_t levels = gs.levelCount;
        if (levels != 0)
        {
            for (uint32_t level = 0;;)
            {
                gs.levels[level].gsAddress = 0;
                if (++level == levels)
                    break;
            }
        }
        gs.pendingUploads |= (1u << (gs.levelCount & 0x1Fu)) - 1u;
        if (gs.clutGsAddress != 0)
        {
            gs.pendingUploads |= kTextureUploadClut;
            gs.clutGsAddress = 0;
        }
        if (unlist != 0 && (gs.cacheFlags & 1u) != 0)
        {
            uint64_t &flagsAndPending = flagsDoubleword(gs);
            flagsAndPending &= ~1ull;
            game::tail(pointerArrayRemove, g_textureCache->residentList, listed);
            return;
        }
    }
    READABLE_FUNCTION(evictGsTexture, Verified);

    // gsLevelSize: the size a level takes in GS memory (width, height in
    // place): one smaller than its format's page in either direction (and
    // neither square nor twice as wide as high) takes the smaller of its
    // size padded to the page and its longer side squared.
    void gsLevelSize(uint32_t psm, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height)
    {
        uint32_t &w = *width;
        uint32_t &h = *height;
        const uint32_t levelWidth = w;
        const uint32_t levelHeight = h;
        if (levelWidth == levelHeight || levelWidth == levelHeight << 1)
            return;
        uint32_t formatClass = 0;
        if (psm < 21)
        {
            // The table by format; the rest: the default case.
            switch (psm)
            {
            case gs::kPsmCt32: formatClass = 1; break;
            case gs::kPsmCt24: formatClass = 2; break;
            case gs::kPsmCt16: formatClass = 3; break;
            case gs::kPsmT8: formatClass = 5; break;
            case gs::kPsmT4: formatClass = 6; break;
            default: break;
            }
        }
        const uint32_t pageWidth = kPageWidths[formatClass];
        const uint32_t pageHeight = kPageHeights[formatClass];
        const bool narrow = levelWidth < pageWidth;
        if (!narrow && levelHeight >= pageHeight)
            return;
        const uint32_t paddedWidth = narrow ? pageWidth : levelWidth;
        const uint32_t paddedHeight = levelHeight < pageHeight ? pageHeight : levelHeight;
        const uint32_t squareHeight = levelHeight < levelWidth ? levelWidth : levelHeight;
        const uint32_t squareWidth = levelWidth < levelHeight ? levelHeight : levelWidth;
        if (paddedWidth * paddedHeight < squareWidth * squareHeight)
        {
            w = paddedWidth;
            h = paddedHeight;
            return;
        }
        w = squareWidth;
        h = squareHeight;
    }
    READABLE_FUNCTION(gsLevelSize, Verified);
}
