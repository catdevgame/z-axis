// zanim.c's queries and copies (zge/scene/animation.h): a bone's channel
// found, its rotation, translation and scale read and set (the game's
// sway, car wheels and hips set them by hand), a pose mirrored left to
// right, an animator's pose and tracks copied, an animator freed; and a
// playing motion's events: their kinds, times and fields.
//

#include "animation.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "zge/scene/animate_system.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/animation_queries.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kHeapFree = sym::heapFree;      // (block, ...): hands its other registers on
        constexpr uint32_t kAllocateSized = sym::allocateSized; // (bytes) -> a block of the smallest size-class pool that fits
        constexpr uint32_t kFreeSized = sym::freeSized;     // (block, bytes)
        // This module's, called through the table as the original calls them.
        constexpr uint32_t kSceneObjectAnimator = sym::sceneObjectAnimator;

        // The game's event hook (animation.cpp's update calls it).
        inline ee::Global<uint32_t, sym::g_animationEventHook> g_animationEventHook;
    }

    // ---- a bone's channel --------------------------------------------------

    // sceneObjectAnimator: the object's animator.
    ee::Ptr<Animator> sceneObjectAnimator(ee::Ptr<SceneObject> object) { return object->animator; }
    READABLE_FUNCTION(sceneObjectAnimator, Verified);

    // findBoneChannel: the animator's channel for a bone's name hash, or 0.
    ee::Ptr<AnimationChannel> findBoneChannel(ee::Ptr<Animator> animator, uint32_t boneId)
    {
        ee::Ptr<AnimationChannel> channel = animator->channels;
        for (uint32_t left = animator->channelCount; left != 0; --left, channel = channelAt(channel, 1))
            if (channel->boneId == boneId)
                return channel;
        return ee::Ptr<AnimationChannel>();
    }
    READABLE_FUNCTION(findBoneChannel, Verified);

    // setAnimatorChanges: the animator's changes word set (its animator).
    ee::Ptr<Animator> setAnimatorChanges(ee::Ptr<SceneObject> object, uint32_t changes)
    {
        const ee::Ptr<Animator> animator = object->animator;
        animator->changes = changes;
        return animator;
    }
    READABLE_FUNCTION(setAnimatorChanges, Verified);

    // setBoneRotation, setBoneTranslation, setBoneScale: a bone's rotation,
    // translation or scale set, its flag raised and its object's pose
    // marked (invalidateAnimator's result, the original's tail call; the
    // flags without an object, and always in ZGE2, whose channels keep none).
    int32_t setBoneRotation(ee::Ptr<AnimationChannel> channel, ee::Ptr<Quaternion> rotation)
    {
        const uint32_t flags = channel->flags | kChannelRotation;
        channel->rotation.x = rotation->x;
        const ee::Ptr<SceneObject> object = channelObject(channel);
        channel->rotation.y = rotation->y;
        channel->rotation.z = rotation->z;
        channel->flags = static_cast<uint16_t>(flags);
        channel->rotation.w = rotation->w;
        if constexpr (!kChannelsKeepNoObject)
            if (object != 0)
                return game::tail(invalidateAnimator, object);
        return static_cast<int32_t>(flags);
    }
    READABLE_FUNCTION(setBoneRotation, Verified);

    // boneRotation: a bone's rotation.
    ee::Ptr<Quaternion> boneRotation(ee::Ptr<AnimationChannel> channel) { return &channel->rotation; }
    READABLE_FUNCTION(boneRotation, Verified);

    int32_t setBoneTranslation(ee::Ptr<AnimationChannel> channel, ee::Ptr<Vec4> translation)
    {
        channel->translation = quadwordOf(translation);
        const ee::Ptr<SceneObject> object = channelObject(channel);
        const uint32_t flags = channel->flags | kChannelTranslation;
        channel->flags = static_cast<uint16_t>(flags);
        if constexpr (!kChannelsKeepNoObject)
            if (object != 0)
                return game::tail(invalidateAnimator, object);
        return static_cast<int32_t>(flags);
    }
    READABLE_FUNCTION(setBoneTranslation, Verified);

    // boneTranslation: a bone's translation (the channel itself).
    ee::Ptr<Vec4> boneTranslation(ee::Ptr<AnimationChannel> channel) { return &channel->translation; }
    READABLE_FUNCTION(boneTranslation, Verified);

    int32_t setBoneScale(ee::Ptr<AnimationChannel> channel, ee::Ptr<Vec4> scale)
    {
        channel->scale = quadwordOf(scale);
        const ee::Ptr<SceneObject> object = channelObject(channel);
        const uint32_t flags = channel->flags | kChannelScale;
        channel->flags = static_cast<uint16_t>(flags);
        if constexpr (!kChannelsKeepNoObject)
            if (object != 0)
                return game::tail(invalidateAnimator, object);
        return static_cast<int32_t>(flags);
    }
    READABLE_FUNCTION(setBoneScale, Verified);

    // boneScale: a bone's scale.
    ee::Ptr<Vec4> boneScale(ee::Ptr<AnimationChannel> channel) { return &channel->scale; }
    READABLE_FUNCTION(boneScale, Verified);

    // ---- a pose mirrored ---------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t allocateSized(uint32_t bytes) { return ee::game<uint32_t>(kAllocateSized, bytes); }
    }

    // newBoneMirror: the bone pairs of a table of name pairs ({left name,
    // right name} ..., ended by a 0 name) for an object, hashed
    // (allocateSized: the header, then the pairs).
    uint32_t newBoneMirror(ee::Ptr<SceneObject> object, ee::Ptr<BoneNamePair> names)
    {
        uint32_t pairs = 0;
        while (names[pairs].left)
            ++pairs;
        const uint32_t bytes = kBoneMirrorHeaderBytes + pairs * sizeof(BonePair);
        // (ZGE2 has no sized pools: the heap)
        const uint32_t block = game::readable::kIsDm2 ? heapAllocate(bytes) : allocateSized(bytes);
        const ee::Ptr<BoneMirror> mirror(block);
        mirror->object = object;
        mirror->pairs = ee::Ptr<BonePair>(block + kBoneMirrorHeaderBytes);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no size)
            mirror->bytes = bytes;
        for (mirror->count = 0; names[mirror->count].left; ++mirror->count)
        {
            mirror->pairs[mirror->count].a = nameHash(names[mirror->count].left);
            mirror->pairs[mirror->count].b = nameHash(names[mirror->count].right);
        }
        return block;
    }
    READABLE_FUNCTION_WAITS(newBoneMirror, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t freeSized(ee::Ptr<BoneMirror> memory, uint32_t bytes) { return ee::tailCall<int32_t>(kFreeSized, memory, bytes); }
        }
    }

    // freeBoneMirror: the pairs freed (freeSized's result, the original's tail
    // call).
    int32_t freeBoneMirror(ee::Ptr<BoneMirror> mirror) { return tail::freeSized(mirror, mirror->bytes); }
    READABLE_FUNCTION_WAITS(freeBoneMirror, Verified);

    // mirrorPose: the object's pose mirrored: each pair's bones swap their
    // rotations, each turned to Euler angles with y and z negated, and take
    // the first bone's translation with x negated (both: the original reads
    // the first bone's twice).
    void mirrorPose(ee::Ptr<BoneMirror> mirror)
    {
        ee::Local<Quaternion> rotationA;
        ee::Local<Quaternion> rotationB;
        ee::Local<Matrix> matrixA;
        ee::Local<Matrix> matrixB;
        ee::Local<EulerAngles> anglesA;
        ee::Local<EulerAngles> anglesB;
        ee::Local<Vec4> translationA;
        ee::Local<Vec4> translationB;
        for (uint32_t pair = 0; pair < mirror->count; ++pair)
        {
            const ee::Ptr<AnimationChannel> boneA(findAnimatorBone(mirror->object, mirror->pairs[pair].a));
            const ee::Ptr<AnimationChannel> boneB(findAnimatorBone(mirror->object, mirror->pairs[pair].b));
            // ZGE4 leaves a pair alone when the object lacks either bone.
            if constexpr (game::readable::kIsBmx)
                if (!boneA || !boneB)
                    continue;
            rotationA = *boneRotation(boneA);
            rotationB = *boneRotation(boneB);
            translationA = *boneTranslation(boneA);
            translationB = *boneTranslation(boneA);
            quaternionToMatrix(matrixA, rotationA);
            quaternionToMatrix(matrixB, rotationB);
            matrixToEuler(anglesA, matrixA);
            matrixToEuler(anglesB, matrixB);
            anglesB->z = static_cast<Angle>(-anglesB->z);
            anglesA->z = static_cast<Angle>(-anglesA->z);
            anglesA->y = static_cast<Angle>(-anglesA->y);
            anglesB->y = static_cast<Angle>(-anglesB->y);
            translationA->x = -translationA->x;
            translationB->x = -translationB->x;
            quaternionFromEuler(rotationA, anglesA);
            quaternionFromEuler(rotationB, anglesB);
            setBoneRotation(boneB, rotationA);
            setBoneRotation(boneA, rotationB);
            setBoneTranslation(boneB, translationA);
            setBoneTranslation(boneA, translationB);
        }
    }
    READABLE_FUNCTION(mirrorPose, Verified);

    // ---- animators copied and freed ----------------------------------------

    // copyAnimatorPose (no caller in this build): the source object's pose
    // copied to the object's animator: the channels' rotation, translation,
    // scale and flags. The original steps its animator pointers by an
    // animator's size and its channels not at all: each round copies the
    // first channel again and reads the next "animator"'s count, and the
    // changes word goes to and from where the walk ended.
    uint32_t copyAnimatorPose(ee::Ptr<SceneObject> object, ee::Ptr<SceneObject> source)
    {
        ee::Ptr<Animator> to = object->animator;
        ee::Ptr<Animator> from = source->animator;
        const ee::Ptr<AnimationChannel> toChannel = to->channels;
        const ee::Ptr<AnimationChannel> fromChannel = from->channels;
        if (to->channelCount != 0)
        {
            uint32_t copied = 0;
            for (;;)
            {
                ++to;
                memoryCopy(&toChannel->rotation, &fromChannel->rotation, sizeof(Vec4));
                ++copied;
                ++from;
                memoryCopy(toChannel, fromChannel, sizeof(Vec4));
                memoryCopy(&toChannel->scale, &fromChannel->scale, sizeof(Vec4));
                toChannel->flags = fromChannel->flags;
                if (!(copied < to->channelCount))
                    break;
            }
        }
        const uint32_t changes = from->changes;
        to->changes = changes;
        return changes;
    }
    READABLE_FUNCTION(copyAnimatorPose, Verified);

    // invalidateAnimator: the object's pose marked out of date with every change
    // pending (kPoseChangesPending).
    int32_t invalidateAnimator(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> animator(sceneObjectAnimator(object));
        markPoseStale(object);
        animator->changes |= kPoseChangesPending;
        return static_cast<int32_t>(kPoseChangesPending);
    }
    READABLE_FUNCTION(invalidateAnimator, Verified);

    // A new clear track record on `copy` for each of the source channel's
    // tracks, linked to each other; the copy's mix cursor goes to the record
    // standing where the source's stands.
    static void newTracksLike(ee::Ptr<AnimationChannel> source, ee::Ptr<AnimationChannel> copy)
    {
        ee::Ptr<AnimationTrack> previous;
        for (ee::Ptr<AnimationTrack> track = source->tracks; track; track = track->next)
        {
            const ee::Ptr<AnimationTrack> made(newTrack());
            if (previous)
                previous->next = made;
            made->previous = previous;
            if (same(source->mixNext, track))
                copy->mixNext = made;
            previous = made;
        }
    }

    // cloneAnimator (no caller in this build): a copy of the object's
    // animator (0 for none): the header and channels copied, a new track
    // record for each of a channel's tracks, linked to each other (the
    // records left clear; the copied channels' track lists still the
    // source's, their mix cursor the copy's record where it was).
    uint32_t cloneAnimator(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> source = object->animator;
        if (!source)
            return 0;
        const uint32_t block = heapAllocate(source->channelCount * kAnimationChannelBytes + kAnimatorBytes);
        const ee::Ptr<Animator> copy(block);
        memoryCopy(copy, source, kAnimatorBytes);
        copy->channels = ee::Ptr<AnimationChannel>(block + kAnimatorBytes);
        for (uint32_t index = 0; index < source->channelCount; ++index)
        {
            memoryCopy(channelAt(copy->channels, index), channelAt(source->channels, index), kAnimationChannelBytes);
            newTracksLike(channelAt(source->channels, index), channelAt(copy->channels, index));
        }
        return block;
    }
    READABLE_FUNCTION_WAITS(cloneAnimator, Verified);

    // copyAnimator (no caller in this build): the source animator's
    // channels copied onto the object's (its tracks taken off first, a new
    // clear record per source track, as cloneAnimator's), its changes and
    // its start, stopped word and events, and the object marked
    // (invalidateAnimator's result, the original's tail call).
    void copyAnimator(ee::Ptr<SceneObject> object, ee::Ptr<Animator> source)
    {
        const ee::Ptr<Animator> animator = object->animator;
        if (!source)
            return;
        if (animator)
        {
            for (uint32_t index = 0; index < source->channelCount; ++index)
            {
                const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
                while (channel->tracks)
                    removeTrack(channel, channel->tracks);
                memoryCopy(channel, channelAt(source->channels, index), kAnimationChannelBytes);
                newTracksLike(channelAt(source->channels, index), channel);
                animator->changes = source->changes;
                animator->start = source->start;
                animator->stopped = source->stopped;
                animator->firstEvent = source->firstEvent;
                animator->events = source->events;
            }
        }
        game::tail(invalidateAnimator, object);
    }
    READABLE_FUNCTION_WAITS(copyAnimator, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t heapFree(ee::Ptr<Animator> block, uint32_t count) { return ee::tailCall<int32_t>(kHeapFree, block, count); }
        }
    }

    // freeAnimator (no caller in this build): an animator freed, its tracks
    // taken off first.
    void freeAnimator(ee::Ptr<Animator> animator)
    {
        if (!animator)
            return;
        uint32_t count = animator->channelCount;
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!(channelAt(animator->channels, index))->tracks)
                continue;
            while ((channelAt(animator->channels, index))->tracks)
            {
                const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
                removeTrack(channel, channel->tracks);
            }
            count = animator->channelCount;
        }
        tail::heapFree(animator, count);
    }
    READABLE_FUNCTION_WAITS(freeAnimator, Verified);

    // ---- a playing motion's events -----------------------------------------

    // toggleAnimationEventHook: the event hook installed when none is (its result 0),
    // removed when one is (its result that hook).
    uint32_t toggleAnimationEventHook(uint32_t hook)
    {
        const uint32_t installed = *g_animationEventHook;
        if (installed != 0)
        {
            *g_animationEventHook = 0;
            return installed;
        }
        *g_animationEventHook = hook;
        return installed;
    }

    // setAnimationEventHook: ZGE2's toggleAnimationEventHook: the event hook set to `hook` (0: none), whatever
    // was installed.
    void setAnimationEventHook(uint32_t hook)
    {
        *g_animationEventHook = hook;
    }
    READABLE_FUNCTION_OWN(toggleAnimationEventHook, Verified, dm2, setAnimationEventHook);

    // nextEventOfKind, previousEventOfKind: the event after (before) this one of a
    // kind, or 0.
    ee::Ptr<AnimationEvent> nextEventOfKind(uint32_t kind, ee::Ptr<AnimationEvent> event)
    {
        ee::Ptr<AnimationEvent> found = event->next;
        while (found && found->definition->kind != kind)
            found = found->next;
        return found;
    }
    READABLE_FUNCTION(nextEventOfKind, Verified);

    ee::Ptr<AnimationEvent> previousEventOfKind(uint32_t kind, ee::Ptr<AnimationEvent> event)
    {
        ee::Ptr<AnimationEvent> found = event->previous;
        while (found && found->definition->kind != kind)
            found = found->previous;
        return found;
    }
    READABLE_FUNCTION(previousEventOfKind, Verified);

    // eventTime: an event's time in seconds from the motion's start.
    ee::Float eventTime(ee::Ptr<AnimationEvent> event)
    {
        return ee::toFloat(static_cast<int32_t>(event->time)) * ee::Float(kKeyTimeUnit);
    }
    READABLE_FUNCTION(eventTime, Verified);

    // motionEvents (nothing calls it in AI): a motion's first event. BMX
    // XXX's one caller walks them and takes eventTime of each.
    ee::Ptr<AnimationEvent> motionEvents(ee::Ptr<Motion> motion)
    {
        return motion->events;
    }
    READABLE_FUNCTION(motionEvents, Draft);

    // newestTrackLength (ZGE4 only): the length (end less start) of the track started last on the object's first
    // channel; 0 without an animator or a track.
    inline ee::Float newestTrackLength(ee::Ptr<SceneObject> object)
    {
        constexpr ee::Float kBeforeAnyStart(-100.0f);
        const ee::Ptr<Animator> animator = object->animator;
        ee::Float length(0.0f);
        if (!animator)
            return length;
        ee::Ptr<AnimationTrack> track = animator->channels->tracks;
        if (!track)
            return length;
        ee::Float latestStart = kBeforeAnyStart;
        for (; track; track = track->next)
            if (latestStart < track->start)
            {
                length = track->end - track->start;
                latestStart = track->start;
            }
        return length;
    }
    READABLE_FUNCTION_ONLY(bmx, newestTrackLength);

    // eventInt, eventFloat: an event's field (by its id) as an
    // integer or a float (copied out of the record), 0 when it has none.
    uint32_t eventInt(ee::Ptr<AnimationEvent> event, uint32_t fieldId)
    {
        ee::Local<uint32_t> value;
        const ee::Ptr<EventDefinition> definition = event->definition;
        *value = 0;
        ee::Ptr<EventField> field = definition->fields;
        for (;;)
        {
            if (!field)
                break;
            if (field->id == fieldId)
            {
                memoryCopy(value, event->record + field->offset, 4);
                break;
            }
            field = field->next;
        }
        return *value;
    }
    READABLE_FUNCTION(eventInt, Verified);

    ee::Float eventFloat(ee::Ptr<AnimationEvent> event, uint32_t fieldId)
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<EventDefinition> definition = event->definition;
        *value = ee::Float(0.0f);
        for (ee::Ptr<EventField> field = definition->fields; field; field = field->next)
            if (field->id == fieldId)
            {
                memoryCopy(value, event->record + field->offset, sizeof(ee::Float));
                break;
            }
        return *value;
    }
    READABLE_FUNCTION(eventFloat, Verified);

    // eventFieldAddress, eventFieldAddressCopy (word for word the same; neither is called
    // in this build): where an event's field (by its id) sits in its
    // record, 0 when it has none.
    namespace
    {
        uint32_t fieldAddress(ee::Ptr<AnimationEvent> event, uint32_t fieldId)
        {
            uint32_t address = 0;
            ee::Ptr<EventField> field = event->definition->fields;
            for (;;)
            {
                if (!field)
                    break;
                if (field->id == fieldId)
                {
                    address = event->record + field->offset;
                    break;
                }
                field = field->next;
            }
            return address;
        }
    }
    uint32_t eventFieldAddress(ee::Ptr<AnimationEvent> event, uint32_t fieldId) { return fieldAddress(event, fieldId); }
    READABLE_FUNCTION(eventFieldAddress, Verified);
    uint32_t eventFieldAddressCopy(ee::Ptr<AnimationEvent> event, uint32_t fieldId) { return fieldAddress(event, fieldId); }
    READABLE_FUNCTION(eventFieldAddressCopy, Verified);

    // eventKind: an event's kind.
    uint32_t eventKind(ee::Ptr<AnimationEvent> event) { return event->definition->kind; }
    READABLE_FUNCTION(eventKind, Verified);

    // motionLength: a motion's length in seconds: its first translation
    // track's last key, else its first rotation track's, else 0.
    ee::Float motionLength(ee::Ptr<Motion> motion)
    {
        const ee::Ptr<TranslationTrack> translations = motion->translations;
        uint32_t lastTime = 0;
        if (translations)
        {
            if (translations->packed == 0)
                return ee::Ptr<TranslationKey>(translations->keys)[translations->count - 1].time;
            lastTime = ee::Ptr<PackedTranslationKey>(translations->keys)[translations->count - 1].time;
        }
        else
        {
            const ee::Ptr<RotationTrack> rotations = motion->rotations;
            if (!rotations)
                return ee::Float(0.0f);
            if constexpr (kRotationTracksKeepForm)
                if (rotationKeysPacked(rotations)) // ZGE4's packed keys
                    return ee::toFloat(static_cast<int32_t>(packedRotationKeys(rotations)[rotationKeyCount(rotations) - 1].time)) *
                           ee::Float(kKeyTimeUnit);
            lastTime = rotations->keys[rotationKeyCount(rotations) - 1].time;
        }
        return ee::toFloat(static_cast<int32_t>(lastTime)) * ee::Float(kKeyTimeUnit);
    }
    READABLE_FUNCTION(motionLength, Verified);

    uint32_t animateStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(animateStubZero, Draft);

    void animateNoop()
    {
    }
    READABLE_FUNCTION(animateNoop, Draft);

    int32_t freeAnimateRecord(ee::Ptr<ZpdnRecord> animateSystem)
    {
        if (!animateSystem)
        {
            return 0;
        }
        freeZpdnRecordLists(animateSystem);
        return 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(freeAnimateRecord, Draft);

}
