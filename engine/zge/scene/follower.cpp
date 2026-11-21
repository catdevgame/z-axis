// Path followers (block 228, visitLevelObjectOfType .. clearPathRecords; DM2 PS2 and
// BMX XXX PS2 word for word): something moving along a path of
// waypoints (the level's "Waypoint" objects, loadWaypoints) at a distance along
// it. The path is a cubic Bezier spline: its points run knot, handle,
// handle, knot ..., each linked to the next and the previous by its tag
// record, which also holds its distance along the path. A follower keeps
// the knots either side of its distance and caches its position and
// heading on the curve between them. The game's traffic cars are
// followers (kFollowerPosition's callers walkPickup, moveTrafficCar).

#include "zge/container/array.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/dispatch.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/container/tree.h"
#include "zge/math/curve.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "follower.h"
#include "locators.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/follower.cpp.h"

namespace zge
{

    enum : uint32_t
    {
        kPointNextOffset = 0,
        kPointPreviousOffset = 4,
        kPointDistanceOffset = 8,
        // A path's last point names the paths its exits (0 .. 2) lead to,
        // three words for leaving from the path's start, three for its end
        // (segmentExit).
        kPointExitsFromStart = 12,
        kPointExitsFromEnd = 24,
    };

    struct PathRecord;

    struct Follower
    {
        ee::Ptr<PathRecord> path;  // +0x00
        ee::Ptr<PathPoint> behind; // +0x04 the knot at or before the distance
        ee::Ptr<PathPoint> ahead;  // +0x08 the knot after it
        uint32_t endMode;          // +0x0C FollowerEndMode
        ee::Float speed;           // +0x10 its sign: the heading's direction
        ee::Float distance;        // +0x14 along the path
        Vec4 position;             // +0x20 the cached point on the curve
        uint32_t positionStale;    // +0x30
        Vec4 heading;              // +0x40 the cached direction
        uint32_t headingStale;     // +0x50
        // The path's followers, sorted by distance: its neighbours on that list.
        ee::Ptr<Follower> followerAhead;  // +0x54 the next further along
        ee::Ptr<Follower> followerBehind; // +0x58 the next nearer the start
    };
    static_assert(offsetof(Follower, distance) == 0x14 && offsetof(Follower, position) == 0x20);
    static_assert(offsetof(Follower, positionStale) == 0x30 && offsetof(Follower, heading) == 0x40);
    static_assert(offsetof(Follower, headingStale) == 0x50 && offsetof(Follower, followerBehind) == 0x58);
    static_assert(sizeof(Follower) == 0x60 && offsetof(Follower, endMode) == 0x0C);

    // What a follower does at an end of its path (advanceFollower); with
    // neither it stops there.
    enum FollowerEndMode : uint32_t
    {
        kFollowerTurnsBack = 1u << 0, // back from that end, its speed reversed
        kFollowerLoops = 1u << 1,     // on from the other end
    };

    // A path (0x20 bytes), in the tree of paths (loadPath) by its name.
    struct PathRecord
    {
        uint32_t nameHash;         // +0x00 the key
        uint32_t pointCount;       // +0x04
        ee::Float length;          // +0x08
        ee::Ptr<PathPoint> waypoints; // +0x0C
        ee::Ptr<PathPoint> lastPoint; // +0x10
        ee::Ptr<Follower> nearestFollower;  // +0x14 of the followers' list: the least distance
        ee::Ptr<Follower> furthestFollower; // +0x18
        uint32_t junction;         // +0x1C the traffic light at its end (DM2's TrafficLight objects), 0: none
    };
    static_assert(sizeof(PathRecord) == 0x20);
    struct PathNode
    {
        SearchTreeNode links; // +0x00 in the tree of paths
        PathRecord path;   // +0x08
    };
    static_assert(sizeof(PathNode) == 0x28);

    // loadWaypoints' tree of its points by order.
    struct WaypointEntry
    {
        SearchTreeNode links; // +0x00 in the tree
        uint32_t order;    // +0x08 the key
        ee::Ptr<PathPoint> point; // +0x0C
    };
    static_assert(sizeof(WaypointEntry) == 0x10);
    struct PointChain
    {
        ee::Ptr<PathPoint> first, last;
    };

    // What nearestPathVisitor keeps for a caller: the place is first.
    struct NearestPathSearch
    {
        Vec4 point;        // +0x00
        ee::Ptr<PathRecord> path; // +0x10 0: none yet
        ee::Float distance; // +0x14
        ee::Float along;   // +0x18
    };
    static_assert(offsetof(NearestPathSearch, along) == 0x18);

    ee::Ptr<TagRecord> pathPointTags(ee::Ptr<PathPoint> point);
    ee::Ptr<PathRecord> addPathRecord(uint32_t nameHash);
    ee::Ptr<PathPoint> loadWaypoints(uint32_t nameHash);
    ee::Ptr<PathPoint> lastPathPoint(ee::Ptr<PathPoint> point);
    ee::Float measurePathPoints(ee::Ptr<PathPoint> path);
    int32_t pathPointCount(ee::Ptr<PathPoint> point);
    void setPathPointNext(ee::Ptr<PathPoint> point, ee::Address next);
    void setPathPointPrevious(ee::Ptr<PathPoint> point, ee::Address previous);
    READABLE_RESULT_EXCEPT(bmx, int32_t) setPathPointDistance(ee::Ptr<PathPoint> point, ee::Float distance);
    uint32_t nextPathPoint(ee::Ptr<PathPoint> point);
    uint32_t previousPathPoint(ee::Ptr<PathPoint> point);
    ee::Float pathPointDistance(ee::Ptr<PathPoint> point);
    void followerPosition(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out);
    void followerHeading(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out);
    void setFollowerAlong(ee::Ptr<Follower> follower, ee::Float distance);

    namespace
    {
        constexpr uint32_t kPaths = game::readable::perGame(0x80u, 0x40u); // (DM2 has half as many paths)
        constexpr uint32_t kPathsUsed = sym::pathsUsed;     // a word each
        constexpr uint32_t kPathNodes = sym::pathNodes;
        inline ee::Global<SearchTree, sym::g_paths> g_paths;
        inline ee::Global<ee::Ptr<Pool>, sym::g_followerPool> g_followerPool;
        constexpr uint32_t kFollowersABlock = game::readable::perGame(0x20u, 0x20u, 0x30u);
        inline ee::Global<ee::Float, sym::g_farFromPath> g_farFromPath; // the largest float
        constexpr uint32_t kTextWaypoint = sym::g_textWaypoint;   // "Waypoint"

        constexpr uint32_t kPathPointTag = 8, kTagNext = 0, kTagPrevious = 4, kTagDistance = 8; // setTagWord's

        constexpr uint32_t kCompareKeys = sym::lessWord;          // (the tree's less)
        constexpr uint32_t kLinkPathPoint = sym::linkPathPoint;

    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
            // setFollowerAlong (void), by address: advanceFollower and followerSnap return
            // what it leaves in the result register.
            int32_t setFollowerAlongKeepingResult(ee::Ptr<Follower> follower, ee::Float along) { return ee::tailCall<int32_t>(sym::setFollowerAlong, follower, along); }
        }
        // The same as a call (ZGE2's followerSnap calls it, then returns what it left).
        int32_t setFollowerAlongKeepingResult(ee::Ptr<Follower> follower, ee::Float along) { return ee::game<int32_t>(sym::setFollowerAlong, follower, along); }
    }

    void placeFollower(ee::Ptr<Follower> follower); // (below)

    // pathPointTags: a point's tag record.
    ee::Ptr<TagRecord> pathPointTags(ee::Ptr<PathPoint> point) { return point->tags; }
    READABLE_FUNCTION(pathPointTags, Verified);

    // pathFirstPoint: the path's first waypoint (nothing calls it in AI).
    ee::Ptr<PathPoint> pathFirstPoint(ee::Ptr<PathRecord> path)
    {
        return path->waypoints;
    }
    READABLE_FUNCTION(pathFirstPoint, Draft);

    // pathFirstPointTags: the tag record of the path's first waypoint
    // (nothing calls it in AI).
    ee::Ptr<TagRecord> pathFirstPointTags(ee::Ptr<PathRecord> path)
    {
        return pathPointTags(path->waypoints);
    }
    READABLE_FUNCTION(pathFirstPointTags, Draft);

    // nextPathPoint: the next point along the path.
    uint32_t nextPathPoint(ee::Ptr<PathPoint> point)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        return static_cast<uint32_t>(tagWord(tags, 8u, uint32_t(kPointNextOffset)));
    }
    READABLE_FUNCTION(nextPathPoint, Verified);

    // previousPathPoint: the previous point along the path.
    uint32_t previousPathPoint(ee::Ptr<PathPoint> point)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        return static_cast<uint32_t>(tagWord(tags, 8u, uint32_t(kPointPreviousOffset)));
    }
    READABLE_FUNCTION(previousPathPoint, Verified);

    // pathPointDistance: a point's distance along the path.
    ee::Float pathPointDistance(ee::Ptr<PathPoint> point)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        return tagFloat(tags, 8u, uint32_t(kPointDistanceOffset));
    }
    READABLE_FUNCTION(pathPointDistance, Verified);

    // pathStartTagsInclude: 1 when `value` is one of the five words at 36 to
    // 52 of the path's first point's tag record, else 0. (Nothing in AI
    // calls this; DM2's traffic cars ask it with a car's name hash at a
    // junction, and take the exit whose start names the car.)
    uint32_t pathStartTagsInclude(ee::Ptr<PathRecord> path, int32_t value)
    {
        constexpr uint32_t kFirstWord = 36, kWords = 5;
        const ee::Ptr<TagRecord> tags = pathPointTags(path->waypoints);
        for (uint32_t word = 0; word < kWords; ++word)
        {
            if (tagWord(tags, kPathPointTag, kFirstWord + word * sizeof(uint32_t)) == value)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(pathStartTagsInclude, Draft);

    namespace
    {
        // The Bezier segment a follower is on: the knot behind, its handle,
        // the handle before the knot ahead, the knot ahead.
        void segmentCurve(ee::Ptr<Follower> follower, Vec4 *curve)
        {
            curve[0] = follower->behind->position;
            curve[1] = ee::Ptr<PathPoint>(nextPathPoint(follower->behind))->position;
            curve[2] = ee::Ptr<PathPoint>(previousPathPoint(follower->ahead))->position;
            curve[3] = follower->ahead->position;
        }

        // How far along that segment the follower is, 0 to 1.
        ee::Float followerSegmentFraction(ee::Ptr<Follower> follower)
        {
            const ee::Float from = pathPointDistance(follower->behind);
            const ee::Float to = pathPointDistance(follower->ahead);
            return (follower->distance - from) / (to - from);
        }
    }

    // followerPosition: the follower's point on the curve, worked out again
    // when stale.
    void followerPosition(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out)
    {
        ee::Local<Vec4[4]> curve;
        if (follower->positionStale)
        {
            segmentCurve(follower, &curve[0]);
            bezierPoint(followerSegmentFraction(follower), curve, &follower->position.xyz());
            follower->positionStale = 0;
        }
        aligned(out) = follower->position;
    }
    READABLE_FUNCTION(followerPosition, Verified);

    // followerHeading: the follower's direction along the curve (its tangent
    // there, turned round when its speed is negative), worked out again when
    // stale; not unit length.
    void followerHeading(ee::Ptr<Follower> follower, ee::Ptr<Vec4> out)
    {
        ee::Local<Vec4[4]> curve;
        if (follower->headingStale)
        {
            Vec3 *heading = &follower->heading.xyz();
            segmentCurve(follower, &curve[0]);
            bezierTangent(followerSegmentFraction(follower), curve, heading);
            if (follower->speed < 0.0f)
                vec3Negate(heading, heading);
            follower->headingStale = 0;
        }
        aligned(out) = follower->heading;
    }
    READABLE_FUNCTION(followerHeading, Verified);

    // followerOrientation: the follower's matrix: its x axis across the path
    // (up x heading, both negated), its y axis square to the heading in the
    // plane with up, its z axis the heading turned round, all unit length;
    // its translation the follower's position.
    void followerOrientation(ee::Ptr<Follower> follower, ee::Ptr<const Vec3> up, ee::Ptr<Matrix> out)
    {
        ee::Local<Vec4> x;
        ee::Local<Vec4> y;
        ee::Local<Vec4> z;
        ee::Local<Vec4> position;
        ee::Local<Vec4> down;
        followerHeading(follower, z);
        followerPosition(follower, position);
        // (ZGE2's z axis is the heading itself and its x axis up x heading; ZGE3 turns both round)
        if constexpr (game::readable::kIsDm2)
            x = cross(quadwordOf(up), *z);
        else
        {
            vec3Negate(&z->xyz(), &z->xyz());
            vec3Negate(&down->xyz(), up);
            x = cross(*down, *z);
        }
        y = cross(*z, *x);
        z = unit(*z);
        y = unit(*y);
        x = unit(*x);
        matrixIdentity(out);
        out->row(2) = quadwordOf(ee::Ptr<Vec4>(z));
        out->row(1) = quadwordOf(ee::Ptr<Vec4>(y));
        out->row(0) = quadwordOf(ee::Ptr<Vec4>(x));
        out->row(3) = quadwordOf(ee::Ptr<Vec4>(position));
    }
    READABLE_FUNCTION(followerOrientation, Verified);

    namespace
    {
        // The knots either side of one: two handles away.
        ee::Ptr<PathPoint> knotBefore(ee::Ptr<PathPoint> knot)
        {
            const ee::Ptr<PathPoint> handle(previousPathPoint(knot));
            const ee::Ptr<PathPoint> otherHandle(previousPathPoint(handle));
            return ee::Ptr<PathPoint>(previousPathPoint(otherHandle));
        }
        ee::Ptr<PathPoint> knotAfter(ee::Ptr<PathPoint> knot)
        {
            const ee::Ptr<PathPoint> handle(nextPathPoint(knot));
            const ee::Ptr<PathPoint> otherHandle(nextPathPoint(handle));
            return ee::Ptr<PathPoint>(nextPathPoint(otherHandle));
        }
    }

    // setFollowerAlong: the follower put at a distance along the path: its
    // knots stepped back or on (three points a knot) until they hold it, its
    // position and heading marked stale, its place in the path's list updated.
    void setFollowerAlong(ee::Ptr<Follower> follower, ee::Float distance)
    {
        if (distance < pathPointDistance(follower->behind))
        {
            ee::Ptr<PathPoint> knot = follower->behind;
            ee::Ptr<PathPoint> previous = knotBefore(knot);
            while (distance < pathPointDistance(previous))
            {
                knot = previous;
                previous = knotBefore(knot);
            }
            follower->behind = previous;
            follower->ahead = knot;
        }
        else if (pathPointDistance(follower->ahead) < distance)
        {
            ee::Ptr<PathPoint> knot = follower->ahead;
            ee::Ptr<PathPoint> next = knotAfter(knot);
            while (pathPointDistance(next) < distance)
            {
                knot = next;
                next = knotAfter(knot);
            }
            follower->ahead = next;
            follower->behind = knot;
        }
        follower->distance = distance;
        follower->headingStale = 1;
        follower->positionStale = 1;
        game::tail(placeFollower, follower);
    }
    READABLE_FUNCTION(setFollowerAlong, Verified);

    // addPathRecord: a free path of the kPaths, in the tree of paths by its name.
    // 0 if none is free or the name is there already.
    ee::Ptr<PathRecord> addPathRecord(uint32_t nameHash)
    {
        const ee::Ptr<PathNode> nodes(kPathNodes);
        const ee::Ptr<uint32_t> used(kPathsUsed);
        for (uint32_t slot = 0; slot < kPaths; ++slot)
        {
            if (used[slot] != 0)
                continue;
            used[slot] = 1;
            const ee::Ptr<PathNode> node = nodes + slot;
            node->path.nameHash = nameHash;
            if (searchTreeInsert(g_paths.get(), &node->links) == 0)
                break;
            return &node->path;
        }
        return ee::Ptr<PathRecord>(0u);
    }
    READABLE_FUNCTION_WAITS(addPathRecord, Verified);

    // loadPath: the level's waypoints of a name as a path. The count.
    int32_t loadPath(uint32_t nameHash)
    {
        const ee::Ptr<PathRecord> path(addPathRecord(nameHash));
        path->waypoints = loadWaypoints(nameHash);
        path->lastPoint = lastPathPoint(path->waypoints);
        path->length = measurePathPoints(path->waypoints);
        const int32_t count = pathPointCount(path->waypoints);
        path->pointCount = static_cast<uint32_t>(count);
        path->junction = 0;
        path->nearestFollower = ee::Ptr<Follower>(0u);
        path->furthestFollower = ee::Ptr<Follower>(0u);
        return count;
    }
    READABLE_FUNCTION_WAITS(loadPath, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void copyControlPoint(Vec4 &to, ee::Ptr<PathPoint> point) { to = point->position; }
    }

    // measurePathPoints: every knot's distance along the path, by the length of the
    // Bezier segment before it, then each as a fraction of the whole (the
    // first 0, the last 1). Returns the whole. The handles keep their
    // distances from the first pass.
    ee::Float measurePathPoints(ee::Ptr<PathPoint> path)
    {
        ee::Local<Vec4[4]> controlPoints;
        ee::Ptr<PathPoint> point = path;
        ee::Float length(0.0f);
        while (nextPathPoint(point) != 0)
        {
            copyControlPoint(controlPoints[0], point);
            setPathPointDistance(point, length);
            for (uint32_t i = 1; i < 4; ++i)
            {
                point = ee::Ptr<PathPoint>(nextPathPoint(point));
                copyControlPoint(controlPoints[i], point);
            }
            length = length + bezierLengthDefault(controlPoints);
            setPathPointDistance(point, length);
        }
        point = path;
        setPathPointDistance(point, ee::Float(0.0f));
        while (nextPathPoint(point) != 0)
        {
            const ee::Ptr<PathPoint> handle(nextPathPoint(point));
            const ee::Ptr<PathPoint> secondHandle(nextPathPoint(handle));
            point = ee::Ptr<PathPoint>(nextPathPoint(secondHandle));
            const ee::Float distance = pathPointDistance(point);
            setPathPointDistance(point, distance / length);
        }
        setPathPointDistance(point, ee::Float(1.0f));
        return length;
    }
    READABLE_FUNCTION(measurePathPoints, Verified);

    // loadWaypoints: the level's "Waypoint" locators of a name, sorted by their
    // order number through a tree, as a chain. Returns its first point.
    ee::Ptr<PathPoint> loadWaypoints(uint32_t nameHash)
    {
        ee::Local<PointChain> chain;
        ee::Local<SearchTree> sorted;
        ee::Local<LocatorList> locators;
        const uint32_t kindHash = zge::nameHash(ee::Ptr<const char>(kTextWaypoint));
        const uint32_t found = findLocators(locators, kindHash, nameHash);
        const uint32_t memory = allocateMemory(found * sizeof(WaypointEntry));
        initSearchTree(sorted, kCompareKeys);
        ee::Ptr<WaypointEntry> entry(memory);
        for (ee::Ptr<Locator> locator = locators->head; locator; locator = locator->next, entry = entry + 1)
        {
            entry->point = ee::Ptr<PathPoint>(locator->point);
            entry->order = ee::Ptr<PathPoint>(locator->point)->order;
            searchTreeInsert(sorted, &entry->links);
        }
        chain->first = ee::Ptr<PathPoint>(0u);
        searchTreeForEach(sorted, kLinkPathPoint, chain);
        freeMemory(memory);
        freeLocators(locators);
        return chain->first;
    }
    READABLE_FUNCTION_WAITS(loadWaypoints, Verified);

    // lastPathPoint
    ee::Ptr<PathPoint> lastPathPoint(ee::Ptr<PathPoint> point)
    {
        while (nextPathPoint(point) != 0)
            point = ee::Ptr<PathPoint>(nextPathPoint(point));
        return point;
    }
    READABLE_FUNCTION(lastPathPoint, Verified);

    // pathPointCount
    int32_t pathPointCount(ee::Ptr<PathPoint> point)
    {
        int32_t count = 0;
        if (point)
        {
            for (;;)
            {
                ++count;
                point = ee::Ptr<PathPoint>(nextPathPoint(point));
                if (point)
                    continue;
                break;
            }
        }
        return count;
    }
    READABLE_FUNCTION(pathPointCount, Verified);

    // linkPathPoint: a search tree visitor: the entry's point at the end of the
    // chain.
    uint32_t linkPathPoint(ee::Ptr<WaypointEntry> entry, ee::Ptr<PointChain> chain)
    {
        const ee::Ptr<PathPoint> point = entry->point;
        if (!chain->first)
        {
            chain->first = point;
            chain->last = point;
            setPathPointPrevious(point, 0);
            return 1;
        }
        setPathPointNext(chain->last, point);
        setPathPointPrevious(point, chain->last);
        setPathPointNext(point, 0);
        chain->last = point;
        return 1;
    }
    READABLE_FUNCTION(linkPathPoint, Verified);

    // setPathPointNext, setPathPointPrevious, setPathPointDistance: the point's tag's words.
    void setPathPointNext(ee::Ptr<PathPoint> point, ee::Address next)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        game::tail(zge::setTagWord, tags, kPathPointTag, kTagNext, next);
    }
    READABLE_FUNCTION(setPathPointNext, Verified);

    void setPathPointPrevious(ee::Ptr<PathPoint> point, ee::Address previous)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        game::tail(zge::setTagWord, tags, kPathPointTag, kTagPrevious, previous);
    }
    READABLE_FUNCTION(setPathPointPrevious, Verified);

    // (ZGE4's setTagFloat answers nothing, so neither does its setPathPointDistance, the same tail jump.)
    READABLE_RESULT_EXCEPT(bmx, int32_t) setPathPointDistance(ee::Ptr<PathPoint> point, ee::Float distance)
    {
        const ee::Ptr<TagRecord> tags = pathPointTags(point);
        return READABLE_RETURN_EXCEPT(bmx, game::tail(setTagFloat, tags, kPathPointTag, kTagDistance, distance));
    }
    READABLE_FUNCTION(setPathPointDistance, Verified);

    // newFollowerRecord: the followers' pool lives while a follower does.
    uint32_t newFollowerRecord()
    {
        if (*g_followerPool == 0)
            *g_followerPool = ee::Ptr<Pool>(createPool(sizeof(Follower), kFollowersABlock, 0));
        return poolAlloc(ee::Ptr<Pool>(*g_followerPool));
    }
    READABLE_FUNCTION_WAITS(newFollowerRecord, Verified);

    namespace // the game functions below call. By address: a linker may fold poolCount with
    {         // a function of identical code.
        int32_t recordsInUse(ee::Ptr<Pool> pool) { return poolCount(pool); }
    }

    // freeFollowerRecord: the record back to its pool, and the pool gone with
    // the last one.
    int32_t freeFollowerRecord(ee::Address record)
    {
        poolFree(*g_followerPool, record);
        int32_t result = recordsInUse(*g_followerPool);
        if (result == 0)
        {
            result = static_cast<int32_t>(destroyPool(*g_followerPool));
            *g_followerPool = ee::Ptr<Pool>(0u);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(freeFollowerRecord, Verified);

    // unlinkFollower: off its path's list.
    void unlinkFollower(ee::Ptr<Follower> follower)
    {
        const ee::Ptr<Follower> behind = follower->followerBehind;
        const ee::Ptr<Follower> ahead = follower->followerAhead;
        if (behind)
            behind->followerAhead = ahead;
        else
            follower->path->nearestFollower = ahead;
        if (ahead)
            ahead->followerBehind = behind;
        else
            follower->path->furthestFollower = behind;
        follower->followerBehind = ee::Ptr<Follower>(0u);
        follower->followerAhead = ee::Ptr<Follower>(0u);
    }
    READABLE_FUNCTION(unlinkFollower, Verified);

    // linkFollowerBeforeNext: on the list just ahead of `behind`.
    void linkFollowerBeforeNext(ee::Ptr<Follower> follower, ee::Ptr<Follower> behind)
    {
        const ee::Ptr<Follower> ahead = behind->followerAhead;
        behind->followerAhead = follower;
        if (ahead)
            ahead->followerBehind = follower;
        else
            follower->path->furthestFollower = follower;
        follower->followerAhead = ahead;
        follower->followerBehind = behind;
    }
    READABLE_FUNCTION(linkFollowerBeforeNext, Verified);

    // linkFollowerAfterPrevious: on the list just behind `ahead`.
    void linkFollowerAfterPrevious(ee::Ptr<Follower> follower, ee::Ptr<Follower> ahead)
    {
        const ee::Ptr<Follower> behind = ahead->followerBehind;
        ahead->followerBehind = follower;
        if (behind)
            behind->followerAhead = follower;
        else
            follower->path->nearestFollower = follower;
        follower->followerAhead = ahead;
        follower->followerBehind = behind;
    }
    READABLE_FUNCTION(linkFollowerAfterPrevious, Verified);

    // placeFollower: a moved follower to its place on the list. Going one way
    // it walks the list; the other way the walk's test has its operands
    // turned, so it moves one place a call: the bug kept.
    void placeFollower(ee::Ptr<Follower> follower)
    {
        ee::Ptr<Follower> other(follower->followerAhead);
        const ee::Float distance = follower->distance;
        if (other)
        {
            if (other->distance < distance)
            {
                if (other->followerAhead != 0)
                {
                    other = ee::Ptr<Follower>(other->followerAhead);
                    for (;;)
                    {
                        const ee::Ptr<Follower> further(other->followerAhead);
                        if (further && other->distance < distance)
                        {
                            other = further;
                            continue;
                        }
                        break;
                    }
                }
                unlinkFollower(follower);
                linkFollowerBeforeNext(follower, other);
            }
        }
        other = ee::Ptr<Follower>(follower->followerBehind);
        if (other)
        {
            const ee::Float otherDistance = other->distance;
            if (!(distance < otherDistance))
                return;
            if (other->followerBehind != 0 && otherDistance < distance)
            {
                other = ee::Ptr<Follower>(other->followerBehind);
                for (;;)
                {
                    const ee::Ptr<Follower> further(other->followerBehind);
                    if (further && other->distance < distance)
                    {
                        other = further;
                        continue;
                    }
                    break;
                }
            }
            unlinkFollower(follower);
            game::tail(zge::linkFollowerAfterPrevious, follower, other); return;
        }
    }
    READABLE_FUNCTION(placeFollower, Verified);

    // addFollowerToList: a new follower on its path's list, in its place by
    // distance.
    void addFollowerToList(ee::Ptr<Follower> follower)
    {
        const ee::Ptr<PathRecord> path = follower->path;
        ee::Ptr<Follower> other = path->nearestFollower;
        if (!other)
        {
            path->nearestFollower = follower;
            path->furthestFollower = follower;
            follower->followerBehind = ee::Ptr<Follower>(0u);
            follower->followerAhead = ee::Ptr<Follower>(0u);
            return;
        }
        const ee::Float distance = follower->distance;
        if (distance < other->distance)
        {
            game::tail(zge::linkFollowerAfterPrevious, follower, other);
            return;
        }
        // The last follower it is not behind.
        while (other->followerAhead && other->distance < distance)
            other = other->followerAhead;
        game::tail(zge::linkFollowerBeforeNext, follower, other);
    }
    READABLE_FUNCTION(addFollowerToList, Verified);

    // nearestPointOnPath: the fraction along the path nearest `point`, segment by
    // segment; `distanceOut` (if any) gets how far off it is.
    ee::Float nearestPointOnPath(ee::Ptr<PathRecord> path, ee::Ptr<Vec4> point, ee::Ptr<ee::Float> distanceOut)
    {
        ee::Local<Vec4[4]> controlPoints;
        ee::Local<ee::Float> segmentDistance;
        ee::Float nearest = *g_farFromPath;
        ee::Float result(0.0f);
        ee::Ptr<PathPoint> knot = path->waypoints;
        while (nextPathPoint(knot) != 0)
        {
            const ee::Float start = pathPointDistance(knot);
            copyControlPoint(controlPoints[0], knot);
            const ee::Ptr<PathPoint> handle(nextPathPoint(knot));
            copyControlPoint(controlPoints[1], handle);
            const ee::Ptr<PathPoint> secondHandle(nextPathPoint(handle));
            copyControlPoint(controlPoints[2], secondHandle);
            knot = ee::Ptr<PathPoint>(nextPathPoint(secondHandle));
            copyControlPoint(controlPoints[3], knot);
            const ee::Float end = pathPointDistance(knot);
            const ee::Float along = nearestOnBezier(controlPoints, point, segmentDistance);
            if (*segmentDistance < nearest)
            {
                nearest = *segmentDistance;
                result = ee::max(ee::min(start + along * (end - start), ee::Float(1.0f)), ee::Float(0.0f));
            }
        }
        if (distanceOut)
            *distanceOut = nearest;
        return result;
    }
    READABLE_FUNCTION(nearestPointOnPath, Verified);

    // nearestPathVisitor: a tree visitor over the paths: the search keeps the
    // nearest. Returns 1 to go on.
    uint32_t nearestPathVisitor(ee::Ptr<PathNode> node, ee::Ptr<NearestPathSearch> search)
    {
        ee::Local<ee::Float> distanceOut;
        const ee::Ptr<PathRecord> path = &node->path;
        const ee::Float along = nearestPointOnPath(path, &search->point, distanceOut);
        const ee::Float off = *distanceOut;
        if (search->path && !(off < search->distance))
            return 1;
        search->path = path;
        search->distance = off;
        search->along = along;
        return 1;
    }
    READABLE_FUNCTION(nearestPathVisitor, Verified);

    // findNearestPath: the path nearest the point, every path tried
    // (nearestPathVisitor), with how far along it the nearest place is (0 ..
    // 1) into along when given. 0 when the level has no paths (along is then
    // left alone).
    ee::Ptr<PathRecord> findNearestPath(ee::Ptr<Vec4> point, ee::Ptr<ee::Float> along)
    {
        ee::Local<NearestPathSearch> search;
        search->point = quadwordOf(point);
        search->path = ee::Ptr<PathRecord>();
        searchTreeForEach(g_paths.get(), sym::nearestPathVisitor, search);
        if (!search->path)
            return ee::Ptr<PathRecord>();
        if (along)
            *along = search->along;
        return search->path;
    }
    READABLE_FUNCTION_WAITS(findNearestPath, Verified);

    namespace
    {
        constexpr uint32_t kPathsWord = sym::g_paths;
    }

    int32_t deletePathFollower(ee::Ptr<Follower> follower)
    {
        unlinkFollower(follower);
        return game::tail(zge::freeFollowerRecord, follower);
    }

    // freePathFollower: ZGE2's deletePathFollower: the follower's record back to its pool (freeFollowerRecord's
    // result, the original's tail jump); ZGE2 does not unlink it from its list first.
    int32_t freePathFollower(ee::Ptr<Follower> follower)
    {
        return game::tail(zge::freeFollowerRecord, follower);
    }
    READABLE_FUNCTION_WAITS_OWN(deletePathFollower, Verified, dm2, freePathFollower);

    // findPathRecord: the path of that name (the tree of paths' value), or none.
    ee::Ptr<PathRecord> findPathRecord(uint32_t key)
    {
        ee::Local<int32_t> keySlot;
        *keySlot = key;
        const ee::Ptr<SearchTreeNode> node = searchTreeFind(ee::Ptr<SearchTree>(static_cast<uint32_t>(kPathsWord)), keySlot);
        if (!node)
            return {};
        return searchTreeValueOf<PathRecord>(node);
    }
    READABLE_FUNCTION_WAITS(findPathRecord, Verified);

    // loadLevelPaths: a path for each name the level's waypoints have (the
    // first waypoint of a name loads all of them, loadPath). 1.
    uint32_t loadLevelPaths()
    {
        ee::Local<LocatorList> waypoints;
        initSearchTree(g_paths.get(), kCompareKeys);
        findLocators(waypoints, zge::nameHash(ee::Ptr<const char>(kTextWaypoint)), 0);
        for (ee::Ptr<Locator> locator = waypoints->head; locator; locator = locator->next)
        {
            const uint32_t pathName = ee::Ptr<PathPoint>(locator->point)->nameHash;
            if (!findPathRecord(pathName))
                zge::loadPath(pathName);
        }
        freeLocators(waypoints);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadLevelPaths, Verified);

    // clearPathRecords: every one of the 128 paths free and the tree of paths
    // empty (shutdownLevelSession). 1.
    uint32_t clearPathRecords()
    {
        const ee::Ptr<uint32_t> used(kPathsUsed);
        for (uint32_t slot = 0; slot < kPaths; ++slot)
            used[slot] = 0;
        initSearchTree(g_paths.get(), kCompareKeys);
        return 1;
    }
    READABLE_FUNCTION(clearPathRecords, Verified);

    // The follower's speed in units along its path. It keeps it as a fraction of the path's length.
    ee::Float followerSpeed(ee::Ptr<Follower> follower)
    {
        return follower->speed * ee::Ptr<PathRecord>(follower->path)->length;
    }
    READABLE_FUNCTION(followerSpeed, Verified);

    // Sets the follower's speed along its path. Returns the path.
    ee::Ptr<PathRecord> setFollowerSpeed(ee::Ptr<Follower> follower, ee::Float speed)
    {
        const ee::Ptr<PathRecord> path = follower->path;
        follower->speed = speed / path->length;
        return path;
    }
    READABLE_FUNCTION(setFollowerSpeed, Verified);

    // How far along its path the follower is.
    ee::Float followerAlong(ee::Ptr<Follower> follower)
    {
        return follower->distance;
    }
    READABLE_FUNCTION(followerAlong, Verified);

    // The path the follower is on: a road segment for a traffic car.
    ee::Ptr<PathRecord> followerSegment(ee::Ptr<Follower> follower)
    {
        return follower->path;
    }
    READABLE_FUNCTION(followerSegment, Verified);

    // The length of a path.
    ee::Float segmentEnd(ee::Ptr<PathRecord> segment)
    {
        return segment->length;
    }
    READABLE_FUNCTION(segmentEnd, Verified);

    namespace
    {

        // The knots either side, before setFollowerAlong finds the right
        // ones: the path's last and its first.
        void startOnPath(ee::Ptr<Follower> follower, ee::Ptr<PathRecord> path)
        {
            follower->behind = path->lastPoint;
            follower->ahead = path->waypoints;
        }
    }

    // setFollowerEndMode: what the follower does at an end of its path
    // (FollowerEndMode).
    void setFollowerEndMode(ee::Ptr<Follower> follower, uint32_t endMode)
    {
        follower->endMode = endMode;
    }
    READABLE_FUNCTION(setFollowerEndMode, Verified);

    // newPathFollower: a follower on the path at `along` (0 .. 1), moving
    // at speed (units of the path's length a second), with the end mode;
    // 0 if there is no room.
    ee::Ptr<Follower> newPathFollower(ee::Ptr<PathRecord> path, uint32_t endMode, ee::Float speed, ee::Float along)
    {
        const ee::Ptr<Follower> follower(zge::newFollowerRecord());
        if (!follower)
            return ee::Ptr<Follower>();
        follower->distance = along;
        follower->path = path;
        addFollowerToList(follower);
        startOnPath(follower, path);
        setFollowerAlong(follower, follower->distance);
        setFollowerSpeed(follower, speed);
        setFollowerEndMode(follower, endMode);
        follower->headingStale = 1;
        follower->positionStale = 1;
        return follower;
    }
    READABLE_FUNCTION_WAITS(newPathFollower, Verified);

    // advanceFollower: the follower moved on by its speed over `time`. Past
    // an end of its path it starts over at the other end (kFollowerLoops),
    // turns back there (kFollowerTurnsBack: its speed reversed) or stops.
    int32_t advanceFollower(ee::Ptr<Follower> follower, ee::Float time)
    {
        const ee::Float speed = follower->speed;
        const ee::Float step = speed * time;
        ee::Float along = follower->distance + step;
        if (along > 1.0f || along < 0.0f)
        {
            ee::Float passedEnd = 1.0f;
            ee::Float otherEnd = 0.0f;
            if (!(step > 0.0f))
            {
                passedEnd = 0.0f;
                otherEnd = 1.0f;
            }
            if (follower->endMode & kFollowerLoops)
                along = otherEnd;
            else if (follower->endMode & kFollowerTurnsBack)
            {
                along = passedEnd;
                follower->speed = -speed;
            }
            else
                along = ee::max(ee::min(along, ee::Float(1.0f)), ee::Float(0.0f));
        }
        return tail::setFollowerAlongKeepingResult(follower, along);
    }
    READABLE_FUNCTION(advanceFollower, Verified);

    // setFollowerSegment: the follower moved onto another path (none: 0),
    // keeping its place along it (0 .. 1) and its speed in units. 1.
    int32_t setFollowerSegment(ee::Ptr<Follower> follower, ee::Ptr<PathRecord> path)
    {
        if (!path)
            return 0;
        // (ZGE2 takes the speed from the follower once it is on the path; ZGE3 asks followerSpeed first)
        ee::Float speed{};
        if constexpr (!game::readable::kIsDm2)
            speed = followerSpeed(follower);
        unlinkFollower(follower);
        follower->path = path;
        addFollowerToList(follower);
        startOnPath(follower, path);
        setFollowerAlong(follower, follower->distance);
        if constexpr (game::readable::kIsDm2)
            speed = follower->speed;
        setFollowerSpeed(follower, speed);
        follower->headingStale = 1;
        follower->positionStale = 1;
        return 1;
    }
    READABLE_FUNCTION(setFollowerSegment, Verified);

    // segmentExit: the path exit `exit` (0 .. 2) leads to, from the path's
    // start or from its end (the names its last point's tags hold), or 0.
    ee::Ptr<PathRecord> segmentExit(ee::Ptr<PathRecord> path, uint32_t exit, uint32_t atStart)
    {
        uint32_t name = 0;
        if (exit <= 2)
        {
            const uint32_t exits = atStart ? kPointExitsFromStart : kPointExitsFromEnd;
            name = static_cast<uint32_t>(tagWord(pathPointTags(path->lastPoint), 8u, exits + exit * 4));
        }
        return findPathRecord(name);
    }
    READABLE_FUNCTION_WAITS(segmentExit, Verified);

    // followerSnap: the follower put at the place on its path nearest the
    // point.
    int32_t followerSnap(ee::Ptr<Follower> follower, ee::Ptr<Vec4> point)
    {
        const ee::Float along = nearestPointOnPath(follower->path, point, ee::Ptr<ee::Float>());
        return tail::setFollowerAlongKeepingResult(follower, along);
    }

    // snapFollowerToPath: ZGE2's followerSnap, with the search written in:
    // nearestPointOnPath's walk over the segments, without its clamp to
    // 0 .. 1 or a distance out.
    int32_t snapFollowerToPath(ee::Ptr<Follower> follower, ee::Ptr<Vec4> point)
    {
        ee::Local<Vec4[4]> controlPoints;
        ee::Local<ee::Float> segmentDistance;
        ee::Float nearest = *g_farFromPath;
        ee::Float along(0.0f);
        ee::Ptr<PathPoint> knot = follower->path->waypoints;
        while (nextPathPoint(knot) != 0)
        {
            const ee::Float start = pathPointDistance(knot);
            copyControlPoint(controlPoints[0], knot);
            const ee::Ptr<PathPoint> handle(nextPathPoint(knot));
            copyControlPoint(controlPoints[1], handle);
            const ee::Ptr<PathPoint> secondHandle(nextPathPoint(handle));
            copyControlPoint(controlPoints[2], secondHandle);
            knot = ee::Ptr<PathPoint>(nextPathPoint(secondHandle));
            copyControlPoint(controlPoints[3], knot);
            const ee::Float end = pathPointDistance(knot);
            const ee::Float onSegment = nearestOnBezier(controlPoints, point, segmentDistance);
            if (*segmentDistance < nearest)
            {
                nearest = *segmentDistance;
                along = start + onSegment * (end - start);
            }
        }
        return setFollowerAlongKeepingResult(follower, along);
    }
    READABLE_FUNCTION_OWN(followerSnap, Verified, dm2, snapFollowerToPath);

    // followerDistance: how far along its path the follower is, in units.
    ee::Float followerDistance(ee::Ptr<Follower> follower)
    {
        return follower->distance * follower->path->length;
    }
    READABLE_FUNCTION(followerDistance, Verified);

    // The followers either side of this one on its path's list.
    ee::Ptr<Follower> followerAhead(ee::Ptr<Follower> follower)
    {
        return follower->followerAhead;
    }
    READABLE_FUNCTION(followerAhead, Verified);

    ee::Ptr<Follower> followerBehind(ee::Ptr<Follower> follower)
    {
        return follower->followerBehind;
    }
    READABLE_FUNCTION(followerBehind, Verified);

    // pathNameHash: the path's name (what a save keeps).
    uint32_t pathNameHash(ee::Ptr<PathRecord> path)
    {
        return path->nameHash;
    }
    READABLE_FUNCTION(pathNameHash, Verified);

    // pathNearestFollower: the follower least far along the path, the head of
    // its followers' list; 0 when nothing is on it (the traffic lights ask
    // whether a car is on a road, trafficLightSegmentsClear).
    ee::Ptr<Follower> pathNearestFollower(ee::Ptr<PathRecord> path)
    {
        return path->nearestFollower;
    }
    READABLE_FUNCTION(pathNearestFollower, Verified);

    // The traffic light at the path's end (DM2's TrafficLight objects).
    uint32_t segmentJunction(ee::Ptr<PathRecord> path)
    {
        return path->junction;
    }
    READABLE_FUNCTION(segmentJunction, Verified);

    void setSegmentJunction(ee::Ptr<PathRecord> path, uint32_t light)
    {
        path->junction = light;
    }
    READABLE_FUNCTION(setSegmentJunction, Verified);
}
