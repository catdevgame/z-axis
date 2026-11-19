// The traffic lights' state (traffic_light_state.h): the paths, the state and
// the times in the light's tag table, and the light's frame. BMX XXX's own code
// (its game block), word for word Aggressive Inline's
// (game/aggressive/level/traffic_light_state.cpp).

#include "traffic_light_state.h"
#include "zge/scene/scene.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/follower.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/traffic_light_state.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // A path's tag word (paths are 1 .. 4, others 0). `code`: the
        // original copy's address: there are two, for the hash and for the
        // segment later found.
        int32_t pathWord(uint32_t light, uint32_t path, uint32_t code)
        {
            const auto read = [&](uint32_t tag)
            {
                const ee::Ptr<TagRecord> tags = trafficLightTags(light);
                const int32_t word = zge::tagWord(tags, kLightTagWords, tag);
                return word;
            };
            if (path == 2)
                return read(kLightTagPath2);
            if (path < 3)
            {
                if (path == 1)
                    return read(kLightTagPath1);
                return 0;
            }
            if (path == 3)
                return read(kLightTagPath3);
            if (path == 4)
                return read(kLightTagPath4);
            return 0;
        }
    }

    ee::Ptr<TagRecord> trafficLightTags(uint32_t light)
    {
        return zge::objectComponent(ee::Ptr<zge::SceneObject>(light));
    }

    // trafficLightPathTag: a path of the light, what the level's data gave.
    int32_t trafficLightPathTag(uint32_t light, uint32_t path)
    {
        return pathWord(light, path, sym::trafficLightPathTag);
    }
    READABLE_FUNCTION(trafficLightPathTag, Draft);

    // setTrafficLightPathTag: the path's tag set; answers 1 or 4 for a path
    // past those. BMX XXX's setTagWord gives no answer (ZGE4's stores and
    // returns nothing), so a written path answers the light's tag record,
    // what the original leaves as its result.
    int32_t setTrafficLightPathTag(uint32_t light, ee::Address value, uint32_t path)
    {
        const auto write = [&](uint32_t tag)
        {
            const ee::Ptr<TagRecord> tags = trafficLightTags(light);
            zge::setTagWord(tags, kLightTagWords, tag, value);
            return static_cast<int32_t>(ee::Address(tags));
        };
        if (path == 2)
            return write(kLightTagPath2);
        if (path < 3)
        {
            if (path == 1)
                return write(kLightTagPath1);
            return 1;
        }
        if (path == 3)
            return write(kLightTagPath3);
        if (path == 4)
            return write(kLightTagPath4);
        return 4;
    }
    READABLE_FUNCTION(setTrafficLightPathTag, Draft);

    // trafficLightSegment: a path of the light, as a segment once newTrafficLight
    // has found it.
    int32_t trafficLightSegment(uint32_t light, uint32_t path)
    {
        return pathWord(light, path, sym::trafficLightSegment);
    }
    READABLE_FUNCTION(trafficLightSegment, Draft);

    // setTrafficLightState: which path has the green (no answer: BMX XXX's
    // setTagWord gives none).
    void setTrafficLightState(uint32_t light, uint32_t path)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        game::tail(zge::setTagWord, tags, kLightTagWords, kLightTagState, path);
    }
    READABLE_FUNCTION(setTrafficLightState, Draft);

    // trafficLightState
    int32_t trafficLightState(uint32_t light)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        return zge::tagWord(tags, kLightTagWords, kLightTagState);
    }
    READABLE_FUNCTION(trafficLightState, Draft);

    // setTrafficLightTime: the time into its state (no answer, as above).
    void setTrafficLightTime(uint32_t light, ee::Float seconds)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        game::tail(setTagFloat, tags, kLightTagWords, kLightTagElapsed, seconds);
    }
    READABLE_FUNCTION(setTrafficLightTime, Draft);

    // trafficLightSegmentsClear: the mode that waits for cars: the second path's cars
    // [inferred] hold the first path's light red, or let it go green.
    int32_t trafficLightSegmentsClear(uint32_t light)
    {
        const uint32_t first = static_cast<uint32_t>(trafficLightSegment(light, 1));
        const uint32_t second = static_cast<uint32_t>(trafficLightSegment(light, 2));
        if (pathNearestFollower(ee::Ptr<PathRecord>(second)))
            return game::tail(setSegmentJunctionLights, first, kLightRed);
        return game::tail(setSegmentJunctionLights, first, kLightGreen);
    }
    READABLE_FUNCTION(trafficLightSegmentsClear, Draft);

    // updateTrafficLight: the light's frame: green for its cycle time, amber for
    // lightCycle, then the next path's turn (back to the first past the
    // last that has one).
    void updateTrafficLight(uint32_t light, ee::Float seconds)
    {
        uint32_t paths = 0;
        for (uint32_t path = kLightFirstPath;;)
        {
            const uint32_t segment = static_cast<uint32_t>(trafficLightSegment(light, path));
            ++path;
            if (segment != 0)
                ++paths;
            if (path >= kLightPathsEnd)
                break;
        }
        ee::Float elapsed = lightElapsed(light) + seconds;
        const uint32_t path = static_cast<uint32_t>(trafficLightState(light));
        const uint32_t segment = static_cast<uint32_t>(trafficLightSegment(light, path));
        const uint32_t state = lightState(segment) & 0xFFFFu;
        bool timesUp = false;
        if (state == kLightGreen)
        {
            if (trafficLightCycleTime(light) < elapsed)
            {
                setSegmentJunctionLights(segment, kLightAmber);
                elapsed = ee::Float(0.0f);
                timesUp = true;
            }
        }
        if (!timesUp)
        {
            if (state == kLightAmber)
            {
                if (lightCycle(light) < elapsed)
                {
                    setSegmentJunctionLights(segment, kLightRed);
                    const uint32_t following = path + 1;
                    const uint32_t nextPath = paths < following ? state : following; // state is 1 here: the first path
                    elapsed = ee::Float(0.0f);
                    setTrafficLightState(light, nextPath);
                    const uint32_t nextSegment = static_cast<uint32_t>(trafficLightSegment(light, nextPath));
                    setSegmentJunctionLights(nextSegment, kLightGreen);
                }
            }
        }
        game::tail(setTrafficLightTime, light, elapsed);
    }
    READABLE_FUNCTION(updateTrafficLight, Draft);

    // trafficLightCycleTime: how long a path stays green.
    ee::Float trafficLightCycleTime(uint32_t light)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        return zge::tagFloat(tags, kLightTagWords, kLightTagCycleTime);
    }
    READABLE_FUNCTION(trafficLightCycleTime, Draft);

    // lightCycle: the amber's time is the same tag as lightElapsed (0), the
    // time already gone, so a light is amber for one frame.
    ee::Float lightCycle(uint32_t light)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        return zge::tagFloat(tags, kLightTagWords, kLightTagElapsed);
    }
    READABLE_FUNCTION(lightCycle, Draft);

    // lightElapsed
    ee::Float lightElapsed(uint32_t light)
    {
        const ee::Ptr<TagRecord> tags = trafficLightTags(light);
        return zge::tagFloat(tags, kLightTagWords, kLightTagElapsed);
    }
    READABLE_FUNCTION(lightElapsed, Draft);

    // setSegmentTrafficLight: a road segment told of its light.
    void setSegmentTrafficLight(ee::Ptr<PathRecord> segment, uint32_t sceneObject)
    {
        game::tail(setSegmentJunction, segment, sceneObject);
    }
    READABLE_FUNCTION(setSegmentTrafficLight, Draft);
}
