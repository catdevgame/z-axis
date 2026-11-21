// zanim.c's records and files (zge/scene/animation.h): the module's record
// pools, the ANIM form's chunk handlers (a motion read: DEFI's bone names,
// AROT, ATRA, ATRW and ASCA tracks, ACBI / ACBH / ACBD events), the tracks
// sorted by bone once the form is read, and a motion freed.
//
// The handlers are registered with the chunk parser (registerAnimationChunks:
// the model objects' set-up calls it) and run while a loader parses an ANIM
// chunk (zge/assets/chunk_reader.h). The motion being read is
// g_loadingMotion; ACBI sets the event set the next ACBH and ACBD belong to.
//

#include "animation.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/animation_file.cpp.h"

namespace zge
{

    namespace
    {
        // A track's keys as the kind the track holds, a motion's translation tracks as the float-keyed tracks a
        // heap motion has, and such a track as the list's own type: typed views of the same records.
        template <typename Key, typename Track>
        ee::Ptr<Key> keysAs(ee::Ptr<Track> track) { return track->keys.template cast<Key>(); }
        inline ee::Ptr<FloatTranslationTrack> floatTranslationsOf(ee::Ptr<Motion> motion) { return motion->translations.cast<FloatTranslationTrack>(); }
        inline ee::Ptr<TranslationTrack> asTranslationTrack(ee::Ptr<FloatTranslationTrack> track) { return track.cast<TranslationTrack>(); }

        constexpr uint32_t kHeapFree = sym::heapFree;     // (block)
        constexpr uint32_t kDestroyPool = sym::destroyPool;  // (pool): its records and blocks freed
        constexpr uint32_t kPoolFree = sym::poolFree;     // (pool, record) (zge/memory/pool.h poolFree)

        // This file's, called through the table as the original calls them.
        constexpr uint32_t kFreeAnimationPools = sym::freeAnimationPools;
        constexpr uint32_t kNewEventField = sym::newEventField;
        constexpr uint32_t kNewEvent = sym::newEvent;
        constexpr uint32_t kNewEventDefinition = sym::newEventDefinition;
        constexpr uint32_t kNewTranslationTrack = sym::newTranslationTrack;
        constexpr uint32_t kNewScaleTrack = sym::newScaleTrack;
        constexpr uint32_t kNewRotationTrack = sym::newRotationTrack;
        constexpr uint32_t kLoadMotion = sym::loadMotion;

        // The ANIM form's chunk handlers (this file's), by tag.
        constexpr uint32_t kHandleAnim = sym::handleAnimChunk;
        constexpr uint32_t kHandleDefi = sym::handleDefiChunk;
        constexpr uint32_t kHandleArot = sym::handleArotChunk;
        constexpr uint32_t kHandleAtra = sym::handleAtraChunk;
        constexpr uint32_t kHandleAtrw = sym::handleAtrwChunk;
        constexpr uint32_t kHandleAsca = sym::handleAscaChunk;
        constexpr uint32_t kHandleAcbh = sym::handleAcbhChunk;
        constexpr uint32_t kHandleAcbd = sym::handleAcbdChunk;
        constexpr uint32_t kHandleAcbi = sym::handleAcbiChunk;

        // The module's record pools (made at the first record).
        inline ee::Global<uint32_t, sym::g_trackPool> g_trackPool;            // AnimationTrack, 0x180 a block
        inline ee::Global<uint32_t, sym::g_rotationTrackPool> g_rotationTrackPool;    // RotationTrack, 0x1900 a block
        inline ee::Global<uint32_t, sym::g_translationTrackPool> g_translationTrackPool; // TranslationTrack, 0x200 a block
        inline ee::Global<uint32_t, sym::g_scaleTrackPool> g_scaleTrackPool;       // ScaleTrack, 0x40 a block
        inline ee::Global<uint32_t, sym::g_eventDefinitionPool> g_eventDefinitionPool;  // EventDefinition, 0x100 a block
        inline ee::Global<uint32_t, sym::g_eventFieldPool> g_eventFieldPool;       // EventField, 0x100 a block
        inline ee::Global<uint32_t, sym::g_eventPool> g_eventPool;            // AnimationEvent, 750 a block

        // Set while the ANIM handlers are registered.
        inline ee::Global<int32_t, sym::g_animationChunksRegistered> g_animationChunksRegistered;
        // The motion the handlers fill.
        inline ee::Global<ee::Ptr<Motion>, sym::g_loadingMotion> g_loadingMotion;
        // Told of each motion read: (motion, context, 0) (setMotionLoadedHook).
        using MotionLoadedHook = ee::FunctionPtr<void(ee::Ptr<Motion> motion, uint32_t context, uint32_t unused)>;
        inline ee::Global<uint32_t, sym::g_motionLoadedHook> g_motionLoadedHook;
        inline ee::Global<uint32_t, sym::g_motionLoadedContext> g_motionLoadedContext;
        // The event set ACBI chose (0 at each ANIM). ZGE2 reads no ACBI chunk.
        inline ee::Global<uint32_t, sym::g_eventSet> g_eventSet;
        constexpr bool kHasEventSets = !game::readable::kIsDm2;

        constexpr uint32_t kKeyBytes = sizeof(RotationKey);

        // ZGE4's own: the form the next AROT chunk's keys take (RotationKeyForm: packed unless an AFLR chunk made
        // them full; loadMotion puts it back after each ANIM form).
        inline ee::Global<uint32_t, sym::bmx::g_rotationKeyForm> g_rotationKeyForm;
        // ZGE4 reads a motion into a record of this pool (Motion, 0x180 a block), then copies it into its block.
        inline ee::Global<uint32_t, sym::bmx::g_motionRecordPool> g_motionRecordPool;
        // ZGE4: the memory motion blocks are taken from (0: the heap), what is left of it, and the largest block
        // asked for.
        inline ee::Global<uint32_t, sym::bmx::g_motionArena> g_motionArena;
        inline ee::Global<uint32_t, sym::bmx::g_motionArenaBytes> g_motionArenaBytes;
        inline ee::Global<uint32_t, sym::bmx::g_largestMotionBlock> g_largestMotionBlock;

        // A record of `pool` (the pool made first, with `bytes` records and
        // `perBlock` a block), cleared. The seven makers are this word for
        // word from their start: the block after the pool is made at +0x28,
        // the two calls' returns at +0x30 and +0x44.
        template <typename PoolGlobal>
        uint32_t newPoolRecord(PoolGlobal pool, uint32_t start, uint32_t bytes, uint32_t perBlock)
        {
            if (*pool == 0)
                *pool = createPool(bytes, perBlock, 0);
            const uint32_t record = poolAlloc(ee::Ptr<Pool>(*pool));
            memoryFill(record, 0, bytes);
            return record;
        }

        // The bone of the index a track chunk starts with (the motion's
        // newest DEFI names).
        uint32_t boneIdAt(uint32_t index)
        {
            return (*g_loadingMotion)->boneNames->names[index].hash;
        }

        uint16_t packTime(ee::Float seconds) { return static_cast<uint16_t>(ee::toInt(seconds * ee::Float(kKeyTimeScale))); }
    }

    // ---- the record pools --------------------------------------------------

    // freeMotionTrackPools (ZGE4 only; freeAnimationPools and the game's animation library loader call it): the
    // pools of the motions' rotation, translation and scale tracks and of their records as read destroyed.
    inline void freeMotionTrackPools()
    {
        auto destroy = [](uint32_t pool) {
            if (pool != 0)
                destroyPool(ee::Ptr<Pool>(pool));
        };
        destroy(*g_rotationTrackPool);
        const uint32_t translations = *g_translationTrackPool;
        *g_rotationTrackPool = 0;
        destroy(translations);
        const uint32_t scales = *g_scaleTrackPool;
        *g_translationTrackPool = 0;
        destroy(scales);
        const uint32_t records = *g_motionRecordPool;
        *g_scaleTrackPool = 0;
        destroy(records);
        *g_motionRecordPool = 0;
    }
    READABLE_FUNCTION_ONLY(bmx, freeMotionTrackPools);

    // freeAnimationPools: every pool of the module destroyed (the ANIM handlers
    // unregistered: unregisterAnimationChunks).
    void freeAnimationPools()
    {
        auto destroy = [](uint32_t pool) {
            if (pool != 0)
                destroyPool(ee::Ptr<Pool>(pool));
        };
        destroy(*g_trackPool);
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 destroys the motions' pools in a function of their own.
            *g_trackPool = 0;
            freeMotionTrackPools();
        }
        else
        {
            const uint32_t rotations = *g_rotationTrackPool;
            *g_trackPool = 0;
            destroy(rotations);
            const uint32_t translations = *g_translationTrackPool;
            *g_rotationTrackPool = 0;
            destroy(translations);
            const uint32_t scales = *g_scaleTrackPool;
            *g_translationTrackPool = 0;
            destroy(scales);
        }
        const uint32_t definitions = *g_eventDefinitionPool;
        if constexpr (!game::readable::kIsBmx)
            *g_scaleTrackPool = 0;
        destroy(definitions);
        const uint32_t fields = *g_eventFieldPool;
        *g_eventDefinitionPool = 0;
        destroy(fields);
        const uint32_t events = *g_eventPool;
        *g_eventFieldPool = 0;
        destroy(events);
        *g_eventPool = 0;
    }
    // (registered with ZGE2's own body below)

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, uint32_t record) { return ee::tailCall<int32_t>(kPoolFree, pool, record); }
        }
    }

    // The records' makers (a cleared record) and their frees (poolFree's
    // result, the original's tail call).
    int32_t freeEventField(uint32_t record) { return tail::poolFree(*g_eventFieldPool, record); }
    READABLE_FUNCTION(freeEventField, Verified);
    uint32_t newEventField() { return newPoolRecord(g_eventFieldPool, sym::newEventField, sizeof(EventField), 0x100); }
    READABLE_FUNCTION_WAITS(newEventField, Verified);

    int32_t freeEvent(uint32_t record) { return tail::poolFree(*g_eventPool, record); }
    READABLE_FUNCTION(freeEvent, Verified);
    uint32_t newEvent() { return newPoolRecord(g_eventPool, sym::newEvent, sizeof(AnimationEvent), game::readable::perGame(750u, 750u, 0x80u)); }
    READABLE_FUNCTION_WAITS(newEvent, Verified);

    int32_t freeEventDefinition(uint32_t record) { return tail::poolFree(*g_eventDefinitionPool, record); }
    READABLE_FUNCTION(freeEventDefinition, Verified);
    uint32_t newEventDefinition() { return newPoolRecord(g_eventDefinitionPool, sym::newEventDefinition, sizeof(EventDefinition), 0x100); }
    READABLE_FUNCTION_WAITS(newEventDefinition, Verified);

    int32_t freeTranslationTrack(uint32_t record) { return tail::poolFree(*g_translationTrackPool, record); }
    READABLE_FUNCTION(freeTranslationTrack, Verified);
    uint32_t newTranslationTrack() { return newPoolRecord(g_translationTrackPool, sym::newTranslationTrack, sizeof(TranslationTrack), game::readable::perGame(0x200u, 0x200u, 0x640u)); }
    READABLE_FUNCTION_WAITS(newTranslationTrack, Verified);

    int32_t freeScaleTrack(uint32_t record) { return tail::poolFree(*g_scaleTrackPool, record); }
    READABLE_FUNCTION(freeScaleTrack, Verified);
    uint32_t newScaleTrack() { return newPoolRecord(g_scaleTrackPool, sym::newScaleTrack, sizeof(ScaleTrack), 0x40); }
    READABLE_FUNCTION_WAITS(newScaleTrack, Verified);

    int32_t freeRotationTrack(uint32_t record) { return tail::poolFree(*g_rotationTrackPool, record); }
    READABLE_FUNCTION(freeRotationTrack, Verified);
    uint32_t newRotationTrack() { return newPoolRecord(g_rotationTrackPool, sym::newRotationTrack, sizeof(RotationTrack), game::readable::perGame(0x1900u, 0x1900u, 0x3390u)); }
    READABLE_FUNCTION_WAITS(newRotationTrack, Verified);

    // (registered with ZGE2's own bodies below)
    int32_t freeTrack(ee::Address record) { return tail::poolFree(*g_trackPool, record); }
    uint32_t newTrack() { return newPoolRecord(g_trackPool, sym::newTrack, kAnimationTrackBytes, 0x180); }

    // ---- the ANIM form's chunks --------------------------------------------

    void loadMotion(ChunkReader *reader);

    // handleAnimChunk: ANIM: a motion read from the form (no event set yet;
    // ZGE2 has no event sets).
    uint32_t handleAnimChunk(ChunkReader *reader)
    {
        if constexpr (kHasEventSets)
            *g_eventSet = 0;
        loadMotion(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAnimChunk, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // A motion's bone names, keys and event records (the chunk handlers): ZGE2
        // and ZGE3 take them from the heap; ZGE4 (BMX XXX) from the asset allocator
        // (allocateMemory; its event records allocateSized). loadMotion's own
        // record stays the heap's in each.
        uint32_t allocateMotionMemory(uint32_t bytes)
        {
            if constexpr (game::readable::kIsBmx)
                return allocateMemory(bytes);
            else
                return heapAllocate(bytes);
        }
        uint32_t allocateEventRecord(uint32_t bytes)
        {
            if constexpr (game::readable::kIsBmx)
                return allocateSized(bytes);
            else
                return heapAllocate(bytes);
        }
    }

    // handleDefiChunk: DEFI: the motion's id, then its bones' names (0x24
    // bytes each), hashed.
    uint32_t handleDefiChunk(ChunkReader *reader)
    {
        uint32_t hashed = 0;
        const ee::Ptr<BoneNames> names(allocateMotionMemory(sizeof(BoneNames)));
        memoryFill(names, 0, sizeof(BoneNames));
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        names->next = motion->boneNames;
        motion->boneNames = names;
        readChunkU32(reader, &motion->id);
        const uint32_t count = (chunkDataSize(reader) - 4u) / sizeof(BoneName);
        names->count = count;
        names->names = ee::Ptr<BoneName>(allocateMotionMemory(count * sizeof(BoneName)));
        const uint32_t bytes = chunkDataSize(reader) - 4u;
        readChunk(reader, names->names.get(), bytes);
        if (names->count != 0)
        {
            for (;;)
            {
                const uint32_t index = hashed++;
                const ee::Ptr<BoneName> name = names->names + index;
                name->hash = nameHash(name->name);
                if (!(hashed < names->count))
                    break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleDefiChunk, Verified);

    namespace
    {
        // ZGE4 keeps a rotation key's components inside this (either way) as it packs them.
        constexpr float kRotationComponentLimit = 0.99999f;

        // An AROT chunk's keys (per key its time and quaternion as floats) into the track's keys of the form Key:
        // each component times Scale, then read back into the word (times Unit).
        template <typename Key, typename Component, float Scale, float Unit>
        [[gnu::always_inline]] inline void readRotationKeys(ChunkReader *const &reader, const ee::Ptr<RotationTrack> &track,
                                                            ee::Local<uint32_t> &counter, ee::Local<ee::Float> &word)
        {
            const uint32_t count = (chunkDataSize(reader) - 4u) / 20u; // a key: five floats
            setRotationKeyCount(track, count);
            track->keys = ee::Ptr<RotationKey>(allocateMotionMemory(count * sizeof(Key)));
            for (*counter = 0; *counter < rotationKeyCount(track); *counter = *counter + 1)
            {
                readChunkWord(reader, word);
                keysAs<Key>(track)[*counter].time = packTime(*word);
                // Each component packed (ZGE4: first kept inside the limit), then read back into the word.
                const auto component = [&](Component Key::*part) {
                    readChunkWord(reader, word);
                    if constexpr (game::readable::kIsBmx)
                        *word = ee::max(ee::min(*word, ee::Float(kRotationComponentLimit)), ee::Float(-kRotationComponentLimit));
                    keysAs<Key>(track)[*counter].*part = static_cast<Component>(ee::toInt(*word * ee::Float(Scale)));
                    *word = ee::toFloat(keysAs<Key>(track)[*counter].*part) * ee::Float(Unit);
                };
                component(&Key::x);
                component(&Key::y);
                component(&Key::z);
                component(&Key::w);
            }
        }
    }

    // readFloatRotationTrack: ZGE2's AROT: the bone's rotation keys kept as
    // the chunk's floats (FloatRotationKey), the track from the heap.
    uint32_t readFloatRotationTrack(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        const ee::Ptr<RotationTrack> track(heapAllocate(sizeof(RotationTrack)));
        memoryFill(track, 0, sizeof(RotationTrack));
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->next = motion->rotations;
        motion->rotations = track;
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        const uint32_t count = (chunkDataSize(reader) - 4u) / sizeof(FloatRotationKey);
        track->count = count;
        track->keys = ee::Ptr<RotationKey>(heapAllocate(count * sizeof(FloatRotationKey)));
        for (*counter = 0; *counter < track->count; *counter = *counter + 1)
        {
            const auto key = [&] { return keysAs<FloatRotationKey>(track) + *counter; };
            readChunkWord(reader, &key()->time);
            readChunkWord(reader, &key()->x);
            readChunkWord(reader, &key()->y);
            readChunkWord(reader, &key()->z);
            readChunkWord(reader, &key()->w);
        }
        return 1;
    }

    // readFloatTranslationTrack: ZGE2's ATRA: the bone's translation keys
    // kept as the chunk's floats (FloatTranslationKey), the track from the
    // heap.
    uint32_t readFloatTranslationTrack(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        const ee::Ptr<FloatTranslationTrack> track(heapAllocate(sizeof(FloatTranslationTrack)));
        memoryFill(track, 0, sizeof(FloatTranslationTrack));
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->next = floatTranslationsOf(motion);
        motion->translations = asTranslationTrack(track);
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        const uint32_t count = (chunkDataSize(reader) - 4u) >> 4; // a key: four floats in the chunk
        track->count = count;
        track->keys = ee::Ptr<FloatTranslationKey>(heapAllocate(count * sizeof(FloatTranslationKey)));
        for (*counter = 0; *counter < track->count; *counter = *counter + 1)
        {
            const auto key = [&] { return track->keys + *counter; };
            readChunkWord(reader, &key()->time);
            readChunkWord(reader, &key()->translation.x);
            readChunkWord(reader, &key()->translation.y);
            readChunkWord(reader, &key()->translation.z);
        }
        return 1;
    }

    // handleArotChunk: AROT: a bone's rotation keys (the bone's index, then per
    // key its time and quaternion as floats), packed into 16 bits each (ZGE4:
    // into 8 bits each unless g_rotationKeyForm asks for full keys).
    uint32_t handleArotChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        ee::Local<ee::Float> word;
        const ee::Ptr<RotationTrack> track(newRotationTrack());
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->next = motion->rotations;
        motion->rotations = track;
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        // ZGE4 keeps the keys' form in the track's count word: packed unless the game asks for full keys.
        if constexpr (kRotationTracksKeepForm)
        {
            if (*g_rotationKeyForm != kRotationKeysFull)
            {
                setRotationTrackForm(track, kRotationKeysPacked);
                readRotationKeys<PackedRotationKey, int8_t, kPackedKeyScale, kPackedKeyUnit>(reader, track, counter, word);
                return 1;
            }
            setRotationTrackForm(track, kRotationKeysFull);
            readRotationKeys<RotationKey, int16_t, kRotationKeyScale, kRotationKeyUnit>(reader, track, counter, word);
            return 1;
        }
        const uint32_t count = (chunkDataSize(reader) - 4u) / 20u; // a key: five floats
        track->count = count;
        track->keys = ee::Ptr<RotationKey>(allocateMotionMemory(count * sizeof(RotationKey)));
        for (*counter = 0; *counter < track->count; *counter = *counter + 1)
        {
            readChunkWord(reader, word);
            track->keys[*counter].time = packTime(*word);
            // Each component packed, then read back into the word.
            const auto component = [&](int16_t RotationKey::*part) {
                readChunkWord(reader, word);
                track->keys[*counter].*part = static_cast<int16_t>(ee::toInt(*word * ee::Float(kRotationKeyScale)));
                *word = ee::toFloat(track->keys[*counter].*part) * ee::Float(kRotationKeyUnit);
            };
            component(&RotationKey::x);
            component(&RotationKey::y);
            component(&RotationKey::z);
            component(&RotationKey::w);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleArotChunk, Verified, dm2, readFloatRotationTrack);

    // handleAtraChunk (ATRA) and handleAtrwChunk (ATRW): a bone's translation
    // keys, packed into 16 bits each (ATRA) or kept as floats (ATRW).
    uint32_t handleAtraChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        ee::Local<ee::Float> word;
        const ee::Ptr<TranslationTrack> track(newTranslationTrack());
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->packed = 1;
        track->next = motion->translations;
        motion->translations = track;
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        const uint32_t count = (chunkDataSize(reader) - 4u) >> 4; // a key: four floats
        track->count = count;
        track->keys = allocateMotionMemory(count * sizeof(PackedTranslationKey));
        const ee::Float unitsPerSecond(kPackedKeyScale);
        for (*counter = 0; *counter < track->count; ++*counter)
        {
            PackedTranslationKey &key = ee::Ptr<PackedTranslationKey>(track->keys)[*counter];
            readChunkWord(reader, word);
            key.time = static_cast<uint16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.x = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.y = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.z = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(handleAtraChunk, Verified, dm2, readFloatTranslationTrack);

    uint32_t handleAtrwChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        ee::Local<ee::Float> word;
        const ee::Ptr<TranslationTrack> track(newTranslationTrack());
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->packed = 0;
        track->next = motion->translations;
        motion->translations = track;
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        const uint32_t count = (chunkDataSize(reader) - 4u) >> 4;
        track->count = count;
        const uint32_t memory = allocateMotionMemory(count * sizeof(TranslationKey));
        const ee::Ptr<TranslationKey> keys(memory);
        track->keys = memory;
        for (*counter = 0; *counter < track->count; ++*counter)
        {
            for (ee::Float TranslationKey::*part : {&TranslationKey::time, &TranslationKey::x, &TranslationKey::y, &TranslationKey::z})
            {
                readChunkWord(reader, word);
                keys[*counter].*part = *word;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAtrwChunk, Verified);

    // handleAscaChunk: ASCA: a bone's scale keys, packed into 16 bits each.
    uint32_t handleAscaChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> counter;
        ee::Local<ee::Float> word;
        const ee::Ptr<ScaleTrack> track(newScaleTrack());
        const ee::Ptr<Motion> motion = *g_loadingMotion;
        track->next = motion->scales;
        motion->scales = track;
        readChunkU32(reader, &*counter);
        track->boneId = boneIdAt(*counter);
        const uint32_t count = (chunkDataSize(reader) - 4u) >> 4;
        track->count = count;
        track->keys = ee::Ptr<ScaleKey>(allocateMotionMemory(count * sizeof(ScaleKey)));
        const ee::Float unitsPerSecond(kPackedKeyScale);
        for (*counter = 0; *counter < track->count; ++*counter)
        {
            ScaleKey &key = track->keys[*counter];
            readChunkWord(reader, word);
            key.time = static_cast<uint16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.x = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.y = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
            readChunkWord(reader, word);
            key.z = static_cast<int16_t>(ee::toInt(*word * unitsPerSecond));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAscaChunk, Verified);

    // handleAcbhChunk: ACBH: the event set's definitions: per definition its
    // kind and record size, then its fields (id, type, offset), each list
    // ended by a 0 word. 0 when a pool is out of records.
    uint32_t handleAcbhChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> word;
        uint32_t field = 0;
        uint32_t definitionAddress = 0;
        readChunkU32(reader, &*word);
        if (*word == 0)
            return 1;
        for (;;)
        {
            definitionAddress = newEventDefinition();
            const ee::Ptr<EventDefinition> definition(definitionAddress);
            if (!definition)
                return 0;
            const ee::Ptr<Motion> motion = *g_loadingMotion;
            definition->set = *g_eventSet;
            definition->kind = *word;
            definition->next = motion->eventDefinitions;
            motion->eventDefinitions = definition;
            // The record's size, then field after field: each one's id
            // (0 ends them), type and offset.
            uint32_t *value = &definition->recordBytes;
            for (;;)
            {
                readChunkU32(reader, value);
                readChunkU32(reader, &*word);
                if (*word == 0)
                    break;
                field = newEventField();
                const ee::Ptr<EventField> made(field);
                if (!made)
                    return 0;
                made->id = *word;
                made->next = definition->fields;
                definition->fields = made;
                readChunkU32(reader, &made->type);
                value = &made->offset;
            }
            readChunkU32(reader, &*word);
            if (*word == 0)
                break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAcbhChunk, Verified);

    // handleAcbiChunk: ACBI: the set the next definitions and events are of.
    uint32_t handleAcbiChunk(ChunkReader *reader)
    {
        readChunkU32(reader, g_eventSet.get());
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAcbiChunk, Verified);

    // handleAcbdChunk: ACBD: the events: per event its kind (0 ends them), its
    // time and its record (its set's definition of that kind gives the
    // size). 0 when an event's definition is not found or the pool is out.
    uint32_t handleAcbdChunk(ChunkReader *reader)
    {
        ee::Local<uint32_t> kind;
        ee::Local<ee::Float> time;
        const ee::Float timeScale(kKeyTimeScale);
        for (readChunkU32(reader, &*kind); *kind != 0; readChunkU32(reader, &*kind))
        {
            const ee::Ptr<AnimationEvent> event(newEvent());
            if (!event)
                return 0;
            const ee::Ptr<Motion> motion = *g_loadingMotion;
            const uint32_t set = *g_eventSet;
            // The set's definition of the kind.
            ee::Ptr<EventDefinition> definition = motion->eventDefinitions;
            while (definition && !(definition->set == set && definition->kind == *kind))
                definition = definition->next;
            if (!definition)
                return 0;
            event->set = set;
            event->definition = definition;
            readChunkWord(reader, time);
            event->time = static_cast<uint16_t>(ee::toInt(*time * timeScale));
            const uint32_t record = allocateEventRecord(definition->recordBytes);
            event->record = record;
            readChunk(reader, ee::Ptr<uint8_t>(record).get(), definition->recordBytes);
            const ee::Ptr<AnimationEvent> newest = motion->events;
            event->next = newest;
            if (newest)
                newest->previous = event;
            motion->events = event;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleAcbdChunk, Verified);

    // registerAnimationChunks: the ANIM form's handlers registered with the chunk
    // parser (the model objects' set-up). Always 1.
    uint32_t registerAnimationChunks()
    {
        *g_animationChunksRegistered = 1;
        addChunkHandler(chunkTag("ANIM"), kHandleAnim);
        struct Chunk
        {
            uint32_t tag, handler;
        };
        if constexpr (game::readable::kIsDm2) // ZGE2 reads no ATRW, ASCA or ACBI chunks
        {
            const Chunk others[] = {
                {chunkTag("DEFI"), kHandleDefi}, {chunkTag("AROT"), kHandleArot}, {chunkTag("ATRA"), kHandleAtra},
                {chunkTag("ACBH"), kHandleAcbh}, {chunkTag("ACBD"), kHandleAcbd},
            };
            for (const auto &other : others)
                addChunkHandler(other.tag, other.handler);
        }
        else if constexpr (game::readable::kIsBmx) // ZGE4 reads an AFLR chunk too
        {
            const Chunk others[] = {
                {chunkTag("DEFI"), kHandleDefi}, {chunkTag("AFLR"), sym::bmx::handleAflrChunk},
                {chunkTag("AROT"), kHandleArot}, {chunkTag("ATRA"), kHandleAtra}, {chunkTag("ATRW"), kHandleAtrw},
                {chunkTag("ASCA"), kHandleAsca}, {chunkTag("ACBH"), kHandleAcbh}, {chunkTag("ACBD"), kHandleAcbd},
                {chunkTag("ACBI"), kHandleAcbi},
            };
            for (const auto &other : others)
                addChunkHandler(other.tag, other.handler);
        }
        else
        {
            const Chunk others[] = {
                {chunkTag("DEFI"), kHandleDefi}, {chunkTag("AROT"), kHandleArot}, {chunkTag("ATRA"), kHandleAtra},
                {chunkTag("ATRW"), kHandleAtrw}, {chunkTag("ASCA"), kHandleAsca}, {chunkTag("ACBH"), kHandleAcbh},
                {chunkTag("ACBD"), kHandleAcbd}, {chunkTag("ACBI"), kHandleAcbi},
            };
            for (const auto &other : others)
                addChunkHandler(other.tag, other.handler);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerAnimationChunks, Verified);

    // unregisterAnimationChunks: the pools destroyed if the handlers were registered.
    // Always 1.
    uint32_t unregisterAnimationChunks()
    {
        if (*g_animationChunksRegistered != 0)
        {
            freeAnimationPools();
            *g_animationChunksRegistered = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(unregisterAnimationChunks, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t heapFree(ee::Ptr<Motion> block) { return ee::tailCall<int32_t>(kHeapFree, block); }
        }
        // A motion's memory (allocateMotionMemory's) freed: ZGE4's with freeMemory.
        void freeMotionMemory(ee::Address memory)
        {
            if constexpr (game::readable::kIsBmx)
                freeMemory(memory);
            else
                heapFree(memory);
        }
    }

    // ---- ZGE2's tracks: a free list, no pool -----------------------------------
    namespace
    {
        // The tracks given back and not yet taken again, linked through `next` (ZGE2's newTrack takes from here before
        // the heap; its freeAnimationPools gives them all back to the heap).
        inline ee::Global<ee::Ptr<AnimationTrack>, sym::dm2::g_freeTracks> g_freeTracks;
    }

    // newTrackFromFreeList (ZGE2's newTrack): the first free track, else a new one from the heap; cleared.
    uint32_t newTrackFromFreeList()
    {
        ee::Ptr<AnimationTrack> track = *g_freeTracks;
        if (!track)
            track = ee::Ptr<AnimationTrack>(heapAllocate(kAnimationTrackBytes));
        else
            *g_freeTracks = track->next;
        memoryFill(track, 0, kAnimationTrackBytes);
        return ee::Address(track);
    }
    READABLE_FUNCTION_WAITS_OWN(newTrack, Verified, dm2, newTrackFromFreeList);

    // freeTrackToFreeList (ZGE2's freeTrack): the track put first on the free list; the one that was first.
    int32_t freeTrackToFreeList(ee::Address record)
    {
        const ee::Ptr<AnimationTrack> track(record);
        const ee::Ptr<AnimationTrack> first = *g_freeTracks;
        track->next = first;
        *g_freeTracks = track;
        return static_cast<int32_t>(ee::Address(first));
    }
    READABLE_FUNCTION_OWN(freeTrack, Verified, dm2, freeTrackToFreeList);

    // freeTrackFreeList (ZGE2's freeAnimationPools): every free track back to the heap, the list emptied one by one.
    void freeTrackFreeList()
    {
        for (ee::Ptr<AnimationTrack> track = *g_freeTracks; track; track = *g_freeTracks)
        {
            *g_freeTracks = track->next;
            heapFree(track);
        }
    }
    READABLE_FUNCTION_WAITS_OWN(freeAnimationPools, Verified, dm2, freeTrackFreeList);

    // A list of motion tracks freed: each one's keys, then the record.
    template <typename Track>
    static void freeTracks(ee::Ptr<Track> track, ee::Ptr<Pool> pool)
    {
        while (track)
        {
            const ee::Ptr<Track> following = track->next;
            freeMotionMemory(track->keys);
            poolFree(ee::Ptr<Pool>(pool), track);
            track = following;
        }
    }

    // A list sorted by bone id: a pass over it swaps each record whose
    // successor has a lower id with it (what they hold, not their places on
    // the list), until a pass swaps none.
    template <typename Track>
    static void sortByBone(ee::Ptr<Track> first, ee::Ptr<Track> held)
    {
        bool swapped;
        do
        {
            swapped = false;
            for (ee::Ptr<Track> track = first; track; track = track->next)
            {
                const ee::Ptr<Track> next = track->next;
                if (next && next->boneId < track->boneId)
                {
                    *held = *next;
                    *next = *track;
                    *track = *held;
                    next->next = track->next;
                    track->next = next;
                    swapped = true;
                }
            }
        } while (swapped);
    }

    // loadMotion: the ANIM form read into a new motion: its chunks parsed
    // (the handlers above), then its rotation, translation and scale tracks
    // sorted by bone id, and the motion handed to the loaded hook.
    void loadMotion(ChunkReader *reader)
    {
        ee::Local<RotationTrack[1]> rotationHeld;
        ee::Local<TranslationTrack[1]> translationHeld;
        ee::Local<ScaleTrack[1]> scaleHeld;
        if (enterChunk(reader) == 0)
            return;
        const ee::Ptr<Motion> motion(heapAllocate(sizeof(Motion)));
        memoryFill(motion, 0, sizeof(Motion));
        *g_loadingMotion = motion;
        parseChunks(reader, 0);
        leaveChunk(reader);

        sortByBone<RotationTrack>(motion->rotations, rotationHeld);
        sortByBone<TranslationTrack>(motion->translations, translationHeld);
        sortByBone<ScaleTrack>(motion->scales, scaleHeld);

        const MotionLoadedHook hook(*g_motionLoadedHook);
        if (hook)
            hook(*g_loadingMotion, *g_motionLoadedContext, 0);
    }

    // ---- ZGE4: a motion in one block -----------------------------------------
    //
    // ZGE4 reads an ANIM form as ZGE3 does, into a record of its own pool, then
    // copies the motion, its tracks and their keys into one block (from the
    // motion arena the game may give, else the heap) and frees what it read.

    // rotationKeyForm, setRotationKeyForm (ZGE4 only): the form the AROT chunks' keys
    // take (RotationKeyForm); a form past kRotationKeysFull is not taken.
    inline uint32_t rotationKeyForm()
    {
        return *g_rotationKeyForm;
    }
    READABLE_FUNCTION_ONLY(bmx, rotationKeyForm);
    inline void setRotationKeyForm(uint32_t form)
    {
        if (form <= kRotationKeysFull)
            *g_rotationKeyForm = form;
    }
    READABLE_FUNCTION_ONLY(bmx, setRotationKeyForm);
    // setRotationKeyForm1 (ZGE4 only): the AROT chunks' keys read full (kRotationKeysFull) from now on.
    inline void setRotationKeyForm1()
    {
        *g_rotationKeyForm = kRotationKeysFull;
    }
    READABLE_FUNCTION_ONLY(bmx, setRotationKeyForm1);
    // setMotionArena (ZGE4 only): the block the motions read from now on are copied into (allocateMotionBlock),
    // with its bytes; 0: the heap.
    inline void setMotionArena(uint32_t arena, uint32_t bytes)
    {
        *g_motionArena = arena;
        *g_motionArenaBytes = bytes;
    }
    READABLE_FUNCTION_ONLY(bmx, setMotionArena);

    // newMotionRecord, freeMotionRecord (ZGE4 only): a record of g_motionRecordPool for a
    // motion being read (cleared), and one handed back (poolFree's result, the original's tail call).
    inline uint32_t newMotionRecord()
    {
        return newPoolRecord(g_motionRecordPool, sym::bmx::newMotionRecord, sizeof(Motion), 0x180);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newMotionRecord);
    inline int32_t freeMotionRecord(ee::Ptr<Motion> record)
    {
        return tail::poolFree(*g_motionRecordPool, ee::Address(record));
    }
    READABLE_FUNCTION_ONLY(bmx, freeMotionRecord);

    // allocateMotionBlock (ZGE4 only): a motion's block: from the motion arena while
    // it has the bytes left (taken a word at a time), else the heap. The largest
    // block asked for is kept.
    inline uint32_t allocateMotionBlock(uint32_t bytes)
    {
        if (*g_largestMotionBlock < bytes)
            *g_largestMotionBlock = bytes;
        const uint32_t arena = *g_motionArena;
        const uint32_t left = *g_motionArenaBytes;
        if (arena == 0 || left < bytes)
            return heapAllocate(bytes);
        const uint32_t partWord = bytes & 3u;
        if (partWord != 0)
            bytes = bytes + 4u - partWord;
        *g_motionArena = arena + bytes;
        *g_motionArenaBytes = left - bytes;
        return arena;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, allocateMotionBlock);

    // freeLoadedMotion (ZGE4 only): a motion as read, once copied into its block: its
    // translation, rotation and scale tracks (their keys, then the records), then
    // its record (freeMotionRecord's result, the original's tail call).
    inline int32_t freeLoadedMotion(ee::Ptr<Motion> motion)
    {
        freeTracks(motion->translations, ee::Ptr<Pool>(*g_translationTrackPool));
        freeTracks(motion->rotations, ee::Ptr<Pool>(*g_rotationTrackPool));
        freeTracks(motion->scales, ee::Ptr<Pool>(*g_scaleTrackPool));
        return game::tail(freeMotionRecord, motion);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeLoadedMotion);

    namespace
    {
        // ZGE4 tells the hook (motion, context): no third word.
        using MotionLoadedHookOfTwo = ee::FunctionPtr<void(ee::Ptr<Motion> motion, uint32_t context)>;

        // The bytes of a motion's block: the motion, its tracks' records and their keys.
        uint32_t motionBlockBytes(ee::Ptr<Motion> motion)
        {
            uint32_t bytes = sizeof(Motion);
            for (ee::Ptr<ScaleTrack> track = motion->scales; track; track = track->next)
                bytes += sizeof(ScaleTrack) + track->count * sizeof(ScaleKey);
            for (ee::Ptr<TranslationTrack> track = motion->translations; track; track = track->next)
                bytes += sizeof(TranslationTrack) + track->count * (track->packed != 0 ? sizeof(PackedTranslationKey) : sizeof(TranslationKey));
            for (ee::Ptr<RotationTrack> track = motion->rotations; track; track = track->next)
                bytes += sizeof(RotationTrack) + rotationKeyCount(track) * (rotationKeysPacked(track) ? sizeof(PackedRotationKey) : sizeof(RotationKey));
            return bytes;
        }

        // A list of track records copied into the block at `cursor`, each copy linked to the next;
        // `first` the first copy (left as it is for an empty list). The cursor past them.
        template <typename Track>
        uint32_t copyTrackRecords(ee::Ptr<Track> from, ee::Ptr<Track> &first, uint32_t cursor)
        {
            if (from)
                first = ee::Ptr<Track>(cursor);
            ee::Ptr<Track> copy(cursor);
            while (from)
            {
                memoryCopy(copy, from, sizeof(Track));
                cursor += sizeof(Track);
                ee::Ptr<Track> next;
                if (copy->next)
                {
                    copy->next = ee::Ptr<Track>(cursor);
                    next = copy->next;
                }
                from = from->next;
                copy = next;
            }
            return cursor;
        }

        // Each track's keys copied into the block at `cursor` (keyBytes(track, copy) of them), its copy pointed at
        // them. The cursor past them.
        template <typename Track, typename KeyBytes>
        uint32_t copyTrackKeys(ee::Ptr<Track> from, ee::Ptr<Track> copy, uint32_t cursor, KeyBytes keyBytes)
        {
            for (; from; from = from->next, copy = copy->next)
            {
                copy->keys = decltype(copy->keys)(cursor);
                memoryCopy(cursor, from->keys, keyBytes(from, copy));
                cursor += keyBytes(from, copy);
            }
            return cursor;
        }
    }

    // loadMotionIntoBlock: ZGE4's loadMotion: the ANIM form read into a record of
    // g_motionRecordPool and its tracks sorted by bone id (as loadMotion); its bone
    // names freed (the tracks keep their bones' ids); the motion, its scale,
    // translation and rotation tracks and then their keys copied into one block
    // (allocateMotionBlock), what was read freed, and the block handed to the loaded
    // hook. g_rotationKeyForm is put back as it was before the form.
    void loadMotionIntoBlock(ChunkReader *reader)
    {
        ee::Local<RotationTrack[1]> rotationHeld;
        ee::Local<TranslationTrack[1]> translationHeld;
        ee::Local<ScaleTrack[1]> scaleHeld;
        const uint32_t form = rotationKeyForm();
        if (enterChunk(reader) == 0)
        {
            setRotationKeyForm(form);
            return;
        }
        const ee::Ptr<Motion> read(newMotionRecord());
        memoryFill(read, 0, sizeof(Motion));
        *g_loadingMotion = read;
        parseChunks(reader, 0);
        leaveChunk(reader);

        sortByBone<RotationTrack>(read->rotations, rotationHeld);
        sortByBone<TranslationTrack>(read->translations, translationHeld);
        sortByBone<ScaleTrack>(read->scales, scaleHeld);

        for (ee::Ptr<BoneNames> names = read->boneNames; names;)
        {
            const ee::Ptr<BoneNames> following = names->next;
            freeMemory(names->names);
            freeMemory(names);
            names = following;
        }
        read->boneNames = ee::Ptr<BoneNames>();

        const uint32_t bytes = motionBlockBytes(read);
        const ee::Ptr<Motion> motion(allocateMotionBlock(bytes));
        memoryFill(motion, 0, bytes);
        uint32_t cursor = ee::Address(motion) + sizeof(Motion);
        memoryCopy(motion, *g_loadingMotion, sizeof(Motion));
        cursor = copyTrackRecords((*g_loadingMotion)->scales, motion->scales, cursor);
        cursor = copyTrackRecords((*g_loadingMotion)->translations, motion->translations, cursor);
        cursor = copyTrackRecords((*g_loadingMotion)->rotations, motion->rotations, cursor);
        cursor = copyTrackKeys((*g_loadingMotion)->scales, motion->scales, cursor,
                               [](ee::Ptr<ScaleTrack>, ee::Ptr<ScaleTrack> copy) { return copy->count * uint32_t(sizeof(ScaleKey)); });
        cursor = copyTrackKeys((*g_loadingMotion)->translations, motion->translations, cursor,
                               [](ee::Ptr<TranslationTrack> track, ee::Ptr<TranslationTrack> copy) {
                                   return copy->count * uint32_t(track->packed != 0 ? sizeof(PackedTranslationKey) : sizeof(TranslationKey));
                               });
        copyTrackKeys((*g_loadingMotion)->rotations, motion->rotations, cursor, [](ee::Ptr<RotationTrack> track, ee::Ptr<RotationTrack> copy) {
            return rotationKeyCount(copy) * uint32_t(rotationKeysPacked(track) ? sizeof(PackedRotationKey) : sizeof(RotationKey));
        });
        freeLoadedMotion(*g_loadingMotion);

        const MotionLoadedHookOfTwo hook(*g_motionLoadedHook);
        *g_loadingMotion = motion;
        if (hook)
            hook(motion, *g_motionLoadedContext);
        setRotationKeyForm(form);
    }

    // loadHeapMotion: ZGE2's loadMotion: the ANIM form read as loadMotion
    // reads it (its tracks, all 16 bytes, sorted the same way), and the
    // loaded hook told (motion, context) with no test (ZGE2's game always
    // sets one). The motion, or 0 without the form.
    ee::Ptr<Motion> loadHeapMotion(ChunkReader *reader)
    {
        ee::Local<RotationTrack[1]> rotationHeld;
        ee::Local<FloatTranslationTrack[1]> translationHeld;
        ee::Local<ScaleTrack[1]> scaleHeld;
        if (enterChunk(reader) == 0)
            return ee::Ptr<Motion>();
        const ee::Ptr<Motion> motion(heapAllocate(sizeof(Motion)));
        memoryFill(motion, 0, sizeof(Motion));
        *g_loadingMotion = motion;
        parseChunks(reader, 0);
        leaveChunk(reader);

        sortByBone<RotationTrack>(motion->rotations, rotationHeld);
        sortByBone<FloatTranslationTrack>(floatTranslationsOf(motion), translationHeld);
        sortByBone<ScaleTrack>(motion->scales, scaleHeld);

        const MotionLoadedHookOfTwo hook(*g_motionLoadedHook);
        hook(*g_loadingMotion, *g_motionLoadedContext);
        return *g_loadingMotion;
    }
    READABLE_FUNCTION_WAITS_OWN(loadMotion, Verified, bmx, loadMotionIntoBlock, dm2, loadHeapMotion);

    // setMotionLoadedHook: the hook told of each motion read, and its context; the
    // hook it replaces.
    uint32_t setMotionLoadedHook(uint32_t hook, ee::Address context)
    {
        const uint32_t previous = *g_motionLoadedHook;
        *g_motionLoadedHook = hook;
        *g_motionLoadedContext = context;
        return previous;
    }
    READABLE_FUNCTION(setMotionLoadedHook, Verified);

    // motionId: a motion's id.
    uint32_t motionId(ee::Ptr<Motion> motion) { return motion->id; }
    READABLE_FUNCTION(motionId, Verified);

    // freeMotion: a motion freed: its translation, rotation and scale
    // tracks (their keys, then the records), its bone names, its event
    // definitions (their fields), its events (their records), then the
    // motion (heapFree's result, the original's tail call).
    int32_t freeMotion(ee::Ptr<Motion> motion)
    {
        freeTracks(motion->translations, ee::Ptr<Pool>(*g_translationTrackPool));
        freeTracks(motion->rotations, ee::Ptr<Pool>(*g_rotationTrackPool));
        freeTracks(motion->scales, ee::Ptr<Pool>(*g_scaleTrackPool));

        for (ee::Ptr<BoneNames> names = motion->boneNames; names;)
        {
            const ee::Ptr<BoneNames> following = names->next;
            heapFree(names->names);
            heapFree(names);
            names = following;
        }

        for (ee::Ptr<EventDefinition> definition = motion->eventDefinitions; definition;)
        {
            const ee::Ptr<EventDefinition> following = definition->next;
            for (ee::Ptr<EventField> field = definition->fields; field;)
            {
                const ee::Ptr<EventField> nextField = field->next;
                poolFree(ee::Ptr<Pool>(*g_eventFieldPool), field);
                field = nextField;
            }
            poolFree(ee::Ptr<Pool>(*g_eventDefinitionPool), definition);
            definition = following;
        }

        for (ee::Ptr<AnimationEvent> event = motion->events; event;)
        {
            const ee::Ptr<AnimationEvent> following = event->next;
            heapFree(event->record);
            poolFree(ee::Ptr<Pool>(*g_eventPool), event);
            event = following;
        }
        return tail::heapFree(motion);
    }

    // freeMotionBlock: ZGE4's freeMotion: a motion in its block (loadMotionIntoBlock's):
    // its events (their records: freeSized, as allocateEventRecord gave them), its
    // event definitions (their fields), then the block (heapFree's result, the
    // original's tail call). Its tracks and keys are in the block.
    int32_t freeMotionBlock(ee::Ptr<Motion> motion)
    {
        for (ee::Ptr<AnimationEvent> event = motion->events; event;)
        {
            const ee::Ptr<AnimationEvent> following = event->next;
            freeSized(event->record, event->definition->recordBytes);
            poolFree(ee::Ptr<Pool>(*g_eventPool), event);
            event = following;
        }

        for (ee::Ptr<EventDefinition> definition = motion->eventDefinitions; definition;)
        {
            const ee::Ptr<EventDefinition> following = definition->next;
            for (ee::Ptr<EventField> field = definition->fields; field;)
            {
                const ee::Ptr<EventField> nextField = field->next;
                poolFree(ee::Ptr<Pool>(*g_eventFieldPool), field);
                field = nextField;
            }
            poolFree(ee::Ptr<Pool>(*g_eventDefinitionPool), definition);
            definition = following;
        }
        return tail::heapFree(motion);
    }

    namespace
    {
        // ZGE2: a list of tracks given back to the heap, each its keys first.
        template <typename Track>
        void freeHeapTracks(ee::Ptr<Track> track)
        {
            while (track)
            {
                const ee::Ptr<Track> following = track->next;
                heapFree(track->keys);
                heapFree(track);
                track = following;
            }
        }
    }

    // freeHeapMotion: ZGE2's freeMotion: its translation, rotation and scale
    // tracks (their keys, then the records) and its bone names given back to
    // the heap, then the motion (heapFree's result, the original's tail
    // call). ZGE2's motions have no events.
    int32_t freeHeapMotion(ee::Ptr<Motion> motion)
    {
        freeHeapTracks(floatTranslationsOf(motion));
        freeHeapTracks(motion->rotations);
        freeHeapTracks(motion->scales);
        for (ee::Ptr<BoneNames> names = motion->boneNames; names;)
        {
            const ee::Ptr<BoneNames> following = names->next;
            heapFree(names->names);
            heapFree(names);
            names = following;
        }
        return tail::heapFree(motion);
    }
    READABLE_FUNCTION_WAITS_OWN(freeMotion, Verified, bmx, freeMotionBlock, dm2, freeHeapMotion);
}
