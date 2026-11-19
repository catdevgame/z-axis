// The TrafficLight object type (traffic_light_state.h has the light's state):
// its registration, making a light, its frame message, its part of the
// level's save, and the light a road segment shows. BMX XXX's own code (its
// game block). Making a light, the frame message and the segment's light are
// Aggressive Inline's (game/aggressive/level/traffic_lights.cpp) but for the
// save's three messages and the frame message's answer.

#include "traffic_light_state.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/traffic_lights.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kTypeName = sym::trafficLightTypeName; // "TrafficLight"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // A light's tag data is 0x28 bytes (traffic_light_state.h): its
        // slots' light states are four bytes from here, a slot for each path.
        constexpr uint32_t kLightTagBytesTotal = 0x28;
        constexpr uint32_t kLightTagSlotState = 0x1F; // + the path (1 .. 4): a LightState

        // A light in the level's save: its tag data, with each path as its
        // name's hash again, and whether the path's segment had this light.
        struct LightSave
        {
            uint8_t tags[kLightTagBytesTotal];           // +0x00
            uint32_t segmentHasLight[kLightPathsEnd - 1]; // +0x28 by path
        };
        static_assert(offsetof(LightSave, segmentHasLight) == 0x28 && sizeof(LightSave) == 0x38);
        // The saved tags as the tag accessors read them.
        ee::Ptr<zge::TagRecord> savedTagsOf(ee::Ptr<LightSave> save) { return save.cast<zge::TagRecord>(); }
    }

    // registerTrafficLightType: the "TrafficLight" level object type.
    uint32_t registerTrafficLightType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::newTrafficLight));
    }
    READABLE_FUNCTION(registerTrafficLightType, Verified);

    // newTrafficLight (the type's init): the object made a light: its
    // messages subscribed, each path's hash turned into its segment (0 if
    // the level has none) and the segments told of it; the first with a
    // segment goes green. The second loop's `next` stays put over a path
    // with no segment, so a gap in the four asks the same path again.
    int32_t newTrafficLight(uint32_t light)
    {
        zge::subscribe(light, zge::kUpdateMessage, zge::MessageHandler(sym::trafficLightFrameMessage));
        zge::subscribe(light, zge::kSaveSizeMessage, zge::MessageHandler(sym::trafficLightSaveSize));
        zge::subscribe(light, zge::kSaveMessage, zge::MessageHandler(sym::saveTrafficLight));
        zge::subscribe(light, zge::kRestoreMessage, zge::MessageHandler(sym::restoreTrafficLight));
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            const uint32_t tag = static_cast<uint32_t>(trafficLightPathTag(light, path));
            const ee::Ptr<zge::PathRecord> segment = zge::findPathRecord(tag);
            if (segment)
            {
                setTrafficLightPathTag(light, segment, path);
                setSegmentTrafficLight(segment, light);
            }
            else
                setTrafficLightPathTag(light, 0, path);
        }
        bool first = true;
        uint32_t next = kLightFirstPath;
        uint32_t lastFound = 0;
        for (uint32_t tries = kLightFirstPath; tries < kLightPathsEnd; ++tries)
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
            next = lastFound + 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(newTrafficLight, Draft);

    // trafficLightSaveSize (the engine's save size message).
    int32_t trafficLightSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(LightSave);
        return 0;
    }
    READABLE_FUNCTION(trafficLightSaveSize, Draft);

    // restoreTrafficLight (the engine's restore message): the saved tag data
    // put back, each path's hash turned into its segment again (0 for none),
    // and the segments that had this light told of it.
    int32_t restoreTrafficLight(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t light = message->target;
        const ee::Ptr<LightSave> saved(message->data);
        zge::memoryCopy(trafficLightTags(light), saved, kLightTagBytesTotal);
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            const uint32_t tag = static_cast<uint32_t>(trafficLightPathTag(light, path));
            ee::Ptr<zge::PathRecord> segment;
            if (tag != 0)
                segment = zge::findPathRecord(tag);
            if (segment)
                setTrafficLightPathTag(light, segment, path);
            else
                setTrafficLightPathTag(light, 0, path);
            if (saved->segmentHasLight[path - kLightFirstPath] != 0)
                setSegmentTrafficLight(segment, light);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreTrafficLight, Draft);

    // saveTrafficLight (the engine's save message): the light's tag data
    // copied out, then each path's segment replaced in the copy by its
    // name's hash (0 for none), with whether the segment has this light.
    int32_t saveTrafficLight(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<LightSave> saved(message->data);
        const ee::Ptr<zge::TagRecord> savedTags = savedTagsOf(saved);
        zge::memoryCopy(saved, trafficLightTags(message->target), kLightTagBytesTotal);
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            const ee::Ptr<zge::PathRecord> segment(static_cast<uint32_t>(trafficLightSegment(message->target, path)));
            uint32_t pathName = 0;
            uint32_t &hasLight = saved->segmentHasLight[path - kLightFirstPath];
            if (segment)
            {
                pathName = zge::pathNameHash(segment);
                hasLight = 0;
                if (zge::segmentJunction(segment) == message->target)
                    hasLight = 1;
            }
            else
                hasLight = 0;
            switch (path)
            {
            case 1:
                zge::setTagWord(savedTags, kLightTagWords, kLightTagPath1, pathName);
                break;
            case 2:
                zge::setTagWord(savedTags, kLightTagWords, kLightTagPath2, pathName);
                break;
            case 3:
                zge::setTagWord(savedTags, kLightTagWords, kLightTagPath3, pathName);
                break;
            case 4:
                zge::setTagWord(savedTags, kLightTagWords, kLightTagPath4, pathName);
                break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(saveTrafficLight, Draft);

    // trafficLightFrameMessage (the engine's update message): the light's
    // frame, by its mode tag.
    int32_t trafficLightFrameMessage(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t light = message->target;
        const uint32_t mode = zge::tagByte(trafficLightTags(light), kLightTagBytes, kLightTagMode);
        if (mode == kLightModeCycling)
            updateTrafficLight(light, *ee::Ptr<ee::Float>(message->data));
        else if (mode == kLightModeWaitForCars)
            trafficLightSegmentsClear(light);
        return 0;
    }
    READABLE_FUNCTION_WAITS(trafficLightFrameMessage, Draft);

    // lightState: the light a road segment shows. The segment's light lists
    // up to four paths, and the first of its slots with this segment whose
    // state byte says red, amber or green gives the answer; any other value
    // goes on to the next slot. Red when no slot answers.
    uint32_t lightState(uint32_t segment)
    {
        const uint32_t light = zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment));
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            if (static_cast<uint32_t>(trafficLightSegment(light, path)) != segment)
                continue;
            const uint32_t state = zge::tagByte(trafficLightTags(light), kLightTagBytes, kLightTagSlotState + path);
            if (state == kLightRed || state == kLightAmber || state == kLightGreen)
                return state;
        }
        return kLightRed;
    }
    READABLE_FUNCTION_WAITS(lightState, Draft);

    // setSegmentJunctionLights: a road segment's light set at its traffic
    // light, in each of the light's slots with this segment (what lightState
    // reads). Answers the fourth slot's segment when it is not this one,
    // else the light's tag record (BMX XXX's setTagByte gives no answer:
    // what the original leaves as its result).
    int32_t setSegmentJunctionLights(uint32_t segment, LightState state)
    {
        const uint32_t light = zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment));
        constexpr uint32_t kLastPath = kLightPathsEnd - 1;
        for (uint32_t path = kLightFirstPath; path < kLastPath; ++path)
        {
            if (static_cast<uint32_t>(trafficLightSegment(light, path)) == segment)
                zge::setTagByte(trafficLightTags(light), kLightTagBytes, kLightTagSlotState + path, state & 0xFFu);
        }
        const int32_t last = trafficLightSegment(light, kLastPath);
        if (static_cast<uint32_t>(last) != segment)
            return last;
        const ee::Ptr<zge::TagRecord> tags = trafficLightTags(light);
        zge::setTagByte(tags, kLightTagBytes, kLightTagSlotState + kLastPath, state & 0xFFu);
        return static_cast<int32_t>(ee::Address(tags));
    }
    READABLE_FUNCTION_WAITS(setSegmentJunctionLights, Draft);
}
