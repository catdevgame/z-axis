// The animated objects: the "AnimObject" level objects, each playing one of
// the rider motions over and over (its tag picks it, from 0x208 on), its
// time kept in its tags so a saved level state holds it. Their type's
// registration (which also hooks the motions' events), the objects' set-up
// and their messages. DM2's own code (its game block, the level objects).

#include "dynamics_objects.h"
#include "object_node.h"

#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/anim_objects.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kAnimObjectTypeName{sym::animObjectTypeName}; // "AnimObject"
        // The event kinds the hook compares (an event's first field: its
        // kind's name hash).
        constexpr ee::Ptr<const char> kTriggerEventName{sym::triggerEventName};             // "TRIGGER"
        constexpr ee::Ptr<const char> kInvisibilityEventName{sym::invisibilityEventName};   // "INVISIBILITY"
        constexpr ee::Ptr<const char> kCollidibilityEventName{sym::collidibilityEventName}; // "COLLIDIBILITY"

        // The object's component: its motion's time (a float at 4) and which
        // motion it plays (byte 8: the rider motions from 0x208).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kAnimTagTime = 4, kAnimTagMotion = 8;
        constexpr uint32_t kFirstObjectMotion = 0x208;
        // The activate message (goals.cpp's kActivateMessage).
        constexpr uint32_t kActivateMessage = 0x100;

        // An object's state as animObjectState hands it on: one record for
        // them all, filled from the asking object's tags.
        struct AnimObjectState
        {
            // Scaffolding (unnamed-field): nothing writes it; a save copies it
            // as it stands.
            uint32_t _00;
            ee::Float time; // +0x04
            uint8_t motion; // +0x08
        };
        static_assert(offsetof(AnimObjectState, time) == 4 && offsetof(AnimObjectState, motion) == 8);
        constexpr uint32_t kAnimObjectStateBytes = 0x0C;
        inline ee::Global<AnimObjectState, sym::g_animObjectState> g_animObjectState;
        // Set as an object is made: the next frame of any (the first) starts
        // its motion.
        inline ee::Global<uint32_t, sym::g_animObjectsStarting> g_animObjectsStarting;
        // A frame's share of a second (1 / 60).
        inline ee::Global<ee::Float, sym::g_animObjectFrameSeconds> g_animObjectFrameSeconds;

        // What an animated object keeps in a saved level state.
        struct AnimObjectSave
        {
            zge::Vec4 position;           // +0x00
            zge::Quaternion orientation;  // +0x10
            AnimObjectState state;        // +0x20
        };
        static_assert(offsetof(AnimObjectSave, state) == 0x20 && sizeof(AnimObjectSave) == 0x30);
        constexpr uint32_t kVectorBytes = 0x10;
    }

    // registerAnimObjectType: the "AnimObject" level objects made animated
    // objects, and the motions' events hooked (animObjectEventHook). The
    // registration's answer (the hook's setter leaves it).
    uint32_t registerAnimObjectType()
    {
        const uint32_t registered = zge::registerEntityInit(kAnimObjectTypeName, sym::newAnimObject);
        game::tail(zge::setAnimationEventHook, sym::animObjectEventHook);
        return registered;
    }
    READABLE_FUNCTION(registerAnimObjectType, Verified);

    // animObjectState: the object's state, in the one shared record.
    ee::Ptr<AnimObjectState> animObjectState(ee::Ptr<zge::SceneObject> object)
    {
        g_animObjectState->motion = static_cast<uint8_t>(zge::tagByte(zge::objectComponent(object), kTagByte, kAnimTagMotion));
        g_animObjectState->time = zge::tagFloat(zge::objectComponent(object), kTagWord, kAnimTagTime);
        return &*g_animObjectState;
    }
    READABLE_FUNCTION(animObjectState, Verified);

    // setAnimObjectState: a state written back to the object's tags.
    void setAnimObjectState(ee::Ptr<zge::SceneObject> object, ee::Ptr<AnimObjectState> state)
    {
        zge::setTagByte(zge::objectComponent(object), kTagByte, kAnimTagMotion, state->motion);
        game::tail(zge::setTagFloat, zge::objectComponent(object), kTagWord, kAnimTagTime, state->time);
    }
    READABLE_FUNCTION(setAnimObjectState, Verified);

    // newAnimObject: the object made an animated object: its node looked up
    // and its state read (both unused), an animator made if it has none; its
    // touch, activate, save-size, save and restore messages; its pose put
    // back to its rest, the next frame told to start the motion, its frame
    // message.
    int32_t newAnimObject(ee::Ptr<zge::SceneObject> object)
    {
        objectNodeHandle(object);
        animObjectState(object);
        if (!object->animator)
            zge::createObjectAnimator(object);
        zge::subscribe(object, kTouchMessage, zge::MessageHandler(sym::animObjectTouched));
        zge::subscribe(object, kActivateMessage, zge::MessageHandler(sym::animObjectActivated));
        zge::subscribe(object, zge::kSaveSizeMessage, zge::MessageHandler(sym::animObjectSaveSize));
        zge::subscribe(object, zge::kSaveMessage, zge::MessageHandler(sym::saveAnimObject));
        zge::subscribe(object, zge::kRestoreMessage, zge::MessageHandler(sym::restoreAnimObject));
        zge::resetAnimatorPose(object); // the model's pose back at its rest
        *g_animObjectsStarting = 1;
        zge::subscribe(object, zge::kUpdateMessage, zge::MessageHandler(sym::animObjectFrame));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newAnimObject, Verified);

    // animObjectActivated, animObjectTouched (the activate and touch
    // messages): nothing.
    int32_t animObjectActivated(ee::Ptr<zge::EngineMessage>)
    {
        return 1;
    }
    READABLE_FUNCTION(animObjectActivated, Draft);

    int32_t animObjectTouched(ee::Ptr<zge::EngineMessage>)
    {
        return 1;
    }
    READABLE_FUNCTION(animObjectTouched, Verified);

    // animObjectFrame (the update message): its time on a frame; the first
    // frame after an object is made starts its motion; a motion that has
    // run out starts again from 0 (its pose put back first); the motion run
    // to the time, kept, posed.
    int32_t animObjectFrame(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<AnimObjectState> state = animObjectState(object);
        state->time = state->time + *g_animObjectFrameSeconds;
        if (*g_animObjectsStarting != 0)
        {
            if (rider::riderMotion(state->motion + kFirstObjectMotion))
                zge::startAnimation(object, rider::riderMotion(state->motion + kFirstObjectMotion), state->time);
            *g_animObjectsStarting = 0;
        }
        if (zge::animationDone(object, state->time))
        {
            zge::resetAnimatorPose(object);
            state->time = ee::Float(0.0f);
            zge::startAnimation(object, rider::riderMotion(state->motion + kFirstObjectMotion), state->time);
        }
        zge::updateMotionTracks(object, state->time); // (the hook called at each event passed)
        setAnimObjectState(object, state);
        zge::updatePose(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(animObjectFrame, Verified);

    // animObjectSaveSize (the save-size message): an animated object's save
    // is 48 bytes.
    int32_t animObjectSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(AnimObjectSave);
        return 1;
    }
    READABLE_FUNCTION(animObjectSaveSize, Verified);

    // saveAnimObject (the save message): its state, where it stands and how
    // it is turned.
    int32_t saveAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<AnimObjectSave> save(message->data);
        zge::memoryCopy(&save->state, animObjectState(ee::Ptr<zge::SceneObject>(message->target)), kAnimObjectStateBytes);
        zge::memoryCopy(save, zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)), kVectorBytes);
        zge::memoryCopy(&save->orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)),
                        kVectorBytes);
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveAnimObject, Verified);

    // restoreAnimObject (the restore message): the saved state written back;
    // the object put where it already stands, turned as it already is (a
    // copy of its own place and turn: the saved ones are not read), its
    // pose back to its rest.
    int32_t restoreAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Quaternion> orientation;
        const ee::Ptr<zge::SceneObject> object(message->target);
        objectNodeHandle(object);
        const ee::Ptr<AnimObjectSave> save(message->data);
        setAnimObjectState(object, &save->state);
        zge::memoryCopy(position, zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)), kVectorBytes);
        zge::memoryCopy(orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)), kVectorBytes);
        zge::setObjectPosition(object, position);
        zge::setObjectOrientation(object, orientation);
        zge::resetAnimatorPose(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreAnimObject, Verified);

    // animObjectEventHook (the motions' event hook, registerAnimObjectType's;
    // the object and the event its motion passed): for an animated object,
    // its state read and the event's kind (its first field) compared with
    // "TRIGGER", then "INVISIBILITY", then "COLLIDIBILITY"'s hashes, nothing
    // done on any (the answers are unused: an empty hook).
    void animObjectEventHook(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::AnimationEvent> event)
    {
        if (zge::levelObjectTypeHash(object) != zge::nameHash(&*kAnimObjectTypeName))
            return;
        animObjectState(object);
        const ee::Ptr<uint32_t> kind(event->record);
        if (*kind == zge::nameHash(&*kTriggerEventName))
            return;
        const ee::Ptr<uint32_t> kindAgain(event->record);
        if (*kindAgain == zge::nameHash(&*kInvisibilityEventName))
            return;
        zge::nameHash(&*kCollidibilityEventName);
    }
    READABLE_FUNCTION_WAITS(animObjectEventHook, Draft);
}
