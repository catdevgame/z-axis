// The traffic lights, AI's side: making a light and its frame message
// (traffic_light_state.h has the rest). No AI level has
// one (only Prop_TrafficLight props): DM2's COMMERCI does (TrafficLight_tl01,
// _tl02), and the TrafficLight type is not registered in AI
// (registerTrafficLightType has no caller). DM2's newTrafficLight subscribes
// to two more messages and its frame message does more: these are AI's.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/scene.h"
#include "aggressive/level/traffic_light_state.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "aggressive/level/traffic_lights.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/traffic_lights.cpp.h"

namespace aggressive
{

    namespace
    {
        constexpr uint32_t kUpdateMessage = 0x7000;
        constexpr uint32_t kOnUpdate = sym::trafficLightFrameMessage;          // trafficLightFrameMessage

        // The note of a light's frame.
        struct LightUpdate
        {
            uint8_t _00[0x08];
            uint32_t sceneObject;        // +0x08
            uint8_t _0c[0x08];
            ee::Ptr<ee::Float> seconds;  // +0x14 the frame's time
        };
        static_assert(offsetof(LightUpdate, sceneObject) == 0x08 && offsetof(LightUpdate, seconds) == 0x14);
    }

    // newTrafficLight: the object made a light: each path's hash turned into
    // its segment (0 if the level has none), the segments told of it; the
    // first with a segment goes green. The loop's `next` stays put over a
    // path with no segment, so a gap in the four asks the same path again.
    int32_t newTrafficLight(uint32_t light)
    {
        zge::subscribe(light, kUpdateMessage, kOnUpdate);
        for (uint32_t path = kLightFirstPath;;)
        {
            const uint32_t tag = static_cast<uint32_t>(trafficLightPathTag(light, path));
            const ee::Ptr<zge::PathRecord> segment = zge::findPathRecord(tag);
            if (segment != 0)
            {
                setTrafficLightPathTag(light, segment, path);
                setSegmentTrafficLight(segment, light);
            }
            else
                setTrafficLightPathTag(light, 0, path);
            ++path;
            if (path >= kLightPathsEnd)
                break;
        }
        bool first = true;
        uint32_t next = 1;
        uint32_t lastFound = 0;
        for (uint32_t tries = 1;;)
        {
            const uint32_t segment = static_cast<uint32_t>(trafficLightSegment(light, next));
            if (segment != 0)
            {
                if (first)
                {
                    setSegmentJunctionLights(segment, kLightGreen);
                    first = false;
                    setTrafficLightState(light, tries);
                }
                lastFound = next;
            }
            ++tries;
            next = lastFound + 1;
            if (tries >= kLightPathsEnd)
                return 1;
        }
    }
    READABLE_FUNCTION_WAITS(newTrafficLight, Verified);

    // trafficLightFrameMessage: the message's handler, by the light's mode tag.
    int32_t trafficLightFrameMessage(ee::Ptr<LightUpdate> note)
    {
        const uint32_t light = note->sceneObject;
        const ee::Ptr<zge::TagRecord> tags = trafficLightTags(light);
        const uint32_t mode = zge::tagByte(tags, kLightTagBytes, kLightTagMode);
        if (mode == kLightModeCycling)
            updateTrafficLight(light, *note->seconds);
        else
        {
            if (mode == kLightModeWaitForCars)
                trafficLightSegmentsClear(light);
        }
        return 1;
    }
    READABLE_FUNCTION(trafficLightFrameMessage, Verified);

    namespace
    {
        constexpr uint32_t kTextTrafficLight = sym::g_textTrafficLight;
        constexpr uint32_t kSetTagByte = sym::setTagByte; // setTagByte ((record, 2, offset, value); readable: setTagByte_, Verified)
    }

    namespace
    {
        namespace tail
        {
        uint32_t registerLevelObjectType(uint32_t name, uint32_t createLevelEntity, uint32_t newTrafficLight) { return game::tail(zge::registerLevelObjectType, name, createLevelEntity, newTrafficLight); }
        }
    }

    uint32_t registerTrafficLightType()
    {
        return tail::registerLevelObjectType(kTextTrafficLight, sym::createLevelEntity, sym::newTrafficLight);
    }
    READABLE_FUNCTION(registerTrafficLightType, Verified);

    namespace
    {
        namespace tail
        {
        // setTagByte returns nothing: the answer is what the caller had in its result register,
        // objectComponent's record (the address form keeps it).
        int32_t setTagByteKeepingResult(ee::Ptr<zge::TagRecord> record, uint32_t width, uint32_t offset, uint32_t value) { return ee::tailCall<int32_t>(kSetTagByte, record, width, offset, value); }
        }
    }

    // The state of the light on a road segment. A junction lists up to four
    // segments, and the first of its slots with this segment whose tag byte
    // (one per slot, from 0x20) says 0, 1 or 2 gives the answer; any other
    // value goes on to the next slot. 0 when no slot answers.
    uint32_t lightState(uint32_t segment)
    {
        const int32_t junction = zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment)); // the lights' segment word
        for (int32_t slot = 1; slot <= 4; ++slot)
        {
            if (trafficLightSegment(junction, slot) != segment)
                continue;
            const ee::Ptr<zge::TagRecord> tags = zge::objectComponent(ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(junction)));
            const int32_t state = zge::tagByte(tags, 2, 0x1F + slot);
            if (state == 0 || state == 1 || state == 2)
                return state;
        }
        return 0;
    }
    READABLE_FUNCTION(lightState, Verified);

    namespace
    {
        constexpr uint32_t kTagSlotLight = 0x1F; // + the slot (1 .. 4): that slot's light state (LightState), a byte
    }

    // setSegmentJunctionLights: a road segment's light state set at the
    // junction at its end, in each of its slots with this segment (what
    // lightState reads). Returns the fourth slot's segment when it is not
    // this one, else the tag write's answer.
    int32_t setSegmentJunctionLights(uint32_t segment, LightState state)
    {
        const int32_t junction = zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment)); // the lights' segment word
        const ee::Ptr<zge::SceneObject> light(static_cast<uint32_t>(junction));
        for (int32_t slot = 1; slot < 4; ++slot)
            if (trafficLightSegment(junction, slot) == static_cast<int32_t>(segment))
                zge::setTagByte(zge::objectComponent(light), kLightTagBytes, kTagSlotLight + slot, state & 0xFFu);
        const int32_t last = trafficLightSegment(junction, 4);
        if (last != static_cast<int32_t>(segment))
            return last;
        return tail::setTagByteKeepingResult(zge::objectComponent(light), kLightTagBytes, kTagSlotLight + 4, state & 0xFFu);
    }
    READABLE_FUNCTION(setSegmentJunctionLights, Draft);
}
