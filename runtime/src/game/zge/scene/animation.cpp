// zanim.c's playing (zge/scene/animation.h): a motion's keys sampled at a
// time, an object's animator made and freed, motions started on it (alone,
// crossfaded, layered by bone weights, or blended in at their keys), its
// tracks taken off, and the update: the events up to the time fired
// through the game's hook; then each channel's tracks sampled in turn
// (rotation, translation, scale), each weighted by its fade and summed, and
// the sums normalised into the channel's result; what changed is recorded
// for the pose update (zge/scene/pose.cpp), and the object marked for it.
//

#include "animation.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/field.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/animation.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kHeapFree = sym::heapFree;       // (block, ...): hands its other registers on

        // The game's event hook: (object, event, the event's time); it may
        // set the animator's stopped word.
        inline ee::Global<ee::FunctionPtr<void(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event, ee::Float time)>,
                          sym::g_animationEventHook> g_animationEventHook;
        // What the update tells the game: (object, the track's id, the time).
        using AnimationCallback = ee::FunctionPtr<void(ee::Ptr<SceneObject> object, uint32_t trackId, ee::Float time)>;
        // A track weighing less than this does not count.
        inline ee::Global<ee::Float, sym::g_minimumTrackWeight> g_minimumTrackWeight;
        // ZGE2's samplers weigh their track themselves and compare with this, each with its own copy of the
        // literal (the value AI's g_minimumTrackWeight holds).
        constexpr float kMinimumTrackWeight = 1.0e-05f;
        // How long past its end a started track stays on its channel (10000
        // seconds: until something takes it off), and a crossfaded one.
        inline ee::Global<ee::Float, sym::g_trackHold> g_trackHold;
        inline ee::Global<ee::Float, sym::g_crossfadeHold> g_crossfadeHold;
        // The plays' serials (startAnimation's result: the tracks' ids), 27
        // bits.
        inline ee::Global<uint32_t, sym::g_playSerial> g_playSerial;
        constexpr uint32_t kPlaySerialMask = 0x07FFFFFFu;
        // The track id whose game record is kept apart (setTrackRecord).
        inline ee::Global<uint32_t, sym::g_heldTrackId> g_heldTrackId;
        inline ee::Global<uint32_t, sym::g_heldTrackRecord> g_heldTrackRecord;

        // An object of kind 3 (zge/scene/scene.h) has one channel that takes
        // any bone's tracks (ZGE4's kind 5: BMX XXX's newCamera makes its objects so).
        constexpr uint32_t kObjectKindSingleChannel = game::readable::perGame(3u, 3u, 5u);
        // What an update keeps of the animator's changes word.
        constexpr uint32_t kChangesKept = 0xFFF8u;

        ee::Float keyTime(uint32_t time) { return ee::toFloat(static_cast<int32_t>(time)) * ee::Float(kKeyTimeUnit); }
        ee::Float packed(int16_t component) { return ee::toFloat(component) * ee::Float(kPackedKeyUnit); }
        bool sameSetAsPrevious(ee::Ptr<AnimationEvent> event) { return event->previous->set == event->set; }
        uint32_t objectKind(ee::Ptr<SceneObject> object) { return static_cast<uint32_t>(object->flags) & kObjectKindMask; }

        // The time a track list's last key gives (the motion's end on that
        // track).
        ee::Float lastKeyTime(ee::Ptr<RotationTrack> track)
        {
            if (rotationKeysPacked(track)) // ZGE4's packed keys
                return keyTime(packedRotationKeys(track)[rotationKeyCount(track) - 1].time);
            return keyTime(track->keys[rotationKeyCount(track) - 1].time);
        }
        ee::Float lastKeyTime(ee::Ptr<ScaleTrack> track) { return keyTime(track->keys[track->count - 1].time); }

        // The key before the time t in a track, searched from the last one
        // used.
        template <typename TimeOf>
        uint32_t keyBefore(uint32_t key, uint32_t count, ee::Float t, TimeOf timeOf)
        {
            if (t < timeOf(key))
            {
                while (key != 0 && !(timeOf(key) < t))
                    --key;
                return key;
            }
            while (key < count - 1u && !(t < timeOf(key)))
                ++key;
            return key - 1;
        }

        // A list sorted by bone, walked on past the bones below the channel's.
        template <typename Track>
        ee::Ptr<Track> skipBelowChannel(ee::Ptr<Track> track, ee::Ptr<AnimationChannel> channel)
        {
            while (track && track->boneId < channel->boneId)
                track = track->next;
            return track;
        }
        template <typename Track>
        bool onChannel(ee::Ptr<Track> track, ee::Ptr<AnimationChannel> channel)
        {
            return track && track->boneId == channel->boneId;
        }
    }

    // topObject: the object at the top of what it is attached to.
    ee::Ptr<SceneObject> topObject(ee::Ptr<SceneObject> object)
    {
        auto holder = [](ee::Ptr<SceneObject> o) -> ee::Ptr<SceneObject> {
            const ee::Ptr<ObjectAttachment> attachment = o->attachment;
            return attachment ? attachment->parent : ee::Ptr<SceneObject>();
        };
        ee::Ptr<SceneObject> above = holder(object);
        while (above)
        {
            object = above;
            above = holder(object);
            if (!above)
                break;
        }
        return object;
    }
    READABLE_FUNCTION(topObject, Verified);

    // markPoseStale: the pose of the object (the top of what it hangs from)
    // marked out of date.
    ee::Ptr<SceneObject> markPoseStale(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<SceneObject> top = topObject(object);
        top->flags |= kObjectPoseStale;
        return top;
    }
    READABLE_FUNCTION(markPoseStale, Verified);

    // ---- a track off its channel -------------------------------------------

    // removeTrack: the track taken off its channel (linked out of the
    // channel's list) and freed (freeTrack's result, the original's tail
    // call).
    int32_t removeTrack(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track)
    {
        ee::Ptr<AnimationTrack> next = track->next;
        ee::Ptr<AnimationTrack> previous = track->previous;
        if (next)
        {
            next->previous = previous;
            next = track->next;
            previous = track->previous;
        }
        if (previous)
            previous->next = next;
        else
            channel->tracks = next;
        return game::tail(freeTrack, track);
    }
    READABLE_FUNCTION(removeTrack, Verified);

    // ---- ZGE2: a channel's tracks singly linked -------------------------------
    //
    // ZGE2 keeps no pool of tracks: one taken off goes onto a list of free
    // ones (g_freeTracks), and a channel's tracks are linked one way, so the
    // one before is handed in.

    namespace
    {
        inline ee::Global<ee::Ptr<AnimationTrack>, sym::dm2::g_freeTracks> g_freeTracks;
    }

    // releaseTrack (ZGE2 only): a track put on the free tracks' list.
    inline void releaseTrack(ee::Ptr<AnimationTrack> track)
    {
        track->next = *g_freeTracks;
        *g_freeTracks = track;
    }
    READABLE_FUNCTION_ONLY(dm2, releaseTrack);

    // unlinkTrack (ZGE2 only; ZGE2's removeTrack): the track taken off its channel (`previous` the
    // track before it, 0 for the channel's first) and released (the original's tail call).
    inline void unlinkTrack(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Ptr<AnimationTrack> previous)
    {
        if (previous)
            previous->next = track->next;
        else
            channel->tracks = track->next;
        game::tail(releaseTrack, track);
    }
    READABLE_FUNCTION_ONLY(dm2, unlinkTrack);

    // ---- a motion's keys at a time -----------------------------------------
    //
    // The key functions (blendLayer's) take the first key at or after the
    // time (the last if none), unblended; the samplers (the update's)
    // search from the key the track used last and interpolate between the
    // keys either side.

    namespace
    {
        // A rotation track's key at a time from its keys of the form Key (each
        // component Unit): the first at or after the time (the last if none),
        // into a quaternion; the key's w.
        template <typename Key, float Unit>
        [[gnu::always_inline]] inline int32_t rotationKeyAt(ee::Ptr<RotationTrack> track, ee::Ptr<Quaternion> out, ee::Float time)
        {
            uint32_t key = 0;
            const uint32_t count = rotationKeyCount(track);
            const ee::Ptr<Key> keys = track->keys.cast<Key>();
            while (key < count && !(time < keyTime(keys[key].time)))
                ++key;
            if (key == count)
                --key;
            const Key &chosen = track->keys.cast<Key>()[key];
            const ee::Float unit = ee::Float(Unit);
            out->x = ee::toFloat(chosen.x) * unit;
            out->y = ee::toFloat(chosen.y) * unit;
            out->z = ee::toFloat(chosen.z) * unit;
            out->w = ee::toFloat(chosen.w) * unit;
            return chosen.w;
        }
    }

    // ---- ZGE2: the key functions on float keys --------------------------------

    namespace
    {
        // A float key list's key at a time: the first past the time (the last if none).
        template <typename Key>
        [[gnu::always_inline]] inline uint32_t floatKeyAt(ee::Ptr<Key> keys, uint32_t count, ee::Float time)
        {
            uint32_t key = 0;
            while (key < count && !(time < keys[key].time))
                ++key;
            if (key == count)
                --key;
            return key;
        }
        // A float rotation key's quaternion (its x, y, z and w, a word into the key).
        ee::Ptr<Quaternion> keyRotation(ee::Ptr<FloatRotationKey> key)
        {
            return ee::field(key, &FloatRotationKey::x).cast<Quaternion>();
        }
    }

    // copyFloatRotationKey: ZGE2's sampleRotationKey: the rotation track's float key at a time (the first past
    // it, the last if none) copied into `out` (quaternionCopy's result, the original's tail call).
    ee::Ptr<Quaternion> copyFloatRotationKey(ee::Ptr<RotationTrack> track, ee::Ptr<Quaternion> out, ee::Float time)
    {
        const ee::Ptr<FloatRotationKey> keys = track->keys.cast<FloatRotationKey>();
        const uint32_t key = floatKeyAt(keys, track->count, time);
        return game::tail(quaternionCopy, out, keyRotation(keys + key));
    }

    // copyFloatTranslationKey: ZGE2's sampleTranslationKey: the translation track's float key at a time (as
    // copyFloatRotationKey's) copied into `out`, the whole quadword (w too).
    void copyFloatTranslationKey(ee::Ptr<FloatTranslationTrack> track, ee::Ptr<Vec4> out, ee::Float time)
    {
        const ee::Ptr<FloatTranslationKey> keys = track->keys;
        *out = keys[floatKeyAt(keys, track->count, time)].translation;
    }

    // copyFloatScaleKey (no caller in DM2): ZGE2's sampleScaleKey, copyFloatTranslationKey's code on a scale
    // track (ZGE2 reads no ASCA chunk, so a motion has none; the code reads one with the float translation
    // keys' layout).
    void copyFloatScaleKey(ee::Ptr<ScaleTrack> track, ee::Ptr<Vec4> out, ee::Float time)
    {
        const ee::Ptr<FloatTranslationKey> keys = track->keys.cast<FloatTranslationKey>();
        *out = keys[floatKeyAt(keys, track->count, time)].translation;
    }

    // sampleRotationKey: a rotation track's key at a time, into a quaternion; the
    // key's w.
    int32_t sampleRotationKey(ee::Ptr<RotationTrack> track, ee::Ptr<Quaternion> out, ee::Float time)
    {
        if (rotationKeysPacked(track)) // ZGE4's packed keys
            return rotationKeyAt<PackedRotationKey, kPackedKeyUnit>(track, out, time);
        return rotationKeyAt<RotationKey, kRotationKeyUnit>(track, out, time);
    }
    READABLE_FUNCTION_OWN(sampleRotationKey, Verified, dm2, copyFloatRotationKey);

    // sampleTranslationKey: a translation track's key at a time, into a point (x,
    // y, z). The float keys' search compares the time with each key's
    // first half-word, stepping 8 bytes a key, as the packed keys are laid
    // out (the original's: it reads ATRW's keys with ATRA's layout).
    void sampleTranslationKey(ee::Ptr<TranslationTrack> track, ee::Ptr<Vec3> out, ee::Float time)
    {
        const uint32_t count = track->count;
        const ee::Ptr<PackedTranslationKey> keys(track->keys);
        uint32_t key = 0;
        if (track->packed != 0)
        {
            while (key < count && !(time < keyTime(keys[key].time)))
                ++key;
            if (key == count)
                --key;
            const PackedTranslationKey &chosen = keys[key];
            out->x = packed(chosen.x);
            out->y = packed(chosen.y);
            out->z = packed(chosen.z);
            return;
        }
        while (key < count && !(time < ee::toFloat(static_cast<int32_t>(keys[key].time))))
            ++key;
        if (key == count)
            --key;
        const TranslationKey &chosen = ee::Ptr<TranslationKey>(track->keys)[key];
        out->x = chosen.x;
        out->y = chosen.y;
        out->z = chosen.z;
    }
    READABLE_FUNCTION_OWN(sampleTranslationKey, Verified, dm2, copyFloatTranslationKey);

    // sampleScaleKey: a scale track's key at a time, into a scale (x, y, z).
    void sampleScaleKey(ee::Ptr<ScaleTrack> track, ee::Ptr<Vec3> out, ee::Float time)
    {
        uint32_t key = 0;
        const uint32_t count = track->count;
        const ee::Ptr<ScaleKey> keys = track->keys;
        while (key < count && !(time < keyTime(keys[key].time)))
            ++key;
        if (key == count)
            --key;
        const ScaleKey &chosen = track->keys[key];
        out->x = packed(chosen.x);
        out->y = packed(chosen.y);
        out->z = packed(chosen.z);
    }
    READABLE_FUNCTION_OWN(sampleScaleKey, Verified, dm2, copyFloatScaleKey);

    namespace
    {
        // A track's weight at a time before the minimum: its weight, faded in from its start to fadeInEnd and
        // out from fadeOutStart to fadeOutEnd (trackWeight's; ZGE2's samplers have it inline).
        [[gnu::always_inline]] inline ee::Float fadedWeight(ee::Ptr<AnimationTrack> track, ee::Float time)
        {
            ee::Float level;
            const ee::Float fadeInEnd = track->fadeInEnd;
            if (time < fadeInEnd)
            {
                const ee::Float start = track->start;
                level = (time - start) / (fadeInEnd - start);
            }
            else
            {
                const ee::Float fadeOutStart = track->fadeOutStart;
                if (time < fadeOutStart)
                    level = ee::Float(1.0f);
                else
                {
                    const ee::Float fadeOutEnd = track->fadeOutEnd;
                    if (time < fadeOutEnd)
                        level = ee::Float(1.0f) - (time - fadeOutStart) / (fadeOutEnd - fadeOutStart);
                    else
                        level = ee::Float(0.0f);
                }
            }
            const ee::Float weight = track->weight;
            return level * weight;
        }
    }

    // trackWeight: a track's weight at a time: its weight, faded in from
    // its start to fadeInEnd and out from fadeOutStart to fadeOutEnd; 0
    // under the minimum.
    ee::Float trackWeight(ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Float result = fadedWeight(track, time);
        if (result < *g_minimumTrackWeight)
            result = ee::Float(0.0f);
        return result;
    }
    READABLE_FUNCTION(trackWeight, Verified);

    // ---- ZGE2: the samplers weigh their track and add it in ---------------------
    //
    // ZGE2's update keeps no sums apart: each sampler weighs the track (trackWeight's fades, inline), and one
    // that counts adds its sample times the weight into the channel's key, cleared by the frame's first track
    // with that part. The sampler's result is the weight (0: under the minimum, nothing added).

    namespace
    {
        // A float vector key list's keys either side of t (found from lastKey, which keeps the one before),
        // lerped into sample's x, y and z.
        [[gnu::always_inline]] inline void lerpFloatKeys(ee::Ptr<FloatTranslationTrack> keyList, uint32_t &lastKey, ee::Float t, Vec4 &sample)
        {
            const ee::Ptr<FloatTranslationKey> keys = keyList->keys;
            const uint32_t key = keyBefore(lastKey, keyList->count, t, [&](uint32_t k) { return keys[k].time; });
            lastKey = key;
            const ee::Ptr<FloatTranslationKey> a = keys + key;
            const ee::Ptr<FloatTranslationKey> b = keys + (key + 1);
            vec3Lerp(&sample.xyz(), &a->translation.xyz(), &b->translation.xyz(), (t - a->time) / (b->time - a->time));
        }
    }

    // addWeightedRotation: ZGE2's sampleRotation: the track's rotation at a time (clamped to its end: the float
    // keys either side found from the last one used, slerped) times the track's weight added into the
    // channel's. The weight.
    ee::Float addWeightedRotation(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Local<Quaternion> sample;
        const ee::Float clamped = track->end < time ? track->end : time;
        const ee::Float weight = fadedWeight(track, time);
        if (weight < ee::Float(kMinimumTrackWeight))
            return ee::Float(0.0f);
        if (!(channel->flags & kChannelRotation))
        {
            channel->flags |= kChannelRotation;
            channel->rotation = Quaternion{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f)};
        }
        const ee::Ptr<RotationTrack> rotations = track->rotations;
        const ee::Ptr<FloatRotationKey> keys = rotations->keys.cast<FloatRotationKey>();
        const ee::Float t = clamped - track->start;
        const uint32_t key = keyBefore(track->rotationKey, rotations->count, t, [&](uint32_t k) { return keys[k].time; });
        track->rotationKey = key;
        const ee::Ptr<FloatRotationKey> a = keys + key;
        const ee::Ptr<FloatRotationKey> b = keys + (key + 1);
        quaternionSlerp(sample, keyRotation(a).get(), keyRotation(b).get(), (t - a->time) / (b->time - a->time));
        quaternionScale(sample, sample, weight);
        quaternionAccumulate(&channel->rotation, sample, &channel->rotation);
        return weight;
    }

    // addWeightedTranslation: ZGE2's sampleTranslation: the track's translation at a time (as
    // addWeightedRotation's keys, lerped) times its weight added into the channel's (all four lanes: the
    // sample's w is what the scale left). The weight.
    ee::Float addWeightedTranslation(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Local<Vec4> sample;
        const ee::Float clamped = track->end < time ? track->end : time;
        const ee::Float weight = fadedWeight(track, time);
        if (weight < ee::Float(kMinimumTrackWeight))
            return ee::Float(0.0f);
        if (!(channel->flags & kChannelTranslation))
        {
            channel->flags |= kChannelTranslation;
            channel->translation.xyz() = Vec3{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f)};
        }
        lerpFloatKeys(track->translations.cast<FloatTranslationTrack>(), track->translationKey, clamped - track->start, *sample);
        *sample = *sample * weight;
        channel->translation = *sample + channel->translation;
        return weight;
    }

    // addWeightedScale (never reached in DM2: ZGE2 reads no ASCA chunk, so no track has scales): ZGE2's
    // sampleScale, addWeightedTranslation's code on the scale (a scale track read with the float translation
    // keys' layout). The weight.
    ee::Float addWeightedScale(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Local<Vec4> sample;
        const ee::Float clamped = track->end < time ? track->end : time;
        const ee::Float weight = fadedWeight(track, time);
        if (weight < ee::Float(kMinimumTrackWeight))
            return ee::Float(0.0f);
        if (!(channel->flags & kChannelScale))
        {
            channel->flags |= kChannelScale;
            channel->scale.xyz() = Vec3{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f)};
        }
        lerpFloatKeys(track->scales.cast<FloatTranslationTrack>(), track->scaleKey, clamped - track->start, *sample);
        *sample = *sample * weight;
        channel->scale = *sample + channel->scale;
        return weight;
    }

    namespace
    {
        // The track's rotation at a time (clamped to its end) from its keys of
        // the form Key (each component Unit): the keys either side found from
        // the last one used, blended into the channel's.
        template <typename Key, float Unit>
        [[gnu::always_inline]] inline void sampleRotationKeys(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
        {
            ee::Local<Quaternion> from;
            ee::Local<Quaternion> to;
            ee::Float clamped = time;
            if (track->end < time)
                clamped = track->end;
            uint32_t key = track->rotationKey;
            const ee::Ptr<RotationTrack> keys = track->rotations;
            const ee::Float t = clamped - track->start;
            key = keyBefore(key, rotationKeyCount(keys), t, [&](uint32_t k) { return keyTime(keys->keys.cast<Key>()[k].time); });
            track->rotationKey = key;
            const Key &a = keys->keys.cast<Key>()[key];
            const Key &b = keys->keys.cast<Key>()[key + 1];
            const ee::Float unit = ee::Float(Unit);
            *from = Quaternion{ee::toFloat(a.x) * unit, ee::toFloat(a.y) * unit, ee::toFloat(a.z) * unit, ee::toFloat(a.w) * unit};
            *to = Quaternion{ee::toFloat(b.x) * unit, ee::toFloat(b.y) * unit, ee::toFloat(b.z) * unit, ee::toFloat(b.w) * unit};
            const ee::Float along = (t - keyTime(a.time)) / (ee::toFloat(static_cast<int32_t>(b.time) - a.time) * ee::Float(kKeyTimeUnit));
            // ZGE4 blends the keys by a normalised lerp, ZGE3 by a slerp.
            if constexpr (game::readable::kIsBmx)
                quaternionNlerp(&channel->rotation, ee::Ptr<Quaternion>(from.address()).get(), ee::Ptr<Quaternion>(to.address()).get(), along);
            else
                quaternionSlerp(&channel->rotation, ee::Ptr<Quaternion>(from.address()).get(), ee::Ptr<Quaternion>(to.address()).get(), along);
        }
    }

    // sampleRotation: the track's rotation at a time (clamped to its end):
    // the keys either side found from the last one used, slerped.
    void sampleRotation(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        // ZGE4's tracks keep their keys' form: packed or full keys.
        if constexpr (kRotationTracksKeepForm)
        {
            if (rotationKeysPacked(track->rotations))
                return sampleRotationKeys<PackedRotationKey, kPackedKeyUnit>(channel, track, time);
            return sampleRotationKeys<RotationKey, kRotationKeyUnit>(channel, track, time);
        }
        ee::Local<Quaternion> from;
        ee::Local<Quaternion> to;
        ee::Float clamped = time;
        if (track->end < time)
            clamped = track->end;
        uint32_t key = track->rotationKey;
        const ee::Ptr<RotationTrack> keys = track->rotations;
        const ee::Float t = clamped - track->start;
        key = keyBefore(key, keys->count, t, [&](uint32_t k) { return keyTime(keys->keys[k].time); });
        track->rotationKey = key;
        const RotationKey &a = keys->keys[key];
        const RotationKey &b = keys->keys[key + 1];
        const ee::Float unit = ee::Float(kRotationKeyUnit);
        *from = Quaternion{ee::toFloat(a.x) * unit, ee::toFloat(a.y) * unit, ee::toFloat(a.z) * unit, ee::toFloat(a.w) * unit};
        *to = Quaternion{ee::toFloat(b.x) * unit, ee::toFloat(b.y) * unit, ee::toFloat(b.z) * unit, ee::toFloat(b.w) * unit};
        const ee::Float along = (t - keyTime(a.time)) / (ee::toFloat(static_cast<int32_t>(b.time) - a.time) * ee::Float(kKeyTimeUnit));
        quaternionSlerp(&channel->rotation, ee::Ptr<Quaternion>(from.address()).get(), ee::Ptr<Quaternion>(to.address()).get(), along);
    }
    READABLE_FUNCTION_OWN(sampleRotation, Verified, dm2, addWeightedRotation);

    // sampleTranslation: the track's translation at a time (clamped to its end)
    // into the channel's: the keys either side found from the last one
    // used, interpolated.
    void sampleTranslation(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Local<Vec3> from;
        ee::Local<Vec3> to;
        const ee::Float clamped = track->end < time ? track->end : time;
        const ee::Ptr<TranslationTrack> translations = track->translations;
        const ee::Float t = clamped - track->start;
        if (translations->packed != 0)
        {
            const ee::Ptr<PackedTranslationKey> keys(translations->keys);
            const uint32_t key = keyBefore(track->translationKey, translations->count, t, [&](uint32_t k) { return keyTime(keys[k].time); });
            track->translationKey = key;
            const PackedTranslationKey &a = keys[key];
            const PackedTranslationKey &b = keys[key + 1];
            *from = Vec3{packed(a.x), packed(a.y), packed(a.z)};
            *to = Vec3{packed(b.x), packed(b.y), packed(b.z)};
            const int32_t aTime = a.time;
            const ee::Float along = (t - ee::toFloat(aTime) * ee::Float(kKeyTimeUnit)) /
                                    (ee::toFloat(static_cast<int32_t>(b.time) - aTime) * ee::Float(kKeyTimeUnit));
            vec3Lerp(&channel->translation.xyz(), from, to, along);
            return;
        }

        const ee::Ptr<TranslationKey> keys(translations->keys);
        const uint32_t key = keyBefore(track->translationKey, translations->count, t, [&](uint32_t k) { return keys[k].time; });
        track->translationKey = key;
        const TranslationKey &a = keys[key];
        const TranslationKey &b = keys[key + 1];
        *from = Vec3{a.x, a.y, a.z};
        *to = Vec3{b.x, b.y, b.z};
        const ee::Float aTime = a.time;
        vec3Lerp(&channel->translation.xyz(), from, to, (t - aTime) / (b.time - aTime));
    }
    READABLE_FUNCTION_OWN(sampleTranslation, Verified, dm2, addWeightedTranslation);

    // sampleScale: the track's scale at a time (clamped to its end) into
    // the channel's, as sampleTranslation's packed keys.
    void sampleScale(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track, ee::Float time)
    {
        ee::Local<Vec3> from;
        ee::Local<Vec3> to;
        const ee::Float clamped = track->end < time ? track->end : time;
        const ee::Ptr<ScaleTrack> scales = track->scales;
        const ee::Float t = clamped - track->start;
        const ee::Ptr<ScaleKey> keys = scales->keys;
        const uint32_t key = keyBefore(track->scaleKey, scales->count, t, [&](uint32_t k) { return keyTime(keys[k].time); });
        track->scaleKey = key;
        const ScaleKey &a = keys[key];
        const ScaleKey &b = keys[key + 1];
        *from = Vec3{packed(a.x), packed(a.y), packed(a.z)};
        *to = Vec3{packed(b.x), packed(b.y), packed(b.z)};
        const int32_t aTime = a.time;
        const ee::Float along = (t - ee::toFloat(aTime) * ee::Float(kKeyTimeUnit)) /
                                (ee::toFloat(static_cast<int32_t>(b.time) - aTime) * ee::Float(kKeyTimeUnit));
        vec3Lerp(&channel->scale.xyz(), from, to, along);
    }
    READABLE_FUNCTION_OWN(sampleScale, Verified, dm2, addWeightedScale);

    // fadeOutTrack: the track faded out from `now` over `duration` (a
    // crossfade's old track): its fade-in cut short at now, its end no
    // later than the fade's. Always 1.
    int32_t fadeOutTrack(ee::Ptr<AnimationTrack> track, ee::Float now, ee::Float duration)
    {
        if (now < track->fadeInEnd)
            track->fadeInEnd = now;
        const ee::Float gone = now + duration;
        const ee::Float end = track->end;
        track->fadeOutStart = now;
        track->expires = gone;
        track->fadeOutEnd = gone;
        if (gone < end)
            track->end = gone;
        track->fadingOut = 1;
        return 1;
    }
    READABLE_FUNCTION(fadeOutTrack, Verified);

    // setTrackFadeIn: the track faded in over `duration` from `start` (a
    // crossfade's new track).
    void setTrackFadeIn(ee::Ptr<AnimationTrack> track, ee::Float start, ee::Float duration)
    {
        track->fadeInEnd = start + duration;
    }
    READABLE_FUNCTION(setTrackFadeIn, Verified);

    // ---- an object's animator ----------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Ptr<Animator> block) { ee::tailCall<int32_t>(kHeapFree, block); }
        }
    }

    // createObjectAnimator: the object's animator made (a model's: a channel per
    // bone; kObjectKindSingleChannel: one channel), freeing the old one: the channels sorted
    // by bone id (each takes the next id up of the model's), at rest (unit
    // scale, no rotation). A model's is bound to its scene's bones
    // (setModelAnimator). 0 for another kind of object.
    uint32_t createObjectAnimator(ee::Ptr<SceneObject> object)
    {
        // ZGE2 animates model objects alone: it asks for no kind (and frees no animator the object had).
        const uint32_t kind = game::readable::kIsDm2 ? kObjectModelKind : objectKind(object);
        int32_t channelCount = 0;
        if (kind == kObjectModelKind)
            channelCount = static_cast<int32_t>(modelBoneCount(object));
        else
        {
            if (kind != kObjectKindSingleChannel)
                return 0;
            channelCount = 1;
        }
        if (!game::readable::kIsDm2 && object->animator)
        {
            heapFree(object->animator);
            object->animator = ee::Ptr<Animator>();
        }
        const uint32_t bytes = channelCount * kAnimationChannelBytes + kAnimatorBytes;
        const uint32_t block = heapAllocate(bytes);
        const ee::Ptr<Animator> animator(block);
        memoryFill(animator, 0, bytes);
        const ee::Ptr<uint32_t> boneIds(allocateMemory(channelCount << 2));
        if (kind == kObjectModelKind)
            modelBoneIds(object, boneIds);
        else
            boneIds[0] = 1;
        animator->channelCount = channelCount;
        animator->channels = ee::Ptr<AnimationChannel>(block + kAnimatorBytes);
        ee::Ptr<AnimationChannel> channel = animator->channels;
        uint32_t previousId = 0;
        for (int32_t index = 0; index < channelCount; ++index, channel = channelAt(channel, 1))
        {
            // The lowest id above the previous channel's.
            uint32_t lowest = 0xFFFFFFFFu;
            for (int32_t i = 0; i < channelCount; ++i)
            {
                const uint32_t id = boneIds[i];
                if (id < lowest && previousId < id)
                    lowest = id;
            }
            previousId = lowest;
            if constexpr (!kChannelsKeepNoObject)
                channel->object = object;
            channel->boneId = lowest;
            channel->rotation = Quaternion{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
            channel->scale.x = ee::Float(1.0f);
            channel->scale.y = ee::Float(1.0f);
            channel->scale.z = ee::Float(1.0f);
        }
        freeMemory(boneIds);
        if (kind == kObjectModelKind)
            setModelAnimator(object, animator);
        else
            object->animator = animator;
        return block;
    }
    READABLE_FUNCTION_WAITS(createObjectAnimator, Verified);

    // freeObjectAnimator: the object's animator freed, its tracks taken off first.
    void freeObjectAnimator(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> animator = object->animator;
        if (!animator)
            return;
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            while (channel->tracks)
            {
                if constexpr (game::readable::kIsDm2)
                    unlinkTrack(channel, channel->tracks, ee::Ptr<AnimationTrack>());
                else
                    removeTrack(channel, channel->tracks);
            }
        }
        // ZGE2 hands the animator to heapFree as its last act and leaves the object's word as it was.
        if constexpr (game::readable::kIsDm2)
            return tail::heapFree(object->animator);
        heapFree(object->animator);
        object->animator = ee::Ptr<Animator>();
    }
    READABLE_FUNCTION_WAITS(freeObjectAnimator, Verified);

    // shiftTracks: every track of the object's animator moved earlier by
    // `seconds` (its start, end, expiry and fades).
    void shiftTracks(ee::Ptr<SceneObject> object, ee::Float seconds)
    {
        const ee::Ptr<Animator> animator = object->animator;
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            for (ee::Ptr<AnimationTrack> track = channelRecord(animator->channels, index).tracks; track; track = track->next)
            {
                track->end = track->end - seconds;
                track->expires = track->expires - seconds;
                track->start = track->start - seconds;
                track->fadeInEnd = track->fadeInEnd - seconds;
                track->fadeOutEnd = track->fadeOutEnd - seconds;
                track->fadeOutStart = track->fadeOutStart - seconds;
            }
        }
    }
    READABLE_FUNCTION(shiftTracks, Verified);

    namespace
    {
        template <typename Track>
        ee::Ptr<Track> skipLowerBones(ee::Ptr<Track> track, ee::Ptr<SceneObject> object, ee::Ptr<AnimationChannel> channel)
        {
            while (track && objectKind(object) != kObjectKindSingleChannel && track->boneId < channel->boneId)
                track = track->next;
            return track;
        }
        template <typename Track>
        bool forChannel(ee::Ptr<Track> track, ee::Ptr<SceneObject> object, ee::Ptr<AnimationChannel> channel)
        {
            return objectKind(object) == kObjectKindSingleChannel || track->boneId == channel->boneId;
        }
        ee::Float translationsEnd(ee::Ptr<TranslationTrack> track)
        {
            if (track->packed != 0)
                return keyTime(ee::Ptr<PackedTranslationKey>(track->keys)[track->count - 1].time);
            return ee::Ptr<TranslationKey>(track->keys)[track->count - 1].time;
        }

        // For one channel, a track for the motion's keys of its bone (0:
        // none): the rotations', translations' and scales' lists advanced
        // past lower bones; a track made (start `time`, ending at its
        // first list's last key) for the first that has the bone, the
        // others added to it.
        ee::Ptr<AnimationTrack> trackForChannel(ee::Ptr<SceneObject> object, ee::Ptr<AnimationChannel> channel, ee::Ptr<RotationTrack> &rotations,
                                                ee::Ptr<TranslationTrack> &translations, ee::Ptr<ScaleTrack> &scales, ee::Float time)
        {
            ee::Ptr<AnimationTrack> track;
            const auto make = [&]
            {
                track = ee::Ptr<AnimationTrack>(newTrack());
                track->start = time;
            };
            rotations = skipLowerBones(rotations, object, channel);
            if (rotations && forChannel(rotations, object, channel))
            {
                const ee::Float end = lastKeyTime(rotations);
                make();
                track->rotations = rotations;
                track->end = end + time;
            }
            translations = skipLowerBones(translations, object, channel);
            if (translations && forChannel(translations, object, channel))
            {
                if (!track)
                {
                    make();
                    track->end = translationsEnd(translations) + time;
                }
                track->translations = translations;
            }
            scales = skipLowerBones(scales, object, channel);
            if (scales && forChannel(scales, object, channel))
            {
                if (!track)
                {
                    make();
                    track->end = lastKeyTime(scales) + time;
                }
                track->scales = scales;
            }
            return track;
        }

        // The playing motion's first event of a set for the object: set 0
        // (everyone's), or the one its class's id hook gives (a single-
        // channel object takes the first set); then the animator's events
        // from there.
        void startEvents(ee::Ptr<SceneObject> object, ee::Ptr<Animator> animator, ee::Ptr<AnimationEvent> events, ee::Float time)
        {
            while (events)
            {
                const uint32_t set = events->set;
                if (set == 0 || objectKind(object) == kObjectKindSingleChannel)
                    break;
                // (ZGE4: an object with no class has no id hook to ask.)
                if (hasObjectClass(object))
                {
                    const auto idHook = object->objectClass->id;
                    if (idHook && set == idHook(object))
                        break;
                }
                events = events->next;
            }
            animator->events = events;
            animator->stopped = 1;
            animator->firstEvent = events;
            animator->start = time;
        }
    }

    // startAnimation: the motion started on the object at `time`: every
    // channel's tracks taken off, and a track for the channel's bone put on
    // it, at full weight, ending at its last key and staying g_trackHold
    // longer; the motion's events of the object's set from the start. The
    // play's serial (each track's id).
    uint32_t startAnimation(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time)
    {
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<TranslationTrack> translations = motion->translations;
        ee::Ptr<ScaleTrack> scales = motion->scales;
        *g_playSerial = (*g_playSerial + 1) & kPlaySerialMask;
        startEvents(object, animator, motion->events, time);
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            while (channel->tracks)
                removeTrack(channel, channel->tracks);
            const ee::Ptr<AnimationTrack> track = trackForChannel(object, channel, rotations, translations, scales, time);
            if (track)
            {
                const ee::Float expires = track->end + *g_trackHold;
                track->weight = ee::Float(1.0f);
                track->id = *g_playSerial;
                track->expires = expires;
                track->fadeInEnd = time - ee::Float(1000.0f);
                track->fadeOutStart = expires;
                track->fadeOutEnd = expires + ee::Float(1.0f);
                channel->tracks = track;
            }
        }
        return *g_playSerial;
    }

    // startMotionTracks: ZGE2's startAnimation: the motion started on the
    // object at `time`, as startAnimation starts it, with ZGE2's records:
    // its keys floats, no event sets (the animator's events from the
    // motion's first), no play serial, a track fully in from `time`. Scale
    // tracks are passed over and never played (ZGE2 reads no ASCA chunk, so
    // a motion has none: the original's pass over them would not end at a
    // bone at or past the channel's; kept).
    void startMotionTracks(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time)
    {
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<FloatTranslationTrack> translations = motion->translations.cast<FloatTranslationTrack>();
        ee::Ptr<ScaleTrack> scales = motion->scales;
        animator->events = motion->events;
        animator->firstEvent = motion->events;
        animator->start = time;
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            while (channel->tracks)
                unlinkTrack(channel, channel->tracks, ee::Ptr<AnimationTrack>());
            ee::Ptr<AnimationTrack> track;
            while (rotations && rotations->boneId < channel->boneId)
                rotations = rotations->next;
            if (rotations && rotations->boneId == channel->boneId)
            {
                track = ee::Ptr<AnimationTrack>(newTrack());
                track->start = time;
                track->rotations = rotations;
                track->end = rotations->keys.cast<FloatRotationKey>()[rotations->count - 1].time + time;
            }
            while (translations && translations->boneId < channel->boneId)
                translations = translations->next;
            if (translations && translations->boneId == channel->boneId)
            {
                if (!track)
                {
                    track = ee::Ptr<AnimationTrack>(newTrack());
                    track->start = time;
                    track->end = translations->keys[translations->count - 1].time + time;
                }
                track->translations = translations.cast<TranslationTrack>();
            }
            while (scales)
            {
                if (scales->boneId < channel->boneId)
                    scales = scales->next;
            }
            if (track)
            {
                const ee::Float expires = track->end + *g_trackHold;
                track->weight = ee::Float(1.0f);
                track->fadeInEnd = time;
                track->expires = expires;
                track->fadeOutStart = expires;
                track->fadeOutEnd = expires + ee::Float(1.0f);
                channel->tracks = track;
            }
        }
    }
    READABLE_FUNCTION_WAITS_OWN(startAnimation, Verified, dm2, startMotionTracks);

    // crossfadeAnimation: the motion crossfaded in at `time` over `duration`: each
    // channel's tracks (up to one already fading) faded out, the new track
    // put in front of them, faded in once anything was faded; it stays
    // g_crossfadeHold past its end. The play's serial.
    uint32_t crossfadeAnimation(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time, ee::Float duration)
    {
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<TranslationTrack> translations = motion->translations;
        ee::Ptr<ScaleTrack> scales = motion->scales;
        *g_playSerial = (*g_playSerial + 1) & kPlaySerialMask;
        bool nothingFaded = true;
        startEvents(object, animator, motion->events, time);
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            // The channel's tracks faded out, up to one already fading.
            for (ee::Ptr<AnimationTrack> old = channel->tracks; old && old->fadingOut != 1; old = old->next)
            {
                nothingFaded = false;
                fadeOutTrack(old, time, duration);
            }
            const ee::Ptr<AnimationTrack> track = trackForChannel(object, channel, rotations, translations, scales, time);
            if (track)
            {
                const ee::Float expires = track->end + *g_crossfadeHold;
                track->weight = ee::Float(1.0f);
                track->fadeInEnd = time;
                track->expires = expires;
                track->fadeOutStart = expires;
                track->fadeOutEnd = expires + ee::Float(1.0f);
                const ee::Ptr<AnimationTrack> first = channel->tracks;
                track->id = *g_playSerial;
                track->next = first;
                if (first)
                    first->previous = track;
                channel->tracks = track;
                if (!nothingFaded)
                    setTrackFadeIn(track, time, duration);
            }
        }
        return *g_playSerial & kPlaySerialMask;
    }
    READABLE_FUNCTION_WAITS(crossfadeAnimation, Verified);

    // A bone's weight for layerAnimation (8 bytes; the list sorted by bone).
    struct BoneWeight
    {
        uint32_t boneId;
        ee::Float weight;
    };
    struct BoneWeights
    {
        uint32_t count;
        ee::Ptr<BoneWeight> weights;
    };

    // layerAnimation (no caller in this build): the motion layered over the
    // object's playing tracks at `time`: per channel a track for its bone's
    // keys (one key list at least; without rotations the channel's first
    // track takes the translations and scales), put first, taking the
    // first track's timing and the weight `weights` gives the bone (0.5
    // for a bone it does not list); the tracks after it, up to one fading
    // out, keep 1 - that weight of theirs. The play's serial.
    uint32_t layerAnimation(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Ptr<BoneWeights> weights, ee::Float time)
    {
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<ScaleTrack> scales = motion->scales;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<TranslationTrack> translations = motion->translations;
        *g_playSerial = (*g_playSerial + 1) & kPlaySerialMask;
        uint32_t weightIndex = 0; // the bones' list walked once for all channels
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            ee::Ptr<AnimationTrack> track = channel->tracks;
            const auto make = [&]
            {
                track = ee::Ptr<AnimationTrack>(newTrack());
                track->start = time;
            };
            rotations = skipBelowChannel(rotations, channel);
            if (onChannel(rotations, channel))
            {
                make();
                track->rotations = rotations;
                track->end = lastKeyTime(rotations) + time;
            }
            translations = skipBelowChannel(translations, channel);
            if (onChannel(translations, channel))
            {
                if (!track)
                {
                    make();
                    track->end = translationsEnd(translations) + time;
                }
                track->translations = translations;
            }
            scales = skipBelowChannel(scales, channel);
            if (onChannel(scales, channel))
            {
                if (!track)
                {
                    make();
                    track->end = lastKeyTime(scales) + time;
                }
                track->scales = scales;
            }
            if (!track)
                continue;

            track->next = channel->tracks;
            channel->tracks = track;
            // The bone's weight: the list walked on from the last channel's
            // place to the first entry at or above the bone.
            const uint32_t boneId = channel->boneId;
            const ee::Ptr<BoneWeight> entries = weights->weights;
            while (weightIndex < weights->count - 1u && entries[weightIndex].boneId < boneId)
                ++weightIndex;
            const ee::Float weight = entries[weightIndex].boneId == boneId ? entries[weightIndex].weight : ee::Float(0.5f);
            const ee::Ptr<AnimationTrack> timing = track->next;
            track->weight = weight;
            track->fadeInEnd = timing->fadeInEnd;
            track->expires = timing->expires;
            track->fadeOutStart = timing->fadeOutStart;
            track->fadeOutEnd = timing->fadeOutEnd;
            track->id = *g_playSerial;
            track->end = timing->end;
            timing->previous = track;
            // The tracks after it keep the rest, up to one fading out.
            const ee::Float rest = ee::Float(1.0f) - weight;
            for (ee::Ptr<AnimationTrack> after = track->next; after && after->fadingOut == 0; after = after->next)
                after->weight = after->weight * rest;
        }
        return *g_playSerial;
    }
    READABLE_FUNCTION_WAITS(layerAnimation, Draft);

    // stopTracks: every track of the object's animator taken off, its
    // events dropped.
    void stopTracks(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> animator = object->animator;
        if (!animator)
            return;
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            while (channel->tracks)
                removeTrack(channel, channel->tracks);
        }
        animator->events = ee::Ptr<AnimationEvent>();
        animator->firstEvent = ee::Ptr<AnimationEvent>();
    }
    READABLE_FUNCTION(stopTracks, Verified);

    // resetAnimatorPose: every channel of the object's animator at rest (no
    // translation, unit scale, no rotation; its flags all set but the
    // scale's, as the original ORs 0xFFFB), the object marked
    // (invalidateAnimator's result).
    int32_t resetAnimatorPose(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> animator = object->animator;
        animator->changes &= 0xFFF8u;
        for (uint32_t i = 0; i < animator->channelCount; ++i)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, i);
            channel->flags = static_cast<uint16_t>(channel->flags | 0xFFFBu);
            channel->translation = Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
            channel->scale = Vec4{ee::Float(1.0f), ee::Float(1.0f), ee::Float(1.0f), ee::Float(1.0f)};
            quaternionIdentity(&channel->rotation);
        }
        return game::tail(invalidateAnimator, object);
    }

    // resetFirstChannelPose: ZGE2's resetAnimatorPose: for each channel, the first
    // channel put at rest (its flags all set but the scale's, no translation, a
    // scale of (0, 0, 0, 1), no rotation: the original writes the first channel
    // every round and the scale as the translation, the bugs kept) and the
    // channel's tracks taken off; then the animator's events dropped. The
    // changes word is left and the pose not marked.
    void resetFirstChannelPose(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<Animator> animator = object->animator;
        for (uint32_t i = 0; i < animator->channelCount; ++i)
        {
            const ee::Ptr<AnimationChannel> first = animator->channels;
            const ee::Ptr<AnimationChannel> channel = channelAt(first, i);
            ee::Ptr<AnimationTrack> track = channel->tracks;
            first->flags = static_cast<uint16_t>(first->flags | 0xFFFBu);
            const ee::Ptr<AnimationChannel> rested = animator->channels;
            rested->translation = Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
            rested->scale = Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
            quaternionIdentity(&rested->rotation);
            while (track)
            {
                unlinkTrack(channel, track, ee::Ptr<AnimationTrack>());
                track = channel->tracks;
            }
        }
        animator->events = ee::Ptr<AnimationEvent>();
        animator->firstEvent = ee::Ptr<AnimationEvent>();
    }
    READABLE_FUNCTION_OWN(resetAnimatorPose, Verified, dm2, resetFirstChannelPose);

    // animationDone: whether the object's animation is over at `time`: 1 once
    // the first track (channel by channel) that is not fading out has
    // passed its end, or when there is none; 0 while it plays.
    int32_t animationDone(ee::Ptr<SceneObject> object, ee::Float time)
    {
        const ee::Ptr<Animator> animator = object->animator;
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            for (ee::Ptr<AnimationTrack> track = channelRecord(animator->channels, index).tracks; track; track = track->next)
            {
                if (track->fadingOut == 0)
                    return time <= track->end ? 0 : 1;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(animationDone, Verified);

    // The events up to the time fired through the hook, the cursor left on
    // the next. Time going back moves the cursor back through the set's earlier
    // events it has not reached. False if the hook stopped the update.
    static bool fireEvents(ee::Ptr<SceneObject> object, ee::Ptr<Animator> animator, ee::Float time)
    {
        ee::Ptr<AnimationEvent> event = animator->events;
        while (event)
        {
            const ee::Float base = animator->start;
            const ee::Float eventTime = keyTime(event->time) + base;
            if (time < eventTime)
            {
                while (event->previous && sameSetAsPrevious(event))
                {
                    event = event->previous;
                    if (!(time < keyTime(event->time) + base))
                        break;
                    animator->events = event;
                }
                return true;
            }
            if (*g_animationEventHook)
                (*g_animationEventHook)(object, event, eventTime);
            if (animator->stopped)
                return false;
            const ee::Ptr<AnimationEvent> next = animator->events->next;
            animator->events = next;
            if (!next)
                break;
            if (!sameSetAsPrevious(next))
                animator->events = ee::Ptr<AnimationEvent>();
            event = animator->events;
        }
        return true;
    }

    // updateAnimation: the object's animation brought to a time. The events
    // passed are fired through the hook (which may stop the update); each
    // channel's tracks that have expired are taken off (callback(object,
    // id, -1) once per id for those with a game record); then, while any
    // channel has a track left to mix, each such channel samples its next
    // track and the samples are summed by weight (callback(object, track
    // id, time) after each round); finally each channel's sums are
    // normalised into its result, and the object is marked if anything
    // changed. The callback is a game function's address or 0. (Nothing
    // past these three arguments is read, whatever a caller's last call left
    // beyond them.)
    void updateAnimation(ee::Ptr<SceneObject> object, uint32_t callbackAddress, ee::Float time)
    {
        ee::Local<uint32_t[12]> told; // the ids told of so far
        const AnimationCallback callback(callbackAddress);
        const ee::Ptr<Animator> animator = object->animator;
        animator->stopped = 0;
        if (!fireEvents(object, animator, time))
            return;
        animator->changes &= kChangesKept;

        // Each channel's sums cleared and its expired tracks taken off.
        uint32_t toldCount = 0;
        const auto alreadyTold = [&](uint32_t id)
        {
            for (uint32_t i = 0; i < toldCount; ++i)
                if (told[i] == id)
                    return true;
            return false;
        };
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            channel->translationSum = kZero;
            channel->scaleSum = kZero;
            memoryFill(&channel->rotationSum, 0, sizeof(Quaternion));
            channel->translationWeight = ee::Float(0.0f);
            channel->rotationWeight = ee::Float(0.0f);
            channel->flags &= static_cast<uint16_t>(~kChannelParts);
            channel->scaleWeight = ee::Float(0.0f);
            ee::Ptr<AnimationTrack> track = channel->tracks;
            while (track)
            {
                if (!(track->expires < time))
                {
                    track = track->next;
                    continue;
                }
                const ee::Ptr<AnimationTrack> previous = track->previous;
                if (track->gameRecord && callback && !alreadyTold(track->id))
                {
                    told[toldCount++] = track->id;
                    callback(object, track->id, ee::Float(-1.0f));
                }
                removeTrack(channel, track);
                track = previous ? previous->next : channel->tracks;
            }
            channel->mixNext = channel->tracks;
        }

        // Round by round, each channel's next track sampled and summed.
        for (;;)
        {
            ee::Ptr<AnimationTrack> lastTrack;
            for (uint32_t index = 0; index < animator->channelCount; ++index)
            {
                const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
                const ee::Ptr<AnimationTrack> track = channel->mixNext;
                channel->trackWeight = ee::Float(0.0f);
                if (!track)
                    continue;
                channel->mixNext = track->next;
                lastTrack = track;
                channel->flags &= static_cast<uint16_t>(~kChannelParts);
                channel->trackWeight = trackWeight(track, time);
                if (track->rotations)
                {
                    sampleRotation(channel, track, time);
                    channel->flags |= kChannelRotation;
                }
                if (track->translations)
                {
                    sampleTranslation(channel, track, time);
                    channel->flags |= kChannelTranslation;
                }
                if (track->scales)
                {
                    sampleScale(channel, track, time);
                    channel->flags |= kChannelScale;
                }
            }
            if (!lastTrack)
                break;
            if (callback)
                callback(object, lastTrack->id, time);

            // The samples added in by the track's weight (only where the
            // channel has a second track: a lone track's sample stands).
            for (uint32_t index = 0; index < animator->channelCount; ++index)
            {
                const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
                const uint32_t flags = channel->flags;
                const auto second = [&] { return channel->tracks->next; };
                if ((flags & kChannelRotation) && second() && second()->rotations)
                {
                    quaternionScale(&channel->rotation, &channel->rotation, channel->trackWeight);
                    quaternionAccumulate(&channel->rotationSum, &channel->rotationSum, &channel->rotation);
                    channel->rotationWeight = channel->rotationWeight + channel->trackWeight;
                }
                if ((flags & kChannelTranslation) && second() && second()->translations)
                {
                    channel->translation *= channel->trackWeight;
                    channel->translationSum += channel->translation;
                    channel->translationWeight = channel->translationWeight + channel->trackWeight;
                }
                if ((flags & kChannelScale) && second() && second()->scales)
                {
                    channel->scale *= channel->trackWeight;
                    channel->scaleSum += channel->scale;
                    channel->scaleWeight = channel->scaleWeight + channel->trackWeight;
                }
            }
        }

        // No track left: the sums normalised into the results.
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            if (ee::Float(0.0f) < channel->rotationWeight)
            {
                quaternionNormalize(&channel->rotationSum);
                quaternionCopy(&channel->rotation, &channel->rotationSum);
            }
            const ee::Float translationWeight = channel->translationWeight;
            if (ee::Float(0.0f) < translationWeight)
                channel->translation = channel->translationSum * (ee::Float(1.0f) / translationWeight);
            const ee::Float scaleWeight = channel->scaleWeight;
            if (ee::Float(0.0f) < scaleWeight)
                channel->scale = channel->scaleSum * (ee::Float(1.0f) / scaleWeight);
            animator->changes |= channel->flags;
        }
        if (animator->changes)
            markPoseStale(object);
    }

    // updateMotionTracks: ZGE2's updateAnimation: the object's animation brought to a time. The events up to
    // the time are told to the hook (object, event, the time), the cursor left on the next; each channel's
    // expired tracks are taken off; then the channel's tracks, newest first, add their weighted samples into
    // its key, and the sums are made the result: the rotation normalised, the translation and scale divided
    // by their weights' total. ZGE2 has no callback, keeps no change flags and marks no pose.
    void updateMotionTracks(ee::Ptr<SceneObject> object, ee::Float time)
    {
        const ee::Ptr<Animator> animator = object->animator;
        const auto nextEvent = [&] { return animator->events.cast<MotionEvent>(); };
        while (nextEvent() && !(time < nextEvent()->time + animator->start))
        {
            if (*g_animationEventHook)
                (*g_animationEventHook)(object, animator->events, time);
            animator->events = nextEvent()->next.cast<AnimationEvent>();
        }
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            ee::Ptr<AnimationTrack> previous;
            ee::Ptr<AnimationTrack> track = channel->tracks;
            while (track)
            {
                if (track->expires < time)
                {
                    unlinkTrack(channel, track, previous);
                    track = previous ? previous->next : channel->tracks;
                    continue;
                }
                previous = track;
                track = track->next;
            }

            channel->flags &= static_cast<uint16_t>(~kChannelParts);
            ee::Float translationWeight{0.0f};
            ee::Float scaleWeight{0.0f};
            for (track = channel->tracks; track; track = track->next)
            {
                if (track->rotations)
                    addWeightedRotation(channel, track, time);
                if (track->translations)
                    translationWeight = translationWeight + addWeightedTranslation(channel, track, time);
                if (track->scales)
                    scaleWeight = scaleWeight + addWeightedScale(channel, track, time);
            }
            if (channel->flags & kChannelRotation)
                quaternionNormalize(&channel->rotation);
            if (channel->flags & kChannelTranslation)
                channel->translation = channel->translation * (ee::Float(1.0f) / translationWeight);
            if (channel->flags & kChannelScale)
                channel->scale = channel->scale * (ee::Float(1.0f) / scaleWeight);
        }
    }
    READABLE_FUNCTION_OWN(updateAnimation, Verified, dm2, updateMotionTracks);

    // blendLayer: the motion blended into the object's pose at `time` by
    // `weight` (no tracks: each channel's bone's keys at the time, the key
    // functions' unblended keys): the channel's rotation, translation and
    // scale become (1 - weight) of the old plus weight of the key's; the
    // game told first (setTrackRecord for the held id, then
    // callback(object, held id, time)) when a callback is given. The
    // object marked if anything changed. scaleWords is a quadword the
    // callers hand on and nothing here reads.
    void blendLayer(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, uint32_t callbackAddress, uint32_t gameRecord, ee::Qword scaleWords,
                    ee::Float time, ee::Float weight)
    {
        ee::Local<Quaternion> scaled;
        ee::Local<Vec4> translationKept;
        ee::Local<Vec4> scaleKept;
        ee::Local<uint32_t> heldRecord;
        *heldRecord = gameRecord;
        const AnimationCallback callback(callbackAddress);
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<ScaleTrack> scales = motion->scales;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<TranslationTrack> translations = motion->translations;
        animator->changes &= kChangesKept;
        const ee::Float share = ee::Float(1.0f) - weight; // what stays of the old
        const ee::Float notBlended{-1.0f};
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            channel->translationWeight = notBlended;
            channel->rotationWeight = notBlended;
            channel->scaleWeight = notBlended;
            rotations = skipBelowChannel(rotations, channel);
            if (onChannel(rotations, channel))
            {
                quaternionScale(&channel->rotationSum, &channel->rotation, share);
                sampleRotationKey(rotations, &channel->rotation, time);
                channel->rotationWeight = share;
                channel->flags |= kChannelRotation;
            }
            translations = skipBelowChannel(translations, channel);
            if (onChannel(translations, channel))
            {
                channel->translationSum = channel->translation * share;
                sampleTranslationKey(translations, &channel->translation.xyz(), time);
                channel->translationWeight = share;
                channel->flags |= kChannelTranslation;
            }
            scales = skipBelowChannel(scales, channel);
            if (onChannel(scales, channel))
            {
                channel->scaleSum = channel->scale * share;
                sampleScaleKey(scales, &channel->scale.xyz(), time);
                channel->scaleWeight = share;
                channel->flags |= kChannelScale;
            }
        }
        if (callback)
        {
            setTrackRecord(object, *g_heldTrackId, *heldRecord);
            callback(object, *g_heldTrackId, time);
        }
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            if (ee::Float(0.0f) < channel->rotationWeight)
            {
                quaternionScale(scaled, &channel->rotation, weight);
                quaternionAccumulate(&channel->rotation, scaled, &channel->rotationSum);
                quaternionNormalize(&channel->rotation);
            }
            if (ee::Float(0.0f) < channel->translationWeight)
            {
                translationKept = channel->translation * weight;
                channel->translation = translationKept + channel->translationSum;
            }
            if (ee::Float(0.0f) < channel->scaleWeight)
            {
                scaleKept = channel->scale * weight;
                channel->scale = scaleKept + channel->scaleSum;
            }
            // The first channel's flags, every round: the bug kept.
            animator->changes |= animator->channels->flags;
        }
        if (animator->changes)
            markPoseStale(object);
    }

    // blendMotionKeys: ZGE2's blendLayer: the motion blended into the object's pose at `time` by `weight` (no
    // tracks: each channel's bone's float keys at the time, the key functions'): the channel's rotation
    // becomes weight of the key's plus (1 - weight) of the old, normalised, and its translation the same. No
    // callback, change flags or pose marked. Scale tracks are passed over and never blended (ZGE2 reads no
    // ASCA chunk, so a motion has none: the pass would not end at a bone at or past the channel's; kept, as
    // startMotionTracks').
    void blendMotionKeys(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time, ee::Float weight)
    {
        ee::Local<Quaternion> rotation;
        ee::Local<Vec4> translation;
        const ee::Ptr<Animator> animator = object->animator;
        ee::Ptr<ScaleTrack> scales = motion->scales;
        ee::Ptr<RotationTrack> rotations = motion->rotations;
        ee::Ptr<FloatTranslationTrack> translations = motion->translations.cast<FloatTranslationTrack>();
        for (uint32_t index = 0; index < animator->channelCount; ++index)
        {
            const ee::Ptr<AnimationChannel> channel = channelAt(animator->channels, index);
            rotations = skipBelowChannel(rotations, channel);
            if (onChannel(rotations, channel))
            {
                copyFloatRotationKey(rotations, rotation, time);
                quaternionScale(rotation, rotation, weight);
                quaternionScale(&channel->rotation, &channel->rotation, ee::Float(1.0f) - weight);
                quaternionAccumulate(&channel->rotation, rotation, &channel->rotation);
                quaternionNormalize(&channel->rotation);
            }
            translations = skipBelowChannel(translations, channel);
            if (onChannel(translations, channel))
            {
                copyFloatTranslationKey(translations, translation, time);
                *translation = *translation * weight;
                channel->translation = channel->translation * (ee::Float(1.0f) - weight);
                channel->translation = channel->translation + *translation;
            }
            while (scales)
            {
                if (scales->boneId < channel->boneId)
                    scales = scales->next;
            }
        }
    }
    READABLE_FUNCTION_OWN(blendLayer, Verified, dm2, blendMotionKeys);
}

namespace zge
{
    // ---- the game's records on the tracks ------------------------------------
    //
    // A track's gameRecord word is the game's record for its play (the
    // animator's records: zge/scene/animator.cpp), found by the
    // track's id. One id's record is kept apart, not on its tracks
    // (blendLayer's plays have no tracks).

    // setTrackRecord: every track of the object's animator with this id gets
    // the record (and the held one, for that id).
    void setTrackRecord(ee::Ptr<SceneObject> object, uint32_t id, ee::Address record)
    {
        if (id == *g_heldTrackId)
            *g_heldTrackRecord = record;
        const Animator &animator = *object->animator;
        for (uint32_t channel = 0; channel < animator.channelCount; ++channel)
            for (ee::Ptr<AnimationTrack> track = channelRecord(animator.channels, channel).tracks; track; track = track->next)
                if (track->id == id)
                    track->gameRecord = record;
    }
    READABLE_FUNCTION(setTrackRecord, Verified);

    // trackRecord: the record of the first track with this id (channel by
    // channel), or 0.
    uint32_t trackRecord(ee::Ptr<SceneObject> object, uint32_t id)
    {
        if (id == *g_heldTrackId)
            return *g_heldTrackRecord;
        const Animator &animator = *object->animator;
        for (uint32_t channel = 0; channel < animator.channelCount; ++channel)
            for (ee::Ptr<AnimationTrack> track = channelRecord(animator.channels, channel).tracks; track; track = track->next)
                if (track->id == id)
                    return track->gameRecord;
        return 0;
    }
    READABLE_FUNCTION(trackRecord, Verified);
}
