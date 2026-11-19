// The movers: the "Mover" level objects, which once activated move and turn
// from where the level put them to a second place over a set time (a door,
// a gate, a platform). DM2's own code (its game block); BMX XXX's movers
// (game/bmx/level/movers.cpp) are its descendants, with the tags in other
// places and the sounds by name. Its tags say how it gets there (evenly,
// speeding up or slowing down, at once or after a wait), how far it moves
// and turns, whether it is only there to be seen until it has moved, and a
// sample it plays while it moves. Its state and the time in it are saved.

#include "object_node.h"
#include "object_sounds.h"

#include "dm2/sound/sound_events.h"
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
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/movers.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The mover's own tag data (objectComponent).
        constexpr uint32_t kSoundTag = 0;          // word: its object sound (object_sounds.h)
        constexpr uint32_t kStateTag = 4;          // byte: MoverState
        constexpr uint32_t kMotionTag = 5;         // byte: how it gets there (MoverMotion)
        constexpr uint32_t kGoneAtEndTag = 6;      // byte, bit 0: shown and collided with at its start, neither at its end
        constexpr uint32_t kMoveSampleTag = 7;     // byte: the level bank's sample + 1 it plays while it moves (0: none)
        // floats: how far it moves along its own axes at its start (the
        // tags keep the second and third the other way round)
        constexpr uint32_t kMoveXTag = 8, kMoveZTag = 0xC, kMoveYTag = 0x10;
        // floats: how far it turns about each, in degrees (kept the same way)
        constexpr uint32_t kTurnXTag = 0x14, kTurnZTag = 0x18, kTurnYTag = 0x1C;
        constexpr uint32_t kMoveSecondsTag = 0x20; // float: how long the move takes
        constexpr uint32_t kTimeTag = 0x24;        // float: seconds in the state
        constexpr uint32_t kStartPositionTag = 0x28; // vector: where the level put it
        // three vectors, one after the other: the rows of its rotation there
        constexpr uint32_t kStartRotationTag = 0x34;
        constexpr uint32_t kTagVectorBytes = 0xC;
        // The tag accessors' second argument (the value's kind).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kTagVector = 0x40;

        // kMotionTag: the odd ones of the first four wait before they move.
        enum MoverMotion : uint32_t
        {
            kMotionEven = 0,
            kMotionEvenAfterWait = 1,
            kMotionSpeedingUp = 2,
            kMotionSpeedingUpAfterWait = 3,
            kMotionSlowingDown = 4,
            kMotionSlowingDown5 = 5, // the same as 4
        };

        enum MoverState : uint32_t
        {
            kMoverAtStart = 0,
            kMoverWaiting = 1, // activated, not moving yet
            kMoverMoving = 2,
            kMoverAtEnd = 3,
        };

        // The activate message (goals.cpp's kActivateMessage; its data's
        // first word set: the mover is put at its end at once) and message
        // 0x101 (object_sounds.cpp's kMessage101: its sound's turn only).
        constexpr uint32_t kActivateMessage = 0x100, kMessage101 = 0x101;

        // A frame's share of a second (1 / 60), as each of the two running
        // states adds it.
        inline ee::Global<ee::Float, sym::g_moverWaitFrameSeconds> g_moverWaitFrameSeconds;
        inline ee::Global<ee::Float, sym::g_moverMoveFrameSeconds> g_moverMoveFrameSeconds;
        // Degrees to the engine's angles (65536 / 360).
        inline ee::Global<ee::Float, sym::g_moverDegreesToAngle> g_moverDegreesToAngle;
        // How long a waiting mover waits, in seconds.
        constexpr float kWaitSeconds = 0.75f;
        // A whole turn in the engine's angles (the slowing motion runs a
        // quarter of one).
        constexpr float kWholeTurn = 65536.0f;
        // soundEvent 37 (the sound sources, as 38): b 3 plays the level
        // bank's sample c once at a.
        constexpr uint32_t kSoundSourcesEvent = 37, kPlayOnce = 3, kFullLevel = 0x7F;

        // Scaffolding (address-call): DM2's registerLevelObjectType (its code
        // differs from the engine's: the name and the set-up only).
        namespace tail
        {
            uint32_t registerLevelObjectType(ee::Ptr<const char> name, uint32_t setUp)
            {
                return ee::tailCall<uint32_t>(sym::registerLevelObjectType, name, setUp);
            }
        }
        constexpr ee::Ptr<const char> kMoverTypeName{sym::moverTypeName}; // "Mover"

        // The mover's sound.
        uint32_t moverSound(ee::Ptr<zge::SceneObject> mover)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(mover), kTagWord, kSoundTag));
        }

        // What a mover keeps in a saved level state.
        struct MoverSave
        {
            uint32_t state; // +0x00
            ee::Float time; // +0x04 seconds in it
        };
        static_assert(sizeof(MoverSave) == 8);
    }

    int32_t updateMover(ee::Ptr<zge::EngineMessage> message);
    void placeMover(ee::Ptr<zge::SceneObject> mover, ee::Float progress);
    void setMoverState(ee::Ptr<zge::SceneObject> mover, uint32_t state);

    // registerMoverType: the "Mover" level objects made movers.
    uint32_t registerMoverType()
    {
        return tail::registerLevelObjectType(kMoverTypeName, sym::newMover);
    }
    READABLE_FUNCTION(registerMoverType, Verified);

    // newMover: the object made a mover: where it stands and how it is
    // turned kept in its tags as its start, its messages subscribed, its
    // sound started.
    int32_t newMover(ee::Ptr<zge::SceneObject> mover)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Matrix> rotation;
        *position = zge::quadwordOf(zge::objectPosition(mover));
        zge::quaternionToMatrix(rotation, &*zge::objectOrientation(mover));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartPositionTag, position);
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag, &zge::matrixRow(*rotation, 0));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 1 * kTagVectorBytes, &zge::matrixRow(*rotation, 1));
        zge::setTagVector(zge::objectComponent(mover), kTagVector, kStartRotationTag + 2 * kTagVectorBytes, &zge::matrixRow(*rotation, 2));
        zge::subscribe(mover, kActivateMessage, zge::MessageHandler(sym::activateMover));
        zge::subscribe(mover, kMessage101, zge::MessageHandler(sym::moverMessage101));
        zge::subscribe(mover, zge::kSaveSizeMessage, zge::MessageHandler(sym::moverSaveSize));
        zge::subscribe(mover, zge::kSaveMessage, zge::MessageHandler(sym::saveMover));
        zge::subscribe(mover, zge::kRestoreMessage, zge::MessageHandler(sym::restoreMover));
        startObjectSound(mover, static_cast<uint32_t>(zge::tagWord(zge::objectComponent(mover), kTagWord, kSoundTag)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newMover, Verified);

    // moverState: its state.
    uint32_t moverState(ee::Ptr<zge::SceneObject> mover)
    {
        return zge::tagByte(zge::objectComponent(mover), kTagByte, kStateTag);
    }
    READABLE_FUNCTION(moverState, Verified);

    // activateMover (the activate message): it starts to move, after a
    // wait where its motion has one; an activation whose data's first word
    // is set puts it at its end at once. Its sound has its turn.
    int32_t activateMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> mover(message->target);
        if (*ee::Ptr<uint32_t>(message->data) == 0)
        {
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
        }
        else
            setMoverState(mover, kMoverAtEnd);
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(activateMover, Verified);

    // moverMessage101 (message 0x101): its sound has its turn.
    int32_t moverMessage101(ee::Ptr<zge::EngineMessage> message)
    {
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(moverMessage101, Draft);

    // updateMover (the update message, while it waits or moves): a waiting
    // one starts to move when its wait is over. A moving one is placed as
    // far along as its time and its motion say (evenly; by the square of
    // the time, speeding up; or by the sine of a quarter turn, slowing
    // down), playing its move sample there, and is at its end when the
    // move's time is up. Its sound has its turn.
    int32_t updateMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> mover(message->target);
        const uint32_t state = moverState(mover);
        if (state == kMoverWaiting)
        {
            const ee::Float time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag) + *g_moverWaitFrameSeconds;
            if (time < ee::Float(kWaitSeconds))
                zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, time);
            else
            {
                setMoverState(mover, kMoverMoving);
                zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, ee::Float(0.0f));
            }
        }
        else if (state == kMoverMoving)
        {
            const ee::Float time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag) + *g_moverMoveFrameSeconds;
            const ee::Float moveSeconds = zge::tagFloat(zge::objectComponent(mover), kTagWord, kMoveSecondsTag);
            if (!(time < moveSeconds))
                setMoverState(mover, kMoverAtEnd);
            else
            {
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
                if (const uint32_t sample = zge::tagByte(zge::objectComponent(mover), kTagByte, kMoveSampleTag))
                    soundEvent(kSoundSourcesEvent, zge::objectPosition(mover), kPlayOnce, sample - 1, kFullLevel);
                zge::setTagFloat(zge::objectComponent(mover), kTagWord, kTimeTag, time);
            }
        }
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateMover, Verified);

    // moverSaveSize (the save-size message): a mover's save is 8 bytes; its
    // sound has its turn.
    int32_t moverSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(MoverSave);
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(moverSaveSize, Verified);

    // restoreMover (the restore message): the saved state entered and one
    // update run at the saved time (with a message of its own that names
    // only the mover: the rest is what the frame held), which then stands.
    // Its sound has its turn.
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
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreMover, Verified);

    // saveMover (the save message): its state and the time in it; its
    // sound has its turn.
    int32_t saveMover(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> mover(message->target);
        const ee::Ptr<MoverSave> saved(message->data);
        saved->state = moverState(mover);
        saved->time = zge::tagFloat(zge::objectComponent(mover), kTagWord, kTimeTag);
        objectSoundMessage(message, moverSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveMover, Verified);

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
        zge::transformPoint(move, start, move);
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

    // setMoverState: the state entered, its time from 0. A waiting or
    // moving one is updated every frame; one starting to move plays its
    // move sample; one put at its start or its end is placed there, and
    // where its tag says so is shown and tested at its start and neither at
    // its end.
    void setMoverState(ee::Ptr<zge::SceneObject> mover, uint32_t state)
    {
        const uint64_t node = objectNodeHandle(mover);
        const uint32_t was = zge::tagByte(zge::objectComponent(mover), kTagByte, kStateTag);
        if (was == kMoverWaiting || was == kMoverMoving)
            zge::unsubscribe(mover, zge::kUpdateMessage, zge::MessageHandler(sym::updateMover));
        switch (state)
        {
        case kMoverWaiting:
            zge::subscribe(mover, zge::kUpdateMessage, zge::MessageHandler(sym::updateMover));
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
            if (const uint32_t sample = zge::tagByte(zge::objectComponent(mover), kTagByte, kMoveSampleTag))
                soundEvent(kSoundSourcesEvent, zge::objectPosition(mover), kPlayOnce, sample - 1, kFullLevel);
            zge::subscribe(mover, zge::kUpdateMessage, zge::MessageHandler(sym::updateMover));
            break;
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
        game::tail(zge::setTagFloat, zge::objectComponent(mover), kTagWord, kTimeTag, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(setMoverState, Verified);
}
