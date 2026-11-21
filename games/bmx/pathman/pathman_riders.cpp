// BMX XXX's path men (pathman.h): how near the riders are. Each frame a path
// man measures its distance to the nearest rider, twice: to the rider's
// physics position and to its drawn body. BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"

#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <limits>
#include "sym/bmx/pathman/pathman_riders.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::vec4;

    namespace
    {
        // No rider measured yet.
        constexpr ee::Float kNoRiderNear = std::numeric_limits<float>::max();

        // The riders that count are shown and a player's (kFlagShown set,
        // kFlagNoPlayer clear: both of the first flag byte).
        inline bool riderCounts(const rider::Rider *rider)
        {
            static_assert(rider::kFlagShown.byte == 0 && rider::kFlagNoPlayer.byte == 0);
            return (rider->flags[0] & (rider::kFlagNoPlayer.mask | rider::kFlagShown.mask)) == rider::kFlagShown.mask;
        }
    }

    // watchRiders (each frame): the wait before its next phrase runs down,
    // the riders are measured, and the frames a rider has stayed within
    // touching range are counted.
    void watchRiders(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->phraseFrames != 0)
            --record->phraseFrames;
        measureRiders(pathMan);
        if (pathManDrawnRiderWithin(pathMan, record->touchRangeSquared) == 0)
            record->riderNearFrames = 0;
        else
            ++record->riderNearFrames;
    }
    READABLE_FUNCTION_WAITS(watchRiders, Verified);

    // measureRider (a rider visit): the squared distance from the path man
    // to the rider's physics position, kept when it is the nearest so far.
    uint32_t measureRider(uint32_t riderIndex, ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<Vec4> pathManAt;
        ee::Local<Vec4> offset;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex & 0xFF);
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (riderCounts(rider))
        {
            *offset = *vec4(rider->position) + *vec4(rider->rootOffset);
            const ee::Ptr<Vec4> at = pathManPosition(pathMan, pathManAt);
            *offset = *offset - *at;
            record->riderDistanceSquared = ee::min(squaredLength(*offset), record->riderDistanceSquared);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(measureRider, Verified);

    // measureDrawnRider (a rider visit): the same to the rider's drawn body.
    uint32_t measureDrawnRider(uint32_t riderIndex, ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<Vec4> pathManAt;
        ee::Local<Vec4> offset;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex & 0xFF);
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (riderCounts(rider))
        {
            *offset = *vec4(rider->drawnPosition) + *vec4(rider->drawnOffset);
            const ee::Ptr<Vec4> at = pathManPosition(pathMan, pathManAt);
            *offset = *offset - *at;
            record->drawnRiderDistanceSquared = ee::min(squaredLength(*offset), record->drawnRiderDistanceSquared);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(measureDrawnRider, Verified);

    // measureRiders: both distances to the nearest rider, afresh.
    void measureRiders(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        record->drawnRiderDistanceSquared = kNoRiderNear;
        record->riderDistanceSquared = kNoRiderNear;
        rider::forEachRider(rider::RiderVisit(sym::measureRider), pathMan);
        pathman::forEachRiderTail(rider::RiderVisit(sym::measureDrawnRider), pathMan);
    }
    READABLE_FUNCTION_WAITS(measureRiders, Verified);

    // pathManRiderWithin: 1 when the nearest rider's physics position is
    // within that squared distance.
    uint32_t pathManRiderWithin(ee::Ptr<SceneObject> pathMan, ee::Float rangeSquared)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        return record->riderDistanceSquared <= rangeSquared ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(pathManRiderWithin, Verified);

    // pathManDrawnRiderWithin: the same for the nearest rider's drawn body.
    uint32_t pathManDrawnRiderWithin(ee::Ptr<SceneObject> pathMan, ee::Float rangeSquared)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        return record->drawnRiderDistanceSquared <= rangeSquared ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(pathManDrawnRiderWithin, Verified);
}
