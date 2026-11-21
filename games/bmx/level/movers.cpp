// BMX XXX's "Mover" level object type: an object that, once activated,
// moves and turns from where the level put it to a second place over a set
// time (a door, a gate, a platform). BMX XXX's own code (its game block).
// Its tags say what sets it off (a script's or the game's activation
// always does; a rider's touch, tap or grind when its tag asks for one),
// how it gets there (evenly, speeding up or slowing down, at once or after
// a wait), how far it moves and turns, and whether it is only there to be
// seen until it has moved. It has a "move" sound that follows it, an
// "activate" sound when it starts and its class's touch sounds. Its state
// and the time in it are saved.

#include "entities.h"
#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/movers.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        // The mover's own tag data (objectComponent).
        constexpr uint32_t kTriggerTag = 0;        // byte: what touch of a rider's activates it (MoverTrigger)
        constexpr uint32_t kMotionTag = 1;         // byte: how it gets there (MoverMotion)
        constexpr uint32_t kGoneAtEndTag = 2;      // byte, bit 0: shown and collided with at its start, neither at its end
        // floats: how far it moves along its own axes at its start (the
        // tags keep the second and third the other way round)
        constexpr uint32_t kMoveXTag = 4, kMoveZTag = 8, kMoveYTag = 0xC;
        // floats: how far it turns about each, in degrees (kept the same way)
        constexpr uint32_t kTurnXTag = 0x10, kTurnZTag = 0x14, kTurnYTag = 0x18;
        constexpr uint32_t kMoveSecondsTag = 0x1C; // float: how long the move takes
        constexpr uint32_t kStateTag = 0x20;       // byte: MoverState
        constexpr uint32_t kTimeTag = 0x24;        // float: seconds in the state
        constexpr uint32_t kStartPositionTag = 0x28; // vector: where the level put it
        // three vectors, one after the other: the rows of its rotation there
        constexpr uint32_t kStartRotationTag = 0x34;
        constexpr uint32_t kTagVectorBytes = 0xC;
        constexpr uint32_t kSoundTag = 0x58;       // word: its "move" sound's handle
        // Its class's tag data (levelObjectClassTag).
        constexpr uint32_t kSoundNameTag = 4;      // text: the middle of its sounds' names
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kTagText = 0x10, kTagVector = 0x40;

        enum MoverTrigger : uint32_t
        {
            kTriggerNone = 0,  // activated by message only
            kTriggerTouch = 1, // any touch of a rider's
            kTriggerTap = 2,
            kTriggerGrind = 3,
        };

        // kMotionTag: the odd ones of the first four wait before they move.
        enum MoverMotion : uint32_t
        {
            kMotionEven = 0,
            kMotionEvenAfterWait = 1,
            kMotionSpeedingUp = 2,
            kMotionSpeedingUpAfterWait = 3,
            kMotionSlowingDown = 4,
            kMotionSlowingDown5 = 5, // the same as 4
            kMotions = 6,
        };

        enum MoverState : uint32_t
        {
            kMoverAtStart = 0,
            kMoverWaiting = 1, // activated, not moving yet
            kMoverMoving = 2,
            kMoverAtEnd = 3,
        };

        constexpr uint32_t kTypeName = sym::moverTypeName;                             // "Mover"
        constexpr uint32_t kMoveSoundFormat = sym::bmx::moverMoveSoundFormat;         // "%s.%s.move"
        constexpr uint32_t kTouchSoundFormat = sym::bmx::moverCollideSoundFormat;     // "%s.%s"
        constexpr uint32_t kActivateSoundFormat = sym::bmx::moverActivateSoundFormat; // "%s.%s.activate"
        // A frame's share of a second (1 / 60), as each of the two running states adds it.
        inline ee::Global<ee::Float, sym::g_moverWaitFrameSeconds> g_moverWaitFrameSeconds;
        inline ee::Global<ee::Float, sym::g_moverMoveFrameSeconds> g_moverMoveFrameSeconds;
        // Degrees to the engine's angles (65536 / 360).
        inline ee::Global<ee::Float, sym::g_moverDegreesToAngle> g_moverDegreesToAngle;
        // How long a waiting mover waits, in seconds.
        constexpr float kWaitSeconds = 0.75f;
        // A whole turn in the engine's angles (the slowing motion runs a quarter of one).
        constexpr float kWholeTurn = 65536.0f;

        constexpr uint32_t kCreateEntity = sym::createEntity;
        constexpr zge::MessageHandler kUpdateMover{sym::updateMover};

    }

    uint32_t initMover(ee::Ptr<zge::SceneObject> mover);
    int32_t activateMover(ee::Ptr<zge::EngineMessage> message);
    int32_t updateMover(ee::Ptr<zge::EngineMessage> message);
    uint32_t moverState(ee::Ptr<zge::SceneObject> mover);
    void setMoverState(ee::Ptr<zge::SceneObject> mover, uint32_t state);
    void storeMoverStart(ee::Ptr<zge::SceneObject> mover, ee::Ptr<zge::SceneObject> placed);
    void placeMover(ee::Ptr<zge::SceneObject> mover, ee::Float progress);

    // registerMoverType: the "Mover" type, made as a plain entity and set up
    // by initMover.
    uint32_t registerMoverType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initMover));
    }
    READABLE_FUNCTION(registerMoverType, Verified);

    // initMover (the type's init): where it stands kept as its start, its
    // messages subscribed, its "move" sound started. Always 1.
    uint32_t initMover(ee::Ptr<zge::SceneObject> mover)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        storeMoverStart(mover, mover);
        zge::subscribe(mover, zge::kRemovedMessage, zge::MessageHandler(sym::moverRemoved));
        zge::subscribe(mover, zge::kSaveSizeMessage, zge::MessageHandler(sym::moverSaveSize));
        zge::subscribe(mover, zge::kSaveMessage, zge::MessageHandler(sym::saveMover));
        zge::subscribe(mover, zge::kRestoreMessage, zge::MessageHandler(sym::restoreMover));
        zge::subscribe(mover, kRiderTouchMessage, zge::MessageHandler(sym::moverCollide));
        zge::subscribe(mover, kActivateMessage, zge::MessageHandler(sym::activateMover));
        zge::subscribe(mover, kEnableMessage, zge::MessageHandler(sym::enableMover));
        zge::tagString(zge::levelObjectClassTag(mover), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kMoveSoundFormat, kTypeName, className.address());
        const uint32_t sound = soundEvent(kObjectSoundEvent, zge::objectPosition(mover), kObjectSoundStart, soundName.address(), kFullVolume);
        zge::setTagWord(zge::objectComponent(mover), kTagWord, kSoundTag, sound);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initMover, Verified);

    // moverRemoved (the engine's removed message): its "move" sound stopped.
    int32_t moverRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(ee::Ptr<zge::SceneObject>(message->target)), kTagWord, kSoundTag));
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
        soundEvent(kObjectSoundEvent, position, kObjectSoundStop, sound, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(moverRemoved, Verified);

    // moverCollide (kRiderTouchMessage): a mover at its start whose trigger
    // the collision is gets activated by the collider's rider (the game is
    // asked first and may take the activation). Whatever its state, its
    // class's sound for the touch plays.
    int32_t moverCollide(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const ee::Ptr<zge::SceneObject> mover(message->target);
        const ee::Ptr<RiderTouch> collision(message->data);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(message->sender));
        bool triggered = false;
        switch (zge::tagByte(zge::objectComponent(mover), kTagByte, kTriggerTag))
        {
        case kTriggerTouch:
            triggered = true;
            break;
        case kTriggerTap:
            triggered = collision->kind == kTouchTap;
            break;
        case kTriggerGrind:
            triggered = collision->kind == kTouchGrind;
            break;
        default:
            break;
        }
        if (triggered && moverState(mover) == kMoverAtStart)
        {
            event->quiet = 0;
            event->player = rider->player;
            event->velocity = zge::kZero;
            activation->target = message->target;
            activation->sender = message->sender;
            activation->type = kActivateMessage;
            activation->flags = zge::kMessageNow;
            activation->data = event;
            if (riderActivatedObject(activation) == 0)
                activateMover(activation);
        }
        zge::tagString(zge::levelObjectClassTag(mover), kTagText, kSoundNameTag, className.address());
        zge::formatString(soundName, kTouchSoundFormat, kTypeName, className.address());
        playTouchSound(message, soundName);
        return 0;
    }
    READABLE_FUNCTION_WAITS(moverCollide, Draft);

    // enableMover (kEnableMessage): back at its start.
    int32_t enableMover(ee::Ptr<zge::EngineMessage> message)
    {
        setMoverState(ee::Ptr<zge::SceneObject>(message->target), kMoverAtStart);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableMover, Verified);

    // activateMover (kActivateMessage): it starts to move, after a wait
    // where its motion has one; an event with its first word set puts it
    // at its end at once.
    int32_t activateMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ActivationEvent> event(message->data);
        const ee::Ptr<zge::SceneObject> mover(message->target);
        if (event->quiet != 0)
        {
            setMoverState(mover, kMoverAtEnd);
            return 0;
        }
        switch (zge::tagByte(zge::objectComponent(mover), kTagByte, kMotionTag))
        {
        case kMotionEvenAfterWait:
        case kMotionSpeedingUpAfterWait:
            setMoverState(mover, kMoverWaiting);
            break;
        default:
            setMoverState(mover, kMoverMoving);
            break;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(activateMover, Draft);

    // updateMover (the frame's update, while it waits or moves): a waiting
    // one starts to move when its wait is over. A moving one is placed as
    // far along as its time and its motion say (evenly; by the square of
    // the time, speeding up; or by the sine of a quarter turn, slowing
    // down), its "move" sound following it, and is at its end when the
    // move's time is up.
    int32_t updateMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> mover(message->target);
        const uint32_t state = moverState(mover);
        if (state == kMoverWaiting)
        {
            const ee::Float time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag) + *g_moverWaitFrameSeconds;
            if (time < ee::Float(kWaitSeconds))
            {
                zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, time);
                return 0;
            }
            setMoverState(mover, kMoverMoving);
            zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, ee::Float(0.0f));
            return 0;
        }
        if (state != kMoverMoving)
            return 0;
        const ee::Float time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag) + *g_moverMoveFrameSeconds;
        const ee::Float moveSeconds = zge::tagFloat(zge::objectComponent(mover), kTagWord, kMoveSecondsTag);
        if (!(time < moveSeconds))
        {
            setMoverState(mover, kMoverAtEnd);
            return 0;
        }
        ee::Float progress;
        switch (zge::tagByte(zge::objectComponent(mover), kTagByte, kMotionTag))
        {
        case kMotionSpeedingUp:
        case kMotionSpeedingUpAfterWait:
        {
            const ee::Float whole = moveSeconds * moveSeconds;
            progress = time * time / whole;
            break;
        }
        case kMotionSlowingDown:
        case kMotionSlowingDown5:
            progress = zge::angleSin(static_cast<zge::Angle>(ee::toInt(time * ee::Float(0.25f) / moveSeconds * ee::Float(kWholeTurn))));
            break;
        default:
            progress = time / moveSeconds;
            break;
        }
        placeMover(mover, progress);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(mover), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(mover), kObjectSoundMove, sound, kFullVolume);
        zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, time);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateMover, Verified);

    namespace
    {
        // What a mover keeps in a saved level state.
        struct MoverSave
        {
            uint32_t state; // +0x00
            ee::Float time; // +0x04 seconds in it
        };
        static_assert(sizeof(MoverSave) == 8);
    }

    // moverSaveSize (the engine's save size message).
    int32_t moverSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(MoverSave);
        return 0;
    }
    READABLE_FUNCTION(moverSaveSize, Verified);

    // restoreMover (the engine's restore message): the saved state entered
    // and one update run at the saved time, which then stands.
    int32_t restoreMover(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::EngineMessage> update;
        const ee::Ptr<MoverSave> saved(message->data);
        const ee::Ptr<zge::SceneObject> mover(message->target);
        setMoverState(mover, saved->state);
        zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, saved->time);
        update->target = mover;
        updateMover(update);
        zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, saved->time);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreMover, Verified);

    // saveMover (the engine's save message).
    int32_t saveMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> mover(message->target);
        const ee::Ptr<MoverSave> saved(message->data);
        saved->state = moverState(mover);
        saved->time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag);
        return 0;
    }
    READABLE_FUNCTION(saveMover, Verified);

    // moverState: its state.
    uint32_t moverState(ee::Ptr<zge::SceneObject> mover)
    {
        return zge::tagByte(zge::objectComponent(mover), kTagByte, kStateTag);
    }
    READABLE_FUNCTION(moverState, Verified);

    // setMoverState: the state entered, its time from 0. A waiting or
    // moving one is updated every frame; one starting to move plays its
    // "activate" sound; one put at its start or its end is placed there,
    // and where its tag says so is shown and collided with at its start
    // and neither at its end.
    void setMoverState(ee::Ptr<zge::SceneObject> mover, uint32_t state)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> className;
        const uint64_t node = objectNodeHandle(mover);
        const uint32_t was = zge::tagByte(zge::objectComponent(mover), kTagByte, kStateTag);
        if (was == kMoverWaiting || was == kMoverMoving)
            zge::unsubscribe(mover, zge::kUpdateMessage, kUpdateMover);
        switch (state)
        {
        case kMoverWaiting:
            zge::subscribe(mover, zge::kUpdateMessage, kUpdateMover);
            break;
        case kMoverAtStart:
            placeMover(mover, ee::Float(0.0f));
            if ((zge::tagByte(zge::objectComponent(mover), kTagByte, kGoneAtEndTag) & 1u) != 0)
            {
                rider::collision::showNode(node, 1);
                rider::collision::setNodeCollidable(node, 1);
            }
            break;
        case kMoverMoving:
        {
            zge::tagString(zge::levelObjectClassTag(mover), kTagText, kSoundNameTag, className.address());
            zge::formatString(soundName, kActivateSoundFormat, kTypeName, className.address());
            soundEvent(kObjectSoundEvent, zge::objectPosition(mover), kObjectSoundOnce, soundName.address(), kFullVolume);
            zge::subscribe(mover, zge::kUpdateMessage, kUpdateMover);
            break;
        }
        case kMoverAtEnd:
            placeMover(mover, ee::Float(1.0f));
            if ((zge::tagByte(zge::objectComponent(mover), kTagByte, kGoneAtEndTag) & 1u) != 0)
            {
                rider::collision::showNode(node, 0);
                rider::collision::setNodeCollidable(node, 0);
            }
            break;
        default:
            break;
        }
        zge::setTagByte(zge::objectComponent(mover), kTagByte, kStateTag, state & 0xFFu);
        zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(setMoverState, Verified);

    // storeMoverStart: where `placed` stands and how it is turned kept in
    // the mover's tags as its start.
    void storeMoverStart(ee::Ptr<zge::SceneObject> mover, ee::Ptr<zge::SceneObject> placed)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Matrix> rotation;
        *position = zge::quadwordOf(zge::objectPosition(placed));
        zge::quaternionToMatrix(rotation, zge::objectOrientation(placed));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartPositionTag, position.address());
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag, &zge::matrixRow(*rotation, 0));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 1 * kTagVectorBytes, &zge::matrixRow(*rotation, 1));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 2 * kTagVectorBytes, &zge::matrixRow(*rotation, 2));
    }
    READABLE_FUNCTION_WAITS(storeMoverStart, Verified);

    // placeMover: the mover put `progress` (0 to 1) of the way from its
    // start to its end: that share of its move (along its own axes at its
    // start) added to its start position, and that share of its turn about
    // x, then y, then z put before its start rotation.
    void placeMover(ee::Ptr<zge::SceneObject> mover, ee::Float progress)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> move;
        ee::Local<zge::Vec4> turnDegrees;
        ee::Local<zge::Matrix> start;
        ee::Local<zge::Matrix> turn;
        zge::tagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag, &zge::matrixRow(*start, 0));
        zge::tagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 1 * kTagVectorBytes, &zge::matrixRow(*start, 1));
        zge::tagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 2 * kTagVectorBytes, &zge::matrixRow(*start, 2));
        zge::matrixRow(*start, 3) = zge::kZero;
        move->x = zge::tagFloat(zge::objectComponent(mover), kTagWord, kMoveXTag);
        move->y = zge::tagFloat(zge::objectComponent(mover), kTagWord, kMoveYTag);
        move->z = zge::tagFloat(zge::objectComponent(mover), kTagWord, kMoveZTag);
        *move = zge::transformed(*start, *move);
        zge::tagVector(zge::objectComponent(mover), kTagVector, kStartPositionTag, position);
        *move = *move * progress;
        *position = *position + *move;
        const ee::Float degreesToAngle = *g_moverDegreesToAngle;
        zge::matrixIdentity(turn);
        turnDegrees->x = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTurnXTag);
        turnDegrees->y = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTurnYTag);
        turnDegrees->z = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTurnZTag);
        zge::matrixRotateX(turn, turn, static_cast<zge::Angle>(ee::toInt(turnDegrees->x * progress * degreesToAngle)));
        zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(ee::toInt(turnDegrees->y * progress * degreesToAngle)));
        zge::matrixRotateZ(turn, turn, static_cast<zge::Angle>(ee::toInt(turnDegrees->z * progress * degreesToAngle)));
        zge::matrixRow(*turn, 3) = zge::kZero;
        zge::matrixMultiply(start, start, turn);
        zge::matrixRow(*start, 3) = *position;
        zge::setObjectRotationMatrix(mover, start);
        zge::setObjectPosition(mover, position);
        zge::updatePose(mover);
    }
    READABLE_FUNCTION_WAITS(placeMover, Verified);
}
