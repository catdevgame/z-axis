// The script's animated objects: the "AnimObj" and "AnimPropDef" level
// types (attached here, their state saved and put back for the level's
// save) and the plain script object's attach.

#include "aggressive/level/animated_objects.h"
#include "aggressive/skater/action_tricks.h"
#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/script/anim_objects.cpp.h"

namespace aggressive::script
{
    using namespace aggressive::level;

    namespace
    {
        // "play" and "TEXT" (playAnimSoundEvent) and "SOUND_EVENT" (attachAnimObject), hashed at first use.
        constexpr uint32_t kTextPlay = sym::g_textPlay;
        constexpr uint32_t kTextText = sym::g_textText4;
        constexpr uint32_t kTextSoundEvent = sym::g_textSoundEvent;
        inline ee::Global<int32_t, sym::g_textAnimPlayHashPending> g_textAnimPlayHashPending;
        inline ee::Global<int32_t, sym::g_textAnimPlayHash> g_textAnimPlayHash;
        inline ee::Global<int32_t, sym::g_textHash> g_textHash;
        inline ee::Global<int32_t, sym::g_soundEventHash2Pending> g_soundEventHash2Pending;
        inline ee::Global<int32_t, sym::g_soundEventHash2> g_soundEventHash2;

        // Engine message 0x7002: the target's velocity asked for, the
        // message's data the vector to fill (zge objectVelocity sends it;
        // DM2's collision reads the answer).
        constexpr uint32_t kVelocityMessage = 0x7002u;
    }

    uint32_t returnOneHandler()
    {
        return 1;
    }
    READABLE_FUNCTION(returnOneHandler, Verified);

    namespace
    {
        constexpr uint32_t kTextAnimObj = sym::g_textAnimObj;
        inline ee::Global<int32_t, sym::g_animObjHashPending> g_animObjHashPending;
        inline ee::Global<int32_t, sym::g_animObjHash> g_animObjHash;
    }

    uint32_t animObjHash()
    {
        if (*g_animObjHashPending != 0)
        {
            const int32_t nameHashResult = zge::nameHash(ee::Ptr<const char>(static_cast<uint32_t>(kTextAnimObj)));
            *g_animObjHash = nameHashResult;
            *g_animObjHashPending = 0;
            return *g_animObjHash;
        }
        return *g_animObjHash;
    }
    READABLE_FUNCTION(animObjHash, Verified);

    namespace
    {
        void releaseObjectAnimator(ee::Ptr<zge::SceneObject> object) { zge::freeObjectAnimator(object); }
    }

    // The script object level type's attach callback: the object answers
    // the removal message with 1 and does nothing.
    uint32_t attachScriptObject(ee::Ptr<zge::MessageTarget> object)
    {
        zge::subscribe(object, zge::kRemovedMessage, sym::returnOneHandler);
        return 1;
    }
    READABLE_FUNCTION_WAITS(attachScriptObject, Verified);

    // An animated object's answer to the velocity message (objectVelocity):
    // its velocity tags into the message's vector.
    uint32_t animObjectVelocity(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<ee::Float> velocity(message->data);
        const ee::Ptr<zge::TagRecord> record = zge::objectComponent(ee::Ptr<zge::SceneObject>(message->target));
        velocity[0] = zge::tagFloat(record, kTagWords, kTagVelocityX);
        velocity[1] = zge::tagFloat(record, kTagWords, kTagVelocityY);
        velocity[2] = zge::tagFloat(record, kTagWords, kTagVelocityZ);
        return 1;
    }
    READABLE_FUNCTION(animObjectVelocity, Verified);

    // The removal message's handler: the receiving object's animator freed.
    uint32_t freeComponentAnimator(ee::Ptr<zge::EngineMessage> message)
    {
        releaseObjectAnimator(ee::Ptr<zge::SceneObject>(message->target));
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeComponentAnimator, Verified);

    // An animated object's animation reached a "SOUND_EVENT": the object the
    // event's "TEXT" names (a movable sound, level/movable_sound.cpp) is
    // sent "play" at once, from the animated object.
    uint32_t playAnimSoundEvent(ee::Ptr<zge::EngineMessage> soundEvent)
    {
        ee::Local<zge::EngineMessage> play;
        if (*g_textAnimPlayHashPending != 0)
        {
            *g_textAnimPlayHashPending = 0;
            *g_textAnimPlayHash = zge::nameHash(ee::Ptr<const char>(kTextPlay));
            *g_textHash = zge::nameHash(ee::Ptr<const char>(kTextText));
        }
        const uint32_t soundName = zge::eventInt(ee::Ptr<zge::AnimationEvent>(soundEvent->data), *g_textHash);
        play->target = zge::findSceneObject(soundName, 0, 0);
        play->sender = soundEvent->target;
        play->type = *g_textAnimPlayHash;
        play->flags = zge::kMessageNow;
        play->data = 0;
        zge::sendEngineMessage(play);
        return 1;
    }
    READABLE_FUNCTION_WAITS(playAnimSoundEvent, Verified);

    // A walk over the level's objects: `count` goes up for each animated object.
    uint32_t countAnimObjects(ee::Ptr<zge::SceneObject> object, ee::Ptr<uint32_t> count)
    {
        if (animObjHash() == zge::levelObjectTypeHash(object))
            *count = *count + 1;
        return 1;
    }
    READABLE_FUNCTION(countAnimObjects, Verified);

    // A walk over the level's objects: each animated object's state
    // (level/animated_objects.h AnimObjectState) written at `cursor`, which
    // moves on past it. Always 1 (go on).
    uint32_t saveAnimObjectState(ee::Ptr<zge::SceneObject> object, ee::Ptr<uint32_t> cursor)
    {
        ee::Local<AnimObjectState> state;
        ee::Local<uint32_t> paused;
        *paused = 0;
        if (animObjHash() != zge::levelObjectTypeHash(object))
            return 1;

        const ee::Ptr<zge::TagRecord> record = zge::objectComponent(object);
        zge::objectLevelNode(object); // (the original asks, and drops the answer)
        state->name = zge::objectNameHash(object);
        state->animation = zge::tagWord(record, kTagWords, kTagAnimationName);
        state->playing = zge::tagByte(record, kTagBytes, kTagAnimationPlaying) & 1u;
        if (zge::animationPaused(object, ee::Ptr<uint32_t>(paused)) != 0)
            state->paused = *paused & 1u;
        else
            state->paused = 0;
        // An object without an animator: a played animation is saved an
        // hour in (3600 seconds), one never played at 0.
        if (zge::animationTime(object, ee::Ptr<ee::Float>(&state->time)) == 0)
            state->time = state->playing ? ee::Float(3600.0f) : ee::Float(0.0f);
        state->mode = zge::tagByte(record, kTagBytes, kTagAnimationMode) & 0xFu;
        zge::memoryCopy(*cursor, state, sizeof(AnimObjectState));
        *cursor = *cursor + sizeof(AnimObjectState);
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveAnimObjectState, Verified);

    // A saved state put back on its object (restoreAnimObjectStates): a
    // played animation plays again from the saved time, paused if it was,
    // and the tags say so again. Always 1.
    uint32_t restoreAnimObjectState(ee::Ptr<zge::SceneObject> object, ee::Ptr<AnimObjectState> saved)
    {
        ee::Local<AnimObjectState> state;
        ee::Local<ee::Ptr<zge::Motion>> animation;
        *animation = {};
        const ee::Ptr<zge::TagRecord> record = zge::objectComponent(object);
        zge::objectLevelNode(object); // (the original asks, and drops the answer)
        zge::memoryCopy(state, saved, sizeof(AnimObjectState));
        if (!state->playing)
            return 1;

        zge::findAnimationByHash(state->animation, animation);
        zge::playAnimation(object, *animation, state->mode, ee::Float(0.0f), ee::Float(-1.0f));
        // setAnimationTime's second parameter is unused: what the caller's
        // register holds goes on, as in the original.
        zge::setAnimationTime(object, state->time);
        if (state->paused)
            zge::pauseAnimation(object);
        zge::setTagWord(record, kTagWords, kTagAnimationName, state->animation);
        zge::setTagByte(record, kTagBytes, kTagAnimationMode, state->mode);
        zge::setTagByte(record, kTagBytes, kTagAnimationPlaying, state->playing);
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreAnimObjectState, Verified);

    // The "AnimObj" and "AnimPropDef" level types' attach callback: the
    // object answers the velocity and removal messages and its animation's
    // "SOUND_EVENT"s, gets its animator, plays nothing, stands still, and
    // keeps the position it was attached at.
    uint32_t attachAnimObject(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<zge::TagRecord> record = zge::objectComponent(object);
        if (*g_soundEventHash2Pending != 0)
        {
            *g_soundEventHash2Pending = 0;
            *g_soundEventHash2 = zge::nameHash(ee::Ptr<const char>(kTextSoundEvent));
        }
        zge::subscribe(object, kVelocityMessage, sym::animObjectVelocity);
        zge::subscribe(object, zge::kRemovedMessage, sym::freeComponentAnimator);
        zge::subscribe(object, *g_soundEventHash2, sym::playAnimSoundEvent);
        zge::createObjectAnimator(object);
        zge::setTagByte(record, kTagBytes, kTagAnimationMode, 0);
        zge::setTagByte(record, kTagBytes, kTagAnimationPlaying, 0);
        zge::setTagWord(record, kTagWords, kTagVelocityX, 0);
        zge::setTagWord(record, kTagWords, kTagVelocityY, 0);
        zge::setTagWord(record, kTagWords, kTagVelocityZ, 0);
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(object);
        zge::setTagWord(record, kTagWords, kTagAttachedX, sce::libgcc::fptoui(position->x));
        zge::setTagWord(record, kTagWords, kTagAttachedY, sce::libgcc::fptoui(position->y));
        zge::setTagWord(record, kTagWords, kTagAttachedZ, sce::libgcc::fptoui(position->z));
        return 1;
    }
    READABLE_FUNCTION_WAITS(attachAnimObject, Verified);
}
