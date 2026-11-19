// BMX XXX's "AnimNPC" and "AnimObj" level object types (one init for both):
// an object that plays an animation by name when told to. BMX XXX's own code
// (its game block); no other build names its functions, so the names are
// from what each does and the engine calls it makes (pauseAnimation,
// resumeAnimation). The workdb paired two of them with Aggressive Inline's
// carVelocityMessage and hideDynamicsObject by shape alone: they are
// animObjectRecord and disableAnimObject here.
//
// What an object plays is kept in its own tag data (the animation's name and
// whether it plays), so a saved level state can hold it.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/anim_objects.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        // The messages an animated object takes beside entity_messages.h's.
        // The data is the animation's name (text).
        constexpr uint32_t kPlayAnimationMessage = 0x108;

        enum AnimObjectState : uint8_t
        {
            kAnimNone = 0,
            kAnimPlaying = 1,
            kAnimPaused = 2,
        };

        // What the object plays: in its own tag data (objectComponent), after
        // the first word.
        struct AnimObjectRecord
        {
            char animation[0x40]; // +0x00 the animation's name, empty for none
            uint8_t state;        // +0x40 AnimObjectState
        };
        static_assert(offsetof(AnimObjectRecord, state) == 0x40 && sizeof(AnimObjectRecord) == 0x41);
        constexpr uint32_t kRecordTag = 4;

        // What an animated object keeps in a saved level state.
        struct AnimObjectSave
        {
            ee::Float time;          // +0x00 the animation's time (written only while one plays)
            AnimObjectRecord record; // +0x04
            uint8_t _45[3];
        };
        static_assert(offsetof(AnimObjectSave, record) == 4 && sizeof(AnimObjectSave) == 0x48);

        constexpr uint32_t kAnimNpcTypeName = sym::animNpcTypeName; // "AnimNPC"
        constexpr uint32_t kAnimObjTypeName = sym::animObjTypeName; // "AnimObj"
        constexpr uint32_t kCreateEntity = sym::createEntity;
    }

    ee::Ptr<AnimObjectRecord> animObjectRecord(ee::Ptr<zge::SceneObject> object);
    uint32_t initAnimObject(ee::Ptr<zge::SceneObject> object);
    void playAnimObjectAnimation(ee::Ptr<zge::SceneObject> object, ee::Address name);
    void pauseAnimObject(ee::Ptr<zge::SceneObject> object);
    void resumeAnimObject(ee::Ptr<zge::SceneObject> object);

    // animObjectBlendHook (the played track's blend hook, TrackHooks'
    // onBlend): once the track's blend has ended (weight -1) the object
    // plays nothing any more.
    void animObjectBlendHook(ee::Ptr<zge::SceneObject> object, uint32_t /*track*/, uint32_t /*context*/, ee::Float weight)
    {
        if (weight == ee::Float(-1.0f))
        {
            const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
            record->animation[0] = 0;
            record->state = kAnimNone;
        }
    }
    READABLE_FUNCTION(animObjectBlendHook, Draft);

    // registerAnimNpcType, registerAnimObjType: the "AnimNPC" and "AnimObj"
    // types, made as plain entities and set up by initAnimObject.
    uint32_t registerAnimNpcType()
    {
        return game::tail(zge::registerLevelObjectType, kAnimNpcTypeName, kCreateEntity, static_cast<uint32_t>(sym::initAnimObject));
    }
    READABLE_FUNCTION(registerAnimNpcType, Verified);

    uint32_t registerAnimObjType()
    {
        return game::tail(zge::registerLevelObjectType, kAnimObjTypeName, kCreateEntity, static_cast<uint32_t>(sym::initAnimObject));
    }
    READABLE_FUNCTION(registerAnimObjType, Verified);

    // animObjectRecord: what the object plays (in its own tag data).
    ee::Ptr<AnimObjectRecord> animObjectRecord(ee::Ptr<zge::SceneObject> object)
    {
        return zge::tagFieldAs<AnimObjectRecord>(zge::objectComponent(object), kRecordTag);
    }
    READABLE_FUNCTION(animObjectRecord, Verified);

    // initAnimObject (both types' init): its messages subscribed; riders
    // pass through it. Always 1.
    uint32_t initAnimObject(ee::Ptr<zge::SceneObject> object)
    {
        zge::subscribe(object, kPlayAnimationMessage, zge::MessageHandler(sym::animObjectPlayMessage));
        zge::subscribe(object, kEnableMessage, zge::MessageHandler(sym::enableAnimObject));
        zge::subscribe(object, kDisableMessage, zge::MessageHandler(sym::disableAnimObject));
        zge::subscribe(object, kPauseAnimationMessage, zge::MessageHandler(sym::animObjectPauseMessage));
        zge::subscribe(object, kResumeAnimationMessage, zge::MessageHandler(sym::animObjectResumeMessage));
        zge::subscribe(object, zge::kSaveSizeMessage, zge::MessageHandler(sym::animObjectSaveSize));
        zge::subscribe(object, zge::kSaveMessage, zge::MessageHandler(sym::saveAnimObject));
        zge::subscribe(object, zge::kRestoreMessage, zge::MessageHandler(sym::restoreAnimObject));
        zge::subscribe(object, zge::kRemovedMessage, zge::MessageHandler(sym::animObjectRemoved));
        rider::collision::setNodeCollidable(objectNodeHandle(object), 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initAnimObject, Verified);

    // animObjectPlayMessage (kPlayAnimationMessage): the named animation played.
    int32_t animObjectPlayMessage(ee::Ptr<zge::EngineMessage> message)
    {
        playAnimObjectAnimation(ee::Ptr<zge::SceneObject>(message->target), message->data);
        return 0;
    }
    READABLE_FUNCTION_WAITS(animObjectPlayMessage, Draft);

    // playAnimObjectAnimation: what the object played is dropped (its
    // animator freed, its record emptied), then the named animation, if the
    // libraries in use have it, is played from its start and the record
    // keeps its name. A name that is the object's own record (the enable
    // and restore handlers pass it) has been emptied by then when the
    // record said it played: nothing is found and nothing plays, as in the
    // original.
    void playAnimObjectAnimation(ee::Ptr<zge::SceneObject> object, ee::Address name)
    {
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        if (record->state != kAnimNone)
        {
            zge::freeAnimatorOf(object);
            record->state = kAnimNone;
            record->animation[0] = 0;
        }
        const ee::Ptr<zge::Motion> motion = zge::findAnimationInLibraries(name);
        if (motion)
        {
            zge::playAnimationWithHooks(object, motion, 0, sym::animObjectBlendHook, 0, ee::Float(0.0f));
            record->state = kAnimPlaying;
            zge::stringCopy(record, name);
        }
    }
    READABLE_FUNCTION_WAITS(playAnimObjectAnimation, Draft);

    // animObjectPauseMessage (kPauseAnimationMessage).
    int32_t animObjectPauseMessage(ee::Ptr<zge::EngineMessage> message)
    {
        pauseAnimObject(ee::Ptr<zge::SceneObject>(message->target));
        return 0;
    }
    READABLE_FUNCTION_WAITS(animObjectPauseMessage, Verified);

    // pauseAnimObject: a playing object's animation paused. The original
    // stores the state it read, playing, back into the record, so the
    // record never says paused: resumeAnimObject, which wants a paused
    // record, then does nothing for it.
    void pauseAnimObject(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        const uint8_t state = record->state;
        if (state == kAnimPlaying)
        {
            zge::pauseAnimation(object);
            record->state = state;
        }
    }
    READABLE_FUNCTION_WAITS(pauseAnimObject, Verified);

    // animObjectResumeMessage (kResumeAnimationMessage).
    int32_t animObjectResumeMessage(ee::Ptr<zge::EngineMessage> message)
    {
        resumeAnimObject(ee::Ptr<zge::SceneObject>(message->target));
        return 0;
    }
    READABLE_FUNCTION_WAITS(animObjectResumeMessage, Verified);

    // resumeAnimObject: a paused object's animation runs again.
    void resumeAnimObject(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        if (record->state == kAnimPaused)
        {
            zge::resumeAnimation(object);
            record->state = kAnimPlaying;
        }
    }
    READABLE_FUNCTION_WAITS(resumeAnimObject, Verified);

    // animObjectSaveSize (the engine's save size message).
    int32_t animObjectSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(AnimObjectSave);
        return 0;
    }
    READABLE_FUNCTION(animObjectSaveSize, Verified);

    // saveAnimObject (the engine's save message): the animation's time while
    // one plays, and the record.
    int32_t saveAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<AnimObjectSave> saved(message->data);
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        if (record->state != kAnimNone)
            zge::animationTime(object, &saved->time);
        zge::memoryCopy(&saved->record, record, sizeof(AnimObjectRecord));
        return 0;
    }
    READABLE_FUNCTION_WAITS(saveAnimObject, Verified);

    // restoreAnimObject (the engine's restore message): what it played is
    // dropped and the saved record taken; a record that says it plays is
    // played again from its own name (see playAnimObjectAnimation: that
    // empties the name first), set to the saved time and paused if it was
    // saved paused.
    int32_t restoreAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<AnimObjectSave> saved(message->data);
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        if (record->state != kAnimNone)
            zge::freeAnimatorOf(object);
        zge::memoryCopy(record, &saved->record, sizeof(AnimObjectRecord));
        if (record->state != kAnimNone)
        {
            playAnimObjectAnimation(object, record);
            zge::setAnimationTime(object, saved->time);
            if (saved->record.state == kAnimPaused)
                pauseAnimObject(object);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreAnimObject, Verified);

    // animObjectRemoved (the engine's removed message): its animator freed.
    int32_t animObjectRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        if (animObjectRecord(object)->state != kAnimNone)
            zge::freeAnimatorOf(object);
        return 0;
    }
    READABLE_FUNCTION_WAITS(animObjectRemoved, Draft);

    // enableAnimObject (kEnableMessage): shown, its record's animation
    // played again, and the record says it plays whether one was found or not.
    int32_t enableAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        zge::setSceneObjectShown(object, 1);
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        playAnimObjectAnimation(object, record);
        record->state = kAnimPlaying;
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableAnimObject, Draft);

    // disableAnimObject (kDisableMessage): its animator freed, playing
    // nothing (the record keeps the name), hidden.
    int32_t disableAnimObject(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> object(message->target);
        const ee::Ptr<AnimObjectRecord> record = animObjectRecord(object);
        zge::freeAnimatorOf(object);
        record->state = kAnimNone;
        zge::setSceneObjectShown(object, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableAnimObject, Draft);
}
