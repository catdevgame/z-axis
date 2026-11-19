#pragma once

// What findLocators hands back (the scene points' module): a list of the
// scene points of a kind and a name, each in a record of the list's own pool.
// freeLocators gives the records back.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct TagRecord; // (zge/assets/tag_table.h)

    // A scene point (SDPT chunk; readSdptChunk); a path's points (follower.cpp) go on from it.
    // ZGE3 and ZGE4 keep its position first in 0x30 bytes, the index and the
    // tags' size in halfwords; ZGE2 (DM2) keeps the hashes first in 0x40
    // bytes, the index and the tags' size in words.
    struct ScenePoint
    {
#if PS2X_GAME_DM2
        uint32_t kindHash;       // DM2 +0x00 what findLocators matches (a spawn point: its entity type)
        uint32_t nameHash;       // DM2 +0x04
        uint32_t index;          // DM2 +0x08 findScenePoint's third key
        uint32_t tagBytes;       // DM2 +0x0C the point's tags
        ee::Ptr<TagRecord> tags; // DM2 +0x10 its tag record, 0: none
        uint8_t _14[0x0C];       // (the position's quadword alignment)
        Vec4 position;           // DM2 +0x20 (w 1)
        Quaternion orientation;  // DM2 +0x30
#else
        Vec4 position;          // +0x00 (w 1)
        Quaternion orientation; // +0x10 (a spawn point's: setSpawnOrientation)
        uint32_t kindHash;      // +0x20 what findLocators matches (a spawn point: its entity type)
        uint32_t nameHash;      // +0x24
        uint16_t index;         // +0x28 findScenePoint's third key
        uint16_t tagBytes;      // +0x2A the point's tags (spawnNpcModel)
        ee::Ptr<TagRecord> tags; // +0x2C its tag record (tagByte, tagFloat and the rest read it), 0: none
#endif
    };
    static_assert(offsetof(ScenePoint, position) == game::readable::perGame(0x00u, 0x20u, 0x00u) &&
                  offsetof(ScenePoint, orientation) == game::readable::perGame(0x10u, 0x30u, 0x10u) &&
                  offsetof(ScenePoint, kindHash) == game::readable::perGame(0x20u, 0x00u, 0x20u) &&
                  offsetof(ScenePoint, index) == game::readable::perGame(0x28u, 0x08u, 0x28u) &&
                  offsetof(ScenePoint, tagBytes) == game::readable::perGame(0x2Au, 0x0Cu, 0x2Au) &&
                  offsetof(ScenePoint, tags) == game::readable::perGame(0x2Cu, 0x10u, 0x2Cu) &&
                  sizeof(ScenePoint) == game::readable::perGame(0x30u, 0x40u, 0x30u));

    // The fixed part of an SDPT chunk (a scene point; readSdptChunk); what
    // follows it in the chunk, up to the chunk's size, is the point's tag
    // data. ZGE2 reads the index as a word.
    struct SdptChunk
    {
        uint32_t headerBytes; // +0x00 this part's size
        uint32_t kindHash;    // +0x04
        uint32_t nameHash;    // +0x08
#if PS2X_GAME_DM2
        uint32_t index;       // DM2 +0x0C
#else
        uint16_t index;       // +0x0C
        uint16_t _0e;
#endif
        Vec3 position;        // +0x10
        ee::Float orientation[4]; // +0x1C a quaternion
    };
    static_assert(offsetof(SdptChunk, position) == 0x10 && sizeof(SdptChunk) == 44);

    struct Locator
    {
        ee::Ptr<Locator> next; // +0x00
        ee::Ptr<Locator> prev; // +0x04
        uint32_t point;        // +0x08 the scene point
    };
    static_assert(offsetof(Locator, point) == 0x08);

    struct LocatorList
    {
        ee::Ptr<Locator> head; // +0x00
        ee::Ptr<Locator> tail; // +0x04
        int32_t count;         // +0x08
        ee::Ptr<Pool> pool;    // +0x0C (findLocators keeps its records in the module's g_locatorPool, not here)
    };
    static_assert(sizeof(LocatorList) == 0x10);
}

namespace zge
{
    // (scene_points.cpp)
    uint32_t leaveModelScenes();
    uint32_t registerScenePointChunkHandlers();
    uint32_t shutdownScenePoints();
    ee::Ptr<ScenePoint> findScenePoint(uint32_t kindHash, uint32_t nameHash, uint32_t index);
    uint32_t findLocators(ee::Address listAddress, uint32_t kindHash, uint32_t nameHash);
    void freeLocators(ee::Address listAddress);
    // A copy of the point (and of its tags) added to the level's scene
    // points, as the next index of its kind and name.
    void addScenePoint(ee::Ptr<ScenePoint> source);
}
