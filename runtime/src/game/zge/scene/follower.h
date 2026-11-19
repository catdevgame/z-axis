#pragma once
// Path followers (zge/scene/follower.cpp): the level's paths of waypoints and
// what moves along them.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Follower;   // (follower.cpp)
    struct PathRecord; // (follower.cpp)
    struct TagRecord;  // (zge/assets/tag_table.h)

    // A point of a path: its position first, its tag record at +0x2C (the
    // words: the next point +0, the previous +4, the distance along the
    // path +8).
    struct PathPoint
    {
#if PS2X_GAME_DM2
        // ZGE2's point keeps its hashes first and its position last: the kind and name hashes at +0x00 and +0x04
        // and the order word at +0x08 (a word; DM2's findScenePoint compares the three, loadWaypoints reads the
        // order), the tag record at +0x10 (pathPointTags), the position at +0x20 (followerPosition, followerHeading).
        uint32_t kindHash; // DM2 +0x00 what findLocators matches ("Waypoint")
        uint32_t nameHash; // DM2 +0x04
        uint32_t order;    // DM2 +0x08 a waypoint's place on its path
        uint8_t _0C[0x04];
        ee::Ptr<TagRecord> tags; // DM2 +0x10 the record
        uint8_t _14[0x0C];
        Vec4 position;     // DM2 +0x20
#else
        Vec4 position;     // +0x00
        uint8_t _10[0x10];
        uint32_t kindHash; // +0x20 what findLocators matches ("Waypoint")
        uint32_t nameHash; // +0x24
        uint16_t order;    // +0x28 a waypoint's place on its path
        uint8_t _2A[0x02];
        ee::Ptr<TagRecord> tags; // +0x2C the record
#endif
    };
    static_assert(offsetof(PathPoint, position) == game::readable::perGame(0x00u, 0x20u, 0x00u) &&
                  offsetof(PathPoint, kindHash) == game::readable::perGame(0x20u, 0x00u, 0x20u) &&
                  offsetof(PathPoint, order) == game::readable::perGame(0x28u, 0x08u, 0x28u) &&
                  offsetof(PathPoint, tags) == game::readable::perGame(0x2Cu, 0x10u, 0x2Cu));

    void followerPosition(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out);
    void followerHeading(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out);
    void followerOrientation(ee::Ptr<Follower> follower, ee::Ptr<const Vec3> up, ee::Ptr<Matrix> out);
    void setFollowerAlong(ee::Ptr<Follower> follower, ee::Float distance);
    int32_t loadPath(uint32_t nameHash);
    ee::Ptr<PathRecord> findNearestPath(ee::Ptr<Vec4> point, ee::Ptr<ee::Float> along);
    int32_t deletePathFollower(ee::Ptr<Follower> follower);
    int32_t freePathFollower(ee::Ptr<Follower> follower); // (ZGE2's deletePathFollower: no unlink first)
    ee::Ptr<PathRecord> findPathRecord(uint32_t key);
    // The path's first waypoint (follower.cpp).
    ee::Ptr<PathPoint> pathFirstPoint(ee::Ptr<PathRecord> path);
    // The tag record of the path's first waypoint (follower.cpp).
    ee::Ptr<TagRecord> pathFirstPointTags(ee::Ptr<PathRecord> path);
    // What the follower does at an end of its path (FollowerEndMode; setFollowerEndMode sets it). ZGE has no
    // function of its own for it: the games (DM2's boats and traffic cars, BMX XXX's cars, camera objects and
    // pathman objects) call pathFirstPoint's code on the follower, which reads the same word (+0x0C) a path record
    // keeps its waypoints in.
    inline uint32_t followerEndMode(ee::Ptr<Follower> follower)
    {
        return pathFirstPoint(ee::Ptr<PathRecord>(follower.address)).address;
    }
    uint32_t loadLevelPaths();
    uint32_t clearPathRecords();
    ee::Float followerSpeed(ee::Ptr<Follower> follower);
    ee::Ptr<PathRecord> setFollowerSpeed(ee::Ptr<Follower> follower, ee::Float speed);
    ee::Float followerAlong(ee::Ptr<Follower> follower);
    ee::Ptr<PathRecord> followerSegment(ee::Ptr<Follower> follower);
    ee::Float segmentEnd(ee::Ptr<PathRecord> segment);
    // What the follower does at an end of its path (follower.cpp).
    void setFollowerEndMode(ee::Ptr<Follower> follower, uint32_t endMode);
    ee::Ptr<Follower> newPathFollower(ee::Ptr<PathRecord> path, uint32_t endMode, ee::Float speed, ee::Float along);
    int32_t advanceFollower(ee::Ptr<Follower> follower, ee::Float time);
    int32_t setFollowerSegment(ee::Ptr<Follower> follower, ee::Ptr<PathRecord> path);
    // What the follower does at an end of its path (FollowerEndMode).
    void setFollowerEndMode(ee::Ptr<Follower> follower, uint32_t endMode);
    ee::Ptr<PathRecord> segmentExit(ee::Ptr<PathRecord> path, uint32_t exit, uint32_t atStart);
    // 1 when `value` is one of the five words at 36 to 52 of the path's
    // first point's tag record (follower.cpp; DM2's traffic cars ask it
    // with a car's name hash).
    uint32_t pathStartTagsInclude(ee::Ptr<PathRecord> path, int32_t value);
    int32_t followerSnap(ee::Ptr<Follower> follower, ee::Ptr<Vec4> point);
    ee::Float followerDistance(ee::Ptr<Follower> follower);
    ee::Ptr<Follower> followerAhead(ee::Ptr<Follower> follower);
    ee::Ptr<Follower> followerBehind(ee::Ptr<Follower> follower);
    uint32_t pathNameHash(ee::Ptr<PathRecord> path);
    ee::Ptr<Follower> pathNearestFollower(ee::Ptr<PathRecord> path);
    uint32_t segmentJunction(ee::Ptr<PathRecord> path);
    void setSegmentJunction(ee::Ptr<PathRecord> path, uint32_t light);
}
