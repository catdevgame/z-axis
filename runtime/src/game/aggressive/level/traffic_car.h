#pragma once

// A traffic car: its scene object's component of kind 0x1C (traffic_cars.cpp; the skater's collision reads its
// follower: skater/move_collide.cpp).

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct SceneObject;
    struct SweepBox;
}

namespace aggressive
{
    struct TrafficCar
    {
        ee::Ptr<zge::SceneObject> object; // +0x00 its scene object
        ee::Ptr<zge::SweepBox> sweepBox; // +0x04 its box in the level's sweep, a list's item (kListAdd)
        uint32_t follower;          // +0x08 its path follower, or 0
        ee::Float wheelCircumference; // +0x0C
        uint8_t engineSample;       // +0x10
        uint8_t secondSample;       // +0x11
        uint8_t engineLevel;        // +0x12
        uint8_t _13;
        uint16_t soundSource;       // +0x14
        uint16_t _16;
        uint32_t findExclusion;     // +0x18 nonzero until the exclusion object was looked up
    };
    static_assert(offsetof(TrafficCar, follower) == 0x08 && offsetof(TrafficCar, wheelCircumference) == 0x0C);
    static_assert(offsetof(TrafficCar, engineLevel) == 0x12 && offsetof(TrafficCar, soundSource) == 0x14);
    static_assert(offsetof(TrafficCar, findExclusion) == 0x18);
}

namespace aggressive
{
    // (traffic_cars.cpp)
    int32_t hideCars();
    void setTrafficCarVolume(uint32_t name, uint32_t engineLevel);
    int32_t showCars();
    uint32_t upVector();
}
