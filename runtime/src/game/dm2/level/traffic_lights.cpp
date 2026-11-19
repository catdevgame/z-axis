// The traffic lights (traffic_light_state.h has their state): a light made
// of a level object, its frame, save, restore and save-size messages, the
// type's registration, and a road segment's light at its junction. DM2's
// own code (its game block); Aggressive Inline's level/traffic_lights.cpp is
// its ancestor (AI's light subscribes to the frame alone and has no sound,
// and its slot states sit 4 bytes earlier). DM2's COMMERCI has the lights
// (TrafficLight_tl01, _tl02).

#include "traffic_light_state.h"
#include "object_sounds.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/text/cstring.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/level/traffic_lights.cpp.h"

namespace game::dm2
{
    namespace
    {

        constexpr ee::Ptr<const char> kTrafficLightTypeName{sym::trafficLightTypeName}; // "TrafficLight"

        // The light's sound (its tag word at kLightTagSound).
        uint32_t lightSound(uint32_t light)
        {
            return static_cast<uint32_t>(zge::tagWord(trafficLightTags(light), kLightTagWords, kLightTagSound));
        }

        // A path's tag word (1 .. 4) in a saved copy of the tag table.
        constexpr uint32_t pathTagOffset(uint32_t path)
        {
            return path == 1 ? kLightTagPath1 : path == 2 ? kLightTagPath2 : path == 3 ? kLightTagPath3 : kLightTagPath4;
        }

        // A light's save: its tag table's first bytes (the paths as their
        // name hashes), then for each path whether the light holds the
        // path's junction.
        struct LightSave
        {
            uint8_t tags[kLightSavedTagBytes];
            uint32_t holdsJunction[kLightPathsEnd - kLightFirstPath];
        };
        static_assert(sizeof(LightSave) == 0x3C);
        // The saved tags as the tag accessors read them.
        ee::Ptr<zge::TagRecord> savedTags(ee::Ptr<LightSave> save) { return save.cast<zge::TagRecord>(); }
    }

    // trafficLightSaveSize (the save-size message): a light's save is 60
    // bytes; the object's sound has its turn.
    int32_t trafficLightSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(LightSave);
        objectSoundMessage(message, lightSound(message->target));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficLightSaveSize, Verified);

    // trafficLightSave (the save message): the light's tag table copied into
    // the save with each path's segment as its name hash (0 none), and
    // whether the light holds that segment's junction; the object's sound
    // has its turn.
    int32_t trafficLightSave(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<LightSave> save(message->data);
        zge::memoryCopy(save, trafficLightTags(message->target), kLightSavedTagBytes);
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            uint32_t hash = 0;
            const ee::Ptr<zge::PathRecord> segment(static_cast<uint32_t>(trafficLightSegment(message->target, path)));
            save->holdsJunction[path - kLightFirstPath] = 0;
            if (segment)
            {
                hash = zge::pathNameHash(segment);
                if (static_cast<uint32_t>(zge::segmentJunction(segment)) == message->target)
                    save->holdsJunction[path - kLightFirstPath] = 1;
            }
            zge::setTagWord(savedTags(save), kLightTagWords, pathTagOffset(path), hash);
        }
        objectSoundMessage(message, lightSound(message->target));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficLightSave, Verified);

    // trafficLightRestore (the restore message): the light's tag table back
    // from the save, each path's segment found again by its name hash (0
    // none), the junctions the light held given back to it; the object's
    // sound has its turn.
    int32_t trafficLightRestore(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t light = message->target;
        const ee::Ptr<LightSave> save(message->data);
        zge::memoryCopy(trafficLightTags(light), save, kLightSavedTagBytes);
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            ee::Ptr<zge::PathRecord> segment;
            if (const int32_t hash = trafficLightPathTag(light, path))
                segment = zge::findPathRecord(static_cast<uint32_t>(hash));
            setTrafficLightPathTag(light, segment, path);
            if (save->holdsJunction[path - kLightFirstPath])
                setSegmentTrafficLight(segment, light);
        }
        objectSoundMessage(message, lightSound(message->target));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficLightRestore, Verified);

    // newTrafficLight: the object made a light: its frame, save-size, save
    // and restore messages; each path's hash turned into its segment (0 if
    // the level has none), the segments told of it; the first with a
    // segment goes green; its sound started. The loop's `next` stays put
    // over a path with no segment, so a gap in the four asks the same path
    // again (as AI's).
    int32_t newTrafficLight(uint32_t light)
    {
        zge::subscribe(light, zge::kUpdateMessage, zge::MessageHandler(sym::trafficLightFrameMessage));
        zge::subscribe(light, zge::kSaveSizeMessage, zge::MessageHandler(sym::trafficLightSaveSize));
        zge::subscribe(light, zge::kSaveMessage, zge::MessageHandler(sym::trafficLightSave));
        zge::subscribe(light, zge::kRestoreMessage, zge::MessageHandler(sym::trafficLightRestore));
        for (uint32_t path = kLightFirstPath; path < kLightPathsEnd; ++path)
        {
            const ee::Ptr<zge::PathRecord> segment = zge::findPathRecord(static_cast<uint32_t>(trafficLightPathTag(light, path)));
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
        startObjectSound(ee::Ptr<zge::SceneObject>(light), lightSound(light));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newTrafficLight, Verified);

    // trafficLightFrameMessage: the light's frame, by its mode: cycling, or
    // waiting for the cars; then the object's sound has its turn.
    int32_t trafficLightFrameMessage(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t light = message->target;
        const uint32_t mode = zge::tagByte(trafficLightTags(light), kLightTagBytes, kLightTagMode);
        if (mode == kLightModeCycling)
            updateTrafficLight(light, *ee::Ptr<ee::Float>(message->data));
        else if (mode == kLightModeWaitForCars)
            trafficLightSegmentsClear(light);
        objectSoundMessage(message, lightSound(message->target));
        return 1;
    }
    READABLE_FUNCTION_WAITS(trafficLightFrameMessage, Verified);

    // registerTrafficLightType: the "TrafficLight" level objects made lights.
    uint32_t registerTrafficLightType()
    {
        return game::tail(zge::registerEntityInit, kTrafficLightTypeName, sym::newTrafficLight);
    }
    READABLE_FUNCTION(registerTrafficLightType, Verified);

    // lightState: the state of the light on a road segment. A junction lists
    // up to four segments, and the first of its slots with this segment
    // whose state byte says 0, 1 or 2 gives the answer; any other value goes
    // on to the next slot. 0 when no slot answers.
    uint32_t lightState(uint32_t segment)
    {
        const uint32_t junction = static_cast<uint32_t>(zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment)));
        for (uint32_t slot = kLightFirstPath; slot < kLightPathsEnd; ++slot)
        {
            if (static_cast<uint32_t>(trafficLightSegment(junction, slot)) != segment)
                continue;
            const uint32_t state = zge::tagByte(trafficLightTags(junction), kLightTagBytes, kLightTagSlotStates + slot - 1);
            if (state == kLightAmber || state == kLightRed || state == kLightGreen)
                return state;
        }
        return 0;
    }
    READABLE_FUNCTION(lightState, Verified);

    // setSegmentJunctionLights: a road segment's light state set at the
    // junction at its end, in each of its slots with this segment (what
    // lightState reads).
    void setSegmentJunctionLights(uint32_t segment, LightState state)
    {
        const uint32_t junction = static_cast<uint32_t>(zge::segmentJunction(ee::Ptr<zge::PathRecord>(segment)));
        constexpr uint32_t kLastSlot = kLightPathsEnd - 1;
        for (uint32_t slot = kLightFirstPath; slot < kLastSlot; ++slot)
            if (static_cast<uint32_t>(trafficLightSegment(junction, slot)) == segment)
                zge::setTagByte(trafficLightTags(junction), kLightTagBytes, kLightTagSlotStates + slot - 1, state & 0xFFu);
        if (static_cast<uint32_t>(trafficLightSegment(junction, kLastSlot)) == segment)
            game::tail(zge::setTagByte, trafficLightTags(junction), kLightTagBytes, kLightTagSlotStates + kLastSlot - 1, state & 0xFFu);
    }
    READABLE_FUNCTION(setSegmentJunctionLights, Verified);
}
