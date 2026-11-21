// BMX XXX's "Pickup" level object type: something a rider takes by riding
// close to it. BMX XXX's own code (its game block); no other build names its
// functions, so the names are from what each does and from the type's own
// strings ("Pickup", "%s.%s.idle", "%s.%s.activate") and the script commands
// that reach it (entity_messages.h).
//
// A pickup is in one of six states, kept in its own tag data with the time
// the state's animation has reached. Its class may name one animation; three
// of that animation's events, found at the first init, say where each
// animated state starts in it:
//   enabling  the animation from its enable event; then idle
//   idle      the animation looped from its start; riders can take it
//   activating  the animation from its activate event; then taken
//   taken     hidden, nothing runs
//   disabling the animation from its disable event; then disabled
//   disabled  hidden, nothing runs
// A class with no animation shows or hides the pickup at once.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/pickups.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        enum PickupState : uint32_t
        {
            kPickupEnabling = 0,
            kPickupIdle = 1,
            kPickupActivating = 2,
            kPickupTaken = 3,
            kPickupDisabling = 4,
            kPickupDisabled = 5,
            kPickupStates = 6,
        };

        // The pickup's own tag data (objectComponent).
        constexpr uint32_t kStateTag = 0;     // byte: its PickupState
        constexpr uint32_t kTimeTag = 4;      // float: the animation's time
        constexpr uint32_t kSoundTag = 8;     // word: its idle sound's handle
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kAnimationNameTag = 4;  // word: the animation's name hash, 0 for none
        constexpr uint32_t kRadiusTag = 8;         // float: how close a rider takes it
        constexpr uint32_t kSoundNameTag = 0xC;    // text: the middle of its sounds' names
        constexpr uint32_t kMotionTag = 0x4C;      // word: the animation, once found
        constexpr uint32_t kActivateTimeTag = 0x50;  // float: where the activating state starts in it
        constexpr uint32_t kEnableTimeTag = 0x54;    // float: where the enabling state starts
        constexpr uint32_t kDisableTimeTag = 0x58;   // float: where the disabling state starts
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kTagText = 0x10;

        constexpr uint32_t kTypeName = sym::pickupTypeName;                     // "Pickup"
        constexpr uint32_t kIdleSoundFormat = sym::bmx::pickupIdleSoundFormat;         // "%s.%s.idle"
        constexpr uint32_t kActivateSoundFormat = sym::bmx::pickupActivateSoundFormat; // "%s.%s.activate"
        // The sets of the three events that mark the animation, in the order
        // they come in it: the activate, the enable and the disable event.
        inline ee::Global<uint32_t[3], sym::pickupEventSets> g_pickupEventSets;
        // A frame's share of a second (1 / 60): what the animation's time grows by each update.
        inline ee::Global<ee::Float, sym::g_pickupFrameSeconds> g_pickupFrameSeconds;

        // A rider's flag byte 0: both bits are read as one value here.
        // Scaffolding (unnamed-field): bit 0x02 has no name yet; riders take
        // pickups only with it set and kFlagNoPlayer clear.
        constexpr uint8_t kRiderFlag02 = 0x02;

        constexpr zge::MessageHandler kUpdatePickup(sym::updatePickup);
        constexpr uint32_t kCreateEntity = sym::createEntity;
    }

    uint32_t initPickup(ee::Ptr<zge::SceneObject> pickup);
    int32_t pickupRemoved(ee::Ptr<zge::EngineMessage> message);
    int32_t activatePickup(ee::Ptr<zge::EngineMessage> message);
    int32_t pickupIsEnabled(ee::Ptr<zge::EngineMessage> message);
    int32_t enablePickup(ee::Ptr<zge::EngineMessage> message);
    int32_t disablePickup(ee::Ptr<zge::EngineMessage> message);
    int32_t updatePickup(ee::Ptr<zge::EngineMessage> message);
    int32_t pickupSaveSize(ee::Ptr<zge::EngineMessage> message);
    int32_t restorePickup(ee::Ptr<zge::EngineMessage> message);
    int32_t savePickup(ee::Ptr<zge::EngineMessage> message);
    uint32_t pickupState(ee::Ptr<zge::SceneObject> pickup);
    void setPickupState(ee::Ptr<zge::SceneObject> pickup, uint32_t state);
    uint32_t pickupStateTime(ee::Ptr<zge::SceneObject> pickup, uint32_t state, ee::Ptr<ee::Float> timeOut);
    uint32_t playPickupAnimation(ee::Ptr<zge::SceneObject> pickup, uint32_t restart, ee::Float time);
    uint32_t riderReachesPickup(uint32_t riderIndex, ee::Ptr<zge::SceneObject> pickup);

    // registerPickupType: the "Pickup" type, made as a plain entity and set
    // up by initPickup.
    uint32_t registerPickupType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initPickup));
    }
    READABLE_FUNCTION(registerPickupType, Verified);

    // initPickup (the type's init): its messages subscribed; riders pass
    // through it; its class's animation looked up once, with the times of its
    // three marking events; its idle sound started; disabled until the level
    // enables it. Always 1.
    uint32_t initPickup(ee::Ptr<zge::SceneObject> pickup)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        ee::Local<uint32_t[3]> eventSets;
        for (uint32_t i = 0; i < 3; ++i)
            eventSets[i] = (*g_pickupEventSets)[i];

        zge::subscribe(pickup, zge::kRemovedMessage, zge::MessageHandler(sym::pickupRemoved));
        zge::subscribe(pickup, zge::kSaveSizeMessage, zge::MessageHandler(sym::pickupSaveSize));
        zge::subscribe(pickup, zge::kSaveMessage, zge::MessageHandler(sym::savePickup));
        zge::subscribe(pickup, zge::kRestoreMessage, zge::MessageHandler(sym::restorePickup));
        zge::subscribe(pickup, kActivateMessage, zge::MessageHandler(sym::activatePickup));
        zge::subscribe(pickup, kIsEnabledMessage, zge::MessageHandler(sym::pickupIsEnabled));
        zge::subscribe(pickup, kEnableMessage, zge::MessageHandler(sym::enablePickup));
        zge::subscribe(pickup, kDisableMessage, zge::MessageHandler(sym::disablePickup));
        rider::collision::setNodeCollidable(objectNodeHandle(pickup), 0);

        if (zge::tagWord(zge::levelObjectClassTag(pickup), kTagWord, kMotionTag) == 0)
        {
            const uint32_t animationName = static_cast<uint32_t>(zge::tagWord(zge::levelObjectClassTag(pickup), kTagWord, kAnimationNameTag));
            if (animationName != 0)
            {
                const ee::Ptr<zge::Motion> motion = zge::findLibraryAnimation(animationName);
                if (motion)
                {
                    // The marking events, in the animation's order. An
                    // animation with more than three matches is compared on
                    // with whatever follows the three sets in the frame, as
                    // the original's loop is.
                    uint32_t found = 0;
                    for (ee::Ptr<zge::AnimationEvent> event = zge::motionEvents(motion); event; event = event->next)
                    {
                        if (event->set != eventSets[found])
                            continue;
                        const ee::Float time = zge::eventTime(event);
                        if (found == 0)
                            zge::setTagFloat(zge::levelObjectClassTag(pickup), kTagWord, kActivateTimeTag, time);
                        else if (found == 1)
                            zge::setTagFloat(zge::levelObjectClassTag(pickup), kTagWord, kEnableTimeTag, time);
                        else if (found == 2)
                            zge::setTagFloat(zge::levelObjectClassTag(pickup), kTagWord, kDisableTimeTag, time);
                        ++found;
                    }
                    zge::setTagWord(zge::levelObjectClassTag(pickup), kTagWord, kMotionTag, motion);
                }
            }
        }

        zge::tagString(zge::levelObjectClassTag(pickup), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kIdleSoundFormat, kTypeName, className.address());
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(pickup);
        const uint32_t sound = soundEvent(kObjectSoundEvent, position, kObjectSoundStart, soundName.address(), kFullVolume);
        zge::setTagWord(zge::objectComponent(pickup), kTagWord, kSoundTag, sound);
        setPickupState(pickup, kPickupDisabled);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initPickup, Verified);

    // pickupRemoved (the engine's removed message): its idle sound stopped.
    int32_t pickupRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> pickup(message->target);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(pickup), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(pickup), kObjectSoundStop, sound, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(pickupRemoved, Verified);

    // activatePickup (kActivateMessage): taken. It plays its activation
    // (the activating state and its "activate" sound), or with the event's
    // first word set is taken at once.
    int32_t activatePickup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const ee::Ptr<zge::SceneObject> pickup(message->target);
        // The activating rider is looked up and not used.
        rider::riderOf(rider::playerIndexOfNode(message->sender));
        if (ee::Ptr<ActivationEvent>(message->data)->quiet == 0)
        {
            setPickupState(pickup, kPickupActivating);
            zge::tagString(zge::levelObjectClassTag(pickup), kTagText, kSoundNameTag, className.address());
            zge::formatString(soundName, kActivateSoundFormat, kTypeName, className.address());
            const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
            soundEvent(kObjectSoundEvent, position, kObjectSoundOnce, soundName.address(), kFullVolume);
        }
        else
        {
            setPickupState(pickup, kPickupTaken);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(activatePickup, Verified);

    // pickupIsEnabled (kIsEnabledMessage): answers whether it is there to
    // take (enabling or idle).
    int32_t pickupIsEnabled(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t state = pickupState(ee::Ptr<zge::SceneObject>(message->target));
        *ee::Ptr<uint32_t>(message->data) = state < kPickupActivating;
        return 0;
    }
    READABLE_FUNCTION(pickupIsEnabled, Verified);

    // enablePickup (kEnableMessage).
    int32_t enablePickup(ee::Ptr<zge::EngineMessage> message)
    {
        setPickupState(ee::Ptr<zge::SceneObject>(message->target), kPickupEnabling);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enablePickup, Verified);

    // disablePickup (kDisableMessage).
    int32_t disablePickup(ee::Ptr<zge::EngineMessage> message)
    {
        setPickupState(ee::Ptr<zge::SceneObject>(message->target), kPickupDisabling);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disablePickup, Verified);

    // updatePickup (the frame's update, in the four states that run): every
    // rider that reaches it activates it; the state's animation moves on a
    // frame, a finished state giving way to the next and the idle one looping
    // from its start; its idle sound follows it.
    int32_t updatePickup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ee::Float> time;
        const ee::Ptr<zge::SceneObject> pickup(message->target);
        const uint32_t state = pickupState(pickup) & 0xFFu;
        rider::forEachRider(rider::RiderVisit(sym::riderReachesPickup), pickup);
        *time = zge::tagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag) + *g_pickupFrameSeconds;
        switch (state)
        {
        case kPickupEnabling:
        case kPickupActivating:
        case kPickupDisabling:
            if (playPickupAnimation(pickup, 0, *time) != 0)
                setPickupState(pickup, state + 1);
            break;
        case kPickupIdle:
            if (playPickupAnimation(pickup, 0, *time) != 0)
                pickupStateTime(pickup, state, time);
            break;
        case kPickupTaken:
        case kPickupDisabled:
            // A hidden pickup still subscribed (a rider took it this frame): nothing more.
            return 0;
        default:
            break;
        }
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(pickup), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(pickup), kObjectSoundMove, sound, kFullVolume);
        zge::setTagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag, *time);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updatePickup, Verified);

    namespace
    {
        // What a pickup keeps in a saved level state.
        struct PickupSave
        {
            uint32_t state; // +0x00
            ee::Float time; // +0x04
        };
        static_assert(sizeof(PickupSave) == 8);
    }

    // pickupSaveSize (the engine's save size message).
    int32_t pickupSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(PickupSave);
        return 0;
    }
    READABLE_FUNCTION(pickupSaveSize, Verified);

    // restorePickup (the engine's restore message): the saved state entered
    // and one update run at the saved time, which then stands.
    int32_t restorePickup(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::EngineMessage> update;
        const ee::Ptr<PickupSave> saved(message->data);
        const ee::Ptr<zge::SceneObject> pickup(message->target);
        setPickupState(pickup, saved->state);
        zge::setTagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag, saved->time);
        update->target = pickup;
        updatePickup(update);
        zge::setTagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag, saved->time);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restorePickup, Verified);

    // savePickup (the engine's save message).
    int32_t savePickup(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> pickup(message->target);
        const ee::Ptr<PickupSave> saved(message->data);
        saved->state = pickupState(pickup);
        saved->time = zge::tagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag);
        return 0;
    }
    READABLE_FUNCTION(savePickup, Verified);

    // pickupState: its PickupState.
    uint32_t pickupState(ee::Ptr<zge::SceneObject> pickup)
    {
        return zge::tagByte(zge::objectComponent(pickup), kTagByte, kStateTag);
    }
    READABLE_FUNCTION(pickupState, Verified);

    // setPickupState: the state entered. A state whose animation has no
    // start time is skipped: enabling shows the pickup idle at once,
    // activating and disabling hide it as taken and disabled. The state's
    // animation starts at its time; the update runs in the animated states
    // and while idle.
    void setPickupState(ee::Ptr<zge::SceneObject> pickup, uint32_t state)
    {
        ee::Local<ee::Float> time;
        zge::tagByte(zge::objectComponent(pickup), kTagByte, kStateTag);
        zge::unsubscribe(pickup, zge::kUpdateMessage, kUpdatePickup);
        if (pickupStateTime(pickup, state, time) == 0)
        {
            switch (state)
            {
            case kPickupEnabling:
                state = kPickupIdle;
                zge::setSceneObjectShown(pickup, 1);
                break;
            case kPickupIdle:
                zge::setSceneObjectShown(pickup, 1);
                break;
            case kPickupActivating:
                state = kPickupTaken;
                zge::setSceneObjectShown(pickup, 0);
                break;
            case kPickupDisabling:
                state = kPickupDisabled;
                zge::setSceneObjectShown(pickup, 0);
                break;
            default:
                zge::setSceneObjectShown(pickup, 0);
                break;
            }
        }
        zge::setTagByte(zge::objectComponent(pickup), kTagByte, kStateTag, state & 0xFFu);
        if (pickupStateTime(pickup, state, time) != 0)
            playPickupAnimation(pickup, 1, *time);
        zge::setTagFloat(zge::objectComponent(pickup), kTagWord, kTimeTag, *time);
        if (state < kPickupTaken || state == kPickupDisabling)
            zge::subscribe(pickup, zge::kUpdateMessage, kUpdatePickup);
    }
    READABLE_FUNCTION_WAITS(setPickupState, Verified);

    // pickupStateTime: where the state's animation starts, into timeOut (0
    // when it has none). 1 when the state is animated from a time of its
    // own, or is the idle state of a class with an animation (from 0); 0
    // for a class with no animation, a hidden state, or an event the
    // animation lacks.
    uint32_t pickupStateTime(ee::Ptr<zge::SceneObject> pickup, uint32_t state, ee::Ptr<ee::Float> timeOut)
    {
        *timeOut = ee::Float(0.0f);
        if (zge::tagWord(zge::levelObjectClassTag(pickup), kTagWord, kMotionTag) == 0 || state >= kPickupStates)
            return 0;
        uint32_t timeTag;
        switch (state)
        {
        case kPickupEnabling:
            timeTag = kEnableTimeTag;
            break;
        case kPickupIdle:
            return 1;
        case kPickupActivating:
            timeTag = kActivateTimeTag;
            break;
        case kPickupDisabling:
            timeTag = kDisableTimeTag;
            break;
        default:
            return 0;
        }
        const ee::Float time = zge::tagFloat(zge::levelObjectClassTag(pickup), kTagWord, timeTag);
        if (time == ee::Float(0.0f))
            return 0;
        *timeOut = time;
        return 1;
    }
    READABLE_FUNCTION(pickupStateTime, Verified);

    // playPickupAnimation: the class's animation at `time`, started there
    // first when `restart` is set. 1 when it is over for the state: the
    // animation has ended, or the time has reached where the next state
    // starts in it. A pickup that was not drawn is not posed.
    uint32_t playPickupAnimation(ee::Ptr<zge::SceneObject> pickup, uint32_t restart, ee::Float time)
    {
        ee::Local<ee::Float> nextStart;
        const ee::Ptr<zge::Motion> motion(static_cast<uint32_t>(zge::tagWord(zge::levelObjectClassTag(pickup), kTagWord, kMotionTag)));
        if (!motion)
            return 0;
        if (restart != 0)
            zge::startAnimation(pickup, motion, time);
        if (zge::animationDone(pickup, time) != 0)
            return 1;
        if (zge::nodeNotDrawn(ee::Ptr<zge::Node>(static_cast<uint32_t>(objectNodeHandle(pickup)))) == 0)
            zge::updateAnimation(pickup, 0, time);
        const uint32_t next = (pickupState(pickup) & 0xFFu) + 1;
        if (pickupStateTime(pickup, next, nextStart) != 0 && *nextStart <= time)
            return 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(playPickupAnimation, Verified);

    // riderReachesPickup (forEachRider's visit; the context is the pickup):
    // a rider within the class's radius of it activates it: the game is
    // asked first, and a pickup the game does not take is activated here,
    // which ends the walk (0). 1: the next rider.
    uint32_t riderReachesPickup(uint32_t riderIndex, ee::Ptr<zge::SceneObject> pickup)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> offset;
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndex & 0xFFu);
        if ((rider->flags[0] & (rider::kFlagNoPlayer.mask | kRiderFlag02)) != kRiderFlag02)
            return 1;
        ee::Float radius = zge::tagFloat(zge::levelObjectClassTag(pickup), kTagWord, kRadiusTag);
        *at = *rider::vec4(rider->position) + *rider::vec4(rider->rootOffset);
        *offset = *at - *zge::objectPosition(pickup);
        const ee::Float distanceSquared = zge::dot(*offset, *offset);
        radius = radius * radius;
        if (!(distanceSquared <= radius))
            return 1;
        zge::memoryFill(event, 0, sizeof(ActivationEvent));
        event->quiet = 0;
        event->player = rider->player;
        activation->target = pickup;
        activation->sender = ee::Address(rider->messenger);
        activation->type = kActivateMessage;
        activation->data = event;
        activation->flags = zge::kMessageNow;
        if (riderActivatedObject(activation) != 0)
            return 1;
        activatePickup(activation);
        return 0;
    }
    READABLE_FUNCTION_WAITS(riderReachesPickup, Verified);
}
