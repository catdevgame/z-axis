#pragma once

// The traffic lights (traffic_light_state.cpp): a light is a scene
// object whose component's tag table holds its state. It holds up to four
// road paths, by their tags: as a hash in the level's data, as the path's
// segment once found. Each in turn goes green for its cycle, amber, red.
// DM2's COMMERCI has them (TrafficLight_tl01, _tl02); AI registers no light
// type (registerTrafficLightType has no caller), so its lights never run.
// What makes a light and what its frame message does: traffic_lights.cpp.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstdint>

namespace zge
{
    struct PathRecord; // (zge/scene/follower.h)
    struct TagRecord;  // (zge/assets/tag_table.h)
}

namespace aggressive
{
    // A segment's light [inferred from the order they follow].
    enum LightState : uint32_t
    {
        kLightRed = 0,
        kLightAmber = 1,
        kLightGreen = 2,
    };

    // The light's tag table: words (elapsed, state, the four paths, the
    // cycle time), and the mode as a byte.
    constexpr uint32_t kLightFirstPath = 1, kLightPathsEnd = 5;
    constexpr uint32_t kLightTagWords = 8, kLightTagBytes = 2;
    constexpr uint32_t kLightTagElapsed = 0, kLightTagState = 4, kLightTagPath1 = 8, kLightTagPath2 = 12;
    constexpr uint32_t kLightTagPath3 = 0x10, kLightTagPath4 = 20;
    constexpr uint32_t kLightTagCycleTime = 24, kLightTagMode = 36;
    constexpr uint32_t kLightModeCycling = 0, kLightModeWaitForCars = 1;

    // A light's tag record (its scene object's component).
    ee::Ptr<zge::TagRecord> trafficLightTags(uint32_t light);

    int32_t trafficLightPathTag(uint32_t light, uint32_t path);
    int32_t setTrafficLightPathTag(uint32_t light, ee::Address value, uint32_t path);
    int32_t trafficLightSegment(uint32_t light, uint32_t path);
    int32_t setTrafficLightState(uint32_t light, uint32_t path);
    int32_t trafficLightState(uint32_t light);
    int32_t setTrafficLightTime(uint32_t light, ee::Float seconds);
    int32_t trafficLightSegmentsClear(uint32_t light);
    int32_t updateTrafficLight(uint32_t light, ee::Float seconds);
    ee::Float trafficLightCycleTime(uint32_t light);
    ee::Float lightCycle(uint32_t light);
    ee::Float lightElapsed(uint32_t light);
    void setSegmentTrafficLight(ee::Ptr<zge::PathRecord> segment, uint32_t sceneObject);
}
