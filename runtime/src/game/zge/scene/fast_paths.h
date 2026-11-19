#pragma once
// ZGE4's fast paths (zge/scene/fast_paths.cpp; BMX XXX only): paths read
// whole from a level's ZFPH, ZFPD and ZFPT chunks as keys in time (a time,
// a position and an orientation each), and their fast followers, each
// somewhere on its path by time, between two keys. The engine's own
// followers (follower.h) go by distance along a path of waypoints; a fast
// follower keeps the same kind of list on its path, by time.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct ChunkReader;    // (zge/assets/chunk_reader.h)
    struct TagRecord;      // (zge/assets/tag_table.h)
    struct TagTableHeader; // (zge/assets/tag_table_header.h)
    struct FastFollower;

    // A key of a fast path: where the path is at a time.
    struct FastPathKey
    {
        ee::Float time;         // +0x00 the keys go up in time
        Vec3 position;          // +0x04
        Quaternion orientation; // +0x10
    };
    static_assert(offsetof(FastPathKey, position) == 0x04 && offsetof(FastPathKey, orientation) == 0x10 &&
                  sizeof(FastPathKey) == 0x20);

    // A fast path (ZFPD and its ZFPT), one of the level's array of them.
    struct FastPath
    {
        uint32_t id;                         // +0x00 what findFastPath looks for
        uint32_t keyCount;                   // +0x04
        ee::Ptr<FastPathKey> keys;           // +0x08 its keys in the level's array of them
        ee::Ptr<FastFollower> firstFollower; // +0x0C its followers, the earliest first
        ee::Ptr<FastFollower> lastFollower;  // +0x10
        ee::Ptr<TagTableHeader> tags;        // +0x14 the ZFPT chunk's tag table, 0: none
    };
    static_assert(offsetof(FastPath, firstFollower) == 0x0C && offsetof(FastPath, tags) == 0x14 && sizeof(FastPath) == 0x18);

    // A fast follower: somewhere on its path by time, between the key it is
    // at and the next; its time runs at a rate, which its owner applies.
    struct FastFollower
    {
        ee::Ptr<FastPath> path;            // +0x00
        uint32_t endMode;                  // +0x04 kept for its owner: newPathFollower's end mode's place (a path man saves the one word for either kind)
        ee::Float rate;                    // +0x08 how fast its time runs (signed)
        ee::Float time;                    // +0x0C where it is on the path
        uint32_t key;                      // +0x10 the path's key at or before the time
        ee::Float placedTime;              // +0x14 its time when made and when its position was last asked for (its owner sets it when it wraps round)
        uint8_t _18[8];                    // Scaffolding (unnamed-field): the maker clears it, nothing here reads it
        Vec4 position;                     // +0x20 room for its owner (a path man's position)
        Quaternion orientation;            // +0x30 room for its owner (a path man's orientation)
        ee::Ptr<FastFollower> later;       // +0x40 the next on the path's list
        ee::Ptr<FastFollower> earlier;     // +0x44 the one before
        uint8_t _48[8];                    // Scaffolding (unnamed-field): the maker clears it, nothing here reads it
    };
    static_assert(offsetof(FastFollower, placedTime) == 0x14 && offsetof(FastFollower, position) == 0x20 &&
                  offsetof(FastFollower, orientation) == 0x30 && offsetof(FastFollower, later) == 0x40 &&
                  offsetof(FastFollower, earlier) == 0x44 && sizeof(FastFollower) == 0x50);

    // (fast_paths.cpp, BMX XXX only)
    uint32_t readFastPathHeaderChunk(ChunkReader *reader);
    uint32_t readFastPathChunk(ChunkReader *reader);
    uint32_t readFastPathTagsChunk(ChunkReader *reader);
    void initFastPaths();
    void freeFastPaths();
    void shutDownFastPaths();
    ee::Ptr<FastPath> findFastPath(uint32_t id);
    uint32_t fastPathId(ee::Ptr<FastPath> path);
    ee::Float fastPathEndTime(ee::Ptr<FastPath> path);
    ee::Ptr<TagRecord> fastPathTags(ee::Ptr<FastPath> path);

    uint32_t newFastFollowerRecord();
    void freeFastFollowerRecord(ee::Ptr<FastFollower> follower);
    void unlinkFastFollower(ee::Ptr<FastFollower> follower);
    void linkFastFollowerBeforeNext(ee::Ptr<FastFollower> follower, ee::Ptr<FastFollower> earlier);
    void linkFastFollowerAfterPrevious(ee::Ptr<FastFollower> follower, ee::Ptr<FastFollower> later);
    void addFastFollowerToList(ee::Ptr<FastFollower> follower);
    ee::Ptr<FastFollower> newFastFollower(ee::Ptr<FastPath> path, uint32_t endMode, ee::Float rate, ee::Float time);
    void deleteFastFollower(ee::Ptr<FastFollower> follower);
    void fastFollowerPosition(ee::Ptr<FastFollower> follower, ee::Ptr<Vec4> out); // (x, y and z)
    void fastFollowerOrientation(ee::Ptr<FastFollower> follower, ee::Ptr<Quaternion> out);
    void fastFollowerVelocity(ee::Ptr<FastFollower> follower, ee::Ptr<Vec4> out);
    uint32_t fastFollowerEndMode(ee::Ptr<FastFollower> follower);
    void setFastFollowerEndMode(ee::Ptr<FastFollower> follower, uint32_t endMode);
    ee::Float fastFollowerTime(ee::Ptr<FastFollower> follower);
    void setFastFollowerTime(ee::Ptr<FastFollower> follower, ee::Float time);
    ee::Float fastFollowerRate(ee::Ptr<FastFollower> follower);
    void setFastFollowerRate(ee::Ptr<FastFollower> follower, ee::Float rate);
    ee::Ptr<FastPath> fastFollowerPath(ee::Ptr<FastFollower> follower);
    void setFastFollowerPath(ee::Ptr<FastFollower> follower, ee::Ptr<FastPath> path);
}
