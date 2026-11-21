#pragma once

// The engine's animation module (zanim.c, freeAnimationPools .. animateStubZero; DM2's ELF
// keeps zanim.o in its link map between zassetLib.o and zstring.o; BMX XXX
// has it word for word, all three games' GameCube and Xbox builds the same
// records): motions read from ANIM chunk files, played on an object's
// animator.
//
// A Motion is an ANIM form (animation_file.cpp): per bone (a DEFI name's
// hash) a rotation track (AROT), a translation track (ATRA packed or ATRW
// in floats) and a scale track (ASCA), each a list of timed keys; and its
// events (ACBD), each with a record of fields its definition (ACBH) lays
// out, in sets (ACBI) chosen by the object's id.
//
// An object's Animator (its +0x3C) has a channel per bone; playing a motion
// (startAnimation) puts an AnimationTrack on each channel the motion has
// keys for, and the update (updateAnimation) samples each channel's tracks,
// weighted by their fades, into the channel's rotation, translation and
// scale, which the pose update (zge/scene/pose.cpp) builds the bones from.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct SceneObject; // zge/scene/scene.h

    // Key times are in 1/128ths of a second (the ANIM file's floats times
    // 128, kept in 16 bits).
    constexpr float kKeyTimeUnit = 0.0078125f;
    constexpr float kKeyTimeScale = 128.0f;
    // A rotation key's quaternion is in 1/16384ths (ZGE4's full keys: 1/32768ths).
    constexpr float kRotationKeyUnit = game::readable::perGame(6.1035156e-05f, 6.1035156e-05f, 3.0517578e-05f);
    constexpr float kRotationKeyScale = game::readable::perGame(16384.0f, 16384.0f, 32768.0f);
    // A packed translation's and a scale's components are in 1/128ths (and
    // ZGE4's packed rotation keys' quaternions).
    constexpr float kPackedKeyUnit = 0.0078125f;
    constexpr float kPackedKeyScale = 128.0f;

    // ---- a motion's records (an ANIM form's chunks) --------------------------

    struct RotationKey
    {
        uint16_t time;
        int16_t x, y, z, w;
    };
    static_assert(sizeof(RotationKey) == 10);

    // ZGE4's packed rotation key: the time, then the quaternion in 1/128ths.
    struct PackedRotationKey
    {
        uint16_t time;
        int8_t x, y, z, w;
    };
    static_assert(sizeof(PackedRotationKey) == 6);

    // AROT (16 bytes, a record of the rotation tracks' pool).
    struct RotationTrack
    {
        // +0x00 keys. ZGE4: the keys << 2, and their form (RotationKeyForm) in bits 0-1.
        uint32_t count;
        uint32_t boneId;             // +0x04 its bone's name hash (the DEFI name the chunk gives by index)
        ee::Ptr<RotationKey> keys;   // +0x08 (ZGE4's packed form: PackedRotationKeys)
        ee::Ptr<RotationTrack> next; // +0x0C sorted by boneId once the motion is read
    };
    static_assert(sizeof(RotationTrack) == 0x10);

    // ZGE4's two forms of rotation keys: what g_rotationKeyForm is when an AROT chunk is read (an AFLR chunk
    // makes it full for the rest of its ANIM form), kept in the track's count word. ZGE2 and ZGE3 have full
    // keys alone, in a count word of keys.
    enum RotationKeyForm : uint32_t
    {
        kRotationKeysPacked = 0, // PackedRotationKey
        kRotationKeysFull = 1,   // RotationKey
    };
    inline constexpr bool kRotationTracksKeepForm = game::readable::kIsBmx;
    inline constexpr uint32_t kRotationFormBits = 0x3u;
    // A rotation track's keys.
    [[gnu::always_inline]] inline uint32_t rotationKeyCount(ee::Ptr<RotationTrack> track)
    {
        if constexpr (kRotationTracksKeepForm)
            return track->count >> 2;
        else
            return track->count;
    }
    // Whether a rotation track's keys are PackedRotationKeys (ZGE4's form 0).
    [[gnu::always_inline]] inline bool rotationKeysPacked(ee::Ptr<RotationTrack> track)
    {
        if constexpr (kRotationTracksKeepForm)
            return (track->count & kRotationFormBits) == kRotationKeysPacked;
        else
            return false;
    }
    // ZGE4: a packed rotation track's keys.
    [[gnu::always_inline]] inline ee::Ptr<PackedRotationKey> packedRotationKeys(ee::Ptr<RotationTrack> track)
    {
        return track->keys.cast<PackedRotationKey>();
    }
    // A rotation track's keys set (ZGE4: its form kept).
    [[gnu::always_inline]] inline void setRotationKeyCount(ee::Ptr<RotationTrack> track, uint32_t count)
    {
        if constexpr (kRotationTracksKeepForm)
            track->count = (track->count & kRotationFormBits) | count << 2;
        else
            track->count = count;
    }
    // A rotation track's form set (ZGE4; ZGE2 and ZGE3 keep none).
    [[gnu::always_inline]] inline void setRotationTrackForm(ee::Ptr<RotationTrack> track, RotationKeyForm form)
    {
        if constexpr (kRotationTracksKeepForm)
            track->count = (track->count & ~kRotationFormBits) | form;
    }

    // ATRA's keys: the time and the translation in 1/128ths.
    struct PackedTranslationKey
    {
        uint16_t time;
        int16_t x, y, z;
    };
    static_assert(sizeof(PackedTranslationKey) == 8);

    // ATRW's keys: floats (the time in seconds).
    struct TranslationKey
    {
        ee::Float time;
        ee::Float x, y, z;
    };
    static_assert(sizeof(TranslationKey) == 0x10);

    // ATRA and ATRW (20 bytes, a record of the translation tracks' pool).
    struct TranslationTrack
    {
        uint32_t packed;                // +0x00 1: ATRA's PackedTranslationKeys, 0: ATRW's TranslationKeys
        uint32_t count;                 // +0x04 keys
        uint32_t boneId;                // +0x08
        uint32_t keys;                  // +0x0C PackedTranslationKey or TranslationKey array
        ee::Ptr<TranslationTrack> next; // +0x10
    };
    static_assert(sizeof(TranslationTrack) == 0x14);

    // ZGE2 (DM2) keeps a track's keys as the chunk's floats, its tracks 16
    // bytes from the heap (count, boneId, keys, next): an AROT key the time
    // and the quaternion (a RotationTrack's keys), an ATRA key the time and,
    // a quadword on, the translation.
    struct FloatRotationKey
    {
        ee::Float time;
        ee::Float x, y, z, w;
    };
    static_assert(sizeof(FloatRotationKey) == 0x14);
    struct FloatTranslationKey
    {
        ee::Float time;    // +0x00
        uint8_t _04[0x0C]; // (the translation's quadword alignment)
        Vec4 translation;  // +0x10 x, y, z (w is not read in)
    };
    static_assert(sizeof(FloatTranslationKey) == 0x20 && offsetof(FloatTranslationKey, translation) == 0x10);
    struct FloatTranslationTrack
    {
        uint32_t count;                      // +0x00 keys
        uint32_t boneId;                     // +0x04
        ee::Ptr<FloatTranslationKey> keys;   // +0x08
        ee::Ptr<FloatTranslationTrack> next; // +0x0C
    };
    static_assert(sizeof(FloatTranslationTrack) == 0x10);

    // ASCA's keys: the time and the scale in 1/128ths.
    struct ScaleKey
    {
        uint16_t time;
        int16_t x, y, z;
    };
    static_assert(sizeof(ScaleKey) == 8);

    // ASCA (16 bytes, a record of the scale tracks' pool).
    struct ScaleTrack
    {
        uint32_t count;           // +0x00
        uint32_t boneId;          // +0x04
        ee::Ptr<ScaleKey> keys;   // +0x08
        ee::Ptr<ScaleTrack> next; // +0x0C
    };
    static_assert(sizeof(ScaleTrack) == 0x10);

    // DEFI: the bones the tracks name by index.
    struct BoneName
    {
        char name[0x20];
        uint32_t hash; // +0x20 nameHash(name)
    };
    static_assert(sizeof(BoneName) == 0x24);

    struct BoneNames // 12 bytes (heap)
    {
        uint32_t count;              // +0x00
        ee::Ptr<BoneName> names;     // +0x04
        ee::Ptr<BoneNames> next;     // +0x08 an earlier DEFI chunk's
    };
    static_assert(sizeof(BoneNames) == 0x0C);

    // ACBH's fields: where an event's record keeps a value.
    struct EventField // 16 bytes (a record of the event fields' pool)
    {
        uint32_t id;                // +0x00 the name the game asks for (a name hash)
        uint32_t type;              // +0x04 [inferred: the value's type; read nowhere in the engine]
        uint32_t offset;            // +0x08 in the event's record
        ee::Ptr<EventField> next;   // +0x0C
    };
    static_assert(sizeof(EventField) == 0x10);

    // ACBH: a kind of event of one set, its record's size and fields.
    struct EventDefinition // 20 bytes (a record of the event definitions' pool)
    {
        uint32_t set;                    // +0x00 the ACBI set it was read in
        uint32_t kind;                   // +0x04 what the game tells events apart by
        uint32_t recordBytes;            // +0x08
        ee::Ptr<EventField> fields;      // +0x0C
        ee::Ptr<EventDefinition> next;   // +0x10
    };
    static_assert(sizeof(EventDefinition) == 0x14);

    // ACBD: an event at a time of the motion (sounds, effects: the game's
    // hook decides). A motion's events are a list through next, the last
    // one read first; previous goes back the other way.
    struct AnimationEvent // 24 bytes (a record of the events' pool)
    {
        uint32_t set;                         // +0x00 its definition's set (0: every object's)
        uint16_t time;                        // +0x04 in key time units from the animation's start
        uint16_t _06;
        uint32_t record;                      // +0x08 its fields' values (recordBytes, heap)
        ee::Ptr<EventDefinition> definition;  // +0x0C
        ee::Ptr<AnimationEvent> next;         // +0x10
        ee::Ptr<AnimationEvent> previous;     // +0x14
    };
    static_assert(sizeof(AnimationEvent) == 0x18);
    static_assert(offsetof(AnimationEvent, record) == 0x08 && offsetof(AnimationEvent, next) == 0x10);

    // ZGE2's ACBD event (16 bytes, heap; handleAcbdChunk reads it): its time a float in seconds from the
    // motion's start, its record, the definition of its kind, the next. ZGE2 has no event sets and no link
    // back. A ZGE2 motion's and animator's event words point at these.
    struct MotionEvent
    {
        ee::Float time;                       // +0x00
        uint32_t record;                      // +0x04 its fields' values (the definition's recordBytes, heap)
        uint32_t definition;                  // +0x08 ZGE2's definition record (its kind the first word, not AI's layout)
        ee::Ptr<MotionEvent> next;            // +0x0C
    };
    static_assert(sizeof(MotionEvent) == 0x10);

    // A motion: an ANIM form read (28 bytes, heap).
    struct Motion
    {
        uint32_t id;                                // +0x00 DEFI's first word (motionId)
        ee::Ptr<BoneNames> boneNames;               // +0x04
        ee::Ptr<RotationTrack> rotations;           // +0x08 by boneId, ascending
        ee::Ptr<TranslationTrack> translations;     // +0x0C
        ee::Ptr<ScaleTrack> scales;                 // +0x10
        ee::Ptr<EventDefinition> eventDefinitions;  // +0x14
        ee::Ptr<AnimationEvent> events;             // +0x18
    };
    static_assert(sizeof(Motion) == 0x1C);

    // ---- playing: an object's animator ---------------------------------------

    // One motion playing on a channel (72 bytes, a record of the tracks'
    // pool). Times are the animator's (seconds).
    struct AnimationTrack
    {
        uint32_t fadingOut;       // +0x00 set once a crossfade fades it out (animationDone ignores it then)
        ee::Float start;          // +0x04 the motion's time 0 (and where it starts fading in)
        ee::Float end;            // +0x08 its last key; sampled no later than this
        ee::Float expires;        // +0x0C taken off the channel once the time passes it
        ee::Float fadeInEnd;      // +0x10
        ee::Float fadeOutStart;   // +0x14
        ee::Float fadeOutEnd;     // +0x18
        ee::Float weight;         // +0x1C at full strength
        uint32_t rotationKey;     // +0x20 the key last sampled (the search starts there)
        uint32_t translationKey;  // +0x24
        uint32_t scaleKey;        // +0x28
        ee::Ptr<RotationTrack> rotations;       // +0x2C 0: none
        ee::Ptr<TranslationTrack> translations; // +0x30 0: none
        ee::Ptr<ScaleTrack> scales;             // +0x34 0: none
#if ZX_GAME_DM2
        // DM2 +0x38: ZGE2's track ends with its link (0x3C bytes, kAnimationTrackBytes): a channel's tracks are
        // singly linked, and ZGE2 keeps no game record, play serial or link back. ZGE3's fields below stand past
        // the end of DM2's record: nothing of DM2's reads them.
        ee::Ptr<AnimationTrack> next;     // DM2 +0x38
#endif
        // +0x38 the game's record for the play (setTrackRecord); non-zero:
        // the update's callback is told when the track is taken off.
        uint32_t gameRecord;
        uint32_t id;              // +0x3C the play's serial (startAnimation's result): what the callback is given
#if !ZX_GAME_DM2
        ee::Ptr<AnimationTrack> next;     // +0x40
#endif
        ee::Ptr<AnimationTrack> previous; // +0x44 0: the channel's first
    };
    inline constexpr size_t kAnimationTrackBytes = game::readable::perGame<size_t>(0x48u, 0x3Cu, 0x48u);
    static_assert(game::readable::kIsDm2 || sizeof(AnimationTrack) == kAnimationTrackBytes);
    static_assert(offsetof(AnimationTrack, rotationKey) == 0x20 && offsetof(AnimationTrack, rotations) == 0x2C);
    static_assert(offsetof(AnimationTrack, next) == game::readable::perGame(0x40u, 0x38u, 0x40u));
    static_assert(game::readable::kIsDm2 || (offsetof(AnimationTrack, gameRecord) == 0x38 && offsetof(AnimationTrack, previous) == 0x44));

    // AnimationChannel::flags: what this frame's tracks set.
    enum ChannelFlags : uint16_t
    {
        kChannelRotation = 1u << 0,
        kChannelTranslation = 1u << 1,
        kChannelScale = 1u << 2,
        kChannelParts = 0x7u,
    };

    // A bone's channel (0x90 bytes, ZGE2's 0x40; the animator's channels follow it,
    // sorted by bone id).
    struct AnimationChannel
    {
        // The last track's sample, then (once mixed) the channel's result:
        // the bone's key (zge/scene/scene.h BoneKey).
        Vec4 translation;           // +0x00
        Vec4 scale;                 // +0x10
        Quaternion rotation;        // +0x20
#if ZX_GAME_DM2
        // DM2 +0x30: ZGE2's channel is the key, the bone, its flags and its tracks (0x40 bytes,
        // kAnimationChannelBytes): it keeps no sums, weights or object. ZGE3's fields below stand past the end
        // of DM2's record: nothing of DM2's reads them.
        uint32_t boneId;            // DM2 +0x30 the bone's name hash
        uint16_t flags;             // DM2 +0x34 ChannelFlags
        uint16_t _36;
        ee::Ptr<AnimationTrack> tracks; // DM2 +0x38 the newest first (singly linked)
        uint32_t _3c;
#endif
        // The weighted sums of the tracks' samples.
        Vec4 translationSum;        // +0x30
        Vec4 scaleSum;              // +0x40
        Quaternion rotationSum;     // +0x50
        ee::Float trackWeight;      // +0x60 the track being mixed
        // The sums' total weights (blendLayer: the layer's share, -1 for
        // none).
        ee::Float translationWeight; // +0x64
        ee::Float scaleWeight;      // +0x68
        ee::Float rotationWeight;   // +0x6C
#if !ZX_GAME_DM2
        uint32_t boneId;            // +0x70 the bone's name hash
        uint16_t flags;             // +0x74 ChannelFlags (the other bits kept)
        uint16_t _76;
#endif
#if !ZX_GAME_DM2
        ee::Ptr<AnimationTrack> tracks;  // +0x78 the newest first
#endif
        ee::Ptr<AnimationTrack> mixNext; // +0x7C the next track to mix
        ee::Ptr<SceneObject> object; // +0x80 the one it animates (0: none)
        uint8_t _84[0x0C];
    };
    // A channel record's size: the animator's channels stand this far apart (createObjectAnimator makes room for
    // them). DM2's record is shorter than the struct here.
    inline constexpr size_t kAnimationChannelBytes = game::readable::perGame<size_t>(0x90u, 0x40u, 0x90u);
    static_assert(game::readable::kIsDm2 || sizeof(AnimationChannel) == kAnimationChannelBytes);
    static_assert(offsetof(AnimationChannel, boneId) == game::readable::perGame(0x70u, 0x30u, 0x70u));
    static_assert(offsetof(AnimationChannel, flags) == game::readable::perGame(0x74u, 0x34u, 0x74u));
    static_assert(offsetof(AnimationChannel, tracks) == game::readable::perGame(0x78u, 0x38u, 0x78u));
    static_assert(game::readable::kIsDm2 || (offsetof(AnimationChannel, trackWeight) == 0x60 && offsetof(AnimationChannel, tracks) == 0x78));
    static_assert(game::readable::kIsDm2 || offsetof(AnimationChannel, object) == 0x80);

    // The channel `index` places after `channels` (records kAnimationChannelBytes apart).
    inline ee::Ptr<AnimationChannel> channelAt(ee::Ptr<AnimationChannel> channels, ptrdiff_t index)
    {
        if constexpr (kAnimationChannelBytes == sizeof(AnimationChannel))
            return channels + index;
        else
            return (channels.cast<uint8_t>() + index * static_cast<ptrdiff_t>(kAnimationChannelBytes)).cast<AnimationChannel>();
    }
    // ZGE2's channels keep no object of their own (the bone setters then mark no pose).
    inline constexpr bool kChannelsKeepNoObject = game::readable::kIsDm2;
    // The object a channel animates (0: none; always 0 in ZGE2).
    inline ee::Ptr<SceneObject> channelObject(ee::Ptr<AnimationChannel> channel)
    {
        if constexpr (kChannelsKeepNoObject)
            return ee::Ptr<SceneObject>();
        else
            return channel->object;
    }
    // The same channel's record, read where it stands (as the pointer's indexing reads it).
    inline AnimationChannel &channelRecord(const ee::Ptr<AnimationChannel> &channels, size_t index)
    {
        if constexpr (kAnimationChannelBytes == sizeof(AnimationChannel))
            return channels[index];
        else
            return channelAt(channels, static_cast<ptrdiff_t>(index))[0];
    }

    // An object's animator (heap: the header, then its channels).
    struct Animator
    {
        uint32_t channelCount;          // +0x00
        uint32_t changes;               // +0x04 the channels' ChannelFlags (bits 0-2), cleared by the pose update
        ee::Ptr<AnimationChannel> channels; // +0x08 (right after the header)
        ee::Float start;                // +0x0C the motion's start: event times count from here
#if ZX_GAME_DM2
        // DM2 +0x10: ZGE2's animator has no stopped word (its startAnimation sets both event words, its update
        // walks the second). ZGE3's stopped word stands past the end of DM2's header: nothing of DM2's reads it.
        ee::Ptr<AnimationEvent> firstEvent; // DM2 +0x10
        ee::Ptr<AnimationEvent> events;     // DM2 +0x14
        uint32_t _18;
        uint32_t _1c;
#endif
        // +0x10 set by the event hook: the update stops (startAnimation
        // sets it, the update clears it first).
        uint32_t stopped;
#if !ZX_GAME_DM2
        ee::Ptr<AnimationEvent> firstEvent; // +0x14 the playing motion's first event (of the object's set)
        ee::Ptr<AnimationEvent> events;     // +0x18 the next event
        uint32_t _1c;
#endif
    };
    // The animator's header: its channels follow it (createObjectAnimator). DM2's struct is longer than its header.
    inline constexpr size_t kAnimatorBytes = 0x20;
    static_assert(game::readable::kIsDm2 || sizeof(Animator) == kAnimatorBytes);
    static_assert(offsetof(Animator, start) == 0x0C);
    static_assert(offsetof(Animator, firstEvent) == game::readable::perGame(0x14u, 0x10u, 0x14u));
    static_assert(offsetof(Animator, events) == game::readable::perGame(0x18u, 0x14u, 0x18u));

    // A left and right pairing of bones (newBoneMirror, the game's mirrorPairs
    // table: Hips with Hips, L_UpperArm with R_UpperArm ...): mirrorPose
    // swaps each pair's keys, mirrored (allocateSized: the header, then the
    // pairs).
    // A row of the table of names newBoneMirror takes.
    struct BoneNamePair
    {
        ee::Ptr<const char> left; // 0 ends the table
        ee::Ptr<const char> right;
    };
    struct BonePair
    {
        uint32_t a; // a bone's name hash
        uint32_t b;
    };
    struct BoneMirror
    {
        ee::Ptr<SceneObject> object; // +0x00 the object whose channels (findAnimatorBone) the pairs name
        uint32_t count;             // +0x04 pairs
        ee::Ptr<BonePair> pairs;    // +0x08 (right after the header)
        // +0x0C the block's size (freeSized). ZGE2 keeps none: its header is the three words above
        // (kBoneMirrorHeaderBytes), and its pairs start here.
        uint32_t bytes;
    };
    inline constexpr size_t kBoneMirrorHeaderBytes = game::readable::perGame<size_t>(0x10u, 0x0Cu, 0x10u);
    static_assert(game::readable::kIsDm2 || sizeof(BoneMirror) == kBoneMirrorHeaderBytes);

    // ---- the module's functions other files call by name ---------------------

    ee::Ptr<SceneObject> topObject(ee::Ptr<SceneObject> object);       // (animation.cpp)
    ee::Ptr<SceneObject> markPoseStale(ee::Ptr<SceneObject> object);   // (animation.cpp)
}

namespace zge
{
    // (animation.cpp)
    int32_t removeTrack(ee::Ptr<AnimationChannel> channel, ee::Ptr<AnimationTrack> track);
    uint32_t createObjectAnimator(ee::Ptr<SceneObject> object);
    void freeObjectAnimator(ee::Ptr<SceneObject> object);
    uint32_t startAnimation(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time);
    uint32_t crossfadeAnimation(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time, ee::Float duration);
    void stopTracks(ee::Ptr<SceneObject> object);
    // resetAnimatorPose: the object's animator's channels put at rest.
    int32_t resetAnimatorPose(ee::Ptr<SceneObject> object);
    int32_t animationDone(ee::Ptr<SceneObject> object, ee::Float time);
    void updateAnimation(ee::Ptr<SceneObject> object, uint32_t callbackAddress, ee::Float time);
    // (ZGE2's updateAnimation and blendLayer: no callback, no game record.)
    void updateMotionTracks(ee::Ptr<SceneObject> object, ee::Float time);
    void blendMotionKeys(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Float time, ee::Float weight);
    // (animation_queries.cpp) A motion's first event, and an event's time in seconds from the motion's start.
    ee::Ptr<AnimationEvent> motionEvents(ee::Ptr<Motion> motion);
    ee::Float eventTime(ee::Ptr<AnimationEvent> event);
    void blendLayer(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, uint32_t callbackAddress, uint32_t gameRecord, ee::Qword scaleWords, ee::Float time, ee::Float weight);
    void setTrackRecord(ee::Ptr<SceneObject> object, uint32_t id, ee::Address record);
    // trackRecord: the game's record of the first track with this id, or 0.
    uint32_t trackRecord(ee::Ptr<SceneObject> object, uint32_t id);
}

namespace zge
{
    // (animation_file.cpp)
    int32_t freeTrack(ee::Address record);
    uint32_t newTrack();
    uint32_t registerAnimationChunks();
    uint32_t unregisterAnimationChunks();
    uint32_t setMotionLoadedHook(uint32_t hook, ee::Address context);
    uint32_t motionId(ee::Ptr<Motion> motion);
    int32_t freeMotion(ee::Ptr<Motion> motion);
    // The motions', tracks' and keys' pools freed (a game's change of state).
    void freeAnimationPools();
    void freeMotionTrackPools(); // (ZGE4 only) the motions' track pools destroyed
    // (ZGE4 only) the form the AROT chunks' keys are read in (RotationKeyForm), and set (setRotationKeyForm1:
    // full keys); the block the motions read from now on are copied into, with its bytes (0: the heap).
    uint32_t rotationKeyForm();
    void setRotationKeyForm(uint32_t form);
    void setRotationKeyForm1();
    void setMotionArena(uint32_t arena, uint32_t bytes);
}

namespace zge
{
    // (animation_queries.cpp)
    ee::Ptr<Animator> sceneObjectAnimator(ee::Ptr<SceneObject> object);
    ee::Ptr<AnimationChannel> findBoneChannel(ee::Ptr<Animator> animator, uint32_t boneId);
    ee::Ptr<Animator> setAnimatorChanges(ee::Ptr<SceneObject> object, uint32_t changes);
    int32_t setBoneRotation(ee::Ptr<AnimationChannel> channel, ee::Ptr<Quaternion> rotation);
    ee::Ptr<Quaternion> boneRotation(ee::Ptr<AnimationChannel> channel);
    int32_t setBoneTranslation(ee::Ptr<AnimationChannel> channel, ee::Ptr<Vec4> translation);
    ee::Ptr<Vec4> boneTranslation(ee::Ptr<AnimationChannel> channel);
    ee::Ptr<Vec4> boneScale(ee::Ptr<AnimationChannel> channel);
    int32_t setBoneScale(ee::Ptr<AnimationChannel> channel, ee::Ptr<Vec4> scale);
    uint32_t newBoneMirror(ee::Ptr<SceneObject> object, ee::Ptr<BoneNamePair> names);
    int32_t freeBoneMirror(ee::Ptr<BoneMirror> mirror);
    void mirrorPose(ee::Ptr<BoneMirror> mirror);
    int32_t invalidateAnimator(ee::Ptr<SceneObject> object);
    void freeAnimator(ee::Ptr<Animator> animator);
    uint32_t toggleAnimationEventHook(uint32_t hook);
    void setAnimationEventHook(uint32_t hook); // (ZGE2's toggleAnimationEventHook: sets the hook, whatever was installed)
    uint32_t eventInt(ee::Ptr<AnimationEvent> event, uint32_t fieldId);
    ee::Float eventFloat(ee::Ptr<AnimationEvent> event, uint32_t fieldId);
    // Where an event's field sits in its record (0: it has none); the two are word for word the same.
    uint32_t eventFieldAddress(ee::Ptr<AnimationEvent> event, uint32_t fieldId);
    uint32_t eventFieldAddressCopy(ee::Ptr<AnimationEvent> event, uint32_t fieldId);
    uint32_t eventKind(ee::Ptr<AnimationEvent> event);
    ee::Float motionLength(ee::Ptr<Motion> motion);
    // (ZGE4 only) the length of the track started last on the object's first channel (0: none).
    ee::Float newestTrackLength(ee::Ptr<SceneObject> object);
}
