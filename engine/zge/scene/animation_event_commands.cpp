// ZGE4's animation event commands (animation_event_commands.h; BMX XXX only). They sit after the animators'
// code in the link order (registerAnimationEventHandler, animator.cpp). An object field holds a level object's id
// (findObjects), or else a world node's name hash.

#include "animation_event_commands.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/zge/scene/animation_event_commands.cpp.h"

namespace zge
{
    uint32_t eventFieldAddress(ee::Ptr<AnimationEvent> event, uint32_t fieldId); // (animation_queries.cpp)

    namespace
    {
        // The hashes of the nodes whose shown state an event changed: a hash is added the first time and taken
        // out the next (a free place is the last one free). Nothing reads them. Full, the hash goes one past the
        // end, into the particle events' list.
        constexpr uint32_t kToggledNodeHashes = 0x200;
        inline ee::Global<uint32_t[kToggledNodeHashes], sym::bmx::g_eventToggledNodeHashes> g_eventToggledNodeHashes;
        // The ids of the objects PARTICLE events have reached, in order, with no end: nothing reads them, and
        // nothing bounds them.
        constexpr uint32_t kParticleEventObjects = sym::bmx::g_particleEventObjects;
        inline ee::Global<uint32_t, sym::bmx::g_particleEventObjectCount> g_particleEventObjectCount;

        // The particle emitter's rate message (particle_emitter_objects.cpp's): the data points to a float,
        // particles a second.
        constexpr uint32_t kEmitterRateMessage = 0xF1;
        constexpr uint32_t kFieldNameBytes = 0x40;

        // A field's id: the hash of its name, "OBJECT0" .. "OBJECT8".
        inline uint32_t fieldId(ee::Local<char[kFieldNameBytes]> &name, uint32_t format, uint32_t field)
        {
            formatString(name, format, field);
            return nameHash(name);
        }
    }

    // eventObjectNode: the node of the level object of that id, else the world node of that name hash.
    inline ee::Ptr<Node> eventObjectNode(uint32_t id)
    {
        ee::Local<uint32_t> ids;
        *ids = id;
        const ee::Ptr<ee::Ptr<SceneObject>> objects = findObjects(ids, 1);
        if (!objects)
            return findWorldNodeByHash(id, 0, 1);
        return objectLevelNode(objects[0]);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, eventObjectNode);

    // eventSceneObject: the level object of that id, or 0.
    inline ee::Ptr<SceneObject> eventSceneObject(uint32_t id)
    {
        ee::Local<uint32_t> ids;
        *ids = id;
        const ee::Ptr<ee::Ptr<SceneObject>> objects = findObjects(ids, 1);
        if (!objects)
            return ee::Ptr<SceneObject>();
        return objects[0];
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, eventSceneObject);

    inline void toggleEventNodeHash(uint32_t hash)
    {
        uint32_t freePlace = kToggledNodeHashes;
        uint32_t i = 0;
        for (; i < kToggledNodeHashes; ++i)
        {
            const uint32_t held = (*g_eventToggledNodeHashes)[i];
            if (held == 0)
                freePlace = i;
            else if (held == hash)
            {
                (*g_eventToggledNodeHashes)[i] = 0;
                return;
            }
        }
        const ee::Ptr<uint32_t> hashes(&(*g_eventToggledNodeHashes)[0]);
        hashes[freePlace] = hash;
    }
    READABLE_FUNCTION_ONLY(bmx, toggleEventNodeHash);

    // noteNodeShownChange (a visit): a node about to be set to the other shown state noted by its hash.
    inline int32_t noteNodeShownChange(ee::Ptr<Node> node, ee::Ptr<uint32_t> shown)
    {
        const uint32_t wanted = *shown;
        if (nodeShown(node) != wanted)
            toggleEventNodeHash(node->nameHash);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, noteNodeShownChange);

    inline void recordParticleEventObject(uint32_t id)
    {
        const uint32_t count = *g_particleEventObjectCount;
        *g_particleEventObjectCount = count + 1;
        const ee::Ptr<uint32_t> objects(kParticleEventObjects);
        objects[count] = id;
    }
    READABLE_FUNCTION_ONLY(bmx, recordParticleEventObject);

    // setEventObjectsShown: each object field's node tree shown or hidden (its nodes that change noted), the
    // level object too when there is one, and its collisions on or off with it.
    inline void setEventObjectsShown(ee::Ptr<AnimationEvent> event, uint32_t shown)
    {
        ee::Local<char[kFieldNameBytes]> name;
        ee::Local<uint32_t> wanted;
        *wanted = shown;
        for (uint32_t field = 0; field < kEventObjectFields;)
        {
            name[0] = 0;
            const uint32_t id = eventInt(event, fieldId(name, sym::bmx::objectFieldFormat, field++));
            if (!id)
                continue;
            const ee::Ptr<Node> node = eventObjectNode(id);
            if (!node)
                continue;
            const ee::Ptr<SceneObject> object = eventSceneObject(id);
            forEachNodeInSubtree(node, sym::bmx::noteNodeShownChange, wanted.address());
            setNodeShown(node, *wanted, 1);
            if (object)
                setSceneObjectShown(object, *wanted);
            setNodeCollides(node, *wanted, 1);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setEventObjectsShown);

    inline int32_t setObjectShownVisit(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> shown)
    {
        setSceneObjectShown(object, *shown);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setObjectShownVisit);

    // setEventCategoriesShown: every object of each category field's name shown or hidden.
    inline void setEventCategoriesShown(ee::Ptr<AnimationEvent> event, uint32_t shown)
    {
        ee::Local<char[kFieldNameBytes]> name;
        ee::Local<uint32_t> wanted;
        *wanted = shown;
        for (uint32_t field = 0; field < kEventObjectFields;)
        {
            name[0] = 0;
            const uint32_t category = eventFieldAddress(event, fieldId(name, sym::bmx::categoryFieldFormat, field++));
            if (category)
                forEachObjectNamed(category, sym::bmx::setObjectShownVisit, wanted.address());
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setEventCategoriesShown);

    inline void hideObjectCommand(ee::Ptr<SceneObject> /*object: not read*/, ee::Ptr<AnimationEvent> event) { game::tail(setEventObjectsShown, event, 0); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, hideObjectCommand);
    inline void showObjectCommand(ee::Ptr<SceneObject> /*object: not read*/, ee::Ptr<AnimationEvent> event) { game::tail(setEventObjectsShown, event, 1); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, showObjectCommand);
    inline void hideCategoryCommand(ee::Ptr<SceneObject> /*object: not read*/, ee::Ptr<AnimationEvent> event)
    {
        game::tail(setEventCategoriesShown, event, 0);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, hideCategoryCommand);
    inline void showCategoryCommand(ee::Ptr<SceneObject> /*object: not read*/, ee::Ptr<AnimationEvent> event)
    {
        game::tail(setEventCategoriesShown, event, 1);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, showCategoryCommand);

    inline void sendMessageToEventObjects(ee::Ptr<AnimationEvent> event, ee::Ptr<EngineMessage> message)
    {
        ee::Local<uint32_t[kEventObjectFields]> ids;
        ee::Local<char[kFieldNameBytes]> name;
        uint32_t count = 0;
        for (uint32_t field = 0; field < kEventObjectFields; ++field)
        {
            const uint32_t id = eventInt(event, fieldId(name, sym::bmx::objectFieldFormat, field));
            if (id)
                ids[count++] = id;
        }
        if (count == 0)
            return;
        const ee::Ptr<ee::Ptr<SceneObject>> objects = findObjects(ids, count);
        if (!objects)
            return;
        for (uint32_t i = 0; objects[i]; ++i)
        {
            message->target = objects[i];
            sendEngineMessage(message);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, sendMessageToEventObjects);

    // particleCommand: each object field's emitter (a scene object by id) sent its RATE field as its particles a
    // second, and its id recorded.
    inline void particleCommand(ee::Ptr<SceneObject> /*object: not read*/, ee::Ptr<AnimationEvent> event)
    {
        ee::Local<char[kFieldNameBytes]> name;
        ee::Local<ee::Float> rate;
        for (uint32_t field = 0; field < kEventObjectFields;)
        {
            *rate = eventFloat(event, fieldId(name, sym::bmx::rateFieldFormat, field));
            const uint32_t id = eventInt(event, fieldId(name, sym::bmx::objectFieldFormat, field++));
            if (!id)
                continue;
            const uint32_t emitter = findSceneObject(id, 0, 0);
            if (!emitter)
                continue;
            sendNewMessage(0, emitter, kEmitterRateMessage, rate.address(), 1);
            recordParticleEventObject(id);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, particleCommand);
}
