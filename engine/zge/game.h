#pragma once

// What the engine calls in the game. The engine declares these and calls them
// by name; each game defines its own: AI in games/aggressive (game_main.cpp,
// zfile_library.cpp, zone_memory.cpp), DM2 in games/dm2 and BMX XXX in games/bmx (game_main.cpp:
// their originals, by address, until each has its readable body). They are the
// engine's names, so each game defines them in the engine's namespace (its
// instance's: zge_dm2, zge_bmx). A game whose engine never makes one of these
// calls still defines it while the engine's body that makes it is shared.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/zfile_library.h"
#include "zge/memory/pool.h"

#include <cstdint>

namespace zge
{
    // The engine's main (zge/platform/boot.cpp, engine.cpp).
    // The game's start, before the main loop: nonzero to run.
    uint32_t gameStartup();
    // The picture shown while the engine loads (its file's name, width,
    // height and bits; a name of 0 shows none). AI's legal screen; DM2's and
    // BMX XXX's engines show none.
    void legalScreen(ee::Ptr<uint32_t> image, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height, ee::Ptr<uint32_t> depth);
    // The game's frame (seconds): the frame callback the engine is handed
    // (setFrameCallback, by its address).
    int32_t steadyPlayFrame(ee::Float seconds);
    // The game's end, after the main loop.
    uint32_t gameShutdown();

    // The packed libraries the scene database reads its animations from
    // (zge/scene/scene_database.cpp loadAnimationLibrary; the library's
    // format is zge/assets/zfile_library.h). AI and DM2 keep the library in
    // their own code.
    ee::Ptr<ZfileLibrary> openZfileLibrary(ee::Address path);
    void closeZfileLibrary(ee::Ptr<ZfileLibrary> library);
    uint32_t zfileLibraryGroupCount(ee::Ptr<ZfileLibrary> library);
    uint32_t nextZfileLibraryGroup(ee::Ptr<ZfileLibrary> library);
    uint32_t zfileLibraryAssetBytes(ee::Ptr<ZfileLibrary> library);
    uint32_t readZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, ee::Address dst);

    // The record pools, by the pool's word: AI's game wraps the engine's
    // (createPool, destroyPool, poolAlloc, poolFree) in these
    // (zone_memory.cpp), and AI's animator and carried objects call them.
    int32_t newPool(uint32_t recordBytes, uint32_t recordsABlock, uint32_t destroyRecord);
    int32_t freePool(uint32_t pool);
    int32_t poolTake(uint32_t pool);
    int32_t poolGive(uint32_t pool, ee::Address record);

    // The animator's (zge/scene/animator.cpp) and the carried objects'
    // (zge/scene/carried_objects.cpp) pools: through the game's wrappers in
    // AI; BMX XXX's engine calls the engine's pools itself.
    inline int32_t newRecordPool(uint32_t recordBytes, uint32_t recordsABlock, uint32_t destroyRecord)
    {
        if constexpr (game::readable::kIsBmx)
        {
            return static_cast<int32_t>(createPool(recordBytes, recordsABlock, destroyRecord));
        }
        else
        {
            return newPool(recordBytes, recordsABlock, destroyRecord);
        }
    }
    inline void freeRecordPool(uint32_t pool)
    {
        if constexpr (game::readable::kIsBmx)
        {
            destroyPool(ee::Ptr<Pool>(pool));
        }
        else
        {
            freePool(pool);
        }
    }
    inline uint32_t takeRecord(uint32_t pool)
    {
        if constexpr (game::readable::kIsBmx)
        {
            return poolAlloc(ee::Ptr<Pool>(pool));
        }
        else
        {
            return static_cast<uint32_t>(poolTake(pool));
        }
    }
    inline void giveRecord(uint32_t pool, ee::Address record)
    {
        if constexpr (game::readable::kIsBmx)
        {
            poolFree(ee::Ptr<Pool>(pool), record);
        }
        else
        {
            poolGive(pool, record);
        }
    }
}
