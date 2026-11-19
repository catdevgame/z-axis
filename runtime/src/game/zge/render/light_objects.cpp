// A level's light objects (zge/render/light_objects.h): one looked up by its
// hash, a new one put on the list, the lights an object takes, and the debug
// drawing of them. readLitoChunk (assets/chunk_reader.cpp) makes them from
// the chunk file; freeLitoRecords (scene/camera_records.cpp) frees them.
// Nothing in AI calls these four; AI's own levels have no lights.

#include "light_objects.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "zge/container/list.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/scene/node.h"
#include "zge/debug/debug.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/render/light_objects.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<List, sym::g_litoRecords> g_litoRecords; // the lights, the directional ones first
        inline ee::Global<ee::Ptr<Pool>, sym::g_litoChunkPool> g_litoChunkPool; // LitoRecords

        // LitoRecord::flags: litoLightsForNode passes over a light without it
        // (readLitoChunk does not write it).
        constexpr uint16_t kLightEnabled = 1u << 0;

        // The most lights litoLightsForNode keeps (its lists' room).
        constexpr uint32_t kMostLights = 8;
        constexpr float kNoLightYet = 1.0e6f; // a slot's distance before a light takes it

        // An object's group bits: the 16 bits from bit 45 of a node's flags
        // doubleword (a light lights the objects that share one with its
        // groups).
        uint32_t objectGroups(const Node &node) { return (node.flagsHigh >> 13) & 0xFFFFu; }

        bool lights(const LitoRecord &light, const Node &object)
        {
            return (light.flags & kLightEnabled) != 0 && (objectGroups(object) & light.groups) != 0;
        }

        // The debug drawing's flags, as the original passes them.
        constexpr uint32_t kDebugPointFlags = 2, kDebugLineFlags = 0;
        constexpr ee::Float kDirectionalLineLength = 1000.0f;
        constexpr ee::Float kLightPointSize = 1.0f, kSmallLightPointSize = 0.25f;
        constexpr uint8_t kOpaque = 255;

        // drawLitoRecords' light types beside kDirectionalLight (types 1 and 4
        // are not drawn).
        constexpr uint32_t kPointLight = 2;
        constexpr uint32_t kSpotLight = 3;


        namespace routed
        {
        }
    }

    // findLitoRecord (nothing calls it in AI): the light with this hash, 0 if
    // there is none.
    ee::Ptr<LitoRecord> findLitoRecord(uint32_t hash)
    {
        for (ee::Ptr<ListNode> link = g_litoRecords->head; link; link = link->next)
        {
            const ee::Ptr<LitoRecord> light = recordOfNode<LitoRecord>(link);
            if (light->hash == hash)
                return light;
        }
        return ee::Ptr<LitoRecord>();
    }
    READABLE_FUNCTION(findLitoRecord, Draft);

    // litoLightsForNode (nothing calls it in AI): the lights that light an
    // object, at most `most`, into `out` (the rest of its `most` slots 0);
    // returns how many it found. Every directional light of the object's
    // groups is taken; then each other light of its groups whose reach,
    // doubled, with the object's radius takes in the object's centre: into
    // the next slot while there is room, else into every slot that holds a
    // farther light.
    // Kept bugs: the directional lights are taken with no check of the room
    // (eight slots on the stack, past them the stack above), and a light that
    // comes when the slots are full can take several of them.
    uint32_t litoLightsForNode(ee::Ptr<Node> object, ee::Ptr<ee::Ptr<LitoRecord>> out, uint32_t most)
    {
        ee::Local<Vec4> centre;
        ee::Local<ee::Ptr<LitoRecord>[kMostLights]> found;
        ee::Local<ee::Float[kMostLights]> distance; // each found light's squared distance (0 a directional one)
        ee::Local<Vec4> apart;
        for (uint32_t slot = 0; slot < kMostLights; ++slot)
        {
            found[slot] = ee::Ptr<LitoRecord>();
            distance[slot] = kNoLightYet;
        }

        uint32_t count = 0;
        ee::Ptr<ListNode> link = g_litoRecords->head;
        for (; link && recordOfNode<LitoRecord>(link)->type == kDirectionalLight; link = link->next)
        {
            const ee::Ptr<LitoRecord> light = recordOfNode<LitoRecord>(link);
            if (lights(*light, *object))
            {
                distance[count] = 0.0f;
                found[count] = light;
                ++count;
            }
        }

        *centre = object->sphereCentre;
        for (; link; link = link->next)
        {
            const ee::Ptr<LitoRecord> light = recordOfNode<LitoRecord>(link);
            if (!lights(*light, *object))
                continue;
            *apart = *centre - light->position;
            const ee::Float squared = squaredLength(*apart);
            const ee::Float reach = light->reach + light->reach + object->radius;
            if (!(squared <= reach * reach))
                continue;
            if (count < most)
            {
                distance[count] = squared;
                found[count] = light;
                ++count;
                continue;
            }
            for (uint32_t slot = 0; slot < most; ++slot)
            {
                if (squared < distance[slot])
                {
                    distance[slot] = squared;
                    found[slot] = light;
                }
            }
        }

        for (uint32_t slot = 0; slot < most; ++slot)
            out[slot] = slot < count ? found[slot] : ee::Ptr<LitoRecord>();
        return count;
    }
    READABLE_FUNCTION(litoLightsForNode, Draft);

    // drawLitoRecords (debug; nothing calls it in AI): each light drawn in
    // its colour, opaque: a directional light as a point and a line 1000
    // long along its direction, a point light as a small point, a spot
    // light as a point and a line along its direction as long as its
    // reach. 1.
    uint32_t drawLitoRecords()
    {
        ee::Local<uint8_t[4]> colour; // RGBA8
        ee::Local<Vec4> end;          // the line's far end
        for (ee::Ptr<ListNode> link = g_litoRecords->head; link; link = link->next)
        {
            const ee::Ptr<LitoRecord> light = recordOfNode<LitoRecord>(link);
            const auto opaqueColour = [&]
            {
                std::memcpy(&colour[0], light->colour, sizeof(light->colour));
                colour[3] = kOpaque;
            };
            const ee::Ptr<Vec4> position(&light->position);
            switch (light->type)
            {
            case kDirectionalLight:
                opaqueColour();
                // (addDebugPoint's fourth argument is not read: 0 here, a leftover
                // register in the original.)
                addDebugPoint(position, colour, kDebugPointFlags, 0, kLightPointSize);
                *end = light->direction * kDirectionalLineLength;
                *end = *end + light->position;
                addDebugLine(position, end, colour, kDebugLineFlags);
                break;
            case kPointLight:
                opaqueColour();
                addDebugPoint(position, colour, kDebugPointFlags, 0, kSmallLightPointSize);
                break;
            case kSpotLight:
                opaqueColour();
                addDebugPoint(position, colour, kDebugPointFlags, 0, kLightPointSize);
                *end = light->direction * light->reach;
                *end = *end + light->position;
                addDebugLine(position, end, colour, kDebugLineFlags);
                break;
            default:
                break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawLitoRecords, Draft);

    // newLitoRecord (nothing calls it in AI): a light of this type from the
    // pool, put on the list: at its front when it is directional, else at
    // its end. Not checked for a full pool.
    ee::Ptr<LitoRecord> newLitoRecord(uint32_t type)
    {
        const ee::Ptr<LitoRecord> light(poolAlloc(*g_litoChunkPool));
        light->type = type;
        if (type == kDirectionalLight)
            listPushFront(ee::Ptr<List>(&*g_litoRecords), ee::Ptr<ListNode>(&light->link));
        else
            zge::listPush(ee::Ptr<List>(&*g_litoRecords), ee::Ptr<ListNode>(&light->link));
        return light;
    }
    READABLE_FUNCTION_WAITS(newLitoRecord, Draft);
}
